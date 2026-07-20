# Hot-air gun firmware (Lukey 702 / 868)

Open firmware for the **hot-air gun (fan) MCU** — ATmega8L-8PU, internal RC 8 MHz —
of the Lukey 702/868 SMD rework station. Reverse-engineered from the schematic and a
reference firmware, then rewritten as clean, documented C with an on-station
calibration menu.

## Layout

```
src/        main.c      - firmware: state machine + PID + CAL menu
            display.c/h  - 7-segment multiplex (font matches the reference firmware)
docs/       HARDWARE_MAP.md - full pin map / peripherals
            FIRMWARE.md     - control logic, CAL menu, calibration
            disasm_raw.txt  - annotated disassembly of the reference firmware
tools/      avr_disasm.py   - standalone AVR disassembler (Python 3, offline)
original/   main.hex/.eep   - reference firmware Luk702_fan_SE-2 (third-party, see root LICENSE)
Makefile    build (avr-gcc) + flash (avrdude / USBASP)
```

## Hardware summary

- **MCU:** ATmega8L, internal RC 8 MHz. Fuses `lfuse=0x24`, `hfuse=0xD1` (EESAVE on).
- **Display:** BDD-4301, 3-digit **common anode**. Segments on PORTD, digit selects
  DG1=PB0 / DG2=PB7 / DG3=PB6 (active high).
- **Heater:** PB1/OC1A (pin 15) → R22 240 → MOC3023 → **BTA16** mains triac. PWM power.
- **Thermocouple:** OP07 amplifier → PC0/ADC0 (pin 23).
- **Fan:** PC3 (pin 26) enables the fan power/latch; fan voltage sensed on PC5/ADC5 (pin 28).
- **Buttons:** UP=PB5 (pin 19), DOWN=PB2 (pin 16), active low. Stand sensor "Power Up" on PB4 (pin 18).
- **Timers:** T0 ≈100 Hz (control tick), T1 Fast-PWM on OC1A (heater), T2 ≈1 kHz (display).

Full pin table: [docs/HARDWARE_MAP.md](docs/HARDWARE_MAP.md).

## Firmware features

- State machine per the official 10-point hot-air algorithm: **standby → work → cooldown**.
- **PID** heater control at 20 Hz (P/I/d), tuned for the laggy in-wand thermocouple.
- Buttons with autorepeat; setpoint range **100–480 °C**, saved to EEPROM.
- High-quality ADC: prescaler /128, 16× oversampling, 20 Hz temperature updates.
- **On-station calibration menu** (hold UP at power-on) to tune P / I / d / ADC offset /
  cutoff temperature without recompiling. See [docs/FIRMWARE.md](docs/FIRMWARE.md).

> **Status:** working and flashed. Temperature is roughly calibrated (±~7 °C) — trim the
> `OF` offset on real hardware. A minor display-refresh quirk is under investigation.

## Build / flash

```bash
# avr-gcc toolchain required (PlatformIO toolchain-atmelavr, or osx-cross/avr).
make                 # build -> fan.hex
make flash           # flash (chip-erase; also clears any read-lock)
make flash-keep      # update flash but keep EEPROM (needs EESAVE, hfuse=0xD1)
make fuses           # write fuses lfuse=0x24 hfuse=0xD1
make flash-orig      # flash the reference firmware (hardware test reference)
make read            # read the chip back for verification
make disasm          # regenerate the reference disassembly
```

## Calibration menu (quick reference)

Hold **UP** for 2 s at power-on → `CAL`. Short UP/DOWN = navigate / change ±1;
UP-2s = enter; DOWN-2s = back / exit. In this mode the heater triac is forced off.

| Item | Parameter | Default | Range |
|------|-----------|---------|-------|
| `P`  | PID proportional gain | 22 | 1…99 |
| `I`  | PID integral gain | 17 | 0…99 |
| `d`  | PID derivative gain | 0 | 0…99 |
| `OF` | ADC→°C offset (temperature calibration) | 38 | 0…150 |
| `LO` | cooldown-complete temperature | 60 | 40…120 |

## Reverse-engineering tool

`tools/avr_disasm.py` is a self-contained AVR disassembler (Python 3 only, offline). It
annotates ATmega8 I/O registers and interrupt vectors. Unknown bytes inside data tables
are left as `.word 0x....`.
