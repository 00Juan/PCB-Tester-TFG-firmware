# pcbtester — PC-side software for the PCB Tester

Python GUI, client library, device simulator and test suite for the PCB
Tester's NDJSON serial protocol (firmware `[env:app]`, `lib/Protocol`).

## Install

```bash
cd pc
python3 -m venv .venv && source .venv/bin/activate
python -m pip install --upgrade pip   # macOS Python 3.9 ships a pip too old for pyproject editable installs
pip install -e ".[dev]"
```

## GUI

```bash
pcbtester-gui --mock      # instant demo against the simulated device
pcbtester-gui             # real hardware: pick the port in the UI (or untick nothing and use --port)
```

What's on screen:

- **Connection bar** — serial port selector (⟳ rescans), *Mock device*
  checkbox, Connect/Disconnect, device identity, telemetry rate.
- **Channel grid** — one card per channel. LVLP cards (CH1–8): live V/I,
  mode selector (HZ/VS/CS/RL, PWM on CH1–4), setpoint editors, *Apply*.
  HP/HV cards (CH9–11): live readings and a relay Connect/Disconnect toggle.
  Every card has *Limits…* (protection thresholds) and a *Reset fault* button
  that lights up when the channel latches.
- **E-STOP** — the red button (or **Spacebar**) opens every output
  immediately; a red banner shows until you *Clear E-stop*.
- **Fault banner** — overcurrent/overvoltage events pop a banner and turn the
  offending card red/blue until reset.
- **Testbench tab** — build test campaigns (schema-driven forms for all 11
  test types + raw JSON view, DUT setup steps incl. power-cycling via
  shift-register bits), save/load them as JSON under `pc/campaigns/`
  (`BSPD.json` ships as the reference), run them with a live progress bar,
  abort mid-run, and watch the color-coded results table fill in as each
  test finishes. Results export to CSV/JSON. While a campaign runs the
  device only serves E-stop/abort/status (everything else answers
  `E_BUSY`) and telemetry pauses.
- **Trends tab** — rolling live plots (10–300 s window) fed by telemetry:
  LVLP CH1–8 voltage or current, plus HP/HV voltages; per-channel visibility
  toggles and pause.
- **Scope tab** — on-demand burst capture of any channel (2–512 samples,
  1–100 ms interval, ≤5 s window) with min/max/pk-pk stats. Telemetry pauses
  during the capture — that's the firmware sampling at a fixed rate.
- **Calibration tab** — read/edit each channel's calibration as JSON, write
  it back (RAM), *Save to device NVS* / *Reload from NVS*, and backup/restore
  the whole device to a JSON file.

The GUI is just another client of the protocol: everything it does can also be
done from the REPL below, and it works identically against the mock.

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
| `pwm <ch> <duty> <freq> [v] [res]` | `pwm 1 512 1000 5.0 10` | PWM generator (CH1–CH4): duty 0…2^res−1, freq in Hz, optional amplitude in V and resolution in bits (1–14, default 8) |
| `hz <ch>` | `hz 1` | High impedance: relay open, DAC parked at 0 V |
| `on <ch>` / `off <ch>` | `on 9` | Close / open an HP or HV relay (ch 9–11 only) |
| `limits <ch> <vmax> [imax]` | `limits 1 5 0.1` | Protection limits; `imax` required except for ch 11 |
| `reset <ch>` | `reset 1` | Clear a latched fault (channel is left disconnected) |
| `rate <hz>` | `rate 5` | Telemetry rate 0–50 Hz (0 = off) |
| `cap <ch> [n] [dt_ms]` | `cap 1 256 2` | Voltage burst capture, prints min/max/pk-pk |
| `cal <ch>` | `cal 11` | Print the channel's calibration data |
| `tb <file.json>` | `tb campaigns/BSPD.json` | Load + run a campaign file, print streamed results |
| `tbstop` | `tbstop` | Abort the running campaign |
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
{"id":4,"cmd":"ch.set","ch":1,"mode":"PWM","duty":512,"freq":1000,"v":5.0,"res":10}
                                                        // v = amplitude (optional), res = bits 1-14
                                                        // (optional, default 8); duty must be < 2^res
{"id":5,"cmd":"ch.set","ch":1,"mode":"HZ"}
{"id":6,"cmd":"ch.connect","ch":9}                      // HP/HV relays only (9-11)
{"id":7,"cmd":"ch.disconnect","ch":9}
{"id":8,"cmd":"ch.limits","ch":1,"vmax":5.0,"imax":0.3} // ch 11: vmax only
{"id":9,"cmd":"ch.reset","ch":1}
{"id":10,"cmd":"telem.rate","hz":10}                    // 0-50, 0 = off
{"id":11,"cmd":"estop"}
{"id":12,"cmd":"estop.clear"}                           // dismiss latch indicator only
{"id":13,"cmd":"ch.capture","ch":1,"n":256,"dt_ms":2}   // burst -> "capture" event (blocking, <=5 s)
{"id":14,"cmd":"cal.get","ch":1}                        // ack carries "cal" object
{"id":15,"cmd":"cal.set","ch":1,"cal":{"offset":0.21}}  // merge into RAM
{"id":16,"cmd":"cal.save"}                              // persist ALL channels to NVS flash
{"id":17,"cmd":"cal.load"}                              // reload ALL channels from NVS
{"id":18,"cmd":"tb.clear"}                              // empty the test queue
{"id":19,"cmd":"tb.add","test":{"name":"normal_op","type":"static_voltage",
   "setup":[{"step":"vs","ch":1,"v":1.0},{"step":"sr","bit":8,"on":false},
            {"step":"wait","ms":1000},{"step":"sr","bit":8,"on":true}],
   "params":{"sense_mask":4,"expected_v":12.0,"tolerance_v":1.0,"settle_ms":200}}}
{"id":20,"cmd":"tb.list"}                               // ack: tests[{name,type},…]
{"id":21,"cmd":"tb.run"}                                // ack "started", then events, then tb_done
{"id":22,"cmd":"tb.abort"}
{"id":23,"cmd":"tb.status"}                             // running?, current test, elapsed
```

Testbench test objects: `type` is one of `static_voltage, voltage_accuracy,
voltage_threshold, voltage_ripple, current_consumption, current_inrush,
power_sequence, pwm_integrity, short_circuit, load_regulation,
cross_isolation`; `setup` is an optional list of DUT-setup steps
(`vs/cs/hz/pwm/wait/sr`, ≤12) executed before the test; `params` fields are
per-type (masks are LVLP bitmasks, bit 0 = CH1 — see the GUI's JSON view or
`campaigns/BSPD.json` for examples). Campaigns are loaded one test per
`tb.add`. During a run only `hello`/`estop`/`tb.abort`/`tb.status` are served
(the rest get `E_BUSY`) and telemetry pauses; `estop` also aborts the run.

Calibration `cal` shapes — LVLP: `K1,K2,offset,mADC,bADC,mDAC,bDAC`;
HP: `mADC_VIn,bADC_VIn,mADC_VOut,bADC_VOut,sens,vref,zero_adc`;
HV: `deadzone,vref,points:[[raw,volts],…]` (≤16 points, auto-sorted).
`cal.set` merges: omitted fields keep their current values. On boot the
firmware loads NVS calibration when present, else the `hardwareIOSetup.h`
factory defaults.

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
{"type":"capture","kind":"scope","ch":1,"dt_ms":2,"unit":"V","samples":[3.301,3.298, ...]}
{"type":"tb_progress","test":2,"of":9,"name":"BSPD Double Fault > 500ms: SDC 0V","elapsed_ms":1520}
{"type":"tb_result","test":2,"name":"…","outcome":"PASS","measured":0.02,"expected":0.0,"ms":1802,"detail":"…"}
{"type":"tb_done","total":9,"pass":8,"fail":1,"aborted":false}
```

`st` status codes: 0 = normal, 1 = overcurrent, 2 = overvoltage, 3 = other
(HV: 0 = normal, 1 = overvoltage, 2 = other). PWM channels also report
`duty`/`freq`/`res` in telemetry (`vt` is the PWM amplitude). Once faulted, a channel refuses everything except
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
| `client.capture(ch, n=128, dt_ms=2)` | `ch.capture` (returns the capture event) |
| `client.load_campaign(tests)` / `client.tb_run()` / `client.tb_abort()` | `tb.clear`+`tb.add`×N / `tb.run` / `tb.abort` |
| `client.tb_list()` / `client.tb_status()` | `tb.list` / `tb.status` |
| `client.get_calibration(ch)` / `client.set_calibration(ch, cal)` | `cal.get` / `cal.set` |
| `client.save_calibration()` / `client.load_calibration()` | `cal.save` / `cal.load` |
| `client.command(cmd, **params)` | anything (escape hatch) |
| `client.next_event("telem")`, `client.events`, `client.on_fault = fn` | event access |

## Tests

```bash
python -m pytest              # 19 protocol-conformance tests against the mock
PCBTESTER_PORT=/dev/cu.usbmodemXXXX python -m pytest tests/test_hardware.py -v
```

The hardware smoke tests only drive CH1 at 1 V and E-stop everything at the
end, so they are safe to run with nothing wired to the front panel.
