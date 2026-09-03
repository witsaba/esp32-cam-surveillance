package natsbus

import (
	"testing"
	"time"
)

func TestStartProvidesWorkingClientConnection(t *testing.T) {
	runtime, err := Start(Config{Host: "127.0.0.1", Port: -1})
	if err != nil {
		t.Fatalf("Start() error = %v", err)
	}
	defer runtime.Close()

	subscription, err := runtime.Client().SubscribeSync("test.subject")
	if err != nil {
		t.Fatalf("SubscribeSync() error = %v", err)
	}
	defer subscription.Unsubscribe()

	want := []byte("nats is ready")
	if err := runtime.Client().Publish("test.subject", want); err != nil {
		t.Fatalf("Publish() error = %v", err)
	}
	if err := runtime.Client().FlushTimeout(time.Second); err != nil {
		t.Fatalf("FlushTimeout() error = %v", err)
	}

	message, err := subscription.NextMsg(time.Second)
	if err != nil {
		t.Fatalf("NextMsg() error = %v", err)
	}
	if string(message.Data) != string(want) {
		t.Fatalf("message = %q, want %q", message.Data, want)
	}
}
