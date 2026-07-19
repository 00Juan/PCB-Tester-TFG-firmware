"""Settling tab: LVLP output step-response capture (ch.settle).

Drives one LVLP channel from a start voltage to a target voltage and captures
the output at microsecond spacing. The apparent current (vcmd - vout)/rshunt is
reconstructed over time so the operator can see the post-step transient and read
off how long it stays above a chosen current limit — the number that sizes the
firmware's over-current debounce window (OVERCURRENT_DEBOUNCE_MS).

Like the scope capture, the firmware side is blocking (telemetry pauses while it
runs), so the button disarms until the capture event arrives or times out.
"""

from __future__ import annotations

import math
from typing import Callable, Dict, List, Optional

import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QComboBox, QDoubleSpinBox, QFormLayout, QHBoxLayout, QLabel,
    QPushButton, QSpinBox, QVBoxLayout, QWidget,
)

from .palette import CATEGORICAL, GRID_ALPHA, SURFACE

_LVLP_CHANNELS = [f"CH{i}" for i in range(1, 9)]

# One firmware channel-update period (app.cpp UPDATE_PERIOD_MS). Used only to
# suggest a debounce margin on top of the measured settling time.
_UPDATE_PERIOD_MS = 50.0


def compute_settling(samples: List[float], dt_us: float, vcmd: float,
                     rshunt: float, imax_a: float) -> Dict[str, object]:
    """Reconstruct the step response and size the debounce from it.

    Returns a dict with time (ms), voltage (V) and apparent-current (mA)
    arrays, the settled voltage, the time the |current| finally stays below
    ``imax_a``, and a suggested debounce window (ms). ``settle_ms`` is 0.0 when
    the current is below the limit for the whole capture and None when it never
    settles below it.
    """
    n = len(samples)
    t_ms = [k * dt_us / 1000.0 for k in range(n)]
    i_ma = [(vcmd - v) / rshunt * 1000.0 for v in samples]

    tail = max(1, n // 20)
    vfinal = sum(samples[-tail:]) / tail

    # First instant after which |current| stays below the limit for the rest of
    # the capture (mirrors the firmware's persistence logic).
    settle_ms: Optional[float] = None
    below = False
    for k in range(n):
        if abs(i_ma[k]) < imax_a * 1000.0:
            if not below:
                below = True
                settle_ms = t_ms[k]
        else:
            below = False
            settle_ms = None

    suggested_ms: Optional[float] = None
    if settle_ms is not None:
        suggested_ms = math.ceil(settle_ms) + _UPDATE_PERIOD_MS

    return {
        "t_ms": t_ms, "v": samples, "i_ma": i_ma, "vfinal": vfinal,
        "settle_ms": settle_ms, "suggested_ms": suggested_ms,
    }


class SettlingTab(QWidget):
    def __init__(self,
                 request_settle: Callable[[int, float, float, int, int, int], bool]) -> None:
        """request_settle(ch, from_v, to_v, n, dt_us, settle_ms) sends the
        command; the capture event arrives later via on_capture()."""
        super().__init__()
        self._request = request_settle
        self._armed_ch: Optional[int] = None

        # ---- controls ------------------------------------------------------
        self.ch_combo = QComboBox()
        self.ch_combo.addItems(_LVLP_CHANNELS)
        self.from_spin = QDoubleSpinBox(minimum=0.0, maximum=15.0, value=1.0,
                                        decimals=2, singleStep=0.5, suffix=" V")
        self.to_spin = QDoubleSpinBox(minimum=0.0, maximum=15.0, value=10.0,
                                      decimals=2, singleStep=0.5, suffix=" V")
        self.n_spin = QSpinBox(minimum=2, maximum=512, value=300,
                               toolTip="Number of samples")
        self.dt_spin = QSpinBox(minimum=50, maximum=100000, value=333,
                                singleStep=50, suffix=" µs",
                                toolTip="Sample spacing (n × dt ≤ 2 s)")
        self.settle_spin = QSpinBox(minimum=0, maximum=2000, value=500,
                                    singleStep=50, suffix=" ms",
                                    toolTip="Pre-step settle time at the start voltage")
        self.limit_spin = QDoubleSpinBox(minimum=0.01, maximum=1000.0, value=70.0,
                                         decimals=2, singleStep=1.0, suffix=" mA",
                                         toolTip="Over-current limit to evaluate "
                                                 "settling against")

        self.capture_btn = QPushButton("Capture step")
        self.capture_btn.clicked.connect(self._capture)

        form = QFormLayout()
        form.addRow("Channel:", self.ch_combo)
        form.addRow("From:", self.from_spin)
        form.addRow("To:", self.to_spin)
        form.addRow("Samples:", self.n_spin)
        form.addRow("Interval:", self.dt_spin)
        form.addRow("Pre-settle:", self.settle_spin)
        form.addRow("Current limit:", self.limit_spin)

        left = QVBoxLayout()
        left.addLayout(form)
        left.addWidget(self.capture_btn)
        self.result_label = QLabel("Run a capture to measure the step response.")
        self.result_label.setWordWrap(True)
        left.addWidget(self.result_label)
        left.addStretch(1)
        left_w = QWidget()
        left_w.setLayout(left)
        left_w.setMaximumWidth(280)

        # ---- plots ---------------------------------------------------------
        self.v_plot = pg.PlotWidget(background=SURFACE)
        self.v_plot.setLabel("left", "output", units="V")
        self.v_plot.showGrid(x=True, y=True, alpha=GRID_ALPHA)
        self.v_curve = self.v_plot.plot(pen=pg.mkPen(CATEGORICAL[0], width=2))

        self.i_plot = pg.PlotWidget(background=SURFACE)
        self.i_plot.setLabel("left", "apparent current", units="mA")
        self.i_plot.setLabel("bottom", "time", units="ms")
        self.i_plot.showGrid(x=True, y=True, alpha=GRID_ALPHA)
        self.i_plot.setXLink(self.v_plot)
        self.i_curve = self.i_plot.plot(pen=pg.mkPen(CATEGORICAL[5], width=2))

        limit_pen = pg.mkPen(CATEGORICAL[7], width=1, style=Qt.DashLine)
        self._limit_hi = pg.InfiniteLine(angle=0, movable=False, pen=limit_pen)
        self._limit_lo = pg.InfiniteLine(angle=0, movable=False, pen=limit_pen)
        self.i_plot.addItem(self._limit_hi)
        self.i_plot.addItem(self._limit_lo)
        settle_pen = pg.mkPen(CATEGORICAL[3], width=1, style=Qt.DashLine)
        self._settle_line = pg.InfiniteLine(angle=90, movable=False, pen=settle_pen)
        self.i_plot.addItem(self._settle_line)
        self._settle_line.hide()

        plots = QVBoxLayout()
        plots.addWidget(self.v_plot, 1)
        plots.addWidget(self.i_plot, 1)

        root = QHBoxLayout(self)
        root.addWidget(left_w)
        root.addLayout(plots, 1)

        self._timeout = QTimer(self, singleShot=True, timeout=self._on_timeout)

    # ---------------------------------------------------------------------- #

    def _capture(self) -> None:
        ch = self.ch_combo.currentIndex() + 1
        from_v = self.from_spin.value()
        to_v = self.to_spin.value()
        n = self.n_spin.value()
        dt_us = self.dt_spin.value()
        settle_ms = self.settle_spin.value()
        if n * dt_us > 2_000_000:
            self.result_label.setText("n × interval must be ≤ 2 s")
            return
        if not self._request(ch, from_v, to_v, n, dt_us, settle_ms):
            return
        self._armed_ch = ch
        self.capture_btn.setEnabled(False)
        self.result_label.setText(
            f"Capturing CH{ch}: {from_v:.2f} → {to_v:.2f} V, "
            f"{n} × {dt_us} µs after {settle_ms} ms settle…")
        # Firmware needs settle_ms + capture window before it answers.
        self._timeout.start(settle_ms + n * dt_us // 1000 + 4000)

    def on_capture(self, msg: dict) -> None:
        if msg.get("kind") != "settling":
            return
        if self._armed_ch is None or msg.get("ch") != self._armed_ch:
            return
        self._disarm()

        samples = msg.get("samples", [])
        if not samples:
            self.result_label.setText("empty capture")
            return
        dt_us = msg.get("dt_us", 333)
        vcmd = msg.get("vcmd", msg.get("to", 0.0))
        rshunt = msg.get("rshunt", 10.5)
        imax_a = self.limit_spin.value() / 1000.0

        res = compute_settling(samples, dt_us, vcmd, rshunt, imax_a)
        self.v_curve.setData(res["t_ms"], res["v"])
        self.i_curve.setData(res["t_ms"], res["i_ma"])

        limit_ma = self.limit_spin.value()
        self._limit_hi.setPos(limit_ma)
        self._limit_lo.setPos(-limit_ma)

        settle_ms = res["settle_ms"]
        if settle_ms is not None and settle_ms > 0:
            self._settle_line.setPos(settle_ms)
            self._settle_line.show()
        else:
            self._settle_line.hide()

        self.result_label.setText(self._format_result(msg, res))

    def _format_result(self, msg: dict, res: Dict[str, object]) -> str:
        ch = msg.get("ch")
        vfinal = res["vfinal"]
        settle_ms = res["settle_ms"]
        suggested = res["suggested_ms"]
        imax_dev = msg.get("imax")
        lines = [
            f"CH{ch}: {msg.get('from')} → {msg.get('to')} V",
            f"Vfinal: {vfinal:.4f} V   (Vcmd {msg.get('vcmd')} V)",
            f"{len(msg.get('samples', []))} samples · {msg.get('dt_us')} µs",
        ]
        if settle_ms is None:
            lines.append(
                f"⚠ |current| never drops below {self.limit_spin.value():.2f} mA "
                f"— likely a steady-state offset, not a transient.")
        elif settle_ms == 0.0:
            lines.append(
                f"Already below {self.limit_spin.value():.2f} mA at t=0 "
                f"(no over-limit transient).")
        else:
            lines.append(
                f"Clears {self.limit_spin.value():.2f} mA at {settle_ms:.2f} ms")
            if suggested is not None:
                lines.append(f"→ suggest OVERCURRENT_DEBOUNCE_MS ≥ {suggested:.0f} ms")
        if imax_dev is not None:
            lines.append(f"(device limit: {imax_dev * 1000:.2f} mA)")
        return "\n".join(lines)

    def _on_timeout(self) -> None:
        if self._armed_ch is not None:
            self.result_label.setText("capture timed out")
            self._disarm()

    def _disarm(self) -> None:
        self._armed_ch = None
        self.capture_btn.setEnabled(True)
        self._timeout.stop()
