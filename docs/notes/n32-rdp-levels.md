---
name: n32-rdp-levels
description: N32L40x read-protection model (from the official user manual) - at L1, flash-resident code CAN read all main flash incl the bootloader; no PCROP exists
metadata:
  type: reference
---

Authoritative, from `hiworld/EN_UM_N32L40x_Series_User_Manual_V2.6.0.pdf`
(§2.2.1.8 write protect, §2.2.1.9 read protection, Tables 2-4/2-5). Settles
whether the canbox bootloader can be dumped. See [[n32-swd-access-notes]],
[[canbox-mcu-is-nations-n32]].

**Three RDP levels (Table 2-5), set by RDP1/RDP2 option bytes:**
- L0 (unprotected): RDP1=0xA5 & nRDP1=0x5A. Everything readable.
- L1 (this chip): any config that is neither L0 nor L2. Flash reads via
  debugger / from SRAM are BLOCKED, but **"when the program is started from the
  main flash memory in non-debugging mode, the read operation of the main
  storage area is allowed"** - i.e. flash-resident user code can read ALL of
  main flash. Pages 0-1 auto write-protected. Debug port still works.
- L2: RDP2=0xCC & nRDP2=0x33. Adds protection on top of L1 AND disables the
  debug port entirely. Irreversible.

**This chip is L1, proven: SWD connects** (L2 would kill the debug port). So the
user's worry "RDP blocks code reads too" is true only at L2, not here.

**No PCROP / execute-only / per-region read protection exists on N32L40x.**
Table 2-4 lists the entire option-byte set: RDP1, RDP2, WRP0-3 (write, per
2-page unit), USER, USER2, Data0/1. Nothing can block app reads of one flash
region but not another. Read protection is purely the global RDP scheme (classic
STM32F1 model).

**Scope:** "main storage area" = 128 KB main flash @ 0x08000000 (incl the 8 KB
vendor IAP bootloader 0x08000000-0x08001FFF). SEPARATE from the "system memory
block (16 KB)" = the factory boot ROM @ 0x1FFF0000 (16 KB, "cannot be changed",
matches the unreadable region found over SWD) and the 2 KB system config block.

**Consequence:** the 8 KB vendor bootloader CAN be dumped non-destructively by a
flash-resident app delivered via the stock .iap, streaming it out the UART.
Caveat: reads only work for flash-resident code in non-debug mode - run the
dumper with SWD DETACHED, output via UART only. SWD-direct and SRAM-code reads
stay blocked (matches observed behaviour).

**Unlock = destruction:** rewriting RDP1 back to 0xA5 (L1->L0) triggers an
automatic full mass-erase of main flash (incl the bootloader). Erasing just the
option-byte block leaves 0xFF = still L1. So there is no way to drop protection
without wiping the only copy of the bootloader - dump it first.
