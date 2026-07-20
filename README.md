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
| [`hot-air/`](hot-air/) | Hot-air gun (fan) — ATmega8L | ✅ working: full PID, on-station CAL menu, flashed & tested |
| [`solder-iron/`](solder-iron/) | Soldering iron — ATmega8 | 🚧 planned |

Each subproject is self-contained (source, docs, Makefile, VS Code config).

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
