# Agent instructions — rebuilding the Hiworld canbox APP

You are picking up a reverse-engineering project to **rebuild the APP firmware** of a Hiworld
car-radio CAN adapter (Nations N32L406CBL7, Cortex-M4F) and re-flash it through the stock IAP
bootloader. Read `README.md` and `app/REBUILD.md` first. This file is the quick, directive brief.

## The goal
Produce a `.iap` file that, when flashed, behaves like the original — ideally reconstructing
clean, compilable C for the app, subsystem by subsystem, verified against the original image.

## Hard constraints (do NOT violate)
1. **Never modify, rebuild, or overwrite the bootloader** (`0x08000000`–`0x08001FFF`). It is the
   only way to flash the device over the radio. `bootloader/src/` is reference-only.
2. **App region is `0x08002000`–`0x0801F7FF`** (118 KB). Do not write the param page
   (`0x0801F800`–`0x0801FFFF`).
3. **The app has no vector table.** It runs on the bootloader's (`VTOR=0x08000000`,
   `MSP=0x20000600`).
4. **Entry + veneer layout is fixed.** The bootloader does `bl 0x08002202`, so the app's reset
   veneer MUST land at exactly `0x08002202` (offset `0x202`). Moreover, because the app shares the
   bootloader's vector table, interrupts dispatch into a **fixed veneer array** in the preamble
   (`~0x0800202a..0x08002201`, `veneer(IRQn) = 0x08002072 + 8*IRQn`) — so the whole
   `0x08002000..0x08002201` preamble is address-constrained, not just the entry word. A rebuild
   must reproduce that preamble/veneer layout, not only the entry. See `app/linker/app.ld` and
   `app/REBUILD.md` — this is the single most important layout constraint.
5. **Ship as `.iap`.** The rebuilt binary is packaged with `tools/iaptool.py` (reuses record-0
   header with box 2429's 16-byte magic) and flashed with `tools/iap_upload.py`. There is no
   signature — only per-record checksum + magic — so a correct repack is accepted.

## Source state
`app/src/*.c` is **Ghidra decompiler output with real names applied** (functions, `g_`/`p_`
globals, `REG_` MMIO bases, plate comments), not clean C yet. Pseudo-types live in
`app/include/n32l406.h`. Treat it as a faithful spec of behavior + addresses, and rewrite it
into real C — don't try to compile the raw decompile as-is.

## Recommended workflow
1. Toolchain is already installed: **xPack GNU Arm Embedded GCC 15.2.1** at `D:/IDE/arm-none-eabi-gcc/bin/` (NOT on PATH -- the `Makefile` defaults `CROSS` to this absolute path; override with `make CROSS=...` if it moves). It has Cortex-M soft-float multilib.
2. Reconstruct crt0 + the entry veneer so the entry point is at `0x08002202` and the preamble at
   `0x08002000` matches the original's purpose (compare bytes with
   `reference/2429-H1N4JPF20A-220524.app.bin`).
3. Port subsystems one at a time from `app/src/` (suggested order: `util` → `rcc`/`gpio`/`tim`
   HAL → `can` → `uart` → `can_rx_decode` → `sys`/`main_loop`). After each, link and **diff the
   produced `.bin` against the reference image** (functional equivalence first; byte-identity is
   a bonus, not required).
4. Package to `.iap` and flash+verify per `app/REBUILD.md` (read back the version string from
   SRAM `g_version_str` @ `0x20001b86`).

## Acceptance
- Builds with the committed toolchain; links within the app region; entry veneer at `0x08002202`.
- Packs to a `.iap` that `tools/iaptool.py verify` accepts.
- On-bench: flashes, boots, auto-bauds the CAN bus, and drives the head-unit UART (AA55) the same
  as stock. Compare behavior against `docs/hiworld-firmware-analysis.md` and the decoders in
  `app/src/can_rx_decode.c`.

## Open questions to resolve before trusting interrupts
- **VTOR / interrupt dispatch:** the app runs on the bootloader's vector table. Confirm from the
  startup code whether the app relocates `SCB->VTOR` to its own table or relies on the
  bootloader's vectors forwarding to fixed app ISR addresses. `app/src/sys.c` (startup) and
  `docs/hiworld-startup-sequence.md` are the places to settle this.
- **Float ABI** (soft vs hard) — infer from the reference image / original build.
- **Exact crt0 preamble** at `0x08002000`–`0x08002201`.

## Maps you will need
- `app/include/globals.h` — every RAM global (`g_*`) with its address.
- `app/include/registers.h` — MMIO peripheral bases (`REG_*`).
- `ghidra/functions.tsv` — address → function name for all 272 functions.
- `ghidra/rename_hiworld.py` — re-applies canonical names if you re-import into Ghidra.
