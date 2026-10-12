---
name: nations-tool-is-dap-v1-only
description: NationsMCUDownloadTool is CMSIS-DAP v1 (HID) only; the Pico debugprobe is DAP v2 (WinUSB) only - they cannot see each other
metadata:
  type: project
---

`tools/NationsMCUDownloadTool/CMSIS_DAP.dll` (dated 2021-05) imports only
`HidD_*` / `HidP_*` / `SetupDi*` - zero `WinUsb_*`. It is a CMSIS-DAP **v1
(HID transport)** client. The stock `debugprobe_on_pico.uf2` builds with
`PROBE_DEBUG_PROTOCOL = PROTO_DAP_V2` (bulk/WinUSB) and exposes no HID
interface at all, so NZDownloadTool will never list the Pico probe.

Two ways out:
- Rebuild debugprobe with `-DPROBE_DEBUG_PROTOCOL=1` - the macro sits behind
  an `#ifndef` in `src/probe_config.h`, so it is overridable at build time.
  Gives a HID probe the vendor tool accepts.
- Use **pyOCD** instead, which speaks DAP v2 and can load the `.FLM` flash
  algorithms already bundled in `tools/NationsMCUDownloadTool/FLM/`.

The tool's primary backend is actually J-Link (`JLinkARM.dll`); its bundled
CMSIS-DAP driver INF targets `VID_C251&PID_F001` (Keil VID) = Nations NSLink.

**Why:** hours can be lost assuming "it's CMSIS-DAP, it should just work" -
the v1/v2 split is invisible in Device Manager unless you look for a missing
HID interface.
**How to apply:** before pairing any probe with a vendor flasher, grep the
vendor DLL for `HidD_` vs `WinUsb_`. Related: [[canbox-mcu-is-nations-n32]].
