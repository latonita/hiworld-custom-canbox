---
name: jeep-profile-is-jpf2
description: "This car's canbox profile is protocol_id 3009001 / tag JPF2, shared across the whole 2005-2010 DaimlerChrysler family"
metadata: 
  node_type: memory
  type: project
  originSessionId: 9e579ff9-c20e-4dc4-95f2-3ebdc611031c
  modified: 2026-09-29T11:36:07.880Z
---

The car is a **2005–2010 Jeep Grand Cherokee (WK)**. Its canbox profile is
**`protocol_id = 3009001`, tag `JPF2`** — confirmed on screen (*Network CAN Set
V1.301, ID: 3009001*, "Protocol is connected") and by the box label reading JPF2.10.

`protocol_id` decomposes as `factory(1) + brand(3) + variant(3)`:
`3` = Hiworld, `009` = Jeep, `001` = variant. (Cross-checked: `3046xxx` = Hiworld/Dodge,
`1009xxx` = Raise/Jeep.)

**JPF2 is one shared decoder across the whole DaimlerChrysler-era family** — Jeep
Grand Cherokee WK / Compass / Patriot / Wrangler JK / Liberty KK / Commander,
Chrysler 300C / Sebring, Dodge RAM Pickup / Challenger / Charger LX / Durango HB+WD.

**Why:** this means any JPF2 capture or documentation from a 300C, Wrangler JK or
Charger LX applies directly to this WK — they share CAN-IHS and the identical
decoder. It widens the search space a lot when hunting for signal maps.

**How to apply:** when searching for Hiworld Jeep signal documentation, search the
whole family, not just "Grand Cherokee". Do NOT treat `3009002`–`3009025` as
compatible — those are JPF1/JPF3/JP06/JP07/FT09, different protocols despite adjacent
IDs. The decoder binary is `system/media/CanBus/plugin/3/9/P2_101.apk`.

Related: [[hiworld-frame-format]], [[canbox-is-translating-gateway]].
