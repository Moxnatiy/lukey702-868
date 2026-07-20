# Lukey 702/868 — soldering-iron MCU hardware map

**MCU:** ATmega8L-8PU (IC2), internal RC 8 MHz.
**Fuses as found on the chip:** `lfuse=0x24`, `hfuse=0xD9`, **`lock=0xFC`** — the factory chip is
read-protected (Lock Mode 3), exactly like the hot-air one, so the original firmware cannot be
dumped (an ISP read returns only the address-ramp artifact).

Sources: the Scorpio schematic *LUKEY868(702) SOLDERING* and the disassembly of the reference
firmware `Luk702_sir-SE` (`docs/disasm_raw.txt`).

---

## Port initialization (reference `hw_init` @0x06B0)

```
PORTB = 0x36 ; DDRB = 0xCB   -> out: PB0,PB1,PB3,PB6,PB7 | in+pullup: PB2,PB4,PB5
PORTC = 0x7E ; DDRC = 0x00   -> ALL PORTC are INPUTS (pull-ups on PC1..PC6, PC0 = analog)
PORTD = 0xFF ; DDRD = 0xFF   -> all 8 pins output (display segments)
```

Note the key difference from the hot-air MCU: **`DDRC = 0x00`** — the iron MCU drives nothing on
port C. There is no fan and no power-latch output here.

## Pinout of IC2 ATMEGA8L-8PU

| Pin | Port | Dir | Function |
|-----|------|-----|----------|
| 1 | PC6/RESET | — | Reset |
| 2 | PD0 | out | Display **segment a** |
| 3 | PD1 | out | Display **segment e** |
| 4 | PD2 | out | Display **segment d** |
| 5 | PD3 | out | Display **segment f** (R25 9.1) |
| 6 | PD4 | out | Display **segment dp** |
| 7 | VCC | — | +5V |
| 8 | GND | — | ground |
| 9 | PB6 | out | **DG3** — digit-3 select (R20 9.1) |
| 10 | PB7 | out | **DG2** — digit-2 select (R18 9.1) |
| 11 | PD5 | out | Display **segment c** |
| 12 | PD6 | out | Display **segment g** |
| 13 | PD7 | out | Display **segment b** |
| 14 | PB0 | out | **DG1** — digit-1 select (R19 9.1) |
| **15** | PB1/OC1A | out (PWM) | **HEATER**: R27 240 → **MOC3023** (in the 937D module) → **BT135** triac → iron heater (~26 V) |
| **16** | PB2 | in, pull-up | **DOWN** button |
| 17 | PB3 | out | (unused on the schematic) |
| **18** | PB4 | in, pull-up | **"Power Up" / supply presence** sense (via R14 470 + D14 zener from the ~10 V rail) |
| **19** | PB5 | in, pull-up | **UP** button |
| 20 | AVCC | — | ADC supply (via R4 33) |
| 21 | AREF | — | **ADC reference ≈ 2.5 V (TL431)** + C32/C7 |
| 22 | GND | — | ground |
| **23** | PC0/ADC0 | in (ADC) | **Iron thermocouple** via the **OP07** amplifier inside the LUKEY 937D module |
| 24–28 | PC1..PC5 | in, pull-up | unused (the reference reads `PINC,2` — likely a service jumper) |

**Display:** BDD-4301, 3 digits, **common anode** — wiring identical to the hot-air board
(PD0=a, PD1=e, PD2=d, PD3=f, PD4=dp, PD5=c, PD6=g, PD7=b; DG1=PB0, DG2=PB7, DG3=PB6, active high).

**Iron connector:** 6-pin DIN ("connect to SOLDER") carrying the heater (~26 V via BT135, R21 27)
and the thermocouple pair, which goes to the OP07 stage in the **LUKEY 937D module**
(R1 10K, R2 15K, R3 7K5, R5 51K, R6 4K7, R7/R9 1K, R8 33, C1 104) and out to pin 23.

---

## Timers / ADC (reference values)

| Peripheral | Value | Meaning |
|------------|-------|---------|
| Timer0 | `TCCR0=0x05`, `TCNT0=0xB2` | clk/1024, reload → ≈100 Hz control tick |
| Timer1 | `TCCR1A=0xC3`, `TCCR1B=0x04` | Fast-PWM 10-bit, clk/256, **OC1A inverting = heater** |
| Timer2 | `TCCR2=0x04`, `TCNT2=0x83` | clk/64, reload → ≈1 kHz display multiplex |
| TIMSK | `0x41` | TOIE0 + TOIE2 |
| ACSR | `0x80` | analog comparator off |
| ADMUX | `0x00` | external AREF (≈2.5 V), channel 0 |
| **ADCSRA** | **`0x8E`** | ADEN + **ADIE (interrupt-driven ADC)** + prescaler /64 |

Active interrupt vectors in the reference: `TIMER0_OVF`, `TIMER2_OVF` and **`ADC`**
(the hot-air firmware used only the two timers and polled the ADC).

---

## Consequences for our firmware

Almost everything from [`../hot-air/`](../hot-air/) carries over unchanged:

- `display.c/h` — identical wiring, usable **as is**.
- PID controller, calibration menu, button gestures, EEPROM settings — same structure.

What has to change:

1. **Remove** the fan / power-latch logic (no PORTC outputs on this MCU).
2. Heater is the same pin (PB1/OC1A) but drives a ~26 V load through BT135 instead of mains.
3. Temperature comes from the 937D module's OP07 → ADC0; needs its own calibration (`OF`).
4. Setpoint range for the iron is **200–480 °C** (per the documentation P.S.).
5. Decide the role of pin 18 (supply/Power-Up sense) for the standby behaviour.
