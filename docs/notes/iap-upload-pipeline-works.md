---
name: iap-upload-pipeline-works
description: End-to-end canbox firmware modify+flash pipeline is PROVEN working - edit byte, iaptool repack, iap_upload.py over COM17+Pico nRST, verify via SRAM read
metadata:
  type: project
---

Validated 2026-10-10. We can now author, flash and verify custom canbox firmware
non-destructively. See [[iap-driving-bench-setup]], [[n32-rdp-levels]].

**Tool: `hiworld/iap_upload.py`** - host-side IAP uploader.
`python iap_upload.py <file.iap> --port COM17 --reset nrst --verify`
Drives reset -> U -> 19780217 -> B/S -> stream 257 records (one per S) -> E.
`--reset nrst` pulses nRST via the Pico (hands-free, ~0.12s to U, ~15s to flash).
`--reset powercycle` waits for a manual power-cycle (use after any pyOCD HALT
session - a real power-on clears the CPU debug-enable latch; the bootloader
cannot write flash in debug mode under RDP-L1). Low-level nRST pulses do NOT set
that latch, so nrst mode is fine as long as no halt session ran since last power-on.
`--verify` reads the 17-byte version string from SRAM 0x20001B86 (RDP-exempt,
connect_mode=attach, app left running).

**Repack:** `hiworld/iaptool.py` pack recomputes all per-record checksums; a
one-byte payload edit + pack = a valid .iap. Confirmed round-trip identical.

**Proven results:**
- Stage 1: uploaded stock 2429-H1N4JPF20A-220524.iap unchanged -> E -> app booted,
  SRAM shows "H1N4JPF20A-220524". Transport works.
- Stage 2: patched payload 0x6e18+16 '4'->'5' (date 220524->220525), repacked,
  uploaded -> SRAM shows "H1N4JPF20A-220525". Modified code runs on the chip.
- Restored stock afterwards (box pristine).

**Verification without CAN:** the app copies its version string from flash
0x08008e18 to SRAM 0x20001B86 at init (before the phase-1 CAN wait), so an SRAM
read confirms both "app booted" and "which image" with no CAN bus. The UART
version poll `AA 55 01 FF F0 EF` also returns it but only in phase 2 (needs >=3
CAN frames) - useless on a bare bench.

**Unbrickable loop holds:** bootloader emits U on every boot before touching the
app; a bad app image is always re-flashable. Only a mass-erase (RDP unlock) breaks
this - never do it; the 8 KB bootloader exists in no .iap.
