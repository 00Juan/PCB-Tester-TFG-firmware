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
