/* ============================================================================
 * port.h -- shared declarations for the reconstructed (ported) canbox APP.
 *
 * The app/src tree is raw Ghidra output, kept as the behavioural spec; the
 * app/port tree is the reconstructed, compilable C. Every function here carries the original
 * flash address of what it reproduces so the two can be diffed.
 * ==========================================================================*/

#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include "n32l406_hw.h"
#include "globals.h"

/* ---- RCC bit definitions used by this firmware -------------------------- */
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
/* the stock gpio_init_pin enables AFIO+IOPA..D in one go */
#define RCC_APB2_GPIO_ALL   0x0000003DU

#define RCC_CTRLSTS_RMVF    0x01000000U
#define RCC_CFG_PPRE1_MASK  0x00000700U
#define RCC_CFG_PPRE1_DIV4  0x00000500U
#define RCC_CFG_PPRE2_MASK  0x00003800U
#define RCC_CFG_PPRE2_DIV4  0x00002800U   /* 0b101 << 11 */

/* RCC_CTRLSTS flag ids for rcc_get_flag_status(): (regsel << 5) | bit */
#define RCC_FLAG_WWDGRST    0x7EU         /* CTRLSTS bit 30 */

/* ---- rcc ---------------------------------------------------------------- */
void rcc_apb2_periph_clock(uint32_t periph, int enable);   /* 0x08005f48 */
void rcc_apb1_periph_clock(uint32_t periph, int enable);   /* 0x08005cb0 */
int  rcc_get_flag_status(uint32_t flag);                   /* 0x08005946 */
void rcc_clear_reset_flags(void);                          /* 0x08006000 */
void rcc_set_bus_prescalers_div4(void);                    /* 0x080032d2 part */

/* ---- wwdg --------------------------------------------------------------- */
void wwdg_kick(void);                                      /* 0x08002d3e */
void wwdg_set_prescaler(uint32_t wdgtb);                   /* 0x080073be */
void wwdg_set_window(uint32_t window);                     /* 0x080073d0 */
void wwdg_enable_set_counter(uint32_t counter);            /* 0x08007406 */
void wwdg_init(void);                                      /* 0x08002dec */
__attribute__((noreturn)) void wwdg_force_reset(void);     /* 0x08002e26/2e2e */

/* ---- sys ---------------------------------------------------------------- */
__attribute__((noreturn)) void fatal_error_reset(uint8_t code);  /* 0x08002e64 */

/* ---- main --------------------------------------------------------------- */
void main_loop(void);                                      /* 0x080088d8 */

#endif /* PORT_H */
