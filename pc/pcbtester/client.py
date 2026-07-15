"""PCBTesterClient: thread-safe client for the tester's NDJSON protocol.

A background reader thread consumes lines from the transport:
  - "ack" messages resolve the pending command that carries the same id;
  - every other message (telem / fault / estop / log) is an *event*: it is
    pushed to ``client.events`` (a Queue) and forwarded to the matching
    ``on_*`` callback if one is set.

Callbacks run on the reader thread — keep them short (GUI code should just
re-post to its own event loop, e.g. via Qt signals).
"""

from __future__ import annotations

import itertools
import json
import queue
import threading
from typing import Any, Callable, Dict, Optional

from .transport import Transport

CommandCallback = Callable[[Dict[str, Any]], None]

DEFAULT_TIMEOUT = 2.0


class CommandError(Exception):
    """The device answered ok:false."""

    def __init__(self, ack: Dict[str, Any]):
        self.ack = ack
        self.err = ack.get("err", "E_UNKNOWN")
        self.msg = ack.get("msg", "")
        super().__init__(f"{self.err}: {self.msg}")


class ProtocolTimeout(Exception):
    """No ack arrived within the timeout."""


class _Pending:
    __slots__ = ("event", "response")

    def __init__(self) -> None:
        self.event = threading.Event()
        self.response: Optional[Dict[str, Any]] = None


class PCBTesterClient:
    def __init__(self, transport: Transport, event_queue_size: int = 2000):
        self._transport = transport
        self._ids = itertools.count(1)
        self._pending: Dict[int, _Pending] = {}
        self._pending_lock = threading.Lock()
        self._write_lock = threading.Lock()
        self._reader: Optional[threading.Thread] = None
        self._running = False

        #: All non-ack messages, in arrival order (dicts). Full-queue drops
        #: the oldest entry so a slow consumer can't wedge the reader.
        self.events: "queue.Queue[Dict[str, Any]]" = queue.Queue(maxsize=event_queue_size)

        # Optional per-type callbacks (called on the reader thread)
        self.on_telemetry: Optional[CommandCallback] = None
        self.on_fault: Optional[CommandCallback] = None
        self.on_estop: Optional[CommandCallback] = None
        self.on_log: Optional[CommandCallback] = None
        self.on_capture: Optional[CommandCallback] = None
        #: Called with raw text for lines that are not JSON (boot prints etc.)
        self.on_raw: Optional[Callable[[str], None]] = None

    # ------------------------------------------------------------------ #
    # Lifecycle
    # ------------------------------------------------------------------ #

    def start(self) -> "PCBTesterClient":
        if self._running:
            return self
        self._running = True
        self._reader = threading.Thread(target=self._read_loop, daemon=True,
                                        name="pcbtester-reader")
        self._reader.start()
        return self

    def close(self) -> None:
        self._running = False
        self._transport.close()
        if self._reader and self._reader.is_alive():
            self._reader.join(timeout=1.0)

    def __enter__(self) -> "PCBTesterClient":
        return self.start()

    def __exit__(self, *exc: Any) -> None:
        self.close()

    # ------------------------------------------------------------------ #
    # Generic command
    # ------------------------------------------------------------------ #

    def command(self, cmd: str, timeout: float = DEFAULT_TIMEOUT,
                **params: Any) -> Dict[str, Any]:
        """Send a command and block until its ack. Returns the ack dict.

        Raises CommandError on ok:false and ProtocolTimeout on no answer.
        """
        if not self._running:
            raise RuntimeError("client not started — call start()")
        msg_id = next(self._ids)
        msg = {"id": msg_id, "cmd": cmd}
        msg.update({k: v for k, v in params.items() if v is not None})

        pending = _Pending()
        with self._pending_lock:
            self._pending[msg_id] = pending
        try:
            self.send_raw(json.dumps(msg))
            if not pending.event.wait(timeout):
                raise ProtocolTimeout(f"no ack for {cmd!r} (id={msg_id}) "
                                      f"within {timeout}s")
        finally:
            with self._pending_lock:
                self._pending.pop(msg_id, None)

        ack = pending.response or {}
        if not ack.get("ok", False):
            raise CommandError(ack)
        return ack

    def send_raw(self, line: str) -> None:
        """Write one raw line to the device (no ack correlation)."""
        with self._write_lock:
            self._transport.write(line.encode() + b"\n")

    # ------------------------------------------------------------------ #
    # Typed commands (mirror lib/Protocol on the firmware side)
    # ------------------------------------------------------------------ #

    def hello(self, **kw: Any) -> Dict[str, Any]:
        return self.command("hello", **kw)

    def set_channel(self, ch: int, mode: str, v: Optional[float] = None,
                    i: Optional[float] = None, duty: Optional[int] = None,
                    freq: Optional[int] = None, res: Optional[int] = None,
                    **kw: Any) -> Dict[str, Any]:
        """LVLP channels (1-8). mode: HZ | VS | CS | RL | PWM.

        For PWM, v sets the high-level amplitude (volts) and res the LEDC
        resolution in bits (1-14); duty must fit the effective resolution.
        """
        return self.command("ch.set", ch=ch, mode=mode, v=v, i=i,
                            duty=duty, freq=freq, res=res, **kw)

    def connect_channel(self, ch: int, **kw: Any) -> Dict[str, Any]:
        """Close the relay of an HP/HV channel (9-11)."""
        return self.command("ch.connect", ch=ch, **kw)

    def disconnect_channel(self, ch: int, **kw: Any) -> Dict[str, Any]:
        return self.command("ch.disconnect", ch=ch, **kw)

    def set_limits(self, ch: int, vmax: float,
                   imax: Optional[float] = None, **kw: Any) -> Dict[str, Any]:
        return self.command("ch.limits", ch=ch, vmax=vmax, imax=imax, **kw)

    def reset_channel(self, ch: int, **kw: Any) -> Dict[str, Any]:
        """Clear a latched fault. The channel is left disconnected."""
        return self.command("ch.reset", ch=ch, **kw)

    def set_telemetry_rate(self, hz: int, **kw: Any) -> Dict[str, Any]:
        return self.command("telem.rate", hz=hz, **kw)

    def estop(self, **kw: Any) -> Dict[str, Any]:
        return self.command("estop", **kw)

    def clear_estop(self, **kw: Any) -> Dict[str, Any]:
        """Dismiss the E-stop latch indicator (channels still need reset_channel)."""
        return self.command("estop.clear", **kw)

    def capture(self, ch: int, n: int = 128, dt_ms: int = 2) -> Dict[str, Any]:
        """Blocking voltage burst capture; returns the "capture" event dict
        ({"ch","dt_ms","unit","samples":[...]}).

        Note: pops from the shared event queue — in GUI code, prefer sending
        the command and handling the on_capture callback instead.
        """
        self.command("ch.capture", ch=ch, n=n, dt_ms=dt_ms)
        deadline_extra = n * dt_ms / 1000.0 + DEFAULT_TIMEOUT
        while True:
            ev = self.next_event("capture", timeout=deadline_extra)
            if ev.get("ch") == ch:
                return ev

    # ------------------------------------------------------------------ #
    # Calibration
    # ------------------------------------------------------------------ #

    def get_calibration(self, ch: int, **kw: Any) -> Dict[str, Any]:
        """Return the channel's calibration data as a dict."""
        return self.command("cal.get", ch=ch, **kw)["cal"]

    def set_calibration(self, ch: int, cal: Dict[str, Any], **kw: Any) -> Dict[str, Any]:
        """Merge calibration fields into the channel (RAM only until save)."""
        return self.command("cal.set", ch=ch, cal=cal, **kw)

    def save_calibration(self, **kw: Any) -> Dict[str, Any]:
        """Persist every channel's calibration to the device's NVS flash."""
        return self.command("cal.save", **kw)

    def load_calibration(self, **kw: Any) -> Dict[str, Any]:
        """Reload every channel's calibration from NVS (ack has "loaded")."""
        return self.command("cal.load", **kw)

    # ------------------------------------------------------------------ #
    # Event helpers
    # ------------------------------------------------------------------ #

    def next_event(self, type_: Optional[str] = None,
                   timeout: float = DEFAULT_TIMEOUT) -> Dict[str, Any]:
        """Pop events until one of the given type arrives (or any event when
        type_ is None). Raises queue.Empty on timeout."""
        import time

        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise queue.Empty(f"no {type_ or 'event'} within {timeout}s")
            ev = self.events.get(timeout=remaining)
            if type_ is None or ev.get("type") == type_:
                return ev

    def drain_events(self) -> None:
        while True:
            try:
                self.events.get_nowait()
            except queue.Empty:
                return

    # ------------------------------------------------------------------ #
    # Reader thread
    # ------------------------------------------------------------------ #

    def _read_loop(self) -> None:
        while self._running:
            try:
                raw = self._transport.readline()
            except Exception:
                if self._running:
                    self._dispatch_event({"type": "log", "lvl": "error",
                                          "msg": "transport read failed"})
                return
            if not raw:
                continue
            text = raw.decode(errors="replace").strip()
            if not text:
                continue
            try:
                msg = json.loads(text)
            except ValueError:
                if self.on_raw:
                    self.on_raw(text)
                continue
            if not isinstance(msg, dict):
                continue

            if msg.get("type") == "ack" and "id" in msg:
                with self._pending_lock:
                    pending = self._pending.get(msg["id"])
                if pending is not None:
                    pending.response = msg
                    pending.event.set()
                    continue
            self._dispatch_event(msg)

    def _dispatch_event(self, msg: Dict[str, Any]) -> None:
        try:
            self.events.put_nowait(msg)
        except queue.Full:
            try:
                self.events.get_nowait()  # drop oldest
            except queue.Empty:
                pass
            try:
                self.events.put_nowait(msg)
            except queue.Full:
                pass

        callback = {
            "telem": self.on_telemetry,
            "fault": self.on_fault,
            "estop": self.on_estop,
            "log": self.on_log,
            "capture": self.on_capture,
        }.get(msg.get("type"))
        if callback:
            try:
                callback(msg)
            except Exception:
                pass  # user callback errors must not kill the reader
