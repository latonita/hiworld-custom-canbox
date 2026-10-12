---
name: iap-driving-bench-setup
description: Validated bench rig to reset the canbox and drive its IAP bootloader over UART - FTDI on COM17 + Pico nRST, all in software
metadata:
  type: project
---

Working, non-destructive rig to talk to the Hiworld canbox bootloader, validated
2026-10-10. See [[n32-rdp-levels]], [[canbox-mcu-is-nations-n32]], and the docs
hiworld-iap-update-sequence.md / hiworld-iap-format.md.

**Hardware**
- **Box radio UART via a plain FTDI (FT232) on COM17**, wired to the board's
  5V radio TX/RX signals (the transistor-level-shifted side, NOT the MCU QFP).
  Confirmed **non-inverting** and clean at **38400 8N1** - no soldering to PA2/PA3
  needed. FTDI RX must sit on the box's TX (box->radio) line; common ground required.
- **nRST driven by the Raspberry Pi Pico** (debugprobe) GP1, via the 5-pad SWD/ISP
  header. Reset entirely in software (no manual power-cycle).

**Reset in software (Python):**
```
from pyocd.probe.pydapaccess import DAPAccess
d=DAPAccess.get_connected_devices()[0]; d.open(); d.connect()
d.assert_reset(True); time.sleep(0.06); d.assert_reset(False)
d.disconnect(); d.close()   # releases SWD lines; box runs free
```
The Pico here is nRST-only; its UART (GP4/5) is NOT used (FTDI handles UART).

**Confirmed IAP entry handshake (bidirectional UART proven):**
- reset -> bootloader emits `U` (0x55) at ~120 ms after nRST release.
- answer within the window with begin magic `19 78 02 17`.
- bootloader replies `B` (0x42) then `S` (0x53) immediately (same read, ~0 ms gap)
  = ready for record 0. **Not erase-on-begin** - app stays intact.
- if `U` is not answered, bootloader sends `E` (0x45) at ~788 ms and jumps to app.
- IAP window `U`->`E` ~= **0.67 s** (plenty for a software responder over USB-serial).

**Why this matters:** this is the delivery path for a flash-resident **bootloader
dumper** (read 0x08000000-0x08001FFF, stream out UART) - the only way to recover the
8 KB vendor bootloader under RDP-L1 (see [[n32-rdp-levels]] §3.1). Build the dumper as
a raw image based at 0x08002000, pack with hiworld/iaptool.py, deliver via this rig.
Run the dumper with SWD DETACHED (reads only work for flash-resident code in non-debug
mode) - use power-cycle or a pre-release nRST, not an attached SWD session, when the
dumper actually reads flash.
