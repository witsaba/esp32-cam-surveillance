// Package relay owns the one-camera-ingest and NATS fan-out boundary.
package relay

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/url"
	"strconv"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"github.com/nats-io/nats.go"
	"github.com/witsaba/esp32-cam-surveillance/backend/internal/discovery"
)

const (
	defaultCameraWSPort = 80
	defaultCameraWSPath = "/cams"
	defaultStreamFPS    = 5
	minReconnectDelay   = 2 * time.Second
	maxReconnectDelay   = 30 * time.Second
)

// CameraConn is the small part of a WebSocket connection needed by ingest.
// It also makes camera sessions deterministic to test without a device.
type CameraConn interface {
	ReadMessage() (messageType int, data []byte, err error)
	WriteMessage(messageType int, data []byte) error
	Close() error
}

// CameraDialer opens the single backend-to-camera connection.
type CameraDialer func(context.Context, discovery.Camera) (CameraConn, error)

// Config controls camera WebSocket dialing and stream startup.
type Config struct {
	Port int
	Path string
	FPS  int
	Logf func(format string, args ...any)
	Dial CameraDialer
}

// Relay maintains at most one active ingest session per camera MAC. A
// session exists while at least one viewer has acquired it.
type Relay struct {
	nc       *nats.Conn
	registry *discovery.Registry
	config   Config

	mu       sync.Mutex
	sessions map[string]*cameraSession
	closed   bool
}

type cameraSession struct {
	mac     string
	cancel  context.CancelFunc
	viewers int
	conn    CameraConn
}

// New creates a relay. It does not dial cameras until Acquire is called.
func New(nc *nats.Conn, registry *discovery.Registry, config Config) *Relay {
	if config.Port == 0 {
		config.Port = defaultCameraWSPort
	}
	if config.Path == "" {
		config.Path = defaultCameraWSPath
	}
	if config.FPS == 0 {
		config.FPS = defaultStreamFPS
	}
	if config.Logf == nil {
		config.Logf = log.Printf
	}
	if config.Dial == nil {
		port, path := config.Port, config.Path
		config.Dial = func(ctx context.Context, camera discovery.Camera) (CameraConn, error) {
			return dialCameraWithConfig(ctx, camera, port, path)
		}
	}
	return &Relay{nc: nc, registry: registry, config: config, sessions: make(map[string]*cameraSession)}
}

// Acquire starts or reuses the one ingest connection for mac.
func (r *Relay) Acquire(mac string) error {
	if r == nil || r.registry == nil || r.nc == nil {
		return fmt.Errorf("camera relay is not configured")
	}
	if !validMAC(mac) {
		return fmt.Errorf("invalid camera MAC %q", mac)
	}
	if _, ok := r.registry.Get(mac); !ok {
		return fmt.Errorf("camera %q is not registered", mac)
	}

	r.mu.Lock()
	defer r.mu.Unlock()
	if r.closed {
		return fmt.Errorf("camera relay is closed")
	}
	if session, ok := r.sessions[mac]; ok {
		session.viewers++
		return nil
	}
	ctx, cancel := context.WithCancel(context.Background())
	session := &cameraSession{mac: mac, cancel: cancel, viewers: 1}
	r.sessions[mac] = session
	go r.run(ctx, session)
	return nil
}

// Release removes one viewer's claim. Closing the last claim closes the
// camera WebSocket; the firmware treats that as stream-off and stops capture.
func (r *Relay) Release(mac string) {
	if r == nil {
		return
	}
	r.mu.Lock()
	session, ok := r.sessions[mac]
	if !ok {
		r.mu.Unlock()
		return
	}
	session.viewers--
	if session.viewers > 0 {
		r.mu.Unlock()
		return
	}
	delete(r.sessions, mac)
	if session.conn != nil {
		_ = session.conn.Close()
	}
	session.cancel()
	r.mu.Unlock()
}

// Close stops all ingest sessions.
func (r *Relay) Close() {
	if r == nil {
		return
	}
	r.mu.Lock()
	if r.closed {
		r.mu.Unlock()
		return
	}
	r.closed = true
	for mac, session := range r.sessions {
		delete(r.sessions, mac)
		if session.conn != nil {
			_ = session.conn.Close()
		}
		session.cancel()
	}
	r.mu.Unlock()
}

func (r *Relay) run(ctx context.Context, session *cameraSession) {
	delay := minReconnectDelay
	for {
		camera, ok := r.registry.Get(session.mac)
		if !ok {
			return
		}
		conn, err := r.config.Dial(ctx, camera)
		if err == nil {
			r.mu.Lock()
			if r.sessions[session.mac] != session {
				r.mu.Unlock()
				_ = conn.Close()
				return
			}
			session.conn = conn
			r.mu.Unlock()
			err = r.consume(ctx, camera, conn)
			_ = conn.Close()
			r.mu.Lock()
			if r.sessions[session.mac] == session {
				session.conn = nil
			}
			r.mu.Unlock()
			delay = minReconnectDelay
		}
		if ctx.Err() != nil {
			return
		}
		r.config.Logf("camera relay %s disconnected: %v; retrying in %s", session.mac, err, delay)
		if !wait(ctx, delay) {
			return
		}
		if delay < maxReconnectDelay {
			delay *= 2
			if delay > maxReconnectDelay {
				delay = maxReconnectDelay
			}
		}
	}
}

func (r *Relay) consume(ctx context.Context, camera discovery.Camera, conn CameraConn) error {
	messageType, data, err := conn.ReadMessage()
	if err != nil {
		return fmt.Errorf("read hello: %w", err)
	}
	if messageType != websocket.TextMessage {
		return fmt.Errorf("first camera message must be text")
	}
	hello, err := parseHello(data, camera.MAC)
	if err != nil {
		return err
	}
	r.registry.Upsert(discovery.Camera{
		MAC: hello.MAC, IP: camera.IP, Name: hello.Name,
		Description: hello.Description, Firmware: hello.Firmware, Chip: hello.Chip,
	}, time.Now())
	r.publishEvent(hello.MAC, data)

	command, _ := json.Marshal(map[string]any{"cmd": "stream", "on": true, "fps": r.config.FPS})
	if err := conn.WriteMessage(websocket.TextMessage, command); err != nil {
		return fmt.Errorf("start stream: %w", err)
	}

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
		}
		messageType, data, err = conn.ReadMessage()
		if err != nil {
			return fmt.Errorf("read camera stream: %w", err)
		}
		switch messageType {
		case websocket.BinaryMessage:
			// Publish is intentionally not followed by Flush: core NATS keeps
			// ingest fire-and-forget so a slow viewer cannot block the camera.
			if err := r.nc.Publish(FrameSubject(hello.MAC), data); err != nil {
				r.config.Logf("camera relay %s publish failed: %v", hello.MAC, err)
			}
		case websocket.TextMessage:
			r.publishEvent(hello.MAC, data)
		}
	}
}

func (r *Relay) publishEvent(mac string, data []byte) {
	if err := r.nc.Publish("cams."+mac+".events", data); err != nil {
		r.config.Logf("camera relay %s event publish failed: %v", mac, err)
	}
}

type helloFrame struct {
	Type        string `json:"type"`
	MAC         string `json:"mac"`
	Name        string `json:"name"`
	Description string `json:"description"`
	Firmware    string `json:"fw"`
	Chip        string `json:"chip"`
}

func parseHello(data []byte, expectedMAC string) (helloFrame, error) {
	var hello helloFrame
	if err := json.Unmarshal(data, &hello); err != nil {
		return hello, fmt.Errorf("decode camera hello: %w", err)
	}
	if hello.Type != "hello" || !validMAC(hello.MAC) || hello.MAC != expectedMAC {
		return hello, fmt.Errorf("invalid camera hello for %s", expectedMAC)
	}
	return hello, nil
}

func dialCameraWithConfig(ctx context.Context, camera discovery.Camera, port int, path string) (CameraConn, error) {
	dialer := websocket.Dialer{HandshakeTimeout: 5 * time.Second}
	u := url.URL{Scheme: "ws", Host: net.JoinHostPort(camera.IP, strconv.Itoa(port)), Path: path}
	conn, _, err := dialer.DialContext(ctx, u.String(), nil)
	if err != nil {
		return nil, err
	}
	return conn, nil
}

func wait(ctx context.Context, duration time.Duration) bool {
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}
