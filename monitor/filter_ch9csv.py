"""PlatformIO device-monitor filter: auto-save CH9 energy-logger CSV dumps.

The test_HPCH9Energy firmware wraps every export between the marker lines

    === CSV BEGIN ===
    ...
    === CSV END ===

This filter watches the incoming serial stream for those markers and writes
everything between them to a timestamped file under pc/, so a run ends up on
disk without any copy-paste.  The stream itself is passed through untouched,
so the monitor still shows the dump as usual.

Enabled per environment in platformio.ini:

    monitor_filters = ch9csv

PlatformIO discovers this file because it lives in the project's "monitor"
directory and is named filter_*.py.
"""

import os
from datetime import datetime

from platformio.public import DeviceMonitorFilterBase

BEGIN_MARKER = "=== CSV BEGIN ==="
END_MARKER = "=== CSV END ==="

# Where the captured files go, relative to the project directory.
OUTPUT_SUBDIR = "pc"

# Matches the existing results-<name>-<date>.csv files in pc/.
FILENAME_TEMPLATE = "results-CH9energy-%Y%m%d-%H%M%S.csv"


class Ch9CsvCapture(DeviceMonitorFilterBase):
    NAME = "ch9csv"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._partial = ""       # incomplete trailing line between chunks
        self._captured = None    # list of lines while inside a CSV block

    # -- helpers ----------------------------------------------------------

    @property
    def _output_dir(self):
        base = self.project_dir or os.getcwd()
        return os.path.join(base, OUTPUT_SUBDIR)

    def _save(self):
        """Write the captured block and return a note for the monitor."""
        lines = self._captured
        self._captured = None

        if not lines:
            return "[ch9csv] empty CSV block — nothing written"

        try:
            os.makedirs(self._output_dir, exist_ok=True)
            path = os.path.join(
                self._output_dir, datetime.now().strftime(FILENAME_TEMPLATE)
            )
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write("\n".join(lines) + "\n")
        except OSError as exc:  # never take the monitor down over a write error
            return "[ch9csv] FAILED to save CSV: %s" % exc

        data_rows = sum(1 for ln in lines if ln and not ln.startswith("#"))
        return "[ch9csv] saved %d rows (%d lines) -> %s" % (
            max(data_rows - 1, 0),  # minus the header row
            len(lines),
            path,
        )

    def _feed_line(self, line):
        """Consume one complete line; return a note to display, or None."""
        stripped = line.strip()

        if stripped == BEGIN_MARKER:
            if self._captured is not None:
                # A previous block never terminated (reset mid-dump); drop it.
                self._captured = None
                return "[ch9csv] new CSV block started — previous one discarded"
            self._captured = []
            return None

        if self._captured is None:
            return None

        if stripped == END_MARKER:
            return self._save()

        self._captured.append(line)
        return None

    # -- DeviceMonitorFilterBase ------------------------------------------

    def rx(self, text):
        self._partial += text
        notes = []

        while "\n" in self._partial:
            line, self._partial = self._partial.split("\n", 1)
            note = self._feed_line(line.rstrip("\r"))
            if note:
                notes.append(note)

        if notes:
            text += "\n" + "\n".join(notes) + "\n"
        return text
