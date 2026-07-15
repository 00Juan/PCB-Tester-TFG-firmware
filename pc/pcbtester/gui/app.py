"""GUI entry point.

    pcbtester-gui                 # pick a port in the UI (or tick "Mock device")
    pcbtester-gui --mock          # start connected to the simulated device
    pcbtester-gui --port /dev/cu.usbmodemXXXX

``--selftest`` (used by the test suite, with QT_QPA_PLATFORM=offscreen):
connects to the mock, waits for telemetry to reach the dashboard, prints
SELFTEST OK/FAIL and exits with the matching status code.
"""

from __future__ import annotations

import argparse
import sys


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="pcbtester-gui")
    ap.add_argument("--mock", action="store_true",
                    help="start connected to the simulated device")
    ap.add_argument("--port", help="serial port to connect to at startup")
    ap.add_argument("--selftest", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication

    from .mainwindow import MainWindow

    app = QApplication(sys.argv[:1])
    app.setApplicationName("PCB Tester")
    win = MainWindow()
    win.resize(1150, 640)
    win.show()

    if args.selftest:
        win.mock_check.setChecked(True)
        win._connect(mock=True)

        def request_scope() -> None:
            win.scope_tab.n_spin.setValue(32)
            win.scope_tab.dt_spin.setValue(1)
            win.scope_tab._capture()

        def run_campaign() -> None:
            win._mock.wire(1, 3)
            win.tb_tab.tests = [
                {"name": "st-accuracy", "type": "voltage_accuracy",
                 "params": {"mask": 1, "target_v": 3.0, "tolerance_v": 0.1,
                            "settle_ms": 50, "samples": 4}},
                {"name": "st-wired-sense", "type": "static_voltage",
                 "setup": [{"step": "vs", "ch": 1, "v": 2.5}],
                 "params": {"sense_mask": 4, "expected_v": 2.5,
                            "tolerance_v": 0.2, "settle_ms": 50}},
            ]
            win.tb_tab._refresh_list()
            win.tb_tab._run()

        def check() -> None:
            x = win.scope_tab.curve.xData  # numpy array or None
            scope_pts = 0 if x is None else len(x)
            tb_rows = win.tb_tab.results.rowCount()
            tb_done = win.tb_tab._done_summary or {}
            ok = (win.client is not None and win.telem_count >= 3
                  and win.trends_tab.sample_count >= 3 and scope_pts == 32
                  and tb_rows == 2 and tb_done.get("pass") == 2)
            print("SELFTEST OK" if ok else
                  f"SELFTEST FAIL (client={win.client is not None}, "
                  f"telem_count={win.telem_count}, "
                  f"trends={win.trends_tab.sample_count}, scope={scope_pts}, "
                  f"tb_rows={tb_rows}, tb_done={tb_done})")
            app.exit(0 if ok else 1)

        QTimer.singleShot(500, request_scope)
        QTimer.singleShot(900, run_campaign)
        QTimer.singleShot(3200, check)
    elif args.mock:
        win.mock_check.setChecked(True)
        win._connect(mock=True)
    elif args.port:
        win._connect(mock=False, port=args.port)

    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
