# Hiworld canbox — Nations N32L406 rebuild project

Reverse-engineered source, documentation, and tooling for the Hiworld car-radio CAN
adapter ("canbox") built on a **Nations N32L406CBL7** (ARM Cortex-M4F, 128 KB flash
@ `0x08000000`, 24 KB SRAM @ `0x20000000`, LQFP48). The goal of this folder is to
**prepare a rebuild of the APP** while keeping the original IAP bootloader intact.

## Why bootloader and app are separate

The flash has two independent programs:

| Region | Span | What | Rebuild? |
|---|---|---|---|
| **Bootloader** | `0x08000000`–`0x08001FFF` (8 KB) | Vendor IAP loader. Owns the vector table (`VTOR=0x08000000`, `MSP=0x20000600`). Receives `.iap` files over UART and flashes the app. | **NO — never overwrite.** It is what lets us flash new firmware over the radio. |
| **App** | `0x08002000`–`0x0801F7FF` (118 KB) | The actual canbox logic (CAN↔UART translation, Jeep WK profile). Delivered as the `.iap` payload. | **YES — this is what we rebuild.** |
| Param page | `0x0801F800`–`0x0801FFFF` (2 KB) | Emulated EEPROM (model/profile byte, settings). | No (runtime data). |

The app has **no vector table of its own** — it runs on the bootloader's. The bootloader
enters it via `bl 0x08002202` after checking a valid-app marker at `0x08001800`. A rebuilt
app must reproduce that entry convention and be packaged back into `.iap`. See
[`app/REBUILD.md`](app/REBUILD.md).

## Layout

```
README.md                  — this file
app/
  src/*.c                  — decompiled, NAMED C by subsystem (can, uart, gpio, tim, rcc, adc, …)
  src/can_rx_decode.c      — the two CAN profile decoders (83k = this car, 125k = FCA variant)
  include/                 — n32l406.h (umbrella), registers.h (MMIO bases), globals.h (RAM map)
  linker/app.ld            — linker script for the app region (see REBUILD.md)
  REBUILD.md               — how to rebuild & re-flash the app (entry convention, IAP, verify)
  MEMORY_MAP.md            — full flash + SRAM map
bootloader/
  src/*.c                  — decompiled bootloader (reference only; do not rebuild)
  src/n32l406.h
docs/                      — all analysis documents
  notes/                   — field/bench notes (RDP, SWD, IAP bench setup, profiles, …)
tools/
  iaptool.py               — pack/unpack/verify .iap containers
  iap_upload.py            — drive the bootloader over UART (reset→U→magic→records→E)
  dumper/                  — the SRAM-copy bootloader dumper (how the 8 KB loader was extracted)
ghidra/
  functions.tsv            — addr → current function name (272 functions)
  final_function_names.txt — snapshot of all names
  merged_globals.tsv / known_globals.tsv / slot2ram.tsv / mmio_slots.tsv — the global/pointer maps
  rename_hiworld.py        — re-applies the canonical names to a fresh Ghidra import
reference/
  bootloader_0x08000000_8k.bin   — original bootloader dump (sha256 60c8a757…)
  2429-H1N4JPF20A-220524.app.bin — original DECODED app image (the rebuild target to diff against)
  parampage_0x0801F800_2k.bin    — param page dump
```

## Source status (important)

`app/src/*.c` and `bootloader/src/*.c` are **Ghidra decompiler output with names applied**,
not yet clean compilable C. They are rebuild *scaffolding*: real function/global/register
names, correct addresses, and plate comments, but with Ghidra pseudo-types
(`undefined`, `uint`, `byte`) declared in `n32l406.h`, and `DAT_` left only where it points
to flash constant tables or dead code. Use them to reconstruct real C subsystem by subsystem,
diffing the rebuilt binary against `reference/2429-H1N4JPF20A-220524.app.bin`.

This box is **box 2429 / part H1N4JPF20A** (Jeep WK family, 83.333 kbit body bus). The 16-byte
`.iap` target magic for it is `01 09 07 08 00 02 01 07 91 32 47 a4` + `4e 33 14 f2` ("N3").

## Device facts
- MCU Nations N32L406CBL7, Cortex-M4F @ 64 MHz. STM32F1-like register map but NOT identical
  (e.g. ADC is at `0x40020800`, GPIO set/clear at `+0x18`/`+0x28`).
- Flash is **RDP Level 1** locked — can't be read over SWD, but flash-resident code can read it
  (that is how the bootloader was dumped; see `tools/dumper/` and `docs/notes/`).
- `.iap` has no signature — only a per-record checksum + the 16-byte magic — so a correctly
  repacked image is accepted. Full modify→flash→verify loop is proven (`docs/notes/iap-upload-pipeline-works.md`).
