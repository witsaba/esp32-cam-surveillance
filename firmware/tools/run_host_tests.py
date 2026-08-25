#!/usr/bin/env python3
"""run_host_tests.py — host-side Unity test runner for FW-02, FW-03,
FW-05, FW-06, FW-07, FW-08, FW-10, and FW-11.

Builds and runs the in-tree host tests against the in-memory
mock surface. Ten compile passes:

  Pass 1 — production build (no stub flags): all FW-02/FW-03/FW-05
           + FW-06 + FW-07 + FW-08 + FW-10 + FW-11 tests must pass
           (current count: 101 — 89 baseline + 7 FW-10 +
           5 FW-11 capture-loop + 1 FW-11 getter).

  Pass 2 — stub build (-DCONFIG_TEST_STUB_VERSION_CHECK): the FW-02.3
           bite-proof MUST FAIL with 'schema_version' in the message.

  Pass 3 — stub build (-DBOOT_TEST_STUB_FLIP_DECISION): the FW-03.4
           bite-proof MUST FAIL with 'determinism' in the message.

  Pass 4 — stub build (-DSOFTAP_TEST_STUB_ACCEPT_ALL_BODIES): the
           FW-05.4 bite-proof MUST FAIL with 'validation' in the
           message (3 fail + 3 pass).

  Pass 5 — stub build (-DLED_TEST_STUB_DISABLE_TIMER): the FW-06.4
           bite-proof MUST FAIL with 'timer_fire' in the message
           (process aborts via the guard tripwire).

  Pass 6 — stub build (-DBUTTON_TEST_STUB_DISABLE_DEBOUNCE): the
           FW-07.4 bite-proof MUST FAIL with 'debounce' in the
           message (test fires via TEST_FAIL_MESSAGE on jitter-
           induced phantom edges).

  Pass 7 — stub build (-DWIFI_TEST_STUB_USE_BLOCKING_WAIT): the
           FW-08.3 bite-proof MUST FAIL with 'bounded_wait' in
           the message. Wired in T-08-D.

  Pass 8 — stub build (-DWIFI_TEST_STUB_SKIP_IP_UP_HANDLER): the
           FW-08.6 bite-proof MUST FAIL with 'teardown' in
           the message. Wired in T-08-G.

  Pass 9 — stub build (-DCAMERA_TEST_STUB_REINIT): the FW-10.3
           no-reinit guard's bite-proof MUST FAIL with 'no_reinit'
           in the message.

  Pass 10 — stub build (-DCAPTURE_TEST_STUB_SECOND_CALLER): the
           FW-11.3 single-owner guard's bite-proof MUST FAIL
           with 'single_owner' in the message. Wired in this
           PR (FW-11 apply cycle).

  Pass 12 — stub build (-DWS_TEST_STUB_ENABLE_CLOSE_RECONNECT): the
           FW-14 clean-CLOSE sleep-invariant guard's bite-proof
           MUST FAIL with 'close_no_reconnect' in the message.
           Wired in the FW-14 apply cycle.

Exits 0 only when all passes satisfy their expected outcomes.
Exits 1 on any unexpected pass/fail pattern (regression in either
direction).

Why this exists: ESP-IDF v5.5.3's stock `idf.py` does not ship a
host-side Unity test runner for the esp32 target — the canonical
host test pattern uses a separate linux-targeted Catch2 project
under each component's `host_test/`/` directory. We keep the
tests inside the firmware project and compile them on the host
via plain gcc. The macro-override link headers (mock_*_link.h)
mean production source's IDF calls redirect to the in-memory
mocks — no real flash required.

Usage:
    python3 tools/run_host_tests.py [firmware/project/dir]

Dependencies (host machine):
    - gcc, g++  (any recent version supporting C11 + C++17)
    - Python 3 (only for this driver script)

Test layer: Unit (host). No IDF runtime; no FreeRTOS; no QEMU.
"""

import os
import shutil
import subprocess
import sys
import tempfile


# Resolve the firmware project directory (where idf.py lives).
PROJECT_DIR = os.path.abspath(
    sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('--')
    else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
)

IDF_PATH = os.environ.get('IDF_PATH')
if not IDF_PATH:
    sys.exit("ERROR: IDF_PATH environment variable is not set")

UNITY_SRC = os.path.join(IDF_PATH, 'components', 'unity', 'unity', 'src', 'unity.c')
UNITY_INTERNALS = os.path.join(IDF_PATH, 'components', 'unity', 'unity', 'src')
NVS_INCLUDE = os.path.join(IDF_PATH, 'components', 'nvs_flash', 'include')


def _find_compiler():
    """Locate host gcc and g++ (skip the IDF xtensa toolchain)."""
    cc = os.environ.get('CC', 'cc')
    cxx = os.environ.get('CXX', 'c++')
    if not shutil.which(cc):
        sys.exit(f"ERROR: C compiler '{cc}' not found on PATH")
    if not shutil.which(cxx):
        sys.exit(f"ERROR: C++ compiler '{cxx}' not found on PATH")
    return cc, cxx


def _run(cmd, **kwargs):
    """Run a subprocess and stream output. Exits on non-zero rc."""
    print(f"  $ {' '.join(cmd)}")
    result = subprocess.run(cmd, **kwargs)
    if result.returncode != 0:
        sys.exit(f"FAIL: command exited {result.returncode}: {' '.join(cmd)}")


def _common_cflags(extra_defines):
    """The host-test include + define baseline."""
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    host_include = os.path.join(PROJECT_DIR, 'tests', 'host_include')
    esp_common_include = os.path.join(IDF_PATH, 'components', 'esp_common', 'include')
    log_include = os.path.join(IDF_PATH, 'components', 'log', 'include')
    json_cjson_include = os.path.join(IDF_PATH, 'components', 'json', 'cJSON')
    flags = [
        '-std=c11',
        '-Wall', '-Wextra', '-Wno-unused-parameter',
        f'-I{host_include}',                      # unity_config.h (host shim)
        f'-I{tools_dir}',                         # unity_host_test_runner.h
        f'-I{UNITY_INTERNALS}',                   # unity_internals.h, unity.h
        f'-I{NVS_INCLUDE}',                       # nvs.h types
        f'-I{esp_common_include}',                # esp_err.h
        f'-I{log_include}',                       # esp_log.h
        f'-I{json_cjson_include}',                # cJSON.h (IDF location)
        f'-I{PROJECT_DIR}/components/config/include',
        f'-I{PROJECT_DIR}/components/boot/include',
        f'-I{PROJECT_DIR}/components/mocks/include',
        f'-I{PROJECT_DIR}/components/softap/include',
        f'-I{PROJECT_DIR}/components/led/include',
        f'-I{PROJECT_DIR}/components/button/include',
        # FW-08 — wifi component public headers.
        f'-I{PROJECT_DIR}/components/wifi/include',
        # FW-10 — camera component public headers.
        f'-I{PROJECT_DIR}/components/camera/include',
        # FW-11 — capture component public headers.
        f'-I{PROJECT_DIR}/components/capture/include',
        # FW-13 — identity + ws component public headers.
        f'-I{PROJECT_DIR}/components/identity/include',
        f'-I{PROJECT_DIR}/components/ws/include',
        # FW-15 — stream component public headers.
        f'-I{PROJECT_DIR}/components/stream/include',
        # FW-16 — health component public headers (window core +
        # task surface).
        f'-I{PROJECT_DIR}/components/health/include',
        '-DUNITY_INCLUDE_CONFIG_H',
        '-DUNITY_HOST_BUILD',                     # select host test_runner shim
        # FW-08 — Kconfig mirrors for the host build (the device
        # build resolves these via sdkconfig; the host has no
        # sdkconfig.h so we set the defaults that mirror
        # Kconfig.projbuild:48-50). CONFIG_FIRMWARE_PROVISIONING
        # _AP_STOP_ON_CONNECT=y enables the FW-08.4 softAP teardown
        # branch. The test_wifi_event_teardown.c file's S2 is
        # gated by `#ifndef CONFIG_FIRMWARE_PROVISIONING_AP_STOP
        # _ON_CONNECT` so it does not compile under the default
        # Pass 1 build.
        '-DCONFIG_FIRMWARE_PROVISIONING_AP_STOP_ON_CONNECT=1',
        # FW-10 — camera Kconfig mirrors for the host build.
        # Device builds resolve these via sdkconfig; the host has
        # no sdkconfig.h so we set the FW-02 defaults that match
        # firmware/sdkconfig.defaults:28-29 + Kconfig.projbuild:6,14.
        '-DCONFIG_FIRMWARE_CAMERA_JPEG_QUALITY=18',
        '-DCONFIG_FIRMWARE_CAMERA_FRAME_SIZE=5',
        # FW-10 follow-up (commit 9188c31) — camera.c:181 references
        # the IDF-side `CAMERA_FB_IN_PSRAM` constant directly (not
        # via a CONFIG_* Kconfig symbol). The host mock declares
        # `camera_config_t.fb_location` as `int` but does not
        # provide this IDF constant, so we mirror its value here.
        # Value 2 per esp_camera.h (IDF v5.5.3).
        '-DCAMERA_FB_IN_PSRAM=2',
        # FW-13 — identity component Kconfig mirrors (T-13-C).
        # Mirror components/identity/Kconfig defaults so the host
        # runner's -D cflag set matches the device sdkconfig.defaults
        # (no sdkconfig.h on host).
        '-DCONFIG_FIRMWARE_IDENTITY_NAME_MAX_LEN=32',
        '-DCONFIG_FIRMWARE_IDENTITY_DESCRIPTION_MAX_LEN=64',
        # FW-13/FW-16 — ws component Kconfig mirrors. Mirror
        # components/ws/Kconfig defaults so the host runner's
        # -D cflag set matches the device sdkconfig.defaults
        # (no sdkconfig.h on host). The endpoint path is a
        # string literal in the C source, so we use a quoted
        # value. Server mode: the outbound-client knobs
        # (URI/ping/pong/timeout/task-stack) are gone.
        '-DCONFIG_FIRMWARE_WS_PATH="/cams"',
        '-DCONFIG_FIRMWARE_WS_BUFFER_SIZE=16384',
        '-DCONFIG_FIRMWARE_WS_STATUS_PERIOD_MS=30000',
        # FW-14 — reconnect backoff Kconfig mirrors. The symbols live
        # in main/Kconfig.projbuild:41-49 (NOT components/ws/Kconfig);
        # sdkconfig.defaults:32-33 already carries both defaults. The
        # host build has no sdkconfig.h so we mirror them via -D.
        '-DCONFIG_FIRMWARE_WS_RECONNECT_INITIAL_MS=2000',
        '-DCONFIG_FIRMWARE_WS_RECONNECT_CAP_MS=30000',
        # FW-16 — soft-recovery Kconfig mirrors (main/Kconfig
        # .projbuild:51-59; sdkconfig.defaults carries both).
        '-DCONFIG_FIRMWARE_SOFT_RECOVERY_FAILS=30',
        '-DCONFIG_FIRMWARE_SOFT_RECOVERY_WINDOW_MIN=10',
    ]
    flags.extend(extra_defines)
    return flags


def _build(basename, extra_defines, test_files, workdir):
    """Build one host test binary. Returns the binary path.

    `extra_defines` apply to ALL sources (cc, cxx, tests).
    `test_files` is the explicit list of test_*.c files to include.
    """
    all_sources = [
        UNITY_SRC,
        os.path.join(IDF_PATH, 'components', 'json', 'cJSON', 'cJSON.c'),
        os.path.join(PROJECT_DIR, 'components', 'config', 'config.c'),
        os.path.join(PROJECT_DIR, 'components', 'softap', 'softap.c'),
        os.path.join(PROJECT_DIR, 'components', 'softap', 'softap_handlers.c'),
        os.path.join(PROJECT_DIR, 'components', 'softap', 'softap_home.c'),
        # FW-05.5 — STA-bound /whoami listener (always-on httpd on
        # the station interface, started on IP_EVENT_STA_GOT_IP).
        os.path.join(PROJECT_DIR, 'components', 'softap', 'softap_sta_listener.c'),
        os.path.join(PROJECT_DIR, 'components', 'led', 'led.c'),
        os.path.join(PROJECT_DIR, 'components', 'button', 'button.c'),
        # FW-08 — wifi component (connect driver + event handlers).
        os.path.join(PROJECT_DIR, 'components', 'wifi', 'wifi.c'),
        os.path.join(PROJECT_DIR, 'components', 'wifi', 'wifi_event.c'),
        # FW-10 — camera component (init + runtime setter + fake
        # settings source).
        os.path.join(PROJECT_DIR, 'components', 'camera', 'camera.c'),
        os.path.join(PROJECT_DIR, 'components', 'camera', 'camera_settings.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_nvs_flash.cpp'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_boot_button.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_init_returns.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_supervision_record.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_log.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_esp_wifi.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_esp_netif.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_esp_event.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_http_server.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_esp_system.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_gpio.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_esp_timer.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_config.c'),
        # FW-08 — softap mock (mirrors mock_esp_wifi shape).
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_softap.c'),
        # FW-10 — esp32-camera mock triplet.
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_esp_camera.c'),
        # FW-13 — esp_websocket_client mock (added in T-13-B;
        # mirrored from the FW-08 mock_esp_wifi pattern).
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_esp_websocket_client.c'),
        # FW-11 — capture component (pure loop body + FreeRTOS wrapper).
        os.path.join(PROJECT_DIR, 'components', 'capture', 'capture.c'),
        # FW-15 — stream component (planner + sender + task loop;
        # sources register as each lands).
        os.path.join(PROJECT_DIR, 'components', 'stream', 'stream_fragment.c'),
        os.path.join(PROJECT_DIR, 'components', 'stream', 'stream_sender.c'),
        os.path.join(PROJECT_DIR, 'components', 'stream', 'stream.c'),
        # FW-16 — health component (pure window core + task glue;
        # sources register as each lands).
        os.path.join(PROJECT_DIR, 'components', 'health', 'health_window.c'),
        os.path.join(PROJECT_DIR, 'components', 'health', 'health.c'),
        # FW-13 — identity component (shared MAC + NVS identity).
        os.path.join(PROJECT_DIR, 'components', 'identity', 'identity.c'),
        # FW-13 — ws component. FW-16 server mode: ws_server.c
        # (the /cams endpoint) joins the build; the retained
        # outbound-client sources stay so the isolated FW-13/FW-14
        # suites keep compiling against the mock.
        os.path.join(PROJECT_DIR, 'components', 'ws', 'ws.c'),
        os.path.join(PROJECT_DIR, 'components', 'ws', 'ws_server.c'),
        os.path.join(PROJECT_DIR, 'components', 'ws', 'ws_text_frame.c'),
        os.path.join(PROJECT_DIR, 'components', 'ws', 'ws_event_handler.c'),
        os.path.join(PROJECT_DIR, 'components', 'ws', 'ws_status_timer.c'),
        os.path.join(PROJECT_DIR, 'components', 'ws', 'ws_runtime_metrics.c'),
        os.path.join(PROJECT_DIR, 'components', 'ws', 'ws_reconnects.c'),
        # FW-14 — reconnect backoff module (retained; unwired from
        # the active path in server mode).
        os.path.join(PROJECT_DIR, 'components', 'ws', 'ws_backoff.c'),
        os.path.join(PROJECT_DIR, 'components', 'boot', 'boot.c'),
        os.path.join(PROJECT_DIR, 'components', 'boot', 'boot_button_stub.c'),
        os.path.join(PROJECT_DIR, 'components', 'boot', 'stub_inits.c'),
        os.path.join(PROJECT_DIR, 'components', 'boot', 'stub_supervision.c'),
        os.path.join(os.path.dirname(__file__), 'host_test_main.c'),
        os.path.join(os.path.dirname(__file__), 'host_idf_runner_shim.c'),
        os.path.join(PROJECT_DIR, 'tests', 'host_include', 'host_err_to_name.c'),
        # FW-16 — viewer-sink recorder (shared host test infra for
        # the sink seam: stream sender, hello/status emission).
        os.path.join(PROJECT_DIR, 'tests', 'host_include', 'ws_sink_recorder.c'),
    ] + test_files
    for s in all_sources:
        if not os.path.exists(s):
            sys.exit(f"ERROR: missing source file: {s}")

    cc, cxx = _find_compiler()
    cflags = _common_cflags(extra_defines)
    cxxflags = list(cflags) + ['-std=c++17']

    out_bin = os.path.join(workdir, basename)

    cc_objs = []
    cxx_objs = []
    for src in all_sources:
        if src.endswith('.cpp'):
            obj = os.path.join(workdir, os.path.basename(src) + '.o')
            _run([cxx, *cxxflags, '-c', src, '-o', obj])
            cxx_objs.append(obj)
        else:
            obj = os.path.join(workdir, os.path.basename(src) + '.o')
            _run([cc, *cflags, '-c', src, '-o', obj])
            cc_objs.append(obj)
    _run([cxx, '-o', out_bin, *cc_objs, *cxx_objs])
    return out_bin


def _run_binary(bin_path):
    """Run the test binary and return (returncode, stdout)."""
    print()
    print(f"== Running {os.path.basename(bin_path)} ==")
    proc = subprocess.run([bin_path], capture_output=True, text=True)
    print(proc.stdout)
    if proc.stderr:
        print("STDERR:", proc.stderr)
    return proc.returncode, proc.stdout


# Names of the FW-02 tests registered by TEST_CASE. Keep in sync
# with the test files (Unity uses the literal first argument).
ALL_TESTS = [
    "config_save then config_load round-trips every field",
    "config_load on a fresh partition returns defaults and marks dirty",
    "stale schema_version returns defaults and sets dirty",
    "future-version stored schema also falls back to defaults (defensive)",
    "save after stale-schema load persists the compiled-in schema_version",
    "matching schema_version passes without dirty flag",
    # FW-03.1 walking skeleton + 6 ordering rows
    "boot_run invokes the FR-1 init sequence in order [fw-03.1][walking-skeleton]",
    "NVS init precedes load config [fw-03.1][ordering][row-1]",
    "load config precedes wifi-station init [fw-03.1][ordering][row-2]",
    "wifi-station init precedes camera init [fw-03.1][ordering][row-3]",
    "camera init precedes WS init [fw-03.1][ordering][row-4]",
    "WS init precedes supervision tasks start [fw-03.1][ordering][row-5]",
    "supervision tasks start precedes boot orchestrator return [fw-03.1][ordering][row-6]",
    # FW-03.2 fail-loud + green path
    "boot fails loud at camera_init when forced non-OK [fw-03.2][bite-proof]",
    "boot green path returns ESP_OK with no error log [fw-03.2][green]",
    # FW-03.3 provisioning decision + non-start-supervision
    "boot_decide_provisioning returns true on empty SSID + button-not-pressed [fw-03.3]",
    "boot_decide_provisioning returns false on non-empty SSID + button-not-pressed [fw-03.3]",
    "boot_decide_provisioning returns true on non-empty SSID + button-pressed [fw-03.3]",
    "boot_run in provisioning branch does not start supervision tasks [fw-03.3]",
    # FW-03.4 stability guard
    "boot_decide_provisioning is deterministic across calls [fw-03.4][guard][bite-proof]",
    "boot_decide_provisioning returns same value twice when stub absent [fw-03.4][green]",
    # FW-05.1 /whoami identity (3 scenarios)
    "whoami_returns_identity_json_fresh_device [fw-05.1]",
    "whoami_response_is_application_json_and_parses [fw-05.1]",
    "whoami_mac_is_12_char_lowercase_hex [fw-05.1]",
    # FW-05.2 /provision writes + reboots (3 outline rows + 2 length-caps)
    "provision_writes_nvs_and_reboots_home_2_4 [fw-05.2][row-1]",
    "provision_writes_nvs_and_reboots_office_5g [fw-05.2][row-2]",
    "provision_writes_nvs_and_reboots_guest [fw-05.2][row-3]",
    "provision_rejects_ssid_over_32_chars [fw-05.2][length-cap]",
    "provision_rejects_description_over_128_chars [fw-05.2][length-cap]",
    # FW-05.3 round-trip + partial update
    "whoami_round_trips_existing_name_and_description [fw-05.3]",
    "whoami_serves_identity_with_null_user_ctx [fw-05][regression]",
    "provision_partial_update_preserves_name_and_description [fw-05.3]",
    # FW-05.4 strict validation guard (wifi_ssid + wifi_password are
    # REQUIRED; name + description are OPTIONAL per PRD § FR-1a +
    # FW-05.3 partial-update semantics)
    "provision_rejects_non_json_body [fw-05.4]",
    "provision_rejects_missing_wifi_ssid [fw-05.4]",
    "provision_rejects_missing_wifi_password [fw-05.4]",
    "provision_accepts_missing_name [fw-05.4]",
    "provision_accepts_missing_description [fw-05.4]",
    "provision_accepts_well_formed_body [fw-05.4]",
    # FW-05 regression: device flash caught missing esp_wifi_init
    # (engram #3627). The host mock doesn't enforce the
    # "init must precede set_mode" invariant; this test makes
    # the dependency load-bearing so any future refactor that
    # drops the init call would fail RED here too.
    "softap_bringup_calls_esp_wifi_init_before_set_mode [fw-05][regression]",
    # FW-05 home page (scope expansion 2026-08-22)
    "home_get_serves_html_form_with_provision_action [fw-05][home-page]",
    "home_get_prefills_existing_identity [fw-05][home-page][fw-05.3]",
    "home_get_html_escapes_identity [fw-05][home-page][security]",
    # FW-05 station-join crash regression (engram #3639)
    "softap_owns_cfg_after_caller_returns [fw-05][regression][engram-3639]",
    # FW-06.1 boot + connecting (3 scenarios)
    "booting_holds_level_on [fw-06.1]",
    "wifi_connecting_period_100ms [fw-06.1]",
    "ws_connecting_period_50ms [fw-06.1]",
    # FW-06.2 connected (2 scenarios)
    "idle_period_500ms [fw-06.2]",
    "streaming_stops_periodic_holds_on [fw-06.2]",
    # FW-06.3 backoff + recovery (3 scenarios)
    "backoff_period_1000ms [fw-06.3]",
    "recovery_period_50ms_and_oneshot_3000ms [fw-06.3]",
    "recovery_fires_callback_after_3000ms [fw-06.3]",
    # FW-06.4 timer-fire guard (green path only; bite-proof is
    # Pass 5 below and uses -DLED_TEST_STUB_DISABLE_TIMER=1)
    "set_state_rearms_timer [fw-06.4][green]",
    # FW-07.1 tap-ignore state machine (4 boundary scenarios).
    # Presses ≤ TAP_MAX_MS (100 ms default) are absorbed by the
    # state machine without firing the runtime cb or asserting
    # boot_button_pressed_at_boot().
    "tap_50ms_is_ignored [fw-07.1]",
    "tap_99ms_is_ignored [fw-07.1]",
    "tap_100ms_is_ignored [fw-07.1]",
    "tap_101ms_is_ignored [fw-07.1]",
    # FW-07.2 boot-time long-press detection (5 scenarios). The
    # latch `g_boot_button_pressed_at_boot` is asserted by the
    # strong symbol in button.c when the press crosses
    # BOOT_LONGPRESS_MS (3 s default) during BOOT_TIME. The
    # runtime cb MUST stay dormant during BOOT_TIME (Phase D
    # wires it for RUNTIME only).
    "boot_longpress_3s_asserts_signal [fw-07.2]",
    "boot_longpress_10s_asserts_signal_no_runtime_cb [fw-07.2]",
    "boot_short_2s_does_not_assert [fw-07.2]",
    "strap_pin_transient_500ms_is_absorbed [fw-07.2]",
    "strap_grace_release_before_window_ends [fw-07.2]",
    # FW-07.3 runtime factory-reset (5 scenarios). Phase D wires
    # the RUNTIME-phase cb dispatch + the
    # `config_factory_reset + esp_restart` cb body (registered
    # by `boot_run_normal` on device). The test covers: S10
    # 10 s runtime press fires the cb exactly once; S11 5 s
    # press is ignored; S12 boundary (10001 ms vs 9999 ms);
    # S13 runtime press does NOT touch the `camera_cfg` NVS
    # namespace; S14 `config_factory_reset + esp_restart`
    # called exactly once each.
    "runtime_10s_press_wipes_and_restarts [fw-07.3][scenario-S10]",
    "runtime_5s_press_is_ignored [fw-07.3][scenario-S11]",
    "runtime_9990ms_does_not_trigger_10010ms_does [fw-07.3][scenario-S12]",
    "runtime_press_does_not_touch_camera_cfg_namespace [fw-07.3][scenario-S13]",
    "factory_reset_calls_once_each [fw-07.3][scenario-S14]",
    # FW-07.4 debounce filter guard (2 green-path scenarios).
    # Phase E lands the DEBOUNCE_MS contact-bounce filter + the
    # `#ifdef BUTTON_TEST_STUB_DISABLE_DEBOUNCE` gate around it.
    # S15 covers the bite-proof jitter pattern (phantom edges
    # collapsed into one transition); S16 covers the green path
    # (clean 50 ms tap is NOT swallowed by the filter). The
    # bite-proof stub build is Pass 6 below and uses
    # -DBUTTON_TEST_STUB_DISABLE_DEBOUNCE=1.
    "debounce_filters_jitter_phantom_press [fw-07.4]",
    "debounce_does_not_swallow_clean_tap [fw-07.4]",
    # FW-08 — wifi component smoke (T-08-A only). Full 18-test
    # surface (16 prod + 2 bite-proofs) lands across T-08-B..T-08-G.
    "test_wifi_init_succeeds [fw-08][smoke][build-infra]",
    # FW-08.1 — 6-row backoff schedule (T-08-B).
    "test_fw08_1_backoff_failures_1 [fw-08.1][row-1]",
    "test_fw08_1_backoff_failures_2 [fw-08.1][row-2]",
    "test_fw08_1_backoff_failures_3 [fw-08.1][row-3]",
    "test_fw08_1_backoff_failures_4 [fw-08.1][row-4]",
    "test_fw08_1_backoff_failures_5 [fw-08.1][row-5][cap-reached]",
    "test_fw08_1_backoff_failures_6 [fw-08.1][row-6][cap-holds]",
    # FW-08.2 — AP-reboot recovery + counter reset (T-08-C).
    "test_fw08_2_ap_reboot_reconnects_within_30s [fw-08.2][scenario-S1]",
    "test_fw08_2_counter_resets_on_ip_up [fw-08.2][scenario-S2]",
    # FW-08.3 — no-wedge guard (T-08-D). Green path only on
    # production build; bite-proof runs under Pass 7 stub.
    "test_fw08_3_misconfigured_ssid_returns_invalid_arg [fw-08.3][scenario-S2]",
    # FW-08.4 — softAP teardown (T-08-E). S1 only under Pass 1
    # (S2 gated by `#ifndef CONFIG_FIRMWARE_PROVISIONING_AP_
    # STOP_ON_CONNECT` which IS defined in cflags, so S2 is
    # excluded).
    "test_fw08_4_ip_up_triggers_teardown_within_1s [fw-08.4][scenario-S1]",
    # 2026-08-24 GOT_IP-teardown regression: esp_wifi_stop must
    # NEVER fire during GOT_IP handling (it killed the whole radio,
    # dropping the fresh STA association); the teardown ends in an
    # APSTA -> STA mode switch owned by softap_stop().
    "got_ip_teardown_does_not_stop_sta_radio [fw-08.4][regression]",
    # FW-08.5 — softAP alive during joining (T-08-F).
    "test_fw08_5_pre_ip_up_keeps_softap_active_at_5s [fw-08.5][scenario-S1]",
    "test_fw08_5_pre_ip_up_retries_do_not_affect_softap [fw-08.5][scenario-S2]",
    # FW-08.6 — no-AP-after-tear-down guard (T-08-G). Green path
    # only on production build; bite-proof runs under Pass 8
    # stub.
    "test_fw08_6_green_path_closes_attack_window [fw-08.6][scenario-S2]",
    # FW-10.1 — camera_init applies PRD § FR-2 parameter table
    # (T-10-A). 6-row Scenario Outline + a 7th pin-map assertion.
    "test_fw10_1_pixel_format_is_jpeg [fw-10.1][row-1]",
    "test_fw10_1_frame_size_is_qvga_default [fw-10.1][row-2]",
    "test_fw10_1_jpeg_quality_default_is_18 [fw-10.1][row-3]",
    "test_fw10_1_fb_count_is_one [fw-10.1][row-4]",
    "test_fw10_1_grab_mode_is_when_empty [fw-10.1][row-5]",
    "test_fw10_1_xclk_freq_is_10mhz [fw-10.1][row-6]",
    "test_fw10_1_ai_thinker_pin_map [fw-10.1][pin-map]",
    # FW-10.2 — PSRAM presence assertion + PSRAM_REQUIRED
    # typed-error (S1 green + S2 typed-error path).
    "test_fw10_2_psram_present_allows_init [fw-10.2][scenario-S1][green]",
    "test_fw10_2_psram_absent_logs_required_and_fails [fw-10.2][scenario-S2]",
    # FW-10.3 — runtime setter path + no-reinit guard. Green
    # path compiles under the production build (this row); the
    # bite-proof runs under Pass 9 stub build.
    "test_fw10_3_setter_path_applies_without_reinit [fw-10.3][scenario-S1][green]",
    # FW-10.4 — PSRAM size logged at first init (mechanical).
    "test_fw10_4_psram_size_logged_at_first_init [fw-10.4][green]",
    # FW-10.5 — camera_settings_source_t vtable + fake
    # in-memory source (3 scenarios).
    "test_fw10_5_no_stored_blob_uses_kconfig_defaults_once [fw-10.5][scenario-S2]",
    "test_fw10_5_stored_blob_overrides_kconfig_defaults [fw-10.5][scenario-S1][walking-skeleton]",
    "test_fw10_5_stored_via_setters_not_reinit [fw-10.5][scenario-S3]",
    # FW-11.1 — capture task produces frames at the requested
    # fps. S1 (5 fps sustained) + S2 (1 fps sustained).
    "test_fw11_1_five_fps_five_iterations_yield_five_frames [fw-11.1][scenario-S1][green]",
    "test_fw11_1_one_fps_single_iteration_yields_one_frame [fw-11.1][scenario-S2]",
    # FW-11.2 — drop-on-overflow + counter. S3 (full queue drop)
    # + S4 (100 frames no stall) + S5 (getters return values).
    "test_fw11_2_full_queue_drops_frame_and_returns_buffer [fw-11.2][scenario-S3]",
    "test_fw11_2_one_hundred_frames_no_stall [fw-11.2][scenario-S4]",
    "test_fw11_2_getters_return_counter_values [fw-11.2][getters]",
    # FW-11.3 — single-owner guard. Green path on production
    # build; bite-proof runs under Pass 10 stub.
    "test_fw11_3_capture_task_start_records_supervision [fw-11.3][scenario-S1][green]",
    # FW-11.4 — 30 s soak (loop-count semantics on host). S1
    # (150 iterations = 2 captured + 148 drops) + S2 (heap
    # bounded).
    "test_fw11_4_one_fifty_iterations_yield_one_fifty_attempts [fw-11.4][scenario-S1][green]",
    "test_fw11_4_heap_stays_bounded_over_soak [fw-11.4][scenario-S2][green]",
    # FW-11.5 — PSRAM heap-metrics closing check. 1 scenario:
    # PSRAM decreased by frame-buffer allocation after first
    # fb_get().
    "test_fw11_5_psram_heap_decreases_by_frame_buffer_allocation [fw-11.5][scenario-S1][green]",
    # FW-13 — identity_mac_to_hex_lower hex-encode surface (T-13-C).
    # Three scenarios: canonical MAC, all-zero MAC, overflow rejection.
    # Brings Pass 1 to 108 (was 105 after T-13-B).
    "test_mac_hex_format [fw-13][identity][mac-hex]",
    "test_mac_hex_zero [fw-13][identity][mac-hex][boundary]",
    "test_mac_hex_overflow [fw-13][identity][mac-hex][overflow]",
    # Three scenarios from REQ-WS-001 S1+S2+S3. Brings Pass 1 to
    # 111 (was 108).
    # 2 scenarios from REQ-WS-002 S1+S2.
    # 2 scenarios from REQ-WS-003 S1+S2.
    # FW-13.6 — status payload (T-13-H). 3 scenarios from
    # REQ-WS-006 S1+S2+S3.
    "test_status_payload_full_fields [fw-13.6][status-payload][scenario-S1]",
    "test_status_reconnects_zero_in_fw13 [fw-13.6][status-payload][scenario-S2]",
    "test_status_rssi_reflects_mock [fw-13.6][status-payload][scenario-S3]",
    # FW-14.1 — exponential backoff schedule (R-19, FR-4). 6-row
    # table: 2000/4000/8000/16000/30000/30000 via the ws_backoff
    # module surface + setter capture + one-shot arming + WARN log.
    "test_fw14_1_backoff_failures_1 [fw-14.1][row-1]",
    "test_fw14_1_backoff_failures_2 [fw-14.1][row-2]",
    "test_fw14_1_backoff_failures_3 [fw-14.1][row-3]",
    "test_fw14_1_backoff_failures_4 [fw-14.1][row-4]",
    "test_fw14_1_backoff_failures_5 [fw-14.1][row-5][cap-reached]",
    "test_fw14_1_backoff_failures_6 [fw-14.1][row-6][cap-holds]",
    # FW-14.2 — failure-counter lifecycle (R-19). Counter resets on
    # CONNECTED; persists across back-to-back failures; CONNECTED
    # clears the clean-CLOSE latch.
    "test_fw14_2_counter_resets_on_connected [fw-14.2][counter][scenario-S1]",
    "test_fw14_2_counter_persists_back_to_back [fw-14.2][counter][scenario-S2]",
    "test_fw14_2_connect_clears_latch [fw-14.2][latch][scenario-S3]",
    # FW-14 Phase 4 — event wiring end-to-end: ERROR parity with
    # DISCONNECTED, clean-CLOSE latch orderings, non-clean close,
    # event-driven counter reset.
    "test_fw14_error_arms_timer_like_disconnected [fw-14][error-parity][scenario-S1]",
    "test_fw14_clean_close_latches_before_disconnect [fw-14][latch-order][scenario-S2]",
    "test_fw14_clean_close_cancels_pending_timer [fw-14][latch-order][scenario-S3]",
    "test_fw14_non_clean_close_does_not_latch [fw-14][latch-order][scenario-S4]",
    "test_fw14_connected_event_resets_counter_end_to_end [fw-14][counter-wiring][scenario-S5]",
    # FW-05.5 — always-on /whoami listener on the STA interface.
    # 4 scenarios covering install, IP-up, disconnect, idempotency.
    "test_fw05_5_install_subscribes_both_events [fw-05.5][install][scenario-S1]",
    "test_fw05_5_ip_up_starts_httpd [fw-13.5][ip-up][scenario-S1]",
    "test_fw05_5_disconnect_stops_httpd [fw-05.5][disconnect][scenario-S2]",
    "test_fw05_5_idempotent_ip_up [fw-05.5][idempotent][scenario-S3]",
    # FW-16 — device-as-server WebSocket endpoint (single inbound
    # viewer). Registration + identity-leak guard, hello-on-accept,
    # single-viewer rejection, close-frees-slot, status cadence,
    # post-close silence.
    "test_fw16_cams_endpoint_registers_as_websocket [fw-16][server][scenario-S1]",
    "test_fw16_hello_emitted_once_on_viewer_accept [fw-16][server][scenario-S2]",
    "test_fw16_second_handshake_rejected_viewer_limit [fw-16][server][scenario-S3]",
    "test_fw16_viewer_close_frees_slot [fw-16][server][scenario-S4]",
    "test_fw16_status_cadence_3_frames_in_90s [fw-16][status-cadence][scenario-S5]",
    "test_fw16_no_status_after_viewer_close [fw-16][status-cadence][scenario-S6]",
    # FW-15.1 — bounded-timeout receive (REQ-ST-006). 3 scenarios:
    # empty-queue ≈T timeout, queued-item exact-pointer receive,
    # cross-thread producer wakes a waiting consumer early.
    "test_fw15_empty_queue_receive_times_out_bounded [fw-15.1][req-st-006][scenario-S1]",
    "test_fw15_queued_item_received_with_exact_pointer [fw-15.1][req-st-006][scenario-S2]",
    "test_fw15_waiting_consumer_wakes_on_producer_push [fw-15.1][req-st-006][scenario-S3]",
    # FW-15.2 — pure fragment planner (REQ-ST-003/004). 4 scenarios:
    # part-count table, boundary/degenerate rows, byte-exact
    # partition via offsets, out-of-range clamp.
    "test_fw15_part_count_table_matches_chunk_boundaries [fw-15.2][req-st-003][scenario-S1]",
    "test_fw15_part_count_boundary_and_degenerate_rows [fw-15.2][req-st-003][scenario-S2]",
    "test_fw15_fragment_offsets_partition_len_exactly [fw-15.2][req-st-003][scenario-S3]",
    "test_fw15_fragment_offset_out_of_range_clamps_to_len [fw-15.2][req-st-003][scenario-S4]",
    # FW-15.2/FW-16 — sender mapping. 2 scenarios: 8 KB single
    # binary send + oversized frame rides ONE complete message.
    "test_fw15_8k_frame_ships_as_single_binary_event [fw-15.2][req-st-001][scenario-S5]",
    "test_fw16_oversized_frame_ships_as_one_complete_binary_frame [fw-16][req-st-003][scenario-S6]",
    # FW-15.3/FW-16 — stream task loop (REQ-ST-005/007 + happy path
    # + no-viewer drop-count).
    "test_fw15_failed_send_returns_fb_exactly_once [fw-15.3][req-st-005][scenario-S1]",
    "test_fw15_dead_socket_drains_all_frames [fw-15.3][req-st-007][scenario-S2]",
    "test_fw16_no_viewer_frame_dropped_and_counted [fw-16][server][scenario-S4]",
    "test_fw15_healthy_socket_loop_sends_and_counts [fw-15.3][req-st-002][scenario-S3]",
    # Diagnostic GET /snapshot endpoint (queued-frame bisect tool).
    "test_snapshot_queued_frame_served_as_jpeg [snapshot][scenario-S1]",
    "test_snapshot_empty_queue_returns_503 [snapshot][scenario-S2]",
    "test_snapshot_listener_registers_both_uris [snapshot][scenario-S3]",
    # FW-16.1 — pure sliding-window threshold core (R-FW16-1.1).
    # Brings Pass 1 to 158 (was 153).
    "test_soft_recovery_29_in_window_failures_do_not_trigger [fw-16.1][window][scenario-S1]",
    "test_soft_recovery_30th_in_window_failure_triggers [fw-16.1][window][scenario-S2]",
    "test_soft_recovery_31_in_window_failures_hold_trigger [fw-16.1][window][scenario-S3]",
    "test_soft_recovery_15_failures_over_20_minutes_pruned_no_trigger [fw-16.1][window][scenario-S4]",
    "test_soft_recovery_window_boundary_entry_kept_then_expired [fw-16.1][window][boundary]",
    # FW-16.1 — episode coalescing (R-FW16-1.1). Brings Pass 1 to
    # 162 (was 158).
    "test_soft_recovery_paired_burst_advances_counter_exactly_one [fw-16.1][coalesce][scenario-C1]",
    "test_soft_recovery_got_ip_closes_episode_next_drop_counts [fw-16.1][coalesce][scenario-C2]",
    "test_soft_recovery_initial_latch_closed_first_drop_ever_counts [fw-16.1][coalesce][scenario-C3]",
    "test_soft_recovery_distinct_episodes_accumulate_to_threshold [fw-16.1][coalesce][scenario-C4]",
    # FW-16.2 — forensic reason persistence + boot surfacing
    # (R-FW16-1.2). Brings Pass 1 to 166 (was 162).
    "test_soft_recovery_persist_reason_readable_before_any_restart [fw-16.2][persist][scenario-P1]",
    "test_soft_recovery_persist_survives_config_namespace_erase [fw-16.2][persist][scenario-P2]",
    "test_soft_recovery_next_boot_logs_stored_reason_verbatim [fw-16.2][surface][scenario-P3]",
    "test_soft_recovery_surface_silent_when_no_reason_stored [fw-16.2][surface][scenario-P4]",
    # FW-16.1/16.3 — trigger sequence + green path (R-FW16-1.1/1.3).
    # Brings Pass 1 to 169 (was 166).
    "test_soft_recovery_threshold_sequence_persist_before_restart [fw-16.1][trigger][scenario-S1]",
    "test_soft_recovery_trigger_latched_idempotent_after_firing [fw-16.1][trigger][scenario-S2]",
    "test_soft_recovery_healthy_stream_60s_never_triggers [fw-16.3][green-path][scenario-G1]",
    # FW-16.3 — healthy-stream bite-proof guard (Pass 13). Green-path
    # branch compiles into Pass 1 (brings it to 170); the bite-proof
    # branch compiles ONLY under -DHEALTH_TEST_STUB_COUNT_WHILE_
    # HEALTHY=1 in Pass 13.
    "test_health_guard_60s_healthy_stream_must_not_count [fw-16.3][guard][bite-proof]",
]

# The FW-03.4 bite-proof test name. The host runner's Pass 3
# compiles boot.c with -DBOOT_TEST_STUB_FLIP_DECISION=1 (so
# boot_decide_provisioning() flips its return value on each
# call); the assertion `call1 == call2` fails and the failure
# message must contain the literal "determinism" so the runner
# can verify the guard is load-bearing.
FW03_BITE_PROOF_TEST_NAME = (
    "boot_decide_provisioning is deterministic across calls "
    "[fw-03.4][guard][bite-proof]"
)

# The FW-02.3 bite-proof test that MUST fail when the version
# check is stubbed out. Its name deliberately contains the literal
# "schema_version" per the milestones doc bite-proof requirement.
BITE_PROOF_TEST_NAME = (
    "stale schema_version is rejected when check runs "
    "(guard bite-proof: fails if CONFIG_TEST_STUB_VERSION_CHECK is set)"
)

# Test file lists per build. The stub build only contains the guard
# file so that the stale-fallback tests are not also broken by the
# stub (each binary has its own compiled version of config.c).
GUARD_TEST_FILES = [
    os.path.join(PROJECT_DIR, 'tests', 'test_config', 'test_config_guard.c'),
]
ALL_TEST_FILES = GUARD_TEST_FILES + [
    os.path.join(PROJECT_DIR, 'tests', 'test_config', 'test_config_load_fresh.c'),
    os.path.join(PROJECT_DIR, 'tests', 'test_config', 'test_config_roundtrip.c'),
    os.path.join(PROJECT_DIR, 'tests', 'test_config', 'test_config_schema_mismatch.c'),
    os.path.join(PROJECT_DIR, 'tests', 'test_config', 'test_config_schema_persists.c'),
    os.path.join(PROJECT_DIR, 'tests', 'test_boot', 'test_boot_order.c'),
    os.path.join(PROJECT_DIR, 'tests', 'test_boot', 'test_boot_fail_loud.c'),
    os.path.join(PROJECT_DIR, 'tests', 'test_boot', 'test_boot_decide.c'),
    os.path.join(PROJECT_DIR, 'tests', 'test_boot', 'test_boot_stability_guard.c'),
    # FW-05 softAP provisioning
    os.path.join(PROJECT_DIR, 'tests', 'test_softap', 'test_softap_whoami.c'),
    os.path.join(PROJECT_DIR, 'tests', 'test_softap', 'test_softap_provision.c'),
    os.path.join(PROJECT_DIR, 'tests', 'test_softap', 'test_softap_guard.c'),
    # FW-05 home page (scope expansion 2026-08-22)
    os.path.join(PROJECT_DIR, 'tests', 'test_softap', 'test_softap_home.c'),
    # FW-05.5 — always-on /whoami listener on the STA interface
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_softap_sta_listener.c'),
    # FW-06 status LED
    os.path.join(PROJECT_DIR, 'tests', 'test_led', 'test_led_boot_connecting.c'),
    os.path.join(PROJECT_DIR, 'tests', 'test_led', 'test_led_connected.c'),
    os.path.join(PROJECT_DIR, 'tests', 'test_led', 'test_led_backoff_recovery.c'),
    # test_led_guard.c compiles both the green-path test (under
    # production build, no LED_TEST_STUB_DISABLE_TIMER) and the
    # bite-proof test (under Pass 5 stub build). The
    # #ifdef LED_TEST_STUB_DISABLE_TIMER inside the file selects
    # which one is compiled into each build.
    os.path.join(PROJECT_DIR, 'tests', 'test_led', 'test_led_guard.c'),
    # FW-07.1 tap-ignore state machine (4 boundary scenarios).
    # Phase B lands the tap-ignore logic + these 4 tests; the
    # full FW-07 surface (boot-time + runtime + debounce guard)
    # arrives in Phases C/D/E.
    os.path.join(PROJECT_DIR, 'tests', 'test_button', 'test_button_tap_ignore.c'),
    # FW-07.2 boot-time long-press detection (5 scenarios).
    # Phase C lands the BOOT_TIME latch logic + these 5 tests.
    os.path.join(PROJECT_DIR, 'tests', 'test_button', 'test_button_boot_longpress.c'),
    # FW-07.3 runtime factory-reset (5 scenarios). Phase D
    # wires the RUNTIME-phase cb dispatch + the
    # `config_factory_reset + esp_restart` cb body. Tests
    # verify the button driver fires the registered cb
    # exactly once when the press crosses
    # RUNTIME_LONGPRESS_MS during RUNTIME, and that the
    # cb body does NOT touch the `camera_cfg` NVS
    # namespace.
    os.path.join(PROJECT_DIR, 'tests', 'test_button', 'test_button_runtime_reset.c'),
    # FW-07.4 debounce filter (2 green-path scenarios). Phase E
    # lands the DEBOUNCE_MS contact-bounce filter + the
    # `#ifdef BUTTON_TEST_STUB_DISABLE_DEBOUNCE` gate. The
    # green-path tests compile under the production build
    # (Pass 1 above); the bite-proof test compiles under the
    # Pass 6 stub build below. The #ifdef inside the file
    # selects which one is compiled into each build.
    os.path.join(PROJECT_DIR, 'tests', 'test_button', 'test_button_guard.c'),
    # FW-08 — wifi component smoke (T-08-A only). Full test
    # files (test_wifi_backoff.c, test_wifi_recovery.c,
    # test_wifi_guard.c, test_wifi_event_teardown.c,
    # test_wifi_event_joining.c, test_wifi_event_guard.c)
    # land in T-08-B..T-08-G and get added to this list as each
    # commit lands. The bite-proof test files are compiled only
    # under their respective Pass 7 / Pass 8 stub builds (mirrors
    # the LED_TEST_STUB_DISABLE_TIMER / BUTTON_TEST_STUB_DISABLE
    # _DEBOUNCE pattern).
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_wifi_main.c'),
    # FW-08.1 — 6-row backoff schedule (T-08-B). Tests the pure
    # wifi_backoff_delay_ms(N) helper against the charter
    # L742-748 table.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_wifi_backoff.c'),
    # FW-08.2 — AP-reboot recovery + counter reset (T-08-C).
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_wifi_recovery.c'),
    # FW-08.3 — no-wedge guard (T-08-D). Compiles the green-path
    # test under the production build; the bite-proof test is
    # compiled under the Pass 7 stub build (see FW08_3_GUARD
    # _TEST_FILES below). The #ifdef inside the file selects
    # which one is compiled into each build.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_wifi_guard.c'),
    # FW-08.4 — softAP teardown within 1s of IP_EVENT_STA_GOT_IP
    # (T-08-E). Two scenarios: S1 (Kconfig=y → teardown fires)
    # + S2 (Kconfig=n → no teardown). S2 is gated by `#ifndef
    # CONFIG_FIRMWARE_PROVISIONING_AP_STOP_ON_CONNECT` so it
    # only compiles when the symbol is NOT defined. The
    # production build passes the symbol via cflags (mirroring
    # sdkconfig.defaults:36 default y) so S2 is excluded from
    # Pass 1.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_wifi_event_teardown.c'),
    # FW-08.5 — softAP alive during STA joining (T-08-F).
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_wifi_event_joining.c'),
    # FW-08.6 — no-AP-after-tear-down guard (T-08-G). Compiles
    # the green-path test under the production build; the
    # bite-proof test is compiled under the Pass 8 stub build
    # (see FW08_6_GUARD_TEST_FILES below). The #ifdef inside
    # the file selects which one is compiled into each build.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_wifi_event_guard.c'),
    # FW-10.1 — camera_init applies PRD § FR-2 parameter table.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_camera_init.c'),
    # FW-10.2 — PSRAM presence assertion + PSRAM_REQUIRED
    # typed-error. 2 scenarios: present allows init; absent
    # logs PSRAM_REQUIRED + returns ESP_FAIL.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_camera_psram.c'),
    # FW-10.3 — runtime setter path green scenario + bite-proof
    # (Pass 9 below uses -DCAMERA_TEST_STUB_REINIT=1).
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_camera_guard.c'),
    # FW-10.4 — PSRAM size logged at first init.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_camera_psram_size.c'),
    # FW-10.5 — camera_settings_source_t vtable + fake
    # in-memory source. 3 scenarios: stored blob overrides
    # Kconfig defaults; no stored blob uses Kconfig once;
    # stored applied via setters (no reinit).
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_camera_settings_fake.c'),
    # FW-11.1/11.2 — capture-loop production + drop-on-overflow.
    # 4 scenarios: 5 fps sustained, 1 fps sustained, full queue
    # drop, 100 frames no stall.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_capture_loop.c'),
    # FW-11.3 — single-owner guard. Compiles the green-path
    # test under the production build; the bite-proof test is
    # compiled under the Pass 10 stub build (see
    # FW11_3_GUARD_TEST_FILES below). The #ifdef inside the
    # file selects which one is compiled into each build.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_capture_guard.c'),
    # FW-11.4 — 30 s soak (loop-count semantics on host). 2
    # scenarios: 150 iterations + heap bounded.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_capture_soak.c'),
    # FW-11.5 — PSRAM heap-metrics closing check. 1 scenario:
    # PSRAM decreased by frame-buffer allocation after first
    # fb_get().
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_capture_heap.c'),
    # FW-13 — identity_mac_to_hex_lower (T-13-C). Three tests
    # covering canonical MAC, all-zero MAC, and overflow
    # rejection. Pure helper (no IDF mocks needed).
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_identity_mac_hex.c'),
    # FW-16 — device-as-server WS endpoint: registration +
    # identity-leak guard, hello-on-accept, single-viewer
    # rejection, close-frees-slot, status cadence + post-close
    # silence. Supersedes the client-era suites.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_ws_server.c'),
    # FW-13.6 — status frame payload (T-13-H). 3 scenarios:
    # full 8-field payload, reconnects == 0, rssi_dbm reflects mock.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_ws_status_payload.c'),
    # FW-14 — ws_backoff module (reconnect loop owner). Leaf tests:
    # 6-row FR-4 backoff table + counter lifecycle + event wiring +
    # latch orderings land across the FW-14 apply commits.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_ws_reconnect_backoff.c'),
    # FW-15 — stream component surface: bounded receive timeout
    # (REQ-ST-006), single-send/opcode (REQ-ST-001/002), disconnect
    # drain-drop-count (REQ-ST-007). Files register as each lands.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_stream_loop.c'),
    # FW-15.2 — pure fragment planner: REQ-ST-003/004 part-count
    # table + byte-exact offset partition. No mocks needed.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_stream_fragment.c'),
    # FW-15.3 — disconnect drain-drop-count + loop happy path
    # (REQ-ST-005/007). 3 scenarios: failed-send fb return, dead-
    # socket drain, healthy-socket send+count through the loop.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_stream_disconnect.c'),
    # Diagnostic GET /snapshot: queued frame served as image/jpeg
    # via the capture queue (single-caller invariant intact), 503
    # on empty queue, dual URI registration with /whoami.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_snapshot_endpoint.c'),
    # FW-16.1 — pure sliding-window threshold core (R-FW16-1.1).
    # 29→no-trigger, 30th in-window→trigger, 31→holds, 15-over-
    # 20-min pruned false, lazy-prune boundary edge. Zero IDF deps.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_soft_recovery_window.c'),
    # FW-16.1 — episode coalescing (R-FW16-1.1, AD2): paired burst
    # = exactly one increment; GOT_IP re-arms the next fault;
    # initial latch closed.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_soft_recovery_coalesce.c'),
    # FW-16.2 — forensic reason persistence + boot surfacing
    # (R-FW16-1.2, AD4/AD5): write-before-restart ordering,
    # factory-reset survival (ns "recovery" vs ns "config"),
    # next-boot verbatim surfacing, silent NOT_FOUND miss.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_soft_recovery_persist.c'),
    # FW-16.1/16.3 — trigger-sequence ordering + healthy-stream green
    # path (R-FW16-1.1 seq + R-FW16-1.3, AD5/AD6): persist → LED-arm
    # observable before restart; one-shot-driven completion cb;
    # latched idempotence; 60 s event-driven-only green path.
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_soft_recovery_trigger.c'),
    # FW-16.3 — healthy-stream bite-proof guard. Green-path branch
    # runs in Pass 1 (event-driven-only invariant); the bite-proof
    # branch compiles only under the Pass 13 stub flag (see
    # FW16_GUARD_TEST_FILES below).
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_health_guard.c'),
]

# FW-08.3 — Pass 7 stub build includes ONLY the FW-08.3 guard
# file. The build defines -DWIFI_TEST_STUB_USE_BLOCKING_WAIT=1
# so wifi_init() short-circuits into the guard tripwire. The
# bite-proof test asserts the guard fires with the literal
# "bounded_wait" in the message. Mirrors Pass 5 / Pass 6 shape
# (LED_TEST_STUB_DISABLE_TIMER / BUTTON_TEST_STUB_DISABLE
# _DEBOUNCE) — the same compile flag is applied to BOTH the
# production source (wifi.c) and the test file
# (test_wifi_guard.c). The green-path test in test_wifi_guard.c
# is excluded from this build by the `#ifndef WIFI_TEST_STUB
# _USE_BLOCKING_WAIT` inside the file.
FW08_3_GUARD_TEST_FILES = [
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_wifi_guard.c'),
]

# FW-08.6 — Pass 8 stub build includes ONLY the FW-08.6 guard
# file. The build defines -DWIFI_TEST_STUB_SKIP_IP_UP_HANDLER=1
# so on_sta_got_ip_handler() is replaced by a no-op + the
# guard tripwire. The bite-proof test asserts the guard fires
# with the literal "teardown" in the message. Mirrors Pass 7
# shape (WIFI_TEST_STUB_USE_BLOCKING_WAIT) — the same compile
# flag is applied to BOTH the production source (wifi_event.c)
# and the test file (test_wifi_event_guard.c). The green-path
# test in test_wifi_event_guard.c is excluded from this build
# by the `#ifndef WIFI_TEST_STUB_SKIP_IP_UP_HANDLER` inside
# the file.
FW08_6_GUARD_TEST_FILES = [
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_wifi_event_guard.c'),
]

# The literal substrings the Pass 7 / Pass 8 runners grep for.
# Mirrors the LED_TEST_STUB_DISABLE_TIMER ("timer_fire") and
# BUTTON_TEST_STUB_DISABLE_DEBOUNCE ("debounce") patterns.
WIFI_BITE_PROOF_KEYWORD = "bounded_wait"
WIFI_EVENT_BITE_PROOF_KEYWORD = "teardown"

# FW-10.3 — Pass 9 stub build includes ONLY the FW-10.3 guard
# file. The build defines -DCAMERA_TEST_STUB_REINIT=1 so
# camera_init() short-circuits on the second invocation into
# the no_reinit guard tripwire. The bite-proof test asserts
# the guard fires with the literal "no_reinit" in the message.
# Mirrors the WIFI_TEST_STUB_USE_BLOCKING_WAIT (Pass 7) +
# WIFI_TEST_STUB_SKIP_IP_UP_HANDLER (Pass 8) pattern: the same
# compile flag is applied to BOTH the production source
# (camera.c) AND the test file (test_camera_guard.c). The
# green-path test in test_camera_guard.c is excluded by the
# `#ifndef CAMERA_TEST_STUB_REINIT` inside the file.
FW10_3_GUARD_TEST_FILES = [
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_camera_guard.c'),
]

# Pass-9 keyword + bite-proof test marker — mirrors
# WIFI_BITE_PROOF_KEYWORD above. The literal substring
# "no_reinit" must appear in the guard tripwire's
# TEST_FAIL_MESSAGE so Pass 9 of run_host_tests.py can grep
# for it.
CAMERA_BITE_PROOF_KEYWORD = "no_reinit"

# FW-11.3 — Pass 10 stub build includes ONLY the FW-11.3
# guard file. The build defines -DCAPTURE_TEST_STUB_SECOND
# _CALLER=1 so capture_task_start() short-circuits into
# capture_guard_fail_single_owner() with the literal
# "single_owner" in TEST_FAIL_MESSAGE. Mirrors Pass 9
# (CAMERA_TEST_STUB_REINIT) shape exactly. The green-path
# test in test_capture_guard.c is excluded by the
# `#ifndef CAPTURE_TEST_STUB_SECOND_CALLER` inside the file.
FW11_3_GUARD_TEST_FILES = [
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_capture_guard.c'),
]

# Pass-10 keyword + bite-proof test marker. The literal
# substring "single_owner" must appear in the guard tripwire's
# TEST_FAIL_MESSAGE so Pass 10 of run_host_tests.py can grep
# for it.
CAPTURE_BITE_PROOF_KEYWORD = "single_owner"

# FW-14 — Pass 12 stub build includes ONLY the FW-14 close-guard
# file. The build defines -DWS_TEST_STUB_ENABLE_CLOSE_RECONNECT=1,
# which compiles the clean-CLOSE latch check OUT of
# ws_event_handler.c's failure path (simulating the regression
# where a clean CLOSE no longer suppresses reconnect scheduling).
# The bite-proof test asserts NOTHING was scheduled after
# CLOSED(1000) + DISCONNECTED; under the stub it fails with the
# literal "close_no_reconnect". Mirrors Pass 11
# (WS_TEST_STUB_INJECT_MAC_INTO_URL) shape exactly: the same
# compile flag is applied to BOTH the production sources AND the
# test file.
FW14_GUARD_TEST_FILES = [
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_ws_close_guard.c'),
]

# Pass-12 keyword + bite-proof test marker. The literal substring
# "close_no_reconnect" must appear in the bite-proof's failure
# message so Pass 12 of run_host_tests.py can grep for it.
WS_CLOSE_GUARD_BITE_PROOF_KEYWORD = "close_no_reconnect"

# FW-16.3 — Pass 13 stub build includes ONLY the FW-16.3 health
# guard file. The build defines
# -DHEALTH_TEST_STUB_COUNT_WHILE_HEALTHY=1, which compiles the
# guard-only tick health_green_path_tick_for_guard() INTO the
# production health sources; each tick records ONE phantom failure
# with no wifi event and no episode latch (a model of a future
# always-sweeping miscounting implementation). 60 ticks at 1 Hz
# exceed the default threshold of 30, so the guard test fails with
# the literal "healthy-stream" in its message. Mirrors the Pass 9-12
# pattern exactly: the same compile flag is applied to BOTH the
# production sources AND the test file. The green-path assertions
# in test_health_guard.c are selected by `#ifndef` so the prod build
# never sees the tick calls.
FW16_GUARD_TEST_FILES = [
    os.path.join(PROJECT_DIR, 'tests', 'host_test', 'test_health_guard.c'),
]

# Pass-13 keyword + bite-proof test marker. The literal substring
# "healthy-stream" must appear in the bite-proof's failure message
# so Pass 13 of run_host_tests.py can grep for it.
HEALTH_BITE_PROOF_KEYWORD = "healthy-stream"

FW16_BITE_PROOF_TEST_NAME = (
    "test_health_guard_60s_healthy_stream_must_not_count "
    "[fw-16.3][guard][bite-proof]"
)

# Pass-3 stub build includes ONLY the FW-03.4 bite-proof file. The
# green-path test in that file is guarded by `#ifndef
# BOOT_TEST_STUB_FLIP_DECISION` so it auto-excludes itself; what
# remains is exactly the determinism bite-proof, which MUST fail.
FW03_4_GUARD_TEST_FILES = [
    os.path.join(PROJECT_DIR, 'tests', 'test_boot', 'test_boot_stability_guard.c'),
]

# Pass-4 stub build includes ONLY the FW-05.4 guard file. All 6
# guard tests compile under -DSOFTAP_TEST_STUB_ACCEPT_ALL_BODIES=1:
#   - The 3 required-key rejection tests (non-JSON, missing-
#     wifi_ssid, missing-wifi_password) FAIL because the handler
#     no longer enforces validation (it bypasses the parse +
#     required-key checks and proceeds straight to merge + save +
#     restart).
#   - The 2 accepts-missing-* tests (missing name / description)
#     continue to PASS — those keys are OPTIONAL, so they were
#     always going to be merged-absent + preserved-from-cfg
#     regardless of whether the validation block ran.
#   - The 1 well-formed test continues to PASS.
# So the runner expects 3 fail + 3 pass. Each failure message
# contains the literal "validation" so the runner can verify the
# bite-proof pattern.
FW05_4_GUARD_TEST_FILES = [
    os.path.join(PROJECT_DIR, 'tests', 'test_softap', 'test_softap_guard.c'),
]

# Pass-5 stub build includes ONLY the FW-06.4 guard file. The
# build defines -DLED_TEST_STUB_DISABLE_TIMER=1 so led.c skips
# esp_timer_create (the periodic handle stays NULL). On any
# led_set_state() into a blink state, the guard trips with a
# message containing the literal "timer_fire" and aborts the
# process. The runner expects:
#   - rc != 0 (process aborted)
#   - literal "timer_fire" in stdout (the guard's printf + the
#     test's "bite-proof stub build entered" marker)
# The guard test is the ONLY test in test_led_guard.c under the
# stub flag (the green-path test is guarded by #ifndef
# LED_TEST_STUB_DISABLE_TIMER).
FW06_4_GUARD_TEST_FILES = [
    os.path.join(PROJECT_DIR, 'tests', 'test_led', 'test_led_guard.c'),
]

# Pass-6 stub build includes ONLY the FW-07.4 guard file. The
# build defines -DBUTTON_TEST_STUB_DISABLE_DEBOUNCE=1 so
# button.c's `button_edge_is_bouncing()` returns false
# unconditionally (every edge is accepted). The bite-proof
# test drives the S15 jitter pattern (LOW at t=0, HIGH at t=5,
# LOW at t=15, HIGH at t=50) which the production filter would
# collapse into a single transition; with the filter
# short-circuited the jitter propagates and the test's
# TEST_FAIL_MESSAGE fires with a body containing the literal
# "debounce". The runner expects:
#   - rc != 0 (test failed via TEST_FAIL_MESSAGE)
#   - literal "debounce" in stdout (from the test's
#     "bite-proof stub build entered" marker AND from the
#     TEST_FAIL_MESSAGE body)
# Mirrors the FW-06.4 LED_TEST_STUB_DISABLE_TIMER pattern
# exactly: the same compile flag is applied to BOTH the
# production source (button.c) and the test file
# (test_button_guard.c). The green-path tests in
# test_button_guard.c are excluded from this build by the
# `#ifndef BUTTON_TEST_STUB_DISABLE_DEBOUNCE` inside the file.
FW07_4_GUARD_TEST_FILES = [
    os.path.join(PROJECT_DIR, 'tests', 'test_button', 'test_button_guard.c'),
]


def _pass16_bite_proof(workdir):
    """Pass 13 — FW-16.3 healthy-stream bite-proof (shared by the
    full run and --stub mode). Builds test_health_guard.c AND the
    production sources with -DHEALTH_TEST_STUB_COUNT_WHILE_HEALTHY=1
    and expects EXACTLY ONE failing test whose output names the
    healthy-stream invariant."""
    print()
    print("=== Pass 13: FW-16.3 stub build (HEALTH_TEST_STUB_COUNT_WHILE_HEALTHY, guard file) ===")
    fw16_bin = _build('fw16_tests_stub',
                      ['-DHEALTH_TEST_STUB_COUNT_WHILE_HEALTHY=1'],
                      FW16_GUARD_TEST_FILES, workdir)
    fw16_rc, fw16_out = _run_binary(fw16_bin)
    if fw16_rc == 0:
        sys.exit(f"FAIL: FW-16.3 stub build returned 0; expected "
                 f"the bite-proof guard to trip. The stub tick didn't "
                 f"corrupt the healthy-stream invariant. "
                 f"Output:\n{fw16_out}")
    if HEALTH_BITE_PROOF_KEYWORD not in fw16_out:
        sys.exit(f"FAIL: FW-16.3 stub build output does not "
                 f"contain the literal '{HEALTH_BITE_PROOF_KEYWORD}':\n{fw16_out}")
    if "test_health_guard_60s_healthy_stream_must_not_count" not in fw16_out:
        sys.exit(f"FAIL: FW-16.3 stub build did not run the "
                 f"bite-proof test. Output:\n{fw16_out}")
    # Exactly one failure (the bite-proof) — a second failing
    # assertion would mean the stub corrupts MORE than the
    # counting invariant.
    fail_count = sum(1 for line in fw16_out.splitlines() if line.startswith("FAIL ["))
    if fail_count != 1:
        sys.exit(f"FAIL: FW-16.3 stub build should have exactly 1 failure "
                 f"(healthy-stream bite-proof); got {fail_count}. "
                 f"Output:\n{fw16_out}")
    print(f"OK: FW-16.3 stub build → guard tripped on "
          f"'{HEALTH_BITE_PROOF_KEYWORD}' invariant as expected "
          f"(test failed with rc={fw16_rc}).")


def main():
    # --stub: run ONLY the FW-16.3 bite-proof pass (Pass 13). The
    # production suite is untouched; exit code stays 0 when the
    # single expected healthy-stream failure is observed.
    if '--stub' in sys.argv[1:]:
        workdir = tempfile.mkdtemp(prefix='fw02-host-tests-stub-')
        print("FW-02 host test runner — STUB MODE (--stub): Pass 13 only")
        _pass16_bite_proof(workdir)
        print()
        print("=== stub mode OK: single expected healthy-stream bite-proof failure observed ===")
        return

    workdir = tempfile.mkdtemp(prefix='fw02-host-tests-')
    print(f"FW-02 host test runner")
    print(f"  project_dir: {PROJECT_DIR}")
    print(f"  IDF_PATH:    {IDF_PATH}")
    print(f"  workdir:     {workdir}")

    # ----- Pass 1: production build (all FW-02 tests, no stub) -----
    print()
    print("=== Pass 1: production build (no CONFIG_TEST_STUB_VERSION_CHECK, all tests) ===")
    prod_bin = _build('fw02_tests_prod', [], ALL_TEST_FILES, workdir)
    prod_rc, prod_out = _run_binary(prod_bin)
    if prod_rc != 0:
        sys.exit(f"FAIL: production build returned {prod_rc}; expected all "
                 f"{len(ALL_TESTS) + 1} tests to pass. Output:\n{prod_out}")
    passed_count = sum(1 for line in prod_out.splitlines() if line.startswith("PASS ["))
    if passed_count != len(ALL_TESTS) + 1:
        sys.exit(f"FAIL: production build expected {len(ALL_TESTS) + 1} passes; "
                 f"got {passed_count}. Output:\n{prod_out}")
    print(f"OK: production build → all {passed_count} tests pass.")

    # ----- Pass 2: stub build (only guard test, with stub) -----
    # Only the guard test file is compiled with the stub flag; config.c
    # is recompiled with the same flag, so the version check is a no-op.
    # The bite-proof test MUST FAIL (the guard is bitey); the matching
    # schema test must still pass (matching schema is "fresh" under
    # any policy).
    print()
    print("=== Pass 2: stub build (CONFIG_TEST_STUB_VERSION_CHECK, guard test only) ===")
    stub_bin = _build('fw02_tests_stub',
                      ['-DCONFIG_TEST_STUB_VERSION_CHECK=1'],
                      GUARD_TEST_FILES, workdir)
    stub_rc, stub_out = _run_binary(stub_bin)
    if stub_rc == 0:
        sys.exit(f"FAIL: stub build returned 0; expected the bite-proof test "
                 f"to fail. Output:\n{stub_out}")
    if BITE_PROOF_TEST_NAME not in stub_out:
        sys.exit(f"FAIL: stub build failed but the bite-proof test name "
                 f"'{BITE_PROOF_TEST_NAME}' is not in the output:\n{stub_out}")
    if "FAIL" not in stub_out:
        sys.exit(f"FAIL: stub build rc != 0 but no FAIL line in output:\n{stub_out}")
    # The failure message must mention "schema_version" per the
    # milestones doc bite-proof requirement.
    if "schema_version" not in stub_out:
        sys.exit(f"FAIL: stub build failure message does not contain "
                 f"the literal 'schema_version':\n{stub_out}")
    # Exactly one test should fail (the bite-proof); the matching-schema
    # test in the same file must still pass.
    fail_count = sum(1 for line in stub_out.splitlines() if line.startswith("FAIL ["))
    pass_count = sum(1 for line in stub_out.splitlines() if line.startswith("PASS ["))
    if fail_count != 1:
        sys.exit(f"FAIL: stub build should have exactly 1 failure (bite-proof); "
                 f"got {fail_count}. Output:\n{stub_out}")
    if pass_count != 1:
        sys.exit(f"FAIL: stub build should have exactly 1 pass (matching schema); "
                 f"got {pass_count}. Output:\n{stub_out}")
    print(f"OK: stub build → bite-proof test fails as expected, "
          f"matching-schema test still passes.")

    # ----- Pass 3: FW-03.4 determinism stub build -----
    # The stub build defines -DBOOT_TEST_STUB_FLIP_DECISION so the
    # production boot_decide_provisioning() body is swapped for the
    # stub-and-flip variant (alternates true/false on each call).
    # The bite-proof asserts call1 == call2; under the flip that
    # invariant fails and the failure message contains "determinism"
    # so the runner can verify the guard is load-bearing. The
    # green-path test in test_boot_stability_guard.c is guarded by
    # `#ifndef BOOT_TEST_STUB_FLIP_DECISION` so it does not compile
    # into this build — only the bite-proof runs.
    print()
    print("=== Pass 3: FW-03.4 stub build (BOOT_TEST_STUB_FLIP_DECISION, bite-proof only) ===")
    fw03_4_bin = _build('fw03_4_tests_stub',
                        ['-DBOOT_TEST_STUB_FLIP_DECISION=1'],
                        FW03_4_GUARD_TEST_FILES, workdir)
    fw03_4_rc, fw03_4_out = _run_binary(fw03_4_bin)
    if fw03_4_rc == 0:
        sys.exit(f"FAIL: FW-03.4 stub build returned 0; expected the bite-proof "
                 f"test to fail. Output:\n{fw03_4_out}")
    if FW03_BITE_PROOF_TEST_NAME not in fw03_4_out:
        sys.exit(f"FAIL: FW-03.4 stub build failed but the bite-proof test "
                 f"name '{FW03_BITE_PROOF_TEST_NAME}' is not in the output:\n{fw03_4_out}")
    if "FAIL" not in fw03_4_out:
        sys.exit(f"FAIL: FW-03.4 stub build rc != 0 but no FAIL line in output:\n{fw03_4_out}")
    # The failure message must mention "determinism" per the
    # milestones doc bite-proof requirement — this is how the
    # verify phase proves the guard surfaces the violated invariant.
    if "determinism" not in fw03_4_out:
        sys.exit(f"FAIL: FW-03.4 stub build failure message does not contain "
                 f"the literal 'determinism':\n{fw03_4_out}")
    # Exactly one test should fail (the bite-proof); the green-path
    # test is excluded by the `#ifndef BOOT_TEST_STUB_FLIP_DECISION`
    # guard so no passes are expected.
    fail_count = sum(1 for line in fw03_4_out.splitlines() if line.startswith("FAIL ["))
    pass_count = sum(1 for line in fw03_4_out.splitlines() if line.startswith("PASS ["))
    if fail_count != 1:
        sys.exit(f"FAIL: FW-03.4 stub build should have exactly 1 failure "
                 f"(bite-proof); got {fail_count}. Output:\n{fw03_4_out}")
    if pass_count != 0:
        sys.exit(f"FAIL: FW-03.4 stub build should have 0 passes (green-path "
                 f"test excluded by ifndef); got {pass_count}. Output:\n{fw03_4_out}")
    print(f"OK: FW-03.4 stub build → bite-proof test fails with "
          f"'determinism' message as expected.")

    # ----- Pass 4: FW-05.4 strict-validation stub build -----
    # Compile softap_handlers.c + test_softap_guard.c with
    # -DSOFTAP_TEST_STUB_ACCEPT_ALL_BODIES=1 so the validation block
    # in provision_post_handler_impl is macro-skipped. Under stub:
    #   - 3 rejection tests FAIL (non-JSON, missing-wifi_ssid,
    #     missing-wifi_password): they assert 400 + no NVS write +
    #     no esp_restart, but the handler skips validation and
    #     proceeds to merge + save + restart (200). Each failure
    #     message contains the literal "validation".
    #   - 2 acceptance tests PASS (accepts-missing-name,
    #     accepts-missing-description): the handler reaches merge
    #     with absent identity keys → preserves from cfg seed → test
    #     asserts 200 + save + restart, all of which hold.
    #   - 1 well-formed test PASSES (its assertions hold under
    #     both builds).
    # So Pass 4 expects exactly 3 FAIL + 3 PASS.
    print()
    print("=== Pass 4: FW-05.4 stub build (SOFTAP_TEST_STUB_ACCEPT_ALL_BODIES, guard file) ===")
    fw05_4_bin = _build('fw05_4_tests_stub',
                        ['-DSOFTAP_TEST_STUB_ACCEPT_ALL_BODIES=1'],
                        FW05_4_GUARD_TEST_FILES, workdir)
    fw05_4_rc, fw05_4_out = _run_binary(fw05_4_bin)
    if fw05_4_rc == 0:
        sys.exit(f"FAIL: FW-05.4 stub build returned 0; expected bite-proof "
                 f"to fail. The stub gate didn't bypass the validation "
                 f"path, so the rejection tests still passed. Output:\n{fw05_4_out}")
    if "FAIL" not in fw05_4_out:
        sys.exit(f"FAIL: FW-05.4 stub build rc != 0 but no FAIL line in "
                 f"output:\n{fw05_4_out}")
    # The failure messages must mention "validation" per the
    # milestones doc bite-proof requirement — this is how the verify
    # phase proves the guard surfaces the violated invariant.
    if "validation" not in fw05_4_out:
        sys.exit(f"FAIL: FW-05.4 stub build failure message does not "
                 f"contain the literal 'validation':\n{fw05_4_out}")
    # Exactly 3 tests should fail (non-JSON + missing-wifi_ssid +
    # missing-wifi_password). The 2 accepts-missing-* tests +
    # accepts-well-formed test must pass.
    fail_count = sum(1 for line in fw05_4_out.splitlines() if line.startswith("FAIL ["))
    pass_count = sum(1 for line in fw05_4_out.splitlines() if line.startswith("PASS ["))
    if fail_count != 3:
        sys.exit(f"FAIL: FW-05.4 stub build should have exactly 3 "
                 f"failures (non-JSON + missing-wifi_ssid + "
                 f"missing-wifi_password); got {fail_count}. "
                 f"Output:\n{fw05_4_out}")
    if pass_count != 3:
        sys.exit(f"FAIL: FW-05.4 stub build should have exactly 3 "
                 f"passes (accepts-missing-name + "
                 f"accepts-missing-description + well-formed); got "
                 f"{pass_count}. Output:\n{fw05_4_out}")
    print(f"OK: FW-05.4 stub build → 3 rejection tests fail with "
          f"'validation' message as expected; 3 green/partial tests "
          "still pass.")

    # ----- Pass 5: FW-06.4 timer-fire invariant stub build -----
    # Compile test_led_guard.c (and led.c) with
    # -DLED_TEST_STUB_DISABLE_TIMER=1 so the timer-create body in
    # led_init() is short-circuited. The guard tripwire in
    # led_set_state() fires when any blink state is requested
    # without a running timer, printing the literal "timer_fire"
    # and aborting the process. The Pass-5 runner expects:
    #   - rc != 0 (SIGABRT from the guard's abort())
    #   - literal "timer_fire" present in stdout (from the guard's
    #     printf AND from the test's marker line)
    # The green-path test (test_led_guard.c::set_state_rearms_timer)
    # is excluded from this build by the #ifndef
    # LED_TEST_STUB_DISABLE_TIMER inside the test file.
    print()
    print("=== Pass 5: FW-06.4 stub build (LED_TEST_STUB_DISABLE_TIMER, guard file) ===")
    fw06_4_bin = _build('fw06_4_tests_stub',
                        ['-DLED_TEST_STUB_DISABLE_TIMER=1'],
                        FW06_4_GUARD_TEST_FILES, workdir)
    fw06_4_rc, fw06_4_out = _run_binary(fw06_4_bin)
    # Under stub, the guard abort()s the process. Pass 5 expects
    # rc != 0 + literal "timer_fire" in stdout. Mirrors the
    # behavior of Pass 2 (schema_version) and Pass 3 (determinism).
    if fw06_4_rc == 0:
        sys.exit(f"FAIL: FW-06.4 stub build returned 0; expected "
                 f"the bite-proof guard to trip. The stub gate "
                 f"didn't bypass the timer-fire invariant. "
                 f"Output:\n{fw06_4_out}")
    if "timer_fire" not in fw06_4_out:
        sys.exit(f"FAIL: FW-06.4 stub build output does not "
                 f"contain the literal 'timer_fire':\n{fw06_4_out}")
    # The guard test name should appear in the output so the
    # runner can match it to the expected bite-proof.
    if "guard_bite_proof_timer_fire_disabled" not in fw06_4_out:
        sys.exit(f"FAIL: FW-06.4 stub build did not run the "
                 f"bite-proof test. Output:\n{fw06_4_out}")
    print(f"OK: FW-06.4 stub build → guard tripped on "
          f"'timer_fire' invariant as expected (process aborted "
          f"with rc={fw06_4_rc}).")

    # ----- Pass 6: FW-07.4 debounce invariant stub build -----
    # Compile test_button_guard.c (and button.c) with
    # -DBUTTON_TEST_STUB_DISABLE_DEBOUNCE=1 so the
    # `button_edge_is_bouncing()` body in button.c is
    # short-circuited to return false (every edge passes).
    # The bite-proof test drives the S15 jitter pattern
    # (LOW at t=0, HIGH at t=5, LOW at t=15, HIGH at t=50);
    # under stub the three jitter edges all pass and the
    # test's TEST_FAIL_MESSAGE trips with a message
    # containing the literal "debounce". The Pass-6 runner
    # expects:
    #   - rc != 0 (TEST_FAIL_MESSAGE exits non-zero)
    #   - literal "debounce" present in stdout (from the
    #     test's marker line AND from the TEST_FAIL_MESSAGE
    #     body)
    # The green-path tests (test_button_guard.c ::
    # debounce_filters_jitter_phantom_press + ::
    # debounce_does_not_swallow_clean_tap) are excluded from
    # this build by the #ifndef BUTTON_TEST_STUB_DISABLE_DEBOUNCE
    # inside the test file — only the bite-proof runs.
    print()
    print("=== Pass 6: FW-07.4 stub build (BUTTON_TEST_STUB_DISABLE_DEBOUNCE, guard file) ===")
    fw07_4_bin = _build('fw07_4_tests_stub',
                        ['-DBUTTON_TEST_STUB_DISABLE_DEBOUNCE=1'],
                        FW07_4_GUARD_TEST_FILES, workdir)
    fw07_4_rc, fw07_4_out = _run_binary(fw07_4_bin)
    # Under stub, the S15 jitter produces phantom edges and
    # the test's TEST_FAIL_MESSAGE fires. Pass 6 expects
    # rc != 0 + literal "debounce" in stdout. Mirrors the
    # behavior of Pass 5 (timer_fire).
    if fw07_4_rc == 0:
        sys.exit(f"FAIL: FW-07.4 stub build returned 0; expected "
                 f"the bite-proof guard to trip. The stub gate "
                 f"didn't bypass the debounce invariant. "
                 f"Output:\n{fw07_4_out}")
    if "debounce" not in fw07_4_out:
        sys.exit(f"FAIL: FW-07.4 stub build output does not "
                 f"contain the literal 'debounce':\n{fw07_4_out}")
    # The guard test name should appear in the output so the
    # runner can match it to the expected bite-proof.
    if "guard_bite_proof_debounce_disabled" not in fw07_4_out:
        sys.exit(f"FAIL: FW-07.4 stub build did not run the "
                 f"bite-proof test. Output:\n{fw07_4_out}")
    print(f"OK: FW-07.4 stub build → guard tripped on "
          f"'debounce' invariant as expected (test failed "
          f"with rc={fw07_4_rc}).")

    # ----- Pass 7: FW-08.3 bounded-wait invariant stub build -----
    # Compile test_wifi_guard.c (and wifi.c) with
    # -DWIFI_TEST_STUB_USE_BLOCKING_WAIT=1 so wifi_init()'s
    # first-esp_wifi_connect branch short-circuits into the
    # guard tripwire. The bite-proof test asserts the guard
    # fires with the literal "bounded_wait" in the message.
    # The Pass-7 runner expects:
    #   - rc != 0 (TEST_FAIL_MESSAGE exits non-zero via longjmp
    #     or via the abort path)
    #   - literal "bounded_wait" present in stdout (from the
    #     test's marker line AND from the TEST_FAIL_MESSAGE
    #     body)
    # The green-path test in test_wifi_guard.c ::
    # test_fw08_3_misconfigured_ssid_returns_invalid_arg is
    # excluded from this build by the `#ifndef WIFI_TEST_STUB
    # _USE_BLOCKING_WAIT` inside the file — only the bite-proof
    # runs.
    print()
    print("=== Pass 7: FW-08.3 stub build (WIFI_TEST_STUB_USE_BLOCKING_WAIT, guard file) ===")
    fw08_3_bin = _build('fw08_3_tests_stub',
                        ['-DWIFI_TEST_STUB_USE_BLOCKING_WAIT=1'],
                        FW08_3_GUARD_TEST_FILES, workdir)
    fw08_3_rc, fw08_3_out = _run_binary(fw08_3_bin)
    # Under stub, the wifi_init() guard tripwire fires
    # TEST_ASSERT_MESSAGE(0, "bounded_wait invariant violated: ...").
    # The runner greps for the literal keyword in stdout.
    if fw08_3_rc == 0:
        sys.exit(f"FAIL: FW-08.3 stub build returned 0; expected "
                 f"the bite-proof guard to trip. The stub gate "
                 f"didn't bypass the bounded-wait invariant. "
                 f"Output:\n{fw08_3_out}")
    if WIFI_BITE_PROOF_KEYWORD not in fw08_3_out:
        sys.exit(f"FAIL: FW-08.3 stub build output does not "
                 f"contain the literal '{WIFI_BITE_PROOF_KEYWORD}':\n{fw08_3_out}")
    # The guard test name should appear in the output so the
    # runner can match it to the expected bite-proof.
    if "guard_bite_proof_blocking_wait_rejected" not in fw08_3_out:
        sys.exit(f"FAIL: FW-08.3 stub build did not run the "
                 f"bite-proof test. Output:\n{fw08_3_out}")
    print(f"OK: FW-08.3 stub build → guard tripped on "
          f"'{WIFI_BITE_PROOF_KEYWORD}' invariant as expected "
          f"(test failed with rc={fw08_3_rc}).")

    # ----- Pass 8: FW-08.6 teardown-on-IP invariant stub build -----
    # Compile test_wifi_event_guard.c (and wifi_event.c) with
    # -DWIFI_TEST_STUB_SKIP_IP_UP_HANDLER=1 so on_sta_got_ip
    # _handler() is replaced by a no-op + guard tripwire. The
    # bite-proof test asserts the guard fires with the literal
    # "teardown" in the message. The Pass-8 runner expects:
    #   - rc != 0 (TEST_FAIL_MESSAGE exits non-zero)
    #   - literal "teardown" present in stdout (from the test's
    #     marker line AND from the TEST_FAIL_MESSAGE body)
    #   - "guard_bite_proof_teardown_on_ip_disabled" test name
    #     in stdout
    # Mirrors the FW-06.4 LED_TEST_STUB_DISABLE_TIMER +
    # FW-07.4 BUTTON_TEST_STUB_DISABLE_DEBOUNCE + FW-08.3
    # WIFI_TEST_STUB_USE_BLOCKING_WAIT pattern exactly.
    print()
    print("=== Pass 8: FW-08.6 stub build (WIFI_TEST_STUB_SKIP_IP_UP_HANDLER, guard file) ===")
    fw08_6_bin = _build('fw08_6_tests_stub',
                        ['-DWIFI_TEST_STUB_SKIP_IP_UP_HANDLER=1'],
                        FW08_6_GUARD_TEST_FILES, workdir)
    fw08_6_rc, fw08_6_out = _run_binary(fw08_6_bin)
    if fw08_6_rc == 0:
        sys.exit(f"FAIL: FW-08.6 stub build returned 0; expected "
                 f"the bite-proof guard to trip. The stub gate "
                 f"didn't bypass the teardown-on-IP invariant. "
                 f"Output:\n{fw08_6_out}")
    if WIFI_EVENT_BITE_PROOF_KEYWORD not in fw08_6_out:
        sys.exit(f"FAIL: FW-08.6 stub build output does not "
                 f"contain the literal '{WIFI_EVENT_BITE_PROOF_KEYWORD}':\n{fw08_6_out}")
    # The guard test name should appear in the output so the
    # runner can match it to the expected bite-proof.
    if "guard_bite_proof_teardown_on_ip_disabled" not in fw08_6_out:
        sys.exit(f"FAIL: FW-08.6 stub build did not run the "
                 f"bite-proof test. Output:\n{fw08_6_out}")
    print(f"OK: FW-08.6 stub build → guard tripped on "
          f"'{WIFI_EVENT_BITE_PROOF_KEYWORD}' invariant as expected "
          f"(test failed with rc={fw08_6_rc}).")

    # ----- Pass 9: FW-10.3 no-reinit invariant stub build -----
    # Compile test_camera_guard.c (and camera.c) with
    # -DCAMERA_TEST_STUB_REINIT=1 so camera_init()'s body takes
    # the re-entry path on the second invocation. The guard
    # tripwire fires TEST_FAIL_MESSAGE with the literal
    # substring "no_reinit". Pass 9 expects:
    #   - rc != 0 (TEST_FAIL_MESSAGE exits non-zero)
    #   - literal "no_reinit" present in stdout (from the test's
    #     marker line + the TEST_FAIL_MESSAGE body)
    #   - "guard_bite_proof_no_reinit_rejected" test name in
    #     stdout so the runner can match it to the expected
    #     bite-proof.
    # Mirrors Pass 7 (WIFI_TEST_STUB_USE_BLOCKING_WAIT) + Pass 8
    # (WIFI_TEST_STUB_SKIP_IP_UP_HANDLER) pattern.
    print()
    print("=== Pass 9: FW-10.3 stub build (CAMERA_TEST_STUB_REINIT, guard file) ===")
    fw10_3_bin = _build('fw10_3_tests_stub',
                        ['-DCAMERA_TEST_STUB_REINIT=1'],
                        FW10_3_GUARD_TEST_FILES, workdir)
    fw10_3_rc, fw10_3_out = _run_binary(fw10_3_bin)
    # Under stub, camera_init()'s guard tripwire fires
    # TEST_FAIL_MESSAGE(0, "no_reinit invariant violated: ...").
    # The runner greps for the literal keyword in stdout.
    if fw10_3_rc == 0:
        sys.exit(f"FAIL: FW-10.3 stub build returned 0; expected "
                 f"the bite-proof guard to trip. The stub gate "
                 f"didn't bypass the no-reinit invariant. "
                 f"Output:\n{fw10_3_out}")
    if CAMERA_BITE_PROOF_KEYWORD not in fw10_3_out:
        sys.exit(f"FAIL: FW-10.3 stub build output does not "
                 f"contain the literal '{CAMERA_BITE_PROOF_KEYWORD}':\n{fw10_3_out}")
    # The guard test name should appear in the output so the
    # runner can match it to the expected bite-proof.
    if "guard_bite_proof_no_reinit_rejected" not in fw10_3_out:
        sys.exit(f"FAIL: FW-10.3 stub build did not run the "
                 f"bite-proof test. Output:\n{fw10_3_out}")
    print(f"OK: FW-10.3 stub build → guard tripped on "
          f"'{CAMERA_BITE_PROOF_KEYWORD}' invariant as expected "
          f"(test failed with rc={fw10_3_rc}).")

    # ----- Pass 10: FW-11.3 single-owner invariant stub build -----
    # Compile test_capture_guard.c (and capture.c) with
    # -DCAPTURE_TEST_STUB_SECOND_CALLER=1 so capture_task_start()
    # short-circuits into capture_guard_fail_single_owner() with
    # the literal "single_owner" in TEST_FAIL_MESSAGE. Pass 10
    # expects:
    #   - rc != 0 (TEST_FAIL_MESSAGE exits non-zero)
    #   - literal "single_owner" present in stdout (from the
    #     test's marker line + the TEST_FAIL_MESSAGE body)
    #   - "guard_bite_proof_single_owner_rejected" test name in
    #     stdout so the runner can match it to the expected
    #     bite-proof.
    # Mirrors Pass 9 (CAMERA_TEST_STUB_REINIT) shape exactly.
    print()
    print("=== Pass 10: FW-11.3 stub build (CAPTURE_TEST_STUB_SECOND_CALLER, guard file) ===")
    fw11_3_bin = _build('fw11_3_tests_stub',
                        ['-DCAPTURE_TEST_STUB_SECOND_CALLER=1'],
                        FW11_3_GUARD_TEST_FILES, workdir)
    fw11_3_rc, fw11_3_out = _run_binary(fw11_3_bin)
    if fw11_3_rc == 0:
        sys.exit(f"FAIL: FW-11.3 stub build returned 0; expected "
                 f"the bite-proof guard to trip. The stub gate "
                 f"didn't bypass the single_owner invariant. "
                 f"Output:\n{fw11_3_out}")
    if CAPTURE_BITE_PROOF_KEYWORD not in fw11_3_out:
        sys.exit(f"FAIL: FW-11.3 stub build output does not "
                 f"contain the literal '{CAPTURE_BITE_PROOF_KEYWORD}':\n{fw11_3_out}")
    if "guard_bite_proof_single_owner_rejected" not in fw11_3_out:
        sys.exit(f"FAIL: FW-11.3 stub build did not run the "
                 f"bite-proof test. Output:\n{fw11_3_out}")
    print(f"OK: FW-11.3 stub build → guard tripped on "
          f"'{CAPTURE_BITE_PROOF_KEYWORD}' invariant as expected "
          f"(test failed with rc={fw11_3_rc}).")

    # ----- Pass 12: FW-14 clean-CLOSE sleep-invariant stub build -----
    # Compile test_ws_close_guard.c (and the production sources) with
    # -DWS_TEST_STUB_ENABLE_CLOSE_RECONNECT=1 so the latch check in
    # ws_event_handler.c's failure path is compiled out. The
    # bite-proof test drives ws_init → CLOSED(1000) → DISCONNECTED
    # and asserts NOTHING was scheduled. Under the stub the failure
    # path schedules anyway and TEST_FAIL_MESSAGE fires with the
    # literal "close_no_reconnect". Pass 12 expects:
    #   - rc != 0 (TEST_FAIL_MESSAGE exits non-zero)
    #   - literal "close_no_reconnect" present in stdout (from the
    #     test's marker line AND from the TEST_FAIL_MESSAGE body)
    #   - "test_pass12_clean_close_must_not_schedule" test name in
    #     stdout so the runner can match it to the expected
    #     bite-proof.
    print()
    print("=== Pass 12: FW-14 stub build (WS_TEST_STUB_ENABLE_CLOSE_RECONNECT, guard file) ===")
    fw14_bin = _build('fw14_tests_stub',
                      ['-DWS_TEST_STUB_ENABLE_CLOSE_RECONNECT=1'],
                      FW14_GUARD_TEST_FILES, workdir)
    fw14_rc, fw14_out = _run_binary(fw14_bin)
    if fw14_rc == 0:
        sys.exit(f"FAIL: FW-14 stub build returned 0; expected "
                 f"the bite-proof guard to trip. The stub gate "
                 f"didn't bypass the close_no_reconnect invariant. "
                 f"Output:\n{fw14_out}")
    if WS_CLOSE_GUARD_BITE_PROOF_KEYWORD not in fw14_out:
        sys.exit(f"FAIL: FW-14 stub build output does not "
                 f"contain the literal '{WS_CLOSE_GUARD_BITE_PROOF_KEYWORD}':\n{fw14_out}")
    if "test_pass12_clean_close_must_not_schedule" not in fw14_out:
        sys.exit(f"FAIL: FW-14 stub build did not run the "
                 f"bite-proof test. Output:\n{fw14_out}")
    print(f"OK: FW-14 stub build → guard tripped on "
          f"'{WS_CLOSE_GUARD_BITE_PROOF_KEYWORD}' invariant as expected "
          f"(test failed with rc={fw14_rc}).")

    # ----- Pass 13: FW-16.3 healthy-stream bite-proof stub build -----
    _pass16_bite_proof(workdir)

    print()
    print("=== FW-02 + FW-03 + FW-05 + FW-06 + FW-07 + FW-08 + FW-10 + FW-11 + FW-13 host tests: ALL PASS (production) + bite-proofs FAIL (stubs) ===")
    print(f"workdir kept at {workdir} for debugging; safe to rm -rf.")


if __name__ == '__main__':
    main()