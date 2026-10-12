---
name: hiworld-frame-format
description: "Verified Hiworld UART frame format and checksum for this head unit (AA 55, len=payload only, sum-of-all-bytes mod 256)"
metadata: 
  node_type: memory
  type: project
  originSessionId: 9e579ff9-c20e-4dc4-95f2-3ebdc611031c
  modified: 2026-09-29T11:40:14.214Z
---

Hiworld canbox ↔ head-unit UART framing, **verified by computation against 13
captured frames** (13 candidate checksum algorithms tested; all others scored 0 or 1):

```
AA 55 │ L │ CMD │ payload[L] │ CKS      total wire length = L + 5
```

- `L` counts **payload bytes only** — excludes CMD, preamble and checksum
- `CMD` sits at offset 3, *after* the length byte (this distinguishes Hiworld from the
  sibling Raise protocol, which is `2E CMD LEN …`)
- `CKS` = sum of **all** bytes including the `AA 55` preamble, mod 256
- `AA 55 01 FF <X> <cks>` is a **poll** — "send me message type X"; the box replies
  with CMD = X. No authentication, so a TTL UART can impersonate the head unit.

**Confirmed three independent ways:** derived from captures; `smartgauges/canbox`'s
`canbox_hiworld_checksum()` = `sum − 1`; and decompiled verbatim from the vendor's own
decoder (`ToolClass.sendDataToSerial` / `CanbusSerial.hiWorld_rx_uart_aa55` in
`plugin/3/9/P2_101.apk`), which computes `CHK = (LEN + CMD + ΣDATA − 1) & 0xFF`.

**The `AA 55` vs `5A A5` question is settled.** Both dialects exist as separate code
paths chosen by a pseudo-baud selector: `protocol_id 3009001` → selector **38403** →
`hiWorld_rx_uart_aa55` (`AA 55`). Selector 38401 → `hiWorld_rx_uart` (`5A A5`), which
is what all public documentation describes. That is why this box differs from the docs.

**Why:** this was derived once at some cost and is the foundation for every further
decode; re-deriving it would be wasted work.

**How to apply:** parsers should accept both preambles. Full spec with worked examples
in `hiworld-protocol.md`. Related: [[canbox-is-translating-gateway]],
[[jeep-profile-is-jpf2]].
