---
name: n32-swd-access-notes
description: How to actually get an SWD session on the canbox N32L406 - it sleeps (debug clock gated) and its flash is RDP-locked
metadata:
  type: project
---

Bringing up SWD on the Hiworld canbox MCU (N32L406CBL7, Cortex-M4) with a
Raspberry Pi Pico running `debugprobe_on_pico.uf2` + pyocd. Confirmed
2026-10-10. See [[canbox-mcu-is-nations-n32]].

**Probe/target wiring (GPIO numbers, not header positions):**
box nRST->GP1, box SWDCLK->GP2, box SWDIO->GP3, GND->GND. Box on its own PSU;
do NOT also feed Pico 3V3. pyocd target override: `-t cortex_m` (no N32 pack
exists anywhere - Keil index has zero Nationstech vendors).

**Gotcha 1 - it sleeps.** The N32L40x gates the debug clock when idle, so SWD
gives `No ACK` within ~1 min of the last activity. A plain connect catches it
only right after power-up. Fix: a tight connect+halt retry loop while
power-cycling the box; the instant it halts, write `DBGMCU_CR` (0xE0042004) =
0x7 (DBG_SLEEP|DBG_STOP|DBG_STANDBY). After that the session is stable and
reconnects reliably. pyocd default connect halts the core, which also stops it
executing WFI.

**Gotcha 2 - flash is RDP-locked.** With a good session, flash
0x08000000-0x0801FFFF reads FAIL on every word, while SRAM (0x20000000, full
24K), bootROM (0x1FFF0000) and all core/periph regs read fine. `FLASH_OBR`
(0x4002201C) = 0x03FFFFFE => RDPRT set. Readout protection is ON. The only way
to drop it is a mass-erase (destroys flash) - no non-destructive unlock.

**Practical upshot:** cannot dump flash over SWD. But the box firmware is
already decompiled ([[box-firmware-decompiled]]) = same binary, so a dump is
unnecessary. Live debug (halt/step/breakpoint/SRAM/peripherals) works now.
Reflashing own image works (erase/program not read-blocked) but mass-erase
wipes the stock image first - keep the verified dump.

**Why:** a whole session was lost chasing stuck-reset / bad-ground / fried-chip
theories. The real causes were sleep + RDP, and both are diagnosable in
minutes with the memory-region probe table above. The chip returning a valid
CPUID (0x410FC241) and IDCODE (0x2BA01477) proves it is alive - don't suspect
damage when those read correctly.

**BootROM / NV region readability (verified 2026-10-10).** With a stable
session, exactly one non-volatile region outside SRAM is readable:
- Main flash 0x08000000: RDP-locked, all reads FAIL.
- Bootloader ROM 0x1FFF0000 (16K window): NOT readable - every read returns
  different bus artifacts (e.g. 0xE000ED10, 0x40004000, 0x40007000 repeated);
  two back-to-back reads never match. Do NOT trust any "bootrom" dump from this
  address. SRAM and the info page, read the same way in the same session, come
  back bit-identical - so it is the region, not the probe.
- Info/option page 0x1FFFF800 (2K): READABLE and stable. Saved to
  reference/infopage_1FFFF800_2k.bin
  (sha256 323adae2b1521fb65143077028315980354e7e430a5a4eb82958af24af6e5559).
  Option bytes: RDP=0xFF (valid complement 0x00) => read protection ON
  (unprotected would be 0xA5); user option byte at 0x1FFFF810 = 0x8F; rest FF.

RDP is now confirmed three independent ways: flash reads fail, FLASH_OBR
(0x4002201C)=0x03FFFFFE RDPRT set, and option-byte RDP=0xFF. Nothing short of a
mass-erase lifts it. Verified-read helper: re-read each word until two reads
agree; unmapped regions never converge.
