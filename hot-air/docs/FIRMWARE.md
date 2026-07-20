# Hot-air firmware — control logic

Implements the official 10-point "hot-air operating algorithm". Code: `src/main.c` + `src/display.c`.

## State machine

```
        (start: PC3=0 latch, show setpoint 3s -> "---")
                         │
                ┌────────▼────────┐
                │   ST_STANDBY    │  heater off; wand on stand (pin18=0)
                │  show: setpoint │  UP/DOWN edit the setpoint (step 1,
                │  3s, then "---" │  hold -> autorepeat); 3s idle -> "---"
                └────────┬────────┘
     wand removed (pin18>4V, p.2)   │        ▲ t°<=LO (p.8): PC3=5V
                ┌────────▼────────┐ │        │  standby without showing setpoint
                │    ST_WORK      │ │  ┌──────┴──────────┐
                │ PC3=0; if       │ └─▶│  ST_COOLDOWN    │ heater off (p.7),
                │ pin28>0.4V ->   │    │ wand on stand    │ fan keeps cooling;
                │ heat (PWM);     │◀───┤ (removed again ->│ show real t°
                │ show setpoint   │wand│  back to WORK)   │
                │ 1s -> real t°;  │off └─────────────────┘
                │ dp blinks when  │
                │ reached (p.3)   │
                └─────────────────┘
```

## Mapping to the documentation points

| Point | Implementation |
|-------|----------------|
| 1 | `ST_STANDBY`: show setpoint 3s (`T_3S`), then `disp_show_dashes()`; buttons edit setpoint |
| 2 | Enter `ST_WORK` on `is_off_stand()`; `fan_hold_on()` (PC3=0); heat only while `raw_fan > FAN_ON_RAW` (pin28>0.4V); show setpoint 1s then real t° |
| 3 | `update_heater()` sets `at_temp`; `update_display()` blinks the dp (`DP_BLINK`) |
| 4 | `apply_setpoint_step()` — step ±1, show setpoint while editing |
| 5 | `input_idle >= T_3S` -> `adjusting=0`, back to real t° |
| 6 | `save_setpoint_if_dirty()` writes the setpoint to EEPROM (0x02) after editing |
| 7 | Wand back on stand -> `ST_COOLDOWN`, `heater_off()` |
| 8 | `temp_real <= LO` -> `fan_hold_off()` (PC3=5V) -> `ST_STANDBY` without showing setpoint |
| 9 | Power-off via the Power Up button is hardware (PC3=5V drops power); same logic |
| 10 | Display output identical to the reference (shared font table, `display.c`) |

## Heater control — PID (PWM on OC1A, 20 Hz)

`out = P·error + I·integral/2048 − d·(t° rise over 0.4s)`, clamped to 0…1023.
- **P** — proportional band ≈ 1023/P degrees.
- **I** — removes the residual under-shoot (anti-windup: integral clamped to `HEAT_INT_MAX`).
- **d** — derivative on the MEASUREMENT over a 0.4s window (`DHIST=8` @20Hz). Brakes on fast
  temperature rise. Needed because the **thermocouple is laggy** (mounted near the ceramic,
  heats with a delay -> without D the system overshoots).

P/I/d are edited in the CAL menu and stored in EEPROM. Defaults: P=22, I=17, d=0.

## Temperature calibration

`adc_to_temp()`: `T = raw·TEMP_SLOPE_NUM/TEMP_SLOPE_DEN + g_offset`.

- **Slope** `430/643` — from the documentation table (linear, AREF ≈ 2.5V). Confirmed on hardware.
- **Offset** `g_offset` (menu item `OF`, default 38) — calibrated on hardware. Adjust it if the
  displayed temperature differs from the real one.

**Refinement:** measure the real temperature with a reference meter at two points and adjust
`OF` (constant offset) and, if needed, the slope. `FAN_ON_RAW = 164` (the "fan powered"
threshold, 0.4V) is not yet verified.

## Thermocouple voltage table (pin 23), from the documentation

| T,°C | 50 | 100 | 150 | 200 | 250 | 300 | 350 | 400 | 450 | 480 |
|------|----|----|----|----|----|----|----|----|----|----|
| U,V | 0.35 | 0.51 | 0.74 | 1.00 | 1.27 | 1.40 | 1.55 | 1.72 | 1.85 | 1.92 |

Doc note: values are approximate; the characteristic is linear — rely on the end points.

## Setpoint limits

Hot-air: **100–480 °C** (`TEMP_MIN`/`TEMP_MAX`). Default on a blank EEPROM: 200 °C.

## Service menu (CAL) — calibration on the station itself

**Enter:** hold **UP for 2 s** at power-on → `CAL` on the display → menu.

**⚠ In this mode the heater triac is FORCED OFF** (OC1A is disconnected from pin 15,
PB1=5V → zero current through the opto). No voltage reaches the heater.

**Controls:**
- **short UP / DOWN** — navigate items / change the value by ±1
- **UP 2 s** — enter an item (edit)
- **DOWN 2 s** — back (saves while editing) / exit CAL (at the root)

**Menu tree:**
| Item | Parameter | Default | Range |
|------|-----------|---------|-------|
| `P`  | PID proportional gain | 22 | 1…99 |
| `I`  | PID integral gain (term = integral·I/2048) | 17 | 0…99 |
| `d`  | PID derivative gain (brakes on fast t° rise) | 0 | 0…99 |
| `OF` | ADC→°C offset (temperature calibration) | 38 | 0…150 |
| `LO` | cooldown-complete temperature | 60 | 40…120 |

Values are stored in EEPROM (0x04/0x06/0x0C/0x08/0x0A) and take effect after a restart.

**Tuning tips:** if the station reads ~10° high, open `OF` and decrease by 10. If it overshoots
on heat-up, decrease `P` and/or increase `d`. If it settles below the setpoint, increase `I`.
