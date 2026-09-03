// Package api exposes the backend's HTTP API.
package api

import (
	"encoding/json"
	"net/http"
	"time"

	"github.com/witsaba/esp32-cam-surveillance/backend/internal/discovery"
)

const cameraIdleAfter = 2 * time.Minute

const (
	statusOnline = "online"
	statusIdle   = "idle"
)

// CameraHandler serves read-only camera registry endpoints.
type CameraHandler struct {
	registry *discovery.Registry
	stats    CameraStats
	now      func() time.Time
}

// CameraStats supplies live relay counters without coupling the registry to
// the streaming package.
type CameraStats interface {
	DroppedFrames(mac string) uint64
}

// NewCameraHandler creates an HTTP handler backed by the in-memory registry.
func NewCameraHandler(registry *discovery.Registry, stats ...CameraStats) http.Handler {
	var cameraStats CameraStats
	if len(stats) > 0 {
		cameraStats = stats[0]
	}
	return &CameraHandler{registry: registry, stats: cameraStats, now: time.Now}
}

// ServeHTTP serves GET /api/cameras and its status-filtered variants.
func (h *CameraHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/api/cameras" {
		writeError(w, http.StatusNotFound, "not_found", "endpoint not found")
		return
	}
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method_not_allowed", "method not allowed")
		return
	}

	filter, ok := cameraStatusFilter(r)
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid_status", "status must be online or idle")
		return
	}

	now := time.Now()
	if h != nil && h.now != nil {
		now = h.now()
	}
	items := make([]cameraResponse, 0)
	if h != nil && h.registry != nil {
		for _, camera := range h.registry.Snapshot() {
			status := cameraStatus(camera.LastSeen, now)
			if filter != "" && filter != status {
				continue
			}
			items = append(items, cameraResponse{
				MAC:         camera.MAC,
				IP:          camera.IP,
				Name:        camera.Name,
				Description: camera.Description,
				Firmware:    camera.Firmware,
				Chip:        camera.Chip,
				LastSeen:    camera.LastSeen,
				Status:      status,
				Dropped:     h.droppedFrames(camera.MAC),
			})
		}
	}

	writeJSON(w, http.StatusOK, cameraListResponse{Cameras: items, Total: len(items)})
}

type cameraResponse struct {
	MAC         string    `json:"mac"`
	IP          string    `json:"ip"`
	Name        string    `json:"name"`
	Description string    `json:"description"`
	Firmware    string    `json:"fw_version"`
	Chip        string    `json:"chip"`
	LastSeen    time.Time `json:"last_seen_at"`
	Status      string    `json:"status"`
	Dropped     uint64    `json:"dropped_frames"`
}

type cameraListResponse struct {
	Cameras []cameraResponse `json:"cameras"`
	Total   int              `json:"total"`
}

func cameraStatusFilter(r *http.Request) (string, bool) {
	values, exists := r.URL.Query()["status"]
	if !exists || len(values) == 0 || values[0] == "" {
		return "", len(values) <= 1
	}
	if len(values) != 1 || (values[0] != statusOnline && values[0] != statusIdle) {
		return "", false
	}
	return values[0], true
}

func cameraStatus(lastSeen, now time.Time) string {
	if now.Sub(lastSeen) > cameraIdleAfter {
		return statusIdle
	}
	return statusOnline
}

func (h *CameraHandler) droppedFrames(mac string) uint64 {
	if h == nil || h.stats == nil {
		return 0
	}
	return h.stats.DroppedFrames(mac)
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}

func writeError(w http.ResponseWriter, status int, code, message string) {
	writeJSON(w, status, map[string]any{
		"error": map[string]string{
			"code":    code,
			"message": message,
		},
	})
}
