/* ============================================================================
 * scheduler.hpp -- the 1 ms time base, the divider chain and the cadence
 * flags, namespace svc::sched.
 *
 * Ported from reference/decompile/app/tim.c (0x080027F8, 0x08002F72, 0x08007A30) and
 * reference/decompile/app/sys.c (0x0800853C), Ghidra output, and
 * the retired app/port/core.c. Every entry point carries the ORIGINAL flash address of the
 * stock function it reproduces.
 *
 * ---------------------------------------------------------------------------
 * THE TICK MATHS (load-bearing -- do not "tidy" these numbers)
 *
 *     SYSCLK = 64 MHz, APB1 prescaler = /4  ->  PCLK1 = 16 MHz
 *     TIM2 kernel clock = 2 x PCLK1 = 32 MHz
 *         (the x2 multiplier applies whenever PPRE1 != /1; with PPRE1 = /1 the
 *          timer clock would be PCLK1 and the tick would come out at 500 Hz)
 *     PSC = 7 -> divide by 8 -> 4 MHz;  AR = 4000  ->  1000 Hz  ->  1 ms
 *
 * Confirmed independently of the clock-tree reconstruction: this interrupt
 * drives the soft real-time clock, whose seconds only advance at the right
 * speed if the interrupt fires at exactly 1 kHz. The numbers themselves live
 * in cfg:: (app_config.hpp).
 *
 * ---------------------------------------------------------------------------
 * THE DIVIDER CHAIN, once per update event (onTimerInterrupt)
 *
 *     every   1 ms : onMillisecond()                -- counter + cadence flags
 *     every  10 ms : svc::timers::tickShortTimers(), tickCycleCounter()
 *     every 100 ms : svc::clock::tick(), svc::can::timersTick()
 *     every   1 s  : nothing but the divider wrap (ram::AppRam::tickDiv1s is
 *                    readable by other subsystems as a phase marker)
 *
 * The three dividers are ram::AppRam::tickDiv10ms / tickDiv100ms / tickDiv1s
 * at 0x20001000..0x20001002 -- shared firmware state, not local statics.
 *
 * This is the one place the layering inverts (ARCHITECTURE.md section 5): the
 * 1 ms interrupt drives work owned by several services, so the scheduler fans
 * OUT to them rather than any service reaching sideways into another.
 *
 * ---------------------------------------------------------------------------
 * THE CADENCE FLAGS (onMillisecond), all one-shot, all cleared by the consumer
 *
 *   field                       set when                  period
 *   sched.flagFast      0x110C  (c & 0x003) == 0x001         4 ms
 *   sched.flagMed       0x110D  (c & 0x007) == 0x003         8 ms
 *   sched.flagDiv16     0x110E  (c & 0x00F) == 0x007        16 ms
 *   sched.flagDiv32     0x110F  (c & 0x01F) == 0x00F        32 ms
 *   sched.flagDiv64     0x1110  (c & 0x03F) == 0x03F        64 ms
 *   sched.flagDiv128    0x1111  (c & 0x07F) == 0x07F       128 ms
 *   sched.flagDiv256    0x1112  (c & 0x0FF) == 0x0FF       256 ms
 *   sched.flagDiv512    0x1113  (c & 0x1FF) == 0x1FF       512 ms
 *   sched.flagSlow      0x1114  (c & 0x3FF) == 0x3FF      1024 ms
 *   sched.flagAutobaud  0x1115  (c & 0x7FF) == 0x7FF      2048 ms
 *
 * c is the POST-increment value of ram::AppRam::sched.counter, which is a
 * genuine 32-bit free-running millisecond counter. An 8-bit counter would make
 * the 0x3FF and 0x7FF tests unreachable, so the 1024 ms parameter save and the
 * 2048 ms autobaud retry would never fire and phase 1 would hang forever.
 *
 * The phases are deliberately staggered (1, 3, 7, 15, ...) so the 4/8/16/32 ms
 * jobs never land on the same millisecond, which is what keeps the worst-case
 * main_loop pass inside the ~65.5 ms watchdog. From 64 ms down they all
 * trigger on an all-ones mask instead and so DO coincide -- that is what the
 * stock image does, not a transcription slip.
 * ==========================================================================*/

#ifndef SCHEDULER_HPP
#define SCHEDULER_HPP

#include <cstdint>

namespace svc {
namespace sched {

/* ---------------------------------------------------------------------------
 * NVIC settings for the time base. These are tim2_init's own arguments and
 * belong to nothing else, so they live here rather than in app_config.hpp.
 *
 *   0x300 selects PRIGROUP = 3 (4 bits of preempt priority, 0 of subpriority),
 *   which is written by tim2_init itself in the stock image, not by a separate
 *   step. TIM2 is IRQ 28 on the N32L40x -- see app/include/n32l406_hw.h; the
 *   number differs from STM32's and the stock code passes 0x1C.
 * ------------------------------------------------------------------------- */
inline constexpr std::uint32_t kNvicPriorityGroup = 0x300U;
inline constexpr std::uint8_t  kTim2Preempt       = 4U;
inline constexpr std::uint8_t  kTim2Subpriority   = 0U;

/* --- 0x080027F8 tim2_init ---
 * Priority grouping, NVIC, APB1 clock gate, time base, prescaler reload,
 * update interrupt, counter enable -- in exactly that order.
 *
 * Note the ordering: the interrupt is unmasked in the NVIC BEFORE the timer is
 * started, exactly as the stock code does. That is safe because main_loop runs
 * this whole init sequence with PRIMASK set (CPSID i) and only enables
 * interrupts once every subsystem is up. */
void start() noexcept;

/* --- 0x08002F72 TIM2_IRQHandler (body) ---
 * The update-flag check, the acknowledge and the divider chain. The
 * `extern "C"` veneer that the bootloader's vector table dispatches into lives
 * in app/logic/interrupts.cpp; this is the plain C++ body it calls.
 *
 * Structure note: the stock code uses three independent `if`s, NOT `else if`,
 * so the 100 ms and 1 s branches are evaluated in the same interrupt that
 * incremented their counter. That is why the cascade settles in a single pass
 * and the 10 ms work and the 100 ms work always land on the same tick. Kept
 * bit-for-bit compatible. */
void onTimerInterrupt() noexcept;

/* --- 0x08007A30 tim2_sched_rate_flags ---
 * Bump the free-running millisecond counter and raise the ten cadence flags.
 * Never clears them; the consumer does. */
void onMillisecond() noexcept;

/* --- 0x0800853C sys_cycle_counter_tick ---
 * The other 10 ms hook: a rolling counter over 1 .. (cycleCtrMax - 1), used by
 * the UART TX slot rotation. It wraps back to 1, not 0, and the comparison is
 * against the POST-increment value. */
void tickCycleCounter() noexcept;

/* ---------------------------------------------------------------------------
 * Flag consumers. main_loop's steady superloop tests each flag and clears it
 * in the same breath, so these three are test-and-clear.
 * ------------------------------------------------------------------------- */
[[nodiscard]] bool takeFast() noexcept;   /*    4 ms: status, CAN TX, SWC scan */
[[nodiscard]] bool takeMed() noexcept;    /*    8 ms: decode, UART, GPIO       */
[[nodiscard]] bool takeSlow() noexcept;   /* 1024 ms: parameter page save      */

/* The 2048 ms autobaud flag is deliberately NOT test-and-clear.
 *
 * The stock phase-1 loop clears it in the retry branch (autobaudTries < 8) but
 * deliberately NOT in the give-up branch, so the shutdown path runs on every
 * pass of the loop until the WFI inside it takes effect. Folding these two
 * into one accessor would change behaviour: the box would sleep only on every
 * other 2048 ms window at best, and the loop would spin with nothing to do in
 * between.
 *
 * autobaudDue() reads the flag exactly as the stock `== 1` comparison does and
 * leaves it alone. */
[[nodiscard]] bool autobaudDue() noexcept;
void clearAutobaud() noexcept;

}  // namespace sched
}  // namespace svc

#endif /* SCHEDULER_HPP */
