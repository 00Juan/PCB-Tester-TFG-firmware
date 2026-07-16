"""Testbench tab: build/edit test campaigns, run them on the device, and
view streamed results.

A campaign is a list of test dicts ({"name","type","setup":[…],"params":{…}})
matching the firmware's tb.add schema 1:1. Campaign files are JSON documents
{"format":"pcbtester-campaign","name":…,"tests":[…]} kept under pc/campaigns/.

Campaign files may also carry optional "pre" / "post" lists of raw protocol
commands ({"cmd":"ch.connect","ch":9}, … or {"cmd":"wait","ms":300}) that the
GUI sends once before tb.run and once after tb_done. Used e.g. by the TSAL
campaign to close the HP relays (CH9/CH10 LED pull-ups) and the HV relay
(CH11, DUT GND return) for the whole run and release them afterwards.

The test editor is schema-driven: TEST_SCHEMAS maps each test type to its
parameter fields, from which the form is generated. A JSON tab mirrors the
same test object for hand editing (power users / copy-paste).
"""

from __future__ import annotations

import csv
import json
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

from PySide6.QtCore import Qt
from PySide6.QtGui import QBrush, QColor
from PySide6.QtWidgets import (
    QAbstractItemView, QCheckBox, QComboBox, QDialog, QDialogButtonBox,
    QDoubleSpinBox, QFileDialog, QFormLayout, QHBoxLayout, QHeaderView, QLabel,
    QLineEdit, QListWidget, QPlainTextEdit, QProgressBar, QPushButton,
    QSpinBox, QTabWidget, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget,
)

CAMPAIGN_DIR = Path(__file__).resolve().parents[2] / "campaigns"

# --------------------------------------------------------------------------- #
# Test type schemas: (param_key, label, kind, min, max, default)
# kind: "float" | "int" | "mask"
# --------------------------------------------------------------------------- #

TEST_TYPES = [
    ("static_voltage", "Static voltage"),
    ("voltage_accuracy", "Voltage accuracy"),
    ("voltage_threshold", "Voltage threshold (latency)"),
    ("voltage_ripple", "Voltage ripple"),
    ("current_consumption", "Current consumption"),
    ("current_inrush", "Current inrush"),
    ("power_sequence", "Power sequence"),
    ("pwm_integrity", "PWM integrity"),
    ("short_circuit", "Short-circuit protection"),
    ("load_regulation", "Load regulation"),
    ("cross_isolation", "Cross-channel isolation"),
]

TEST_SCHEMAS: Dict[str, list] = {
    "static_voltage": [
        ("sense_mask", "Sense channels", "mask", 0, 0, 0),
        ("sense_hp_mask", "Sense HP mask (1=CH9, 2=CH10, 3=both)", "int", 0, 3, 0),
        ("expected_v", "Expected voltage (V)", "float", -1.0, 15.0, 12.0),
        ("tolerance_v", "Tolerance (V)", "float", 0.0, 5.0, 0.5),
        ("settle_ms", "Settle (ms)", "int", 0, 60000, 200),
    ],
    "voltage_accuracy": [
        ("mask", "Drive channels", "mask", 0, 0, 0),
        ("target_v", "Target voltage (V)", "float", 0.0, 12.0, 3.3),
        ("tolerance_v", "Tolerance (V)", "float", 0.0, 2.0, 0.05),
        ("settle_ms", "Settle (ms)", "int", 0, 60000, 100),
        ("samples", "Samples to average", "int", 1, 64, 8),
    ],
    "voltage_threshold": [
        ("drive_mask", "Drive channels", "mask", 0, 0, 0),
        ("drive_v", "Drive voltage (V)", "float", 0.0, 12.0, 5.0),
        ("sense_mask", "Sense channels", "mask", 0, 0, 0),
        ("threshold_v", "Threshold (V)", "float", 0.0, 15.0, 3.0),
        ("timeout_ms", "Timeout (ms)", "int", 1, 120000, 1000),
    ],
    "voltage_ripple": [
        ("mask", "Channels", "mask", 0, 0, 0),
        ("drive_v", "Drive voltage (V)", "float", 0.0, 12.0, 5.0),
        ("window_ms", "Window (ms)", "int", 1, 60000, 200),
        ("samples", "Samples", "int", 1, 64, 32),
        ("max_ripple_v", "Max ripple pk-pk (V)", "float", 0.0, 5.0, 0.1),
    ],
    "current_consumption": [
        ("mask", "Channels", "mask", 0, 0, 0),
        ("drive_v", "Drive voltage (V)", "float", 0.0, 12.0, 5.0),
        ("min_a", "Min current (A)", "float", 0.0, 0.5, 0.0),
        ("max_a", "Max current (A)", "float", 0.0, 0.5, 0.1),
        ("settle_ms", "Settle (ms)", "int", 0, 60000, 100),
        ("samples", "Samples to average", "int", 1, 64, 8),
    ],
    "current_inrush": [
        ("mask", "Channels", "mask", 0, 0, 0),
        ("drive_v", "Drive voltage (V)", "float", 0.0, 12.0, 5.0),
        ("window_ms", "Capture window (ms)", "int", 1, 60000, 500),
        ("interval_ms", "Sample interval (ms)", "int", 1, 1000, 2),
        ("max_a", "Max inrush (A)", "float", 0.0, 0.5, 0.3),
        ("settle_band_a", "Settle band (A)", "float", 0.0, 0.5, 0.01),
    ],
    "power_sequence": [
        ("tolerance_v", "Step tolerance (V)", "float", 0.0, 5.0, 0.2),
        # per-step list (mask/target_v/delay_ms) is edited in the JSON tab
    ],
    "pwm_integrity": [
        ("drive_mask", "PWM channels (CH1-4)", "mask", 0, 0, 0),
        ("sense_mask", "Sense channels", "mask", 0, 0, 0),
        ("drive_v", "Amplitude (V)", "float", 0.0, 12.0, 3.3),
        ("duty", "Duty (0-255)", "int", 0, 255, 128),
        ("freq", "Frequency (Hz)", "int", 1, 150000, 1000),
        ("settle_ms", "Settle (ms)", "int", 0, 60000, 200),
        ("tolerance_v", "DC tolerance (V)", "float", 0.0, 5.0, 0.2),
    ],
    "short_circuit": [
        ("mask", "Channels", "mask", 0, 0, 0),
        ("drive_v", "Drive voltage (V)", "float", 0.0, 12.0, 5.0),
        ("short_limit_a", "Trip limit (A)", "float", 0.0, 0.5, 0.05),
        ("timeout_ms", "Timeout (ms)", "int", 1, 60000, 1000),
    ],
    "load_regulation": [
        ("drive_mask", "Drive channels", "mask", 0, 0, 0),
        ("load_mask", "Load channels (0 = none)", "mask", 0, 0, 0),
        ("drive_v", "Drive voltage (V)", "float", 0.0, 12.0, 5.0),
        ("load_a", "Load current (A)", "float", 0.0, 0.5, 0.02),
        ("settle_ms", "Settle (ms)", "int", 0, 60000, 200),
        ("max_drop_v", "Max drop (V)", "float", 0.0, 5.0, 0.2),
    ],
    "cross_isolation": [
        ("drive_mask", "Drive channels", "mask", 0, 0, 0),
        ("sense_mask", "Sense channels (0 = all others)", "mask", 0, 0, 0),
        ("drive_v", "Drive voltage (V)", "float", 0.0, 12.0, 5.0),
        ("settle_ms", "Settle (ms)", "int", 0, 60000, 100),
        ("max_coupling_v", "Max coupling (V)", "float", 0.0, 5.0, 0.2),
    ],
}

_OUTCOME_COLORS = {
    "PASS": "#43a047", "FAIL": "#e53935",
    "TIMEOUT": "#fb8c00", "ERROR": "#fb8c00", "SKIP": "#9e9e9e",
}

_SETUP_PLACEHOLDER = (
    '[\n  {"step": "vs", "ch": 1, "v": 1.0},\n'
    '  {"step": "sr", "bit": 8, "on": false},\n'
    '  {"step": "wait", "ms": 1000},\n'
    '  {"step": "sr", "bit": 8, "on": true}\n]'
)


class MaskEdit(QWidget):
    """8 checkboxes -> LVLP channel bitmask (bit 0 = CH1)."""

    def __init__(self) -> None:
        super().__init__()
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(4)
        self.checks = []
        for i in range(8):
            cb = QCheckBox(str(i + 1))
            self.checks.append(cb)
            lay.addWidget(cb)
        lay.addStretch(1)

    def value(self) -> int:
        return sum(1 << i for i, cb in enumerate(self.checks) if cb.isChecked())

    def set_value(self, mask: int) -> None:
        for i, cb in enumerate(self.checks):
            cb.setChecked(bool(mask & (1 << i)))


class TestEditorDialog(QDialog):
    """Edit one test: schema-driven form + raw JSON tab (two-way sync)."""

    def __init__(self, parent: QWidget, test: Optional[dict] = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Edit test")
        self.setMinimumWidth(560)

        self.name_edit = QLineEdit()
        self.type_combo = QComboBox()
        for json_type, label in TEST_TYPES:
            self.type_combo.addItem(label, userData=json_type)
        self.type_combo.currentIndexChanged.connect(self._rebuild_form)

        head = QFormLayout()
        head.addRow("Name", self.name_edit)
        head.addRow("Type", self.type_combo)

        # Form page
        self.form_holder = QWidget()
        self.form_layout = QFormLayout(self.form_holder)
        self.param_widgets: Dict[str, QWidget] = {}
        self.setup_edit = QPlainTextEdit()
        self.setup_edit.setPlaceholderText(_SETUP_PLACEHOLDER)
        self.setup_edit.setMaximumHeight(110)
        self.setup_edit.setStyleSheet("font-family: Menlo, monospace;")

        form_page = QWidget()
        fp = QVBoxLayout(form_page)
        fp.addWidget(self.form_holder)
        fp.addWidget(QLabel("DUT setup steps before the test (JSON list, "
                            "optional — vs/cs/hz/pwm/wait/sr):"))
        fp.addWidget(self.setup_edit)
        fp.addStretch(1)

        # JSON page
        self.json_edit = QPlainTextEdit()
        self.json_edit.setStyleSheet("font-family: Menlo, monospace;")

        self.tabs = QTabWidget()
        self.tabs.addTab(form_page, "Form")
        self.tabs.addTab(self.json_edit, "JSON")
        self.tabs.currentChanged.connect(self._tab_changed)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)

        self.error_label = QLabel("")
        self.error_label.setStyleSheet("color: #e53935;")

        root = QVBoxLayout(self)
        root.addLayout(head)
        root.addWidget(self.tabs, 1)
        root.addWidget(self.error_label)
        root.addWidget(buttons)

        self.result_test: Optional[dict] = None
        self._rebuild_form()
        if test:
            self.from_test(test)

    # ------------------------------------------------------------------ #

    def _current_type(self) -> str:
        return self.type_combo.currentData()

    def _rebuild_form(self) -> None:
        while self.form_layout.rowCount():
            self.form_layout.removeRow(0)
        self.param_widgets.clear()
        for key, label, kind, lo, hi, default in TEST_SCHEMAS[self._current_type()]:
            if kind == "mask":
                w: QWidget = MaskEdit()
            elif kind == "float":
                w = QDoubleSpinBox(minimum=lo, maximum=hi, value=default,
                                   decimals=3, singleStep=0.1)
            else:
                w = QSpinBox(minimum=int(lo), maximum=int(hi), value=int(default))
            self.param_widgets[key] = w
            self.form_layout.addRow(label, w)
        if self._current_type() == "power_sequence":
            self.form_layout.addRow(QLabel(
                'Sequence steps: edit "params.steps" in the JSON tab\n'
                '(list of {"mask","target_v","delay_ms"})'))

    def to_test(self) -> dict:
        params: Dict[str, Any] = {}
        for key, w in self.param_widgets.items():
            if isinstance(w, MaskEdit):
                params[key] = w.value()
            elif isinstance(w, QDoubleSpinBox):
                params[key] = w.value()
            elif isinstance(w, QSpinBox):
                params[key] = w.value()
        if self._current_type() == "power_sequence":
            params.setdefault("steps", getattr(self, "_seq_steps", []))
        test: Dict[str, Any] = {
            "name": self.name_edit.text().strip() or "unnamed",
            "type": self._current_type(),
            "params": params,
        }
        setup_text = self.setup_edit.toPlainText().strip()
        if setup_text:
            test["setup"] = json.loads(setup_text)  # ValueError surfaces to caller
        return test

    def from_test(self, t: dict) -> None:
        self.name_edit.setText(t.get("name", ""))
        idx = next((i for i, (jt, _) in enumerate(TEST_TYPES)
                    if jt == t.get("type")), 0)
        self.type_combo.setCurrentIndex(idx)
        params = t.get("params", {})
        for key, w in self.param_widgets.items():
            if key not in params:
                continue
            if isinstance(w, MaskEdit):
                w.set_value(int(params[key]))
            else:
                w.setValue(params[key])
        self._seq_steps = params.get("steps", [])
        setup = t.get("setup")
        self.setup_edit.setPlainText(
            json.dumps(setup, indent=1) if setup else "")

    def _tab_changed(self, index: int) -> None:
        self.error_label.setText("")
        if index == 1:  # -> JSON: render current form state
            try:
                self.json_edit.setPlainText(json.dumps(self.to_test(), indent=2))
            except ValueError as e:
                self.error_label.setText(f"setup steps JSON: {e}")
        else:           # -> Form: parse JSON back
            text = self.json_edit.toPlainText().strip()
            if text:
                try:
                    self.from_test(json.loads(text))
                except (ValueError, KeyError) as e:
                    self.error_label.setText(f"JSON: {e}")

    def _accept(self) -> None:
        try:
            if self.tabs.currentIndex() == 1:
                self.result_test = json.loads(self.json_edit.toPlainText())
                if not isinstance(self.result_test, dict):
                    raise ValueError("test must be a JSON object")
            else:
                self.result_test = self.to_test()
        except ValueError as e:
            self.error_label.setText(f"invalid JSON: {e}")
            return
        self.accept()


class TestbenchTab(QWidget):
    RESULT_COLS = ("#", "Test", "Outcome", "Measured", "Expected", "ms", "Detail")

    def __init__(self, get_client: Callable[[], Optional[object]],
                 report_error: Callable[[str], None]) -> None:
        super().__init__()
        self._get_client = get_client
        self._report = report_error
        self.tests: List[dict] = []
        # Raw protocol commands run once before tb.run / after tb_done
        # (campaign JSON "pre" / "post" lists; see module docstring).
        self.pre_cmds: List[dict] = []
        self.post_cmds: List[dict] = []
        self.campaign_name = "campaign"
        self._running = False
        self._done_summary: Optional[dict] = None  # for selftest/export

        # ---- left: campaign panel ---------------------------------------
        self.name_edit = QLineEdit(self.campaign_name)
        self.test_list = QListWidget()
        self.test_list.setSelectionMode(QAbstractItemView.SingleSelection)
        self.test_list.itemDoubleClicked.connect(lambda _: self._edit())

        def btn(text, fn, tip=""):
            b = QPushButton(text)
            b.clicked.connect(fn)
            if tip:
                b.setToolTip(tip)
            return b

        list_btns = QHBoxLayout()
        for b in (btn("Add", self._add), btn("Edit", self._edit),
                  btn("Dup", self._duplicate, "Duplicate selected test"),
                  btn("Del", self._remove),
                  btn("↑", self._move_up), btn("↓", self._move_down)):
            list_btns.addWidget(b)

        file_btns = QHBoxLayout()
        file_btns.addWidget(btn("Load…", self._load_file))
        file_btns.addWidget(btn("Save…", self._save_file))
        file_btns.addStretch(1)

        left = QVBoxLayout()
        name_row = QHBoxLayout()
        name_row.addWidget(QLabel("Campaign:"))
        name_row.addWidget(self.name_edit, 1)
        left.addLayout(name_row)
        left.addWidget(self.test_list, 1)
        left.addLayout(list_btns)
        left.addLayout(file_btns)

        # ---- right: run panel --------------------------------------------
        self.run_btn = QPushButton("▶ Run campaign")
        self.run_btn.clicked.connect(self._run)
        self.abort_btn = QPushButton("Abort")
        self.abort_btn.setEnabled(False)
        self.abort_btn.clicked.connect(self._abort)
        self.progress = QProgressBar()
        self.progress.setFormat("idle")
        self.progress.setValue(0)

        run_row = QHBoxLayout()
        run_row.addWidget(self.run_btn)
        run_row.addWidget(self.abort_btn)
        run_row.addWidget(self.progress, 1)

        self.results = QTableWidget(0, len(self.RESULT_COLS))
        self.results.setHorizontalHeaderLabels(self.RESULT_COLS)
        self.results.horizontalHeader().setSectionResizeMode(
            len(self.RESULT_COLS) - 1, QHeaderView.Stretch)
        self.results.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.results.verticalHeader().setVisible(False)

        self.summary_label = QLabel("")
        export_row = QHBoxLayout()
        export_row.addWidget(self.summary_label, 1)
        export_row.addWidget(btn("Export CSV…", self._export_csv))
        export_row.addWidget(btn("Export JSON…", self._export_json))

        right = QVBoxLayout()
        right.addLayout(run_row)
        right.addWidget(self.results, 1)
        right.addLayout(export_row)

        root = QHBoxLayout(self)
        left_w = QWidget()
        left_w.setLayout(left)
        left_w.setMaximumWidth(380)
        root.addWidget(left_w)
        right_w = QWidget()
        right_w.setLayout(right)
        root.addWidget(right_w, 1)

        self._result_rows: List[dict] = []

    # ------------------------------------------------------------------ #
    # Campaign list management
    # ------------------------------------------------------------------ #

    def _refresh_list(self) -> None:
        self.test_list.clear()
        for t in self.tests:
            self.test_list.addItem(f'{t.get("name", "?")}  ·  {t.get("type", "?")}')

    def _selected(self) -> int:
        return self.test_list.currentRow()

    def _add(self) -> None:
        dlg = TestEditorDialog(self)
        if dlg.exec() == QDialog.Accepted and dlg.result_test:
            self.tests.append(dlg.result_test)
            self._refresh_list()

    def _edit(self) -> None:
        i = self._selected()
        if i < 0:
            return
        dlg = TestEditorDialog(self, test=self.tests[i])
        if dlg.exec() == QDialog.Accepted and dlg.result_test:
            self.tests[i] = dlg.result_test
            self._refresh_list()
            self.test_list.setCurrentRow(i)

    def _duplicate(self) -> None:
        i = self._selected()
        if i >= 0:
            copy = json.loads(json.dumps(self.tests[i]))
            copy["name"] = copy.get("name", "test") + " (copy)"
            self.tests.insert(i + 1, copy)
            self._refresh_list()

    def _remove(self) -> None:
        i = self._selected()
        if i >= 0:
            del self.tests[i]
            self._refresh_list()

    def _move(self, i: int, j: int) -> None:
        if 0 <= i < len(self.tests) and 0 <= j < len(self.tests):
            self.tests[i], self.tests[j] = self.tests[j], self.tests[i]
            self._refresh_list()
            self.test_list.setCurrentRow(j)

    def _move_up(self) -> None:
        self._move(self._selected(), self._selected() - 1)

    def _move_down(self) -> None:
        self._move(self._selected(), self._selected() + 1)

    # ------------------------------------------------------------------ #
    # Campaign files
    # ------------------------------------------------------------------ #

    def _campaign_dir(self) -> str:
        try:
            CAMPAIGN_DIR.mkdir(parents=True, exist_ok=True)
        except OSError:
            pass
        return str(CAMPAIGN_DIR)

    def _load_file(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Load campaign",
                                              self._campaign_dir(), "JSON (*.json)")
        if not path:
            return
        try:
            with open(path) as f:
                data = json.load(f)
            tests = data["tests"]
            if not isinstance(tests, list):
                raise ValueError("tests must be a list")
        except (OSError, ValueError, KeyError) as e:
            self._report(f"campaign load failed: {e}")
            return
        self.tests = tests
        self.pre_cmds = data.get("pre", []) or []
        self.post_cmds = data.get("post", []) or []
        self.name_edit.setText(data.get("name", Path(path).stem))
        self._refresh_list()

    def _save_file(self) -> None:
        name = self.name_edit.text().strip() or "campaign"
        path, _ = QFileDialog.getSaveFileName(
            self, "Save campaign", str(Path(self._campaign_dir()) / f"{name}.json"),
            "JSON (*.json)")
        if not path:
            return
        data = {"format": "pcbtester-campaign", "version": 1, "name": name,
                "tests": self.tests}
        if self.pre_cmds:
            data["pre"] = self.pre_cmds
        if self.post_cmds:
            data["post"] = self.post_cmds
        with open(path, "w") as f:
            json.dump(data, f, indent=2)

    # ------------------------------------------------------------------ #
    # Run / events
    # ------------------------------------------------------------------ #

    def _exec_cmds(self, client, cmds: List[dict], label: str) -> bool:
        """Send the campaign's raw pre/post protocol commands one by one.

        Supported entries: {"cmd":"wait","ms":N} (host-side sleep) or any
        protocol command object, e.g. {"cmd":"ch.connect","ch":9}.
        Returns False on the first failure (already reported).
        """
        import time
        from ..client import CommandError, ProtocolTimeout

        for c in cmds:
            try:
                if c.get("cmd") == "wait":
                    time.sleep(float(c.get("ms", 0)) / 1000.0)
                    continue
                args = {k: v for k, v in c.items() if k != "cmd"}
                client.command(c["cmd"], **args)
            except (CommandError, ProtocolTimeout, KeyError, TypeError) as e:
                self._report(f"campaign {label} step {c}: {e}")
                return False
        return True

    def _run(self) -> None:
        client = self._get_client()
        if client is None:
            self._report("not connected")
            return
        if not self.tests:
            self._report("campaign is empty")
            return
        from ..client import CommandError, ProtocolTimeout

        self.results.setRowCount(0)
        self._result_rows = []
        self._done_summary = None
        self.summary_label.setText("")
        if not self._exec_cmds(client, self.pre_cmds, "pre"):
            self._exec_cmds(client, self.post_cmds, "post")  # release relays
            return
        try:
            client.load_campaign(self.tests)
            client.tb_run()
        except (CommandError, ProtocolTimeout) as e:
            self._report(f"testbench: {e}")
            self._exec_cmds(client, self.post_cmds, "post")
            return
        self._set_running(True)

    def _abort(self) -> None:
        client = self._get_client()
        if client is not None:
            try:
                client.tb_abort()
            except Exception as e:
                self._report(f"abort: {e}")

    def _set_running(self, running: bool) -> None:
        self._running = running
        self.run_btn.setEnabled(not running)
        self.abort_btn.setEnabled(running)
        if running:
            self.progress.setRange(0, len(self.tests))
            self.progress.setValue(0)
            self.progress.setFormat("starting…")

    def on_tb_progress(self, msg: dict) -> None:
        idx = msg.get("test", 0)
        self.progress.setRange(0, msg.get("of", len(self.tests)))
        self.progress.setValue(idx)
        self.progress.setFormat(
            f'{idx + 1}/{msg.get("of", "?")} · {msg.get("name", "")} · '
            f'{msg.get("elapsed_ms", 0) / 1000.0:.1f}s')

    def on_tb_result(self, msg: dict) -> None:
        self._result_rows.append(msg)
        row = self.results.rowCount()
        self.results.insertRow(row)
        outcome = msg.get("outcome", "?")
        values = (str(msg.get("test", row) + 1), msg.get("name", ""), outcome,
                  f'{msg.get("measured", 0):.4g}', f'{msg.get("expected", 0):.4g}',
                  str(msg.get("ms", "")), msg.get("detail", ""))
        for col, text in enumerate(values):
            item = QTableWidgetItem(text)
            if col == 2:
                item.setForeground(QBrush(QColor(
                    _OUTCOME_COLORS.get(outcome, "#9e9e9e"))))
                font = item.font()
                font.setBold(True)
                item.setFont(font)
            self.results.setItem(row, col, item)
        self.results.scrollToBottom()

    def on_tb_done(self, msg: dict) -> None:
        self._done_summary = msg
        self._set_running(False)
        self.progress.setValue(self.progress.maximum())
        aborted = " · ABORTED" if msg.get("aborted") else ""
        self.progress.setFormat("done")
        self.summary_label.setText(
            f'<b>{msg.get("pass", 0)} passed · {msg.get("fail", 0)} failed'
            f' · {msg.get("total", 0)} total{aborted}</b>')
        # Campaign teardown (also after an abort): release relays, etc.
        client = self._get_client()
        if client is not None and self.post_cmds:
            self._exec_cmds(client, self.post_cmds, "post")

    # ------------------------------------------------------------------ #
    # Export
    # ------------------------------------------------------------------ #

    def _export_meta(self) -> dict:
        return {"campaign": self.name_edit.text().strip(),
                "date": datetime.now().isoformat(timespec="seconds"),
                "summary": self._done_summary or {}}

    def _export_csv(self) -> None:
        if not self._result_rows:
            self._report("no results to export")
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "Export results CSV",
            f"results-{self.name_edit.text().strip()}-{datetime.now():%Y%m%d-%H%M%S}.csv",
            "CSV (*.csv)")
        if not path:
            return
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["test", "name", "outcome", "measured", "expected",
                        "ms", "detail"])
            for r in self._result_rows:
                w.writerow([r.get("test"), r.get("name"), r.get("outcome"),
                            r.get("measured"), r.get("expected"), r.get("ms"),
                            r.get("detail")])

    def _export_json(self) -> None:
        if not self._result_rows:
            self._report("no results to export")
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "Export results JSON",
            f"results-{self.name_edit.text().strip()}-{datetime.now():%Y%m%d-%H%M%S}.json",
            "JSON (*.json)")
        if not path:
            return
        data = dict(self._export_meta(), results=self._result_rows)
        with open(path, "w") as f:
            json.dump(data, f, indent=2)
