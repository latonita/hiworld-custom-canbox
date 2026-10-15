# What the radio understands vs. what your box sends — the injection surface

**Question answered:** even though your 83.333 k box never emits certain data, the Android
plugin (`P1_501.apk`, protocol 3009001/JPF2) still contains the *handlers*. Any UART frame in
its RX vocabulary that the box leaves silent is something **you can inject yourself** (MITM on
the box↔head-unit UART, or a custom canbox) and the radio will parse and display it.

Frame = `AA 55 | LEN | CMD | payload[LEN] | CHK`, 38400 8N1.
`CHK = (Σ of all bytes incl. AA 55) & 0xFF` ( = `(LEN+CMD+Σpayload − 1) & 0xFF` ).
The plugin's gate is `isDataPass(d, N, T)` = "CMD == T **and** payload length ≥ N", so **LEN
must be ≥ the required length** or the handler silently drops the frame.

Sources: `hiworld-jpf2-jeep.md` (plugin RX/TX map, DECOMPILED), `hiworld-jpf2-83k-io.md` +
`box-firmware-decompiled.md` (what this box actually emits on 83.333 k).

---

## The four buckets

### A. Already covered — box sends it, radio shows it (nothing to inject)

| CMD | LEN | What the radio does with it |
|---|---|---|
| `0x12` | 10 | doors ×4 + trunk (payload[2] bits 7..3) |
| `0x72` | 14 | SWC key (payload[2]) **and** steering angle (payload[4..5], ±540°) |
| `0xA6` | 7 | amplifier state screen (VOL/BAL/FAD/BASS/MID/TRE) |
| `0xF0` | var | firmware version string |

These already work on your car. (The box also emits `0x73` L8, which the **radio ignores** —
dead in both directions.)

### B. VACANT but radio-ready — **inject these and the radio reacts** ← the money list

| CMD | LEN | Inject to get | Notes |
|---|---|---|---|
| **`0x32`** | **14** | **RPM + gear (PRND/S) on the "Body information" screen** | Your 83.333 k box sends `0x32` **all-zero** (it has no RPM/gear decoder — that only exists on the 125 k profile). The radio's JPF2 handler reads **only** `[1]`=gear (`1`=P `2`=N `3`=R `4`=D `5`=S) and `[2..3]`=RPM (big-endian, 1:1, `0xFFFF`=blank). Fill those and it displays. |

> **IMPLEMENTED.** The rebuilt firmware now fills both fields on profile 0, so no injection
> and no plugin change is needed for RPM and gear:
>
> * RPM from CAN `0x002` d[0]:d[1] (1 rpm/count BE), copied verbatim into `[2..3]` — the
>   car's all-`FF` "engine off" value is the same sentinel the radio blanks on, so it is
>   passed straight through.
> * PRNDL from CAN `0x003` d[4], the ASCII character the cluster displays, mapped into `[1]`
>   as 1=P 2=N 3=R 4=D with the AutoStick ranges reported as 5=S.
>
> Both ids are *Confirmed* in `docs/JEEP-WK-CAN-ID-byte-map.md`. See DV-2 in
> `docs/misra-cpp.md`, and nine tests in `tests/host/test_can_decode.cpp`.
> The speed/voltage/temperature caveat below is unchanged and still applies.
| **`0x41`** | **16** | **Front + rear parking-sensor radar display** | Box never emits it on 83.333 k. Handler present via the `default:` branch. `[0..3]`=front L→R, `[4..7]`=rear L→R; value = proximity ring `1..N` (1 = closest/red), `0` = clear, `0xFF` = absent. `[8..15]` ignored. |

**Important — what `0x32` will *not* give you on this radio:** the speed / battery-voltage /
oil-coolant-trans-intake-temp fields (payload `[4..13]`) are **only decoded by the sibling
`JPP004` protocol**, not by your JPF2 plugin. Injecting them does nothing here — the JPF2
`0x32` handler stops after gear + RPM. To show speed/temps you'd have to switch the head unit
to a JPP004 protocol, which changes the whole framing dialect. So realistically **`0x32` buys
you RPM and gear, `0x41` buys you radar** — that's the real gain.

### C. Radio parses it but it's inert for JPF2 — injecting won't help

| CMD | Why it does nothing |
|---|---|
| `0x45` (6) | driver-assist/drive-mode flags — the handler runs, but the icons are **hidden for 3009001** (`MainActivity_P.updateView` shows only Amplifier + Menu). Nothing appears. |
| `0x7D` (2) | turn signals — only ever used to **switch a 360° camera view**, and only when a "metazone" flag is set (`readMetazonedata()==1`). No dashboard indicator. Marginal at best. |
| `0x94` (4) | reverse notification — routed through a non-JPF2 360° path; JPF2 reverse comes from a host broadcast, not this. |
| HVAC / climate | **No handler at all** — there is no `car_aircon_3009001.json`, so `JsonAircon` is null and not one byte is parsed. You cannot inject climate to this plugin. |

### D. Not reachable through this UART at all

ACC/ignition, reverse gear, illumination/dimming reach the plugin as **Android broadcasts from
the host MCU** (`xy.android.acc.on/off`, `android.backcar.action.STARTED/FINISH`), *not* as
UART frames the plugin parses. Injecting them on the box UART won't move them — they're decoded
one layer up. (Reverse *is* also visible as `0x32[1]==3`=R, but the plugin doesn't use gear for
the reverse-camera trigger.)

Signals with **no decoder anywhere** in the plugin (so nothing to inject into): fuel level /
range / odometer / trip, ambient temperature, door-lock state, handbrake, seat belts (sink
exists, no source), hood (only an XREF guess: `0x12[2] bit2`).

---

## Worked frames you can send today

**RPM = 2000 r/min, gear = D:**
```
payload (14) = 00 04 07 D0 00 00 00 00 00 00 00 00 00 00
              [1]=04 (D)   [2..3]=07D0 (2000)
frame = AA 55 0E 32 00 04 07 D0 00 00 00 00 00 00 00 00 1A
                 └LEN └CMD                              └CHK
```
CHK check: `AA+55+0E+32+04+07+D0 = 0x21A → &0xFF = 0x1A` ✓. Sweep `[2..3]` for a live tach;
set `[2..3]=FF FF` to blank it.

**Rear sensors all "closest", front clear:**
```
payload (16) = 00 00 00 00 01 01 01 01 00 00 00 00 00 00 00 00
              front[0..3]=clear   rear[4..7]=ring 1
frame = AA 55 10 41 00 00 00 00 01 01 01 01 00 00 00 00 00 00 00 00 <CHK>
CHK = (10+41+04)-1 = 0x54
```

---

## How to actually inject

- **Cleanest: MITM the UART** between box and head unit (an MCU with two UARTs). Pass box→radio
  traffic through untouched, and **add** your own `0x32`/`0x41` frames (or overwrite the box's
  all-zero `0x32`). No firmware patching, fully reversible, and you keep every real signal the
  box already provides. This is the recommended lever.
- **Or replace the box** with your own canbox emulator that reads the real CAN bus and emits the
  full JPF2 UART vocabulary — more work, but then you also control doors/steering/amp yourself.
- Either way, respect `isDataPass`: **LEN ≥ the required length** (0x0E for `0x32`, 0x10 for
  `0x41`), correct CMD, correct checksum, or the frame is dropped without a trace.

See `hiworld-jpf2-jeep.md` for the full per-byte handler detail and `hiworld-jpf2-83k-io.md`
for exactly what the box does/doesn't emit on your 83.333 k profile.
