"""Channel card widgets: one card per channel on the dashboard grid.

Cards separate *actual* state (live labels fed by telemetry) from *requested*
state (the mode/value editors), so incoming telemetry never fights the user's
edits — controls only act when Apply / Connect is clicked.
"""

from __future__ import annotations

from typing import Callable, Dict, Optional

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QComboBox, QDialog, QDialogButtonBox, QDoubleSpinBox, QFormLayout, QFrame,
    QHBoxLayout, QLabel, QPushButton, QSpinBox, QStackedWidget, QVBoxLayout,
    QWidget,
)

# st code → (label, dot color); index 0 handled from conn state
_FAULT_INFO = {
    1: ("OVERCURRENT", "#e53935"),
    2: ("OVERVOLTAGE", "#1e88e5"),
    3: ("FAULT", "#fb8c00"),
}
_COLOR_ON = "#43a047"
_COLOR_OFF = "#9e9e9e"

CommandFn = Callable[..., None]


def _dot(color: str) -> str:
    return f"<span style='color:{color};'>●</span>"


class _CardBase(QFrame):
    def __init__(self, title: str) -> None:
        super().__init__()
        self.setFrameShape(QFrame.StyledPanel)
        self.setProperty("card", True)
        self._faulted = False

        self.status_label = QLabel(_dot(_COLOR_OFF))
        self.status_label.setTextFormat(Qt.RichText)
        self.title_label = QLabel(f"<b>{title}</b>")
        self.state_label = QLabel("--")
        self.state_label.setAlignment(Qt.AlignRight | Qt.AlignVCenter)

        header = QHBoxLayout()
        header.addWidget(self.status_label)
        header.addWidget(self.title_label)
        header.addStretch(1)
        header.addWidget(self.state_label)

        self.live_label = QLabel("--")
        self.live_label.setStyleSheet("font-size: 15px; font-weight: 600;")

        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(10, 8, 10, 8)
        self._layout.setSpacing(6)
        self._layout.addLayout(header)
        self._layout.addWidget(self.live_label)

    def _set_status(self, st: int, conn: bool, state_text: str) -> None:
        self._faulted = st != 0
        if st in _FAULT_INFO:
            text, color = _FAULT_INFO[st]
            self.status_label.setText(_dot(color))
            self.state_label.setText(f"<b style='color:{color};'>{text}</b>")
            self.setStyleSheet(f"QFrame[card=true] {{ border: 1px solid {color}; border-radius: 6px; }}")
        else:
            self.status_label.setText(_dot(_COLOR_ON if conn else _COLOR_OFF))
            self.state_label.setText(state_text)
            self.setStyleSheet("QFrame[card=true] { border: 1px solid palette(mid); border-radius: 6px; }")


class LvlpCard(_CardBase):
    """LVLP channel (1-8): mode + setpoint editors, live V/I readout."""

    MODES = ("HZ", "VS", "CS", "RL", "PWM")

    def __init__(self, ch: int, has_pwm: bool,
                 apply_fn: Callable[[int, str, dict], None],
                 reset_fn: Callable[[int], None],
                 limits_fn: Callable[[int], None]) -> None:
        super().__init__(f"CH{ch}")
        self.ch = ch
        self._apply_fn = apply_fn

        self.mode_combo = QComboBox()
        for m in self.MODES:
            if m == "PWM" and not has_pwm:
                continue
            self.mode_combo.addItem(m)

        # One editor page per mode
        self.params = QStackedWidget()
        self.params.addWidget(QWidget())  # HZ: nothing to edit

        self.v_spin = QDoubleSpinBox(suffix=" V", decimals=3, maximum=12.0,
                                     singleStep=0.1)
        self.params.addWidget(self._wrap(self.v_spin))

        self.i_spin = QDoubleSpinBox(suffix=" A", decimals=4, maximum=0.5,
                                     singleStep=0.01)
        self.params.addWidget(self._wrap(self.i_spin))     # CS
        self.i_spin_rl = QDoubleSpinBox(suffix=" A", decimals=4, maximum=0.5,
                                        singleStep=0.01)
        self.params.addWidget(self._wrap(self.i_spin_rl))  # RL

        pwm_row = QWidget()
        pl = QHBoxLayout(pwm_row)
        pl.setContentsMargins(0, 0, 0, 0)
        self.duty_spin = QSpinBox(maximum=255, value=128, toolTip="Duty 0-255")
        self.freq_spin = QSpinBox(maximum=150000, minimum=1, value=1000,
                                  suffix=" Hz", toolTip="Frequency")
        pl.addWidget(self.duty_spin)
        pl.addWidget(self.freq_spin)
        self.params.addWidget(pwm_row)

        self.mode_combo.currentTextChanged.connect(self._on_mode_changed)

        apply_btn = QPushButton("Apply")
        apply_btn.clicked.connect(self._apply)

        controls = QHBoxLayout()
        controls.addWidget(self.mode_combo)
        controls.addWidget(self.params, 1)
        controls.addWidget(apply_btn)
        self._layout.addLayout(controls)

        self.reset_btn = QPushButton("Reset fault")
        self.reset_btn.setEnabled(False)
        self.reset_btn.clicked.connect(lambda: reset_fn(self.ch))
        limits_btn = QPushButton("Limits…")
        limits_btn.clicked.connect(lambda: limits_fn(self.ch))
        footer = QHBoxLayout()
        footer.addWidget(self.reset_btn)
        footer.addStretch(1)
        footer.addWidget(limits_btn)
        self._layout.addLayout(footer)

    @staticmethod
    def _wrap(w: QWidget) -> QWidget:
        box = QWidget()
        lay = QHBoxLayout(box)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.addWidget(w)
        return box

    def _on_mode_changed(self, mode: str) -> None:
        self.params.setCurrentIndex(self.MODES.index(mode))

    def _apply(self) -> None:
        mode = self.mode_combo.currentText()
        params: Dict[str, object] = {}
        if mode == "VS":
            params["v"] = self.v_spin.value()
        elif mode == "CS":
            params["i"] = self.i_spin.value()
        elif mode == "RL":
            params["i"] = self.i_spin_rl.value()
        elif mode == "PWM":
            params["duty"] = self.duty_spin.value()
            params["freq"] = self.freq_spin.value()
        self._apply_fn(self.ch, mode, params)

    def update_from(self, d: dict) -> None:
        st, conn = d.get("st", 0), d.get("conn", False)
        mode = d.get("mode", "?")
        extra = ""
        if mode == "PWM":
            extra = f"  {d.get('duty', 0)}/{d.get('freq', 0)}Hz"
        self._set_status(st, conn, f"{mode}{extra}")
        v, i = d.get("v", 0.0), d.get("i", 0.0)
        self.live_label.setText(f"{v:7.3f} V   {i * 1000:8.1f} mA")
        self.reset_btn.setEnabled(st != 0)


class _RelayCard(_CardBase):
    """Shared behavior for HP/HV: relay toggle + reset."""

    def __init__(self, title: str, ch: int,
                 connect_fn: Callable[[int, bool], None],
                 reset_fn: Callable[[int], None],
                 limits_fn: Callable[[int], None]) -> None:
        super().__init__(title)
        self.ch = ch
        self._connected = False
        self._connect_fn = connect_fn

        self.toggle_btn = QPushButton("Connect")
        self.toggle_btn.clicked.connect(self._toggle)
        self.reset_btn = QPushButton("Reset fault")
        self.reset_btn.setEnabled(False)
        self.reset_btn.clicked.connect(lambda: reset_fn(self.ch))
        limits_btn = QPushButton("Limits…")
        limits_btn.clicked.connect(lambda: limits_fn(self.ch))

        footer = QHBoxLayout()
        footer.addWidget(self.toggle_btn)
        footer.addWidget(self.reset_btn)
        footer.addStretch(1)
        footer.addWidget(limits_btn)
        self._layout.addLayout(footer)

    def _toggle(self) -> None:
        self._connect_fn(self.ch, not self._connected)

    def _sync_toggle(self, conn: bool) -> None:
        self._connected = conn
        self.toggle_btn.setText("Disconnect" if conn else "Connect")


class HpCard(_RelayCard):
    def update_from(self, d: dict) -> None:
        st, conn = d.get("st", 0), d.get("conn", False)
        self._set_status(st, conn, "ON" if conn else "off")
        self.live_label.setText(
            f"in {d.get('vin', 0.0):6.2f} V   out {d.get('vout', 0.0):6.2f} V   "
            f"{d.get('i', 0.0):6.3f} A")
        self.reset_btn.setEnabled(st != 0)
        self._sync_toggle(conn)


class HvCard(_RelayCard):
    def update_from(self, d: dict) -> None:
        st, conn = d.get("st", 0), d.get("conn", False)
        self._set_status(st, conn, "ON" if conn else "off")
        self.live_label.setText(f"{d.get('v', 0.0):7.2f} V")
        self.reset_btn.setEnabled(st != 0)
        self._sync_toggle(conn)


class LimitsDialog(QDialog):
    """Set vmax (and imax for LVLP/HP) protection limits of one channel."""

    def __init__(self, parent: QWidget, ch: int, has_imax: bool) -> None:
        super().__init__(parent)
        self.setWindowTitle(f"CH{ch} protection limits")
        form = QFormLayout(self)
        self.vmax = QDoubleSpinBox(suffix=" V", decimals=2, maximum=500.0,
                                   value=12.0)
        form.addRow("Max voltage", self.vmax)
        self.imax: Optional[QDoubleSpinBox] = None
        if has_imax:
            self.imax = QDoubleSpinBox(suffix=" A", decimals=3, maximum=10.0,
                                       value=0.5)
            form.addRow("Max current", self.imax)
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)
