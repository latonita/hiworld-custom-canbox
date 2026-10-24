# Data the JPF20L canbox provides to the head unit

Build: `--protocol jeep` (default), version tag `H1N4JPF20L`. Link: UART, 38400 baud,
Hiworld framing `AA 55 LEN CMD <payload> CHK`, `CHK = (LEN + CMD + Σpayload − 1) & 0xFF`.
Payload byte `[n]` = frame byte `4+n`.

Frames go out round-robin in this order (`kTxTable`, `app/services/hu_hiworld.cpp`):
`0x72 0xA6 0x73 0xF0 0x12 0x32` (stock) then `0xB0 0xB2 0xB3 0xB4 0xB5 0xB7 0xB8 0xBB 0xBD 0xBC` (custom).

Specs: stock frames — `docs/REQUIREMENTS.md` FR-28…FR-58; custom block — `docs/CUSTOM-TX-SPEC.md` (`0xBC` in §10);
`0xB7` — `docs/TPMS-POLL-SPEC.md`.

---

## 1. What and where

CAN sources are on the WK body bus (83.333 kbit/s), written as `ID bN` (id, data byte).

### Stock frames (wire-identical to the Hiworld box, except `0x32` gear/RPM)

| Cmd / byte | Data | Encoding | Source |
|---|---|---|---|
| `0x72 [0]` b0 | ACC on | 1 = on | `000` b0 (`(b0 & 0xE0) > 0x10`) |
| `0x72 [0]` b1 | illumination on | 1 = on | `210` b1 != 0 |
| `0x72 [0]` b2 | reverse | 1 = in reverse | `003` b2 low nibble ∈ {1, 9} |
| `0x72 [0]` b3 | spare | — | `7FF` vendor frame only |
| `0x72 [0]` b4 | raw ignition bit | unidentified | `000` b0 bit 0 |
| `0x72 [0]` b5 | CAN-error latch | internal | box |
| `0x72 [0]` b6 | autobaud searching | internal | box |
| `0x72 [0]` b7 | latched CAN profile | internal | box |
| `0x72 [2]` | steering-wheel key | `0x0B` mode, `0x01`/`0x02` vol+/−, `0x08`/`0x09` next/prev, `0x03` mute, `0x05` phone, `0x07` voice, `0x12` spare, `0x00` none | `3A0` b0:b1 |
| `0x72 [3]` | dash illumination level | 0…100 % (`b1 >> 1`, clamped) | `210` b1 |
| `0x72 [4:5]` | steering angle | BE, 0.5°/LSB, two's complement, updated only in reverse | `015` b4:b5 (centre `0x1000`) |
| `0xA6 [0]` | amplifier level | fixed `0x14` (20) | box init |
| `0xA6 [1:5]` | tone (5 bytes) | stored settings | box init |
| `0x73 [0:7]` | — | always `00` × 8 | — |
| `0xF0 [0:16]` | firmware version string | ASCII, `H1N4JPF20L…` | box |
| `0x12 [2]` | doors | b7 LF, b6 RF, b5 LR, b4 RR, b3 tailgate; 1 = open | `013` b2 (src b0 LF, b1 RF, b3 LR, b2 RR, b4 tailgate) |
| `0x12 [4]` | volume mirror | raw | box |
| `0x12 [5:9]` | tone (alias of `0xA6 [1:5]`) | raw | box |
| `0x32 [1]` | gear | 1 P, 2 N, 3 R, 4 D, 5 S (AutoStick 1–4), 0 blank | `003` b4 (ASCII of cluster char) |
| `0x32 [2:3]` | engine RPM | BE u16, 1 rpm/count, `FFFF` = unavailable | `002` b0:b1 (verbatim) |
| `0x32` rest | — | `00` | not decoded |

### Custom telemetry block (`0xB0`–`0xBD`)

| Cmd / byte | Data | Encoding | Source |
|---|---|---|---|
| `0xB0 [0]` | protocol revision | `0x01` | box |
| `0xB0 [1]` | validity mask | b0 `B2`, b1 `B3`, b2 `B4`, b3 `B5`, b4 `B7`, b5 `B8`, b6 `BB`, b7 `BD`; 1 = live | box |
| `0xB2 [0]` b0 | ACC on | 1 = on | `000` b0 ∈ {`63`, `85`, `A9`} |
| `0xB2 [0]` b1 | engine running | 1 = running (state machine) | `000` b0 + `002` b0:b1 |
| `0xB2 [0]` b2 | reverse | 1 = R | `003` b4 == `'R'` |
| `0xB2 [0]` b3 | parking brake | 1 = set | `0D0` b0 & `0x80` |
| `0xB3 [0:1]` | vehicle speed | u16 km/h | `002` b2:b3 / 128 |
| `0xB3 [2:3]` | engine RPM | u16, verbatim | `002` b0:b1 |
| `0xB3 [4]` | engine load | always `00` (not built) | — |
| `0xB3 [5]` | throttle | 0…100 % | `2C0` b2: `(b2 − 23) × 100 / 211` |
| `0xB4 [0]` | coolant temp | °C + 40, `FF` = n/a | `02C` b1 (verbatim) |
| `0xB4 [1]` | intake air temp | °C + 40 | `015` b2 + 40 |
| `0xB4 [2]` | fuel level | 0…100 % | `159` b5 × 0.6443 |
| `0xB4 [3:4]` | battery voltage | u16, 0.01 V | `015` b1 × 10 |
| `0xB5 [0:1]` | outside air temp | s16, 0.1 °C | `014` b4 × 10 |
| `0xB7 [0]` | tyre position | 0 LF, 1 RF, 2 RR, 3 LR (rotates every 5 s) | — |
| `0xB7 [1:2]` | tyre pressure | u16 kPa, `FFFF` = n/a | WCM KWP2000 `21 3B` poll every 30 s, reply on `500` (psi × 6.894759) |
| `0xB7 [3]` | tyre temperature | always `FF` (no source) | — |
| `0xB7 [4]` | tyre flags | b0 battery low, b1 leak, b2 no signal | WCM poll / staleness |
| `0xB8 [0:1]` | fuel in tank | u16, 0.1 L | `159` b5 × 5 |
| `0xB8 [2:3]` | range | always `FFFF` (no source) | — |
| `0xBB [0:2]` | odometer | u24, 0.1 km, `FFFFFF` = n/a | `014` b0:b2 (verbatim) |
| `0xBD [0]` | compass sector | 0 N, 1 NE, 2 E, 3 SE, 4 S, 5 SW, 6 W, 7 NW; `FF` = n/a | `1B4` b0 |
| `0xBD [1:2]` | compass heading | u16 degrees 0…358, `FFFF` = n/a | `1B4` b1 × 2 |
| `0xBC [0:1]` | fuel flow | u16, 0.1 L/h, `FFFF` = n/a; 1 s time-weighted average | `1A7` b0:b1 (Q / 16; L/h = Q / 160, unverified at the pump) |

### Outside the UART

- Reverse and illumination are also hardwired GPIO lines to the radio (PB6 / PC15).
- The stock `0x500 → 0x516` CAN mirror is switched off in this build (`0x500` is the WCM's tyre-poll reply id).

---

## 2. Plain list — data element → command / byte

- ACC on — `0x72 [0]` bit 0; `0xB2 [0]` bit 0
- Illumination on — `0x72 [0]` bit 1
- Reverse — `0x72 [0]` bit 2; `0xB2 [0]` bit 2
- Raw ignition bit — `0x72 [0]` bit 4
- CAN-error / autobaud / profile status — `0x72 [0]` bits 5, 6, 7
- Steering-wheel key — `0x72 [2]`
- Dash illumination level — `0x72 [3]`
- Steering angle — `0x72 [4:5]`
- Amplifier level — `0xA6 [0]`
- Tone settings — `0xA6 [1:5]`; `0x12 [5:9]`
- Volume — `0x12 [4]`
- Zero filler — `0x73 [0:7]`
- Firmware version — `0xF0 [0:16]`
- Door LF / RF / LR / RR / tailgate — `0x12 [2]` bits 7 / 6 / 5 / 4 / 3
- Gear (PRNDS) — `0x32 [1]`
- Engine RPM — `0x32 [2:3]`; `0xB3 [2:3]`
- Protocol revision — `0xB0 [0]`
- Validity mask — `0xB0 [1]`
- Engine running — `0xB2 [0]` bit 1
- Parking brake — `0xB2 [0]` bit 3
- Vehicle speed — `0xB3 [0:1]`
- Engine load (always 0) — `0xB3 [4]`
- Throttle position — `0xB3 [5]`
- Coolant temperature — `0xB4 [0]`
- Intake air temperature — `0xB4 [1]`
- Fuel level % — `0xB4 [2]`
- Battery voltage — `0xB4 [3:4]`
- Outside air temperature — `0xB5 [0:1]`
- Tyre position — `0xB7 [0]`
- Tyre pressure — `0xB7 [1:2]`
- Tyre temperature (always n/a) — `0xB7 [3]`
- Tyre flags (battery low / leak / no signal) — `0xB7 [4]`
- Fuel in tank, litres — `0xB8 [0:1]`
- Range (always n/a) — `0xB8 [2:3]`
- Odometer — `0xBB [0:2]`
- Compass sector — `0xBD [0]`
- Compass heading — `0xBD [1:2]`
- Fuel flow, L/h — `0xBC [0:1]`
