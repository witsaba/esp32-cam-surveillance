"""firmware/idf_ext.py — project-local `idf.py` extensions.

Adds a `test` action that compiles the FW-02 host-side Unity tests
against the project's own `config` component and the in-memory NVS
mock, then runs the resulting binary. This implements the
`idf.py test --target esp32` command the orchestrator's plan calls for,
without requiring a real ESP32 device or QEMU.

Why project-local: ESP-IDF v5.5.3's stock `idf.py` does not include a
`test` action that targets esp32 for Unity-based host tests. The
host-test pattern in `components/*/host_test/` is a separate
linux-targeted Catch2 project, not a Unity test runner. We extend
`idf.py` in place rather than maintaining a parallel project.

Acceptance semantics: this action exits 0 if all FW-02 host tests pass
and 1 otherwise. The exact command is
`idf.py test --target esp32` (the `--target` flag is accepted and
ignored — we always run on the host for FW-02).
"""

import os
import shlex
import subprocess
import sys


def _idf_py_log(msg):
    """Print a log line to stdout in idf.py's style."""
    print(msg)


def action_extensions(base_actions, project_path=os.getcwd()):
    """Register the `test` action on `idf.py`."""

    def test_action(action_name, ctx, args):
        """Build + run the FW-02 host tests via gcc against Unity."""
        # We accept --target esp32 (or anything) for API compatibility
        # with the orchestrator's plan; we always run on host.
        for _ in getattr(args, 'target', []) or []:
            pass

        tools_dir = os.path.join(project_path, 'tools')
        runner = os.path.join(tools_dir, 'run_host_tests.py')
        if not os.path.exists(runner):
            _idf_py_log(f"ERROR: host test runner not found: {runner}")
            sys.exit(1)

        cmd = [sys.executable, runner, project_path]
        # Pass through any --target flag values (ignored by the runner).
        for t in getattr(args, 'target', []) or []:
            cmd += ['--target', t]

        _idf_py_log(f"Running FW-02 host tests: {' '.join(shlex.quote(c) for c in cmd)}")
        result = subprocess.run(cmd)
        sys.exit(result.returncode)

    extensions = {
        'global_options': [{
            'names': ['--target'],
            'help': 'Accepted for API compatibility; the FW-02 host tests '
                    'always run on the host (no device flashing). Ignored.',
            'multiple': True,
            'scope': 'shared',
        }],
        'actions': {
            'test': {
                'callback': test_action,
                'help': 'Run FW-02 host-side Unity tests against the '
                        'in-memory NVS mock. Implementation: tools/'
                        'run_host_tests.py.',
                'options': [],
            },
        },
    }
    return extensions