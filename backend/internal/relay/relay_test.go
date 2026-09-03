package relay

import (
	"context"
	"encoding/json"
	"errors"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/witsaba/esp32-cam-surveillance/backend/internal/discovery"
	"github.com/witsaba/esp32-cam-surveillance/backend/internal/natsbus"
)

func TestFrameSubjectsRoundTripOnlyCanonicalMACs(t *testing.T) {
	mac := "c8f09e9d5008"
	if got := FrameSubject(mac); got != "cams."+mac+".frames" {
		t.Fatalf("FrameSubject() = %q", got)
	}
	if got, ok := FrameMAC(FrameSubject(mac)); !ok || got != mac {
		t.Fatalf("FrameMAC() = %q, %v", got, ok)
	}
	for _, subject := range []string{"cams.C8f09e9d5008.frames", "cams.bad.frames.extra", "cams.aabbccddeeff.events"} {
		if _, ok := FrameMAC(subject); ok {
			t.Fatalf("FrameMAC(%q) accepted invalid subject", subject)
		}
	}
}

func TestRelayUsesOneCameraConnectionAndPublishesRawFrames(t *testing.T) {
	runtime, err := natsbus.Start(natsbus.Config{Host: "127.0.0.1", Port: -1})
	if err != nil {
		t.Fatal(err)
	}
	defer runtime.Close()

	const mac = "c8f09e9d5008"
	registry := discovery.NewRegistry()
	registry.Upsert(discovery.Camera{MAC: mac, IP: "192.168.1.48"}, time.Now())
	conn := newFakeCameraConn()
	var dialCalls atomic.Int32
	r := New(runtime.Client(), registry, Config{
		Dial: func(context.Context, discovery.Camera) (CameraConn, error) {
			dialCalls.Add(1)
			return conn, nil
		},
		Logf: func(string, ...any) {},
	})

	sub, err := runtime.Client().SubscribeSync(FrameSubject(mac))
	if err != nil {
		t.Fatal(err)
	}
	defer sub.Unsubscribe()
	if err := r.Acquire(mac); err != nil {
		t.Fatal(err)
	}
	if err := r.Acquire(mac); err != nil {
		t.Fatal(err)
	}

	conn.push(websocket.TextMessage, []byte(`{"type":"hello","mac":"c8f09e9d5008","name":"front","description":"","fw":"1.0.0","chip":"ESP32"}`))
	select {
	case command := <-conn.writes:
		var body map[string]any
		if err := json.Unmarshal(command.data, &body); err != nil {
			t.Fatal(err)
		}
		if command.messageType != websocket.TextMessage || body["cmd"] != "stream" || body["on"] != true {
			t.Fatalf("stream command = %+v", body)
		}
	case <-time.After(time.Second):
		t.Fatal("relay did not send stream command")
	}

	conn.push(websocket.BinaryMessage, []byte{0xff, 0xd8, 0x01, 0xff, 0xd9})
	message, err := sub.NextMsg(time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if string(message.Data) != string([]byte{0xff, 0xd8, 0x01, 0xff, 0xd9}) {
		t.Fatalf("published frame = %v, want raw JPEG bytes", message.Data)
	}
	if got := dialCalls.Load(); got != 1 {
		t.Fatalf("dial calls = %d, want one shared camera connection", got)
	}

	r.Release(mac)
	r.Release(mac)
	select {
	case <-conn.closed:
	case <-time.After(time.Second):
		t.Fatal("releasing the last viewer did not close the camera connection")
	}
}

func TestParseHelloRequiresMatchingCanonicalMAC(t *testing.T) {
	tests := []struct {
		name string
		body string
		want bool
	}{
		{name: "valid", body: `{"type":"hello","mac":"c8f09e9d5008"}`, want: true},
		{name: "wrong type", body: `{"type":"status","mac":"c8f09e9d5008"}`},
		{name: "wrong mac", body: `{"type":"hello","mac":"C8F09E9D5008"}`},
		{name: "malformed", body: `{`},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := parseHello([]byte(tt.body), "c8f09e9d5008")
			if (err == nil) != tt.want {
				t.Fatalf("parseHello() error = %v, want valid=%v", err, tt.want)
			}
		})
	}
}

type fakeCameraMessage struct {
	messageType int
	data        []byte
}

type fakeCameraConn struct {
	reads  chan fakeCameraMessage
	writes chan fakeCameraMessage
	closed chan struct{}
	once   sync.Once
}

func newFakeCameraConn() *fakeCameraConn {
	return &fakeCameraConn{
		reads: make(chan fakeCameraMessage, 8), writes: make(chan fakeCameraMessage, 8), closed: make(chan struct{}),
	}
}

func (c *fakeCameraConn) ReadMessage() (int, []byte, error) {
	select {
	case message := <-c.reads:
		return message.messageType, message.data, nil
	case <-c.closed:
		return 0, nil, errors.New("closed")
	}
}

func (c *fakeCameraConn) WriteMessage(messageType int, data []byte) error {
	select {
	case c.writes <- fakeCameraMessage{messageType: messageType, data: append([]byte(nil), data...)}:
		return nil
	case <-c.closed:
		return errors.New("closed")
	}
}

func (c *fakeCameraConn) Close() error {
	c.once.Do(func() { close(c.closed) })
	return nil
}

func (c *fakeCameraConn) push(messageType int, data []byte) {
	c.reads <- fakeCameraMessage{messageType: messageType, data: data}
}
