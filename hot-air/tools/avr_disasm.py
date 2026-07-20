#!/usr/bin/env python3
"""
Minimal but fairly complete AVR disassembler for the classic ATmega8 core.
Reads an Intel-HEX flash image, prints an annotated disassembly.

Focus: correctness for the instruction subset a small ATmega8 firmware uses.
Annotates I/O accesses (IN/OUT/SBI/CBI/SBIC/SBIS) with ATmega8 register names,
resolves relative jumps/calls to absolute byte addresses, and marks the
interrupt vector table.

Usage: python3 avr_disasm.py main.hex
"""
import sys

# ---- ATmega8 I/O register map (I/O space address -> name) ----
# I/O address = data-space address - 0x20 for IN/OUT (0x00..0x3F)
IO = {
    0x00: "TWBR", 0x01: "TWSR", 0x02: "TWAR", 0x03: "TWDR",
    0x04: "ADCL", 0x05: "ADCH", 0x06: "ADCSRA", 0x07: "ADMUX",
    0x08: "ACSR", 0x09: "UBRRL", 0x0A: "UCSRB", 0x0B: "UCSRA",
    0x0C: "UDR", 0x0D: "SPCR", 0x0E: "SPSR", 0x0F: "SPDR",
    0x10: "PIND", 0x11: "DDRD", 0x12: "PORTD", 0x13: "PINC",
    0x14: "DDRC", 0x15: "PORTC", 0x16: "PINB", 0x17: "DDRB",
    0x18: "PORTB", 0x1B: "PORTA?",  # ATmega8 has no PORTA; keep placeholder
    0x1C: "EECR", 0x1D: "EEDR", 0x1E: "EEARL", 0x1F: "EEARH",
    0x20: "UBRRH/UCSRC", 0x21: "WDTCR", 0x22: "ASSR", 0x23: "OCR2",
    0x24: "TCNT2", 0x25: "TCCR2", 0x26: "ICR1L", 0x27: "ICR1H",
    0x28: "OCR1BL", 0x29: "OCR1BH", 0x2A: "OCR1AL", 0x2B: "OCR1AH",
    0x2C: "TCNT1L", 0x2D: "TCNT1H", 0x2E: "TCCR1B", 0x2F: "TCCR1A",
    0x30: "SFIOR", 0x31: "OSCCAL", 0x32: "TCNT0", 0x33: "TCCR0",
    0x34: "MCUCSR", 0x35: "MCUCR", 0x36: "TWCR", 0x37: "SPMCR",
    0x38: "TIFR", 0x39: "TIMSK", 0x3A: "GIFR", 0x3B: "GICR",
    0x3D: "SPL", 0x3E: "SPH", 0x3F: "SREG",
}

# ATmega8 interrupt vector table (word address -> source), each vector = 1 word (rjmp)
VECTORS = {
    0x000: "RESET", 0x001: "INT0", 0x002: "INT1", 0x003: "TIMER2_COMP",
    0x004: "TIMER2_OVF", 0x005: "TIMER1_CAPT", 0x006: "TIMER1_COMPA",
    0x007: "TIMER1_COMPB", 0x008: "TIMER1_OVF", 0x009: "TIMER0_OVF",
    0x00A: "SPI_STC", 0x00B: "USART_RXC", 0x00C: "USART_UDRE",
    0x00D: "USART_TXC", 0x00E: "ADC", 0x00F: "EE_RDY",
    0x010: "ANA_COMP", 0x011: "TWI", 0x012: "SPM_RDY",
}

def io_name(a):
    return IO.get(a, f"0x{a:02x}")

def load_ihex(fn):
    mem = {}
    for line in open(fn):
        line = line.strip()
        if not line.startswith(':'):
            continue
        b = bytes.fromhex(line[1:])
        ln, addr, typ = b[0], (b[1] << 8) | b[2], b[3]
        if typ == 0:
            for i in range(ln):
                mem[addr + i] = b[4 + i]
        elif typ == 1:
            break
    return mem

def word(mem, byte_addr):
    lo = mem.get(byte_addr, 0xFF)
    hi = mem.get(byte_addr + 1, 0xFF)
    return lo | (hi << 8)

def s(bits, val):
    """sign-extend val given bit-width"""
    if val & (1 << (bits - 1)):
        val -= (1 << bits)
    return val

def disasm_word(w, wnext, wa):
    """Return (text, size_in_words). wa = word address of this instruction."""
    # 32-bit instructions first
    # CALL: 1001 010k kkkk 111k  kkkk kkkk kkkk kkkk
    if (w & 0xFE0E) == 0x940E:
        k = ((w >> 4) & 0x1F) << 1 | (w & 1)
        k = (k << 16) | wnext
        return f"call  0x{k*2:04x}", 2
    # JMP: 1001 010k kkkk 110k
    if (w & 0xFE0E) == 0x940C:
        k = ((w >> 4) & 0x1F) << 1 | (w & 1)
        k = (k << 16) | wnext
        return f"jmp   0x{k*2:04x}", 2
    # LDS: 1001 000d dddd 0000  + k
    if (w & 0xFE0F) == 0x9000:
        d = (w >> 4) & 0x1F
        return f"lds   r{d}, 0x{wnext:04x}", 2
    # STS: 1001 001d dddd 0000  + k
    if (w & 0xFE0F) == 0x9200:
        d = (w >> 4) & 0x1F
        return f"sts   0x{wnext:04x}, r{d}", 2

    # ---- 16-bit instructions ----
    # NOP
    if w == 0x0000: return "nop", 1
    # MOVW 0000 0001 dddd rrrr (not on mega8, but harmless)
    if (w & 0xFF00) == 0x0100:
        d = ((w >> 4) & 0xF) * 2; r = (w & 0xF) * 2
        return f"movw  r{d}, r{r}", 1
    # MULS 0000 0010
    if (w & 0xFF00) == 0x0200:
        d = 16 + ((w >> 4) & 0xF); r = 16 + (w & 0xF)
        return f"muls  r{d}, r{r}", 1
    # 2-operand arithmetic/logic  ---- pattern: oooo oord dddd rrrr
    def rd_rr():
        d = (w >> 4) & 0x1F
        r = ((w >> 5) & 0x10) | (w & 0xF)
        return d, r
    op6 = w & 0xFC00
    m = {
        0x0400: "cpc", 0x0800: "sbc", 0x0C00: "add", 0x1000: "cpse",
        0x1400: "cp", 0x1800: "sub", 0x1C00: "adc", 0x2000: "and",
        0x2400: "eor", 0x2800: "or", 0x2C00: "mov",
    }
    if op6 in m:
        d, r = rd_rr()
        return f"{m[op6]:<5} r{d}, r{r}", 1
    # CPI/SBCI/SUBI/ORI/ANDI/LDI : oooo KKKK dddd KKKK, d in 16..31
    def rd16_k():
        d = 16 + ((w >> 4) & 0xF)
        k = ((w >> 4) & 0xF0) | (w & 0xF)
        return d, k
    m4 = {0x3000: "cpi", 0x4000: "sbci", 0x5000: "subi",
          0x6000: "ori", 0x7000: "andi", 0xE000: "ldi"}
    if (w & 0xF000) in m4:
        d, k = rd16_k()
        return f"{m4[w & 0xF000]:<5} r{d}, 0x{k:02x}", 1

    # LDD/STD with displacement (Y/Z)  10q0 qqsd dddd yqqq
    if (w & 0xD000) == 0x8000:
        d = (w >> 4) & 0x1F
        q = ((w >> 8) & 0x20) | ((w >> 7) & 0x18) | (w & 0x7)
        store = (w >> 9) & 1
        yz = 'Y' if (w >> 3) & 1 else 'Z'
        if store:
            return f"std   {yz}+{q}, r{d}", 1
        else:
            return f"ldd   r{d}, {yz}+{q}", 1

    # LD/ST variants 1001 00..
    if (w & 0xFC00) == 0x9000:
        d = (w >> 4) & 0x1F
        sub = w & 0xF
        store = (w >> 9) & 1
        mnem = "st" if store else "ld"
        table = {
            0x1: ("Z+", 'Z'), 0x2: ("-Z", 'Z'),
            0x9: ("Y+", 'Y'), 0xA: ("-Y", 'Y'),
            0xC: ("X", 'X'), 0xD: ("X+", 'X'), 0xE: ("-X", 'X'),
            0x0: ("(LDS32)", None), 0x4: ("lpm Z", None),
            0x5: ("lpm Z+", None), 0xF: ("PUSH/POP", None),
        }
        # POP/PUSH
        if sub == 0xF:
            if store:  # 1001 001d dddd 1111 = push
                return f"push  r{d}", 1
            else:      # 1001 000d dddd 1111 = pop
                return f"pop   r{d}", 1
        # LPM Rd,Z / Z+
        if not store and sub == 0x4:
            return f"lpm   r{d}, Z", 1
        if not store and sub == 0x5:
            return f"lpm   r{d}, Z+", 1
        if sub in (0x1, 0x2, 0x9, 0xA, 0xC, 0xD, 0xE):
            ptr = {0x1:"Z+",0x2:"-Z",0x9:"Y+",0xA:"-Y",0xC:"X",0xD:"X+",0xE:"-X"}[sub]
            if store:
                return f"st    {ptr}, r{d}", 1
            else:
                return f"ld    r{d}, {ptr}", 1

    # One-operand ALU 1001 010d dddd oooo, oooo in {0,1,2,3,5,6,7,A}
    if (w & 0xFE00) == 0x9400:
        d = (w >> 4) & 0x1F
        sub = w & 0xF
        one = {0x0:"com",0x1:"neg",0x2:"swap",0x3:"inc",0x5:"asr",
               0x6:"lsr",0x7:"ror",0xA:"dec"}
        if sub in one:
            return f"{one[sub]:<5} r{d}", 1

    # Bit/flag ops & special 1001 0100/0101 ...
    # SEx/CLx (SREG bit set/clear): 1001 0100 0sss 1000 (se) / 1001 0100 1sss 1000 (cl)
    if (w & 0xFF0F) == 0x9408:
        bit = (w >> 4) & 0x7
        names = ["sec","sez","sen","sev","ses","seh","set","sei"]
        return names[bit], 1
    if (w & 0xFF0F) == 0x9488:
        bit = (w >> 4) & 0x7
        names = ["clc","clz","cln","clv","cls","clh","clt","cli"]
        return names[bit], 1
    # RET/RETI/misc
    specials = {0x9508:"ret",0x9518:"reti",0x9588:"sleep",0x9598:"break",
                0x95A8:"wdr",0x95C8:"lpm",0x95E8:"spm",0x9509:"icall",
                0x9409:"ijmp",0x95F8:"espm"}
    if w in specials:
        return specials[w], 1

    # IN/OUT  1011 0AAd dddd AAAA (in) / 1011 1AAd dddd AAAA (out)
    if (w & 0xF800) == 0xB000:  # in
        d = (w >> 4) & 0x1F
        a = ((w >> 5) & 0x30) | (w & 0xF)
        return f"in    r{d}, {io_name(a)}", 1
    if (w & 0xF800) == 0xB800:  # out
        d = (w >> 4) & 0x1F
        a = ((w >> 5) & 0x30) | (w & 0xF)
        return f"out   {io_name(a)}, r{d}", 1

    # RJMP / RCALL 1100/1101 kkkk kkkk kkkk
    if (w & 0xF000) == 0xC000:
        k = s(12, w & 0xFFF)
        tgt = (wa + 1 + k) & 0xFFFF
        return f"rjmp  0x{tgt*2:04x}", 1
    if (w & 0xF000) == 0xD000:
        k = s(12, w & 0xFFF)
        tgt = (wa + 1 + k) & 0xFFFF
        return f"rcall 0x{tgt*2:04x}", 1

    # Branches 1111 00kk kkkk ksss (brbs) / 1111 01.. (brbc)
    if (w & 0xF800) == 0xF000 or (w & 0xF800) == 0xF400:
        kk = s(7, (w >> 3) & 0x7F)
        bit = w & 0x7
        tgt = (wa + 1 + kk) & 0xFFFF
        set_names = ["brcs","breq","brmi","brvs","brlt","brhs","brts","brie"]
        clr_names = ["brcc","brne","brpl","brvc","brge","brhc","brtc","brid"]
        if (w & 0x0400) == 0:
            return f"{set_names[bit]:<5} 0x{tgt*2:04x}", 1
        else:
            return f"{clr_names[bit]:<5} 0x{tgt*2:04x}", 1

    # SBRC/SBRS 1111 110d dddd 0bbb / 1111 111
    if (w & 0xFC08) == 0xFC00:
        d = (w >> 4) & 0x1F
        b = w & 0x7
        mnem = "sbrs" if (w >> 9) & 1 else "sbrc"
        return f"{mnem}  r{d}, {b}", 1
    # BLD/BST 1111 100d dddd 0bbb (bld) / 1111 101 (bst)
    if (w & 0xFC08) == 0xF800:
        d = (w >> 4) & 0x1F; b = w & 0x7
        return f"bld   r{d}, {b}", 1
    if (w & 0xFC08) == 0xFA00:
        d = (w >> 4) & 0x1F; b = w & 0x7
        return f"bst   r{d}, {b}", 1

    # SBIC/SBIS/SBI/CBI 1001 10oo AAAA Abbb
    if (w & 0xFF00) == 0x9800 or (w & 0xFF00) == 0x9900 or \
       (w & 0xFF00) == 0x9A00 or (w & 0xFF00) == 0x9B00:
        a = (w >> 3) & 0x1F
        b = w & 0x7
        op = (w >> 8) & 0x3
        mnem = {0:"cbi",1:"sbic",2:"sbi",3:"sbis"}[op]
        return f"{mnem}  {io_name(a)}, {b}", 1

    # MUL 1001 11rd dddd rrrr
    if (w & 0xFC00) == 0x9C00:
        d, r = rd_rr()
        return f"mul   r{d}, r{r}", 1

    # ADIW/SBIW 1001 0110/0111 KKdd KKKK
    if (w & 0xFF00) == 0x9600 or (w & 0xFF00) == 0x9700:
        k = ((w >> 2) & 0x30) | (w & 0xF)
        dp = (w >> 4) & 0x3
        d = [24, 26, 28, 30][dp]
        mnem = "adiw" if (w & 0xFF00) == 0x9600 else "sbiw"
        return f"{mnem}  r{d}, 0x{k:02x}", 1

    return f".word 0x{w:04x}", 1

def main():
    fn = sys.argv[1]
    mem = load_ihex(fn)
    last = max(mem)
    print(f"; Disassembly of {fn}  ({last+1} bytes, {(last+1)//2} words)")
    print(f"; Target: ATmega8, internal RC clock")
    print()
    wa = 0
    byte_end = last + 1
    while wa * 2 < byte_end:
        ba = wa * 2
        w = word(mem, ba)
        wnext = word(mem, ba + 2)
        text, size = disasm_word(w, wnext, wa)
        vec = ""
        if wa in VECTORS:
            vec = f"   ; <{VECTORS[wa]}>"
        # raw words
        if size == 2:
            raw = f"{w:04x} {wnext:04x}"
        else:
            raw = f"{w:04x}     "
        print(f"{ba:04x}:\t{raw}\t{text}{vec}")
        wa += size

if __name__ == "__main__":
    main()
