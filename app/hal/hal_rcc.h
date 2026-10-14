/* ============================================================================
 * app/hal/hal_rcc.h -- RCC register driver (HAL layer).
 *
 * LAYER CONTRACT: pure C11, MMIO only. No RAM state, no globals.h, no C++.
 * Every entry point carries the flash address of the stock function it
 * reproduces so the rebuild can be diffed against
 * reference/2429-H1N4JPF20A-220524.app.bin.
 *
 * The app does NOT program the clock tree's source: SYSCLK is already
 * HSE 16 MHz -> PLL -> 64 MHz when it starts, set by the bootloader's
 * InitializeSysClock. (rcc.c's PLL state machine at 0x08005682 is dead code --
 * no live caller.) All the app does is set the bus prescalers and gate
 * peripheral clocks.
 *
 * Resulting tree, cross-checked two ways (CAN bit timing + the 1 ms tick):
 *     SYSCLK/AHB = 64 MHz,  APB1 = 16 MHz (/4),  APB2 = 16 MHz (/4)
 *     TIM2 clock = 2 x PCLK1 = 32 MHz (x2 applies whenever PPRE1 != /1)
 *
 * Register offsets verified against the vendor SDK (RCC_Module):
 *     APB2PCLKEN +0x18   APB1PCLKEN +0x1C   CTRLSTS +0x24
 * ==========================================================================*/

#ifndef HAL_RCC_H
#define HAL_RCC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* ---- API ---------------------------------------------------------------- */
void rcc_apb2_periph_clock(uint32_t periph, int enable);   /* 0x08005f48 */
void rcc_apb1_periph_clock(uint32_t periph, int enable);   /* 0x08005cb0 */
int  rcc_get_flag_status(uint32_t flag);                   /* 0x08005946 */
void rcc_clear_reset_flags(void);                          /* 0x08006000 */
void rcc_set_bus_prescalers_div4(void);                    /* 0x080032d2 part */
int  rcc_enable_hsi_for_flash(void);                       /* 0x080046b4 */

#ifdef __cplusplus
}
#endif

#endif /* HAL_RCC_H */
