# Agent instructions — rebuilding the Hiworld canbox APP

You are picking up a reverse-engineering project to **rebuild the APP firmware** of a Hiworld
car-radio CAN adapter (Nations N32L406CBL7, Cortex-M4F) and re-flash it through the stock IAP
bootloader. Read `README.md` and `app/REBUILD.md` first. This file is the quick, directive brief.

## The goal
Produce a `.iap` file that, when flashed, behaves like the original — ideally reconstructing
clean, compilable C for the app, subsystem by subsystem, verified against the original image.

## Hard constraints (do NOT violate)
1. **Never modify, rebuild, or overwrite the bootloader** (`0x08000000`–`0x08001FFF`). It is the
   only way to flash the device over the radio. `reference/decompile/bootloader/` is reference-only.
2. **App region is `0x08002000`–`0x0801F7FF`** (118 KB). Do not write the param page
   (`0x0801F800`–`0x0801FFFF`).
3. **The app has no vector table.** It runs on the bootloader's (`VTOR=0x08000000`,
   `MSP=0x20000600`).
4. **Entry + veneer layout is fixed.** The bootloader does `bl 0x08002202`, so the app's reset
   veneer MUST land at exactly `0x08002202` (offset `0x202`). Moreover, because the app shares the
   bootloader's vector table, interrupts dispatch into a **fixed veneer array** in the preamble
   (`0x0800202a..0x08002201`, 59 slots of 8 bytes). The slot map is **not** the linear
   `0x08002072 + 8*IRQn` rule quoted in older notes — the array is in STM32F103 vector order
   while the chip is an N32L406, and the bootloader's table bridges the two; the real map is
   read off the bootloader dump and encoded in `app/startup/crt0.S`. So the whole
   `0x08002000..0x08002201` preamble is address-constrained, not just the entry word. A rebuild
   must reproduce that preamble/veneer layout, not only the entry. See `app/linker/app.ld` and
   `app/REBUILD.md` — this is the single most important layout constraint.
5. **Ship as `.iap`.** The rebuilt binary is packaged with `tools/iaptool.py` (reuses record-0
   header with box 2429's 16-byte magic) and flashed with `tools/iap_upload.py`. There is no
   signature — only per-record checksum + magic — so a correct repack is accepted.

## Source state
The rebuild **exists and builds**. It is three layers, and the contract between them is
`docs/ARCHITECTURE.md` — read that before touching any of it:

| Tree | Language | What |
|---|---|---|
| `app/startup/` | asm + C11 | the fixed preamble, 59 IRQ veneers, the `0x08002202` entry, RAM init |
| `app/hal/` | C11 | register drivers. MMIO only; every object has `data = bss = 0` |
| `app/services/` | C++17 | scheduler, clock, timers, parameter store, CAN transport, head-unit link (shared core plus one translation unit per wire dialect), board I/O, power |
| `app/logic/` | C++17 | CAN profile decoders, the Benz-emulation decoders and frame packer, periodic TX, cluster text, link state, main loop, interrupt entry points |

`reference/decompile/app/*.c` is **Ghidra decompiler output with real names applied** (functions, `g_`/`p_`
globals, `REG_` MMIO bases, plate comments). It does not compile and is not built. It is the
**behavioural spec**: the rebuild is written against it, the host tests are written from it,
and every rebuilt function carries the original flash address in a `/* 0x0800xxxx */`
comment so the two can be diffed. Pseudo-types live in `reference/decompile/app/n32l406.h`.

Application state lives in one typed struct, `ram::AppRam` in `app/include/ram_map.hpp`,
defined in `app/services/app_state.cpp`. It is ordinary `.bss`: the linker places it and
`startup.c` zeroes it. Nothing in the rebuild sits at a fixed SRAM address — the stock
firmware's absolute map was reproduced by earlier generations and has been dropped; each
field still notes the stock address it came from, as documentation. Use the named fields. The old
`app/include/globals.h` absolute-address macro map has been deleted; the address -> name key
for reading `reference/decompile/app/` is `ghidra/merged_globals.tsv`.

The C++ subset and every deviation from it are recorded in `docs/misra-cpp.md`. Two rules
bite hardest: this image **never runs `.init_array`** (the linker fails the build if a static
constructor appears), and the five interrupt handlers plus `fatal_error_reset` must keep
`extern "C"` and their exact names, because `app/startup/crt0.S` branches to them by name.

## Recommended workflow
1. Toolchain: **xPack GNU Arm Embedded GCC 15.2.1** at `D:/IDE/arm-none-eabi-gcc/bin/` (NOT on
   PATH — `tools/build.py` has it baked in; override with `$env:CROSS`). Soft-float multilib.
   A host C++ compiler (clang++ or g++) is needed for the unit tests, nothing else.
2. `python tools/build.py` — compiles, links, and runs `tools/verify_preamble.py`, which fails
   the build if the entry veneer moved or a dispatched veneer slot changed address.
   `python tools/build.py --pack` also produces `build/canupdate<version>.iap`; add
   `--protocol benz` for the Mercedes-emulation image (`docs/raise-benz-emulation.md`).
3. `python tools/test.py` — the host unit tests, run for BOTH protocols. Write new ones from the behaviour in
   `reference/decompile/app/*.c`, not from the rebuilt code: a test that restates the implementation proves
   nothing, and these tests are the only equivalence evidence available without a bench.
4. Flash and verify per `app/REBUILD.md` §6 (read back the version string from SRAM
   `ram::g_versionString`, whose address `iap_upload.py --verify` resolves from
   `build/app.elf` -- the rebuild no longer uses the stock firmware's fixed SRAM map).

When adding behaviour, put it in the layer that owns it (`docs/ARCHITECTURE.md` §1) and keep
the `/* 0x0800xxxx */` stock-address annotations: they are how the next reader diffs this
against Ghidra.

## Acceptance
- Builds with the committed toolchain; links within the app region; entry veneer at `0x08002202`.
- Packs to a `.iap` that `tools/iaptool.py verify` accepts.
- On-bench: flashes, boots, auto-bauds the CAN bus, and drives the head-unit UART (AA55) the same
  as stock. Compare behavior against `docs/hiworld-firmware-analysis.md` and the decoders in
  `reference/decompile/app/can_rx_decode.c`.

## Settled questions (do not re-litigate)
- **Interrupt dispatch under a shared VTOR** — resolved. IRQs vector into the veneer array via
  the bootloader's table; the slot map is read off `reference/bootloader_0x08000000_8k.bin`,
  encoded in `app/startup/crt0.S`, and enforced on every build by `tools/verify_preamble.py`.
  `CAN_RX0_IRQn = 54` on this part and the bootloader maps it to slot 29 — that is correct,
  not an anomaly. See `docs/notes/vector-table-slot-map.md`.
- **Float ABI** — `-mfloat-abi=soft`. The stock image contains no VFP instruction and no CPACR
  write (confirmed by disassembly).
- **The crt0 preamble** — hand-written and verified against the stock bytes.

Still open: nothing in the layout. The remaining risk is **behavioural**, and only a bench
can retire it — see `app/REBUILD.md` §6 and §7.

## Maps you will need
- `app/include/ram_map.hpp` — **the** RAM map: every global as a named, offset-asserted field.
- `ghidra/merged_globals.tsv` — address -> name key for reading the `reference/decompile/app/` decompile.
- `app/include/n32l406_hw.h` — typed peripheral register layouts, generated from the vendor SDK.
- `ghidra/functions.tsv` — address → function name for all 272 functions.
- `ghidra/rename_hiworld.py` — re-applies canonical names if you re-import into Ghidra.
