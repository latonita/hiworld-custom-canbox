#!/usr/bin/env python3
"""
Hiworld canbox IAP uploader (host side) — streams a .iap to the 8 KB bootloader
over the radio UART, driving the U / 19780217 / B / S / R / E protocol.

Reverse-engineered flow (see hiworld-iap-update-sequence.md):
  reset -> bootloader emits 'U' (0x55)
  host  -> begin magic 19 78 02 17
  box   -> 'B' then 'S'            ('S' = send me the next record)
  loop  -> host sends one 136-byte record per 'S' (record 0 header first)
  box   -> 'E' after the last (last-flag) record, then jumps to the app
  box   -> 'R' = reject/abort (no retransmission exists)

Reset entry:
  --reset powercycle (default, SAFEST): you power-cycle the box on the prompt.
      A real power-on also clears any latched CPU debug-enable state, which is
      REQUIRED — the bootloader cannot erase/program flash while the core is in
      debug mode (RDP-L1 rule). Use this after any pyOCD/SWD halt session.
  --reset nrst: pulse nRST via the Pico (fast, no hands) — ONLY safe if the chip
      is not in a latched debug state; otherwise the flash write silently fails.

Verify (--verify): after a good upload, read back the 17-byte version string the
  app copies into SRAM at init (SRAM is RDP-exempt, readable over SWD even while
  the app runs). Proves the new image booted. Uses connect_mode=attach so it does
  not reset/halt the running app.
  The address is resolved from build/<protocol>/app.elf (symbol g_versionString),
  rather than hardcoded, because the rebuild no longer pins its state to the stock
  firmware's absolute SRAM map. Which ELF is picked by matching the version string
  in the .iap's own name (build/canupdate-<version>.iap) against the built images.
  Override with --elf or --version-addr; with neither resolvable it falls back to
  the stock 0x20001B86, which is right only for a factory image.

Usage:
  python iap_upload.py <file.iap> [--port COM17] [--reset powercycle|nrst] [--verify]
"""
import os, subprocess, sys, time, argparse

REC = 136
MAGIC = bytes.fromhex("19780217")
U, B, S, R, E = 0x55, 0x42, 0x53, 0x52, 0x45
NAME = {U: 'U', B: 'B', S: 'S', R: 'R', E: 'E'}
VERSION_LEN = 17

# Where the running app keeps its version string. This used to be hardcoded to
# 0x20001B86, because the rebuild reproduced the stock firmware's absolute SRAM
# map. It does not any more -- the state is ordinary .bss and the linker places
# it -- so the address is resolved from the build instead. See the note on
# g_versionString in app/include/ram_map.hpp.
VERSION_SYMBOL = "g_versionString"
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_elf_for(image):
    """Guess which build/<protocol>/app.elf goes with the .iap being flashed.

    Getting this wrong is not cosmetic: --verify reads the version string back over
    SWD and compares it, and the two protocols deliberately carry DIFFERENT version
    strings, so the wrong ELF reports a mismatch on a perfectly good flash.

    The build driver packs build/canupdate-<version>.iap, so the version string is in
    the image's own filename. Rather than mapping version -> protocol from a table
    here -- a second copy of app_config.hpp's literals, free to drift -- the match is
    made against the images themselves: the right ELF is the one whose .bin actually
    contains those bytes. That also survives the file being renamed by hand, as long
    as the version is still somewhere in the name.

    Falls back to the single protocol directory that exists, when there is
    exactly one, and otherwise returns None so the caller can ask for --elf.
    """
    stem = os.path.splitext(os.path.basename(image or ""))[0]

    if stem.startswith("canupdate-"):
        want = stem[len("canupdate-"):].encode("ascii", "ignore")
        build = os.path.join(_ROOT, "build")
        if want and os.path.isdir(build):
            for name in sorted(os.listdir(build)):
                elf = os.path.join(build, name, "app.elf")
                binf = os.path.join(build, name, "app.bin")
                if not os.path.isfile(elf) or not os.path.isfile(binf):
                    continue
                with open(binf, "rb") as f:
                    if want in f.read():
                        return elf

    # Pre-rename layout: build/app-<protocol>.iap. Old containers still flash fine.
    if stem.startswith("app-"):
        cand = os.path.join(_ROOT, "build", stem[len("app-"):], "app.elf")
        if os.path.isfile(cand):
            return cand

    found = []
    build = os.path.join(_ROOT, "build")
    if os.path.isdir(build):
        for name in sorted(os.listdir(build)):
            cand = os.path.join(build, name, "app.elf")
            if os.path.isfile(cand):
                found.append(cand)
    if len(found) == 1:
        return found[0]

    # Pre-split layout, still honoured if someone has an old tree.
    legacy = os.path.join(_ROOT, "build", "app.elf")
    return legacy if os.path.isfile(legacy) else None
# The stock image's address, used only to verify a factory box.
STOCK_VERSION_RAM = 0x20001B86


def resolve_version_addr(elf, explicit=None):
    """Address of the version string, from `nm` on the ELF we just flashed.

    Returns None if it cannot be determined, which is not fatal: the caller
    falls back to the stock address, which is right when verifying a box that is
    still running factory firmware.
    """
    if explicit is not None:
        return explicit
    if not elf or not os.path.exists(elf):
        return None
    cross = os.environ.get("CROSS", "D:/IDE/arm-none-eabi-gcc/bin/arm-none-eabi-")
    try:
        out = subprocess.run([cross + "nm", elf], capture_output=True, text=True)
    except OSError:
        return None
    if out.returncode != 0:
        return None
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == VERSION_SYMBOL:
            return int(parts[0], 16)
    return None


def pico_nrst_pulse(ms=60):
    from pyocd.probe.pydapaccess import DAPAccess
    devs = DAPAccess.get_connected_devices()
    if not devs:
        raise RuntimeError("no CMSIS-DAP probe found for nRST")
    d = devs[0]; d.open(); d.connect()
    d.assert_reset(True); time.sleep(ms / 1000.0); d.assert_reset(False)
    d.disconnect(); d.close()


def upload(port, iap_path, reset_mode):
    blob = open(iap_path, "rb").read()
    if len(blob) % REC:
        print(f"ERROR: {iap_path} is {len(blob)} B, not a multiple of {REC}")
        return False
    n = len(blob) // REC
    records = [blob[i * REC:(i + 1) * REC] for i in range(n)]
    print(f"{iap_path}: {len(blob)} B = {n} records (1 header + {n-1} data)")

    import serial
    ser = serial.Serial(port, 38400, timeout=0.02, bytesize=8, parity='N', stopbits=1)
    ser.reset_input_buffer()

    # --- trigger a cold boot and catch U ---
    if reset_mode == "nrst":
        print("reset: pulsing nRST via Pico ...")
        pico_nrst_pulse()
        u_timeout = 5.0
    else:
        print("\n>>> POWER-CYCLE THE BOX NOW: switch OFF, wait ~2 s, switch ON <<<\n")
        u_timeout = 25.0

    t0 = time.time(); seen = bytearray(); got_u = False
    while time.time() - t0 < u_timeout:
        b = ser.read(64)
        if b:
            seen += b
            if U in b:
                got_u = True
                break
    if not got_u:
        print(f"FAIL: never saw U (0x55). bytes seen: {bytes(seen).hex(' ') or '(none)'}")
        ser.close(); return False
    print(f"got U  (+{time.time()-t0:.3f}s)  ->  sending begin magic {MAGIC.hex(' ')}")
    ser.write(MAGIC); ser.flush()

    # --- stream records: one per S, until E (done) or R (abort) ---
    idx = 0; start = time.time(); last = time.time()
    while time.time() - start < 120:
        b = ser.read(64)
        if not b:
            if time.time() - last > 3.0:
                print(f"FAIL: stalled waiting for S (sent {idx}/{n})")
                ser.close(); return False
            continue
        for x in b:
            last = time.time()
            if x == S:
                if idx < n:
                    ser.write(records[idx]); ser.flush()
                    idx += 1
                    if idx % 32 == 0 or idx == n:
                        print(f"  sent {idx:3d}/{n}  ({idx*100//n:3d}%)")
                # extra S past the last record: ignore, wait for E
            elif x == E:
                dt = time.time() - start
                ok = idx >= n
                print(f"got E  ->  transfer complete: {idx}/{n} records in {dt:.1f}s. "
                      f"Bootloader hands off to app.")
                ser.close(); return ok
            elif x == R:
                print(f"FAIL: got R (reject/abort) after {idx} records. No retransmit exists.")
                ser.close(); return False
            elif x == B:
                pass  # 'begin accepted' marker, informational
            else:
                print(f"  (unexpected byte 0x{x:02X} '{NAME.get(x,'?')}')")
    print(f"FAIL: overall timeout (sent {idx}/{n})")
    ser.close(); return False


def verify_version(addr):
    """Read the version string from SRAM over SWD (app running, attach mode)."""
    from pyocd.core.helpers import ConnectHelper
    import logging; logging.disable(logging.WARNING)
    time.sleep(0.5)  # let the app finish init and copy the string
    try:
        with ConnectHelper.session_with_chosen_probe(
                target_override="cortex_m",
                options={"connect_mode": "attach", "frequency": 1000000}) as s:
            data = bytes(s.target.read_memory_block8(addr, VERSION_LEN))
    except Exception as e:
        print(f"verify: SWD read failed ({e}). "
              f"If 'No ACK', the app may be sleeping — give it CAN or power-cycle and retry.")
        return None
    asc = ''.join(chr(c) if 32 <= c < 127 else '.' for c in data)
    print(f"verify: SRAM 0x{addr:08X} = {data.hex(' ')}  |{asc}|")
    return data


def main():
    ap = argparse.ArgumentParser(description="Hiworld canbox IAP uploader")
    ap.add_argument("iap", help="path to the .iap file to upload")
    ap.add_argument("--port", default="COM17", help="serial port (default COM17)")
    ap.add_argument("--reset", choices=["powercycle", "nrst"], default="powercycle")
    ap.add_argument("--verify", action="store_true",
                    help="after upload, read the version string from SRAM over SWD")
    ap.add_argument("--elf", default=None,
                    help="ELF to resolve the version-string address from "
                         "(default: the build/<protocol>/app.elf matching the "
                         ".iap being flashed)")
    ap.add_argument("--version-addr", type=lambda v: int(v, 0), default=None,
                    help="override the SRAM address of the version string")
    a = ap.parse_args()
    if a.elf is None:
        a.elf = default_elf_for(a.iap)

    ok = upload(a.port, a.iap, a.reset)
    print("\n==== UPLOAD " + ("OK ====" if ok else "FAILED ===="))
    if ok and a.verify:
        addr = resolve_version_addr(a.elf, a.version_addr)
        if addr is None:
            addr = STOCK_VERSION_RAM
            print(f"verify: could not resolve {VERSION_SYMBOL} from {a.elf}; "
                  f"falling back to the stock address 0x{addr:08X}. "
                  f"That is correct for a factory image, but for a rebuilt one "
                  f"it will read the wrong bytes -- build first, or pass "
                  f"--version-addr.")
        verify_version(addr)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
