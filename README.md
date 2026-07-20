# Lukey 702 / 868 — open firmware

Reverse-engineered and rewritten open firmware for the **Lukey 702** (board silkscreen
**Lukey 868**) SMD rework soldering station. The station has **two microcontrollers**
(both ATmega8L): one drives the **hot-air gun (fan)**, the other the **soldering iron**.
Both are implemented here.

> 🇺🇦 [Читати українською](README.uk.md)

Both factory MCUs are **read-locked** (`lock=0xFC`), so the original firmware cannot be dumped —
an ISP read returns only an address-ramp artifact. Each firmware was therefore rebuilt from
scratch: the pin map was recovered from the schematic and from the disassembly of a reference
firmware, then reimplemented as clean, documented C with an on-station calibration menu.

## Repository layout

| Folder | MCU | Status |
|--------|-----|--------|
| [`hot-air/`](hot-air/) | Hot-air gun (fan) — ATmega8L | ✅ working: PID + auto-tune, CAL menu, calibrated, flashed & tested |
| [`solder-iron/`](solder-iron/) | Soldering iron — ATmega8L | ✅ working: PID + auto-tune, CAL menu, temperature calibrated, flashed |

Each subproject is self-contained (source, docs, Makefile, VS Code config). The display driver
(`src/display.c`) is shared verbatim — both boards use the same BDD-4301 wiring.

## Shared features

- **PID** heater control at 20 Hz with **conditional-integration anti-windup** (no cold-start overshoot).
- **One-touch PID auto-tune** — relay / Åström–Hägglund method at 250 °C: it measures the
  limit-cycle period and amplitude, computes the ultimate gain and applies Tyreus–Luyben rules,
  then stores P/I/d.
- **On-station calibration menu**, no recompiling: hold **UP** at power-on → `CAL`.
  Short UP/DOWN = navigate / ±1, UP-2 s = enter, DOWN-2 s = back / exit.
  The heater triac is forced off inside the menu (except during auto-tune).
- High-quality ADC: prescaler /128, 16× oversampling, 20 Hz temperature updates.
- Setpoint remembered in EEPROM; the **EESAVE** fuse (`hfuse=0xD1`) keeps calibration across reflashes.
- Buttons with autorepeat (0.5 s delay, then 20 steps/s).

## Hot-air firmware

- State machine **standby → work → cooldown** per the official 10-point algorithm.
- Fan/power-latch control on PC3 and stand sensing on PB4.
- Setpoint **100–480 °C**.
- CAL menu: `P` `I` `d` `OF` (ADC offset) `LO` (cooldown-complete temp) `TST` `AT`.
- **Decimal points:** all three steady while the wand rests on the stand; while heating the last
  dp blinks at a rate proportional to the heater PWM (fast = full power).

## Soldering-iron firmware

- Single operating mode — this MCU has **no fan, no power latch and no stand sensor**
  (pin 18 is tied to ground, `DDRC=0x00`), so it simply regulates to the setpoint.
- Heater via MOC3023 → **BT135** triac (~26 V); thermocouple through the **OP07** stage inside
  the LUKEY 937D module → ADC0.
- Setpoint **200–480 °C**, plus a hard **500 °C** over-temperature cutoff.
- CAL menu: `P` `I` `d` `OF` `Ad` `TST` `AT`, where **`Ad`** shows the **live raw ADC** — it was
  added because the iron has no documented voltage-vs-temperature table.
- Temperature calibrated on hardware from two points (raw 0 = 30 °C, raw 535 = 300 °C).
- Last dp blinks proportionally to the heater PWM.

Prebuilt firmware is attached to each [release](https://github.com/Moxnatiy/lukey702-868/releases).

## Hardware

- **MCUs:** ATmega8L-8PU, internal RC 8 MHz (no external crystal), AREF ≈ 2.5 V (TL431).
- **Board:** Scorpio "LUKEY868(702)" — SMD REWORK and SOLDERING sheets.
- **Display:** BDD-4301, 3-digit common anode (segments on PORTD, digits DG1=PB0 DG2=PB7 DG3=PB6).
- **Programmer:** USBASP (ISP), flashed with `avrdude`.
- **Toolchain:** `avr-gcc` (built here with the PlatformIO AVR toolchain, gcc 7.3.0).

## Quick start

```bash
cd hot-air        # or: cd solder-iron
make              # build -> fan.hex / iron.hex
make flash        # chip-erase (also clears the factory lock) + flash via USBASP
make fuses        # lfuse=0x24 hfuse=0xD1 (EESAVE)
make flash-keep   # later: update flash but keep your calibration
```

See [`hot-air/README.md`](hot-air/README.md) and [`solder-iron/README.md`](solder-iron/README.md)
for pin maps, the calibration menu and per-project details.

## Credits

- Schematic: **Scorpio** (LUKEY868/702, SMD rework + soldering sheets).
- Reference firmwares used to recover the hardware maps: **Luk702_fan_SE-2** and
  **Luk702_sir-SE** by *Volly* (radiokot.ru forum). See `*/original/`.
- Reverse engineering & new firmware: **Serhii ([@Moxnatiy](https://github.com/Moxnatiy))**.

## License

Our code (firmware in `*/src`, tools, Makefiles, docs) is released under the
[MIT License](LICENSE). Third-party material (the reference firmware binaries and the
schematic) belongs to its respective authors and is included only for interoperability
and documentation.
