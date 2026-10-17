# Pretending to be a Mercedes — the `--protocol benz` build

What the emulation publishes, what it cannot, and what it costs. Protocol
evidence: `../richest-raise-benz.md` and `../richest-protocol-comparison.md`,
both from the decompiled Raise Benz plugin
`my-radio-files/CanBus/plugin/1/34/P1_301.apk`. WK signal sources and their
confidence levels: `JEEP-WK-CAN-ID-byte-map.md`.

## Why

The Benz plugin has the richest **live gauge** set of any protocol in the
408-plugin corpus — and, unusually, it is reachable without writing an Android
app. The cost is that the head unit must be set to a Benz protocol id, after
which the whole plugin interprets Benz semantics for everything.

## What is published

Five messages, built by `logic::benzPublish()` and transmitted by the existing
round-robin in `svc::hu`.

| Benz field | frame | WK source | confidence |
|---|---|---|---|
| Engine rpm | `0x08` `[0:1]` | `0x002` b0:b1, 1 rpm/count | Confirmed |
| Road speed, km/h | `0x07` `[0:1]` | `0x002` b2:b3 ÷ 128 | Confirmed |
| Doors ×4 + tailgate | `0x41` p1 `[1]` bits 0‑4 | `0x013` b2, via the stock decoder | Confirmed |
| Outside temp, 0.1 °C | `0x41` p2 `[1:2]` | `0x014` b4, whole °C ×10 | high |
| Odometer, km | `0x41` p2 `[3:5]` | `0x014` b0:b2 ÷ 10 | Confirmed |
| Coolant temp, °C | `0x41` p3 `[1:2]` | `0x02C` b1 − 40 | high |
| Battery voltage, 0.1 V | `0x41` p3 `[5:6]` | `0x015` b1 | Confirmed |

Everything the WK can source and the Benz protocol can display is in that
table. Nothing else in the protocol has a WK source.

## What is NOT published, and why

**Fields with no WK source** — sent as zero, never fabricated:

- **engine-oil temperature** (`0x41` p3 `[3:4]`) — not on CAN‑B
- **power (hp)** and **torque (lb‑ft)** (`0x41` p3 `[7:10]`) — on a real Benz
  these are gateway-computed estimates. A plausible invented curve is a number
  somebody eventually believes; zero is obviously uninstrumented.

**Frames deliberately never sent:**

| frame | reason |
|---|---|
| `0x21`/`0x06` HVAC | the plugin ships no `car_aircon.json`, so its change test is always false and **every** frame force-shows the climate overlay. The WK's manual ATC is not on the bus anyway. |
| `0x22`/`0x23` radar | same overlay behaviour, 5 s auto-hide re-armed per frame; no WK sensors to populate it |
| `0x25` park assist | nothing to report without PDC |
| `0x26` steering angle | the WK's SAS is on CAN‑C at 500 k, which this box cannot see |
| `0x41` p`0xA8` ESP/SPORT/drive-mode | no CAN‑B source |

## WK signals the Benz protocol cannot display at all

Verified absent from the plugin — no decoder, no string resource, no layout row:

- **per-wheel TPMS pressure and temperature.** The framework classes exist
  (`com.canbus.json.tire.*`, a `tpms_rx_uart` dialect) but there is no
  `car_tire.json`, `getCarTire()` has zero callers, and the TPMS UART dialect
  is selected by a baud code the Benz config can never produce. There is
  nowhere to put it.
- **intake-air temperature.** The protocol has exactly two temperature fields
  (coolant and oil) plus the HVAC ambient. No third slot.
- **fuel level, gear/PRND, trip meters.**

These need a custom Android app on `xy.auto.canbus.rxpduses`, or the Hiworld
build.

## The functional trade-off — read this before flashing

**Steering-wheel buttons stop working.** Benz SWC is `0x20`/`0x27` with its own
key-code table, and the published map is incomplete (`1=Vol+, 2=Vol−, 3=Prev,
4=Next, 5=Phone, 6=Hangup, …`). Guessing it would ship wrong button behaviour,
so it is not implemented. In a jeep build the buttons work as they always have.

Also lost, relative to the jeep build: gear/PRND display, the amplifier screen,
and the Hiworld `0x12`/`0x72` body messages.

## Timing

The plugin has **no staleness timeout and no blanking**: one valid frame marks
the box connected for the session, and every field holds its last value. No
handshake, no heartbeat, no version frame, and the box never has to ACK
anything — the head unit sends a naked `0xFF` (good checksum) or `0xF0` (bad)
per frame, which the parser ignores.

The stock cadence is therefore sufficient: a changed message every 48 ms, a
keep-alive sweep about every 720 ms.

Two hard limits the head unit's parser imposes, both enforced by
`static_assert` against the TX table:

- `1 ≤ LEN ≤ 117`. It counts LEN down in a **signed** byte, so `LEN = 0` makes
  it consume bytes until its own 120-byte cap trips.
- Page 2 must be 6 bytes and page 3 must be 11. The plugin's length gate admits
  shorter frames and then reads past them — an under-long frame does not get
  ignored, it throws inside the radio's canbus service.

## Using it

```
python tools/build.py --protocol benz --pack
python tools/iap_upload.py build/canupdateH1N4BNZ10L-171026.iap --port COM17 --reset powercycle --verify
```

Then set the head unit's Canset protocol id to **1034025**, **1034027** or
**1034028** — the three ids whose `MainActivity_P` enables all four screens
(Analog-key, Set, Menu, Oil). Any other Benz id decodes the same wire data but
shows fewer screens.

Going back is an ordinary flash (`--protocol jeep`) plus setting Canset back to
3009001. The version string tells you which image is on the box:
`H1N4BNZ10L-…` or `H1N4JPF20L-…`.

**Updating from the radio still works in either mode** — pick "Hiworld" in the
upgrade screen's spinner regardless of what the box currently speaks; that
choice is the operator's and is never derived from the box. See the IAP escape
hatch note in `app/include/app_config.hpp`.
