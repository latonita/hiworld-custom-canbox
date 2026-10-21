# Custom TX block `0xB0`–`0xBF` — firmware implementation spec

> **This is a work order, not an analysis.** Everything here is decided. Build
> exactly this. Where something is deliberately NOT built, it says so and why.
>
> **Status: BUILT, 2026-10-21**, in `app/logic/custom_decode.cpp` and
> `custom_tx.cpp`, 55 tests in `tests/host/test_custom_tx.cpp`, shipped as
> `H1N4JPF20L-211026` (only the DDMMYY date moves -- `JPF20` is the
> protocol token, not a build number). Requirements FR-108…FR-121 in `docs/REQUIREMENTS.md`.
> The only place the firmware does something this document does not say is the
> `kSlotRequest` correction boxed in §6.
>
> Target: `app/` in this repository, the **Hiworld/Jeep build only**
> (`--protocol jeep`, `cfg::kIsBenz == false`). The Benz build is unchanged.
>
> Evidence and reasoning live in `../canbus-to-canbox-signal-map.md`. Read it if
> you want to know *why* a byte means what it means. You do not need it to
> implement this.
>
> **§9 is BUILT too, 2026-10-21.** Its two further commands, `0xBB` ODOMETER
> and `0xBD` COMPASS, took the block from 12 slots to 14. §1–§9 all describe
> the shipped image now; nothing in this document is outstanding.
>
> Scope decided 2026-10-21: **no contract changes.** `0xB3` keeps rpm even
> though stock `0x32` also carries it; `0xB2` keeps all four bits even though
> ACC and reverse are also in stock `0x72`. Fuel flow is **not** added to
> `0xB8`. Engine load is **parked**. `0xB1` and `0xB6` are dropped as pure
> duplicates of stock `0xF0` and `0x12` — they add no signal, only slots.

---

## 1. Frame format — read this twice

```
AA 55 | L | CMD | payload[L] | CKS            wire length = L + 5

L   = PAYLOAD LENGTH ONLY. Not CMD, not the preamble, not the checksum.
CKS = (0xAA + 0x55 + L + CMD + Sum(payload)) & 0xFF
    = (L + CMD + Sum(payload) - 1) & 0xFF      since 0xAA + 0x55 = -1 (mod 256)
```

This is what `svc::hu::emitFrame()` already does; the new commands go through
the same path and **need no framing code at all**. It is restated here only
because a restatement of it got this wrong once already — a parser was written
with `L` counting CMD+payload and the preamble left out of the sum, and it
rejected 99.6 % of real frames. If anything you write disagrees with the block
above, the block above wins. Source of truth: `docs/notes/hiworld-frame-format.md`
and requirement **FR-77**.

Single best regression vector: `AA 55 02 E0 00 00 E1`, the head unit's real
upgrade-trigger frame.

Multi-byte values are **big-endian**. Temperatures are **one byte biased +40**
unless a field says otherwise.

---

## 2. What to build, and what not to

| cmd | name | build? | slots |
|---|---|---|---|
| `0xB0` | HEARTBEAT | **yes** | 1 |
| `0xB1` | VERSION | **NO** — redundant | 0 |
| `0xB2` | IGNITION | **yes** | 1 |
| `0xB3` | DRIVETRAIN | **yes** | 1 |
| `0xB4` | ENGINE | **yes** | 1 |
| `0xB5` | CLIMATE | **yes** | 1 |
| `0xB6` | BODY | **NO** — redundant | 0 |
| `0xB7` | TYRE | **NO** — parked | 0 |
| `0xB8` | FUEL | **yes** | 1 |
| `0xB9` | KEY | **NO** — no source | 0 |
| `0xBB` | ODOMETER | **yes**, §9 | 1 |
| `0xBD` | COMPASS | **yes**, §9 | 1 |

**Do not implement `0xB6`.** The door bitfield already goes to the head unit in
stock Hiworld `0x12` at payload byte 2 — `VehicleState::kDoors` (`0x15`) sits
inside the `0x12` window (`0x13`, len 10), and `decodeDoors()` already fills it
with the correct bit permutation. A `0xB6` would put the same data on the same
wire twice. The command stays **reserved**; consuming `0x12` is Dashwheel-side
work and is not your concern.

**Do not implement `0xB1`.** The firmware version already goes to the head unit
in stock Hiworld `0xF0`: a 17-byte periodic TX slot whose live buffer is
`ram::g_versionString`, filled from `cfg::kVersionString` in `svc::hu::init()`
(`H1N4JPF20L-211026` on this build). It is on the wire and in the `rxpduses`
mirror already. Reserved.

**Do not implement `0xB7`.** TPMS is not broadcast on this car's CAN-B; it needs
a request/response exchange that has not been reverse-engineered. Reserved.

**Do not implement `0xB9`.** Steering-wheel buttons have no verified source on
this car: `0x3A0`, which the stock decoder reads, is static `00 00` across all
2 944 frames of `testlog/drive.log`. Reserved.

**Never emit `0xE0`** (firmware-upgrade trigger), `0xFF`/`0xFE` (ACK/NAK) or
`0xD2` (cluster text) from this block.

---

## 3. The payloads

Byte offsets are into the payload, i.e. after CMD. "Source" is CAN identifier
and byte on the 83.333 kbit/s bus, zero-based.

### `0xB0` HEARTBEAT — 2 bytes, once a second

| off | field | value |
|---|---|---|
| 0 | protocol revision | `0x01` |
| 1 | validity bitmask | bit0 `0xB2`, bit1 `0xB3`, bit2 `0xB4`, bit3 `0xB5`, bit4 `0xB7`, bit5 `0xB8`. **1 = the data in that command is live.** bits 6–7 reserved, send 0 |

Byte 1 is the contract's "box state flags", which was never given a meaning —
defining it is not a contract change. A bit goes to 0 when the box is alive but
has lost the source (see §4), which is the one case a consumer cannot detect by
timing. `0xB7` bit 4 is always 0 in this build.

`0xB0` must change every second so the round-robin actually sends it. The
validity mask alone may be constant for hours, so **toggle nothing artificially**
— instead mark this slot with `kSlotRequest` from the 1 s task (§6). This is
the first real use of that state: the scheduler implements it and nothing in
the image has ever set it.

### `0xB2` IGNITION — 1 byte

| bit | meaning | source | rule |
|---|---|---|---|
| 0 | ACC on | `000` b0 | set when b0 ∈ {`0x63`, `0x85`, `0xA9`}; clear on `0x00`/`0x01` |
| 1 | engine running | `002` b0:b1 | **state machine, §5** |
| 2 | reverse | `003` b4 | set when b4 == `0x52` (`'R'`) |
| 3 | parking brake | `0D0` b0 | set when `b0 & 0x80` |
| 4–7 | reserved | — | 0 |

`0D0` b0 reads `0x04` released and `0x84` engaged — **mask bit 7 only**, the
`0x04` baseline is not the brake.

### `0xB3` DRIVETRAIN — 6 bytes

| off | field | source | arithmetic |
|---|---|---|---|
| 0:1 | speed, km/h, u16 | `002` b2:b3 | `(b2<<8 \| b3) / 128`, rounded |
| 2:3 | rpm, u16 | `002` b0:b1 | **verbatim two-byte copy** |
| 4 | engine load % | — | **always `0x00`** — parked, see below |
| 5 | throttle % | `2C0` b2 | `(b2 - 23) * 100 / 211`, clamp 0…100 |

- **`002` all-`FF` means unavailable.** When `b0:b1 == 0xFFFF`, speed and rpm are
  both invalid: send `0xFFFF` in **both** fields and clear the `0xB3` validity
  bit. Do **not** compute `0xFFFF / 128` — that is 512 km/h.
- **Throttle has a redundancy check.** `2C0` b6 mirrors b2 exactly in all 14 457
  frames of `drive.log`. If `b2 != b6`, discard the sample and keep the previous
  throttle value.
- **Engine load is parked.** `02C` b2 is the load-shaped channel (r=+0.988 with
  pedal) but its 0…136 range has never been calibrated against a scan tool.
  Send `0x00`, which the contract defines as unknown. Do not scale a guess.

### `0xB4` ENGINE — 5 bytes

| off | field | source | arithmetic |
|---|---|---|---|
| 0 | coolant, °C+40 | `02C` b1 | **verbatim byte** — the car already sends °C+40 |
| 1 | intake, °C+40 | `015` b2 | `b2 + 40` |
| 2 | fuel level % | `159` b5 | `round(b5 * 0.6443)`, clamp 0…100 |
| 3:4 | volts, centivolts, u16 | `015` b1 | `b1 * 10` |

- **`0xFF` is the reserved "not available" code for every +40-biased
  temperature.** Changed 2026-10-21, superseding an earlier rule that said to
  suppress the whole frame — that was wrong, because `0xB4` also carries intake,
  fuel % and **voltage**, and one dead sensor must not cost three good signals.
  When `02C` is all-`FF`, write `0xFF` into `[0]`, keep sending the frame, and
  clear the `0xB4` validity bit.
  Without a reserved code there was no way to say "unknown": every byte from
  `0x00` to `0xFF` decodes inside the accepted `-40…215 °C`, so `0xFF` read as a
  perfectly plausible 215 °C and would have tripped the overheat alert.
  The consumer side is already done — `XyautoProtocol.TEMP_NOT_AVAILABLE`. The
  cost is that exactly 215 °C is unrepresentable; the usable top is `0xFE` =
  214 °C, which no coolant, intake or tyre reaches.
- **The coolant byte is still a verbatim copy**, and that now works in both
  directions: the car sends `0xFF` for unavailable and `0xFF` is our code for
  unavailable, so no special case is needed on this field at all.
- **`159` b4 is a hard gate.** `b4 == 0x01` means `b5 = 0xFF` is not a reading.
  When b4 is set, hold the previous fuel figure and clear the `0xB8` validity
  bit (not `0xB4`'s — the other three fields are still good).
- **Intake must be clamped before biasing, not computed blindly.** `b2 + 40`
  overflows: `0xFF + 40` truncates to `0x27` = −1 °C, a plausible-looking lie.
  If `015` b2 ≥ `0xD7` (which would bias past the reserved code) or the source
  is unavailable, write `0xFF` explicitly.
- Intake negative encoding has never been observed on this car. If `015` b2
  ≥ `0x80`, treat the sample as suspect and send `0xFF`.

### `0xB5` CLIMATE — 2 bytes

| off | field | source | arithmetic |
|---|---|---|---|
| 0:1 | outside air, decidegrees C, **s16** | `014` b4 | `b4 * 10` |

Signed, so −7.5 °C is `0xFFB5`. Negative encoding of `014` b4 has never been
captured — if b4 ≥ `0x80`, hold the previous value and clear the validity bit
rather than guessing the car's sign convention.

### `0xB8` FUEL — 4 bytes

| off | field | source | arithmetic |
|---|---|---|---|
| 0:1 | decilitres, u16 | `159` b5 | `b5 * 5` (b5 is 0.5 L/count) |
| 2:3 | range, km, u16 | — | **always `0xFFFF`** — no source |

Gated on `159` b4 as above. Range needs a consumption history the box does not
keep; `0xFFFF` is out of any plausible range and is rejected for free.

---

## 4. Validity, staleness and when to stay silent

Three distinct cases. Get them right and nothing downstream shows a frozen
gauge:

| case | what to do |
|---|---|
| never had the signal (not fitted) | never send that command; its validity bit stays 0 |
| had it, lost it | **keep sending the command**, put the field's sentinel in, **clear its validity bit** |
| have it | send it, set the bit |

Sentinels, one per field type: **`0xFF`** for a +40-biased temperature,
**`0xFFFF`** for speed, rpm and range. Never suppress a whole frame to hide one
bad field — every command here carries several, and the others are still good.

All of them are rejected by the consumer even if it ignores `0xB0`: `0xFFFF`
falls outside every accepted numeric range, and `0xFF` is explicitly decoded as
null by `XyautoProtocol.TEMP_NOT_AVAILABLE`. There is no longer any field that
needs the frame suppressed.

> **Known open issue, not yours to fix.** A consumer that ignores `0xB0` and has
> no timeout will hold the last good value for ever. That half of the fix is
> Dashwheel-side. Your job is to never send a wrong number and to publish the
> validity bits honestly.

---

## 5. The engine-running state machine

Do not use `rpm > 0`, and do not use `000` b0 alone. Measured on
`jeep-can-b-logs/playback.log`, grouping every `002` frame by ignition state:

| `000` b0 | state | rpm observed |
|---|---|---|
| `0x63` | ACC | **all `FFFF`** |
| `0xA9` | **crank** | **0 … 410** |
| `0x85` | run | 0 … 4163 |

Cranking reaches 410 rpm and the run state also shows 0, so neither signal alone
works. Idle on this 3.0 CRD is 577–580, which leaves a clean gap:

```
rpmValid   = (002 b0:b1 != 0xFFFF)
cranking   = (000 b0 == 0xA9)

set   bit1 when  rpmValid && rpm >= 500 && !cranking, for 3 consecutive
                 002 frames (150 ms at the 50 ms cadence)
clear bit1 when  !rpmValid || rpm < 300 || cranking      (immediate, no debounce)
```

Asymmetric on purpose: slow to claim the engine is running, instant to give up.
The 300/500 hysteresis band sits between crank-max 410 and idle-min 577.

---

## 6. Wiring it into the TX scheduler

> **AS BUILT, 2026-10-21 — one correction to this section.** The scheduler DID
> need a change, for `0xB0` only. `kSlotRequest` was **unreachable** as written:
> the stock branch only fires when the round-robin index happens to be standing
> on the requested slot, and on an idle link it never is —
>
> ```
> a sweep that finds nothing changed parks uartTxSlotIdx on the kTxSlots sentinel
>     -> the next pass clamps it back to 0 AND clears slotState[0] on the way
>        -> a request on slot 0 is wiped before it is ever tested
>        -> a request on any other slot is never reached, because the index is
>           reset to 0 on every pass
> ```
>
> which is consistent with this spec's own note that nothing in the image has
> ever set that state. `pendingRequest()` in `app/services/head_unit.cpp` scans
> for an outstanding request and points the index at it *before* the clamp. It
> is **inert in any build that never calls `requestSend()`**, so the stock
> scheduler path is bit-for-bit unchanged, and two tests fail if it is removed.
> Everything else in this section was built exactly as written. FR-112.

**You do not need to touch the framer or the cadence, and the scheduler needs
only the one correction above.** Writing the payload buffers is otherwise
enough; `svc::hu::txSched()` notices a payload differs from its shadow and
sends it. Keep the producer in the 8 ms task alongside `benzPublish()`, which
is the model to copy.

Adding six slots to the Hiworld table touches **five** things, and four of them
fail silently. `head_unit.cpp:95-125` documents them; reproduce each one:

1. `cfg::kTxSlotCount` — `kIsBenz ? 5U : 6U` becomes `kIsBenz ? 5U : 12U`.
2. `svc::hu::hiworld::kSlots` in `hu_dialect.hpp` — 6 → 12. A `static_assert`
   already couples these two; it will catch you if you do one and not the other.
3. `ram::AppRam::uartSlotState` is **immediately followed by `uartTxSlotIdx`**.
   A short array means the scheduler silently corrupts its own index every
   sweep and the tests keep passing. It is sized from `cfg::kTxSlotCount`, so
   step 1 already carries it — but there is a `static_assert` either way, and
   it must not be defeated.
4. New live buffers **and** new shadows, one pair per command, in `ram::AppRam`
   and `ram::UartShadows`. These are standalone objects like `buf73`/`bufF0`,
   **not** windows onto `VehicleState` — do not extend `VehicleState`, its
   relative offsets are protocol.
5. New rows in `hiworld::kTxTable` plus the matching row-geometry
   `static_assert`s next to it. Note the table is in transmit order while
   `UartShadows` is in stock RAM order, so pairing a row with its shadow is a
   lookup, not a glance — that is what the asserts are for.

Budget after this change: **12 slots**, one frame per 48 ms, so 576 ms worst
case per signal and an 8.6 s idle keep-alive. Both comfortable. Do not add
further slots without recomputing — the keep-alive is `slots × 720 ms` and
crosses a 15 s consumer staleness check at 21 slots.

The whole TX block is already gated on `kFlagAccOn`; the new slots inherit that
and need no gate of their own.

**Deadband before you pack.** A payload that changes on noise costs a slot every
48 ms for nothing. Quantise: volts to 0.1 V (so centivolts move in steps of 10),
throttle to 1 %, speed to 1 km/h. Temperatures and fuel are already coarse.

---

## 7. Test vectors

Every one of these is computed from the §1 rule. Use them as unit tests.

| frame | meaning |
|---|---|
| `AA 55 02 B0 01 2F E1` | heartbeat, rev 1: B2/B3/B4/B5/B8 valid, B7 (bit 4) always 0 in this build |
| `AA 55 01 B2 03 B5` | ACC on, engine running, not reverse, brake off |
| `AA 55 06 B3 00 3F 0A 8D 00 2C BA` | 63 km/h, 2701 rpm, load unknown, throttle 44 % |
| `AA 55 05 B4 85 55 5C 05 96 89` | coolant 93 °C, intake 45 °C, fuel 92 %, 14.30 V |
| `AA 55 05 B4 FF 55 5C 05 96 03` | **coolant not available**, everything else still live |
| `AA 55 02 B5 00 FA B0` | outside +25.0 °C |
| `AA 55 02 B5 FF B5 6A` | outside −7.5 °C (sign check) |
| `AA 55 04 B8 02 C6 FF FF 81` | 71.0 L in the tank, range unknown |

Worked check on the first one: `0xAA+0x55+0x02+0xB0+0x01+0x2F = 0x1E1` → `0xE1`.

Also test, against the host unit tests in `tests/host/`:

- `002` all-`FF` ⇒ `0xB3` carries `FF FF FF FF` in `[0..3]` and `0xB0` bit1 is 0.
- `02C` all-`FF` ⇒ `0xB4` **is still emitted**, `[0]` is `0xFF`, `[1..4]` still
  carry live intake/fuel/volts, and `0xB0` bit2 is 0.
- `015` b2 == `0xFF` ⇒ `0xB4[1]` is `0xFF`, **not** `0x27`/−1 °C.
- `159` b4 == 1 ⇒ fuel holds its previous value and `0xB0` bit5 is 0.
- `2C0` b2 != b6 ⇒ throttle holds its previous value.
- crank: `000` b0 == `0xA9` with rpm 410 ⇒ `0xB2` bit1 stays 0.
- run-up: rpm 600 for 3 consecutive frames ⇒ bit1 sets on the third, not the first.
- shutdown: rpm → `FFFF` ⇒ bit1 clears on the first frame.

---

## 8. Out of scope — do not do these

- `0xB1`, `0xB6`, `0xB7`, `0xB9` (§2).
- Anything in `0xBA`–`0xBF`. Lighting, belts and the 4WD/wiper group all have
  confirmed sources, but no consumer yet.
  **Amended 2026-10-21:** odometer and compass now have one, are specified as
  `0xBB` and `0xBD` in **§9**, and are **built**. The rest of the range stays
  out of scope.
- Changing stock `0x72`, `0x12`, `0x32`, `0xA6`, `0x73` or `0xF0`. They work and
  they already carry doors, gear, rpm, reverse, ACC, illumination and steering.
- Touching the `0x400`–`0x43F` handler. It looks like a steering-wheel-button
  decoder and is in fact the OSEK network-management ring; it is correct as-is
  (see the note after FR-53).
- Renaming anything for tidiness. The Ghidra diff depends on the stock names.

---

## 9. Addendum A — odometer and compass (**BUILT** 2026-10-21)

> **This section has been executed.** `0xBB` ODOMETER and `0xBD` COMPASS are in
> the image, the block is **14 slots**, and the imperative voice below now
> describes what the firmware does rather than what it should do. 16 tests in
> `tests/host/test_custom_tx.cpp` section 8, all seven §9.6 vectors among them.
> Requirements FR-122…FR-127.
>
> It was the first deliberate exception to §8's "anything in `0xBA`–`0xBF`" ban,
> which stood only because those signals had no consumer. These two now have
> one.
>
> Built exactly as written — the only judgement call not spelled out here was
> the compass *sentinel split*: §9.3 gives `FF FF FF` as the unavailable frame,
> which means `0xFF` in the sector byte and `0xFFFF` in the heading, so those
> are the two encodings `customPrimeSignals()` uses.

### 9.1 What is added

| cmd | name | bytes | slots | source |
|---|---|---|---|---|
| `0xBB` | ODOMETER | 3 | 1 | `014` b0:b1:b2 |
| `0xBD` | COMPASS | 3 | 1 | `1B4` b0, b1 |

The command numbers are **not** free choices — they are the allocation already
made in `../canbus-to-canbox-signal-map.md` §9, which reserves `0xBA` LIGHTING,
`0xBE` BODY2 and `0xBF` for the groups that are still unbuilt. Do not compact
the numbering.

`0xBB` is called ODOMETER here and TRIP in the signal map. Same command. It was
renamed because it carries no trip counter and never will: a trip needs
persistent state across power cycles, which this box does not keep (the same
argument that parks range-to-empty in `0xB8`). Three bytes, one meaning.

**Neither of these is a duplicate of anything on the wire.** That is the test
`0xB1` and `0xB6` failed. No stock Hiworld command carries distance or heading,
and nothing in `0xB0`–`0xB8` does either.

### 9.2 `0xBB` ODOMETER — 3 bytes

| off | field | source | arithmetic |
|---|---|---|---|
| 0:2 | odometer, 24-bit, 0.1 km/count | `014` b0:b1:b2 | **verbatim three-byte copy** |

- **Verbatim, like coolant, and for the same happy reason.** The car sends
  0.1 km/count big-endian and `FFFFFF` for unavailable; all-ones is already
  this block's unsigned sentinel. Source encoding and wire encoding coincide,
  so there is no arithmetic and no special case — copy the three bytes.
- **Do not convert to whole km.** `benzTrip()` divides by 10 because the Benz
  plugin's field is km; this block has no such constraint, and throwing away
  the last digit costs a consumer the ability to measure a 100 m leg.
- Full scale is `0xFFFFFE` = 1 677 721.4 km. No rollover concern on any vehicle.
- **`FFFFFF` is unavailable, not a reading.** Write the sentinel through and
  clear the validity bit — the §4 "had it, lost it" rule. Do **not** hold the
  previous value the way `benzTrip()` does; that behaviour exists only because
  the Benz plugin cannot render a blanked field, and this block can.
- Deadband: none needed. One count is 100 m, so the payload changes every
  ~2.8 s at 130 km/h and less often below that. It is the cheapest slot in the
  table.

`014` is **already dispatched** — `can_decode.cpp` routes it to
`customAmbient()` for `0xB5`. The odometer is a second field of a frame that is
already decoded, so this costs no new dispatch case and no new CAN id constant.

### 9.3 `0xBD` COMPASS — 3 bytes

| off | field | source | arithmetic |
|---|---|---|---|
| 0 | sector, 8-way enum | `1B4` b0 | **verbatim byte**, `0`=N, `1`=NE, `2`=E, `3`=SE, `4`=S, `5`=SW, `6`=W, `7`=NW |
| 1:2 | magnetic heading, whole degrees, u16 | `1B4` b1 | `b1 * 2`, range 0…358 |

- **Heading is converted, sector is not.** `b1` is 2°/count, which is not a
  physical unit, so the decoder scales it exactly like speed and volts. `b0` is
  already an enum and has no unit to convert to. Resolution stays 2° — do not
  send tenths of a degree and invent precision the sender does not have.
- **Send both bytes even though `b0` is derivable from `b1`.** `b0` equals
  `round(b1*2/45) mod 8` in 99.46 % of 2 953 frames, and the 0.54 % that differ
  are all boundary-quantisation artefacts. Forwarding the car's own sector byte
  means the consumer's compass rose agrees with the cluster's at a boundary
  instead of flickering against a formula re-derived downstream. One byte, zero
  extra slots.
- **Do NOT cross-check `b0` against the formula.** This is not the `2C0` b2/b6
  mirror — that is a true redundant copy where a mismatch means a bad sample.
  Here a mismatch is *expected* at sector boundaries, and discarding on
  mismatch would throw away one sample in two hundred for nothing.
- **Plausibility gate, both bytes together.** Accept the sample only when
  `b0 <= 7 && b1 <= 179`. On anything else: hold both previous values and clear
  the validity bit. Mask nothing — a `b0` of 9 means the byte no longer means
  what this table says, and silently truncating it to 1 would publish "NE".
- Deadband: none needed, and this is measured, not assumed. Across all 196
  consecutive stationary (≤2 km/h) sample pairs the 500 ms heading delta was
  exactly 0.0° — mean, median, max and stdev all zero. There is no jitter to
  filter. Moving, the mean |delta| is 1.9° per 500 ms, so the slot is claimed at
  most twice a second and only while actually turning.
- `0` is a **real heading** (due north), not a blank. The sentinel is `0xFFFF`.

**`1B4` needs a new dispatch case**, unlike `014`:

- add `inline constexpr std::uint32_t kJeepCompass = 0x1B4U;` to the jeep-only
  group in `can_decode.hpp`;
- add `case canid::kJeepCompass: if constexpr (!cfg::kIsBenz) { customCompass(d); } break;`
  alongside `kJeepFuel` / `kJeepParkBrake` / `kJeepThrottle`;
- the id currently falls into `default:` → `decodeSwcService()`, which returns
  immediately because `(0x1B4 & 0xFC0) != 0x400`. Intercepting it costs no stock
  behaviour — **the same no-op argument already written out for the other three
  jeep-only ids**, and it is the reason this is safe rather than a divergence;
- the CAN filter is accept-all (`can_bus.cpp`: "ALL real filtering is done in
  software"), so the frame already reaches the dispatcher. Nothing in the
  bring-up path changes.

> **A consumer trap, and it is not fixable in the firmware.** `1B4` has **no
> unavailable sentinel and does not stop at key-off**: in `playback.log` the
> frame keeps transmitting at its normal 500 ms cadence with an unchanged valid
> heading (`06 85 07 00 00 00`, W/266°) straight through the `000` ignition-off
> transition and for ~1000 further lines until the capture ends. So `0xBD`'s
> validity bit stays 1 across key-off, *correctly* — the compass really is still
> reporting. **Key-off is `0xB2` bit0, never the compass.** The whole TX block is
> gated on `kFlagAccOn` so the box stops sending anyway, but a consumer must not
> read a live `0xBD` as "the car is awake".

### 9.4 Validity bits — this fills the mask

Two new bits in the `0xB0` byte-1 mask, continuing the existing allocation:

| bit | cmd | set when |
|---|---|---|
| 6 | `0xBB` | `014` b0:b1:b2 != `FFFFFF` |
| 7 | `0xBD` | a `1B4` sample passed the §9.3 plausibility gate |

```
kValidBB = 0x40
kValidBD = 0x80
```

**The mask is now full.** §3 said bits 6–7 were reserved and sent as 0; this
spends both. Say so out loud in `ram_map.hpp` next to the bit block, because the
next person adding a command hits a wall that is not visible from the code:

> The growth path is a **second mask byte**, appended as `0xB0` payload byte 2,
> with `0xBA` / `0xBC` / `0xBE` / `0xBF` taking its bits 0–3. That is a **length
> change to a shipped command**, so by §7 of the contract the firmware and
> `XyautoProtocol.kt` move together in one release. Do not do it speculatively,
> and do not renumber bits 0–7 when you do — they are wire format.

Note bit 6 goes to `0xBB` and bit 7 to `0xBD` even though `0xBA`, `0xBC` and
`0xBE` sit between them numerically. The rule is **allocation order, not command
order**; it already was, since bit 5 is `0xB8` and skips the unbuilt `0xB6`.

### 9.5 Wiring — the same five-point checklist as §6

Nothing new in kind. Reproduce every step; four of the five fail silently.

1. `cfg::kTxSlotCount` (`app_config.hpp:234`) — `kIsBenz ? 5U : 12U` becomes
   `kIsBenz ? 5U : 14U`.
2. `svc::hu::hiworld::kSlots` (`hu_dialect.hpp:91`) — 12 → 14. The
   `static_assert` in `head_unit.cpp:125` couples the two.
3. `ram::AppRam::uartSlotState` is sized from `cfg::kTxSlotCount`, so step 1
   carries it. It is still **immediately followed by `uartTxSlotIdx`** — a short
   array makes the scheduler corrupt its own index every sweep while the tests
   keep passing. Do not defeat the assert.
4. New live buffers and new shadows, one pair per command:
   `ram::CustomFrames::bB[3]` / `bD[3]`, and `ram::UartShadows::msgBB[3]` /
   `msgBD[3]`. **Append** the shadows, as the `0xB0`–`0xB8` six already are, so
   the stock six keep the RAM order a reader diffs against Ghidra.
5. Two rows in `hiworld::kTxTable` with their accessor pairs (`bufBB` /
   `shadowBB`, `bufBD` / `shadowBD`) and the four new row-geometry
   `static_assert`s at `[12]` and `[13]` — one against the shadow, one against
   the buffer. `noReservedCommands()` and `txFramesFit()` are folds over
   `kSlots` and need no edit.

Then the block's own files:

6. `CustomSignals` gains `odometerDeciKm` (u32), `headingDeg` (u16) and
   `compassSector` (u8); `ram_map.hpp` gains `kCustomUnknown24 = 0x00FFFFFFU`
   alongside the other sentinels.
7. `customPrimeSignals()` must prime all three. A zeroed `CustomSignals`
   publishes "0.0 km, due north, 0°" as though the car had said it — which is
   exactly the trap priming exists to close, and this time the fabricated values
   look *more* plausible than `-40 degC` does, not less.
8. `customOdometer()` and `customCompass()` in `custom_decode.cpp`, declared in
   `custom_decode.hpp`. **Keep them as two functions, and keep
   `customOdometer()` separate from `customAmbient()`** even though both read
   `014` — one function, one validity bit is the rule that has kept this block's
   ownership clean, and `0xB5` and `0xBB` have different gates on the same frame.
9. `logic::kCmdOdometer = 0xBBU` and `logic::kCmdCompass = 0xBDU` in
   `custom_tx.hpp`; `customPublish()` packs both, with a new `put24()` helper
   next to `put16()`.

**Budget after this change: 14 slots.** One frame per 48 ms gives 672 ms worst
case per signal, and the idle keep-alive is `14 x 720 ms` = **10.1 s**, still
inside Dashwheel's 15 s staleness check with six slots of headroom — 20 slots is
14.4 s and 21 crosses. The two new slots are also among the quietest in the
table (§9.2, §9.3), so the realistic effect on the `0xB3`/`0xB4` ~10 Hz
alternation is nil.

### 9.6 Test vectors

Computed from the §1 rule; use them as unit tests.

| frame | meaning |
|---|---|
| `AA 55 03 BB 12 D6 87 2C` | odometer 123 456.7 km |
| `AA 55 03 BB FF FF FF BA` | odometer not available |
| `AA 55 03 BD 06 01 0A D0` | West, 266° — the real `1B4` payload `06 85 07 00 00 00` |
| `AA 55 03 BD 00 00 00 BF` | **North, 0° — a real reading, not a blank** |
| `AA 55 03 BD 00 01 66 26` | North, 358° — sector and heading legitimately look inconsistent at the wrap |
| `AA 55 03 BD FF FF FF BC` | compass not available (before the first valid sample) |
| `AA 55 02 B0 01 EF A1` | heartbeat with `0xBB` and `0xBD` also valid (`0x2F \| 0x40 \| 0x80`) |

Worked check on the first: `0x03 + 0xBB + 0x12 + 0xD6 + 0x87 - 1 = 0x22C` →
`0x2C`. And `0x12D687` = 1 234 567 counts = 123 456.7 km.

Also test, against the host unit tests in `tests/host/`:

- `014` b0:b1:b2 == `FF FF FF` ⇒ `0xBB` carries `FF FF FF`, `0xB0` bit6 is 0,
  **and `0xB5` is unaffected** — ambient has its own gate on the same frame.
- `014` odometer valid but b4 ≥ `0x80` ⇒ `0xBB` is live with bit6 set while
  `0xB5` holds and bit3 clears. The converse of the above; both directions
  together prove the two gates are independent.
- `1B4` b1 == `0xFF` ⇒ heading and sector both hold their previous values and
  `0xB0` bit7 is 0 — **not** `0x01FE` = 510°.
- `1B4` b0 == `0x09` ⇒ sample rejected whole, nothing masked to `0x01`/NE.
- `1B4` b0 == 7, b1 == 146 ⇒ sample **accepted**, sector forwarded as 7, no
  discard. This is the documented boundary disagreement: 146 is 292°, half a
  degree shy of the 292.5° NW boundary, so the formula says 6/W and the car
  says 7/NW. The car wins, and nothing is thrown away.
- cold boot with no `014` / `1B4` yet ⇒ `0xBB` is `FF FF FF`, `0xBD` is
  `FF FF FF`, bits 6 and 7 both 0.
- `kTxSlotCount` == 14 ⇒ `uartSlotState` is 14 long and `uartTxSlotIdx` is still
  intact after a full sweep.

### 9.7 Still out of scope after this

- `0xBA` LIGHTING, `0xBE` BODY2, `0xBF`. Confirmed sources, no consumer.
- Trip distance, average speed, consumption. All need persistent state the box
  does not keep.
- True north. `1B4` b1 is **magnetic** heading and the variance correction is
  not on this bus. `1B4` b2 is constant `07` in 2 953/2 953 frames and is
  *likely* a variance-zone/status byte — only its constancy is established, so
  it is neither forwarded nor acted on.
- Yaw rate. A circular derivative of a 2°/count heading at 2 Hz is a coarse
  proxy at best; sustained turns run 18–26°/s with a genuine 192°/s peak, which
  a 500 ms sample cannot represent honestly. Do not publish one.
