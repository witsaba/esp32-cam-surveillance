package relay

import (
	"encoding/json"
	"log"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	"github.com/nats-io/nats.go"
	"github.com/witsaba/esp32-cam-surveillance/backend/internal/discovery"
)

const viewerQueueDepth = 8

// CameraRelay is the viewer hub's small lifecycle dependency.
type CameraRelay interface {
	Acquire(mac string) error
	Release(mac string)
}

// Hub fans each camera's NATS subject out to bounded viewer queues.
type Hub struct {
	nc       *nats.Conn
	relay    CameraRelay
	registry *discovery.Registry
	logf     func(format string, args ...any)
	fps      int
	upgrader websocket.Upgrader

	mu      sync.RWMutex
	viewers map[string]map[*viewer]struct{}
	dropped map[string]*atomic.Uint64
	sub     *nats.Subscription
}

type viewer struct {
	conn    *websocket.Conn
	mac     string
	frames  chan []byte
	done    chan struct{}
	stop    sync.Once
	dropped atomic.Uint64
	onStop  func()
	onDrop  func()
}

// NewHub creates the one process-lifetime wildcard NATS subscription.
func NewHub(nc *nats.Conn, registry *discovery.Registry, relay CameraRelay, logf func(format string, args ...any), fps ...int) (*Hub, error) {
	if nc == nil || registry == nil || relay == nil {
		return nil, &configurationError{"NATS, registry, and relay are required"}
	}
	if logf == nil {
		logf = log.Printf
	}
	streamFPS := defaultStreamFPS
	if len(fps) > 0 && fps[0] > 0 {
		streamFPS = fps[0]
	}
	h := &Hub{
		nc: nc, relay: relay, registry: registry, logf: logf, fps: streamFPS,
		viewers: make(map[string]map[*viewer]struct{}),
		dropped: make(map[string]*atomic.Uint64),
	}
	// Leave CheckOrigin nil so Gorilla applies its safe same-origin policy.
	sub, err := nc.Subscribe("cams.*.frames", h.receive)
	if err != nil {
		return nil, err
	}
	if err := sub.SetPendingLimits(viewerQueueDepth, 131072); err != nil {
		_ = sub.Unsubscribe()
		return nil, err
	}
	h.sub = sub
	nc.SetErrorHandler(func(_ *nats.Conn, sub *nats.Subscription, err error) {
		if err == nats.ErrSlowConsumer {
			var subject string
			var count int
			if sub != nil {
				subject = sub.Subject
				count, _ = sub.Dropped()
			}
			h.logf("NATS slow consumer: subject=%s dropped=%d", subject, count)
		}
	})
	return h, nil
}

type configurationError struct{ message string }

func (e *configurationError) Error() string { return e.message }

// Close removes the process-lifetime subscription.
func (h *Hub) Close() error {
	if h == nil || h.sub == nil {
		return nil
	}
	return h.sub.Unsubscribe()
}

// DroppedFrames returns cumulative viewer queue drops for a camera.
func (h *Hub) DroppedFrames(mac string) uint64 {
	if h == nil {
		return 0
	}
	h.mu.RLock()
	counter := h.dropped[mac]
	h.mu.RUnlock()
	if counter == nil {
		return 0
	}
	return counter.Load()
}

func (h *Hub) receive(message *nats.Msg) {
	mac, ok := FrameMAC(message.Subject)
	if !ok || len(message.Data) == 0 {
		return
	}
	h.mu.RLock()
	viewers := make([]*viewer, 0, len(h.viewers[mac]))
	for item := range h.viewers[mac] {
		viewers = append(viewers, item)
	}
	h.mu.RUnlock()
	// NATS owns message.Data after the callback returns; retain one immutable
	// copy for the asynchronous viewer writers.
	frame := append([]byte(nil), message.Data...)
	for _, item := range viewers {
		item.enqueue(frame)
	}
}

// ServeHTTP handles GET /api/cameras/{mac}/stream.
func (h *Hub) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	mac, ok := streamMAC(r.URL.Path)
	if r.Method != http.MethodGet || !ok {
		writeHubError(w, http.StatusNotFound, "not_found", "stream endpoint not found")
		return
	}
	if _, ok := h.registry.Get(mac); !ok {
		writeHubError(w, http.StatusNotFound, "camera_not_found", "camera is not registered")
		return
	}
	if err := h.relay.Acquire(mac); err != nil {
		writeHubError(w, http.StatusConflict, "camera_unavailable", err.Error())
		return
	}
	conn, err := h.upgrader.Upgrade(w, r, nil)
	if err != nil {
		h.relay.Release(mac)
		return
	}
	v := &viewer{
		conn: conn, mac: mac, frames: make(chan []byte, viewerQueueDepth), done: make(chan struct{}),
	}
	v.onStop = func() {
		h.removeViewer(v)
		h.relay.Release(mac)
	}
	v.onDrop = func() { h.recordDrop(mac) }
	h.addViewer(v)
	if err := conn.WriteJSON(map[string]any{"type": "stream_meta", "mac": mac, "fps": h.fps}); err != nil {
		v.shutdown()
		return
	}
	go v.writeLoop()
	defer v.shutdown()

	conn.SetReadLimit(1024)
	_ = conn.SetReadDeadline(time.Now().Add(30 * time.Second))
	conn.SetPongHandler(func(string) error {
		return conn.SetReadDeadline(time.Now().Add(30 * time.Second))
	})
	for {
		messageType, _, err := conn.ReadMessage()
		if err != nil {
			return
		}
		if messageType == websocket.PongMessage {
			continue
		}
		_ = conn.WriteControl(websocket.CloseMessage,
			websocket.FormatCloseMessage(websocket.ClosePolicyViolation, "viewer is read-only"),
			time.Now().Add(time.Second))
		return
	}
}

func (v *viewer) enqueue(data []byte) {
	select {
	case <-v.done:
		return
	default:
	}
	select {
	case v.frames <- data:
		return
	default:
	}
	select {
	case <-v.frames:
	default:
	}
	select {
	case v.frames <- data:
	default:
		return
	}
	v.dropped.Add(1)
	if v.onDrop != nil {
		v.onDrop()
	}
}

func (v *viewer) writeLoop() {
	ping := time.NewTicker(10 * time.Second)
	defer ping.Stop()
	for {
		select {
		case <-v.done:
			return
		case frame := <-v.frames:
			if err := v.conn.WriteMessage(websocket.BinaryMessage, frame); err != nil {
				v.shutdown()
				return
			}
		case <-ping.C:
			if err := v.conn.WriteControl(websocket.PingMessage, nil, time.Now().Add(time.Second)); err != nil {
				v.shutdown()
				return
			}
		}
	}
}

func (v *viewer) shutdown() {
	v.stop.Do(func() {
		close(v.done)
		_ = v.conn.Close()
		if v.onStop != nil {
			v.onStop()
		}
	})
}

func (h *Hub) addViewer(v *viewer) {
	h.mu.Lock()
	if h.viewers[v.mac] == nil {
		h.viewers[v.mac] = make(map[*viewer]struct{})
	}
	h.viewers[v.mac][v] = struct{}{}
	h.mu.Unlock()
}

func (h *Hub) removeViewer(v *viewer) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if viewers := h.viewers[v.mac]; viewers != nil {
		delete(viewers, v)
		if len(viewers) == 0 {
			delete(h.viewers, v.mac)
		}
	}
}

func (h *Hub) recordDrop(mac string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.dropped[mac] == nil {
		h.dropped[mac] = &atomic.Uint64{}
	}
	h.dropped[mac].Add(1)
}

func streamMAC(path string) (string, bool) {
	const prefix = "/api/cameras/"
	if !strings.HasPrefix(path, prefix) {
		return "", false
	}
	parts := strings.Split(strings.TrimPrefix(path, prefix), "/")
	if len(parts) != 2 || parts[1] != "stream" || !validMAC(parts[0]) {
		return "", false
	}
	return parts[0], true
}

func writeHubError(w http.ResponseWriter, status int, code, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]any{
		"error": map[string]string{"code": code, "message": message},
	})
}
