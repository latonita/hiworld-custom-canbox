# Architecture — the rebuilt Hiworld canbox APP

How the application is put together, which layer may talk to which, and the exact
public API of every module. This is the contract: if a module's behaviour and its
header agree with this document, it drops into the build.

Background on *what* the firmware does lives in `docs/hiworld-firmware-analysis.md`
and `docs/hiworld-startup-sequence.md`. Build and flash commands are in `BUILD.md`.
The non-negotiable flash/entry constraints are in `app/REBUILD.md` §2 — nothing in
this document may be read as permission to move the preamble.

---

## 1. Layers

```
         ┌──────────────────────────────────────────────────────────┐
         │  app/logic/      C++17   vehicle behaviour               │
         │  decoders · periodic CAN TX · cluster text · link state  │
         │  main loop · the five interrupt entry points             │
         └───────────────────────────┬──────────────────────────────┘
                                     │ calls svc::
         ┌───────────────────────────┴──────────────────────────────┐
         │  app/services/   C++17   stateful services               │
         │  scheduler · clock · timers · params · CAN · head unit   │
         │  board I/O · power · byte helpers                        │
         └───────────────────────────┬──────────────────────────────┘
                                     │ calls extern "C" hal_*
         ┌───────────────────────────┴──────────────────────────────┐
         │  app/hal/        C11     register drivers, MMIO ONLY     │
         │  rcc nvic wwdg gpio exti tim can usart flash adc pwr     │
         └──────────────────────────────────────────────────────────┘

  app/startup/   crt0.S + startup.c   asm + C11, the fixed preamble
  app/include/   ram_map.hpp · app_config.hpp · n32l406_hw.h   shared
```

**The rules, in dependency order:**

1. **HAL touches registers, never RAM.** No HAL translation unit includes
   `ram_map.hpp`, and `arm-none-eabi-size` reports `data = 0, bss = 0` for every
   HAL object. That is a mechanically checked invariant, not a style preference.
2. **Services own the state.** Every byte of persistent state lives in the single
   `ram::AppRam` structure. Services read and write it through named fields.
3. **Logic owns the behaviour.** Vehicle semantics — what CAN id 0x015 means, what
   the head unit expects in message 0x72 — live here and nowhere else.
4. **Calls go downward only.** Logic → services → HAL. A service never calls into
   logic; the one apparent exception, the TIM2 tick driving logic-layer work, is
   inverted through the interrupt layer (§5).
5. **No weak-symbol scaffolding.** The previous generation used `__attribute__((weak))`
   no-op stubs so half-finished subsystems would link. Every one is gone. If a symbol
   is missing the link fails, which is the correct outcome.


> **A note on `app/port/` references.** Comments throughout the rebuilt layers cite files like
> `the retired app/port/hw_can.c`. That was the *earlier single-layer C port* — one flat directory, weak
> no-op stubs between subsystems, and absolute-address macros for state. The layering refactor
> replaced it, and its explanatory comments were carried forward into the files that cite it,
> so the references record where a piece of reasoning came from. The directory itself is in git
> history (it was last present at commit `ff070e1`) if you ever want to diff against it.

---

## 2. Where the state lives

All of it is in one object: `ram::AppRam`, declared in `app/include/ram_map.hpp` and
defined in `app/services/app_state.cpp`. The linker places it in `.bss`;
`app/startup/startup.c` zeroes it along with every other static. Access is through
`ram::app()`.

It used to be pinned to the stock firmware's absolute SRAM map
(`0x20001000`–`0x20003028`), reproduced field by field with a `static_assert` per
offset. Nothing required that. The only hardware constraint on SRAM is the stack —
the bootloader leaves `MSP = 0x20000600` and it grows down — and the only thing that
read a fixed app address was our own flashing tool. Holding runtime data at historical
addresses cost ~5.5 KB of reserved padding, a `reinterpret_cast`, 468 bytes of flash,
and ~10 KB of SRAM the linker could not see and therefore could not protect.

What survives is the **provenance**: each field carries the stock address it corresponds
to in a `stock @0x...` comment, so any of this can still be diffed against Ghidra and
against the notes in `docs/`. Those comments are documentation, not a layout contract.

**Still load-bearing:** offsets *within* a block, where the stock firmware overlaps two
views of the same bytes. `ram::VehicleState` is the one that matters — four head-unit
messages (`0x72`, `0x12`, `0x32`, `0xA6`) are overlapping windows onto a single run of
bytes, and those relative offsets are the wire protocol. They keep their asserts.

**The one externally-visible address** is the 17-byte version string, which
`tools/iap_upload.py --verify` reads back over SWD after a flash to prove the image
booted. It is deliberately *not* a member of `AppRam`: it is a standalone `extern "C"`
symbol, `g_versionString`, and the uploader resolves it from `build/app.elf` with `nm`
instead of carrying a hardcoded address.

Under the host test harness nothing is substituted — `tools/test.py` links the same
`app_state.cpp` the firmware does, so the tests exercise the real definition.

---

## 3. Services — `app/services/`, namespace `svc`

Addresses in the right-hand column are the **original flash addresses** of the stock
functions each one reproduces. Keep them in the code comments: they are how a future
reader diffs this against Ghidra.

### `bytes.hpp` — `svc::bytes`
Leaf helpers. Pure, no state, fully unit-tested.

```cpp
void         copy(volatile std::uint8_t* dst, const volatile std::uint8_t* src,
                  std::uint8_t len) noexcept;                       // 0x08008598
bool         differ(const volatile std::uint8_t* a,
                    const volatile std::uint8_t* b,
                    std::uint8_t len) noexcept;                     // 0x08008574
std::uint8_t wrapIndex(int base, int delta, int hi, int lo,
                       bool clamp) noexcept;                        // 0x080085C4
void         bitAssign(volatile std::uint8_t& target,
                       std::uint8_t mask, bool value) noexcept;
constexpr bool bitTest(std::uint32_t value, std::uint32_t mask) noexcept; // 0x0800413C
```

`len` is `uint8_t` deliberately: the stock loop counters mask every increment with
`0xFF`, so a length of 256 would spin forever. The type makes that unrepresentable.

`clamp` inverts the stock `wrap_mode` argument, whose sense is the opposite of its
name: stock `wrap_mode == 0` means **wrap** (below `lo` → `hi`), non-zero means
**clamp**. So `clamp == false` is the wrapping case. Document this at the call site.

### `soft_timer.hpp` — `svc::timers`
The 64-slot software-timer pool (`ram::AppRam::softTimers`), aged every 10 ms,
armed to 150 counts = 1.5 s.

```cpp
void armForKeycode() noexcept;    // 0x08007CD2  slot[swc.keycode] = kSoftTimerReload
void clearCurrent() noexcept;     // 0x08007CE0  slot[swc.keydataRef] = 0
void tickAll() noexcept;          // 0x08007CEE  age every slot, floor 0
void scanActive() noexcept;       // 0x08007D12  publish first live slot
void tickShortTimers() noexcept;  // 0x080083E2  the 10 ms hook; ages miscTimers then tickAll
```

`scanActive` looks like a scan of the upper range but is a **full circular sweep of
all 64 slots that merely starts at 0x17**; index 0x16 is both the last slot examined
and the "nothing held" sentinel. Starting high gives high key codes priority.

`armForKeycode` has no range check, matching stock: a key code above 0x3F would write
past the pool. The decoder never produces one. Reproduce the behaviour, do not
silently patch it — but a `static_assert`/comment recording the hazard is welcome.

### `scheduler.hpp` — `svc::sched`
The 1 ms tick and the cadence flags.

```cpp
void start() noexcept;              // 0x080027F8  TIM2 timebase + NVIC + enable
void onTimerInterrupt() noexcept;   // 0x08002F72  ISR body: divider chain
void onMillisecond() noexcept;      // 0x08007A30  raise the ten cadence flags
void tickCycleCounter() noexcept;   // 0x0800853C  10 ms, rolling 1..cycleCtrMax
bool takeFast() noexcept;           // test-and-clear flagFast     (4 ms)
bool takeMed() noexcept;            // test-and-clear flagMed      (8 ms)
bool takeSlow() noexcept;           // test-and-clear flagSlow  (1024 ms)
bool autobaudDue() noexcept;        // READ flagAutobaud, do not clear
void clearAutobaud() noexcept;      // clear it explicitly
```

`autobaudDue` and `clearAutobaud` are separate **on purpose**: the stock phase-1 loop
clears the flag in the retry branch but deliberately **not** in the give-up branch, so
the shutdown path runs on every pass until the WFI takes effect. A test-and-clear
accessor here would change behaviour.

The cadence phases are staggered (1, 3, 7, 15, …) so the 4/8/16/32 ms jobs never land
on the same millisecond; from 64 ms down they all trigger on an all-ones mask and
**do** coincide. That is stock, not a transcription slip.

### `soft_clock.hpp` — `svc::clock`
The soft real-time clock and the 500 ms heartbeat.

```cpp
void init() noexcept;            // 0x080078DA  zero h/m/s/tenths, recompute 12 h
void tick() noexcept;            // 0x080078EE  the 100 ms hook
void recalc12h() noexcept;       // 0x08007898  AM/PM bit + 12-hour hour
void blinkEdgeDetect() noexcept; // 0x08007858  the 4 ms heartbeat edge detector
```

### `param_store.hpp` — `svc::params`
The emulated-EEPROM page (`0x0801F800`, the **only** flash this firmware may write)
mirrored into `ram::AppRam::param` with a shadow copy for dirty detection.

```cpp
inline constexpr std::size_t  kMagic     = 0x00U;   // must read 0xAA
inline constexpr std::size_t  kProfile   = 0x01U;   // CAN profile, 0 or 1
inline constexpr std::size_t  kVolume    = 0x10U;
inline constexpr std::size_t  kToneFirst = 0x11U;   // BAL FAD BASS MID TRE
inline constexpr std::size_t  kToneCount = 5U;
inline constexpr std::uint8_t kMagicValue = 0xAAU;

void         load() noexcept;             // 0x08002DBC  flash -> RAM + shadow
void         save() noexcept;             // 0x08002D46  RAM -> flash, if dirty
void         validateDefaults() noexcept; // 0x08007B6C  magic/clamp, latches profileActive
std::uint8_t profile() noexcept;
void         setProfile(std::uint8_t value) noexcept;
```

**`save()` must call `rcc_enable_hsi_for_flash()` before `flash_unlock()`.** The stock
code does, and the HAL split moved the flash primitives away from that call. Omitting
it gives a silent erase/program failure or a hang in the busy wait.

The 32 config bytes are stored **one byte per 32-bit word**, so they occupy the first
128 bytes of the page.

### `can_bus.hpp` — `svc::can`
The CAN peripheral bring-up, the two 50-entry rings, and the bus-presence counters.

```cpp
void init(int baudKbps, std::uint8_t mode) noexcept;   // 0x08002A90  pins+NVIC+filter
void initNormal(int baudKbps) noexcept;                // 0x08002B76
void initSilent(int baudKbps) noexcept;                // 0x08002B84
void reinitBitrateA(int baudKbps) noexcept;            // 0x08007482
void reinitBitrateB(int baudKbps) noexcept;            // 0x0800744E
void setBaudByProfile() noexcept;                      // 0x08007B2C
void applyBitrateB() noexcept;                         // 0x08007B4C

void txEnqueue(std::uint32_t canId, std::uint8_t ide, std::uint8_t dlc,
               const volatile std::uint8_t* data) noexcept;          // 0x080074C0
void txDrain() noexcept;                               // 0x08007698  the 4 ms task
bool transmitEntry(const volatile std::uint8_t* entry) noexcept;     // 0x08002B98

void onRxInterrupt() noexcept;                         // 0x08002C0A  push into the ring
std::uint8_t busActiveFlag() noexcept;                 // 0x080074B6
void timersTick() noexcept;                            // 0x080077E8  the 100 ms hook
bool enterSleep() noexcept;                            // 0x08002D1A  true = ack'd

// RX ring reader, used by the decoders.
bool                        rxAvailable() noexcept;
std::uint32_t               rxId() noexcept;
const volatile std::uint8_t* rxData() noexcept;        // -> data[0] of the tail entry
void                        rxAdvance() noexcept;      // tail = (tail + 1) % 50
void                        txFlush() noexcept;        // txTail = txHead, drop the queue
```

`txEnqueue` takes `const volatile std::uint8_t*` so the decoders can hand it a ring
pointer without a cast — the old signature forced `(const uint8_t *)` casts at every
call site.

`enterSleep()` returns whether the controller **acknowledged** sleep. The shutdown path
does nothing at all if it did not, and the caller just keeps spinning; the watchdog is
still clocked at that point, so a wedge ends in a reset rather than a hang.

### `head_unit.hpp` — `svc::hu`
USART2 and the `AA 55` framing to the Android head unit. Constants in `cfg::`.

```cpp
void init() noexcept;        // 0x08008A54  usart2_init + publish the version string
void resetState() noexcept;  // 0x08008A18
void service() noexcept;     // 0x08008FE2  the 8 ms RX parse + TX schedule task
void onInterrupt() noexcept; // 0x0800286E  USART2 RXNE/TXE
void disable() noexcept;     // 0x08003088  shutdown path
void txAck(std::uint8_t cmd, std::uint8_t arg) noexcept;  // 0x08008CCE
```

Call the HAL as `usart_configure(USART2, cfg::kPclk1Hz, cfg::kUartBaud)` — the HAL
function gained an explicit `pclk_hz` parameter because picking PCLK1 vs PCLK2 is a
board decision, not a driver one. That reproduces the stock `BRCF = 0x01A1` exactly.

The six outbound slots and two inbound slots are descriptor tables. Their payload
buffers are now named fields: the shadows are `ram::AppRam::uartShadows`, and the live
buffers are windows into `ram::VehicleState` (`kWin72Off/Len`, `kWin12Off/Len`,
`kWin32Off/Len`, `kWinA6Off/Len`) plus `versionStr` and `buf73`.

**The implementation is four files; the header is not.** `head_unit.cpp` keeps everything
both dialects share — board policy, the inbound table, `detail::rxDispatch()`, the slot
machinery, `roundRobin`, `buildFrame`, `txSched` and `usart2Init` — and each wire dialect
owns a translation unit of its own behind `hu_dialect.hpp`. The public API above did not
change when that split happened. The dialect is chosen at exactly three `if constexpr`
points in `head_unit.cpp`: the active `kTxTable` pointer, `emitFrame()` and `rxParse()`.

### `hu_dialect.hpp` — the internal dialect contract

Not a public header: only `head_unit.cpp`, `hu_hiworld.cpp` and `hu_raise_benz.cpp` may
include it, and nothing in it is reachable through `head_unit.hpp`. It declares `BufFn` and
`struct TxDesc` (cmd, len, live buffer, shadow), the two frame accessors `fb()` / `rxf()`
that both parsers need, `svc::hu::detail::rxDispatch()` — defined in `head_unit.cpp` and
shared verbatim, because both parsers reassemble into `ram::AppRam::uartRxFrame` in the same
layout — and, per dialect, `kSlots`, `extern const TxDesc kTxTable[]`, `emit(cmd, payload,
len)` and `rxParse()`.

| | `svc::hu::hiworld` → `hu_hiworld.cpp` | `svc::hu::raisebenz` → `hu_raise_benz.cpp` |
|---|---|---|
| dialect | the stock `AA 55` | the Raise `2E` |
| `kSlots` | 6 | 5 |
| `kTxTable` | `0x72 0xA6 0x73 0xF0 0x12 0x32` | `0x08`, `0x07`, three pages of `0x41` |
| live buffers | `ram::VehicleState` windows, `buf73`, `g_versionString` | `ram::AppRam::benzFrames` |
| shadows | `ram::AppRam::uartShadows` | `ram::AppRam::benzShadows` |
| stock ancestry | `emit` 0x08008D04, `rxParse` 0x08008BB2 | none — there is no stock `2E` code |

Each table's row geometry — a row's length against its shadow, against its live buffer and
against the outbound frame area — is `static_assert`ed **beside the table it is about**,
because an `extern` array is not usable in a constant expression from another translation
unit. Only the coupling the two dialects share, one `ram::AppRam::uartSlotState` byte per
slot, is asserted in `head_unit.cpp`.

### Two head-unit protocols — `cfg::kProtocol`

`svc::hu` speaks one of two dialects, chosen at BUILD time
(`tools/build.py --protocol jeep|benz`, default `jeep`) and tested as
`cfg::kIsBenz` with `if constexpr` — never `#if`, so both dialect translation
units are compiled and type-checked in every build, each table's geometry is
proved by either build, and only the dead one is dropped by `--gc-sections`.
The one exception is the version-string literal, which cannot survive a
ternary.

| | jeep (default) | benz |
|---|---|---|
| framing | `AA 55 LEN CMD payload CKS`, CKS = sum − 1 | `2E CMD LEN payload CKS`, CKS = ~sum |
| wire overhead | 5 | 4 |
| TX slots | 16: the stock 6 (`0x72 0xA6 0x73 0xF0 0x12 0x32`) + the custom 10 (`0xB0 0xB2 0xB3 0xB4 0xB5 0xB7 0xB8 0xBB 0xBD 0xBC`) | 5 (`0x08 0x07`, three pages of `0x41`) |
| head-unit protocol id | 3009001 | 1034025 / 27 / 28 |
| version string | `H1N4JPF20L-241026` | `H1N4BNZ10L-…` |
| wire behaviour | stock-equivalent for the stock 6 slots | deliberately diverges |

Only the **jeep** build is a faithful rebuild — and only for the six stock
slots. It also carries the **custom `0xB0`–`0xB8` telemetry block**
(`docs/CUSTOM-TX-SPEC.md`), six commands of its own in the free `0xB0`–`0xBF`
range that publish WK signals the Hiworld protocol has no field for: coolant,
intake air, outside air, battery volts, throttle, fuel, road speed, the parking
brake, the odometer and a magnetic compass. The stock six are untouched, so the Android plugin's view of
them is unchanged; the block is additional, not a modification. It is the
jeep-side counterpart of the Benz emulation and is absent from a benz image
for the same reason. Everything Benz-specific — the
`0x2E` emitter and parser in `app/services/hu_raise_benz.cpp`,
`logic::benzPublish()` in `app/logic/raise_benz_tx.cpp`, and the five WK
decoders in `app/logic/raise_benz_decode.cpp` that feed it — is absent from a
jeep image, so nothing about the emulation can change what the stock-equivalent
build puts on the wire. `app/logic/can_decode.cpp` and
`app/services/hu_hiworld.cpp` are stock code only.

**The IAP escape hatch.** The radio's "Update box" button does not hand over an
image; it asks the running app to reboot into the bootloader by sending
`AA 55 02 E0 00 00 E1`. A box that spoke only `0x2E` could never be updated from
the radio again, so that exact frame is matched unconditionally, in every build,
by a side-branch of the Raise RX parser (`iapHatchFeed`, file-local in
`hu_raise_benz.cpp`) — a jeep build already acts on the same frame through the
ordinary `0xE0` dispatch. It is fed only between frames, so the same bytes
inside a legitimate payload cannot reboot the box. Before the reset it drains
the transmitter — a byte still in the shift register arrives at the radio ahead
of the bootloader's `U` greeting and costs the attempt. See the Protocol block in `app/include/app_config.hpp`.

**Telemetry is protocol-neutral.** `ram::VehicleState` is the *Hiworld wire
format* — four overlapping message windows over one run of bytes — so nothing
Benz-shaped fits in it. The Benz decoders write `ram::Telemetry` in physical
units instead, and `logic::benzPublish()` packs that into `ram::BenzFrames`.
The custom telemetry block has the same shape and for the same reason:
`custom_decode.cpp` writes `ram::CustomSignals`, `logic::customPublish()` packs
that into `ram::CustomFrames`, and neither touches `ram::VehicleState`.
`svc::hu`'s existing round-robin then transmits a payload when it differs from
its shadow, exactly as it does for the Hiworld messages: no new scheduling.

### `board_io.hpp` — `svc::io`
This board's hardwired lines.

```cpp
void statusLinesInit() noexcept;       // 0x08008458  PB5 PC15 PB6 PB4 out, PB2 in
void writeStatusLines() noexcept;      // 0x080084F6  the 8 ms fan-out
void statusLinesShutdown() noexcept;   // 0x080084BE  outputs low, PB3/PB2 back to in
void setCanTransceiverStandby(bool standby) noexcept;  // 0x08007430 / 0x08007440
void configurePb8Wake() noexcept;      // 0x08003222  PB8 -> EXTI8, the wake source
```

Status fan-out, from `ram::VehicleState` byte 0: bit 0 → PB5, bit 1 → PC15
(illumination), bit 2 → PB6 (reverse), bit 3 → PB4.

`setCanTransceiverStandby(true)` drives PB12 **high** (transceiver asleep);
`false` drives it low (normal mode). The two stock entry points differ only in that
argument, so they collapse into one function.

`configurePb8Wake()` composes from the HAL, in the stock function's order — the NVIC
comes **first**, which is not the order you would write it from scratch:

1. `nvic_set_priority_grouping(0x500)`
2. `nvic_init(EXTI9_5_IRQn /* 23 */, 0, 0, 1)`
3. `gpio_init_pin(GPIOB, GPIO_PIN_8, GPIO_MODE_INPUT_EXTI_IT_FALLING)`
4. `exti_set_line_source(AFIO_EXTI_PORT_B, 8)`
5. `exti_line_init(EXTI_LINE8, EXTI_MODE_INTERRUPT, EXTI_TRIG_FALLING, 1)`

> **This is the only wake source armed before the deep-sleep WFI.** If it does not run,
> the box sleeps and never comes back without a power cycle.

### `power.hpp` — `svc::power`
Fault handling and the give-up path.

```cpp
extern "C" [[noreturn]] void fatal_error_reset(std::uint8_t code) noexcept; // 0x08002E64
void shutdownDeepSleep() noexcept;      // 0x080032F6
void disableWatchdogClock() noexcept;   // 0x08002E18
```

`fatal_error_reset` keeps **exactly that C name**: `app/startup/crt0.S` branches to it
from 54 default trap handlers (`movs r0,#code; b fatal_error_reset`) and
`app/startup/startup.c` calls it. It must be `extern "C"`, must not return, and must
keep the signature `void(uint8_t)`.

`shutdownDeepSleep()` order is load-bearing: CAN must acknowledge sleep first, and PB8
is only re-armed as the EXTI wake input once the CAN peripheral has released it.

---

## 4. Logic — `app/logic/`, namespace `logic`

### `can_decode.hpp`
```cpp
void dispatchGate() noexcept;       // 0x08007D44  entry point, once per 8 ms
void decodeProfileJeep() noexcept;  // 0x08009010  83.333 kbit/s — THIS box
void decodeProfileFca() noexcept;   // 0x08009680  125 kbit/s — the FCA variant
```
Each decoder makes at most 20 passes per call, so a flooded bus cannot starve the main
loop. The two profiles share `decode_doors`, `decode_swc_buttons`, `decode_illum_level`,
`decode_ignition`, the 0x7FC handshake, the 0x7FF injection and the 0x400..0x43F SWC
service block — those belong in a shared internal header, not duplicated.

Write the decoders against `ram::VehicleState`'s named accessors
(`setDoorBit`, `setSteeringAngle`, `setGear`, `setFlag`, …), **not** raw indices.

`can_decode.cpp` is stock code only. It still owns the RX ring and the dispatch switch, but
the Benz-only readers live in `raise_benz_decode.hpp` below and every call to them sits
behind `if constexpr (cfg::kIsBenz)`.

### `raise_benz_decode.hpp`
```cpp
void benzSpeed(const volatile std::uint8_t* d) noexcept;    // 0x002 d[2]:d[3]
void benzRpm(const volatile std::uint8_t* d) noexcept;      // 0x002 d[0]:d[1]
void benzTrip(const volatile std::uint8_t* d) noexcept;     // 0x014 odometer + ambient
void benzCoolant(const volatile std::uint8_t* d) noexcept;  // 0x02C d[1]
void benzVoltage(const volatile std::uint8_t* d) noexcept;  // 0x015 d[1]
```
**Not stock decodes**, which is why there are no `0x0800xxxx` annotations: the stock image
has no reader for any of these. They exist only because the Mercedes emulation has screen
real estate the Hiworld protocol does not — odometer, ambient temperature, coolant
temperature and battery voltage all have a Benz field and no Hiworld field at all.

Jeep WK frames in, `ram::Telemetry` in **physical units** out; nothing here touches a wire
format. Evidence for every identifier, byte and scale is in
`docs/JEEP-WK-CAN-ID-byte-map.md`, quoted per function in the `.cpp`.

### `raise_benz_tx.hpp`
```cpp
void benzPrimeFrames() noexcept;  // write the 0x41 page selectors, once at init
void benzPublish() noexcept;      // ram::Telemetry -> ram::AppRam::benzFrames
```
**Not a port of anything**: its spec is the decompiled Raise Benz plugin, written up in
`docs/raise-benz-emulation.md`, which also lists what is deliberately *not* sent and why.
It packs the five Benz payload buffers and does **not** transmit — `svc::hu`'s round-robin
notices a payload differs from its shadow and sends it, so the cadence is the stock one and
nothing new schedules anything. `benzPrimeFrames()` must run after the RAM block is zeroed
and before the first transmit: a zero page byte matches no `0x41` page and the head unit
discards the frame.

### `custom_decode.hpp`
```cpp
void customPrimeSignals() noexcept;                              // sentinels, once at init
void customIgnition(std::uint8_t b0) noexcept;                   // 0x000 b0
void customEngine(const volatile std::uint8_t* d) noexcept;      // 0x002 rpm + speed
void customGear(const volatile std::uint8_t* d) noexcept;        // 0x003 b4 ASCII PRNDL
void customAmbient(const volatile std::uint8_t* d) noexcept;     // 0x014 b4
void customVoltsIntake(const volatile std::uint8_t* d) noexcept; // 0x015 b1, b2
void customCoolant(const volatile std::uint8_t* d) noexcept;     // 0x02C b1
void customParkBrake(const volatile std::uint8_t* d) noexcept;   // 0x0D0 b0 bit 7
void customFuel(const volatile std::uint8_t* d) noexcept;        // 0x159 b4 gate, b5
void customThrottle(const volatile std::uint8_t* d) noexcept;    // 0x2C0 b2 (b6 mirror)
void customOdometer(const volatile std::uint8_t* d) noexcept;    // 0x014 b0:b1:b2
void customCompass(const volatile std::uint8_t* d) noexcept;     // 0x1B4 b0, b1
void customTpmsPoll() noexcept;                                  // -> 0x600, every 30 s
void customTpmsReply(const volatile std::uint8_t* d) noexcept;   // 0x500 KWP answer
```
**`customTpmsPoll()` is the one place this firmware originates a frame.** Everything else it
transmits is either an answer to something or a periodic frame the stock box also sends.
It runs inside `periodicTxScheduler()`'s ACC hold gate and additionally checks
`canBusStatus == kBusRunning`, because `abom = 0` / `nart = 0` mean an unACKed frame takes
the controller bus-off permanently — which is what a poll would do on a bench with no second
node. See `docs/TPMS-POLL-SPEC.md`.
**Not stock decodes**, and the exact mirror of `raise_benz_decode.hpp`: these exist because
the Hiworld protocol has nowhere to show any of it. Jeep WK frames in, `ram::CustomSignals`
out; every call site sits behind `if constexpr (!cfg::kIsBenz)`.

Several are **second readers** of a frame a stock decoder already handles — `0x002` for rpm,
`0x003` for the reverse flag and PRNDL, `0x015` for the steering angle. Neither side may
assume the other ran, and neither may write the other's destination. The same holds *within*
this header: `customAmbient()` and `customOdometer()` both read `0x014` and stay two
functions, because they have different gates and different validity bits — one function, one
validity bit is what keeps the block's ownership legible.

**Every decoder has a sentinel path.** On a bad sample it writes the field's "no value"
encoding *and* clears that command's bit in the `0xB0` validity mask; it never writes a value
derived *from* a sentinel. That is what stops `0x02C`'s all-`FF` becoming a plausible-looking
215 °C. The encodings are in the sentinel block of `app/include/ram_map.hpp`.

### `custom_tx.hpp`
```cpp
void customPrimeFrames() noexcept;  // sentinels into ram::AppRam::customFrames, once at init
void customPublish() noexcept;      // ram::CustomSignals -> ram::AppRam::customFrames
```
Its spec is `docs/CUSTOM-TX-SPEC.md`, which also lists the four commands in the range that are
deliberately **not** built and why (`0xB1` and `0xB6` duplicate stock `0xF0` and `0x12`;
`0xB7` TPMS and `0xB9` KEY have no source on this car). Like `benzPublish()` it packs payload
buffers and does **not** transmit — the round-robin notices a payload differs from its shadow.

The one exception is `0xB0`, whose payload is a validity mask that may legitimately be
constant for hours: a change-driven scheduler would only send it on the ~8.6 s keep-alive
sweep. Nothing is toggled artificially; instead the publisher calls
`svc::hu::requestSend(0xB0)` once a second, which is the first and only use of the
`kSlotRequest` slot state.

### `periodic_tx.hpp`
```cpp
void primeTxTemplates() noexcept;     // 0x08007B6C  rebuild the frame templates
void periodicTxScheduler() noexcept;  // 0x08008110  151-phase table, ~400 ms/frame
void clusterTextToCan() noexcept;     // 0x08007DA8  the 0xD2 cluster text -> CAN
```

### `can_link.hpp`
```cpp
void linkStateMachine() noexcept;     // 0x0800755C  the 8 ms CAN task
```
Drives the five-state bus-presence machine in `ram::AppRam::canBusStatus`
(`cfg::kBusDown` … `cfg::kBusLost`). In state `kBusHello`, entered once by the main
loop when the bus answers, it emits the 0x416 hello frame and drops to `kBusRunning`.

### `main_loop.cpp`
```cpp
extern "C" void main_loop(void);      // 0x080088D8
```
Two phases, documented in full in the file's own header comment. The watchdog is kicked at the
top of **both** loops; every path through either must stay well inside ~65.5 ms.

---

## 5. Interrupts — `app/logic/interrupts.cpp`

Every hardware entry point into the application, in one file, so the contract with
`app/startup/crt0.S` is readable in one place.

The app has **no vector table**. It runs on the bootloader's (`VTOR = 0x08000000`),
which dispatches into the fixed veneer array in the preamble; each veneer `bl`s a
handler **by name**. Those names are therefore an ABI:

```cpp
extern "C" void TIM2_IRQHandler(void);     // veneer slot 37, IRQ 28
extern "C" void CAN_RX0_IRQHandler(void);  // veneer slot 29, IRQ 54
extern "C" void USART2_IRQHandler(void);   // veneer slot 47, IRQ 38
extern "C" void EXTI2_IRQHandler(void);    // fatal_error_reset(0x7A)
extern "C" void EXTI9_5_IRQHandler(void);  // fatal_error_reset(0x85)
```

Each is a shim that clears its pending flag and delegates to the owning service. They
must be `extern "C"` and **non-weak**: a C++-mangled name silently leaves the weak trap
handler from `crt0.S` in place, and the box resets with a trap code instead of working.

`crt0.S`'s weak defaults carry non-stock trap codes (217, 229, 232, 237, 247) precisely
so an unported handler is identifiable. Seeing one of those in
`ram::AppRam::fatalErrorCode` means a handler name did not match.

The TIM2 divider chain is the one place the layering inverts: the 1 ms interrupt drives
10 ms and 100 ms work owned by several services. `onTimerInterrupt()` fans out —
`svc::sched`, `svc::timers`, `svc::clock`, `svc::can::timersTick()` — instead of any
service reaching sideways into another.

---

## 6. Language rules

C++17, freestanding, following MISRA C++:2023 where it applies to a bare-metal image
with a fixed memory map. The enforced subset and every deviation are in
`docs/misra-cpp.md`. The short version:

- no dynamic allocation, no exceptions, no RTTI, no virtual functions;
- no namespace-scope objects with runtime constructors — this image never runs
  `.init_array`, and `app/linker/app.ld` fails the build if one appears;
- `constexpr` over `#define`, `enum class` over bare enums, `std::uint8_t` over
  `unsigned char`;
- no `reinterpret_cast` anywhere in the firmware;
- every function that can be `noexcept` is;
- comments explaining *why the stock image does this* are load-bearing documentation.
  Carry them over. They are the reverse-engineering record.

---

## 7. Tests — `tests/host/`

The pure-logic units build for the host with `-DCANBOX_HOST_TEST` and link against recording
fakes in `tests/host/fake_hal.cpp` instead of `app/hal/`. Run them with `python tools/test.py`.

This is the only evidence of equivalence available without a car on a bench, so the tests are
written from the behavioural spec in `reference/decompile/app/*.c` — not from the refactored
code. A test that merely restates the implementation proves nothing.

**What is covered:** every service and every logic module, plus the five interrupt entry
points in `app/logic/interrupts.cpp`, which are ordinary functions a test can call directly.

**What is not, and why:** `app/logic/main_loop.cpp`. Its one external symbol is `main_loop`,
which does not clash with the runner's `main()` — but phase 2 is an unbounded superloop and
the phase bodies are file-local, so there is nothing a host test can call. It is covered
instead by a statement-by-statement audit against `reference/decompile/app/sys.c:455`.

### The reset contract — read this before writing a test that resets the box

`wwdg_force_reset()` never returns on target. The host fake **records the call and returns**,
and `app/hal/hal_wwdg.h` drops the `noreturn` attribute in the host build so callers do not
emit an `unreachable` after it. That pairing is load-bearing: a fake that returns from a
function the compiler believes cannot return drops the process into undefined behaviour, and
the symptom is a runner that prints nothing and exits 0 — the crash takes the unflushed
stdout buffer with it.

Consequences:

- `svc::power::shutdownDeepSleep()` ends *with* the reset, so it is fully testable and its
  ordering is asserted — that path's failure mode is a box that never wakes.
- `fatal_error_reset()` is `for (;;) { ...; wwdg_force_reset(); }`, so calling it from a test
  spins forever. Assert on the fault **codes** and on the call, never by invoking it.

Escaping the reset instead — so a test could resume afterwards — was tried both ways and does
not work here. Throwing dies because the path crosses `noreturn`, `[[noreturn]]` and
`noexcept` boundaries; `longjmp` dies because on this Windows/MSVC target it unwinds through
the intervening frames and corrupts the process.
