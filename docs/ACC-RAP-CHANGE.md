# ACC output → follow Chrysler RAP (change spec)

Status: **implemented**. Behaviour: [ACC-OUTPUT.md](ACC-OUTPUT.md).

Deviations from the spec as written:
- Compile switch `cfg::kAccFollowsRap` (default true, `app/include/app_config.hpp`); false restores the stock
  rule exactly. It covers both protocols: the Benz build decodes the same 0x000 through the same
  `decodeProfileJeep()`, so it gets the same change rather than diverging.
- New state is `accIgnOn/accRapOn/accIgnSeenMs/accRapSeenMs` in `ram::AppRam` (not `VehicleState`).
- FCA profile: its ignition is never aged out (its 0x20B cadence is unknown), so it is bit-for-bit as before.
- `can_bus.hpp` gained `rxDlc()` for the `len >= 2` check on 0x006.
- The static_assert is `kAccStaleMs * 4 < kBusAliveReloadJeep * 100` (can_decode_shared.hpp) plus a test
  twin; the 10 s wedge is that counter reload (100 ticks x 100 ms), not a named ms constant.
- `0x7FF` override: accepted as overwritten by the next recompute.
- Tests added beyond the 9 listed: `acc_ignition_goes_stale_too`, `rap_frame_shorter_than_two_bytes_is_ignored`.
  Existing tests that set the flag by hand and run `dispatchGate()` now also seed `accIgnOn` (helpers
  `forceAcc` / `setAcc` / `runDebounceReprime`). Not yet verified in the car (see plan below).

## Goal
ACC 12 V (PB5) behaves like the factory radio's power:
- on at ACC/RUN, **stays on through crank**;
- stays on after key-off (Retained Accessory Power);
- off when the body module ends RAP: a door opens, or 600 s after OFF;
- never comes on just because the bus woke up.

## CAN facts (Jeep WH/WK CAN-B, 83.3k)
Source: `jeep-can-b-logs`, reference log
`rap test - crank then 10 min timeout - 2026-10-25_101103.log`.

| Signal | Meaning |
|---|---|
| `0x000 b0` (100 ms) | `00` no key/wake, `01` OFF, `63` ACC, `85` RUN, `A9` CRANK |
| `0x006 b1` bit1 `0x02` (100 ms) | **RAP / accessory power** from the body module: set at ACC, **cleared during crank**, held after key-off, cleared on door open or exactly 600 s after `0x000`→`01` |
| `0x006 b1` bit5 `0x20` | unknown; ignore |
| bus | goes silent ~2.5 s after RAP clears on timeout |

Observed sequence: `006 b1` = `00` → (ACC) `20`,`22` → (crank) `20` → (run) `22` → (key-off) `22` … → (door / 600 s) `02`,`00`.

## New rule

```
ign_on  = 0x000 b0 ∈ {63, 85, A9}        // existing test (b0 & 0xE0) > 0x10
rap_on  = 0x006 b1 & 0x02
accOn   = (ign_on && ign_fresh) || (rap_on && rap_fresh)
fresh   = that frame seen within the last 1000 ms
```

Why the OR instead of RAP alone:
- **Crank:** RAP bit drops during crank; `0x000 = A9` keeps ACC on. No race between
  the two frames in the same 100 ms cycle.
- **ACC on:** `0x000 = 63` arrives ~0.2 s before RAP bit1 is set, so the output comes on as fast as today.
- **Fallback:** if `0x006` is ever missing, behaviour degrades to today's rule instead of never switching on.

| Situation | `0x000` | `006 b1` | PB5 today | PB5 new |
|---|---|---|---|---|
| Unlock / door / key in | `00`/`01` | `00` | off | off |
| ACC, RUN | `63`/`85` | `22` | on | on |
| Crank | `A9` | `20` | on | **on** (via ign) |
| Key OFF / key pulled | `01`/`00` | `22` | **off** | **on** (RAP) |
| Door opened during RAP | `00` | `00` or `20` | off | **off** |
| 600 s after OFF | `00` | `00` | off | **off**, bus sleeps 2.5 s later |
| Door after timeout (bus wakes) | `00` | `00` | off | off |
| Bus silent | — | — | off after reset | off after 1 s (stale) |

## Code changes

1. **CAN id** — `app/logic/can_decode.hpp`: add `kJeepRap = 0x006U` (not decoded anywhere today;
   the hardware filter already passes it).
2. **Decoder state** — add to the decoder (or `VehicleState`, if RAM map allows) two booleans and two
   timestamps: `ignOn`, `ignSeenMs`, `rapOn`, `rapSeenMs`. Use `ram::app().sched.counter` (ms).
3. **`decodeIgnition(v)`** (`can_decode_shared.hpp:232`): keep `kFlagIgnRaw`; store `ignOn = (v & 0xE0) > 0x10`,
   `ignSeenMs = now`; call `recomputeAcc()` instead of writing `kFlagAccOn` directly.
4. **New `decodeRap(d1)`**: `rapOn = (d1 & 0x02) != 0`, `rapSeenMs = now`, `recomputeAcc()`.
   Dispatch in `decodeProfileJeep()` switch (`can_decode.cpp:414-521`):
   `case canid::kJeepRap: if (len >= 2) decodeRap(d[1]); break;`
5. **`recomputeAcc()`**: `setFlag(kFlagAccOn, (ignOn && fresh(ignSeenMs)) || (rapOn && fresh(rapSeenMs)))`.
6. **Staleness** — call `recomputeAcc()` once per 8 ms pass at the top of `dispatchGate()`
   (`can_decode.cpp:572`), i.e. before `periodicTxScheduler`, `serviceBusPresence` and `writeStatusLines`.
   `kAccStaleMs = 1000` in `app_config.hpp`. **Must stay well below the 10 s link-wedged timeout**
   (`can_link.cpp:193-196`), otherwise a sleeping bus with ACC still on forces a WWDG reset.
7. **FCA 125k profile** (`0x20B`, `can_decode.cpp:541`): unchanged behaviour — never sets `rapOn`, so
   `accOn = ign` only. Keep its path writing through `recomputeAcc()` so both profiles share the code.
8. **0x7FF vendor override**: today the next `0x000` frame overwrites it; after the change the next
   `0x000`/`0x006` frame or the 8 ms recompute does. Accept (never used by the car) — or, if wanted,
   add an override latch honoured for 1 s. Recommendation: accept, note in docs.
9. **UART `0xB2` ACC bit** (`custom_decode.cpp:313-337`): leave on the ignition ladder. Android gets ACC
   from the PB5 wire via its host MCU, and `0xB2` then still tells the app the true key position
   during RAP.

## Knock-on effects (behaviour that now lasts up to 10 min after key-off)
All of these follow `kFlagAccOn` and will now behave as "radio on" during RAP — which is what the factory
radio does:
- `0x09F b0` stays `01` (on); `0x18C` "no ignition" bit4 stays clear; `0x3D0` amp frame keeps going.
- Periodic TX (incl. TPMS poll) keeps running; the 3.2 s TX stop starts when RAP ends.
- Link state stays Running; NM probes on `0x416` answer "awake" (`3F`) until RAP ends, then `BF`.
- Deep sleep: RAP clear → ACC off → Lost → bus silent 10 s → Down → STOP. Same path as today, just later.

**Check in the car:** with the box answering `3F` during RAP, the body module still ends RAP at 600 s and
the bus still sleeps (the 600 s test was done with no radio/canbox connected). If the bus stays awake,
the NM answer must follow true ignition instead.

## Tests (`tests/host`)
Update:
- `test_can_decode.cpp`: `jeep_ignition_decodes_acc_and_the_raw_bit` (:132),
  `jeep_ignition_ignores_bit4_of_the_source_byte` (:149), `fca_ignition_decodes_acc_on_its_own_id` (:159),
  `vendor_7ff_...` (:706), `swc_service_probe_is_qualified_by_the_acc_flag` (:976), `dispatch_gate_*` (:1144-1174).
- `test_can_link.cpp`: `link_running_moves_to_lost_when_acc_goes_away` (:424) — drive it via RAP clear / staleness.
- `test_periodic_tx.cpp` (:400-439) 9F edges and hold timer.

Add:
1. `acc_on_at_acc_before_rap_bit` — `000=63`, no `006` → on.
2. `acc_stays_on_through_crank` — `000=A9` + `006 b1=20` (either order) → on.
3. `acc_held_after_key_off_by_rap` — `000=01`/`00` + `006 b1=22` → on.
4. `acc_off_when_rap_clears` — `000=00` + `006 b1=00` → off; also `b1=20` → off.
5. `acc_off_when_frames_go_stale` — last `006 b1=22`, then no frames 1000 ms → off; 999 ms → still on.
6. `acc_ignores_bit5_alone` — `006 b1=20`, `000=00` → off.
7. `bus_wake_after_timeout_does_not_turn_acc_on` — frames resume with `000=00`, `006 b1=00` → off.
8. `fca_profile_unchanged` — `0x20B` only, behaves as before.
9. `stale_timeout_below_link_wedged` — static_assert `kAccStaleMs < 10 s` link timeout.

## Docs
- `docs/ACC-OUTPUT.md`: replace quick summary / "Where the ACC flag comes from" / "When OFF" with the new rule.
- `docs/REQUIREMENTS.md:102` (FR-28): reword to the RAP rule.

## Car test plan
Log CAN while doing each, checking PB5 / radio power:
1. ACC → RUN → crank → run: no drop during crank.
2. Key off, key out, wait 30 s, open door: on until door, off within ~100 ms.
3. Key off, key out, doors closed: off at 600 s; bus sleeps; box enters STOP.
4. Door after (3): bus wakes, radio stays off.
5. Confirm with the box connected that RAP still ends at 600 s (NM `3F` during RAP doesn't hold the bus awake).
