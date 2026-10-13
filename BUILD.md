# BUILD — command reference

Quick commands for building, inspecting and flashing the canbox APP.
Background and the *why* behind the constraints: [`app/REBUILD.md`](app/REBUILD.md).

> The bootloader (`0x08000000`–`0x08001FFF`) is **never** built or flashed by anything here.
> `reference/bootloader_0x08000000_8k.bin` is read-only input — the build only reads its
> vector table to learn which addresses interrupts dispatch into.

---

## 1. Build

There is **no `make` on this machine**, so the driver is Python. (The `Makefile` still works
if you install one — `scoop install make` — and produces the same `build/app.bin`.)

```powershell
python tools\build.py                 # compile + link + verify preamble
python tools\build.py --pack          # ...and package to build\app.iap
python tools\build.py --clean         # remove build/
python tools\build.py -v              # echo the real gcc command lines
```

Outputs, all in `build/`:

| File | What |
|---|---|
| `app.elf` | linked image with symbols |
| `app.bin` | raw image, loads at `0x08002000` |
| `app.map` | linker map (check where things landed) |
| `app.iap` | flashable container (`--pack` only) |

### Toolchain

Baked in as `D:/IDE/arm-none-eabi-gcc/bin/arm-none-eabi-` (xPack GNU Arm Embedded 15.2.1).
Override if it moves:

```powershell
$env:CROSS = "C:/other/path/arm-none-eabi-"; python tools\build.py
```

```bash
CROSS=/c/other/path/arm-none-eabi- python tools/build.py    # bash
```

Flags are `-mcpu=cortex-m4 -mthumb -mfloat-abi=soft`. **Soft float is correct** — the stock
image contains no VFP instruction and no CPACR write (confirmed by disassembly, see
[`docs/notes/vector-table-slot-map.md`](docs/notes/vector-table-slot-map.md) §6).

---

## 2. Verify the image is safe to flash

This runs automatically inside `build.py` and **fails the build** on a violation. Run it
standalone to inspect:

```powershell
python tools\verify_preamble.py build\app.bin
python tools\verify_preamble.py build\app.bin -v            # list all 59 veneer slots
python tools\verify_preamble.py build\app.bin --compare reference\2429-H1N4JPF20A-220524.app.bin
```

It checks the three things that make an image bootable: entry veneer at exactly
`0x08002202`, every hardware-dispatched veneer slot still at its original address with the
`push {r4,lr}; bl …; pop {r4,pc}` shape, and no branch out of the app region. It validates
against the **bootloader dump**, not the (wrong) `0x08002072 + 8*IRQn` formula.

Sanity check — it must also pass on the stock image:

```powershell
python tools\verify_preamble.py reference\2429-H1N4JPF20A-220524.app.bin
```

---

## 3. Inspect

```powershell
# container contents, checksums, flash span
python tools\iaptool.py info   build\app.iap
python tools\iaptool.py verify build\app.iap

# sizes and symbols
D:\IDE\arm-none-eabi-gcc\bin\arm-none-eabi-size build\app.elf
D:\IDE\arm-none-eabi-gcc\bin\arm-none-eabi-nm -n build\app.elf

# disassemble the rebuilt image
D:\IDE\arm-none-eabi-gcc\bin\arm-none-eabi-objdump -d build\app.elf

# disassemble the STOCK image (raw binary, no symbols)
D:\IDE\arm-none-eabi-gcc\bin\arm-none-eabi-objdump -D -b binary -m armv7e-m -M force-thumb `
    --adjust-vma=0x08002000 reference\2429-H1N4JPF20A-220524.app.bin
```

Compare the preamble against stock — the first `0x20A` bytes are what must match.
These need **Git Bash** (`xxd` and `cmp` are not PowerShell commands):

```bash
cmp -l build/app.bin reference/2429-H1N4JPF20A-220524.app.bin | head
xxd -l 0x210 build/app.bin
xxd -l 0x210 reference/2429-H1N4JPF20A-220524.app.bin
```

Expected today: the preamble matches structurally, the code body differs (different
compiler). `iaptool info` reporting `id string 'app' NOT FOUND` is also expected — the
version string arrives with the `uart` port.

---

## 4. Flash

```powershell
python tools\iap_upload.py build\app.iap --port COM17 --reset powercycle --verify
python tools\iap_upload.py build\app.iap --port COM17 --reset nrst --verify
```

* `--reset powercycle` (default, **safest**) — you power-cycle at the prompt. A true
  power-on clears any latched debug-enable, which is **required**: under RDP-L1 the
  bootloader cannot program flash while the core is in debug mode. Use this after any
  SWD/pyOCD session.
* `--reset nrst` — faster, only safe when the chip isn't in a latched debug state.
* `--verify` — reads back the 17-byte version string from SRAM `0x20001B86` over SWD.
  Seeing `H1N4JPF20A-220524` proves the new image booted.

**Keep the line quiet during a flash** — the tty is shared with normal traffic, and a stray
transmitter corrupts a record (loader replies `R` and aborts, no retransmission).

**A bad image cannot brick the box.** The bootloader is write-protected and always
re-advertises `U` on boot — just reflash.

**Bare-bench caveat:** with no CAN bus attached the stock app is *silent* — it stays in
phase 1 until 3 CAN frames arrive and sends nothing on UART. "No `AA 55` traffic" is
expected, not a failure. Verify via the SRAM version string, or inject ≥3 frames at
83.333 kbit/s.

---

## 5. Regenerate generated sources

Two source files are generated from evidence rather than hand-written. You only need this
if an input changes (new bootloader dump, new SDK version) — otherwise just build.

Both generators take the path to the **extracted** Nations SDK, which ships as a zip
alongside the datasheets and is not vendored into this repo:

```powershell
# one-time: extract the vendor SDK somewhere
python -c "import zipfile; zipfile.ZipFile(r'<android-radio>\n32l406\Nations.N32L40x_Library.2.3.0.zip').extractall(r'D:\sdk')"

# app/startup/crt0.S        <- bootloader vector table + stock image + SDK IRQn names
python tools\generate\gen_crt0.py D:\sdk\Nations.N32L40x_Library.2.3.0

# app/include/n32l406_hw.h  <- vendor SDK register layouts
python tools\generate\gen_hw.py   D:\sdk\Nations.N32L40x_Library.2.3.0
```

Both are deterministic: re-running them on unchanged inputs reproduces the committed files
byte-for-byte, so a regeneration that produces a diff means an input really changed.

`gen_crt0.py` cross-references all three sources of truth —
`reference/bootloader_0x08000000_8k.bin` (which veneer slot each interrupt dispatches to),
`reference/2429-H1N4JPF20A-220524.app.bin` (the stock handler and trap code per slot), and
the SDK's `IRQn_Type` (official peripheral names).

> Extraction of the full SDK zip may fail on one deep `rt-thread` path that exceeds the
> Windows path limit. That file is irrelevant here — only `firmware/CMSIS/device/n32l40x.h`
> is read.

Register layouts are machine-copied from the vendor header on purpose: this part looks
STM32-ish but is not one (GPIO is F4/L4-style, `CAN_RX0` is IRQ54, ADC is at `0x40020800`),
and hand-transcribing those is how a rebuild silently bricks.

---

## 6. Current state

```powershell
python tools\build.py --pack     # green: links, verifies, packs
```

> **`build/app.iap` is not yet a functional replacement.** `main_loop` is a deliberate trap
> stub (`fatal_error_reset(250)`) that watchdog-resets back into the bootloader — safe and
> re-flashable, but it will not talk CAN or UART.

| Subsystem | Status | File |
|---|---|---|
| `rcc` | done | `app/port/hw_rcc.c` |
| `wwdg` | done | `app/port/hw_wwdg.c` |
| `sys` | partial — fatal path only | `app/port/sys.c` |
| crt0 / veneers / entry | done, verified vs stock | `app/startup/crt0.S` |
| `gpio` `tim` `uart` `can` `can_rx_decode` `flash` `core` | **TODO** | — |

Porting order and the `main_loop` checklist are in the header comment of
[`app/port/main.c`](app/port/main.c).
