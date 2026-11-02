# Canbox ACC 12 V output — how the firmware controls it

Applies to the rebuilt Hiworld JPF2 app (Nations N32L406) in this repo, with `cfg::kAccFollowsRap` = true
(the default; spec and rationale in [ACC-RAP-CHANGE.md](ACC-RAP-CHANGE.md)).

## Quick summary — CAN bus view
- **ACC ON ⇐ (0x000 b0 ∈ {63 ACC, 85 RUN, A9 CRANK}) OR (0x006 b1 bit 1 = RAP)**, each only while its
  frame is fresh (seen within 1000 ms) → ACC 12 V (PB5) high within ~100 ms.
- **ACC OFF ⇐ ignition 01/00 AND RAP bit clear**, or both frames stale → PB5 low within ~100 ms.
- Ignition test is still `(b0 & 0xE0) > 0x10`; RAP is `d[1] & 0x02` (bit 5 `0x20` is ignored).
- RAP holds ACC through crank (RAP drops, `A9` keeps it) and after key-off until the body module
  ends RAP: a door opens, or 600 s after OFF. A bus that merely wakes up (`000=00`, `006=00`) never turns it on.
- **Bus silent ≥ 1 s** → both sources stale → ACC off (recomputed every 8 ms), well before the 10 s link-wedged reset.
- Compile switch `cfg::kAccFollowsRap` (`app/include/app_config.hpp`, default **true**). Set to `false`
  to get the stock rule back: ACC = ignition ladder alone, no 0x006 decode, no staleness. Applies to both
  protocols (the Benz build decodes the same Jeep 0x000/0x006). The FCA 0x20B profile is unaffected
  either way: no RAP, ignition never aged out.
- Other ACC source: vendor frame **0x7FF** `7F F7 a a b b 3A A3` → ACC = a bit 0 (not used by the car);
  the next 0x000/0x006 frame or the next 8 ms recompute overwrites it.
- Box TX reacting to ACC: **0x09F** b0 = 01 on / 07 off; **0x18C** b0 bit 4 set while off;
  all periodic TX stops 3.2 s after off; ~10 s after off the box sleeps and wakes on any CAN activity.
  With RAP all of this now follows RAP, i.e. lasts up to 10 min after key-off.

## Hardware
- ACC 12 V output to the radio is driven by MCU pin **PB5** (push-pull, active high).
- Assumed (not traced on this pin): a 12 V high-side stage like the traced ILL output
  (PC15 → MMBT3904 → P-FET → +12 V, 47k bleeder to GND), so PB5 high = +12 V, low = 0 V.
- Set in `svc::io::writeStatusLines()` (`app/services/board_io.cpp:125`), called every **8 ms**
  from the super loop (`app/logic/main_loop.cpp:196`). PB5 = `VehicleState::kFlagAccOn` (flags bit 0).

## Where the ACC flag comes from
- Jeep WK CAN (83.333 kbit/s), two frames, both every 100 ms
  (`docs/ACC-RAP-CHANGE.md` has the capture the rule is based on):
  - id **0x000** byte 0, the ignition ladder: `00` wake/uninit, `01` OFF, `63` ACC, `85` RUN, `A9` CRANK.
  - id **0x006** byte 1 bit 1 (`0x02`), Retained Accessory Power from the body module: set at ACC,
    clear during crank, held after key-off, cleared by a door opening or 600 s after OFF.
- Rule (`app/logic/can_decode_shared.hpp`, `recomputeAcc()`):
  `accOn = (ignOn && ignFresh) || (rapOn && rapFresh)`, `fresh` = frame seen within `cfg::kAccStaleMs`
  (1000 ms, on `ram::app().sched.counter`). State is `accIgnOn/accRapOn/accIgnSeenMs/accRapSeenMs` in `ram::AppRam`.
- Recomputed on every 0x000 (`decodeIgnition`), every 0x006 with DLC ≥ 2 (`decodeRap`, dispatched from
  `decodeProfileJeep()`), and once per pass at the top of `dispatchGate()` so staleness turns ACC off.
- Why OR: RAP drops during crank and `A9` covers it; `63` precedes the RAP bit by ~0.2 s so ACC comes on as
  fast as before; and if 0x006 is ever missing the rule degrades to the old ignition-only behaviour.
- `kFlagIgnRaw` (flags bit 4, d[0] bit 0 of 0x000) is unchanged. UART custom slot `0xB2` bit 0 stays on
  the ignition ladder alone (`customIgnition`), so the Android side still sees the true key position during RAP.
- `cfg::kAccFollowsRap = false` restores the stock rule: `kFlagAccOn = (b0 & 0xE0) > 0x10` written directly by
  each 0x000 frame, 0x006 ignored, no staleness.
- The FCA 125k profile uses id 0x20B instead (not this car). It goes through the same recompute but has no RAP
  and its ignition is never aged out, so it follows each 0x20B frame exactly as before.
- Another way to set it, never seen in normal driving: vendor frame CAN **0x7FF**
  `7F F7 x x y y 3A A3` (d2==d3, d4==d5) → ACC = d[2] bit 0 (`decodeVendor7ffCommon`). With the switch on
  the override lasts only until the next recompute (≤ 8 ms); with it off, until the next 0x000 frame.

## When PB5 / ACC 12 V is ON
- After the box has booted AND finished the bus search (≥3 CAN frames received), on the first
  0x000 frame with ACC/RUN/CRANK or the first 0x006 frame with the RAP bit.

## When PB5 / ACC 12 V is OFF
1. Key OFF with RAP ended: ignition 01/00 and RAP bit clear (door opened, or 600 s after OFF) → low within
   ~100 ms. Key OFF with RAP still set → stays on (the factory-radio behaviour).
2. Frames stale: neither 0x000 nor 0x006 seen for 1 s → low (the bus went to sleep).
3. Box reset / power-up: `statusLinesInit()` drives PB5 low (`board_io.cpp:78`). It stays low
   through the bootloader (~0.67 s IAP window), boot and the bus search, until a fresh 0x000 / 0x006 says on.
   Resets happen on:
   - bus silent for 10 s while ACC is still on ("link wedged", `can_link.cpp:196`, WWDG forced reset).
     With the staleness rule ACC is already off after 1 s, so this no longer happens on a sleeping bus;
   - watchdog bite / fatal error.
   → **A canbox reset while driving drops ACC to the radio for ≥ ~1 s.**
4. Before the bus search finishes (phase 1), the decoder doesn't run → PB5 stays low.

## What happens after ACC goes off (does NOT touch PB5)
- t=0 (ACC off, i.e. RAP ended, not key-off): the 0x09F TX frame byte 0 = 07 (01 when on); the 0x18C frame gets bit 4 ("no ignition");
  the bus state machine goes from running to lost (10 s grace).
- t=3.2 s: all periodic CAN TX to the car stops (`periodic_tx.cpp:598`, 400 × 8 ms).
- t≈10 s with ACC still off: bus down → deep sleep (STOP); wake only on CAN activity (PB8 EXTI8)
  → WWDG reset → reboot. `statusLinesShutdown()` leaves PB5 alone, but it is already low.
- An NM frame on 0x400..0x43F with d[0]=FD, d[2]=FF while ACC is off → bus marked down at once and
  the TX queue flushed.
- After ACC comes back on: the 0x18C bit 4 clears and the 0x3D0 amp frame re-arms after 6.4 s (800 × 8 ms).

## Also reported (not the power line)
- UART custom slot `0xB2` bit 0 = ACC (jeep build), `app/logic/custom_decode.cpp`.
- The radio's Android side gets ACC from its own host MCU as `xy.android.acc.on/off` broadcasts,
  not from canbox UART.
