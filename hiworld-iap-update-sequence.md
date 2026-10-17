# Hiworld canbox firmware update (IAP) — sequence

Covers both sides of a `.iap` update: the head-unit radio app and the canbox bootloader.
Container format: `hiworld-iap-format.md`. Bootloader internals: `hiworld-bootloader-analysis.md`.
Radio-side detail: `hiworld-iap-flashing-flow.md`.

---

## 1. Orientation

The head unit does not hand a firmware image to the running canbox application — it tells the
application to reset into the bootloader. One `AA 55 02 E0 00 00 E1` frame reaches
`uart_rx_dispatch`, which calls `wwdg_force_reset()` (an infinite `WWDG_CR = 0xBF` write at
`0x40002C00`); the N32L406 resets in microseconds with nothing buffered and nothing acknowledged.
Control lands in the 8 KB bootloader at `0x08000000`, which speaks an unframed protocol on the
same `/dev/ttyMT3` at the same 38400 8N1: a one-byte `U` greeting, a 4-byte `19 78 02 17` begin
magic from the radio, `B` then `S`, then 257 raw 136-byte `.iap` records streamed strictly
stop-and-wait (each `S` buys one more record), and finally `E`.

The radio ships the de-obfuscation key to the box in the clear inside record 0 and then streams
ciphertext; the bootloader does the `plain[i] = cipher[i] - i - key[i&3]` transform and the flash
programming. There is no retransmission: a 1 Hz UI tick abandons the update if no byte of progress
happened since the previous tick, which makes the unmuted Jeep plugin — still polling the same tty
because the `CAN_BOX_UPDATA` gate has no setter in this app generation — the most likely cause of
random update failures.

---

## 2. Actors

| Actor | What it is | Where its code lives |
|---|---|---|
| `User` | The person tapping "Update box" in the canbus settings UI | — |
| `RadioApp` | `com.kyhero.car.myhost2.activity.BoxUpgradeActivity` — the IAP state machine (`update_temp` 1..4) plus the progress UI. Writes the tty through `iCanbusService.setValue` -> `CanbusMonitor.onDataSend` -> `mOutputStream.write`, unframed | `my-radio-files\CanBus\host\1_025\CanBus2.apk`, `classes3.dex` (OTA twin: `host\2_01`, `classes28.dex`, logically identical) |
| `HU_MCU` | The head unit's own MCU on the keypad/MCU link — not the canbox. Target of `action.mcukey.set` with `mcu_key = {3, 0x0F, n, 0}`; the length byte is dropped and `{0x0F, n, 0x00}` is sent | consumer `com.mediatek.server.NewkeypadMonitor$37` in ROM `services.jar` |
| `FileSrc` | `BoxFileUtil.findBoxUpgradeFile` — walks every storage volume root and `<volume>/CanBus/box/`, takes the first `*.iap` (case-insensitive), reads it whole into `bFileData`. `BoxDownloadService` can fetch it from the vendor server first | `...\CanBus2.apk` -> `...\util\BoxFileUtil.java:161-201` |
| `Plugin` | The Jeep JPF2 decoder plugin, `CanbusPattern_P.SendDataEngine` -> `ToolClass.sendDataToSerial`, polling at ~1 Hz. Hazard — it keeps writing the same fd during the transfer | `CanBus\plugin\3\9\P2_101.apk` (the unit runs the older `P1_501.apk` of the same line) |
| `TTY` | `/dev/ttyMT3`, 38400 8N1 for everything — normal traffic, the reset command and the whole bootloader transfer. The 38401/38403/... suffixes are integer-divided by 100 and are parser-mode selectors, not baud rates. Reads are always into a 64-byte buffer, passed on untrimmed | opened by `Xycanbusinfo.Application.getSerialPort()` in ROM `services.jar` |
| `BoxApp` | The canbox application firmware at `0x08002000` (32768 B). No firmware writer, no 136/128-byte logic, no buffer >= 1 KB, parser rejects `LEN >= 0x38`. Its only flash surface is `param_page_save_0801F800` | `hiworld\extracted\2429-H1N4JPF20A-220524.app.bin`, decompile in `hiworld\decomp\full\` |
| `BootLdr` | The 8 KB bootloader at `0x08000000..0x08001FFF`. Owns the vector table (the app has none and never writes `VTOR`); does the de-obfuscation, checksum verification, flash programming, and the app launch | `hiworld/extracted/bootloader_0x08000000_8k.bin`; analysis in `hiworld-bootloader-analysis.md` |
| `Flash` | Box flash: app region `0x08002000+`, parameter / emulated-EEPROM page `0x0801F800` (2 KB, last page of 128 KB) | — |

File under test: `2429-H1N4JPF20A-220524.iap`, 34952 B = 136 x 257 records (1 header + 256 data
records x 128 B = 32768 B payload), key `68 cc e3 3e`.

---

## 3. Main sequence

```mermaid
sequenceDiagram
    autonumber
    participant User as User
    participant RadioApp as RadioApp / BoxUpgradeActivity
    participant HU_MCU as HU_MCU / head unit own MCU
    participant FileSrc as FileSrc / USB SD or vendor server
    participant Plugin as Plugin / Jeep JPF2 plugin
    participant TTY as TTY / dev ttyMT3 38400 8N1
    participant BoxApp as BoxApp / canbox app 0x08002000
    participant BootLdr as BootLdr / 8 KB loader 0x08000000
    participant Flash as Flash / box flash

    User->>RadioApp: taps Update box
    RadioApp->>FileSrc: findBoxUpgradeFile - first *.iap under volume root or CanBus/box
    opt no local file
        RadioApp->>FileSrc: BoxDownloadService fetch from vendor server
        FileSrc-->>RadioApp: .iap written to CanBus/box
    end
    FileSrc-->>RadioApp: bFileData = 34952 bytes = 136 x 257 records
    RadioApp->>HU_MCU: action.mcukey.set mcu_key 03 0F 01 00 - send_to_mcu 1
    Note over HU_MCU: MCU command 0x0F - meaning UNKNOWN (head-unit MCU, not the canbox). Inferred from call sites: canbox upgrade in progress, stay awake, do not touch the canbus line
    Note over RadioApp: update_temp = 1. Broadcast com.xygala.canbus.hiworld.data hiworld true so CanbusService starts sniffing U B S R E

    loop every 1000 ms while update_temp is 1
        RadioApp->>TTY: 14 bytes 5A A5 02 E0 00 00 E1 AA 55 02 E0 00 00 E1
        TTY->>BoxApp: same 14 bytes, verbatim, no added framing
        BoxApp->>BoxApp: parser skips the 5A A5 frame, accepts AA 55 LEN 02 CMD E0 CHK E1
        BoxApp->>BoxApp: uart_rx_dispatch sees frame 3 is 0xE0 so calls wwdg_force_reset
    end
    Note over BoxApp: wwdg_force_reset writes 0xBF to WWDG_CR 0x40002C00 in a loop. Reset in microseconds. Nothing stored, no NV flag, no reply. The app never sees one byte of the image

    BoxApp->>BootLdr: MCU reset, control to 0x08000000
    BootLdr->>TTY: U 0x55
    Note over BootLdr: U is emitted unconditionally on every boot, before any input
    TTY->>RadioApp: U 0x55 inside an untrimmed 64-byte read buffer
    Note over RadioApp: update_temp = 2, udata_erro = 1, the 1 Hz reset thread exits
    RadioApp->>TTY: 19 78 02 17
    TTY->>BootLdr: 19 78 02 17
    Note over BootLdr: begin magic. The loader also accepts 20 08 10 20 (begin + switch to 600000 baud); the radio only sends 19 78 02 17
    BootLdr-->>TTY: B 0x42
    BootLdr-->>TTY: S 0x53
    TTY-->>RadioApp: B 0x42 then S 0x53
    Note over BootLdr: B = begin accepted; S = send record 0

    Note over RadioApp: update_temp = 3, updata_temp_data = 1
    RadioApp->>TTY: 137-byte array, 0x88 length prefix stripped, 136 raw bytes on the wire
    TTY->>BootLdr: record 0 header - 16-byte magic 01 09 07 08 00 02 01 07 91 32 47 a4 4e 33 14 f2, 116 opaque bytes (unread), key 68 cc e3 3e at offset 132
    Note over BootLdr: validates all 16 target magic bytes (12 universal + 4 per-box tag); mismatch -> R then E. The key ships in the clear; the loader de-obfuscates
    BootLdr->>Flash: unlock flash (enable HSI), then bulk-erase page 0x08001800 + app region 0x08002000..0x0801F800 in 2 KB steps - once, after record 0
    BootLdr-->>TTY: S 0x53
    TTY-->>RadioApp: S 0x53

    loop 256 data records, strict stop and wait, one record per S
        RadioApp->>TTY: 137-byte array, 0x88 prefix stripped, 136 raw bytes of record k
        TTY->>BootLdr: 128 obfuscated bytes, u16 LE seq k, u8 percent, u8 last flag, u32 LE checksum
        BootLdr->>BootLdr: verify checksum = sum of the first 132 bytes read as 33 LE u32
        BootLdr->>BootLdr: de-obfuscate plain[i] = cipher[i] - i - key[i&3]
        BootLdr->>Flash: program 128 bytes at 0x08002000 + 128*(seq-1)
        alt record accepted
            BootLdr-->>TTY: S 0x53
            TTY-->>RadioApp: S 0x53 - udata_erro = 1, updata_temp_data increments
        else bad magic or checksum
            BootLdr-->>TTY: R 0x52
            TTY-->>RadioApp: R 0x52
            Note over RadioApp: update_temp = 0, update abandoned. No per-record retransmission exists
        end
    end
    Note over RadioApp,BootLdr: 256 records x 136 bytes is about 9.1 s of line time at 3840 B/s, realistically 15-30 s

    opt UI tick finds no progress, about 1 to 2 s window
        Note over RadioApp: runnable_dialog_d re-posts every 1000 ms. If udata_erro is 0 at a tick the update fails
        Note over RadioApp: progress = updata_temp_data * 100 / (bFileData length / 136)
        RadioApp->>User: failure dialog, update_temp = 0
        RadioApp->>HU_MCU: send_to_mcu 0 posted 10000 ms later - 03 0F 00 00
    end

    opt HAZARD - plugin interference, can fire at any point above
        Note over Plugin: ToolClass.sendDataToSerial is gated on CAN_BOX_UPDATA == 1, but no app in host 1_025 or 2_01 ever sets it to 1. Only onStop writes 0. The 2017 monolith did set it
        Plugin->>TTY: about 1 Hz AA 55 poll from CanbusPattern_P.SendDataEngine
        TTY->>BootLdr: plugin bytes interleaved inside a 136-byte record
        BootLdr-->>TTY: R 0x52 - bad per-record checksum
        Note over User,Flash: mitigation - settings put system CAN_BOX_UPDATA 1 before the update and 0 after
    end

    Note over RadioApp,BootLdr: last record has last-flag byte[0x83] set (seq 256, percent 100)
    BootLdr->>Flash: finalize: write valid-app markers 0x19780217->0x08001800, 0xA4473291->0x08001F80, 0x01043749->0x08001F84
    BootLdr-->>TTY: E 0x45
    TTY-->>RadioApp: E 0x45
    Note over RadioApp: update_temp = 4. The radio depends on E and not S here - another S would index bFileData at its length and throw ArrayIndexOutOfBoundsException
    RadioApp->>HU_MCU: action.mcukey.set mcu_key 03 0F 00 00 - send_to_mcu 0
    RadioApp->>User: success dialog, plus broadcast com.xygala.canbus.update.xy with canbusdata 0
    BootLdr->>BootLdr: ResetViaWatchdog (WWDG_CR=0xBF) -> MCU reset
    BootLdr->>BootLdr: reboot: SystemInit -> bootloader main -> fresh U/E window
    BootLdr-->>TTY: U 0x55 (fresh boot); no answer -> E 0x45
    Note over BootLdr: valid-app marker at 0x08001800 == 0x19780217 is present, so the loader launches the app: bl 0x08002202 (reset veneer -> __main -> startup_crt0 -> main). App inherits VTOR=0x08000000, MSP=0x20000600
    BootLdr->>BoxApp: bl 0x08002202
    BoxApp->>TTY: AA 55 11 F0 H1N4JPF20A-220524 DA
    TTY->>RadioApp: version frame, iap_Version_Hiworld keys on byte 3 being 0xF0
    Note over BoxApp: on a bench with no CAN bus this frame never appears - the app stays silent in phase 1 until 3 CAN frames have been received
```

---

## 4. Cold boot with no flasher attached (bench)

```mermaid
sequenceDiagram
    autonumber
    participant TTY as TTY / dev ttyMT3 38400 8N1
    participant BootLdr as BootLdr / 8 KB loader 0x08000000
    participant BoxApp as BoxApp / canbox app 0x08002000

    Note over BootLdr: power-on or any reset, nothing attached, no flasher listening
    BootLdr->>TTY: U 0x55 emitted unconditionally
    Note over TTY: no taker - nobody sends 19 78 02 17
    BootLdr->>TTY: E 0x45 - IAP window closed
    Note over BootLdr: launch is gated on the valid-app marker 0x08001800 == 0x19780217 (written after the last successful flash); if absent the loader stays in the IAP window
    BootLdr->>BoxApp: bl 0x08002202 (reset veneer -> __main -> startup_crt0 -> main_loop); app inherits VTOR=0x08000000, MSP=0x20000600
    Note over BoxApp: app init - USART2 at 38400 8N1, CAN1 at the baud stored in 0x0801F800, WWDG armed at about 65.5 ms
    Note over BoxApp: phase 1 - silent on UART and CAN until 3 CAN frames arrive. UART bytes are buffered by the ISR but nothing parses them, so even the 0xE0 reset command is inert here
```

---

## 5. Token legend

| Byte | ASCII | Direction | Meaning |
|---|---|---|---|
| `0x55` | `U` | box -> radio | Bootloader alive, IAP window open. Emitted unconditionally on every boot |
| `19 78 02 17` | — | radio -> box | Begin-download magic (standard) |
| `20 08 10 20` | — | radio -> box | Begin-download magic that also switches the UART to 600000 baud before transfer. Present in the loader; the stock radio does not use it |
| `0x42` | `B` | box -> radio | Begin accepted |
| `0x53` | `S` | box -> radio | Send the next record. One record per `S`, strict stop-and-wait |
| `0x52` | `R` | box -> radio | Reject / abort (bad 16-byte magic or bad 33-word checksum). Radio gives up — no retry |
| `0x45` | `E` | box -> radio | Transfer finished, or IAP window timed out at boot. After a successful transfer it is followed by a watchdog reset |
| `0x88` | — | radio-internal | Length prefix of the 137-byte Java array. Stripped by `sendBroadcastsHiworld`; 136 bytes reach the wire |
| `AA 55 02 E0 00 00 E1` | — | radio -> app | Reset into IAP. `CHK = (LEN + CMD + sum(data) - 1) & 0xFF` |
| `5A A5 02 E0 00 00 E1` | — | radio -> app | The same command in the other Hiworld dialect, sent back-to-back and skipped by the box's `AA`-scanner |

Both generations of radio code special-case exactly `{0x55 U, 0x42 B, 0x53 S, 0x52 R, 0x45 E}` as
"Hiworld box in update mode".

---

## 6. Key facts and open items

- **App launch:** the loader launches the app with `bl 0x08002202` (the app's reset veneer
  `push {r4,lr}; bl __main; pop`), gated by the valid-app marker `*0x08001800 == 0x19780217`. After a
  successful transfer it writes the marker, sends `E`, and forces a watchdog reset; the subsequent
  boot finds the marker and launches the app. The app runs on the bootloader's vector table
  (`VTOR=0x08000000`, `MSP=0x20000600`).
- **Erase:** after validating record 0, a bulk erase of `0x08001800` + app region
  `0x08002000..0x0801F800` in 2 KB steps, once, before the first data record.
- **Validation:** only the 16-byte record-0 magic and each record's 33-word LE checksum. No
  whole-image signature/MAC; a correctly repacked image is accepted.
- **Baud:** 38400 8N1 on the standard path. The `20 08 10 20` fast path switches to 600000. The
  38401/38403 radio suffixes are parser-mode selectors, not baud rates.
- **Records on the wire:** raw/unframed, 136 bytes each, one per `S`.
- **SRAM:** 24 KB (`0x20000000..0x20005FFF`).

Open / not covered by the canbox bootloader dump:

- **HU_MCU command `0x0F`** — the head unit's own MCU, firmware not in hand. Inferred: "upgrade in
  progress, stay awake / do not touch the canbus line" (`1` before, `0` on success and on the 10 s
  failure timer).
- **Record-0 header bytes 16..131** — per-file, not read by the loader; see `hiworld-iap-format.md`.
- **This unit's `services.jar`** — the carved copy is from a sibling ROM; the baud mapping, raw
  `write()` path and `{U,B,S,R,E}` sniff are corroborated by the installed APKs.
- **Plugin APK generation** — `P2_101.apk` (OTA copy) vs `P1_501.apk` (loaded from
  `/storage/emulated/0/CanBus/plugin/3/9/`); same `SendDataEngine` polling, same hazard.
