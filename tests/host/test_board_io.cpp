/* ============================================================================
 * test_board_io.cpp -- svc::io, the board's hardwired discrete lines.
 *
 * WRITTEN FROM THE DECOMPILE, NOT FROM board_io.cpp. Every expectation below
 * was read out of reference/decompile/app/gpio.c (gpio_static_init_a @0x08008458,
 * gpio_static_init_b @0x080084be, gpio_write_status_lines @0x080084f6,
 * gpio_can_xcvr_standby_pb12 @0x08007430, gpio_can_xcvr_standby_on_pb12
 * @0x08007440) and reference/decompile/app/misc.c (update_path_exti_setup @0x08003222), and
 * the port masks/ports are restated here as raw literals so this file is a
 * cross-check of hal_gpio.h rather than a tautology against it.
 *
 * WHY THIS MATTERS IN THE CAR
 *   - PB6 is the REVERSE / back-up line and PC15 is the ILLUMINATION line, and
 *     both are read directly by the head unit. Crossing them is a visible
 *     fault: the camera comes up when the headlights do.
 *   - configurePb8Wake() arms the ONLY wake source the box has before it goes
 *     into STOP. If any of its four steps is wrong or out of order, the box
 *     sleeps and never comes back without a power cycle. That is the test to
 *     be paranoid about.
 * ==========================================================================*/

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "board_io.hpp"
#include "fake_hal.hpp"
#include "ram_map.hpp"

namespace {

/* ---- the board, restated from the decompile ----------------------------- *
 * Port tags are the stable values tests/host/fake_hal.cpp records for the
 * GPIO_Module pointers; the pin masks are the literals the stock image loads. */
constexpr std::uint32_t kGpiob = 0xB0U;
constexpr std::uint32_t kGpioc = 0xC0U;

constexpr std::uint32_t kPb2  = 0x0004U;  /* static input                     */
constexpr std::uint32_t kPb3  = 0x0008U;  /* static input (sleep path only)   */
constexpr std::uint32_t kPb4  = 0x0010U;  /* status bit 3                     */
constexpr std::uint32_t kPb5  = 0x0020U;  /* status bit 0                     */
constexpr std::uint32_t kPb6  = 0x0040U;  /* status bit 2 = REVERSE           */
constexpr std::uint32_t kPb8  = 0x0100U;  /* CAN_RX / EXTI8 wake              */
constexpr std::uint32_t kPb12 = 0x1000U;  /* CAN transceiver standby          */
constexpr std::uint32_t kPc15 = 0x8000U;  /* status bit 1 = ILLUMINATION      */

/* GPIO_MODE_* values, from the pool literals in the decompile. */
constexpr std::uint32_t kModeInput    = 0x00000000U;
constexpr std::uint32_t kModeOutputPp = 0x00000001U;
/* 0x10210000 from the pool at 0x0800339C: input + EXTI + IT + falling edge. */
constexpr std::uint32_t kModeInputExtiItFalling = 0x10210000U;

/* EXTI/AFIO/NVIC arguments, from update_path_exti_setup's stack blocks. */
constexpr std::uint32_t kExtiLine8      = 0x00000100U;
constexpr std::uint32_t kAfioPortB      = 1U;
constexpr std::uint32_t kExtiModeIt     = 0x00U;  /* EXTI->IMASK  offset      */
constexpr std::uint32_t kExtiTrigFall   = 0x0CU;  /* EXTI->FT_CFG offset      */
constexpr std::uint32_t kPriorityGroup  = 0x500U;
constexpr std::uint32_t kExti9_5Irqn    = 23U;    /* 0x17, the N32 number     */

/* ---- helpers over the call log ------------------------------------------ */

/* Position of the nth (0-based) call to `name` in the global call log, or -1.
 * Used to assert ORDER across different HAL functions. */
int indexOf(const char* name, std::size_t n = 0U) noexcept
{
    std::size_t seen = 0U;
    for (std::size_t i = 0U; i < fake::callCount(); ++i) {
        if (std::strcmp(fake::calls()[i].name, name) == 0) {
            if (seen == n) { return static_cast<int>(i); }
            ++seen;
        }
    }
    return -1;
}

/* The value of the last gpio_write_pin to (port, pin), or -1 if the pin was
 * never written at all. The "never written" case is an assertion in its own
 * right: the sleep path must leave PB5 alone. */
int lastLevelOn(std::uint32_t port, std::uint32_t pin) noexcept
{
    int level = -1;
    for (std::size_t i = 0U; i < fake::countOf("gpio_write_pin"); ++i) {
        const fake::Call* c = fake::nthCall("gpio_write_pin", i);
        if (c != nullptr && c->a0 == port && c->a1 == pin) {
            level = static_cast<int>(c->a2);
        }
    }
    return level;
}

/* The mode gpio_init_pin was last asked to put (port, pin) into, or -1. */
int lastModeOn(std::uint32_t port, std::uint32_t pin) noexcept
{
    int mode = -1;
    for (std::size_t i = 0U; i < fake::countOf("gpio_init_pin"); ++i) {
        const fake::Call* c = fake::nthCall("gpio_init_pin", i);
        if (c != nullptr && c->a0 == port && c->a1 == pin) {
            mode = static_cast<int>(c->a2);
        }
    }
    return mode;
}

/* Drive the four status bits from a raw nibble and fan it out. */
void fanOut(std::uint8_t nibble) noexcept
{
    ram::app().vehicle.at(ram::VehicleState::kFlags) = nibble;
    svc::io::writeStatusLines();
}

}  // namespace

/* ===========================================================================
 * writeStatusLines -- 0x080084F6
 *
 * The stock body is four gpio_write_pin calls, in this order:
 *     PB6  <- (b & 7) >> 2      PB5  <- b & 1
 *     PB4  <- (b & 0xf) >> 3    PC15 <- (b & 3) >> 1
 * ========================================================================= */

TEST(board_io_status_fanout_all_bits_set)
{
    fake::reset();

    fanOut(0x0FU);

    CHECK_EQ(fake::countOf("gpio_write_pin"), 4U);
    CHECK_EQ(lastLevelOn(kGpiob, kPb5),  1);
    CHECK_EQ(lastLevelOn(kGpioc, kPc15), 1);
    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  1);
    CHECK_EQ(lastLevelOn(kGpiob, kPb4),  1);
}

TEST(board_io_status_fanout_all_bits_clear)
{
    fake::reset();

    fanOut(0x00U);

    CHECK_EQ(fake::countOf("gpio_write_pin"), 4U);
    CHECK_EQ(lastLevelOn(kGpiob, kPb5),  0);
    CHECK_EQ(lastLevelOn(kGpioc, kPc15), 0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb4),  0);
}

/* Each bit on its own, so a mask-off-by-one in the extraction shows up as a
 * neighbouring pin moving. This is the test that catches "(b & 7) >> 2"
 * mistranscribed as "(b & 3) >> 2" and friends. */
TEST(board_io_status_fanout_bit0_drives_only_pb5)
{
    fake::reset();
    fanOut(ram::VehicleState::kFlagAccOn);

    CHECK_EQ(lastLevelOn(kGpiob, kPb5),  1);
    CHECK_EQ(lastLevelOn(kGpioc, kPc15), 0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb4),  0);
}

TEST(board_io_status_fanout_bit1_drives_only_pc15)
{
    fake::reset();
    fanOut(ram::VehicleState::kFlagIllumOn);

    CHECK_EQ(lastLevelOn(kGpioc, kPc15), 1);
    CHECK_EQ(lastLevelOn(kGpiob, kPb5),  0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb4),  0);
}

TEST(board_io_status_fanout_bit2_drives_only_pb6)
{
    fake::reset();
    fanOut(ram::VehicleState::kFlagReverse);

    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  1);
    CHECK_EQ(lastLevelOn(kGpiob, kPb5),  0);
    CHECK_EQ(lastLevelOn(kGpioc, kPc15), 0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb4),  0);
}

TEST(board_io_status_fanout_bit3_drives_only_pb4)
{
    fake::reset();
    fanOut(ram::VehicleState::kFlagSpare3);

    CHECK_EQ(lastLevelOn(kGpiob, kPb4),  1);
    CHECK_EQ(lastLevelOn(kGpiob, kPb5),  0);
    CHECK_EQ(lastLevelOn(kGpioc, kPc15), 0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  0);
}

/* THE CROSSED-WIRE TEST. Reverse and illumination are the two lines the head
 * unit acts on visibly; set them one at a time through the NAMED accessors and
 * insist on the exact pin AND port. PC15 is on a different port from PB6, so a
 * swap here is also a port error, which is why both halves are asserted. */
TEST(board_io_reverse_is_pb6_and_illum_is_pc15_not_the_other_way_round)
{
    fake::reset();

    ram::app().vehicle.setFlag(ram::VehicleState::kFlagReverse, true);
    svc::io::writeStatusLines();
    CHECK(ram::app().vehicle.reverseOn());
    CHECK_FALSE(ram::app().vehicle.illumOn());
    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  1);   /* REVERSE -> PB6  */
    CHECK_EQ(lastLevelOn(kGpioc, kPc15), 0);   /* ILLUM stays off */

    fake::reset();

    ram::app().vehicle.setFlag(ram::VehicleState::kFlagIllumOn, true);
    svc::io::writeStatusLines();
    CHECK(ram::app().vehicle.illumOn());
    CHECK_FALSE(ram::app().vehicle.reverseOn());
    CHECK_EQ(lastLevelOn(kGpioc, kPc15), 1);   /* ILLUM -> PC15   */
    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  0);   /* REVERSE off     */
}

/* The upper nibble belongs to other consumers (kFlagIgnRaw, kFlagAutobaud) and
 * must not reach a pin: the stock extractions all mask first. */
TEST(board_io_status_fanout_ignores_the_high_nibble)
{
    fake::reset();

    fanOut(0xF0U);

    CHECK_EQ(fake::countOf("gpio_write_pin"), 4U);
    CHECK_EQ(lastLevelOn(kGpiob, kPb5),  0);
    CHECK_EQ(lastLevelOn(kGpioc, kPc15), 0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb4),  0);
}

/* Stock call order is PB6, PB5, PB4, PC15 -- kept in case anything downstream
 * is edge-sensitive. */
TEST(board_io_status_fanout_keeps_the_stock_call_order)
{
    fake::reset();

    fanOut(0x0FU);

    const fake::Call* c0 = fake::nthCall("gpio_write_pin", 0U);
    const fake::Call* c1 = fake::nthCall("gpio_write_pin", 1U);
    const fake::Call* c2 = fake::nthCall("gpio_write_pin", 2U);
    const fake::Call* c3 = fake::nthCall("gpio_write_pin", 3U);

    CHECK(c0 != nullptr && c1 != nullptr && c2 != nullptr && c3 != nullptr);
    if (c0 == nullptr || c1 == nullptr || c2 == nullptr || c3 == nullptr) { return; }

    CHECK_EQ(c0->a0, kGpiob);  CHECK_EQ(c0->a1, kPb6);
    CHECK_EQ(c1->a0, kGpiob);  CHECK_EQ(c1->a1, kPb5);
    CHECK_EQ(c2->a0, kGpiob);  CHECK_EQ(c2->a1, kPb4);
    CHECK_EQ(c3->a0, kGpioc);  CHECK_EQ(c3->a1, kPc15);
}

/* The fan-out is pure output: it must never reconfigure a pin. */
TEST(board_io_status_fanout_configures_nothing)
{
    fake::reset();
    fanOut(0x05U);
    CHECK_EQ(fake::countOf("gpio_init_pin"), 0U);
}

/* ===========================================================================
 * setCanTransceiverStandby -- 0x08007430 / 0x08007440
 *
 * HIGH = standby (transceiver asleep), LOW = normal mode. Getting this
 * backwards leaves the transceiver asleep on a live bus: no CAN at all.
 * ========================================================================= */

TEST(board_io_can_transceiver_standby_on_drives_pb12_high)
{
    fake::reset();

    svc::io::setCanTransceiverStandby(true);

    CHECK_EQ(fake::countOf("gpio_write_pin"), 1U);
    const fake::Call* c = fake::lastCall("gpio_write_pin");
    CHECK(c != nullptr);
    if (c == nullptr) { return; }
    CHECK_EQ(c->a0, kGpiob);
    CHECK_EQ(c->a1, kPb12);
    CHECK_EQ(c->a2, 1U);
}

TEST(board_io_can_transceiver_normal_mode_drives_pb12_low)
{
    fake::reset();

    svc::io::setCanTransceiverStandby(false);

    CHECK_EQ(fake::countOf("gpio_write_pin"), 1U);
    const fake::Call* c = fake::lastCall("gpio_write_pin");
    CHECK(c != nullptr);
    if (c == nullptr) { return; }
    CHECK_EQ(c->a0, kGpiob);
    CHECK_EQ(c->a1, kPb12);
    CHECK_EQ(c->a2, 0U);
}

/* It touches PB12 and nothing else -- in particular not a status line. */
TEST(board_io_can_transceiver_standby_touches_only_pb12)
{
    fake::reset();

    svc::io::setCanTransceiverStandby(true);
    svc::io::setCanTransceiverStandby(false);

    CHECK_EQ(fake::countOf("gpio_write_pin"), 2U);
    CHECK_EQ(fake::countOf("gpio_init_pin"), 0U);
    CHECK_EQ(lastLevelOn(kGpiob, kPb5),  -1);
    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  -1);
    CHECK_EQ(lastLevelOn(kGpiob, kPb4),  -1);
    CHECK_EQ(lastLevelOn(kGpioc, kPc15), -1);
}

/* ===========================================================================
 * statusLinesInit -- 0x08008458
 *
 *   gpio_write_pin(B,0x20,0) (C,0x8000,0) (B,0x40,0) (B,0x10,0)
 *   gpio_init_pin (B,0x20,1) (C,0x8000,1) (B,0x40,1) (B,0x10,1)
 *   gpio_init_pin (B,0x04,0)
 *   *p_gpio_initialized = 0xff
 * ========================================================================= */

TEST(board_io_status_lines_init_configures_four_outputs_and_pb2_input)
{
    fake::reset();

    svc::io::statusLinesInit();

    CHECK_EQ(fake::countOf("gpio_write_pin"), 4U);
    CHECK_EQ(fake::countOf("gpio_init_pin"), 5U);

    CHECK_EQ(lastModeOn(kGpiob, kPb5),  static_cast<int>(kModeOutputPp));
    CHECK_EQ(lastModeOn(kGpioc, kPc15), static_cast<int>(kModeOutputPp));
    CHECK_EQ(lastModeOn(kGpiob, kPb6),  static_cast<int>(kModeOutputPp));
    CHECK_EQ(lastModeOn(kGpiob, kPb4),  static_cast<int>(kModeOutputPp));

    /* PB2 is an INPUT, despite what docs/hiworld-gpio-io.md claims. */
    CHECK_EQ(lastModeOn(kGpiob, kPb2), static_cast<int>(kModeInput));

    /* PB3 belongs to the SHUTDOWN block only; init must not touch it. */
    CHECK_EQ(lastModeOn(kGpiob, kPb3), -1);
}

TEST(board_io_status_lines_init_sets_gpio_initialised_flag)
{
    fake::reset();
    CHECK_EQ(ram::app().gpioInitialised, 0U);

    svc::io::statusLinesInit();

    /* The stock store is 0xFF, not 1. */
    CHECK_EQ(ram::app().gpioInitialised, 0xFFU);
}

/* Every line is driven low BEFORE its pin becomes an output, so none of them
 * glitches high while the direction changes. */
TEST(board_io_status_lines_init_drives_low_before_configuring)
{
    fake::reset();

    svc::io::statusLinesInit();

    CHECK_EQ(lastLevelOn(kGpiob, kPb5),  0);
    CHECK_EQ(lastLevelOn(kGpioc, kPc15), 0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb4),  0);

    const int lastWrite = indexOf("gpio_write_pin", 3U);
    const int firstInit = indexOf("gpio_init_pin", 0U);
    CHECK(lastWrite >= 0 && firstInit >= 0);
    CHECK(lastWrite < firstInit);
}

/* ===========================================================================
 * statusLinesShutdown -- 0x080084BE
 *
 *   gpio_write_pin(C,0x8000,0) (B,0x40,0) (B,0x10,0)
 *   gpio_init_pin (B,0x08,0)   (B,0x04,0)
 *
 * Three asymmetries against the init block, all deliberate:
 *   PB5 untouched; the three outputs are NOT re-made outputs; PB3 and PB2 ARE
 *   re-made inputs; and the gpioInitialised flag is left alone.
 * ========================================================================= */

TEST(board_io_shutdown_drives_pc15_pb6_pb4_low)
{
    fake::reset();

    svc::io::statusLinesShutdown();

    CHECK_EQ(fake::countOf("gpio_write_pin"), 3U);
    CHECK_EQ(lastLevelOn(kGpioc, kPc15), 0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb4),  0);
}

TEST(board_io_shutdown_does_not_touch_pb5)
{
    fake::reset();

    svc::io::statusLinesShutdown();

    /* -1 means "never written". PB5 is absent from 0x080084BE entirely: it is
     * neither driven low nor reconfigured. */
    CHECK_EQ(lastLevelOn(kGpiob, kPb5), -1);
    CHECK_EQ(lastModeOn(kGpiob, kPb5),  -1);
}

TEST(board_io_shutdown_reconfigures_pb3_and_pb2_as_inputs)
{
    fake::reset();

    svc::io::statusLinesShutdown();

    CHECK_EQ(fake::countOf("gpio_init_pin"), 2U);

    /* Stock order: PB3 first, then PB2. */
    const fake::Call* i0 = fake::nthCall("gpio_init_pin", 0U);
    const fake::Call* i1 = fake::nthCall("gpio_init_pin", 1U);
    CHECK(i0 != nullptr && i1 != nullptr);
    if (i0 == nullptr || i1 == nullptr) { return; }

    CHECK_EQ(i0->a0, kGpiob);  CHECK_EQ(i0->a1, kPb3);  CHECK_EQ(i0->a2, kModeInput);
    CHECK_EQ(i1->a0, kGpiob);  CHECK_EQ(i1->a1, kPb2);  CHECK_EQ(i1->a2, kModeInput);
}

/* The three status pins are driven low but NOT reconfigured -- they already
 * are outputs. Only PB3/PB2 appear in the init calls. */
TEST(board_io_shutdown_does_not_reconfigure_the_status_outputs)
{
    fake::reset();

    svc::io::statusLinesShutdown();

    CHECK_EQ(lastModeOn(kGpioc, kPc15), -1);
    CHECK_EQ(lastModeOn(kGpiob, kPb6),  -1);
    CHECK_EQ(lastModeOn(kGpiob, kPb4),  -1);
}

TEST(board_io_shutdown_leaves_gpio_initialised_alone)
{
    fake::reset();

    svc::io::statusLinesInit();
    CHECK_EQ(ram::app().gpioInitialised, 0xFFU);

    svc::io::statusLinesShutdown();
    CHECK_EQ(ram::app().gpioInitialised, 0xFFU);

    /* And it does not set it either, when it was clear to begin with. */
    fake::reset();
    svc::io::statusLinesShutdown();
    CHECK_EQ(ram::app().gpioInitialised, 0U);
}

/* ===========================================================================
 * configurePb8Wake -- 0x08003222   *** THE WAKE PATH ***
 *
 * Stock body, in order:
 *      nvic_set_priority_group(0x500)
 *      nvic_init({irq 0x17, preempt 0, sub 0, enable 1})
 *      gpio_init_pin(GPIOB, 0x100, 0x10210000)
 *      afio_pin_config(1, 8)                       -- port B drives line 8
 *      exti_init({line 0x100, mode 0x00, trig 0x0C, enable 1})
 *
 * Every one of these five is load-bearing. Missing the AFIO step leaves line 8
 * pointed at port A; missing the falling-edge trigger leaves the line armed for
 * nothing; a wrong IRQ number leaves the NVIC entry disabled. Any of them and
 * the box never wakes.
 * ========================================================================= */

TEST(board_io_pb8_wake_configures_the_pin_as_exti_falling_input)
{
    fake::reset();

    svc::io::configurePb8Wake();

    CHECK_EQ(fake::countOf("gpio_init_pin"), 1U);
    const fake::Call* c = fake::lastCall("gpio_init_pin");
    CHECK(c != nullptr);
    if (c == nullptr) { return; }
    CHECK_EQ(c->a0, kGpiob);
    CHECK_EQ(c->a1, kPb8);
    CHECK_EQ(c->a2, kModeInputExtiItFalling);
}

TEST(board_io_pb8_wake_points_exti_line8_at_port_b)
{
    fake::reset();

    svc::io::configurePb8Wake();

    CHECK_EQ(fake::countOf("exti_set_line_source"), 1U);
    const fake::Call* c = fake::lastCall("exti_set_line_source");
    CHECK(c != nullptr);
    if (c == nullptr) { return; }
    CHECK_EQ(c->a0, kAfioPortB);
    /* a LINE NUMBER, not a mask -- passing 0x100 here would index off the end
     * of AFIO->EXTI_CFG[]. */
    CHECK_EQ(c->a1, 8U);
}

TEST(board_io_pb8_wake_arms_exti_line8_interrupt_on_the_falling_edge)
{
    fake::reset();

    svc::io::configurePb8Wake();

    CHECK_EQ(fake::countOf("exti_line_init"), 1U);
    const fake::Call* c = fake::lastCall("exti_line_init");
    CHECK(c != nullptr);
    if (c == nullptr) { return; }
    CHECK_EQ(c->a0, kExtiLine8);          /* a MASK here, unlike the source   */
    CHECK_EQ(c->a1, kExtiModeIt);         /* IMASK, i.e. interrupt not event  */
    /* fake_hal packs (trig_ofs << 8) | enable into a2. */
    CHECK_EQ(c->a2, (kExtiTrigFall << 8) | 1U);
}

TEST(board_io_pb8_wake_enables_exti9_5_in_the_nvic)
{
    fake::reset();

    svc::io::configurePb8Wake();

    CHECK_EQ(fake::countOf("nvic_set_priority_grouping"), 1U);
    const fake::Call* g = fake::lastCall("nvic_set_priority_grouping");
    CHECK(g != nullptr);
    if (g != nullptr) { CHECK_EQ(g->a0, kPriorityGroup); }

    CHECK_EQ(fake::countOf("nvic_init"), 1U);
    const fake::Call* n = fake::lastCall("nvic_init");
    CHECK(n != nullptr);
    if (n == nullptr) { return; }
    /* EXTI9_5_IRQn = 23 on the N32L40x, NOT an STM32F1 number. */
    CHECK_EQ(n->a0, kExti9_5Irqn);
    /* fake_hal packs (preempt << 8) | sub into a1. */
    CHECK_EQ(n->a1, 0U);
    CHECK_EQ(n->a2, 1U);   /* enable */
}

/* The stock ORDER: NVIC first, then the pin, then the line source, then the
 * line itself. Reproduced because the image does it that way, and because
 * arming the NVIC before the line can latch an edge is the safe direction. */
TEST(board_io_pb8_wake_issues_the_five_steps_in_stock_order)
{
    fake::reset();

    svc::io::configurePb8Wake();

    const int grp  = indexOf("nvic_set_priority_grouping");
    const int nvic = indexOf("nvic_init");
    const int pin  = indexOf("gpio_init_pin");
    const int src  = indexOf("exti_set_line_source");
    const int line = indexOf("exti_line_init");

    CHECK(grp >= 0);
    CHECK(nvic >= 0);
    CHECK(pin >= 0);
    CHECK(src >= 0);
    CHECK(line >= 0);

    CHECK(grp  < nvic);
    CHECK(nvic < pin);
    CHECK(pin  < src);
    CHECK(src  < line);
}

/* Arming the wake source must not disturb a single discrete line: the status
 * outputs have already been parked by statusLinesShutdown() and PB12 is
 * already in standby by the time this runs on the sleep path. */
TEST(board_io_pb8_wake_drives_no_output_pin)
{
    fake::reset();

    svc::io::configurePb8Wake();

    CHECK_EQ(fake::countOf("gpio_write_pin"), 0U);
    CHECK_EQ(fake::countOf("gpio_set_bit"), 0U);
    CHECK_EQ(fake::countOf("gpio_clear_bit"), 0U);
}

/* The whole sleep sequence as the shutdown path runs it, end to end: park the
 * transceiver, park the lines, then arm the one wake source. Proves the three
 * compose without stepping on each other -- PB5 still untouched, PB8 armed. */
TEST(board_io_shutdown_sequence_parks_the_lines_and_arms_the_wake)
{
    fake::reset();

    svc::io::setCanTransceiverStandby(true);
    svc::io::statusLinesShutdown();
    svc::io::configurePb8Wake();

    CHECK_EQ(lastLevelOn(kGpiob, kPb12), 1);   /* transceiver asleep */
    CHECK_EQ(lastLevelOn(kGpioc, kPc15), 0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb6),  0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb4),  0);
    CHECK_EQ(lastLevelOn(kGpiob, kPb5), -1);   /* never touched      */

    CHECK_EQ(lastModeOn(kGpiob, kPb8), static_cast<int>(kModeInputExtiItFalling));
    CHECK_EQ(fake::countOf("exti_line_init"), 1U);
}
