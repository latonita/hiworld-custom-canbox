# BUILD — command reference

Quick commands for building, inspecting and flashing the canbox APP.
Background and the *why* behind the constraints: [`app/REBUILD.md`](app/REBUILD.md).

> The bootloader (`0x08000000`–`0x08001FFF`) is **never** built or flashed by anything here.
> `reference/bootloader_0x08000000_8k.bin` is read-only input — the build only reads its
> vector table to learn which addresses interrupts dispatch into.

---

## 0. Build in Docker (no toolchain on the machine)

The quickest way to get a flashable `.iap` on a machine that has nothing
installed. Docker is the only prerequisite -- no cross toolchain, no Python, no
host compiler.

```powershell
.\build-docker.ps1                      # Windows
```
```bash
./build-docker.sh                       # Linux, macOS, WSL, Git Bash
```

First run builds the image (a few minutes, ~200 MB download); after that it is
cached. The repo is bind-mounted, so the output lands in `build/` exactly where
a native build puts it:

```
build/canupdateH1N4JPF20L-171026.iap    jeep -- stock-equivalent
build/canupdateH1N4BNZ10L-171026.iap    benz -- Mercedes emulation
```

The default run executes the host unit tests for **both** protocols, then builds,
verifies the preamble and packs **both** images, printing each container's
sha256. Options are the same on both launchers:

| `build-docker.sh` | `build-docker.ps1` | |
|---|---|---|
| `--protocol jeep\|benz\|both` | `-Protocol jeep\|benz\|both` | default `both` |
| `--no-tests` | `-NoTests` | skip the host unit tests |
| `--tests-only` | `-TestsOnly` | run the host unit tests and stop |
| `--clean` | `-Clean` | remove `build/` |
| `--shell` | `-Shell` | interactive shell in the builder |
| `--rebuild-image` | `-RebuildImage` | force a fresh image build |
| `-v`, `--verbose` | `-ShowCommands` | echo the compiler command lines |

**Flashing is deliberately not containerised.** `tools/iap_upload.py` needs a USB
serial port and an SWD probe, which do not belong in a build container -- and
passing a COM port into a Docker Desktop VM does not work on Windows at all. Run
it on the host against the `.iap` the container produced (§4).

### Output ownership, and mixing Windows with WSL

The container starts as root but reads the owner off the mounted tree and
becomes them, so the build output belongs to you, not to root -- no `sudo` to
clean `build/`.

One wrinkle is worth knowing if you drive the *same* working tree from both
Windows and WSL (e.g. the repo lives on `D:` and you reach it as
`/mnt/d/...`). Docker reports a different owner depending on which path the bind
mount came in through: a Windows path reports everything as root, the WSL path
reports your real uid. So a `build/` written from one side can be unwritable
from the other, and these shares silently ignore `chown`, so it cannot be
repaired in place.

The entrypoint therefore detects a `build/` containing files owned by anyone
else and **deletes it** before starting, printing a note:

```
note: build/ holds files owned by another uid; this build runs as 1000.
      Removing it -- it is generated output, and this share ignores chown.
```

That costs one full rebuild each time you switch sides, and nothing else --
`build/` is generated output and is gitignored. Keeping to one side, or putting
the repo on the WSL filesystem rather than `/mnt/d` (which also builds faster),
avoids it entirely.

### What is in the image

`docker/Dockerfile` pins **xPack GNU Arm Embedded GCC 15.2.1-1.1**, verified
against the publisher's sha256 and fetched for the host's architecture (linux
x64 and arm64 both work, so Apple Silicon is fine). It is the same toolchain
version this project was developed against, which is why the container and a
native build produce **byte-identical** `.iap` files -- a useful cross-check, and
the reason the run prints sha256 for each container.

The toolchain is pruned to what a Cortex-M4F build reaches (gdb and the
non-`v7e-m` newlib multilibs go, 1.9 GB -> 1.07 GB) and then smoke-tested inside
the image build with this project's exact flag set, so a bad prune fails there
rather than in somebody's firmware.

`docker/build-all.sh` is the container entrypoint and is what both launchers
drive; it is also usable directly:

```bash
docker build -t canbox-builder docker
docker run --rm -v "$PWD:/work" canbox-builder --protocol jeep
```

Overrides: `DOCKER=podman` selects a different runtime, `CANBOX_IMAGE` /
`CANBOX_TAG` rename the image. To move to a different toolchain release, bump
`TOOLCHAIN_VERSION` **and both** `TOOLCHAIN_SHA256_*` args in the Dockerfile
together, then `--rebuild-image`.

---

## 1. Build

There is **no `make` on this machine**, so the driver is Python — and `tools/build.py` is
now the only driver: the old `Makefile` predates the three-layer split and does not build
the C++ layers.

```powershell
python tools\build.py                 # compile + link + verify preamble
python tools\build.py --pack          # ...and package to build\app.iap
python tools\build.py --clean         # remove build/
python tools\build.py -v              # echo the real gcc command lines
```

The build is mixed-language: `.S` and `.c` go through `arm-none-eabi-gcc -std=gnu11`,
`.cpp` through `arm-none-eabi-g++ -std=c++17` with `-fno-exceptions -fno-rtti
-fno-threadsafe-statics -fno-use-cxa-atexit`, and the link is `-nostdlib -nostartfiles`.
Sources are discovered automatically from `app/hal/`, `app/services/` and `app/logic/`,
startup first.

Host unit tests are a separate build and need only a host C++ compiler (clang++ or g++):

```powershell
python tools\test.py                  # build + run everything
python tools\test.py -k steering      # only cases whose name matches a substring
python tools\test.py --units can_decode can_bus bytes   # one module and its dependencies
```

Outputs, in `build/<protocol>/` (plus the packed container in `build/`):

| File | What |
|---|---|
| `<protocol>/app.elf` | linked image with symbols |
| `<protocol>/app.bin` | raw image, loads at `0x08002000` |
| `<protocol>/app.map` | linker map (check where things landed) |
| `canupdate<version>.iap` | flashable container, named after the image's own 17-byte version string (`--pack` only) |

### Toolchain

Baked in as `D:/IDE/arm-none-eabi-gcc/bin/arm-none-eabi-` (xPack GNU Arm Embedded 15.2.1).
Override if it moves:

```powershell
$env:CROSS = "C:/other/path/arm-none-eabi-"; python tools\build.py
```

## Which protocol to build

```
python tools/build.py                      # jeep: Hiworld JPF2, stock-equivalent
python tools/build.py --protocol benz      # Mercedes-Benz emulation
python tools/build.py --protocol benz --pack
```

`--protocol` picks the head-unit dialect. `jeep` (the default) is the faithful
rebuild and the only variant whose wire behaviour must match the original box.
`benz` emulates a Raise Mercedes canbox to reach a much richer gauge screen --
see `docs/raise-benz-emulation.md` for exactly what it publishes, what it cannot, and
what it costs (steering-wheel buttons, among other things).

The two are packed separately (`build/canupdateH1N4JPF20L-171026.iap`,
`build/canupdateH1N4BNZ10L-171026.iap`) and
carry different version strings, so `iap_upload.py --verify` tells you which one
is on the box. Objects live in per-protocol directories, so switching
`--protocol` cannot link a stale object from the other dialect.

Both are updatable from the radio either way: a Benz image still recognises the
Hiworld reboot-into-the-bootloader frame. Pick "Hiworld" in the upgrade screen's
spinner whatever the box currently speaks.

Tests run BOTH configurations by default:

```
python tools/test.py                    # jeep and benz
python tools/test.py --protocol benz    # just one
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
python tools\build.py --pack     # compile, link, verify the preamble, package
python tools\test.py             # host unit tests for the logic layers
```

The firmware is a three-layer rebuild; `docs/ARCHITECTURE.md` is the layer contract and the
public API of every module, `docs/misra-cpp.md` is the C++ subset and its deviations.

| Layer | Language | What |
|---|---|---|
| `app/startup/` | asm + C11 | the fixed preamble, the 59 IRQ veneers, the `0x08002202` entry, RAM init |
| `app/hal/` | C11 | register drivers: rcc nvic wwdg cpu gpio exti tim can usart flash adc pwr. MMIO only — every object reports `data = 0, bss = 0` |
| `app/services/` | C++17 | scheduler, soft clock, soft timers, parameter store, CAN transport, head-unit link (shared core plus one translation unit per wire dialect), board I/O, power, byte helpers |
| `app/logic/` | C++17 | the two CAN profile decoders, the Benz-emulation decoders and frame packer, periodic TX, cluster text, link state machine, main loop, the five interrupt entry points |

Application state is not scattered across absolute-address macros any more: it is one
`ram::AppRam` struct declared in `app/include/ram_map.hpp` and defined in
`app/services/app_state.cpp`. It is ordinary `.bss` — the linker places it, `startup.c`
zeroes it, and nothing in the rebuild sits at a fixed SRAM address. Each field still notes
the stock address it corresponds to, so the code can be diffed against Ghidra.

The one address visible from outside is the version string `tools/iap_upload.py --verify`
reads back over SWD; it is a standalone `g_versionString` symbol the uploader resolves from
`build/app.elf`, not a constant in the tool.

### What is proven, and what is not

- **Builds and links** within the app region, entry veneer at `0x08002202`, all 59 veneer
  slots intact (`tools/verify_preamble.py`, run automatically by `build.py`).
- **Host unit tests** cover the layers that do not touch a register — decoders, scheduler,
  clock, software timers, parameter store, CAN rings, head-unit framing, board I/O. They are
  written from the behavioural spec in `reference/decompile/app/*.c`, not from the rebuilt code.
- **Not yet proven on a bench.** Nothing here replaces flashing the box and watching it talk
  to the car. Until that happens, "the tests pass" means the logic matches the decompile —
  not that the firmware is correct.

Recovery remains unconditional: the bootloader is write-protected and always re-advertises
`U` on boot, so a bad image cannot brick the box.
