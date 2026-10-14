# Requirements — rebuilt Hiworld canbox APP firmware

Box **2429 / H1N4JPF20A**, Nations **N32L406CBL7** (Cortex-M4F). This document says **what**
the application image must do and **how well**, and what evidence exists for each claim today.
It does not describe how the code is structured — that is `docs/ARCHITECTURE.md`.

Every requirement is a reproduction of an existing product. Where the stock firmware does
something odd, **the odd thing is the requirement**. Nothing here is derived from general
knowledge of automotive firmware; each row cites a stock flash address, a file and line, a
section of an analysis document, or a host test name.

| | |
|---|---|
| Functional requirements | **FR-1 … FR-107** |
| Non-functional requirements | **NFR-1 … NFR-24** |
| Host unit tests today | 310, 0 failing (`python tools/test.py`) |
| Image size today | `text 11798`, `data 0`, `bss 2604` (`arm-none-eabi-size build/app.elf`) |
| Hardware runs to date | **none** |

---

## 1. Purpose and scope

### 1.1 What this firmware is

The box sits between a Jeep/Chrysler interior CAN bus and an Android head unit. It:

* receives every frame on one CAN bus, decodes a small set of identifiers into a vehicle-state
  block, and reports that block to the head unit over a 38400 8N1 serial link using the Hiworld
  `AA 55` framing;
* accepts a short command set from the head unit (cluster text, amplifier/tone, vendor
  handshake, reset) and turns it into CAN frames the car understands;
* transmits a small periodic frame set onto the vehicle bus while the ignition is on;
* mirrors four decoded flags onto hardwired status outputs;
* keeps six configuration bytes in a flash parameter page;
* auto-detects which of two CAN bit rates (and therefore which of two decode profiles) the
  car uses.

### 1.2 What this firmware is explicitly NOT responsible for

| Not in scope | Why |
|---|---|
| The IAP bootloader at `0x08000000`–`0x08001FFF` | Never rebuilt, never erased, never written. It is the only way to reflash the box (`AGENTS.md` "Hard constraints" 1; `app/REBUILD.md` §1). |
| The Cortex-M vector table | The app has none. It runs on the bootloader's at `VTOR = 0x08000000` (`app/REBUILD.md` §2; `app/linker/app.ld:21-27`). |
| Clock-tree bring-up (HSE → PLL → 64 MHz) and `MSP` | Done by the bootloader before entry (`docs/hiworld-startup-sequence.md` §1 row 0, §3). |
| Writing the application region at runtime | The only live flash writer is `svc::params::save()`, and it addresses only the parameter page (`app/services/param_store.cpp:107-137`, `app/hal/hal_flash.c:214`). |
| The valid-app marker `*0x08001800 = 0x19780217` | Written by the bootloader after a good flash; the build never emits it (`app/REBUILD.md` §2). |
| Firmware update transport | The `.iap` container and the UART update protocol live in the bootloader; the app's only involvement is UART command `0xE0` (reset into the loader). |
| Vehicle signals the stock box does not decode | Speed, battery volts, temperatures, fuel, odometer, handbrake, belts, hood, locks, indicators, parking radar, HVAC — none are decoded by either profile (`docs/hiworld-firmware-analysis.md` §3.8 "Not decoded"). Adding any is a product change, not a rebuild. |
| Byte-identity with the stock code body | Only the preamble `0x000..0x209` is targeted for byte-identity; the body is a different compiler (`app/REBUILD.md` §4, §7). |

---

## 2. Functional requirements

### 2.1 Boot sequence

| ID | Requirement | Evidence |
|---|---|---|
| **FR-1** | `main_loop` shall be entered through the fixed veneer at `0x08002202` → `_start_c` → `main_loop`, and shall never return. If it returns, `fatal_error_reset(100)` shall run. | `app/startup/startup.c:70-88`; stock `0x080088D8` |
| **FR-2** | The entire boot block shall execute with interrupts masked (`CPSID i` on entry, `CPSIE i` only at the top of phase 1). | `app/logic/main_loop.cpp:182`, `:100` |
| **FR-3** | The boot block shall run in exactly this order: parameter load → APB prescalers `/4` → TIM2 1 ms time base → PWR clock enable → status GPIO init → soft clock init → USART2 + version string → parameter validate/default → CAN TX template prime → CAN bring-up at the stored profile → watchdog arm. | `app/logic/main_loop.cpp:69-87`; stock `0x08002DBC`, `0x080032D2`, `0x08008458`, `0x080078DA`, `0x08008A54`, `0x08007B6C`, `0x08007B2C`, `0x08002DEC` |
| **FR-4** | The bus prescalers shall be programmed **before** the TIM2 time base, because the 1 ms reload arithmetic assumes `PCLK1 = 16 MHz`. | `app/logic/main_loop.cpp:62-67`, `:73-74` |
| **FR-5** | The PWR peripheral clock shall be enabled during boot, not in the shutdown path, although only the deep-sleep entry uses the PWR block. | `app/logic/main_loop.cpp:75-77` |
| **FR-6** | TIM2 shall be programmed `PSC = 7`, `AR = 4000`, up-counting, update interrupt enabled, giving exactly 1000 Hz from a 32 MHz kernel clock; the prescaler shall be written twice (once via the time-base struct as 0, once via an explicit reload with an update event). | `app/services/scheduler.cpp:29-69`; `app/include/app_config.hpp:43-45`; stock `0x080027F8` |
| **FR-7** | The window watchdog shall be armed as the last boot step with `WDGTB = /4`, `W = 0x7F`, `CR = 0xFF`, i.e. a ~65.5 ms timeout and no closed window. | `app/logic/main_loop.cpp:86`; `docs/hiworld-startup-sequence.md` §8 |
| **FR-8** | The 17-byte version string `H1N4JPF20A-220524` shall be copied into the `g_versionString` symbol at init, not NUL-terminated on the wire. | `app/services/head_unit.cpp:623-631`; `app/include/app_config.hpp:159-162` |

### 2.2 Phase 1 — CAN bit-rate / profile autodetection

| ID | Requirement | Evidence |
|---|---|---|
| **FR-9** | Phase 1 shall spin until the CAN RX ring write index has reached **3**, i.e. three frames of any identifier have been received, and shall kick the watchdog **before** evaluating that test on every pass (so the exiting pass has also kicked). | `app/logic/main_loop.cpp:102-106`; `docs/hiworld-startup-sequence.md` §5 |
| **FR-10** | On each 2048 ms flag while still searching, the firmware shall set bit 6 (`kFlagAutobaud`) of the vehicle-state flags byte, so the head unit sees "still looking". | `app/logic/main_loop.cpp:113`; `app/include/ram_map.hpp:250` |
| **FR-11** | While `autobaudTries < 8`, each 2048 ms retry shall: toggle the stored profile byte (`!= 1 → 1`, else `0`), save the parameter page, re-initialise CAN at the other bit rate with interrupts masked, increment the try counter and clear the 2048 ms flag — in that order. | `app/logic/main_loop.cpp:123-138`; stock `0x080088D8` body in `docs/hiworld-startup-sequence.md` §5 |
| **FR-12** | The profile toggle shall be a **flip-flop between exactly two profiles**, not a modular cycle; the retry budget of 8 shall remain a whole number of two-profile sweeps. | `app/logic/main_loop.cpp:125-130`; `app/include/app_config.hpp:97-105` |
| **FR-13** | Once `autobaudTries >= 8`, the shutdown/deep-sleep path shall run on the NEXT 2048 ms tick — so with no bus the box goes quiet at ≈18.4 s, not ≈16.4 s (the 8th retry is at 16.38 s; the give-up branch needs one further tick to observe the counter at 8) and the 2048 ms flag shall **not** be cleared, so the path re-runs on every subsequent loop pass until the WFI takes effect. | `app/logic/main_loop.cpp:115-121`; `app/services/scheduler.hpp` "autobaudDue"; `docs/misra-cpp.md` N-3 |
| **FR-14** | Nothing shall be transmitted in phase 1: neither the CAN TX pump nor the head-unit service is called, so the box is silent on both CAN and UART until the bus answers. | `app/logic/main_loop.cpp:102-139` (no TX calls); `docs/hiworld-startup-sequence.md` §7, §9.2 |
| **FR-15** | On leaving phase 1 the firmware shall: latch `profileActive` from the parameter byte, re-run parameter validation, re-prime the TX templates, re-apply the bit rate **without masking interrupts**, set the bus state to `kBusHello` and load the 800-pass bus-present debounce. | `app/logic/main_loop.cpp:190-204`; stock `0x08007B4C` |
| **FR-16** | Profile 1 shall select 125 kbit/s and profile 0 (this box) 83.333 kbit/s, with `BRP = 2000 / kbps` (truncating to 24 for 83), 8 tq per bit, `SJW = 1`, `BS1 = 4`, `BS2 = 3`, sample point 62.5 %. | `app/services/can_bus.cpp:65-70`, `:206-210`; `app/include/app_config.hpp:106-108`; `docs/hiworld-firmware-analysis.md` §3.1 |
| **FR-17** | A stored profile byte that is neither 0 nor 1 shall be **normalised to 0 and written back into the RAM mirror** by the bit-rate selector (not rejected), so the next parameter save commits it. | `app/services/can_bus.cpp:267-286`; test `can_set_baud_by_profile_normalises_a_corrupt_profile_byte` |
| **FR-18** | Each CAN (re-)initialisation shall configure PB8/PB9 as AF push-pull, pulse `can_deinit`, program the bit timing, install one accept-all filter, enable FIFO-0 message-pending interrupt, configure PB12 as an output twice, drive PB12 **low** (transceiver awake), set the bus state to `kBusSettling` and load the activity counter with 200 (= 20 s at the 100 ms hook). | `app/services/can_bus.cpp:152-172`, `:183-229`; stock `0x08002A90`, `0x08007482` |

### 2.3 CAN reception and ring buffering

| ID | Requirement | Evidence |
|---|---|---|
| **FR-19** | Hardware filtering shall accept every identifier: one 32-bit mask-mode bank, id 0 / mask 0, routed to FIFO 0. All real filtering is in software. | `app/services/can_bus.cpp:215-226`; `docs/hiworld-firmware-analysis.md` §3.2 |
| **FR-20** | The RX interrupt shall push each frame into a 50 × 16-byte ring as `[0..1]` id bits 31..16, `[2..3]` id bits 15..0, `[5]` DLC, `[6..13]` data, advancing `rxHead` modulo 50. | `app/services/can_bus.cpp:400-436`; `app/include/ram_map.hpp:135-148` |
| **FR-21** | When fewer than two ring slots are free the RX interrupt shall still read the mailbox (releasing FIFO 0), discard the frame and set `kCanErrRxRingFull`. | `app/services/can_bus.cpp:408-418`; test `can_rx_interrupt_with_one_free_slot_sets_the_ring_full_flag` |
| **FR-22** | The RX interrupt shall **not** write the IDE byte of a ring entry; the stale value from the previous occupant is left in place and never read. | `app/services/can_bus.cpp:430-433` |
| **FR-23** | `txEnqueue()` shall never be called from an interrupt: `txHead` is a read-modify-write with a 16-byte copy in the middle and the TX ring is single-threaded main-loop state. | `app/include/ram_map.hpp:110-128` |
| **FR-24** | Each decoder call shall make **at most 20 passes**; on an empty ring the pass is still consumed (the loop `continue`s rather than returning), so the pass count and not the ring depth ends the call. | `app/logic/can_decode.cpp:312-318`, `:349-352`; `app/logic/can_decode_shared.hpp:38`; tests `decoder_consumes_at_most_twenty_frames_per_call`, `decoder_on_an_empty_ring_changes_nothing` |
| **FR-25** | Every accepted frame shall reload the bus-alive counter, with **different reloads per profile**: 100 on profile 0, 0x14 (20) on profile 1. | `app/logic/can_decode.cpp:321`, `:355`; `app/logic/can_decode_shared.hpp:42-43`; test `bus_alive_reload_differs_between_the_profiles` |
| **FR-26** | The dispatch gate shall, before decoding: reload the 800-pass debounce and **set** bit 4 of the `0x18C` periodic frame while ACC is off; clear that bit and re-prime the amplifier frame from the amp level once the debounce has expired with ACC on; otherwise count the debounce down by one. | `app/logic/can_decode.cpp:393-412`; tests `dispatch_gate_reloads_the_debounce_while_acc_is_off`, `dispatch_gate_clears_the_marker_when_the_debounce_expires` |
| **FR-27** | The decoder shall be selected by `profileActive`: value 1 → FCA 125 kbit/s decoder, **anything else** → Jeep 83.333 kbit/s decoder. | `app/logic/can_decode.cpp:414-418`; test `dispatch_gate_selects_the_profile_from_profile_active` |

### 2.4 Decoded signals — profile 0 (Jeep WK, 83.333 kbit/s, THIS box)

| ID | Requirement | Evidence |
|---|---|---|
| **FR-28** | From CAN `0x000` byte 0: `kFlagIgnRaw` (bit 4) ← `d[0] & 0x01`; `kFlagAccOn` (bit 0) ← `(d[0] & 0xE0) > 0x10`. The threshold form shall be kept verbatim even though it is equivalent to "any of bits 5..7". | `app/logic/can_decode_shared.hpp:211-218`; tests `jeep_ignition_decodes_acc_and_the_raw_bit`, `jeep_ignition_ignores_bit4_of_the_source_byte` |
| **FR-29** | From CAN `0x003` byte 2 low nibble: reverse is set when the nibble is **1 or 9**, cleared otherwise. | `app/logic/can_decode.cpp:82-88`; test `jeep_reverse_nibble_sets_and_clears_the_flag` |
| **FR-30** | From CAN `0x013` byte 2: doors map source bit 0→LF, 1→RF, **3→LR, 2→RR**, 4→tailgate onto destination bits 7,6,5,4,3 of the door byte. The 2/3 crossover is load-bearing. | `app/logic/can_decode_shared.hpp:123-142`; test `jeep_doors_map_every_source_bit_to_the_right_destination_bit` |
| **FR-31** | From CAN `0x015` bytes 4..5: steering angle = big-endian raw, centre `0x1000`, 0.5°/LSB, stored big-endian; raw `0` is reported as `0` without centring; raw `== 0x1000` reports 0; raw above centre is a plain two's-complement negation. There is **no clamp** on this profile. | `app/logic/can_decode.cpp:54-79`; test `jeep_steering_centre_both_directions_and_the_zero_special_case` |
| **FR-32** | The steering angle shall be updated **only while the reverse flag is set**; outside reverse the last value stands. | `app/logic/can_decode.cpp:58-60`; test `jeep_steering_writes_nothing_while_not_in_reverse` |
| **FR-33** | From CAN `0x210` byte 1: `kFlagIllumOn` ← `d[1] != 0`; illumination level ← `min(d[1] >> 1, 100)`. | `app/logic/can_decode_shared.hpp:193-204`; tests `illum_flag_follows_nonzero_and_percent_is_half_the_raw`, `illum_percent_clamps_at_100` |
| **FR-34** | From CAN `0x3A0` bytes 0..1: the SWC key byte shall take the plugin key codes 0x0B (mode), 0x01/0x02 (vol ±), 0x08/0x09 (next/prev), 0x03 (mute), 0x12 (spare, unmapped in the Android plugin), 0x05 (phone), 0x07 (voice); `d[0] == 0 && d[1] == 0` reports 0x00. | `app/logic/can_decode_shared.hpp:150-188`, `:78-87`; test `swc_buttons_report_the_plugin_key_code_for_every_group` |
| **FR-35** | The SWC tests shall be independent `if`s, so a frame with several bits set reports the **last** matching group, not the first. | `app/logic/can_decode_shared.hpp:146-149`; test `swc_buttons_last_match_overwrites_earlier_ones`; `docs/misra-cpp.md` N-5 |
| **FR-36** | From CAN `0x500` bytes 0..1: copy to the echo buffer, zero-pad to 8 bytes and retransmit as standard id `0x516`, DLC 8. | `app/logic/can_decode.cpp:92-105`; test `jeep_echo_500_retransmits_as_516_zero_padded` |
| **FR-37** | Profile 0 shall decode **no** gear, RPM or vehicle speed. UART message `0x32` therefore stays all zeros on this profile for ever. | `app/logic/can_decode.cpp:310-342` (no gear/rpm cases); `docs/hiworld-firmware-analysis.md` §3.6 note |

### 2.5 Decoded signals — profile 1 (FCA, 125 kbit/s, not this car)

| ID | Requirement | Evidence |
|---|---|---|
| **FR-38** | From CAN `0x20B` byte 0: identical ignition decode to FR-28. | `app/logic/can_decode.cpp:362`; test `fca_ignition_decodes_acc_on_its_own_id` |
| **FR-39** | From CAN `0x1E1` bytes 2..3: same centre and scale as FR-31, but clamped — the positive side at `0x21B`, and the negative side at **`0x21C`** — and the negative half encoded as **`0x10001 - v`** (not `0x10000`), truncated to 16 bits. Gated on reverse like FR-32. | `app/logic/can_decode.cpp:112-170`; tests `fca_steering_clamps_both_sides_at_0x21b`, `fca_steering_negative_half_uses_the_0x10001_encoding` |
| **FR-40** | From CAN `0x20E` byte 2 low nibble: 0→P(1), 2→N(2), 4→D(4), 9→leave the gear unchanged, anything else→invalid(0). Then bit 3 of the same byte sets/clears reverse and, when set, **forces the reported gear to R(3)**. | `app/logic/can_decode.cpp:179-200`; test `fca_gear_maps_the_prndl_nibble` |
| **FR-41** | Nibble 9 shall be unreachable as a "hold" outcome: 9 has bit 3 set, so every nibble from 8 to F reports R. The hold branch exists but can never be observed. | `app/include/ram_map.hpp:373-377`; test `fca_gear_nibble_9_holds_in_the_chain_but_bit3_still_forces_r` |
| **FR-42** | From CAN `0x211` bytes 0..1: engine speed is **decimated 1-in-21** (a counter runs 0..0x14; only the pass that finds it already at 0x14 decodes and resets it) and values ≥ 8000 rpm are rejected outright. | `app/logic/can_decode.cpp:207-223`; tests `fca_rpm_is_decimated_one_frame_in_twenty_one`, `fca_rpm_rejects_8000_and_above` |
| **FR-43** | From CAN `0x244` byte 0: doors, same permutation as FR-30. From CAN `0x308` byte 1: illumination, same as FR-33. From CAN `0x3A3`: SWC buttons, same as FR-34/FR-35. | `app/logic/can_decode.cpp:364-367`; tests `fca_doors_…`, `fca_illum_uses_its_own_id`, `fca_swc_buttons_use_their_own_id` |
| **FR-44** | From CAN `0x513` bytes 0..3: copy four bytes, zero-pad to 8 and retransmit as `0x516` DLC 8. | `app/logic/can_decode.cpp:226-239`; test `fca_echo_513_retransmits_four_bytes_as_516` |

### 2.6 Vendor / service frames (both profiles)

| ID | Requirement | Evidence |
|---|---|---|
| **FR-45** | CAN `0x7FC` carrying magic `3F F3 F2 01 ?? ?? 3A A3` shall be echoed verbatim as `0x7FD`, DLC 8. The comparison order (d0, d1, d6, d7, d2, d3) shall be preserved. | `app/logic/can_decode_shared.hpp:223-232`; test `vendor_7fc_echoes_as_7fd_only_on_the_exact_magic` |
| **FR-46** | CAN `0x7FF` carrying magic `7F F7 x x y y 3A A3` with `d[2] == d[3]` and `d[4] == d[5]` shall write flags bits 0..3 (ACC, illum, reverse, spare) from `d[2]` bits 0..3. A non-matching frame shall change nothing. | `app/logic/can_decode_shared.hpp:241-259`; test `vendor_7ff_writes_the_four_status_bits_only_on_the_exact_magic` |
| **FR-47** | On profile 0, `0x7FF` shall produce **no reply**. | `app/logic/can_decode.cpp:335-336`; test `jeep_vendor_7ff_does_not_reply` |
| **FR-48** | On profile 1, a matching `0x7FF` shall additionally map `d[4]` 0..6 into the **SWC key slot** (0→0, 1→VolDn 0x02, 2→VolUp 0x01, 3→Prev 0x09, 4→Next 0x08, 5→Mode 0x0B, 6→Mute 0x03); values 7..255 leave the slot unchanged. | `app/logic/can_decode.cpp:254-263`; test `fca_vendor_7ff_replies_on_7fe_and_maps_d4_into_the_key_slot` |
| **FR-49** | On profile 1, a matching `0x7FF` shall also emit a one-byte reply on CAN `0x7FE` reporting the head unit's current display mode, written as four independent set-or-clear steps. | `app/logic/can_decode.cpp:265-302` |
| **FR-50** | That display-mode reply shall reproduce the stock bug: **mode 4 is tested against bit 2 again** instead of bit 3, and its `else` branch clears bit 2. The wire values are therefore mode 1 → `0x01`, mode 2 → `0x02`, **mode 3 → `0x00` (unreportable, indistinguishable from "no mode")**, mode 4 → `0x04`. This must not be "fixed". | `app/logic/can_decode.cpp:290-300`; `docs/misra-cpp.md` N-4; test `fca_vendor_7ff_display_mode_4_reuses_mode_3_bit` |
| **FR-51** | Identifiers matching `0x400..0x43F` shall be treated as the Hiworld node/SWC service protocol: the **low 6 bits of the identifier are the key index**, which arms that soft-timer slot to 150 counts (1.5 s). | `app/logic/can_decode_shared.hpp:271-281`; test `swc_service_takes_the_key_index_from_the_low_six_bits_of_the_id` |
| **FR-52** | Within that block, `d[0] != 0xFD` shall end processing; `d[1] == 0x16` shall set the response mode to 9 and stage a `0xFD` hello reply; `d[1] == keydataRef` shall stage and send a `0xFE` key reply on `0x416` DLC 8. | `app/logic/can_decode_shared.hpp:282-298`; tests `swc_service_ignores_a_frame_that_is_not_a_key_report`, `swc_service_handshake_sets_the_response_mode`, `swc_service_repeat_key_replies_on_0x416` |
| **FR-53** | The `d[2]` probe shall be qualified by the ACC flag: `0x3F` with ACC on and `0xBF` with ACC off update the reply status byte; `0xFF` with ACC off shall set the bus state to `kBusDown` **and flush the whole CAN TX queue**. | `app/logic/can_decode_shared.hpp:312-323`; tests `swc_service_probe_is_qualified_by_the_acc_flag`, `swc_service_bus_down_probe_clears_the_status_and_flushes_tx` |

### 2.7 Vehicle state block

| ID | Requirement | Evidence |
|---|---|---|
| **FR-54** | The decoded state shall live in one 0x34-byte block with four **overlapping** head-unit windows: `0x72` at offset 0x00 len 14, `0x12` at 0x13 len 10, `0x32` at 0x1D len 14, `0xA6` at 0x2C len 8. These relative offsets are the wire protocol. | `app/include/ram_map.hpp:191-237`, `:357-371`; tests in `tests/host/test_ram_map.cpp` |
| **FR-55** | The flags byte shall carry: bit 0 ACC, bit 1 illumination, bit 2 reverse, bit 3 spare, bit 4 raw ignition, bit 5 CAN-error latch, bit 6 autobaud-searching, bit 7 latched profile. | `app/include/ram_map.hpp:239-251` |
| **FR-56** | On every superloop pass, if the change-source byte differs from its shadow, the shadow shall be updated and the change counter incremented — the counter the head unit watches. | `app/logic/main_loop.cpp:152-158` |
| **FR-57** | The five stored tone bytes shall be aliased into the `0x12` window at payload bytes `[4..9]` as well as into the `0xA6` window, reproducing the documented "`0A 0A 0D 0A 0A` tail" quirk. `0x12` payload bytes `[0] [1] [3]` are never written. | `app/logic/periodic_tx.cpp:490-513`; `docs/hiworld-firmware-analysis.md` §4.4; test `prime_tx_templates_duplicates_the_tone_tail` |
| **FR-58** | The reported amplifier level shall be primed to `0x1E` (30) on profile 1 and `0x14` (20) on profile 0. | `app/logic/periodic_tx.cpp:515-517`; test `prime_tx_templates_amp_level_follows_the_profile` |

### 2.8 Periodic CAN transmission

| ID | Requirement | Evidence |
|---|---|---|
| **FR-59** | A 151-phase counter (0..0x96) shall advance one step per 8 ms call, **before** the table is consulted, so the first call after a reset transmits on phase 1. | `app/logic/periodic_tx.cpp:620-628`; tests `periodic_tx_phase_advances_exactly_one_per_call`, `periodic_tx_lap_is_151_phases_long` |
| **FR-60** | Only the odd phases 1, 3, 5, 7, 9, 0x0B shall carry a frame; phase 0x0D is an explicit empty case and 0x0E..0x96 are dead time. | `app/logic/periodic_tx.cpp:354-407`; test `periodic_tx_even_phases_and_phase_0d_send_nothing` |
| **FR-61** | Profile 0 shall emit `0x3D0`/7, `0x0F0`/4, `0x326`/8, `0x190`/7, `0x18C`/8, `0x09F`/8 on those phases; profile 1 shall emit `0x3D9`/7, `0x2E9`/4, `0x290`/8, `0x291`/7, `0x293`/8 and **nothing at all on the 0x09F phase**. | `app/logic/periodic_tx.cpp:39-65`, `:382-402`; tests `periodic_tx_full_151_phase_sweep_emits_each_profile0_frame_once`, `periodic_tx_profile1_has_no_frame_on_the_9f_phase` |
| **FR-62** | Any latched profile other than 0 or 1 shall transmit **nothing** (the stock tests are `== 0` / `== 1` with no else). | `app/logic/periodic_tx.cpp:403-406`; test `periodic_tx_an_unknown_profile_transmits_nothing_at_all` |
| **FR-63** | An ACC edge shall immediately prime `txFrame9f[0]` with 1 (ACC on) or 7 (ACC off), using a whole-byte shadow holding 0 or 1. | `app/logic/periodic_tx.cpp:578-586`; tests `periodic_tx_acc_rising_edge_primes_the_9f_frame_with_1`, `…_falling_edge_primes_the_9f_frame_with_7` |
| **FR-64** | The post-ACC hold shall be pinned at 400 while ACC is on and count down one per 8 ms call once ACC drops; at zero all periodic transmission stops and the **phase counter is not advanced**, so the table resumes where it stopped (400 × 8 ms = 3.2 s). | `app/logic/periodic_tx.cpp:588-602`; tests `periodic_tx_hold_timer_is_pinned_at_400_while_acc_is_on`, `periodic_tx_everything_stops_once_the_hold_timer_reaches_zero` |
| **FR-65** | Bit 7 of the flags byte shall be rewritten from `profileActive` on every scheduler pass that is not held off, preserving the low seven bits. | `app/logic/periodic_tx.cpp:606-614`; test `periodic_tx_republishes_the_latched_profile_in_bit_7_of_the_status_byte` |
| **FR-66** | A pending `0xD2` cluster-text block shall be consumed (flag cleared, translation run) only while the hold timer is non-zero; with ACC long gone it stays pending for ever. | `app/logic/periodic_tx.cpp:630-636`; test `periodic_tx_consumes_the_cluster_new_data_flag_and_runs_the_translation` |
| **FR-67** | Cluster text shall translate the `0xD2` sub-class into the `0x190`/`0x18C`/`0x09F` buffers: class 0 → code 0x1D; classes 1..3 → code 1 with the numeric value whose **units digit comes from cluster byte 8**; classes 4..5 → code 0 with the units digit from **byte 7**; classes 6,7,0x0D,0x0E,0x15,0x16 → code 4 sub-code 3 (class 7 overwrites the sub-code with 8) and a three-digit value from bytes 1..3 parsed only if byte 1 is an ASCII digit; classes 0x0A/0x0C → codes 8/6; everything else accepted and ignored. On the numeric classes `txFrame9f[1]` is OR-ed with 0x20, on the text classes it is not. | `app/logic/periodic_tx.cpp:199-218`, `:268-349`; tests `cluster_text_class1_…`, `cluster_text_class4_takes_the_units_digit_from_byte_7_not_byte_8`, `cluster_text_class7_overwrites_the_sub_code_with_8` |
| **FR-68** | A pending `0xAD` tone/volume command shall be clamped (selector 1 → ≤ 0x26; otherwise 0 → 1 and ≥ 0x14 → 0x13), written back into the command block, then applied to **three places at once** — the amplifier CAN frame, the vehicle-state byte and the parameter page — and the amplifier frame transmitted immediately rather than at the next lap. Selectors 0 and 7..255 write nothing but still trigger the transmit. | `app/logic/periodic_tx.cpp:415-454`, `:638-655`; tests `periodic_tx_volume_is_clamped_to_0x26`, `periodic_tx_tone_zero_is_pulled_up_to_one`, `periodic_tx_tone_command_lands_in_all_three_places_for_every_selector` |
| **FR-69** | `txEnqueue()` shall drop the frame and OR `kCanErrTxRingFull` when the ring is full; bytes past DLC in a reused slot keep the previous occupant's content. | `app/services/can_bus.cpp:293-323`; tests `can_tx_enqueue_on_a_full_ring_drops_the_frame`, `can_tx_enqueue_copies_only_dlc_bytes` |
| **FR-70** | `txDrain()` shall transmit at most 10 entries per 4 ms pass; on "no free mailbox" it shall **assign** (not OR) `kCanErrTxNoMailbox`, bump a stall counter that wraps to 0 at 100, leave the frame queued and return. | `app/services/can_bus.cpp:357-387`; tests `can_tx_drain_stops_after_ten_frames_per_pass`, `can_tx_drain_assigns_the_error_flag_rather_than_or_ing_it`, `can_tx_drain_stall_counter_wraps_at_one_hundred` |

### 2.9 Link state machine

| ID | Requirement | Evidence |
|---|---|---|
| **FR-71** | The 8 ms vehicle task shall run, in order: decode gate → SWC replies → one periodic-TX phase → CAN-error hold → bus-presence machine. | `app/logic/can_link.cpp:222-238`; test `link_full_pass_decodes_replies_and_steps_the_table_in_one_call` |
| **FR-72** | An immediate SWC reply (`swcRespMode == 1`) shall send `0x416` DLC 8 and arm the repeat timer to 0x28 (40 × 8 ms = 320 ms); the repeat (`swcFlag == 1`) shall release the previously reported soft-timer slot, rescan, write the surviving key index into reply byte 1 and send again. | `app/logic/can_link.cpp:87-110`; tests `link_immediate_swc_reply_sends_0x416_and_arms_the_repeat`, `link_repeat_swc_reply_releases_the_old_slot_and_reports_the_next_key` |
| **FR-73** | The CAN-error hold shall reload to 5 (× 100 ms) while **bit 2** of the flags byte is set and clear **bit 5** once the hold reaches zero. The two bits are deliberately different and must not be collapsed. | `app/logic/can_link.cpp:113-127`; tests `link_canerr_hold_is_rearmed_while_bit_2_is_set`, `link_canerr_latch_bit_5_is_cleared_once_the_hold_expires` |
| **FR-74** | `kBusHello` shall emit the constant greeting `FE 16 3F FF FF FF FF FF` on `0x416` exactly once and drop to `kBusRunning`. | `app/logic/can_link.cpp:130-142`, `:202-206`; test `link_hello_emits_the_0x416_greeting_once_and_drops_to_running` |
| **FR-75** | The bus-presence machine shall behave as: `kBusDown` + bus dead → deep-sleep path, then (only if CAN refused to sleep) a forced reset; `kBusDown` + bus alive → `kBusSettling` with a 10-count (1 s) timer; `kBusSettling`/`kBusLost` + alive → `kBusRunning` and status byte 0x3F, or timer expired → `kBusDown`; `kBusRunning` + bus lost → `kBusLost`, status 0xBF, timer 100 (10 s); `kBusRunning` + timer expired → forced reset; any out-of-range state → `kBusSettling`. | `app/logic/can_link.cpp:156-214`; tests `link_down_state_moves_to_settling_…` through `link_an_out_of_range_state_byte_restarts_at_settling` |

### 2.10 Head-unit serial link

| ID | Requirement | Evidence |
|---|---|---|
| **FR-76** | USART2 shall run 38400 8N1 on PA2 (TX) / PA3 (RX), RX interrupt always enabled and TX interrupt armed per frame, with PA3 configured before PA2 and `BRCF = 0x01A1` derived from `PCLK1 = 16 MHz`. | `app/services/head_unit.cpp:234-270`; tests `head_unit_init_configures_usart2_with_pclk1_and_38400`, `head_unit_init_brings_up_the_port_in_stock_order` |
| **FR-77** | The frame format shall be `AA 55 | LEN | CMD | payload[LEN] | CKS`, wire length `LEN + 5`, `CKS = (LEN + CMD + Σpayload − 1) & 0xFF` (algebraically the full byte sum including the preamble). There is **no escaping**: payload bytes may be `0xAA`/`0x55`. | `app/services/head_unit.cpp:419-460`; `app/include/app_config.hpp:136-150`; tests `head_unit_checksum_firmware_form_equals_full_sum`, `head_unit_payload_may_contain_preamble_bytes` |
| **FR-78** | The parser shall be a byte-at-a-time state machine over a 200-byte ring: wait for `0xAA`, require `0x55`, reject `LEN >= 0x38`, wait for `LEN + 2` further bytes before committing, then accumulate and compare the final byte with `(sum − 1) & 0xFF`. Exactly one frame is processed per call. | `app/services/head_unit.cpp:324-417`; tests `head_unit_rejects_oversized_length`, `head_unit_accepts_maximum_length`, `head_unit_partial_frame_is_retried_next_pass` |
| **FR-79** | The RX ring tail shall be committed only when the machine is back in state 0 or a frame has completed, so a half-received frame is re-parsed from its `0xAA` on the next call and no timeout is needed. The `5A A5` dialect shall never be accepted. | `app/services/head_unit.cpp:335-345`, `:370-376`; test `head_unit_rejects_5A_A5_dialect` |
| **FR-80** | The RX interrupt shall drop a byte and latch `uartRxOverflow` when advancing head would meet tail; the flag is never cleared again. | `app/services/head_unit.cpp:676-688`; test `head_unit_isr_rx_ring_overflow_drops_byte` |
| **FR-81** | The outbound table shall be exactly six slots in transmit order `0x72`/14, `0xA6`/8, `0x73`/8, `0xF0`/17, `0x12`/10, `0x32`/14, each with its own shadow copy. | `app/services/head_unit.cpp:126-134`; `docs/hiworld-firmware-analysis.md` §4.1 |

> The remaining head-unit behaviour is numbered FR-82 … FR-88 below; the link is the largest
> single capability in the box and splitting it keeps each row atomic.

| ID | Requirement | Evidence |
|---|---|---|
| **FR-82** | The transmit scheduler shall run from the 8 ms slot and emit at most one data frame every **6th pass (48 ms)**; on a sweep where no payload differs from its shadow it shall wait 15 idle sweeps (~720 ms) and then send the next table entry anyway as a keep-alive, using a separate auxiliary index. | `app/services/head_unit.cpp:491-581`; tests `head_unit_scheduler_emits_only_every_sixth_pass`, `head_unit_keepalive_waits_fifteen_idle_sweeps` |
| **FR-83** | The round-robin shall send the first slot whose live payload differs from its shadow, refresh that shadow and leave the index on that slot; a full sweep with no change parks the index on the sentinel value 6. | `app/services/head_unit.cpp:462-489`; tests `head_unit_round_robin_sends_changed_slot_and_updates_shadow`, `head_unit_round_robin_skips_unchanged_slot` |
| **FR-84** | Message `0x73` shall be transmitted as **eight zero bytes for ever**: nothing in the image ever writes its buffer. | `app/services/head_unit.cpp:111-113`; `docs/hiworld-firmware-analysis.md` §4.7 |
| **FR-85** | Inbound `0xD2` (13 bytes) and `0xAD` (2 bytes) shall be copied into their destinations **only when the payload actually changed**, and the matching "new data" flag raised for the consumer to clear. | `app/services/head_unit.cpp:291-299`; test `head_unit_unchanged_payload_does_not_reraise_new_data` |
| **FR-86** | Inbound `0xFC` with payload `3F F3 F1 01 ?? ?? 3A A3 ??` shall be echoed back with CMD changed to `0xFD` and the checksum incremented by one, as a fixed 14-byte frame, and only while the transmitter is idle. | `app/services/head_unit.cpp:301-315`; test `head_unit_fc_handshake_echoes_fd` |
| **FR-87** | Inbound `0xE0` shall force a watchdog reset (dropping back into the IAP bootloader). Every other command — `0xFF` ACKs included — shall be silently ignored; the box cannot be polled. | `app/services/head_unit.cpp:317-321`; `docs/hiworld-firmware-analysis.md` §4.5, §4.6 |
| **FR-88** | The 10-entry ACK queue and `txAck(0xFF, code)` shall be reproduced, including the skip of reserved codes `0xFE`/`0xFF` and the tick roll-back, even though the producer index is never advanced and the path is therefore **dead on the wire** in this build. | `app/services/head_unit.cpp:527-538`, `:640-655`; test `head_unit_tx_ack_frame_shape` |

### 2.11 Parameter store

| ID | Requirement | Evidence |
|---|---|---|
| **FR-89** | Boot load shall zero the 32-byte RAM mirror, then read 32 bytes from page `0x0801F800` **one byte per 32-bit word** (byte *i* at base + 4*i*) and mirror them into the shadow so the first save is a no-op. | `app/services/param_store.cpp:62-77`; tests `param_store_load_takes_the_low_byte_of_each_word`, `param_store_load_mirrors_the_block_into_the_shadow` |
| **FR-90** | Validation shall apply defaults when byte 0 is not `0xAA`: magic `0xAA`, profile **1**, volume 0, five tone bytes 10, and a trailing 0 at `[0x16]` (the NUL of the stock `strncpy`). A profile byte > 1 shall then be clamped to 1. The latched `profileActive` shall be published from the result. | `app/services/param_store.cpp:144-173`; tests `param_store_validate_defaults_rewrites_a_virgin_page`, `param_store_validate_defaults_clamps_an_illegal_profile` |
| **FR-91** | Validation shall write **no flash**; defaults land in RAM only and are committed by the next dirty save. | `app/services/param_store.cpp:142-143`; test `param_store_validate_defaults_never_writes_flash` |
| **FR-92** | Save shall do nothing unless the live block differs from the shadow. | `app/services/param_store.cpp:112-116`; test `param_store_save_does_nothing_when_the_shadow_matches` |
| **FR-93** | A dirty save shall run: mask interrupts → kick → **HSI on for flash** → unlock → kick → erase page → kick → for each of 32 bytes { update shadow, program word, kick } → lock → unmask. The HSI step must precede the unlock. | `app/services/param_store.cpp:107-137`; tests `param_store_save_enables_hsi_before_unlocking`, `param_store_save_kicks_the_watchdog_around_every_step` |
| **FR-94** | Save shall **ignore every status return** — no retry, no error latch, no early exit — and shall update the shadow regardless, so a failed burn is not retried until the live block changes again. | `app/services/param_store.cpp:100-106`, `:128-133`; tests `param_store_save_ignores_a_flash_error_status`, `param_store_save_ignores_a_refused_status_and_an_unready_hsi` |
| **FR-95** | `setProfile()` shall store the byte verbatim with **no range check**, matching stock; the clamp lives only in validation. | `app/services/param_store.cpp:180-184`; test `param_store_set_profile_does_not_clamp_like_stock` |

### 2.12 Hardwired status outputs

| ID | Requirement | Evidence |
|---|---|---|
| **FR-96** | Status init shall drive PB5, PC15, PB6, PB4 **low before** configuring them as push-pull outputs, configure PB2 as an input, and set the GPIO-initialised flag to `0xFF` (not 1). | `app/services/board_io.cpp:76-93`; tests `board_io_status_lines_init_drives_low_before_configuring`, `board_io_status_lines_init_sets_gpio_initialised_flag` |
| **FR-97** | Every 8 ms the four flag bits shall be fanned out as: bit 0 → PB5 (ACC), bit 1 → PC15 (illumination), bit 2 → PB6 (reverse), bit 3 → PB4, in the stock call order PB6, PB5, PB4, PC15, with the high nibble ignored and no pin reconfigured. | `app/services/board_io.cpp:125-138`; tests `board_io_reverse_is_pb6_and_illum_is_pc15_not_the_other_way_round`, `board_io_status_fanout_keeps_the_stock_call_order`, `board_io_status_fanout_configures_nothing` |
| **FR-98** | Status shutdown shall drive PC15, PB6, PB4 low, **leave PB5 untouched**, return PB3 and PB2 to inputs, not reconfigure the status outputs and not clear the initialised flag. | `app/services/board_io.cpp:102-115`; tests `board_io_shutdown_does_not_touch_pb5`, `board_io_shutdown_leaves_gpio_initialised_alone` |
| **FR-99** | PB12 shall drive the CAN transceiver: **high = standby**, low = normal. | `app/services/board_io.cpp:153-156`; tests `board_io_can_transceiver_standby_on_drives_pb12_high`, `…_normal_mode_drives_pb12_low` |

### 2.13 Fault handling and the sleep path

| ID | Requirement | Evidence |
|---|---|---|
| **FR-100** | `fatal_error_reset(code)` shall keep that exact C name and signature, record the code in SRAM, re-arm the watchdog, force a watchdog reset and never return; the outer loop makes `[[noreturn]]` true even if the forced reset somehow returned. | `app/services/power.cpp:28-35`; `app/services/power.hpp:20-40` |
| **FR-101** | 54 default trap handlers shall exist as weak symbols branching to `fatal_error_reset` with the **stock codes 100..164**; the five slots that carry a real handler shall default to the **non-stock codes 217, 229, 232, 237, 247** so an unported or mis-named handler is identifiable; CAN_TX, CAN_RX1, CAN_SCE, USART1 and USART3 shall be bare `bx lr` stubs. | `app/startup/crt0.S:214-297` |
| **FR-102** | The five live handlers `TIM2_IRQHandler`, `CAN_RX0_IRQHandler`, `USART2_IRQHandler`, `EXTI2_IRQHandler`, `EXTI9_5_IRQHandler` shall be `extern "C"`, non-weak, and shall do nothing but acknowledge and delegate. | `app/logic/interrupts.cpp:49-97`; non-weakness checked against the linked ELF by `tools/check_stubs.py`; delegation covered by 12 tests in `tests/host/test_interrupts.cpp` |
| **FR-103** | `EXTI2_IRQHandler` shall clear line 2 and call `fatalErrorReset(0x7A)`; `EXTI9_5_IRQHandler` shall clear line 8 and call `fatalErrorReset(0x85)`. Both codes are stock. | `app/logic/interrupts.cpp:76-95`; `app/services/power.hpp:69-70` |
| **FR-104** | The deep-sleep path shall do **nothing at all** if the CAN controller does not acknowledge sleep; the caller keeps spinning and the still-clocked watchdog resolves it. | `app/services/power.cpp:45-51`; test `power_shutdown_does_nothing_at_all_when_can_refuses_to_sleep` |
| **FR-105** | On a successful sleep acknowledgement the order shall be: transceiver to standby → status lines down → three ADC writes (inert; the ADC is never clocked) → USART2 off → WWDG clock gated → **PB8 armed as the EXTI wake source** → `WFI` with stop/standby configuration. | `app/services/power.cpp:45-69`; tests `power_shutdown_follows_the_stock_teardown_order`, `power_shutdown_arms_the_pb8_wake_source_before_sleeping` |
| **FR-106** | PB8 wake arming shall issue, in stock order: priority grouping `0x500` → NVIC EXTI9_5 (IRQ 23) preempt 0 → PB8 as EXTI falling-edge input → AFIO line source port B → EXTI line 8 interrupt enable. **This is the only wake source armed before the WFI.** | `app/services/board_io.cpp:176-185`; test `board_io_pb8_wake_issues_the_five_steps_in_stock_order` |
| **FR-107** | After the WFI the code shall still attempt a forced watchdog reset even though the WWDG clock is gated and the write lands on a dead peripheral; the reset that actually happens comes from the EXTI9_5 handler. | `app/services/power.cpp:70-76`; test `power_shutdown_forces_a_reset_on_wake` |

---

## 3. Non-functional requirements

### 3.1 Memory map and entry

| ID | Requirement | Evidence |
|---|---|---|
| **NFR-1** | The image shall occupy only `0x08002000`–`0x0801F7FF` (118 KB). The bootloader region and the parameter page are not part of it. | `app/linker/app.ld:70-75`; `app/MEMORY_MAP.md` |
| **NFR-2** | The reset veneer label `image_entry` shall be at **exactly `0x08002202`**, shaped `push {r4,lr}; bl …; pop {r4,pc}`. The build shall fail otherwise. | `app/linker/app.ld:243-245`; `tools/verify_preamble.py` check 1 |
| **NFR-3** | Every veneer slot in `0x0800202A`–`0x08002201` that the bootloader's table dispatches to shall keep its original address and `push/bl/pop` shape, and its `bl` target shall lie inside the app region. The slot map is read off the bootloader dump, **not** from the linear `0x08002072 + 8·IRQn` rule. | `tools/verify_preamble.py` checks 2–4; `app/linker/app.ld:38-60`; `docs/notes/vector-table-slot-map.md` |
| **NFR-4** | The image shall contain no vector table, shall never write `SCB->VTOR`, shall never set `SP`, and shall not re-program the clock tree. | `app/linker/app.ld:21-27`; `app/startup/startup.c:36-44` |
| **NFR-5** | The linked image shall fit the 118 KB region; the build fails if it does not. Current size: **11 800 bytes (9.8 %)**. | `tools/build.py:167-170`; `arm-none-eabi-size build/app.elf` |
| **NFR-6** | Linker-allocated RAM shall start at or above `0x20000600` (the bootloader's stack lives below it, growing down from `MSP = 0x20000600`) and end at or below `0x20006000`. Current `.bss` is **2 604 bytes**, `.data` is **0**; no heap and no stack are reserved in the script. | `app/linker/app.ld:76-102`, `:246-250` |
| **NFR-7** | The application shall keep **no state at a fixed SRAM address**. The one externally visible datum is `g_versionString`, which the flashing tool resolves from `build/app.elf` with `nm`. | `app/include/ram_map.hpp:10-31`, `:514-526` |

### 3.2 Real-time behaviour

| ID | Requirement | Evidence |
|---|---|---|
| **NFR-8** | Every path through either main loop shall complete well inside the **~65.5 ms** watchdog window; both loops kick at the top of every pass. The in-loop work is bounded by FR-24 (20 decode passes), FR-70 (10 CAN frames) and FR-82 (one UART frame per pass). | `app/logic/main_loop.cpp:99-103`, `:148`; `app/include/app_config.hpp:52-58` |
| **NFR-9** | The 1 ms tick shall drive a divider chain of 10 ms (soft timers, cycle counter) and 100 ms (soft clock, CAN timers) hooks using three independent `if`s, so the cascade settles in one interrupt. | `app/services/scheduler.cpp:75-102`; test `sched_isr_divider_chain_yields_exactly_one_second_per_1000_interrupts` |
| **NFR-10** | The cadence flags shall fire on the exact stock phases: 4 ms at `(c & 3) == 1`, 8 ms at `(c & 7) == 3`, …, 1024 ms at `(c & 0x3FF) == 0x3FF`, 2048 ms at `(c & 0x7FF) == 0x7FF`. The 4/8/16/32 ms jobs shall never coincide; from 64 ms down they all fire on all-ones masks and **do** coincide — stock, not a slip. | `app/services/scheduler.cpp:107-126`; tests `sched_the_four_fast_cadences_never_coincide`, `sched_the_slow_cadences_do_coincide_at_2047ms` |
| **NFR-11** | The millisecond counter shall be 32-bit: an 8-bit counter would make the 1024 ms and 2048 ms tests unreachable and hang phase 1. | `app/include/ram_map.hpp:82-101`; test `sched_the_1024ms_and_2048ms_flags_are_reachable_at_all` |
| **NFR-12** | Interrupt priorities shall be the stock ones: USART2 preempt 2, CAN_RX0 preempt 3, TIM2 preempt 4 (lower = higher priority), EXTI9_5 preempt 0 for the wake path. Priority grouping is written `0x300` by the TIM2/CAN/USART2 initialisers and `0x500` by the wake arming; the last writer wins, as in stock. | `app/services/scheduler.hpp` NVIC block; `app/services/can_bus.cpp:59-63`; `app/services/head_unit.cpp:39-42`; `app/services/board_io.cpp:50-58` |
| **NFR-13** | Masking interrupts across a parameter save is accepted to cost time: a page erase plus 32 word programs runs tens of milliseconds, during which the millisecond counter and soft clock lose time and buffered CAN/UART traffic beyond the hardware FIFOs is dropped. This shall not be "fixed" by shortening the mask. | `app/services/param_store.hpp:121-133` |

### 3.3 Flash safety and recoverability

| ID | Requirement | Evidence |
|---|---|---|
| **NFR-14** | **Only the parameter page `0x0801F800`–`0x0801FFFF` may ever be erased or programmed at runtime.** The erase entry point accepts exactly that one page address, the program guard refuses any address outside the page with `FLASH_STS_REFUSED` and never touches the controller, and the caller's own addressing (`base + 4·i`, `i < 32`) cannot leave the page by construction. | `app/hal/hal_flash.c:102-113`, `:214-215`, `:245`; `app/services/param_store.cpp:45-52`; tests `param_store_save_only_ever_addresses_the_parameter_page`, `harness_refuses_flash_writes_outside_the_param_page` |
| **NFR-15** | A bad application image shall not be able to brick the box: the bootloader is write-protected, is never part of the payload, and re-advertises its `U` prompt on every boot, so the box can always be reflashed. | `app/REBUILD.md` §6 "Recovery"; `app/MEMORY_MAP.md` flash table |
| **NFR-16** | Every deliberate reset path shall terminate in a watchdog reset, which re-enters the IAP bootloader — a wedged app is still flashable. | `app/logic/can_link.cpp:144-155`; `app/services/power.cpp:28-35` |
| **NFR-17** | Phase-1 autobaud shall be accepted to cost up to **8 flash erase/program cycles per bus-less power-up** on the parameter page, because the profile byte is toggled and saved on every retry. | `docs/hiworld-startup-sequence.md` §9.3, §9.7; FR-11 |

### 3.4 Language, toolchain and determinism

| ID | Requirement | Evidence |
|---|---|---|
| **NFR-18** | Services and logic shall be C++17, freestanding, built `-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fno-unwind-tables`; the HAL shall be C11. No heap, no exceptions, no RTTI, no virtual functions, no recursion. | `docs/misra-cpp.md` §1, §2 |
| **NFR-19** | **No namespace-scope object with a runtime constructor.** This image never walks `.init_array`; the linker asserts `.preinit_array`, `.init_array` and `.fini_array` are empty and fails the build otherwise. | `app/linker/app.ld:256-261`; `docs/misra-cpp.md` §1 |
| **NFR-20** | The firmware shall contain **no `reinterpret_cast`**; fixed-width types everywhere; `constexpr` over `#define` in the C++ layers; `noexcept` on every function that can be. Recorded deviations D-1…D-6 are the only exceptions and are enumerated, not discovered. | `docs/misra-cpp.md` §3 |
| **NFR-21** | Behaviour shall be deterministic: no randomness, no uninitialised reads except the one documented reconstruction (the stock cluster-text function reads an uninitialised register when the digit test fails; the rebuild initialises that local to 0 — the only defined choice), all loop bounds compile-time constant, and loop counters typed to their range so a 0xFF-masked stock counter cannot spin for ever. | `app/logic/periodic_tx.cpp:256-266`; `docs/misra-cpp.md` §2 "Loop counters" |
| **NFR-22** | Build-time integration checks shall fail the build when an interrupt handler name drifts and silently leaves a weak trap in place, or when one stock function is reconstructed twice under two names. | `tools/check_stubs.py`; `tools/build.py:180-186` |
| **NFR-23** | The number of CAN profiles shall remain exactly 2. Three `static_assert`s reference `cfg::kProfileCount` — the bit-rate selectors are write-back normalisers, the profile is published to the head unit in one bit, and the autobaud is a flip-flop — so raising it is a protocol change that stops the build rather than corrupting the parameter page at runtime. | `app/include/app_config.hpp:79-105`; `app/services/can_bus.cpp:259-265`; `app/logic/periodic_tx.cpp:615-618` |
| **NFR-24** | Diagnosability: SRAM is the **only** post-mortem available (flash is RDP-locked, there is no printf, SRAM is readable over SWD while the app runs). Every deliberate reset shall name itself in `fatalErrorCode`, in three ranges — **100..164** stock per-vector codes, **217..247** "a handler name did not match", **192..194** this rebuild's deliberate additions. `canErrFlags` and `uartRxOverflow` are write-only diagnostics with no reader in the image. | `app/services/power.hpp:45-73`; `app/services/can_bus.cpp:301-310`; `app/include/ram_map.hpp:150-153` |

---

## 4. Constraints and assumptions

These are taken as given. If any turns out to be false, requirements above fail with it.

| # | Assumption | Source |
|---|---|---|
| A-1 | The bootloader sets `MSP = 0x20000600`, programs HSE 16 MHz → PLL → SYSCLK 64 MHz, leaves `VTOR = 0x08000000`, and enters the app with `bl 0x08002202` gated on `*0x08001800 == 0x19780217`. | `docs/hiworld-startup-sequence.md` §1, §2; `docs/hiworld-bootloader-analysis.md` |
| A-2 | The app itself sets only the bus prescalers, `PPRE1 = PPRE2 = /4`, giving `PCLK1 = PCLK2 = 16 MHz` and a TIM2 kernel clock of 32 MHz (×2 multiplier because PPRE1 ≠ /1). Confirmed three ways: CAN bit-timing arithmetic, the 1 ms TIM2 reload, and the single initialised word in the stock scatter table (`SystemCoreClock = 64000000`). | `app/include/app_config.hpp:20-36`; `app/startup/startup.c:17-32` |
| A-3 | The bootloader writes no NVIC enables; `CAN_RX0_IRQn = 54` on this part and the bootloader's table maps vector 54 to veneer slot 29. This is settled — do not re-litigate. | `AGENTS.md` "Settled questions"; `docs/notes/vector-table-slot-map.md` |
| A-4 | The float ABI is soft; the stock image contains no VFP instruction and no CPACR write. | `AGENTS.md` "Settled questions"; `app/REBUILD.md` §3 |
| A-5 | The `.iap` container is 136-byte records with a **16-byte target magic** in record 0 (universal `01 09 07 08 00 02 01 07 91 32 47 a4` plus per-box `4e 33 14 f2`), a 4-byte obfuscation key in the clear (`68 cc e3 3e` for box 2429), per-record 33-word checksums and a position-additive byte transform. There is no signature, so a correctly packed image is accepted. | `docs/hiworld-iap-format.md` §2–§4 |
| A-6 | The device is at **RDP Level 1**: a debugger attaches and SRAM is readable, but every debugger read of main flash faults, and the bootloader cannot erase or program while the core is in a latched debug state — hence `--reset powercycle` before flashing. | `docs/writeup-reading-locked-n32l406.md`; `app/REBUILD.md` §6 |
| A-7 | The parameter page's one-byte-per-32-bit-word encoding is fixed by every box already in the field; it is not a design choice that can be revisited. | `app/services/param_store.hpp:31-41` |
| A-8 | Board wiring: PB8 = CAN_RX (dual-role as the EXTI8 wake input), PB9 = CAN_TX, PB12 = transceiver standby, PA2/PA3 = USART2, PB5/PC15/PB6/PB4 = status outputs, PB2/PB3 = static inputs. | `app/services/can_bus.cpp:45-57`; `app/services/board_io.cpp:27-40` |
| A-9 | The head unit free-runs as a listener: it never polls, and `AA 55 01 FF <X>` on the wire is the head unit acknowledging message `<X>`, not a request. | `docs/hiworld-firmware-analysis.md` §4.6 |
| A-10 | With no CAN bus attached the box is **silent** and goes to sleep after ≈18.4 s. That is correct behaviour on a bench, not a failure — and it bounds the window for reading the version string over SWD. | `docs/hiworld-startup-sequence.md` §9; `app/REBUILD.md` §6 |

---

## 5. Verification status

**The firmware has never run on hardware.** Everything below is static evidence.

| Method | What it means here |
|---|---|
| **HOST** | Covered by a named test in `tests/host/`, run by `python tools/test.py` (298 tests, 0 failing). The tests are written from the Ghidra decompile in `reference/decompile/app/`, not from the rebuilt code. |
| **BUILD** | Enforced at compile or link time — `static_assert`, a linker `ASSERT`, or a `tools/*.py` check that fails the build. |
| **STATIC** | Verified by reading the rebuilt source against the decompile/doc; no automated check. |
| **BENCH** | Can only be closed by running on hardware. Not done. |

| Requirements | Verified by | Notes |
|---|---|---|
| FR-1 … FR-8 (boot) | STATIC + BUILD | Order and register values read against `0x080088D8`/`0x080032D2`; `sched_start_programs_the_1ms_timebase` and `sched_start_enables_the_nvic_before_the_counter` cover FR-6 at HOST level. The watchdog arming itself (FR-7) is BENCH. |
| FR-9 … FR-15 (phase 1) | STATIC + **BENCH** | No host test drives `main_loop`. The 2048 ms/1024 ms flags are reachable (HOST, `sched_the_1024ms_and_2048ms_flags_are_reachable_at_all`) and the profile save round-trip is HOST (`param_store_save_persists_an_autobaud_profile_toggle`), but the loop itself, the 3-frame exit and the ≈18.4 s give-up are unverified. `main_loop` is instead covered by a statement-by-statement audit against `reference/decompile/app/sys.c:455`, which found no divergence. |
| FR-16 … FR-18 (bit rate) | HOST | `can_set_baud_by_profile_picks_125k_for_profile_one`, `…_83k_for_profile_zero`, `…_normalises_a_corrupt_profile_byte`, `can_init_configures_pins_filter_and_interrupt`, `can_reinit_bitrate_wakes_the_transceiver_and_resets_bus_state`. Real bus timing is BENCH. |
| FR-19 … FR-27 (RX path, gate) | HOST | 28 tests in `test_can_bus.cpp` plus the gate tests in `test_can_decode.cpp`. |
| FR-28 … FR-53 (decoders, vendor frames) | HOST | 48 tests in `test_can_decode.cpp`, including the three reproduced oddities FR-41, FR-50 and FR-35. Whether the decoded *meaning* is right (e.g. illumination vs fuel, §6) is BENCH/vehicle. |
| FR-54 … FR-58 (vehicle state) | HOST + BUILD | Window geometry is `static_assert`ed (`app/include/ram_map.hpp:357-371`) and exercised by 16 tests in `test_ram_map.cpp`; FR-56 (the change counter in `main_loop`) is STATIC only. |
| FR-59 … FR-70 (periodic TX) | HOST | 47 tests in `test_periodic_tx.cpp` and `test_can_bus.cpp`; full 151-phase sweeps are walked for both profiles. |
| FR-71 … FR-75 (link machine) | HOST | 19 tests in `test_can_link.cpp`, every state transition included. The two forced-reset exits are observed through the fake HAL, not on silicon. |
| FR-76 … FR-88 (head-unit link) | HOST + BUILD | 30 tests in `test_head_unit.cpp`; the descriptor table's lengths, shadows and buffer windows are `static_assert`ed (`app/services/head_unit.cpp:161-197`). Actual 38400 8N1 framing on a wire is BENCH. |
| FR-89 … FR-95 (parameters) | HOST | 22 tests in `test_param_store.cpp`, including the real dumped page (`param_store_load_reads_the_real_dumped_page`). Real flash timing/erase success is BENCH. |
| FR-96 … FR-99 (status outputs) | HOST | 28 tests in `test_board_io.cpp`, including pin-crossing regressions. Electrical behaviour is BENCH. |
| FR-100 … FR-107 (faults, sleep) | HOST + STATIC | 9 tests in `test_power.cpp` cover the teardown order, the no-op-on-refusal path and the wake reset; `power_fault_codes_do_not_collide_with_the_crt0_trap_codes` covers the code ranges. The trap table itself (FR-101) is STATIC. Actual deep sleep and the PB8 wake are **BENCH**. |
| NFR-1 … NFR-5 | BUILD | `app/linker/app.ld` ASSERTs plus `tools/verify_preamble.py` against the bootloader dump, run on every build. |
| NFR-6, NFR-7 | BUILD | Linker region overflow and the `_sdata`/`_ebss` ASSERTs; current figures from `arm-none-eabi-size`. |
| NFR-8 | **BENCH** | **Not verified.** Nothing measures worst-case loop latency. The bounds in FR-24/FR-70/FR-82 make it plausible; a scope and a WWDG that does not bite are the only proof. |
| NFR-9 … NFR-11 | HOST | 14 tests in `test_scheduler.cpp` plus `test_soft_clock.cpp`. |
| NFR-12 | STATIC | Priority values read against the stock literals; never observed under load. |
| NFR-13 | STATIC | The cost is reasoned about and documented, not measured. |
| NFR-14 | HOST + BUILD | The guard is unit-tested from both sides (`param_store_save_only_ever_addresses_the_parameter_page`, `harness_refuses_flash_writes_outside_the_param_page`). |
| NFR-15, NFR-16 | STATIC | Follows from the bootloader analysis and the write-protect bits; the uploader path itself is proven end-to-end (`docs/notes/iap-upload-pipeline-works.md`) but not with a rebuilt image. |
| NFR-17 | STATIC | Arithmetic from FR-11, not measured. |
| NFR-18 … NFR-23 | BUILD | Compiler flags, linker ASSERTs, `tools/check_stubs.py`, and the `static_assert`s named in each row. **MISRA conformance itself is not tool-checked** — see `docs/misra-cpp.md` §"Honesty about compliance". |
| NFR-24 | HOST (codes) + BENCH (readback) | The code ranges are tested; reading `fatalErrorCode` out of a real box over SWD has never been done. |

### What a bench session must close

1. The box boots at all from the rebuilt image (version string readable over SWD).
2. Phase 1 exits on three real CAN frames; the ≈18.4 s give-up and the PB8 wake both work.
3. The watchdog never bites in normal operation (NFR-8).
4. The head unit accepts the `AA 55` stream and the box accepts `0xD2`/`0xAD`/`0xFC`/`0xE0`.
5. The parameter page survives a power cycle with real flash timing.
6. The ambiguous signals in §6 resolve on a real car.

---

## 6. Open questions and known divergences

### 6.1 Deliberate divergence from stock — fault codes at three reset sites

Three sites call `fatalErrorReset()` where the stock image open-codes `wwdg_init();
wwdg_force_reset();` and records **nothing**:

| Code | Site | Stock behaviour |
|---|---|---|
| `0xC0` (192) `kFatalBusDown` | `kBusDown` with the bus dead, reached only when CAN refused to sleep | forced reset, no record |
| `0xC1` (193) `kFatalLinkWedged` | `kBusRunning` with the activity counter expired | forced reset, no record |
| `0xC2` (194) `kFatalHostCommand` | head-unit command `0xE0` | forced reset, no record |

The divergence is one store to a byte **no code in the image ever reads**. It cannot change
behaviour on CAN, on the UART or on any pin; it only makes the reset identifiable afterwards,
which matters because SRAM is the only post-mortem this box has. Documented in
`app/services/power.hpp:52-73` and `docs/misra-cpp.md`.
Covered by `power_fault_codes_do_not_collide_with_the_crt0_trap_codes`.

### 6.2 Reproduced stock defects (not to be fixed)

| What | Consequence | Where |
|---|---|---|
| Profile 1's `0x7FF` display-mode reply tests mode 4 against bit 2 again | **Mode 3 is unreportable** (`0x00`) and mode 4 occupies its bit | FR-50 |
| Profile 1 gear nibble 9 | the "leave the gear alone" branch is unreachable; 8..F all report R | FR-41 |
| `armForKeycode()` has no bounds check | a key code above 0x3F would write past the 64-slot pool; the decoder never produces one | `docs/misra-cpp.md` N-2 |
| `txDrain()` assigns rather than ORs the error flag | a mailbox stall wipes a pending RX-ring-full or TX-ring-full bit | FR-70 |
| Message `0x73` | eight zero bytes for ever | FR-84 |
| The ACK queue | never produced into; `txAck` is dead on the wire | FR-88 |
| `0x12` payload `[4..9]` | amplifier state aliased into a door message | FR-57 |

### 6.3 Behavioural unknowns that only a car can settle

1. **`0x72[0]` bit 1 and `0x72[3]`** (from CAN `0x210`/`0x308` byte 1). Modelled as
   "illumination on + dimmer percent" because bit 1 drives a GPIO, but fuel level is not
   excluded. One stimulus test settles it. (`docs/hiworld-firmware-analysis.md` §9.2)
2. **`0x72[0]` bit 4** (`kFlagIgnRaw`) is unidentified; it is set in every capture and routed
   to no pin.
3. **The semantics of the transmitted `0x0F0`/`0x2E9`, `0x326`/`0x290` and `0x09F` frames.**
   The templates are reproduced byte for byte; what the car does with them is unknown.
4. **Which physical bus each profile belongs to** is inferred from the box's printed vehicle
   list, not from the firmware.
5. **Whether `PWR_CR.PDDS` + `SLEEPDEEP` is Stop or Standby on the N32L406**, and therefore
   the sleeping current draw. (`docs/hiworld-startup-sequence.md` §10)

### 6.4 Resolved since the analysis documents were written

* The `0x40020800` register block poked in the sleep path — listed as **UNKNOWN** in
  `docs/hiworld-startup-sequence.md` §10 — is the **ADC**. The rebuild issues
  `adc_dma_cmd` / `adc_enable` / `adc_sw_start_regular`, and notes the writes are inert
  because the ADC is never clocked by this firmware (`app/services/power.cpp:56-60`).
* The cluster-text sub-command table, described as "details beyond the sub-command switch not
  fully reversed" in `docs/hiworld-firmware-analysis.md` §5.2, is now fully reconstructed and
  pinned by tests (FR-67).

### 6.5 Documentation contradictions found while writing this

These are inconsistencies **between documents and code**, not firmware defects. None of them
affects the built image; all of them will mislead the next reader.

| Where | Problem |
|---|---|
| `app/services/power.hpp:67` | Cites "`docs/misra-cpp.md` section 6" for the fault-code divergence. That file has sections 1, 2, 3, 4, 4a and 5 — **there is no section 6**, and the divergence is described in §4 N-1…N-5 and §3 only indirectly. |
| `app/services/board_io.cpp:164-167` | Says "the summary in `docs/ARCHITECTURE.md` §3 lists the NVIC step last; the image puts it first". `docs/ARCHITECTURE.md` §3 now lists the NVIC steps **first** (items 1–2). The code comment is stale. |
| `AGENTS.md` "Recommended workflow" 4 | Tells the reader to read back the version string from SRAM `0x20001B86` via `ram::app().versionStr`. There is no `versionStr` member any more: it is the standalone `extern "C" g_versionString`, and the uploader resolves it with `nm` rather than from a fixed address (`app/include/ram_map.hpp:514-526`). |
| `app/MEMORY_MAP.md` "Application preamble" | Still states `veneer(IRQn) = 0x08002072 + 8*IRQn` with no superseded marker. `app/linker/app.ld:46-54`, `AGENTS.md` and `docs/hiworld-startup-sequence.md` §2 all record that this holds only for IRQ0..10 and IRQ23..41. |
| `app/REBUILD.md` §7 | The open-questions table still lists the float ABI as unresolved ("objdump not available in this environment yet") and advises "keep using the `globals.h` absolute pointers for known state". `AGENTS.md` records the float ABI as settled by disassembly, and `app/include/globals.h` has been deleted. |
| `docs/hiworld-firmware-analysis.md` §3.7 | States that profile 1's `0x7FF` "also sets the **gear** from `d[4]`" with the mapping 0→0, 1→2, 2→1, 3→9, 4→8, 5→11, 6→3. Those are exactly the SWC **key codes**, and the rebuild writes them into the SWC key slot, flagging explicitly that it is "the key slot, not the gear slot — that is what the stock code does" (`app/logic/can_decode.cpp:241-263`, test `fca_vendor_7ff_replies_on_7fe_and_maps_d4_into_the_key_slot`). One of the two readings is wrong; the code and its test are the more recent and more specific claim, but this has never been checked on hardware. |

### 6.6 Requirements with no verification path short of a bench

NFR-8 (the watchdog margin), FR-9…FR-15 (the whole of phase 1 as a loop), FR-104…FR-107
(deep sleep and the PB8 wake), and every electrical requirement (FR-96…FR-99, FR-76). Nothing
in this repository can close them.
