"""GUI smoke test: boots the PySide6 app offscreen against the mock device
and verifies telemetry reaches the dashboard (the app's --selftest mode)."""

import os
import subprocess
import sys

import pytest

pytest.importorskip("PySide6")


def test_gui_selftest_offscreen():
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    result = subprocess.run(
        [sys.executable, "-m", "pcbtester.gui", "--selftest"],
        env=env, capture_output=True, text=True, timeout=60,
    )
    assert "SELFTEST OK" in result.stdout, result.stdout + result.stderr
    assert result.returncode == 0
