// Command server starts the first backend runtime: an embedded core NATS
// server and a client connection owned by the process.
package main

import (
	"context"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/witsaba/esp32-cam-surveillance/backend/internal/api"
	"github.com/witsaba/esp32-cam-surveillance/backend/internal/discovery"
	"github.com/witsaba/esp32-cam-surveillance/backend/internal/natsbus"
	"github.com/witsaba/esp32-cam-surveillance/backend/internal/relay"
)

const (
	defaultNATSHost     = "127.0.0.1"
	defaultNATSPort     = 4222
	defaultHTTPHost     = "127.0.0.1"
	defaultHTTPPort     = 8080
	defaultCameraWSPort = 80
	defaultStreamFPS    = 5
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
	cfg.discovery.Logf = cameraOnlyDiscoveryLogf(log.Printf)
	discoveryScanner, err := discovery.NewScanner(cfg.discovery, registry)
	if err != nil {
		log.Fatal(err)
	}
	startDiscoveryJob(ctx, discoveryScanner)
	cameraRelay := relay.New(runtime.Client(), registry, relay.Config{
		Port: cfg.cameraWSPort,
		Path: cfg.cameraWSPath,
		FPS:  cfg.streamFPS,
		Logf: log.Printf,
	})
	defer cameraRelay.Close()
	hub, err := relay.NewHub(runtime.Client(), registry, cameraRelay, log.Printf, cfg.streamFPS)
	if err != nil {
		log.Fatal(err)
	}
	defer hub.Close()
	mux := http.NewServeMux()
	mux.Handle("/api/cameras", api.NewCameraHandler(registry, hub))
	mux.Handle("/api/cameras/", hub)

	httpServer := &http.Server{
		Addr:    net.JoinHostPort(cfg.httpHost, strconv.Itoa(cfg.httpPort)),
		Handler: mux,
	}
	go func() {
		log.Printf("camera API listening at http://%s", httpServer.Addr)
		if err := httpServer.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Printf("camera API failed: %v", err)
			stop()
		}
	}()
	<-ctx.Done()
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := httpServer.Shutdown(shutdownCtx); err != nil {
		log.Printf("camera API shutdown failed: %v", err)
	}
	log.Printf("shutdown signal received")
}

type config struct {
	host         string
	port         int
	httpHost     string
	httpPort     int
	cameraWSPort int
	cameraWSPath string
	streamFPS    int
	discovery    discovery.Settings
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

	httpHost := os.Getenv("HTTP_HOST")
	if httpHost == "" {
		httpHost = defaultHTTPHost
	}
	httpPort := defaultHTTPPort
	if value := os.Getenv("HTTP_PORT"); value != "" {
		parsed, err := strconv.Atoi(value)
		if err != nil || parsed < 1 || parsed > 65535 {
			return config{}, fmt.Errorf("HTTP_PORT must be an integer between 1 and 65535: %q", value)
		}
		httpPort = parsed
	}

	cameraWSPort := defaultCameraWSPort
	if value := os.Getenv("CAMERA_WS_PORT"); value != "" {
		parsed, err := strconv.Atoi(value)
		if err != nil || parsed < 1 || parsed > 65535 {
			return config{}, fmt.Errorf("CAMERA_WS_PORT must be an integer between 1 and 65535: %q", value)
		}
		cameraWSPort = parsed
	}
	cameraWSPath := os.Getenv("CAMERA_WS_PATH")
	if cameraWSPath == "" {
		cameraWSPath = "/cams"
	}
	if !strings.HasPrefix(cameraWSPath, "/") {
		cameraWSPath = "/" + cameraWSPath
	}
	streamFPS := defaultStreamFPS
	if value := os.Getenv("STREAM_FPS"); value != "" {
		parsed, err := strconv.Atoi(value)
		if err != nil || parsed < 1 || parsed > 15 {
			return config{}, fmt.Errorf("STREAM_FPS must be an integer between 1 and 15: %q", value)
		}
		streamFPS = parsed
	}

	return config{
		host: host, port: port, httpHost: httpHost, httpPort: httpPort,
		cameraWSPort: cameraWSPort, cameraWSPath: cameraWSPath, streamFPS: streamFPS,
		discovery: discoverySettings,
	}, nil
}

type discoveryJob interface {
	Run(context.Context)
}

func startDiscoveryJob(ctx context.Context, job discoveryJob) {
	go job.Run(ctx)
}

func cameraOnlyDiscoveryLogf(logf func(format string, args ...any)) func(format string, args ...any) {
	return func(format string, args ...any) {
		if strings.HasPrefix(format, "DISCOVERY_CAMERA:") {
			logf(format, args...)
		}
	}
}
