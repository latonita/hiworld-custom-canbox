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

The rebuild loop is: reconstruct real C from `app/src/*.c` subsystem by subsystem →
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

## 4. Recommended workflow

Work toward a **functional-equivalent** image first; pursue byte-identical only where it
buys confidence (see §7).

1. **Reconstruct subsystem by subsystem.** `app/src/*.c` is named Ghidra output with
   correct addresses but pseudo-types (`undefined`, `uint`, `byte`) and `DAT_` constants;
   it does not compile as-is. Turn one subsystem at a time into clean C, using the fixed
   RAM globals from `app/include/globals.h` and the MMIO bases in
   `app/include/registers.h`. Sensible order: `rcc` → `gpio` → `tim` (1 ms tick) →
   `uart` → `can` + `can_rx_decode` → `flash`/param page → `core`/scheduler → `wwdg` →
   `misc`. The scheduler and ISR map are documented in `docs/hiworld-startup-sequence.md`
   §3–§6.
2. **Write `startup/crt0.S`** (the `.image_entry` section) to reproduce the preamble of §2:
   crt0 fragments, the 59-veneer array at the correct addresses, and the entry veneer at
   `0x202`. Point each veneer's `bl` at the corresponding reconstructed handler.
3. **Compile and link:**
   ```
   arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os \
       -ffreestanding -nostartfiles -Iapp/include \
       startup/crt0.S app/src/*.c \
       -T app/linker/app.ld -Wl,--gc-sections -Wl,-Map=app.map -o app.elf
   arm-none-eabi-objcopy -O binary app.elf app.bin
   ```
   The `app.ld` `ASSERT`s fail the link if the entry veneer is not at `0x08002202` or if
   linker-managed `.bss` collides with the fixed global block.
4. **Diff against the reference.** The stock decoded image is
   `reference/2429-H1N4JPF20A-220524.app.bin` (32 KB). Compare:
   ```
   cmp -l app.bin reference/2429-H1N4JPF20A-220524.app.bin | head
   # or a visual diff of the first 0x210 bytes to confirm the preamble/entry:
   xxd app.bin | sed -n '1,34p'
   ```
   Priorities, in order: (a) bytes `0x000..0x209` (crt0 + veneer array + entry veneer)
   match or are provably equivalent; (b) the version string `H1N4JPF20A-220524` is present;
   (c) behaviour matches on the bench. Early on, expect the code body to differ (different
   compiler) while the preamble matches — that is the intended state.

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
- **`--verify`** reads back the 17-byte version string the app copies to SRAM
  `0x20001B86` at init (SRAM is RDP-exempt, readable over SWD while the app runs). Seeing
  `H1N4JPF20A-220524` (or your rebuilt string) proves the new image booted.
- **Bench caveat:** with no CAN bus attached the app is **silent** — it stays in phase 1
  until 3 CAN frames arrive and sends nothing on UART. So "no `AA 55` traffic" on a bare
  bench is expected, not a failure; verify via the SRAM version string, or inject ≥3 CAN
  frames at 83.333 kbit/s to bring up phase 2. See `docs/hiworld-startup-sequence.md` §9.
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
| **Float ABI** | Chosen `-mfloat-abi=soft`; evidence is a no-FPU-opcode scan of the reference, not a disassembly. Small risk the original was `softfp` with an FPU enable we haven't seen. | Disassemble the reference (`arm-none-eabi-objdump -d`, not available in this environment yet) and check for VFP ops / a CPACR write in crt0. If present, match it; otherwise soft is correct. |
| **Exact crt0 preamble** | The `0x08002000..0x08002201` layout (crt0 fragments + 59 veneers) must be reproduced by hand; GCC will not generate an ARM-C-lib `__main`/`Region$$Table` scatterload. | Hand-write `startup/crt0.S` to match the reference bytes (disassemble `0x000..0x209`); point veneers at rebuilt handlers. Lean on the `app.ld` entry ASSERT. |
| **Byte-identical vs functional-equivalent** | GCC output will differ from the vendor's Keil/armcc code body, so a full byte match is unrealistic. Byte-identity matters only for the preamble/veneers and the fixed global addresses. | Target byte-identity for `0x000..0x209` and the RAM map; target functional equivalence for the rest, validated by the bench behaviour in `docs/hiworld-startup-sequence.md` §5–§9. |
| **SRAM layout for rebuilt statics** | The stock globals are at fixed absolute addresses (`0x20001000+`, via `globals.h`) and are not linker-allocated. Any `.data`/`.bss` the new C introduces must avoid them and the stack (`0x20000000..0x20000600`). The `app.ld` VMA for `.data` is provisional. | Keep using the `globals.h` absolute pointers for known state; place linker-managed statics in a verified-free range (and/or reproduce the original scatter-loaded map). The `app.ld` overlap ASSERT guards the common mistake. |
| **Pack template availability** | `iaptool.py pack` needs a box-2429 record 0 (magic + key). `reference/2429-H1N4JPF20A-220524.iap` is in-repo and is the primary template; `tools/dumper/dumper2.iap` is an equivalent fallback (same box-2429 magic/key). |
