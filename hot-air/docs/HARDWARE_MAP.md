# Lukey 702/868 — hot-air MCU hardware map

**MCU:** ATmega8L, internal RC 8 MHz (no external crystal).
**Fuses:** `lfuse=0x24`, `hfuse=0xD1` (EESAVE on; the chip must be left unlocked).
**Reference firmware compiler:** avr-gcc + avr-libc (startup shows `__do_clear_bss` /
`__do_copy_data`, and float math `__mulsf3/__addsf3`).

The register values below are extracted from the reference firmware's disassembly
(`hw_init` @0x0734); the pin *functions* are confirmed against the Scorpio schematic.

---

## Port initialization (`hw_init` @0x0734)

```
PORTB = 0x36 ; DDRB = 0xCB   -> out: PB0,PB1,PB3,PB6,PB7 | in+pullup: PB2,PB4,PB5
PORTC = 0x56 ; DDRC = 0x08   -> out: PC3               | in+pullup: PC2
PORTD = 0xFF ; DDRD = 0xFF   -> all 8 pins output, driven high
```

## Full pinout of IC1 ATMEGA8L-8PU (confirmed by the Scorpio LUKEY868/702 schematic)

| Pin | Port | Dir | Function |
|-----|------|-----|----------|
| 1 | PC6/RESET | — | Reset, R5' 10K pull-up |
| 2 | PD0 | out | Display **segment a** |
| 3 | PD1 | out | Display **segment e** |
| 4 | PD2 | out | Display **segment d** |
| 5 | PD3 | out | Display **segment f** (R25 9.1) |
| 6 | PD4 | out | Display **segment dp** |
| 7 | VCC | — | +5V |
| 8 | GND | — | ground |
| 9 | PB6 | out | **DG3** — digit-3 select (R20 9.1 -> PNP) |
| 10 | PB7 | out | **DG2** — digit-2 select (R18 9.1 -> PNP) |
| 11 | PD5 | out | Display **segment c** |
| 12 | PD6 | out | Display **segment g** |
| 13 | PD7 | out | Display **segment b** |
| 14 | PB0 | out | **DG1** — digit-1 select (R19 9.1 -> PNP) |
| **15** | PB1/OC1A | out (PWM) | **HEATER**: R22 240 -> MOC3023 -> **BTA16** (mains, 3A fuse). Timer1 PWM |
| **16** | PB2 | in, pull-up | **DOWN** button |
| 17 | PB3 | out | (unused on the schematic; DDRB=out) |
| **18** | PB4 | in, pull-up | **"Power Up"** — power/stand sense (~10V/~29V tap select) |
| **19** | PB5 | in, pull-up | **UP** button |
| 20 | AVCC | — | ADC supply |
| 21 | AREF | — | ADC reference (cap; ~2.5V via TL431) |
| 22 | GND | — | ground |
| **23** | PC0/ADC0 | in (ADC) | **Thermocouple** via **OP07** amplifier (R8 240 / R23 22K) |
| 24 | PC1 | — | (free) |
| 25 | PC2 | in, pull-up | (`sbic PINC,2`) — service/jumper |
| **26** | PC3 | out | **Fan**: 2N3904 -> **TIP122** -> motor (enable + max speed) |
| 27 | PC4 | — | (free) |
| **28** | PC5/ADC5 | in (ADC) | **Fan voltage**: divider R9 8K2 / R10 10K from the motor "+" |

**Display:** BDD-4301, 3 digits, **common ANODE**. Segments on PORTD with a non-standard
bit order (PD0=a, PD1=e, PD2=d, PD3=f, PD4=dp, PD5=c, PD6=g, PD7=b). Digit selects DG1..DG3
on PB0/PB7/PB6. A segment lights when its PORTD output is **0** (cathode pulled to ground);
a digit is active when its PNP high-side switch is on (MCU pin high).

**Power stage:** heater = BTA16 (16A) mains triac driven via MOC3023, power set by the OC1A
PWM. Fan motor M driven by 2N3904/TIP122 with PC3 control, supplied from the rectifier
(~10/29V). Two more MOC3023+BT131 at the top of the schematic switch transformer taps
(low-voltage supply).

> **Independent check of the segment mapping.** The 7-segment font table in the reference
> firmware at flash @0x0026 = `50 5F 38 1A 17 92 90 5E 10 12` (digits 0–9) is **byte-for-byte
> identical** to the table computed from the schematic (PD0=a…PD7=b, see `src/display.c`).
> So the segment map and the schematic are confirmed by the firmware itself.

---

## Timers

| Timer | Config | Rate | Role |
|-------|--------|------|------|
| **Timer0** | `TCCR0=0x05` (clk/1024), `TCNT0` reload `0xB2` | ≈ **100 Hz** (10 ms) | Control tick (ISR `TIMER0_OVF`): ADC, heater control, logic |
| **Timer1** | `TCCR1A=0xC3, TCCR1B=0x04` | Fast-PWM 10-bit, clk/256 (~30 Hz) | **PWM on OC1A/PB1** (inverting). `OCR1A` = **HEATER** power (via MOC3023) |
| **Timer2** | `TCCR2=0x04` (clk/64), `TCNT2` reload `0x83` | ≈ **1 kHz** (1 ms) | ISR `TIMER2_OVF`: display multiplex |

Active interrupts (`TIMSK=0x41`): **TIMER0_OVF + TIMER2_OVF**. All other vectors -> `rjmp reset`.

Note: this rewritten firmware uses ADCSRA=0x87 (prescaler /128, 62.5 kHz) for higher ADC
quality; the reference firmware used /64 (125 kHz).

---

## ADC

- `ADMUX` = external AREF, right-adjusted. Channels are switched in software.
- **ch0 (ADC0/pin23)** = hot-air thermocouple via the OP07 amplifier.
- **ch5 (ADC5/PC5/pin28)** = fan voltage/speed (divider from the motor "+").

**Temperature linearization:** the reference firmware used float math (constants
`0x3FE666E6 ≈ 1.8f`, `0x41C80000 = 25.0f`). This firmware uses an integer linear map
`T = raw·430/643 + offset` derived from the documentation table (see `FIRMWARE.md`).

---

## EEPROM map (16-bit words, little-endian)

This firmware:

| Address | Contents |
|---------|----------|
| 0x02 | temperature setpoint |
| 0x04 | PID `P` |
| 0x06 | PID `I` |
| 0x08 | ADC offset `OF` |
| 0x0A | cutoff temperature `LO` |
| 0x0C | PID `d` |

Blank cells (`0xFFFF`) fall back to compiled defaults. With EESAVE programmed (hfuse=0xD1)
these values survive a chip-erase, so calibration persists across firmware reflashes.

> The reference firmware instead stored four temperature presets (200/250/300/350 °C) at
> 0x02/0x04/0x06/0x08 — see `original/main.eep`.
