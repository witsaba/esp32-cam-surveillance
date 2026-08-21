#!/usr/bin/env python3
"""run_host_tests.py — FW-02 host-side Unity test runner.

Builds and runs the FW-02 host tests against the in-memory NVS mock.
Two compile passes:

  1. Without `-DCONFIG_TEST_STUB_VERSION_CHECK`: the production
     schema-version guard is active. ALL tests must pass.

  2. With `-DCONFIG_TEST_STUB_VERSION_CHECK`: the guard is stubbed
     out. The FW-02.3 bite-proof test MUST FAIL — that failure
     proves the guard is load-bearing. All other tests must pass.

Exits 0 only when both passes satisfy their expected outcomes.
Exits 1 on any unexpected pass/fail pattern (regression in either
direction).

Why this exists: ESP-IDF v5.5.3's stock `idf.py` does not ship a
host-side Unity test runner for the esp32 target — the canonical
host test pattern uses a separate linux-targeted Catch2 project
under each component's `host_test/`/` directory. For FW-02 we keep
the tests inside the firmware project (per the orchestrator's plan)
and compile them on the host via plain gcc. The
`mock_nvs_flash_link.h` macro-override header means `config.c`'s
`nvs_*` calls redirect to the in-memory mock — no real flash
required.

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
    flags = [
        '-std=c11',
        '-Wall', '-Wextra', '-Wno-unused-parameter',
        f'-I{host_include}',                      # unity_config.h (host shim)
        f'-I{tools_dir}',                         # unity_host_test_runner.h
        f'-I{UNITY_INTERNALS}',                   # unity_internals.h, unity.h
        f'-I{NVS_INCLUDE}',                       # nvs.h types
        f'-I{esp_common_include}',                # esp_err.h
        f'-I{log_include}',                       # esp_log.h
        f'-I{PROJECT_DIR}/components/config/include',
        f'-I{PROJECT_DIR}/components/boot/include',
        f'-I{PROJECT_DIR}/components/mocks/include',
        '-DUNITY_INCLUDE_CONFIG_H',
        '-DUNITY_HOST_BUILD',                     # select host test_runner shim
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
        os.path.join(PROJECT_DIR, 'components', 'config', 'config.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_nvs_flash.cpp'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_boot_button.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_init_returns.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_supervision_record.c'),
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_log.c'),
        os.path.join(PROJECT_DIR, 'components', 'boot', 'boot.c'),
        os.path.join(PROJECT_DIR, 'components', 'boot', 'boot_button_stub.c'),
        os.path.join(PROJECT_DIR, 'components', 'boot', 'stub_inits.c'),
        os.path.join(PROJECT_DIR, 'components', 'boot', 'stub_supervision.c'),
        os.path.join(os.path.dirname(__file__), 'host_test_main.c'),
        os.path.join(os.path.dirname(__file__), 'host_idf_runner_shim.c'),
        os.path.join(PROJECT_DIR, 'tests', 'host_include', 'host_err_to_name.c'),
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
]

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
]


def main():
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

    print()
    print("=== FW-02 host tests: ALL PASS (production) + bite-proof FAILS (stub) ===")
    print(f"workdir kept at {workdir} for debugging; safe to rm -rf.")


if __name__ == '__main__':
    main()