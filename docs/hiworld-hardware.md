# Hiworld JPF2.10 canbox — hardware teardown

Box: Hiworld (LHL) **JPF2.10**, part **H1N4JPF20A**, date code 220524, batch
`24290709S1_51110 38276 (25.11.10)`. PCB **V07.09**, Hiworld, date code **2534**
(week 34, 2025). Protocol profile `3009001` / tag JPF2 — see `canbus-architecture.md`.

---

## 0. Summary

SWD access is via a Raspberry Pi Pico running `debugprobe_on_pico.uf2` (CMSIS-DAP v2) + pyOCD
`-t cortex_m`.

- **MCU identity over SWD.** DAP IDCODE `0x2BA01477`; CPUID `0x410FC241` = **Cortex-M4 r0p1**;
  DBGMCU_IDCODE (`0xE0042000`) `0x22644017` (Nations device/rev — not in ST's numbering).
- **RDP = Level 1** (§3). SWD connects ⇒ not L2. Flash reads over SWD all fail; `FLASH_OBR`
  (`0x4002201C`) = `0x03FFFFFE` (RDPRT set); option-byte RDP1 (`0x1FFFF800`) = `0xFF`
  (unprotected would be `0xA5`).
- **ISP1 (5-pad) = SWD:** GND / SWDIO / SWCLK / 3V3 / nRST.
- **SRAM = 24 KB** — readable to `0x20005FFC`.
- **Pin usage (from the firmware binary):** USART2 on PA2/PA3, CAN on PB8/PB9 (§7).
- **Firmware/bootloader is recoverable non-destructively** under RDP-L1 — see §3.1.

The MCU gates its **debug clock in low-power/idle**, so SWD gives `No ACK` within ~1 min. Catch
it right after a power-cycle (connect+halt), then write `DBGMCU_CR` (`0xE0042004`) = `0x7` to
keep the clock alive; the session is then stable.

---

## 1. Bill of materials (read from photographs)

| Ref | Marking | Part | Function |
|---|---|---|---|
| U6 | `Nation N32L406 CBL7 / THF20015` | **Nations N32L406CBL7** | MCU — see §2 |
| — | `CSC1040 CVHaY` (SOIC-8) | **CrossIC CSC1040** | CAN transceiver, TJA1040-class |
| U1 | `7550-1` | HT7550-1 | 5 V LDO |
| U4 | `7533-1` | HT7533-1 | 3.3 V LDO |
| X2 | `16.000M` | 16 MHz crystal | MCU clock |
| D10 | `SS54` | Schottky | reverse-polarity protection |
| B1 | — | buzzer | matches the case's speaker grille |
| CN1 | 12-pin | — | vehicle harness side |
| — | 20-pin, pads 1–20 (reverse) | — | head-unit side |
| F1 | `JH` | fuse/jumper | |
| U7 | **unpopulated** QFP (reverse) | — | shared board design across variants |
| ISP1 | 5 pads (reverse) | — | **likely SWD** — see §4 |
| ISP2 | 4 pads (component side) | — | **likely UART bootloader** — see §4 |

`THF20015` and `CVHaY` are lot/trace codes, not ordering numbers.

**There is no external flash chip.** Firmware lives entirely in the MCU's internal
flash — consistent with the 35–56 KB `.iap` update files.

---

## 2. MCU — Nations N32L406CBL7

Nations Technologies (国民技术), Shenzhen.
Source: <https://www.nsingtech.com/product/general/n32l/n32l40x/n32l406>

### Part number decode

| Field | Value | Meaning |
|---|---|---|
| `N32` | — | Nations 32-bit MCU |
| `L` | low-power | product line |
| `4` | Cortex-M4F | core generation |
| `06` | 406 | sub-family |
| **`C`** | **48 pins** | K=32, **C=48**, R=64, M=80 |
| **`B`** | **128 KB** | flash (8 = 64 KB, **B = 128 KB**) |
| `L` | LQFP | package (L=LQFP, Q=QFN) |
| `7` | −40…+105 °C | temperature grade |

⇒ **LQFP-48, 128 KB flash, industrial grade.** Package confirmed independently by
LCSC (C2981444) as LQFP48 — so it is 48 pins, not 64.

### Specifications

| | |
|---|---|
| Core | Arm **Cortex-M4F @ 64 MHz** (FPU + DSP), ~80 DMIPS — **Cortex-M4 r0p1 confirmed over SWD** (CPUID `0x410FC241`) |
| Flash | **128 KB** @ `0x08000000`, **2 KB page**, encrypted eFlash with ECC, 100k cycles |
| SRAM | **24 KB** @ `0x20000000` with hardware parity — **confirmed over SWD** (read clean to `0x20005FFC`) |
| Supply | 1.8–3.6 V |
| **CAN** | **1 × CAN 2.0A/B on-die — classic CAN, NOT CAN-FD** |
| UART | 5 × U(S)ART (3 USART + 2 UART) + 1 × LPUART |
| Other | 2×SPI, 2×I²C, USB 2.0 FS, 12-bit ADC (4.5 Msps), 12-bit DAC, 2 op-amps, 2 comparators, segment-LCD driver, AES/DES/SHA/SM crypto |

**SRAM: 24 KB.** Readable over SWD to `0x20005FFC` (last word of 24 KB); the Nations download
tool's device table lists `N32L406CBL7` SRAM = `0x6000`.

The on-die CAN controller confirms the board architecture: MCU does the CAN protocol,
the SOIC-8 is only the line transceiver.

### STM32 equivalence — careful verdict

**STM32L4-*class* and source-portable — NOT a documented pin/register clone.**

What is supported: standard Cortex-M4 core, CMSIS packs, StdPeriph-style library, the
same J-Link/Keil/IAR flow, SWD debug. STM32L4 code ports with edits.

What is **not** supported by any source found: pin-for-pin or register-for-register
identity with any specific STM32L4 part. N32L406 also carries peripherals ST's L4 parts
don't ship identically (SM-series crypto, its particular LCD/op-amp mix).

**Do not assume an STM32L4 flash loader or pinout matches without checking.** The
"STM32-compatible" framing is ecosystem-level, not a register clone.

---

## 3. Readout protection — RDP Level 1

The N32L406 implements an **STM32-RDP-style three-level scheme** (L0/L1/L2) plus
separate WRP write-protection. Authoritative behaviour from the official user manual
`EN_UM_N32L40x_Series_User_Manual_V2.6.0.pdf` §2.2.1.8–2.2.1.9, Tables 2-4/2-5.

**This chip reads back at L1, confirmed three independent ways:**
1. SWD connects at all (CPUID/SRAM/registers readable) ⇒ **not L2** (L2 kills the debug port).
2. All flash reads over SWD FAIL, while SRAM/registers succeed ⇒ read protection active ⇒ **not L0**.
3. `FLASH_OBR` (`0x4002201C`) = `0x03FFFFFE` with RDPRT set; option-byte RDP1 (`0x1FFFF800`) = `0xFF`
   (L0/unprotected would be `0xA5`).

| Level | RDP1 | Flash via **debugger**? | Flash via **flash-resident code**? | Debug port |
|---|---|---|---|---|
| **L0** | `0xA5` | YES | YES | open |
| **L1** (this chip) | not `0xA5`/not L2 | **NO** | **YES** (non-debug boot) | open |
| **L2** | RDP2=`0xCC`/nRDP2=`0x33` | NO | NO | **permanently dead** |

### 3.1 KEY: the firmware/bootloader IS recoverable, non-destructively

The common belief "RDP blocks code reads too" is true only at **L2**. At **L1**, the manual
(§2.2.1.9) is explicit: *"when the program is started from the main flash memory in
non-debugging mode, the read operation of the main storage area is allowed."* So **code
executing from flash can read all 128 KB of main flash — including the 8 KB IAP bootloader
at `0x08000000`** that ships in no `.iap`.

**There is NO PCROP / execute-only / per-region read protection on N32L40x.** Table 2-4
lists the entire option-byte set — RDP1, RDP2, WRP0–3, USER, USER2, Data0/1 — nothing can
block app reads of one flash region but not another.

⇒ **The bootloader can be dumped** by delivering a small flash-resident app via the stock
`.iap` flow that reads `0x08000000..0x08001FFF` and streams it out USART2. No erase, no
unlock, nothing irreversible. Caveat: reads are allowed only for flash-resident code in
non-debug mode — run the dumper with **SWD detached**, output via UART only (SWD-direct and
SRAM-code reads stay blocked, which is exactly the behaviour observed).

### 3.2 Unlock = destruction (don't)

Rewriting RDP1 back to `0xA5` (L1→L0) triggers an **automatic full mass-erase of main
flash**, bootloader included (manual §2.2.1.9). Erasing just the option-byte block leaves
`0xFF` = still L1. Since the 8 KB bootloader exists in **no `.iap` and nowhere else**, there
is no non-destructive way to drop protection — **dump it via §3.1 first.**

### Why direct SWD dump matters less than it looks

The UART protocol is **already fully documented** (`hiworld-protocol.md`,
`hiworld-jpf2-jeep.md`) and the app region is **already decompiled** from the `.iap`
payloads (`hiworld-iap-format.md`, base `0x08002000`). The only thing not yet in hand is the
8 KB bootloader — recoverable via §3.1.

---

## 4. Programming headers

| Header | Pads | Role | Signals |
|---|---|---|---|
| **ISP1** | 5 | **SWD** | GND / SWDIO / SWCLK / 3V3 / nRST |
| **ISP2** | 4 | UART bootloader — still inferred | TX / RX / GND / VDD |

ISP1 is SWD: we established a live SWD session through it. Observed pad order as wired:
**GND, SWDIO, SWCLK, 3V3, nRST** (SWDIO = PA13, SWCLK = PA14, nRST = the dedicated reset).
ISP2 (4-pad) is still assumed to be the UART path — unverified.

Hardware used: **Raspberry Pi Pico + `debugprobe_on_pico.uf2`** (CMSIS-DAP v2 / WinUSB) +
pyOCD. Any ST-Link/V2, J-Link, or CMSIS-DAP probe works on SWD. Note the Nations MCU
Download Tool needs a **CMSIS-DAP v1 (HID)** probe and will NOT see the v2 Pico probe.

### 4.1 Live-debug procedure (works around the low-power debug-clock gating)

1. pyOCD target override `-t cortex_m` (no N32 pack needed just to attach/read/halt).
2. The MCU gates the debug clock when idle → `No ACK` within ~1 min. **Power-cycle the box
   and connect+halt in a tight retry loop** to catch it awake.
3. On first halt, write **`DBGMCU_CR` (`0xE0042004`) = `0x7`** (DBG_SLEEP|DBG_STOP|DBG_STANDBY).
   The session is then stable and reconnects reliably.

### Toolchain

- **CMSIS pack:** `Nationstech.N32L40x_DFP` (flash algorithms + `N32L406.svd`),
  <https://github.com/NSING-Community/NSING-Pack>. **Not in the Keil/pyOCD online pack index**
  (zero Nationstech entries) — download the `.pack` from GitHub and point the tool at it.
- **pyOCD:** attaches, reads SRAM/registers, halts/steps as generic `cortex_m` with **no pack**.
  The pack's `.FLM` is needed only to **program** flash. (`.FLM` files for N32L40x also ship
  inside `tools/NationsMCUDownloadTool/FLM/`.)
- **Keil MDK / IAR EWARM / J-Link:** officially supported by Nations.
- **OpenOCD:** **no** target config for N32L40x. Do **not** use `target/stm32f1x.cfg` — the
  flash controller and option-byte scheme differ.

---

## 5. CAN transceiver — CSC1040

**CSC1040**, listed by **CrossIC** as a high-speed CAN transceiver: **SOP-8,
4.75–5.25 V, 1 Mbaud, up to 110 nodes**. <https://crossic.com/product/csc1040/>

The "ST-like" logo is the vendor's own house mark, **not** STMicroelectronics.

Spec profile is identical to the **NXP TJA1040**, so this is best read as a
TJA1040-class / house-marked replacement.

**Provenance caveat:** CrossIC aggregates many makers' datasheets and lists the maker
only as "CS Chips". **No full CSC1040 datasheet was obtained**, so the manufacturer
identity rests on that listing alone.

### Pinout — INFERRED from TJA1040, not a CSC1040 datasheet fact

| Pin | Signal | Function |
|---|---|---|
| 1 | TXD | from MCU CAN_TX |
| 2 | GND | |
| 3 | VCC | +5 V |
| 4 | RXD | to MCU CAN_RX |
| 5 | Vio / NC | |
| 6 | CANL | bus low |
| 7 | CANH | bus high |
| 8 | STB | standby (low = normal) |

Verified NXP TJA1040 SO-8 pinout
(<https://www.nxp.com/docs/en/data-sheet/TJA1040.pdf>). Given the matching spec sheet
this is the most likely arrangement — **but verify pin-1 orientation and pins 6/7
(CANH/CANL) with a meter before trusting it.**

---

## 6. LQFP48 pin map + flash memory map (confirmed)

Pin number ↔ signal from the **datasheet** `N32L406CBL7.pdf` (LQFP48 is pin-identical to
STM32F103 LQFP48). Signal ↔ actual box usage from the **decompiled firmware + binary
literal constants** (`usart2_init`, the GPIO/AF helper, and the resolved port bases —
GPIOA `0x40010800`, GPIOB `0x40010C00`, USART2 `0x40004400`), *not* from our reconstruction.

| LQFP48 pin | MCU pin | Box use | AF / note |
|---|---|---|---|
| 12 | PA2 | **radio UART TX** (box→HU) | USART2_TX, AF4 |
| 13 | PA3 | **radio UART RX** (HU→box) | USART2_RX, AF4 |
| 45 | PB8 | **CAN RX** (from transceiver RXD) | bxCAN, AF5 |
| 46 | PB9 | **CAN TX** (to transceiver TXD) | bxCAN, AF5 |
| 34 | PA13 | SWDIO | fixed-function |
| 37 | PA14 | SWCLK | fixed-function |
| 7  | NRST | reset | — |
| 44 | BOOT0/PD0 | boot select | high+reset → Nations factory serial/USB loader |
| 23/35/47 | VSS | ground | (pin 8 = VSSA) |
| 24/36/48 | VDD | 3.3 V | — |

Radio UART is **38400 8N1**. Tap PA2/PA3 directly for clean 3.3 V non-inverted logic
(the board-level radio link is level-shifted/likely inverted via transistors). These pins
are **3.3 V, NOT 5 V tolerant** — never put the 5 V board side on them. Driving PA3 directly
conflicts with the on-board buffer unless the radio is disconnected. CAN bitrate: body bus
83.333k (+125k autobaud) — see `hiworld-iap-flashing-flow.md` / `canbus-architecture.md`.

### Flash / system memory map (confirmed over SWD + from manual)

```
0x08000000 .. 0x08001FFF   8 KB   IAP bootloader   (owns vectors; in NO .iap; dump via §3.1)
0x08002000 .. 0x0801F7FF  118 KB  application       (the .iap payload; decompiled)
0x0801F800 .. 0x0801FFFF   2 KB   parameter / emulated-EEPROM page (param_page_load/save)
---- separate "system information area" (not main flash) ----
0x1FFF0000 .. 0x1FFF3FFF  16 KB   factory boot ROM  (Nations ISP/DFU; "cannot be changed"; unreadable at L1)
0x1FFF4000 ..  ~          ~        system config block (chip info)
0x1FFFF800 .. 0x1FFFFFFF   2 KB   option / info page (READABLE at L1; RDP1/WRP/USER bytes)
0x20000000 .. 0x20005FFF  24 KB   SRAM
```
The 16 KB factory boot ROM is what reads back as unstable bus artifacts over SWD (locked);
the 8 KB **vendor** IAP bootloader at `0x08000000` is in main flash and is the dump target.

---

## 7. Open / unverified

1. **ISP2 (4-pad) role** — assumed UART, not yet verified.
2. **CSC1040 pinout and true manufacturer** — no datasheet obtained; TJA1040-class assumed.
3. **STB control GPIO** — which MCU pin drives the transceiver standby (pin 8) not yet
   pulled from the binary.
---

The 8 KB bootloader is dumped and fully analyzed: `hiworld/extracted/bootloader_0x08000000_8k.bin`
(sha256 `60c8a757…`); see `hiworld-bootloader-analysis.md`. It enters the application via
`bl 0x08002202` (the app's reset veneer → `__main`), gated by a valid-app marker at `0x08001800`;
the application runs on the bootloader's vector table (`VTOR = 0x08000000`, MSP `0x20000600`).
