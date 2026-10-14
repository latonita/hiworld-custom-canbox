/* ============================================================================
 * app/hal/hal_exti.c -- EXTI + AFIO line-source register driver (HAL layer).
 *
 * Ported from reference/decompile/app/gpio.c and reference/decompile/app/misc.c (Ghidra output), cross-checked
 * instruction-by-instruction against reference/2429-H1N4JPF20A-220524.app.bin.
 *
 * LAYER CONTRACT: pure C11, MMIO only -- this file touches peripheral
 * registers and nothing else. No RAM state, no globals.h.
 *
 * WHAT THE PRODUCT DOES WITH THIS BLOCK (for context; the policy itself lives
 * in the services/logic layers, not here)
 *   PB8 doubles as CAN_RX (AF5) and, in the firmware-update / sleep path, as
 *   an EXTI8 falling-edge input so CAN bus activity wakes the part out of
 *   STOP. Waking is not resumed in place: both EXTI handlers clear the line
 *   and go to fatal_error_reset(), which forces a WWDG reset -- i.e. "wake"
 *   means "reboot into the bootloader", which is exactly what the update
 *   path wants.
 *
 * WHY THE LINE SOURCE IS NOT AN F1 EXTICR
 *   This is a Nations N32L406CBL7. There is no F1-style AFIO_MAPR here, and
 *   AFIO is only { RMP_CFG, EXTI_CFG[4] }. EXTI_CFG[] is used solely as the
 *   EXTI line -> port selector (what F1 calls EXTICR). Pin muxing proper is a
 *   per-pin 4-bit AF number in GPIO AFL/AFH -- see hal_gpio.c.
 *
 * All register offsets are expressed through EXTI_Module / AFIO_Module from
 * app/include/n32l406_hw.h, which is generated from the vendor SDK, so
 * nothing here open-codes an offset -- with the single deliberate exception
 * of exti_line_init(), whose whole calling convention IS a byte offset into
 * EXTI_Module (see hal_exti.h).
 * ==========================================================================*/

#include "hal_exti.h"

/* ===========================================================================
 * exti_set_line_source -- 0x080054a0 (afio_pin_config in the decompile)
 *
 * AFIO->EXTI_CFG[] is this part's EXTICR: four registers, four 4-bit-spaced
 * slots each, selecting which port drives EXTI line n. The stock code masks
 * only 2 bits (0b11) of each slot even though the slots are 4 bits apart --
 * reproduced, because ports A..D need exactly 2 bits and widening the mask
 * would change what gets cleared.
 *
 *   port_sel  AFIO_EXTI_PORT_A..D
 *   line_num  EXTI line number 0..15 (the pin number), NOT a bit mask
 * ==========================================================================*/
void exti_set_line_source(uint32_t port_sel, uint32_t line_num)
{
    uint32_t idx   = line_num >> 2;
    uint32_t shift = (line_num & 3U) * 4U;

    AFIO->EXTI_CFG[idx] &= ~(3UL << shift);
    AFIO->EXTI_CFG[idx] |= port_sel << shift;
}

/* ===========================================================================
 * exti_line_init -- 0x080044ea (exti_init in the decompile)
 *
 * mode_ofs / trig_ofs are byte offsets into EXTI_Module, exactly as the stock
 * caller supplies them (see EXTI_MODE_* / EXTI_TRIG_* in hal_exti.h).
 * Disabling only clears the line in the selected mode register; enabling
 * clears it in both mask registers first, then sets mode and trigger.
 *
 *   line_mask one or more EXTI_LINE(n) bits
 *   mode_ofs  EXTI_MODE_INTERRUPT (IMASK) or EXTI_MODE_EVENT (EMASK)
 *   trig_ofs  EXTI_TRIG_RISING / _FALLING / _BOTH; ignored when disabling
 *   enable    non-zero to arm, zero to mask the line(s) off again
 * ==========================================================================*/
void exti_line_init(uint32_t line_mask, uint32_t mode_ofs,
                    uint32_t trig_ofs, int enable)
{
    volatile uint32_t *regs = (volatile uint32_t *)EXTI;

    if (!enable) {
        regs[mode_ofs / 4U] &= ~line_mask;
        return;
    }

    EXTI->IMASK &= ~line_mask;
    EXTI->EMASK &= ~line_mask;
    regs[mode_ofs / 4U] |= line_mask;

    EXTI->RT_CFG &= ~line_mask;
    EXTI->FT_CFG &= ~line_mask;

    if (trig_ofs == EXTI_TRIG_BOTH) {
        EXTI->RT_CFG |= line_mask;
        EXTI->FT_CFG |= line_mask;
    } else {
        regs[trig_ofs / 4U] |= line_mask;
    }
}

/* --- 0x080045b6 --- Pending AND unmasked. Both halves matter: PEND latches
 * regardless of IMASK, so the mask has to be consulted too. */
int exti_get_it_status(uint32_t line_mask)
{
    return ((EXTI->IMASK & line_mask) != 0U) && ((EXTI->PEND & line_mask) != 0U);
}

/* --- 0x080045d8 --- PEND is write-1-to-clear; the stock code writes the bare
 * line mask (not a read-modify-write), which is correct for a W1C register. */
void exti_clear_it_pending(uint32_t line_mask)
{
    EXTI->PEND = line_mask;
}
