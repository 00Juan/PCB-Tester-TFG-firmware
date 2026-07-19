"""Transport layer: how NDJSON lines get to/from the device.

``SerialTransport`` talks to real hardware over USB-CDC (pyserial).
``MockTransport`` (see mock.py) talks to an in-process simulated device.
Both expose the same tiny interface consumed by ``PCBTesterClient``.
"""

from __future__ import annotations

import json
from typing import List, Optional


class Transport:
    """Line-oriented byte transport. Subclasses must be thread-safe for
    one reader thread + one writer thread."""

    def readline(self) -> bytes:
        """Return one line (with or without trailing newline), or b'' on
        timeout. Must not block forever."""
        raise NotImplementedError

    def write(self, data: bytes) -> None:
        raise NotImplementedError

    def close(self) -> None:
        pass


class SerialTransport(Transport):
    """USB-CDC serial transport (pyserial).

    The ESP32-S3 native USB-CDC ignores the baud rate; 115200 is passed for
    compatibility with UART-bridge setups.

    Note: the ESP32-S3 talks over its built-in USB-Serial-JTAG peripheral
    (VID:PID 303a:1001). A close/reopen cycle makes the USB host issue a bus
    reset which, on the device's arduino-esp32 core, used to mask the CDC RX
    interrupt and stop the firmware from receiving commands ("no ack for
    'hello'"). That is fixed on the firmware side (keepRxInterruptArmed in
    src/app.cpp), so the host just opens the port normally here. We keep the
    default DTR/RTS handling, which leaves the running app untouched (it does
    not reset the chip), so reconnecting preserves device state.
    """

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 0.2):
        import serial  # lazy: mock-only usage must not require pyserial

        self.port = port
        self._ser = serial.Serial(port, baudrate, timeout=timeout)

    def readline(self) -> bytes:
        return self._ser.readline()

    def write(self, data: bytes) -> None:
        self._ser.write(data)

    def close(self) -> None:
        try:
            self._ser.close()
        except Exception:
            pass


def discover_ports() -> List[str]:
    """Return serial ports that plausibly belong to an ESP32-S3 (USB modem /
    USB serial devices). Cheap filter — use find_tester() to actually probe."""
    from serial.tools import list_ports

    candidates = []
    for p in list_ports.comports():
        dev = p.device
        if any(tag in dev for tag in ("usbmodem", "usbserial", "ttyACM", "ttyUSB")):
            candidates.append(dev)
    return candidates


def find_tester(timeout: float = 1.5) -> Optional[str]:
    """Probe candidate ports with a "hello" command; return the first port
    where a PCB Tester answers, or None."""
    for port in discover_ports():
        try:
            t = SerialTransport(port, timeout=0.2)
        except Exception:
            continue
        try:
            import time

            # Resend hello periodically: (re)opening the port bus-resets the
            # ESP32-S3 USB-CDC and the first hello can land while its RX is
            # briefly masked (the firmware re-arms it within ~100 ms).
            deadline = time.monotonic() + timeout
            next_hello = 0.0
            while time.monotonic() < deadline:
                if time.monotonic() >= next_hello:
                    t.write(b'{"id":0,"cmd":"hello"}\n')
                    next_hello = time.monotonic() + 0.5
                line = t.readline()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue  # boot prints / partial line
                if msg.get("type") == "ack" and msg.get("name") == "PCB-Tester":
                    return port
        except Exception:
            pass
        finally:
            t.close()
    return None
