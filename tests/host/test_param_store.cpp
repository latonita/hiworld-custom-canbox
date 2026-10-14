/* ============================================================================
 * test_param_store.cpp -- the parameter page service.
 *
 * Written against the BEHAVIOURAL SPEC in the decompile, not against
 * param_store.cpp:
 *     reference/decompile/app/sys.c  param_page_save_0801F800 @0x08002d46
 *                    param_page_load_0801F800 @0x08002dbc
 *     reference/decompile/app/can.c  can_config_load_defaults @0x08007b6c
 * plus the real dumped page, reference/parampage_0x0801F800_2k.bin, which is
 * reproduced byte for byte in kRealPage below. Every constant here is restated
 * independently of param_store.hpp so these cases are a cross-check and not a
 * tautology.
 *
 * What is being protected:
 *   - the page at 0x0801F800 is the ONLY flash this firmware may erase, and it
 *     sits directly above the app image. Every erase/program address is
 *     asserted to be inside it.
 *   - the page is written on the 1024 ms tick. If the dirty check regresses the
 *     box erases a flash page every second and the parameter page dies.
 *   - save() must bring HSI up before unlocking the controller, or the erase
 *     silently fails / the busy wait hangs.
 * ==========================================================================*/

#include "test_framework.hpp"

#include "fake_hal.hpp"
#include "param_store.hpp"
#include "ram_map.hpp"

extern "C" {
#include "hal_flash.h"
}

namespace {

/* ---- the facts, restated independently ---------------------------------- */
constexpr std::uint32_t kPageBase = 0x0801F800U;
constexpr std::uint32_t kPageLast = 0x0801FFFFU;
constexpr std::size_t   kBytes    = 32U;       /* config bytes               */
constexpr std::size_t   kWords    = 32U;       /* one 32-bit word each       */

/* Byte indices, from the decompile. */
constexpr std::size_t kIdxMagic   = 0x00U;
constexpr std::size_t kIdxProfile = 0x01U;
constexpr std::size_t kIdxVolume  = 0x10U;
constexpr std::size_t kIdxTone0   = 0x11U;     /* BAL FAD BASS MID TRE       */
constexpr std::size_t kIdxToneNul = 0x16U;     /* the strncpy NUL            */

/* Vehicle-block offsets the stock code reached through g_vstate_mid
 * (= the vehicle block + 0x17). Restated as absolute SRAM addresses minus the
 * block base, exactly as globals.h had them. */

/* The first 32 config bytes of reference/parampage_0x0801F800_2k.bin, i.e. the
 * low byte of each of the first 32 words:
 *     aa 00 00 00  01 00 00 00  ff 00 00 00 ...
 *     [0x40] 10 00 00 00  0a .. 0a .. 0d .. 0a .. 0a ..  00 00 00 00
 * magic AA, profile 1, volume 0x10, tones 0A 0A 0D 0A 0A -- the documented
 * "0A 0A 0D 0A 0A tail" -- and the NUL at [0x16]. */
constexpr std::uint8_t kRealPage[kBytes] = {
    0xAAU, 0x01U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0x10U, 0x0AU, 0x0AU, 0x0DU, 0x0AU, 0x0AU, 0x00U, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
};

/* ---- helpers ------------------------------------------------------------ */

/* Program config byte `i` the way the stock image stores it: one byte per
 * 32-bit word, the other three bytes left at their erased value of 0x00 after
 * a program (see the real dump: "aa 00 00 00"). */
void pageByte(std::size_t i, std::uint8_t v) noexcept
{
    fake::setParamWord(i, v);
}

void writeRealPage() noexcept
{
    for (std::size_t i = 0U; i < kBytes; ++i) {
        pageByte(i, kRealPage[i]);
    }
}

std::uint8_t live(std::size_t i) noexcept { return ram::app().param[i]; }
std::uint8_t shadow(std::size_t i) noexcept { return ram::app().paramShadow[i]; }

/* Position of the nth call to `name` in the global call log, so tests can
 * assert ORDER across different HAL entry points. -1 if it never happened. */
long callOrder(const char* name, std::size_t n = 0U) noexcept
{
    const fake::Call* c = fake::nthCall(name, n);
    if (c == nullptr) {
        return -1;
    }
    return static_cast<long>(c - fake::calls());
}

}  // namespace

/* ===========================================================================
 * load() -- 0x08002dbc
 * ========================================================================= */

/* A virgin / fully erased page reads as all ones. The stock loader does not
 * sanity-check anything: 0xFF lands in the block verbatim and it is
 * validateDefaults() that notices the magic is wrong. */
TEST(param_store_load_from_an_erased_page_yields_all_ff)
{
    fake::reset();

    svc::params::load();

    for (std::size_t i = 0U; i < kBytes; ++i) {
        CHECK_EQ(live(i), 0xFF);
        CHECK_EQ(shadow(i), 0xFF);
    }
}

/* The real dumped page, byte for byte. This is the one test that proves the
 * whole on-flash encoding, because kRealPage is a transcription of hardware. */
TEST(param_store_load_reads_the_real_dumped_page)
{
    fake::reset();
    writeRealPage();

    svc::params::load();

    for (std::size_t i = 0U; i < kBytes; ++i) {
        CHECK_EQ(live(i), kRealPage[i]);
    }
    CHECK_EQ(live(kIdxMagic), 0xAA);
    CHECK_EQ(live(kIdxProfile), 1);
    CHECK_EQ(live(kIdxVolume), 0x10);
}

/* One byte per 32-bit WORD: byte i comes from the LOW byte of the word at
 * base + 4*i, and the upper three bytes are ignored. If this ever regressed to
 * a packed byte array the box would read its volume out of the magic. */
TEST(param_store_load_takes_the_low_byte_of_each_word)
{
    fake::reset();

    fake::setParamWord(0U, 0xDEADBEAAU);   /* -> 0xAA */
    fake::setParamWord(1U, 0xFFFFFF01U);   /* -> 0x01 */
    fake::setParamWord(2U, 0x12345678U);   /* -> 0x78 */
    fake::setParamWord(31U, 0x00000099U);  /* -> 0x99 */

    svc::params::load();

    CHECK_EQ(live(0U), 0xAA);
    CHECK_EQ(live(1U), 0x01);
    CHECK_EQ(live(2U), 0x78);
    CHECK_EQ(live(31U), 0x99);
}

/* The shadow is primed from the same read, which is what makes the first save()
 * after boot a no-op on a box whose settings have not changed. */
TEST(param_store_load_mirrors_the_block_into_the_shadow)
{
    fake::reset();
    writeRealPage();

    svc::params::load();

    for (std::size_t i = 0U; i < kBytes; ++i) {
        CHECK_EQ(shadow(i), live(i));
    }
}

/* load() is a pure read. Nothing about boot may touch the erase/program path. */
TEST(param_store_load_never_writes_flash)
{
    fake::reset();
    writeRealPage();

    svc::params::load();

    CHECK_EQ(fake::eraseCount(), 0U);
    CHECK_EQ(fake::countOf("flash_erase_page"), 0U);
    CHECK_EQ(fake::countOf("flash_program_word"), 0U);
    CHECK_EQ(fake::countOf("flash_unlock"), 0U);
}

/* ===========================================================================
 * validateDefaults() -- 0x08007b6c
 * ========================================================================= */

/* Magic != 0xAA (the decompile's `!= -0x56`) rewrites the whole default set:
 * magic, profile 1, volume 0, five tone bytes of 10, and the strncpy NUL at
 * [0x16]. Bytes outside that set are deliberately NOT touched -- which is why
 * the field page still reads 0xFF at [0x02..0x0F]. */
TEST(param_store_validate_defaults_rewrites_a_virgin_page)
{
    fake::reset();
    svc::params::load();           /* erased page -> magic is 0xFF */

    svc::params::validateDefaults();

    CHECK_EQ(live(kIdxMagic), 0xAA);
    CHECK_EQ(live(kIdxProfile), 1);
    CHECK_EQ(live(kIdxVolume), 0);
    for (std::size_t i = 0U; i < 5U; ++i) {
        CHECK_EQ(live(kIdxTone0 + i), 10);
    }
    CHECK_EQ(live(kIdxToneNul), 0);

    /* Untouched by the default set, still erased. */
    CHECK_EQ(live(0x02U), 0xFF);
    CHECK_EQ(live(0x0FU), 0xFF);
    CHECK_EQ(live(0x1FU), 0xFF);
}

/* Defaults land in RAM only. The next save() burns them in; validateDefaults()
 * itself must never reach the flash controller (it runs twice during boot). */
TEST(param_store_validate_defaults_never_writes_flash)
{
    fake::reset();
    svc::params::load();

    svc::params::validateDefaults();

    CHECK_EQ(fake::eraseCount(), 0U);
    CHECK_EQ(fake::countOf("flash_program_word"), 0U);
}

/* A block whose magic is right is left exactly as loaded -- including the
 * tone NUL byte, which is NOT re-zeroed, and a volume the user has changed. */
TEST(param_store_validate_defaults_leaves_a_valid_block_alone)
{
    fake::reset();
    writeRealPage();
    pageByte(kIdxProfile, 0U);      /* profile 0, the Jeep box              */
    pageByte(kIdxToneNul, 0x77U);   /* a non-zero tail, to prove no rewrite */
    svc::params::load();

    svc::params::validateDefaults();

    CHECK_EQ(live(kIdxMagic), 0xAA);
    CHECK_EQ(live(kIdxProfile), 0);
    CHECK_EQ(live(kIdxVolume), 0x10);
    CHECK_EQ(live(kIdxTone0 + 0U), 0x0A);
    CHECK_EQ(live(kIdxTone0 + 2U), 0x0D);   /* the "0D" of the tail quirk */
    CHECK_EQ(live(kIdxToneNul), 0x77);
}

/* Only profiles 0 and 1 exist. Anything else means a half-written page and is
 * clamped to 1; 0 and 1 survive untouched. */
TEST(param_store_validate_defaults_clamps_an_illegal_profile)
{
    fake::reset();
    writeRealPage();
    pageByte(kIdxProfile, 7U);
    svc::params::load();

    svc::params::validateDefaults();

    CHECK_EQ(live(kIdxProfile), 1);
    CHECK_EQ(ram::app().profileActive, 1);

    /* 0xFF -- what an erased profile byte on an otherwise valid page reads --
     * clamps the same way. */
    fake::reset();
    writeRealPage();
    pageByte(kIdxProfile, 0xFFU);
    svc::params::load();
    svc::params::validateDefaults();
    CHECK_EQ(live(kIdxProfile), 1);

    /* The two legal values are not disturbed. */
    for (int i = 0; i <= 1; ++i) {
        const std::uint8_t p = static_cast<std::uint8_t>(i);
        fake::reset();
        writeRealPage();
        pageByte(kIdxProfile, p);
        svc::params::load();
        svc::params::validateDefaults();
        CHECK_EQ(live(kIdxProfile), p);
        CHECK_EQ(ram::app().profileActive, p);
    }
}

/* profileActive @0x2000118f is a latched copy of param[1], published here. */
TEST(param_store_validate_defaults_publishes_the_active_profile)
{
    fake::reset();
    writeRealPage();
    pageByte(kIdxProfile, 0U);
    svc::params::load();

    svc::params::validateDefaults();

    CHECK_EQ(ram::app().profileActive, 0);
    CHECK_EQ(ram::app().profileActive, live(kIdxProfile));
}

/* ===========================================================================
 * save() -- 0x08002d46
 * ========================================================================= */

/* THE WEAR TEST. save() runs on the 1024 ms tick. With the shadow matching the
 * live block it must not reach the controller at all -- not an erase, not an
 * unlock, not even the HSI enable. */
TEST(param_store_save_does_nothing_when_the_shadow_matches)
{
    fake::reset();
    writeRealPage();
    svc::params::load();

    svc::params::save();

    CHECK_EQ(fake::eraseCount(), 0U);
    CHECK_EQ(fake::countOf("flash_erase_page"), 0U);
    CHECK_EQ(fake::countOf("flash_program_word"), 0U);
    CHECK_EQ(fake::countOf("flash_unlock"), 0U);
    CHECK_EQ(fake::countOf("flash_lock"), 0U);
    CHECK_EQ(fake::countOf("rcc_enable_hsi_for_flash"), 0U);
    CHECK_EQ(fake::countOf("wwdg_kick"), 0U);

    /* And it stays quiet on every subsequent tick. */
    svc::params::save();
    svc::params::save();
    CHECK_EQ(fake::eraseCount(), 0U);
}

/* A single changed byte makes the whole block dirty: one erase, all 32 words
 * reprogrammed, and the shadow brought up to date so the next tick is quiet. */
TEST(param_store_save_erases_once_and_programs_32_words_when_dirty)
{
    fake::reset();
    writeRealPage();
    svc::params::load();

    ram::app().param[kIdxVolume] = 0x26U;   /* the user turned it up */

    svc::params::save();

    CHECK_EQ(fake::eraseCount(), 1U);
    CHECK_EQ(fake::countOf("flash_erase_page"), 1U);
    CHECK_EQ(fake::countOf("flash_program_word"), kWords);
    CHECK_EQ(fake::countOf("flash_unlock"), 1U);
    CHECK_EQ(fake::countOf("flash_lock"), 1U);

    /* Shadow updated -> the next 1024 ms tick does nothing. */
    for (std::size_t i = 0U; i < kBytes; ++i) {
        CHECK_EQ(shadow(i), live(i));
    }
    svc::params::save();
    CHECK_EQ(fake::eraseCount(), 1U);
}

/* The on-flash encoding, round-tripped: one config byte per 32-bit word, the
 * upper three bytes programmed to zero, exactly like the real dump's
 * "aa 00 00 00  01 00 00 00". And a reload must see what was written. */
TEST(param_store_save_writes_one_byte_per_word)
{
    fake::reset();
    writeRealPage();
    svc::params::load();

    ram::app().param[kIdxVolume] = 0x26U;

    svc::params::save();

    CHECK_EQ(fake::paramWord(kIdxMagic), 0x000000AAU);
    CHECK_EQ(fake::paramWord(kIdxProfile), 0x00000001U);
    CHECK_EQ(fake::paramWord(kIdxVolume), 0x00000026U);
    CHECK_EQ(fake::paramWord(kIdxTone0 + 2U), 0x0000000DU);
    CHECK_EQ(fake::paramWord(kIdxToneNul), 0x00000000U);

    /* Words past the 32 config bytes are left erased by the program loop. */
    CHECK_EQ(fake::paramWord(kWords), 0xFFFFFFFFU);
    CHECK_EQ(fake::paramWord(kWords + 1U), 0xFFFFFFFFU);

    svc::params::load();
    CHECK_EQ(live(kIdxVolume), 0x26);
}

/* *** THE ONE THAT BITES. *** The flash controller is clocked from HSI on this
 * part, so the stock save path enables it BEFORE unlocking. With the HAL split
 * the two calls live in different modules and nothing but this assertion keeps
 * them in order; get it wrong and the erase silently fails or the busy wait
 * never returns. The whole prologue order is pinned here:
 *     wwdg_kick -> rcc_enable_hsi_for_flash -> flash_unlock -> wwdg_kick
 *     -> flash_erase_page -> flash_program_word ... -> flash_lock */
TEST(param_store_save_enables_hsi_before_unlocking)
{
    fake::reset();
    writeRealPage();
    svc::params::load();
    ram::app().param[kIdxVolume] = 0x26U;

    svc::params::save();

    const long kick0   = callOrder("wwdg_kick", 0U);
    const long hsi     = callOrder("rcc_enable_hsi_for_flash", 0U);
    const long unlock  = callOrder("flash_unlock", 0U);
    const long erase   = callOrder("flash_erase_page", 0U);
    const long program = callOrder("flash_program_word", 0U);
    const long lock    = callOrder("flash_lock", 0U);

    CHECK(hsi >= 0);
    CHECK(unlock >= 0);
    CHECK(kick0 >= 0 && kick0 < hsi);   /* watchdog kicked first  */
    CHECK(hsi < unlock);                /* <-- the load-bearing one */
    CHECK(unlock < erase);
    CHECK(erase < program);
    CHECK(program < lock);
    CHECK_EQ(fake::countOf("rcc_enable_hsi_for_flash"), 1U);
}

/* The watchdog window is ~65.5 ms and a page erase is far longer, with
 * interrupts masked for the whole sequence -- so the stock code kicks it before
 * every step: once at entry, once after the unlock, once after the erase, and
 * once per programmed word. */
TEST(param_store_save_kicks_the_watchdog_around_every_step)
{
    fake::reset();
    writeRealPage();
    svc::params::load();
    ram::app().param[kIdxMagic] = 0x00U;

    svc::params::save();

    CHECK_EQ(fake::countOf("wwdg_kick"), 3U + kWords);

    /* The last kick comes after the last program, not before it. */
    const long lastProgram = callOrder("flash_program_word", kWords - 1U);
    const long lastKick    = callOrder("wwdg_kick", 3U + kWords - 1U);
    CHECK(lastProgram >= 0);
    CHECK(lastProgram < lastKick);
}

/* SAFETY. Every address this module hands the flash controller must be inside
 * 0x0801F800..0x0801FFFF -- the app image starts at 0x08002000 and the
 * bootloader, the only way back into the box, is below that. The 32 words span
 * 0x0801F800..0x0801F87C and nothing else is ever touched. */
TEST(param_store_save_only_ever_addresses_the_parameter_page)
{
    fake::reset();
    writeRealPage();
    svc::params::load();
    ram::app().param[kIdxVolume] = 0x26U;

    svc::params::save();

    const fake::Call* e = fake::nthCall("flash_erase_page", 0U);
    CHECK(e != nullptr);
    if (e != nullptr) {
        CHECK_EQ(e->a0, kPageBase);
    }

    for (std::size_t i = 0U; i < kWords; ++i) {
        const fake::Call* p = fake::nthCall("flash_program_word", i);
        CHECK(p != nullptr);
        if (p == nullptr) {
            break;
        }
        CHECK_EQ(p->a0, kPageBase + (static_cast<std::uint32_t>(i) * 4U));
        CHECK((p->a0 & 3U) == 0U);
        CHECK(p->a0 >= kPageBase);
        CHECK(p->a0 <= (kPageLast - 3U));
        /* and the datum is the config byte, zero-extended */
        CHECK_EQ(p->a1, static_cast<std::uint32_t>(live(i)));
    }
}

/* The stock code ignores every status the controller returns here: no retry, no
 * error latch, no early exit. A failed erase leaves the page as it was while
 * the shadow is updated anyway, so the firmware stops trying until something
 * changes again -- which is the safe outcome, because a retry loop inside a
 * masked-interrupt critical section is a watchdog reset waiting to happen. */
TEST(param_store_save_ignores_a_flash_error_status)
{
    fake::reset();
    writeRealPage();
    svc::params::load();
    ram::app().param[kIdxVolume] = 0x26U;

    fake::returns().flashStatus = FLASH_STS_TIMEOUT;

    svc::params::save();

    /* Ran the full sequence exactly once, then stopped. */
    CHECK_EQ(fake::eraseCount(), 1U);
    CHECK_EQ(fake::countOf("flash_program_word"), kWords);
    CHECK_EQ(fake::countOf("flash_lock"), 1U);

    /* Shadow updated regardless of the status, exactly as stock. */
    for (std::size_t i = 0U; i < kBytes; ++i) {
        CHECK_EQ(shadow(i), live(i));
    }

    /* So the next tick is quiet -- it does not spin retrying a dead page. */
    svc::params::save();
    CHECK_EQ(fake::eraseCount(), 1U);
}

/* Same story for a status the stock image never saw: the rebuild's own
 * FLASH_STS_REFUSED, and for the HSI enable reporting "not ready". Neither
 * changes the control flow. */
TEST(param_store_save_ignores_a_refused_status_and_an_unready_hsi)
{
    fake::reset();
    writeRealPage();
    svc::params::load();
    ram::app().param[kIdxProfile] = 0U;

    fake::returns().flashStatus = FLASH_STS_REFUSED;
    fake::returns().hsiReady = 0;

    svc::params::save();

    CHECK_EQ(fake::countOf("flash_erase_page"), 1U);
    CHECK_EQ(fake::countOf("flash_program_word"), kWords);
    CHECK_EQ(fake::countOf("flash_lock"), 1U);
    CHECK_EQ(shadow(kIdxProfile), 0);
}

/* A change in the LAST config byte must be detected too -- the dirty scan has
 * to cover all 32, not stop at the named ones. */
TEST(param_store_save_detects_a_change_in_any_byte)
{
    for (std::size_t idx = 0U; idx < kBytes; ++idx) {
        fake::reset();
        writeRealPage();
        svc::params::load();

        ram::app().param[idx] = static_cast<std::uint8_t>(live(idx) ^ 0x5AU);

        svc::params::save();
        CHECK_EQ(fake::eraseCount(), 1U);
    }
}

/* The autobaud retry path, end to end: toggle the profile byte, save, and the
 * new value must be what a reload sees. This is the only reason the page is
 * ever rewritten in the field. */
TEST(param_store_save_persists_an_autobaud_profile_toggle)
{
    fake::reset();
    writeRealPage();
    svc::params::load();
    svc::params::validateDefaults();
    CHECK_EQ(svc::params::profile(), 1);

    /* stock: param[1] = (param[1] != 1) */
    svc::params::setProfile(
        static_cast<std::uint8_t>((svc::params::profile() != 1U) ? 1U : 0U));
    svc::params::save();

    CHECK_EQ(fake::eraseCount(), 1U);
    CHECK_EQ(fake::paramWord(kIdxProfile), 0x00000000U);

    fake::reset();                 /* a power cycle would clear RAM... */
    /* ...but the fake page is reset too, so re-stage the saved value. */
    writeRealPage();
    pageByte(kIdxProfile, 0U);
    svc::params::load();
    CHECK_EQ(svc::params::profile(), 0);
}

/* ===========================================================================
 * profile() / setProfile()
 * ========================================================================= */

/* Both land on param[0x01], which is the byte can_set_baud_by_profile
 * @0x08007b2c reads -- not the latched profileActive copy. */
TEST(param_store_profile_accessors_address_param_byte_1)
{
    fake::reset();
    svc::params::load();

    svc::params::setProfile(0U);
    CHECK_EQ(live(kIdxProfile), 0);
    CHECK_EQ(svc::params::profile(), 0);

    svc::params::setProfile(1U);
    CHECK_EQ(live(kIdxProfile), 1);
    CHECK_EQ(svc::params::profile(), 1);

    /* profileActive is NOT updated by the setter: only validateDefaults()
     * publishes it, and the stock main loop re-reads param[1] directly after
     * phase 1. */
    CHECK_EQ(ram::app().profileActive, 0);
}

/* NO RANGE CHECK, matching stock: the store is a plain byte write and the
 * clamp lives in validateDefaults(). Documented hazard, reproduced on purpose
 * -- see the note in param_store.hpp. */
TEST(param_store_set_profile_does_not_clamp_like_stock)
{
    fake::reset();
    svc::params::load();

    svc::params::setProfile(7U);
    CHECK_EQ(svc::params::profile(), 7);

    /* validateDefaults() is where it gets fixed. */
    ram::app().param[kIdxMagic] = 0xAAU;
    svc::params::validateDefaults();
    CHECK_EQ(svc::params::profile(), 1);
}
