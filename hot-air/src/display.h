/*
 * display.h - 3-digit 7-segment display BDD-4301 (common ANODE).
 * Lukey 702/868. Segments on PORTD, digit selects DG1..DG3 on PB0/PB7/PB6.
 * Segment-to-PORTD mapping (from the Scorpio schematic):
 *   PD0=a PD1=e PD2=d PD3=f PD4=dp PD5=c PD6=g PD7=b
 */
#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdint.h>

/* Show an integer (0..999) on the 3 digits (leading zero of hundreds is blanked). */
void disp_set_number(int16_t value);

/* Show "---" (three middle dashes, segment g) - idle/standby indication. */
void disp_show_dashes(void);

/* Turn the decimal point (dp) of the least-significant digit on/off (at-temp blink). */
void disp_set_dp(uint8_t on);

/* Call from the Timer2 ISR (~1 kHz) - refreshes one digit per call. */
void disp_multiplex(void);

/* Set raw segment masks for the 3 digits (left=DG3, middle=DG2, right=DG1).
   Used for the menu / letters. Masks are in "lit" bits (1 = segment on). */
void disp_set_raw(uint8_t left, uint8_t mid, uint8_t right);

/* Segment bit positions within PORTD (Scorpio layout: PD0=a .. PD7=b) */
#define SG_A  0x01
#define SG_E  0x02
#define SG_D  0x04
#define SG_F  0x08
#define SG_DP 0x10
#define SG_C  0x20
#define SG_G  0x40
#define SG_B  0x80

/* Glyphs for the calibration menu */
#define GL_BLANK 0
#define GL_C (SG_A|SG_F|SG_E|SG_D)
#define GL_A (SG_A|SG_B|SG_C|SG_E|SG_F|SG_G)
#define GL_L (SG_D|SG_E|SG_F)
#define GL_P (SG_A|SG_B|SG_E|SG_F|SG_G)
#define GL_I (SG_B|SG_C)
#define GL_O (SG_A|SG_B|SG_C|SG_D|SG_E|SG_F)
#define GL_F (SG_A|SG_E|SG_F|SG_G)
#define GL_d (SG_B|SG_C|SG_D|SG_E|SG_G)

#endif
