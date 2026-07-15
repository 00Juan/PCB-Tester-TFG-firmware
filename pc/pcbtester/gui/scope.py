"""Scope tab: on-demand voltage burst capture (ch.capture) plotted vs time.

The capture is blocking on the firmware side (telemetry pauses while it runs),
so the button disarms until the capture event arrives or times out.
"""

from __future__ import annotations

from typing import Callable, Optional

import pyqtgraph as pg
from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (
    QComboBox, QHBoxLayout, QLabel, QPushButton, QSpinBox, QVBoxLayout, QWidget,
)

from .palette import CATEGORICAL, GRID_ALPHA, SURFACE

_CHANNELS = [f"CH{i}" for i in range(1, 9)] + ["CH9 (HP1)", "CH10 (HP2)", "CH11 (HV)"]


class ScopeTab(QWidget):
    def __init__(self, request_capture: Callable[[int, int, int], bool]) -> None:
        """request_capture(ch, n, dt_ms) sends the command; the capture event
        arrives later via on_capture()."""
        super().__init__()
        self._request = request_capture
        self._armed_ch: Optional[int] = None

        controls = QHBoxLayout()
        self.ch_combo = QComboBox()
        self.ch_combo.addItems(_CHANNELS)
        self.n_spin = QSpinBox(minimum=2, maximum=512, value=256,
                               toolTip="Number of samples")
        self.dt_spin = QSpinBox(minimum=1, maximum=100, value=2, suffix=" ms",
                                toolTip="Sample interval")
        self.capture_btn = QPushButton("Capture")
        self.capture_btn.clicked.connect(self._capture)
        self.info_label = QLabel("")

        controls.addWidget(QLabel("Channel:"))
        controls.addWidget(self.ch_combo)
        controls.addWidget(QLabel("Samples:"))
        controls.addWidget(self.n_spin)
        controls.addWidget(QLabel("Interval:"))
        controls.addWidget(self.dt_spin)
        controls.addWidget(self.capture_btn)
        controls.addWidget(self.info_label, 1)

        self.plot = pg.PlotWidget(background=SURFACE)
        self.plot.setLabel("left", "V")
        self.plot.setLabel("bottom", "time", units="ms")
        self.plot.showGrid(x=True, y=True, alpha=GRID_ALPHA)
        self.curve = self.plot.plot(pen=pg.mkPen(CATEGORICAL[0], width=2))

        layout = QVBoxLayout(self)
        layout.addLayout(controls)
        layout.addWidget(self.plot, 1)

        self._timeout = QTimer(self, singleShot=True, timeout=self._on_timeout)

    def _capture(self) -> None:
        ch = self.ch_combo.currentIndex() + 1
        n, dt = self.n_spin.value(), self.dt_spin.value()
        if n * dt > 5000:
            self.info_label.setText("n × dt must be ≤ 5000 ms")
            return
        if not self._request(ch, n, dt):
            return
        self._armed_ch = ch
        self.capture_btn.setEnabled(False)
        self.info_label.setText(f"capturing CH{ch}: {n} × {dt} ms "
                                f"({n * dt} ms window)…")
        self._timeout.start(n * dt + 3000)

    def on_capture(self, msg: dict) -> None:
        if self._armed_ch is None or msg.get("ch") != self._armed_ch:
            return
        self._disarm()
        dt = msg.get("dt_ms", 1)
        samples = msg.get("samples", [])
        self.curve.setData([k * dt for k in range(len(samples))], samples)
        self.plot.setLabel("left", msg.get("unit", "V"))
        if samples:
            vmin, vmax = min(samples), max(samples)
            self.info_label.setText(
                f"CH{msg.get('ch')}: {len(samples)} pts · min {vmin:.3f} · "
                f"max {vmax:.3f} · pk-pk {vmax - vmin:.3f} {msg.get('unit', 'V')}")

    def _on_timeout(self) -> None:
        if self._armed_ch is not None:
            self.info_label.setText("capture timed out")
            self._disarm()

    def _disarm(self) -> None:
        self._armed_ch = None
        self.capture_btn.setEnabled(True)
        self._timeout.stop()
