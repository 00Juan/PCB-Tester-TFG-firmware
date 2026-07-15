"""Interactive REPL for manual protocol testing.

    python -m pcbtester.cli --mock            # simulated device
    python -m pcbtester.cli                   # auto-detect real tester
    python -m pcbtester.cli --port /dev/cu.usbmodemXXXX

Commands:
    hello                     device identity
    vs <ch> <volts>           voltage source
    cs <ch> <amps>            current source
    pwm <ch> <duty> <freq>    PWM generator (CH1-4)
    hz <ch>                   high impedance
    on <ch> | off <ch>        HP/HV relay (ch 9-11)
    limits <ch> <vmax> [imax]
    reset <ch>                clear latched fault
    rate <hz>                 telemetry rate (0 = off)
    cap <ch> [n] [dt_ms]      voltage burst capture (prints stats)
    cal <ch>                  print channel calibration
    estop
    telem                     print the next telemetry frame
    watch                     stream telemetry until Enter
    load <ch> <ohms>          [mock only] attach load resistor
    { ...raw json... }        send a raw protocol line
    quit
"""

from __future__ import annotations

import argparse
import json
import queue
import sys
import threading

from .client import CommandError, PCBTesterClient, ProtocolTimeout


def _print_event(ev: dict) -> None:
    print(f"\r<< {json.dumps(ev)}")


def main(argv: list = None) -> int:
    ap = argparse.ArgumentParser(prog="pcbtester-repl", description=__doc__)
    ap.add_argument("--mock", action="store_true", help="use the simulated device")
    ap.add_argument("--port", help="serial port (default: auto-detect)")
    args = ap.parse_args(argv)

    mock = None
    if args.mock:
        from .mock import MockTester

        mock = MockTester()
        transport = mock.transport()
        print("Connected to MOCK device")
    else:
        from .transport import SerialTransport, find_tester

        port = args.port or find_tester()
        if not port:
            print("No PCB Tester found. Is it on the native USB port? "
                  "(or use --mock)", file=sys.stderr)
            return 1
        transport = SerialTransport(port)
        print(f"Connected to {port}")

    client = PCBTesterClient(transport).start()
    client.on_fault = _print_event
    client.on_estop = _print_event
    client.on_log = _print_event

    try:
        _repl(client, mock)
    finally:
        client.close()
    return 0


def _repl(client: PCBTesterClient, mock) -> None:
    while True:
        try:
            line = input("pcb> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not line:
            continue
        try:
            if not _run(client, mock, line):
                return
        except CommandError as e:
            print(f"!! {e}")
        except ProtocolTimeout as e:
            print(f"!! timeout: {e}")
        except (ValueError, IndexError):
            print("!! bad arguments (see --help header for usage)")


def _run(client: PCBTesterClient, mock, line: str) -> bool:
    if line.startswith("{"):
        client.send_raw(line)
        return True
    parts = line.split()
    cmd, args = parts[0].lower(), parts[1:]

    if cmd in ("quit", "exit", "q"):
        return False
    if cmd == "hello":
        print(json.dumps(client.hello(), indent=2))
    elif cmd == "vs":
        client.set_channel(int(args[0]), "VS", v=float(args[1]))
    elif cmd == "cs":
        client.set_channel(int(args[0]), "CS", i=float(args[1]))
    elif cmd == "pwm":
        client.set_channel(int(args[0]), "PWM", duty=int(args[1]), freq=int(args[2]))
    elif cmd == "hz":
        client.set_channel(int(args[0]), "HZ")
    elif cmd == "on":
        client.connect_channel(int(args[0]))
    elif cmd == "off":
        client.disconnect_channel(int(args[0]))
    elif cmd == "limits":
        imax = float(args[2]) if len(args) > 2 else None
        client.set_limits(int(args[0]), float(args[1]), imax)
    elif cmd == "reset":
        client.reset_channel(int(args[0]))
    elif cmd == "rate":
        client.set_telemetry_rate(int(args[0]))
    elif cmd == "cap":
        ch = int(args[0])
        n = int(args[1]) if len(args) > 1 else 128
        dt = int(args[2]) if len(args) > 2 else 2
        ev = client.capture(ch, n=n, dt_ms=dt)
        s = ev["samples"]
        print(f"CH{ch}: {len(s)} pts @ {ev['dt_ms']} ms  "
              f"min={min(s):.3f} max={max(s):.3f} pkpk={max(s) - min(s):.3f} "
              f"{ev['unit']}")
    elif cmd == "cal":
        print(json.dumps(client.get_calibration(int(args[0])), indent=2))
    elif cmd == "estop":
        client.estop()
        print("E-STOP sent")
    elif cmd == "telem":
        try:
            print(json.dumps(client.next_event("telem", timeout=3.0), indent=2))
        except queue.Empty:
            print("!! no telemetry (rate 0? not connected?)")
    elif cmd == "watch":
        stop = threading.Event()
        client.on_telemetry = lambda ev: print(f"\r<< {json.dumps(ev)}")
        threading.Thread(target=lambda: (input(), stop.set()), daemon=True).start()
        stop.wait()
        client.on_telemetry = None
    elif cmd == "load":
        if mock is None:
            print("!! load is only available with --mock")
        else:
            mock.set_load(int(args[0]), float(args[1]))
    else:
        print(f"!! unknown command {cmd!r}")
    return True


if __name__ == "__main__":
    sys.exit(main())
