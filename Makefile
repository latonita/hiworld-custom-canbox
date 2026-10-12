# Build scaffold for the Hiworld canbox APP (Nations N32L406, Cortex-M4F).
# STARTING POINT — not yet buildable: app/src/*.c is named Ghidra decompiler output that must be
# rewritten into clean C first (see AGENTS.md / app/REBUILD.md). Fill in SRCS as you port each
# subsystem, and resolve the float ABI + startup/crt0 (entry veneer MUST land at 0x08002202).

CROSS   ?= D:/IDE/arm-none-eabi-gcc/bin/arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# Cortex-M4F. NOTE: soft vs hard float is an OPEN QUESTION — match the original image.
# Start with soft-float; switch to "-mfpu=fpv4-sp-d16 -mfloat-abi=hard" if the original uses it.
CPU     := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
CFLAGS  := $(CPU) -Os -ffreestanding -fno-common -ffunction-sections -fdata-sections \
           -Wall -Iapp/include
LDFLAGS := $(CPU) -nostdlib -Tapp/linker/app.ld -Wl,--gc-sections -Wl,-Map=build/app.map

BUILD   := build
# Add sources here as you reconstruct them (order suggested in AGENTS.md):
SRCS    := \
  app/src/util.c \
  # app/src/rcc.c app/src/gpio.c app/src/tim.c app/src/can.c app/src/uart.c \
  # app/src/can_rx_decode.c app/src/sys.c ...

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))

# Box 2429 (H1N4JPF20A). iaptool reuses the original record-0 header (magic) from this .iap:
REF_IAP := reference/2429-H1N4JPF20A-220524.iap
IAPTOOL := tools/iaptool.py

.PHONY: all bin pack flash verify clean
all: $(BUILD)/app.elf bin

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/app.elf: $(OBJS) app/linker/app.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@
	$(SIZE) $@

bin: $(BUILD)/app.elf
	$(OBJCOPY) -O binary $(BUILD)/app.elf $(BUILD)/app.bin
	@echo "Compare against reference: reference/2429-H1N4JPF20A-220524.app.bin"

# Package the raw app image into a .iap (reuses the box's magic/key from an existing .iap header).
pack: bin
	python $(IAPTOOL) pack $(BUILD)/app.bin $(REF_IAP) -o $(BUILD)/app.iap
	python $(IAPTOOL) verify $(BUILD)/app.iap

# Flash over UART via the bootloader (set PORT=COMx). See app/REBUILD.md.
PORT ?= COM17
flash: pack
	python tools/iap_upload.py $(BUILD)/app.iap --port $(PORT) --reset nrst --verify

clean:
	rm -rf $(BUILD)
