package discovery

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestHosts(t *testing.T) {
	tests := []struct {
		name string
		cidr string
		want []string
	}{
		{name: "normal subnet excludes network and broadcast", cidr: "192.168.1.0/30", want: []string{"192.168.1.1", "192.168.1.2"}},
		{name: "point to point subnet includes both addresses", cidr: "192.168.1.0/31", want: []string{"192.168.1.0", "192.168.1.1"}},
		{name: "single host network", cidr: "192.168.1.48/32", want: []string{"192.168.1.48"}},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, network, err := net.ParseCIDR(tt.cidr)
			if err != nil {
				t.Fatal(err)
			}
			got := hostStrings(network)
			if strings.Join(got, ",") != strings.Join(tt.want, ",") {
				t.Fatalf("hostStrings(%s) = %v, want %v", tt.cidr, got, tt.want)
			}
		})
	}
}

func TestNetworksFromInterfaceAddrs(t *testing.T) {
	addrs := []net.Addr{
		&net.IPNet{IP: net.ParseIP("192.168.1.20"), Mask: net.CIDRMask(24, 32)},
		&net.IPNet{IP: net.ParseIP("127.0.0.1"), Mask: net.CIDRMask(8, 32)},
		&net.IPNet{IP: net.ParseIP("2001:db8::20"), Mask: net.CIDRMask(64, 128)},
		&net.IPNet{IP: net.ParseIP("10.0.0.20"), Mask: net.CIDRMask(24, 32)},
	}

	got, err := networksFromInterfaceAddrs(addrs)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 2 {
		t.Fatalf("networksFromInterfaceAddrs() returned %d networks, want 2", len(got))
	}
	if got[0].String() != "192.168.1.0/24" || got[1].String() != "10.0.0.0/24" {
		t.Fatalf("networksFromInterfaceAddrs() = %v, want [192.168.1.0/24 10.0.0.0/24]", got)
	}
}

func TestParseWhoAmIRequiresTheCanonicalShape(t *testing.T) {
	tests := []struct {
		name string
		body string
		want string
	}{
		{name: "valid identity", body: `{"mac":"c8f09e9d5008","name":"front-door","description":"entrance","fw":"v5.5.3","chip":"ESP32-D0WDQ6"}`, want: "c8f09e9d5008"},
		{name: "missing field", body: `{"mac":"c8f09e9d5008","name":"","description":"","fw":"v5.5.3"}`},
		{name: "wrong mac case", body: `{"mac":"C8F09E9D5008","name":"","description":"","fw":"v5.5.3","chip":"ESP32-D0WDQ6"}`},
		{name: "unknown field", body: `{"mac":"c8f09e9d5008","name":"","description":"","fw":"v5.5.3","chip":"ESP32-D0WDQ6","extra":true}`},
		{name: "wrong field type", body: `{"mac":"c8f09e9d5008","name":"","description":false,"fw":"v5.5.3","chip":"ESP32-D0WDQ6"}`},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, err := parseWhoAmI(strings.NewReader(tt.body))
			if tt.want == "" {
				if err == nil {
					t.Fatal("parseWhoAmI() error = nil, want error")
				}
				return
			}
			if err != nil {
				t.Fatalf("parseWhoAmI() error = %v", err)
			}
			if got.MAC != tt.want {
				t.Fatalf("MAC = %q, want %q", got.MAC, tt.want)
			}
		})
	}
}

func TestScanRegistersValidCamerasAndRefreshesByMAC(t *testing.T) {
	registry := NewRegistry()
	cameraLogged := make(chan string, 1)
	var current int32
	var active int32
	var maxActive int32
	probe := func(ctx context.Context, ip net.IP) (Camera, error) {
		activeNow := atomic.AddInt32(&active, 1)
		defer atomic.AddInt32(&active, -1)
		for {
			oldMax := atomic.LoadInt32(&maxActive)
			if activeNow <= oldMax || atomic.CompareAndSwapInt32(&maxActive, oldMax, activeNow) {
				break
			}
		}
		time.Sleep(time.Millisecond)
		if current == 0 && ip.String() == "192.168.1.1" {
			return Camera{MAC: "c8f09e9d5008", IP: ip.String(), Name: "front-door"}, nil
		}
		if current == 1 && ip.String() == "192.168.1.2" {
			return Camera{MAC: "c8f09e9d5008", IP: ip.String(), Name: "side-door"}, nil
		}
		return Camera{}, fmt.Errorf("not a camera")
	}

	scanner := newTestScanner(t, registry, []string{"192.168.1.0/30"}, 2, probe)
	scanner.settings.Logf = func(format string, args ...any) {
		message := fmt.Sprintf(format, args...)
		if strings.Contains(message, "DISCOVERY_CAMERA:") {
			cameraLogged <- message
		}
	}
	scanner.now = func() time.Time { return time.Unix(100, 0) }
	if err := scanner.Scan(context.Background()); err != nil {
		t.Fatal(err)
	}
	current = 1
	scanner.now = func() time.Time { return time.Unix(200, 0) }
	if err := scanner.Scan(context.Background()); err != nil {
		t.Fatal(err)
	}

	camera, ok := registry.Get("c8f09e9d5008")
	if !ok {
		t.Fatal("camera was not registered")
	}
	if camera.IP != "192.168.1.2" || camera.Name != "side-door" || !camera.LastSeen.Equal(time.Unix(200, 0)) {
		t.Fatalf("camera = %+v, want refreshed IP, metadata, and last-seen", camera)
	}
	if got := len(registry.Snapshot()); got != 1 {
		t.Fatalf("registry contains %d cameras, want 1", got)
	}
	if got := atomic.LoadInt32(&maxActive); got < 2 {
		t.Fatalf("max concurrent probes = %d, want concurrent probes", got)
	}
	select {
	case message := <-cameraLogged:
		if !strings.Contains(message, "mac=c8f09e9d5008") {
			t.Fatalf("camera log = %q, want MAC", message)
		}
	case <-time.After(time.Second):
		t.Fatal("successful camera observation was not logged")
	}
}

func TestScanHonorsContextCancellation(t *testing.T) {
	registry := NewRegistry()
	started := make(chan struct{})
	probe := func(ctx context.Context, ip net.IP) (Camera, error) {
		select {
		case started <- struct{}{}:
		default:
		}
		<-ctx.Done()
		return Camera{}, ctx.Err()
	}
	scanner := newTestScanner(t, registry, []string{"192.168.1.0/24"}, 2, probe)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- scanner.Scan(ctx) }()
	select {
	case <-started:
		cancel()
	case <-time.After(time.Second):
		t.Fatal("scanner did not start a probe")
	}
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Scan() error = %v, want nil after cancellation", err)
		}
	case <-time.After(time.Second):
		t.Fatal("Scan() did not stop after cancellation")
	}
}

func TestScanSerializesOverlappingSweeps(t *testing.T) {
	registry := NewRegistry()
	entered := make(chan struct{})
	release := make(chan struct{})
	var calls atomic.Int32
	probe := func(context.Context, net.IP) (Camera, error) {
		if calls.Add(1) == 1 {
			close(entered)
			<-release
		}
		return Camera{}, fmt.Errorf("not a camera")
	}
	scanner := newTestScanner(t, registry, []string{"192.168.1.48/32"}, 1, probe)
	firstDone := make(chan struct{})
	go func() {
		_ = scanner.Scan(context.Background())
		close(firstDone)
	}()
	select {
	case <-entered:
	case <-time.After(time.Second):
		t.Fatal("first sweep did not start")
	}

	secondDone := make(chan struct{})
	go func() {
		_ = scanner.Scan(context.Background())
		close(secondDone)
	}()
	select {
	case <-secondDone:
		t.Fatal("overlapping sweep completed before the first sweep")
	case <-time.After(20 * time.Millisecond):
	}
	close(release)
	select {
	case <-firstDone:
	case <-time.After(time.Second):
		t.Fatal("first sweep did not finish")
	}
	select {
	case <-secondDone:
	case <-time.After(time.Second):
		t.Fatal("second sweep did not finish after the first sweep")
	}
}

func TestHTTPProbeRejectsNonSuccessAndMalformedResponses(t *testing.T) {
	tests := []struct {
		name    string
		status  int
		body    string
		wantErr bool
	}{
		{name: "valid response", status: http.StatusOK, body: `{"mac":"c8f09e9d5008","name":"","description":"","fw":"v5.5.3","chip":"ESP32-D0WDQ6"}`},
		{name: "server error", status: http.StatusInternalServerError, body: `{}`, wantErr: true},
		{name: "malformed identity", status: http.StatusOK, body: `{"mac":"not-a-mac"}`, wantErr: true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path != "/whoami" || r.Method != http.MethodGet {
					t.Fatalf("request = %s %s, want GET /whoami", r.Method, r.URL.Path)
				}
				w.WriteHeader(tt.status)
				_, _ = w.Write([]byte(tt.body))
			})
			listener, err := net.Listen("tcp4", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			server := &http.Server{Handler: handler}
			go func() { _ = server.Serve(listener) }()
			defer server.Close()
			_, portString, _ := net.SplitHostPort(listener.Addr().String())
			port, _ := strconv.Atoi(portString)
			probe := newHTTPProbe(&http.Client{}, port, "/whoami")
			camera, err := probe(context.Background(), net.ParseIP("127.0.0.1"))
			if tt.wantErr {
				if err == nil {
					t.Fatal("probe() error = nil, want error")
				}
				return
			}
			if err != nil || camera.MAC != "c8f09e9d5008" {
				t.Fatalf("probe() = %+v, %v", camera, err)
			}
		})
	}
}

func TestRunScansImmediatelyAndOnEachInterval(t *testing.T) {
	registry := NewRegistry()
	var scans atomic.Int32
	probe := func(context.Context, net.IP) (Camera, error) {
		scans.Add(1)
		return Camera{}, fmt.Errorf("not a camera")
	}
	scanner := newTestScanner(t, registry, []string{"192.168.1.48/32"}, 1, probe)
	scanner.settings.Interval = 10 * time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() {
		scanner.Run(ctx)
		close(done)
	}()
	deadline := time.After(time.Second)
	for scans.Load() < 2 {
		select {
		case <-deadline:
			t.Fatalf("Run() performed %d scans, want at least 2", scans.Load())
		default:
			time.Sleep(time.Millisecond)
		}
	}
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("Run() did not stop after cancellation")
	}
}

func TestSettingsFromEnvDefaultsAndValidates(t *testing.T) {
	tests := []struct {
		name    string
		env     map[string]string
		wantErr bool
	}{
		{name: "defaults", env: map[string]string{}},
		{name: "invalid workers", env: map[string]string{"DISCOVERY_WORKERS": "65"}, wantErr: true},
		{name: "invalid cidr", env: map[string]string{"DISCOVERY_CIDR": "not-cidr"}, wantErr: true},
		{name: "invalid timeout", env: map[string]string{"DISCOVERY_TIMEOUT": "not-duration"}, wantErr: true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			settings, err := SettingsFromEnv(func(key string) string { return tt.env[key] })
			if tt.wantErr {
				if err == nil {
					t.Fatal("SettingsFromEnv() error = nil, want error")
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if settings.Interval != time.Minute || settings.Workers != 64 || settings.ProbeTimeout != 500*time.Millisecond {
				t.Fatalf("settings = %+v, want one-minute interval, 64 workers, 500ms timeout", settings)
			}
		})
	}
}

func newTestScanner(t *testing.T, registry *Registry, cidrs []string, workers int, probe ProbeFunc) *Scanner {
	t.Helper()
	networks := make([]*net.IPNet, 0, len(cidrs))
	for _, cidr := range cidrs {
		_, network, err := net.ParseCIDR(cidr)
		if err != nil {
			t.Fatal(err)
		}
		networks = append(networks, network)
	}
	settings := DefaultSettings()
	settings.Workers = workers
	return newScanner(settings, registry, func() ([]*net.IPNet, error) {
		return networks, nil
	}, probe)
}
