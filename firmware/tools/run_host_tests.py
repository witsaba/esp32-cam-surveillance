#!/usr/bin/env python3
"""run_host_tests.py — FW-02 host-side Unity test runner.

Compiles the FW-02 `config` component + the in-memory NVS mock + the
host-side Unity tests with the system gcc/g++, links against the IDF
Unity sources, and runs the resulting binary. Exits 0 if every test
passes and non-zero otherwise.

Why this exists: ESP-IDF v5.5.3's stock `idf.py` does not ship a
host-side Unity test runner for the esp32 target — the canonical host
test pattern uses a separate linux-targeted Catch2 project under each
component's `host_test/` directory. For FW-02 we keep the tests inside
the firmware project (per the orchestrator's plan) and compile them on
the host via plain gcc. The mock_nvs_flash_link.h macro-override
header means `config.c`'s `nvs_*` calls redirect to the in-memory
mock — no real flash required.

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
UNITY_INCLUDE = os.path.join(IDF_PATH, 'components', 'unity', 'include')
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


def main():
    cc, cxx = _find_compiler()
    workdir = tempfile.mkdtemp(prefix='fw02-host-tests-')
    print(f"FW-02 host test runner")
    print(f"  project_dir: {PROJECT_DIR}")
    print(f"  IDF_PATH:    {IDF_PATH}")
    print(f"  workdir:     {workdir}")

    sources = [
        # Unity framework
        UNITY_SRC,
        # Production code under test
        os.path.join(PROJECT_DIR, 'components', 'config', 'config.c'),
        # In-memory NVS mock
        os.path.join(PROJECT_DIR, 'components', 'mocks', 'mock_nvs_flash.cpp'),
        # Host test driver (main + run-all wrapper)
        os.path.join(os.path.dirname(__file__), 'host_test_main.c'),
        # Host shim of IDF's unity_runner.c (provides the
        # unity_testcase_register() that TEST_CASE macros call).
        os.path.join(os.path.dirname(__file__), 'host_idf_runner_shim.c'),
        # FW-02.1 test cases
        os.path.join(PROJECT_DIR, 'tests', 'test_config', 'test_config_load_fresh.c'),
        os.path.join(PROJECT_DIR, 'tests', 'test_config', 'test_config_roundtrip.c'),
    ]
    for s in sources:
        if not os.path.exists(s):
            sys.exit(f"ERROR: missing source file: {s}")

    # The IDF unity_config.h pulls in sdkconfig.h and esp_err.h — neither
    # exists in a plain gcc host build. We ship our own unity_config.h
    # under firmware/tests/host_include/ which is added to the include
    # path BEFORE the IDF path. Unity's `#include "unity_config.h"`
    # resolves to our shim, leaving the IDF shim unused on host.
    # We still pull in the IDF headers for `esp_err.h` and `esp_log.h`
    # because `config.c` uses `ESP_LOGW` and the `esp_err_t` enum.
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    host_include = os.path.join(PROJECT_DIR, 'tests', 'host_include')
    cflags = [
        '-std=c11',
        '-Wall', '-Wextra', '-Wno-unused-parameter',
        f'-I{host_include}',                      # unity_config.h (host shim)
        f'-I{tools_dir}',                         # unity_host_test_runner.h
        f'-I{UNITY_INTERNALS}',                   # unity_internals.h, unity.h
        f'-I{NVS_INCLUDE}',                       # nvs.h, nvs_flash.h types
        f'-I{os.path.join(IDF_PATH, "components", "log", "include")}',        # esp_log.h
        f'-I{os.path.join(IDF_PATH, "components", "esp_common", "include")}', # esp_err.h
        f'-I{PROJECT_DIR}/components/config/include',
        f'-I{PROJECT_DIR}/components/mocks/include',
        '-DUNITY_INCLUDE_CONFIG_H',
        '-DUNITY_HOST_BUILD',                     # select host test_runner shim
    ]
    cxxflags = list(cflags) + ['-std=c++17']

    out_bin = os.path.join(workdir, 'fw02_tests')

    # Build in one shot — the project is small. Split by language to
    # keep gcc/g++ happy.
    cc_objs = []
    cxx_objs = []
    for src in sources:
        if src.endswith('.cpp'):
            obj = os.path.join(workdir, os.path.basename(src) + '.o')
            _run([cxx, *cxxflags, '-c', src, '-o', obj])
            cxx_objs.append(obj)
        else:
            obj = os.path.join(workdir, os.path.basename(src) + '.o')
            _run([cc, *cflags, '-c', src, '-o', obj])
            cc_objs.append(obj)

    # Link with C++ driver last so the linker uses g++ for the merge.
    _run([cxx, '-o', out_bin, *cc_objs, *cxx_objs])

    # Run; inherit stdout so Unity's PASS/FAIL output goes straight to
    # the orchestrator's terminal.
    print()
    print(f"== Running FW-02 host tests ==")
    run = subprocess.run([out_bin])
    sys.exit(run.returncode)


if __name__ == '__main__':
    main()