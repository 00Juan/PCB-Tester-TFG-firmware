"""DUT profile model: signal names, macros and indicators (Qt-free).

A profile is a plain dict (like campaigns) persisted as JSON under
pc/profiles/:

    {"format": "pcbtester-dut-profile", "version": 1, "name": "TSAL",
     "channels": {"1": "HV_Accu_State", ...},
     "macros": [{"name": "Power DUT", "color": "#43a047",
                 "actions": [{"cmd": "ch.connect", "ch": 9},
                             {"cmd": "wait", "ms": 300}, ...]}],
     "indicators": [{"name": "Green LED",
                     "states": [{"state": "ON", "color": "#43a047",
                                 "when": [{"ch": 9, "field": "vout",
                                           "op": "<", "value": 2.0}]}],
                     "fallback": {"state": "?", "color": "#9e9e9e"}}]}

Macro actions use the same raw-command schema as campaign "pre"/"post"
lists ({"cmd": "ch.set"/"ch.connect"/…} plus the host-side
{"cmd": "wait", "ms": N}), so any protocol command works.

Indicators are host-side only: each state carries a "when" list of
conditions that must ALL hold on the latest telemetry snapshot; the first
matching state wins, else the "fallback" state is shown.

This module also holds the app-wide signal-name registry so other tabs
(dashboard cards, trends, testbench mask editors) can show the loaded
names without depending on the Operate tab.
"""

from __future__ import annotations

import json
import operator
from typing import Any, Dict, List, Optional, Tuple

PROFILE_FORMAT = "pcbtester-dut-profile"

N_CHANNELS = 11
FIELDS = ("v", "i", "vin", "vout", "conn", "st")
OPS: Dict[str, Any] = {
    "<": operator.lt, ">": operator.gt, "<=": operator.le,
    ">=": operator.ge, "==": operator.eq, "!=": operator.ne,
}
DEFAULT_FALLBACK = {"state": "—", "color": "#9e9e9e"}

# --------------------------------------------------------------------------- #
# App-wide signal-name registry
# --------------------------------------------------------------------------- #

SIGNAL_NAMES: Dict[int, str] = {}


def set_signal_names(names: Dict[int, str]) -> None:
    SIGNAL_NAMES.clear()
    SIGNAL_NAMES.update({int(ch): str(n) for ch, n in names.items() if n})


def signal_label(ch: int) -> str:
    """"CH3 · SDC_END" when a name is registered, else "CH3"."""
    name = SIGNAL_NAMES.get(ch)
    return f"CH{ch} · {name}" if name else f"CH{ch}"


# --------------------------------------------------------------------------- #
# Profile (de)serialization
# --------------------------------------------------------------------------- #

def new_profile(name: str = "DUT") -> dict:
    return {"format": PROFILE_FORMAT, "version": 1, "name": name,
            "channels": {}, "macros": [], "indicators": []}


def load_profile(path: str) -> dict:
    with open(path) as f:
        data = json.load(f)
    if data.get("format") != PROFILE_FORMAT:
        raise ValueError(f"not a DUT profile (format={data.get('format')!r})")
    # JSON keys are strings; normalize channel keys to int
    channels = {}
    for ch, name in (data.get("channels") or {}).items():
        try:
            channels[int(ch)] = str(name)
        except (TypeError, ValueError):
            continue
    data["channels"] = channels
    data.setdefault("macros", [])
    data.setdefault("indicators", [])
    return data


def save_profile(path: str, profile: dict) -> None:
    data = dict(profile)
    data["format"] = PROFILE_FORMAT
    data.setdefault("version", 1)
    data["channels"] = {str(ch): n for ch, n in
                        (profile.get("channels") or {}).items()}
    with open(path, "w") as f:
        json.dump(data, f, indent=2)


# --------------------------------------------------------------------------- #
# Telemetry snapshot + indicator evaluation
# --------------------------------------------------------------------------- #

def telemetry_snapshot(msg: dict) -> Dict[int, dict]:
    """Flatten one telemetry message into {ch: {field: value}}."""
    snap: Dict[int, dict] = {}
    for d in msg.get("lvlp", []):
        ch = d.get("ch")
        if ch:
            snap[ch] = dict(d)
    for d in msg.get("hp", []):
        ch = d.get("ch")
        if ch:
            snap[ch] = dict(d, mode="RELAY")
    for d in msg.get("hv", []):
        ch = d.get("ch")
        if ch:
            snap[ch] = dict(d, mode="RELAY")
    return snap


def _condition_holds(cond: dict, snap: Dict[int, dict]) -> bool:
    ch_data = snap.get(int(cond.get("ch", 0)))
    if ch_data is None:
        return False
    value = ch_data.get(cond.get("field"))
    op = OPS.get(cond.get("op"))
    if value is None or op is None:
        return False
    try:
        return bool(op(float(value), float(cond.get("value", 0))))
    except (TypeError, ValueError):
        return False


def evaluate_indicator(ind: dict, snap: Dict[int, dict]) -> Tuple[str, str]:
    """Return (state, color) for an indicator against a telemetry snapshot.

    First state whose conditions all hold wins; an empty "when" list never
    matches (use "fallback" for the default state).
    """
    for state in ind.get("states", []):
        when = state.get("when", [])
        if when and all(_condition_holds(c, snap) for c in when):
            return (state.get("state", "?"),
                    state.get("color", DEFAULT_FALLBACK["color"]))
    fb = ind.get("fallback") or DEFAULT_FALLBACK
    return (fb.get("state", DEFAULT_FALLBACK["state"]),
            fb.get("color", DEFAULT_FALLBACK["color"]))


# --------------------------------------------------------------------------- #
# Capture current state as macro actions
# --------------------------------------------------------------------------- #

def capture_actions(snap: Dict[int, dict]) -> List[dict]:
    """Build macro actions reproducing the state seen in a telemetry snapshot.

    LVLP channels become ch.set with the reported mode/values (VS/CS use the
    *measured* value — round-tripped, so verify before relying on it for
    tight setpoints); relay channels become ch.connect / ch.disconnect.
    """
    actions: List[dict] = []
    for ch in sorted(snap):
        d = snap[ch]
        mode = d.get("mode", "HZ")
        if mode == "RELAY":
            cmd = "ch.connect" if d.get("conn") else "ch.disconnect"
            actions.append({"cmd": cmd, "ch": ch})
        elif mode == "VS":
            actions.append({"cmd": "ch.set", "ch": ch, "mode": "VS",
                            "v": round(float(d.get("v", 0.0)), 2)})
        elif mode in ("CS", "RL"):
            actions.append({"cmd": "ch.set", "ch": ch, "mode": mode,
                            "i": round(float(d.get("i", 0.0)), 4)})
        elif mode == "PWM":
            actions.append({"cmd": "ch.set", "ch": ch, "mode": "PWM",
                            "duty": int(d.get("duty", 0)),
                            "freq": int(d.get("freq", 1000)),
                            "res": int(d.get("res", 8))})
        else:
            actions.append({"cmd": "ch.set", "ch": ch, "mode": "HZ"})
    return actions


# --------------------------------------------------------------------------- #
# Display helpers (shared by editors / list views)
# --------------------------------------------------------------------------- #

def action_summary(a: dict) -> str:
    cmd = a.get("cmd", "?")
    ch = a.get("ch")
    if cmd == "wait":
        return f'wait {a.get("ms", 0)} ms'
    if cmd == "ch.set":
        mode = a.get("mode", "?")
        detail = ""
        if mode == "VS":
            detail = f' {a.get("v", 0)} V'
        elif mode in ("CS", "RL"):
            detail = f' {a.get("i", 0)} A'
        elif mode == "PWM":
            detail = f' duty {a.get("duty", 0)} @ {a.get("freq", 0)} Hz'
        return f"{signal_label(ch)} → {mode}{detail}"
    if cmd == "ch.connect":
        return f"{signal_label(ch)} → connect"
    if cmd == "ch.disconnect":
        return f"{signal_label(ch)} → disconnect"
    if cmd == "ch.limits":
        return (f'{signal_label(ch)} limits vmax={a.get("vmax", "?")}'
                + (f' imax={a["imax"]}' if a.get("imax") is not None else ""))
    return json.dumps(a)


def condition_summary(c: dict) -> str:
    return (f'{signal_label(int(c.get("ch", 0)))}.{c.get("field", "?")} '
            f'{c.get("op", "?")} {c.get("value", "?")}')
