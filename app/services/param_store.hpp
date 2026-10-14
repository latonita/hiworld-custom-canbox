/* ============================================================================
 * param_store.hpp -- the parameter page (emulated EEPROM) service.
 *
 * Ported from the three stock routines that own the page:
 *     0x08002dbc  param_page_load_0801F800    flash -> RAM + shadow
 *     0x08002d46  param_page_save_0801F800    RAM -> flash, only when dirty
 *     0x08007b6c  can_config_load_defaults    validate / default / latch
 *                 (its frame-priming half is now logic::primeTxTemplates)
 * (reference/decompile/app/sys.c and reference/decompile/app/can.c,). The FLASH
 * primitives those routines call moved to app/hal/hal_flash.c; what is left --
 * the RAM state, the dirty check and the derived blocks -- is this unit.
 *
 * ---------------------------------------------------------------------------
 * SAFETY -- READ THIS BEFORE CHANGING ANYTHING HERE
 *
 * The N32L406CBL7 has 128 KB of flash in 2 KB pages:
 *     0x08000000..0x08001FFF   IAP BOOTLOADER. Write protected. It is the ONLY
 *                              way to reflash this box. Erasing it bricks the
 *                              device permanently -- there is no other path in.
 *     0x08002000..0x0801F7FF   the app image we are running from.
 *     0x0801F800..0x0801FFFF   the parameter page. The ONLY page this firmware
 *                              may ever erase or program at runtime.
 *
 * save() computes PARAM_PAGE_BASE + 4*i for i < 32, i.e. 0x0801F800..0x0801F87C,
 * which is inside the page by construction. hal_flash's bounds guard refuses
 * anything else with FLASH_STS_REFUSED and never touches the controller, so
 * there is no way to reach the erase/program hardware with a bad address
 * through this module. Keep it that way.
 * ---------------------------------------------------------------------------
 *
 * PAGE LAYOUT
 *
 * 32 config bytes live in the first 128 bytes of page 0x0801F800, ONE BYTE PER
 * 32-BIT WORD (byte i at 0x0801F800 + 4*i, the other three bytes left erased).
 * That is not an encoding choice we are free to revisit: it is what is already
 * programmed into every box in the field, and the dumped page
 * (reference/parampage_0x0801F800_2k.bin) reads
 *     AA 00 00 00  01 00 00 00  FF FF FF FF ...
 *
 * They are mirrored to RAM:
 *     ram::app().param[]        0x200011d0  the live copy the firmware uses
 *     ram::app().paramShadow[]  0x20001220  what is believed to be in flash;
 *                                           save() compares the two and only
 *                                           burns a page when they differ.
 *
 * Byte meanings, from can_config_load_defaults @0x08007b6c cross-checked
 * against the real dumped page:
 *
 *   [0x00]  validity magic, must be 0xAA
 *   [0x01]  CAN profile / bit rate selector: 1 = 125 kbit/s profile 1,
 *           0 = 83.333 kbit/s profile 0. Clamped to <= 1. Toggled by the
 *           phase-1 autobaud retry, which is why this page gets rewritten.
 *   [0x02..0x0F]  unused, erased (0xFF in the real dump)
 *   [0x10]  stored amplifier volume (0..38)
 *   [0x11]  stored BAL    } all 1..19, centre 10; defaulted to 10 ("\n")
 *   [0x12]  stored FAD    }
 *   [0x13]  stored BASS   }
 *   [0x14]  stored MID    }
 *   [0x15]  stored TRE    }
 *   [0x16]  0 -- the NUL of the 6-byte strncpy that defaults [0x11..0x15]
 *   [0x17..0x1F]  unused, erased
 * ==========================================================================*/

#ifndef PARAM_STORE_HPP
#define PARAM_STORE_HPP

#include <cstddef>
#include <cstdint>

#include "ram_map.hpp"

namespace svc {
namespace params {

/* ---- byte indices into the 32-byte block (the old PARAM_* macros) -------- */

/* Number of config bytes. One per 32-bit flash word, so they span the first
 * 128 bytes of the page. Tied to the RAM mirror so the two cannot drift. */
inline constexpr std::size_t kCount = 32U;
static_assert(sizeof(ram::AppRam::param) == kCount,
              "the RAM mirror must hold exactly the stock 32 config bytes");
static_assert(sizeof(ram::AppRam::paramShadow) == kCount,
              "the shadow must be the same size as the live block");

inline constexpr std::size_t  kMagic      = 0x00U;  /* must read 0xAA         */
inline constexpr std::size_t  kProfile    = 0x01U;  /* CAN profile, 0 or 1    */
inline constexpr std::size_t  kVolume     = 0x10U;
inline constexpr std::size_t  kToneFirst  = 0x11U;  /* BAL FAD BASS MID TRE   */
inline constexpr std::size_t  kToneCount  = 5U;
inline constexpr std::uint8_t kMagicValue = 0xAAU;

/* The highest legal value of [kProfile]; anything above it means a half-written
 * page and is clamped by validateDefaults(). Profile 0 is THIS box (Jeep WK,
 * 83.333 kbit/s); profile 1 is the FCA 125 kbit/s variant. */
inline constexpr std::uint8_t kProfileMax = 1U;

/* The stock default for a virgin or corrupt page: profile 1, volume 0, and the
 * five tone bytes at the centre of their 1..19 range. The decompile shows them
 * written as strncpy(param + 0x11, "\n\n\n\n\n", 6) -- five 0x0A plus the NUL
 * that lands in [0x16], which is why that byte is part of the default set. */
inline constexpr std::uint8_t kDefaultProfile = 1U;
inline constexpr std::uint8_t kDefaultVolume  = 0U;
inline constexpr std::uint8_t kDefaultTone    = 10U;  /* '\n' */

/* ---- the service -------------------------------------------------------- */

/* --- 0x08002dbc --- Boot-time load: zero the RAM copy, then read the 32 bytes
 * out of the page (one byte per word) into param[] and mirror them into
 * paramShadow[], so the first save() sees a clean (non-dirty) block. */
void load() noexcept;

/* --- 0x08002d46 --- THE ONLY LIVE FLASH WRITER.
 *
 * Does nothing unless param[] differs from paramShadow[]: this runs every
 * 1024 ms from the main loop, so the dirty check is what stops the page being
 * burnt to death.
 *
 * It MUST bring HSI up (rcc_enable_hsi_for_flash) before flash_unlock(): the
 * flash controller on this part is fed from HSI, and skipping that call gives a
 * silent erase/program failure or a hang in the busy wait. */
/* Masks interrupts across the whole erase+program sequence.
 *
 * That mask is load-bearing and it is NOT free: a page erase plus 32 word
 * programs runs for tens of milliseconds, during which the NVIC can hold only
 * ONE pending TIM2 interrupt. The rest are discarded outright, so the
 * millisecond counter and the soft clock lose that time; roughly 115 bytes of
 * head-unit UART traffic and any CAN frames past the 3-deep FIFO are dropped
 * too. The UART parser resynchronises on its own, but a 0xD2 or 0xFC frame
 * arriving mid-save is simply gone.
 *
 * All of that is stock behaviour and the unavoidable price of not being
 * interrupted mid-erase. Do not "fix" the resulting clock drift by shortening
 * the mask. */
void save() noexcept;

/* --- 0x08007b6c --- Validate the loaded block, apply defaults to a virgin or
 * corrupt page, then latch the result into ram::AppRam::profileActive.
 *
 * This used to ALSO rebuild every outbound CAN frame template, as the stock
 * function does. That half is logic::primeTxTemplates() now -- it is vehicle
 * protocol, not parameter handling, and hiding it behind this name concealed
 * that calling validateDefaults() a third time would stamp over the live
 * cluster-text frames. Call primeTxTemplates() immediately after this; the
 * order matters, because everything it writes reads the latch set here.
 *
 * Note it does NOT write flash: the defaults land in param[] only, and the next
 * save() sees the block as dirty and burns them in. */
void validateDefaults() noexcept;

/* The live profile byte, param[0x01]. This is the authority the CAN bit-rate
 * selection reads (can_set_baud_by_profile @0x08007b2c); ram::app().profileActive
 * is a latched copy validateDefaults() publishes, not the other way round. */
[[nodiscard]] std::uint8_t profile() noexcept;

/* Store the profile byte verbatim.
 *
 * NO RANGE CHECK, matching stock: the autobaud retry in main_loop writes
 * `param[1] = (param[1] != 1)`, a plain byte store with no guard, and
 * can_set_baud_by_profile normalises anything that is not 1 to 0 on its own.
 * The clamp lives in validateDefaults(), exactly where the stock image put it.
 * Recording the hazard here rather than silently patching it -- a caller that
 * passes 7 will get 7 into the page until the next validateDefaults(). */
void setProfile(std::uint8_t value) noexcept;

}  // namespace params
}  // namespace svc

#endif /* PARAM_STORE_HPP */
