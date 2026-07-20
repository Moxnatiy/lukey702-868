# Lukey 702 / 868 — open firmware

Reverse-engineered and rewritten open firmware for the **Lukey 702** (board silkscreen
**Lukey 868**) SMD rework soldering station. The station has **two microcontrollers**
(both ATmega8): one drives the **hot-air gun (fan)**, the other the **soldering iron**.

> 🇺🇦 [Читати українською](README.uk.md)

The original firmware was factory-locked (read-protected) and worked incorrectly, so the
project rebuilds each MCU's firmware from scratch — pin map recovered from the schematic and
the reference firmware, then a clean, documented C implementation with an on-station
calibration menu.

## Repository layout

| Folder | MCU | Status |
|--------|-----|--------|
| [`hot-air/`](hot-air/) | Hot-air gun (fan) — ATmega8L | ✅ working: PID + auto-tune, on-station CAL menu, flashed & tested |
| [`solder-iron/`](solder-iron/) | Soldering iron — ATmega8 | 🚧 planned |

Each subproject is self-contained (source, docs, Makefile, VS Code config).

## Hot-air firmware — features

- Full state machine **standby → work → cooldown** per the official 10-point algorithm.
- **PID** heater control (P/I/d) at 20 Hz, tuned for the laggy in-wand thermocouple.
- **On-station calibration menu** (hold **UP** at power-on → `CAL`), no recompiling needed:
  - edit `P` / `I` / `d`, the ADC→°C offset (`OF`) and the cutoff temperature (`LO`);
  - `TST` — display self-test (lights every segment);
  - **`AT` — one-touch PID auto-tune** (relay / Åström–Hägglund method @250 °C).
- High-quality ADC (prescaler /128, 16× oversampling, 20 Hz temperature updates).
- Setpoint **100–480 °C**, saved to EEPROM; the EESAVE fuse keeps your calibration across reflashes.

Prebuilt firmware is attached to each [release](https://github.com/Moxnatiy/lukey702-868/releases).

## Hardware

- **MCUs:** ATmega8L-8PU, internal RC 8 MHz (no external crystal).
- **Board:** Scorpio "LUKEY868(702) SMD REWORK".
- **Programmer:** USBASP (ISP). `avrdude` for flashing.
- **Toolchain:** `avr-gcc` (this repo was built with the PlatformIO AVR toolchain, gcc 7.3.0).

## Quick start (hot-air)

```bash
cd hot-air
make            # build -> fan.hex
make flash      # chip-erase + flash via USBASP
```

See [`hot-air/README.md`](hot-air/README.md) for details, pin map, and the calibration menu.

## Credits

- Schematic: **Scorpio** (LUKEY868/702 SMD rework).
- Reference firmware used to recover the hardware map: **Luk702_fan_SE-2** by *Volly*
  (radiokot.ru forum). See `hot-air/original/`.
- Reverse engineering & new firmware: **Serhii ([@Moxnatiy](https://github.com/Moxnatiy))**.

## License

Our code (firmware in `*/src`, tools, Makefiles, docs) is released under the
[MIT License](LICENSE). Third-party material (the reference firmware binary and the
schematic) belongs to its respective authors and is included only for interoperability
and documentation.
