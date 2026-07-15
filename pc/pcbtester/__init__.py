"""PC-side client for the PCB Tester NDJSON serial protocol.

Quick start against real hardware::

    from pcbtester import PCBTesterClient, SerialTransport, find_tester

    port = find_tester()                      # probes serial ports with "hello"
    client = PCBTesterClient(SerialTransport(port))
    client.start()
    print(client.hello())
    client.set_channel(1, "VS", v=3.3)
    client.estop()
    client.close()

Quick start without hardware (mock device)::

    from pcbtester import PCBTesterClient, MockTester

    client = PCBTesterClient(MockTester().transport())
    client.start()
"""

from .client import PCBTesterClient, CommandError, ProtocolTimeout
from .transport import Transport, SerialTransport, discover_ports, find_tester
from .mock import MockTester, MockTransport

__all__ = [
    "PCBTesterClient",
    "CommandError",
    "ProtocolTimeout",
    "Transport",
    "SerialTransport",
    "discover_ports",
    "find_tester",
    "MockTester",
    "MockTransport",
]

__version__ = "0.1.0"
