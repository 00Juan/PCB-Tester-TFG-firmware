# PCBT — Configurable platform for automated PCB testing

Firmware and PC software for the **PCB Tester (PCBT)**, an eleven-channel automated test bench built from scratch to validate the PCBs and embedded systems of the **Málaga Racing Team (MART)** Formula Student car.

Final-year project — Electronic Systems Engineering, School of Telecommunications Engineering, University of Málaga.

## Why this exists

Every season the team designs, manufactures and validates a fair number of boards: telemetry, motor power control, electrical safety. Until now that validation was entirely manual — setting up oscilloscopes, power supplies and multimeters, wiring every test by hand, and repeating the same measurements each time a design changed. It is slow and, above all, easy to get wrong: an accidental short or a reversed polarity can destroy the board under test.

The PCBT sits in the gap between the two usual alternatives: **more versatile and repeatable than manual lab validation, and far cheaper and smaller than a commercial ATE or HIL platform**, so that it is actually within reach of a university team. Validating a new board means describing its test scenarios from the interface — not touching the firmware architecture.

## The bench

An **ESP32-S3** on a four-layer board drives eleven multipurpose channels:

- **8 low-power channels (LVLP)** — generate voltage, act as a current source or as a resistive load through a firmware-closed PID loop, produce PWM (CH1–4) and take analog readings.
- **2 high-power channels (HP)** — high-side switching for high-current loads, each with its own sensing and protection.
- **1 galvanically isolated high-voltage channel (HV)** — reads and switches potentially dangerous levels without compromising the control logic.

Around them: external ADC and DAC, Hall-effect current sensors, shift registers, CAN, an OLED display, a rotary encoder, addressable LEDs and a buzzer.

**Safety is handled on two levels at once.** Physically: fuses, TVS diodes, isolation, and disconnection through solid-state relays and MOSFETs. In firmware: per-channel voltage and current limits, latched fault states that require a manual reset, immediate disconnection on out-of-range conditions, and an emergency stop reachable both from the PC and from the physical button.

## Firmware

Modular C++ organised in three layers — application, services and hardware abstraction — on a **non-blocking superloop**, with no RTOS. It includes an automated test engine with a catalogue of **eleven test types** covering the usual validation cycle: voltage accuracy and ripple, consumption and inrush current, power-up sequencing, PWM integrity, short-circuit protection, load regulation, inter-channel isolation and static measurements.

**Each test is defined by data, not by code**, so complete campaigns are assembled at run time without reprogramming the equipment. Self-test and self-calibration routines store their parameters in non-volatile memory, and everything is exposed over a custom **NDJSON protocol on the ESP32-S3 native USB** (baud-agnostic USB-CDC, ~1 MB/s).

## PC application

A Python + Qt (PySide6) client of that same protocol, in six tabs: **Dashboard** (manual channel control), **Operate** (drives the board under test through profiles and macros, so the way each board is operated is written down once and anyone can repeat it without knowing the protocol), **Testbench** (define, save and run campaigns), **Trends** and **Scope** (live monitoring), and **Calibration**.

It ships with a **device simulator**, so the interface and the tests can be exercised with no hardware at hand:

```bash
cd pc && pip install -e ".[dev]"
pcbtester-gui --mock
```

Full details in [`pc/README.md`](pc/README.md).

## Validated on real boards

Validation ran in three steps: each peripheral and channel type verified separately as it was integrated; then **the bench testing itself**, with four channels acting as the tester and another four emulating a board under test; and finally automated campaigns on two real boards from the car.

- **BSPD** (brake system plausibility device) — passed all **9 tests required by the regulations**, including timings and voltage thresholds.
- **TSAL** (tractive system active light) — **23 scenarios, 21 passes and 2 genuine non-conformities**, which pinned down a defect in the output stage of the board's green LED.

That second result is the point: the bench is not only useful for checking whether a board complies, but for locating where the fault is when it does not.

## Repository layout

```
platformio.ini      one build environment per firmware target
src/app.cpp         unified application firmware (all 11 channels + NDJSON protocol)
src/test_*.cpp      per-peripheral and per-DUT benches (BSPD, TSAL, self-test, PWM,
                    settling time, encoder, displays, ADC/DAC, shift registers…)
src/calibrate_CH1   external absolute calibration routine for an LVLP channel
lib/LVLPChannel     low-power channel driver (VS / CS / RL / PWM modes)
lib/HPChannel       high-power channel driver
lib/HVChannel       isolated high-voltage channel driver
lib/DUTTestbench    test engine: the eleven data-defined test types
lib/Protocol        NDJSON protocol over native USB
lib/CalibrationStore calibration persistence in NVS
pc/                 Python GUI, client library, mock device and test suite
pc/campaigns/       ready-made campaigns (BSPD.json, TSAL*.json)
```

## Build

```bash
pio run -e app -t upload          # application firmware
pio device monitor -b 115200
pio run -e test_selftest          # bench tests itself: CH1..4 <-> CH5..8
```

## More about the project

[00juan.dev/#/p/pcbtester](https://00juan.dev/#/p/pcbtester) — photos, the written report (Spanish and English) and the demo video.
