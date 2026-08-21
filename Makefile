# Makefile for the FW-02 ESP32-CAM firmware.
#
# Wraps `idf.py` for the everyday workflows so a contributor does not need
# to remember the per-target flag combinations. Default PORT matches
# `docs/firmware-prd.md` § Hardware target (the AI-Thinker ESP32-CAM
# documented on `/dev/cu.usbserial-130`).
#
# Usage:
#   make help          — show targets
#   make build         — idf.py build
#   make test          — host-side Unity tests (production)
#   make test-stub     — host-side Unity tests with FW-02.3 bite-proof stub
#   make flash         — flash firmware to $(PORT)
#   make monitor       — open serial monitor on $(PORT)
#   make smoke         — build + flash + monitor capture + nvs_get_stats verify
#   make all           — clean + build + test + smoke
#
# Variables (override on the command line):
#   PORT=/dev/cu.usbserial-130
#   IDF_PATH=$HOME/.espressif/v5.5.3/esp-idf
#   SMOKE_TIMEOUT=8          (seconds the smoke target captures monitor output)
#   SMOKE_LOG=build/smoke.log (where the captured monitor output is written)

SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c

PORT          ?= /dev/cu.usbserial-130
IDF_PATH      ?= $(HOME)/.espressif/v5.5.3/esp-idf
SMOKE_TIMEOUT ?= 8
SMOKE_LOG     ?= $(CURDIR)/firmware/build/smoke.log

# Source the ESP-IDF env inside a fresh bash login shell for each recipe so
# environment variables do not leak between targets and so contributors do
# not need to remember to `source $IDF_PATH/export.sh` themselves.
IDF_ENV = source $(IDF_PATH)/export.sh >/dev/null 2>&1
IDF = bash -lc '$(IDF_ENV) && idf.py'

.DEFAULT_GOAL := help
.PHONY: help build test test-stub flash monitor smoke clean all size-components

help:
	@echo "FW-02 ESP32-CAM firmware — make targets:"
	@echo ""
	@echo "  build            idf.py build"
	@echo "  test             host-side Unity tests (production build)"
	@echo "  test-stub        host-side Unity tests (FW-02.3 bite-proof stub)"
	@echo "  flash            idf.py -p \$$PORT flash"
	@echo "  monitor          idf.py -p \$$PORT monitor (Ctrl-] to exit)"
	@echo "  smoke            build + flash + monitor capture + nvs_get_stats verify"
	@echo "  size-components  idf.py size-components"
	@echo "  clean            idf.py clean (keeps sdkconfig)"
	@echo "  all              clean + build + test + smoke"
	@echo ""
	@echo "Variables (override on the command line):"
	@echo "  PORT=$(PORT)"
	@echo "  IDF_PATH=$(IDF_PATH)"
	@echo "  SMOKE_TIMEOUT=$(SMOKE_TIMEOUT)"
	@echo "  SMOKE_LOG=$(SMOKE_LOG)"

build:
	cd firmware && $(IDF) build

test:
	cd firmware && $(IDF) test --target esp32

test-stub:
	cd firmware && bash -lc '$(IDF_ENV) && python tools/run_host_tests.py --stub'

flash:
	cd firmware && $(IDF) -p $(PORT) flash

monitor:
	cd firmware && $(IDF) -p $(PORT) monitor

smoke:
	@mkdir -p $(dir $(SMOKE_LOG))
	@./scripts/smoke.sh $(PORT) $(SMOKE_TIMEOUT) $(SMOKE_LOG)

size-components:
	cd firmware && $(IDF) size-components

clean:
	cd firmware && $(IDF) clean

all: clean build test smoke
