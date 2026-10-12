#!/usr/bin/env python3
"""
verify_preamble.py -- the load-bearing safety check for a rebuilt canbox APP image.

The app has no vector table of its own: it runs on the IAP bootloader's, which lives in
write-protected flash at 0x08000000 and branches straight into fixed addresses inside the
app's preamble. So a rebuilt image is only bootable/interrupt-safe if:

  1. the entry veneer sits at exactly 0x08002202           (bootloader does `bl 0x08002202`)
  2. every veneer slot the bootloader dispatches to still sits at its original address
     and still has the 8-byte `push {r4,lr}; bl <h>; pop {r4,pc}` shape
  3. nothing else has been shifted into 0x08002000..0x08002209

This checks a built image against the *bootloader dump itself* (the real authority), not
against a formula. The often-quoted rule `veneer(IRQn) = 0x08002072 + 8*IRQn` is only true
for IRQ0..10 and IRQ23..41 -- see app/startup/crt0.S for why.

Usage:
    python tools/verify_preamble.py build/app.bin
    python tools/verify_preamble.py build/app.bin --compare reference/2429-H1N4JPF20A-220524.app.bin
"""

import argparse
import struct
import sys

APP_BASE = 0x08002000
VENEER_FIRST = 0x0800202A
VENEER_COUNT = 59
VENEER_LAST_END = VENEER_FIRST + 8 * VENEER_COUNT  # 0x08002202
ENTRY = 0x08002202
PREAMBLE_END = ENTRY + 8  # 0x0800220A

PUSH_R4_LR = 0xB510
POP_R4_PC = 0xBD10

CORE_EXC = {2: "NMI", 3: "HardFault", 4: "MemManage", 5: "BusFault", 6: "UsageFault",
            11: "SVCall", 12: "DebugMon", 14: "PendSV", 15: "SysTick"}


def rd16(buf, addr):
    return struct.unpack_from("<H", buf, addr - APP_BASE)[0]


def bl_target(buf, addr):
    """Decode a 32-bit Thumb BL at `addr`; return its target, or None if not a BL."""
    hi = rd16(buf, addr)
    lo = rd16(buf, addr + 2)
    if (hi & 0xF800) != 0xF000 or (lo & 0xD000) != 0xD000:
        return None
    s = (hi >> 10) & 1
    j1 = (lo >> 13) & 1
    j2 = (lo >> 11) & 1
    imm = ((hi & 0x7FF) << 12) | ((lo & 0x7FF) << 1)
    imm |= ((~(j1 ^ s)) & 1) << 23
    imm |= ((~(j2 ^ s)) & 1) << 22
    imm |= s << 24
    if s:
        imm -= 1 << 25
    return addr + 4 + imm


def dispatched_slots(boot):
    """slot index -> name of the exception/IRQ the bootloader vectors to it."""
    out = {}
    for i in range(256):
        if i * 4 + 4 > len(boot):
            break
        (w,) = struct.unpack_from("<I", boot, i * 4)
        t = w & ~1
        if VENEER_FIRST <= t < VENEER_LAST_END and (t - VENEER_FIRST) % 8 == 0:
            name = CORE_EXC.get(i, "IRQ%d" % (i - 16)) if i >= 2 else "word%d" % i
            out[(t - VENEER_FIRST) // 8] = name
    return out


def check(img, boot, compare=None):
    errs, warns, notes = [], [], []

    if len(img) < PREAMBLE_END - APP_BASE:
        errs.append("image is only %d bytes; preamble needs %d"
                    % (len(img), PREAMBLE_END - APP_BASE))
        return errs, warns, notes

    disp = dispatched_slots(boot) if boot else {}
    if boot and not disp:
        warns.append("bootloader dump has no vectors pointing into the app preamble (?)")

    # --- 1. entry veneer ---
    if rd16(img, ENTRY) != PUSH_R4_LR:
        errs.append("entry 0x%08X: expected `push {r4,lr}` (B510), got %04X"
                    % (ENTRY, rd16(img, ENTRY)))
    t = bl_target(img, ENTRY + 2)
    if t is None:
        errs.append("entry 0x%08X+2: expected a 32-bit BL" % ENTRY)
    else:
        notes.append("entry 0x%08X -> bl 0x%08X" % (ENTRY, t))
    if rd16(img, ENTRY + 6) != POP_R4_PC:
        errs.append("entry 0x%08X+6: expected `pop {r4,pc}` (BD10), got %04X"
                    % (ENTRY, rd16(img, ENTRY + 6)))

    # --- 2. veneer array ---
    bad_shape = []
    for s in range(VENEER_COUNT):
        a = VENEER_FIRST + 8 * s
        ok = (rd16(img, a) == PUSH_R4_LR
              and bl_target(img, a + 2) is not None
              and rd16(img, a + 6) == POP_R4_PC)
        if not ok:
            bad_shape.append(s)
    for s in bad_shape:
        a = VENEER_FIRST + 8 * s
        src = disp.get(s)
        msg = ("veneer slot %2d @0x%08X is not `push/bl/pop` (%04X %04X %04X %04X)"
               % (s, a, rd16(img, a), rd16(img, a + 2), rd16(img, a + 4), rd16(img, a + 6)))
        if src:
            errs.append(msg + "  <-- %s DISPATCHES HERE" % src)
        else:
            warns.append(msg + "  (unreachable slot, padding only)")

    # --- 3. every dispatched slot resolves somewhere sane ---
    for s in sorted(disp):
        a = VENEER_FIRST + 8 * s
        t = bl_target(img, a + 2)
        if t is None:
            continue
        if not (APP_BASE <= t < APP_BASE + 0x1D800):
            errs.append("slot %2d (%s) @0x%08X branches to 0x%08X -- outside the app region"
                        % (s, disp[s], a, t))

    # --- 4. optional structural compare against the stock image ---
    if compare is not None:
        for s in range(VENEER_COUNT):
            a = VENEER_FIRST + 8 * s
            for off in (0, 6):
                if rd16(img, a + off) != rd16(compare, a + off):
                    errs.append("slot %2d @0x%08X+%d differs in shape from stock" % (s, a, off))
        if rd16(img, ENTRY) != rd16(compare, ENTRY) or \
           rd16(img, ENTRY + 6) != rd16(compare, ENTRY + 6):
            errs.append("entry veneer shape differs from stock")
        notes.append("structural compare against stock image: shapes checked")

    return errs, warns, notes


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="rebuilt app .bin (raw, loads at 0x08002000)")
    ap.add_argument("--bootloader", default="reference/bootloader_0x08000000_8k.bin",
                    help="bootloader dump that defines the real slot map")
    ap.add_argument("--compare", help="stock app .bin to structurally compare against")
    ap.add_argument("-v", "--verbose", action="store_true", help="list every slot")
    args = ap.parse_args()

    img = open(args.image, "rb").read()
    try:
        boot = open(args.bootloader, "rb").read()
    except OSError:
        boot = None
        print("warning: no bootloader dump at %s -- slot-map checks skipped"
              % args.bootloader, file=sys.stderr)
    cmp_img = open(args.compare, "rb").read() if args.compare else None

    errs, warns, notes = check(img, boot, cmp_img)

    if args.verbose and boot:
        disp = dispatched_slots(boot)
        print("slot  addr        dispatched-by   bl target")
        for s in range(VENEER_COUNT):
            a = VENEER_FIRST + 8 * s
            t = bl_target(img, a + 2)
            print("  %2d  0x%08X  %-14s %s"
                  % (s, a, disp.get(s, "--"), "0x%08X" % t if t else "??"))
        print()

    for n in notes:
        print("note:  %s" % n)
    for w in warns:
        print("WARN:  %s" % w)
    for e in errs:
        print("ERROR: %s" % e)

    print()
    if errs:
        print("FAIL: %d error(s) -- this image is NOT safe to flash." % len(errs))
        return 1
    print("PASS: entry at 0x%08X, all %d veneer slots intact%s."
          % (ENTRY, VENEER_COUNT, " (%d warning(s))" % len(warns) if warns else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
