"""Bridge between the client's reader thread and the Qt main thread.

PCBTesterClient invokes its callbacks on its own reader thread; Qt widgets
must only be touched from the main thread. Emitting a Qt signal from a
foreign thread is safe — Qt queues it onto the receiver's thread — so the
bridge simply forwards each callback into a signal.
"""

from __future__ import annotations

from PySide6.QtCore import QObject, Signal

from ..client import PCBTesterClient


class ClientBridge(QObject):
    telemetry = Signal(object)  # dict
    fault = Signal(object)      # dict
    estop = Signal(object)      # dict
    log = Signal(object)        # dict
    capture = Signal(object)      # dict
    tb_progress = Signal(object)  # dict
    tb_result = Signal(object)    # dict
    tb_done = Signal(object)      # dict

    def attach(self, client: PCBTesterClient) -> None:
        client.on_telemetry = self.telemetry.emit
        client.on_fault = self.fault.emit
        client.on_estop = self.estop.emit
        client.on_log = self.log.emit
        client.on_capture = self.capture.emit
        client.on_tb_progress = self.tb_progress.emit
        client.on_tb_result = self.tb_result.emit
        client.on_tb_done = self.tb_done.emit

    def detach(self, client: PCBTesterClient) -> None:
        client.on_telemetry = None
        client.on_fault = None
        client.on_estop = None
        client.on_log = None
        client.on_capture = None
        client.on_tb_progress = None
        client.on_tb_result = None
        client.on_tb_done = None
