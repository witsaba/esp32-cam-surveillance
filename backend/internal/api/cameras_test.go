package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/witsaba/esp32-cam-surveillance/backend/internal/discovery"
)

func TestCameraListIncludesDiscoveredCamerasAndProjectsIdleStatus(t *testing.T) {
	seenAt := time.Date(2026, 9, 3, 12, 0, 0, 0, time.UTC)
	registry := discovery.NewRegistry()
	registry.Upsert(discovery.Camera{
		MAC:         "c8f09e9d5008",
		IP:          "192.168.1.48",
		Name:        "front-door",
		Description: "entrance",
		Firmware:    "v5.5.3",
		Chip:        "ESP32-D0WDQ6",
	}, seenAt)
	registry.Upsert(discovery.Camera{MAC: "aabbccddeeff", IP: "192.168.1.49"}, seenAt.Add(-2*time.Minute-1*time.Second))

	handler := NewCameraHandler(registry).(*CameraHandler)
	handler.now = func() time.Time { return seenAt }
	request := httptest.NewRequest(http.MethodGet, "/api/cameras", nil)
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)

	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", recorder.Code, http.StatusOK)
	}
	var response cameraListResponse
	if err := json.NewDecoder(recorder.Body).Decode(&response); err != nil {
		t.Fatal(err)
	}
	if response.Total != 2 || len(response.Cameras) != 2 {
		t.Fatalf("response = %+v, want two cameras", response)
	}
	if response.Cameras[0].MAC != "aabbccddeeff" || response.Cameras[0].Status != statusIdle {
		t.Fatalf("first camera = %+v, want idle camera sorted by MAC", response.Cameras[0])
	}
	if response.Cameras[1].MAC != "c8f09e9d5008" || response.Cameras[1].Status != statusOnline {
		t.Fatalf("second camera = %+v, want online camera", response.Cameras[1])
	}
	if response.Cameras[1].Firmware != "v5.5.3" || response.Cameras[1].LastSeen != seenAt {
		t.Fatalf("camera identity = %+v, want discovery fields and last seen", response.Cameras[1])
	}
}

func TestCameraListStatusFilter(t *testing.T) {
	seenAt := time.Date(2026, 9, 3, 12, 0, 0, 0, time.UTC)
	registry := discovery.NewRegistry()
	registry.Upsert(discovery.Camera{MAC: "c8f09e9d5008"}, seenAt)
	registry.Upsert(discovery.Camera{MAC: "aabbccddeeff"}, seenAt.Add(-cameraIdleAfter-1))

	handler := NewCameraHandler(registry).(*CameraHandler)
	handler.now = func() time.Time { return seenAt }
	request := httptest.NewRequest(http.MethodGet, "/api/cameras?status=idle", nil)
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)

	var response cameraListResponse
	if err := json.NewDecoder(recorder.Body).Decode(&response); err != nil {
		t.Fatal(err)
	}
	if response.Total != 1 || len(response.Cameras) != 1 || response.Cameras[0].MAC != "aabbccddeeff" {
		t.Fatalf("filtered response = %+v, want only idle camera", response)
	}
}

func TestCameraListKeepsTheTwoMinuteBoundaryOnline(t *testing.T) {
	seenAt := time.Date(2026, 9, 3, 12, 0, 0, 0, time.UTC)
	registry := discovery.NewRegistry()
	registry.Upsert(discovery.Camera{MAC: "c8f09e9d5008"}, seenAt)

	handler := NewCameraHandler(registry).(*CameraHandler)
	handler.now = func() time.Time { return seenAt.Add(cameraIdleAfter) }
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/api/cameras", nil))

	var response cameraListResponse
	if err := json.NewDecoder(recorder.Body).Decode(&response); err != nil {
		t.Fatal(err)
	}
	if got := response.Cameras[0].Status; got != statusOnline {
		t.Fatalf("status at two-minute boundary = %q, want %q", got, statusOnline)
	}
}

func TestCameraListErrorsUseTheAPIEnvelope(t *testing.T) {
	handler := NewCameraHandler(discovery.NewRegistry())
	for _, test := range []struct {
		name string
		path string
		want int
	}{
		{name: "unknown route", path: "/api/unknown", want: http.StatusNotFound},
		{name: "invalid filter", path: "/api/cameras?status=unknown", want: http.StatusBadRequest},
	} {
		t.Run(test.name, func(t *testing.T) {
			recorder := httptest.NewRecorder()
			handler.ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, test.path, nil))
			if recorder.Code != test.want {
				t.Fatalf("status = %d, want %d", recorder.Code, test.want)
			}
			var envelope struct {
				Error struct {
					Code string `json:"code"`
				} `json:"error"`
			}
			if err := json.NewDecoder(recorder.Body).Decode(&envelope); err != nil {
				t.Fatal(err)
			}
			if envelope.Error.Code == "" {
				t.Fatal("error envelope has no code")
			}
		})
	}
}
