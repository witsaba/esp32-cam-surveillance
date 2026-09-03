package main

import (
	"bytes"
	"context"
	"log"
	"strings"
	"testing"
	"time"
)

func TestStartDiscoveryJobRunsInBackground(t *testing.T) {
	started := make(chan struct{})
	job := fakeDiscoveryJob{started: started}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	startDiscoveryJob(ctx, job)
	select {
	case <-started:
	case <-time.After(time.Second):
		t.Fatal("discovery job did not start in the background")
	}
}

func TestCameraOnlyDiscoveryLogfFiltersNonCameraEvents(t *testing.T) {
	var output bytes.Buffer
	logger := log.New(&output, "", 0)
	logf := cameraOnlyDiscoveryLogf(logger.Printf)

	logf("DISCOVERY_SCAN: networks=192.168.1.0/24")
	logf("UNIDENTIFIED_DEVICE: ip=192.168.1.1 error=timeout")
	logf("DISCOVERY_CAMERA: mac=c8f09e9d5008 ip=192.168.1.48")

	if got := output.String(); !strings.Contains(got, "DISCOVERY_CAMERA: mac=c8f09e9d5008 ip=192.168.1.48") {
		t.Fatalf("camera log = %q, want successful discovery", got)
	}
	if strings.Contains(output.String(), "DISCOVERY_SCAN:") || strings.Contains(output.String(), "UNIDENTIFIED_DEVICE:") {
		t.Fatalf("non-camera discovery logs were emitted: %q", output.String())
	}
}

type fakeDiscoveryJob struct {
	started chan struct{}
}

func (j fakeDiscoveryJob) Run(context.Context) {
	close(j.started)
}
