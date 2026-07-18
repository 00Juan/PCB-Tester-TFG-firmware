"""Operate tab: high-level DUT control between Dashboard and Testbench.

Loads a DUT profile (see gui/profile.py) and presents:
  - macro buttons (ordered raw-command sequences, run non-blocking via a
    QTimer stepper so "wait" actions never freeze the UI; keys 1-9 fire
    the first nine macros while the tab is visible);
  - indicator lamps (named states derived from telemetry conditions);
  - a compact live overview table of every channel (name, mode, V, I, status).

"Edit mode" flips the tab from operator view to configuration view:
clicking a macro/indicator opens its editor, signal names become editable
in the overview table, and Add / Capture-state buttons appear.
"""

from __future__ import annotations

from pathlib import Path
from typing import Callable, Dict, List, Optional

from PySide6.QtCore import Qt, QTimer, Signal
from PySide6.QtGui import QBrush, QColor, QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QAbstractItemView, QComboBox, QDialog, QDialogButtonBox, QDoubleSpinBox,
    QFileDialog, QFormLayout, QFrame, QGridLayout, QGroupBox, QHBoxLayout,
    QHeaderView, QLabel, QLineEdit, QListWidget, QMessageBox, QPlainTextEdit,
    QPushButton, QSpinBox, QStackedWidget, QTableWidget, QTableWidgetItem,
    QVBoxLayout, QWidget,
)

from . import profile as prof
from .profile import (
    FIELDS, N_CHANNELS, OPS, action_summary, capture_actions,
    evaluate_indicator, signal_label, telemetry_snapshot,
)

PROFILE_DIR = Path(__file__).resolve().parents[2] / "profiles"

_COLORS = [("Gray", "#546e7a"), ("Green", "#43a047"), ("Red", "#e53935"),
           ("Blue", "#1e88e5"), ("Orange", "#fb8c00"), ("Purple", "#8e24aa"),
           ("Teal", "#00897b")]
_FAULT_TEXT = {1: "OVERCURRENT", 2: "OVERVOLTAGE", 3: "FAULT"}
_MACROS_PER_ROW = 3


def _color_combo(current: str = "#546e7a") -> QComboBox:
    combo = QComboBox()
    for name, hexcolor in _COLORS:
        combo.addItem(name, userData=hexcolor)
        idx = combo.count() - 1
        combo.setItemData(idx, QBrush(QColor(hexcolor)), Qt.ForegroundRole)
    idx = next((i for i, (_, h) in enumerate(_COLORS) if h == current), 0)
    combo.setCurrentIndex(idx)
    return combo


def _channel_combo(channels, current: Optional[int] = None) -> QComboBox:
    combo = QComboBox()
    for ch in channels:
        combo.addItem(signal_label(ch), userData=ch)
    if current in channels:
        combo.setCurrentIndex(list(channels).index(current))
    return combo


def _btn(text: str, fn, tip: str = "") -> QPushButton:
    b = QPushButton(text)
    b.clicked.connect(fn)
    if tip:
        b.setToolTip(tip)
    return b


# --------------------------------------------------------------------------- #
# Macro editing
# --------------------------------------------------------------------------- #

class ActionEditorDialog(QDialog):
    """Edit one macro action: typed forms for the common commands + raw JSON."""

    TYPES = ["Set LVLP channel", "Relay (HP/HV)", "Wait", "Set limits",
             "Raw JSON"]

    def __init__(self, parent: QWidget, action: Optional[dict] = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Edit action")
        self.setMinimumWidth(420)
        self.result_action: Optional[dict] = None

        self.type_combo = QComboBox()
        self.type_combo.addItems(self.TYPES)
        self.pages = QStackedWidget()

        # -- Set LVLP channel
        set_page = QWidget()
        form = QFormLayout(set_page)
        self.set_ch = _channel_combo(range(1, 9))
        self.set_mode = QComboBox()
        self.set_mode.addItems(["HZ", "VS", "CS", "RL", "PWM"])
        self.set_v = QDoubleSpinBox(suffix=" V", decimals=3, maximum=12.0,
                                    singleStep=0.1)
        self.set_i = QDoubleSpinBox(suffix=" A", decimals=4, maximum=0.5,
                                    singleStep=0.01)
        self.set_duty = QSpinBox(maximum=16383, value=128)
        self.set_freq = QSpinBox(minimum=1, maximum=150000, value=1000,
                                 suffix=" Hz")
        self.set_res = QSpinBox(minimum=1, maximum=14, value=8, suffix=" bit")
        form.addRow("Channel", self.set_ch)
        form.addRow("Mode", self.set_mode)
        form.addRow("Voltage (VS/PWM ampl.)", self.set_v)
        form.addRow("Current (CS/RL)", self.set_i)
        form.addRow("PWM duty", self.set_duty)
        form.addRow("PWM freq", self.set_freq)
        form.addRow("PWM res", self.set_res)
        self.set_mode.currentTextChanged.connect(self._sync_set_enables)
        self.pages.addWidget(set_page)

        # -- Relay
        relay_page = QWidget()
        form = QFormLayout(relay_page)
        self.relay_ch = _channel_combo(range(9, N_CHANNELS + 1))
        self.relay_action = QComboBox()
        self.relay_action.addItems(["connect", "disconnect"])
        form.addRow("Channel", self.relay_ch)
        form.addRow("Action", self.relay_action)
        self.pages.addWidget(relay_page)

        # -- Wait
        wait_page = QWidget()
        form = QFormLayout(wait_page)
        self.wait_ms = QSpinBox(minimum=1, maximum=60000, value=300,
                                suffix=" ms")
        form.addRow("Wait", self.wait_ms)
        self.pages.addWidget(wait_page)

        # -- Limits
        lim_page = QWidget()
        form = QFormLayout(lim_page)
        self.lim_ch = _channel_combo(range(1, N_CHANNELS + 1))
        self.lim_vmax = QDoubleSpinBox(suffix=" V", decimals=2, maximum=500.0,
                                       value=12.0)
        self.lim_imax = QDoubleSpinBox(suffix=" A", decimals=3, maximum=10.0,
                                       value=0.5)
        form.addRow("Channel", self.lim_ch)
        form.addRow("Max voltage", self.lim_vmax)
        form.addRow("Max current", self.lim_imax)
        self.pages.addWidget(lim_page)

        # -- Raw JSON
        self.raw_edit = QPlainTextEdit()
        self.raw_edit.setPlaceholderText('{"cmd": "ch.set", "ch": 1, '
                                         '"mode": "VS", "v": 5.0}')
        self.raw_edit.setStyleSheet("font-family: Menlo, monospace;")
        self.pages.addWidget(self.raw_edit)

        self.type_combo.currentIndexChanged.connect(self.pages.setCurrentIndex)

        self.error_label = QLabel("")
        self.error_label.setStyleSheet("color: #e53935;")
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)

        root = QVBoxLayout(self)
        root.addWidget(self.type_combo)
        root.addWidget(self.pages, 1)
        root.addWidget(self.error_label)
        root.addWidget(buttons)

        self._sync_set_enables(self.set_mode.currentText())
        if action:
            self._from_action(action)

    def _sync_set_enables(self, mode: str) -> None:
        self.set_v.setEnabled(mode in ("VS", "PWM"))
        self.set_i.setEnabled(mode in ("CS", "RL"))
        for w in (self.set_duty, self.set_freq, self.set_res):
            w.setEnabled(mode == "PWM")

    def _from_action(self, a: dict) -> None:
        import json

        cmd = a.get("cmd")
        if cmd == "ch.set" and a.get("ch") in range(1, 9):
            self.type_combo.setCurrentIndex(0)
            self.set_ch.setCurrentIndex(int(a.get("ch", 1)) - 1)
            self.set_mode.setCurrentText(a.get("mode", "HZ"))
            if "v" in a:
                self.set_v.setValue(float(a["v"]))
            if "i" in a:
                self.set_i.setValue(float(a["i"]))
            if "duty" in a:
                self.set_duty.setValue(int(a["duty"]))
            if "freq" in a:
                self.set_freq.setValue(int(a["freq"]))
            if "res" in a:
                self.set_res.setValue(int(a["res"]))
        elif cmd in ("ch.connect", "ch.disconnect") and \
                a.get("ch") in range(9, N_CHANNELS + 1):
            self.type_combo.setCurrentIndex(1)
            self.relay_ch.setCurrentIndex(int(a.get("ch", 9)) - 9)
            self.relay_action.setCurrentIndex(0 if cmd == "ch.connect" else 1)
        elif cmd == "wait":
            self.type_combo.setCurrentIndex(2)
            self.wait_ms.setValue(int(a.get("ms", 300)))
        elif cmd == "ch.limits" and a.get("ch") in range(1, N_CHANNELS + 1):
            self.type_combo.setCurrentIndex(3)
            self.lim_ch.setCurrentIndex(int(a.get("ch", 1)) - 1)
            self.lim_vmax.setValue(float(a.get("vmax", 12.0)))
            if a.get("imax") is not None:
                self.lim_imax.setValue(float(a["imax"]))
        else:
            self.type_combo.setCurrentIndex(4)
            self.raw_edit.setPlainText(json.dumps(a, indent=1))

    def _to_action(self) -> dict:
        import json

        page = self.type_combo.currentIndex()
        if page == 0:
            mode = self.set_mode.currentText()
            a = {"cmd": "ch.set", "ch": self.set_ch.currentData(),
                 "mode": mode}
            if mode == "VS":
                a["v"] = self.set_v.value()
            elif mode in ("CS", "RL"):
                a["i"] = self.set_i.value()
            elif mode == "PWM":
                a.update(v=self.set_v.value(), duty=self.set_duty.value(),
                         freq=self.set_freq.value(), res=self.set_res.value())
            return a
        if page == 1:
            cmd = ("ch.connect" if self.relay_action.currentIndex() == 0
                   else "ch.disconnect")
            return {"cmd": cmd, "ch": self.relay_ch.currentData()}
        if page == 2:
            return {"cmd": "wait", "ms": self.wait_ms.value()}
        if page == 3:
            return {"cmd": "ch.limits", "ch": self.lim_ch.currentData(),
                    "vmax": self.lim_vmax.value(),
                    "imax": self.lim_imax.value()}
        a = json.loads(self.raw_edit.toPlainText())
        if not isinstance(a, dict) or "cmd" not in a:
            raise ValueError('action must be a JSON object with a "cmd" key')
        return a

    def _accept(self) -> None:
        try:
            self.result_action = self._to_action()
        except ValueError as e:
            self.error_label.setText(f"invalid action: {e}")
            return
        self.accept()


class MacroEditorDialog(QDialog):
    """Edit one macro: name, button color and its ordered action list."""

    def __init__(self, parent: QWidget, macro: Optional[dict] = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Edit macro")
        self.setMinimumSize(520, 420)
        macro = macro or {}
        self.actions: List[dict] = [dict(a) for a in macro.get("actions", [])]
        self.result_macro: Optional[dict] = None
        self.deleted = False

        self.name_edit = QLineEdit(macro.get("name", ""))
        self.color_combo = _color_combo(macro.get("color", _COLORS[0][1]))
        head = QFormLayout()
        head.addRow("Name", self.name_edit)
        head.addRow("Button color", self.color_combo)

        self.action_list = QListWidget()
        self.action_list.itemDoubleClicked.connect(lambda _: self._edit())

        row = QHBoxLayout()
        for b in (_btn("Add", self._add), _btn("Edit", self._edit),
                  _btn("Del", self._remove),
                  _btn("↑", lambda: self._move(-1)),
                  _btn("↓", lambda: self._move(+1))):
            row.addWidget(b)
        row.addStretch(1)

        delete_btn = _btn("Delete macro", self._delete)
        delete_btn.setStyleSheet("color: #e53935;")
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)
        buttons.addButton(delete_btn, QDialogButtonBox.ActionRole)

        root = QVBoxLayout(self)
        root.addLayout(head)
        root.addWidget(QLabel("Actions (run in order):"))
        root.addWidget(self.action_list, 1)
        root.addLayout(row)
        root.addWidget(buttons)
        self._refresh()

    def _refresh(self) -> None:
        self.action_list.clear()
        for a in self.actions:
            self.action_list.addItem(action_summary(a))

    def _add(self) -> None:
        dlg = ActionEditorDialog(self)
        if dlg.exec() == QDialog.Accepted and dlg.result_action:
            self.actions.append(dlg.result_action)
            self._refresh()

    def _edit(self) -> None:
        i = self.action_list.currentRow()
        if i < 0:
            return
        dlg = ActionEditorDialog(self, action=self.actions[i])
        if dlg.exec() == QDialog.Accepted and dlg.result_action:
            self.actions[i] = dlg.result_action
            self._refresh()
            self.action_list.setCurrentRow(i)

    def _remove(self) -> None:
        i = self.action_list.currentRow()
        if i >= 0:
            del self.actions[i]
            self._refresh()

    def _move(self, delta: int) -> None:
        i = self.action_list.currentRow()
        j = i + delta
        if 0 <= i < len(self.actions) and 0 <= j < len(self.actions):
            self.actions[i], self.actions[j] = self.actions[j], self.actions[i]
            self._refresh()
            self.action_list.setCurrentRow(j)

    def _delete(self) -> None:
        self.deleted = True
        self.accept()

    def _accept(self) -> None:
        self.result_macro = {
            "name": self.name_edit.text().strip() or "unnamed",
            "color": self.color_combo.currentData(),
            "actions": self.actions,
        }
        self.accept()


# --------------------------------------------------------------------------- #
# Indicator editing
# --------------------------------------------------------------------------- #

class StateEditorDialog(QDialog):
    """One indicator state: name, color and its AND-ed condition list."""

    COLS = ("Channel", "Field", "Op", "Value")

    def __init__(self, parent: QWidget, state: Optional[dict] = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Edit indicator state")
        self.setMinimumWidth(480)
        state = state or {}
        self.result_state: Optional[dict] = None

        self.name_edit = QLineEdit(state.get("state", ""))
        self.color_combo = _color_combo(state.get("color", _COLORS[1][1]))
        head = QFormLayout()
        head.addRow("State name", self.name_edit)
        head.addRow("Color", self.color_combo)

        self.table = QTableWidget(0, len(self.COLS))
        self.table.setHorizontalHeaderLabels(self.COLS)
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        self.table.verticalHeader().setVisible(False)

        row = QHBoxLayout()
        row.addWidget(_btn("Add condition", self._add_row))
        row.addWidget(_btn("Remove selected", self._remove_row))
        row.addStretch(1)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)

        root = QVBoxLayout(self)
        root.addLayout(head)
        root.addWidget(QLabel("Conditions (ALL must hold — AND):"))
        root.addWidget(self.table, 1)
        root.addLayout(row)
        root.addWidget(buttons)

        for cond in state.get("when", []):
            self._add_row(cond)
        if not state.get("when"):
            self._add_row()

    def _add_row(self, cond: Optional[dict] = None) -> None:
        cond = cond if isinstance(cond, dict) else {}
        r = self.table.rowCount()
        self.table.insertRow(r)
        ch_combo = _channel_combo(range(1, N_CHANNELS + 1),
                                  current=int(cond.get("ch", 1)))
        field_combo = QComboBox()
        field_combo.addItems(FIELDS)
        if cond.get("field") in FIELDS:
            field_combo.setCurrentText(cond["field"])
        op_combo = QComboBox()
        op_combo.addItems(list(OPS))
        if cond.get("op") in OPS:
            op_combo.setCurrentText(cond["op"])
        value_spin = QDoubleSpinBox(minimum=-1000.0, maximum=1000.0,
                                    decimals=3, singleStep=0.1,
                                    value=float(cond.get("value", 0.0)))
        for col, w in enumerate((ch_combo, field_combo, op_combo, value_spin)):
            self.table.setCellWidget(r, col, w)

    def _remove_row(self) -> None:
        r = self.table.currentRow()
        if r >= 0:
            self.table.removeRow(r)

    def _accept(self) -> None:
        when = []
        for r in range(self.table.rowCount()):
            when.append({
                "ch": self.table.cellWidget(r, 0).currentData(),
                "field": self.table.cellWidget(r, 1).currentText(),
                "op": self.table.cellWidget(r, 2).currentText(),
                "value": self.table.cellWidget(r, 3).value(),
            })
        self.result_state = {
            "state": self.name_edit.text().strip() or "?",
            "color": self.color_combo.currentData(),
            "when": when,
        }
        self.accept()


class IndicatorEditorDialog(QDialog):
    """Edit one indicator: name + ordered states (first match wins) + fallback."""

    def __init__(self, parent: QWidget,
                 indicator: Optional[dict] = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Edit indicator")
        self.setMinimumSize(520, 420)
        indicator = indicator or {}
        self.states: List[dict] = [dict(s) for s in indicator.get("states", [])]
        self.result_indicator: Optional[dict] = None
        self.deleted = False

        fallback = indicator.get("fallback") or dict(prof.DEFAULT_FALLBACK)
        self.name_edit = QLineEdit(indicator.get("name", ""))
        self.fb_name = QLineEdit(fallback.get("state", "—"))
        self.fb_color = _color_combo(fallback.get("color", _COLORS[0][1]))
        head = QFormLayout()
        head.addRow("Indicator name", self.name_edit)
        head.addRow("Fallback state (no match)", self.fb_name)
        head.addRow("Fallback color", self.fb_color)

        self.state_list = QListWidget()
        self.state_list.itemDoubleClicked.connect(lambda _: self._edit())

        row = QHBoxLayout()
        for b in (_btn("Add", self._add), _btn("Edit", self._edit),
                  _btn("Del", self._remove),
                  _btn("↑", lambda: self._move(-1)),
                  _btn("↓", lambda: self._move(+1))):
            row.addWidget(b)
        row.addStretch(1)

        delete_btn = _btn("Delete indicator", self._delete)
        delete_btn.setStyleSheet("color: #e53935;")
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)
        buttons.addButton(delete_btn, QDialogButtonBox.ActionRole)

        root = QVBoxLayout(self)
        root.addLayout(head)
        root.addWidget(QLabel("States (evaluated top-down, first match wins):"))
        root.addWidget(self.state_list, 1)
        root.addLayout(row)
        root.addWidget(buttons)
        self._refresh()

    def _refresh(self) -> None:
        self.state_list.clear()
        for s in self.states:
            conds = " AND ".join(prof.condition_summary(c)
                                 for c in s.get("when", [])) or "(never)"
            self.state_list.addItem(f'{s.get("state", "?")}  ·  {conds}')

    def _add(self) -> None:
        dlg = StateEditorDialog(self)
        if dlg.exec() == QDialog.Accepted and dlg.result_state:
            self.states.append(dlg.result_state)
            self._refresh()

    def _edit(self) -> None:
        i = self.state_list.currentRow()
        if i < 0:
            return
        dlg = StateEditorDialog(self, state=self.states[i])
        if dlg.exec() == QDialog.Accepted and dlg.result_state:
            self.states[i] = dlg.result_state
            self._refresh()
            self.state_list.setCurrentRow(i)

    def _remove(self) -> None:
        i = self.state_list.currentRow()
        if i >= 0:
            del self.states[i]
            self._refresh()

    def _move(self, delta: int) -> None:
        i = self.state_list.currentRow()
        j = i + delta
        if 0 <= i < len(self.states) and 0 <= j < len(self.states):
            self.states[i], self.states[j] = self.states[j], self.states[i]
            self._refresh()
            self.state_list.setCurrentRow(j)

    def _delete(self) -> None:
        self.deleted = True
        self.accept()

    def _accept(self) -> None:
        self.result_indicator = {
            "name": self.name_edit.text().strip() or "unnamed",
            "states": self.states,
            "fallback": {"state": self.fb_name.text().strip() or "—",
                         "color": self.fb_color.currentData()},
        }
        self.accept()


# --------------------------------------------------------------------------- #
# Live widgets
# --------------------------------------------------------------------------- #

class IndicatorLamp(QFrame):
    clicked = Signal()

    def __init__(self, indicator: dict) -> None:
        super().__init__()
        self.indicator = indicator
        self.setFrameShape(QFrame.StyledPanel)
        self.setStyleSheet("QFrame { border: 1px solid palette(mid);"
                           " border-radius: 6px; }")
        self.dot = QLabel("●")
        self.dot.setStyleSheet("font-size: 22px; border: none;")
        self.name_label = QLabel(f'<b>{indicator.get("name", "?")}</b>')
        self.name_label.setStyleSheet("border: none;")
        self.state_label = QLabel("—")
        self.state_label.setStyleSheet("border: none;")
        text = QVBoxLayout()
        text.setSpacing(0)
        text.addWidget(self.name_label)
        text.addWidget(self.state_label)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(10, 6, 12, 6)
        lay.addWidget(self.dot)
        lay.addLayout(text)

    def update_state(self, snap: Dict[int, dict]) -> None:
        state, color = evaluate_indicator(self.indicator, snap)
        self.dot.setStyleSheet(
            f"font-size: 22px; border: none; color: {color};")
        self.state_label.setText(
            f"<b style='color:{color};'>{state}</b>")

    def mousePressEvent(self, event) -> None:  # noqa: N802 (Qt override)
        self.clicked.emit()
        super().mousePressEvent(event)


# --------------------------------------------------------------------------- #
# The tab
# --------------------------------------------------------------------------- #

class OperateTab(QWidget):
    OVERVIEW_COLS = ("CH", "Signal", "Mode", "V", "I", "Status")

    #: emitted with {ch(int): name} whenever signal names change (load/edit).
    #: Signal(object), not Signal(dict): int keys don't survive the
    #: QVariantMap conversion a dict-typed signal would apply.
    names_changed = Signal(object)

    def __init__(self, get_client: Callable[[], Optional[object]],
                 report_error: Callable[[str], None]) -> None:
        super().__init__()
        self._get_client = get_client
        self._report = report_error
        self.profile: dict = prof.new_profile()
        self._snapshot: Dict[int, dict] = {}
        self._running_macro = False
        self._macro_actions: List[dict] = []
        self._macro_index = 0
        self.macro_ok_count = 0  # used by --selftest
        self._updating_table = False

        # ---- top bar -----------------------------------------------------
        self.name_edit = QLineEdit(self.profile.get("name", "DUT"))
        self.name_edit.textEdited.connect(
            lambda t: self.profile.__setitem__("name", t))
        self.edit_btn = QPushButton("Edit mode")
        self.edit_btn.setCheckable(True)
        self.edit_btn.toggled.connect(self._set_edit_mode)

        top = QHBoxLayout()
        top.addWidget(QLabel("DUT profile:"))
        top.addWidget(self.name_edit, 1)
        top.addWidget(_btn("Load…", self._load_file))
        top.addWidget(_btn("Save…", self._save_file))
        top.addWidget(_btn("New", self._new_profile))
        top.addSpacing(12)
        top.addWidget(self.edit_btn)

        # ---- indicators ----------------------------------------------------
        self.ind_box = QGroupBox("Indicators")
        self.ind_layout = QGridLayout(self.ind_box)
        self.ind_layout.setSpacing(6)
        self.ind_widgets: List[IndicatorLamp] = []
        self.add_ind_btn = _btn("+ Indicator", self._add_indicator)

        # ---- macros --------------------------------------------------------
        self.macro_box = QGroupBox("Macros")
        self.macro_layout = QGridLayout(self.macro_box)
        self.macro_layout.setSpacing(6)
        self.macro_buttons: List[QPushButton] = []
        self.add_macro_btn = _btn("+ Macro", self._add_macro)
        self.capture_btn = _btn(
            "Capture state", self._capture_state,
            "Create a macro from the current channel states (from telemetry)")

        self.macro_status = QLabel("")
        self.macro_status.setStyleSheet("color: palette(mid);")

        # ---- overview table -------------------------------------------------
        self.table = QTableWidget(N_CHANNELS, len(self.OVERVIEW_COLS))
        self.table.setHorizontalHeaderLabels(self.OVERVIEW_COLS)
        self.table.verticalHeader().setVisible(False)
        self.table.setSelectionMode(QAbstractItemView.NoSelection)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        self.table.itemChanged.connect(self._on_table_item_changed)
        for r in range(N_CHANNELS):
            for c in range(len(self.OVERVIEW_COLS)):
                item = QTableWidgetItem("")
                item.setFlags(Qt.ItemIsEnabled)
                self.table.setItem(r, c, item)
            self.table.item(r, 0).setText(f"CH{r + 1}")

        overview_box = QGroupBox("Channel overview")
        ov = QVBoxLayout(overview_box)
        self.edit_hint = QLabel("Edit mode: double-click a Signal cell to "
                                "rename; click macros/indicators to edit them.")
        self.edit_hint.setVisible(False)
        self.edit_hint.setWordWrap(True)
        ov.addWidget(self.edit_hint)
        ov.addWidget(self.table, 1)

        # ---- layout ----------------------------------------------------------
        left = QVBoxLayout()
        left.addWidget(self.ind_box)
        left.addWidget(self.macro_box)
        left.addWidget(self.macro_status)
        left.addStretch(1)

        body = QHBoxLayout()
        left_w = QWidget()
        left_w.setLayout(left)
        body.addWidget(left_w, 1)
        body.addWidget(overview_box, 1)

        root = QVBoxLayout(self)
        root.addLayout(top)
        root.addLayout(body, 1)

        # Keys 1-9 fire the first nine macros while this tab has focus
        for n in range(1, 10):
            QShortcut(QKeySequence(str(n)), self,
                      context=Qt.WidgetWithChildrenShortcut,
                      activated=lambda n=n: self._run_macro_index(n - 1))

        self._set_edit_mode(False)
        self._rebuild_all()

    # ------------------------------------------------------------------ #
    # Profile lifecycle
    # ------------------------------------------------------------------ #

    def _profile_dir(self) -> str:
        try:
            PROFILE_DIR.mkdir(parents=True, exist_ok=True)
        except OSError:
            pass
        return str(PROFILE_DIR)

    def _new_profile(self) -> None:
        if self.profile.get("channels") or self.profile.get("macros"):
            ok = QMessageBox.question(
                self, "New profile",
                "Discard the current profile (unsaved changes are lost)?")
            if ok != QMessageBox.Yes:
                return
        self.profile = prof.new_profile(self.name_edit.text().strip() or "DUT")
        self._rebuild_all()

    def _load_file(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Load DUT profile",
                                              self._profile_dir(),
                                              "JSON (*.json)")
        if path:
            self.load_profile_file(path)

    def load_profile_file(self, path: str) -> bool:
        try:
            self.profile = prof.load_profile(path)
        except (OSError, ValueError, KeyError) as e:
            self._report(f"profile load failed: {e}")
            return False
        self.name_edit.setText(self.profile.get("name", ""))
        self._rebuild_all()
        return True

    def _save_file(self) -> None:
        name = self.name_edit.text().strip() or "profile"
        self.profile["name"] = name
        path, _ = QFileDialog.getSaveFileName(
            self, "Save DUT profile",
            str(PROFILE_DIR / f"{name}.json"), "JSON (*.json)")
        if not path:
            return
        try:
            self._profile_dir()
            prof.save_profile(path, self.profile)
        except OSError as e:
            self._report(f"profile save failed: {e}")

    def _rebuild_all(self) -> None:
        self._publish_names()
        self._rebuild_macros()
        self._rebuild_indicators()
        self._refresh_signal_column()

    def _publish_names(self) -> None:
        names = self.profile.get("channels", {})
        prof.set_signal_names(names)
        self.names_changed.emit(dict(names))

    # ------------------------------------------------------------------ #
    # Macros
    # ------------------------------------------------------------------ #

    def _rebuild_macros(self) -> None:
        for b in self.macro_buttons:
            b.setParent(None)
        self.add_macro_btn.setParent(None)
        self.capture_btn.setParent(None)
        self.macro_buttons = []

        macros = self.profile.get("macros", [])
        for i, m in enumerate(macros):
            b = QPushButton(m.get("name", "?"))
            b.setMinimumHeight(44)
            color = m.get("color", _COLORS[0][1])
            b.setStyleSheet(
                f"QPushButton {{ background-color: {color}; color: white;"
                f" font-weight: bold; border-radius: 6px; padding: 4px 10px; }}"
                f"QPushButton:disabled {{ background-color: palette(mid); }}")
            if i < 9:
                b.setToolTip(f"Shortcut: {i + 1}")
            b.clicked.connect(lambda _=False, i=i: self._macro_clicked(i))
            self.macro_buttons.append(b)
            self.macro_layout.addWidget(b, i // _MACROS_PER_ROW,
                                        i % _MACROS_PER_ROW)
        n = len(macros)
        self.macro_layout.addWidget(self.add_macro_btn,
                                    n // _MACROS_PER_ROW, n % _MACROS_PER_ROW)
        self.macro_layout.addWidget(self.capture_btn,
                                    (n + 1) // _MACROS_PER_ROW,
                                    (n + 1) % _MACROS_PER_ROW)
        edit = self.edit_btn.isChecked()
        self.add_macro_btn.setVisible(edit)
        self.capture_btn.setVisible(edit)

    def _macro_clicked(self, index: int) -> None:
        if self.edit_btn.isChecked():
            self._edit_macro(index)
        else:
            self._run_macro_index(index)

    def _add_macro(self) -> None:
        dlg = MacroEditorDialog(self)
        if dlg.exec() == QDialog.Accepted and dlg.result_macro \
                and not dlg.deleted:
            self.profile.setdefault("macros", []).append(dlg.result_macro)
            self._rebuild_macros()

    def _edit_macro(self, index: int) -> None:
        macros = self.profile.get("macros", [])
        if not 0 <= index < len(macros):
            return
        dlg = MacroEditorDialog(self, macro=macros[index])
        if dlg.exec() != QDialog.Accepted:
            return
        if dlg.deleted:
            del macros[index]
        elif dlg.result_macro:
            macros[index] = dlg.result_macro
        self._rebuild_macros()

    def _capture_state(self) -> None:
        if not self._snapshot:
            self._report("no telemetry yet — connect first")
            return
        macro = {"name": "Captured state", "color": _COLORS[0][1],
                 "actions": capture_actions(self._snapshot)}
        dlg = MacroEditorDialog(self, macro=macro)
        if dlg.exec() == QDialog.Accepted and dlg.result_macro \
                and not dlg.deleted:
            self.profile.setdefault("macros", []).append(dlg.result_macro)
            self._rebuild_macros()

    # -- execution (QTimer stepper: waits don't block the UI) ------------- #

    def run_macro_named(self, name: str) -> None:
        for i, m in enumerate(self.profile.get("macros", [])):
            if m.get("name") == name:
                self._run_macro_index(i)
                return
        self._report(f"no macro named {name!r}")

    def _run_macro_index(self, index: int) -> None:
        macros = self.profile.get("macros", [])
        if self.edit_btn.isChecked() or self._running_macro \
                or not 0 <= index < len(macros):
            return
        if self._get_client() is None:
            self._report("not connected")
            return
        macro = macros[index]
        self._macro_actions = list(macro.get("actions", []))
        self._macro_index = 0
        self._macro_name = macro.get("name", "?")
        self._set_macro_running(True)
        self._macro_step()

    def _set_macro_running(self, running: bool) -> None:
        self._running_macro = running
        for b in self.macro_buttons:
            b.setEnabled(not running)
        if not running:
            self.macro_status.setText("")

    def _macro_step(self) -> None:
        from ..client import CommandError, ProtocolTimeout

        if self._macro_index >= len(self._macro_actions):
            self.macro_ok_count += 1
            self._set_macro_running(False)
            self.macro_status.setText(f"macro '{self._macro_name}' ✓")
            return
        client = self._get_client()
        if client is None:
            self._report("disconnected during macro")
            self._set_macro_running(False)
            return
        a = self._macro_actions[self._macro_index]
        self._macro_index += 1
        self.macro_status.setText(
            f"macro '{self._macro_name}': "
            f"{self._macro_index}/{len(self._macro_actions)} "
            f"{action_summary(a)}")
        if a.get("cmd") == "wait":
            QTimer.singleShot(int(a.get("ms", 0)), self._macro_step)
            return
        try:
            args = {k: v for k, v in a.items() if k != "cmd"}
            client.command(a["cmd"], **args)
        except (CommandError, ProtocolTimeout, KeyError, TypeError) as e:
            self._report(f"macro '{self._macro_name}' stopped at "
                         f"{action_summary(a)}: {e}")
            self._set_macro_running(False)
            return
        QTimer.singleShot(0, self._macro_step)

    # ------------------------------------------------------------------ #
    # Indicators
    # ------------------------------------------------------------------ #

    def _rebuild_indicators(self) -> None:
        for w in self.ind_widgets:
            w.setParent(None)
        self.add_ind_btn.setParent(None)
        self.ind_widgets = []

        indicators = self.profile.get("indicators", [])
        for i, ind in enumerate(indicators):
            lamp = IndicatorLamp(ind)
            lamp.clicked.connect(lambda i=i: self._indicator_clicked(i))
            lamp.update_state(self._snapshot)
            self.ind_widgets.append(lamp)
            self.ind_layout.addWidget(lamp, i // _MACROS_PER_ROW,
                                      i % _MACROS_PER_ROW)
        n = len(indicators)
        self.ind_layout.addWidget(self.add_ind_btn, n // _MACROS_PER_ROW,
                                  n % _MACROS_PER_ROW)
        self.add_ind_btn.setVisible(self.edit_btn.isChecked())

    def _indicator_clicked(self, index: int) -> None:
        if not self.edit_btn.isChecked():
            return
        indicators = self.profile.get("indicators", [])
        if not 0 <= index < len(indicators):
            return
        dlg = IndicatorEditorDialog(self, indicator=indicators[index])
        if dlg.exec() != QDialog.Accepted:
            return
        if dlg.deleted:
            del indicators[index]
        elif dlg.result_indicator:
            indicators[index] = dlg.result_indicator
        self._rebuild_indicators()

    def _add_indicator(self) -> None:
        dlg = IndicatorEditorDialog(self)
        if dlg.exec() == QDialog.Accepted and dlg.result_indicator \
                and not dlg.deleted:
            self.profile.setdefault("indicators", []).append(
                dlg.result_indicator)
            self._rebuild_indicators()

    # ------------------------------------------------------------------ #
    # Edit mode + signal names
    # ------------------------------------------------------------------ #

    def _set_edit_mode(self, on: bool) -> None:
        self.add_macro_btn.setVisible(on)
        self.capture_btn.setVisible(on)
        self.add_ind_btn.setVisible(on)
        self.edit_hint.setVisible(on)
        self._updating_table = True
        for r in range(N_CHANNELS):
            item = self.table.item(r, 1)
            flags = Qt.ItemIsEnabled
            if on:
                flags |= Qt.ItemIsEditable | Qt.ItemIsSelectable
            item.setFlags(flags)
        self._updating_table = False

    def _refresh_signal_column(self) -> None:
        names = self.profile.get("channels", {})
        self._updating_table = True
        for r in range(N_CHANNELS):
            self.table.item(r, 1).setText(names.get(r + 1, ""))
        self._updating_table = False

    def _on_table_item_changed(self, item: QTableWidgetItem) -> None:
        if self._updating_table or item.column() != 1:
            return
        ch = item.row() + 1
        name = item.text().strip()
        channels = self.profile.setdefault("channels", {})
        if name:
            channels[ch] = name
        else:
            channels.pop(ch, None)
        self._publish_names()

    # ------------------------------------------------------------------ #
    # Telemetry
    # ------------------------------------------------------------------ #

    def update_telemetry(self, msg: dict) -> None:
        self._snapshot = telemetry_snapshot(msg)
        for lamp in self.ind_widgets:
            lamp.update_state(self._snapshot)

        self._updating_table = True
        for ch, d in self._snapshot.items():
            r = ch - 1
            if not 0 <= r < N_CHANNELS:
                continue
            st, conn = d.get("st", 0), d.get("conn", False)
            mode = d.get("mode", "?")
            if mode == "RELAY":
                v = d.get("vout", d.get("v", 0.0))
                v_text = f"{v:.2f} V"
                if "vin" in d:
                    v_text += f'  (in {d.get("vin", 0.0):.2f})'
                i_text = f'{d.get("i", 0.0):.3f} A' if "i" in d else "—"
                mode_text = "relay ON" if conn else "relay open"
            else:
                v_text = f'{d.get("v", 0.0):.3f} V'
                i_text = f'{d.get("i", 0.0) * 1000:.1f} mA'
                mode_text = mode
            status = _FAULT_TEXT.get(st, "on" if conn else "off")
            self.table.item(r, 2).setText(mode_text)
            self.table.item(r, 3).setText(v_text)
            self.table.item(r, 4).setText(i_text)
            status_item = self.table.item(r, 5)
            status_item.setText(status)
            color = ("#e53935" if st else
                     ("#43a047" if conn else "#9e9e9e"))
            status_item.setForeground(QBrush(QColor(color)))
        self._updating_table = False
