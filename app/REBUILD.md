# Rebuilding the Hiworld canbox APP

How to rebuild **only** the application image (flash `0x08002000`+) for the Hiworld
canbox (Nations **N32L406CBL7**, Cortex-M4F) and re-flash it through the stock vendor
IAP bootloader, which is **never** modified. This is box **2429 / H1N4JPF20A**.

Read first for authoritative detail:
`docs/hiworld-startup-sequence.md`, `docs/hiworld-bootloader-analysis.md`,
`docs/hiworld-iap-format.md`, `docs/hiworld-iap-update-sequence.md`,
`docs/hiworld-iap-flashing-flow.md`, `docs/hiworld-hardware.md`,
`app/MEMORY_MAP.md`.

---

## 1. Overview

The flash holds two independent programs:

| Region | Span | Rebuild? |
|---|---|---|
| IAP bootloader | `0x08000000`–`0x08001FFF` (8 KB) | **No — never touch.** Owns the vector table and does the flashing. |
| **Application** | `0x08002000`–`0x0801F7FF` (118 KB) | **Yes — this is the target.** Delivered as the `.iap` payload. |
| Param / EEPROM | `0x0801F800`–`0x0801FFFF` (2 KB) | No — runtime data, must not be written by the image. |

The rebuild loop is: reconstruct real C from `reference/decompile/app/*.c` subsystem by subsystem →
compile for Cortex-M4 Thumb → link with `app/linker/app.ld` → diff the produced `.bin`
against `reference/2429-H1N4JPF20A-220524.app.bin` → pack to `.iap` → flash over UART →
verify. The bootloader accepts any correctly-packed image (no signature), so the path is
open; the engineering risk is entirely in reproducing the entry/interrupt layout.

---

## 2. Memory and entry constraints (the non-negotiable part)

These come straight from the bootloader dump and the reference image. Violating any of
them bricks the boot (recoverable — the bootloader is unerasable — but the app won't run).

- **No vector table in the image.** The app has none. The bootloader keeps
  **VTOR = 0x08000000** and **MSP = 0x20000600**; the app never relocates VTOR and never
  sets SP. Bytes at `0x08002000` are Thumb code, not vectors.
- **Entry veneer at exactly `0x08002202`** (image offset `0x202`). The bootloader does
  `bl 0x08002202`, gated on the valid-app marker `*0x08001800 == 0x19780217` (the
  bootloader writes that marker itself after a good flash — the build does not). The
  veneer is 8 bytes:
  ```
  10 b5          push {r4, lr}
  00 f0 fc f8    bl   __main        ; ARM C-lib __main -> scatterload -> __rt_entry -> main
  10 bd          pop  {r4, pc}
  ```
  Confirmed verbatim at offset `0x202` of the reference image.
- **`0x08002000..0x08002201` is a fixed preamble.** It is `startup_crt0` fragments
  followed by a dense array of **59 eight-byte IRQ/exception veneers** at
  `0x0800202a..0x08002201` (`push {r4,lr}; bl <handler>; pop {r4,pc}` each). The array is
  **address-indexed by the bootloader's immutable vector table**:
  `veneer(IRQn) = 0x08002072 + 8*IRQn` (IRQ0 = WWDG), and the core exceptions are
  forwarded to fixed slots (NMI→`0x0800202B`, HardFault→`0x08002033`, …). The hardware
  dispatches straight to these addresses, so every veneer slot an enabled interrupt uses
  must stay at its original address. The veneer *body* (the `bl` target) may point at a
  rebuilt handler; the slot *address* may not move.
- **Shared VTOR = shared interrupt plumbing.** Because the app runs on the bootloader's
  table, you cannot change which address an IRQ vectors to — only what lives there. This
  is why the preamble must be reproduced, not just the `0x202` entry.

Practical consequence: emit the whole preamble (crt0 + veneer array + entry veneer) from a
single hand-written `startup/crt0.S` placed first by the linker (`app.ld` section
`.image_entry`), laid out to match the original. The `app.ld` `ASSERT`s catch a misplaced
entry at link time.

---

## 3. Toolchain

- **Or skip all of this:** `./build-docker.sh` / `.\build-docker.ps1` builds in a
  container that already has the pinned toolchain, and produces the same bytes.
  See `BUILD.md` §0. Flashing (§6) still happens on the host.
- `arm-none-eabi-gcc` / `-ld` / `-objcopy`, GNU Arm Embedded.
- **Installed here:** xPack GNU Arm Embedded GCC 15.2.1 at `D:/IDE/arm-none-eabi-gcc/bin/` (not on PATH; the `Makefile` sets `CROSS` to this absolute path). Cortex-M soft-float multilib present.
- Core flags: `-mcpu=cortex-m4 -mthumb`.
- **Float ABI: use `-mfloat-abi=soft` for the first rebuild.** A heuristic opcode scan of
  the reference image finds **no VFP/coprocessor instructions** (the only two
  coprocessor-space halfwords are the low words of the `0xE000ED0C`/`0xE000ED10` register
  *address* literals, not FPU ops). The stock app is control/byte logic with no float
  math, and it runs on the bootloader's unknown CPACR state, so soft-float is both the
  likely original ABI and the safe choice (no dependency on the FPU being enabled). Revisit
  only if a disassembly later shows VFP use or an explicit CPACR enable in crt0 — see §7.
- Suggested: `-Os -ffreestanding -fno-exceptions -fno-unwind-tables -nostartfiles`
  (the image provides its own crt0 and `__main`-equivalent; do not pull in the full newlib
  startup/vector table), and link with `-T app/linker/app.ld -Wl,--gc-sections`.

---

## 4. Build, test, verify

The reconstruction is **done**: `app/hal/` (C11 register drivers), `app/services/` and
`app/logic/` (C++17) are the firmware, and `reference/decompile/app/*.c` is the Ghidra
behavioural spec they were written against. `docs/ARCHITECTURE.md` is the layer contract.

```powershell
python tools\build.py            # compile + link + verify the preamble + integration checks
python tools\build.py --pack     # ...and package to build\app.iap
python tools\test.py             # host unit tests for the logic layers
```

`build.py` discovers sources from the three layer directories, startup first, and fails the
build if:

- the entry veneer is not at `0x08002202`, or a dispatched veneer slot moved
  (`tools/verify_preamble.py`, against the bootloader dump — not against the wrong
  `0x08002072 + 8*IRQn` formula);
- a C++ static constructor was emitted (this image never runs `.init_array`);
- an interrupt handler name drifted and silently left a `crt0.S` trap in place, or one stock
  function got reconstructed twice under two names (`tools/check_stubs.py`).

**Diff against the reference.** The stock decoded image is
`reference/2429-H1N4JPF20A-220524.app.bin`:

```bash
cmp -l build/app.bin reference/2429-H1N4JPF20A-220524.app.bin | head
xxd -l 0x210 build/app.bin        # the preamble, which is what must match
```

Priorities, in order: (a) bytes `0x000..0x209` match or are provably equivalent; (b) the
version string is present; (c) behaviour matches on the bench. The code body differs — a
different compiler — and that is the intended state.

**SRAM note.** The rebuild no longer reproduces the stock firmware's absolute RAM map. Its
state is one linker-placed `.bss` object (`app/include/ram_map.hpp`); the original addresses
survive as `stock @0x...` comments and in `ghidra/merged_globals.tsv`. The only externally
known address is the version string, and `iap_upload.py --verify` resolves that from
`build/app.elf` rather than hardcoding it.

---

## 5. Package to `.iap`

The container format is in `docs/hiworld-iap-format.md`. Reuse record 0 from an existing
box-2429 `.iap` as the pack template — it carries the required 16-byte target magic
(universal `01 09 07 08 00 02 01 07 91 32 47 a4` + per-box `4e 33 14 f2` "N3") and the
obfuscation key, both of which `iaptool.py pack` copies verbatim while recomputing every
per-record checksum.

A ready box-2429 template with the correct magic/key (`68 cc e3 3e`) is
`reference/2429-H1N4JPF20A-220524.iap` (the original firmware container; `tools/dumper/dumper2.iap` also
ideal template if you have it; it is not checked into this repo.)

```
python tools/iaptool.py pack app.bin reference/2429-H1N4JPF20A-220524.iap -o app-rebuilt.iap
python tools/iaptool.py verify app-rebuilt.iap        # round-trip + checksum self-check
python tools/iaptool.py info   app-rebuilt.iap
```

`iaptool.py pack` pads the payload to a 128-byte boundary, re-sequences, re-flags the last
record, and recomputes the 33-word checksums, so the output is a loader-valid container.

---

## 6. Flash and verify

Flashing drives the bootloader's UART protocol (38400 8N1): reset → `U` → begin-magic
`19 78 02 17` → `B`/`S` → one 136-byte record per `S` → `E`. See
`docs/hiworld-iap-update-sequence.md`. The uploader is proven end to end
(`docs/notes/iap-upload-pipeline-works.md`); bench wiring is in
`docs/notes/iap-driving-bench-setup.md` (FTDI @ COM17, Pico for nRST).

```
python tools/iap_upload.py app-rebuilt.iap --port COM17 --reset powercycle --verify
```

- **`--reset powercycle` (default, safest):** you power-cycle the box at the prompt. A true
  power-on also clears any latched debug-enable — **required**, because under RDP-L1 the
  bootloader cannot erase/program flash while the core is in debug mode. Use this after any
  SWD/pyOCD session. `--reset nrst` is faster but only safe when the chip is not in a
  latched debug state.
- **`--verify`** reads back the 17-byte version string the app copies into SRAM at init
  (SRAM is RDP-exempt, readable over SWD while the app runs; flash is not). Seeing
  `H1N4JPF20A-220524` (or your rebuilt string) proves the new image booted.
  The address is **resolved from `build/app.elf`** (symbol `g_versionString`), not
  hardcoded — the rebuild no longer reproduces the stock firmware's absolute SRAM map.
  `0x20001B86` is where the *factory* image keeps it, and the uploader falls back to
  that with a warning if it cannot resolve the symbol. Build before verifying, or pass
  `--elf` / `--version-addr`.
- **Bench caveat:** with no CAN bus attached the app is **silent** — it stays in phase 1
  until 3 CAN frames arrive and sends nothing on UART. So "no `AA 55` traffic" on a bare
  bench is expected, not a failure; verify via the SRAM version string, or inject ≥3 CAN
  frames at 83.333 kbit/s to bring up phase 2. See `docs/hiworld-startup-sequence.md` §9.
- **You have ~18.4 s to read the version string.** With no bus, phase 1 gives up and the
  box enters deep sleep at t ≈ 18.4 s (the 8th autobaud retry is at 16.38 s; the give-up
  branch runs on the *next* 2048 ms tick). After that the SWD read fails with "No ACK"
  because the core is stopped — that is the box working correctly, not a bad flash.
  Re-power it, or feed it CAN frames, and read again.
- **Line-sharing hazard:** the same tty carries normal traffic; a stray transmitter during
  an update corrupts a record → the loader replies `R` and aborts with no retransmission.
  Keep the line quiet during flashing.

Recovery: the bootloader is write-protected and always re-advertises `U` on boot, so a bad
image cannot brick the box — just reflash.

---

## 7. Open questions / risks

| Item | Status / risk | How to resolve |
|---|---|---|
| **Interrupt dispatch under shared VTOR** | ✅ **RESOLVED.** IRQs vector into the veneer array, but the map is **not** `0x08002072 + 8*IRQn` — that holds only for IRQ0..10 and IRQ23..41. The app's array is in **STM32F103 vector order** while the chip is an N32L406; the bootloader's table is the shim between them. The "anomaly" was a misreading: `CAN_RX0_IRQn = 54` on this part, and the bootloader maps vector 54 → veneer slot 29 = the CAN RX handler. `NVIC_Init(0x36)` is correct. | Done — read off `reference/bootloader_0x08000000_8k.bin` and confirmed against the vendor SDK's `IRQn_Type`. Map encoded in `app/startup/crt0.S`; enforced on every build by `tools/verify_preamble.py`. See `docs/notes/vector-table-slot-map.md`. |
| **Float ABI** | **RESOLVED.** `-mfloat-abi=soft`. The stock image contains no VFP instruction and no CPACR write, confirmed by disassembly of `reference/2429-H1N4JPF20A-220524.app.bin`, not just by an opcode scan. | Done. |
| **Exact crt0 preamble** | The `0x08002000..0x08002201` layout (crt0 fragments + 59 veneers) must be reproduced by hand; GCC will not generate an ARM-C-lib `__main`/`Region$$Table` scatterload. | Hand-write `startup/crt0.S` to match the reference bytes (disassemble `0x000..0x209`); point veneers at rebuilt handlers. Lean on the `app.ld` entry ASSERT. |
| **Byte-identical vs functional-equivalent** | GCC output will differ from the vendor's Keil/armcc code body, so a full byte match is unrealistic. Byte-identity matters only for the preamble/veneers and the fixed global addresses. | Target byte-identity for `0x000..0x209` and the RAM map; target functional equivalence for the rest, validated by the bench behaviour in `docs/hiworld-startup-sequence.md` §5–§9. |
| **SRAM layout for rebuilt statics** | **RESOLVED, and the premise is gone.** The rebuild no longer reproduces the stock firmware's absolute RAM map: all state is one linker-placed `.bss` object (`app/include/ram_map.hpp`, defined in `app/services/app_state.cpp`). Nothing sits at a fixed SRAM address, so there is nothing for new statics to collide with; the only reserved region is the stack below `0x20000600`, which `app.ld` keeps out of the allocatable region. `app/include/globals.h` was deleted -- the address key for reading the decompile is `ghidra/merged_globals.tsv`. | Done. |
| **Pack template availability** | `iaptool.py pack` needs a box-2429 record 0 (magic + key). `reference/2429-H1N4JPF20A-220524.iap` is in-repo and is the primary template; `tools/dumper/dumper2.iap` is an equivalent fallback (same box-2429 magic/key). |
