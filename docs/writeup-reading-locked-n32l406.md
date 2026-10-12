# Reading the flash of a "locked" Nations N32L406 — without unlocking it

*by [latonita](https://github.com/latonita) (Anton Viktorov)*

## TL;DR

A cheap car-radio CAN adapter uses a **Nations N32L406** (a Chinese Cortex-M4F, broadly
STM32-compatible). I had the application firmware — it ships as vendor `.iap` update files — but
**not** the 8 KB bootloader that lives at `0x08000000` and applies those updates. I wanted that
bootloader, and I wanted to keep it: it's what lets the box take new firmware over the radio, and
it's in none of the update files.

The flash is **readout-protected (RDP Level 1)**: a debugger connects, but every read of main
flash faults. The documented way to lift RDP is a mass-erase — which would have destroyed the
very bootloader I was trying to save. So I didn't.

Instead I used a property of RDP Level 1: **it only blocks reads coming from the debugger (or
from code running in RAM); code executing from flash itself may read all of flash.** The box
accepts unsigned `.iap` images, so — after reverse-engineering the `.iap` format and proving I
could flash my own images safely — I flashed a tiny program through the normal update path, let
it **copy the locked bootloader into SRAM**, and read the SRAM back over SWD (SRAM is never
protected).

Result: the full 8 KB bootloader, recovered **non-destructively**. Tooling is in the repo
(`iaptool.py`, `iap_upload.py`, the dumper).

> **Scope / ethics.** My own hardware, done for repair and interoperability, every step
> non-destructive. Nothing here helps attack a device you don't own — it's about recovering
> *your own* firmware without the vendor's destructive unlock.

---

## The target

- **MCU:** Nations Technologies **N32L406CBL7** — ARM **Cortex-M4F @ 64 MHz**, **LQFP48**,
  128 KB flash @ `0x08000000`, 24 KB SRAM @ `0x20000000`. The register map is STM32F1-ish
  (RCC `0x40021000`, FLASH `0x40022000`, USART2 `0x40004400`), but it is **not** a
  pin/register-identical clone — "STM32-compatible" is ecosystem-level only.
- **Board:** a Hiworld CAN-bus adapter. The flash map is:

```
0x08000000 .. 0x08001FFF   8 KB    IAP bootloader   (in NO .iap file; the thing I want)
0x08002000 .. 0x0801F7FF   118 KB  application      (shipped as .iap updates)
0x0801F800 .. 0x0801FFFF   2 KB    parameter page
```

The application is replaceable over the air; the bootloader is not. Keeping that bootloader
intact was the whole constraint — lose it and the box can never take a radio update again.

---

## 1. What I had, and what I wanted

I started with a handful of `.iap` update files and a board that flashes them. The bootloader
that does the flashing was the target, for two reasons: I wanted to understand the update
mechanism fully, and I wanted the option to build my own firmware later — all of which needs the
loader. But it isn't in any update file, and (spoiler) it can't be read off the chip directly.

So the job split into two halves: **(a)** understand the `.iap` format well enough to author my
own valid images, and **(b)** find a way to make the chip hand me the locked bootloader without
destroying it.

---

## 2. Reverse-engineering the `.iap` format

Before touching the chip, I worked out the container from the files alone. It's simple once you
see it:

- The file is **N records of exactly 136 bytes** (`128` data + an `8`-byte trailer). No global
  header.
- **Record 0** is special: a 16-byte magic, then opaque bytes, then a **4-byte key in the
  clear** at offset 132.
- **Records 1..N-1** carry 128 bytes of **obfuscated** payload plus `{u16 sequence, u8 percent,
  u8 last-flag, u32 checksum}`.
- The payload "encryption" is just a position-additive byte scrambler, keyed by that cleartext
  key: `plain[i] = (cipher[i] - i - key[i & 3]) & 0xFF`, with `i` resetting each record. One
  block of erased (all-`0x00`) flash makes the keystream obvious, which is how the key falls out.
- Each record carries a checksum = the 33 little-endian 32-bit words of `record[0:132]` summed
  mod 2³². The decoded payload is a **raw Cortex-M image based at `0x08002000`.**

I wrote `iaptool.py` to unpack and repack; it round-trips all sample files **byte-for-byte**,
which is the proof the format is fully understood. (Full spec: `hiworld-iap-format.md`.)

Having a byte-exact packer is the foundation for everything after — it means any image I build
can be wrapped into a file the bootloader will accept.

---

## 3. The locked chip

**Finding the debug port.** The board has a 5-pad ISP header; against the datasheet's LQFP48
pinout:

| Signal | MCU pin | LQFP48 |
|---|---|---|
| SWDIO | PA13 | pin 34 |
| SWCLK | PA14 | pin 37 |
| nRST  | —    | reset |

For a probe I used a **Raspberry Pi Pico** flashed with `debugprobe_on_pico.uf2` — a CMSIS-DAP
v2 adapter pyOCD speaks natively. No CMSIS pack exists for the N32L40x, so I use the generic
Cortex-M target:

```bash
pip install pyocd
pyocd list
pyocd commander -t cortex_m
```

**Gotcha — the debug clock sleeps.** The N32L40x gates its debug clock when the core is idle, so
SWD starts returning `No ACK` within a minute. Catch it right after a reset and keep the clock
alive by writing **`DBGMCU_CR` (`0xE0042004`) = 0x7**. Then:

```
read32 0xE000ED00 -> 0x410FC241   # CPUID: Cortex-M4 r0p1
```

**Trying to read the firmware — and failing.** With a stable session I mapped what responds:

| Region | Result |
|---|---|
| SRAM `0x20000000`+ | reads fine |
| Core/debug registers, option bytes | read fine |
| **Main flash `0x08000000`** | **every read FAULTS** |

Everything works except flash. That's readout protection, confirmed three ways:

1. `read32 0x08000000` faults; `read32 0x20000000` doesn't.
2. `FLASH_OBR` (`0x4002201C`) = `0x03FFFFFE` — the `RDPRT` bit is set.
3. Option byte `RDP1` (`0x1FFFF800`) = `0xFF`, not the unprotected `0xA5`.

Dead end — by the usual playbook. The "fix" is a mass-erase to clear RDP, which wipes the chip,
bootloader and all. Not an option.

---

## 4. The loophole, and the plan

The **Nations N32L40x user manual** (flash read-protection section) has the way out. Level 1,
near-verbatim:

> *Only the read of main memory **from user code** is allowed — when the program runs from main
> flash in **non-debug mode**, reading main memory is allowed. Reads via the debugger, or after
> booting from SRAM, are blocked.*

So flash isn't unreadable — it's unreadable *except by code already running from flash, with no
debugger halting it.* Two facts seal it:

- **No PCROP / per-region protection** on this family: if flash-resident code can read any flash,
  it can read all of it, bootloader included.
- **This is Level 1, not Level 2.** L2 would block flash-resident reads too and permanently kill
  the debug port. The debug port works → L1. (I never touched the RDP bytes, so it stayed L1.)

And the enabler I'd already built: **the box runs an unsigned-`.iap` bootloader, and I can pack
valid images.** So I can get my own code running from flash through the stock update path — the
exact privileged context the manual allows to read the locked flash.

The plan:

> Flash a tiny program via IAP → it runs *from flash, non-debug* → it copies the bootloader into
> SRAM → read SRAM back over SWD.

The copy-to-SRAM indirection is the crux: I must **not** read flash over SWD (debugger context,
blocked) and must **not** run the dumper from SRAM. The dumper runs from flash and only *writes*
to SRAM; the debugger only ever touches SRAM.

---

## 5. De-risking the flash procedure in stages

Flashing hand-built images into the only bootloader I had was the scary part, so I validated it
in increasing-risk steps — each one recoverable — before trusting a real dumper.

**Driving the update.** `iap_upload.py` drives the serial protocol over a 3.3 V USB-UART adapter
on the radio port (**USART2, PA2/PA3 = pins 12/13, 38400 8N1**), using the Pico's nRST for a
clean cold boot. The handshake, from the bootloader:

```
reset -> box: 'U' (0x55)        bootloader alive, window open
host  -> 19 78 02 17            begin-download magic
box   -> 'B' then 'S'           send me the next record
loop  -> host sends one 136-byte record per 'S'   (record 0 header, then data)
box   -> 'E' after the last record; 'R' = reject (bad magic/checksum, no retransmission)
```

A verification trick made each stage checkable without a CAN bus: at startup the application
copies its own version string into SRAM (`0x20001B86`), and **SRAM is readable over SWD** even
on the locked chip. So after any flash I can attach and read back exactly what's running.

**Stage 1 — flash the original image, unchanged.** First prove the whole procedure: pack nothing,
just re-send the stock `.iap`.

```bash
python iap_upload.py 2429-H1N4JPF20A-220524.iap --port COM17 --reset nrst
# then over SWD:
read 0x20001B86 (17 bytes) -> "H1N4JPF20A-220524"
```

The box took all 257 records, booted, and the SRAM string matched. The flash path works and the
box survives a full re-flash.

> One subtlety I hit here: the bootloader **can't program flash while the core is in debug mode**
> (same RDP-L1 rule family). A real power-cycle, or a clean nRST pulse with the probe otherwise
> idle, clears the latched debug-enable state so the write succeeds.

**Stage 2 — flash a deliberately modified image.** Now prove I *understand* the format and that
the loader accepts an image I altered. I changed one digit of the version string in the decoded
payload (`220524` → `220525`), repacked with `iaptool.py` (which recomputes the per-record
checksum), and flashed it.

```bash
python iaptool.py unpack 2429-H1N4JPF20A-220524.iap -o app.bin
# edit the version byte in app.bin
python iaptool.py pack app.bin 2429-H1N4JPF20A-220524.iap -o modified.iap
python iap_upload.py modified.iap --port COM17 --reset nrst
# over SWD:
read 0x20001B86 -> "H1N4JPF20A-220525"
```

The changed version came back. That confirms three things at once: the checksum math is right,
the obfuscation is right, and the bootloader performs **no whole-image signature check** — a
modified image is accepted and executed. (I re-flashed the stock image afterward to leave the box
as I found it.)

**Stage 3 — only now, the bootloader reader.** With the procedure proven and the format
confirmed, flashing a *functional* program instead of a byte-tweaked stock image was low-risk.

---

## 6. The dumper

The dumper is ~90 bytes of Thumb (`hiworld/dumper/dumper2.s`): set its own stack, drop a
"started" marker, `memcpy` the 8 KB bootloader and the 2 KB parameter page into SRAM, drop a
"done" marker, spin.

```asm
        .syntax unified
        .cpu cortex-m4
        .thumb
        .section .entry,"ax"
        .global _start
        .thumb_func
_start:
        .rept 128
        nop                      @ NOP sled (see "entry" note below)
        .endr
        ldr   r0, =0x20006000
        mov   sp, r0             @ our own stack (top of 24 KB SRAM)
        ldr   r0, =0x20005000     @ "STAR" marker -> entry executed
        ldr   r1, =0x53544152
        str   r1, [r0]
        ldr   r0, =0x20002000     @ copy 8 KB: bootloader 0x08000000 -> SRAM 0x20002000
        ldr   r1, =0x08000000
        ldr   r2, =0x2000
1:      ldr   r3, [r1], #4
        str   r3, [r0], #4
        subs  r2, r2, #4
        bne   1b
        ldr   r0, =0x20004000     @ copy 2 KB param page 0x0801F800 -> SRAM 0x20004000
        ldr   r1, =0x0801F800
        ldr   r2, =0x800
2:      ldr   r3, [r1], #4
        str   r3, [r0], #4
        subs  r2, r2, #4
        bne   2b
        ldr   r0, =0x20005004     @ "DONE" marker -> copy finished
        ldr   r1, =0x444F4E45
        str   r1, [r0]
3:      b     3b                  @ spin forever
        .ltorg
```

**The entry gotcha.** The bootloader doesn't enter the app at the image base. It launches it with
`bl 0x08002202` — a fixed reset veneer (`push {r4,lr}; bl __main; pop`) at offset `0x202` — after
checking a valid-app marker, and the app then runs on the bootloader's own vector table (`VTOR`
stays `0x08000000`, MSP `0x20000600`). Rather than lay my throwaway dumper's entry exactly at
`0x202`, I used the **NOP sled**: execution reaches the dump code wherever control lands in the
low part of the image — including via the fault handlers, which the bootloader forwards into that
same region (HardFault → `0x08002033`, all within the first 256 bytes). For *real* firmware you'd
put a proper reset veneer at `0x202` instead.

Build, pack, flash:

```bash
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -nostdlib -nostartfiles -Wl,-T,dumper.ld \
    -o dumper.elf dumper2.s && arm-none-eabi-objcopy -O binary dumper.elf dumper.bin
python iaptool.py pack dumper.bin 2429-H1N4JPF20A-220524.iap -o dumper.iap
python iap_upload.py dumper.iap --port COM17 --reset nrst
```

**Read the loot back.** The dumper is now spinning with the bootloader sitting in SRAM. Attach
**without resetting or halting** so the copy survives, and read it:

```python
from pyocd.core.helpers import ConnectHelper
with ConnectHelper.session_with_chosen_probe(
        target_override="cortex_m", options={"connect_mode": "attach"}) as s:
    t = s.target
    assert t.read32(0x20005000) == 0x53544152   # 'STAR'
    assert t.read32(0x20005004) == 0x444F4E45   # 'DONE'
    open("bootloader.bin", "wb").write(bytes(t.read_memory_block8(0x20002000, 0x2000)))
```

SRAM reads are never readout-protected, so this just works.

---

## 7. Results

An 8 KB bootloader, pulled out of a chip whose flash "can't be read":

```
bootloader.bin   8192 bytes
sha256  60c8a757be2e559c09bf30ea0a973766cf9251e8e3c0d2fab8f1412cda528fe0
```

Unmistakably real code — a valid Cortex-M vector table at the top:

```
[0] MSP       = 0x20000600
[1] Reset     = 0x08000149
[2] NMI       = 0x0800202B   ┐ handlers forwarded into the app region
[3] HardFault = 0x08002033   ┘ (the app runs on this table; never relocates VTOR)
```

In Ghidra at base `0x08000000` it disassembles cleanly and confirmed the IAP story from the
authoritative side: the 16-byte header magic, the 33-word checksum, the additive de-obfuscation,
programming at `0x08002000 + 128·(seq−1)`, and no whole-image signature — which is exactly why
the Stage-2 modified image flashed and ran.

### Takeaways

- **RDP Level 1 is not a wall around your own firmware** if the device can run code you supply —
  an IAP bootloader is plenty. L1 stops the *debugger*, not flash-resident code. Bounce through
  SRAM and the debugger only ever sees the unprotected RAM copy.
- **Reverse the update format first.** A byte-exact packer and a proven, staged flash procedure
  turned "run my own code on a locked chip" from a leap into three small, recoverable steps.
- **What would have stopped me: RDP Level 2.** It blocks flash-resident reads too and kills the
  debug port permanently. SWD connecting at all told me I was at L1.
- **Non-destructive beats the official unlock.** The documented way to clear RDP is a mass-erase
  that would have destroyed the one copy of the bootloader. Flashing through the vendor's own
  update path kept the device perfectly intact — and because the bootloader's own pages are
  auto-write-protected under L1, the box stays unbrickable throughout.

The tooling — `iaptool.py`, `iap_upload.py`, and the dumper — is in the repository.

---

### Hardware & tools used

- Nations **N32L406CBL7** target (on its own board)
- **Raspberry Pi Pico** + `debugprobe_on_pico.uf2` (CMSIS-DAP v2 SWD probe + nRST)
- A 3.3 V **USB-UART adapter** for the radio/update port
- **pyOCD** (SWD), **arm-none-eabi-gcc** (dumper), **Ghidra** (analysis)
- `iaptool.py`, `iap_upload.py`, `dumper2.s` (this repo)
