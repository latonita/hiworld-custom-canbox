# JPF20L — replacement firmware for the Hiworld JPF2 canbox (Jeep WK)

Replacement firmware for the **Hiworld JPF2 canbox** (Nations N32L406) in a **Jeep Grand Cherokee
WK, 2005–2010**, on an Android head unit. It installs through the stock canbox updater and sends the
head unit far more car data than the stock firmware does.

| | |
|---|---|
| Version tag | `H1N4JPF20L-<ddmmyy>` (current build: `H1N4JPF20L-061126`) |
| `L` | latonita (the author); the stock vendor image is `A` |
| Stock firmware | `H1N4JPF20A-220524` |
| Protocol | Hiworld JPF2, protocol id 3009001 (unchanged) |
| Alternative | A Raise Benz emulation build (`H1N4BNZ10L`) also exists. It is not actively used. |

## What the box exports to the head unit

Link: UART, 38400 baud, Hiworld frames `AA 55 LEN CMD payload CHK`. CAN sources are on the WK body
bus (83.333 kbit/s), written `ID bN` (CAN id, data byte). Full byte-level detail:
[`data-provided-JPF20L.md`](data-provided-JPF20L.md).

### Stock commands (still sent)

| Signal | Unit / resolution | CAN source | Command |
|---|---|---|---|
| ACC on | flag | `000` b0, `006` b1 (RAP) | `0x72` |
| Illumination on, dash level | flag; 0–100 % | `210` b1 | `0x72` |
| Reverse | flag | `003` b2 | `0x72` |
| Steering-wheel key | key code | `3A0` b0:b1 | `0x72` |
| Steering angle | 0.5 deg/LSB, updated only in reverse | `015` b4:b5 | `0x72` |
| Doors LF/RF/LR/RR, tailgate | open flags | `013` b2 | `0x12` |
| Gear (P R N D S) | 1 P, 2 N, 3 R, 4 D, 5 S | `003` b4 | `0x32` |
| Engine RPM | 1 rpm | `002` b0:b1 | `0x32` |
| Amplifier / tone / volume, firmware version | settings; ASCII | box | `0xA6`, `0x12`, `0xF0` |

### Custom telemetry block (layout v2, `docs/TELEMETRY-V2-SPEC.md`)

The current custom protocol, on the car since 2026-11-07 (`H1N4JPF20L-071126`). Layout v1
(`docs/CUSTOM-TX-SPEC.md`) is superseded and kept as history; `-061126` is the rollback image.

| Signal | Unit / resolution | CAN source | Command |
|---|---|---|---|
| Heartbeat: layout version, capability bitmap, counter | 1 Hz | box | `0xB0` |
| Ignition state, engine running, RAP, key in, reverse, parking brake | enum + flags | `000`, `002`, `006`, `012`, `003`, `0D0` | `0xB2` S0 |
| Brake pedal, driver belt, doors, hatch | flags | `003`, `013` | `0xB2` S1 |
| Indicators (debounced), hazard, position, low/high beam, fogs | flags | `006` b0 | `0xB2` S2 |
| Cruise, A/C compressor, ESP, MIL, oil/charge/ABS lamps, 4WD range, glow, water-in-fuel | flags + enum | `003`, `1B2` | `0xB2` S3/S4 |
| Gear incl. AutoStick 1-4, source-alive bits | enum, bitmap | `003` b4 | `0xB2` S5/S6 |
| Vehicle speed, RPM | 0.1 km/h, 1 rpm | `002` | `0xB3` (every 100 ms) |
| Engine load, accelerator pedal | 0-100 % | `1A7`+`002`, `02C` b2 | `0xB3` |
| Fuel flow | 0.01 L/h, 100 ms mean | `1A7` b0:b1 | `0xB3` |
| Oil pressure, boost (MAP) | ~4 kPa/count, 1 hPa abs | `02C` b0, `2C0` b4 | `0xB3` |
| Coolant, battery, fuel % and litres | °C+40, 0.1 V, %, 0.1 L | `02C`, `015`, `159` | `0xB4` |
| Outside air, minutes since RUN | 0.1 °C, min | `014` b4:b5, `159` b0 | `0xB5` |
| Tyres, one corner per 5 s: kPa, temperature, battery/leak/no-signal/low-pressure flags | | polled from the WCM every 10 s | `0xB7` |
| Odometer, compass sector + heading | 0.1 km, 2° | `014`, `1B4` | `0xB8` |

Every field has its own "n/a" sentinel and every CAN source is aged in the box (3x its period).
Not sent (no source on this car's bus): intake air temperature, steering angle, range.

### Confirmed vs not

| Item | State |
|---|---|
| Fuel-flow scale (`Q/160` = L/h) | **Not verified at the pump.** Fits idle, rated power and a 10.4 L/100 km drive replay. Treat as a rate estimate. |
| Fuel level / litres scale | **Not verified** against a pump fill or a dipstick. |
| Tyre corner order (LF, RF, RR, LR) | Taken from the module's reply; **not yet confirmed by a deflation test**. |
| Spare tyre | Not read. |
| Steering-wheel keys | The CAN id the firmware reads (`3A0`) stays `00 00` in the drive log, so no key presses arrive on this car. |
| Illumination source (`210`) | Best guess (medium-low confidence). |
| Outside / intake temperature below 0 deg C | Never observed on the car; negative values are untested. |
| Compass | Magnetic heading, not true north. |

## Stock JPF20A vs JPF20L

| Feature | Stock JPF20A | JPF20L |
|---|---|---|
| ACC | ✓ follows key only | ✓ also held after key-off (Retained Accessory Power) |
| Illumination, reverse | ✓ | ✓ unchanged |
| Reverse / illumination hardwired lines to the radio | ✓ | ✓ unchanged |
| Steering angle | ✓ | ✓ unchanged |
| Doors | ✓ | ✓ unchanged |
| Steering-wheel keys | decoded from `3A0` | same (no data on this car, see above) |
| Gear | — (byte always 0 on this profile) | ✓ |
| Engine RPM | — | ✓ |
| Vehicle speed | — | ✓ |
| Parking brake | — | ✓ |
| Engine running flag | — | ✓ |
| Throttle | — | ✓ |
| Coolant, outside temperature | — | ✓ |
| Fuel level, litres | — | ✓ |
| Battery voltage | — | ✓ |
| Odometer | — | ✓ |
| Compass | — | ✓ |
| Fuel flow L/h | — | ✓ (scale unverified) |
| Tyre pressures (TPMS) | — | ✓ polled every 30 s |
| Intake temperature, range, tyre temperature | — | — (fields exist, always "n/a") |
| Data validity reporting | — | ✓ per-signal bits in `0xB0` |
| Stock protocol 3009001 | ✓ | ✓ still works; stock frames unchanged except `0x32` (gear and RPM now filled) |
| Custom block `0xB0`–`0xB8` (v2) | — | additive; needs an app that reads it (for example Dashwheel) |
| Firmware updatable from the radio | ✓ | ✓ same updater, bootloader untouched |

Differences to know about:

- Stock apps ignore the custom block, so with a stock radio app you get the same behaviour as before plus gear and RPM.
- The tyre poll sends one small diagnostic request on the car's body bus every 30 s. The stock `0x500` to `0x516` CAN mirror is switched off in this build, because `0x500` is the tyre module's reply id.
- ACC (the radio's power-on line, PB5) now follows the car's Retained Accessory Power like the factory radio: it stays on through cranking and after key-off, and goes off when a door opens or 10 minutes after key-off. A bus that wakes up alone (no key) no longer switches it on. Stock follows the key position only, so the radio switched off at key-off.
- The amplifier frame `0x3D0` now uses the byte order the WK's factory amp expects (VOL, BASS, MID, TRE, BAL, FADE, `FF`), the same order the original radio sent. Stock sends the radio app's slider order, so stock BAL/FAD/BASS/MID/TRE land on the wrong amp controls. The radio app still sees its values in its own order.
- The saved amplifier volume is now restored at boot (held between 5 and 20, or 30 on the 125 kbit/s profile) instead of always starting at 20. A fresh box starts at 5.
- Volume and tone settings are written to flash about 5 seconds after you stop moving the slider, not every second while you drag, so dragging no longer drops head-unit messages. A change made in the last 5 seconds before power is cut may be lost.

## Install

1. Get `canupdateH1N4JPF20L-<date>.iap`. **No separator** between `canupdate` and the version: `canupdateH1N4JPF20L-061126.iap` is correct; do not add a dash after `canupdate`.
2. Copy it to the head unit's **sdcard root** (the updater takes the first `.iap` it finds in a storage root or in `CanBus/box/`, so leave only one `.iap` there).
3. On the head unit open the canbus settings (debug menu), choose the canbox update (Hiworld) and run it. Keep the unit powered and do not touch it until it reports success. The box resets into its bootloader, takes the file, then restarts.
4. Check the version shown in the canbus settings: `H1N4JPF20L-<ddmmyy>`.

**Back to stock:** run the same update with the stock `.iap` for `H1N4JPF20A-220524` (kept in the repo's `hiworld/` folder, not in this one).

**Safety:** the 8 KB bootloader is never touched or replaced, so a bad or interrupted update can always be fixed by flashing again.

## Status and limitations

- WIP; used daily on one car: Jeep Grand Cherokee WH, 3.0 CRD, 2005.
- Unverified items are listed above. Intake temperature, range and tyre temperature have no source.
- Other WK-family years and engines are untested.
- For reading the data in an app: Dashwheel (the launcher, repo `dashwheel`) reads the custom block.

## For developers

Building, firmware layout, protocol specs, reverse-engineering notes: [`docs/DEVELOPER.md`](docs/DEVELOPER.md).
Build commands: [`BUILD.md`](BUILD.md).
