"""Trends tab: rolling live plots fed by telemetry.

Two separate plots (never >8 series per axes, one y-scale each):
  - LVLP CH1-8, switchable between voltage and current
  - HP1/HP2 output voltage + HV voltage
Channel visibility toggles + legends carry identity alongside color.
"""

from __future__ import annotations

import time
from collections import deque
from typing import Dict, List

import pyqtgraph as pg
from PySide6.QtWidgets import (
    QCheckBox, QComboBox, QHBoxLayout, QLabel, QPushButton, QSpinBox,
    QVBoxLayout, QWidget,
)

from .palette import CATEGORICAL, GRID_ALPHA, SURFACE, TEXT_SECONDARY, channel_color

_MAX_POINTS = 3600  # 60 s at the maximum 50 Hz telemetry rate


class _Series:
    def __init__(self, plot: pg.PlotItem, name: str, color: str) -> None:
        self.t: deque = deque(maxlen=_MAX_POINTS)
        self.y: deque = deque(maxlen=_MAX_POINTS)
        self.curve = plot.plot(name=name, pen=pg.mkPen(color, width=2))

    def append(self, t: float, y: float) -> None:
        self.t.append(t)
        self.y.append(y)

    def refresh(self, now: float, window_s: float, visible: bool) -> None:
        self.curve.setVisible(visible)
        if not visible or not self.t:
            return
        xs, ys = [], []
        for t, y in zip(self.t, self.y):
            age = t - now
            if age >= -window_s:
                xs.append(age)
                ys.append(y)
        self.curve.setData(xs, ys)


def _make_plot(title: str, y_label: str) -> pg.PlotWidget:
    w = pg.PlotWidget(background=SURFACE, title=title)
    w.setLabel("left", y_label)
    w.setLabel("bottom", "time", units="s")
    w.showGrid(x=True, y=True, alpha=GRID_ALPHA)
    w.addLegend(offset=(10, 10), labelTextColor=TEXT_SECONDARY)
    w.getPlotItem().getViewBox().setDefaultPadding(0.05)
    return w


class TrendsTab(QWidget):
    def __init__(self) -> None:
        super().__init__()
        self.sample_count = 0  # used by --selftest
        self._paused = False

        # ---- controls row -------------------------------------------------
        controls = QHBoxLayout()
        self.signal_combo = QComboBox()
        self.signal_combo.addItems(["Voltage (V)", "Current (mA)"])
        self.signal_combo.currentIndexChanged.connect(self._refresh)
        self.window_spin = QSpinBox(minimum=10, maximum=300, value=60,
                                    suffix=" s", toolTip="Rolling window")
        self.pause_btn = QPushButton("Pause")
        self.pause_btn.setCheckable(True)
        self.pause_btn.toggled.connect(self._set_paused)
        controls.addWidget(QLabel("LVLP signal:"))
        controls.addWidget(self.signal_combo)
        controls.addWidget(QLabel("Window:"))
        controls.addWidget(self.window_spin)
        controls.addWidget(self.pause_btn)
        controls.addStretch(1)

        # ---- channel visibility checkboxes --------------------------------
        self.lvlp_checks: List[QCheckBox] = []
        for i in range(8):
            cb = QCheckBox(f"CH{i + 1}")
            cb.setChecked(True)
            cb.setStyleSheet(f"QCheckBox {{ color: {channel_color(i + 1)}; }}")
            cb.toggled.connect(self._refresh)
            self.lvlp_checks.append(cb)
            controls.addWidget(cb)

        # ---- plots ---------------------------------------------------------
        self.lvlp_plot = _make_plot("LVLP channels", "V")
        self.aux_plot = _make_plot("HP / HV", "V")

        aux_row = QHBoxLayout()
        self.aux_checks: List[QCheckBox] = []
        for i, name in enumerate(("HP1 out", "HP2 out", "HV")):
            cb = QCheckBox(name)
            cb.setChecked(True)
            cb.setStyleSheet(f"QCheckBox {{ color: {CATEGORICAL[i]}; }}")
            cb.toggled.connect(self._refresh)
            self.aux_checks.append(cb)
            aux_row.addWidget(cb)
        aux_row.addStretch(1)

        layout = QVBoxLayout(self)
        layout.addLayout(controls)
        layout.addWidget(self.lvlp_plot, 3)
        layout.addLayout(aux_row)
        layout.addWidget(self.aux_plot, 2)

        # ---- series --------------------------------------------------------
        # LVLP: one V-series and one I-series per channel; the combo picks
        # which set is displayed.
        lp = self.lvlp_plot.getPlotItem()
        self.lvlp_v = [_Series(lp, f"CH{i + 1}", channel_color(i + 1))
                       for i in range(8)]
        self.lvlp_i = [_Series(lp, f"CH{i + 1}", channel_color(i + 1))
                       for i in range(8)]
        ap = self.aux_plot.getPlotItem()
        self.aux_series = [_Series(ap, name, CATEGORICAL[i])
                           for i, name in enumerate(("HP1 out", "HP2 out", "HV"))]

    # ------------------------------------------------------------------ #

    def clear(self) -> None:
        for s in self.lvlp_v + self.lvlp_i + self.aux_series:
            s.t.clear()
            s.y.clear()
            s.curve.setData([], [])
        self.sample_count = 0

    def add_telemetry(self, msg: dict) -> None:
        now = time.monotonic()
        for d in msg.get("lvlp", []):
            idx = d.get("ch", 0) - 1
            if 0 <= idx < 8:
                self.lvlp_v[idx].append(now, d.get("v", 0.0))
                self.lvlp_i[idx].append(now, d.get("i", 0.0) * 1000.0)
        hp = msg.get("hp", [])
        for i in range(min(2, len(hp))):
            self.aux_series[i].append(now, hp[i].get("vout", 0.0))
        hv = msg.get("hv", [])
        if hv:
            self.aux_series[2].append(now, hv[0].get("v", 0.0))
        self.sample_count += 1
        if not self._paused:
            self._refresh()

    # ------------------------------------------------------------------ #

    def _set_paused(self, paused: bool) -> None:
        self._paused = paused
        self.pause_btn.setText("Resume" if paused else "Pause")
        if not paused:
            self._refresh()

    def _refresh(self) -> None:
        now = time.monotonic()
        window = float(self.window_spin.value())
        show_v = self.signal_combo.currentIndex() == 0
        self.lvlp_plot.setLabel("left", "V" if show_v else "mA")
        for i in range(8):
            on = self.lvlp_checks[i].isChecked()
            self.lvlp_v[i].refresh(now, window, visible=on and show_v)
            self.lvlp_i[i].refresh(now, window, visible=on and not show_v)
        for i, s in enumerate(self.aux_series):
            s.refresh(now, window, visible=self.aux_checks[i].isChecked())
