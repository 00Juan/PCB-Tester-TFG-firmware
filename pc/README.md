# pcbtester — PC-side software for the PCB Tester

Python client library, device simulator and test suite for the PCB Tester's
NDJSON serial protocol (firmware `[env:app]`, `lib/Protocol`). The PySide6 GUI
(phase 3 of the roadmap) will build on this package.

## Install

```bash
cd pc
python3 -m venv .venv && source .venv/bin/activate
python -m pip install --upgrade pip   # macOS Python 3.9 ships a pip too old for pyproject editable installs
pip install -e ".[dev]"
```

## Try it without hardware (mock device)

```bash
pcbtester-repl --mock
```

```
pcb> hello                # device identity
pcb> vs 1 3.3             # CH1 = 3.3 V voltage source
pcb> load 1 100           # simulate a 100 ohm load on CH1
pcb> telem                # print one telemetry frame  (v≈3.30, i≈0.033)
pcb> estop
pcb> quit
```

## Try it with hardware

Flash `pio run -e app -t upload` and plug into the ESP32-S3 **native USB**
connector (not the UART bridge). Then:

```bash
pcbtester-repl            # auto-detects the tester by probing "hello"
```

## Use as a library

```python
from pcbtester import PCBTesterClient, SerialTransport, MockTester, find_tester

# Real device...
client = PCBTesterClient(SerialTransport(find_tester())).start()
# ...or simulated device (same protocol, same API):
# client = PCBTesterClient(MockTester().transport()).start()

print(client.hello())
client.set_channel(1, "VS", v=3.3)          # LVLP: HZ | VS | CS | RL | PWM
client.connect_channel(9)                   # HP/HV relays are ch 9-11
client.set_limits(1, vmax=5.0, imax=0.3)
client.on_fault = print                     # events: telem/fault/estop/log
client.estop()
client.close()
```

Commands raise `CommandError` (device said `ok:false`) or `ProtocolTimeout`.
Events also queue up in `client.events`; `client.next_event("telem")` pops the
next one of a type. Callbacks run on the reader thread — in GUI code, re-post
them to the UI loop instead of touching widgets directly.

## Command reference

Channel numbering everywhere: **1–8 = LVLP**, **9–10 = HP**, **11 = HV**.

### REPL commands (`pcbtester-repl`)

| Command | Example | What it does |
|---|---|---|
| `hello` | `hello` | Print device identity (firmware, protocol version, channel counts) |
| `vs <ch> <volts>` | `vs 1 3.3` | LVLP channel → voltage source |
| `cs <ch> <amps>` | `cs 2 0.05` | LVLP channel → current source |
| `pwm <ch> <duty> <freq>` | `pwm 1 128 1000` | PWM generator, duty 0–255, freq in Hz (CH1–CH4 only) |
| `hz <ch>` | `hz 1` | High impedance: relay open, DAC parked at 0 V |
| `on <ch>` / `off <ch>` | `on 9` | Close / open an HP or HV relay (ch 9–11 only) |
| `limits <ch> <vmax> [imax]` | `limits 1 5 0.1` | Protection limits; `imax` required except for ch 11 |
| `reset <ch>` | `reset 1` | Clear a latched fault (channel is left disconnected) |
| `rate <hz>` | `rate 5` | Telemetry rate 0–50 Hz (0 = off) |
| `estop` | `estop` | Emergency stop: everything opens immediately |
| `telem` | `telem` | Print the next telemetry frame |
| `watch` | `watch` | Stream telemetry until you press Enter |
| `load <ch> <ohms>` | `load 1 100` | **Mock only**: attach a load resistor to an LVLP channel |
| `{...}` | `{"id":1,"cmd":"hello"}` | Send a raw protocol line as-is |
| `quit` | `quit` | Exit the REPL |

Fault and E-stop events print on their own as `<< {...}` whenever they happen.

### Raw protocol (NDJSON over serial)

What the REPL/library actually send — useful from any language or a plain
serial monitor. One JSON object per line; every command carries a client-chosen
`id` and is answered exactly once with an ack carrying the same `id`.

```json
{"id":1,"cmd":"hello"}
{"id":2,"cmd":"ch.set","ch":1,"mode":"VS","v":3.14}     // mode: HZ|VS|CS|RL|PWM
{"id":3,"cmd":"ch.set","ch":1,"mode":"CS","i":0.05}     // CS/RL need "i"
{"id":4,"cmd":"ch.set","ch":1,"mode":"PWM","duty":128,"freq":1000}
{"id":5,"cmd":"ch.set","ch":1,"mode":"HZ"}
{"id":6,"cmd":"ch.connect","ch":9}                      // HP/HV relays only (9-11)
{"id":7,"cmd":"ch.disconnect","ch":9}
{"id":8,"cmd":"ch.limits","ch":1,"vmax":5.0,"imax":0.3} // ch 11: vmax only
{"id":9,"cmd":"ch.reset","ch":1}
{"id":10,"cmd":"telem.rate","hz":10}                    // 0-50, 0 = off
{"id":11,"cmd":"estop"}
{"id":12,"cmd":"estop.clear"}                           // dismiss latch indicator only
```

Acks:

```json
{"type":"ack","id":2,"ok":true}
{"type":"ack","id":2,"ok":false,"err":"E_STATE","msg":"channel faulted, send ch.reset first"}
```

| Error | Meaning |
|---|---|
| `E_PARSE` | Line was not valid JSON |
| `E_CMD` | Unknown or missing `cmd` |
| `E_ARG` | Bad/missing parameter (`ch` out of range, missing `v`, ...) |
| `E_STATE` | Command valid but refused: channel faulted, PWM on CH5–8, ... |

Unsolicited events (no `id`), pushed by the device:

```json
{"type":"telem","t":123456,"estop":false,
 "lvlp":[{"ch":1,"mode":"VS","st":0,"conn":true,"vt":3.3,"it":0.0,"v":3.29,"i":0.033}, ...],
 "hp":[{"ch":9,"st":0,"conn":true,"vin":12.01,"vout":11.95,"i":0.31}, ...],
 "hv":[{"ch":11,"st":0,"conn":false,"v":0.0}]}
{"type":"fault","ch":1,"code":"OVERCURRENT","v":4.98,"i":0.52}
{"type":"estop","src":"cmd"}        // src: "cmd" | "button"
{"type":"log","lvl":"info","msg":"PCB Tester app ready"}
```

`st` status codes: 0 = normal, 1 = overcurrent, 2 = overvoltage, 3 = other
(HV: 0 = normal, 1 = overvoltage, 2 = other). PWM channels also report
`duty`/`freq` in telemetry. Once faulted, a channel refuses everything except
`ch.reset` — that's the same latch the firmware's LEDs show in red/blue.

### Python client methods

| Method | Protocol command |
|---|---|
| `client.hello()` | `hello` |
| `client.set_channel(ch, mode, v=, i=, duty=, freq=)` | `ch.set` |
| `client.connect_channel(ch)` / `client.disconnect_channel(ch)` | `ch.connect` / `ch.disconnect` |
| `client.set_limits(ch, vmax, imax=None)` | `ch.limits` |
| `client.reset_channel(ch)` | `ch.reset` |
| `client.set_telemetry_rate(hz)` | `telem.rate` |
| `client.estop()` / `client.clear_estop()` | `estop` / `estop.clear` |
| `client.command(cmd, **params)` | anything (escape hatch) |
| `client.next_event("telem")`, `client.events`, `client.on_fault = fn` | event access |

## Tests

```bash
python -m pytest              # 19 protocol-conformance tests against the mock
PCBTESTER_PORT=/dev/cu.usbmodemXXXX python -m pytest tests/test_hardware.py -v
```

The hardware smoke tests only drive CH1 at 1 V and E-stop everything at the
end, so they are safe to run with nothing wired to the front panel.
