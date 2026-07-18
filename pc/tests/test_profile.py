"""Unit tests for the DUT profile model (gui/profile.py — Qt-free)."""

import json

import pytest

from pcbtester.gui import profile as prof

TELEM = {
    "type": "telem",
    "lvlp": [
        {"ch": 1, "mode": "VS", "v": 5.02, "i": 0.0101, "st": 0, "conn": True},
        {"ch": 2, "mode": "HZ", "v": 0.0, "i": 0.0, "st": 0, "conn": False},
        {"ch": 3, "mode": "PWM", "v": 1.6, "i": 0.0, "st": 0, "conn": True,
         "duty": 128, "freq": 1000, "res": 8},
        {"ch": 4, "mode": "CS", "v": 2.0, "i": 0.02, "st": 0, "conn": True},
    ],
    "hp": [
        {"ch": 9, "conn": True, "vin": 12.0, "vout": 0.4, "i": 0.02, "st": 0},
        {"ch": 10, "conn": True, "vin": 12.0, "vout": 11.9, "i": 0.0, "st": 0},
    ],
    "hv": [{"ch": 11, "conn": False, "v": 0.0, "st": 0}],
}


def snapshot():
    return prof.telemetry_snapshot(TELEM)


def test_snapshot_flattens_all_channel_groups():
    snap = snapshot()
    assert set(snap) == {1, 2, 3, 4, 9, 10, 11}
    assert snap[1]["v"] == 5.02
    assert snap[9]["mode"] == "RELAY"
    assert snap[11]["mode"] == "RELAY"


def test_indicator_first_matching_state_wins():
    ind = {
        "name": "Green LED",
        "states": [
            {"state": "ON", "color": "#0f0",
             "when": [{"ch": 9, "field": "vout", "op": "<", "value": 2.0}]},
            {"state": "OFF", "color": "#999",
             "when": [{"ch": 9, "field": "vout", "op": ">", "value": 10.0}]},
        ],
        "fallback": {"state": "?", "color": "#f80"},
    }
    assert prof.evaluate_indicator(ind, snapshot()) == ("ON", "#0f0")


def test_indicator_multi_channel_and_logic():
    ind = {
        "states": [
            {"state": "SAFE", "color": "#0f0",
             "when": [{"ch": 9, "field": "vout", "op": "<", "value": 2.0},
                      {"ch": 10, "field": "vout", "op": ">", "value": 10.0}]},
        ],
    }
    assert prof.evaluate_indicator(ind, snapshot())[0] == "SAFE"
    # Break one condition -> AND fails -> fallback (default)
    bad = snapshot()
    bad[10]["vout"] = 0.3
    state, color = prof.evaluate_indicator(ind, bad)
    assert state == prof.DEFAULT_FALLBACK["state"]


def test_indicator_conn_field_and_fallback():
    ind = {
        "states": [
            {"state": "POWERED", "color": "#0f0",
             "when": [{"ch": 11, "field": "conn", "op": "==", "value": 1}]},
        ],
        "fallback": {"state": "OFF", "color": "#999"},
    }
    assert prof.evaluate_indicator(ind, snapshot()) == ("OFF", "#999")
    on = snapshot()
    on[11]["conn"] = True
    assert prof.evaluate_indicator(ind, on) == ("POWERED", "#0f0")


def test_indicator_missing_channel_or_field_never_matches():
    ind = {"states": [{"state": "X", "color": "#000",
                       "when": [{"ch": 7, "field": "v", "op": ">",
                                 "value": -99}]}]}
    assert prof.evaluate_indicator(ind, snapshot())[0] == \
        prof.DEFAULT_FALLBACK["state"]
    ind2 = {"states": [{"state": "X", "color": "#000",
                        "when": [{"ch": 11, "field": "vout", "op": ">",
                                  "value": -99}]}]}
    assert prof.evaluate_indicator(ind2, snapshot())[0] == \
        prof.DEFAULT_FALLBACK["state"]


def test_empty_when_list_never_matches():
    ind = {"states": [{"state": "X", "color": "#000", "when": []}],
           "fallback": {"state": "FB", "color": "#111"}}
    assert prof.evaluate_indicator(ind, snapshot()) == ("FB", "#111")


def test_capture_actions_reproduce_snapshot():
    actions = prof.capture_actions(snapshot())
    by_ch = {a["ch"]: a for a in actions}
    assert by_ch[1] == {"cmd": "ch.set", "ch": 1, "mode": "VS", "v": 5.02}
    assert by_ch[2] == {"cmd": "ch.set", "ch": 2, "mode": "HZ"}
    assert by_ch[3] == {"cmd": "ch.set", "ch": 3, "mode": "PWM",
                        "duty": 128, "freq": 1000, "res": 8}
    assert by_ch[4] == {"cmd": "ch.set", "ch": 4, "mode": "CS", "i": 0.02}
    assert by_ch[9]["cmd"] == "ch.connect"
    assert by_ch[11]["cmd"] == "ch.disconnect"


def test_profile_round_trip(tmp_path):
    p = prof.new_profile("TSAL")
    p["channels"] = {1: "HV_Accu_State", 9: "GreenLed"}
    p["macros"] = [{"name": "m", "color": "#000",
                    "actions": [{"cmd": "wait", "ms": 10}]}]
    path = tmp_path / "p.json"
    prof.save_profile(str(path), p)
    loaded = prof.load_profile(str(path))
    assert loaded["channels"] == {1: "HV_Accu_State", 9: "GreenLed"}
    assert loaded["macros"] == p["macros"]
    # JSON on disk keeps string keys
    raw = json.loads(path.read_text())
    assert raw["channels"] == {"1": "HV_Accu_State", "9": "GreenLed"}


def test_load_rejects_non_profile(tmp_path):
    path = tmp_path / "c.json"
    path.write_text(json.dumps({"format": "pcbtester-campaign", "tests": []}))
    with pytest.raises(ValueError):
        prof.load_profile(str(path))


def test_shipped_tsal_profile_loads():
    from pathlib import Path

    path = Path(__file__).resolve().parents[1] / "profiles" / "TSAL.json"
    p = prof.load_profile(str(path))
    assert p["channels"][1] == "HV_Accu_State"
    assert len(p["macros"]) == 18
    assert len(p["indicators"]) == 3
    # relay-state macros follow test_TSAL.cpp levels: AUX2 closed=5V/open=0.8V,
    # Coil closed=0V/open=5V, SDC closed=0V/open=5V
    by_name = {m["name"]: m["actions"] for m in p["macros"]}
    assert by_name["AIR+ mech CLOSED"] == [
        {"cmd": "ch.set", "ch": 7, "mode": "VS", "v": 5.0}]
    assert by_name["AIR+ mech OPENED"] == [
        {"cmd": "ch.set", "ch": 7, "mode": "VS", "v": 0.8}]
    assert by_name["AIR+ cmd CLOSE"] == [
        {"cmd": "ch.set", "ch": 8, "mode": "VS", "v": 0.0}]
    assert by_name["SDC closed"] == [
        {"cmd": "ch.set", "ch": 4, "mode": "VS", "v": 0.0}]
    # every macro action carries a cmd; indicators evaluate without error
    for m in p["macros"]:
        assert all("cmd" in a for a in m["actions"])
    for ind in p["indicators"]:
        prof.evaluate_indicator(ind, snapshot())


def test_signal_label_registry():
    prof.set_signal_names({1: "SDC_END"})
    try:
        assert prof.signal_label(1) == "CH1 · SDC_END"
        assert prof.signal_label(2) == "CH2"
    finally:
        prof.set_signal_names({})
