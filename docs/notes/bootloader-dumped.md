---
name: bootloader-dumped
description: The 8 KB vendor IAP bootloader (0x08000000) was extracted non-destructively via an IAP-delivered SRAM-copy dumper + SWD RAM read - the one piece in no .iap
metadata:
  type: project
---

Achieved 2026-10-10. The 8 KB vendor bootloader at 0x08000000 - absent from every
.iap and unreadable directly over SWD (RDP-L1) - is now in hand. See
[[iap-upload-pipeline-works]], [[n32-rdp-levels]], [[iap-driving-bench-setup]].

**Artifacts (hiworld/extracted/):**
- `bootloader_0x08000000_8k.bin` - 8192 B, sha256
  60c8a757be2e559c09bf30ea0a973766cf9251e8e3c0d2fab8f1412cda528fe0
- `parampage_0x0801F800_2k.bin` - 2 KB config/EEPROM page

**Method (non-destructive, repeatable):** a flash-resident dumper
(hiworld/dumper/dumper2.s) does NOP-sled -> set SP -> memcpy 0x08000000..0x08001FFF
to SRAM 0x20002000 (+ param page to 0x20004000) -> markers -> spin. Packed with
iaptool.py (padded to 32 KB, stock header/key), uploaded with iap_upload.py over
COM17+nRST, then read out of SRAM over SWD (connect_mode=attach - SRAM is
RDP-exempt; the flash read already happened in non-debug mode). Restore stock
after with `iap_upload.py 2429-H1N4JPF20A-220524.iap`.

**App-entry convention:** the bootloader does NOT enter at the image base 0x08002000. It
launches the app with `bl 0x08002202` (a fixed reset veneer: push{r4,lr}; bl __main(0x08002400);
pop -> scatterload -> startup_crt0 -> main), and only if the valid-app marker at 0x08001800 ==
0x19780217 (written by IAP on a good flash). For custom firmware put the entry veneer at offset
0x202 (0x08002202). A NOP-sled dumper works too, but by accident: a bad entry faults and the
bootloader's forwarded fault vectors (HardFault 0x08002033 etc., all in the first 256 B) land in
the sled.

**Bootloader facts (from the dump):** vector table at 0x08000000 - SP=0x20000600,
Reset=0x08000149; it FORWARDS the app's exception handlers (NMI=0x0800202B,
HardFault=0x08002033, ... SVC=0x0800015B, PendSV=0x0800015F, SysTick=0x08000161).
The app runs with VTOR=0x08000000 (never relocates) and SP ~0x20000600, matching the
observed running-app SP. ~43% of the 8 KB is code.

**Key dead-ends learned:** image size didn't matter (full 32 KB vs tiny both fine);
there is NO whole-image integrity check that blocks modified images (Stage-2 byte
patch ran); the blocker was purely the entry offset.

**Ghidra analysis done 2026-10-10** (full report: hiworld-bootloader-analysis.md; 21
functions renamed + plate-commented in the .gpr, saved). Key new findings:
- The bootloader checks a **16-byte target** = **12-byte universal magic**
  (01 09 07 08 00 02 01 07 91 32 47 a4, constant across all boxes) **+ a 4-byte per-box tag**
  [12:16]: [12:14]=ASCII family ("N3" for H1N4, "I3" for H1H2), [14:16]=per-firmware. The 2429
  loader requires 4e 33 14 f2; 2883=4e 33 16 06, 2948=49 33 09 f1. It's a cross-flash guard, NOT
  universal magic - iaptool.py keeps the 12-byte universal check; a per-box flasher matches all 16.
- **Two** begin-magics: `19 78 02 17` (standard) and `20 08 10 20` (begin + baud change).
- Erase is a **bulk app-region page-erase right after record 0** (post magic-check, pre
  data) - not per-record, not on begin-magic (explains the ~0ms B->S gap).
- **No whole-image signature/MAC** - only header magic + per-record 33-word sum; modified
  images run (confirms the custom-fw path).
- After a successful flash the bootloader calls ResetViaWatchdog (WWDG_CR=0xBF) to reboot
  into the new app; then the normal-boot path launches the app via `bl 0x08002202` (marker-gated).
- App runs on the bootloader's vector table (VTOR=0x08000000, MSP=0x20000600); entry is the
  reset veneer at 0x08002202 (-> __main -> scatterload -> main).
