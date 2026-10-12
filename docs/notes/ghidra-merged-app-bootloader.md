---
name: ghidra-merged-app-bootloader
description: "The Ghidra project now holds bootloader + app in one program at 0x08000000, with app functions renamed; how to reproduce the naming"
metadata: 
  node_type: memory
  type: project
  originSessionId: 797fe656-b848-4f0c-b9fa-f0005c8d7f0d
  modified: 2026-10-11T20:47:38.285Z
---

The Ghidra program `bootloader_0x08000000_8k.bin` now contains BOTH the 8 KB bootloader
(`0x08000000`) and the 32 KB app image (`0x08002000`, imported via File > Add To Program) in
one address space — 272 functions, cross-refs resolve across the boundary (e.g. bootloader
`bl 0x08002202` into the app).

- **126 functions named** (21 bootloader + 105 app), plus plate comments on the key ones. The
  remaining ~137 app `FUN_` are leaf/library helpers and a **dead software-SPI/bit-bang
  cluster** (zero callers or called only from `*_DEAD`) — deliberately left unnamed.
- App names were restored from the prior full decompilation, not re-derived: canonical map in
  `hiworld/decomp/rename_hiworld.py` (56 names), extended by analysis of `hiworld/decomp/full/`
  (per-function `.c`). The per-signal CAN decoders are **inline** inside
  `can_dispatch_profile0_83k` (0x08009010) / `can_dispatch_profile1_125k` (0x08009680) — there
  are no separate `can_decode_*` functions. UART TX is table-driven (single
  `uart_build_frame_AA55`), so there are no per-CMD builder functions either.
- Corrections made this pass: `0x08002494` is `gpio_init_pin` (GPIO_Init), not DMA; the group
  at `0x08007cd2/ce0/cee/d12` is a generic 64-slot **software-timer array** (`softtimer_*`,
  base ptr @0x08007f50), NOT steering-wheel keys; `can_periodic_tx_scheduler` (0x08008110)
  emits CAN frames (0x3d0/0xf0/0x326), the head-unit UART round-robin is `uart_tx_sched`.

**Why:** re-importing/renaming from scratch is wasted work; the live DB already has it.

**How to apply:** to re-apply names after a fresh import, run `hiworld/decomp/rename_hiworld.py`
in Ghidra (handles any image base). Related: [[box-firmware-decompiled]], [[bootloader-dumped]].

## Global/variable naming (DAT_ cleanup, in progress)

Created an **SRAM block @0x20000000** so RAM globals get labels. Scheme: `g_<name>` on the RAM
variable, `p_<name>` on each flash literal-pool pointer slot (so decompiles read `*p_...`).
Done so far: `main_loop`, the scheduler (`tim2_isr_scheduler_tick`, `tim2_sched_rate_flags` —
tick counter `g_sched_tick_counter` @0x20001108, flag array `g_tick_flag_*` @0x2000110c..15,
10ms/100ms/1s dividers @0x20001000..02), `can_profile_dispatch_gate` (partial), and
**`can_dispatch_profile0_83k` (the 83k WK decoder) — fully DAT_-free, locals renamed, plate-
documented**. Key RAM: RX ring head/tail `g_can_rx_ring_head/tail` (0x20001118/111c), buffer
`g_can_rx_ring_buf` (0x20001284, 50x16B), decoded output `g_status_block` (0x20001b49: byte0
bits = illum/acc/reverse, [2]=turn, [3]=fuel, [4:5]=steering, [0x15]=lamps), SWC keys
`g_swc_*`. Matches `hiworld-all-car-profiles.md`. NOT yet done: profile1_125k decoder, UART
status builders, the rest of the state struct.

## Full-decode pass (6 agents, all app functions)

Decoded all 243 app functions via 6 cluster agents (decomp on disk + slot2ram/known_globals maps),
merged centrally, applied through Ghidra. Result: ~132 functions named + 4 reclassified, ~70 RAM
globals named with p_<name> propagated to every pointer slot (111 slots), 74 locals renamed. The
big decoders (83k + 125k), main_loop, scheduler, gate, UART AA55 path, and the CAN/DMA/EXTI/FLASH/
RCC/TIM/USART/WWDG/NVIC HAL are all DAT_-clean.

Corrections found this pass (authoritative over earlier guesses):
- **There IS an ADC** on the N32L406 at base 0x40020800 (STM32F1 scan missed it — it looks for
  0x40012400). Functions 080030b0 adc_subsystem_init_DEAD + 0800349c.. adc_* + 080035b2
  adc_regular_channel_config. BUT it is effectively unused: init is DEAD (0 callers); the only live
  touch is exti8_bus_wake_setup DISABLING it before reset. So hiworld-gpio-io.md "no ADC" is wrong
  about presence, right about it being unused. (080035b2 was mislabeled gpio_af_remap_config;
  080030b0 was mislabeled spi_master_init_DEAD.)
- 0x20001120/0x20001124 are the CAN TX ring head/tail (not swc vars); TX ring buf @0x200015a4.
- param struct is 0x200011d0/d1 (earlier note 0x200111d0 was a typo).
- can_bittiming_struct_init -> can_struct_init_defaults (full CAN_StructInit, not just timing).
- There is a dead bit-banged I2C set (0x2558..0x27a6) and dead libc/crt0 shims.
- Naming scan caveat: 4-byte words equal to 0x20004601/02/0x20004770 are the common Thumb byte
  pattern `xx 46 00 20` (mov;movs), NOT globals — ignore them.

## Finalization

- hiworld-gpio-io.md updated: ADC is present (0x40020800) but present-but-unused (dead init,
  only live ref disables it); spi_master_init/gpio_af_remap mislabels corrected there too.
- Labeled 64 MMIO register bases (REG_FLASH/RCC/GPIOA-D/USART1-3/CAN/TIM1/ADC/DMA1_CH1/NVIC/SCB).
- Remaining DAT_ after MMIO+globals pass are flash constant lookup tables (TIM base table,
  clock-freq table, UART dispatch table) and dead-code refs — not decode gaps.
- Named last app globals incl. bootloader IAP state (g_boot_iap_state/substate/offset,
  g_boot_rx_timeout, g_boot_sysclk_hz), g_amp_level (0x20001b75, amp byte broadcast by gate),
  g_d2_cluster_state, g_periodic_tx_cfg, g_uart_rx_frame (0x20001b01).
- The 0x20001b5c.. group are g_status_block sub-fields with no direct xrefs (accessed via
  base+offset in the decoders) — they do not appear as DAT_ anywhere, so left unlabeled.
