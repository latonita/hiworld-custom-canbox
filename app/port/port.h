/* ============================================================================
 * port.h -- shared contract for the reconstructed (ported) canbox APP.
 *
 * The app/src tree is raw Ghidra output, kept as the behavioural spec; the
 * app/port tree is the reconstructed, compilable C. Every declaration carries
 * the original flash address of what it reproduces so the two can be diffed.
 *
 * OWNERSHIP RULE (so parallel work does not collide): this file holds only the
 * SHARED core -- RCC, NVIC, WWDG, the fatal path, and common bit definitions.
 * Each subsystem owns its own <subsys>.h next to its .c and declares its own
 * API there. Do not add subsystem-specific declarations to this file.
 * ==========================================================================*/

#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include "n32l406_hw.h"
#include "globals.h"

/* ---- RCC peripheral-clock bits used by this firmware -------------------- */
#define RCC_APB1_TIM2       0x00000001U
#define RCC_APB1_WWDG       0x00000800U
#define RCC_APB1_USART2     0x00020000U
#define RCC_APB1_CAN        0x02000000U
#define RCC_APB1_PWR        0x10000000U

#define RCC_APB2_AFIO       0x00000001U
#define RCC_APB2_IOPA       0x00000004U
#define RCC_APB2_IOPB       0x00000008U
#define RCC_APB2_IOPC       0x00000010U
#define RCC_APB2_IOPD       0x00000020U
/* the stock gpio_init_pin enables AFIO + IOPA..IOPD in one write */
#define RCC_APB2_GPIO_ALL   0x0000003DU

#define RCC_CTRL_HSIEN      0x00000001U
#define RCC_CTRL_HSIRDY     0x00000002U
#define RCC_CTRLSTS_RMVF    0x01000000U
#define RCC_CFG_PPRE1_MASK  0x00000700U
#define RCC_CFG_PPRE1_DIV4  0x00000500U
#define RCC_CFG_PPRE2_MASK  0x00003800U
#define RCC_CFG_PPRE2_DIV4  0x00002800U   /* 0b101 << 11 */

/* flag ids for rcc_get_flag_status(): (register_selector << 5) | bit */
#define RCC_FLAG_WWDGRST    0x7EU         /* CTRLSTS bit 30 */

/* ---- clock tree (set up by the BOOTLOADER, not by this app) -------------
 * HSE 16 MHz -> PLL -> SYSCLK 64 MHz; the app only sets the bus prescalers.
 * Confirmed three ways: the CAN bit-timing arithmetic, the 1 ms TIM2 tick, and
 * the single initialised word in the stock scatter table
 * (*0x200010DC = 64000000). */
#define SYSCLK_HZ           64000000U
#define PCLK1_HZ            16000000U     /* APB1 = SYSCLK / 4 */
#define PCLK2_HZ            16000000U     /* APB2 = SYSCLK / 4 */
#define TIM2_CLK_HZ         32000000U     /* 2 x PCLK1 (x2 applies when PPRE1 != /1) */

/* ---- rcc (hw_rcc.c) ----------------------------------------------------- */
void rcc_apb2_periph_clock(uint32_t periph, int enable);   /* 0x08005f48 */
void rcc_apb1_periph_clock(uint32_t periph, int enable);   /* 0x08005cb0 */
int  rcc_get_flag_status(uint32_t flag);                   /* 0x08005946 */
void rcc_clear_reset_flags(void);                          /* 0x08006000 */
void rcc_set_bus_prescalers_div4(void);                    /* 0x080032d2 part */
int  rcc_enable_hsi_for_flash(void);                       /* 0x080046b4 */

/* ---- nvic (hw_nvic.c) --------------------------------------------------- */
void nvic_set_priority_grouping(uint32_t group);           /* 0x080033a4 */
void nvic_init(uint8_t irqn, uint8_t preempt, uint8_t sub, int enable); /* 0x080033ae */

/* ---- wwdg (hw_wwdg.c) --------------------------------------------------- */
void wwdg_kick(void);                                      /* 0x08002d3e */
void wwdg_set_prescaler(uint32_t wdgtb);                   /* 0x080073be */
void wwdg_set_window(uint32_t window);                     /* 0x080073d0 */
void wwdg_enable_set_counter(uint32_t counter);            /* 0x08007406 */
void wwdg_init(void);                                      /* 0x08002dec */
__attribute__((noreturn)) void wwdg_force_reset(void);     /* 0x08002e26/2e2e */

/* ---- sys (sys.c) -------------------------------------------------------- */
__attribute__((noreturn)) void fatal_error_reset(uint8_t code);  /* 0x08002e64 */

/* ---- main (main.c) ------------------------------------------------------ */
void main_loop(void);                                      /* 0x080088d8 */

#endif /* PORT_H */
