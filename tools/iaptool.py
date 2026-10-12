#!/usr/bin/env python3
"""
Hiworld .iap container unpacker / repacker. 
(c) Anton Viktorov, https://github.com/latonita

Format (fully reverse-engineered, round-trips byte-identical on all known samples):

  File = N records of exactly 136 bytes.

  Record 0 (header):
      [0:12]    magic, identical across all samples:
                01 09 07 08 00 02 01 07 91 32 47 a4
      [12:128]  opaque (unsolved; entropy ~6.4, resisted analysis)
      [128:132] unknown, matched no checksum tried
      [132:136] *** the 4-byte obfuscation key, in the clear ***

  Records 1..N-1 (data):
      [0:128]   obfuscated payload chunk
      [128:130] u16 LE sequence number, == record index
      [130]     u8  percent complete = k*100/(N-1)   (for the progress bar)
      [131]     u8  last-record flag
      [132:136] u32 LE checksum

  Checksum: sum of the record's first 132 bytes read as 33 little-endian
  uint32, mod 2**32 — computed over the OBFUSCATED bytes, and including the
  seq/percent/flag word.

  Obfuscation (not encryption — the key ships inside the file):
      plain[i] = (cipher[i] - i - key[i & 3]) & 0xFF
  where i is the 0..127 offset WITHIN each record (resets every record).

  Payload: a raw Cortex-M4 application image based at flash 0x08002000.
  An 8 KB bootloader occupies 0x08000000 and is NOT shipped in the .iap.
  0x0801F800 is used as a parameter page.

  NOTE: the image begins with Thumb code, not a classic vector table — the
  base address was established by pointer closure (the largest in-image flash
  pointer lands within 0-1 bytes of the end of the unpadded image), not by a
  vector-table signature.

  There is NO signature and NO whole-image integrity check. A repacked image
  passes the container's own validation.

Usage:
    python iaptool.py info    <file.iap> [...]
    python iaptool.py unpack  <file.iap> [-o out.bin]
    python iaptool.py pack    <app.bin> <template.iap> [-o new.iap]
    python iaptool.py verify  <file.iap> [...]      # round-trip check
"""
import argparse
import os
import sys

REC = 136
DATA = 128
MAGIC = bytes.fromhex("0109070800020107913247a4")  # 12-byte UNIVERSAL magic (bytes 12-15 are a per-box target tag; the box's own bootloader checks all 16)
FLASH_BASE = 0x08002000


def _records(blob):
    if len(blob) % REC:
        raise ValueError(f"not a multiple of {REC} bytes (got {len(blob)})")
    return [blob[i:i + REC] for i in range(0, len(blob), REC)]


def _checksum(rec):
    return sum(int.from_bytes(rec[j * 4:j * 4 + 4], "little")
               for j in range(33)) & 0xFFFFFFFF


def deobfuscate(chunk, key):
    return bytes((chunk[i] - i - key[i & 3]) & 0xFF for i in range(len(chunk)))


def obfuscate(chunk, key):
    return bytes((chunk[i] + i + key[i & 3]) & 0xFF for i in range(len(chunk)))


def unpack(blob, strict=True):
    """Return (payload, key, problems[])."""
    recs = _records(blob)
    key = recs[0][132:136]
    problems = []
    if recs[0][:len(MAGIC)] != MAGIC:
        problems.append(f"record 0 magic mismatch: {recs[0][:12].hex()}")
    out = bytearray()
    n = len(recs)
    for k, r in enumerate(recs[1:], 1):
        stored = int.from_bytes(r[132:136], "little")
        calc = _checksum(r)
        if stored != calc:
            problems.append(f"record {k}: checksum {stored:#010x} != {calc:#010x}")
        seq = int.from_bytes(r[128:130], "little")
        if seq != k:
            problems.append(f"record {k}: sequence {seq}")
        pct = k * 100 // (n - 1)
        if r[130] != pct:
            problems.append(f"record {k}: percent {r[130]} != {pct}")
        out += deobfuscate(r[:DATA], key)
    if strict and problems:
        raise ValueError("; ".join(problems[:5]))
    return bytes(out), key, problems


def pack(payload, template, key=None):
    """Rebuild an .iap around `payload`, reusing `template`'s header record."""
    trecs = _records(template)
    header = bytearray(trecs[0])
    if key is None:
        key = bytes(header[132:136])
    else:
        header[132:136] = key
    if len(payload) % DATA:
        payload = payload + b"\xff" * (DATA - len(payload) % DATA)
    n = len(payload) // DATA
    out = bytearray(header)
    for k in range(1, n + 1):
        chunk = payload[(k - 1) * DATA:k * DATA]
        rec = bytearray(obfuscate(chunk, key))
        rec += (k).to_bytes(2, "little")
        rec.append(k * 100 // n)
        rec.append(1 if k == n else 0)
        rec += _checksum(rec).to_bytes(4, "little")
        out += rec
    return bytes(out)


def info(path):
    blob = open(path, "rb").read()
    payload, key, problems = unpack(blob, strict=False)
    n = len(blob) // REC
    ident = os.path.basename(path).split("-", 1)[-1].rsplit(".", 1)[0]
    idx = payload.find(ident.encode())
    stripped = len(payload.rstrip(b"\xff\x00"))
    print(f"{os.path.basename(path)}")
    print(f"  container   {len(blob)} B = {REC} x {n} records")
    print(f"  magic       {'OK' if blob[:len(MAGIC)] == MAGIC else 'MISMATCH'}  {blob[:len(MAGIC)].hex()}")
    print(f"  key         {key.hex()}")
    print(f"  payload     {len(payload)} B  ({stripped} B before trailing fill)")
    print(f"  flash span  {FLASH_BASE:#010x}..{FLASH_BASE + stripped:#010x}")
    if idx >= 0:
        print(f"  id string   '{ident}' @ payload {idx:#x} -> flash {FLASH_BASE + idx:#010x}")
    else:
        print(f"  id string   '{ident}' NOT FOUND")
    print(f"  problems    {len(problems)}")
    for p in problems[:5]:
        print(f"              {p}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info");    p.add_argument("files", nargs="+")
    p = sub.add_parser("verify");  p.add_argument("files", nargs="+")
    p = sub.add_parser("unpack");  p.add_argument("file"); p.add_argument("-o", "--out")
    p = sub.add_parser("pack")
    p.add_argument("payload"); p.add_argument("template"); p.add_argument("-o", "--out")

    a = ap.parse_args()

    if a.cmd == "info":
        for f in a.files:
            info(f); print()

    elif a.cmd == "verify":
        bad = 0
        for f in a.files:
            blob = open(f, "rb").read()
            payload, key, problems = unpack(blob, strict=False)
            rebuilt = pack(payload, blob, key)
            ok = rebuilt == blob
            bad += (not ok) or bool(problems)
            print(f"{os.path.basename(f)}: round-trip "
                  f"{'IDENTICAL' if ok else 'DIFFERS'}, {len(problems)} problems")
        sys.exit(1 if bad else 0)

    elif a.cmd == "unpack":
        blob = open(a.file, "rb").read()
        payload, key, problems = unpack(blob, strict=False)
        out = a.out or a.file.rsplit(".", 1)[0] + ".app.bin"
        open(out, "wb").write(payload)
        print(f"wrote {out} ({len(payload)} B), key {key.hex()}, "
              f"{len(problems)} problems, base {FLASH_BASE:#010x}")

    elif a.cmd == "pack":
        payload = open(a.payload, "rb").read()
        template = open(a.template, "rb").read()
        out = a.out or a.payload.rsplit(".", 1)[0] + ".repacked.iap"
        open(out, "wb").write(pack(payload, template))
        print(f"wrote {out}")


if __name__ == "__main__":
    main()
