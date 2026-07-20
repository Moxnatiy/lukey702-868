/*
 * display.c - 7-segment display multiplexing, BDD-4301 (common ANODE).
 * Segment-to-PORTD mapping (from the Scorpio LUKEY868/702 schematic):
 *   PD0=a PD1=e PD2=d PD3=f PD4=dp PD5=c PD6=g PD7=b
 * Digits (common anodes driven directly via R18-R20 9.1 ohm, ACTIVE HIGH):
 *   DG1=PB0, DG2=PB7, DG3=PB6.
 *
 * Common anode: a segment is LIT when its PORTD output = 0 (cathode -> ground),
 * so we write the INVERSE of the "lit" segment mask to PORTD.
 */
#include <avr/io.h>
#include "display.h"

/* Segment bit positions within PORTD */
#define SEG_A  (1 << 0)
#define SEG_E  (1 << 1)
#define SEG_D  (1 << 2)
#define SEG_F  (1 << 3)
#define SEG_DP (1 << 4)
#define SEG_C  (1 << 5)
#define SEG_G  (1 << 6)
#define SEG_B  (1 << 7)

/* "Lit" segment masks for digits 0..9 (bit=1 -> segment on) */
static const uint8_t font[10] = {
    /* 0 */ SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F,          /* 0xAF */
    /* 1 */ SEG_B|SEG_C,                                   /* 0xA0 */
    /* 2 */ SEG_A|SEG_B|SEG_D|SEG_E|SEG_G,                 /* 0xC7 */
    /* 3 */ SEG_A|SEG_B|SEG_C|SEG_D|SEG_G,                 /* 0xE5 */
    /* 4 */ SEG_B|SEG_C|SEG_F|SEG_G,                       /* 0xE8 */
    /* 5 */ SEG_A|SEG_C|SEG_D|SEG_F|SEG_G,                 /* 0x6D */
    /* 6 */ SEG_A|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,           /* 0x6F */
    /* 7 */ SEG_A|SEG_B|SEG_C,                             /* 0xA1 */
    /* 8 */ SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,     /* 0xEF */
    /* 9 */ SEG_A|SEG_B|SEG_C|SEG_D|SEG_F|SEG_G,           /* 0xED */
};

/* Digit-select masks on PORTB. Common ANODE, direct drive via R18-R20 9.1 ohm
 * -> digit select is ACTIVE HIGH (pins 9,10,14 at 5V, per the doc P.S.). */
#define DIG1_MASK (1 << PB0)   /* DG1 = right digit (units)   */
#define DIG2_MASK (1 << PB7)   /* DG2 = middle (tens)         */
#define DIG3_MASK (1 << PB6)   /* DG3 = left digit (hundreds) */
/* Scan order as in the reference firmware: dbuf[0]->DG3(left), dbuf[2]->DG1(right) */
static const uint8_t digit_mask[3] = { DIG3_MASK, DIG2_MASK, DIG1_MASK };

static volatile uint8_t dbuf[3];   /* ready segment masks for the 3 digits */
static uint8_t cur_digit;          /* current digit in the multiplex scan  */

void disp_set_number(int16_t value)
{
    uint8_t dp = dbuf[2] & SEG_DP;         /* preserve the decimal point */
    if (value < 0)   value = 0;
    if (value > 999) value = 999;
    /* Compute the already-blanked hundreds digit BEFORE writing dbuf[0], so the
       multiplex ISR can never catch a transient un-blanked leading zero. */
    uint8_t hundreds = (value < 100) ? 0 : font[(value / 100) % 10];
    dbuf[0] = hundreds;                    /* DG3 (left)   - hundreds (blank if <100) */
    dbuf[1] = font[(value / 10)  % 10];    /* DG2 (middle) - tens     */
    dbuf[2] = font[ value        % 10] | dp;   /* DG1 (right) - units (+dp) */
}

void disp_show_dashes(void)
{
    dbuf[0] = dbuf[1] = dbuf[2] = SEG_G;   /* middle dash on all digits */
}

void disp_set_raw(uint8_t left, uint8_t mid, uint8_t right)
{
    dbuf[0] = left;    /* DG3 (left)   */
    dbuf[1] = mid;     /* DG2 (middle) */
    dbuf[2] = right;   /* DG1 (right)  */
}

void disp_set_dp(uint8_t on)
{
    if (on) dbuf[2] |=  SEG_DP;
    else    dbuf[2] &= (uint8_t)~SEG_DP;
}

void disp_set_all_dp(uint8_t on)
{
    if (on) {
        dbuf[0] |= SEG_DP; dbuf[1] |= SEG_DP; dbuf[2] |= SEG_DP;
    } else {
        dbuf[0] &= (uint8_t)~SEG_DP; dbuf[1] &= (uint8_t)~SEG_DP; dbuf[2] &= (uint8_t)~SEG_DP;
    }
}

void disp_multiplex(void)
{
    uint8_t seg = dbuf[cur_digit];
    /* 1) blank ALL digits (active high -> drive to 0) */
    PORTB &= (uint8_t)~(DIG1_MASK | DIG2_MASK | DIG3_MASK);
    /* 2) drive the segments of the current digit (inverted: 0 = lit) */
    PORTD = (uint8_t)~seg;
    /* 3) enable the current digit ONLY if it has something to show.
     *    A blank digit keeps its anode low, which prevents ghosting (a faint
     *    image bleeding onto an otherwise-dark digit during multiplexing). */
    if (seg) PORTB |= digit_mask[cur_digit];
    /* 4) advance to the next digit */
    if (++cur_digit >= 3) cur_digit = 0;
}
