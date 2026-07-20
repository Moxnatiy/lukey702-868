# Lukey 702/868 hot-air (fan) MCU  -  ATmega8L, internal RC 8 MHz
# Toolchain: avr-gcc  (e.g. PlatformIO's toolchain-atmelavr, or osx-cross/avr).
# Programmer: USBASP.  Fuses: lfuse=0x24, hfuse=0xD1 (EESAVE on).

MCU      = atmega8
F_CPU    = 8000000
PROG     = usbasp
TARGET   = fan
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
# Note: no separate .eep is generated - the setpoint is written to EEPROM at
# runtime (main() stores the 200 C default on first boot if EEPROM is blank).

# Flash OUR build (chip-erase first; also clears any read-lock)
flash: $(TARGET).hex
	avrdude -c $(PROG) -p m8 -B 8 -e -U flash:w:$(TARGET).hex:i

# Update ONLY flash, keeping EEPROM (requires EESAVE already programmed, hfuse=0xD1)
flash-keep: $(TARGET).hex
	avrdude -c $(PROG) -p m8 -B 4 -e -U flash:w:$(TARGET).hex:i

# Flash the reference firmware Luk702_fan_SE-2 (third-party, see original/)
flash-orig:
	avrdude -c $(PROG) -p m8 -B 8 -e \
	  -U flash:w:original/main.hex:i \
	  -U eeprom:w:original/main.eep:i

# Fuses: lfuse=0x24 (internal RC 8 MHz), hfuse=0xD1 (EESAVE=0 -> EEPROM survives
# chip-erase, so the calibration persists across firmware reflashes).
fuses:
	avrdude -c $(PROG) -p m8 -B 8 -U lfuse:w:0x24:m -U hfuse:w:0xd1:m

# Read the chip back (verification after flashing)
read:
	avrdude -c $(PROG) -p m8 -B 8 -U flash:r:readback.hex:i -U eeprom:r:readback.eep:i

# Regenerate the annotated disassembly of the reference firmware
disasm:
	python3 tools/avr_disasm.py original/main.hex > docs/disasm_raw.txt

clean:
	rm -f $(TARGET).elf $(TARGET).hex readback.*
# Note: fan.eep is a hand-written EEPROM image and is NOT removed by clean.
