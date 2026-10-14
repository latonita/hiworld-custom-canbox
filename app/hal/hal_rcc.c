/* ============================================================================
 * app/hal/hal_rcc.c -- RCC register driver (HAL layer).
 * Ported from the retired app/port/hw_rcc.c, which was ported from reference/decompile/app/rcc.c (Ghidra).
 *
 * MMIO only -- this file touches no RAM state. See hal_rcc.h for the clock-tree
 * notes and the register-offset provenance.
 * ==========================================================================*/

#include "n32l406_hw.h"
#include "hal_rcc.h"

/* --- 0x08005f48 rcc_apb2_enable --- */
void rcc_apb2_periph_clock(uint32_t periph, int enable)
{
    if (enable)
        RCC->APB2PCLKEN |= periph;
    else
        RCC->APB2PCLKEN &= ~periph;
}

/* --- 0x08005cb0 rcc_apb1_periph_clock_cmd --- */
void rcc_apb1_periph_clock(uint32_t periph, int enable)
{
    if (enable)
        RCC->APB1PCLKEN |= periph;
    else
        RCC->APB1PCLKEN &= ~periph;
}

/* --- 0x08005946 rcc_get_flag_status ---
 * flag is encoded as (register_selector << 5) | bit_number:
 *   >>5 == 1 -> CTRL,  == 2 -> LDCTRL,  else -> CTRLSTS
 * e.g. 0x7E = 126 -> CTRLSTS bit 30 = WWDGRSTF. */
int rcc_get_flag_status(uint32_t flag)
{
    uint32_t reg;

    switch ((int)flag >> 5) {
    case 1:  reg = RCC->CTRL;    break;
    case 2:  reg = RCC->LDCTRL;  break;
    default: reg = RCC->CTRLSTS; break;
    }
    return (reg & (1u << (flag & 0x1F))) != 0;
}

/* --- 0x08006000 rcc_clear_reset_flags --- CTRLSTS.RMVF pulse (bit 24). */
void rcc_clear_reset_flags(void)
{
    RCC->CTRLSTS |= RCC_CTRLSTS_RMVF;
    RCC->CTRLSTS &= ~RCC_CTRLSTS_RMVF;
}

/* --- 0x080032d2 (part) sets both bus prescalers to /4 ---
 * CFG.PPRE1 = bits 10:8, CFG.PPRE2 = bits 13:11; 0b101 = /4 in both fields.
 * The stock code writes 0x500 to PPRE1 and (0x500 << 3) to PPRE2, i.e. the same
 * 0b101 pattern shifted into each field. */
void rcc_set_bus_prescalers_div4(void)
{
    RCC->CFG = (RCC->CFG & ~RCC_CFG_PPRE1_MASK) | RCC_CFG_PPRE1_DIV4;
    RCC->CFG = (RCC->CFG & ~RCC_CFG_PPRE2_MASK) | RCC_CFG_PPRE2_DIV4;
}

/* --- 0x080046b4 rcc_enable_hsi_for_flash ---
 * The flash controller is HSI-fed on this part, so the param-page save path
 * brings HSI up before FLASH_Unlock. Inferred from the stock call order (this
 * is called immediately before the unlock), not from a datasheet statement.
 * Returns non-zero once HSIRDY is set; gives up after the stock spin budget. */
int rcc_enable_hsi_for_flash(void)
{
    uint32_t spin = 0;

    if ((RCC->CTRL & RCC_CTRL_HSIRDY) != 0U)
        return 1;                              /* already running */

    RCC->CTRL |= RCC_CTRL_HSIEN;
    do {
        spin++;
        if ((RCC->CTRL & RCC_CTRL_HSIRDY) != 0U)
            break;
    } while (spin != 0x500U);                  /* stock spin budget */

    return (RCC->CTRL & RCC_CTRL_HSIRDY) != 0U;
}
