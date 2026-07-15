"""Calibration tab: view/edit per-channel calibration, NVS persistence,
and JSON file backup/restore of the whole device.

The editor shows the exact JSON the protocol uses (cal.get/cal.set), so an
engineer can tweak a single coefficient and write it back. RAM writes take
effect immediately; nothing survives a power cycle until "Save to device NVS".
"""

from __future__ import annotations

import json
from datetime import datetime
from typing import Callable, Optional

from PySide6.QtWidgets import (
    QComboBox, QFileDialog, QHBoxLayout, QLabel, QPlainTextEdit, QPushButton,
    QVBoxLayout, QWidget,
)

_CHANNELS = [f"CH{i}" for i in range(1, 9)] + ["CH9 (HP1)", "CH10 (HP2)", "CH11 (HV)"]


class CalibrationTab(QWidget):
    def __init__(self, get_client: Callable[[], Optional[object]]) -> None:
        super().__init__()
        self._get_client = get_client

        top = QHBoxLayout()
        self.ch_combo = QComboBox()
        self.ch_combo.addItems(_CHANNELS)
        read_btn = QPushButton("Read from device")
        read_btn.clicked.connect(self._read)
        write_btn = QPushButton("Write to device (RAM)")
        write_btn.clicked.connect(self._write)
        top.addWidget(QLabel("Channel:"))
        top.addWidget(self.ch_combo)
        top.addWidget(read_btn)
        top.addWidget(write_btn)
        top.addStretch(1)

        self.editor = QPlainTextEdit()
        self.editor.setStyleSheet("font-family: Menlo, monospace;")
        self.editor.setPlaceholderText(
            "Select a channel and click 'Read from device'.")

        bottom = QHBoxLayout()
        save_btn = QPushButton("Save to device NVS")
        save_btn.setToolTip("Persist ALL channels' current calibration to flash")
        save_btn.clicked.connect(self._save_nvs)
        load_btn = QPushButton("Reload from NVS")
        load_btn.clicked.connect(self._load_nvs)
        backup_btn = QPushButton("Backup all to file…")
        backup_btn.clicked.connect(self._backup)
        restore_btn = QPushButton("Restore from file…")
        restore_btn.clicked.connect(self._restore)
        bottom.addWidget(save_btn)
        bottom.addWidget(load_btn)
        bottom.addStretch(1)
        bottom.addWidget(backup_btn)
        bottom.addWidget(restore_btn)

        self.status = QLabel("")

        layout = QVBoxLayout(self)
        layout.addLayout(top)
        layout.addWidget(self.editor, 1)
        layout.addLayout(bottom)
        layout.addWidget(self.status)

    # ------------------------------------------------------------------ #

    def _client(self):
        client = self._get_client()
        if client is None:
            self.status.setText("Not connected.")
        return client

    def _run(self, what: str, fn, *args, **kwargs):
        from ..client import CommandError, ProtocolTimeout

        try:
            result = fn(*args, **kwargs)
            self.status.setText(f"{what}: OK")
            return result
        except (CommandError, ProtocolTimeout, ValueError) as e:
            self.status.setText(f"{what}: {e}")
            return None

    def _current_ch(self) -> int:
        return self.ch_combo.currentIndex() + 1

    # ------------------------------------------------------------------ #

    def _read(self) -> None:
        client = self._client()
        if client is None:
            return
        ch = self._current_ch()
        cal = self._run(f"read CH{ch}", client.get_calibration, ch)
        if cal is not None:
            self.editor.setPlainText(json.dumps(cal, indent=2))

    def _write(self) -> None:
        client = self._client()
        if client is None:
            return
        ch = self._current_ch()
        try:
            cal = json.loads(self.editor.toPlainText())
        except ValueError as e:
            self.status.setText(f"invalid JSON: {e}")
            return
        self._run(f"write CH{ch} (RAM only — use 'Save to device NVS' to persist)",
                  client.set_calibration, ch, cal)

    def _save_nvs(self) -> None:
        client = self._client()
        if client is None:
            return
        self._run("save all channels to NVS", client.save_calibration)

    def _load_nvs(self) -> None:
        client = self._client()
        if client is None:
            return
        ack = self._run("reload from NVS", client.load_calibration)
        if ack is not None:
            self.status.setText(f"reload from NVS: {ack.get('loaded', 0)}/11 "
                                f"channels had stored calibration")

    # ------------------------------------------------------------------ #

    def _backup(self) -> None:
        client = self._client()
        if client is None:
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "Backup calibration",
            f"pcbtester-cal-{datetime.now():%Y%m%d-%H%M%S}.json",
            "JSON (*.json)")
        if not path:
            return
        data = {"format": "pcbtester-calibration", "version": 1,
                "date": datetime.now().isoformat(timespec="seconds"),
                "channels": {}}
        for ch in range(1, 12):
            cal = self._run(f"read CH{ch}", client.get_calibration, ch)
            if cal is None:
                return
            data["channels"][str(ch)] = cal
        with open(path, "w") as f:
            json.dump(data, f, indent=2)
        self.status.setText(f"backup written: {path}")

    def _restore(self) -> None:
        client = self._client()
        if client is None:
            return
        path, _ = QFileDialog.getOpenFileName(self, "Restore calibration", "",
                                              "JSON (*.json)")
        if not path:
            return
        try:
            with open(path) as f:
                data = json.load(f)
            channels = data["channels"]
        except (ValueError, KeyError) as e:
            self.status.setText(f"invalid backup file: {e}")
            return
        for ch_str, cal in channels.items():
            if self._run(f"restore CH{ch_str}", client.set_calibration,
                         int(ch_str), cal) is None:
                return
        self.status.setText(
            f"restored {len(channels)} channels to RAM — "
            f"click 'Save to device NVS' to persist")
