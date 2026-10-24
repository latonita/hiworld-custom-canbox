# Removing read protection (RDP L1 → L0) over SWD

Plan for permanently unlocking the canbox N32L406CBL7 via the Pico debugprobe, then restoring
bootloader + param page + app. **Not yet executed** — analysis only (2026-10-22).

Sources: `n32l406/EN_UM_N32L40x_Series_User_Manual_V2.6.0.pdf` §2.2.1.4.3, §2.2.1.7–2.2.1.9,
Tables 2-4/2-5/2-6, FLASH and RCC register maps; info-page dump
`n32-bootrom/infopage_1FFFF800_2k.bin`; bootloader dump `hiworld/extracted/bootloader_0x08000000_8k.bin`.
Related: `hiworld-hardware.md` §3, `hiworld-bootloader-analysis.md`.

**Verdict:** no blocker. Fully recoverable as long as `0xCC` never lands in RDP2. Four things
must be handled (below); two of them fail silently.

## 1. Current option bytes (decoded)

Layout per word (Table 2-4): `[31:24]` complement, `[23:16]` byte, `[15:8]` complement, `[7:0]` byte.

| Address | Raw (LE bytes) | Bytes | Meaning |
|---|---|---|---|
| `0x1FFFF800` | `ff 00 ff 00` | RDP1=**FF**, USER=FF | L1 (RDP1 ≠ A5). USER=FF → SW watchdog, no reset on stop2/standby |
| `0x1FFFF804` | `ff 00 ff 00` | Data0=FF, Data1=FF | unused |
| `0x1FFFF808` | `ff 00 ff 00` | WRP0=FF, WRP1=FF | no write protection |
| `0x1FFFF80C` | `ff 00 ff 00` | WRP2=FF, WRP3=FF | no write protection |
| `0x1FFFF810` | `ff 00 8f 70` | RDP2=FF, **USER2=0x8F** | not L2. BOR_LEV=000 (1.64 V); nBOOT0=nBOOT1=nSWBOOT0=1 → boot per BOOT0 pin |

USER2 bits: [7] res, [6:4] BOR_LEV (000=1.64 V, 001=2.10, 010=2.30, 011=2.60, 100=2.90, other
**reserved**), [3] res, [2] nSWBOOT0, [1] nBOOT1, [0] nBOOT0.

## 2. Gotchas

1. **USER2 / BOR level — silent.** The unlock erases the whole option page. Erased USER2 = 0xFF →
   BOR_LEV=111 = *reserved*. Boot bits stay identical, but the brown-out level becomes undefined.
   Reprogram `0x1FFFF810` with RDP2=0xFF, USER2=0x8F (hardware writes the complements). Verify
   `FLASH_OB2` (0x40022018) BOR_LEV[10:8]=0 after reset.
2. **HSI is off after reset — silent.** The chip resets onto MSI; the manual requires HSI on for any
   flash program/erase (the vendor bootloader has `EnableHsiClock` for exactly this). Halted over
   SWD, set `RCC_CTRL.HSIEN` (0x40021000 bit 0) and wait for HSIRDF (bit 1) before touching FLASH_CTRL.
3. **RDP2 is the only brick.** `RDP2=0xCC` (low byte of `0x1FFFF810`) = L2: SWD permanently dead,
   protection level frozen. Only ever write that word with low byte `0xFF`.
4. **App depends on the bootloader.** App runs on the bootloader's vector table (forwarded
   handlers, VTOR=0x08000000), is entered via the veneer at `0x08002202`, and is launched only if
   the valid-app markers exist. The 8 KB dump already contains them:
   `0x08001800`=0x19780217, `0x08001F80`=0xA4473291, `0x08001F84`=0x01043749.

## 3. Checked, not blockers

- **No re-lock.** Bootloader uses KEY1/KEY2 only for `FLASH_KEY` (IAP unlock); never OPTKEY, no
  `0x1FFFF800` literal. Stock app's RDP/option-byte routines are dead code (see
  `hiworld-iap-flashing-flow.md`); the rebuild has none.
- **No UID / anti-clone binding.** Neither bootloader nor stock app references `0x1FFFF7E0..7FF`.
- **SWD may do this at L1.** Table 2-6: option-byte area is R/W/E from JTAG/SWD at L1. Main-flash
  page program/erase from the debugger is blocked only until L0.
- **Other option bytes:** USER/WRP/Data erase to FF = same as today.
- **Interruption is recoverable.** Power loss after option erase, before the A5 write → still L1
  (erased = FF). During the internal mass erase → still L1 with partial flash; redo.

## 4. Sequence (pyocd `-t cortex_m`, raw register writes)

FLASH base `0x40022000`: KEY +0x04, OPTKEY +0x08, STS +0x0C (BUSY bit 0), CTRL +0x10, OB2 +0x18,
OB +0x1C. CTRL bits: PG 0, PER 1, MER 2, OPTPG 4, OPTER 5, START 6, LOCK 7, OPTWE 9.
KEY1=`0x45670123`, KEY2=`0xCDEF89AB`.

0. **Before anything:** re-run the bootloader dumper (`hiworld/dumper/dumper2.s` via IAP) and
   confirm sha256 is still `60c8a757be2e559c09bf30ea0a973766cf9251e8e3c0d2fab8f1412cda528fe0`.
   The mass erase destroys the only other copy. Also keep `parampage_0x0801F800_2k.bin`.
1. Connect + halt retry loop while power-cycling; write `DBGMCU_CR` (0xE0042004) = 0x7.
2. HSI on: `RCC_CTRL |= 1`, wait `RCC_CTRL & 2`.
3. Unlock: KEY1, KEY2 → `FLASH_KEY`; KEY1, KEY2 → `FLASH_OPTKEY`; check `CTRL.OPTWE`=1.
4. Erase option page: `CTRL |= OPTER`; `CTRL |= START`; wait BUSY=0; `CTRL &= ~OPTER`.
5. RDP1: `CTRL |= OPTPG`; write `0x00FF00A5` → `0x1FFFF800`; wait BUSY=0 (internal mass erase,
   slow). Hardware stores A5/5A.
6. USER2: still OPTPG; write `0x008F00FF` → `0x1FFFF810`; wait BUSY=0; `CTRL &= ~OPTPG`.
   **Double-check the low byte is FF, never CC.**
7. Power-cycle. Verify: `FLASH_OB` RDPRT1 (bit 1)=0, OBERR (bit 0)=0, RDPRT2 (bit 31)=0;
   `FLASH_OB2` BOR_LEV=0; flash at 0x08000000 now reads (all FF).
8. Re-do steps 1–3 (FLASH_KEY only). Program with `CTRL |= PG` + word writes, wait BUSY each word:
   bootloader dump → `0x08000000` (8 KB, includes markers); param page → `0x0801F800` (2 KB).
   Read back and compare.
9. App: raw `build/app.bin` → `0x08002000` over SWD, or the usual `tools/iap_upload.py` over
   UART (re-stamps markers itself). Liveness check per `BUILD.md`.

After this the chip stays L0: flash readable/programmable over SWD, flash breakpoints work, and the
Hiworld head-unit updater still works because the bootloader is restored unchanged.
