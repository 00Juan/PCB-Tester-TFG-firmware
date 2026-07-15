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
            t.write(b'{"id":0,"cmd":"hello"}\n')
            import time

            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
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
