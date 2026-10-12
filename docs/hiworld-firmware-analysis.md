# Hiworld JPF2 canbox — MCU firmware analysis (`H1N4JPF20A-220524`)

Static reverse engineering of the **vehicle-CAN side** of the Hiworld (LHL) JPF2.10 box,
part `H1N4JPF20A`, firmware date code 220524, protocol_id `3009001`/`3009002`.

This is the half that could not be seen from the Android plugin. It answers:
which CAN IDs the box reads off the car, what it extracts from them, and how those bytes
become the `AA 55` UART frames documented in
[`hiworld-protocol.md`](./hiworld-protocol.md) and [`hiworld-jpf2-jeep.md`](./hiworld-jpf2-jeep.md).

**Evidence tags**

| Tag | Meaning |
|---|---|
| **DECOMPILED** | Read directly out of Ghidra's decompilation; the address is quoted so it can be re-checked. |
| **CROSS-CHECKED** | Independently agrees with the captured UART frames and/or the decompiled Android plugin. |
| **INFERRED** | Reasoning from indirect evidence. Confidence stated inline. Never presented as fact. |

---

## 0. TL;DR — the things that matter

1. **There is no CAN acceptance-filter table.** Filter bank 0 is configured with
   ID = 0 and mask = 0 → **the box accepts every CAN ID on the bus** and filters in software.
   Any "CAN ID list" therefore has to come from the RX dispatch code, and it does (§3).
   **DECOMPILED.**
2. The box drives **one** CAN controller on **one** bus, and **auto-detects between two
   bit rates**: **83.333 kbit/s** and **125 kbit/s**. No 500 kbit/s anywhere. **DECOMPILED.**
3. There are **two complete, mutually exclusive CAN decode profiles**, one per bit rate.
   Profile 0 (83.333 kbit/s) reads CAN IDs `0x000 0x003 0x013 0x015 0x210 0x3A0 0x500`;
   profile 1 (125 kbit/s) reads `0x1E1 0x20B 0x20E 0x211 0x244 0x308 0x3A3 0x513`.
   **DECOMPILED.**
4. The **UART message set is a literal table in flash** at `0x08009F2C` — six TX messages
   (`0x72 0xA6 0x73 0xF0 0x12 0x32`) and two RX messages (`0xD2 0xAD`), with their exact
   lengths and their source/destination RAM buffers. Every length matches the capture.
   **DECOMPILED + CROSS-CHECKED.**
5. **`0xFF` is an ACK, not a poll — settled.** The firmware has *no* handler for `0xFF`
   at all. It free-runs its six TX messages in table order, and the captured order
   `73 → F0 → 12 → 32 → 72 → A6` is exactly the table order starting at index 2.
   **DECOMPILED + CROSS-CHECKED.**
6. **`0x73` is settled too, and it is a disappointment:** its 8-byte payload buffer
   (`0x20001B7D..0x20001B84`) has **zero references in the entire image**. It is never
   written, so the box always sends `AA 55 08 73 00 00 00 00 00 00 00 00 7A`.
   It is a reserved slot carrying no data in this build. **DECOMPILED + CROSS-CHECKED.**
7. The box **transmits on the vehicle CAN bus** — the amplifier control frame and the
   instrument-cluster text frames. IDs in §5.

---

## 1. Analysis setup and quality

### 1.1 Ghidra project

| Item | Value |
|---|---|
| Project file | `<ghidra_projects>/hiworld.gpr` |
| Program | `/2429-H1N4JPF20A-220524.app.bin` |
| Ghidra | 12.1.3 PUBLIC, headless (`analyzeHeadless.bat`), Java 21 |
| Loader | **Binary Loader** |
| Language | **`ARM:LE:32:Cortex`**, default compiler spec |
| Base address | **`0x08002000`** |
| Extra memory blocks | `SRAM` `0x20000000` len `0x6000` RW · `PERIPH` `0x40000000` len `0x20000000` RW volatile · `SCS` `0xE000E000` len `0x1000` RW volatile |
| Helper scripts | `…\scratchpad\ghidra\scripts\{AddMem,ForceDisasm,Sweep,MkFunc,FixBody,DumpAll,Final,D2}.java` |
| Bulk output | `…\scratchpad\ghidra\out1\{all_decomp.c, funclist.txt, can_dispatch.c, xrefs.txt}` |

Exact headless command used for the import:

```
analyzeHeadless.bat <projdir> hiworld ^
  -import 2429-H1N4JPF20A-220524.app.bin ^
  -processor ARM:LE:32:Cortex -loader BinaryLoader -loader-baseAddr 0x08002000 ^
  -scriptPath <scripts> -preScript AddMem.java
```

To reproduce in the GUI: *File → Import File*, format **Raw Binary**, language
`ARM / v8 / 32 / little / Cortex`, then in *Options* set **Base Address `08002000`**;
afterwards add the three memory blocks above in the Memory Map window and run
auto-analysis. Everything below is reproducible from that state.

### 1.2 Coverage — honest numbers

| Stage | Functions | Instruction bytes | Defined data | Coverage of 32 768 B |
|---|---|---|---|---|
| Default auto-analysis | 73 | 6 766 | 1 400 | 25 % |
| + Thumb `push {…,lr}` prologue scan, re-analysed | 378 | 19 430 | 1 603 | 64 % |
| + linear Thumb sweep of the remaining gaps | 383 | 31 120 | 1 607 | 97 % |

Two caveats, stated plainly:

* Ghidra's default analysis is poor on this image (25 %) because the file **starts with
  Thumb code, not a vector table** (`1f b5 00 f0 44 f9` = `push {r0-r4,lr}` / `bl`), so
  almost nothing seeds the disassembler. Forcing functions at every `0xB5xx` prologue is
  what unlocks it.
* The final linear sweep also disassembles literal pools as if they were code. That is
  harmless for the functions analysed here (all of which were recovered at the 64 % stage
  or by explicitly setting a function body), but a handful of "functions" in the project
  will contain nonsense. **Do not trust a decompilation from this project that is not
  listed in §7.**

### 1.3 Where is the vector table?

**There isn't one in this image.** A scan for any run of aligned 32-bit words pointing
into `0x08002000..0x0800A000` with bit 0 set finds nothing — the only long aligned runs are
zero fill at `0x0800229C` and `0x08009FC4`. The bootloader at `0x08000000` (not in this
file) keeps `VTOR` pointing at **its own** table and branches to `0x08002000`; the
application's ISRs are reached through the bootloader's vectors. Consistent with this, the
image never writes `SCB->VTOR` (`0xE000ED08`). **DECOMPILED (by absence) + INFERRED.**

Practical consequence: the CAN and USART ISRs are *not* identifiable from a vector table.
They were found instead by following the peripheral register accesses (§2).

---

## 2. Memory map and peripherals

The MCU is a Nations **N32L406CBL7**, and its peripheral map is an **STM32F1 clone**, not an
STM32L4 one. Every base address found in the literal pools lands on an F1 address.
**DECOMPILED** (literal-pool scan of the whole image).

| Address | Peripheral | Used for |
|---|---|---|
| `0x08000000` | Flash — bootloader, 8 KB | not in this file |
| `0x08002000` | Flash — application | this image, 32 768 B, ends at `0x08009FC3` + zero fill |
| `0x0801F800` | Flash — parameter page | persistent config (§6) |
| `0x20000000` | SRAM 24 KB | see §4 for the message buffers |
| `0x40004400` | **USART2** | **head-unit link, 38 400 8N1** |
| `0x40004800` | USART3 | init code present, unreferenced |
| `0x40013800` | USART1 | init code present, unreferenced |
| `0x40006400` | **bxCAN1** | **the vehicle bus** |
| `0x40006600` | bxCAN1 filter bank registers | `CAN_FilterInit` |
| `0x40010000/0400` | AFIO / EXTI | `AFIO_MAPR` CAN remap |
| `0x40010800/0C00/1000/1400` | GPIOA/B/C/D | |
| `0x40021000` | RCC | |
| `0x40022000` | FLASH interface | parameter-page write |
| `0x40002C00` | WWDG | used as a **forced reset** (§7.4) |
| `0x40000000…0x40001400` | TIM2…TIM7 | |
| `0x40007000` | PWR | |

Pin assignment, from the clock/GPIO enables in the init functions — **DECOMPILED**:

* **CAN**: `AFIO_MAPR` CAN remap bit pattern `0x02000000` written, GPIOB bits `0x100`/`0x200`
  configured → **PB8 = CAN_RX, PB9 = CAN_TX**.
* **UART**: GPIOA pins 2 and 3 configured with USART2 → **PA2 = TX (to head unit),
  PA3 = RX**.
* Four discrete outputs driven from the decoded status byte (§4.3): GPIOB5, GPIOB6,
  GPIOB4, GPIOC15.

---

## 3. THE VEHICLE CAN SIDE

### 3.1 CAN controller initialisation — `can_init @ 0x08002A90`

**DECOMPILED.** `can_init(bitrate_kbps, mode)` fills an STM32-StdPeriph
`CAN_InitTypeDef` and calls `CAN_Init` (`0x0800398C`, a verbatim StdPeriph
`CAN_Init`, recognised by its `CAN_BTR` assembly):

```c
init.CAN_Prescaler = 2000 / bitrate_kbps;   /* local_18 */
init.CAN_Mode      = mode;                  /* 0 = NORMAL (only value ever passed) */
init.CAN_SJW       = 0;                     /* SJW = 1 tq  */
init.CAN_BS1       = 3;                     /* BS1 = 4 tq  */
init.CAN_BS2       = 2;                     /* BS2 = 3 tq  */
init.CAN_TTCM = init.CAN_ABOM = init.CAN_AWUM = init.CAN_NART = init.CAN_RFLM = 0;
init.CAN_TXFP = 1;                          /* FIFO-priority TX */
CAN_Init(CAN1 /*0x40006400*/, &init);
CAN_ITConfig(CAN1, CAN_IT_FMP0 /*0x02*/, ENABLE);
```

Nominal bit time = `SJW + BS1 + BS2` = `1 + 4 + 3` = **8 tq**, sample point at 62.5 %.

Solving `bitrate = PCLK1 / (prescaler × 8)` with `prescaler = 2000 / kbps` gives
**PCLK1 = 16 MHz** exactly for both rates. **DECOMPILED + arithmetic.**

| Caller | Argument | Prescaler | Resulting bitrate |
|---|---|---|---|
| `can_apply_bitrate_* @ 0x08007B2C / 0x08007B4C`, `config[1] == 1` | `0x7D` = 125 | 16 | **125.000 kbit/s** |
| same, `config[1] == 0` | `0x53` = 83 | 24 (2000/83 truncates) | **83.333 kbit/s** |

> **No 500 kbit/s path exists anywhere in the image.** This box does **not** touch
> Chrysler CAN-C. It is an interior-bus-only device. **DECOMPILED.**

### 3.2 CAN acceptance filters — `CAN_FilterInit @ 0x08003AA0`

**DECOMPILED.** `can_init` builds exactly one `CAN_FilterInitTypeDef` and calls the
StdPeriph `CAN_FilterInit` once:

```c
filter.CAN_FilterIdHigh     = 0;   /* local_28 */
filter.CAN_FilterIdLow      = 0;   /* local_26 */
filter.CAN_FilterMaskIdHigh = 0;   /* local_24 */
filter.CAN_FilterMaskIdLow  = 0;   /* local_22 */
filter.CAN_FilterFIFOAssignment = 0;   /* FIFO 0 */
filter.CAN_FilterNumber     = 0;
filter.CAN_FilterMode       = 0;   /* IdMask */
filter.CAN_FilterScale      = 1;   /* 32-bit */
filter.CAN_FilterActivation = 1;   /* ENABLE */
```

Mask = 0 in IdMask mode ⇒ **every identifier matches**. There is **no hardware CAN-ID
table in this firmware, and I am not going to invent one.** The real ID list is the set of
constants compared in the software dispatch, §3.4/§3.5, and that list is exhaustive and
literal.

### 3.3 CAN receive path

**DECOMPILED.**

* `can_rx0_irq_push_ring @ 0x08002C0A` — the FIFO-0 message-pending ISR. Calls the
  StdPeriph `CAN_Receive` (`0x08003E2E`) and pushes the message into a ring buffer:
  **50 entries × 16 bytes at `0x20001284`**, producer index `0x20001118`,
  consumer index `0x2000111C`.
  Entry layout: `[0..1]` ID high half, `[2..3]` ID low half, `[5]` DLC, `[6..13]` data 0..7.
  If the ring is nearly full the frame is read and discarded (overflow flag set).
* `can_poll_dispatch @ 0x08007D44` — called from the periodic task. Selects the profile:

```c
if (*(uint8_t*)0x2000118F == 1) can_dispatch_profile1_125k();   /* 0x08009680 */
else                            can_dispatch_profile0_83k();    /* 0x08009010 */
```

  `0x2000118F` is a copy of persistent-config byte `[1]`, the same byte that selects the
  bit rate. **Bit rate and decode profile are locked together.**
* Each dispatcher drains **up to 20 ring entries per call** in a `for (i = 0; i <= 0x13; i++)`
  loop and compares the 32-bit identifier against literal constants.

### 3.4 Autobaud — how the profile is chosen

**DECOMPILED**, `main @ 0x080088D8`:

```c
config_load_defaults();          /* 0x08007B6C — magic 0xAA at config[0]; fresh config -> config[1] = 1 */
can_apply_bitrate_A();           /* 0x08007B2C — config[1]==1 ? 125 kbit/s : 83.333 kbit/s */
...
while (can_ring_produce_index < 3) {          /* wait for 3 received CAN frames */
    if (timer_tick) {
        if (retry_count < 8) {
            config[1] = (config[1] != 1);     /* toggle 125 <-> 83.333 */
            can_apply_bitrate_A();            /* re-init CAN at the other rate */
            retry_count++;
        } else { watchdog_kick_or_reset(); }
    }
}
config_mirror_0x2000118F = config[1];   /* lock in the rate that produced traffic */
config_load_defaults();
can_apply_bitrate_B();                  /* 0x08007B4C, same selection */
/* ... then the main loop ... */
```

So the box **probes both rates until it hears at least three frames**, then commits.
The default on a virgin parameter page is 125 kbit/s.

### 3.5 Which profile is this box actually running? — **INFERRED, high confidence**

`config_load_defaults @ 0x08007B6C` sets the initial reported volume from the profile flag:

```c
uVar3 = (*0x2000118F == 1) ? 0x1E /*30*/ : 0x14 /*20*/;
amp_state[0] = uVar3;          /* = 0xA6 payload[0] */
```

The captured `0xA6` frame is `AA 55 08 A6 **14** 0A 0A 0D 0A 0A 00 F6` — payload[0] = `0x14`
= 20. That is the **profile-0 / 83.333 kbit/s** value. **CROSS-CHECKED.**

This is consistent with the vehicle list on the box (2005–2010 Grand Cherokee WK,
2005–2010 300C LX): those are Mercedes-derived platforms whose interior body bus runs at
**83.333 kbit/s**, whereas the 125 kbit/s profile fits the later Chrysler CAN-IHS cars in
the same listing (Wrangler JK, Compass/Patriot MK). That vehicle attribution is
**INFERRED** — the firmware names no vehicles. The *bit rate* and the *ID sets* are
DECOMPILED fact; which car sits behind which is the inference.

### 3.6 CAN RX dispatch — profile 0, 83.333 kbit/s (`can_dispatch_profile0_83k @ 0x08009010`)

All **DECOMPILED**. `d[n]` = CAN data byte n.

| CAN ID | Bytes read | Decoded meaning | Lands in |
|---|---|---|---|
| **`0x000`** | `d[0]` | `flags.bit4 = d[0] & 1` ; `flags.bit0 = ((d[0] & 0xE0) > 0x10)` | UART `0x72` payload `[0]` bits 4 and 0 |
| **`0x003`** | `d[2]` | `flags.bit2 = ((d[2] & 0x0F) == 1 \|\| (d[2] & 0x0F) == 9)` — **reverse gear** | UART `0x72` payload `[0]` bit 2 |
| **`0x013`** | `d[2]` | **Doors.** `bit0→LF, bit1→RF, bit3→LR, bit2→RR, bit4→trunk` | UART **`0x12` payload `[2]`** bits 7,6,5,4,3 |
| **`0x015`** | `d[4] d[5]` | **Steering angle.** `raw = d[4]<<8 \| d[5]`; `angle = (0x1000 − raw) / 2` (0 if `raw == 0` or `raw == 0x1000`). Centre `0x1000`, **0.5° / LSB**. Gated on `flags.bit2` (reverse) | UART **`0x72` payload `[4..5]`**, signed 16-bit big-endian |
| **`0x210`** | `d[1]` | `flags.bit1 = (d[1] != 0)`; `value = min(d[1] >> 1, 100)` | UART `0x72` payload `[0]` bit 1 and payload `[3]` |
| **`0x3A0`** | `d[0] d[1]` | **Steering-wheel control buttons** — see table below | UART **`0x72` payload `[2]`** |
| **`0x500`** | `d[0] d[1]` | copied out and **retransmitted as CAN `0x516`** (vendor echo) | — |
| `0x400…0x43F` | `d[0] d[1] d[2]` | Hiworld node-addressing / service protocol (§5.3) | — |
| `0x7FC` | `d[0..7]` | vendor handshake, magic `3F F3 F2 01 · · 3A A3` → replies CAN `0x7FD` | — |
| `0x7FF` | `d[0..7]` | vendor **status injection**, magic `7F F7 · · · · 3A A3` → `flags` bits 0..3 = `d[2]` bits 0..3 | UART `0x72` payload `[0]` |

**SWC key decode, CAN `0x3A0`** — the values written are exactly the key codes the Android
plugin's `car_key_3009001.json` maps. **DECOMPILED + CROSS-CHECKED.**

| Source bit | `0x72[2]` code | Plugin meaning |
|---|---|---|
| `d[0]` bit 0 | `0x0B` | MODE / source (`SCR`) |
| `d[0]` bit 1 | `0x01` | Volume + |
| `d[0]` bit 2 | `0x02` | Volume − |
| `d[0]` bit 3 | `0x08` | Next / seek up |
| `d[0]` bit 4 | `0x09` | Previous / seek down |
| `d[0]` bit 5 | `0x03` | Mute |
| `d[0]` bit 6 | `0x12` | **unmapped in the plugin** — an 18th code the Android side drops |
| `d[1]` bit 0 | `0x05` | Answer / phone |
| `d[1]` bit 1 | `0x07` | Voice assistant |
| `d[0] == 0 && d[1] == 0` | `0x00` | no key |

> Note what profile 0 **does not** have: **no RPM and no gear decoder.** Nothing in
> `can_dispatch_profile0_83k` ever writes `0x20001B67` (gear) or `0x20001B68/69` (RPM).
> On an 83.333 kbit/s car this box therefore emits `AA 55 0E 32 00×14 3F` **permanently**,
> engine running or not — which is exactly what the capture shows. **DECOMPILED
> (xref audit, `decomp/xref_audit.txt`) + CROSS-CHECKED.**

### 3.7 CAN RX dispatch — profile 1, 125 kbit/s (`can_dispatch_profile1_125k @ 0x08009680`)

All **DECOMPILED**.

| CAN ID | Bytes read | Decoded meaning | Lands in |
|---|---|---|---|
| **`0x1E1`** | `d[2] d[3]` | **Steering angle.** `raw = d[2]<<8 \| d[3]`; `angle = (0x1000 − raw)/2`, **clamped to ±539 (`0x21B`)**, negative encoded as `0x10001 − v`. Gated on `flags.bit2` (reverse) | UART **`0x72` payload `[4..5]`** |
| **`0x20B`** | `d[0]` | `flags.bit4 = d[0] & 1`; `flags.bit0 = ((d[0] & 0xE0) > 0x10)` | UART `0x72` payload `[0]` |
| **`0x20E`** | `d[2]` | **Gear.** `n = d[2] & 0x0F`: `0→1 (P)`, `2→2 (N)`, `4→4 (D)`, `9→unchanged`, else `→0 (invalid)`; then `if (d[2] & 0x08) { flags.bit2 = 1; gear = 3 (R); } else flags.bit2 = 0` | UART **`0x32` payload `[1]`** |
| **`0x211`** | `d[0] d[1]` | **Engine speed.** every 21st frame: `rpm = d[0]<<8 \| d[1]`; accepted only if `< 8000` | UART **`0x32` payload `[2..3]`**, big-endian, 1 rpm/LSB |
| **`0x244`** | `d[0]` | **Doors.** `bit0→LF, bit1→RF, bit3→LR, bit2→RR, bit4→trunk` | UART **`0x12` payload `[2]`** bits 7,6,5,4,3 |
| **`0x308`** | `d[1]` | `flags.bit1 = (d[1] != 0)`; `value = min(d[1] >> 1, 100)` | UART `0x72` payload `[0]` bit 1 and `[3]` |
| **`0x3A3`** | `d[0] d[1]` | **SWC buttons** — bit-for-bit the same map as `0x3A0` above | UART **`0x72` payload `[2]`** |
| **`0x513`** | `d[0..3]` | copied out and **retransmitted as CAN `0x516`** | — |
| `0x400…0x43F`, `0x7FC`, `0x7FF` | | identical vendor service protocol; `0x7FF` here also sets the gear from `d[4]` (`0→0, 1→2, 2→1, 3→9, 4→8, 5→11, 6→3`) and replies CAN `0x7FE` | — |

The **±540°** clamp in profile 1 is an exact, independent match for
`assets/car_trajectory_3009001.json`'s `"max": 540, "min": -540`. **CROSS-CHECKED.**

### 3.8 The master mapping table — CAN ID → signal → UART CMD

This is the deliverable. Everything in it is **DECOMPILED** unless marked.

| Signal | Profile 0 (83.333 kbit/s) | Profile 1 (125 kbit/s) | UART CMD | UART payload byte(s) | Encoding |
|---|---|---|---|---|---|
| **Doors** (LF/RF/LR/RR/trunk) | **`0x013` d[2]** | **`0x244` d[0]** | `0x12` | `[2]` bits 7/6/5/4/3 | `1` = open. Source bits: `b0→LF b1→RF b3→LR b2→RR b4→trunk` |
| **Steering angle** | **`0x015` d[4..5]** | **`0x1E1` d[2..3]** | `0x72` | `[4..5]` | BE signed 16, **`(0x1000 − raw)/2` degrees**, centre `raw=0x1000`, 0.5°/LSB, ±540 max; negative = left |
| **SWC buttons** | **`0x3A0` d[0..1]** | **`0x3A3` d[0..1]** | `0x72` | `[2]` | bit→code map in §3.6 |
| **Gear / PRND** | *(not decoded)* | **`0x20E` d[2] low nibble** | `0x32` | `[1]` | `1=P 2=N 3=R 4=D`, `0`=invalid |
| **Engine RPM** | *(not decoded)* | **`0x211` d[0..1]** | `0x32` | `[2..3]` | BE u16, 1 rpm/LSB, only if `<8000` |
| **Reverse-gear flag** | **`0x003` d[2] low nibble == 1 or 9** | **`0x20E` d[2] bit 3** | `0x72` | `[0]` bit 2 | also drives GPIOB6 and gates the steering-angle update |
| **Ignition / ACC** — INFERRED | **`0x000` d[0]&0xE0 > 0x10** | **`0x20B` d[0]&0xE0 > 0x10** | `0x72` | `[0]` bit 0 | gates the whole TX block + GPIOB5; high confidence |
| unnamed flag — INFERRED | **`0x000` d[0] bit 0** | **`0x20B` d[0] bit 0** | `0x72` | `[0]` bit 4 | set in the capture (`0x72[0] = 0x13`); not routed to GPIO |
| unnamed 0–100 % — INFERRED | **`0x210` d[1]** | **`0x308` d[1]** | `0x72` | `[0]` bit 1 and `[3]` | `[3] = min(d[1]/2, 100)`, `[0].bit1 = (d[1] != 0)`; bit 1 drives GPIOC15. **Best guess: illumination on + dimmer %** (a canbox almost always has an ILLUM output). Medium-low confidence; could equally be fuel level. **Test it on the car.** |

**Not decoded from CAN by any profile, at all:** vehicle speed, battery voltage, any
temperature, oil pressure, fuel, odometer, handbrake, seat belts, hood, door **locks**,
turn signals, parking radar (`0x41`), HVAC. The XREF-derived `0x32` fields in
`hiworld-jpf2-jeep.md` §3.2 (speed, volts, temps, oil pressure) borrowed from the JPP004
sibling **do not exist in this firmware** — `0x32` payload `[0]` and `[4..13]` are never
written and are always zero. `0x41` is not even in the TX table. **DECOMPILED
(xref audit) — this closes several "unverified" rows in the existing docs as
definitively empty.**

---

## 4. The UART side

### 4.1 The message table — flash `0x08009F2C` / `0x08009F74`

**DECOMPILED.** Two arrays of 12-byte records, `{ uint8 cmd; uint8 len; uint8 _0; uint8 _0;
void *buf; void *shadow; }`, referenced from `0x08008E30` (TX) and `0x08008E38` (RX).

**TX — box → head unit (6 entries, emitted round-robin in this order):**

| # | CMD | LEN | Source RAM | Content |
|---|---|---|---|---|
| 0 | `0x72` | 14 | `0x20001B49` | status flags, SWC key, steering angle |
| 1 | `0xA6` | 8 | `0x20001B75` | amplifier: VOL, BAL, FAD, BASS, MID, TRE, +2 |
| 2 | `0x73` | 8 | `0x20001B7D` | **never written — always zero** |
| 3 | `0xF0` | 17 | `0x20001B86` | `"H1N4JPF20A-220524"` |
| 4 | `0x12` | 10 | `0x20001B5C` | doors (+ aliased amp bytes, §4.4) |
| 5 | `0x32` | 14 | `0x20001B66` | gear, RPM |

**RX — head unit → box (2 table entries):**

| # | CMD | LEN | Dest RAM | Content |
|---|---|---|---|---|
| 0 | `0xD2` | 13 | `0x20001BE7` | cluster display text: `[sub, 12 × ASCII]` |
| 1 | `0xAD` | 2 | `0x20001BF5` | amplifier write: `[sel, val]` |

Plus two commands handled outside the table: `0xFC` (vendor handshake) and `0xE0`
(reset), §4.5.

**Differential confirmation:** the same 12-byte record shape appears in
`2883-H1N4MT060A-230712.app.bin` at `0x0800C6F8` (13 records: TX `0x11 0x12 0x31 0x32 0x34
0xF2 0xEF 0xF0 0xEB`, RX `0x3D 0x1B 0xEA 0xFD`) and, with a 16-byte stride, in
`2948-H1H2JP063A-231006.app.bin` at `0x0800EA6C` (39 records, the full Hiworld command
space: TX `0x11 0x12 0x13 0x14 0x15 0x21 0x22 0x31 0x41 0x43 0x45 0x62 0x64 0x60 0xAE 0xC2
0xC1 0xF0 0xA6 0x9C 0xA5 0x2E 0x32 0x1A`, RX `0x9B 0x0A 0x1B 0x3D 0x44 0x63 0x61 0xAF 0xCB
0x9A 0xCA 0xAD 0x24 0x95 0x9D`). The record layout is identical across three unrelated
firmwares. **DECOMPILED + CROSS-CHECKED.**

That sibling table is also why the JPF2 box does *not* answer `0x90`, `0x0A`, `0x9D`,
`0x24`, `0xCB` &c.: those exist in other Hiworld firmwares but **not in this one**.

### 4.2 Frame builder — `uart_build_frame_AA55 @ 0x08008D04`

**DECOMPILED. This confirms the checksum arithmetic exactly.**

```c
memcpy(&out[4], tbl[i].buf, tbl[i].len);
out[0] = 0xAA;
out[1] = 0x55;
out[2] = tbl[i].len;                 /* L, payload only          */
out[3] = tbl[i].cmd;                 /* CMD                      */
k = (uint8_t)(tbl[i].len + 4);       /* checksum index           */
out[k] = 0;
for (j = 2; j < k; j++) out[k] += out[j];   /* L + CMD + payload  */
out[k] -= 1;                                /* ... minus one      */
out_total_length = tbl[i].len + 5;
```

⇒ `CHK = (LEN + CMD + Σpayload − 1) & 0xFF`, total wire length `L + 5`.
**CROSS-CHECKED**, identical to `hiworld-protocol.md` §2.1 and to the Android
`ToolClass.sendDataToSerial`.

### 4.3 Frame parser — `uart_rx_parser_AA55 @ 0x08008BB2`

**DECOMPILED.** Byte-at-a-time state machine over a 200-byte receive ring:

```
state 0: wait for 0xAA
state 1: expect 0x55 (else back to 0)
state 2: LEN byte; rejected if LEN >= 0x38 (56); needs LEN+2 more bytes buffered
state 3: accumulate CMD + payload, running sum; final byte compared with (sum - 1) & 0xFF
         -> on match, uart_rx_dispatch()
```

* Maximum accepted payload = **55 bytes** (`LEN < 0x38`). The Android side's limit for the
  `AA 55` dialect is `0x32` = 50; the firmware is slightly more permissive. Minor
  discrepancy, noted, harmless.
* The preamble is hard-coded `AA 55`. `5A A5` is **not** accepted by this firmware —
  this finally settles `hiworld-protocol.md` open question 1 for this box: the `AA 55`
  dialect is real and is the only one here. **DECOMPILED.**

### 4.4 The `0x12` / `0xA6` payload overlap — a real firmware quirk, not a transcription error

`hiworld-protocol.md` §7.3 flagged that the captured `0x12` payload ends with
`10 0A 0A 0D 0A 0A`, the same tail as the `0xA6` amplifier frame, and suspected the sniffer
had smeared two frames. **It had not.** `config_load_defaults @ 0x08007B6C` writes through
a pointer based at `0x20001B60`:

```c
*(uint8_t*)0x20001B60 = config[0x10];                 /* = 0x12 payload[4] */
for (i = 1; i < 6; i++) *(uint8_t*)(0x20001B60 + i) = config[0x10 + i];  /* = 0x12 payload[5..9] */
*(uint8_t*)(0x20001B60 + 0x15) = (profile == 1) ? 30 : 20;               /* = 0xA6 payload[0] */
*(uint8_t*)(0x20001B60 + 0x16 + n) = amp[n];                             /* = 0xA6 payload[1..5] */
```

`0x20001B60` is **inside the `0x12` buffer** (`0x20001B5C` + 4). The stored amplifier
settings are therefore written into `0x12` payload bytes `[4..9]` as well as into the
`0xA6` buffer. The capture is faithful:

```
0x12 payload : 00 00 00 00 | 10 | 0A 0A 0D 0A 0A
                 └doors=0┘   ▲    └ BAL FAD BASS MID TRE ┘   (= 0xA6[1..5])
                             └ stored volume, config[0x10] = 16
0xA6 payload : 14 | 0A 0A 0D 0A 0A | 00
                ▲ profile-0 default volume 20
```

**DECOMPILED + CROSS-CHECKED.** `0x12` payload bytes `[0] [1] [3]` are never written and
are always zero; `[4..9]` are amplifier state leaking through, not door data. Only `[2]`
is meaningful.

### 4.5 UART RX dispatch — `uart_rx_dispatch @ 0x08008A96`

**DECOMPILED.** The complete set of commands this firmware reacts to:

| CMD | LEN | Action |
|---|---|---|
| `0xD2` | 13 | copy `[sub, 12 ASCII]` to `0x20001BE7`, raise "new data"; handler at `0x08007DA8` formats it into the cluster CAN frames (§5.2) |
| `0xAD` | 2 | copy `[sel, val]` to `0x20001BF5`, raise "new data"; handler clamps and pushes to CAN (§5.1) |
| `0xFC` | ≥9, payload must be `3F F3 F1 01 ? ? 3A A3 ?` | vendor handshake. Replies with the **same frame, CMD changed to `0xFD`, checksum + 1** (correct precisely because `CHK` is a plain sum). Total reply length 14 |
| `0xE0` | any | writes `0xBF` to `WWDG_CR` in an infinite loop → **immediate watchdog reset**. This is the "enter bootloader / firmware update" trigger |
| anything else | — | **silently ignored** |

There is **no `0xFF` handler, no `0xFE` handler, no `0x90`, no `0x0A`, no `0x81`, no
`0x9D`, no `0x24`, no `0xCB`.**

### 4.6 `0xFF`: poll or ACK? — **ANSWERED: it is an ACK.**

Three independent pieces of evidence, all pointing the same way:

1. The firmware has **no code path whatsoever** that inspects `0xFF`. A poll would have to
   be parsed; it is not. **DECOMPILED.**
2. The TX scheduler `uart_tx_sched @ 0x08008E62` free-runs. Every 6 ticks it either
   (a) sends the entry that `uart_tx_round_robin @ 0x08008D9C` found changed (a payload
   that differs from its shadow copy), or (b) if nothing changed, waits 15 ticks and sends
   the **next entry in table order anyway** — `idx = FUN_080085C4(idx, +1, max 5, min 0);
   uart_build_frame_AA55(idx);`. Nothing is ever requested. **DECOMPILED.**
3. The **order** in the capture matches the table order exactly. TX table order is
   `0:0x72, 1:0xA6, 2:0x73, 3:0xF0, 4:0x12, 5:0x32`. The captured box-to-head-unit sequence
   is `73 → F0 → 12 → 32 → 72 → A6` — i.e. indices `2 → 3 → 4 → 5 → 0 → 1`, a clean wrap of
   the table. Each `AA 55 01 FF <X>` sits *after* the frame `<X>`. **CROSS-CHECKED.**

⇒ `AA 55 01 FF <X>` is the **head unit acknowledging message type `<X>`**, exactly as the
Android `CanbusSerial.hiWorld_rx_uart_aa55` code says. Sending it to the box does nothing
at all — the box will not answer, because it does not parse it.

**A footnote that is interesting rather than load-bearing:** the same scheduler *contains*
an ACK **emitter** — `uart_send_2byte(0xFF, X)` drives a 10-entry queue at `0x20001B3D`,
explicitly skipping the values `0xFE` and `0xFF` — so the vendor's common code base does
ACK received frames with `0xFF` in the box→head-unit direction too. In *this* build the
queue's producer index (`0x20001B47`) is never advanced by anything, so the queue is
permanently empty and **the box never actually emits a `0xFF` frame**. Every `AA 55 01 FF
<X>` on the wire here is the head unit's. **DECOMPILED.**

**Operational consequence, and it is important:** you **cannot** impersonate the head unit
by polling. Just **listen** — the box transmits all six messages on its own as soon as it
sees CAN traffic. (`hiworld-protocol.md` §4's "practical consequence" needs correcting on
this point.)

### 4.7 `0x73`: what is it? — **ANSWERED: nothing.**

A full cross-reference audit of RAM `0x20001B40..0x20001BF8` (exported as
`decomp/xref_audit.txt`) shows **not a single read or write** anywhere in the 32 KB image
touching `0x20001B7D..0x20001B84`, the `0x73` payload buffer. The buffer is BSS, zeroed at
reset, and stays zero for ever.

⇒ `0x73` is a **reserved 8-byte message slot that this firmware always emits as all
zeros**. The capture (`AA 55 08 73 00×8 7A`) is not "engine off" — it is the permanent
value. There is nothing to discover on the car for `0x73`. **DECOMPILED +
CROSS-CHECKED.**

### 4.8 UART baud rate

**DECOMPILED.** `uart_init_38400_and_version @ 0x08008A54`:

```c
uart_reset_state();
usart2_init_wrapper(0x9600);          /* 0x9600 = 38400 */
memcpy((void*)0x20001B86, "H1N4JPF20A-220524", 0x11);   /* the 0xF0 payload */
```

`usart2_init @ 0x0800291E` builds a `USART_InitTypeDef` (default `0x2580` = 9600,
immediately overwritten with the argument), word length 8, stop 1, parity none, no flow
control, TX+RX enabled (`0x0C`), then `USART_Init(USART2, …)` and
`USART_ITConfig(USART2, USART_IT_RXNE, ENABLE)`.

* **38 400 8N1 confirmed.** **CROSS-CHECKED** with the Android `0x9603` token and with the
  existing spec.
* **No second baud rate exists in this image.** `115200` / `0x1C200` appears nowhere.
  Hiworld's IAP/update mode must live in the 8 KB bootloader at `0x08000000`, which is not
  in this file — consistent with `0xE0` being a "reset into the bootloader" command (§4.5).
* Two further UART init functions exist for USART1 (`0x40013800`) and USART3
  (`0x40004800`) but have **zero call sites** — dead code carried from the vendor's
  common tree.

---

## 5. What the box TRANSMITS onto the vehicle CAN bus

**DECOMPILED.** `can_tx_enqueue @ 0x080074C0 (id, ide, dlc, data*)` pushes into a 50-entry
TX ring at `0x200015A4`. All call sites enumerated:

### 5.1 Periodic frames (only while the ACC/ignition flag `0x72[0].bit0` is set)

A 150-tick counter fires a different frame on each odd tick:

| Tick | Profile 0 (83.333 kbit/s) | Profile 1 (125 kbit/s) | Source buffer | Meaning |
|---|---|---|---|---|
| 1 | **`0x3D0`** DLC 7 | **`0x3D9`** DLC 7 | `0x20001150` | **amplifier: VOL, BAL, FAD, BASS, MID, TRE, +1** |
| 3 | **`0x0F0`** DLC 4 | **`0x2E9`** DLC 4 | `0x20001157` | |
| 5 | **`0x326`** DLC 8 | **`0x290`** DLC 8 | `0x2000115C` | |
| 7 | **`0x190`** DLC 7 | **`0x291`** DLC 7 | `0x20001164` | cluster display text A |
| 9 | **`0x18C`** DLC 8 | **`0x293`** DLC 8 | `0x2000116B` | cluster display text B |
| 11 | **`0x09F`** DLC 8 | *(none)* | `0x200011A1` | |

On every UART `0xAD` amplifier write the box additionally sends `0x3D0` / `0x3D9`
immediately.

### 5.2 UART `0xD2` → cluster text

`uart_D2_cluster_text_to_can @ 0x08007DA8` (a 384-byte function dispatched through a
24-entry `TBB` jump table on the sub-command byte) converts the 12 ASCII characters into
packed BCD/bitfields in the `0x190`/`0x291` and `0x18C`/`0x293` buffers — e.g. sub-command
1/2/3 parses `payload[4..6] + payload[8]` as a decimal number (radio frequency), sub-command
4/5 uses `payload[4..7]` (track number). **DECOMPILED**, details beyond the sub-command
switch not fully reversed.

### 5.3 Vendor service / factory-test frames (both profiles)

| CAN ID | Direction | Notes |
|---|---|---|
| `0x400…0x43F` RX | in | node-address protocol; `id & 0x3F` is a node id. `d[0] == 0xFD` starts a sequence; replies `0x416` DLC 8. Looks like Hiworld's own CAN firmware-update / configuration channel |
| `0x416` TX | out | reply to the above |
| `0x7FC` RX / `0x7FD` TX | in/out | handshake, magic `3F F3 F2 01 · · 3A A3` — echoes the frame back on `0x7FD` |
| `0x7FF` RX / `0x7FE` TX | in/out | **status injection** with magic `7F F7 · · · · 3A A3`: writes the flags byte and (profile 1) the gear directly. This is a bench-test / simulator input, not a vehicle message |
| `0x500` / `0x513` RX → `0x516` TX | in/out | echoes 2 (profile 0) or 4 (profile 1) received bytes back on `0x516` |

These are **not vehicle signals** and should not be chased on a real bus.

---

## 6. Persistent configuration (flash parameter page `0x0801F800`)

**DECOMPILED**, `config_load_defaults @ 0x08007B6C`. RAM mirror at `0x200011D0`:

| Offset | Default (virgin) | Meaning |
|---|---|---|
| `[0]` | `0xAA` | validity magic |
| `[1]` | `1` | **CAN profile / bit rate: `1` = 125 kbit/s + profile 1, `0` = 83.333 kbit/s + profile 0**; clamped to ≤ 1 |
| `[0x10]` | `0` | stored volume |
| `[0x11..0x15]` | `10, 10, 10, 10, 10` | stored BAL, FAD, BASS, MID, TRE |
| `[0x16]` | `0` | |

The amplifier centre value **10** and the 1–19 range are hard-coded here, and the clamps in
the `0xAD` handler are `val ≤ 38` for `sel == 1` (volume) and `1 ≤ val ≤ 19` for
`sel == 2..6`. **CROSS-CHECKED** against `hiworld-jpf2-jeep.md` §3.6 ("Volume 0…38",
"raw 1…19, centre 10") — exact agreement.

---

## 7. Cross-checks against the existing documents

| Existing claim | Firmware says | Verdict |
|---|---|---|
| `CHK = (LEN + CMD + Σpayload − 1) & 0xFF` | identical code at `0x08008D04` | ✅ confirmed |
| total wire length `L + 5` | `out_len = len + 5` | ✅ confirmed |
| preamble `AA 55` | hard-coded, `5A A5` not accepted | ✅ confirmed |
| baud 38 400 8N1 | `usart2_init(0x9600)`, 8N1 | ✅ confirmed |
| `0x12` L=10, `0x32` L=14, `0x72` L=14, `0xA6` L=8, `0x73` L=8, `0xF0` L=17 | exactly these six, exactly these lengths | ✅ confirmed |
| `0x12` payload `[2]` bits 7/6/5/4/3 = LF/RF/LR/RR/trunk | exactly | ✅ confirmed |
| `0x32` payload `[1]` gear `1=P 2=N 3=R 4=D` | exactly (profile 1) | ✅ confirmed |
| `0x32` payload `[2..3]` RPM BE | exactly (profile 1) | ✅ confirmed |
| `0x72` payload `[2]` SWC key, codes 1,2,3,5,7,8,9,0x0B | exactly, **plus an extra code `0x12`** | ✅ + one addition |
| `0x72` payload `[4..5]` steering angle BE signed ±540 | exactly, `(0x1000 − raw)/2`, clamp ±539 | ✅ confirmed, and the raw CAN encoding recovered |
| `0xA6` volume 0…38, tone 1…19 centre 10 | exactly | ✅ confirmed |
| `0x12` hood = `[2]` bit 2 (XREF from a sibling plugin) | **not decoded** — nothing writes it | ❌ this box does not report hood |
| `0x32` speed / volts / temps / oil pressure (XREF from JPP004) | **not decoded** — those bytes are never written | ❌ always zero on this box |
| `0x41` radar | **not in the TX table at all** | ❌ this box never sends `0x41` |
| `0x45`, `0x7D`, `0x94` | not in the TX table | ❌ never sent |
| `0x9D`, `0x90`, `0x0A`, `0x81`, `0x24`, `0xCB` (head unit → box) | **no handler** | ❌ silently ignored |
| `0xFF` = poll | **ACK**; box free-runs | ✅ the Android reading was right |
| `0x73` = "unknown state page" | **never written, always zero** | ✅ resolved, empty |
| `0x12` tail `0A 0A 0D 0A 0A` = transcription smear | **real** — amplifier state aliased into `0x12[4..9]` | ✅ resolved, firmware quirk |
| "the box must be told which car it is (`0x24 ForwardCarType`)" | **false for this box** — it autobauds and has two hard-wired profiles | ❌ corrected |

---

## 8. Exported decompilation

`<android-radio>\hiworld\decomp\`

| File | Contents |
|---|---|
| `can_init.c` | `can_init` (bit timing), `can_init_normal`, `CAN_FilterInit`, `CAN_Init`, `can_apply_bitrate_A/B`, `config_load_defaults`, `main` (the autobaud loop), `usart2_init`, `uart_init_38400_and_version`, `USART_Init` |
| `can_rx_dispatch.c` | `can_rx0_irq_push_ring`, `CAN_Receive`, `can_poll_dispatch` (profile selector), **`can_dispatch_profile0_83k`**, **`can_dispatch_profile1_125k`** |
| `uart_frame.c` | **`uart_build_frame_AA55`**, **`uart_rx_parser_AA55`**, `uart_rx_dispatch`, `uart_tx_round_robin`, **`uart_tx_sched`** (the free-running scheduler), `uart_tick`, `uart_send_2byte`, memcmp/memcpy/wrap helpers |
| `can_tx.c` | `can_tx_enqueue`, `CAN_Transmit`, the periodic CAN-TX + amplifier function, `uart_D2_cluster_text_to_can` |
| `xref_audit.txt` | every reference to RAM `0x20001B40..0x20001BF8` — the evidence for "`0x73` is never written" and for which payload bytes are live |

Symbol key for the raw decompilation (Ghidra prints `PTR_DAT_<flash addr>` for a
literal-pool pointer; the value is the RAM address):

| Symbol / address | Meaning |
|---|---|
| `0x20001284` | CAN RX ring, 50 × 16 B. Entry: `[0..1]` ID hi, `[2..3]` ID lo, `[5]` DLC, `[6..13]` data |
| `0x20001118` / `0x2000111C` | ring producer / consumer index |
| `0x200015A4` | CAN TX ring, 50 × 16 B |
| `0x200019F8` | UART TX/RX working buffer; `[0x3C]` = length, `[0x3E]` = busy, `[0x3F…]` = 200-byte RX ring |
| `0x20001B01` | reassembled UART RX frame; `[2]` = LEN, `[3]` = CMD, `[4…]` = payload |
| `0x20001B49` | **`0x72` payload** (14 B) — the main state block |
| `0x20001B5C` | **`0x12` payload** (10 B) |
| `0x20001B66` | **`0x32` payload** (14 B) |
| `0x20001B75` | **`0xA6` payload** (8 B) |
| `0x20001B7D` | **`0x73` payload** (8 B) — never written |
| `0x20001B86` | **`0xF0` payload** (17 B) |
| `0x20001BE7` | `0xD2` RX buffer (13 B + flag) |
| `0x20001BF5` | `0xAD` RX buffer (2 B + flag) |
| `0x200011D0` | persistent config mirror |
| `0x2000118F` | active CAN profile / bit-rate flag |
| `0x08009F2C` | UART TX descriptor table (6 × 12 B) |
| `0x08009F74` | UART RX descriptor table (2 × 12 B) |
| `0x08008E18` | `"H1N4JPF20A-220524"` |

---

## 9. What remains unknown

1. **Which physical Chrysler bus each profile is for.** The bit rates and ID sets are
   fact; the mapping to "CAN-IHS on a WK" versus "CAN-IHS on a JK" is INFERRED from the
   box's printed vehicle list and platform history. Verify with a scope/analyser on the
   car before wiring anything.
2. **The identity of `0x72[0]` bits 1 and 4 and of `0x72[3]`** (the 0–100 value from CAN
   `0x210`/`0x308` `d[1]`). Illumination-on + dimmer is the leading hypothesis because
   bit 1 drives a GPIO output, but fuel level is not excluded. One stimulus test on the car
   (turn the headlights on, then vary the dash dimmer) settles it.
3. **The full semantics of CAN `0x0F0`/`0x2E9`, `0x326`/`0x290`, `0x09F`** that the box
   transmits. The buffers are filled by code not fully traced; the amplifier frame
   (`0x3D0`/`0x3D9`) and the two cluster-text frames are identified, these three are not.
4. **The `0x400…0x43F` vendor protocol.** Clearly a CAN-side configuration/update channel
   with a node-addressing scheme; not reversed in detail. It is the likely route for
   changing `config[1]` (the profile) without a UART.
5. **The bootloader** at `0x08000000` (8 KB) is not in any of these files. It holds the
   vector table, the IAP protocol, and whatever `0xE0` resets into.
6. **`0x72` payload `[1]`, `[6..13]`; `0x12` `[0] [1] [3]`; `0x32` `[0] [4..13]`;
   `0xA6` `[6] [7]`** — confirmed by the xref audit to be **never written**. These are not
   "unknown", they are **empty**. Anyone hunting for meaning in them is wasting their time.

---

## 10. Practical upshot for going straight to the vehicle bus

If the goal is to bypass the box:

* Tap the **interior** CAN bus, not CAN-C. Set your interface to **83.333 kbit/s** first
  (that is what this box settled on, per §3.5); if you see nothing, try **125 kbit/s**.
* Then log and decode:

| You want | Listen for (83.333 k) | Listen for (125 k) |
|---|---|---|
| doors | `0x013` byte 2 | `0x244` byte 0 |
| steering angle | `0x015` bytes 4–5, `(0x1000 − raw)/2` ° | `0x1E1` bytes 2–3, same formula |
| steering-wheel buttons | `0x3A0` bytes 0–1 | `0x3A3` bytes 0–1 |
| gear | `0x003` byte 2 low nibble (reverse only) | `0x20E` byte 2 low nibble |
| engine RPM | *not carried in this profile's decode* | `0x211` bytes 0–1, big-endian |
| ignition / ACC | `0x000` byte 0 | `0x20B` byte 0 |

* To **control the OEM amplifier** without the box, transmit `0x3D0` (83.333 k) or `0x3D9`
  (125 k), DLC 7, `[VOL(0..38), BAL, FAD, BASS, MID, TRE, x]` with the tone values 1..19.
* To put text in the instrument cluster, the frames are `0x190`+`0x18C` (83.333 k) or
  `0x291`+`0x293` (125 k) — but the encoding is a packed BCD/bitfield format that still
  needs work (§9.3).

Everything the box can tell you is in the six UART messages, and now you know that four of
them (`0x72`, `0x12`, `0x32`, `0xA6`) carry real data, one (`0xF0`) is a constant string,
and one (`0x73`) is permanently zero.
