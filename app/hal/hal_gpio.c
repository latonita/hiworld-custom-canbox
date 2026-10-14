/* ============================================================================
 * app/hal/hal_gpio.c -- GPIO register driver (HAL layer).
 *
 * Ported from reference/decompile/app/gpio.c, reference/decompile/app/core.c and reference/decompile/app/misc.c (Ghidra
 * output), cross-checked instruction-by-instruction against
 * reference/2429-H1N4JPF20A-220524.app.bin.
 *
 * LAYER CONTRACT: pure C11, MMIO only -- this file touches peripheral
 * registers and nothing else. It holds no RAM state, declares no globals and
 * holds no RAM state at all. Board policy (which pin is a status
 * line, which pin wakes the part) belongs to the services/logic layers.
 *
 * WHY THIS DOES NOT LOOK LIKE AN STM32F1 PORT
 *   This is a Nations N32L406CBL7. Its GPIO block is F4/L4-shaped:
 *     PMODE +0x00  POTYPE +0x04  SR +0x08  PUPD +0x0C
 *     PID   +0x10 (input)   POD +0x14 (output)   PBSC +0x18 (bit set)
 *     PLOCK +0x1C  AFL +0x20  AFH +0x24  PBC +0x28 (bit clear)  DS +0x2C
 *   There is no CRL/CRH and no F1-style AFIO_MAPR: muxing is a per-pin 4-bit
 *   AF NUMBER in AFL (pins 0-7) / AFH (pins 8-15). AFIO on this part is only
 *   { RMP_CFG, EXTI_CFG[4] }, and EXTI_CFG is used solely as the EXTI line ->
 *   port selector (what F1 calls EXTICR) -- see hal_exti.c.
 *   Note also the two separate bit-banging registers: PBSC +0x18 sets, PBC
 *   +0x28 clears. There is no BSRR-with-high-half trick here.
 *   (docs/hiworld-gpio-io.md still carries an older, wrong offset table --
 *   PID at +0x08, POD at +0x0C. The disassembly reads the input at +0x10 and
 *   the output at +0x14, matching n32l406_hw.h. Trust the header.)
 *
 * All register offsets below are expressed through GPIO_Module / EXTI_Module
 * from app/include/n32l406_hw.h, which is generated from the vendor SDK, so
 * nothing here open-codes an offset.
 * ==========================================================================*/

#include "hal_gpio.h"
#include "hal_rcc.h"    /* rcc_apb2_periph_clock(), RCC_APB2_GPIO_ALL */

/* ===========================================================================
 * gpio_apply_pin_config -- 0x08005172
 *
 * The workhorse (vendor SDK GPIO_InitPeripheral). Walks pin 0..15 while any
 * bit of cfg->pin at or above the current index is still set, and for each
 * selected pin programs AF, PMODE, PUPD, DS, SR, POTYPE and -- if the mode
 * carries GPIO_MODE_EXTI -- the four EXTI masks.
 *
 * The EXTI tail (step 8) is NOT a layering slip: the stock vendor driver
 * really does program EXTI.IMASK/EMASK/RT_CFG/FT_CFG from inside the GPIO
 * init routine, so it stays here. Only the raw masks are touched -- the line
 * source selector and the enable sequencing live in hal_exti.c.
 *
 * Two quirks worth knowing, both reproduced verbatim:
 *  1) the AF nibble is written for modes INPUT, AF_PP, AF_OD and ANALOG, but
 *     NOT for the two plain output modes. So an input pin still gets an AF
 *     number stamped into AFL/AFH (0xF = none, unless gpio_init_pin overrode
 *     it -- see there).
 *  2) a pull-up (1) also SETS the output latch via PBSC, a pull-down (2)
 *     CLEARS it via PBC. The stock image really does touch the output register
 *     from a "pull" setting; harmless here because every call made by this
 *     firmware passes GPIO_PULL_NONE.
 * ==========================================================================*/
void gpio_apply_pin_config(GPIO_Module *port, const gpio_pin_cfg_t *cfg)
{
    uint32_t pins = cfg->pin;
    uint32_t pin;

    for (pin = 0; (pins >> pin) != 0U; pin++) {
        uint32_t bit     = 1UL << pin;
        uint32_t field2  = pin * 2U;          /* 2-bit fields: PMODE/PUPD/DS  */
        uint32_t afshift = (pin & 7U) * 4U;   /* 4-bit AF nibble within AFL/H */

        if ((pins & bit) == 0U)
            continue;

        /* 1. alternate-function nibble (see quirk 1 above) */
        if (cfg->mode == GPIO_MODE_AF_PP   || cfg->mode == GPIO_MODE_AF_OD ||
            cfg->mode == GPIO_MODE_INPUT   || cfg->mode == GPIO_MODE_ANALOG) {
            if ((pin & 8U) != 0U)
                port->AFH = (port->AFH & ~(0xFUL << afshift)) |
                            (cfg->alternate << afshift);
            else
                port->AFL = (port->AFL & ~(0xFUL << afshift)) |
                            (cfg->alternate << afshift);
        }

        /* 2. PMODE. The stock code masks the field with 3 but ORs in the low
         * NIBBLE of the mode byte; for every mode this firmware uses the low
         * nibble is already <= 3, so the two agree. Kept as-is. */
        port->PMODE = (port->PMODE & ~(3UL << field2)) |
                      ((cfg->mode & 0xFUL) << field2);

        /* 3. PUPD */
        port->PUPD = (port->PUPD & ~(3UL << field2)) |
                     ((uint32_t)(cfg->pull & 3U) << field2);

        /* 4. DS (drive strength) -- only for driving modes, never for analog */
        if (((cfg->mode & 3UL) != 0UL) && (cfg->mode != GPIO_MODE_ANALOG)) {
            port->DS = (port->DS & ~(3UL << field2)) |
                       ((uint32_t)(cfg->drive & 3U) << field2);
        }

        /* 5. SR (slew-rate limit), one bit per pin */
        port->SR = (port->SR & ~bit) |
                   ((uint32_t)(cfg->slew & 1U) << pin);

        /* 6. pre-drive the output latch from the pull setting (quirk 2) */
        if (cfg->pull == GPIO_PULL_DOWN)
            port->PBC |= bit;
        else if (cfg->pull == GPIO_PULL_UP)
            port->PBSC |= bit;

        /* 7. POTYPE: bit 4 of the mode byte is the open-drain flag */
        if (cfg->mode == GPIO_MODE_OUTPUT_PP || cfg->mode == GPIO_MODE_AF_PP ||
            cfg->mode == GPIO_MODE_OUTPUT_OD || cfg->mode == GPIO_MODE_AF_OD) {
            port->POTYPE = (port->POTYPE & ~bit) |
                           (((cfg->mode >> 4) & 1UL) << pin);
        }

        /* 8. the EXTI half of the mode word */
        if ((cfg->mode & GPIO_MODE_EXTI) != 0U) {
            EXTI->IMASK  = (EXTI->IMASK  & ~bit) |
                           ((cfg->mode & GPIO_MODE_EXTI_IT)      ? bit : 0U);
            EXTI->EMASK  = (EXTI->EMASK  & ~bit) |
                           ((cfg->mode & GPIO_MODE_EXTI_EVT)     ? bit : 0U);
            EXTI->RT_CFG = (EXTI->RT_CFG & ~bit) |
                           ((cfg->mode & GPIO_MODE_EXTI_RISING)  ? bit : 0U);
            EXTI->FT_CFG = (EXTI->FT_CFG & ~bit) |
                           ((cfg->mode & GPIO_MODE_EXTI_FALLING) ? bit : 0U);
        }
    }
}

/* ===========================================================================
 * gpio_init_pin -- 0x08002494
 *
 * The three-argument convenience wrapper every subsystem calls. It clocks the
 * whole GPIO/AFIO group, builds a gpio_pin_cfg_t on the stack and hands it to
 * gpio_apply_pin_config.
 *
 * The interesting part is the hardcoded port+pin -> AF-number table. On this
 * part there is no remap register, so the only way a peripheral reaches a pin
 * is through this nibble, and the stock firmware keeps the mapping here rather
 * than at the call sites:
 *      PA2, PA3   -> AF4   (USART2 TX/RX)
 *      PB8, PB9   -> AF5   (CAN RX/TX)
 *      PB11       -> AF5
 *      PB10       -> AF0
 *      everything else -> 0xF (no alternate function)
 * The EXTI bring-up of PB8 leaves AF5 sitting in AFH -- the pin stays mapped to
 * the CAN peripheral while PMODE says "input". That is stock behaviour, not a
 * porting slip. Note WHY, because the obvious explanation is wrong:
 * gpio_apply_pin_config does NOT stamp the AF nibble for an input pin, it skips
 * the AF step entirely when mode == GPIO_MODE_INPUT. The nibble is simply left
 * over from the CAN bring-up and nothing ever clears it.
 *
 * Faithfulness note on cfg.drive: the stock function builds its config block
 * on top of its own pushed argument registers and only initialises offsets
 * 0, 3, 4, 8 and 12. Offset 2 (DS) is left holding byte 2 of the port base
 * address, which is 0x01 for GPIOA..GPIOD alike -- so every pin configured
 * this way gets DS = 1, deterministically, by accident. Reproduced here as an
 * explicit 1 so the rebuilt firmware drives the status lines with the same
 * strength as the stock one. Offsets 5..7 are likewise uninitialised in the
 * original but gpio_apply_pin_config never reads them.
 *
 * Note this is the one place in the GPIO HAL that reaches outside the GPIO
 * block: it gates the APB2 clocks through hal_rcc, exactly as the stock
 * function does (one write enabling AFIO + IOPA..IOPD).
 * ==========================================================================*/
void gpio_init_pin(GPIO_Module *port, uint32_t pin_mask, uint32_t mode)
{
    gpio_pin_cfg_t cfg;

    rcc_apb2_periph_clock(RCC_APB2_GPIO_ALL, 1);

    cfg.pin   = (uint16_t)pin_mask;
    cfg.drive = 1U;                 /* see the note above -- stock artefact   */
    cfg.slew  = 0U;
    cfg.pull  = GPIO_PULL_NONE;
    cfg.reserved[0] = 0U;
    cfg.reserved[1] = 0U;
    cfg.reserved[2] = 0U;
    cfg.mode  = mode;

    if (port == GPIOA && pin_mask == GPIO_PIN(2))
        cfg.alternate = GPIO_AF4;           /* USART2_TX  */
    else if (port == GPIOA && pin_mask == GPIO_PIN(3))
        cfg.alternate = GPIO_AF4;           /* USART2_RX  */
    else if (port == GPIOB && pin_mask == GPIO_PIN(10))
        cfg.alternate = GPIO_AF0;
    else if (port == GPIOB && pin_mask == GPIO_PIN(11))
        cfg.alternate = GPIO_AF5;
    else if (port == GPIOB && pin_mask == GPIO_PIN(8))
        cfg.alternate = GPIO_AF5;           /* CAN_RX     */
    else if (port == GPIOB && pin_mask == GPIO_PIN(9))
        cfg.alternate = GPIO_AF5;           /* CAN_TX     */
    else
        cfg.alternate = GPIO_AF_NONE;

    gpio_apply_pin_config(port, &cfg);
}

/* --- 0x080053a2 --- PBSC: writing a 1 sets that output bit. */
void gpio_set_bit(GPIO_Module *port, uint32_t pin_mask)
{
    port->PBSC = pin_mask;
}

/* --- 0x080053aa --- PBC: writing a 1 clears that output bit. Separate
 * register on this part; not the upper half of PBSC. */
void gpio_clear_bit(GPIO_Module *port, uint32_t pin_mask)
{
    port->PBC = pin_mask;
}

/* --- 0x0800252a --- */
void gpio_write_pin(GPIO_Module *port, uint32_t pin_mask, int value)
{
    if (value == 0)
        gpio_clear_bit(port, pin_mask);
    else
        gpio_set_bit(port, pin_mask);
}

/* --- 0x08002548 (tail-calls gpio_read_input_bit @0x0800536e) ---
 * Reads PID (+0x10), the INPUT data register. */
int gpio_read_pin(GPIO_Module *port, uint32_t pin_mask)
{
    return (port->PID & pin_mask) != 0U;
}
