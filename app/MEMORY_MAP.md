# Memory map — Hiworld canbox APP (Nations N32L406CBL7)

Flash 128 KB @ `0x08000000` (2 KB pages), SRAM 24 KB @ `0x20000000`. The app occupies the
middle flash region only; it runs on the bootloader's vector table (`VTOR = 0x08000000`,
`MSP = 0x20000600`). Sources: `docs/hiworld-hardware.md`, `docs/hiworld-startup-sequence.md`
§4, and the Ghidra maps in `ghidra/`. SRAM addresses are the ORIGINAL firmware's;
the rebuild no longer uses them (see `app/include/ram_map.hpp`).

---

## Flash

| Region | Span | Size | Rebuilt? | Notes |
|---|---|---|---|---|
| IAP bootloader | `0x08000000`–`0x08001FFF` | 8 KB | **No** | Owns the real vector table; enters the app via `bl 0x08002202`. Write-protected (pages 0–1). Not shipped in any `.iap`. |
| Valid-app marker page | within `0x08001800`–`0x08001FFF` | — | No | Bootloader writes `0x19780217`→`0x08001800`, `0xA4473291`→`0x08001F80`, `0x01043749`→`0x08001F84` after a good flash; the boot gate reads `*0x08001800`. |
| **Application** | `0x08002000`–`0x0801F7FF` | **118 KB** (`0x1D800`) | **Yes** | The `.iap` payload. Linker `FLASH` region. Stock image is only ~32 KB; rest is headroom. |
| Param / emulated EEPROM | `0x0801F800`–`0x0801FFFF` | 2 KB | No | `param_page_load/save`. Runtime config (magic `0xAA`, profile byte). Image must not write it. |

### Application preamble (fixed addresses inside the app region)

| Address | What |
|---|---|
| `0x08002000`–`0x08002009` | `startup_crt0` / `__rt_stackheap_init` fragments |
| `0x0800202a`–`0x08002201` | 59 eight-byte IRQ/exception veneers, dispatched to by the bootloader's immutable table, so the addresses are fixed. **The linear rule `veneer(IRQn) = 0x08002072 + 8*IRQn` is WRONG and superseded** -- it holds only for IRQ0..10 and IRQ23..41. The array is in STM32F103 vector order while the part is an N32L406, and the bootloader table bridges the two. The real map is read off `reference/bootloader_0x08000000_8k.bin`, encoded in `app/startup/crt0.S`, and enforced on every build by `tools/verify_preamble.py`. |
| **`0x08002202`** | **App entry veneer** (`push {r4,lr}; bl __main; pop`) — the `bl 0x08002202` target. |
| `0x0800220a` | `call_main` veneer → `main_loop` |
| `0x08002400` | `__main` (ARM C-lib: scatterload + `__rt_entry`) |
| `0x08002434` | `Region$$Table` (scatterload descriptors: RW copy + ZI zero) |
| `0x08008e18` | flash copy of the version string `H1N4JPF20A-220524` |

---

## SRAM (`0x20000000`–`0x20005FFF`, 24 KB)

| Range | Use |
|---|---|
| `0x20000000`–`0x20000600` | **System stack** — MSP top `0x20000600`, grows down (~1.5 KB). Set by the bootloader; the app does not re-init SP. |
| `0x20000600`–`0x20000FFF` | Largely unused by the stock app (candidate space for linker-managed `.data`/`.bss` in a rebuild — verify first). |
| `0x20001000`–`~0x20001C24` | **Stock firmware's fixed global block** (below), addressed by hard pointers in the original. The REBUILD no longer reproduces this: its state is linker-allocated `.bss` from `0x20000600` up (see `app/include/ram_map.hpp`). The addresses here remain the reference for reading `reference/decompile/app/` and Ghidra. |

### Globals by subsystem (the ORIGINAL firmware's fixed addresses)

**Timebase / scheduler**

| Address | Symbol | Meaning |
|---|---|---|
| `0x20001000`/`01`/`02` | `g_tick_10ms_div` / `_100ms_div` / `_1s_div` | TIM2 ISR ÷10 / ÷100 / ÷1000 counters (1 ms tick) |
| `0x20001003` | `g_fatal_error_code` | last fatal-error code |
| `0x20001104` | `g_blink_halfsec` | 500 ms heartbeat bit-field |
| `0x20001108` | `g_sched_tick_counter` | free-running millisecond counter |
| `0x2000110c`–`0x20001114` | `g_tick_flag_fast`…`g_tick_flag_slow` | one-shot tick flags (4 / 8 / 16 / 32 / 64 / 128 / 256 / 512 / 1024 ms) |
| `0x20001115` | `g_autobaud_tick_flag` | 2048 ms flag (phase-1 profile retry) |
| `0x200010fc`–`0x200010ff` | `g_soft_clock[]` | soft RTC: hours / minutes / seconds / tenths |

**Boot / IAP shadow (mirrors of bootloader state)**

| Address | Symbol | Meaning |
|---|---|---|
| `0x200010c4`/`c5` | `g_boot_iap_state` / `_substate` | IAP state shadow |
| `0x200010d8` | `g_boot_iap_offset` | IAP offset shadow |
| `0x200010dc` | `g_boot_sysclk_hz` | sysclk shadow |
| `0x200010e0` | `g_d2_cluster_state` | cluster/diagnostic state |

**CAN — rings, status, timers**

| Address | Symbol | Meaning |
|---|---|---|
| `0x20001118` / `0x2000111c` | `g_can_rx_ring_head` / `_tail` | CAN RX ring indices (mod 50); head advanced by RX ISR |
| `0x20001120` / `0x20001124` | `g_swc_var_120` / `_124` | CAN TX ring write / read indices (mod 50) |
| `0x20001130` | `g_can_link_flag` | CAN link flag |
| `0x20001134` | `g_can_retry_ctr` | CAN retry counter |
| `0x20001138` | `g_can_err_flags` | bit0 RX overrun, bit1 no TX mailbox, bit2 TX ring full |
| `0x20001139` | `g_can_bus_status` | bus state machine state |
| `0x2000113a` | `g_profile0_activity_ctr` | 100 ms bus-state countdown / activity |
| `0x20001140` | `g_can_echo_buf_516` | CAN `0x516` echo buffer |
| `0x20001148` | `g_swc_tx_buf` | 8-byte CAN `0x416` payload |
| `0x20001198` | `g_can_reinit_bitrate` | selected baud selector (`0x7D` 125 k / `0x53` 83.3 k) |
| `0x2000119c` / `0x200011a0` | `g_can_tx_hold_timer` / `g_canerr_hold_timer` | CAN TX / error hold timers |
| `0x20001284` | `g_can_rx_ring_buf` | CAN RX ring — 50 × 16 bytes |
| `0x200015a4` | `g_can_tx_ring_buf` | CAN TX ring — 50 × 16 bytes |
| `0x200018e4` | `g_can_tx_busy_block` | CAN TX in-flight mailbox scratch |
| `0x200018f8` | `g_softtimer_slots` | 64 × u32 per-message timeout array |

**SWC / periodic CAN TX & bus bring-up**

| Address | Symbol | Meaning |
|---|---|---|
| `0x20001150` / `57` / `5c` / `64` / `6b` | `g_tx_frame_amp` / `_f0` / `_326` / `_190` / `_18c` | pre-built periodic CAN TX frames |
| `0x200011a1` | `g_tx_frame_9f` | periodic CAN `0x09F` frame |
| `0x2000117f`–`0x20001182` | `g_swc_keycode` / `_keydata` / `_keycode_prev` / `_keydata_ref` | steering-wheel key state |
| `0x20001188` | `g_periodic_tx_phase` | rotating TX-table phase |
| `0x2000118a`–`0x2000118d` | `g_swc_resp_mode` / `_flag_118b` / `_timer_118c` / `g_misc_timer_118d` | SWC response mode + soft timers |
| `0x2000118e` | `g_prndl_tx_7fe` | PRNDL / gear TX frame |
| `0x2000118f` | `g_profile_active` | active CAN profile (selects decoder) |
| `0x20001190` | `g_autobaud_try_count` | phase-1 profile retry count (cap 8) |
| `0x20001194` | `g_bus_present_debounce` | ACC settle countdown (800 × 8 ms = 6.4 s) |
| `0x200011ce` | `g_rpm_decimator_ctr` | RPM decimation counter |

**UART (USART2, 38400 8N1, PA2/PA3)**

| Address | Symbol | Meaning |
|---|---|---|
| `0x200011aa` | `g_uart_aux_rx_overflow_flag` | aux RX overflow flag |
| `0x200011ab` | `g_uart_slot_state` | per-slot TX state (0 idle, 1 pending, 3 sent) |
| `0x200011b1` / `b2` | `g_uart_tx_slot_idx` / `g_uart_aux_slot_idx` | TX slot / rotation cursors |
| `0x200011b3` / `b4` | `g_uart_idle_delay` / `g_uart_tx_tick` | idle keep-alive gap / 8 ms TX scheduler tick |
| `0x200011b5` | `g_gpio_initialized` | GPIO-init-done flag (`0xFF`) |
| `0x200011b7` / `b8` | `g_cycle_ctr` / `g_cycle_ctr_max` | rolling cycle counter + bound |
| `0x200019f8` | `g_uart_frame_buf` | USART2 frame/ring struct (build buf +0, len +0x3c, cursor +0x3d, busy +0x3e, RX ring +0x3f…) |
| `0x20001b01` | `g_uart_rx_frame` | parsed inbound UART frame |
| `0x20001b3d` | `g_uart_ack_queue` | outbound ack queue |

**Parameter block**

| Address | Symbol | Meaning |
|---|---|---|
| `0x200011d0` | `g_param_struct` | 32-byte param block (`[0]` magic `0xAA`, `[1]` CAN profile 0/1) |
| `0x200011d1` | `g_param_profile_sel` | profile byte (alias of param `[1]`) |
| `0x20001220` | `g_param_shadow` | dirty-compare shadow of the param block |

**Vehicle-state / UART TX payloads**

| Address | Symbol | Meaning |
|---|---|---|
| `0x20001b49` | `g_status_block` | decoded vehicle-state block = UART TX slot 0 data |
| `0x20001b60` | `g_vstate_mid` | mid vehicle-state block |
| `0x20001b75` | `g_amp_level` | amplifier / profile marker byte |
| `0x20001b86` | `g_version_str` | version string `H1N4JPF20A-220524` (TX slot 3; read back by `--verify`) |
| `0x20001be7` | `g_cmd_block` | inbound command block |
| `0x20001bf5` | `g_periodic_tx_cfg` | periodic-TX config byte |

**C runtime**

| Address | Symbol | Meaning |
|---|---|---|
| `0x20001c1c` | `g_libc_exit_var` | libc exit scratch |
| `0x20001c20` | `g_libc_impure_ptr` | libc reentrancy pointer |

---

## Peripheral MMIO (see `app/include/n32l406_hw.h` for the typed layouts)

N32L406 is STM32F1-like but **not** identical — notably **ADC @ `0x40020800`** (not
`0x40012400`). Bases used by this firmware: TIM2 `0x40000000`, WWDG `0x40002C00`,
USART2 `0x40004400`, USART3 `0x40004800`, CAN `0x40006400` (filter `0x40006600`),
PWR `0x40007000`, AFIO `0x40010000`, EXTI `0x40010400`, GPIOA–D `0x40010800`/`0C00`/`1000`/`1400`,
TIM1 `0x40012C00`, USART1 `0x40013800`, DMA1 `0x40020000`, ADC `0x40020800`, RCC `0x40021000`,
FLASH `0x40022000`, NVIC/SCB at the Cortex-M PPB (`0xE000E100`/`E180`/`E400`, `0xE000ED00`).
