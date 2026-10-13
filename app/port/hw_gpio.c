/* ============================================================================
 * hw_gpio.c -- GPIO + EXTI, ported from app/src/gpio.c, app/src/core.c and
 * app/src/misc.c (Ghidra output), cross-checked instruction-by-instruction
 * against reference/2429-H1N4JPF20A-220524.app.bin.
 *
 * WHAT THIS BLOCK DOES IN THE PRODUCT
 *   - Four hardwired status lines to the head unit: PB5, PC15 (illumination),
 *     PB6 (reverse) and PB4, all push-pull outputs driven low at boot.
 *   - PB2 (and PB3, in the sleep path) as plain inputs.
 *   - PB8 doubles as CAN_RX (AF5) and, in the firmware-update / sleep path, as
 *     an EXTI8 falling-edge input so CAN bus activity wakes the part out of
 *     STOP. Waking is not resumed in place: both EXTI handlers clear the line
 *     and go to fatal_error_reset(), which forces a WWDG reset -- i.e. "wake"
 *     means "reboot into the bootloader", which is exactly what the update
 *     path wants.
 *
 * WHY IT DOES NOT LOOK LIKE AN STM32F1 PORT
 *   This is a Nations N32L406CBL7. Its GPIO block is F4/L4-shaped:
 *     PMODE +0x00  POTYPE +0x04  SR +0x08  PUPD +0x0C
 *     PID   +0x10 (input)   POD +0x14 (output)   PBSC +0x18 (bit set)
 *     PLOCK +0x1C  AFL +0x20  AFH +0x24  PBC +0x28 (bit clear)  DS +0x2C
 *   There is no CRL/CRH and no F1-style AFIO_MAPR: muxing is a per-pin 4-bit
 *   AF NUMBER in AFL (pins 0-7) / AFH (pins 8-15). AFIO on this part is only
 *   { RMP_CFG, EXTI_CFG[4] }, and EXTI_CFG is used solely as the EXTI line ->
 *   port selector (what F1 calls EXTICR).
 *   Note also the two separate bit-banging registers: PBSC +0x18 sets, PBC
 *   +0x28 clears. There is no BSRR-with-high-half trick here.
 *   (docs/hiworld-gpio-io.md still carries an older, wrong offset table --
 *   PID at +0x08, POD at +0x0C. The disassembly reads the input at +0x10 and
 *   the output at +0x14, matching n32l406_hw.h. Trust the header.)
 *
 * All register offsets below are expressed through GPIO_Module / EXTI_Module /
 * AFIO_Module from app/include/n32l406_hw.h, which is generated from the
 * vendor SDK, so nothing here open-codes an offset.
 * ==========================================================================*/

#include "port.h"
#include "hw_gpio.h"

/* Offsets into EXTI_Module, used the way the stock HAL uses them: the caller
 * of the EXTI init helper passes a REGISTER OFFSET, not an enum. Interrupt vs
 * event is IMASK/EMASK; trigger is RT_CFG/FT_CFG, with 0x10 meaning "both"
 * (0x10 is EXTI.SWIE's offset and is never written as a register here -- the
 * vendor SDK simply reuses the value as a sentinel). */
#define EXTI_MODE_INTERRUPT     0x00U   /* EXTI->IMASK  */
#define EXTI_MODE_EVENT         0x04U   /* EXTI->EMASK  */
#define EXTI_TRIG_RISING        0x08U   /* EXTI->RT_CFG */
#define EXTI_TRIG_FALLING       0x0CU   /* EXTI->FT_CFG */
#define EXTI_TRIG_BOTH          0x10U   /* sentinel: set RT_CFG and FT_CFG    */

/* Fatal codes the stock EXTI handlers hand to fatal_error_reset(). Taken from
 * the image (movs r0,#0x7a / movs r0,#0x85); they are part of the firmware's
 * reset-reason numbering, so keep them. */
#define FATAL_EXTI2             0x7AU
#define FATAL_EXTI9_5           0x85U

/* ===========================================================================
 * gpio_apply_pin_config -- 0x08005172
 *
 * The workhorse (vendor SDK's GPIO_InitPeripheral). Walks pin 0..15 while any
 * bit of cfg->pin at or above the current index is still set, and for each
 * selected pin programs AF, PMODE, PUPD, DS, SR, POTYPE and -- if the mode
 * carries GPIO_MODE_EXTI -- the four EXTI masks.
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
 * Because gpio_apply_pin_config also stamps the AF nibble for INPUT mode, the
 * EXTI bring-up of PB8 leaves AF5 sitting in AFH -- the pin stays mapped to
 * the CAN peripheral while PMODE says "input". That is stock behaviour, not a
 * porting slip.
 *
 * Faithfulness note on cfg.drive: the stock function builds its config block
 * on top of its own pushed argument registers and only initialises offsets
 * 0, 3, 4, 8 and 12. Offset 2 (DS) is left holding byte 2 of the port base
 * address, which is 0x01 for GPIOA..GPIOD alike -- so every pin configured
 * this way gets DS = 1, deterministically, by accident. Reproduced here as an
 * explicit 1 so the rebuilt firmware drives the status lines with the same
 * strength as the stock one. Offsets 5..7 are likewise uninitialised in the
 * original but gpio_apply_pin_config never reads them.
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

/* ===========================================================================
 * gpio_status_lines_init -- 0x08008458 (gpio_static_init_a in the decompile)
 *
 * Boot-time bring-up of the discrete lines to the head unit. Order matters:
 * each line is driven low through PBC *before* its pin becomes an output, so
 * the lines never glitch high while the direction changes.
 *
 *      PB5  <- state bit 0        PC15 <- state bit 1 (illumination)
 *      PB6  <- state bit 2 (R)    PB4  <- state bit 3
 *
 * PB2 is then configured as an INPUT (mode 0). docs/hiworld-gpio-io.md calls
 * PB2 a "static output forced low"; the image disagrees -- the call is
 * gpio_init_pin(GPIOB, 0x4, 0), i.e. PMODE = 00 = input. The preceding writes
 * to PBC never touch PB2.
 *
 * Finally the "GPIO brought up" flag at the fixed RAM address 0x200011B5 is
 * set to 0xFF.
 * ==========================================================================*/
void gpio_status_lines_init(void)
{
    gpio_write_pin(GPIOB, GPIO_PIN_5,  0);
    gpio_write_pin(GPIOC, GPIO_PIN_15, 0);
    gpio_write_pin(GPIOB, GPIO_PIN_6,  0);
    gpio_write_pin(GPIOB, GPIO_PIN_4,  0);

    gpio_init_pin(GPIOB, GPIO_PIN_5,  GPIO_MODE_OUTPUT_PP);
    gpio_init_pin(GPIOC, GPIO_PIN_15, GPIO_MODE_OUTPUT_PP);
    gpio_init_pin(GPIOB, GPIO_PIN_6,  GPIO_MODE_OUTPUT_PP);
    gpio_init_pin(GPIOB, GPIO_PIN_4,  GPIO_MODE_OUTPUT_PP);

    gpio_init_pin(GPIOB, GPIO_PIN_2,  GPIO_MODE_INPUT);

    g_gpio_initialized = 0xFFU;
}

/* ===========================================================================
 * EXTI
 * ==========================================================================*/

/* --- 0x080054a0 (afio_pin_config) ---
 * AFIO->EXTI_CFG[] is this part's EXTICR: four registers, four 4-bit-spaced
 * slots each, selecting which port drives EXTI line n. The stock code masks
 * only 2 bits (0b11) of each slot even though the slots are 4 bits apart --
 * reproduced, because ports A..D need exactly 2 bits and widening the mask
 * would change what gets cleared. */
static void afio_exti_line_source(uint32_t port_sel, uint32_t pin)
{
    uint32_t idx   = pin >> 2;
    uint32_t shift = (pin & 3U) * 4U;

    AFIO->EXTI_CFG[idx] &= ~(3UL << shift);
    AFIO->EXTI_CFG[idx] |= port_sel << shift;
}

/* --- 0x080044ea (exti_init) ---
 * mode_ofs / trig_ofs are byte offsets into EXTI_Module, exactly as the stock
 * caller supplies them (see EXTI_MODE_* / EXTI_TRIG_* above). Disabling only
 * clears the line in the selected mode register; enabling clears it in both
 * mask registers first, then sets mode and trigger. */
static void exti_line_init(uint32_t line, uint32_t mode_ofs,
                           uint32_t trig_ofs, int enable)
{
    volatile uint32_t *regs = (volatile uint32_t *)EXTI;

    if (!enable) {
        regs[mode_ofs / 4U] &= ~line;
        return;
    }

    EXTI->IMASK &= ~line;
    EXTI->EMASK &= ~line;
    regs[mode_ofs / 4U] |= line;

    EXTI->RT_CFG &= ~line;
    EXTI->FT_CFG &= ~line;

    if (trig_ofs == EXTI_TRIG_BOTH) {
        EXTI->RT_CFG |= line;
        EXTI->FT_CFG |= line;
    } else {
        regs[trig_ofs / 4U] |= line;
    }
}

/* ===========================================================================
 * exti_pb8_wake_config -- 0x08003222 (update_path_exti_setup in the decompile)
 *
 * Arms PB8 (= CAN_RX) as a falling-edge EXTI8 interrupt just before the part
 * is put into STOP on the firmware-update / sleep path. Any CAN bus activity
 * then pulls the line low, EXTI9_5 fires, and the handler resets the box.
 *
 * IRQ 23 is EXTI9_5 on the N32L40x. That is NOT the STM32F1 number (F1 puts
 * EXTI9_5 at 23 as well, but almost every other vector on this part is
 * renumbered -- see n32l406_hw.h); 0x17 in the image is the N32 number and is
 * used directly.
 *
 * Priority group 0x500 = AIRCR PRIGROUP 5, then preempt 0 / sub 0 / enabled.
 * ==========================================================================*/
void exti_pb8_wake_config(void)
{
    nvic_set_priority_grouping(0x500U);
    nvic_init(23U, 0U, 0U, 1);                       /* EXTI9_5_IRQn = 23 */

    gpio_init_pin(GPIOB, GPIO_PIN_8, GPIO_MODE_INPUT_EXTI_IT_FALLING);
    afio_exti_line_source(AFIO_EXTI_PORT_B, 8U);

    exti_line_init(EXTI_LINE8, EXTI_MODE_INTERRUPT, EXTI_TRIG_FALLING, 1);
}

/* --- 0x080045b6 --- Pending AND unmasked. Both halves matter: PEND latches
 * regardless of IMASK, so the mask has to be consulted too. */
int exti_get_it_status(uint32_t line)
{
    return ((EXTI->IMASK & line) != 0U) && ((EXTI->PEND & line) != 0U);
}

/* --- 0x080045d8 --- PEND is write-1-to-clear; the stock code writes the bare
 * line mask (not a read-modify-write), which is correct for a W1C register. */
void exti_clear_it_pending(uint32_t line)
{
    EXTI->PEND = line;
}

/* ===========================================================================
 * Interrupt handlers.
 *
 * These names must match app/startup/crt0.S exactly; defining them non-weak
 * here overrides the TRAP_HANDLER stubs declared there.
 *
 * Both are deliberate reset paths, not wake-and-continue paths:
 * fatal_error_reset() stores the code at 0x20001003 and spins on the window
 * watchdog until it resets the part, which re-enters the IAP bootloader. The
 * trap codes are the stock ones (0x7A and 0x85).
 * ==========================================================================*/

/* --- 0x08002edc --- EXTI line 2 (PB2). Armed only by the dead
 * exti_line_setup_DEAD @0x0800327c, so in practice this never fires; ported
 * anyway because the vector slot must not fall back to a generic trap. */
void EXTI2_IRQHandler(void)
{
    if (exti_get_it_status(EXTI_LINE2)) {
        exti_clear_it_pending(EXTI_LINE2);
        fatal_error_reset(FATAL_EXTI2);
    }
}

/* --- 0x08002f3e --- EXTI lines 5..9; only line 8 (PB8 = CAN_RX) is ever
 * armed, by exti_pb8_wake_config above. */
void EXTI9_5_IRQHandler(void)
{
    if (exti_get_it_status(EXTI_LINE8)) {
        exti_clear_it_pending(EXTI_LINE8);
        fatal_error_reset(FATAL_EXTI9_5);
    }
}
