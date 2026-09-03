// Package natsbus owns the embedded core-NATS lifecycle used by the backend.
package natsbus

import (
	"fmt"
	"time"

	"github.com/nats-io/nats-server/v2/server"
	"github.com/nats-io/nats.go"
)

const readyTimeout = 10 * time.Second

// Config contains the options needed to start the embedded NATS server.
type Config struct {
	Host       string
	Port       int
	ServerName string
}

// Runtime is an embedded NATS server and its in-process client connection.
type Runtime struct {
	server *server.Server
	client *nats.Conn
}

// Start starts an embedded NATS server and connects the backend to it.
func Start(cfg Config) (*Runtime, error) {
	if cfg.Host == "" {
		return nil, fmt.Errorf("NATS host must not be empty")
	}
	if cfg.Port < -1 || cfg.Port > 65535 {
		return nil, fmt.Errorf("NATS port must be -1 or between 0 and 65535: %d", cfg.Port)
	}
	if cfg.ServerName == "" {
		cfg.ServerName = "esp32-cam-surveillance"
	}

	options := &server.Options{
		Host:       cfg.Host,
		Port:       cfg.Port,
		ServerName: cfg.ServerName,
		NoSigs:     true,
	}
	embedded, err := server.NewServer(options)
	if err != nil {
		return nil, fmt.Errorf("create embedded NATS server: %w", err)
	}

	go embedded.Start()
	if !embedded.ReadyForConnections(readyTimeout) {
		embedded.Shutdown()
		return nil, fmt.Errorf("embedded NATS server was not ready within %s", readyTimeout)
	}

	client, err := nats.Connect(embedded.ClientURL())
	if err != nil {
		embedded.Shutdown()
		return nil, fmt.Errorf("connect backend NATS client: %w", err)
	}

	return &Runtime{server: embedded, client: client}, nil
}

// Client returns the in-process core-NATS client.
func (r *Runtime) Client() *nats.Conn {
	return r.client
}

// URL returns the client URL advertised by the embedded server.
func (r *Runtime) URL() string {
	return r.server.ClientURL()
}

// Close drains the client and shuts down the embedded server.
func (r *Runtime) Close() error {
	if r == nil {
		return nil
	}
	if r.client != nil {
		if err := r.client.Drain(); err != nil {
			r.client.Close()
			return fmt.Errorf("drain NATS client: %w", err)
		}
	}
	if r.server != nil {
		r.server.Shutdown()
		r.server.WaitForShutdown()
	}
	return nil
}
