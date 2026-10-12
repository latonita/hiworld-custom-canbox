# Hiworld canbox 8 KB IAP bootloader — full reverse-engineering report

**Target:** the vendor IAP bootloader occupying `0x08000000`–`0x08001FFF` (8 KB) of the
Nations **N32L406CBL7** in the Hiworld **H1N4JPF20A** canbox.
**Source:** `hiworld/extracted/bootloader_0x08000000_8k.bin` (sha256 `60c8a757…`),
extracted non-destructively via an IAP-delivered SRAM-copy dumper (see
`hiworld-hardware.md` §3.1). Analyzed in Ghidra (`ARM:LE:32:Cortex`, base `0x08000000`).
Every claim here is **DECOMPILED** from that image unless marked INFERRED.

---

## 0. One-paragraph summary

The bootloader is a small, self-contained Cortex-M4 program that owns the chip's real
vector table and, on every boot, offers a brief UART "In-Application Programming" window
before launching the application at `0x08002202` (its reset veneer). It speaks an unframed, stop-and-wait
serial protocol on USART2 @ 38400 8N1 (`U` → `19 78 02 17` → `B`/`S` → 136-byte records →
`E`), verifies a per-record 33-word checksum, de-obfuscates each record with a 4-byte
additive key shipped in the file header, and programs the application region of internal
flash. It performs **no** cryptographic authentication of the image — only a format/
checksum check — and it is itself unerasable in normal operation (pages 0–1 are
auto-write-protected under RDP-L1).

---

## 1. Memory & boot architecture

```
0x08000000  +-----------------------------+  <- vector table (OWNED BY BOOTLOADER)
            |  [0] MSP      = 0x20000600  |     the app never relocates VTOR; it runs
            |  [1] Reset    = 0x08000149  |     on THIS table (VTOR stays 0x08000000)
            |  [2] NMI      = 0x0800202B  |  ┐  exception handlers are FORWARDED into
            |  [3] HardFault= 0x08002033  |  ├  the application region, so the app
            |  ...  SVC/PendSV/SysTick -> 0x0800015B/5F/61 (bootloader's own)
0x08000148  |  Reset_Handler / crt0       |  -> InitializeSystem, then HandleIapProtocol
0x08000188  |  code (functions below)     |
0x08001FFF  +-----------------------------+
0x08002000  ...... APPLICATION ...... (programmed via IAP; entry at 0x08002202, marker-gated)
0x0801F800  parameter / emulated-EEPROM page (2 KB)
0x20000000  SRAM 24 KB  (MSP top = 0x20000600, i.e. ~1.5 KB system stack)
```

Boot sequence:

```
POWER-ON / reset
  -> Reset vector 0x08000149
  -> crt0: copy .data, zero .bss  (ldr-pool at 0x08000170: SystemInit ptr, main ptr, RAM ptrs)
  -> InitializeSystem        (0x08000eac)  clocks + flash latency + VTOR
       -> InitializeSysClock (0x08000d82)  HSE 16 MHz -> PLL -> 64 MHz
            -> SetClockStep  (0x08000d34)
  -> HandleIapProtocol @ normal-boot entry 0x08000bcc   (the "main")
       -> InitializeUsart2(38400)
       -> IAP window loop (see §2)
       -> if valid-app marker set: bl 0x08002202 (app reset veneer -> __main)
```

---

## 2. The IAP protocol (decompiled from `HandleIapProtocol`)

Two entry points into the same function:
- **`0x08000bcc` (normal boot / "main")** — the `U`-advertising window.
- **`0x08000888` (transfer handler)** — the record-streaming engine, re-entered when a
  begin-magic is seen.

### 2.1 Normal-boot window (0x08000bcc)

```
InitializeUsart2(0x9600)          ; 38400 8N1 on PA2/PA3
loop:
    SendUartByte('U' = 0x55)       ; advertise "bootloader alive"
    b = ReceiveUartByte(0)         ; ~60000-tick timeout
    if (received):
        if b0 == 0x19:  recv b1,b2,b3; if "19 78 02 17" -> goto transfer (0x08000888)
        if b0 == 0x20:  recv b1,b2,b3; if "20 08 10 20" -> change baud (DAT_08000e90),
                                                            then goto transfer
    else:
        SendUartByte('E' = 0x45)   ; timeout, no flasher
    if (retry budget exhausted) break
    delay
-> if *0x08001800==0x19780217: bl 0x08002202 (app entry); else stay in IAP
```

So **`U` is emitted unconditionally every boot**, and `E` closes the window — exactly the
bench behaviour observed. There are **two** begin-magics: the standard `19 78 02 17`, and
`20 08 10 20` which additionally switches the UART baud before transferring.

### 2.2 Transfer engine (0x08000888)

```
SendUartByte('B' = 0x42)
SendUartByte('S' = 0x53)           ; "send record 0"
recv 136 bytes  (record 0 = header)
verify 16-byte target magic:
    01 09 07 08 00 02 01 07 91 32 47 a4 4e 33 14 f2
    (= 12-byte universal magic + a 4-byte PER-BOX tag [12:16]; this 2429 loader requires
     4e 33 14 f2 — other boxes carry different [12:16], e.g. 2883=4e 33 16 06, 2948=49 33 09 f1.
     [12:14] is ASCII family "N3"/"I3". So this is a cross-flash compatibility guard.)
if magic bad: SendUartByte('R'); SendUartByte('E'); return
UnlockFlash
erase application pages:  for addr in [start .. end] step 0x800: EraseFlashPage(addr)
loop over data records:
    SendUartByte('S')              ; request next record
    recv 136 bytes
    verify  sum(record[0..131] as 33x u32 LE) == record[132..135]   ; 33-word checksum
            (else SendUartByte('R'); return)
    de-obfuscate 128 payload bytes:  plain[i] = cipher[i] - i - key[i&3]
            key = header bytes [132..135]  (shipped in the clear)
    seq = record[128..129] (u16 LE)
    for off in 0..127 step 4: ProgramFlashWord(appbase + 128*(seq-1) + off, word)
    if record[0x83] (last-flag) != 0:
        program trailing words (param page / finalize)
        SendUartByte('E' = 0x45)
        ResetViaWatchdog()         ; WWDG_CR=0xBF -> immediate reset -> reboots into new app
```

**Protocol byte legend** (all confirmed from the sender side here):

| Byte | Dir | Meaning |
|---|---|---|
| `U` 0x55 | box→host | bootloader alive, window open (every boot) |
| `19 78 02 17` | host→box | begin download (standard) |
| `20 08 10 20` | host→box | begin download + baud change |
| `B` 0x42 | box→host | begin accepted |
| `S` 0x53 | box→host | send next record (strict stop-and-wait) |
| `R` 0x52 | box→host | reject (bad magic/checksum) — **no retransmission** |
| `E` 0x45 | box→host | end / window closed |

### 2.3 Format details enforced by the loader

- **Record checksum:** 33-word little-endian modular sum over `record[0:132]`.
- **Obfuscation:** `plain[i] = cipher[i] - i - key[i&3]`, key from record 0 `[132:135]`.
- **Program address:** `0x08002000 + 128*(seq-1)`.
- **Record-0 target magic:** 16 bytes — 12-byte universal magic + 4-byte per-box tag.
- **Erase:** a bulk app-region page-erase after record 0 (post magic-check, before the data
  records) — not per-record, and not on the begin-magic.
- **Integrity:** per-record checksum + record-0 magic only; no whole-image signature or MAC, so
  a correctly repacked image is accepted.

See `hiworld-iap-format.md` for the full container format.

---

## 3. Function catalogue (renamed & commented in Ghidra)

| Address | Name | Role |
|---|---|---|
| 0x08000148 | *(Reset/crt0)* | reset vector target; data/bss init; calls SystemInit + main |
| 0x08000eac | **InitializeSystem** | SystemInit: periph clocks, flash latency, VTOR=0x08000000 |
| 0x08000d82 | **InitializeSysClock** | HSE 16 MHz → PLL → 64 MHz, poll ready/lock |
| 0x08000d34 | **SetClockStep** | clock-source enable/wait substep |
| 0x08000888 | **HandleIapProtocol** | the core: IAP window + transfer engine + app jump |
| 0x080002e4 | **ResetViaWatchdog** | force reset via `WWDG_CR=0xBF` loop (never returns) |
| 0x08000630 | **InitializeUsart2** | USART2 PA2/PA3 bring-up at a given baud |
| 0x0800055a | **SetUsartRegisters** | program USART CR1/2/3 + BRR |
| 0x080001b6 | **GetPeripheralClock** | derive PCLK for BRR/timing |
| 0x0800067c | **SendUartByte** | blocking TX (TXE wait, ~30000 timeout) |
| 0x080006ac | **ReceiveUartByte** | blocking RX into buf[idx], fixed timeout |
| 0x080006e6 | **ReceiveUartByteTimeout** | blocking RX, configurable timeout |
| 0x0800071e | **EnableHsiClock** | turn on HSI (RCC HSION/HSIRDY) — required for flash program/erase |
| 0x08000766 | **GetFlashStatus** | decode FLASH_STS → status code |
| 0x080007b2 | **WaitFlashReady** | poll GetFlashStatus while busy |
| 0x080007d8 | **ProgramFlashWord** | word-aligned 32-bit flash program |
| 0x08000834 | **EraseFlashPage** | 2 KB page erase |
| 0x080001a0 | **DelayMs** | coarse busy-wait delay |
| 0x0800019a | **DelaySpinLoop** | innermost calibrated spin |
| 0x0800034a | **SetGpioPinConfig** | low-level GPIO MODER/AFR/… programming |
| 0x0800052e | **SetGpioAltFunction** | configure one pin's alternate function |

(`FUN_08000028`–`0x40` are the exception-vector/table region Ghidra mis-grouped — not real
functions.)

### 3.1 Key global pointers (literal pools, now labelled in Ghidra)

Resolved from the flash literal pools at `0x08000a2c…` and `0x08000e60…`:

| Label | Value | Meaning |
|---|---|---|
| `p_GPIOA` / `p_GPIOB` | `0x40010800` / `0x40010C00` | GPIO port bases |
| `p_USART2_A` / `p_USART2_DR` | `0x40004400` / `0x40004404` | USART2 base / data register |
| `p_RCC_A` | `0x40021000` | RCC base |
| `p_FLASH_A` | `0x40022000` | FLASH controller base |
| `g_flashKey1` / `g_flashKey2` | `0x45670123` / `0xCDEF89AB` | FLASH unlock keys (written to KEYR) |
| `g_rxTimeout` | `0x000F4240` (1,000,000) | RX poll timeout |
| `g_fastBaud` | `0x000927C0` (600,000) | baud used by the `20 08 10 20` fast path |
| `p_rxRecord` | `0x20001004` | 136-byte record RX buffer (RAM) |
| `p_deobfKey` | `0x200010C5` | 4-byte de-obfuscation key (from record 0) |
| `g_appRegionStart`/`End` | `0x08002000` / `0x0801F800` | app region erased on each update |
| `g_bootFlagPage`/`Addr` | `0x08001800` | bootloader's own last page (valid-app marker) |
| `g_bootMagic1/2/3` | `0x19780217` / `0xA4473291` / `0x01043749` | valid-app markers (see §4.1) |

### 4.1 Valid-app marker mechanism (new)

On a **successful** flash (last record), before resetting, the bootloader writes three
constants into its **own** last page / the bytes just below the app:
`0x19780217 → 0x08001800`, `0xA4473291 → 0x08001F80`, `0x01043749 → 0x08001F84`.
These act as a "good application present" stamp (note `0xA4473291` is the header-magic tail
`91 32 47 a4` as a word). The start-of-transfer erase clears `0x08001800` first, so the
stamp only exists after a complete, checksum-valid image — the bootloader's way of deciding
whether to launch the app vs. stay in the IAP window. (Exact check site: the boot-path
condition `*g_bootFlagAddr == g_bootMagic1`.)

---

## 4. The application-entry convention

The boot decision and launch are explicit in `HandleIapProtocol` (0x08000ce8–0x08000cfa):

```
r0 = *(0x08001800)            ; read the valid-app marker
if (r0 == 0x19780217)         ; marker present (written by IAP on a good flash)
    bl 0x08002202             ; LAUNCH APP
else
    goto IAP window           ; no valid app -> stay in the loader
```

- **App entry point = `0x08002202`** — a fixed offset (0x202) into the image. The stock app
  has a reset veneer there: `push {r4,lr}; bl 0x08002400 (__main); pop {r4,pc}`, i.e.
  `0x08002202 → __main → scatterload → startup_crt0 → main`. It is reached by `bl` (a call),
  so the app is **entered as a subroutine**.
- The app runs on the **bootloader's vector table** (`VTOR=0x08000000`, MSP=`0x20000600`); it
  never sets its own VTOR. Confirmed live over SWD.
- **Boot gate:** the launch is conditional on the valid-app marker `*0x08001800 == 0x19780217`
  (§4.1). A blank/half-erased app region has no marker, so the loader stays in the IAP window.
- The bootloader's vector table forwards the fault/exception handlers into the app region
  (NMI `0x0800202B`, HardFault `0x08002033`, MemManage `0x0800203B`, BusFault `0x08002043`,
  UsageFault `0x0800204B`, SVC `0x0800015B`, PendSV `0x0800015F`, SysTick `0x08000161`).

**For custom firmware:** place the entry — a reset veneer that runs `__main`/scatterload, or a
jump to your own `main` — at offset `0x202` (`0x08002202`), and run on the inherited MSP/VTOR.
The IAP flash writes the valid-app marker at `0x08001800`, so the loader will launch the image.

---

## 5. Security assessment

- **No image authentication.** Only a 16-byte header magic + per-record 33-word sum. Any
  correctly-packed image is flashed and run. (This is what makes the custom-firmware path
  viable.)
- **Obfuscation, not encryption.** The de-obfuscation key ships in the clear in record 0.
- **RDP-L1** protects flash *reads* from a debugger but not from the running bootloader/app;
  pages 0–1 (the bootloader) are auto-write-protected, so IAP cannot overwrite itself —
  the box is effectively unbrickable through the IAP path.
- **Unframed shared line.** The same `/dev/ttyMT3` @ 38400 carries normal AA55 traffic and
  the raw bootloader protocol; a stray transmitter during an update corrupts a record and
  triggers `R` (no retransmission) → failed update. (Matches the plugin-interference hazard
  noted in `hiworld-iap-flashing-flow.md`.)

---

## 6. Cross-references

- Extraction method & RDP model: `hiworld-hardware.md` §3, §3.1
- `.iap` container format: `hiworld-iap-format.md` (magic now 16 B; erase-after-record-0)
- On-wire update flow (radio side): `hiworld-iap-flashing-flow.md`,
  `hiworld-iap-update-sequence.md`
- Upload tooling: `hiworld/iap_upload.py`, dumper `hiworld/dumper/dumper2.s`
