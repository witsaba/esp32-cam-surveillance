GO ?= go
BINARY ?= bin/cam-surveillance
PACKAGES := ./...

.PHONY: all build format format-check get-deps lint test vet

all: test lint build

build:
	mkdir -p $(dir $(BINARY))
	$(GO) build -trimpath -o $(BINARY) ./cmd/server

format:
	$(GO) fmt $(PACKAGES)

format-check:
	@test -z "$$($(GO)fmt -l .)" || { echo "gofmt required"; exit 1; }

get-deps:
	$(GO) mod download

lint: format-check vet

test:
	$(GO) test $(PACKAGES)

vet:
	$(GO) vet $(PACKAGES)
