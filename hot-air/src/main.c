/*
 * Lukey 702 / 868  --  HOT-AIR GUN MCU firmware (reverse-engineered + rewritten)
 * ---------------------------------------------------------------------------
 * ATmega8L-8PU, internal RC 8 MHz. Board: Scorpio LUKEY868(702) SMD rework.
 *
 * Hardware init uses the EXACT register values recovered from the reference
 * firmware (hw_init @0x0734). The control logic implements the official
 * "hot-air operating algorithm" (10 points) plus the pin-23 thermocouple
 * voltage-vs-temperature table from the documentation.
 *
 * F_CPU = 8 MHz (internal RC, CKSEL=0100). There is NO external oscillator.
 *
 * Pin map (confirmed against the schematic; details in docs/HARDWARE_MAP.md):
 *   Display:      segments on PORTD, digits DG1=PB0 DG2=PB7 DG3=PB6 (common anode).
 *   Heater:       PB1/OC1A (pin15) -> MOC3023 -> BTA16 triac. Active level = 0V (PWM).
 *   Thermocouple: OP07 -> PC0/ADC0 (pin23).
 *   Fan:          PC3 (pin26) - power/latch control (0V = running).
 *   Fan voltage:  PC5/ADC5 (pin28) - >0.4V means the fan is powered.
 *   "Power Up":   PB4 (pin18) - 0V = wand on stand, >4V = wand removed.
 *   Buttons:      UP=PB5(pin19), DOWN=PB2(pin16), active low.
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
#define FAN_PORT     PORTC
#define FAN_EN_PIN   PC3              /* 0V = running/latch, 5V = standby      */
#define STAND_PIN    PB4              /* pin18: 0=on stand, 1=removed          */
#define BTN_UP       PB5              /* pin19 */
#define BTN_DOWN     PB2              /* pin16 */
#define ADC_CH_THERMO  0             /* pin23: thermocouple (OP07)            */
#define ADC_CH_FAN     5             /* pin28: fan voltage                    */

#define is_off_stand()      (PINB & (1 << STAND_PIN))   /* wand removed from stand */
#define btn_up_pressed()    (!(PINB & (1 << BTN_UP)))
#define btn_down_pressed()  (!(PINB & (1 << BTN_DOWN)))
#define fan_hold_on()       (FAN_PORT &= (uint8_t)~(1 << FAN_EN_PIN))  /* PC3=0 */
#define fan_hold_off()      (FAN_PORT |=  (1 << FAN_EN_PIN))           /* PC3=5V */
#define heater_off()        (HEATER_PWM = 0)   /* pin15 -> 5V, heater off */

/* ===================================================================== */
/*  Temperature calibration                                               */
/*  The doc table is LINEAR; slope is taken from its two end points.       */
/*  ADC: REFS=00 (external AREF). Board: AREF ~ 2.5V (TL431).               */
/*  Raw counts = V / AREF * 1023.  At AREF=2.5V:                            */
/*      50°C -> 0.35V -> ~143      480°C -> 1.92V -> ~786                    */
/*  Slope 430°C / 643 counts. OFFSET was calibrated on hardware and can be  */
/*  refined in the CAL menu (item OF) -> runtime variable g_offset.         */
/* ===================================================================== */
#define TEMP_SLOPE_NUM 430
#define TEMP_SLOPE_DEN 643
/* OFFSET (OF) is editable in the CAL menu -> variable g_offset, default DEF_OFFSET. */

/* "Fan powered" threshold on ADC5: >0.4V (at AREF=2.5V ~164). */
#define FAN_ON_RAW   164

/* ===================================================================== */
/*  Control parameters / limits                                           */
/* ===================================================================== */
#define TEMP_MIN     100             /* hot-air setpoint limits, °C (per docs)   */
#define TEMP_MAX     480
#define TEMP_DEFAULT 200

#define REACHED_HYST 3               /* °C: "temperature reached" band           */

/* Defaults for the CAL-menu-editable parameters (stored in EEPROM). */
#define DEF_KP       22              /* P: proportional gain (out += err*KP)      */
#define DEF_KI       17              /* I: integral gain (term = integral*KI/2048)*/
#define DEF_KD        0              /* d: derivative gain (brakes on fast rise)  */
#define DEF_OFFSET   38              /* OF: ADC->°C offset (calibrated 2026-07)   */
#define DEF_LO       60              /* LO: cooldown-complete temperature         */
#define HEAT_INT_MAX 80000           /* anti-windup: integral accumulator limit   */
#define DHIST         8              /* derivative window: 8 steps @20Hz = 0.4 s  */

/* Parameter editing ranges */
#define KP_MIN 1
#define KP_MAX 99
#define KI_MIN 0
#define KI_MAX 99
#define KD_MIN 0
#define KD_MAX 99
#define OFFSET_MIN 0
#define OFFSET_MAX 150
#define LO_MIN 40
#define LO_MAX 120

#define NUM_ITEMS 5                  /* menu items: P I d OF LO */

/* Timings in 10 ms ticks (Timer0 ~100 Hz) */
#define T_3S         300
#define T_1S         100
#define REPEAT_DELAY  50             /* 0.5 s before autorepeat starts            */
#define REPEAT_PERIOD  5             /* then 20 steps/s (every 50 ms)             */
#define DP_BLINK      50             /* dp blink: toggle every 500 ms             */

/* Temperature sampling and heater control run at 20 Hz (every 5 ticks) */
#define CTRL_DIV      5

/* ===================================================================== */
/*  EEPROM map (16-bit words)                                             */
/* ===================================================================== */
#define EE_SETPOINT  ((uint16_t*)0x02)   /* temperature setpoint       */
#define EE_KP        ((uint16_t*)0x04)   /* P                          */
#define EE_KI        ((uint16_t*)0x06)   /* I                          */
#define EE_KD        ((uint16_t*)0x0C)   /* d                          */
#define EE_OFFSET    ((uint16_t*)0x08)   /* OF (ADC offset)            */
#define EE_LO        ((uint16_t*)0x0A)   /* LO (cutoff temperature)    */
#define EE_BLANK     0xFFFF              /* unwritten-cell marker      */

/* ===================================================================== */
/*  State                                                                 */
/* ===================================================================== */
typedef enum { ST_STANDBY, ST_WORK, ST_COOLDOWN } state_t;

static state_t   state;
static int16_t   setpoint;           /* target temperature, °C          */
/* Editable parameters (CAL menu) */
static int16_t   g_kp;               /* P */
static int16_t   g_ki;               /* I */
static int16_t   g_kd;               /* d */
static int16_t   g_offset;           /* OF: ADC->°C offset */
static int16_t   g_lo;               /* LO: cooldown-complete temperature */
static int16_t   temp_real;          /* measured temperature, °C        */
static uint16_t  raw_thermo;         /* raw thermocouple ADC            */
static uint16_t  raw_fan;            /* raw fan-voltage ADC             */

static uint16_t  entry_timer;        /* ticks since entering the state  */
static uint16_t  input_idle;         /* ticks since last button press   */
static uint8_t   adjusting;          /* currently showing/changing setpoint */
static uint8_t   at_temp;            /* temperature reached (maintaining)   */
static uint8_t   show_sp_on_standby; /* show setpoint when entering standby  */
static uint8_t   sp_dirty;           /* setpoint changed, needs EEPROM save  */

static volatile uint8_t tick_flag;   /* 10 ms flag from Timer0          */

/* ===================================================================== */
/*  ADC: averaged read of channel mux (n samples) - maximum quality.       */
/*  Prescaler /128 -> ADC clk 62.5 kHz (in the 50-200 kHz sweet spot).      */
/*  The first conversion after a channel change is discarded (settling).    */
/*  One conversion ~208 us; n=16 -> ~3.5 ms per temperature reading.        */
/* ===================================================================== */
static uint16_t read_adc_avg(uint8_t mux, uint8_t n)
{
    ADMUX = mux;                 /* external AREF, right-adjust, channel = mux */
    _delay_us(30);
    /* discard the first conversion (settling after channel switch) */
    ADCSRA |= (1 << ADSC);
    while (!(ADCSRA & (1 << ADIF))) ;
    ADCSRA |= (1 << ADIF);
    uint32_t acc = 0;
    for (uint8_t i = 0; i < n; i++) {
        ADCSRA |= (1 << ADSC);
        while (!(ADCSRA & (1 << ADIF))) ;
        ADCSRA |= (1 << ADIF);
        acc += ADC;              /* safe 16-bit read of ADCL+ADCH together */
    }
    return (uint16_t)(acc / n);
}

/* Linear conversion of raw ADC to °C: T = raw*slope + offset. */
static int16_t adc_to_temp(uint16_t raw)
{
    int32_t t = (int32_t)raw * TEMP_SLOPE_NUM / TEMP_SLOPE_DEN + g_offset;
    if (t < 0)   t = 0;
    if (t > 999) t = 999;
    return (int16_t)t;
}

/* ===================================================================== */
/*  Hardware init (exact register values from hw_init @0x0734)            */
/* ===================================================================== */
static void hw_init(void)
{
    /* Ports (directions/pull-ups as in the reference). PC3=0 -> latch power. */
    PORTB = 0x36;  DDRB = 0xCB;
    PORTC = 0x56;  DDRC = 0x08;
    PORTD = 0xFF;  DDRD = 0xFF;

    /* Timer0: clk/1024, reload 0xB2 -> overflow ~100 Hz (control tick) */
    TCCR0 = 0x05;
    TCNT0 = 0xB2;

    /* Timer1: Fast-PWM 10-bit (WGM=0011), clk/256, OC1A inverting -> heater.
       Inverting mode: pin15 is low for a fraction OCR1A/1023 -> heating. */
    TCCR1A = 0xC3;
    TCCR1B = 0x04;
    TCNT1  = 0x0000;
    ICR1   = 0x0000;
    OCR1A  = 0x0000;   /* heater off */
    OCR1B  = 0x0000;

    /* Timer2: clk/64, reload 0x83 -> overflow ~1 kHz (display multiplex) */
    ASSR  = 0x00;
    TCCR2 = 0x04;
    TCNT2 = 0x83;
    OCR2  = 0x00;

    MCUCR = 0x00;
    TIMSK = 0x41;      /* TOIE2 | TOIE0 */
    ACSR  = 0x80;      /* analog comparator disabled */
    SFIOR = 0x00;
    ADMUX  = 0x00;
    ADCSRA = 0x87;     /* ADEN, prescaler /128 -> 62.5 kHz (max 10-bit quality) */

    sei();
}

/* ===================================================================== */
/*  Interrupts                                                            */
/* ===================================================================== */
ISR(TIMER0_OVF_vect)          /* ~100 Hz: base control tick */
{
    TCNT0 = 0xB2;
    tick_flag = 1;
}

ISR(TIMER2_OVF_vect)          /* ~1 kHz: display multiplex */
{
    TCNT2 = 0x83;
    disp_multiplex();
}

/* ===================================================================== */
/*  Buttons: step ±1 on press; autorepeat after a short hold.             */
/*  Autorepeat for one button. *held is the held-ticks counter (10 ms).   */
/*  First step immediately; after REPEAT_DELAY, repeat at REPEAT_PERIOD.   */
/* ===================================================================== */
static uint8_t button_repeat(uint8_t pressed, uint16_t *held)
{
    if (!pressed) { *held = 0; return 0; }
    uint16_t h = ++(*held);
    if (h == 1)            return 1;   /* press edge: one step immediately */
    if (h < REPEAT_DELAY)  return 0;   /* 0.5 s pause before autorepeat    */
    return (h % REPEAT_PERIOD) == 0;   /* then 20 steps/s                  */
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
    sp_dirty = 1;                 /* save to EEPROM once editing settles */
}

/* Point 6: persist the setpoint to EEPROM (only when actually changed). */
static void save_setpoint_if_dirty(void)
{
    if (sp_dirty) {
        eeprom_write_word(EE_SETPOINT, (uint16_t)setpoint);
        sp_dirty = 0;
    }
}

/* ===================================================================== */
/*  PID heater controller (PWM on OC1A). Runs at 20 Hz in ST_WORK.         */
/*  P brakes early (wide band) -> less overshoot; I removes residual error; */
/*  D damps overshoot from the laggy thermocouple. Anti-windup clamps I.    */
/* ===================================================================== */
static int32_t heat_integral;        /* integral accumulator */
static int16_t thist[DHIST];         /* temperature history for the derivative */
static uint8_t thidx;

/* Reset the controller (on entering WORK): integral 0, history = current temp. */
static void heater_reset(void)
{
    heat_integral = 0;
    for (uint8_t i = 0; i < DHIST; i++) thist[i] = temp_real;
    thidx = 0;
}

static void update_heater(void)
{
    int16_t err = setpoint - temp_real;

    /* I: integrate the error (20 Hz) with anti-windup. Never below 0 (heater
       cannot cool). */
    heat_integral += err;
    if (heat_integral < 0)            heat_integral = 0;
    if (heat_integral > HEAT_INT_MAX) heat_integral = HEAT_INT_MAX;

    /* D: derivative on the MEASUREMENT over a ~0.4 s window (smooths the 1°C
       quantization). Brakes when temperature rises fast -> less overshoot
       given the laggy sensor. */
    int16_t oldt   = thist[thidx];
    thist[thidx]   = temp_real;
    thidx          = (uint8_t)((thidx + 1) % DHIST);
    int16_t dtemp  = temp_real - oldt;

    int32_t out = (int32_t)err * g_kp
                + (heat_integral * g_ki) / 2048
                - (int32_t)g_kd * dtemp;
    if (out < 0)    out = 0;
    if (out > 1023) out = 1023;
    HEATER_PWM = (uint16_t)out;

    at_temp = (temp_real >= setpoint - REACHED_HYST);   /* point 3: near setpoint */
}

/* ===================================================================== */
/*  Update the display according to the current state                     */
/* ===================================================================== */
static void update_display(void)
{
    static uint16_t blink;

    switch (state) {
    case ST_STANDBY:
        if (adjusting) {                      /* show setpoint while editing */
            disp_set_dp(0);
            disp_set_number(setpoint);
        } else if (show_sp_on_standby && entry_timer < T_3S) {
            disp_set_dp(0);
            disp_set_number(setpoint);        /* point 1: show setpoint for 3 s */
        } else {
            disp_show_dashes();               /* point 1: then "---"            */
        }
        break;

    case ST_WORK:
        if (adjusting) {                      /* point 4: show setpoint         */
            disp_set_dp(0);
            disp_set_number(setpoint);
        } else if (entry_timer < T_1S) {
            disp_set_dp(0);
            disp_set_number(setpoint);        /* point 2: show setpoint for 1 s */
        } else {
            disp_set_number(temp_real);       /* real temperature               */
            /* point 3: blink the dp on the least-significant digit when reached */
            if (at_temp) disp_set_dp((blink / DP_BLINK) & 1);
            else         disp_set_dp(0);
        }
        break;

    case ST_COOLDOWN:
        disp_set_dp(0);
        disp_set_number(temp_real);           /* show the wand cooling down      */
        break;
    }
    blink++;
}

/* ===================================================================== */
/*  State transition                                                      */
/* ===================================================================== */
static void enter_state(state_t s)
{
    save_setpoint_if_dirty();    /* don't lose a setpoint change on mode switch */
    state = s;
    entry_timer = 0;
    adjusting = 0;
    heat_integral = 0;          /* new mode - reset the PI integral */
    switch (s) {
    case ST_STANDBY:
        heater_off();
        /* PC3=5V: fan section power off (except the first standby at power-on) */
        break;
    case ST_WORK:
        fan_hold_on();          /* PC3=0: latch power + enable fan control */
        at_temp = 0;
        heater_reset();         /* clean controller start (integral + D history) */
        break;
    case ST_COOLDOWN:
        heater_off();           /* point 7: heater off, fan keeps cooling        */
        break;
    }
}

/* ===================================================================== */
/*  Temperature sampling + state machine. Called at 20 Hz.                 */
/* ===================================================================== */
static void control_step(void)
{
    /* Highest-quality read: 16-sample average for t°, 4 for fan voltage */
    raw_thermo = read_adc_avg(ADC_CH_THERMO, 16);
    temp_real  = adc_to_temp(raw_thermo);
    raw_fan    = read_adc_avg(ADC_CH_FAN, 4);

    switch (state) {
    case ST_STANDBY:
        heater_off();
        if (is_off_stand()) {                  /* point 2: wand removed from stand */
            show_sp_on_standby = 1;
            enter_state(ST_WORK);
        }
        break;

    case ST_WORK:
        if (!is_off_stand()) {                 /* point 7: wand put back on stand */
            enter_state(ST_COOLDOWN);
            break;
        }
        /* point 2: heat only while fan power is present (pin28 >0.4V) */
        if (raw_fan > FAN_ON_RAW)
            update_heater();
        else
            heater_off();
        break;

    case ST_COOLDOWN:
        heater_off();
        /* point 8: real t° fell to LO -> power off fan section, go to standby */
        if (temp_real <= g_lo) {
            fan_hold_off();                    /* PC3=5V */
            show_sp_on_standby = 0;            /* point 8: without showing setpoint */
            enter_state(ST_STANDBY);
        } else if (is_off_stand()) {           /* wand removed again -> back to work */
            enter_state(ST_WORK);
        }
        break;
    }
}

/* Base 100 Hz tick: buttons and display run fast, control runs at 20 Hz. */
static void task_10ms(void)
{
    entry_timer++;
    input_idle++;

    /* Buttons - every 10 ms (autorepeat responsiveness) */
    apply_setpoint_step(poll_buttons());
    if (adjusting && input_idle >= T_3S) {                 /* point 5: 3 s idle */
        adjusting = 0;
        save_setpoint_if_dirty();
    }

    /* Temperature + state machine - every CTRL_DIV ticks (20 Hz) */
    static uint8_t sub;
    if (++sub >= CTRL_DIV) { sub = 0; control_step(); }

    update_display();
}

/* ===================================================================== */
/*  Settings: range clamp, access, EEPROM persistence.                    */
/*  item: 0=P 1=I 2=d 3=OF 4=LO                                            */
/* ===================================================================== */
static int16_t clamp_param(uint8_t item, int16_t v)
{
    switch (item) {
    case 0: if (v < KP_MIN) v = KP_MIN; if (v > KP_MAX) v = KP_MAX; break;
    case 1: if (v < KI_MIN) v = KI_MIN; if (v > KI_MAX) v = KI_MAX; break;
    case 2: if (v < KD_MIN) v = KD_MIN; if (v > KD_MAX) v = KD_MAX; break;
    case 3: if (v < OFFSET_MIN) v = OFFSET_MIN; if (v > OFFSET_MAX) v = OFFSET_MAX; break;
    default:if (v < LO_MIN) v = LO_MIN; if (v > LO_MAX) v = LO_MAX; break;
    }
    return v;
}

static int16_t get_param(uint8_t item)
{
    switch (item) {
    case 0:  return g_kp;
    case 1:  return g_ki;
    case 2:  return g_kd;
    case 3:  return g_offset;
    default: return g_lo;
    }
}

static void set_param(uint8_t item, int16_t v)
{
    switch (item) {
    case 0:  g_kp = v; break;
    case 1:  g_ki = v; break;
    case 2:  g_kd = v; break;
    case 3:  g_offset = v; break;
    default: g_lo = v; break;
    }
}

static void save_param(uint8_t item)
{
    switch (item) {
    case 0:  eeprom_write_word(EE_KP,     (uint16_t)g_kp);     break;
    case 1:  eeprom_write_word(EE_KI,     (uint16_t)g_ki);     break;
    case 2:  eeprom_write_word(EE_KD,     (uint16_t)g_kd);     break;
    case 3:  eeprom_write_word(EE_OFFSET, (uint16_t)g_offset); break;
    default: eeprom_write_word(EE_LO,     (uint16_t)g_lo);     break;
    }
}

/* Load all settings from EEPROM (with defaults and range clamp). */
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
    w = eeprom_read_word(EE_LO);     g_lo     = (w == EE_BLANK) ? DEF_LO     : (int16_t)w;

    g_kp     = clamp_param(0, g_kp);
    g_ki     = clamp_param(1, g_ki);
    g_kd     = clamp_param(2, g_kd);
    g_offset = clamp_param(3, g_offset);
    g_lo     = clamp_param(4, g_lo);
}

/* ===================================================================== */
/*  Button gesture detector for the menu.                                 */
/*  A short tap is emitted on RELEASE; a long press (2 s) fires at 2 s.     */
/* ===================================================================== */
enum { EV_NONE, EV_UP, EV_DN, EV_UP_LONG, EV_DN_LONG };
#define LONG_TICKS 200               /* 2 s at 100 Hz */

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
        if (up_p && !up_long && up_c > 1) ev = EV_UP;   /* short tap on release */
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
    case 0:  disp_set_raw(GL_P, GL_BLANK, GL_BLANK); break;   /* P  */
    case 1:  disp_set_raw(GL_I, GL_BLANK, GL_BLANK); break;   /* I  */
    case 2:  disp_set_raw(GL_d, GL_BLANK, GL_BLANK); break;   /* d  */
    case 3:  disp_set_raw(GL_O, GL_F, GL_BLANK);     break;   /* OF */
    default: disp_set_raw(GL_L, GL_O, GL_BLANK);     break;   /* LO */
    }
}

/* ===================================================================== */
/*  Calibration menu. The triac is FORCED OFF (zero voltage to the heater).*/
/*  Tree: [P] [I] [d] [OF] [LO]. UP-2s = enter, DN-2s = back/exit,          */
/*  short UP/DN = navigate / change the value ±1.                          */
/* ===================================================================== */
static void cal_mode(void)
{
    /* SAFETY: disconnect OC1A from the pin and drive PB1 high -> opto LED off -> triac off */
    TCCR1A = 0x00;
    PORTB |= (1 << PB1);
    HEATER_PWM = 0;

    /* show "CAL" for ~1 s */
    disp_set_raw(GL_C, GL_A, GL_L);
    for (uint16_t t = 0; t < 100; )
        if (tick_flag) { tick_flag = 0; t++; }

    /* wait for UP release (it was used to enter) - so we don't fall into P */
    while (btn_up_pressed() || btn_down_pressed())
        if (tick_flag) tick_flag = 0;

    uint8_t level = 0;   /* 0=root, 1=editing */
    uint8_t item  = 0;
    int16_t val   = 0;

    for (;;) {
        if (!tick_flag) continue;
        tick_flag = 0;

        HEATER_PWM = 0;                 /* guarantee: heater stays off the whole time */
        PORTB |= (1 << PB1);

        uint8_t ev = get_gesture();

        if (level == 0) {               /* --- ROOT --- */
            if      (ev == EV_UP)      item = (uint8_t)((item + 1) % NUM_ITEMS);
            else if (ev == EV_DN)      item = (uint8_t)((item + NUM_ITEMS - 1) % NUM_ITEMS);
            else if (ev == EV_UP_LONG) { val = get_param(item); level = 1; }
            else if (ev == EV_DN_LONG) return;             /* exit calibration */
            show_menu_label(item);
        } else {                        /* --- EDIT --- */
            if      (ev == EV_UP)      val = clamp_param(item, val + 1);
            else if (ev == EV_DN)      val = clamp_param(item, val - 1);
            else if (ev == EV_UP_LONG || ev == EV_DN_LONG) {
                set_param(item, val);   /* long press - save and go back */
                save_param(item);
                level = 0;
            }
            disp_set_number(val);
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
        disp_set_raw(GL_C, GL_A, GL_L);        /* show CAL while held */
        uint8_t ok = 1;
        for (uint16_t t = 0; t < LONG_TICKS; ) {
            if (tick_flag) {
                tick_flag = 0; t++;
                if (!btn_up_pressed()) { ok = 0; break; }
            }
        }
        if (ok) cal_mode();
        /* on exit - restore the heater PWM (Timer1) */
        TCCR1A = 0xC3; TCCR1B = 0x04; OCR1A = 0;
        /* wait for buttons release so normal mode is not triggered */
        while (btn_up_pressed() || btn_down_pressed())
            if (tick_flag) tick_flag = 0;
    }

    /* Initial reading and start state (point 1: show setpoint 3 s -> "---") */
    raw_thermo = read_adc_avg(ADC_CH_THERMO, 16);
    temp_real  = adc_to_temp(raw_thermo);
    show_sp_on_standby = 1;
    fan_hold_on();               /* latch MCU power at startup */
    enter_state(ST_STANDBY);

    for (;;) {
        if (tick_flag) {
            tick_flag = 0;
            task_10ms();
        }
    }
}
