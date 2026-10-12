---
name: canbox-mcu-is-nations-n32
description: The Hiworld canbox MCU is a Nations Technologies N32 part (not STM32/GD32); flash base 0x08000000, ~60 KB image
metadata:
  type: project
---

`tools/NationsMCUDownloadTool/` was fetched for this board - the canbox MCU is a
**Nations Technologies N32L406CBL7**, confirmed 2026-10-10 from the package
marking (`N32L406 CBL7 THF20015`, LQFP48). Chinese Cortex-M4F clone family,
NOT an STM32 or GD32.

- Cortex-M4F @ 64 MHz, **128 KB** flash at `0x08000000`, **24 KB** SRAM at
  `0x20000000`, **2 KB** page size, LQFP48. Has CAN - hence its use here.
- Nations markets the N32 flash as *encrypted*; readout protection is
  stronger than STM32 RDP level 1. Treat the existing dump as irreplaceable.

`SysConfig.ini` carries a previously configured read range:
`ReadStartAddr=08000000`, `ReadEndAddr=0800EF3F` = **61,248 bytes (~60 KB)**,
consistent with the firmware image already decompiled - see
[[box-firmware-decompiled]]. That is the *image* size, not the chip size: the
CBL7 holds 128 KB, so roughly half the flash is unused.

Pin names seen in that firmware (PB6, PC15) read as ST-style, which is
expected: N32 clones the STM32 peripheral map and GPIO naming.

**Why:** picking `target/stm32f1x.cfg` in OpenOCD would be wrong - the N32
flash controller and option-byte/readout-protection scheme differ, and
upstream OpenOCD has no N32 target support.
**How to apply:** flash work goes through the vendor tool or pyOCD + the
bundled `.FLM`. Never select a `*_UNLOCKOPT` device in NZDownloadTool - those
entries mass-erase the chip. Related: [[nations-tool-is-dap-v1-only]],
[[box-gpio-hardwired-lines]].
