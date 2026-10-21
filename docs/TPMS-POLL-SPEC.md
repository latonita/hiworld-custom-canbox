# `0xB7` TYRE — polling the WCM for TPMS. Firmware implementation spec

> **This is a work order, not an analysis.** Everything here is decided. Build
> exactly this.
>
> Target: `app/` in this repository, the **Hiworld/Jeep build only**
> (`--protocol jeep`, `cfg::kIsBenz == false`). The Benz build is unchanged.
>
> This unparks `0xB7`, which `CUSTOM-TX-SPEC.md` §2 reserved with "TPMS is not
> broadcast on this car's CAN-B; it needs a request/response exchange that has
> not been reverse-engineered." It has now been reverse-engineered — on the car,
> by a sibling project — so the only reason for the reservation is gone.
>
> Scope decided 2026-10-21, by the owner: non-stock bus traffic is acceptable
> **provided it stays minimal**; the poll runs **every 30 s**; the corner order
> ships as documented and is rearranged head-unit-side if a deflation test ever
> contradicts it; the **spare is deferred**.

---

## 1. The exchange

Plain **KWP2000 over ISO-TP**, 11-bit ids, single frame in each direction.
No diagnostic session, no security access, no tester-present keepalive: the
request goes out and the module answers.

```
TX  0x600  DLC 3   02 21 3B
                   │  │  └ local identifier 0x3B
                   │  └─── service 0x21, readDataByLocalIdentifier
                   └────── ISO-TP PCI: single frame, 2 PDU bytes

RX  0x500  DLC 7+  06 61 3B 21 20 21 1E [xx]
                   │  │  │  │  │  │  │   └ junk past the declared length
                   │  │  │  │  │  │  └──── tyre 4  LEFT  REAR   0x1E = 30 psi
                   │  │  │  │  │  └─────── tyre 3  RIGHT REAR   0x21 = 33 psi
                   │  │  │  │  └────────── tyre 2  RIGHT FRONT  0x20 = 32 psi
                   │  │  │  └───────────── tyre 1  LEFT  FRONT  0x21 = 33 psi
                   │  │  └──────────────── local identifier echo
                   │  └─────────────────── 0x21 + 0x40 = positive response
                   └────────────────────── ISO-TP PCI: single frame, 6 PDU bytes
```

Each wheel is **one byte of plain psi, 1 psi per count, no offset and no
scaling**. Note the order is **LF, RF, RR, LR** — the rears are *not* in the
order most sources assume.

### Three traps, each already paid for by somebody

- **The PDU is `21 3B`, not `21 21 3B`.** A request builder that re-emits the
  service byte for a service-qualified identifier sends `21 21 3B` and **every
  module answers nothing**. That cost the sibling project a debugging session;
  do not re-earn it.
- **Trust the PCI nibble, not the DLC.** The byte after the declared length is
  whatever was in the responder's buffer last. It is not data and it is not
  zero. Read exactly the 6 bytes the `06` promises.
- **Do not implement service `0x22`.** The UDS form (`03 22 A0 20`) is a forum
  suggestion and is *actively refuted* on this car — it answers `7F 22 80`.

### Evidence

As good as it gets without the car in the room:

- A 7-minute live driving log: **47 requests, 47 answers, zero failures, zero
  negative responses**, four distinct payloads as pressures changed.
- Cross-checked three ways on the discovery sweep: the owner's expected
  2.1–2.3 bar, one genuinely soft corner at 30 psi, and a stored **`C0077`
  Low Tire Pressure** DTC in the same module.
- An independent CLI implementation decodes the identical frame identically.

Latency is **under 200 ms** — every one of those 47 replies landed inside a
200 ms listen window.

**What is NOT proven:** the corner order is triangulated from three sources but
no deflation test has ever been done, and a fourth source swaps the rears. It
ships as written above, in one named constant, so one test fixes it in one line.

---

## 2. The blocker — `0x500` already belongs to the stock decoder

`can_decode.hpp:103` has `kJeepVendorEcho = 0x500U`, and `jeepEcho500()` copies
`d[0]:d[1]`, zero-pads to eight and **transmits the result on `0x516`**. The WCM
replies to our poll on `0x500`. So, naively wired, every poll would make the box
inject `06 61 00 00 00 00 00 00` onto the car's bus — on an identifier that is
the *radio's* KWP response id.

This has never happened because **`0x500` appears nowhere in the 221 853 frames
of `testlog/drive.log`**: the stock echo is dead code on this car. Polling wakes
it up.

**The rule, decided by the owner 2026-10-21: in a jeep build the `0x500` echo
is switched OFF and the identifier belongs to TPMS.**

```
0x500 arrives
  ├─ benz build -> jeepEcho500(), exactly as stock. That build never polls,
  │                so there is no collision and no reason to diverge.
  └─ jeep build -> customTpmsReply(); the echo is disabled
                   ├─ 06 61 3B ... -> decode the four pressures
                   ├─ 7F ...       -> negative response, count a failure
                   └─ anything else-> ignored
```

The echo is **disabled, not deleted**. `jeepEcho500()` stays in the file with
its stock-address annotation, reached through a named constant:

```cpp
/* Stock mirrors 0x500 -> 0x516. Switched off in a jeep build because 0x500 is
 * now the WCM's KWP response id ... Set this true to restore stock behaviour. */
constexpr bool kVendorEcho500Enabled = false;
```

Flipping one word restores it, the Ghidra diff still finds the function, and the
reverse-engineering record is intact — which is what this repo keeps comments
for.

**What this costs: nothing observable.** `0x500` appears in **zero** of the
221 853 frames of `testlog/drive.log`, so the echo has never fired on this car
and switching it off removes no behaviour anyone has seen. It is a documented
deviation all the same, because it is the first time this rebuild turns a stock
path off rather than adding beside it.

---

## 3. Polling

### Where it lives

A divider inside `logic::periodicTxScheduler()`, the pattern `custom_tx.cpp`
already uses for the 1 Hz heartbeat. Put it there and it inherits, for free:

- the **ACC hold gate** — polling stops 3.2 s after key-off, which is what stock
  behaviour demands of anything that talks to this bus;
- the **profile gate** — an unrecognised profile transmits nothing;
- **FR-14**, the phase-1 silence rule: nothing is sent before the bus answers.

### The bus-presence gate — THIS ONE IS NOT OPTIONAL

Poll **only** when `ram::AppRam::canBusStatus == cfg::kBusRunning`.

`can_bus.cpp` configures the controller with `abom = 0` (no automatic bus-off
recovery) and `nart = 0` (hardware retries a frame forever). On the car that is
harmless: some other node always ACKs. **On a bench with no second node, a
transmitted frame is never acknowledged, the error counter climbs, and the
controller goes bus-off permanently until reset.**

The stock firmware never met this because it only ever transmits in reply to
traffic that proves somebody is listening. A poll is the first frame this box
would send unprompted, so it has to check for itself.

### Cadence

| | | |
|---|---|---|
| poll period | **30 s** | 3750 passes of the 8 ms task |
| reply deadline | **500 ms** | 63 passes; then give up, no immediate retry |
| failures before the data goes stale | **4** (120 s) | then clear validity and blank the fields |

A `uint8_t` divider saturates at 2.04 s on the 8 ms pass. **Use `uint16_t`.**

No retry inside a poll: a timeout simply waits for the next 30 s tick. The
sibling project's measured success rate was 47/47, so a retry would be solving a
problem nobody has observed, at the cost of more bus traffic — which the owner's
"minimal" constraint rules out.

---

## 4. The `0xB7` payload

The Dashwheel contract already specifies this; it is not ours to invent.

| off | field | notes |
|---|---|---|
| 0 | position | `0`=LF `1`=RF `2`=RR `3`=LR, `4`=spare (**not sent**, deferred) |
| 1:2 | pressure, u16 **kPa** | `0xFFFF` = no value |
| 3 | temperature, °C+40 | **always `0xFF`** — see below |
| 4 | flags | bit0 battery low, bit1 leak, bit2 no signal |

**Units.** The WCM gives psi; the contract wants kPa.

```
kPa = round(psi * 6.894759)   implemented as   (psi * 68948 + 5000) / 10000
```
33 psi → 228 kPa. The widest intermediate is `255 * 68948 = 17 581 740`,
comfortably inside 32 bits.

**Temperature is always `0xFF`.** `21 3B` carries no temperature at all. The
per-sensor identifiers `21 20`..`21 24` do have a temperature-shaped byte, but
its `raw − 40` decode is explicitly a *hypothesis* and reading them costs five
more requests. `0xFF` is already this block's reserved
`TEMP_NOT_AVAILABLE` code, so the field says "unknown" honestly and costs
nothing. This also retires the concern in `../canbus-to-canbox-signal-map.md` §7
that a temperature sentinel would be needed in the flags byte: it is not, because
the reserved code now exists.

**Flags.** Nothing in `21 3B` reports battery, leak or signal state — those live
in identifiers this spec does not read. Send `0x00` while a reading is live, and
set **bit 2 (no signal)** on every position once the data has gone stale.

### Rotation — one slot, not five

Five positions must **not** become five TX slots: slot count is what sets every
other signal's update interval. One slot carries one position at a time and
**advances every 5 s** (625 passes). Four wheels, so a full set takes 20 s.

The round-robin notices the changed payload and sends it; no explicit request is
needed. 5 s between sends keeps the *command* comfortably inside Dashwheel's 15 s
TPMS staleness check even though the *poll* is six times slower.

### Before the first successful poll, and after it goes stale

Pressure `0xFFFF`, temperature `0xFF`, flags bit 2 set, and `0xB0` **bit 4
clear**. The frame still goes out — the scheduler's keep-alive cannot be told to
skip a slot, which `CUSTOM-TX-SPEC.md` §0 already establishes — but every field
says "no value" and the validity bit says so too.

`0xB0` bit 4 (`kValidB7`) was reserved for exactly this and needs no mask
extension.

---

## 5. Wiring — the same checklist as `CUSTOM-TX-SPEC.md` §6

1. `cfg::kTxSlotCount` — `kIsBenz ? 5U : 14U` becomes `kIsBenz ? 5U : 15U`.
2. `svc::hu::hiworld::kSlots` — 14 → 15. A `static_assert` couples the two.
3. `ram::AppRam::uartSlotState` is sized from `cfg::kTxSlotCount`, so step 1
   carries it. The assert stays; do not defeat it.
4. `ram::CustomFrames::b7[5]` and `ram::UartShadows::msgB7[5]`.
5. A row in `hiworld::kTxTable` in **command order**, between `0xB5` and `0xB8`,
   with its accessor pair and the two row-geometry `static_assert`s. Inserting
   rather than appending renumbers the asserts below it — that is mechanical and
   the asserts catch any slip.
6. **A DLC-vs-buffer assert for the request.** Unlike the head-unit table, the
   CAN side has *no* coupling between the DLC passed to `txEnqueue()` and the
   size of the buffer handed with it — `txFrameF0` is `[5]` sent with DLC 4.
   `txEnqueue` copies `min(dlc, 8)` bytes from a pointer whose extent it cannot
   know, so an over-long DLC silently reads past the buffer and puts whatever
   follows it in `.bss` onto the vehicle bus. Assert it for the new frame.
7. `canid::kWcmTpmsRequest = 0x600U` in the jeep-only group of `can_decode.hpp`.
   The response id needs no new constant: it is the existing `kJeepVendorEcho`,
   whose name is now misleading in a jeep build and is kept anyway, because the
   Ghidra diff depends on the stock names (`CUSTOM-TX-SPEC.md` §8).
8. No CAN filter change. The hardware accepts every identifier and all real
   filtering is in software, so `0x500` already reaches the dispatcher.

---

## 6. Test vectors

Computed from the `CUSTOM-TX-SPEC.md` §1 framing rule; use them as unit tests.

These are the four corners of the real captured reply `06 61 3B 21 20 21 1E`.

| frame | meaning |
|---|---|
| `AA 55 05 B7 00 00 E4 FF 00 9E` | LF, 228 kPa (33 psi), no temperature, no flags |
| `AA 55 05 B7 01 00 DD FF 00 98` | RF, 221 kPa (32 psi) |
| `AA 55 05 B7 02 00 E4 FF 00 A0` | RR, 228 kPa (33 psi) |
| `AA 55 05 B7 03 00 CF FF 00 8C` | LR, 207 kPa (30 psi) |
| `AA 55 05 B7 00 FF FF FF 04 BC` | LF, no reading: sentinel pressure, no-signal flag |

Worked check on the first: `0x05 + 0xB7 + 0x00 + 0x00 + 0xE4 + 0xFF + 0x00 - 1
= 0x29E` → `0x9E`. And `33 psi x 6.894759 = 227.5` → `228` = `0x00E4`.

Also test, against the host unit tests in `tests/host/`:

- the request frame is `0x600`, DLC 3, bytes `02 21 3B` — **not** `21 21 3B`;
- a reply of `06 61 3B 21 20 21 1E` with a poll outstanding ⇒ the four pressures
  land as 228/221/228/207 kPa and `0xB0` bit 4 sets;
- the same reply with **no** poll outstanding ⇒ ignored, and **nothing is
  transmitted on `0x516`**;
- a `0x500` frame that is not a `61 3B` response ⇒ ignored, nothing transmitted;
- a `7F` negative response ⇒ counted as a failure, nothing transmitted;
- four consecutive timeouts ⇒ pressures go to `0xFFFF`, flags bit 2 sets and
  `0xB0` bit 4 clears;
- `canBusStatus != kBusRunning` ⇒ **no request is transmitted at all**;
- ACC off for 3.2 s ⇒ no request is transmitted;
- the position byte advances 0→1→2→3→0 and does not reach 4.

---

## 7. Out of scope

- **The spare** (`21 24`, position 4). A second request with a different
  response shape. Deferred by the owner; the position value stays allocated.
- **Per-sensor identifiers** `21 20`..`21 23`. They add a temperature whose
  decode is unproven and status bits this spec does not need, for four extra
  requests per cycle — which the "keep it minimal" constraint rules out.
- **Per-tyre fault identifiers** `21 25`..`21 29`. They carry real sensor-fault
  and low-pressure bits that would fill the flags byte properly. Worth revisiting
  once the pressure path is proven on the bench; not now.
- **DTC reads**, security-gated identifiers, and anything needing a diagnostic
  session. Reads that need `27 01`/`27 02` are out; so is `10 92`, which the
  vendor database suggests as a fallback and which this car has never needed.
- **Retrying inside a poll.** See §3.
