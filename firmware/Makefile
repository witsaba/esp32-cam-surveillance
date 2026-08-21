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
SMOKE_LOG     ?= $(CURDIR)/build/smoke.log

# Run an idf.py subcommand with the ESP-IDF env sourced. Pass the
# subcommand + any args as the call argument, e.g.:
#   $(call idf,build)
#   $(call idf,test --target esp32)
#   $(call idf,-p $(PORT) flash)
# The arguments land INSIDE the single-quoted bash -lc command so bash
# does not strip them and idf.py sees the full invocation.
define idf
bash -lc 'source $(IDF_PATH)/export.sh >/dev/null 2>&1 && idf.py $(1)'
endef

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
	$(call idf,build)

test:
	$(call idf,test --target esp32)

test-stub:
	bash -lc 'source $(IDF_PATH)/export.sh >/dev/null 2>&1 && python tools/run_host_tests.py --stub'

flash:
	$(call idf,-p $(PORT) flash)

monitor:
	$(call idf,-p $(PORT) monitor)

smoke:
	@mkdir -p $(dir $(SMOKE_LOG))
	@./scripts/smoke.sh $(PORT) $(SMOKE_TIMEOUT) $(SMOKE_LOG)

size-components:
	$(call idf,size-components)

clean:
	$(call idf,clean)

all: clean build test smoke
