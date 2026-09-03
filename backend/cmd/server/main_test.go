package main

import (
	"context"
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

type fakeDiscoveryJob struct {
	started chan struct{}
}

func (j fakeDiscoveryJob) Run(context.Context) {
	close(j.started)
}
