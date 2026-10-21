# CLAUDE.md

This project rebuilds the **APP** firmware of a Hiworld canbox (Nations N32L406, Cortex-M4F)
to re-flash through the stock IAP bootloader.

**Read `AGENTS.md` for the full brief** — goal, hard constraints, workflow, and acceptance
criteria. Then `docs/ARCHITECTURE.md` (the layer contract and every module's public API),
`README.md` (layout), and `app/REBUILD.md` (build + flash detail).

## Build and test

```
python tools/build.py          # compile + link + verify the preamble
python tools/build.py --pack   # ...and package to build/canupdate<version>.iap
python tools/build.py --protocol benz --pack   # the Mercedes emulation image
python tools/test.py           # host unit tests, BOTH protocols
```

No toolchain on the machine? `./build-docker.sh` (or `.\build-docker.ps1`) does the
whole thing in a container -- tests, both protocols, both `.iap` packed into `build/`.
Docker is the only prerequisite; the image pins the same xPack GCC 15.2.1 a native
build uses, so the output is byte-identical. Flashing stays on the host.

## Most important rules

- **Never touch the bootloader** (`0x08000000`–`0x08001FFF`). It is the only way to flash
  the box. The app lives at `0x08002000`–`0x0801F7FF`; the param page at `0x0801F800` is the
  only flash the firmware may write at runtime.
- **The preamble is address-constrained.** The app has no vector table — it runs on the
  bootloader's, which dispatches into a fixed array of 59 veneers at
  `0x0800202a..0x08002201`, with the entry veneer at exactly `0x08002202`.
  `tools/verify_preamble.py` enforces this on every build. Do not move anything in
  `app/startup/crt0.S`.
- **The five interrupt handlers and `fatal_error_reset` are an ABI.** `crt0.S` branches to
  them by name, so they must stay `extern "C"` and keep their exact spelling. A mangled name
  links cleanly and silently leaves a trap handler in place.
- **This image never runs `.init_array`.** No namespace-scope object with a runtime
  constructor; the linker fails the build if one appears. State goes in `ram::AppRam`
  (`app/services/app_state.cpp`), which is ordinary `.bss` — nothing is at a fixed
  SRAM address any more except the stack the bootloader set up.
- **`reference/decompile/app/*.c` is the spec, not the source.** It is named Ghidra output; it does not
  compile and is not built. The rebuild in `app/hal/`, `app/services/` and `app/logic/` is
  written against it, and host tests are written from it.
- **Ship as `.iap`** via `tools/`.
- **Two protocols, one tree.** `--protocol jeep` (default) is the faithful
  rebuild; `--protocol benz` emulates a Raise Mercedes canbox for a richer gauge
  screen. Code selects on `cfg::kIsBenz` with `if constexpr`, never `#if`, so
  both are compiled in every build. Each dialect has its own translation units
  -- `app/services/hu_hiworld.cpp` vs `hu_raise_benz.cpp` (contract in
  `hu_dialect.hpp`), `app/logic/can_decode.cpp` vs `raise_benz_decode.cpp` --
  so `can_decode.cpp` is pure stock again and `head_unit.cpp` holds only what
  both dialects share. Only the jeep image is stock-equivalent on the wire.
  See `docs/raise-benz-emulation.md` and the Protocol block in
  `app/include/app_config.hpp` -- including why the Hiworld
  reboot frame is matched in EVERY build (it is the radio's only way to reflash
  the box).

## Where things live

`app/hal/` C11 register drivers (MMIO only) · `app/services/` C++17 stateful services ·
`app/logic/` C++17 vehicle behaviour · `app/include/ram_map.hpp` all runtime state as one
typed, linker-allocated struct · `docs/misra-cpp.md` the C++ subset and its deviations.
The Benz emulation is `app/services/hu_raise_benz.cpp` (wire dialect) and
`app/logic/raise_benz_decode.cpp` + `raise_benz_tx.cpp` (telemetry in, Benz frames out);
`app/services/head_unit.cpp` holds only what both dialects share.
The jeep-only **custom `0xB0`–`0xBD` telemetry block** is
`app/logic/custom_decode.cpp` + `custom_tx.cpp`, same shape: WK CAN in,
`ram::CustomSignals` in physical units, packed payloads out. Its contract is
`docs/CUSTOM-TX-SPEC.md`, and the six stock TX slots are untouched by it.
`0xB7` TYRE is the exception to "WK CAN in": it is **polled** from the WCM over
KWP2000 every 30 s (`docs/TPMS-POLL-SPEC.md`), the only frame this box sends
unprompted — and the only place a stock path (the `0x500` -> `0x516` mirror) is
switched off rather than added beside.

Keep the `/* 0x0800xxxx */` stock-address annotations on reconstructed functions, and keep
the explanatory comments — in this repo they are the reverse-engineering record, not noise.
