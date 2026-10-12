---
name: box-firmware-decompiled
description: "The Hiworld canbox MCU firmware was extracted from .iap and decompiled; key facts about CAN bus, profiles, and the plugin-vs-firmware relationship"
metadata: 
  node_type: memory
  type: project
  originSessionId: 9e579ff9-c20e-4dc4-95f2-3ebdc611031c
  modified: 2026-09-29T20:09:56.520Z
---

The Hiworld JPF2.10 box's MCU firmware (Nations N32L406, Cortex-M4) was extracted from
the `.iap` update file (`hiworld/iaptool.py`, image base `0x08002000`) and decompiled in
Ghidra. Findings that change how to think about the whole system:

- **The box only reads the low-speed body/IHS CAN bus** — auto-detected at boot as
  **83.333 kbit/s OR 125 kbit/s**. It has **no 500 kbit/s / CAN-C path at all.** So even
  powertrain-looking data (RPM) comes off the body bus on the 125 k profile.
- **This unit runs the 83.333 k profile**, which has **no RPM/gear decoder** — that is
  why the captured `0x32` frame is permanently zero. Not a fault.
- **The firmware has EXACTLY TWO CAN profiles** (only two funcs compare CAN IDs):
  `FUN_08009010` = profile 0 = **83.333 k Jeep WK (this car)**; `FUN_08009680` = profile 1 =
  **125 k FCA variant**. Selected by a persisted model byte in the param page (0x0801F800 →
  RAM 0x200011D1), auto-detected at boot (cycles baud until ≥3 frames, persists winner;
  factory default 1). `FUN_08007d44` gates dispatch; `FUN_08007b2c` sets baud (0→83.3k,
  1→125k). **The byte sets BOTH baud and decode map — profiles are baud-locked.**
- **Profile 1 (125k) DOES decode RPM** (CAN 0x211 [6:7] BE, <8000, 1-in-21 → 0x32[2:3]) and
  **gear** (0x20E → 0x32[1]; reverse → 0x72[0] bit2; 0x7FF PRNDL → 0x72[2]), plus textbook
  FCA IDs 0x1E1 steer / 0x244 doors / 0x308 fuel / 0x20B ign / 0x3A3 SWC. **Neither profile
  decodes vehicle SPEED.** Caveat: you CANNOT just switch this box to profile 1 to get RPM —
  it expects a 125 k bus, and the WK body bus is physically 83.333 k, so it would fail to
  read and auto-revert. Real RPM to the radio still needs a MITM/custom box. Full map:
  `hiworld-all-car-profiles.md`; injection surface: `hiworld-radio-injectable.md`.
- **No hardware CAN acceptance filter** (ID 0 / mask 0, filtered in software). The real
  CAN-ID → signal → UART-CMD table is in `canbus-architecture.md` §5·CAN and
  `hiworld-firmware-analysis.md`. These CAN IDs (e.g. doors `0x244`, steering `0x1E1`,
  RPM `0x211` on 125 k) are the **bypass route** — read them directly off the IHS bus.
- **The Android plugin is a SUPERSET of what this box firmware implements.** The box
  only sends `72 A6 73 F0 12 32` and receives `D2 AD`. Plugin handlers for `0x41` radar,
  `0x7D` lights/speed, temps etc. never fire here. Don't trust the plugin's command list
  as the box's actual capability.
- **`0xFF` is an ACK, not a poll** (no `0xFF` handler exists; TX scheduler free-runs the
  table). **You cannot poll this box — only listen.** `0x73` is a permanently-empty
  buffer. Both were standing open questions, now closed.
- **The `.iap` has no signature / integrity check** — a repacked image passes the
  container's own validation. Firmware readout from the chip is likely blocked (RDP), but
  the `.iap` gave us the image anyway.
- **Full decomp (383/383 funcs, 95.1% coverage) settled the flash path:** the APP itself
  only ever programs the **32-byte parameter page at 0x0801F800** (`param_page_save` @
  0x08002d46, the sole caller of the only live `FLASH_ErasePage`/`FLASH_ProgramWord`). It
  has **no receive-and-flash path** — it never writes the app region or bootloader, and the
  `.iap` container logic (136-B stride, `key[i&3]` deob, per-record checksum) appears
  **nowhere** in the app. UART `0xE0` does NOT flash: it hangs in `while(1){WWDG_CR=0xBF}`
  (`wwdg_force_reset` @ 0x08002e2e) → immediate watchdog reset, **no auth, no update flag
  written**. So the `.iap` is parsed & written **only by the 8 KB bootloader** (not in the
  image). Mass-erase + all RDP/option-byte routines are present but **dead (0 callers)**.
- **Upload-custom-code verdict:** we can build a container-valid custom `.iap` today, but
  whether the device accepts/runs it hinges on the **bootloader's** validation (signature /
  whole-image check?), which is UNKNOWN. **Decisive next step = SWD dump of the 8 KB
  bootloader @ 0x08000000.** Brick risk lives only in the bootloader's flashing; the app
  provably never touches the recovery path. Report: `hiworld-iap-upload.md`; decomp:
  `hiworld/decomp/full/`.

**Why:** these facts overturn several plausible-but-wrong assumptions (that the box could
reach CAN-C, that the plugin's command list = the box's, that 0x32 being zero was a bug).

**How to apply:** for real vehicle data, tap the IHS bus at 83.333/125 k, not CAN-C.
Related: [[jeep-profile-is-jpf2]], [[canbox-is-translating-gateway]], [[hiworld-frame-format]].
