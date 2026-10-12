# JPF2 canbox — ALL car profiles (full decoder map, both models)

Companion to `hiworld-jpf2-83k-io.md` (which fully documents profile 0). This doc adds
the **profile-selection mechanism** and the **second profile (model 1, 125 kbit/s)**,
and confirms there are **exactly two** decoder profiles in this firmware — not the dozens
implied by a naïve grep of CAN-ID-looking literals.

All facts tagged **[D]** = DECOMPILED (read from named code + resolved RAM/flash address)
or **[I]** = INFERRED. Source image: `2429-H1N4JPF20A-220524.app.bin`, base 0x08002000.
CAN data bytes numbered **D0 = first data byte** (RX ring entry byte +6). UART frame =
`AA 55 | LEN | CMD | payload | CHK`.

---

## 0. TL;DR

- **Two** profiles, hard-selected by one **model byte**: `config[1]` in the parameter page
  (0x0801F800), shadowed to RAM at **0x200011D1**, clamped to `{0,1}`.
- The model byte drives BOTH the **CAN bit-timing** (0 → 83.333 kbit/s, 1 → 125 kbit/s)
  and the **decoder dispatch function** (0 → `FUN_08009010`, 1 → `FUN_08009680`).
- Selection is **auto-detected at boot** by cycling the two bauds until CAN frames appear,
  then the winning model is **persisted to flash**. Default factory value is **1** (125 k).
- **Profile 0** = the user's **2005–2010 Jeep Grand Cherokee WK** (83.333 k body bus).
- **Profile 1** = the **125 kbit/s FCA variant** — and it **DOES decode engine RPM
  (CAN 0x211) and gear/PRNDL (CAN 0x20E)**, which profile 0 does not. Neither profile
  decodes vehicle **speed**.
- Every ID in the 0x400–0x43F / 0x500 / 0x513 / 0x7FC / 0x7FF range on **both** profiles is
  the **vendor service/diagnostic handshake**, not a dashboard signal.
- Mapping is **hardcoded if/else** in both profiles (confirmed) — no data-driven table.

---

## 1. Profile-selection mechanism

### 1.1 The model byte and the two dispatchers  **[D]**

`FUN_08007d44` @0x08007d44 (called every CAN tick by `can_autobaud_state_machine`) is the
single CAN-RX dispatch gate:

```c
if (*profile_sel == 0x01)  FUN_08009680();   // profile 1  (125 kbit/s FCA)
else                       FUN_08009010();   // profile 0  (83.333 kbit/s Jeep)
```

- `profile_sel` = **0x2000118F** (RAM), resolved from literal @0x08007f2c.  **[D]**
- `FUN_08009010` @0x08009010 = **profile 0** dispatcher (the 83.333 k Jeep map; = the
  `can_dispatch_profile0_83k` of the prior doc).  **[D]**
- `FUN_08009680` @0x08009680 = **profile 1** dispatcher (125 k FCA map, §3 below).  **[D]**

These are the **only two** functions in the image that compare received CAN IDs. A grep
for ID-range literals also hits `FUN_08003d74` (a FLASH/USB register-mask decoder — its
`0x200`/`0x20000`/`0x4000001` constants are **not** CAN IDs) and hits many bit-value and
loop-bound immediates; none of those are decoders. **Confirmed: two profiles.**  **[D]**

### 1.2 Where `profile_sel` comes from  **[D]**

`FUN_08007b6c` @0x08007b6c (run once in `main_loop` init):

```c
config = 0x200011D0;                 // literal @0x08007f28 -> 0x200011D0
if (config[0] != 0xAA) {             // first-boot magic; init defaults
    config[0]=0xAA; config[1]=1; ... // DEFAULT MODEL = 1 (125 kbit/s)
}
if (config[1] > 1) config[1] = 1;    // clamp model to {0,1}
*profile_sel = config[1];            // 0x2000118F = model byte
```

`config` (base **0x200011D0**) is the **parameter-page shadow**: the same pointer is used
by `param_page_load_0801F800` @0x08002dbc and `param_page_save_0801F800` @0x08002d46
(literal @0x08002e58 → 0x200011D0). So the model byte is **persisted in flash at
0x0801F800+1** and reloaded every boot.  **[D]**

### 1.3 The model byte also sets the CAN baud  **[D]**

`FUN_08007b2c` @0x08007b2c and `FUN_08007b4c` @0x08007b4c:

```c
if (config[1] == 1) FUN_08007482(0x7D);   // 0x7D = 125  -> 125 kbit/s
else              { config[1]=0; FUN_08007482(0x53); } // 0x53 = 83 -> 83.333 kbit/s
```

`FUN_08007482`/`FUN_0800744e` pass that number straight into the bit-timing setup
(`FUN_08002b76`). So **model 0 ⇔ 83.333 k, model 1 ⇔ 125 k** — the two "profiles" ARE the
two body-bus bit rates of the FCA family. **[D]**

### 1.4 Auto-detection at boot  **[D/I]**

`main_loop` @0x080088d8 startup loop:

```c
while (wwdg_kick(), *can_rx_wr_idx < 3) {     // 0x20001118 = CAN RX ring write index
    if (*tick_flag == 1) {                    // 0x20001115, set every 2048 ticks
        if (*try_cnt < 8) {                   // 0x20001190, max 8 attempts
            config[1] = (config[1] != 1);     // TOGGLE model 0<->1
            param_page_save_0801F800();       // persist
            FUN_08007b2c();                   // re-apply baud for the new model
            (*try_cnt)++; *tick_flag = 0;
        } else FUN_080032f6();                // give up
    }
}
*profile_sel = config[1];                     // lock the detected model
```

- `tick_flag` (0x20001115) is set by `FUN_08007a30` @0x08007a30 when a free-running counter
  hits `(n & 0x7FF)==0x7FF`, i.e. a slow periodic pulse (~every 2048 ticks).  **[D]**
- The loop exits once **≥3 CAN frames** have been received on the currently-selected baud.
  So the box **tries a baud, and if no traffic arrives it toggles to the other baud/model**,
  up to 8 times, then persists whichever produced traffic.  **[D for the code; I for
  "this is autobaud/automodel detect".]**

**Consequence:** on the user's WK the box saw traffic at 83.333 k, locked `config[1]=0`,
and it now boots straight into profile 0. There is **no evidence of a UART "set car model"
command** — the head unit does not choose the profile; the vehicle bus does. (Consistent
with the prior finding that almost all inbound UART commands are ignored.)  **[D/I]**

### 1.5 Shared runtime layout  **[D]**

Both dispatchers read the same **50-entry CAN-RX ring** at **0x20001284** (16-byte stride;
ID = 4 bytes @+0, DLC/flags @+4/5, data D0..D7 @+6..+0x0D), indexed by read-idx 0x2000111C
/ write-idx 0x20001118, and write the same **UART payload region based at 0x20001B49**:

| offset from 0x20001B49 | UART CMD | buffer addr | len |
|---|---|---|---|
| 0x00 | `0x72` | 0x20001B49 | 14 |
| 0x13 | `0x12` | 0x20001B5C | 10 |
| 0x1D | `0x32` | 0x20001B66 | 14 |
| 0x2C | `0xA6` | 0x20001B75 | 8 |
| 0x34 | `0x73` | 0x20001B7D | 8 |
| 0x3D | `0xF0` | 0x20001B86 | 17 |

(So e.g. profile-1 writes to `buf[0x15]` = 0x12 payload[2] = doors; `buf[0x1E]` = 0x32[1] =
gear; `buf[0x1F:0x20]` = 0x32[2:3] = RPM.)  **[D]**

---

## 2. Profile 0 — Jeep, 83.333 kbit/s  (THIS USER'S CAR)

Fully documented in **`hiworld-jpf2-83k-io.md`**; not repeated here. Summary of decoded IDs
(`FUN_08009010`): **0x000** ACC/ign→0x72[0], **0x003** reverse→0x72[0] bit2, **0x013**
doors→0x12[2], **0x015** steering→0x72[4:5], **0x210** fuel/level→0x72[0]bit1 & 0x72[3],
**0x3A0** SWC→0x72[2]; service **0x400–0x43F/0x500/0x7FC/0x7FF**. **No RPM, gear, or speed.**

---

## 3. Profile 1 — FCA, 125 kbit/s  (the "other" profile)

Decompiled from `FUN_08009680` @0x08009680. Data-byte convention D0 = ring entry +6.
Output offsets resolved via the table in §1.5.

### 3.1 CAN → UART (car → head unit), 125 k  **[D]**

| CAN ID | CAN byte·bit | → UART | meaning |
|---|---|---|---|
| **0x20B** | D0 bits5–7 (`(D0&0xE0)>0x10`) | `0x72`[0] **bit0** | ACC / ignition on |
| **0x20B** | D0 bit0 | `0x72`[0] **bit4** | secondary ign/run flag |
| **0x308** | D1 != 0 | `0x72`[0] **bit1** | level-present flag |
| **0x308** | D1 | `0x72`[3] = `D1>>1`, cap 100 | **0–100 % level (fuel)** |
| **0x20E** | D2 bit3 set | `0x72`[0] **bit2** (+ `0x32`[1]=3) | **reverse / R-gear** |
| **0x20E** | D2 low nibble | `0x32`[1] | **gear code**: 0→1, 2→2, 4→4, (9→hold), else→0 |
| **0x1E1** | D2:D3 (BE), gated on `0x72[0] bit2` | `0x72`[4:5] | **steering angle**, signed |
| **0x211** | D0:D1 (BE), if `<8000`, 1-in-21 throttle | `0x32`[2:3] | **engine RPM (raw)** |
| **0x244** | D0 bits | `0x12`[2] | **doors** (see below) |
| **0x3A3** | D0/D1 bit combos | `0x72`[2] | **steering-wheel button** code |

**Steering angle** (0x1E1, raw = `D2<<8 | D3`), only while `0x72[0] bit2` (running):
- `raw < 0x1000` → `min((0x1000−raw)/2, 0x21B)`  (one direction)
- `raw == 0x1000` → `0`
- `raw > 0x1000` → `0x10001 − min((raw−0x1000)/2, 0x21C)`  (other direction, wrapped)
→ written big-endian to `0x72[4:5]`. (Same idea as profile 0's 0x015 but with ±0x21B caps.)

**RPM** (0x211): a 21-tick decimator (`counter 0x200011CE`); when it fires and
`D0*256+D1 < 8000`, copies `0x32[2]=D0`, `0x32[3]=D1`. Raw big-endian RPM, capped <8000.

**Doors** (0x244 D0 → `0x12`[2]): D0.0→bit7 (LF), D0.1→bit6 (RF), D0.3→bit5 (LR),
D0.2→bit4 (RR), D0.4→bit3 (trunk/tailgate). **Identical bit layout to profile 0's 0x013.**

**SWC key codes** (0x3A3 → `0x72`[2]): D0.0→0x0B · D0[1:2]=01→1,=10→2 · D0[3:4]=01→8,=10→9 ·
D0[5:6]=01→3,=10→0x12 · D1.0→5 · D1.1→7 · all-zero→0 (released). **Identical table to
profile 0's 0x3A0** → same Android key map.

### 3.2 Service / diagnostic IDs on profile 1  **[D]**

Same vendor handshake as profile 0, with two ID substitutions:

| CAN ID | role |
|---|---|
| **0x400–0x43F** (`id & 0xFC0 == 0x400`) | service exchange; box replies **0x416** (magic-byte challenge/response, `FUN_08007CD2` subfn) |
| **0x513** | replaces profile 0's **0x500**; box echoes D0..D3 → reply **0x516** |
| **0x7FC** | magic `3F F3 F1 01 … 3A A3` → reply **0x7FD** |
| **0x7FF** | magic → sets status bits in `0x72[0]`, decodes a PRNDL byte (D4 switch → `0x72[2]`), reflects display mode (reads RX-`0xD2` buffer @0x20001BE7) and replies **0x7FE** |

These are **not dashboard signals** — they are the module-keep-alive / pairing handshake,
CAN-side only, never exposed to UART.

### 3.3 UART → CAN and autonomous TX on profile 1  **[D]**

The head-unit→CAN commands are the same two as profile 0 (`0xAD`→amplifier,
`0xD2`→cluster text), but the **transmitted CAN IDs differ by profile** — chosen in the
periodic scheduler `FUN_08008110` @0x08008110 on `profile_sel` (0x2000118F):

| function | profile 0 (83.3 k) TX ID | profile 1 (125 k) TX ID |
|---|---|---|
| amplifier control (from UART `0xAD`/`0xA6`) | **0x3D0** | **0x3D9** |
| status / periodic | **0xF0** | **0x2E9** |
| cluster/radio text block | **0x326**, **0x18C**, **0x9F** | **0x290**, **0x293** |
| periodic status | **0x190** (400), (0x18C) | **0x291** |

(Text formatter `func_0x08007da8` = `uart_D2_cluster_text_to_can` fills the buffers; the
scheduler emits them round-robin on cases 1/3/5/7/9/0xB gated on ACC.)

So profile 1 drives the amp on **0x3D9** and cluster text on **0x290/0x293** — the 125 k
FCA equivalents of profile 0's 0x3D0 / 0x326-0x18C-0x9F.

---

## 4. Profile → likely car model

| profile | model byte | baud | decoded IDs (RX) | likely vehicles |
|---|---|---|---|---|
| **0** | 0 | 83.333 k | 0x000,0x003,0x013,0x015,0x210,0x3A0 | **2005–2010 Jeep Grand Cherokee WK / Commander XK** and FCA siblings on the 83.333 k body bus — **the user's car** |
| **1** | 1 (default) | 125 k | 0x1E1,0x20B,0x20E,0x211,0x244,0x308,0x3A3 | **125 kbit/s FCA body-CAN variant** — later/other Chrysler-Jeep-Dodge on the 125 k interior bus |

Both are the **same FCA family** (part H1N4JPF2* = "Jeep Profile 2", protocol_id 3009001),
just the two body-bus bit rates. The profile-1 IDs are textbook FCA CAN-IHS:
- **0x1E1** = steering-wheel angle (classic Chrysler CAN-IHS steering ID)  **[I, strong]**
- **0x211** = engine data / RPM  **[I, strong]**
- **0x244** = door/latch status  **[I, strong]**
- **0x20E** = PRNDL / gear selector  **[I, strong]**
- **0x308** = fuel / analog level  **[I]**
- **0x3A3** = steering-wheel control switches  **[I]**
- **0x20B** = ignition/run status  **[I]**

These are all FCA-consistent; nothing here looks like a different brand. **Not overclaimed:**
the exact model-year split between the 83.333 k and 125 k FCA buses isn't encoded in the
firmware — the box just picks whichever baud has traffic.

---

## 5. Answers to the specific questions

- **How many profiles?** Exactly **two** (`FUN_08009010`, `FUN_08009680`).
- **How selected?** Persistent **model byte** `config[1]` (param page 0x0801F800 → RAM
  0x200011D1 → `profile_sel` 0x2000118F), clamped `{0,1}`, **auto-detected at boot** by
  baud-cycling until CAN frames arrive, then saved. Model 0 = 83.333 k, model 1 = 125 k.
  The head unit does **not** set it over UART (no such command found).
- **Does any profile decode RPM / speed / gear?** **Profile 1 decodes RPM (0x211 → 0x32[2:3])
  and gear/PRNDL (0x20E → 0x32[1], reverse → 0x72[0] bit2; also 0x7FF PRNDL → 0x72[2]).**
  Profile 0 (the user's Jeep) decodes **neither**. **Neither profile decodes vehicle speed.**
- **Is the mapping a table?** **No — hardcoded if/else** with immediate ID compares, byte
  indices, masks, shifts and caps baked into the Thumb code, in **both** profiles. The only
  real tables are the UART message table @0x08009F2C and the RX/TX rings. **Confirms** the
  prior conclusion for the second profile too.
- **The "60+ CAN IDs" from the grep** are not 60 profiles: they are the two profiles' RX
  sets **plus** the profile-split **TX** IDs (0x3D0/0x3D9, 0x326/0x290, 0x18C/0x293,
  0x9F, 0x190/0x291, 0xF0/0x2E9), **plus** service IDs (0x416/0x516/0x7FD/0x7FE), **plus**
  false positives (bit values, loop bounds, angle caps 0x21B/0x21C, register masks).

---

## 6. Address index (new anchors from this pass)

| addr | what |
|---|---|
| 0x08009680 | `can_dispatch_profile1_125k` (FUN_08009680) |
| 0x08007d44 | dispatch gate: `if *0x2000118F==1 → profile1 else profile0` |
| 0x08007b6c | init model byte / `profile_sel = config[1]`, default 1, clamp {0,1} |
| 0x08007b2c / 0x08007b4c | model→baud: `FUN_08007482/44e(0x7D=125 | 0x53=83)` |
| 0x08007482 → 0x08002b76 | CAN bit-timing setup from baud number |
| 0x080088d8 | `main_loop` boot auto-detect toggle loop |
| 0x08007a30 | free-running tick → periodic flags (sets toggle flag @0x20001115) |
| 0x08008110 | periodic CAN-TX scheduler, profile-split TX IDs |
| 0x08002dbc / 0x08002d46 | param-page load/save (config @0x0801F800 ↔ 0x200011D0) |
| RAM 0x200011D0 | config/param shadow; [0]=0xAA magic, **[1]=model byte** |
| RAM 0x2000118F | `profile_sel` (active model) |
| RAM 0x20001284 | 50-entry CAN-RX ring (16-B stride, data @+6) |
| RAM 0x20001B49 | base of UART payload region (see §1.5) |
