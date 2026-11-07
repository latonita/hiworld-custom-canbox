# Custom telemetry block `0xB0`–`0xBF` — new layout

> **Status: BUILT 2026-11-07** on branch `feature/telemetry-v2` (firmware `H1N4JPF20L-071126`,
> host-tested, not yet flashed). Written 2026-11-05 on `spec/telemetry-v2`. Corrections made while
> building are listed in §15; **§14 is the exact wire summary** a decoder is written from.
>
> **This layout REPLACES the current custom block (`docs/CUSTOM-TX-SPEC.md`) entirely.** There is
> one firmware protocol and one Dashwheel decoder: no revision switch, no dual support, no compatibility
> layer, no selection by a revision byte. The `0xB0`–`0xBF` range is reused with new meanings. When this
> is built, mark `CUSTOM-TX-SPEC.md` "superseded by TELEMETRY-V2-SPEC.md" and keep it only as history.
> `0xB7` TYRE keeps its current (feature/tpms-temp) layout and its own spec (`docs/TPMS-POLL-SPEC.md`).
>
> Target: `app/`, the Hiworld/Jeep build only (`--protocol jeep`). The Benz build is untouched.
>
> **Source of truth for every CAN signal is `docs/JEEP-WK-CAN-ID-byte-map.md`**
> (including its "Derived values" section). Where it disagrees with
> `docs/CUSTOM-TX-SPEC.md`, the byte map wins. Problems this fixes are listed in
> `../canbox-vs-dashwheel-data.md` (outside the repo).

---

## 0. Why, in five lines

1. The current block mixes fast and slow signals in a change-driven round-robin at 48 ms per frame; speed and rpm
   change on every pass and starve the rest, so a quiet frame is resent only every ~2.3 s.
2. Its validity is one bit per frame: coolant alone gated four fields and made the Fuel tile flicker.
3. It carries fields with no CAN source (intake, range, steering) and one fake one (load = 0).
4. It has no lights, indicators, belt, telltales, cruise, A/C, 4WD, oil pressure or boost, all of
   which the byte map now confirms.
5. It has no firmware-side staleness: a dead CAN source leaves a frozen value marked valid.

The new layout sorts the data into three classes with three cadences, uses sentinels per field, ages every CAN
source in the firmware, and adds a priority lane in the TX scheduler.

---

## 1. Frame format (unchanged)

```
AA 55 | L | CMD | payload[L] | CKS            wire length = L + 5

L   = PAYLOAD LENGTH ONLY. Not CMD, not the preamble, not the checksum.
CKS = (0xAA + 0x55 + L + CMD + Sum(payload)) & 0xFF
    = (L + CMD + Sum(payload) - 1) & 0xFF
```

`svc::hu::emitFrame()` already does this; no framing code changes. Regression vector:
`AA 55 02 E0 00 00 E1` (the head unit's real upgrade-trigger frame; the checksum rule above gives `E1`).

Multi-byte values are **big-endian**. Never emit `0xE0`, `0xFE`, `0xFF`, `0xD2` from this block.
Bit 0 is the least significant bit.

---

## 2. The three classes, and the frame table

| cmd | name | class | payload | wire | cadence | contents |
|---|---|---|---|---|---|---|
| `0xB0` | HEARTBEAT | request | 4 | 9 | 1 Hz | version, capability bitmap, rolling counter |
| `0xB2` | STATE | priority | 7 | 12 | on change (next 100 ms tick) + every 1 s | bitfields and small enums |
| `0xB3` | FAST | priority | 11 | 16 | every 100 ms, always | speed, rpm, load, fuel flow, oil pressure, boost, accelerator pedal |
| `0xB4` | ENGINE | slow | 5 | 10 | round-robin ≤ 1.5 s + on change | coolant, battery volts, fuel %, fuel litres |
| `0xB5` | AMBIENT | slow | 3 | 8 | round-robin + on change | outside temperature, minutes since RUN |
| `0xB7` | TYRE | slow | 5 | 10 | round-robin + on change | one corner per frame (current layout, unchanged) |
| `0xB8` | TRIP | slow | 6 | 11 | round-robin + on change | odometer, compass |

Not used (reserved, never emit): `0xB1`, `0xB6`, `0xB9`, `0xBA`, `0xBB`, `0xBC`, `0xBD`, `0xBE`, `0xBF`.
`0xBB` ODOMETER and `0xBD` COMPASS fold into `0xB8`; `0xBC` FLOW folds into `0xB3`.

The **version** string is not a new frame: stock `0xF0` (17 bytes ASCII, `ram::g_versionString`) already
carries it and stays in the slow round-robin (§7). `0xB1` stays unused.

### Ground rules

- **Sentinels, per field, no group mask.** Every field has its own "unavailable" code (§8). No frame is
  ever suppressed to hide a bad field. Heartbeat withdraws nothing.
- **Fast frame is liveness.** `0xB3` is sent every 100 ms even when every field is a sentinel. A
  consumer that has not seen `0xB3` for 0.5 s treats the box as gone.
- **No ACC gate on the UART** (corrected while building: the stock TX slots are not gated on
  `kFlagAccOn` either; only the CAN-side periodic TX and the TPMS poll are). The block runs whenever the box
  is awake, i.e. while CAN-B is awake; the box sleeps on bus silence. So `NoKey`/`KeyOff` are seen during RAP
  and while the bus is awake after key-off; the head unit itself is powered by ACC/RAP.
- **One CAN source, one age** (§6). A field whose source is older than 3x its nominal period goes to its
  sentinel (bitfields: see the SRC byte in STATE).

---

## 3. Payloads

Offsets are into the payload (after CMD). "Source" is CAN id and byte at 83.333 kbit/s.
Confidence is the byte map's label. Only Confirmed/Probable signals are used.

### 3.1 `0xB0` HEARTBEAT — 4 bytes, 1 Hz

| off | field | value |
|---|---|---|
| 0 | layout version (diagnostics only; no consumer branches on it) | `0x02` |
| 1:2 | capability bitmap, u16 | bit *n* set = this build sends command `0xB0 + n`. Full build: `0x01BD` (`B0 B2 B3 B4 B5 B7 B8`) |
| 3 | rolling counter, u8 | +1 per heartbeat, wraps `FF`→`00` |

The heartbeat withdraws nothing and carries no validity. The capability bitmap says which frames this
firmware can ever send (a build without TPMS clears bit 7), so a consumer does not wait for a frame that
will never come. The counter makes every heartbeat differ, so it also goes out through the normal change
path, but it is still marked `kSlotRequest` from the 1 s task as today (FR-112: `pendingRequest()`).
A consumer detects "box rebooted" as a counter that does not continue by +1.

### 3.2 `0xB2` STATE — 7 bytes

All bits are 1 = true. A bit whose CAN source is dead is written **0** and its source bit in `S6` is 0.
**Consumer rule: a state bit is valid only if its source-alive bit in `S6` is 1; otherwise withdraw it.**

**S0 POWER**

| bit | field | source | rule | confidence |
|---|---|---|---|---|
| 0–2 | ignition enum | `000` b0 | `00`→0 NoKey, `01`→1 KeyOff, `63`→2 ACC, `85`→3 ON, `A9`→4 CRANK; any other value, or `000` dead → 7 | Confirmed |
| 3 | engine running | `002` b0:b1, `000` b0 | the current state machine (§5): ≥500 rpm for 3 frames and not cranking; clear at once below 300 rpm, on `FFFF`, or cranking | Confirmed/derived |
| 4 | RAP (accessory power) | `006` b1 bit 1 | verbatim | Confirmed |
| 5 | key in | `012` b2 bit 1 (valid SKIM key) | verbatim | Confirmed |
| 6 | reverse | `003` b4 | set exactly when the S5 gear byte is 1 (R): `b4 == 0x52` and `b2 != C0` | Confirmed |
| 7 | parking brake | `0D0` b0 bit 7 | mask bit 7 only; `04` is the released baseline, not the brake | Confirmed |

**S1 BODY**

| bit | field | source | confidence |
|---|---|---|---|
| 0 | brake pedal switch | `003` b3 bit 0 (also `2C0` b7 bits 2/3) | Confirmed (17/17 vs PCM) |
| 1 | driver belt **unfastened** | `013` b0 bit 3 | Confirmed |
| 2 | door FL open | `013` b2 bit 0 | Confirmed |
| 3 | door FR open | `013` b2 bit 1 | Confirmed |
| 4 | door RR open | `013` b2 bit 2 | Confirmed |
| 5 | door RL open | `013` b2 bit 3 | Confirmed |
| 6 | hatch open | `013` b2 bit 4 | Confirmed |
| 7 | reserved, 0 | — | — |

Bits follow the CAN order (FL, FR, RR, RL); the current permutation in `decodeDoors()` is not needed. There
is no bonnet signal on CAN-B; the consumer must treat bonnet as unknown, not closed.

**S2 LIGHTS** (source `006` b0, 100 ms; cross-check `1C8` b0, 500 ms)

| bit | field | source bit | confidence |
|---|---|---|---|
| 0 | left indicator flashing | `006` b0 bit 0, **debounced** (§3.2.1) | Confirmed |
| 1 | right indicator flashing | `006` b0 bit 1, debounced | Confirmed |
| 2 | hazard | `006` b0 bits 0 and 1 together in one frame, debounced; while set, bits 0 and 1 are also set | Confirmed |
| 3 | position lamps | `006` b0 bit 3 | Confirmed |
| 4 | low beam | `006` b0 bit 4 | Confirmed |
| 5 | high beam | `006` b0 bit 5 | Confirmed |
| 6 | front fog | `006` b0 bit 6 | Confirmed |
| 7 | rear fog | `006` b0 bit 7 | Confirmed |

**S3 ASSIST / LAMPS A** (source `003`, ~450 ms; its bytes equal CCN KWP `213F`)

| bit | field | source | confidence |
|---|---|---|---|
| 0 | cruise on (armed) | `003` b1 bit 7 | Confirmed |
| 1 | cruise engaged (speed set) | `003` b1 bit 6 | Confirmed |
| 2 | A/C compressor running | `003` b1 bit 5 (same signal as `0EC` b0) | Confirmed |
| 3 | ESP off (lamp steady) | `003` b0 bit 2 | Confirmed |
| 4 | MIL (check engine) | `003` b1 bit 0 | Confirmed |
| 5 | oil-pressure lamp | `003` b1 bit 2 | Confirmed |
| 6 | charging-system lamp | `003` b0 bit 7 | Probable |
| 7 | ABS lamp | `003` b0 bit 0 | Confirmed |

**S4 LAMPS B / 4WD**

| bit | field | source | confidence |
|---|---|---|---|
| 0–2 | 4WD range enum | `1B2` b1: `02`→1 (4H), `01`→2 (4L), `00`→3 (shifting), `07` or dead→0 (unavailable); cross-check `003` b6 bits 2/3 | Confirmed |
| 3 | ESP lamp | `003` b0 bit 1 | Confirmed |
| 4 | wait-to-start (glow) lamp | `003` b1 bit 3 | Probable |
| 5 | water-in-fuel lamp | `003` b1 bit 4 | Probable |
| 6–7 | reserved, 0 | — | — |

**S5 GEAR** (source `003`, a full byte so it has its own sentinel)

`003` b4 ASCII: `50`→0 P, `52`→1 R, `4E`→2 N, `44`→3 D, `31`/`32`/`33`/`34`→4/5/6/7 (AutoStick 1/2/3/4).
`003` b2 == `C0` (selector unavailable), an unknown ASCII, or `003` dead → `0xFF`. Confirmed (byte map 003 b4).
Stock `0x32` gear collapses AutoStick 1–4 into "S"; this byte keeps the range.

**S6 SRC** — source-alive bitmap (1 = a frame from that CAN id arrived within 3x its nominal period)

| bit | CAN id | nominal period | feeds |
|---|---|---|---|
| 0 | `000` | 100 ms | S0 ignition enum |
| 1 | `002` | 50 ms | S0 engine running |
| 2 | `003` | ~450 ms | S0 reverse; S1 brake; S3; S4 bits 3–5 (and the gear sentinel) |
| 3 | `006` | 100 ms | S0 RAP; S2 |
| 4 | `013` | 500 ms | S1 belt, doors, hatch |
| 5 | `0D0` | ~1.15 s | S0 parking brake |
| 6 | `1B2` | ~1.45 s | S4 4WD range |
| 7 | `012` | ~1 s | S0 key in |

This byte is the one place the block carries a per-source flag. It is not a group-withdraw mask: it lives in
the same frame as the bits it qualifies, the heartbeat never carries it, and it exists because a single bit
cannot hold a sentinel. See open question 1.

#### 3.2.1 Indicator debounce (firmware)

`006` b0 bits 0/1 follow the flasher, so they toggle at the blink rate. Pass them to the head unit raw and
the consumer sees an on/off strobe. The firmware turns them into a steady "flashing" state:

- on each `006` frame with bit 0 (or 1) set, load a 700 ms hold timer for that side; the state bit is set
  while the timer runs and clears when it expires at a 100 ms tick (so release shows within ~700-800 ms of
  the last blink);
- hazard = bits 0 and 1 both set in one `006` frame (byte map: `1B`), with its own 700 ms timer;
  while the hazard timer runs, bits 0 and 1 are also set;
- **hold time 700 ms, because** a WK-class flasher runs 60-120 flashes/min (half period 250-500 ms), the
  `006` sample interval is 100 ms, and the hold must be longer than the longest off phase plus one sample
  plus margin (500 + 100 + 100) yet short enough that the arrow does not visibly outlast the stalk.
  This is an assumption until the blink timing is read from a real capture; see open question 2 and the
  bench plan (§12). Cross-check source if needed: `11D` b0 bits 3/4 (stalk), held while the stalk is out.
- the state frame changes only on the on/off edges, not per blink, so it adds no traffic.
- **pulse or hold, both work** (owner open item: whether `006` b0 bits 0/1 pulse with the flasher or stay set
  while the stalk is out is unmeasured). Every frame with the bit set reloads the timer, so a held bit keeps the
  state set and it clears 700-800 ms after the last set frame either way. Host-tested for both.
- **lock-confirm flash suppression** (folded in from the withdrawn `0xBA LIGHTING` draft): locking the car flashes
  `006` b0 = `03` for ~0.2 s, which the hold would turn into a 0.7 s hazard. While `000` is alive and reads `00`
  (no key) or `01` (key in at OFF), indicator bits in `006` are ignored. Accepted cost: hazards switched on with
  the key out are not shown.
- `006` dead (no frame for 300 ms): the whole S2 byte is 0 and S6 bit 3 is 0; the timers just expire.

### 3.3 `0xB3` FAST — 11 bytes, every 100 ms

| off | field | unit | sentinel | source | arithmetic |
|---|---|---|---|---|---|
| 0:1 | speed, u16 | 0.1 km/h | `FFFF` | `002` b2:b3 | `round(raw * 10 / 128)` = `(5*raw + 32) / 64`; **`002` b2:b3 == `FFFF`** → `FFFF` (its own field; corrected from "b0:b1 == FFFF"); `002` stale → `FFFF`. Largest reading `FFFE` → 5120 |
| 2:3 | rpm, u16 | 1 rpm | `FFFF` | `002` b0:b1 | verbatim two-byte copy; `002` stale → `FFFF` |
| 4 | engine load, u8 | 1 % (0..100) | `FF` | `1A7` Qavg, `002` rpm | `load = round(15625 * Qavg / (324 * rpm))`, clamp 0..100 (exact integer form of `100 * (43.4*Q/rpm) / 90`); `FF` if rpm is `FFFF` or < 300, `002` stale, or Qavg is `FFFF`. `0 %` is a real reading (overrun) |
| 5:6 | fuel flow, u16 | 0.01 L/h | `FFFF` | `1A7` b0:b1 | `round(Qavg * 100 / 128)`. `Qavg` = time-weighted mean of Q over the window since the previous tick, **sample-and-hold**: each `1A7` Q holds until the next frame, for at most 1.2 s after its frame; a `FFFF` frame holds nothing. No held time in the window → `FFFF`. `Q == 0` is a real `0` (fuel cut) |
| 7 | oil pressure, u8 | ≈4 kPa/count gauge | `FF` | `02C` b0 | verbatim byte. `00` = engine off (real). Scale probable (PCM `21AA`: `39.9 * b0 + 1031` hPa abs) |
| 8:9 | boost / MAP, u16 | 1 hPa absolute | `FFFF` | `2C0` b4 | `raw * 8`; raw `FF` → `FFFF`; raw `FE` (saturated, ≥ ~2030 hPa) comes out as `07F0` by the same rule |
| 10 | accelerator pedal, u8 | 1 % (0..100) | `FF` | `02C` b2 | `round(b2 * 0.4)` = `(2*b2 + 2) / 5`, clamp 100; `b2 == FF` or `02C` stale → `FF`. **Added 2026-11-07**: the v1 firmware sent it (`0xB3`[5]) and dropping it was a regression for the owner's gauges |

Evidence: speed/rpm/oil/boost per byte map `002`, `02C`, `2C0` (r ≥ 0.996 against PCM). Load and flow per
byte map "Derived values" (divisor **128**, not 160; injection slope 1.00, r 0.99). Pedal per byte map `02C` b2
(r = 0.9995 vs TCM). Flow is **not** a 1 s average any more: it is the integral over the last ~100 ms, so a
consumer integrates litres as `Σ(flow × 0.1 s)` with no bias. Load at idle (Q ≈ 128, 580 rpm) = 11 %, matching
the byte map's "idle ≈ 10 %". `02C` runs at ~500 ms, so oil and pedal repeat over about five FAST frames.

Not carried, by decision: the wheel distance counters (`002` b4:b7).

### 3.4 `0xB4` ENGINE (slow) — 5 bytes

| off | field | unit | sentinel | source | arithmetic |
|---|---|---|---|---|---|
| 0 | coolant, u8 | °C + 40 | `FF` | `02C` b1 | verbatim (the car sends °C+40 and `FF` for n/a). Confirmed |
| 1 | battery volts, u8 | 0.1 V | `FF` | `015` b1 | verbatim (0.1 V/count; max real 25.4 V). Confirmed |
| 2 | fuel level, u8 | % | `FF` | `159` b5 | `round(b5 * 0.6443)`, clamp 0..100 (0.5 L/count over the 77.6 L tank) |
| 3:4 | fuel, u16 | 0.1 L | `FFFF` | `159` b5 | `b5 * 5` |

Fuel % = `round(b5 * 500 / 776)` (= `b5 × 0.5 L / 77.6 L`), clamp 100. Fuel gate, **hold-on-gate exactly as
implemented before**: `159` b4 == `01` means `b5` is not a reading; keep the previous fuel % and litres (`b5 == FF`
with an open gate is treated the same). They are `FF`/`FFFF` only until the first valid reading since boot,
or when `159` itself is stale (§6); a `159` that comes back shows the held reading again until a new one. No intake field: the byte map finds no intake temperature on CAN-B
(`015` b2 is an unknown temperature), so it is gone, not sentinelled forever.

### 3.5 `0xB5` AMBIENT (slow) — 3 bytes

| off | field | unit | sentinel | source | arithmetic |
|---|---|---|---|---|---|
| 0:1 | outside temperature, s16 | 0.1 °C | `8000` | `014` b4:b5 | `round((raw - 4000) / 10)`, half away from zero; raw `FFFF` keeps the previous value (the cluster does the same at wake); `8000` until the first valid read or when `014` is stale. Probable / high confidence |
| 2 | minutes since RUN, u8 | min | `FF` | `159` b0 | verbatim (`00..FE`); `FF` = transient re-init = unavailable. Confirmed |

### 3.6 `0xB7` TYRE (slow) — 5 bytes, layout as built in `feature/tpms-temp`

`[0]` position (0 FL, 1 FR, 2 RR, 3 RL; the spare, 4, is never sent), `[1:2]` kPa u16 (`FFFF` = no reading),
`[3]` tyre air temperature = **the raw WCM byte, °C = raw − 50** (confirmed on the car), `FF` = none,
`[4]` flags: bit 0 sensor battery low, bit 1 leak (WCM "pressure loss" trigger), bit 2 no signal,
bit 3 the car's low-pressure warning (WCM diag DB, unverified). A corner with no pressure sends `[3] = FF` and
`[4] = 04` whatever the extras said. One corner per frame, rotating every 5 s; the WCM is polled every 10 s in
the cycle `21 3B` (all pressures) → `21 2n` (wheel n temp/battery/trigger) → `21 25+n` (wheel n warning).
Pressures go stale after 4 missed `21 3B` polls. Contract: `docs/TPMS-POLL-SPEC.md`. (Corrected: the draft
said "temperature °C+40, always FF", which predates the tpms-temp firmware now on the car.)

### 3.7 `0xB8` TRIP (slow) — 6 bytes

| off | field | unit | sentinel | source | arithmetic |
|---|---|---|---|---|---|
| 0:2 | odometer, u24 | 0.1 km | `FFFFFF` | `014` b0:b1:b2 | verbatim three-byte copy (car sends `FFFFFF` for n/a). Confirmed |
| 3 | compass sector | enum 0 N..7 NW | `FF` | `1B4` b0 | verbatim; accept only b0 ≤ 7 and b1 ≤ 179 (both bytes together); a rejected frame does not refresh the source, so the last good values hold until `1B4` goes stale (1.5 s) |
| 4:5 | magnetic heading, u16 | degrees (2° steps) | `FFFF` | `1B4` b1 | `b1 * 2`, 0..358. `0` is due north, a real reading |

The compass does not stop at key-off (byte map, "Important absences"); it is not a sign of life.
Key-off is `0xB2` S0 only.

---

## 4. Field → CAN source → Dashwheel key

Keys are in `Catalogue.kt` (`K`). "NEW" keys are proposals; types follow the catalogue style.
Existing publishers: `XyautoBusPublisher.kt`.

| frame.field | CAN source | confidence | Dashwheel key |
|---|---|---|---|
| B2.S0 ignition enum | `000` b0 | Confirmed | NEW `IGNITION_STATE`: `State<IgnitionState>` (NoKey, KeyOff, Acc, On, Crank; 7 → withdraw) |
| B2.S0 engine running | `002`, `000` | Confirmed/derived | `ENGINE_RUNNING` |
| B2.S0 RAP | `006` b1.1 | Confirmed | NEW `ACCESSORY_POWER`: `State<Boolean>` |
| B2.S0 key in | `012` b2.1 | Confirmed | NEW `KEY_IN`: `State<Boolean>` |
| B2.S0 reverse | `003` b4 | Confirmed | `REVERSE` |
| B2.S0 park brake | `0D0` b0.7 | Confirmed | `HANDBRAKE` |
| B2.S1 brake pedal | `003` b3.0 | Confirmed | NEW `BRAKE_PEDAL`: `State<Boolean>` |
| B2.S1 belt unfastened | `013` b0.3 | Confirmed | `DRIVER_BELT` (true = unfastened, same sense) |
| B2.S1 doors, hatch | `013` b2.0–4 | Confirmed | `DOORS` (`DoorState`; bonnet has no source: leave false and do not display) |
| B2.S2 indicators L/R | `006` b0.0–1 | Confirmed | `INDICATORS` (`Indicators(left,right)`) |
| B2.S2 hazard | `006` b0.0+1 | Confirmed | `LIGHTS` contains `CarLight.HAZARD`, plus `INDICATORS(true,true)` |
| B2.S2 position/low/high/fog | `006` b0.3–7 | Confirmed | `LIGHTS` (`SIDELIGHTS`, `DIPPED`, `MAIN_BEAM`, `FRONT_FOG`, `REAR_FOG`) |
| (headlights flag) | stock `0x72` b1 | Confirmed | `HEADLIGHTS` stays on stock `0x72` illum; `INSTRUMENT_DIMMER` stays on stock `0x72`[3] |
| B2.S3 cruise on/engaged | `003` b1.7/.6 | Confirmed | NEW `CRUISE`: `State<Cruise>` (Off, Armed, Engaged) |
| B2.S3 A/C compressor | `003` b1.5 | Confirmed | NEW `AC_COMPRESSOR`: `State<Boolean>` |
| B2.S3 ESP off, MIL, oil lamp, charge lamp, ABS lamp; B2.S4 ESP lamp, glow lamp, water-in-fuel lamp | `003` b0, b1 | Confirmed/Probable | NEW `TELLTALES`: `State<Set<Telltale>>`, `Telltale` = ESP_OFF, MIL, OIL, CHARGE, ABS, ESP, GLOW, WATER_IN_FUEL |
| B2.S4 4WD range | `1B2` b1 | Confirmed | NEW `TRANSFER_CASE`: `State<TransferCase>` (High, Low, Shifting; 0 → withdraw) |
| B2.S5 gear | `003` b4 | Confirmed | `GEAR`: P/R/N/D → `Park/Reverse/Neutral/Drive`; AutoStick 1–4 → `Forward(n)` (see open question 7); `FF` → withdraw |
| B3 speed | `002` b2:b3 | Confirmed | `VEHICLE_SPEED` (km/h, apply the existing speed correction downstream) |
| B3 rpm | `002` b0:b1 | Confirmed | `RPM` |
| B3 load | `1A7`, `002` | Calibrated | `ENGINE_LOAD` (a real `0` is shown; `FF` withdraws) |
| B3 fuel flow | `1A7` | Confirmed | `FUEL_FLOW` (L/h = field / 100); `FUEL_CONSUMPTION` stays computed, from the corrected speed |
| B3 pedal | `02C` b2 | Confirmed | `THROTTLE` (%, accelerator pedal; kept from v1) |
| B3 oil pressure | `02C` b0 | Confirmed, scale probable | NEW `OIL_PRESSURE`: `Measurement<Double>`, kPa gauge (field × 4), warning priority |
| B3 boost / MAP | `2C0` b4 | Confirmed | NEW `MANIFOLD_PRESSURE`: `Measurement<Double>`, hPa absolute (boost above ambient = MAP − ambient, a consumer calculation) |
| B4 coolant | `02C` b1 | Confirmed | `COOLANT_TEMP` |
| B4 volts | `015` b1 | Confirmed | `BATTERY_VOLTS` |
| B4 fuel % | `159` b5 | Probable | `FUEL_LEVEL` |
| B4 fuel litres | `159` b5 | Probable | `FUEL_LITRES` |
| B5 outside temp | `014` b4:b5 | Probable | `OUTSIDE_TEMP` |
| B5 minutes since RUN | `159` b0 | Confirmed | NEW `ENGINE_OFF_MINUTES`: `Measurement<Double>`, min |
| B7 tyre | WCM `600`/`500` poll | proven elsewhere | `TYRES` |
| B8 odometer | `014` b0:b2 | Confirmed | `ODOMETER` |
| B8 sector + heading | `1B4` | Confirmed | `CAR_HEADING` (sector: no key) |
| stock `0xF0` | firmware | — | `CANBOX_VERSION` |

**Dropped, no CAN source:** intake temperature (`INTAKE_TEMP` is no longer fed by the box), steering angle
(`STEERING_ANGLE` can never be filled on CAN-B), range (`RANGE` is Dashwheel's own estimate; use
`FUEL_LITRES` over the measured consumption). The accelerator pedal is **kept** (B3[10] → `THROTTLE`).

**Stock fields that stay constant, which consumers must ignore:** `0x72`[2] SWC key (source `3A0` is
static `00 00`), `0x72`[4..5] steering (`015` has no steering source), `0xA6`/`0x73` (amplifier alias,
always zero), and `0x32`[2:3] rpm (stale by design, §7.2). Take speed, rpm, reverse, doors, gear
and ignition detail only from these frames. `0x72` b0 ACC, b1 illumination and [3] dimmer stay
live and stay in use (`IGNITION`, `HEADLIGHTS`, `INSTRUMENT_DIMMER`).

---

## 5. The engine-running state machine (unchanged)

```
rpmValid = (002 b0:b1 != 0xFFFF)     cranking = (000 b0 == 0xA9)
set   bit when  rpmValid && rpm >= 500 && !cranking, for 3 consecutive 002 frames (150 ms)
clear bit when  !rpmValid || rpm < 300 || cranking                  (immediate)
```

Evidence: byte map / `playback.log`: ACC `63` → rpm all `FFFF`; crank `A9` → 0…410; run `85` → 0…4163;
idle 577–580. Hysteresis band 300/500 sits in the gap.

---

## 6. Staleness (firmware side)

Every CAN source the block reads gets one timestamp (`ram::Scheduler::counter`, 1 kHz, wrap-safe u32
subtraction) stamped by its decoder on every accepted frame. At each 100 ms tick:

```
alive[src] = (now - lastMs[src]) <= limitMs[src]        limit = 3 x nominal period
```

A source with no frame since boot is not alive. A dead source puts its fields at their sentinels (§8), its
STATE bits at 0, and its S6 bit at 0. A returning frame revives it at once.

| src | nominal | limit | fields |
|---|---|---|---|
| `000` | 100 ms | 300 ms | ign enum |
| `002` | 50 ms | 150 ms | speed, rpm, engine running, load rpm |
| `003` | ~450 ms | 1.4 s | gear, telltales, cruise, A/C, brake, reverse |
| `006` | 100 ms | 300 ms | lights, indicators, RAP |
| `012` | ~1 s | 3 s | key in |
| `013` | 500 ms | 1.5 s | belt, doors |
| `014` | 100 ms | 300 ms | odometer, outside temp |
| `015` | 100 ms | 300 ms | volts |
| `02C` | ~500 ms | 1.5 s | coolant, oil pressure |
| `0D0` | ~1.15 s | 3.5 s | parking brake |
| `159` | ~1 s | 3 s | fuel, minutes since RUN |
| `1A7` | 100 ms (400 ms at key-off) | 1.2 s | flow, load Q |
| `1B2` | ~1.45 s | 4.4 s | 4WD range |
| `1B4` | 500 ms | 1.5 s | compass |
| `2C0` | 100 ms | 300 ms | boost |

`159` is special: while alive and gated (`b4 == 01`) it holds, as §3.4 says; once it is stale, it goes to
sentinels (that is the one case where a held fuel value is dropped).

---

## 7. Scheduler change — THE RISKIEST PART

> This is the only place this layout changes `svc::hu::txSched()` beyond the existing `pendingRequest()`
> correction. A mistake here can stop all TX (or, worse, corrupt `uartTxSlotIdx`). Treat it like the
> preamble: small diff, host-simulated first, then the pre-flight gate (`tools/verify_preamble.py` and the
> version-tag check in `tools/iap_upload.py`, `app/REBUILD.md`), then bench, then the car. The bootloader
> is never touched; a bad image is recovered by re-flashing the previous `.iap` through the stock IAP
> (reset → `U` → `19780217`), so keep the last good `canupdate*.iap` (built from
> `fix/can-map-corrections`) next to the new one.

### 7.1 What is wrong with today's scheduler for this load

48 ms per frame is 20.8 frames/s in total, and the stock slots (`0x72`, `0x32`, ...) plus a 10 Hz fast
frame plus slow frames already exceed it. The stock round-robin is change-driven, so a constantly changing
payload (fast frame, and stock `0x32` rpm) owns the line and everything else waits for a "busy refresh".
At 38400 8N1 a 15-byte frame takes 3.9 ms, so the 48 ms spacing is a stock cadence, not a line limit.

### 7.2 The change

Add a **lane** to each `kTxTable` row (`Prio`, `Slow`, `Request`) and a priority branch at the top of `txSched()`.
Everything below the branch is today's code.

```
txSched():                                  (called every 8 ms)
    uartTxTick++ ; if txBusy return
    [ACK queue drain, as today]
    if (prioPending())                      // NEW
        send ONE priority frame; return     // does not touch uartTxTick
    [today's 48 ms logic, unchanged, over Slow rows only]
```

- **Producer (new, 100 ms tick, from the 8 ms task):** drift-free divider on the 1 kHz counter
  (`next += 100`), so the rate is exactly 10 Hz on average with ≤ 8 ms jitter. On each tick:
  1. refresh `alive[]` (§6), run the indicator timers (§3.2.1), recompute STATE and FAST payloads;
  2. **queue `0xB3` FAST unconditionally**;
  3. queue `0xB2` STATE if its payload differs from the last one sent, or if 1 s has passed since the last send.
- **Priority order when several are pending:** table order, and the table puts `0xB3` before `0xB2`, so FAST
  goes first and `0xB2` on the very next pass, 8 ms later. One frame per 8 ms pass; at most 2 per tick, so the
  load is ≤ 16 % of passes. The ACK path keeps its place above the lane; the lane re-checks `txBusy`.
- **Slow rows** (`0xB4 B5 B7 B8` and the six stock rows) keep the 48 ms `uartTxTick` rule. Because a priority
  frame returns before the boundary test, the tick stays ≥ 6 and fires on the next free pass: no starvation,
  at most one pass of delay.
- **Slow-lane repeat:** `kIdleDelay` 15 → **2** in the jeep build (corrected: the keep-alive fires when the
  countdown reaches 0, so the period is `kIdleDelay + 1` boundaries; 3 would give 192 ms, 2 gives **144 ms**)
  and the keep-alive and busy-refresh index walk only the 10 slow rows (`nextSlot()` skips `Prio` and `Request`
  rows; `roundRobin()` compares only `Slow` rows). Nominal cycle 10 × 144 ms = **1.44 s**; priority frames that
  land on a boundary push it by one pass and the heartbeat takes one boundary a second, so the host simulator
  measures ≤ **1.61 s** idle and < 2 s under constant traffic. Changed rows still go out on the next 48 ms
  boundary. Nothing changes for the Benz image: it has no `Prio`/`Request` rows, and the branch is
  under `if constexpr (!cfg::kIsBenz)`.
- **`0xB0` stays `Request`:** marked with `kSlotRequest` from the 1 s task as today; the round-robin and
  keep-alive skip it so it is not sent twice a second.
- **Slow producers run from the 1 s task** (every 10th 100 ms tick; `0xB7` is packed there too), not every 8 ms, so a noisy sensor can claim a slow slot at most once
  a second. (The existing deadband rules still hold: volts 0.1 V, fuel 0.5 L, temperatures whole degrees.)
  The TPMS poll and 5 s rotation keep their own timers.
- **Stock `0x32` rpm:** rpm changes every frame and would resend `0x32` every 48 ms. Compare only
  bytes `[0..1]` (gear) of the `0x32` payload in `roundRobin()` for change detection (a per-row compare
  length `cmp`; 0 = the whole row, for every other row); rpm in `0x32` then goes out only on the slow cycle.
  Confirmed against `ram_map.hpp`: the `0x32` window is `VehicleState` 0x1D..0x2A, `[1]` = gear (0x1E),
  `[2:3]` = rpm (0x1F/0x20).

### 7.3 Wiring checklist (every step; four of them fail silently)

1. `cfg::kTxSlotCount` 16 → **13** (6 stock + `B0 B2 B3 B4 B5 B7 B8`); `svc::hu::hiworld::kSlots` 16 → 13; the existing
   `static_assert` couples them.
2. `ram::AppRam::uartSlotState` is sized from `kTxSlotCount` and is **immediately followed by `uartTxSlotIdx`**;
   a short array corrupts the index while tests still pass. Keep the assert.
3. `ram::CustomFrames`: remove `bB`, `bC`, `bD`; resize `b0[4] b2[7] b3[11] b4[5] b5[3] b8[6]`; `b7[5]` as is.
   `ram::UartShadows`: remove `msgBB`, `msgBC`, `msgBD`; resize to match. Append, do not reorder the stock six.
   `b3` has no meaningful shadow (always sent); keep one so the row machinery stays uniform.
4. `hiworld::kTxTable`: rewrite rows 6..12 in the order `B0 B3 B2 B4 B5 B7 B8` (FAST before STATE, see 7.2), add the `lane` field and the
   per-row compare length, and redo every row-geometry `static_assert`. Indexes shift: nothing may be
   addressed by a hard-coded index.
5. `CustomSignals` (`ram_map.hpp`): drop `throttlePct`, `intakeBias40`, `valid`, `flow*` window state,
   range; add `lastMs[]` per source, indicator timers, STATE bytes, fast-window sums. `customPrimeSignals()` primes
   every field to its sentinel and every source to dead.
6. `custom_decode.cpp`: new decoders (or extensions) for `003`, `006`, `012`, `013`, `0D0`, `1B2`, `02C` b0, `2C0` b4, `159` b0,
   `000` enum; `1A7` now fills the 100 ms window, not a 1 s window; stamp `lastMs` in each. Dispatch cases for ids not yet in
   `can_decode.hpp` (`kJeepFuelFlow` etc. exist; `0x012`, `0x1B2`, `0x003` as needed), each behind
   `if constexpr (!cfg::kIsBenz)`; all are below `0x400`, so the no-op argument for `decodeSwcService()` applies.
7. `custom_tx.cpp`: split `customPublish()` into `customTick100()` (fast + state) and `customTick1000()` (slow
   payloads + heartbeat request). Remove `0xBB/BC/BD` packing and `kRangeNoSource`.
8. `cfg::kVersionString` token stays `H1N4JPF20L`; only the DDMMYY date moves. Nothing selects on a layout version.
9. Requirements: new FR block in `docs/REQUIREMENTS.md`; mark FR-108…FR-141 superseded.

---

## 8. Sentinels

| type | sentinel |
|---|---|
| u8 temperature °C+40, volts, fuel %, load, pedal, oil pressure, minutes, compass sector, gear | `0xFF` |
| u16 speed, rpm, flow, boost, heading, litres | `0xFFFF` |
| s16 outside temperature | `0x8000` |
| u24 odometer | `0xFFFFFF` |
| ignition enum | 7 |
| 4WD enum | 0 |
| tyre | as today (`FFFF` kPa, flag bit 2) |

`0` is a real reading everywhere above (overrun flow, idle oil off, due north, 0 % load).
Largest legal values stay below the sentinels: speed ≤ 512.0 km/h (`FFFE` → 5120), flow ≤ 512.0 L/h (`Q = FFFE` → 51200),
temperature byte ≤ `FE`.

---

## 9. Bandwidth

Line: 38400 baud 8N1 = 3840 B/s. Wire bytes = payload + 5.

| frame | wire | rate (idle / steady) | B/s |
|---|---|---|---|
| `0xB3` FAST | 16 | 10 Hz | 160 |
| `0xB2` STATE | 12 | 1 Hz (changes add; worst 10 Hz) | 12 (120) |
| `0xB0` HEARTBEAT | 9 | 1 Hz | 9 |
| slow lane, 10 rows per 1.44 s: `B4 10, B5 8, B7 10, B8 11`, stock `0x72 19, 0xA6 13, 0x73 13, 0xF0 22, 0x12 15, 0x32 19` | 140 per cycle | 6.9 frames/s | 97 |
| **total, steady** | | **18.9 frames/s** | **278 B/s = 7.2 %** |
| worst case: STATE 10 Hz, slow lane saturated at one frame / 48 ms (20.8/s x 22 B max) | | ≤ 41 frames/s | ≤ 737 B/s = 19 % |

The current block (16 slots, 11.5 s idle keep-alive, 2.3 s refresh) offers no repeat guarantee faster than 2.3 s.
The new layout guarantees: fast every 100 ms, STATE ≤ 108 ms after a change and every 1 s, slow rows ≤ 1.44 s.
Time on the wire per frame: 16 B = 4.2 ms, 22 B (largest, `0xF0`) = 5.7 ms; both finish inside one 8 ms pass.

---

## 10. Test vectors

Every checksum is from the §1 rule (`CKS = (L + CMD + Σpayload − 1) & 0xFF`); each was also checked against
the full sum including `AA 55`. Use them as host unit tests.

| frame | meaning |
|---|---|
| `AA 55 02 E0 00 00 E1` | rule check: the head unit's upgrade trigger |
| `AA 55 04 B0 02 01 BD 2A 9D` | heartbeat, version 2, capability `01BD`, counter `2A` |
| `AA 55 04 B0 02 01 BD 00 73` | the same, counter wrapped to `00` |
| `AA 55 0B B3 02 76 0A 8D 1B 04 B0 3C 04 80 22 7D` | 63.0 km/h, 2701 rpm, load 27 % (Q 1536 at 2701 rpm), 12.00 L/h, oil `3C` (240 kPa), 1152 hPa abs, pedal 34 % |
| `AA 55 0B B3 00 00 02 44 0B 00 64 28 03 F0 00 8D` | stopped at idle: 0.0 km/h, 580 rpm, load 11 % (Q 128), 1.00 L/h, oil 160 kPa, 1008 hPa, pedal 0 % |
| `AA 55 0B B3 FF FF FF FF FF FF FF FF FF FF FF B2` | **every field unavailable** (box alive, CAN dead): still sent, still liveness |
| `AA 55 07 B2 2B 04 19 10 01 03 FF 13` | ON + engine running + key in; FL door open; position + low + left flashing; MIL; 4H; gear D; all sources alive |
| `AA 55 07 B2 23 04 19 10 01 03 FD 09` | same, but `002` dead: engine-running bit written 0, S6 bit 1 clear (consumer withdraws it) |
| `AA 55 07 B2 A1 00 00 00 00 FF A1 F9` | key in at OFF, parking brake on; `003`, `006`, `013`, `1B2`, `002` dead; gear `FF` |
| `AA 55 05 B4 85 8F 5B 02 C6 EF` | coolant 93 °C, 14.3 V, 91 %, 71.0 L (b5 = 142) |
| `AA 55 05 B4 FF 8F 5B 02 C6 69` | coolant not available (`02C` stale), the rest live |
| `AA 55 03 B5 00 FA 0A BB` | outside +25.0 °C, 10 min since RUN |
| `AA 55 03 B5 FF B5 FF 6A` | outside −7.5 °C (sign check), minutes unavailable |
| `AA 55 06 B8 12 D6 87 06 01 0A 3D` | odometer 123 456.7 km, West, 266° |
| `AA 55 06 B8 12 D6 87 00 00 00 2C` | same odometer, **North 0° is a real reading** |
| `AA 55 06 B8 FF FF FF FF FF FF B7` | odometer and compass unavailable |

Worked check, `B3` row 1: `0x0B + 0xB3 + 0x02 + 0x76 + 0x0A + 0x8D + 0x1B + 0x04 + 0xB0 + 0x3C + 0x04 + 0x80 + 0x22 − 1 = 0x37D`
→ `0x7D`. Load: `15625 × 1536 / (324 × 2701) = 27.4` → 27. Pedal: raw 85 → `(170 + 2) / 5 = 34`. Speed: 63 × 128 = 8064 raw; `8064 × 10 / 128 = 630 = 0x0276`.
Flow: Q = 1536 → `1536 × 100 / 128 = 1200 = 0x04B0`. Boost: raw `0x90` = 144 → 1152 = `0x0480`.

Also tested on the host (in `tests/host/test_custom_tx.cpp`; every vector above is a test there, the
`B3`, `B2`, `B4`, `B5` and `B8` ones driven through the real CAN decoder):

- `002` all-`FF` ⇒ B3 speed and rpm `FFFF`, load `FF`, STATE engine bit 0, S6 bit 1 clear.
- `002` silent 151 ms ⇒ B3 speed/rpm `FFFF` (3 × 50 ms); `159` silent 3.1 s ⇒ fuel `FF/FFFF`; `159` b4 == 1 while alive ⇒ fuel holds.
- `1A7` Q 0 ⇒ flow `0000`; Q `FFFF` ⇒ `FFFF`; Q = 1536 for 100 ms ⇒ `04B0`; two intervals (50 ms at 2560, 50 ms at 0) ⇒ 1000 / 128 x ... exactly the time-weighted mean.
- load: rpm 2000 / Q 4147 ⇒ 100; Q 0 ⇒ 0; rpm 250 ⇒ `FF`; `1A7` silent 1.3 s ⇒ `FF`.
- pedal: raw 250 ⇒ 100, 254 ⇒ 100, 2 ⇒ 1, `02C` stale ⇒ `FF`.
- lock flash: `006` b0 `03` with `000` = `00`/`01` ⇒ no indicator bits; with `000` = `63` ⇒ bits 0, 1, 2.
- crank: `000` b0 `A9` with rpm 410 ⇒ engine bit stays 0; rpm 600 for 3 frames ⇒ sets on the third.
- indicators: `006` b0 bit 0 pulsing 400 ms on / 400 ms off ⇒ STATE S2 bit 0 set once, never clears between blinks, clears 700 ms
  (plus ≤ 100 ms tick) after the last set frame; `1B` ⇒ bits 0, 1, 2 all set.
- ignition enum for `00/01/63/85/A9` and an unknown byte (⇒ 7).
- gear for `50/52/4E/44/34/33/32/31`; `003` b2 `C0` ⇒ `FF`; unknown ASCII ⇒ `FF`.
- 4WD: `1B2` b1 `02/01/00/07` ⇒ 1/2/3/0.
- STATE is queued ≤ 100 ms after a change, repeated at 1 s, and not otherwise.
- FAST is queued on every 100 ms tick (1000 ticks ⇒ 1000 ± 1 frames; no drift).
- scheduler: with FAST + STATE + all slow rows changing, every slow row is still sent at least once per 2 s and no
  `uartTxSlotIdx` corruption (§7.3 item 2); `kTxSlotCount == 13`.
- Benz image: byte-identical scheduler behaviour (existing Benz tests unchanged).
- `0x32` compare mask: an rpm-only change does not send `0x32`; a gear change does.

---

## 11. Dashwheel side (for the other repo; not part of this change)

1. **Update `Jpf20lDecoder` in place** (and `Jpf20lCmd`/`Jpf20lSignals`) to this layout. There is one decoder and one
   layout: no second decoder, no selection by version or revision. It keeps wrapping `HiworldJpf2Decoder` and keeps
   filtering the stock values this block replaces: `Rpm`, `Reverse`, `Doors`, `Gear` (stock `0x12`/`0x32`); it ignores
   `0x72`[2], `0x72`[4..5], `0xA6`, `0x73`. Its tests are rewritten to the vectors in section 10.
2. **Decoder matching** stays as it is (`matches(version)`, token `H1N4JPF20L`); the firmware version token does not change.
3. **Publisher:** the case set in `XyautoBusPublisher` (new `CanboxValue` variants for the NEW keys; a null
   in a value withdraws the key). No decoder knows a key and no key knows a canbox, as today.
4. **Withdrawal:** the consumer never needs a group mask. A field at its sentinel is `null`; a STATE bit
   whose S6 bit is 0 is `null`. `0xB0` only informs (diagnostics): log a counter that does not continue by +1; nothing branches on the version byte.
5. **TTL suggestions:** keys fed by `0xB3` ~0.5 s (five missed frames); keys fed by `0xB2`, `0xB4`, `0xB5`, `0xB7`, `0xB8` ~5 s
   (the 1 s state repeat and the 1.44 s slow cycle give 3+ chances). A missing `0xB3` for 0.5 s takes the
   whole source down (replaces today's 10 s `SAY_HOLD_MS`). Fuel and odometer may keep the key's own longer
   hold; the firmware already holds fuel on the gate.
6. **Consumption:** `FUEL_CONSUMPTION = flow / corrected VEHICLE_SPEED × 100`, withdrawn below 5 km/h, as now.
   Integrate litres as `Σ(flow × 0.1 s)` on each `0xB3`.
7. **Profile:** set the Jeep WK tank to 77.6 L and a realistic consumption; use `FUEL_LITRES` for range.
8. **Do not treat `0xB8` compass as a sign of life.** Liveness is `0xB3`; ignition is `0xB2`.

---

## 12. Test plan

### 12.1 Host (no hardware)

Everything in §10 plus a scheduler simulator: feed a synthetic 8 ms clock, record every emitted frame, assert the
cadences of §9 (fast 100 ± 8 ms, state latency, slow ≤ 1.44 s + one pass), no frame overlaps (`txBusy`), no
starvation when STATE changes every tick, `uartTxSlotIdx` intact after 10⁶ passes. `python tools/test.py`
(both protocols) must pass; Benz tests untouched.

### 12.2 Bench (box on the bench; flashing and ports are the owner's step, not part of this spec)

1. Build; pre-flight gate green (preamble and version tag).
2. Flash through the IAP with the existing proven loop; read back and verify as before; boot-liveness check.
3. Replay a CAN capture (the byte map's source logs, or a second CAN node) with a UART capture on the box's TX:
   - measure the spacing of `0xB3` (expect 100 ms ± 8), the repeat of every slow frame (≤ 1.44 s, never > 2 s), STATE edge latency;
   - stop each CAN id in turn and confirm its fields hit the sentinel at 3x its period and recover on return;
   - replay a recorded blink sequence on `006` and read the real on/off durations (resolves open question 2);
   - confirm line load against §9 (< 10 % steady) and that the heartbeat counter increments by 1;
   - confirm the unchanged `0xB7` rotation and 30 s poll.
4. Keep the previous `.iap` ready to re-flash.

### 12.3 Car

1. Ignition cycle: OFF → ACC → ON → crank → run → OFF; log ign enum, RAP, key-in, engine bit.
2. Indicators L, R, hazard for 10 s each: no strobing in STATE; release latency < 1 s.
3. Lights (position, low, high, front and rear fog), doors and hatch one by one, belt, brake pedal, park brake, reverse, each gear incl. AutoStick 1–4.
4. A/C on/off cycles, cruise on / set / off, ESP-off button, 4H ↔ 4L (shift shows "shifting" ~1.6 s).
5. Drive: speed vs GPS, rpm, load vs the byte map's idle ≈ 10 % and overrun 0 %, flow at idle (≈ 1.0 L/h), oil pressure, boost (saturation
   at `FE`).
6. Pull a fuse or unplug nothing on the CAN side; verify staleness only by bench replay (12.2). In the car, check that the Fuel tile no
   longer flickers across key-off/on.
7. Watch bandwidth in the head-unit log: no frame loss, no checksum errors over a 20-minute drive.

---

## 13. Open questions

1. **Per-source flag in STATE (S6).** The decisions say "per-field sentinels only", but a bitfield cannot hold a sentinel.
   S6 is the minimal fix and lives inside the frame, not in the heartbeat. Alternatives: two bits per signal (wasteful), or a
   rule that a stale STATE is withdrawn whole. Confirm S6 is acceptable.
2. **Indicator hold time (700 ms).** Derived, not measured. The byte map has no blink timing and says the `11D` turn bits are "held while the
   stalk is out" (it does not say whether `006` b0 bits 0/1 pulse or hold). Measure from a recorded `006` stream; adjust the constant.
3. **Replacing the block under shipped consumers.** Because the layout changes in place with no version switch, firmware and
   `Jpf20lDecoder` must be flashed and installed together; an old Dashwheel against the new firmware (or the reverse) will mis-decode.
   Confirm this is acceptable (dev mode, one unit).
4. **Stock `0x32` compare mask.** Assumes `0x32`[1] gear and [2:3] rpm (per the Dashwheel decoder notes). Confirm against `can_decode`'s
   `0x32` window, and that no unit-side consumer needs a live rpm from stock `0x32`.
5. **Fuel litres scale.** `159` b5 × 0.5 L reads ~4 L below the PCM's own level at ~70 L (byte map); the low-tank calibration is still open.
   Carry as is, fix the scale later in one place.
6. **Boost offset and oil scale.** `2C0` b4 fits `8.00 × raw + 4` hPa; the spec sends `8 × raw`. Oil gauge scale is "probable". Calibrate in the car.
7. **AutoStick mapping.** AutoStick 1–4 as `Gear.Forward(n)` assumes the cluster digit is the gear in use, not only a held limit.
   If it is a limit, Dashwheel needs a different value. Decide on the consumer.
8. **Key in / `0D0` RUN bit.** Key-in uses `012` b2 bit 1 (period ~1 s); the byte map's `000` b0 bit 0 is the faster Probable alternative
   and is already implied by the ignition enum (≥ 1). Keep `012`, or drop the bit?
9. **Hazard from one frame.** Spec reads hazard as both `006` bits set in one frame (`1B`). If the flasher drives the two sides
   out of phase on some cars, hazard must be detected by both timers running at once.
10. **Slow-lane period vs TPMS.** `0xB7` is resent every cycle with the current corner; the corner rotates every 5 s. Consumers need
    ~20 s for a full set, as today. Acceptable?
11. **`kIdleDelay` 2 (144 ms).** A 144 ms keep-alive step has not been tried against the head unit's serial reader; check on the bench.
12. **ACC/RAP gate.** The block stops with `kFlagAccOn`; the KeyOff/NoKey enum values are visible only while RAP holds the box up.
    Confirm Dashwheel does not expect them otherwise.

13. **A/C request** (button lit, compressor off at low ambient): no body-bus source is known; S3 bit 2 is the
    compressor only. Test (from the withdrawn `0xBA`/`0xBE` draft): engine running, A/C on/off at ~5 °C and at
    ~25 °C ambient with a CAN-B log; look for any id other than `0EC`/`003` changing. Do not feed `K.CLIMATE`
    from the compressor bit: compressor cycling is not "A/C on".

**Status after the build (2026-11-07):** 2 — the debounce works whether the bits pulse or hold (§3.2.1), the
constant still wants a measured blink period. 4 — confirmed (§7.2). 9 — kept as "both bits in one frame".
11 — the constant is 2, not 3 (§7.2); bench check still open. 12 — superseded: there is no ACC gate on the
UART (§2 ground rules). 1, 3, 5, 6, 7, 8, 10, 13 stay open for the owner.

---

## 14. Wire summary (what a decoder implements)

Frame: `AA 55 L CMD payload[L] CKS`, `CKS = (L + CMD + Σpayload − 1) & 0xFF`. Big-endian. Bit 0 = LSB.
A field at its sentinel = unavailable. A STATE bit counts only when its S6 source bit is 1.

| cmd | L | offset: field (unit, sentinel) |
|---|---|---|
| `B0` | 4 | `0` layout version (`02`, diagnostics only) · `1:2` capability u16 (`01BD`: bit n = cmd `B0+n` is sent) · `3` counter u8 (+1 per heartbeat, wraps) |
| `B2` | 7 | `0` S0 · `1` S1 · `2` S2 · `3` S3 · `4` S4 · `5` S5 gear · `6` S6 SRC (bit tables below) |
| `B3` | 11 | `0:1` speed u16 0.1 km/h (`FFFF`) · `2:3` rpm u16 (`FFFF`) · `4` load u8 % (`FF`) · `5:6` fuel flow u16 **0.01 L/h** (`FFFF`) · `7` oil u8 ≈4 kPa/count gauge (`FF`) · `8:9` MAP u16 hPa abs (`FFFF`) · `10` accelerator pedal u8 % (`FF`) |
| `B4` | 5 | `0` coolant u8 °C+40 (`FF`) · `1` battery u8 0.1 V (`FF`) · `2` fuel u8 % (`FF`) · `3:4` fuel u16 0.1 L (`FFFF`) |
| `B5` | 3 | `0:1` outside air s16 0.1 °C (`8000`) · `2` minutes since RUN u8 (`FF`) |
| `B7` | 5 | `0` position 0 FL 1 FR 2 RR 3 RL · `1:2` pressure u16 kPa (`FFFF`) · `3` tyre temp raw, °C = raw − 50 (`FF`) · `4` flags: b0 battery low, b1 leak, b2 no signal, b3 low-pressure warning |
| `B8` | 6 | `0:2` odometer u24 0.1 km (`FFFFFF`) · `3` compass sector 0 N … 7 NW (`FF`) · `4:5` heading u16 degrees, 2° steps (`FFFF`) |

Never sent: `B1`, `B6`, `B9`–`BF`.

STATE bits (1 = true):

| byte | b0 | b1 | b2 | b3 | b4 | b5 | b6 | b7 |
|---|---|---|---|---|---|---|---|---|
| S0 | ign enum b0..b2: 0 NoKey, 1 KeyOff, 2 ACC, 3 ON, 4 Crank, 7 n/a | ← | ← | engine running | RAP | key in | reverse | parking brake |
| S1 | brake pedal | driver belt **unfastened** | door FL | door FR | door RR | door RL | hatch | 0 |
| S2 | left indicator | right indicator | hazard (b0, b1 also set) | position | low beam | high beam | front fog | rear fog |
| S3 | cruise on | cruise engaged | A/C compressor | ESP off | MIL | oil lamp | charge lamp | ABS lamp |
| S4 | 4WD enum b0..b2: 0 n/a, 1 4H, 2 4L, 3 shifting | ← | ← | ESP lamp | wait-to-start | water in fuel | 0 | 0 |
| S5 | gear byte: 0 P, 1 R, 2 N, 3 D, 4..7 AutoStick 1..4, `FF` n/a | | | | | | | |
| S6 | `000` alive (S0 ign) | `002` (engine running) | `003` (reverse, brake, S3, S4 b3-5, S5) | `006` (RAP, S2) | `013` (belt, doors) | `0D0` (park brake) | `1B2` (4WD) | `012` (key in) |

Enum sentinels: ignition 7, 4WD 0, gear `FF`. The ignition enum and the gear byte carry their own sentinel as well
(7 / `FF` when their source is dead).

Cadence: `B3` every 100 ms (liveness: none for 0.5 s = box gone); `B2` on change (≤ 108 ms) and every 1 s; `B0`
1 Hz; `B4 B5 B7 B8` packed once a second and each resent at least every ~1.6 s (nominal 1.44 s).

---

## 15. Corrections made while building (2026-11-07)

Decoder-visible (the Dashwheel decoder must follow these):

1. **`0xB3` is 11 bytes, not 10**: new byte `[10]` = accelerator pedal % (`02C` b2 × 0.4, clamp 100, `FF` n/a).
   Wire length 16. The three `B3` vectors in §10 are recomputed.
2. **`B3` load bytes in the vectors** were inconsistent with the stated inputs: 2701 rpm / 12.00 L/h gives 27 %
   (was 55), idle 580 rpm / 1.00 L/h gives 11 % (was 10).
3. **`B4` fuel % in the vectors**: b5 = 142 (71.0 L) is 91 %, not 92 (`round(142 × 500 / 776)` = 91.49 → 91).
4. **`0xB7` text**: temperature is the raw WCM byte, °C = raw − 50 (not °C+40 / always `FF`); flags bit 0 battery,
   bit 1 leak, bit 2 no signal, bit 3 low-pressure warning; poll every 10 s (`21 3B`, `21 2n`, `21 25+n`).
   This is the layout the car already runs (`feature/tpms-temp`), so a decoder that handles today's `0xB7` needs
   no change.
5. **Speed sentinel**: `FFFF` when `002` b2:b3 is `FFFF` (its own field), not when b0:b1 is. On the car the frame
   goes all-FF together, so this only matters for a speed reading with rpm n/a.
6. **Reverse (S0 bit 6)** is set exactly when the S5 gear byte is 1, i.e. it is also 0 when `003` b2 = `C0`.
7. **Indicators with the key out**: `006` indicator bits are ignored while `000` reads `00`/`01` (lock-confirm
   flash). A hazard switched on with the key out is not shown.

Firmware-side only:

8. `kIdleDelay` is 2, not 3 (144 ms step). Priority order is the table order `B0 B3 B2 …` (FAST first).
9. No ACC gate on the UART (the draft claimed one).
10. Fuel flow is sample-and-hold, each Q held ≤ 1.2 s; load uses the window's Qavg with the exact integer
    `15625 Q / (324 rpm)`.
11. A `1B4` sample outside the gate does not refresh the source (the held values go stale after 1.5 s).
12. Every decoder rejects (and does not stamp) a frame shorter than the bytes it reads.
13. The slow cycle measures ≤ 1.61 s in the host simulator, not exactly 1.44 s (boundary pushes + heartbeat).
