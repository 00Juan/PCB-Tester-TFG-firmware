"""MockTester: an in-process simulation of the PCB Tester firmware.

Speaks exactly the same NDJSON protocol as lib/Protocol on the ESP32
(same commands, ack/error codes, telemetry shape, fault latching), so the
client library, tests and the future GUI can run without hardware.

Extra simulation hooks (not part of the real protocol):
  - ``mock.set_load(ch, ohms)``      — attach a resistive load to an LVLP channel
  - ``mock.set_hp_load(n, ohms)``    — load on HP channel n (1 or 2)
  - ``mock.set_hv_supply(volts)``    — voltage present at the HV input
"""

from __future__ import annotations

import json
import queue
import random
import threading
import time
from dataclasses import dataclass
from typing import Any, Dict, List, Optional

from .transport import Transport

FW_VERSION = "0.1.0"
PROTO_VERSION = 1

# LVLP status codes (mirror LVLPStatus)
ST_NORMAL, ST_OVERCURRENT, ST_OVERVOLTAGE, ST_OTHER = 0, 1, 2, 3
# HV status codes (mirror HVChannelStatus)
HV_ST_NORMAL, HV_ST_OVERVOLTAGE, HV_ST_OTHER = 0, 1, 2

_FAULT_STR = {ST_OVERCURRENT: "OVERCURRENT", ST_OVERVOLTAGE: "OVERVOLTAGE",
              ST_OTHER: "OTHER"}

_MODES = ("HZ", "VS", "CS", "RL", "PWM")
_PWM_CHANNELS = (1, 2, 3, 4)
_LVLP_RAIL_V = 11.0


def _noise(sigma: float = 0.002) -> float:
    return random.gauss(0.0, sigma)


@dataclass
class _Lvlp:
    ch: int
    mode: str = "HZ"
    vt: float = 0.0
    it: float = 0.0
    v: float = 0.0
    i: float = 0.0
    st: int = ST_NORMAL
    conn: bool = False
    vmax: float = 12.0
    imax: float = 0.5
    duty: int = 0
    freq: int = 0
    res: int = 8                       # LEDC resolution bits (1-14)
    load_ohms: Optional[float] = None  # simulation hook


@dataclass
class _Hp:
    ch: int
    st: int = ST_NORMAL
    conn: bool = False
    vin: float = 12.0
    vout: float = 0.0
    i: float = 0.0
    vmax: float = 15.0
    imax: float = 10.0
    load_ohms: Optional[float] = None


@dataclass
class _Hv:
    ch: int
    st: int = HV_ST_NORMAL
    conn: bool = False
    v: float = 0.0
    vmax: float = 70.0
    supply_v: float = 48.0
    cal_valid: bool = True


class MockTransport(Transport):
    """Client-side handle onto a MockTester (get one via mock.transport())."""

    def __init__(self, mock: "MockTester"):
        self._mock = mock

    def readline(self) -> bytes:
        try:
            return self._mock._out.get(timeout=0.1)
        except queue.Empty:
            return b""

    def write(self, data: bytes) -> None:
        for line in data.decode(errors="replace").splitlines():
            if line.strip():
                self._mock.write_line(line)

    def close(self) -> None:
        self._mock.stop()


class MockTester:
    N_LVLP, N_HP, N_HV = 8, 2, 1

    def __init__(self, telem_hz: int = 10, sim_hz: int = 50):
        self.lvlp = [_Lvlp(ch=i + 1) for i in range(self.N_LVLP)]
        self.hp = [_Hp(ch=self.N_LVLP + i + 1) for i in range(self.N_HP)]
        self.hv = [_Hv(ch=self.N_LVLP + self.N_HP + i + 1) for i in range(self.N_HV)]
        self.estop_latched = False
        self.telem_hz = telem_hz
        self.cal = self._default_cal()   # in-"RAM" calibration per channel
        self._nvs: Dict[int, dict] = {}  # persisted by cal.save

        self._out: "queue.Queue[bytes]" = queue.Queue(maxsize=10000)
        self._lock = threading.RLock()
        self._sim_period = 1.0 / sim_hz
        self._running = True
        self._sim_thread = threading.Thread(target=self._sim_loop, daemon=True,
                                            name="mock-tester-sim")
        self._sim_thread.start()

    def transport(self) -> MockTransport:
        return MockTransport(self)

    def _default_cal(self) -> Dict[int, dict]:
        cal: Dict[int, dict] = {}
        for i in range(self.N_LVLP):
            cal[i + 1] = {"K1": 4.922409058, "K2": -3.926402569,
                          "offset": 0.21726886, "mADC": 0.005097771,
                          "bADC": -0.030085115, "mDAC": 819.581665039,
                          "bDAC": -2.384184361}
        for i in range(self.N_HP):
            cal[self.N_LVLP + i + 1] = {
                "mADC_VIn": 0.005075289, "bADC_VIn": 0.015266559,
                "mADC_VOut": 0.005192232, "bADC_VOut": 0.00862406,
                "sens": 0.132556796, "vref": 3.27, "zero_adc": 2048}
        cal[self.N_LVLP + self.N_HP + 1] = {
            "deadzone": 100, "vref": 3.27,
            "points": [[100, 25.23], [118, 30.16], [176, 44.37], [243, 61.13]]}
        return cal

    def stop(self) -> None:
        self._running = False

    # ------------------------------------------------------------------ #
    # Simulation hooks (test/GUI-dev conveniences, not protocol)
    # ------------------------------------------------------------------ #

    def set_load(self, ch: int, ohms: Optional[float]) -> None:
        with self._lock:
            self.lvlp[ch - 1].load_ohms = ohms

    def set_hp_load(self, n: int, ohms: Optional[float]) -> None:
        with self._lock:
            self.hp[n - 1].load_ohms = ohms

    def set_hv_supply(self, volts: float) -> None:
        with self._lock:
            self.hv[0].supply_v = volts

    # ------------------------------------------------------------------ #
    # Device output
    # ------------------------------------------------------------------ #

    def _emit(self, msg: Dict[str, Any]) -> None:
        try:
            self._out.put_nowait(json.dumps(msg).encode() + b"\n")
        except queue.Full:
            pass

    # ------------------------------------------------------------------ #
    # Simulation loop: measurements, limit checks, faults, telemetry
    # ------------------------------------------------------------------ #

    def _sim_loop(self) -> None:
        last_telem = 0.0
        while self._running:
            time.sleep(self._sim_period)
            with self._lock:
                self._tick()
                now = time.monotonic()
                if self.telem_hz > 0 and now - last_telem >= 1.0 / self.telem_hz:
                    last_telem = now
                    self._emit(self._telemetry())

    def _tick(self) -> None:
        for c in self.lvlp:
            if c.st != ST_NORMAL or not c.conn:
                c.v = _noise()
                c.i = 0.0
                continue
            if c.mode == "VS":
                c.v = c.vt + _noise()
                c.i = (c.v / c.load_ohms if c.load_ohms else 0.0) + _noise(0.0005)
            elif c.mode in ("CS", "RL"):
                if c.load_ohms:
                    c.i = c.it + _noise(0.0005)
                    c.v = min(c.it * c.load_ohms, _LVLP_RAIL_V) + _noise()
                else:  # current source into open circuit rails out
                    c.i = _noise(0.0005)
                    c.v = _LVLP_RAIL_V + _noise()
            elif c.mode == "PWM":
                # Average DC of the PWM output: amplitude (vt) x duty ratio
                c.v = c.vt * c.duty / ((1 << c.res) - 1) + _noise()
                c.i = _noise(0.0005)
            # Limit checks (mirror LVLPChannel::checkLimits)
            if c.v > c.vmax:
                self._trip_lvlp(c, ST_OVERVOLTAGE)
            elif abs(c.i) > c.imax:
                self._trip_lvlp(c, ST_OVERCURRENT)

        for c in self.hp:
            c.vin = 12.0 + _noise(0.01)
            if c.st != ST_NORMAL or not c.conn:
                c.vout = _noise(0.01)
                c.i = 0.0
                continue
            c.vout = c.vin - 0.05 + _noise(0.01)
            c.i = (c.vout / c.load_ohms if c.load_ohms else 0.0) + _noise(0.005)
            if c.vout > c.vmax:
                self._trip_hp(c, ST_OVERVOLTAGE)
            elif abs(c.i) > c.imax:
                self._trip_hp(c, ST_OVERCURRENT)

        for c in self.hv:
            c.v = (c.supply_v + _noise(0.05)) if (c.conn and c.st == HV_ST_NORMAL) else 0.0
            if c.st == HV_ST_NORMAL and c.v > c.vmax:
                c.conn = False
                c.st = HV_ST_OVERVOLTAGE
                self._emit({"type": "fault", "ch": c.ch, "code": "OVERVOLTAGE",
                            "v": round(c.v, 3)})
                c.v = 0.0

    def _trip_lvlp(self, c: _Lvlp, st: int) -> None:
        fault_v, fault_i = c.v, c.i
        c.st = st
        c.mode = "HZ"
        c.conn = False
        self._emit({"type": "fault", "ch": c.ch, "code": _FAULT_STR[st],
                    "v": round(fault_v, 3), "i": round(fault_i, 4)})

    def _trip_hp(self, c: _Hp, st: int) -> None:
        c.st = st
        c.conn = False
        self._emit({"type": "fault", "ch": c.ch, "code": _FAULT_STR[st],
                    "vin": round(c.vin, 3), "vout": round(c.vout, 3),
                    "i": round(c.i, 4)})

    def _telemetry(self) -> Dict[str, Any]:
        msg: Dict[str, Any] = {
            "type": "telem",
            "t": int(time.monotonic() * 1000) & 0xFFFFFFFF,
            "estop": self.estop_latched,
        }
        lv: List[Dict[str, Any]] = []
        for c in self.lvlp:
            o = {"ch": c.ch, "mode": c.mode, "st": c.st, "conn": c.conn,
                 "vt": round(c.vt, 3), "it": round(c.it, 4),
                 "v": round(c.v, 3), "i": round(c.i, 4)}
            if c.mode == "PWM":
                o["duty"] = c.duty
                o["freq"] = c.freq
                o["res"] = c.res
            lv.append(o)
        msg["lvlp"] = lv
        msg["hp"] = [{"ch": c.ch, "st": c.st, "conn": c.conn,
                      "vin": round(c.vin, 3), "vout": round(c.vout, 3),
                      "i": round(c.i, 4)} for c in self.hp]
        msg["hv"] = [{"ch": c.ch, "st": c.st, "conn": c.conn,
                      "v": round(c.v, 3)} for c in self.hv]
        return msg

    # ------------------------------------------------------------------ #
    # Command handling (mirrors TesterProtocol::handleLine)
    # ------------------------------------------------------------------ #

    def write_line(self, line: str) -> None:
        with self._lock:
            self._handle(line)

    def _ack(self, msg_id: Optional[int], ok: bool, err: Optional[str] = None,
             msg: Optional[str] = None, **extra: Any) -> None:
        ack: Dict[str, Any] = {"type": "ack"}
        if msg_id is not None and msg_id >= 0:
            ack["id"] = msg_id
        ack["ok"] = ok
        if err:
            ack["err"] = err
        if msg:
            ack["msg"] = msg
        ack.update(extra)
        self._emit(ack)

    def _handle(self, line: str) -> None:
        try:
            doc = json.loads(line)
            if not isinstance(doc, dict):
                raise ValueError("not an object")
        except ValueError as e:
            self._ack(None, False, "E_PARSE", str(e))
            return

        msg_id = doc.get("id", -1)
        cmd = doc.get("cmd")
        if not isinstance(cmd, str):
            self._ack(msg_id, False, "E_CMD", "missing cmd")
            return

        if cmd == "hello":
            self._ack(msg_id, True, name="PCB-Tester", fw=FW_VERSION,
                      proto=PROTO_VERSION, lvlp=self.N_LVLP, hp=self.N_HP,
                      hv=self.N_HV, hv_cal=self.hv[0].cal_valid,
                      telem_hz=self.telem_hz, estop=self.estop_latched,
                      mock=True)
            return

        if cmd == "estop":
            self._do_estop("cmd")
            self._ack(msg_id, True)
            return

        if cmd == "estop.clear":
            self.estop_latched = False
            self._ack(msg_id, True)
            return

        if cmd == "telem.rate":
            hz = doc.get("hz")
            if not isinstance(hz, int) or isinstance(hz, bool):
                self._ack(msg_id, False, "E_ARG", "missing hz")
                return
            if hz < 0 or hz > 50:
                self._ack(msg_id, False, "E_ARG", "hz must be 0-50")
                return
            self.telem_hz = hz
            self._ack(msg_id, True, telem_hz=hz)
            return

        if cmd.startswith("cal."):
            self._handle_cal(msg_id, cmd, doc)
            return

        if cmd.startswith("ch."):
            self._handle_ch(msg_id, cmd, doc)
            return

        self._ack(msg_id, False, "E_CMD", "unknown cmd")

    def _handle_cal(self, msg_id: int, cmd: str, doc: Dict[str, Any]) -> None:
        if cmd == "cal.save":
            self._nvs = {ch: dict(c) for ch, c in self.cal.items()}
            self._ack(msg_id, True)
            return
        if cmd == "cal.load":
            loaded = 0
            for ch, c in self._nvs.items():
                self.cal[ch] = dict(c)
                loaded += 1
            self._ack(msg_id, True, loaded=loaded)
            return
        if cmd in ("cal.get", "cal.set"):
            ch = doc.get("ch", 0)
            if not isinstance(ch, int) or ch not in self.cal:
                self._ack(msg_id, False, "E_ARG", "ch out of range")
                return
            if cmd == "cal.get":
                self._ack(msg_id, True, ch=ch, cal=self.cal[ch])
            else:
                incoming = doc.get("cal")
                if not isinstance(incoming, dict):
                    self._ack(msg_id, False, "E_ARG", "missing cal object")
                    return
                # Merge semantics, like the firmware
                for k, v in incoming.items():
                    if k in self.cal[ch]:
                        self.cal[ch][k] = v
                if "points" in self.cal[ch]:
                    self.cal[ch]["points"].sort(key=lambda p: p[0])
                self._ack(msg_id, True)
            return
        self._ack(msg_id, False, "E_CMD", "unknown cmd")

    def _handle_ch(self, msg_id: int, cmd: str, doc: Dict[str, Any]) -> None:
        ch = doc.get("ch", 0)
        ch_max = self.N_LVLP + self.N_HP + self.N_HV
        if not isinstance(ch, int) or ch < 1 or ch > ch_max:
            self._ack(msg_id, False, "E_ARG", "ch out of range")
            return
        lc = self.lvlp[ch - 1] if ch <= self.N_LVLP else None
        hc = (self.hp[ch - 1 - self.N_LVLP]
              if lc is None and ch <= self.N_LVLP + self.N_HP else None)
        vc = self.hv[ch - 1 - self.N_LVLP - self.N_HP] if lc is None and hc is None else None

        def _num(key: str) -> Optional[float]:
            val = doc.get(key)
            return float(val) if isinstance(val, (int, float)) and not isinstance(val, bool) else None

        if cmd == "ch.set":
            if lc is None:
                self._ack(msg_id, False, "E_ARG",
                          "ch.set only valid for LVLP channels (1-8)")
                return
            mode = doc.get("mode")
            if mode not in _MODES:
                self._ack(msg_id, False, "E_ARG", "mode must be HZ|VS|CS|RL|PWM")
                return
            if mode == "VS" and _num("v") is None:
                self._ack(msg_id, False, "E_ARG", "VS needs v")
                return
            if mode in ("CS", "RL") and _num("i") is None:
                self._ack(msg_id, False, "E_ARG", "CS/RL needs i")
                return
            if mode == "PWM":
                if (not isinstance(doc.get("duty"), int)
                        or not isinstance(doc.get("freq"), int)):
                    self._ack(msg_id, False, "E_ARG", "PWM needs duty and freq")
                    return
                pwm_res = doc.get("res", lc.res)
                if not isinstance(pwm_res, int) or pwm_res < 1 or pwm_res > 14:
                    self._ack(msg_id, False, "E_ARG", "res must be 1-14 bits")
                    return
                if doc["duty"] > (1 << pwm_res) - 1:
                    self._ack(msg_id, False, "E_ARG", "duty exceeds resolution max")
                    return
            # setMode guard: fault latch, PWM capability
            if lc.st != ST_NORMAL and mode != "HZ":
                self._ack(msg_id, False, "E_STATE",
                          "channel faulted, send ch.reset first")
                return
            if mode == "PWM" and lc.ch not in _PWM_CHANNELS:
                self._ack(msg_id, False, "E_STATE",
                          "mode not supported on this channel")
                return

            lc.mode = mode
            lc.conn = mode != "HZ" and lc.st == ST_NORMAL
            if mode == "VS":
                lc.vt = _num("v")  # type: ignore[assignment]
            elif mode in ("CS", "RL"):
                lc.it = _num("i")  # type: ignore[assignment]
            elif mode == "PWM":
                lc.duty = doc["duty"]
                lc.freq = doc["freq"]
                lc.res = doc.get("res", lc.res)
                v = _num("v")
                if v is not None:
                    lc.vt = v
            else:  # HZ parks the DAC at 0 V
                lc.vt = 0.0
            self._ack(msg_id, True)
            return

        if cmd in ("ch.connect", "ch.disconnect"):
            if lc is not None:
                self._ack(msg_id, False, "E_ARG",
                          "LVLP connection is managed via ch.set mode")
                return
            target = hc if hc is not None else vc
            if cmd == "ch.connect":
                if target.st != 0:
                    self._ack(msg_id, False, "E_STATE",
                              "channel faulted, send ch.reset first")
                    return
                target.conn = True
            else:
                target.conn = False
            self._ack(msg_id, True)
            return

        if cmd == "ch.limits":
            vmax = _num("vmax")
            if vmax is None:
                self._ack(msg_id, False, "E_ARG", "missing vmax")
                return
            if vc is not None:
                vc.vmax = vmax
            else:
                imax = _num("imax")
                if imax is None:
                    self._ack(msg_id, False, "E_ARG", "missing imax")
                    return
                target = lc if lc is not None else hc
                target.vmax = vmax
                target.imax = imax
            self._ack(msg_id, True)
            return

        if cmd == "ch.capture":
            n = doc.get("n", 128)
            dt = doc.get("dt_ms", 2)
            if (not isinstance(n, int) or not isinstance(dt, int)
                    or n < 2 or n > 512 or dt < 1 or dt > 100 or n * dt > 5000):
                self._ack(msg_id, False, "E_ARG",
                          "need n 2-512, dt_ms 1-100, n*dt <= 5000 ms")
                return
            self._ack(msg_id, True, n=n, dt_ms=dt)
            # Synthetic waveform around the channel's current level:
            # noise + a small ripple so the scope view shows structure.
            import math

            if lc is not None:
                base = lc.v
            elif hc is not None:
                base = hc.vout
            else:
                base = vc.v
            samples = [round(base + 0.02 * math.sin(2 * math.pi * k / 25.0)
                             + _noise(), 3) for k in range(n)]
            self._emit({"type": "capture", "kind": "scope", "ch": ch,
                        "dt_ms": dt, "unit": "V", "samples": samples})
            return

        if cmd == "ch.reset":
            if lc is not None:
                lc.st = ST_NORMAL
                lc.mode = "HZ"
                lc.conn = False
                lc.vt = 0.0
            elif hc is not None:
                hc.st = ST_NORMAL
                hc.conn = False
            else:
                vc.st = HV_ST_NORMAL
                vc.conn = False
            self._ack(msg_id, True)
            return

        self._ack(msg_id, False, "E_CMD", "unknown cmd")

    def _do_estop(self, source: str) -> None:
        for c in self.lvlp:
            c.mode = "HZ"
            c.conn = False
            c.vt = 0.0
        for c in self.hp:
            c.conn = False
        for c in self.hv:
            c.conn = False
        self.estop_latched = True
        self._emit({"type": "estop", "src": source})

    def press_estop_button(self) -> None:
        """Simulate the physical encoder-button E-stop."""
        with self._lock:
            self._do_estop("button")
