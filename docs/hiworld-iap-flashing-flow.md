# How the head unit flashes the Hiworld canbox (H1N4JPF20A / N32L406) over UART

**Scope:** end-to-end, byte level, both sides. Box/app side from the 383-function decompile of
`2429-H1N4JPF20A-220524.app.bin`; bootloader side from the dumped loader
(`hiworld/extracted/bootloader_0x08000000_8k.bin`, analysis in `hiworld-bootloader-analysis.md`);
radio side from a decompile of the APKs pulled off this unit plus the system `services.jar` carved
from a sibling ROM image.

Claims are tagged **DECOMPILED** / **CONFIRMED** (two independent sources) / **INFERRED** /
**UNKNOWN**.

---

## 0. Bottom line

The box reboots immediately on the reset command; the application never sees one byte of the image.
The whole transfer is done by the 8 KB bootloader at `0x08000000`, speaking an unframed protocol.
**CONFIRMED.**

Three proofs:

1. The 0xE0 handler is two instructions. `uart_rx_dispatch` ends with
   `if (frame[3] == 0xE0) wwdg_force_reset();` and `wwdg_force_reset` is
   `do { *(uint8_t*)0x40002C00 = 0xBF; } while(1);` — an infinite write of `WDGA=1, T=0x3F` to
   `WWDG_CR`, a reset in microseconds. No buffer, no flag, no flash write, no reply. **DECOMPILED**
   (`hiworld\decomp\full\uart_rx_dispatch_08008a96.c:51-53`, `...\wwdg_force_reset_08002e2e.c`;
   `0x40002C00` from the literal pool at `0x08002e54`).
2. The application cannot buffer the image: the payload is 32768 bytes and the MCU has 24 KB SRAM.
   **CONFIRMED.**
3. The application has no firmware writer. The only caller of `FLASH_Unlock` / `FLASH_ErasePage` /
   `FLASH_ProgramWord` is `param_page_save_0801F800`, which erases one page and writes 32 bytes of
   config to `0x0801F800`. `FLASH_MassErase`, RDP and option-byte routines are present but DEAD
   (zero callers). No 136-byte, 128-byte or de-obfuscation logic exists in the app. **DECOMPILED.**

The app's frame parser rejects `LEN >= 0x38` (`uart_rx_parser_AA55_08008bb2.c:55`), so a 136-byte IAP
record cannot be mistaken for a protocol frame by the running application. **DECOMPILED.**

Baud is 38400 8N1 on `/dev/ttyMT3` for everything — normal traffic, the reset command, and the whole
bootloader transfer. **CONFIRMED** (§2.4).

Sequence in one line:

```
radio --AA55 ..E0..--> app        (app resets itself via WWDG)
box  reboots into bootloader, emits 'U'
radio --19 78 02 17--> bootloader (begin)
box  --'B' then 'S'-->            (ack, ready)
radio --136 raw bytes (.iap record 0)--> ... box --'S'--> ... repeat for all 257 records
box  --'E'-->                     (done)
box  WWDG-resets, reboots, re-emits 'U'/'E', then launches the new app via bl 0x08002202
```

---

## 1. The radio side — exactly what is sent

### 1.1 Where the code lives

| Role | APK (on this unit) | dex | Class |
|---|---|---|---|
| The whole IAP state machine + UI | `my-radio-files\CanBus\host\1_025\CanBus2.apk` | `classes3.dex` | `com.kyhero.car.myhost2.activity.BoxUpgradeActivity` |
| RX fan-out to that activity | same | `classes3.dex` | `com.kyhero.car.myhost2.service.DebugService` |
| Normal-traffic framing (`AA 55`) + "mute during update" gate | `my-radio-files\CanBus\host\1_025\CanBus.apk` | `classes39.dex` | `com.canbus.utils.ToolClass`, `com.canbus.status.CanbusStatus` |
| Raw RX hand-off, `isHiworld` switch | `downloaded-files\...\CanBus\host\2_01\CanBus.apk` | `classes.dex` | `com.kyhero.car.myhost.Canset.CanbusService` |
| Protocol-mode selection for our car | `downloaded-files\...\CanBus\plugin\3\9\P2_101.apk` | `classes.dex` | `com.kyhero.car.myplugin.canbus.CanbusConfig_P` |
| UART owner (open/read/write) | ROM `system.img` -> `services.jar` | — | `com.mediatek.server.CanbusMonitor`, `Xycanbusinfo.Application` |
| Older monolithic implementation (cross-check) | `downloaded-files\...\CanBus\old\CanBus_92_v.apk` | `classes.dex` | `com.xygala.set.uninstallCanbus` |

The OTA copies (`host\2_01\CanBus2.apk`, `classes28.dex`) are logically identical to the unit's
`1_025` copy for every method quoted below — diffed. **CONFIRMED.**

### 1.2 The transport primitive

```java
// BoxUpgradeActivity.sendBroadcastsHiworld(byte[])   BoxUpgradeActivity.java:1562
int len = data_array[0] & 255;          // byte 0 is a LENGTH PREFIX, not sent
byte[] tx = new byte[len];
for (i...) tx[i] = data_array[i + 1];
iCanbusService.setValue(tx);            // raw write, no framing added
```

**DECOMPILED.** Every "send" array quoted below has its first element stripped — it is the count of
bytes actually put on the wire.

`iCanbusService.setValue(byte[])` lands in `CanbusMonitor.setValue` which calls `onDataSend(values,
len)` directly — bypassing `sendCanData`, the method that would have added framing. `onDataSend` is
just `mOutputStream.write(buffer)`. So the bytes hit the tty verbatim, in one `write()`.
**DECOMPILED.** (For `canBaudrate` 0x9601/0x9603 the `sendCanData` arm `:sswitch_5b` is also a
verbatim copy with no prefix and no checksum. **CONFIRMED** two ways.)

### 1.3 Step A — arm the head-unit MCU

```java
// BoxUpgradeActivity.send_to_mcu(int)   :1466
Intent i = new Intent("action.mcukey.set");
i.putExtra("mcu_key", new byte[]{3, 15, (byte)temp, 0});   // temp = 1 start, 0 end
```

**DECOMPILED.** Consumer `com.mediatek.server.NewkeypadMonitor$37` drops `value[0]` and sends
`{0x0F, temp, 0x00}` to the head unit's own MCU over the keypad/MCU link — not the canbox link.
What MCU command `0x0F` does is **UNKNOWN** (that MCU's firmware is not in hand); from the call
sites (`1` before the update, `0` on success and on a 10 s failure timer) it is almost certainly
"canbox upgrade in progress — do not sleep / do not touch the canbus line". **INFERRED.**

### 1.4 Step B — the reset / "enter IAP" command (repeated at 1 Hz)

```java
// BoxUpgradeActivity.sendHiworldUpgradeQuery()   :1123
new Thread(() -> {
    while (update_temp == 1) {
        byte[] send = {14, 90,-91, 2,-32, 0, 0,-31,  -86, 85, 2,-32, 0, 0,-31};
        sendBroadcastsHiworld(send);
        Thread.sleep(1000);
    }
}).start();
```

**DECOMPILED.** 14 bytes on the wire — two frames back to back, one in each Hiworld framing:

```
5A A5 02 E0 00 00 E1      <- 5A A5 family (Hiworld "38401" generation)
AA 55 02 E0 00 00 E1      <- AA 55 family (our H1N4JPF20A, "38403")
```

Checksum for `AA 55 02 E0 00 00 E1`:

* box's rule (`uart_build_frame_AA55_08008d04.c:30-36`): `(LEN + CMD + sum(data) - 1) & 0xFF`
  = `(0x02 + 0xE0 - 1) & 0xFF` = `0xE1`.
* "sum of all bytes incl. `AA 55`": `0xAA+0x55+0x02+0xE0 = 0x1E1` -> `0xE1` (since `0xAA+0x55 = 0xFF`).
* radio's own rule (`ToolClass.sendDataToSerial`, case 38403): `AA 55` + body + `(sum(body) & 0xFF) - 1`.

Three independent derivations agree. **CONFIRMED.**

The box's parser scans for `AA` then `55`, so it skips the leading `5A A5` frame and acts on the
second. **DECOMPILED** (`uart_rx_parser_AA55_08008bb2.c:25-40`). Minimal reset command:
`AA 55 02 E0 00 00 E1` (or any `LEN < 0x38` with `CMD = 0xE0` and a correct checksum — the handler
inspects only `frame[3]`). **DECOMPILED.** The older monolithic app sends the same 14 bytes once,
not in a loop. **CONFIRMED** (same literal in two app generations).

### 1.5 Step C — the handshake and the "begin" command

Once the box has rebooted, the bootloader emits `'U'` (0x55). The radio answers:

```java
// BoxUpgradeActivity.update_check_Hiworld, case 1   :304
if (data.length > 2 && data[0] == 85) {          // 'U'
    update_temp = 2;
    sendBroadcastsHiworld(new byte[]{4, 25, 120, 2, 23});   // -> 19 78 02 17
    udata_erro = 1;
}
```

**DECOMPILED.** On the wire: `19 78 02 17` — 4 raw bytes, no framing. The identical constant appears
in the 2017-era monolithic app and in the "NetBy" vendor path (a clone of the Hiworld bootloader).
The old app inserts `SystemClock.sleep(50)` before sending it. **DECOMPILED.** The bootloader also
accepts a second begin token `20 08 10 20`, which switches the UART to 600000 baud before the
transfer; the stock radio never sends it.

### 1.6 Step D — the box acknowledges, radio starts streaming

```java
// case 2
if (data[0]=='B' && data[1]==0)          buf_update[0] = 'B';        // partial: only 'B' arrived
else if ((data[0]=='B' && data[1]=='S') || (buf_update[0]=='B' && data[0]=='S')) {
    update_temp = 3;
    byte[] send = new byte[137];
    send[0] = -120;                       // 0x88 = 136 -> length prefix
    for (i=0;i<136;i++) send[i+1] = bFileData[i];   // .iap record 0, VERBATIM
    updata_temp_data = 1;
    sendBroadcastsHiworld(send);
}
```

**DECOMPILED.** The box replies `'B'` then `'S'`, and the radio puts the raw first 136-byte record
of the `.iap` on the wire. That first record is the header record — magic + opaque blob + the 4-byte
de-obfuscation key in the clear. For `2429-H1N4JPF20A-220524.iap`:

```
000: 01 09 07 08 00 02 01 07 91 32 47 a4   <- 12-byte universal magic (all samples)
00c: 4e 33 14 f2                           <- 4-byte per-box tag; the loader checks it too
010: ... (112 opaque bytes, loader never reads them) ...
080: 17 bc df a8                            <- unknown
084: 68 cc e3 3e                            <- the key
```

The loader validates all 16 bytes `[0:16]` of record 0 and rejects (`R`+`E`) on mismatch. `[0:12]`
is the universal magic; `[12:16]` is a per-box tag (ASCII family "N3"/"I3" in `[12:14]`) that acts
as a cross-flash guard — a `.iap` for a different box carries a different `[12:16]` and is rejected.
**CONFIRMED** (file dump + `hiworld\iaptool.py` + loader code; see `hiworld-bootloader-analysis.md`
§2.2).

The radio ships the key to the box and then streams ciphertext; the bootloader (not the radio) does
the `plain[i] = (cipher[i] - i - key[i&3]) & 0xFF` transform. There is no such arithmetic anywhere
in `BoxUpgradeActivity`. **CONFIRMED.**

### 1.7 Step E — the record loop

```java
// case 3
if (data[0] == 'S') {                       // 0x53 — send next
    byte[] send2 = new byte[137]; send2[0] = (byte)0x88;
    for (i=0;i<136;i++) {
        if (updata_temp_data*136 + i > bFileData.length) return;   // (smali: goto :cond_b)
        send2[i+1] = bFileData[updata_temp_data*136 + i];
    }
    updata_temp_data++;
    sendBroadcastsHiworld(send2);
} else if (data[0] == 'R') { update_temp = 0; }      // 0x52 — abort
  else if (data[0] == 'E') { update_temp = 4; }      // 0x45 — finished
```

**DECOMPILED** (the bounds check is a jump to `return`, i.e. "silently stop", confirmed in smali).
Properties of the loop:

* Record size on the wire: 136 bytes, raw, no header, no CRC added by the radio. The `.iap` record
  carries its own `u16 seq` / `u8 percent` / `u8 last` / `u32 checksum` at offsets 128..135 — those
  are for the bootloader.
* One record per `'S'`. Strict stop-and-wait. No windowing, no sequence negotiation.
* No retransmission. A lost/corrupted ACK trips the watchdog (§1.9) and kills the update.
* `2429-H1N4JPF20A-220524.iap` = 34952 bytes = 257 records (1 header + 256 x 128 B = 32768 B payload
  = the app image). **CONFIRMED.**
* The radio relies on the box answering `'E'`, not `'S'`, after the last record; another `'S'` would
  index `bFileData[len]` and throw `ArrayIndexOutOfBoundsException`. **DECOMPILED** (smali test is
  `>`, not `>=`).

### 1.8 Step F — finish

There is no finish/verify/reboot command for Hiworld. On `'E'` the radio sets `update_temp = 4`; the
UI runnable then does `send_to_mcu(0)` + success dialog + `sendBroadcast("com.xygala.canbus.update.xy",
canbusdata=0)`. **DECOMPILED.** The bootloader restarts and launches the new application itself
(§2.6, §3). (The NetBy clone sends a final `{1,69}` = `'E'`; Hiworld does not.)

### 1.9 Timeouts / failure handling

`showDialog(msg, 3000, 3)` arms `runnable_dialog_d`, which re-posts every 1000 ms:

```
tick:  progress = updata_temp_data*100 / (bFileData.length/136)
       if (udata_erro == 0)  -> FAIL: abort, update_temp = 0, postDelayed(send_to_mcu(0), 10000)
       else if (udata_erro != 2) udata_erro = 0;
       if (update_temp == 4) -> SUCCESS
```

Every successful RX/TX sets `udata_erro = 1`. So if no record is acknowledged within ~1-2 seconds the
update is abandoned. That is the entirety of the retry logic. **DECOMPILED.**

### 1.10 Where the file comes from

`BoxFileUtil.BoxType.HIWORLD` -> suffix `".iap"`; `findBoxUpgradeFile` walks every storage volume
root and `<volume>/CanBus/box/` and takes the first `.iap` (case-insensitive), read whole into
`bFileData`. **DECOMPILED** (`BoxFileUtil.java:161-201`). `BoxDownloadService` can fetch it from the
vendor server first.

### 1.11 The IAP-version query (not part of flashing)

`sendQueryIAPVer()` case 2 temporarily flips the parser mode to 38401 and sends
`ToolClass.sendBroadcast(.., 106, {5,1,240})` -> `5A A5 03 6A 05 01 F0 62`. The box ignores it (its
RX command table accepts only `0xD2` and `0xAD`, §2.5). The version shown arrives unsolicited: the
app broadcasts `AA 55 11 F0 "H1N4JPF20A-220524" DA` on its TX round-robin and `iap_Version_Hiworld()`
keys on `bArr[3] == 0xF0`. **CONFIRMED.**

---

## 2. The box side — code evidence

### 2.1 Frame reception

`uart_rx_parser_AA55 @ 0x08008BB2` — a 4-state scanner over a 200-byte ring:

```
state 0: wait for 0xAA
state 1: expect 0x55
state 2: LEN = byte;  if (LEN >= 0x38) -> back to state 0        <<<< caps payload at 55 bytes
         require (put - get) mod 200 >= LEN + 2, else wait for more
state 3: copy LEN+2 bytes into frame[2..]; running sum;
         last byte must equal (sum - 1) & 0xFF  ->  uart_rx_dispatch()
```

Frame layout: `frame[2] = LEN`, `frame[3] = CMD`, `frame[4..] = data`. **DECOMPILED.**

### 2.2 Dispatch — the 0xE0 branch

```c
/* uart_rx_dispatch @ 0x08008A96, tail */
if (*(char *)(_LAB_08008e34 + 3) == -0x20) {   /* frame[3] == 0xE0 */
    wwdg_force_reset();
}
/* wwdg_force_reset @ 0x08002E2E */
do { *DAT_08002e54 = 0xbf; } while (true);     /* DAT_08002e54 = 0x40002C00 = WWDG_CR */
```

`0xBF` = `WDGA=1, T[6:0]=0x3F`; with T6 clear the counter is already expired, so the reset is
immediate. Nothing is stored, no reply is sent, no NV flag is written before the reset. **DECOMPILED.**

### 2.3 No firmware writer, no IAP knowledge

| function | callers |
|---|---|
| `FLASH_Unlock @ 0x080046FC` | `param_page_save_0801F800` |
| `FLASH_ErasePage @ 0x08004794` | `param_page_save_0801F800` |
| `FLASH_ProgramWord @ 0x08004A96` | `param_page_save_0801F800` |
| `FLASH_MassErase @ 0x080047E2` | (none — DEAD) |
| `FLASH_OB_*` (RDP, write-protect) | (none — DEAD) |

`param_page_save_0801F800 @ 0x08002D46` erases page `0x0801F800` and programs 32 bytes of config,
kicking the watchdog between words — the entire flash-write surface of the application. No `% 136`,
`% 128`, `- i - key[i&3]`, sum-of-33-u32, or buffer >= 1 KB exists in the image. **DECOMPILED.**

### 2.4 Baud — 38400, unchanged

* **Box:** `main_loop` -> `uart_init_38400_and_version @ 0x08008A54` -> `FUN_0800306C(0x9600)` ->
  `usart2_init(0x9600)`. `0x9600 = 38400`, the only call to `usart2_init` in the image. **DECOMPILED.**
* **Radio:** `Xycanbusinfo.Application.getSerialPort()` opens `/dev/ttyMT3` with
  `speed = f(Settings.System "canbus_bandrate" / 100)`. The `/100` is the whole story: the values
  the apps use — 38400, 38401, 38403, 38405, ... — all divide to 384 and open the port at 38400.
  Those suffixes are a protocol-mode selector, not a baud rate. **DECOMPILED.**
* Our car (`protocol_id 3009001`, Hiworld Jeep) selects 38403 -> `AA 55` framing. **CONFIRMED.**

Both the normal protocol and the bootloader transfer run at 38400 8N1. `115200` appears in the
radio's table but is unreachable for a Hiworld box. **CONFIRMED.**

### 2.5 For reference: what the running app accepts

RX command table at `0x08009F74`: `{0xD2, len 13}`, `{0xAD, len 2}`.
TX round-robin at `0x08009F2C`: `{0x72,14} {0xA6,8} {0x73,8} {0xF0,17} {0x12,10} {0x32,14}`.
Plus two specials in `uart_rx_dispatch`: the `0xE0` reset, and a `0xFC`/`0xFD` echo-with-`CMD+1`
handshake gated on a literal payload — not used by any radio code in hand (probably a factory probe).
**DECOMPILED / purpose UNKNOWN.**

### 2.6 The application has no vector table — the bootloader owns the vectors

The 32 KB image has no classic Cortex-M vector table and no write to `SCB->VTOR` (`0xE000ED08`).
The bootloader's vector table at `0x08000000` supplies the vectors and forwards the exception
handlers into the app region:

```
[0] MSP       = 0x20000600
[1] Reset     = 0x08000149   (bootloader's own reset handler)
[2] NMI       = 0x0800202B   (app region)
[3] HardFault = 0x08002033   (app region)
    MemManage = 0x0800203B,  BusFault = 0x08002043,  UsageFault = 0x0800204B   (app region)
    SVC/PendSV/SysTick = 0x0800015B / 0x0800015F / 0x08000161   (bootloader's own)
```

The application runs with **VTOR left at `0x08000000`** and **MSP = `0x20000600`** inherited from the
loader; it never sets up its own SP or vectors. A replacement app must place its exception handlers at
those exact app-region offsets. **CONFIRMED** (`hiworld-bootloader-analysis.md` §1, §4).

---

## 3. Timeline

```
 RADIO (Android, com.kyhero.car.myhost + myhost2)            BOX (N32L406)
 --------------------------------------------------          ----------------------------------
 user taps "Update box"
 reads <storage>/**/*.iap  -> bFileData (34952 B, 257 recs)
 send_to_mcu(1)   [head-unit MCU, not the box]
 update_temp = 1
 +- every 1000 ms while update_temp==1 ------------+
 | TX  5A A5 02 E0 00 00 E1 AA 55 02 E0 00 00 E1   |--38400-->  parser: AA 55, LEN=2<0x38,
 +--------------------------------------------------+           CKS ok, CMD==0xE0
                                                                 wwdg_force_reset()  -> RESET
                                                                 BOOTLOADER @0x08000000 runs
                                              <----------------  'U'  (0x55)
 update_temp = 2, query thread exits
 TX  19 78 02 17                              ---------------->  "begin"
                                              <----------------  'B'        (ack)
                                              <----------------  'S'        (ready)
 update_temp = 3, updata_temp_data = 1
 TX  record[0]  136 B raw  (magic + KEY)      ---------------->  validate 16-byte magic, store key
                                                                 unlock flash (HSI), bulk-erase
                                                                 0x08001800 + 0x08002000..0x0801F800
                                              <----------------  'S'
 TX  record[1]  136 B raw                     ---------------->  verify 33-word checksum, deobfuscate
                                                                 128 B, program at 0x08002000+128*(seq-1)
                                              <----------------  'S'   (or 'R' on error)
   ...  x 256   (~9.1 s of line time at 3840 B/s; realistically 15-30 s)
 TX  record[256] (last-flag = 1, pct = 100)   ---------------->  write valid-app markers
                                                                 (0x19780217->0x08001800, ...)
                                              <----------------  'E'
 update_temp = 4                                                 ResetViaWatchdog (WWDG_CR=0xBF)
 (<=1 s later) send_to_mcu(0), "success"                         reboot: fresh U/E window,
                                              <----------------  ('U' then 'E' on the wire), then:
                                                                 marker present -> bl 0x08002202
                                              <----------------  AA 55 11 F0 "H1N4JPF20A-..." DA
```

After the last record the box writes the valid-app markers, sends `E`, and forces a watchdog reset;
after reboot the normal boot path runs (fresh `U`/`E` window), finds the valid-app marker at
`0x08001800`, and launches the app with `bl 0x08002202` — which is why a `U`/`E` pair appears on the
wire right after a successful update. A 1 Hz UI tick aborts everything if no byte of progress happened
since the previous tick.

### The RX path, concretely

The bootloader's replies are single bytes. They reach the activity like this:

```
/dev/ttyMT3 --> CanbusMonitor$ReadThread: byte[64] buffer; size = read(buffer)
            --> CanbusMonitor.onDataReceived(buffer, size)
            --> (canBaudrate 0x9601/0x9603) if buffer[0] in {0x55,0x42,0x53,0x52,0x45}
                   -> broadcast "com.xygala.canbus.update.send.xy" extra "canbusupdate"  [gen-1]
            --> ICanbusInterface.onResult(buffer, size)
                   -> CanbusService: if (isHiworld && data[0] in {U,B,S,R,E,0xE2})
                        -> broadcast "com.xyauto.hiworld_canbusbox_updata" extra "hiworld" [gen-2]
            --> DebugService -> BoxUpgradeActivity.update_check_Hiworld(data)
```

`isHiworld` is set by the activity broadcasting `"com.xygala.canbus.hiworld.data"` (true on Update,
false in `onDestroy`). **DECOMPILED.** The read buffer is always 64 bytes and passed on untrimmed —
which is why the activity's `data.length > 2` guards pass and why there is a special case for
"`'B'` arrived alone". **CONFIRMED.** Two code generations independently special-case
`{0x55 'U', 0x42 'B', 0x53 'S', 0x52 'R', 0x45 'E'}` as "Hiworld box in update mode".

*Caveat:* the carved `services.jar` is from the sibling ROM, a generation older than the host APKs on
this unit. The baud mapping, the raw `write()` path and the `{U,B,S,R,E}` sniff are corroborated by
the APKs; the exact plumbing on this unit's `services.jar` is **UNKNOWN** (no dump of it).

---

## 4. Bootloader behaviour (from the dumped loader)

From `HandleIapProtocol` and friends (`hiworld-bootloader-analysis.md`):

- **`U` first:** the loader emits `'U'` unconditionally on every boot, in a startup window. It is not
  a reply to `0xE0`. IAP can be entered on any cold boot (even with a broken app) by answering `U`.
- **Begin tokens:** `19 78 02 17` (standard) and `20 08 10 20` (begin + switch to 600000 baud). A
  wrong/absent begin: the loader keeps re-emitting `U` until the window closes with `E`.
- **Erase:** a bulk app-region erase after record 0 (once the 16-byte magic validates): page
  `0x08001800` + `0x08002000..0x0801F800` in 2 KB steps, before the first data record. HSI is enabled
  and flash unlocked (keys `0x45670123` / `0xCDEF89AB`) first.
- **Per-record validation:** only the 16-byte record-0 magic and each record's 33-word LE checksum.
  Not the `seq`, not the opaque header bytes, no whole-image signature. `seq` places the 128 B at
  `0x08002000 + 128*(seq-1)`; `last-flag` triggers finalize.
- **Finalize + launch:** write valid-app markers (`0x19780217`->`0x08001800`,
  `0xA4473291`->`0x08001F80`, `0x01043749`->`0x08001F84`), send `E`, `ResetViaWatchdog`. After the
  reset the loader launches the app with `bl 0x08002202` (the app's reset veneer
  `push {r4,lr}; bl __main; pop` -> `__main` -> scatterload -> `startup_crt0` -> `main_loop`),
  gated on `*0x08001800 == 0x19780217`.
- **RDP:** Level 1 — SWD cannot read flash, but flash-resident code can (how the loader was dumped,
  via an IAP-delivered SRAM-copy dumper; see `hiworld-hardware.md` §3.1).

Still unknown: head-unit MCU command `0x0F` (it targets the head unit's own MCU, not the canbox).

### Custom-firmware constraints

- Place the entry — a reset veneer running `__main`/scatterload, or a jump to your own `main` — at
  offset `0x202` (`0x08002202`); run on the inherited MSP (`0x20000600`) and VTOR (`0x08000000`).
- Place exception handlers at the app-region offsets the loader's vector table forwards to (§2.6).
- The IAP flash writes the valid-app marker, so no need to set `0x08001800` yourself.
- The written region is bounded to `0x08002000..0x0801F800`; a larger image would run into the param
  page.

### Practical: the "mute normal traffic" gate is not armed

`ToolClass.sendDataToSerial` is wrapped in `if (!CanbusStatus.isUpdate(mContext))`, where `isUpdate`
reads `Settings.System "CAN_BOX_UPDATA" == 1`. The 2017 monolith set that flag on the Update button;
`BoxUpgradeActivity` in both host `1_025` and `2_01` only ever writes `"0"` (in `onStop`) — there is
no setter. **CONFIRMED.** Consequence: while records stream, the Jeep plugin's ~1 Hz
`CanbusPattern_P.SendDataEngine -> ToolClass.sendDataToSerial` polling still writes the same
`/dev/ttyMT3`; two threads writing one fd can interleave inside a record -> bad per-record checksum
-> `'R'` -> aborted update. For random, non-reproducible update failures this is the first suspect;
`settings put system CAN_BOX_UPDATA 1` before starting (and `0` after) is the vendor's intended
mitigation.
