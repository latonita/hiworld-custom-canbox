# Hiworld `.iap` firmware container — format specification

A `.iap` file is the firmware update container for the Hiworld canbox (Nations
`N32L406CBL7`, Cortex-M4F). It wraps a raw Cortex-M application image in fixed-size,
checksummed records; the image bytes are lightly **obfuscated** (not encrypted, not
compressed) with a key carried in the file. The box's IAP bootloader consumes the file
record by record over UART.

The whole file can be unpacked to the raw image and repacked byte-for-byte identically.

---

## 1. File structure

```
file = N records, each exactly 136 (0x88) bytes.   file size = 136 * N.
no global header, no global trailer.

offset        record
------        ------
0x0000        record 0        header record       (§2.2)
0x0088        record 1        first data record   (§2.1)
0x0110        record 2
  ...
136*(N-1)     record N-1      last data record (last-flag set)
```

Payload size = `(N-1) * 128` bytes. The last data record is zero/`0xFF`-padded to the full
128-byte data field as needed.

---

## 2. Record layouts

### 2.1 Data record (records 1 .. N-1)

```
offset  size  field
------  ----  -----
 +0     128   obfuscated payload data            (de-obfuscation: §4)
 +128   2     u16 LE  sequence number  = k       (1 .. N-1)
 +130   1     u8      percent complete = k*100/(N-1)   (integer division)
 +131   1     u8      last-record flag: 1 on the final record, else 0
 +132   4     u32 LE  record checksum            (§3)
```

The sequence number gives the flash destination directly (§5): `0x08002000 + 128*(k-1)`.

### 2.2 Header record (record 0)

```
offset  size  field
------  ----  -----
 +0     12    universal magic (constant for all boxes):
              01 09 07 08 00 02 01 07 91 32 47 a4
 +12    2     family tag, ASCII:  "N3" (H1N4 boxes) / "I3" (H1H2 box)
 +14    2     per-firmware tag
 +16    116   per-file, not interpreted by the bootloader
 +132   4     obfuscation key (stored in the clear)   (§4)
```

Record 0 carries no sequence/percent/checksum fields and contributes nothing to the payload.

Bytes `[0:16]` together are the **16-byte target magic** the bootloader validates byte-for-byte;
a mismatch is rejected (the loader replies `R`). Bytes `[0:12]` are universal; bytes `[12:16]`
are a **per-box compatibility tag** that prevents flashing firmware built for a different box:

| box | part number | bytes [12:16] | [12:14] ASCII |
|---|---|---|---|
| 2429 | H1N4JPF20A | `4e 33 14 f2` | `N3` |
| 2883 | H1N4MT060A | `4e 33 16 06` | `N3` |
| 2948 | H1H2JP063A | `49 33 09 f1` | `I3` |

A generic container tool need only match the 12-byte universal magic; a flasher targeting a
specific box must match all 16.

---

## 3. Record checksum

```
record[132:136] (u32 LE) = ( sum of record[0:132] read as 33 little-endian u32 ) mod 2**32
                         = sum(struct.unpack('<33I', record[:132])) & 0xFFFFFFFF
```

Computed over the **obfuscated** bytes as they appear in the file, and **including** the
sequence/percent/flag word at `[128:132]`. Present on data records only (record 0 has the key
in that slot).

Because the sequence/percent/flag word is inside the summed range, two records with identical
payload data still have checksums differing by `seq + (pct << 16)`.

---

## 4. Obfuscation

Per-record, position-additive byte transform with a 4-byte repeating key:

```python
# i = offset 0..127 WITHIN the record's 128-byte data field; it resets every record.
# key = record0[132:136]
plain [i] = (cipher[i] - i - key[i & 3]) & 0xFF      # decode
cipher[i] = (plain [i] + i + key[i & 3]) & 0xFF      # encode
```

Only the 128-byte data field is transformed. The trailer bytes `[128:136]`
(sequence/percent/flag/checksum) are **not** obfuscated.

Worked example — key `68 cc e3 3e`, a plaintext `0x00` at `i = 8`:

```
cipher[8] = (0x00 + 8 + key[0]) & 0xFF = (8 + 0x68) & 0xFF = 0x70
plain[8]  = (0x70 - 8 - key[0]) & 0xFF = 0x00            (round-trips)
```

---

## 5. Payload

The decoded payload is a raw Cortex-M4 application image.

- **Load/flash base:** `0x08002000`.
- **Program address of record k:** `0x08002000 + 128*(k-1)`.
- The image is the application region only; it has **no vector table of its own**. The
  bootloader at `0x08000000` owns the vector table and enters the application via `bl` at
  offset `0x202` (`0x08002202`, the app's reset veneer → `__main`), after checking a valid-app
  marker it wrote at `0x08001800`. The application runs on the bootloader's vector table
  (`VTOR = 0x08000000`).

Flash map of the device:

```
0x08000000 .. 0x08001FFF   8 KB    IAP bootloader   (not contained in the .iap)
0x08002000 .. 0x0801F7FF   118 KB  application      (the .iap payload)
0x0801F800 .. 0x0801FFFF   2 KB    parameter / emulated-EEPROM page
```

The box's ASCII part/version string (e.g. `H1N4JPF20A-220524`) is embedded in the decoded
payload.

---

## 6. Delivery

The records map one-to-one onto the bootloader's UART download protocol (38400 8N1): record 0
is sent first (magic + key), then each data record is sent **raw/unframed**, one per `S` prompt
from the box, which verifies the checksum, de-obfuscates the 128 bytes, and programs them. The
box de-obfuscates (the key ships in record 0). There is no whole-image signature or hash — only
the per-record checksum and the record-0 magic — so a correctly repacked image is accepted.

For the full handshake and timing see `hiworld-iap-update-sequence.md`; for the bootloader
internals see `hiworld-bootloader-analysis.md`.

---

## 7. Sample reference values

| file | size | N | key (rec0[132:136]) | payload | image span |
|---|---|---|---|---|---|
| `2429-H1N4JPF20A-220524.iap` | 34 952 | 257 | `68 cc e3 3e` | 32 768 B | `0x08002000`–`0x08009FC3` |
| `2883-H1N4MT060A-230712.iap` | 45 968 | 338 | `b6 e5 8b 6a` | 43 136 B | `0x08002000`–`0x0800C867` |
| `2948-H1H2JP063A-231006.iap` | 56 440 | 415 | `e5 52 52 f3` | 52 992 B | `0x08002000`–`0x0800EEE4` |

---

## 8. Reference implementation

Maintained tool: `hiworld/iaptool.py` (`info` / `unpack` / `pack` / `verify`). Minimal form:

```python
import sys, struct, os

def unpack(blob):
    assert len(blob) % 136 == 0, "not a multiple of 136"
    n = len(blob) // 136
    key = blob[132:136]
    out, errs = bytearray(), []
    for k in range(1, n):
        rec = blob[136*k : 136*(k+1)]
        seq, pct, last = struct.unpack('<HBB', rec[128:132])
        chk  = struct.unpack('<I', rec[132:136])[0]
        calc = sum(struct.unpack('<33I', rec[:132])) & 0xFFFFFFFF
        if (seq != k or pct != (k*100)//(n-1)
                or last != (1 if k == n-1 else 0) or chk != calc):
            errs.append(k)
        out += bytes((rec[i] - i - key[i & 3]) & 0xFF for i in range(128))
    return blob[:136], key, bytes(out), errs          # payload loads at 0x08002000

def pack(hdr136, payload):
    assert len(payload) % 128 == 0 and len(hdr136) == 136
    key = hdr136[132:136]
    n = len(payload)//128 + 1
    out = bytearray(hdr136)                            # record 0 reused verbatim
    for k in range(1, n):
        p = payload[128*(k-1) : 128*k]
        rec = bytearray((p[i] + i + key[i & 3]) & 0xFF for i in range(128))
        rec += struct.pack('<HBB', k, (k*100)//(n-1), 1 if k == n-1 else 0)
        rec += struct.pack('<I', sum(struct.unpack('<33I', bytes(rec))) & 0xFFFFFFFF)
        out += rec
    return bytes(out)
```

Repacking reuses record 0 verbatim, so the magic and key carry over and the per-record
checksums are recomputed. To build a header from scratch, all 16 target-magic bytes (§2.2) must
match the target box.
