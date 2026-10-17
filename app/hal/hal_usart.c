/* ============================================================================
 * app/hal/hal_usart.c -- USART register driver (HAL layer).
 *
 * Ported from the file-static register helpers in the retired app/port/hw_uart.c, which
 * were themselves ported from the vendor-SDK copies decompiled into
 * reference/decompile/app/uart.c. Original flash addresses are quoted on every function.
 *
 * Where the stock code was specialised to USART2 (uart_tx_start @0x08003078,
 * uart_disable_usart2 @0x08003088) this layer takes the peripheral as a
 * parameter instead; the services layer binds USART2. The generic
 * USART1/USART3 init path (usart_init_uart1_uart3_DEAD @0x080029B2) has no
 * call site in this image and is deliberately not ported.
 *
 * LAYER CONTRACT: pure C11, MMIO only. No RAM state, no globals.h.
 * ==========================================================================*/

#include "hal_usart.h"

/* --- 0x08007320 USART_GetIntStatus --- both the enable bit and the status
 * flag must be set. */
int usart_get_it_status(USART_Module *usart, uint32_t it)
{
    uint16_t ctrl;

    switch ((it & 0xFFU) >> 5) {
    case 1:  ctrl = usart->CTRL1; break;
    case 2:  ctrl = usart->CTRL2; break;
    default: ctrl = usart->CTRL3; break;
    }
    if ((ctrl & (uint16_t)(1U << (it & 0x1FU))) == 0)
        return 0;
    return (usart->STS & (uint16_t)(1U << ((it >> 8) & 0xFFU))) != 0;
}

/* --- 0x08007184 USART_ConfigInt --- set/clear the enable bit only. */
void usart_it_config(USART_Module *usart, uint32_t it, int enable)
{
    volatile uint16_t *reg;
    uint16_t bit = (uint16_t)(1U << (it & 0x1FU));

    switch ((it & 0xFFU) >> 5) {
    case 1:  reg = &usart->CTRL1; break;
    case 2:  reg = &usart->CTRL2; break;
    default: reg = &usart->CTRL3; break;
    }
    if (enable)
        *reg = (uint16_t)(*reg | bit);
    else
        *reg = (uint16_t)(*reg & (uint16_t)~bit);
}

/* --- 0x0800716C USART_Enable --- CTRL1.UE. */
void usart_cmd_enable(USART_Module *usart, int enable)
{
    if (enable)
        usart->CTRL1 = (uint16_t)(usart->CTRL1 | USART_CTRL1_UE);
    else
        usart->CTRL1 = (uint16_t)(usart->CTRL1 & (uint16_t)~USART_CTRL1_UE);
}

/* --- 0x08007246 / 0x0800724E --- the data register is 9 bits wide. */
void usart_send_data(USART_Module *usart, uint8_t data)
{
    usart->DAT = (uint16_t)(data & USART_DAT_MASK);
}

uint8_t usart_receive_data(USART_Module *usart)
{
    return (uint8_t)(usart->DAT & USART_DAT_MASK);
}

/* --- 0x0800707E USART_Init, reduced to what the stock usart2_init asks for ---
 * 8 data bits, 1 stop bit, no parity, no hardware flow control, TX+RX on.
 *
 * Baud divisor, bit-identical to the stock arithmetic:
 *     d        = (25 * PCLK) / (4 * baud)      i.e. 100 x USARTDIV
 *     mantissa = d / 100
 *     fraction = ((d % 100) * 16 + 50) / 100   (rounded)
 *     BRCF     = (mantissa << 4) | (fraction & 0x0F)
 *
 * The stock code picks PCLK2 for USART1 and PCLK1 for everyone else; that
 * choice is the caller's here. USART2 is on APB1, so PCLK1_HZ = 16 MHz:
 *     d = 25 * 16000000 / (4 * 38400) = 2604
 *     mantissa = 26, fraction = (4 * 16 + 50) / 100 = 1  ->  BRCF = 0x01A1
 * = 26.0625, against the ideal 16e6 / (16 * 38400) = 26.0417: +0.08% error.
 *
 * Note the writes happen in the stock order -- CTRL2, CTRL1, CTRL3, BRCF --
 * and use the stock ABSOLUTE clear masks, not ~field. */
void usart_configure(USART_Module *usart, uint32_t pclk_hz, uint32_t baud)
{
    uint32_t d = (25U * pclk_hz) / (4U * baud);
    uint32_t mantissa = d / 100U;
    uint32_t fraction = ((d % 100U) * 16U + 50U) / 100U;

    usart->CTRL2 = (uint16_t)(usart->CTRL2 & USART_CTRL2_INIT_MASK);
    usart->CTRL1 = (uint16_t)((usart->CTRL1 & USART_CTRL1_INIT_MASK)
                              | USART_MODE_TX_RX);
    usart->CTRL3 = (uint16_t)(usart->CTRL3 & USART_CTRL3_INIT_MASK);
    usart->BRCF  = (uint16_t)((mantissa << 4) | (fraction & 0x0FU));
}

/* --- 0x08003078 --- arm the TXE interrupt; the ISR takes it from here.
 * Stock spelling: uart_tx_start(void), hard-wired to USART2. */
void usart_tx_start(USART_Module *usart)
{
    usart_it_config(usart, USART_INT_TXDE, 1);
}

/* --- no stock equivalent --- wait for STS.TXC, bounded. See hal_usart.h for
 * why this exists and why the bound is not optional. */
void usart_tx_drain(USART_Module *usart, uint32_t spins)
{
    while ((spins != 0U) && ((usart->STS & USART_STS_TXC) == 0U)) {
        spins--;
    }
}

/* --- 0x08003088 --- shut the port down (used by the sleep path).
 * Stock spelling: uart_disable_usart2(void). Order matters only in that it is
 * the stock order: UE cleared first, then TXE disarmed. */
void usart_disable(USART_Module *usart)
{
    usart_cmd_enable(usart, 0);
    usart_it_config(usart, USART_INT_TXDE, 0);
}
