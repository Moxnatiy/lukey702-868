# Lukey 702/868 soldering-iron MCU  -  ATmega8L, internal RC 8 MHz
# Toolchain: avr-gcc.  Programmer: USBASP.
# Fuses: lfuse=0x24, hfuse=0xD1 (EESAVE on -> calibration survives a reflash).
#
# NOTE: the factory chip is read-locked (lock=0xFC). The first `make flash`
# performs a chip erase, which clears the lock and lets us program it.

MCU      = atmega8
F_CPU    = 8000000
PROG     = usbasp
TARGET   = iron
SRC      = src/main.c src/display.c

CC       = avr-gcc
OBJCOPY  = avr-objcopy
CFLAGS   = -mmcu=$(MCU) -DF_CPU=$(F_CPU)UL -Os -Wall -Wextra -std=gnu11 \
           -ffunction-sections -fdata-sections
LDFLAGS  = -Wl,--gc-sections

.PHONY: all clean flash flash-keep flash-orig fuses read disasm

all: $(TARGET).hex

$(TARGET).elf: $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	avr-size --format=avr --mcu=$(MCU) $@

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

# Flash our build (chip-erase first; also clears the factory read-lock)
flash: $(TARGET).hex
	avrdude -c $(PROG) -p m8 -B 8 -e -U flash:w:$(TARGET).hex:i

# Update only flash, keeping EEPROM (requires EESAVE, hfuse=0xD1)
flash-keep: $(TARGET).hex
	avrdude -c $(PROG) -p m8 -B 4 -e -U flash:w:$(TARGET).hex:i

# Flash the reference firmware Luk702_sir-SE (third-party, see original/)
flash-orig:
	avrdude -c $(PROG) -p m8 -B 8 -e \
	  -U flash:w:original/main.hex:i \
	  -U eeprom:w:original/main.eep:i

fuses:
	avrdude -c $(PROG) -p m8 -B 8 -U lfuse:w:0x24:m -U hfuse:w:0xd1:m

read:
	avrdude -c $(PROG) -p m8 -B 8 -U flash:r:readback.hex:i -U eeprom:r:readback.eep:i

disasm:
	python3 tools/avr_disasm.py original/main.hex > docs/disasm_raw.txt

clean:
	rm -f $(TARGET).elf $(TARGET).hex readback.*
