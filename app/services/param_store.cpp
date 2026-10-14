/* ============================================================================
 * param_store.cpp -- see param_store.hpp.
 *
 * Ported from reference/decompile/app/sys.c and reference/decompile/app/can.c (Ghidra output) via
 * the retired app/port/param.c. The FLASH primitives those routines called now live in
 * app/hal/hal_flash.c; this unit owns only the RAM state, the dirty check and
 * the derived blocks.
 *
 * Reads of the page go through flash_read_word() rather than dereferencing
 * 0x0801F800. The stock code dereferences it directly, but routing the one read
 * path through the HAL is what makes this module testable on a host where that
 * address is not mapped -- and on target it compiles to a single ldr.
 * ==========================================================================*/

#include "param_store.hpp"

#include "bytes.hpp"
#include "ram_map.hpp"

extern "C" {
#include "hal_cpu.h"
#include "hal_flash.h"
#include "hal_rcc.h"
#include "hal_wwdg.h"
}

namespace svc {
namespace params {
namespace {

/* ---------------------------------------------------------------------------
 * Critical section.
 *
 * The stock save path brackets the whole erase+program sequence with
 * CPSID i / CPSIE i. That is load-bearing: a flash erase stalls the bus for far
 * longer than a CAN or UART interrupt can tolerate, and the ISRs are fetched
 * from the very flash bank that is mid-erase.
 *
 * The instructions themselves belong to the HAL (app/hal/hal_cpu.h), which also
 * carries the host-test no-op form -- `cpsid i` does not assemble for x86.
 * ------------------------------------------------------------------------- */
inline void irqDisable() noexcept { cpu_irq_disable(); }
inline void irqEnable() noexcept { cpu_irq_enable(); }

/* Flash address of config byte `i`: one byte per 32-bit word.
 *
 * i < kCount == 32, so this spans 0x0801F800..0x0801F87C and can never leave
 * the parameter page. hal_flash's bounds guard is belt and braces. */
inline std::uint32_t wordAddr(std::size_t i) noexcept
{
    return PARAM_PAGE_BASE + (static_cast<std::uint32_t>(i) * 4U);
}

}  // namespace

/* === 0x08002dbc (param_page_load_0801F800) ===============================
 * Zero the RAM copy first, then read the page into it and mirror into the
 * shadow. The zeroing pass is a separate loop in the stock image and is kept:
 * it is what guarantees a defined block even if the read path ever changes.
 * Mirroring into the shadow as we go is what makes the first save() a no-op on
 * an unchanged box. */
void load() noexcept
{
    volatile std::uint8_t* const live   = ram::app().param;
    volatile std::uint8_t* const shadow = ram::app().paramShadow;

    for (std::size_t i = 0U; i < kCount; ++i) {
        live[i] = 0U;
    }

    for (std::size_t i = 0U; i < kCount; ++i) {
        /* Byte i is the LOW byte of word i; the other three stay erased. */
        const std::uint32_t word = flash_read_word(wordAddr(i));
        live[i]   = static_cast<std::uint8_t>(word & 0xFFU);
        shadow[i] = live[i];
    }
}

/* === 0x08002d46 (param_page_save_0801F800) ===============================
 * THE ONLY LIVE FLASH WRITER.
 *
 * Stock sequence, reproduced exactly:
 *     disableIRQinterrupts(); wwdg_kick();
 *     RCC_EnableHSI_forFlash(); FLASH_Unlock(); wwdg_kick();
 *     FLASH_ErasePage(0x0801F800); wwdg_kick();
 *     for i in 0..31: shadow[i] = live[i];
 *                     FLASH_ProgramWord(0x0801F800 + 4*i, live[i]);
 *                     wwdg_kick();
 *     FLASH_Lock(); enableIRQinterrupts();
 *
 * Two details that look optional and are not:
 *
 *  - rcc_enable_hsi_for_flash() comes BEFORE flash_unlock(). The flash
 *    controller is clocked from HSI on this part; with HSI off the erase and
 *    every program silently fail, or the busy wait never completes.
 *
 *  - the watchdog is kicked before every step. A page erase takes far longer
 *    than the ~65.5 ms WWDG window, and interrupts are masked, so nothing else
 *    can kick it for us.
 *
 * Status handling: stock IGNORES every return value here -- no retry, no error
 * latch, no early exit. A refused or timed-out erase simply leaves the page as
 * it was while the shadow is updated regardless, so the firmware stops trying
 * until the live block changes again. That is reproduced rather than
 * "improved": a retry loop inside a masked-interrupt critical section is how
 * you turn a failed save into a watchdog reset. */
void save() noexcept
{
    volatile std::uint8_t* const live   = ram::app().param;
    volatile std::uint8_t* const shadow = ram::app().paramShadow;

    /* The dirty check. This runs on the 1024 ms tick; an unconditional
     * erase+program every second would wear the page out in weeks. */
    if (!bytes::differ(live, shadow, static_cast<std::uint8_t>(kCount))) {
        return;
    }

    irqDisable();
    wwdg_kick();

    (void)rcc_enable_hsi_for_flash();
    flash_unlock();
    wwdg_kick();

    (void)flash_erase_page(PARAM_PAGE_BASE);
    wwdg_kick();

    for (std::size_t i = 0U; i < kCount; ++i) {
        const std::uint8_t v = live[i];
        shadow[i] = v;
        (void)flash_program_word(wordAddr(i), static_cast<std::uint32_t>(v));
        wwdg_kick();
    }

    flash_lock();
    irqEnable();
}

/* === 0x08007b6c (can_config_load_defaults) ===============================
 * Validate, default, then prime every RAM block derived from the page.
 *
 * It writes no flash: the defaults land in param[] only, and the next save()
 * sees the block as dirty and burns them in. */
void validateDefaults() noexcept
{
    ram::AppRam&                 r    = ram::app();
    volatile std::uint8_t* const live = r.param;

    /* Virgin or corrupt page -> stock defaults. (-0x56 in the decompile is
     * 0xAA read as a signed char.) */
    if (live[kMagic] != kMagicValue) {
        live[kMagic]   = kMagicValue;
        live[kProfile] = kDefaultProfile;   /* 125k / profile 1 */
        live[kVolume]  = kDefaultVolume;
        /* stock: strncpy(param + 0x11, "\n\n\n\n\n", 6) -- five 10s plus the
         * NUL that lands in [0x16]. 10 is the centre of the 1..19 tone range,
         * and the trailing NUL is part of the stored block, not a string
         * artefact: it is programmed into the page like every other byte. */
        for (std::size_t i = 0U; i < kToneCount; ++i) {
            live[kToneFirst + i] = kDefaultTone;
        }
        live[kToneFirst + kToneCount] = 0U;
    }

    /* Only profiles 0 and 1 exist; anything else means a half-written page. */
    if (live[kProfile] > kProfileMax) {
        live[kProfile] = kDefaultProfile;
    }

    const std::uint8_t active = live[kProfile];
    r.profileActive = active;

}

std::uint8_t profile() noexcept
{
    return ram::app().param[kProfile];
}

void setProfile(std::uint8_t value) noexcept
{
    /* Verbatim store -- see the hazard note in param_store.hpp. */
    ram::app().param[kProfile] = value;
}

}  // namespace params
}  // namespace svc
