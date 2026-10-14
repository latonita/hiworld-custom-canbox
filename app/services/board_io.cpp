/* ============================================================================
 * board_io.cpp -- see board_io.hpp.
 *
 * Ported from reference/decompile/app/gpio.c (gpio_static_init_a @0x08008458,
 * gpio_static_init_b @0x080084be, gpio_write_status_lines @0x080084f6,
 * gpio_can_xcvr_standby_pb12 @0x08007430, gpio_can_xcvr_standby_on_pb12
 * @0x08007440) and reference/decompile/app/misc.c (update_path_exti_setup @0x08003222),
 *.
 * ==========================================================================*/

#include "board_io.hpp"

#include <cstdint>

#include "ram_map.hpp"

extern "C" {
#include "hal_exti.h"
#include "hal_gpio.h"
#include "hal_nvic.h"
}

namespace svc {
namespace io {
namespace {

/* ---------------------------------------------------------------------------
 * The board's pin assignment, named once. The masks themselves come from
 * hal_gpio.h (which carries the per-pin role comments); these aliases exist so
 * the fan-out below reads as signals rather than as bit patterns, and so that
 * crossing REVERSE with ILLUMINATION would have to be a visible edit.
 * ------------------------------------------------------------------------ */
constexpr std::uint32_t kPinStatus0 = GPIO_PIN_5;   /* PB5  on GPIOB         */
constexpr std::uint32_t kPinIllum   = GPIO_PIN_15;  /* PC15 on GPIOC         */
constexpr std::uint32_t kPinReverse = GPIO_PIN_6;   /* PB6  on GPIOB         */
constexpr std::uint32_t kPinStatus3 = GPIO_PIN_4;   /* PB4  on GPIOB         */
constexpr std::uint32_t kPinStaticA = GPIO_PIN_2;   /* PB2  static input     */
constexpr std::uint32_t kPinStaticB = GPIO_PIN_3;   /* PB3  static input     */
constexpr std::uint32_t kPinCanRx   = GPIO_PIN_8;   /* PB8  CAN_RX / EXTI8   */
constexpr std::uint32_t kPinXcvrStb = GPIO_PIN_12;  /* PB12 transceiver STB  */

/* The stock flag at 0x200011B5 is set to 0xFF, not to 1. Kept as 0xFF because
 * nothing in the image tests it for equality with 1 and a future reader
 * diffing against Ghidra should see the same constant. */
constexpr std::uint8_t kGpioInitialisedValue = 0xFFU;

/* EXTI9_5_IRQn on the N32L40x. This is NOT a value to be guessed from an
 * STM32F1 header: almost every vector on this part is renumbered (see
 * docs/notes/vector-table-slot-map.md). 0x17 is what the image loads. */
constexpr std::uint8_t kExti9_5Irqn = 23U;

/* AIRCR PRIGROUP 5, the value the stock call passes (0x500). */
constexpr std::uint32_t kPriorityGroup = 0x500U;

/* PB8's wake interrupt runs at preempt 0 / sub 0 -- the top of the group.
 * Taken from the packed init block 0x01000017 in the decompile: irq 0x17,
 * preempt 0, sub 0, enable 1. */
constexpr std::uint8_t kWakePreempt = 0U;
constexpr std::uint8_t kWakeSubprio = 0U;

/* gpio_write_pin() takes an int level; the bit tests below produce a bool. */
constexpr int level(bool on) noexcept
{
    return on ? 1 : 0;
}

}  // namespace

/* ===========================================================================
 * 0x08008458 -- statusLinesInit
 *
 * Each line is driven low through PBC *before* its pin becomes an output, so
 * the lines never glitch high while the direction changes.
 * ========================================================================= */
/* 0x08008458 */
void statusLinesInit() noexcept
{
    gpio_write_pin(GPIOB, kPinStatus0, 0);
    gpio_write_pin(GPIOC, kPinIllum,   0);
    gpio_write_pin(GPIOB, kPinReverse, 0);
    gpio_write_pin(GPIOB, kPinStatus3, 0);

    gpio_init_pin(GPIOB, kPinStatus0, GPIO_MODE_OUTPUT_PP);
    gpio_init_pin(GPIOC, kPinIllum,   GPIO_MODE_OUTPUT_PP);
    gpio_init_pin(GPIOB, kPinReverse, GPIO_MODE_OUTPUT_PP);
    gpio_init_pin(GPIOB, kPinStatus3, GPIO_MODE_OUTPUT_PP);

    /* PB2 is an INPUT here, despite what docs/hiworld-gpio-io.md says: the
     * image calls gpio_init_pin(GPIOB, 0x4, 0) and mode 0 is PMODE = 00. */
    gpio_init_pin(GPIOB, kPinStaticA, GPIO_MODE_INPUT);

    ram::app().gpioInitialised = kGpioInitialisedValue;
}

/* ===========================================================================
 * 0x080084BE -- statusLinesShutdown (gpio_static_init_b)
 *
 * The teardown counterpart. The three asymmetries against statusLinesInit()
 * above are stock and are reproduced verbatim; see board_io.hpp.
 * ========================================================================= */
/* 0x080084BE */
void statusLinesShutdown() noexcept
{
    /* PB5 is deliberately NOT touched here -- only these three. */
    gpio_write_pin(GPIOC, kPinIllum,   0);
    gpio_write_pin(GPIOB, kPinReverse, 0);
    gpio_write_pin(GPIOB, kPinStatus3, 0);

    /* Driven low but NOT re-made outputs: they already are. The two static
     * pins, however, ARE handed back to the input state. */
    gpio_init_pin(GPIOB, kPinStaticB, GPIO_MODE_INPUT);
    gpio_init_pin(GPIOB, kPinStaticA, GPIO_MODE_INPUT);

    /* ram::app().gpioInitialised is left alone, exactly as in the image. */
}

/* ===========================================================================
 * 0x080084F6 -- writeStatusLines
 *
 * Called from the 8 ms task. The four flag bits come out of the single byte
 * the two CAN decoders write; reading them through the named masks is the
 * whole point of ram::VehicleState existing.
 * ========================================================================= */
/* 0x080084F6 */
void writeStatusLines() noexcept
{
    const ram::VehicleState& v = ram::app().vehicle;

    /* Stock call order: PB6, PB5, PB4, PC15. Kept. */
    gpio_write_pin(GPIOB, kPinReverse,
                   level(v.testFlag(ram::VehicleState::kFlagReverse)));
    gpio_write_pin(GPIOB, kPinStatus0,
                   level(v.testFlag(ram::VehicleState::kFlagAccOn)));
    gpio_write_pin(GPIOB, kPinStatus3,
                   level(v.testFlag(ram::VehicleState::kFlagSpare3)));
    gpio_write_pin(GPIOC, kPinIllum,
                   level(v.testFlag(ram::VehicleState::kFlagIllumOn)));
}

/* ===========================================================================
 * 0x08007430 / 0x08007440 -- setCanTransceiverStandby
 *
 * HIGH = standby, LOW = normal mode. hal_can's bring-up open-codes the "low"
 * half inline rather than calling this, so these remain the named entry points
 * for the shutdown path and for anything else that needs the line.
 * ========================================================================= */
/* 0x08007430 (standby = false) / 0x08007440 (standby = true) */
void configureCanTransceiverPin() noexcept
{
    gpio_init_pin(GPIOB, kPinXcvrStb, GPIO_MODE_OUTPUT_PP);
}

void setCanTransceiverStandby(bool standby) noexcept
{
    gpio_write_pin(GPIOB, kPinXcvrStb, level(standby));
}

/* ===========================================================================
 * 0x08003222 -- configurePb8Wake (update_path_exti_setup)
 *
 * *** THE ONLY WAKE SOURCE ARMED BEFORE THE DEEP-SLEEP WFI. ***
 *
 * Stock ORDER, which is what this reproduces: priority grouping first, then
 * the NVIC entry, THEN the pin, the AFIO line source and the EXTI line. (The
 * summary in docs/ARCHITECTURE.md §3 lists the NVIC step last; the image puts
 * it first. The image wins -- arming the NVIC before the line can latch a
 * pending edge is the safe order anyway.)
 *
 * The composite mode word GPIO_MODE_INPUT_EXTI_IT_FALLING is the literal
 * 0x10210000 from the pool at 0x0800339C: input pin + EXTI + interrupt +
 * falling edge. (Verified against the stock binary, not just the decompile.)
 *
 * PB8 keeps AF5 (CAN_RX) in AFH while PMODE says "input", but NOT because the
 * config helper stamps the AF nibble -- it does not: it skips the AF step
 * entirely when mode == GPIO_MODE_INPUT. The nibble simply survives from the
 * CAN bring-up, because nothing ever clears it. Same in stock. The end state
 * is what matters and it matches; only the earlier explanation of why was wrong.
 * ========================================================================= */
/* 0x08003222 */
void configurePb8Wake() noexcept
{
    nvic_set_priority_grouping(kPriorityGroup);
    nvic_init(kExti9_5Irqn, kWakePreempt, kWakeSubprio, 1);

    gpio_init_pin(GPIOB, kPinCanRx, GPIO_MODE_INPUT_EXTI_IT_FALLING);
    exti_set_line_source(AFIO_EXTI_PORT_B, 8U);

    exti_line_init(EXTI_LINE8, EXTI_MODE_INTERRUPT, EXTI_TRIG_FALLING, 1);
}

}  // namespace io
}  // namespace svc
