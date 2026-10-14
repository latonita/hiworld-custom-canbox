/* ============================================================================
 * fake_hal.hpp -- the host stand-in for app/hal/.
 *
 * Every HAL entry point is replaced by a recording no-op so the services and
 * logic layers can be exercised on a PC. Tests assert on what the code under
 * test ASKED the hardware to do -- which pin it drove, which frame it handed to
 * the CAN mailbox, whether it erased the parameter page -- which is the part
 * that matters and the part a bench cannot easily show you.
 *
 * Two pieces of real behaviour are modelled rather than stubbed, because the
 * code under test depends on them:
 *   - the parameter page (2 KB of simulated flash, with erase/program/read),
 *   - the CAN transmit mailbox (captures the frame so decoders can be checked).
 * ==========================================================================*/

#ifndef FAKE_HAL_HPP
#define FAKE_HAL_HPP

#include <cstddef>
#include <cstdint>

namespace fake {

/* ---- call log ----------------------------------------------------------- */

struct Call {
    const char*   name;
    std::uint32_t a0;
    std::uint32_t a1;
    std::uint32_t a2;
};

inline constexpr std::size_t kMaxCalls = 512U;

/* Every recorded call, oldest first. Saturates at kMaxCalls rather than
 * growing: a test that makes 512 HAL calls has lost the plot anyway. */
const Call*  calls() noexcept;
std::size_t  callCount() noexcept;

/* How many times `name` was called. */
std::size_t countOf(const char* name) noexcept;

/* The nth (0-based) recorded call to `name`, or nullptr. */
const Call*  nthCall(const char* name, std::size_t n) noexcept;

/* The most recent call to `name`, or nullptr. */
const Call*  lastCall(const char* name) noexcept;

/* ---- controllable return values ----------------------------------------- */

struct Returns {
    int           flashStatus;       /* flash_get_status / wait_for_last_op   */
    int           hsiReady;          /* rcc_enable_hsi_for_flash              */
    int           rccFlag;           /* rcc_get_flag_status                   */
    int           gpioPinLevel;      /* gpio_read_pin                         */
    int           extiPending;       /* exti_get_it_status                    */
    int           usartItStatus;     /* usart_get_it_status                   */
    std::uint8_t  usartRxByte;       /* usart_receive_data                    */
    int           timItStatus;       /* tim_get_it_status                     */
    std::uint8_t  canTxResult;       /* can_transmit (0..2 = mailbox, 4 = none)*/
    int           canInitResult;     /* can_init_regs                         */
    int           canSleepResult;    /* can_sleep                             */
    int           canWakeupResult;   /* can_wakeup                            */
};
Returns& returns() noexcept;

/* ---- simulated parameter page (0x0801F800, 2 KB) ------------------------ */

std::uint8_t* paramPage() noexcept;          /* raw bytes, 2048 of them      */
std::uint32_t paramWord(std::size_t index) noexcept;
void          setParamWord(std::size_t index, std::uint32_t value) noexcept;
std::size_t   eraseCount() noexcept;         /* how many page erases happened */

/* ---- captured CAN transmissions ----------------------------------------- */

struct CanFrame {
    std::uint32_t id;
    std::uint8_t  ide;
    std::uint8_t  dlc;
    std::uint8_t  data[8];
};

inline constexpr std::size_t kMaxFrames = 64U;

const CanFrame* canFrames() noexcept;
std::size_t     canFrameCount() noexcept;
const CanFrame* lastCanFrame() noexcept;

/* ---- captured USART output ---------------------------------------------- */

const std::uint8_t* usartTxBytes() noexcept;
std::size_t         usartTxCount() noexcept;

/* ---- modelling a reset -------------------------------------------------- *
 * wwdg_force_reset() never returns on target: it spins on the window watchdog
 * until the part resets. The fake RECORDS the call and returns, which is the
 * only workable choice here -- see the long note in fake_hal.cpp.
 *
 * What that buys and what it costs:
 *   - shutdownDeepSleep() ends WITH the reset, so it is fully testable: the
 *     fake returns and the function simply ends. All of its ordering is
 *     assertable, which is the point, because that path is the one whose
 *     failure mode is a box that never wakes.
 *   - fatal_error_reset() is `for (;;) { ...; wwdg_force_reset(); }`, so
 *     calling it from a test would spin forever. Do not. Assert on the fault
 *     CODES and on the call, never by invoking it.
 *                                                                          */

/* ---- reset everything, including ram::app() ----------------------------- *
 * Call this at the top of every test. The RAM block is zeroed, which is what
 * the stock scatter table does at boot.                                     */
void reset() noexcept;

}  // namespace fake

#endif /* FAKE_HAL_HPP */
