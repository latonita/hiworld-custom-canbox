# CAN bus architecture — GOJOHO / XYAudio B9320B + Hiworld JPF2.10

Status as of 2026-09-29. Companion documents:
- `hiworld-protocol.md` — generic Hiworld UART framing (verified)
- `hiworld-jpf2-jeep.md` — JPF2 payload/signal map (complete, 931 lines)
- `my-radio.md` — raw device/unit identifiers

Every claim below is tagged **VERIFIED** (computed or read from a binary in this
workspace), **SOURCED** (external, URL given), or **INFERRED**.

---

## 1. The stack, end to end

```
┌──────────────────────────────────────────────────────────────────────┐
│ VEHICLE — 2005–2010 Jeep Grand Cherokee (WK)                         │
│   CAN-C     500 kbit/s   powertrain      OBD-II pins 6 / 14          │
│   CAN-IHS   125 kbit/s   body / comfort  doors, locks, HVAC, lights  │
└───────────────┬──────────────────────────────────────────────────────┘
                │  raw CAN frames
┌───────────────▼──────────────────────────────────────────────────────┐
│ HIWORLD CANBOX  (LHL) JPF2.10 / H1N4JPF20A / date code 220524        │
│   TRANSLATING GATEWAY — not a transparent bridge                     │
│   Decodes vehicle CAN into a fixed, model-specific semantic message  │
│   set. Must be told which car it is in before it decodes anything.   │
└───────────────┬──────────────────────────────────────────────────────┘
                │  UART, TTL  •  AA 55 framing  •  38400 8N1
┌───────────────▼──────────────────────────────────────────────────────┐
│ HEAD-UNIT MCU — firmware 5.9                                         │
│   Polls the canbox, forwards decoded state to Android                │
└───────────────┬──────────────────────────────────────────────────────┘
                │  internal serial / AIDL
┌───────────────▼──────────────────────────────────────────────────────┐
│ ANDROID  (AC8227L, Android 8.1 base labelled "15.0")                 │
│   CAN host app  +  per-vehicle plugin APK  +  protocol database      │
└──────────────────────────────────────────────────────────────────────┘
```

### The decisive architectural fact

**The canbox is a translating gateway. You cannot reach raw CAN through it.** VERIFIED
structurally:

- frames carry a *semantic* command ID, with no field capable of holding an 11- or
  29-bit CAN identifier
- payload lengths observed are 8 / 10 / 14 / 17 bytes — not a 0–8 CAN DLC
- the same CMD byte has a **different byte layout per vehicle family** (`0x12` is
  7 bytes on VW-MQB, 10 on PSA, 10 here with a different meaning)
- the box must be configured with a vehicle profile before it emits anything useful

Consequences:

| Want | Possible? | How |
|---|---|---|
| Read decoded vehicle data | **YES** | TTL UART + common ground; listen passively or poll |
| Send/receive arbitrary raw CAN | **NO** | Nothing in the framing can express a CAN ID |
| Command the car | **Partial** | Fixed menu of curated actions the box injects, not CAN writes you control |

For genuine raw CAN access, bypass the box entirely: OBD-II pins 6/14 for CAN-C
(500 k); a body-side tap for CAN-IHS (125 k). Any MCP2515/TJA1051 or SocketCAN
adapter gives full read/write.

---

## 2. Wire protocol (summary — full spec in `hiworld-protocol.md`)

```
AA 55 │ L │ CMD │ payload[L] │ CKS          total wire length = L + 5
```

- **`L`** counts payload bytes only — excludes CMD, preamble and checksum. VERIFIED
- **`CMD`** at offset 3 — *after* the length byte. This is the discriminator versus
  the sibling **Raise** protocol, which is `2E CMD LEN …`. VERIFIED
- **`CKS`** = sum of **all** bytes including the `AA 55` preamble, mod 256.
  Equivalently `(L + CMD + Σpayload − 1) & 0xFF`, since `0xAA + 0x55 = 0xFF ≡ −1`.
  VERIFIED — 13/13 captured frames; 13 candidate algorithms tested, all others
  scored 0 or 1. Independently corroborated by `smartgauges/canbox`'s
  `canbox_hiworld_checksum()` = `sum − 1`. SOURCED

### Polling handshake — no authentication

`AA 55 01 FF <X> <cks>` means **"send me message type X"**; the box replies with a
frame whose CMD equals X. VERIFIED across six poll/response pairs. The head unit
cycles `A6 → 73 → F0 → 12 → 32 → 72 →` and wraps.

**There is no secret handshake.** A TTL UART can impersonate the head unit, poll the
box, and sweep the CMD space to enumerate what the JPF2 profile answers.

### Preamble discrepancy — **RESOLVED** (decompiled)

Public sources document Hiworld as `5A A5`; this box uses `AA 55`. **Both are real and
both are implemented as separate code paths.** From `CanbusSerial.RxData`'s dispatch
switch in `plugin/3/9/P2_101.apk`:

| selector | parser | framing |
|---:|---|---|
| 38401 | `hiWorld_rx_uart` | `5A A5 …` ← the classic dialect all public docs describe |
| **38403** | **`hiWorld_rx_uart_aa55`** | **`AA 55 …`** ← **this unit** |
| 19205 | `hiWorld_rx_uart_aa55` | `AA 55 …` |
| 38407 | `hiWorld_rx_uart_key` | `00 00 …` |
| 38406 | `xp_rx_uart_aa55` | `55 AA …` (XinPu — byte order reversed) |
| default | `canbox_rx_uart` | `2E …` |

`protocol_id 3009001` maps to selector **38403**, hence the AA55 dialect. Every *other*
Hiworld vehicle falls through to 38401 and `5A A5` — which is why the public
documentation says `5A A5` and this box does not.

### The selector mechanism

`CanbusStatus.getBaudRate()` returns a **pseudo-baud protocol selector**, not a baud
rate: `<baud> + <variant>`. For `3009001` it returns `0x9603` = **38403** → 38400 baud,
AA55 variant. DECOMPILED from `CanbusConfig_P.smali`.

The selected vehicle is read from **`Settings.System` int key `Canset_User`** —
on this unit, `3009001`.

### Wiring — status

| Item | Status |
|---|---|
| 38400 8N1 | **STRONG INFERENCE** — selector 38403 decomposes as baud 38400 + variant 3, corroborated by the secondary port's literal `0x9600` = 38400. Not a literal for the primary port (that lives in the framework `canbus_service`) |
| 3.3 V TTL | **INFERRED** |
| Connector pinout | **UNKNOWN** — Hiworld pinouts are not standardised, they vary per model even within the brand; none published for H1N4JPF20A / JPF2.10 |
| Max frame size | **DECOMPILED** — parser guard aborts at `rx_pos > 50`, so LEN ≤ 45 |
| Resync | **DECOMPILED** — 500 ms watchdog resets the parser state machine |

Empirical identification procedure: scope the connector at boot. The pin emitting
`AA 55` bursts is box-TX; the pin driven with `AA 55 01 FF …` is box-RX. Confirm baud
from bit width — 26.0 µs = 38400, 8.68 µs = 115200.

---

## 3. Vehicle profile selection

### Protocol database

`system/media/CanBus/plugin/bsj_car_lang.db` — SQLite 3, **7261 rows** in `lang_en`
(parallel `lang_zhcn` / `lang_zhtw`). VERIFIED

```sql
CREATE TABLE lang_en (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  factory_name TEXT,   -- Hiworld, Raise, Simple Soft, LuZheng, BINARY, DaoJun, OD, XinBas, BSJ, CYT, CLZ
  brand_name   TEXT,   -- Jeep, Chrysler, Dodge, Volkswagen, Toyota, ...
  year_range   TEXT,
  model_base   TEXT,
  sort_weight  TEXT,
  protocol_attr TEXT,  -- Low / Mid / High / screen orientation / amplifier notes
  protocol_id  TEXT,   -- the 7-digit selector
  plugin       TEXT,   -- P2_101.apk
  version      TEXT,
  protocol_tag TEXT,   -- JPF1 / JPF2 / JPF3 / JP06 / JP07 / FT09 / G-RZ-* ...
  remark       TEXT
);
```

Sibling files: `can1_025.json` (same tree as nested JSON, trilingual names),
`config.ini` (column order + display priority), `dbversion.json`.

`dbversion.json` points at the vendor's live update server:
```
http://canbus.xygala.com/manage/data/Group/public/home/CanBusTest/
  plugindefault = P2_101.apk     plugindebug = P2_000.apk
```
`P2_000.apk` is the "debug" plugin slot. **Retrieved and checked: it is byte-identical
to the release `P2_101.apk`** (md5 `be3f97d5…`). `DownloadService.getPluginApk()` merely
computes the name `P<major>_000.apk` for the debug slot, which the vendor populated
with a copy of the release binary. **There is no richer debug build** — dead end.

### `protocol_id` decomposition — VERIFIED

```
3 009 001
│  │   └── model / variant
│  └────── brand    (009 = Jeep, 046 = Dodge, 064 = Chrysler)
└───────── factory  (1 = Raise, 2 = Simple Soft, 3 = Hiworld, 4 = LuZheng,
                     5 = BINARY, 6 = DaoJun, 7 = OD, 8 = XinBas, 9 = BSJ,
                     13 = CYT, 14 = CLZ, 0 = No protocol)
```
Confirmed by cross-checking `3046xxx` = Hiworld/Dodge and `1009xxx` = Raise/Jeep.

### Plugin layout

`system/media/CanBus/plugin/<factory_id>/<brand_id>/P2_101.apk` — 174 brand
directories, one decoder APK each.

**This unit's decoder: `plugin/3/9/P2_101.apk`** (Hiworld / Jeep, 514,605 bytes).

---

## 4. This vehicle's profile — `3009001`, tag **JPF2**

On-screen confirmation: *Network CAN Set V1.301 (ID: 3009001)*, path
`Hiworld / Jeep / [JPP001] 2007~2016 Compass / 2005~2010 Grand-Cherokee / …`,
status **"Protocol is connected"**. Box label reads **JPF2.10**, matching tag JPF2.

**JPF2 is one shared decoder across the whole DaimlerChrysler-era family** — all of
these select `3009001`:

| Make | Models |
|---|---|
| Jeep | **2005–2010 Grand Cherokee (WK)** ← this car, 2007–2016 Compass, 2011–2015 Patriot, 2007–2018 Wrangler JK, 2008–2012 Liberty KK, 2005–2010 Commander |
| Chrysler | 2005–2010 300C, 2007–2010 Sebring |
| Dodge | 2006–2012 RAM Pickup, 2005–2014 Challenger, 2006–2010 Charger LX, 2004–2011 Durango HB, 2012–2014 Durango WD |

**Practical consequence:** any JPF2 capture or documentation from a 300C, Wrangler JK
or Charger LX applies directly to the WK — they share the CAN-IHS architecture and
the identical decoder. This widens the search space considerably.

Distinct, **not** interchangeable: `3009002`–`3009025` use tags JPF1 / JPF3 / JP06 /
JP07 / FT09 — different protocols despite the adjacent IDs.

---

## 5. Observed traffic

Transcribed from the unit's built-in serial sniffer. All 13 frames pass the verified
checksum. Tx = head unit → box, Rx = box → head unit.

| Dir | Frame | Meaning |
|---|---|---|
| Rx | `AA 55 11 F0 48 31 4E 34 4A 50 46 32 30 41 2D 32 32 30 35 32 34 DA` | **VERIFIED by content** — payload is ASCII `H1N4JPF20A-220524`, the box's own ID |
| Tx | `AA 55 01 FF A6 A5` / `73 72` / `F0 EF` / `12 11` / `32 31` / `72 71` | polls for CMD A6 / 73 / F0 / 12 / 32 / 72 |
| Tx | `AA 55 01 9D 04 A1` | downlink with a real payload — **proves the Tx direction carries commands, not just polls** |
| Rx | `AA 55 08 73 00 00 00 00 00 00 00 7A` | CMD 0x73 — unknown |
| Rx | `AA 55 08 A6 14 0A 0A 0D 0A 0A 00 F6` | CMD 0xA6 — DSP / EQ (SOURCED, not verified) |
| Rx | `AA 55 0A 12 00 00 00 00 10 0A 0A 0D 0A 0A 60` | CMD 0x12 — doors (SOURCED) |
| Rx | `AA 55 0E 32 00 00 00 00 00 00 00 00 00 00 00 00 00 3F` | CMD 0x32 — body state (SOURCED) |
| Rx | `AA 55 0E 72 13 00 00 64 00 00 00 00 00 00 00 00 00 F6` | CMD 0x72 — feature mask (SOURCED) |

Four Rx frames transcribe one byte short of `L+5`; in each case the missing byte sits
inside a run of `00`s, which a summing checksum cannot detect. **Transcription loss
from the photographs, not variable framing.** A live capture closes this.

The four "SOURCED" meanings come from other vehicle families — their payload
*lengths* match the sourced specs, but **the per-byte layouts do not transfer between
families.** Treat the meanings as hypotheses until confirmed against `plugin/3/9`.

---

## 5·CAN. Vehicle-CAN side — DECOMPILED FROM BOX FIRMWARE

Recovered by disassembling the box's own MCU firmware (`hiworld-firmware-analysis.md`,
Ghidra). This is the half invisible from the Android plugin: what the box reads off the
car and how it maps to the UART. Confirmed against the live Ghidra session's UART table.

**The box only touches the low-speed body bus.** One bxCAN controller, one bus,
**auto-detected at boot as 83.333 kbit/s OR 125 kbit/s** (it alternates until 3 frames
arrive). **There is no 500 kbit/s path — it never reads CAN-C powertrain.** RPM/gear on
the 125 k profile therefore come off the IHS/body bus, not CAN-C.

**No hardware acceptance filter** — it is set to ID 0 / mask 0 (accept everything) and
filtered in software, so the real CAN-ID list is the literal constants in the dispatch:

| Signal | 83.333 k CAN ID | 125 k CAN ID | → UART |
|---|---|---|---|
| Doors | `0x013` d2 | `0x244` d0 | `0x12` [2] bits 7/6/5/4/3 |
| Steering angle | `0x015` d4–5 | `0x1E1` d2–3 | `0x72` [4..5] = `(0x1000−raw)/2`°, ±540 |
| SWC buttons | `0x3A0` d0–1 | `0x3A3` d0–1 | `0x72` [2] |
| Gear | — | `0x20E` d2 nibble | `0x32` [1]: 1=P 2=N 3=R 4=D |
| RPM | — | `0x211` d0–1 BE | `0x32` [2..3] |
| Reverse | `0x003` | `0x20E` d2 bit3 | `0x72` [0] bit2 |
| ACC | `0x000` d0 | `0x20B` d0 | `0x72` [0] bit0 |

Box also **transmits** on CAN: amplifier `0x3D0`/`0x3D9` DLC7, cluster-text frames, and
a vendor service protocol on `0x400–0x43F`/`0x7FC`/`0x7FF`.

**This box is running the 83.333 k profile** (the `0xA6`[0]=0x14 default is
profile-specific). That is why the captured `0x32` frame is permanently zero — **the
83.333 k profile has no RPM/gear decoder at all.** To get RPM you would need the box on
the 125 k profile, or read `0x211` off the bus yourself.

**These CAN IDs are the bypass route.** Reading `0x244`/`0x1E1`/`0x211` etc. directly off
the IHS bus with your own adapter gives the same data without the box.

---

## 5a. Live sniffing on the unit — DECOMPILED

The on-screen hex sniffer is driven by ordinary Android broadcasts. With ADB (or any
app holding the permission) these can be triggered directly:

| Purpose | Broadcast / key |
|---|---|
| **Enable full tracing** | `com.xygala.canbus.printdata`, boolean extra `isStart` |
| TX frames | `xy.auto.canbus.txpduses`, extra `txpduses` (byte[]) |
| RX frames | `xy.auto.canbus.rxpduses`, extra `rxpduses` (byte[]) |
| Link alive | `xy.auto.canbus.connectionsta`, boolean extra `connectionsta` |
| Secondary box RX | `canbus.intent.action.msg.tx2.rx2`, extra `canbus_data2` |

**Important:** `rxpduses` is emitted **only for frames that FAIL the checksum**, unless
tracing is enabled via `printdata`. So a quiet sniffer does not mean a quiet bus.

Transport: the primary canbox UART is **not** opened by the app. It is owned by the
framework service `ServiceManager.getService("canbus_service")` →
`android.os.ICanbusService`; TX is `setValue(byte[] frame)`, RX arrives as `byte[]`.
The app frames and validates; the service only moves bytes.

The *secondary* (auxiliary) UART is opened directly by the app and its path **is** a
literal: `ro.board.platform == "ac8227l"` → machineType 2 → **`/dev/ttyMT1`**.

---

## 5b. JPP001 signal map — DECOMPILED + CAPTURE-CONFIRMED

> Full map with bit tables and provenance per entry: **`hiworld-jpf2-jeep.md`**.
> Summary below. `getState(d,i)` = `payload[i]` (since `dataIndexOffset = 4`).

**Generation note:** the device runs `P1_501.apk`, not the OTA's `P2_101.apk`. All
four JPF2 JSON assets are **byte-identical** across P1_301 / P1_501 / P2_101, and every
JPF2 decoder class is semantically identical (several byte-identical). **No offset,
mask or scale differs between generations** — the map below applies to all three.

| RX CMD | L | Signals |
|---|---|---|
| `0x12` | 10 | `[2]` b7/b6/b5/b4 = LF/RF/LR/RR door, b3 = trunk, b2 = hood (XREF only) |
| `0x32` | 14 | `[1]` gear: 0=inv 1=P 2=N 3=R 4=D 5=S · `[2..3]` **RPM** big-endian, `0xFFFF`=invalid |
| `0x41` | 16 | `[0..3]` front park sensors, `[4..7]` rear; 0=none, `0xFF`=invalid |
| `0x72` | 14 | `[2]` **SWC key**: 1=VOL+ 2=VOL− 3=MUTE 5=PHONE 6=HANGUP 7=VOICE 8=NEXT 9=PREV B=MODE · `[4..5]` **steering angle**, s16 big-endian, ±540°, negative = left |
| `0xA6` | 7 | `[0]` VOL 0–38 · `[1..5]` BAL/FAD/BASS/MID/TRE, 1–19 with **10 = centre** |
| `0xF0` | var | ASCII version string |

**XREF from JPP004** (same CMD + length, *not* JPF2-proven): `0x32` `[4..5]` km/h,
`[6]`×0.1 V, `[8]`/`[9]` −40 °C oil/coolant, `[10..11]`×0.1 Bar oil pressure,
`[12]`/`[13]` −40 °C trans/intake.

**Capture-confirmed** against the photographed frames: `0xA6` decodes to
VOL 20 / BAL 10 / FAD 10 / BASS 13 / MID 10 / TRE 10 — balance and fader exactly at the
spec's centre value. `0x12` all-zero and `0x72` key 0 / angle 0 are consistent with a
parked, closed car.

### TX commands

| CMD | Payload | Meaning |
|---|---|---|
| `0x81` | `[01]` | hello |
| `0x90` | `[type]` | **request** message `type` |
| `0x0A` | `[01, type]` | alternate request form |
| `0x9D` | `[dir]` | GPS heading push (`CanbusGps_P`) |
| `0xAD` | `[sel, val]` | amplifier write |
| `0xD2` | `[sub, 12×ASCII]` | **cluster text** — explicitly the `case 3009001/3009002` branch |
| `0xFF` / `0xFE` | `[type]` | ACK / NACK |

**Our captured `AA 55 01 9D 04 A1` is identified**: `CanbusGps_P` pushing GPS heading.
The plugin has a real bug here — the live quantiser is 16-sector but `dirArray` expects
8-point, guarded by `if (v <= 8)`, so bearings above ~203° transmit nothing and the
rest are misread by up to 90°.

**Conflict left open, deliberately:** the code emits `0xFF [type]` after every
successfully received frame and `0xFE` on checksum failure — i.e. an **ACK**, not a
poll. The plugin's real request opcodes are `0x90` and `0x0A`. Both readings fit the
captured traffic; a live capture settles it.

### Not present in any generation — do not go looking

ACC/ignition, reverse, handbrake, illumination, **fuel, odometer, ambient temp, door
locks, seat belts**. ACC and reverse arrive as Android broadcasts from the host MCU
instead. **HVAC is absent by construction** — there is no `car_aircon_3009001.json`, no
generic fallback, and `CanbusVoiceControl_P`'s ~120 aircon methods have empty bodies.
`0x73` (captured, L=8) **appears nowhere in any of the three APKs**.

---

## 6. Android-side CAN software

Two generations coexist, and **it is not yet established which this unit runs**:

| Component | Package | Version | Source |
|---|---|---|---|
| CAN host | `com.kyhero.car.myhost` / `myhost2` | 2.011 | OTA `customer_ota` |
| Legacy CAN app | `com.xygala.canbus` | 2.30.0 (42 MB) | OTA, kept as fallback |
| Per-vehicle plugin | `com.kyhero.car.myplugin` | 1.1 | 176 plugins in OTA |
| Installer utility | `com.example.tmedia` (CopyCanbus) | 1.6, `sharedUserId=android.uid.system` | OTA |

**The unit reports CAN Pro 1.023 and CAN Set app V1.301** — which map cleanly onto
*neither* 2.011 nor 2.30.0. So the installed generation differs from the OTA's, and
may well be newer. **Unresolved until the on-device package versions are read.**

---

## 6a. Vendor update server — `canbus.xygala.com`

Live. `47.111.27.85`, nginx, **plain HTTP only**, directory listing disabled (403).

**No query parameters.** The host app fetches `baseURL + relativePath` via an OkHttp
**ranged GET** (`OkHttpDownUtil.postRenewalDownRequest`, `header("RANGE","bytes=N-")`).
The only POSTs are telemetry to `/service/Canbus/{addLog,feedBack,downPluginLog}`.
Path scheme from `PluginUtil`:

```
host/host.json                              host/<source>/CanBus.apk , CanBus2.apk
plugin/can<ver>.json                        plugin/<canId>/<brandId>/<plugin>.apk
assist/assist.json , question.json
```

**Two parallel channels/generations:**

| | Production `/CanBus/` (app's hardcoded base) | Test `/CanBusTest/` (device's `dbversion.json`) |
|---|---|---|
| host.json | source `1_025`, **1.025** ← matches this device | source `2_01`, **2.01** |
| dbversion | configVersion 1.0, dbVersion **1** | configVersion 13.0, dbVersion **13** |
| `bsj_car_lang.db` | 1,433,600 B, **5,393** rows/lang | 2,506,752 B, **7,264** rows/lang |
| plugin 3/9 | P2_101 = P2_000 = 514,481 B | P2_101 = P2_000 = 514,605 B |

The device's local DB (dbVersion 10, 7,261 rows) sits **between** the two server copies —
closest to Test, which has since advanced to 13.

### Where the real signal maps live

Inside each plugin APK's `assets/`, as a **declarative schema** parsed by
`com.canbus.json.rx.RxData`:

```
rxDataType   rxLength   rxDataIndex (byte)   rxStartBit / rxEndBit
txDataType   txCmd      txParam
```
Files: `car_set.json` (85 KB settings tree), `car_aircon_<protocolId>.json`,
`car_key_<id>.json`, `car_trajectory*.json`.

### Security observation

The head unit downloads and **silently installs executable APKs over unauthenticated
HTTP** — confirmed in code: `xy.android.silent.install` with `force_apk_state=1`, and
the update flow shells out `chmod 777` plus package-install intents. That is a real
MITM / supply-chain exposure on any untrusted network. Recorded as an observation
only; not acted on.

Also present in all apps: a second host
`http://v.xygala.com/?app=canbus&token=496d1db4266c9ef9762e036cae802a3b` — a web/promo
view, and the only place a token or query string appears anywhere.

---

## 7. Open questions

**Resolved** since first draft: the `AA 55` vs `5A A5` preamble (§2 — separate code
paths, selector-chosen); the checksum (confirmed from decompiled source, matching the
capture-derived result exactly); the baud rate (strong inference from selector 38403).

Also resolved: the JPF2 signal map (§5b, `hiworld-jpf2-jeep.md`); the device's
installed generation (host 1.025 / `P1_501.apk`, vs the OTA's 2.011 / `P2_101.apk`);
the `3009002`-vs-`3009001` log oddity (`CanbusJson_P.indexOf()` matches by substring,
runtime `Canset_User` really is 3009002 — harmless, since the two JSONs are
byte-identical and every id occurrence is a pair of adjacent `case` labels into one
branch); the `P2_000.apk` debug plugin (identical to release — dead end); and the
update URL's parameter shape (there are none — plain ranged GETs).

**Answered by the firmware disassembly** (`hiworld-firmware-analysis.md`):

- **`0xFF` is an ACK, not a poll.** There is *no* `0xFF` handler anywhere in the box
  firmware. The TX scheduler free-runs the message table; the captured order
  `73→F0→12→32→72→A6` is simply table indices 2→3→4→5→0→1. **You cannot poll this box —
  just listen.**
- **`0x73` is empty by design.** Its payload buffer (`0x20001B7D..84`) has zero
  references in the entire image — permanently all-zero. Nothing on the car maps to it.
- **The `0A 0A 0D 0A 0A` tail shared by `0x12` and `0xA6` was NOT a transcription
  smear.** A config-init routine writes the amplifier settings into `0x12` payload
  `[4..9]`. Genuine.
- **Commands the box never sends** (so the plugin's handlers for them never fire on this
  unit): `0x41` radar, `0x45`, `0x7D` lights/speed, hood, and the temp/voltage fields.
  The Android plugin is a **superset** written for many Hiworld boxes; this firmware
  implements only `72 A6 73 F0 12 32` (TX) + `D2 AD` (RX). Downlinks `0x9D`/`0x90`/
  `0x0A`/`0x81` are ignored by the box.

Still open:

1. **Canbox connector pinout** — unknown; must be determined empirically.
2. **Real eMMC / DRAM size** — see `rom-analysis.md`; blocks any flashing decision.
6. **`updatecfg.zip`** — ZipCrypto-encrypted, holds `updatesplzx.sh` (3010 bytes).
   `customer.cfg` (`smZY6D2710Iz3L`) tested as the password **and rejected**, as were
   eight variants. Attackable by known-plaintext (`#!/system/bin/sh`) with `bkcrack`,
   not installed here. Recorded: CRC `b6bcc592`, plaintext 3010 bytes, ciphertext at
   offset `0x2e`.
7. **`libSerial_key.so`** — not in this OTA (lives in the system image). Its Java
   declarations suggest it is only an open/baud/close `tcsetattr` wrapper with no
   protocol logic, but that is an **inference, not verified**.

---

## 8. Sources

| URL | Value |
|---|---|
| `github.com/smartgauges/canbox` | Working C firmware; `canbox_hiworld_checksum()` independently confirms the checksum. Also documents the Raise contrast (`2E`, `sum XOR 0xFF`, total `L+4`). |
| `github.com/fazerxlo/canbox-core` | Parser plus **real captured dumps** that validate with our algorithm; Toyota/PSA command tables decompiled from `QF_Canbus.apk`. **Caveat:** parts of its Markdown appear machine-generated and at least one worked example is arithmetically wrong. Its *code and dumps* are sound; treat its prose with suspicion. |

Russian-language forums (4pda, drive2) were searched and yielded nothing on framing.
