package relay

import (
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/nats-io/nats.go"
	"github.com/witsaba/esp32-cam-surveillance/backend/internal/discovery"
	"github.com/witsaba/esp32-cam-surveillance/backend/internal/natsbus"
)

func TestHubRoutesFramesAndDropsOldestWhenViewerQueueIsFull(t *testing.T) {
	const mac = "c8f09e9d5008"
	h := &Hub{
		viewers: make(map[string]map[*viewer]struct{}),
		dropped: make(map[string]*atomic.Uint64),
	}
	v := &viewer{mac: mac, frames: make(chan []byte, viewerQueueDepth), done: make(chan struct{})}
	v.onDrop = func() { h.recordDrop(mac) }
	h.addViewer(v)

	for index := byte(0); index < viewerQueueDepth; index++ {
		h.receive(&nats.Msg{Subject: FrameSubject(mac), Data: []byte{index}})
	}
	h.receive(&nats.Msg{Subject: FrameSubject(mac), Data: []byte{99}})

	if got := h.DroppedFrames(mac); got != 1 {
		t.Fatalf("DroppedFrames() = %d, want 1", got)
	}
	if got := <-v.frames; len(got) != 1 || got[0] != 1 {
		t.Fatalf("oldest queue item = %v, want frame 1", got)
	}
}

func TestHubDuplicatesOneNATSFrameToTwoViewerConnections(t *testing.T) {
	const mac = "c8f09e9d5008"
	runtime, err := natsbus.Start(natsbus.Config{Host: "127.0.0.1", Port: -1})
	if err != nil {
		t.Fatal(err)
	}
	defer runtime.Close()
	registry := discovery.NewRegistry()
	registry.Upsert(discovery.Camera{MAC: mac, IP: "192.168.1.48"}, time.Now())
	relay := &fakeHubRelay{}
	hub, err := NewHub(runtime.Client(), registry, relay, func(string, ...any) {})
	if err != nil {
		t.Fatal(err)
	}
	defer hub.Close()
	server := httptest.NewServer(hub)
	defer server.Close()
	wsURL := "ws" + strings.TrimPrefix(server.URL, "http") + "/api/cameras/" + mac + "/stream"
	dialer := websocket.Dialer{}
	viewerOne, _, err := dialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer viewerOne.Close()
	viewerTwo, _, err := dialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer viewerTwo.Close()

	for _, viewer := range []*websocket.Conn{viewerOne, viewerTwo} {
		_ = viewer.SetReadDeadline(time.Now().Add(time.Second))
		messageType, _, err := viewer.ReadMessage()
		if err != nil || messageType != websocket.TextMessage {
			t.Fatalf("meta frame = %d, %v", messageType, err)
		}
	}
	want := []byte{0xff, 0xd8, 0x01, 0xff, 0xd9}
	if err := runtime.Client().Publish(FrameSubject(mac), want); err != nil {
		t.Fatal(err)
	}
	if err := runtime.Client().Flush(); err != nil {
		t.Fatal(err)
	}
	for _, viewer := range []*websocket.Conn{viewerOne, viewerTwo} {
		_ = viewer.SetReadDeadline(time.Now().Add(time.Second))
		messageType, got, err := viewer.ReadMessage()
		if err != nil || messageType != websocket.BinaryMessage || string(got) != string(want) {
			t.Fatalf("viewer frame = %d %v, %v; want raw frame", messageType, got, err)
		}
	}
	if got := relay.acquired.Load(); got != 2 {
		t.Fatalf("relay acquisitions = %d, want one per viewer claim", got)
	}
}

type fakeHubRelay struct {
	acquired atomic.Int32
	released atomic.Int32
	mu       sync.Mutex
}

func (r *fakeHubRelay) Acquire(string) error {
	r.acquired.Add(1)
	return nil
}

func (r *fakeHubRelay) Release(string) {
	r.mu.Lock()
	r.released.Add(1)
	r.mu.Unlock()
}
