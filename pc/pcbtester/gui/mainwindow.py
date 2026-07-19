"""Main window: connection bar, 11-channel dashboard, fault banner, E-stop."""

from __future__ import annotations

import time
from typing import Optional

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QCheckBox, QComboBox, QDialog, QDoubleSpinBox, QFrame, QGridLayout,
    QHBoxLayout, QLabel, QMainWindow, QMessageBox, QPushButton, QSpinBox,
    QTabWidget, QVBoxLayout, QWidget,
)

from ..client import CommandError, PCBTesterClient, ProtocolTimeout
from .bridge import ClientBridge
from .calibration import CalibrationTab
from .cards import HpCard, HvCard, LimitsDialog, LvlpCard
from .operate import OperateTab
from .scope import ScopeTab
from .settling import SettlingTab
from .testbench import TestbenchTab
from .trends import TrendsTab

N_LVLP, N_HP, N_HV = 8, 2, 1
FIRST_HP_CH = N_LVLP + 1
HV_CH = N_LVLP + N_HP + 1


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("PCB Tester")
        self.client: Optional[PCBTesterClient] = None
        self._mock = None
        self._last_telem: float = 0.0
        self.telem_count = 0  # used by --selftest

        self.bridge = ClientBridge(self)
        self.bridge.telemetry.connect(self._on_telemetry)
        self.bridge.fault.connect(self._on_fault)
        self.bridge.estop.connect(self._on_estop)
        self.bridge.log.connect(self._on_log)

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(10, 8, 10, 8)
        root.addLayout(self._build_connection_bar())
        root.addLayout(self._build_global_controls())
        root.addWidget(self._build_banner())

        dashboard = QWidget()
        dash_layout = QVBoxLayout(dashboard)
        dash_layout.setContentsMargins(0, 6, 0, 0)
        dash_layout.addLayout(self._build_grid())
        dash_layout.addStretch(1)

        self.trends_tab = TrendsTab()
        self.scope_tab = ScopeTab(request_capture=self._request_capture)
        self.bridge.capture.connect(self.scope_tab.on_capture)
        self.settling_tab = SettlingTab(request_settle=self._request_settle)
        self.bridge.capture.connect(self.settling_tab.on_capture)
        self.cal_tab = CalibrationTab(get_client=lambda: self.client)
        self.operate_tab = OperateTab(
            get_client=lambda: self.client,
            report_error=lambda m: self.statusBar().showMessage(m, 6000))
        self.operate_tab.names_changed.connect(self._on_signal_names)
        self.tb_tab = TestbenchTab(
            get_client=lambda: self.client,
            report_error=lambda m: self.statusBar().showMessage(m, 6000))
        self.bridge.tb_progress.connect(self.tb_tab.on_tb_progress)
        self.bridge.tb_result.connect(self.tb_tab.on_tb_result)
        self.bridge.tb_done.connect(self.tb_tab.on_tb_done)

        self.tabs = QTabWidget()
        self.tabs.addTab(dashboard, "Dashboard")
        self.tabs.addTab(self.operate_tab, "Operate")
        self.tabs.addTab(self.tb_tab, "Testbench")
        self.tabs.addTab(self.trends_tab, "Trends")
        self.tabs.addTab(self.scope_tab, "Scope")
        self.tabs.addTab(self.settling_tab, "Settling")
        self.tabs.addTab(self.cal_tab, "Calibration")
        root.addWidget(self.tabs, 1)

        self.conn_status = QLabel("disconnected")
        self.statusBar().addPermanentWidget(self.conn_status)

        # Spacebar = E-stop, from anywhere in the window
        QShortcut(QKeySequence(Qt.Key_Space), self, activated=self._estop)

        self._stale_timer = QTimer(self, interval=1000, timeout=self._check_stale)
        self._stale_timer.start()

        self._set_connected_ui(False)

    # ------------------------------------------------------------------ #
    # UI construction
    # ------------------------------------------------------------------ #

    def _build_connection_bar(self) -> QHBoxLayout:
        bar = QHBoxLayout()

        self.port_combo = QComboBox(minimumWidth=220)
        refresh_btn = QPushButton("⟳", maximumWidth=32, toolTip="Rescan serial ports")
        refresh_btn.clicked.connect(self._refresh_ports)
        self.mock_check = QCheckBox("Mock device")
        self.mock_check.toggled.connect(
            lambda on: self.port_combo.setEnabled(not on))
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self._toggle_connection)
        self.device_label = QLabel("")

        self.rate_spin = QSpinBox(minimum=0, maximum=50, value=10, suffix=" Hz",
                                  toolTip="Telemetry rate (0 = off)")
        self.rate_spin.valueChanged.connect(self._rate_changed)

        self.estop_btn = QPushButton("E-STOP")
        self.estop_btn.setMinimumHeight(40)
        self.estop_btn.setStyleSheet(
            "QPushButton { background-color:#c62828; color:white;"
            " font-weight:bold; font-size:15px; border-radius:6px;"
            " padding:4px 22px; }"
            "QPushButton:disabled { background-color:#8d6e63; }")
        self.estop_btn.setToolTip("Emergency stop (Spacebar)")
        self.estop_btn.clicked.connect(self._estop)

        bar.addWidget(self.port_combo)
        bar.addWidget(refresh_btn)
        bar.addWidget(self.mock_check)
        bar.addWidget(self.connect_btn)
        bar.addWidget(self.device_label, 1)
        bar.addWidget(QLabel("Telemetry:"))
        bar.addWidget(self.rate_spin)
        bar.addSpacing(16)
        bar.addWidget(self.estop_btn)
        self._refresh_ports()
        return bar

    def _build_global_controls(self) -> QHBoxLayout:
        """Tester-wide settings: one limit applied to every LVLP channel, and
        the shared over-current debounce window."""
        bar = QHBoxLayout()

        # Global limits, broadcast to LVLP CH1-8 (HP/HV keep their own limits).
        self.glim_vmax = QDoubleSpinBox(minimum=0.0, maximum=15.0, value=12.0,
                                        decimals=2, singleStep=0.5, suffix=" V")
        self.glim_imax = QDoubleSpinBox(minimum=0.0, maximum=0.5, value=0.05,
                                        decimals=3, singleStep=0.01, suffix=" A")
        self.glim_btn = QPushButton("Apply to all LVLP")
        self.glim_btn.setToolTip("Send these Vmax/Imax limits to CH1–CH8")
        self.glim_btn.clicked.connect(self._apply_global_limits)

        # Over-current debounce window (LVLPChannel::overCurrentDebounceMs).
        self.debounce_spin = QSpinBox(minimum=0, maximum=5000, value=2,
                                      singleStep=1, suffix=" ms")
        self.debounce_spin.setToolTip(
            "Over-current must persist this long before a channel trips "
            "(all LVLP channels). The Settling tab suggests a value.")
        self.debounce_btn = QPushButton("Apply")
        self.debounce_btn.clicked.connect(self._apply_debounce)

        bar.addWidget(QLabel("Global limits:"))
        bar.addWidget(self.glim_vmax)
        bar.addWidget(self.glim_imax)
        bar.addWidget(self.glim_btn)
        bar.addSpacing(20)
        bar.addWidget(QLabel("OC debounce:"))
        bar.addWidget(self.debounce_spin)
        bar.addWidget(self.debounce_btn)
        bar.addStretch(1)

        self._global_controls = [self.glim_vmax, self.glim_imax, self.glim_btn,
                                 self.debounce_spin, self.debounce_btn]
        return bar

    def _build_banner(self) -> QFrame:
        self.banner = QFrame()
        self.banner.setVisible(False)
        self.banner.setStyleSheet(
            "QFrame { background-color:#c62828; border-radius:6px; }"
            "QLabel { color:white; font-weight:bold; }")
        lay = QHBoxLayout(self.banner)
        lay.setContentsMargins(12, 6, 8, 6)
        self.banner_label = QLabel("")
        self.banner_action = QPushButton("Dismiss")
        self.banner_action.clicked.connect(self._banner_action_clicked)
        lay.addWidget(self.banner_label, 1)
        lay.addWidget(self.banner_action)
        self._banner_is_estop = False
        return self.banner

    def _build_grid(self) -> QGridLayout:
        grid = QGridLayout()
        grid.setSpacing(8)

        self.lvlp_cards = []
        for i in range(N_LVLP):
            card = LvlpCard(i + 1, has_pwm=(i < 4), apply_fn=self._apply_lvlp,
                            reset_fn=self._reset_channel,
                            limits_fn=self._edit_limits)
            self.lvlp_cards.append(card)
            grid.addWidget(card, i // 4, i % 4)

        self.hp_cards = []
        for i in range(N_HP):
            card = HpCard(f"CH{FIRST_HP_CH + i} · HP{i + 1}", FIRST_HP_CH + i,
                          connect_fn=self._set_relay,
                          reset_fn=self._reset_channel,
                          limits_fn=self._edit_limits)
            self.hp_cards.append(card)
            grid.addWidget(card, 2, i)

        self.hv_card = HvCard(f"CH{HV_CH} · HV", HV_CH,
                              connect_fn=self._set_relay,
                              reset_fn=self._reset_channel,
                              limits_fn=self._edit_limits)
        grid.addWidget(self.hv_card, 2, 2)

        self.grid_widget_rows = grid
        return grid

    # ------------------------------------------------------------------ #
    # Connection management
    # ------------------------------------------------------------------ #

    def _refresh_ports(self) -> None:
        from ..transport import discover_ports

        self.port_combo.clear()
        try:
            ports = discover_ports()
        except Exception:
            ports = []
        self.port_combo.addItems(ports)
        if not ports:
            self.port_combo.addItem("(no serial ports found)")

    def _toggle_connection(self) -> None:
        if self.client is not None:
            self._disconnect()
        else:
            self._connect(mock=self.mock_check.isChecked())

    def _connect(self, mock: bool = False, port: Optional[str] = None) -> None:
        try:
            if mock:
                from ..mock import MockTester

                self._mock = MockTester()
                transport = self._mock.transport()
            else:
                from ..transport import SerialTransport

                port = port or self.port_combo.currentText()
                if not port or port.startswith("("):
                    raise RuntimeError("no serial port selected")
                transport = SerialTransport(port)

            self.client = PCBTesterClient(transport).start()
            self.bridge.attach(self.client)
            info = self.client.hello(timeout=3.0)
            self.client.set_telemetry_rate(self.rate_spin.value())
        except Exception as e:
            self._teardown_client()
            QMessageBox.critical(self, "Connection failed", str(e))
            return

        self.trends_tab.clear()
        tag = " (MOCK)" if info.get("mock") else ""
        self.device_label.setText(
            f"{info.get('name')}{tag} · fw {info.get('fw')} · proto {info.get('proto')}")
        self.conn_status.setText("connected")
        self.debounce_spin.setValue(int(info.get("oc_debounce_ms", 2)))
        self._set_connected_ui(True)
        if info.get("estop"):
            self._show_estop_banner("previous session")

    def _disconnect(self) -> None:
        self._teardown_client()
        self.device_label.setText("")
        self.conn_status.setText("disconnected")
        self._set_connected_ui(False)
        self._hide_banner()

    def _teardown_client(self) -> None:
        if self.client is not None:
            try:
                self.bridge.detach(self.client)
                self.client.close()
            except Exception:
                pass
        if self._mock is not None:
            self._mock.stop()
        self.client = None
        self._mock = None

    def _set_connected_ui(self, connected: bool) -> None:
        self.connect_btn.setText("Disconnect" if connected else "Connect")
        for card in self.lvlp_cards + self.hp_cards + [self.hv_card]:
            card.setEnabled(connected)
        self.estop_btn.setEnabled(connected)
        for w in self._global_controls:
            w.setEnabled(connected)
        self.port_combo.setEnabled(not connected and not self.mock_check.isChecked())
        self.mock_check.setEnabled(not connected)

    def closeEvent(self, event) -> None:  # noqa: N802 (Qt override)
        self._teardown_client()
        event.accept()

    # ------------------------------------------------------------------ #
    # Commands from cards / buttons
    # ------------------------------------------------------------------ #

    def _cmd(self, fn, *args, **kwargs):
        if self.client is None:
            return None
        try:
            return fn(*args, **kwargs)
        except (CommandError, ProtocolTimeout) as e:
            self.statusBar().showMessage(str(e), 6000)
            return None

    def _apply_lvlp(self, ch: int, mode: str, params: dict) -> None:
        self._cmd(self.client.set_channel, ch, mode, **params)

    def _set_relay(self, ch: int, connect: bool) -> None:
        if connect:
            self._cmd(self.client.connect_channel, ch)
        else:
            self._cmd(self.client.disconnect_channel, ch)

    def _reset_channel(self, ch: int) -> None:
        self._cmd(self.client.reset_channel, ch)

    def _edit_limits(self, ch: int) -> None:
        dlg = LimitsDialog(self, ch, has_imax=(ch != HV_CH))
        if dlg.exec() == QDialog.Accepted:
            imax = dlg.imax.value() if dlg.imax else None
            self._cmd(self.client.set_limits, ch, dlg.vmax.value(), imax)

    def _apply_global_limits(self) -> None:
        if self.client is None:
            self.statusBar().showMessage("not connected", 4000)
            return
        vmax, imax = self.glim_vmax.value(), self.glim_imax.value()
        for ch in range(1, N_LVLP + 1):
            if self._cmd(self.client.set_limits, ch, vmax, imax) is None:
                return  # error already surfaced
        self.statusBar().showMessage(
            f"limits applied to CH1–CH{N_LVLP}: {vmax:.2f} V / {imax:.3f} A", 4000)

    def _apply_debounce(self) -> None:
        if self.client is None:
            self.statusBar().showMessage("not connected", 4000)
            return
        ms = self.debounce_spin.value()
        if self._cmd(self.client.set_oc_debounce, ms) is not None:
            self.statusBar().showMessage(f"over-current debounce set to {ms} ms", 4000)

    def _rate_changed(self, hz: int) -> None:
        self._cmd(self.client.set_telemetry_rate, hz) if self.client else None

    def _estop(self) -> None:
        self._cmd(self.client.estop) if self.client else None

    def _request_capture(self, ch: int, n: int, dt_ms: int) -> bool:
        if self.client is None:
            self.statusBar().showMessage("not connected", 4000)
            return False
        return self._cmd(self.client.command, "ch.capture",
                         ch=ch, n=n, dt_ms=dt_ms) is not None

    def _request_settle(self, ch: int, from_v: float, to_v: float, n: int,
                        dt_us: int, settle_ms: int) -> bool:
        if self.client is None:
            self.statusBar().showMessage("not connected", 4000)
            return False
        return self._cmd(self.client.request_settle, ch, from_v, to_v,
                         n=n, dt_us=dt_us, settle_ms=settle_ms) is not None

    # ------------------------------------------------------------------ #
    # Events from the device (already on the Qt thread via the bridge)
    # ------------------------------------------------------------------ #

    def _on_telemetry(self, msg: dict) -> None:
        self._last_telem = time.monotonic()
        self.telem_count += 1
        self.trends_tab.add_telemetry(msg)
        self.operate_tab.update_telemetry(msg)
        for d in msg.get("lvlp", []):
            idx = d.get("ch", 0) - 1
            if 0 <= idx < N_LVLP:
                self.lvlp_cards[idx].update_from(d)
        for d in msg.get("hp", []):
            idx = d.get("ch", 0) - FIRST_HP_CH
            if 0 <= idx < N_HP:
                self.hp_cards[idx].update_from(d)
        for d in msg.get("hv", []):
            if d.get("ch") == HV_CH:
                self.hv_card.update_from(d)
        if not msg.get("estop") and self._banner_is_estop:
            self._hide_banner()
        self.conn_status.setText("connected · telemetry live")

    def _on_fault(self, msg: dict) -> None:
        ch = msg.get("ch")
        code = msg.get("code", "FAULT")
        detail = "  ".join(f"{k}={msg[k]}" for k in ("v", "i", "vin", "vout")
                           if k in msg)
        self._show_banner(f"CH{ch} {code}   {detail}", action="Dismiss",
                          is_estop=False)

    def _on_estop(self, msg: dict) -> None:
        self._show_estop_banner(msg.get("src", "?"))

    def _on_log(self, msg: dict) -> None:
        self.statusBar().showMessage(
            f"[{msg.get('lvl', 'info')}] {msg.get('msg', '')}", 5000)

    def _on_signal_names(self, names: dict) -> None:
        """DUT profile (re)named the channels — propagate across the app."""
        for i, card in enumerate(self.lvlp_cards):
            card.set_signal_name(names.get(i + 1))
        for i, card in enumerate(self.hp_cards):
            card.set_signal_name(names.get(FIRST_HP_CH + i))
        self.hv_card.set_signal_name(names.get(HV_CH))
        self.trends_tab.set_signal_names(names)

    # ------------------------------------------------------------------ #
    # Banner
    # ------------------------------------------------------------------ #

    def _show_banner(self, text: str, action: str, is_estop: bool) -> None:
        self.banner_label.setText(text)
        self.banner_action.setText(action)
        self._banner_is_estop = is_estop
        self.banner.setVisible(True)

    def _show_estop_banner(self, src: str) -> None:
        self._show_banner(f"E-STOP active (source: {src}) — all outputs opened",
                          action="Clear E-stop", is_estop=True)

    def _hide_banner(self) -> None:
        self.banner.setVisible(False)
        self._banner_is_estop = False

    def _banner_action_clicked(self) -> None:
        if self._banner_is_estop:
            self._cmd(self.client.clear_estop) if self.client else None
        self._hide_banner()

    # ------------------------------------------------------------------ #
    # Housekeeping
    # ------------------------------------------------------------------ #

    def _check_stale(self) -> None:
        if self.client is None or self.rate_spin.value() == 0:
            return
        if self._last_telem and time.monotonic() - self._last_telem > 3.0:
            self.conn_status.setText("connected · NO TELEMETRY")
