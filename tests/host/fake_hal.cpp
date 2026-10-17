/* ============================================================================
 * fake_hal.cpp -- host implementations of every app/hal/ entry point.
 *
 * These definitions replace the real HAL objects at link time in the host test
 * build. They are `extern "C"` for the same reason the real ones are: the
 * services layer calls them across the C ABI.
 *
 * See fake_hal.hpp for what tests can inspect.
 * ==========================================================================*/

#include "fake_hal.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ram_map.hpp"

extern "C" {
#include "hal_adc.h"
#include "hal_can.h"
#include "hal_exti.h"
#include "hal_flash.h"
#include "hal_gpio.h"
#include "hal_nvic.h"
#include "hal_pwr.h"
#include "hal_rcc.h"
#include "hal_tim.h"
#include "hal_usart.h"
#include "hal_wwdg.h"
}

/* The application's state object is NOT redefined here. It is an ordinary .bss
 * object defined once in app/services/app_state.cpp, and the host build links
 * that same translation unit -- so the tests exercise exactly the definition
 * the firmware uses, with no host-only substitute to drift out of sync. */

namespace fake {
namespace {

Call        g_calls[kMaxCalls];
std::size_t g_callCount = 0U;

Returns g_returns{};

std::uint8_t g_paramPage[PARAM_PAGE_SIZE];
std::size_t  g_eraseCount = 0U;

CanFrame    g_canFrames[kMaxFrames];
std::size_t g_canFrameCount = 0U;

std::uint8_t g_usartTx[256];
std::size_t  g_usartTxCount = 0U;

void record(const char* name, std::uint32_t a0 = 0U, std::uint32_t a1 = 0U,
            std::uint32_t a2 = 0U) noexcept
{
    if (g_callCount < kMaxCalls) {
        g_calls[g_callCount] = Call{name, a0, a1, a2};
        ++g_callCount;
    }
}

/* Pointers are recorded as a small stable tag rather than a truncated address,
 * so a test can say "it drove GPIOB" portably. */
std::uint32_t tag(const void* p) noexcept
{
    if (p == static_cast<const void*>(GPIOA)) { return 0xA0U; }
    if (p == static_cast<const void*>(GPIOB)) { return 0xB0U; }
    if (p == static_cast<const void*>(GPIOC)) { return 0xC0U; }
    if (p == static_cast<const void*>(GPIOD)) { return 0xD0U; }
    if (p == static_cast<const void*>(USART1)) { return 0x10U; }
    if (p == static_cast<const void*>(USART2)) { return 0x20U; }
    if (p == static_cast<const void*>(USART3)) { return 0x30U; }
    if (p == static_cast<const void*>(TIM1)) { return 0x01U; }
    if (p == static_cast<const void*>(TIM2)) { return 0x02U; }
    if (p == static_cast<const void*>(CAN)) { return 0xCAU; }
    return 0xFFU;
}

}  // namespace

const Call* calls() noexcept { return g_calls; }
std::size_t callCount() noexcept { return g_callCount; }

std::size_t countOf(const char* name) noexcept
{
    std::size_t n = 0U;
    for (std::size_t i = 0U; i < g_callCount; ++i) {
        if (std::strcmp(g_calls[i].name, name) == 0) { ++n; }
    }
    return n;
}

const Call* nthCall(const char* name, std::size_t n) noexcept
{
    std::size_t seen = 0U;
    for (std::size_t i = 0U; i < g_callCount; ++i) {
        if (std::strcmp(g_calls[i].name, name) == 0) {
            if (seen == n) { return &g_calls[i]; }
            ++seen;
        }
    }
    return nullptr;
}

const Call* lastCall(const char* name) noexcept
{
    const Call* found = nullptr;
    for (std::size_t i = 0U; i < g_callCount; ++i) {
        if (std::strcmp(g_calls[i].name, name) == 0) { found = &g_calls[i]; }
    }
    return found;
}

Returns& returns() noexcept { return g_returns; }

std::uint8_t* paramPage() noexcept { return g_paramPage; }

std::uint32_t paramWord(std::size_t index) noexcept
{
    std::uint32_t v = 0U;
    std::memcpy(&v, &g_paramPage[index * 4U], sizeof v);
    return v;
}

void setParamWord(std::size_t index, std::uint32_t value) noexcept
{
    std::memcpy(&g_paramPage[index * 4U], &value, sizeof value);
}

std::size_t eraseCount() noexcept { return g_eraseCount; }

const CanFrame* canFrames() noexcept { return g_canFrames; }
std::size_t canFrameCount() noexcept { return g_canFrameCount; }
const CanFrame* lastCanFrame() noexcept
{
    return (g_canFrameCount == 0U) ? nullptr : &g_canFrames[g_canFrameCount - 1U];
}

const std::uint8_t* usartTxBytes() noexcept { return g_usartTx; }
std::size_t usartTxCount() noexcept { return g_usartTxCount; }

void reset() noexcept
{
    g_callCount = 0U;
    g_eraseCount = 0U;
    g_canFrameCount = 0U;
    g_usartTxCount = 0U;

    std::memset(g_calls, 0, sizeof g_calls);
    std::memset(g_canFrames, 0, sizeof g_canFrames);
    std::memset(g_usartTx, 0, sizeof g_usartTx);
    std::memset(const_cast<ram::AppRam*>(&ram::app()), 0, sizeof(ram::AppRam));
    std::memset(const_cast<std::uint8_t*>(&ram::g_versionString[0]), 0,
                sizeof ram::g_versionString);

    /* Erased flash reads as all ones. */
    std::memset(g_paramPage, 0xFF, sizeof g_paramPage);

    g_returns = Returns{};
    g_returns.flashStatus    = FLASH_STS_COMPL;
    g_returns.hsiReady       = 1;
    g_returns.canTxResult    = 0U;  /* mailbox 0 accepted it */
    g_returns.canInitResult  = 1;
    g_returns.canSleepResult = 1;
    g_returns.canWakeupResult = 1;
}

}  // namespace fake

/* ===========================================================================
 * The HAL surface.
 * ========================================================================= */
extern "C" {

/* ---- rcc ---------------------------------------------------------------- */
void rcc_apb2_periph_clock(uint32_t periph, int enable)
{
    fake::record("rcc_apb2_periph_clock", periph, static_cast<uint32_t>(enable));
}
void rcc_apb1_periph_clock(uint32_t periph, int enable)
{
    fake::record("rcc_apb1_periph_clock", periph, static_cast<uint32_t>(enable));
}
int rcc_get_flag_status(uint32_t flag)
{
    fake::record("rcc_get_flag_status", flag);
    return fake::returns().rccFlag;
}
void rcc_clear_reset_flags(void) { fake::record("rcc_clear_reset_flags"); }
void rcc_set_bus_prescalers_div4(void) { fake::record("rcc_set_bus_prescalers_div4"); }
int rcc_enable_hsi_for_flash(void)
{
    fake::record("rcc_enable_hsi_for_flash");
    return fake::returns().hsiReady;
}

/* ---- nvic --------------------------------------------------------------- */
void nvic_set_priority_grouping(uint32_t group)
{
    fake::record("nvic_set_priority_grouping", group);
}
void nvic_init(uint8_t irqn, uint8_t preempt, uint8_t sub, int enable)
{
    fake::record("nvic_init", irqn, (static_cast<uint32_t>(preempt) << 8) | sub,
                 static_cast<uint32_t>(enable));
}

/* ---- wwdg --------------------------------------------------------------- */
void wwdg_kick(void) { fake::record("wwdg_kick"); }
void wwdg_set_prescaler(uint32_t wdgtb) { fake::record("wwdg_set_prescaler", wdgtb); }
void wwdg_set_window(uint32_t window) { fake::record("wwdg_set_window", window); }
void wwdg_enable_set_counter(uint32_t counter)
{
    fake::record("wwdg_enable_set_counter", counter);
}
void wwdg_init(void) { fake::record("wwdg_init"); }

/* The real one never returns. The fake records and returns.
 *
 * Escaping instead -- so a test could exercise a reset path -- was tried twice
 * and does not work here. Throwing dies because the path crosses functions that
 * are __attribute__((noreturn)), [[noreturn]] and noexcept: the landing pads
 * get optimised away or std::terminate fires. longjmp dies because on this
 * Windows/MSVC target it unwinds through the intervening frames and corrupts
 * the process, which silently loses the whole stdout buffer -- the symptom is a
 * test runner that prints nothing and exits 0.
 *
 * So: returning is deliberate. See the contract note in fake_hal.hpp for what
 * is and is not testable as a result. */
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Winvalid-noreturn"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wsuggest-attribute=noreturn"
#endif
void wwdg_force_reset(void) { fake::record("wwdg_force_reset"); }
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

/* ---- gpio --------------------------------------------------------------- */
void gpio_apply_pin_config(GPIO_Module* port, const gpio_pin_cfg_t* cfg)
{
    fake::record("gpio_apply_pin_config", fake::tag(port),
                 cfg != nullptr ? cfg->pin : 0U, cfg != nullptr ? cfg->mode : 0U);
}
void gpio_init_pin(GPIO_Module* port, uint32_t pin_mask, uint32_t mode)
{
    fake::record("gpio_init_pin", fake::tag(port), pin_mask, mode);
}
void gpio_set_bit(GPIO_Module* port, uint32_t pin_mask)
{
    fake::record("gpio_set_bit", fake::tag(port), pin_mask);
}
void gpio_clear_bit(GPIO_Module* port, uint32_t pin_mask)
{
    fake::record("gpio_clear_bit", fake::tag(port), pin_mask);
}
void gpio_write_pin(GPIO_Module* port, uint32_t pin_mask, int value)
{
    fake::record("gpio_write_pin", fake::tag(port), pin_mask,
                 static_cast<uint32_t>(value));
}
int gpio_read_pin(GPIO_Module* port, uint32_t pin_mask)
{
    fake::record("gpio_read_pin", fake::tag(port), pin_mask);
    return fake::returns().gpioPinLevel;
}

/* ---- exti --------------------------------------------------------------- */
void exti_set_line_source(uint32_t port_sel, uint32_t line_num)
{
    fake::record("exti_set_line_source", port_sel, line_num);
}
void exti_line_init(uint32_t line_mask, uint32_t mode_ofs, uint32_t trig_ofs,
                    int enable)
{
    fake::record("exti_line_init", line_mask, mode_ofs,
                 (trig_ofs << 8) | static_cast<uint32_t>(enable));
}
int exti_get_it_status(uint32_t line_mask)
{
    fake::record("exti_get_it_status", line_mask);
    return fake::returns().extiPending;
}
void exti_clear_it_pending(uint32_t line_mask)
{
    fake::record("exti_clear_it_pending", line_mask);
}

/* ---- tim ---------------------------------------------------------------- */
void tim_timebase_init(TIM_Module* tim, const tim_timebase_init_t* init)
{
    fake::record("tim_timebase_init", fake::tag(tim),
                 init != nullptr ? init->Prescaler : 0U,
                 init != nullptr ? init->Period : 0U);
}
void tim_prescaler_config(TIM_Module* tim, uint16_t prescaler, uint16_t reload_mode)
{
    fake::record("tim_prescaler_config", fake::tag(tim), prescaler, reload_mode);
}
void tim_it_config(TIM_Module* tim, uint16_t it, int enable)
{
    fake::record("tim_it_config", fake::tag(tim), it, static_cast<uint32_t>(enable));
}
void tim_cmd(TIM_Module* tim, int enable)
{
    fake::record("tim_cmd", fake::tag(tim), static_cast<uint32_t>(enable));
}
int tim_get_it_status(TIM_Module* tim, uint16_t it)
{
    fake::record("tim_get_it_status", fake::tag(tim), it);
    return fake::returns().timItStatus;
}
void tim_clear_it_pending(TIM_Module* tim, uint16_t it)
{
    fake::record("tim_clear_it_pending", fake::tag(tim), it);
}

/* ---- can ---------------------------------------------------------------- */
void can_deinit(CAN_Module* canx) { fake::record("can_deinit", fake::tag(canx)); }
void can_struct_init_defaults(can_init_t* init)
{
    fake::record("can_struct_init_defaults");
    if (init != nullptr) { std::memset(init, 0, sizeof *init); }
}
int can_init_regs(CAN_Module* canx, const can_init_t* init)
{
    fake::record("can_init_regs", fake::tag(canx),
                 init != nullptr ? init->prescaler : 0U,
                 init != nullptr ? init->mode : 0U);
    return fake::returns().canInitResult;
}
void can_filter_init(const can_filter_init_t* filt)
{
    fake::record("can_filter_init", filt != nullptr ? filt->number : 0U,
                 filt != nullptr ? filt->activation : 0U);
}
void can_it_config(CAN_Module* canx, uint32_t it, int enable)
{
    fake::record("can_it_config", fake::tag(canx), it, static_cast<uint32_t>(enable));
}
uint8_t can_transmit(CAN_Module* canx, can_tx_msg_t* msg)
{
    fake::record("can_transmit", fake::tag(canx), msg != nullptr ? msg->std_id : 0U,
                 msg != nullptr ? msg->dlc : 0U);
    if (msg != nullptr && fake::g_canFrameCount < fake::kMaxFrames) {
        fake::CanFrame& f = fake::g_canFrames[fake::g_canFrameCount];
        f.id  = (msg->ide == CAN_ID_STANDARD) ? msg->std_id : msg->ext_id;
        f.ide = msg->ide;
        f.dlc = msg->dlc;
        std::memcpy(f.data, msg->data, sizeof f.data);
        ++fake::g_canFrameCount;
    }
    return fake::returns().canTxResult;
}
void can_receive_fifo(CAN_Module* canx, int fifo, can_rx_msg_t* msg)
{
    fake::record("can_receive_fifo", fake::tag(canx), static_cast<uint32_t>(fifo));
    if (msg != nullptr) { std::memset(msg, 0, sizeof *msg); }
}
int can_sleep(CAN_Module* canx)
{
    fake::record("can_sleep", fake::tag(canx));
    return fake::returns().canSleepResult;
}
int can_wakeup(CAN_Module* canx)
{
    fake::record("can_wakeup", fake::tag(canx));
    return fake::returns().canWakeupResult;
}

/* ---- usart -------------------------------------------------------------- */
int usart_get_it_status(USART_Module* usart, uint32_t it)
{
    fake::record("usart_get_it_status", fake::tag(usart), it);
    return fake::returns().usartItStatus;
}
void usart_it_config(USART_Module* usart, uint32_t it, int enable)
{
    fake::record("usart_it_config", fake::tag(usart), it, static_cast<uint32_t>(enable));
}
void usart_cmd_enable(USART_Module* usart, int enable)
{
    fake::record("usart_cmd_enable", fake::tag(usart), static_cast<uint32_t>(enable));
}
void usart_send_data(USART_Module* usart, uint8_t data)
{
    fake::record("usart_send_data", fake::tag(usart), data);
    if (fake::g_usartTxCount < sizeof fake::g_usartTx) {
        fake::g_usartTx[fake::g_usartTxCount] = data;
        ++fake::g_usartTxCount;
    }
}
uint8_t usart_receive_data(USART_Module* usart)
{
    fake::record("usart_receive_data", fake::tag(usart));
    return fake::returns().usartRxByte;
}
void usart_configure(USART_Module* usart, uint32_t pclk_hz, uint32_t baud)
{
    fake::record("usart_configure", fake::tag(usart), pclk_hz, baud);
}
void usart_tx_drain(USART_Module* usart, uint32_t spins)
{
    /* Nothing is in flight in a host build, so the real function's spin has
     * nothing to wait for. Record the call and the bound: a test can assert
     * the drain happened before a reset, and that the bound was not left at
     * something that would outlive the watchdog on target. */
    fake::record("usart_tx_drain", fake::tag(usart), spins);
}
void usart_tx_start(USART_Module* usart)
{
    fake::record("usart_tx_start", fake::tag(usart));
}
void usart_disable(USART_Module* usart)
{
    fake::record("usart_disable", fake::tag(usart));
}

/* ---- flash: modelled, not stubbed --------------------------------------- */
void flash_set_latency(uint32_t latency) { fake::record("flash_set_latency", latency); }
void flash_set_prefetch(uint32_t prefetch) { fake::record("flash_set_prefetch", prefetch); }
void flash_unlock(void) { fake::record("flash_unlock"); }
void flash_lock(void) { fake::record("flash_lock"); }
int flash_get_status(void)
{
    fake::record("flash_get_status");
    return fake::returns().flashStatus;
}
int flash_wait_for_last_operation(uint32_t timeout)
{
    fake::record("flash_wait_for_last_operation", timeout);
    return fake::returns().flashStatus;
}
void flash_clear_status_flags(uint32_t flags)
{
    fake::record("flash_clear_status_flags", flags);
}

int flash_erase_page(uint32_t page_addr)
{
    fake::record("flash_erase_page", page_addr);
    /* The guard is the real HAL's job and is tested there; the fake reproduces
     * it so a services-layer bug cannot quietly "erase" out of range here. */
    if (page_addr != PARAM_PAGE_BASE) { return FLASH_STS_REFUSED; }
    std::memset(fake::g_paramPage, 0xFF, PARAM_PAGE_SIZE);
    ++fake::g_eraseCount;
    return fake::returns().flashStatus;
}

int flash_program_word(uint32_t addr, uint32_t data)
{
    fake::record("flash_program_word", addr, data);
    if ((addr & 3U) != 0U) { return FLASH_STS_ERR_ALIGN; }
    if (addr < PARAM_PAGE_BASE || addr > (PARAM_PAGE_LAST - 3U)) {
        return FLASH_STS_REFUSED;
    }
    std::memcpy(&fake::g_paramPage[addr - PARAM_PAGE_BASE], &data, sizeof data);
    return fake::returns().flashStatus;
}

uint32_t flash_read_word(uint32_t addr)
{
    if (addr < PARAM_PAGE_BASE || addr > (PARAM_PAGE_LAST - 3U)) { return 0xFFFFFFFFU; }
    uint32_t v = 0U;
    std::memcpy(&v, &fake::g_paramPage[addr - PARAM_PAGE_BASE], sizeof v);
    return v;
}

/* ---- adc / pwr ---------------------------------------------------------- */
void adc_enable(uint32_t base, int enable)
{
    fake::record("adc_enable", base, static_cast<uint32_t>(enable));
}
void adc_dma_cmd(uint32_t base, int enable)
{
    fake::record("adc_dma_cmd", base, static_cast<uint32_t>(enable));
}
void adc_sw_start_regular(uint32_t base, int enable)
{
    fake::record("adc_sw_start_regular", base, static_cast<uint32_t>(enable));
}
void pwr_enter_stop(int use_wfi, uint32_t cfg)
{
    fake::record("pwr_enter_stop", static_cast<uint32_t>(use_wfi), cfg);
}

}  // extern "C"
