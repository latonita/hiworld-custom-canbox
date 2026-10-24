# Developer guide — Hiworld canbox, Nations N32L406 rebuild

Reverse-engineered source, documentation and tooling for the Hiworld car-radio CAN adapter
("canbox") built on a **Nations N32L406CBL7** (ARM Cortex-M4F, 128 KB flash @ `0x08000000`,
24 KB SRAM @ `0x20000000`, LQFP48). The goal of this tree is a **rebuild of the APP** while
keeping the original IAP bootloader intact.

User-facing summary (what the box exports, install steps): [`../README.md`](../README.md).

## Why it exists

The stock HiWorld `H1N4JPF20A` canbox on a Jeep Grand Cherokee WH/WK 3.0 CRD (2005) has two problems:

1. It decodes the wrong CAN messages for this car (profile 0 is a generic FCA decoder).
2. It forwards only the bare minimum needed to make the radio work (ACC, reverse, illumination, doors,
   steering angle; no speed, RPM, gear, temperatures, fuel, tyres).

So the box and its bootloader were reverse-engineered, the JPF firmware was rewritten, and a second
variant was added that mimics a Raise Benz17 canbox to expose more CAN data (RPM, temperatures, ...).

The build produces standard `.iap` containers. They load through the standard HiWorld canbox update
procedure: put the file on the head unit, then upload from the canbus settings / debug menu.

## Building it

Docker is the only prerequisite -- no cross toolchain, no Python, no compiler:

```bash
./build-docker.sh         # Linux / macOS / WSL / Git Bash
```
```powershell
.\build-docker.ps1       # Windows
```

Both `.iap` files land in `build/`, ready for the radio's update screen. To build
natively instead (arm-none-eabi GCC 15.x with C++ and binutils >= 2.39, plus
Python 3.8+ and any host C++ compiler for the tests), see [`../BUILD.md`](../BUILD.md).

Flashing is never containerised -- it needs the USB serial port and an SWD probe,
so `tools/iap_upload.py` runs on the host.

## Protocol variants

| `--protocol` | Version tag | What | Status |
|---|---|---|---|
| `jeep` (default) | `H1N4JPF20L-<ddmmyy>` | Faithful rebuild of the stock Hiworld dialect + the custom `0xB0`-`0xBD` telemetry block | The one in use |
| `benz` | `H1N4BNZ10L-<ddmmyy>` | Emulates a Raise Mercedes canbox (see [`raise-benz-emulation.md`](raise-benz-emulation.md)) | Alternative, not actively used |

Version string shape: `H1N4` (BSP family) `JPF2` (car profile, same token as Android protocol id
3009001) `0` (profile sub-revision) `L` (vendor image; stock is `A`) `-ddmmyy`. Only the date moves
between our builds. Authoritative comment: `app/include/app_config.hpp` (Box identity). Current
`kVersionString`: `H1N4JPF20L-241026` (jeep), `H1N4BNZ10L-171026` (benz).

Build history on the wire: `-171026` stock six slots only, `-211026` adds the telemetry block,
`-241026` adds `0xBC` fuel flow.

## Signal reference

| Document | What |
|---|---|
| [`../data-provided-JPF20L.md`](../data-provided-JPF20L.md) | Every data element the jeep build sends, by command and byte, with CAN source |
| [`CUSTOM-TX-SPEC.md`](CUSTOM-TX-SPEC.md) | The custom `0xB0`-`0xBD` block: payloads, validity mask, state machines; section 9 odometer/compass, section 10 fuel flow `0xBC` |
| [`TPMS-POLL-SPEC.md`](TPMS-POLL-SPEC.md) | `0xB7` tyre pressure: KWP2000 `21 3B` poll of the WCM every 30 s |
| [`REQUIREMENTS.md`](REQUIREMENTS.md) | Numbered requirements (FR-xx) for stock and custom behaviour |
| [`JEEP-WK-CAN-ID-byte-map.md`](JEEP-WK-CAN-ID-byte-map.md) | WK CAN ids/bytes and confidence levels |

## Some docs to read

- [`canbus-architecture.md`](canbus-architecture.md) — the data flow end to end:
  the car's buses, what this box is wired to and can actually see, and the UART link up to
  the head unit. Every claim is tagged VERIFIED / SOURCED / INFERRED.
- [`hiworld-iap-format.md`](hiworld-iap-format.md) — the `.iap` container itself:
  fixed-size checksummed records, the header, and the obfuscation key the file carries. This
  is what `tools/iaptool.py` packs and unpacks byte-for-byte identically.
- [`hiworld-iap-update-sequence.md`](hiworld-iap-update-sequence.md) — the update
  as it happens on the wire, both sides of it: the radio tells the running app to reset into
  the bootloader with `AA 55 02 E0 00 00 E1`, and the record-by-record handshake follows.
- [`hiworld-iap-flashing-flow.md`](hiworld-iap-flashing-flow.md) — the radio side of the update
  (APK/class level), including how the updater finds the `.iap` file.
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — layer contract and every module's public API.
- [`../app/REBUILD.md`](../app/REBUILD.md) — build + flash detail (entry convention, IAP, verify).
- [`hiworld-firmware-analysis.md`](hiworld-firmware-analysis.md) — decompiled stock app (what stock decodes and sends).
- [`misra-cpp.md`](misra-cpp.md) — the C++ subset and its recorded deviations.

Note: `hiworld-iap-format.md`, `hiworld-iap-update-sequence.md` and `canbus-data-flow.md` also exist
at the project root; the first two are identical copies of the `docs/` versions.

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
[`../app/REBUILD.md`](../app/REBUILD.md).

## Layout

```
README.md                  — user-facing page (what the box exports, install). Dev docs: docs/DEVELOPER.md
app/                       — THE REBUILD. Three layers; see docs/ARCHITECTURE.md.
  startup/crt0.S           — the fixed preamble: 59 IRQ veneers + the 0x08002202 entry
  startup/startup.c        — RAM init, then main_loop (replaces the ARM C-lib __main chain)
  hal/*.{c,h}              — C11 register drivers. MMIO only, no RAM state (data = bss = 0)
  services/*.{cpp,hpp}     — C++17 stateful services: scheduler, clock, timers, params,
                             CAN transport, head-unit link, board I/O, power
  logic/*.{cpp,hpp}        — C++17 vehicle behaviour: the two CAN profile decoders, the
                             periodic TX table, cluster text, link state, main loop,
                             and the five interrupt entry points
  include/ram_map.hpp      — all runtime state as one typed struct, linker-allocated;
                             each field notes the stock address it came from
  include/app_config.hpp   — clock tree, tick cadences, protocol constants, box identity
  include/n32l406_hw.h     — peripheral register layouts, generated from the vendor SDK
  linker/app.ld            — linker script for the app region (see REBUILD.md)
  REBUILD.md               — how to rebuild & re-flash the app (entry convention, IAP, verify)
  MEMORY_MAP.md            — full flash + SRAM map

  src/*.c                  — REFERENCE ONLY: decompiled, NAMED C by subsystem. This is the
                             behavioural spec the rebuild is written against, not built.
  src/can_rx_decode.c      — the two CAN profile decoders (83k = this car, 125k = FCA variant)
  include/n32l406.h        — Ghidra pseudo-types for reference/decompile/app/ only
                             (the old include/globals.h absolute-address macros are gone;
                             the address -> name key is ghidra/merged_globals.tsv)
  services/app_state.cpp   — the one definition of the firmware's runtime state
bootloader/
  src/*.c                  — decompiled bootloader (reference only; do not rebuild)
  src/n32l406.h
docs/                      — all analysis documents
  DEVELOPER.md             — this file
  notes/                   — field/bench notes (RDP, SWD, IAP bench setup, profiles, …)
tests/
  host/                    — host unit tests: the logic layers built for x86 against a
                             recording fake HAL. `python tools/test.py`
build-docker.sh / .ps1     — build in a container on a machine with no toolchain
docker/
  Dockerfile               — the builder image: pinned xPack arm-none-eabi GCC + host g++
  build-all.sh             — its entrypoint: tests, then both protocols, packed
tools/
  build.py                 — compile + link + verify the preamble (the primary build driver)
  test.py                  — build and run the host unit tests
  verify_preamble.py       — fails the build if the entry veneer or a veneer slot moved
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

There are two generations of source here, and only one of them is built.

**`app/hal/`, `app/services/`, `app/logic/`, `app/startup/` — the rebuild.** Real,
compilable C11 + C++17. This is what `python tools/build.py` turns into `build/app.bin`.
Layering and the public API of every module are in [`ARCHITECTURE.md`](ARCHITECTURE.md);
the C++ subset and its recorded deviations are in [`misra-cpp.md`](misra-cpp.md).

**`reference/decompile/app/*.c` and `reference/decompile/bootloader/*.c` — the spec.** Ghidra decompiler output with names
applied: real function/global/register names, correct addresses, and plate comments, but
with Ghidra pseudo-types (`undefined`, `uint`, `byte`) declared in `n32l406.h`, and `DAT_`
left only where it points to flash constant tables or dead code. **These do not compile and
are not meant to.** They are the behavioural reference the rebuild is written against and
the host tests are written from, and they stay in the tree for exactly that reason.

> **A note on `app/port/` references.** Comments throughout the rebuilt layers cite files like
> `the retired app/port/hw_can.c`. That was the *earlier single-layer C port* — one flat directory, weak
> no-op stubs between subsystems, and absolute-address macros for state. The layering refactor
> replaced it, and its explanatory comments were carried forward into the files that cite it,
> so the references record where a piece of reasoning came from. The directory itself is in git
> history (it was last present at commit `ff070e1`) if you ever want to diff against it.

Every reconstructed function carries the original flash address of what it reproduces in a
`/* 0x0800xxxx */` comment, so any part of the rebuild can be diffed back against Ghidra or
against `reference/2429-H1N4JPF20A-220524.app.bin`.

This box is **box 2429 / part H1N4JPF20A** (Jeep WK family, 83.333 kbit body bus). The 16-byte
`.iap` target magic for it is `01 09 07 08 00 02 01 07 91 32 47 a4` + `4e 33 14 f2` ("N3").

## Device facts
- MCU Nations N32L406CBL7, Cortex-M4F @ 64 MHz. STM32F1-like register map but NOT identical
  (e.g. ADC is at `0x40020800`, GPIO set/clear at `+0x18`/`+0x28`).
- Flash is **RDP Level 1** locked — can't be read over SWD, but flash-resident code can read it
  (that is how the bootloader was dumped; see `tools/dumper/` and `docs/notes/`).
- `.iap` has no signature — only a per-record checksum + the 16-byte magic — so a correctly
  repacked image is accepted. Full modify→flash→verify loop is proven (`docs/notes/iap-upload-pipeline-works.md`).
