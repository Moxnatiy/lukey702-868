# Soldering-iron firmware (Lukey 702 / 868)

Open firmware for the **soldering-iron MCU** — ATmega8L-8PU (IC2) — of the Lukey 702/868
station. Built on the same proven modules as [`../hot-air/`](../hot-air/): identical display
driver, PID controller, calibration menu and relay auto-tune.

## Hardware summary

- **MCU:** ATmega8L, internal RC 8 MHz. AREF ≈ 2.5 V (TL431).
- **Display:** BDD-4301, 3-digit common anode — wiring identical to the hot-air board.
- **Heater:** PB1/OC1A (pin 15) → R27 240 → MOC3023 → **BT135** triac → iron (~26 V).
- **Thermocouple:** OP07 amplifier inside the **LUKEY 937D module** → PC0/ADC0 (pin 23).
- **Buttons:** UP=PB5 (pin 19), DOWN=PB2 (pin 16).
- **No fan, no power latch, no stand sensor** (pin 18 is tied to ground, `DDRC=0x00`).

Full pin table: [docs/HARDWARE_MAP.md](docs/HARDWARE_MAP.md).

> The factory MCU is **read-locked** (`lock=0xFC`), so the original firmware cannot be dumped.
> The pin map was recovered from the schematic and the reference firmware `Luk702_sir-SE`
> (third-party, in `original/`).

## Firmware features

- Single operating mode: always regulates to the setpoint (the iron has no standby).
- **PID** heater control at 20 Hz with conditional-integration anti-windup.
- Setpoint **200–480 °C**, remembered in EEPROM (saved 3 s after the last keypress).
- Last decimal point blinks proportionally to the heater PWM (fast = full power).
- Hard over-temperature cutoff at 500 °C.
- **Calibration menu** (hold **UP** at power-on → `CAL`): `P` `I` `d` `OF` `Ad` `TST` `AT`
  - `Ad` — live **raw ADC** readout (used to calibrate the temperature)
  - `TST` — display self-test
  - `AT` — **PID auto-tune** (relay method @250 °C)

## ⚠ First run: calibrate before soldering

The ADC→°C slope is currently a **starting guess** carried over from the hot-air channel — the
iron uses a different OP07 stage, so **the displayed temperature is not trustworthy yet**.

1. Flash, then hold **UP** at power-on to enter `CAL` (the heater is forced off in this menu).
2. Go to `Ad` and note the **raw ADC at room temperature**.
3. Heat carefully under supervision, measure the real tip temperature with a thermometer, and
   note the raw ADC at a second point.
4. With those two points the slope/offset can be fixed (`OF` for the offset).

## Build / flash

```bash
make                 # build -> iron.hex
make flash           # chip-erase (clears the factory lock) + flash
make fuses           # lfuse=0x24 hfuse=0xD1 (EESAVE: calibration survives reflash)
make flash-keep      # update flash, keep EEPROM
make flash-orig      # flash the reference firmware Luk702_sir-SE (fallback)
make disasm          # regenerate the reference disassembly
```
