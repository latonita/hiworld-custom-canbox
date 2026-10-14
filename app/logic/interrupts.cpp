/* ============================================================================
 * interrupts.cpp -- every hardware entry point into the application, in one
 * file, because the contract they satisfy is subtle and worth reading in one
 * place.
 *
 * THE CONTRACT
 * This image has NO vector table. It runs on the bootloader's, at
 * VTOR = 0x08000000, which is in write-protected flash and cannot be changed.
 * That table dispatches into the fixed veneer array in our preamble
 * (0x0800202A..0x08002201), and each veneer does
 *
 *      push {r4, lr}
 *      bl   <handler>          <- by SYMBOL NAME, resolved at link time
 *      pop  {r4, pc}
 *
 * So the five names below are an ABI. They must be `extern "C"` and they must
 * not be weak:
 *
 *   - a C++-mangled name does not match, the link still succeeds, and the weak
 *     TRAP_HANDLER default in app/startup/crt0.S stays in place. The box then
 *     resets with a trap code instead of working, and nothing in the build
 *     warns you;
 *   - crt0.S gives those five defaults deliberately non-stock codes (217, 229,
 *     232, 237, 247) precisely so this failure is identifiable. Finding one of
 *     them in ram::app().fatalErrorCode means a handler name did not match.
 *
 * Each handler is a shim: acknowledge the interrupt, delegate to the service
 * that owns the state. The bodies live with their service so the state and the
 * code that touches it stay together.
 *
 * THE ONE PLACE THE LAYERING INVERTS
 * The 1 ms TIM2 interrupt drives 10 ms and 100 ms work owned by several
 * different services. Rather than have one service reach sideways into
 * another, svc::sched::onTimerInterrupt() fans out. That keeps the dependency
 * arrows pointing one way everywhere else.
 * ==========================================================================*/

#include <cstdint>

#include "can_bus.hpp"
#include "head_unit.hpp"
#include "power.hpp"
#include "scheduler.hpp"

extern "C" {
#include "hal_exti.h"
}

extern "C" {

/* --- 0x08002F72 --- veneer slot 37, IRQ 28. The 1 ms time base. */
void TIM2_IRQHandler(void)
{
    svc::sched::onTimerInterrupt();
}

/* --- 0x08002F32 --- veneer slot 29, IRQ 54 (CAN_RX0 on this part; the array
 * is in STM32F103 order and the bootloader's table bridges the two -- see
 * app/startup/crt0.S). The FIFO release inside the receive path is what clears
 * the FMP0 condition, so there is nothing else to acknowledge here. */
void CAN_RX0_IRQHandler(void)
{
    svc::can::onRxInterrupt();
}

/* --- 0x0800286E --- veneer slot 47, IRQ 38. USART2 RXNE / TXE. */
void USART2_IRQHandler(void)
{
    svc::hu::onInterrupt();
}

/* --- 0x08002EDC --- EXTI line 2 (PB2).
 * Armed only by a routine with no live caller in this image, so in practice it
 * never fires. Reproduced anyway: the vector slot must not fall back to a
 * generic trap, because then the recorded code would not say what happened. */
void EXTI2_IRQHandler(void)
{
    if (exti_get_it_status(EXTI_LINE2) != 0) {
        exti_clear_it_pending(EXTI_LINE2);
        svc::power::fatalErrorReset(svc::power::kFatalExti2);
    }
}

/* --- 0x08002F3E --- EXTI lines 5..9; only line 8 (PB8 = CAN_RX) is ever
 * armed, by svc::io::configurePb8Wake() just before the deep-sleep WFI.
 *
 * This is the wake path. The box does not resume: by this point the watchdog
 * clock has been gated, so the reset that actually happens is this one. */
void EXTI9_5_IRQHandler(void)
{
    if (exti_get_it_status(EXTI_LINE8) != 0) {
        exti_clear_it_pending(EXTI_LINE8);
        svc::power::fatalErrorReset(svc::power::kFatalExti9_5);
    }
}

}  // extern "C"
