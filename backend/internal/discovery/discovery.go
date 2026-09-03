// Package discovery finds ESP32 cameras exposed on the local IPv4 networks.
package discovery

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	defaultDiscoveryInterval = time.Minute
	defaultProbeTimeout      = 500 * time.Millisecond
	defaultDiscoveryPort     = 80
	defaultDiscoveryWorkers  = 64
	maxDiscoveryWorkers      = 64
	defaultDiscoveryPath     = "/whoami"
)

var macPattern = regexp.MustCompile(`^[0-9a-f]{12}$`)

// Camera is the identity observed from a camera's /whoami endpoint.
type Camera struct {
	MAC         string
	IP          string
	Name        string
	Description string
	Firmware    string
	Chip        string
	LastSeen    time.Time
}

// Registry is the in-memory camera database. MAC is the stable identity key;
// an IP is only the camera's most recently observed network address.
type Registry struct {
	mu      sync.RWMutex
	cameras map[string]Camera
}

// NewRegistry creates an empty in-memory camera registry.
func NewRegistry() *Registry {
	return &Registry{cameras: make(map[string]Camera)}
}

// Upsert records an observation, replacing the address and metadata for the
// same MAC while always refreshing LastSeen.
func (r *Registry) Upsert(camera Camera, seenAt time.Time) {
	if r == nil || camera.MAC == "" {
		return
	}
	camera.LastSeen = seenAt
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.cameras == nil {
		r.cameras = make(map[string]Camera)
	}
	r.cameras[camera.MAC] = camera
}

// Get returns the current record for mac.
func (r *Registry) Get(mac string) (Camera, bool) {
	if r == nil {
		return Camera{}, false
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	camera, ok := r.cameras[mac]
	return camera, ok
}

// Snapshot returns a deterministic copy of the current registry.
func (r *Registry) Snapshot() []Camera {
	if r == nil {
		return nil
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	result := make([]Camera, 0, len(r.cameras))
	for _, camera := range r.cameras {
		result = append(result, camera)
	}
	sort.Slice(result, func(i, j int) bool { return result[i].MAC < result[j].MAC })
	return result
}

// Settings controls the discovery sweep and its startup schedule.
type Settings struct {
	CIDR         string
	Interval     time.Duration
	Port         int
	Path         string
	ProbeTimeout time.Duration
	Workers      int
	Logf         func(format string, args ...any)
}

// DefaultSettings returns the safe default discovery policy.
func DefaultSettings() Settings {
	return Settings{
		Interval:     defaultDiscoveryInterval,
		Port:         defaultDiscoveryPort,
		Path:         defaultDiscoveryPath,
		ProbeTimeout: defaultProbeTimeout,
		Workers:      defaultDiscoveryWorkers,
	}
}

// SettingsFromEnv loads optional discovery settings without requiring a
// configuration file. An empty CIDR means the active, non-loopback IPv4
// interface networks are detected for every sweep.
func SettingsFromEnv(getenv func(string) string) (Settings, error) {
	settings := DefaultSettings()
	settings.CIDR = getenv("DISCOVERY_CIDR")
	if settings.CIDR != "" {
		_, network, err := net.ParseCIDR(settings.CIDR)
		if err != nil || network.IP.To4() == nil {
			return Settings{}, fmt.Errorf("DISCOVERY_CIDR must be an IPv4 CIDR: %q", settings.CIDR)
		}
	}

	var err error
	if value := getenv("DISCOVERY_INTERVAL"); value != "" {
		settings.Interval, err = time.ParseDuration(value)
		if err != nil || settings.Interval <= 0 {
			return Settings{}, fmt.Errorf("DISCOVERY_INTERVAL must be a positive duration: %q", value)
		}
	}
	if value := getenv("DISCOVERY_TIMEOUT"); value != "" {
		settings.ProbeTimeout, err = time.ParseDuration(value)
		if err != nil || settings.ProbeTimeout <= 0 {
			return Settings{}, fmt.Errorf("DISCOVERY_TIMEOUT must be a positive duration: %q", value)
		}
	}
	if value := getenv("DISCOVERY_PORT"); value != "" {
		settings.Port, err = strconv.Atoi(value)
		if err != nil || settings.Port < 1 || settings.Port > 65535 {
			return Settings{}, fmt.Errorf("DISCOVERY_PORT must be an integer between 1 and 65535: %q", value)
		}
	}
	if value := getenv("DISCOVERY_WORKERS"); value != "" {
		settings.Workers, err = strconv.Atoi(value)
		if err != nil || settings.Workers < 1 || settings.Workers > maxDiscoveryWorkers {
			return Settings{}, fmt.Errorf("DISCOVERY_WORKERS must be between 1 and %d: %q", maxDiscoveryWorkers, value)
		}
	}
	return settings, nil
}

// ProbeFunc probes one IP and returns a validated camera observation.
type ProbeFunc func(ctx context.Context, ip net.IP) (Camera, error)

type networkProvider func() ([]*net.IPNet, error)

// Scanner performs one bounded sweep at a time.
type Scanner struct {
	settings        Settings
	registry        *Registry
	networkProvider networkProvider
	probe           ProbeFunc
	now             func() time.Time
	scanMu          chan struct{}
}

// NewScanner creates a scanner that derives its scan ranges from active local
// interfaces unless Settings.CIDR explicitly constrains the range.
func NewScanner(settings Settings, registry *Registry) (*Scanner, error) {
	settings, err := normalizeSettings(settings)
	if err != nil {
		return nil, err
	}
	return newScanner(settings, registry, localIPv4Networks, newHTTPProbe(&http.Client{}, settings.Port, settings.Path)), nil
}

func newScanner(settings Settings, registry *Registry, networks networkProvider, probe ProbeFunc) *Scanner {
	return &Scanner{
		settings:        settings,
		registry:        registry,
		networkProvider: networks,
		probe:           probe,
		now:             time.Now,
		scanMu: func() chan struct{} {
			lock := make(chan struct{}, 1)
			lock <- struct{}{}
			return lock
		}(),
	}
}

func normalizeSettings(settings Settings) (Settings, error) {
	defaults := DefaultSettings()
	if settings.Interval == 0 {
		settings.Interval = defaults.Interval
	}
	if settings.Port == 0 {
		settings.Port = defaults.Port
	}
	if settings.Path == "" {
		settings.Path = defaults.Path
	}
	if !strings.HasPrefix(settings.Path, "/") {
		settings.Path = "/" + settings.Path
	}
	if settings.ProbeTimeout == 0 {
		settings.ProbeTimeout = defaults.ProbeTimeout
	}
	if settings.Workers == 0 {
		settings.Workers = defaults.Workers
	}
	if settings.Interval <= 0 {
		return Settings{}, fmt.Errorf("discovery interval must be positive: %s", settings.Interval)
	}
	if settings.Port < 1 || settings.Port > 65535 {
		return Settings{}, fmt.Errorf("discovery port must be between 1 and 65535: %d", settings.Port)
	}
	if settings.ProbeTimeout <= 0 {
		return Settings{}, fmt.Errorf("probe timeout must be positive: %s", settings.ProbeTimeout)
	}
	if settings.Workers < 1 || settings.Workers > maxDiscoveryWorkers {
		return Settings{}, fmt.Errorf("discovery workers must be between 1 and %d: %d", maxDiscoveryWorkers, settings.Workers)
	}
	if settings.CIDR != "" {
		_, network, err := net.ParseCIDR(settings.CIDR)
		if err != nil || network.IP.To4() == nil {
			return Settings{}, fmt.Errorf("discovery CIDR must be an IPv4 CIDR: %q", settings.CIDR)
		}
	}
	return settings, nil
}

// Scan probes every usable host in the configured or locally detected IPv4
// networks. Workers send only successful observations over found; the single
// collector goroutine owns registry writes.
func (s *Scanner) Scan(ctx context.Context) error {
	select {
	case <-s.scanMu:
		defer func() { s.scanMu <- struct{}{} }()
	case <-ctx.Done():
		return nil
	}

	networks, err := s.scanNetworks()
	if err != nil {
		s.logf("DISCOVERY_SCAN_FAILED: %v", err)
		return err
	}
	if len(networks) == 0 {
		s.logf("DISCOVERY_SKIPPED: no routable IPv4 network")
		return nil
	}
	ranges := make([]string, 0, len(networks))
	for _, network := range networks {
		if network != nil {
			ranges = append(ranges, network.String())
		}
	}
	s.logf("DISCOVERY_SCAN: networks=%s", strings.Join(ranges, ","))

	ips := make(chan net.IP)
	found := make(chan Camera)
	var workers sync.WaitGroup

	go func() {
		defer close(ips)
		for _, network := range networks {
			if err := forEachHost(ctx, network, func(ip net.IP) bool {
				select {
				case ips <- ip:
					return true
				case <-ctx.Done():
					return false
				}
			}); err != nil {
				return
			}
		}
	}()

	for i := 0; i < s.settings.Workers; i++ {
		workers.Add(1)
		go func() {
			defer workers.Done()
			for ip := range ips {
				probeCtx, cancel := context.WithTimeout(ctx, s.settings.ProbeTimeout)
				camera, probeErr := s.probe(probeCtx, ip)
				cancel()
				if probeErr != nil {
					if ctx.Err() == nil {
						s.logf("UNIDENTIFIED_DEVICE: ip=%s error=%v", ip, probeErr)
					}
					continue
				}
				select {
				case found <- camera:
				case <-ctx.Done():
					return
				}
			}
		}()
	}
	go func() {
		workers.Wait()
		close(found)
	}()

	for camera := range found {
		s.logf("DISCOVERY_CAMERA: mac=%s ip=%s", camera.MAC, camera.IP)
		s.registry.Upsert(camera, s.now())
	}
	return nil
}

// Run performs an immediate sweep, then runs one non-overlapping sweep per
// interval until ctx is canceled.
func (s *Scanner) Run(ctx context.Context) {
	if err := s.Scan(ctx); err != nil && ctx.Err() == nil {
		s.logf("DISCOVERY_RUN_FAILED: %v", err)
	}
	ticker := time.NewTicker(s.settings.Interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			if err := s.Scan(ctx); err != nil && ctx.Err() == nil {
				s.logf("DISCOVERY_RUN_FAILED: %v", err)
			}
		}
	}
}

func (s *Scanner) scanNetworks() ([]*net.IPNet, error) {
	if s.settings.CIDR != "" {
		_, network, err := net.ParseCIDR(s.settings.CIDR)
		if err != nil {
			return nil, err
		}
		return []*net.IPNet{network}, nil
	}
	return s.networkProvider()
}

func (s *Scanner) logf(format string, args ...any) {
	if s.settings.Logf != nil {
		s.settings.Logf(format, args...)
	}
}

func localIPv4Networks() ([]*net.IPNet, error) {
	interfaces, err := net.Interfaces()
	if err != nil {
		return nil, err
	}
	var addresses []net.Addr
	for _, iface := range interfaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		ifaceAddresses, err := iface.Addrs()
		if err != nil {
			return nil, err
		}
		addresses = append(addresses, ifaceAddresses...)
	}
	return networksFromInterfaceAddrs(addresses)
}

func networksFromInterfaceAddrs(addresses []net.Addr) ([]*net.IPNet, error) {
	result := make([]*net.IPNet, 0, len(addresses))
	seen := make(map[string]struct{})
	for _, address := range addresses {
		ipNet, ok := address.(*net.IPNet)
		if !ok {
			_, ipNet, err := net.ParseCIDR(address.String())
			if err != nil {
				continue
			}
			ipNet.IP = net.ParseIP(strings.Split(address.String(), "/")[0])
		}
		ip := ipNet.IP.To4()
		if ip == nil || ip.IsLoopback() {
			continue
		}
		mask, bits := ipNet.Mask.Size()
		if bits != 32 {
			continue
		}
		network := &net.IPNet{IP: ip.Mask(net.CIDRMask(mask, 32)), Mask: net.CIDRMask(mask, 32)}
		if _, ok := seen[network.String()]; ok {
			continue
		}
		seen[network.String()] = struct{}{}
		result = append(result, network)
	}
	return result, nil
}

func forEachHost(ctx context.Context, network *net.IPNet, visit func(net.IP) bool) error {
	if network == nil {
		return nil
	}
	ip := network.IP.To4()
	if ip == nil {
		return nil
	}
	prefix, bits := network.Mask.Size()
	if bits != 32 {
		return nil
	}
	base := uint64(ipToUint32(ip.Mask(network.Mask)))
	total := uint64(1) << uint(32-prefix)
	start, end := uint64(0), total
	if prefix <= 30 {
		start, end = 1, total-1
	}
	for offset := start; offset < end; offset++ {
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
		}
		if !visit(uint32ToIP(uint32(base + offset))) {
			return nil
		}
	}
	return nil
}

func hostStrings(network *net.IPNet) []string {
	var result []string
	_ = forEachHost(context.Background(), network, func(ip net.IP) bool {
		result = append(result, ip.String())
		return true
	})
	return result
}

func ipToUint32(ip net.IP) uint32 {
	return uint32(ip[0])<<24 | uint32(ip[1])<<16 | uint32(ip[2])<<8 | uint32(ip[3])
}

func uint32ToIP(value uint32) net.IP {
	return net.IPv4(byte(value>>24), byte(value>>16), byte(value>>8), byte(value))
}

type whoAmI struct {
	MAC         string `json:"mac"`
	Name        string `json:"name"`
	Description string `json:"description"`
	Firmware    string `json:"fw"`
	Chip        string `json:"chip"`
}

func parseWhoAmI(body io.Reader) (Camera, error) {
	var raw map[string]json.RawMessage
	decoder := json.NewDecoder(body)
	if err := decoder.Decode(&raw); err != nil {
		return Camera{}, fmt.Errorf("decode identity: %w", err)
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return Camera{}, fmt.Errorf("identity must contain one JSON object")
	}
	const fieldCount = 5
	if len(raw) != fieldCount {
		return Camera{}, fmt.Errorf("identity must contain exactly %d fields", fieldCount)
	}
	for _, field := range []string{"mac", "name", "description", "fw", "chip"} {
		value, ok := raw[field]
		if !ok || len(value) == 0 || value[0] != '"' {
			return Camera{}, fmt.Errorf("identity field %q must be a string", field)
		}
	}
	var identity whoAmI
	encoded, err := json.Marshal(raw)
	if err != nil {
		return Camera{}, fmt.Errorf("encode identity fields: %w", err)
	}
	if err := json.Unmarshal(encoded, &identity); err != nil {
		return Camera{}, fmt.Errorf("decode identity fields: %w", err)
	}
	if !macPattern.MatchString(identity.MAC) {
		return Camera{}, fmt.Errorf("identity MAC %q is not 12 lowercase hexadecimal characters", identity.MAC)
	}
	return Camera{MAC: identity.MAC, Name: identity.Name, Description: identity.Description, Firmware: identity.Firmware, Chip: identity.Chip}, nil
}

func newHTTPProbe(client *http.Client, port int, path string) ProbeFunc {
	return func(ctx context.Context, ip net.IP) (Camera, error) {
		url := "http://" + net.JoinHostPort(ip.String(), strconv.Itoa(port)) + path
		request, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
		if err != nil {
			return Camera{}, err
		}
		response, err := client.Do(request)
		if err != nil {
			return Camera{}, err
		}
		defer response.Body.Close()
		if response.StatusCode != http.StatusOK {
			return Camera{}, fmt.Errorf("unexpected status %s", response.Status)
		}
		camera, err := parseWhoAmI(io.LimitReader(response.Body, 64*1024))
		if err != nil {
			return Camera{}, err
		}
		camera.IP = ip.String()
		return camera, nil
	}
}
