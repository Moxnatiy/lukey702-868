/*
 * Lukey 702 / 868  --  SOLDERING-IRON MCU firmware
 * ---------------------------------------------------------------------------
 * ATmega8L-8PU (IC2), internal RC 8 MHz. Board: Scorpio LUKEY868(702).
 *
 * Hardware init uses the register values recovered from the reference firmware
 * (Luk702_sir-SE, hw_init @0x06B0) and the SOLDERING schematic.
 *
 * Unlike the hot-air MCU this one has NO fan, NO power latch and NO stand
 * sensor (pin 18 is tied to ground, DDRC = 0x00 - port C drives nothing).
 * The iron therefore has a single operating mode: always regulate to the
 * setpoint.
 *
 * Pin map (details in docs/HARDWARE_MAP.md):
 *   Display:      segments on PORTD, digits DG1=PB0 DG2=PB7 DG3=PB6 (common anode).
 *   Heater:       PB1/OC1A (pin15) -> R27 240 -> MOC3023 -> BT135 triac (~26 V).
 *   Thermocouple: OP07 (inside the LUKEY 937D module) -> PC0/ADC0 (pin23).
 *   Buttons:      UP=PB5(pin19), DOWN=PB2(pin16), active low.
 *   AREF:         ~2.5 V from a TL431.
 */

#define F_CPU 8000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <avr/eeprom.h>
#include <stdint.h>
#include "display.h"

/* ===================================================================== */
/*  Pins                                                                  */
/* ===================================================================== */
#define HEATER_PWM   OCR1A            /* 0..1023: heater power (PWM)           */
#define BTN_UP       PB5              /* pin19 */
#define BTN_DOWN     PB2              /* pin16 */
#define ADC_CH_THERMO  0             /* pin23: thermocouple via OP07          */

#define btn_up_pressed()    (!(PINB & (1 << BTN_UP)))
#define btn_down_pressed()  (!(PINB & (1 << BTN_DOWN)))
#define heater_off()        (HEATER_PWM = 0)   /* pin15 -> 5V, heater off */

/* ===================================================================== */
/*  Temperature calibration                                               */
/*  T = raw * TEMP_SLOPE_NUM / TEMP_SLOPE_DEN + g_offset                   */
/*  ⚠ The slope below is a STARTING GUESS carried over from the hot-air     */
/*  channel (AREF is also ~2.5 V there). The iron uses a different OP07     */
/*  stage, so calibrate on hardware: use the CAL menu item `Ad` to read the */
/*  raw ADC at two known temperatures, then fix the slope/offset.           */
/* ===================================================================== */
#define TEMP_SLOPE_NUM 430
#define TEMP_SLOPE_DEN 643

/* ===================================================================== */
/*  Control parameters / limits                                           */
/* ===================================================================== */
#define TEMP_MIN     200             /* iron setpoint limits, °C (per docs)      */
#define TEMP_MAX     480
#define TEMP_DEFAULT 300
#define TEMP_ABS_MAX 500             /* hard cutoff: kill the heater above this  */

/* Defaults for the CAL-menu-editable parameters (stored in EEPROM). */
#define DEF_KP       22              /* P: proportional gain                      */
#define DEF_KI       17              /* I: integral gain (term = integral*KI/2048)*/
#define DEF_KD        0              /* d: derivative gain                        */
#define DEF_OFFSET   38              /* OF: ADC->°C offset (calibrate on hardware)*/
#define HEAT_INT_MAX 80000           /* anti-windup: integral accumulator limit   */
#define DHIST        32              /* derivative window: 32 steps @20Hz = 1.6 s */

/* PID relay auto-tune (Astrom-Hagglund), see docs/FIRMWARE.md */
#define AT_TEMP        250           /* tuning temperature, °C                    */
#define AT_HYST          2           /* relay hysteresis, °C                      */
#define AT_RELAY_HIGH  400           /* heater PWM while heating (relay high)     */
#define AT_RELAY_LOW     0
#define AT_D           200           /* relay half-amplitude = (HIGH-LOW)/2       */
#define AT_TEMP_MAX    450           /* safety: abort above this temperature      */
#define AT_TIMEOUT    6000           /* safety: abort after this many 20Hz steps  */
#define AT_SKIP          1           /* discard the first cycle (warm-up)         */
#define AT_USE           3           /* cycles to average                         */

/* Parameter editing ranges */
#define KP_MIN 1
#define KP_MAX 99
#define KI_MIN 0
#define KI_MAX 99
#define KD_MIN 0
#define KD_MAX 99
#define OFFSET_MIN 0
#define OFFSET_MAX 150

#define NUM_ITEMS 7                  /* menu: P I d OF Ad TST AT */
#define ITEM_ADC  4                  /* live raw-ADC readout (calibration aid)   */
#define ITEM_TST  5                  /* display self-test                        */
#define ITEM_AT   6                  /* PID relay auto-tune                      */

/* Timings in 10 ms ticks (Timer0 ~100 Hz) */
#define T_3S         300
#define T_1S         100
#define REPEAT_DELAY  50             /* 0.5 s before autorepeat starts            */
#define REPEAT_PERIOD  5             /* then 20 steps/s                           */

/* Heating indicator: last dp blinks proportionally to the heater PWM. */
#define DP_PWM_FAST    5             /* toggle every 50 ms at full PWM (fast)     */
#define DP_PWM_SLOW  100             /* toggle every 1 s at zero PWM (slow)       */

/* Temperature sampling and heater control run at 20 Hz (every 5 ticks) */
#define CTRL_DIV      5

/* ===================================================================== */
/*  EEPROM map (16-bit words)                                             */
/* ===================================================================== */
#define EE_SETPOINT  ((uint16_t*)0x02)
#define EE_KP        ((uint16_t*)0x04)
#define EE_KI        ((uint16_t*)0x06)
#define EE_OFFSET    ((uint16_t*)0x08)
#define EE_KD        ((uint16_t*)0x0C)
#define EE_BLANK     0xFFFF

/* ===================================================================== */
/*  State                                                                 */
/* ===================================================================== */
static int16_t   setpoint;           /* target temperature, °C          */
static int16_t   g_kp, g_ki, g_kd;   /* PID gains (CAL menu)            */
static int16_t   g_offset;           /* OF: ADC->°C offset              */
static int16_t   temp_real;          /* measured temperature, °C        */
static uint16_t  raw_thermo;         /* raw thermocouple ADC            */

static uint16_t  boot_timer;         /* ticks since power-on            */
static uint16_t  input_idle;         /* ticks since last button press   */
static uint8_t   adjusting;          /* showing/changing the setpoint   */
static uint8_t   sp_dirty;           /* setpoint needs an EEPROM save   */

static volatile uint8_t tick_flag;   /* 10 ms flag from Timer0          */

/* ===================================================================== */
/*  ADC: averaged read (max quality). Prescaler /128 -> 62.5 kHz.          */
/* ===================================================================== */
static uint16_t read_adc_avg(uint8_t mux, uint8_t n)
{
    ADMUX = mux;                 /* external AREF (~2.5 V), right-adjust */
    _delay_us(30);
    ADCSRA |= (1 << ADSC);       /* discard the first conversion */
    while (!(ADCSRA & (1 << ADIF))) ;
    ADCSRA |= (1 << ADIF);
    uint32_t acc = 0;
    for (uint8_t i = 0; i < n; i++) {
        ADCSRA |= (1 << ADSC);
        while (!(ADCSRA & (1 << ADIF))) ;
        ADCSRA |= (1 << ADIF);
        acc += ADC;
    }
    return (uint16_t)(acc / n);
}

static int16_t adc_to_temp(uint16_t raw)
{
    int32_t t = (int32_t)raw * TEMP_SLOPE_NUM / TEMP_SLOPE_DEN + g_offset;
    if (t < 0)   t = 0;
    if (t > 999) t = 999;
    return (int16_t)t;
}

/* ===================================================================== */
/*  Hardware init (values from the reference hw_init @0x06B0)             */
/* ===================================================================== */
static void hw_init(void)
{
    PORTB = 0x36;  DDRB = 0xCB;   /* display digits + heater out, buttons in  */
    PORTC = 0x7E;  DDRC = 0x00;   /* all port C inputs (PC0 = ADC, rest pull) */
    PORTD = 0xFF;  DDRD = 0xFF;   /* display segments                          */

    TCCR0 = 0x05;                 /* clk/1024 -> ~100 Hz control tick */
    TCNT0 = 0xB2;

    TCCR1A = 0xC3;                /* Fast-PWM 10-bit, OC1A inverting -> heater */
    TCCR1B = 0x04;                /* clk/256 */
    TCNT1  = 0x0000;
    ICR1   = 0x0000;
    OCR1A  = 0x0000;              /* heater off */
    OCR1B  = 0x0000;

    ASSR  = 0x00;                 /* Timer2 -> ~1 kHz display multiplex */
    TCCR2 = 0x04;
    TCNT2 = 0x83;
    OCR2  = 0x00;

    MCUCR = 0x00;
    TIMSK = 0x41;                 /* TOIE2 | TOIE0 */
    ACSR  = 0x80;                 /* analog comparator off */
    SFIOR = 0x00;
    ADMUX  = 0x00;
    ADCSRA = 0x87;                /* ADEN, prescaler /128 (polled, not IRQ) */

    sei();
}

/* ===================================================================== */
/*  Interrupts                                                            */
/* ===================================================================== */
ISR(TIMER0_OVF_vect)              /* ~100 Hz base tick */
{
    TCNT0 = 0xB2;
    tick_flag = 1;
}

ISR(TIMER2_OVF_vect)              /* ~1 kHz display multiplex */
{
    TCNT2 = 0x83;
    disp_multiplex();
}

/* ===================================================================== */
/*  Buttons: step ±1 on press, autorepeat after a short hold              */
/* ===================================================================== */
static uint8_t button_repeat(uint8_t pressed, uint16_t *held)
{
    if (!pressed) { *held = 0; return 0; }
    uint16_t h = ++(*held);
    if (h == 1)            return 1;
    if (h < REPEAT_DELAY)  return 0;
    return (h % REPEAT_PERIOD) == 0;
}

static int8_t poll_buttons(void)
{
    static uint16_t up_hold, dn_hold;
    int8_t step = 0;
    if (button_repeat(btn_up_pressed(),   &up_hold)) step += 1;
    if (button_repeat(btn_down_pressed(), &dn_hold)) step -= 1;
    return step;
}

static void apply_setpoint_step(int8_t step)
{
    if (!step) return;
    setpoint += step;
    if (setpoint < TEMP_MIN) setpoint = TEMP_MIN;
    if (setpoint > TEMP_MAX) setpoint = TEMP_MAX;
    adjusting = 1;
    input_idle = 0;
    sp_dirty = 1;
}

static void save_setpoint_if_dirty(void)
{
    if (sp_dirty) {
        eeprom_write_word(EE_SETPOINT, (uint16_t)setpoint);
        sp_dirty = 0;
    }
}

/* ===================================================================== */
/*  PID heater controller (PWM on OC1A), 20 Hz                            */
/* ===================================================================== */
static int32_t heat_integral;
static int16_t thist[DHIST];
static uint8_t thidx;

static void heater_reset(void)
{
    heat_integral = 0;
    for (uint8_t i = 0; i < DHIST; i++) thist[i] = temp_real;
    thidx = 0;
}

static void update_heater(void)
{
    int16_t err = setpoint - temp_real;

    /* D: derivative on the measurement over the DHIST window */
    int16_t oldt  = thist[thidx];
    thist[thidx]  = temp_real;
    thidx         = (uint8_t)((thidx + 1) % DHIST);
    int16_t dtemp = temp_real - oldt;

    int32_t out = (int32_t)err * g_kp
                + (heat_integral * g_ki) / 2048
                - (int32_t)g_kd * dtemp;

    /* Anti-windup: integrate only while the output is not saturated. */
    if (out > 0 && out < 1023) {
        heat_integral += err;
        if (heat_integral < 0)            heat_integral = 0;
        if (heat_integral > HEAT_INT_MAX) heat_integral = HEAT_INT_MAX;
        out = (int32_t)err * g_kp
            + (heat_integral * g_ki) / 2048
            - (int32_t)g_kd * dtemp;
    }

    if (out < 0)    out = 0;
    if (out > 1023) out = 1023;
    HEATER_PWM = (uint16_t)out;
}

/* ===================================================================== */
/*  Display: setpoint while adjusting / at boot, otherwise the real temp   */
/*  with the last dp blinking proportionally to the heater PWM.            */
/* ===================================================================== */
static void update_display(void)
{
    static uint16_t dp_cnt;
    static uint8_t  dp_state;

    if (adjusting || boot_timer < T_1S) {
        disp_set_number(setpoint);
        disp_set_all_dp(0);
    } else {
        disp_set_number(temp_real);
        disp_set_all_dp(0);
        uint16_t pwm  = OCR1A;
        uint16_t half = DP_PWM_SLOW -
                        (uint16_t)(((uint32_t)pwm * (DP_PWM_SLOW - DP_PWM_FAST)) / 1023);
        if (half < DP_PWM_FAST) half = DP_PWM_FAST;
        if (++dp_cnt >= half) { dp_cnt = 0; dp_state ^= 1; }
        disp_set_dp(dp_state);
    }
}

/* ===================================================================== */
/*  Control step (20 Hz): sample the temperature and drive the heater      */
/* ===================================================================== */
static void control_step(void)
{
    raw_thermo = read_adc_avg(ADC_CH_THERMO, 16);
    temp_real  = adc_to_temp(raw_thermo);
    /* Hard over-temperature protection (also guards a mis-calibrated sensor). */
    if (temp_real >= TEMP_ABS_MAX) heater_off();
    else                           update_heater();
}

/* Base 100 Hz tick */
static void task_10ms(void)
{
    if (boot_timer < 0xFFFF) boot_timer++;
    input_idle++;

    apply_setpoint_step(poll_buttons());
    if (adjusting && input_idle >= T_3S) {
        adjusting = 0;
        save_setpoint_if_dirty();     /* remember the temperature */
    }

    static uint8_t sub;
    if (++sub >= CTRL_DIV) { sub = 0; control_step(); }

    update_display();
}

/* ===================================================================== */
/*  Settings: clamp / access / EEPROM  (item: 0=P 1=I 2=d 3=OF)            */
/* ===================================================================== */
static int16_t clamp_param(uint8_t item, int16_t v)
{
    switch (item) {
    case 0: if (v < KP_MIN) v = KP_MIN; if (v > KP_MAX) v = KP_MAX; break;
    case 1: if (v < KI_MIN) v = KI_MIN; if (v > KI_MAX) v = KI_MAX; break;
    case 2: if (v < KD_MIN) v = KD_MIN; if (v > KD_MAX) v = KD_MAX; break;
    default:if (v < OFFSET_MIN) v = OFFSET_MIN; if (v > OFFSET_MAX) v = OFFSET_MAX; break;
    }
    return v;
}

static int16_t get_param(uint8_t item)
{
    switch (item) {
    case 0:  return g_kp;
    case 1:  return g_ki;
    case 2:  return g_kd;
    default: return g_offset;
    }
}

static void set_param(uint8_t item, int16_t v)
{
    switch (item) {
    case 0:  g_kp = v; break;
    case 1:  g_ki = v; break;
    case 2:  g_kd = v; break;
    default: g_offset = v; break;
    }
}

static void save_param(uint8_t item)
{
    switch (item) {
    case 0:  eeprom_write_word(EE_KP,     (uint16_t)g_kp);     break;
    case 1:  eeprom_write_word(EE_KI,     (uint16_t)g_ki);     break;
    case 2:  eeprom_write_word(EE_KD,     (uint16_t)g_kd);     break;
    default: eeprom_write_word(EE_OFFSET, (uint16_t)g_offset); break;
    }
}

static void load_settings(void)
{
    uint16_t w;
    w = eeprom_read_word(EE_SETPOINT);
    setpoint = (w == EE_BLANK) ? TEMP_DEFAULT : (int16_t)w;
    if (setpoint < TEMP_MIN || setpoint > TEMP_MAX) setpoint = TEMP_DEFAULT;

    w = eeprom_read_word(EE_KP);     g_kp     = (w == EE_BLANK) ? DEF_KP     : (int16_t)w;
    w = eeprom_read_word(EE_KI);     g_ki     = (w == EE_BLANK) ? DEF_KI     : (int16_t)w;
    w = eeprom_read_word(EE_KD);     g_kd     = (w == EE_BLANK) ? DEF_KD     : (int16_t)w;
    w = eeprom_read_word(EE_OFFSET); g_offset = (w == EE_BLANK) ? DEF_OFFSET : (int16_t)w;

    g_kp     = clamp_param(0, g_kp);
    g_ki     = clamp_param(1, g_ki);
    g_kd     = clamp_param(2, g_kd);
    g_offset = clamp_param(3, g_offset);
}

/* ===================================================================== */
/*  Button gestures for the menu: tap on release, long press at 2 s        */
/* ===================================================================== */
enum { EV_NONE, EV_UP, EV_DN, EV_UP_LONG, EV_DN_LONG };
#define LONG_TICKS 200

static uint8_t get_gesture(void)
{
    static uint8_t  up_p, dn_p, up_long, dn_long;
    static uint16_t up_c, dn_c;
    uint8_t ev = EV_NONE;
    uint8_t up = btn_up_pressed();
    uint8_t dn = btn_down_pressed();

    if (up) {
        up_c++;
        if (up_c >= LONG_TICKS && !up_long) { up_long = 1; ev = EV_UP_LONG; }
    } else {
        if (up_p && !up_long && up_c > 1) ev = EV_UP;
        up_c = 0; up_long = 0;
    }
    up_p = up;

    if (dn) {
        dn_c++;
        if (dn_c >= LONG_TICKS && !dn_long) { dn_long = 1; ev = EV_DN_LONG; }
    } else {
        if (dn_p && !dn_long && dn_c > 1) ev = EV_DN;
        dn_c = 0; dn_long = 0;
    }
    dn_p = dn;

    return ev;
}

static void show_menu_label(uint8_t item)
{
    switch (item) {
    case 0:  disp_set_raw(GL_P, GL_BLANK, GL_BLANK); break;   /* P   */
    case 1:  disp_set_raw(GL_I, GL_BLANK, GL_BLANK); break;   /* I   */
    case 2:  disp_set_raw(GL_d, GL_BLANK, GL_BLANK); break;   /* d   */
    case 3:  disp_set_raw(GL_O, GL_F, GL_BLANK);     break;   /* OF  */
    case 4:  disp_set_raw(GL_A, GL_d, GL_BLANK);     break;   /* Ad  */
    case 5:  disp_set_raw(GL_t, GL_S, GL_t);         break;   /* TST */
    default: disp_set_raw(GL_A, GL_t, GL_BLANK);     break;   /* AT  */
    }
}

/* ===================================================================== */
/*  PID relay auto-tune (see hot-air/docs/FIRMWARE.md for the theory).     */
/*  Returns 1 on success (P/I/d written & saved), 0 on abort/failure.      */
/*  SAFETY: this is the only CAL action that heats the iron.               */
/* ===================================================================== */
static uint8_t auto_tune(void)
{
    TCCR1A = 0xC3; TCCR1B = 0x04; OCR1A = 0;   /* enable heater PWM */

    while (btn_up_pressed() || btn_down_pressed())
        if (tick_flag) tick_flag = 0;

    uint8_t  relay_high = 1, sub = 0, result = 0;
    int16_t  tmax = -1000, tmin = 1000;
    uint16_t step = 0, cycle_start = 0, cyc = 0;
    uint32_t sum_pu = 0;
    int32_t  sum_a = 0;

    for (;;) {
        if (!tick_flag) continue;
        tick_flag = 0;
        if (++sub < CTRL_DIV) continue;
        sub = 0;
        step++;

        if (get_gesture() == EV_DN_LONG) { result = 0; break; }

        int16_t t = adc_to_temp(read_adc_avg(ADC_CH_THERMO, 16));
        if (t > AT_TEMP_MAX || step > AT_TIMEOUT) { result = 0; break; }

        if (t > tmax) tmax = t;
        if (t < tmin) tmin = t;

        uint8_t prev = relay_high;
        if      (t > AT_TEMP + AT_HYST) relay_high = 0;
        else if (t < AT_TEMP - AT_HYST) relay_high = 1;
        HEATER_PWM = relay_high ? AT_RELAY_HIGH : AT_RELAY_LOW;

        if (relay_high && !prev) {
            if (cyc >= 1) {
                uint16_t pu  = step - cycle_start;
                int16_t  amp = tmax - tmin;
                if (cyc > AT_SKIP) { sum_pu += pu; sum_a += amp; }
            }
            cycle_start = step;
            tmax = tmin = t;
            cyc++;
            if (cyc > AT_SKIP + AT_USE) { result = 1; break; }
        }

        disp_set_number(t);
        disp_set_all_dp((step / 10) & 1);    /* blink all dp -> tuning */
    }

    HEATER_PWM = 0;

    if (result) {
        uint16_t pu = (uint16_t)(sum_pu / AT_USE);
        int16_t  a  = (int16_t)((sum_a / AT_USE) / 2);
        if (a  < 1) a  = 1;
        if (pu < 1) pu = 1;
        /* Tyreus-Luyben with AT_D=200, dt=0.05s, Tw=1.6s, Iscale=2048 */
        g_kp = clamp_param(0, (int16_t)(116L / a));
        g_ki = clamp_param(1, (int16_t)(107760L / ((int32_t)a * pu)));
        g_kd = clamp_param(2, (int16_t)((574L * pu) / (1000L * a)));
        save_param(0); save_param(1); save_param(2);
    }
    return result;
}

/* ===================================================================== */
/*  Calibration menu. The heater is FORCED OFF except inside AT.           */
/*  Tree: [P] [I] [d] [OF] [Ad] [TST] [AT].                                */
/*  UP-2s = enter, DN-2s = back/exit, short UP/DN = navigate / value ±1.    */
/* ===================================================================== */
static void cal_mode(void)
{
    TCCR1A = 0x00;                /* disconnect OC1A -> heater cannot fire */
    PORTB |= (1 << PB1);
    HEATER_PWM = 0;

    disp_set_raw(GL_C, GL_A, GL_L);            /* "CAL" for ~1 s */
    for (uint16_t t = 0; t < 100; )
        if (tick_flag) { tick_flag = 0; t++; }
    while (btn_up_pressed() || btn_down_pressed())
        if (tick_flag) tick_flag = 0;

    uint8_t level = 0, item = 0;
    int16_t val = 0;

    for (;;) {
        if (!tick_flag) continue;
        tick_flag = 0;

        HEATER_PWM = 0;
        PORTB |= (1 << PB1);

        uint8_t ev = get_gesture();

        if (level == 0) {                       /* --- ROOT --- */
            if      (ev == EV_UP)      item = (uint8_t)((item + 1) % NUM_ITEMS);
            else if (ev == EV_DN)      item = (uint8_t)((item + NUM_ITEMS - 1) % NUM_ITEMS);
            else if (ev == EV_UP_LONG) {
                if      (item == ITEM_TST) level = 2;
                else if (item == ITEM_ADC) level = 3;
                else if (item == ITEM_AT) {
                    uint8_t ok = auto_tune();
                    TCCR1A = 0x00; PORTB |= (1 << PB1); HEATER_PWM = 0;
                    if (ok) disp_set_raw(GL_A, GL_t, GL_BLANK);
                    else    disp_show_dashes();
                    for (uint16_t w = 0; w < 150; )
                        if (tick_flag) { tick_flag = 0; w++; }
                    while (btn_up_pressed() || btn_down_pressed())
                        if (tick_flag) tick_flag = 0;
                }
                else { val = get_param(item); level = 1; }
            }
            else if (ev == EV_DN_LONG) return;  /* exit calibration */
            show_menu_label(item);
        } else if (level == 1) {                /* --- EDIT --- */
            if      (ev == EV_UP)      val = clamp_param(item, val + 1);
            else if (ev == EV_DN)      val = clamp_param(item, val - 1);
            else if (ev == EV_UP_LONG || ev == EV_DN_LONG) {
                set_param(item, val);
                save_param(item);
                level = 0;
            }
            disp_set_number(val);
        } else if (level == 2) {                /* --- DISPLAY TEST --- */
            disp_set_raw(GL_ALL, GL_ALL, GL_ALL);
            if (ev == EV_UP_LONG || ev == EV_DN_LONG) level = 0;
        } else {                                /* --- LIVE RAW ADC --- */
            static uint8_t sub;
            if (++sub >= CTRL_DIV) {            /* refresh at 20 Hz */
                sub = 0;
                disp_set_number((int16_t)read_adc_avg(ADC_CH_THERMO, 16));
            }
            if (ev == EV_UP_LONG || ev == EV_DN_LONG) level = 0;
        }
    }
}

/* ===================================================================== */
int main(void)
{
    hw_init();
    load_settings();

    /* Enter calibration: UP held for 2 s at power-on */
    if (btn_up_pressed()) {
        disp_set_raw(GL_C, GL_A, GL_L);
        uint8_t ok = 1;
        for (uint16_t t = 0; t < LONG_TICKS; ) {
            if (tick_flag) {
                tick_flag = 0; t++;
                if (!btn_up_pressed()) { ok = 0; break; }
            }
        }
        if (ok) cal_mode();
        TCCR1A = 0xC3; TCCR1B = 0x04; OCR1A = 0;   /* restore heater PWM */
        while (btn_up_pressed() || btn_down_pressed())
            if (tick_flag) tick_flag = 0;
    }

    raw_thermo = read_adc_avg(ADC_CH_THERMO, 16);
    temp_real  = adc_to_temp(raw_thermo);
    heater_reset();

    for (;;) {
        if (tick_flag) {
            tick_flag = 0;
            task_10ms();
        }
    }
}
