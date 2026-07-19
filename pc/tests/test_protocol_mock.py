"""Protocol conformance tests, run against the in-process mock device.

The same suite (minus mock-only hooks) is meant to run against real hardware
later: set PCBTESTER_PORT and see test_hardware.py.
"""

import queue
import time

import pytest

from pcbtester import CommandError, MockTester, PCBTesterClient


@pytest.fixture()
def mock():
    m = MockTester(telem_hz=20, sim_hz=100)
    yield m
    m.stop()


@pytest.fixture()
def client(mock):
    c = PCBTesterClient(mock.transport()).start()
    yield c
    c.close()


# --------------------------------------------------------------------------- #
# Identity / basic ack discipline
# --------------------------------------------------------------------------- #

def test_hello_identity(client):
    info = client.hello()
    assert info["ok"] is True
    assert info["name"] == "PCB-Tester"
    assert info["proto"] == 1
    assert (info["lvlp"], info["hp"], info["hv"]) == (8, 2, 1)


def test_unknown_command_errors(client):
    with pytest.raises(CommandError) as e:
        client.command("does.not.exist")
    assert e.value.err == "E_CMD"


def test_parse_error_is_acked(mock):
    mock.write_line("this is not json")
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        line = mock._out.get(timeout=1.0)
        import json

        msg = json.loads(line)
        if msg.get("type") == "ack" and not msg.get("ok", True):
            assert msg["err"] == "E_PARSE"
            return
    pytest.fail("no E_PARSE ack received")


# --------------------------------------------------------------------------- #
# Channel control
# --------------------------------------------------------------------------- #

def test_vs_reflected_in_telemetry(client):
    client.set_channel(1, "VS", v=3.3)
    ev = client.next_event("telem")
    ch1 = ev["lvlp"][0]
    assert ch1["mode"] == "VS"
    assert ch1["conn"] is True
    assert ch1["vt"] == pytest.approx(3.3, abs=0.001)
    assert ch1["v"] == pytest.approx(3.3, abs=0.05)


def test_hz_parks_channel(client):
    client.set_channel(2, "VS", v=5.0)
    client.set_channel(2, "HZ")
    ev = client.next_event("telem")
    ch2 = ev["lvlp"][1]
    assert ch2["mode"] == "HZ"
    assert ch2["conn"] is False
    assert ch2["vt"] == 0.0


def test_cs_requires_current_argument(client):
    with pytest.raises(CommandError) as e:
        client.set_channel(1, "CS")
    assert e.value.err == "E_ARG"


def test_pwm_amplitude_and_resolution(client):
    client.set_channel(1, "PWM", duty=512, freq=1000, v=5.0, res=10)
    ev = client.next_event("telem")
    ch1 = ev["lvlp"][0]
    assert ch1["mode"] == "PWM"
    assert ch1["duty"] == 512
    assert ch1["res"] == 10
    assert ch1["vt"] == pytest.approx(5.0, abs=0.001)
    # Average DC = amplitude * duty ratio = 5.0 * 512/1023 ≈ 2.50 V
    assert ch1["v"] == pytest.approx(5.0 * 512 / 1023, abs=0.05)


def test_pwm_duty_must_fit_resolution(client):
    with pytest.raises(CommandError) as e:
        client.set_channel(1, "PWM", duty=300, freq=1000, res=8)  # max 255
    assert e.value.err == "E_ARG"
    with pytest.raises(CommandError) as e:
        client.set_channel(1, "PWM", duty=10, freq=1000, res=20)  # bits 1-14
    assert e.value.err == "E_ARG"


def test_pwm_only_on_ch1_to_4(client):
    client.set_channel(4, "PWM", duty=128, freq=1000)
    with pytest.raises(CommandError) as e:
        client.set_channel(5, "PWM", duty=128, freq=1000)
    assert e.value.err == "E_STATE"


def test_channel_out_of_range(client):
    with pytest.raises(CommandError) as e:
        client.set_channel(12, "VS", v=1.0)
    assert e.value.err == "E_ARG"


def test_ch_set_rejected_for_hp_channels(client):
    with pytest.raises(CommandError) as e:
        client.set_channel(9, "VS", v=1.0)
    assert e.value.err == "E_ARG"


def test_lvlp_connect_rejected(client):
    with pytest.raises(CommandError) as e:
        client.connect_channel(1)
    assert e.value.err == "E_ARG"


def test_hp_relay_connect_disconnect(client):
    client.connect_channel(9)
    ev = client.next_event("telem")
    assert ev["hp"][0]["conn"] is True
    assert ev["hp"][0]["vout"] == pytest.approx(12.0, abs=0.5)

    client.disconnect_channel(9)
    client.drain_events()
    ev = client.next_event("telem")
    assert ev["hp"][0]["conn"] is False


def test_hv_relay_and_reading(client, mock):
    mock.set_hv_supply(48.0)
    client.connect_channel(11)
    client.drain_events()
    ev = client.next_event("telem")
    assert ev["hv"][0]["conn"] is True
    assert ev["hv"][0]["v"] == pytest.approx(48.0, abs=0.5)


# --------------------------------------------------------------------------- #
# Faults
# --------------------------------------------------------------------------- #

def _wait_fault(client, ch, timeout=2.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ev = client.next_event("fault", timeout=timeout)
        if ev["ch"] == ch:
            return ev
    pytest.fail(f"no fault event for ch {ch}")


def test_overcurrent_fault_latches(client, mock):
    client.set_limits(1, vmax=12.0, imax=0.1)
    mock.set_load(1, 10.0)               # 5 V / 10 ohm = 0.5 A > 0.1 A
    client.set_channel(1, "VS", v=5.0)

    ev = _wait_fault(client, ch=1)
    assert ev["code"] == "OVERCURRENT"

    # Latched: re-arming without reset must fail
    with pytest.raises(CommandError) as e:
        client.set_channel(1, "VS", v=1.0)
    assert e.value.err == "E_STATE"

    # Telemetry shows the fault status and open relay
    client.drain_events()
    t = client.next_event("telem")
    assert t["lvlp"][0]["st"] == 1
    assert t["lvlp"][0]["conn"] is False

    # Reset clears it
    mock.set_load(1, None)
    client.reset_channel(1)
    client.set_channel(1, "VS", v=1.0)   # must not raise


def test_hv_overvoltage_fault(client, mock):
    client.set_limits(11, vmax=60.0)
    mock.set_hv_supply(65.0)
    client.connect_channel(11)
    ev = _wait_fault(client, ch=11)
    assert ev["code"] == "OVERVOLTAGE"
    with pytest.raises(CommandError):
        client.connect_channel(11)


# --------------------------------------------------------------------------- #
# E-stop
# --------------------------------------------------------------------------- #

def test_estop_opens_everything(client):
    client.set_channel(1, "VS", v=3.0)
    client.set_channel(4, "PWM", duty=100, freq=500)
    client.connect_channel(9)
    client.connect_channel(11)

    client.estop()
    ev = client.next_event("estop")
    assert ev["src"] == "cmd"

    client.drain_events()
    t = client.next_event("telem")
    assert t["estop"] is True
    assert all(not c["conn"] and c["mode"] == "HZ" for c in t["lvlp"])
    assert all(not c["conn"] for c in t["hp"])
    assert all(not c["conn"] for c in t["hv"])


def test_estop_clear_dismisses_latch(client):
    client.estop()
    client.next_event("estop")
    client.clear_estop()
    client.drain_events()
    t = client.next_event("telem")
    assert t["estop"] is False


def test_physical_estop_button(client, mock):
    client.connect_channel(9)
    mock.press_estop_button()
    ev = client.next_event("estop")
    assert ev["src"] == "button"


# --------------------------------------------------------------------------- #
# Burst capture
# --------------------------------------------------------------------------- #

def test_capture_returns_requested_samples(client):
    client.set_channel(1, "VS", v=2.5)
    time.sleep(0.1)
    ev = client.capture(1, n=64, dt_ms=2)
    assert ev["ch"] == 1
    assert ev["unit"] == "V"
    assert ev["dt_ms"] == 2
    assert len(ev["samples"]) == 64
    assert all(abs(s - 2.5) < 0.2 for s in ev["samples"])


def test_capture_validates_parameters(client):
    with pytest.raises(CommandError) as e:
        client.command("ch.capture", ch=1, n=10000, dt_ms=2)
    assert e.value.err == "E_ARG"
    with pytest.raises(CommandError):
        client.command("ch.capture", ch=1, n=512, dt_ms=100)  # 51.2 s > 5 s cap


# --------------------------------------------------------------------------- #
# Settling step-response capture (ch.settle)
# --------------------------------------------------------------------------- #

def test_settle_returns_step_response(client):
    ev = client.settle(1, from_v=1.0, to_v=10.0, n=200, dt_us=333, settle_ms=0)
    assert ev["ch"] == 1
    assert ev["kind"] == "settling"
    assert ev["unit"] == "V"
    assert ev["dt_us"] == 333
    assert len(ev["samples"]) == 200
    assert "vcmd" in ev and "rshunt" in ev and "imax" in ev
    # First-order relaxation: starts near from_v, ends near to_v.
    assert abs(ev["samples"][0] - 1.0) < 0.5
    assert abs(ev["samples"][-1] - 10.0) < 0.2


def test_settle_parks_channel_in_hz(client):
    client.settle(1, from_v=1.0, to_v=10.0, n=64, dt_us=333, settle_ms=0)
    # After the capture the channel must be back in high impedance.
    t = client.next_event("telem")
    ch1 = next(c for c in t["lvlp"] if c["ch"] == 1)
    assert ch1["mode"] == "HZ"
    assert ch1["conn"] is False


def test_settle_only_for_lvlp(client):
    with pytest.raises(CommandError) as e:
        client.request_settle(9, from_v=0.0, to_v=5.0)
    assert e.value.err == "E_ARG"


def test_settle_validates_parameters(client):
    with pytest.raises(CommandError):
        client.request_settle(1, from_v=1.0, to_v=10.0, n=1)  # n < 2
    with pytest.raises(CommandError):
        client.request_settle(1, from_v=1.0, to_v=10.0, dt_us=10)  # dt_us < 50
    with pytest.raises(CommandError):
        client.request_settle(1, from_v=1.0, to_v=10.0, n=512, dt_us=100000)  # > 2 s


# --------------------------------------------------------------------------- #
# Calibration
# --------------------------------------------------------------------------- #

def test_cal_get_shapes(client):
    lvlp = client.get_calibration(1)
    assert set(lvlp) == {"K1", "K2", "offset", "mADC", "bADC", "mDAC", "bDAC"}
    hp = client.get_calibration(9)
    assert "sens" in hp and "zero_adc" in hp
    hv = client.get_calibration(11)
    assert hv["deadzone"] == 100
    assert hv["points"][0] == [100, 25.23]


def test_cal_set_merges(client):
    client.set_calibration(1, {"offset": 0.5})
    cal = client.get_calibration(1)
    assert cal["offset"] == 0.5
    assert cal["K1"] == pytest.approx(4.9224, abs=1e-3)  # untouched field


def test_cal_save_load_roundtrip(client):
    original = client.get_calibration(1)["offset"]
    client.save_calibration()
    client.set_calibration(1, {"offset": 9.9})
    assert client.get_calibration(1)["offset"] == 9.9
    ack = client.load_calibration()
    assert ack["loaded"] == 11
    assert client.get_calibration(1)["offset"] == original


def test_cal_backup_restore_all_channels(client):
    backup = {ch: client.get_calibration(ch) for ch in range(1, 12)}
    client.set_calibration(5, {"offset": 1.234})
    for ch, cal in backup.items():
        client.set_calibration(ch, cal)
    assert client.get_calibration(5)["offset"] == backup[5]["offset"]


# --------------------------------------------------------------------------- #
# Testbench
# --------------------------------------------------------------------------- #

def _quick_campaign():
    return [
        {"name": "drive & check CH1->CH3", "type": "voltage_accuracy",
         "params": {"mask": 1, "target_v": 3.0, "tolerance_v": 0.1,
                    "settle_ms": 50, "samples": 4}},
        {"name": "sense CH3 (wired to CH1)", "type": "static_voltage",
         "setup": [{"step": "vs", "ch": 1, "v": 2.5}],
         "params": {"sense_mask": 4, "expected_v": 2.5, "tolerance_v": 0.2,
                    "settle_ms": 50}},
    ]


def _run_campaign(client, tests, timeout=10.0):
    client.load_campaign(tests)
    client.drain_events()
    client.tb_run()
    results = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ev = client.next_event(timeout=deadline - time.monotonic())
        if ev.get("type") == "tb_result":
            results.append(ev)
        elif ev.get("type") == "tb_done":
            return results, ev
    pytest.fail("no tb_done received")


def test_tb_add_validates(client):
    with pytest.raises(CommandError) as e:
        client.tb_add({"name": "x", "type": "not_a_type"})
    assert e.value.err == "E_ARG"
    with pytest.raises(CommandError) as e:
        client.tb_add({"type": "static_voltage"})  # missing name
    assert e.value.err == "E_ARG"
    with pytest.raises(CommandError) as e:
        client.tb_add({"name": "x", "type": "static_voltage",
                       "setup": [{"step": "teleport"}]})
    assert e.value.err == "E_ARG"


def test_tb_list(client):
    client.load_campaign(_quick_campaign())
    ack = client.tb_list()
    assert [t["name"] for t in ack["tests"]] == [
        "drive & check CH1->CH3", "sense CH3 (wired to CH1)"]


def test_tb_run_streams_results(client, mock):
    mock.wire(1, 3)  # CH3 reads back CH1, like a physical loom
    results, done = _run_campaign(client, _quick_campaign())
    assert [r["test"] for r in results] == [0, 1]
    assert all(r["outcome"] == "PASS" for r in results), results
    assert done["pass"] == 2 and done["fail"] == 0 and not done["aborted"]


def test_tb_fails_without_wiring(client):
    # CH3 floats at ~0 V, so expecting 2.5 V must FAIL
    results, done = _run_campaign(client, [_quick_campaign()[1]])
    assert results[0]["outcome"] == "FAIL"
    assert done["fail"] == 1


def test_tb_busy_guard_and_abort(client, mock):
    mock.tb_time_scale = 1.0
    slow = [{"name": "slow", "type": "static_voltage",
             "setup": [{"step": "wait", "ms": 5000}],
             "params": {"sense_mask": 1, "expected_v": 0.0,
                        "tolerance_v": 1.0, "settle_ms": 1000}}]
    client.load_campaign(slow)
    client.tb_run()
    time.sleep(0.2)
    # Normal commands are rejected while running…
    with pytest.raises(CommandError) as e:
        client.set_channel(1, "VS", v=1.0)
    assert e.value.err == "E_BUSY"
    # …but status and abort are served
    st = client.tb_status()
    assert st["running"] is True
    client.tb_abort()
    ev = client.next_event("tb_done", timeout=5.0)
    assert ev["aborted"] is True
    # And the device is usable again afterwards
    client.set_channel(1, "VS", v=1.0)


def test_tb_run_empty_errors(client):
    client.tb_clear()
    with pytest.raises(CommandError) as e:
        client.tb_run()
    assert e.value.err == "E_STATE"


# --------------------------------------------------------------------------- #
# Telemetry rate
# --------------------------------------------------------------------------- #

def test_telemetry_rate_zero_stops_stream(client):
    client.next_event("telem")               # stream is alive
    client.set_telemetry_rate(0)
    client.drain_events()
    with pytest.raises(queue.Empty):
        client.next_event("telem", timeout=0.5)
    ack = client.set_telemetry_rate(10)
    assert ack["telem_hz"] == 10
    client.next_event("telem")               # alive again


def test_telemetry_rate_validation(client):
    with pytest.raises(CommandError) as e:
        client.set_telemetry_rate(100)
    assert e.value.err == "E_ARG"
