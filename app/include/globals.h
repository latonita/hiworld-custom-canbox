// Reconstructed RAM global map - Hiworld canbox APP (Nations N32L406, SRAM @0x20000000).
// Addresses are the ORIGINAL firmware RAM addresses (from Ghidra labels + slot map).
// Scalars are byte-wide unless the real width is known; buffers/structs are pointer macros
// so existing [index] access compiles. Refine widths during the rebuild.

#ifndef GLOBALS_H
#define GLOBALS_H
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * WIDTHS ARE LOAD-BEARING. The entries below were widened to uint32_t after
 * scanning the reference image for how each address is actually accessed
 * (ldr/str = word, ldrb/strb = byte). Declaring a word-wide global as uint8_t
 * silently truncates:
 *   g_sched_tick_counter @0x20001108 -- the 1 ms hook tests (c & 0x3FF) and
 *       (c & 0x7FF); with 8 bits neither can ever match, so the 1024 ms param
 *       save and the 2048 ms autobaud retry never fire and phase 1 hangs.
 *   g_bus_present_debounce @0x20001194 -- stock does `mov.w r0,#800; str r0,[r1]`
 *       at 0x0800897e and 0x08007d7e; 800 does not fit in a byte.
 *
 * Also word-accessed in the image, left as-is deliberately:
 *   0x200010fc g_soft_clock    -- a byte array [h,m,s,tenths]; only the zeroing
 *                                 pass touches it as a word.
 *   0x20001104 g_blink_halfsec -- only bits 0..2 are used and nothing is mapped
 *                                 at 0x20001105..07, so byte access is equivalent.
 *
 * Known still-wrong, not yet fixed (flagged by the UART port):
 *   0x200011ab g_uart_slot_state -- really a 6-byte ARRAY (0x200011ab..0x200011b0),
 *                                   one state per TX slot, declared here as a scalar.
 * ------------------------------------------------------------------------- */

#define g_tick_10ms_div            (*(volatile uint8_t *)0x20001000U)
#define g_tick_100ms_div           (*(volatile uint8_t *)0x20001001U)
#define g_tick_1s_div              (*(volatile uint8_t *)0x20001002U)
#define g_fatal_error_code         (*(volatile uint8_t *)0x20001003U)
#define g_boot_rx_timeout          (*(volatile uint8_t *)0x20001004U)
#define g_boot_iap_state           (*(volatile uint8_t *)0x200010c4U)
#define g_boot_iap_substate        (*(volatile uint8_t *)0x200010c5U)
#define g_boot_iap_offset          (*(volatile uint8_t *)0x200010d8U)
#define g_boot_sysclk_hz           (*(volatile uint8_t *)0x200010dcU)
#define g_d2_cluster_state         (*(volatile uint8_t *)0x200010e0U)
#define g_soft_clock               ((volatile uint8_t *)0x200010fcU)
#define g_blink_halfsec            (*(volatile uint8_t *)0x20001104U)
#define g_sched_tick_counter       (*(volatile uint32_t *)0x20001108U)  /* 32-bit: free-running ms counter; tested against 0x3FF/0x7FF */
#define g_tick_flag_fast           (*(volatile uint8_t *)0x2000110cU)
#define g_tick_flag_med            (*(volatile uint8_t *)0x2000110dU)
#define g_tick_flag_div16          (*(volatile uint8_t *)0x2000110eU)
#define g_tick_flag_div32          (*(volatile uint8_t *)0x2000110fU)
#define g_tick_flag_div64          (*(volatile uint8_t *)0x20001110U)
#define g_tick_flag_div128         (*(volatile uint8_t *)0x20001111U)
#define g_tick_flag_div256         (*(volatile uint8_t *)0x20001112U)
#define g_tick_flag_div512         (*(volatile uint8_t *)0x20001113U)
#define g_tick_flag_slow           (*(volatile uint8_t *)0x20001114U)
#define g_autobaud_tick_flag       (*(volatile uint8_t *)0x20001115U)
#define g_can_rx_ring_head         (*(volatile uint32_t *)0x20001118U)  /* 32-bit: phase 1 waits for this to reach 3 */
#define g_can_rx_ring_tail         (*(volatile uint32_t *)0x2000111cU)
#define g_swc_var_120              (*(volatile uint32_t *)0x20001120U)  /* 32-bit: CAN TX ring write index */
#define g_swc_var_124              (*(volatile uint32_t *)0x20001124U)  /* 32-bit: CAN TX ring read index */
#define g_can_link_flag            (*(volatile uint32_t *)0x20001130U)
#define g_can_retry_ctr            (*(volatile uint32_t *)0x20001134U)
#define g_can_err_flags            (*(volatile uint8_t *)0x20001138U)
#define g_can_bus_status           (*(volatile uint8_t *)0x20001139U)
#define g_profile0_activity_ctr    (*(volatile uint8_t *)0x2000113aU)
#define g_can_echo_buf_516         ((volatile uint8_t *)0x20001140U)
#define g_swc_tx_buf               ((volatile uint8_t *)0x20001148U)
#define g_tx_frame_amp             ((volatile uint8_t *)0x20001150U)
#define g_tx_frame_f0              ((volatile uint8_t *)0x20001157U)
#define g_tx_frame_326             ((volatile uint8_t *)0x2000115cU)
#define g_tx_frame_190             ((volatile uint8_t *)0x20001164U)
#define g_tx_frame_18c             ((volatile uint8_t *)0x2000116bU)
#define g_swc_keycode              (*(volatile uint8_t *)0x2000117fU)
#define g_swc_keydata              (*(volatile uint8_t *)0x20001180U)
#define g_swc_keycode_prev         (*(volatile uint8_t *)0x20001181U)
#define g_swc_keydata_ref          (*(volatile uint8_t *)0x20001182U)
#define g_periodic_tx_phase        (*(volatile uint8_t *)0x20001188U)
#define g_swc_resp_mode            (*(volatile uint8_t *)0x2000118aU)
#define g_swc_flag_118b            (*(volatile uint8_t *)0x2000118bU)
#define g_swc_timer_118c           (*(volatile uint8_t *)0x2000118cU)
#define g_misc_timer_118d          (*(volatile uint8_t *)0x2000118dU)
#define g_prndl_tx_7fe             ((volatile uint8_t *)0x2000118eU)
#define g_profile_active           (*(volatile uint8_t *)0x2000118fU)
#define g_autobaud_try_count       (*(volatile uint8_t *)0x20001190U)
#define g_bus_present_debounce     (*(volatile uint32_t *)0x20001194U)  /* 32-bit: stock stores 800 here as a word */
#define g_can_reinit_bitrate       (*(volatile uint32_t *)0x20001198U)
#define g_can_tx_hold_timer        (*(volatile uint32_t *)0x2000119cU)
#define g_canerr_hold_timer        (*(volatile uint8_t *)0x200011a0U)
#define g_tx_frame_9f              ((volatile uint8_t *)0x200011a1U)
#define g_uart_aux_rx_overflow_flag (*(volatile uint8_t *)0x200011aaU)
#define g_uart_slot_state          (*(volatile uint8_t *)0x200011abU)
#define g_uart_tx_slot_idx         (*(volatile uint8_t *)0x200011b1U)
#define g_uart_aux_slot_idx        (*(volatile uint8_t *)0x200011b2U)
#define g_uart_idle_delay          (*(volatile uint8_t *)0x200011b3U)
#define g_uart_tx_tick             (*(volatile uint8_t *)0x200011b4U)
#define g_gpio_initialized         (*(volatile uint8_t *)0x200011b5U)
#define g_cycle_ctr                (*(volatile uint8_t *)0x200011b7U)
#define g_cycle_ctr_max            (*(volatile uint8_t *)0x200011b8U)
#define g_rpm_decimator_ctr        (*(volatile uint8_t *)0x200011ceU)
#define g_param_struct             ((volatile uint8_t *)0x200011d0U)
#define g_param_profile_sel        (*(volatile uint8_t *)0x200011d1U)
#define g_param_shadow             ((volatile uint8_t *)0x20001220U)
#define g_can_rx_ring_buf          ((volatile uint8_t *)0x20001284U)
#define g_can_tx_ring_buf          ((volatile uint8_t *)0x200015a4U)
#define g_can_tx_busy_block        ((volatile uint8_t *)0x200018e4U)
#define g_softtimer_slots          ((volatile uint8_t *)0x200018f8U)
#define g_uart_frame_buf           ((volatile uint8_t *)0x200019f8U)
#define g_uart_rx_frame            ((volatile uint8_t *)0x20001b01U)
#define g_uart_ack_queue           ((volatile uint8_t *)0x20001b3dU)
#define g_status_block             ((volatile uint8_t *)0x20001b49U)
#define g_vstate_mid               ((volatile uint8_t *)0x20001b60U)
#define g_amp_level                (*(volatile uint8_t *)0x20001b75U)
#define g_version_str              ((volatile uint8_t *)0x20001b86U)
#define g_cmd_block                ((volatile uint8_t *)0x20001be7U)
#define g_periodic_tx_cfg          (*(volatile uint8_t *)0x20001bf5U)
#define g_libc_exit_var            (*(volatile uint8_t *)0x20001c1cU)
#define g_libc_impure_ptr          (*(volatile uint8_t *)0x20001c20U)

#endif // GLOBALS_H
