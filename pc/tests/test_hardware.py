"""Smoke tests against real hardware. Skipped unless PCBTESTER_PORT is set:

    PCBTESTER_PORT=/dev/cu.usbmodemXXXX python -m pytest tests/test_hardware.py -v

Safe to run with nothing wired to the channels: only CH1 is driven, at 1 V,
and everything is parked afterwards.
"""

import os

import pytest

PORT = os.environ.get("PCBTESTER_PORT")

pytestmark = pytest.mark.skipif(not PORT, reason="PCBTESTER_PORT not set")


@pytest.fixture()
def client():
    from pcbtester import PCBTesterClient, SerialTransport

    c = PCBTesterClient(SerialTransport(PORT)).start()
    yield c
    try:
        c.estop()
    finally:
        c.close()


def test_hello(client):
    info = client.hello(timeout=5.0)
    assert info["name"] == "PCB-Tester"
    assert info["proto"] == 1


def test_telemetry_flows(client):
    client.set_telemetry_rate(10)
    ev = client.next_event("telem", timeout=3.0)
    assert len(ev["lvlp"]) == 8
    assert len(ev["hp"]) == 2
    assert len(ev["hv"]) == 1


def test_vs_and_park(client):
    client.set_channel(1, "VS", v=1.0)
    ev = client.next_event("telem", timeout=3.0)
    assert ev["lvlp"][0]["mode"] == "VS"
    client.set_channel(1, "HZ")


def test_estop(client):
    client.estop()
    ev = client.next_event("estop", timeout=3.0)
    assert ev["src"] == "cmd"
