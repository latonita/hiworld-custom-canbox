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

Verify (--verify): after a good upload, read the 17-byte version string the app
  copies to SRAM 0x20001B86 at init (SRAM is RDP-exempt, readable over SWD even
  while the app runs). Proves the new image booted. Uses connect_mode=attach so
  it does not reset/halt the running app.

Usage:
  python iap_upload.py <file.iap> [--port COM17] [--reset powercycle|nrst] [--verify]
"""
import sys, time, argparse

REC = 136
MAGIC = bytes.fromhex("19780217")
U, B, S, R, E = 0x55, 0x42, 0x53, 0x52, 0x45
NAME = {U: 'U', B: 'B', S: 'S', R: 'R', E: 'E'}
VERSION_RAM = 0x20001B86
VERSION_LEN = 17


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


def verify_version():
    """Read the version string from SRAM over SWD (app running, attach mode)."""
    from pyocd.core.helpers import ConnectHelper
    import logging; logging.disable(logging.WARNING)
    time.sleep(0.5)  # let the app finish init and copy the string
    try:
        with ConnectHelper.session_with_chosen_probe(
                target_override="cortex_m",
                options={"connect_mode": "attach", "frequency": 1000000}) as s:
            data = bytes(s.target.read_memory_block8(VERSION_RAM, VERSION_LEN))
    except Exception as e:
        print(f"verify: SWD read failed ({e}). "
              f"If 'No ACK', the app may be sleeping — give it CAN or power-cycle and retry.")
        return None
    asc = ''.join(chr(c) if 32 <= c < 127 else '.' for c in data)
    print(f"verify: SRAM 0x{VERSION_RAM:08X} = {data.hex(' ')}  |{asc}|")
    return data


def main():
    ap = argparse.ArgumentParser(description="Hiworld canbox IAP uploader")
    ap.add_argument("iap", help="path to the .iap file to upload")
    ap.add_argument("--port", default="COM17", help="serial port (default COM17)")
    ap.add_argument("--reset", choices=["powercycle", "nrst"], default="powercycle")
    ap.add_argument("--verify", action="store_true",
                    help="after upload, read the version string from SRAM over SWD")
    a = ap.parse_args()

    ok = upload(a.port, a.iap, a.reset)
    print("\n==== UPLOAD " + ("OK ====" if ok else "FAILED ===="))
    if ok and a.verify:
        verify_version()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
