// Command server starts the first backend runtime: an embedded core NATS
// server and a client connection owned by the process.
package main

import (
	"context"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strconv"
	"syscall"

	"github.com/witsaba/esp32-cam-surveillance/backend/internal/discovery"
	"github.com/witsaba/esp32-cam-surveillance/backend/internal/natsbus"
)

const (
	defaultNATSHost = "127.0.0.1"
	defaultNATSPort = 4222
)

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	cfg, err := loadConfig()
	if err != nil {
		log.Fatal(err)
	}

	runtime, err := natsbus.Start(natsbus.Config{
		Host:       cfg.host,
		Port:       cfg.port,
		ServerName: "esp32-cam-surveillance",
	})
	if err != nil {
		log.Fatal(err)
	}
	defer func() {
		if err := runtime.Close(); err != nil {
			log.Printf("NATS shutdown failed: %v", err)
		}
	}()

	log.Printf("embedded NATS ready at %s", runtime.URL())
	registry := discovery.NewRegistry()
	cfg.discovery.Logf = log.Printf
	discoveryScanner, err := discovery.NewScanner(cfg.discovery, registry)
	if err != nil {
		log.Fatal(err)
	}
	startDiscoveryJob(ctx, discoveryScanner)
	<-ctx.Done()
	log.Printf("shutdown signal received")
}

type config struct {
	host      string
	port      int
	discovery discovery.Settings
}

func loadConfig() (config, error) {
	host := os.Getenv("NATS_HOST")
	if host == "" {
		host = defaultNATSHost
	}

	port := defaultNATSPort
	if value := os.Getenv("NATS_PORT"); value != "" {
		parsed, err := strconv.Atoi(value)
		if err != nil || parsed < 1 || parsed > 65535 {
			return config{}, fmt.Errorf("NATS_PORT must be an integer between 1 and 65535: %q", value)
		}
		port = parsed
	}

	discoverySettings, err := discovery.SettingsFromEnv(os.Getenv)
	if err != nil {
		return config{}, err
	}
	return config{host: host, port: port, discovery: discoverySettings}, nil
}

type discoveryJob interface {
	Run(context.Context)
}

func startDiscoveryJob(ctx context.Context, job discoveryJob) {
	go job.Run(ctx)
}
