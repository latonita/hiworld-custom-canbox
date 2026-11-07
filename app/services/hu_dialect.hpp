/* ============================================================================
 * hu_dialect.hpp -- the INTERNAL contract between the USART2 scheduler in
 * head_unit.cpp and the two head-unit dialects that live beside it,
 * hu_hiworld.cpp ("AA 55") and hu_raise_benz.cpp ("2E").
 *
 * Not a public header: nothing outside app/services/head_unit*.cpp and
 * app/services/hu_*.cpp may include it. The public face of the module is
 * head_unit.hpp and it does not change when a dialect does.
 *
 * Both dialect translation units are compiled in EVERY build -- see the
 * dialect banner at the top of head_unit.cpp for why that matters and why
 * this is not done with #if.
 * ==========================================================================*/

#ifndef HU_DIALECT_HPP
#define HU_DIALECT_HPP

#include <cstdint>

#include "ram_map.hpp"

namespace svc {
namespace hu {

/* ===========================================================================
 * Shorthand for the fixed RAM this module owns. All of it is named in
 * ram_map.hpp; nothing here may use a raw address.
 *
 * Only the two buffers both dialects touch are here. ackq() and slotState()
 * belong to the scheduler alone and stay in head_unit.cpp.
 * ========================================================================= */
inline volatile std::uint8_t* fb() noexcept { return ram::app().uartFrame; }
inline volatile std::uint8_t* rxf() noexcept { return ram::app().uartRxFrame; }

/* ===========================================================================
 * The message tables, from flash 0x08009F2C (TX) and 0x08009F74 (RX), reached
 * through the literal pools at 0x08008E30 / 0x08008E38 / 0x08009004. The stock
 * record shape is { u8 cmd; u8 len; u8 pad[2]; void *buf; void *shadow; }.
 *
 * Only TxDesc and BufFn are here, because only the TX table is per-dialect.
 * RxDesc and the RX table are the scheduler's and stay in head_unit.cpp --
 * they are shared, for the reason given on detail::rxDispatch() below -- but
 * they are built out of the same BufFn and the rationale below covers both.
 *
 * WHY FUNCTION POINTERS AND NOT DATA POINTERS.
 * Historical, and no longer forced. When this was written the buffers lived
 * inside a ram::app() produced by a reinterpret_cast to a fixed SRAM address,
 * which is NOT a constant expression -- so a table of `volatile uint8_t *`
 * would have needed a runtime initialiser in .init_array, which this image
 * never runs and which app/linker/app.ld fails the build over. A table of
 * POINTERS TO FUNCTIONS is a constant expression, so the table is `constexpr`,
 * lands in .rodata, and the accessors below fold to one constant address each
 * at -Os.
 *
 * The state is an ordinary .bss object now, so `&ram::g_appRam.uartShadows.msg72`
 * IS an address constant and a plain data-pointer table would work. The
 * indirection is kept because it costs nothing after inlining and because the
 * accessors are where the old bare addresses became names; collapsing it is a
 * tidy-up for someone with a bench, not a correctness fix.
 *
 * The accessors are also the place where the old bare addresses became NAMES:
 * what hw_uart.c spelled 0x20001B98 is ram::app().uartShadows.msg72.
 * ========================================================================= */
using BufFn = volatile std::uint8_t* (*)() noexcept;

/* ---------------------------------------------------------------------------
 * LANES (not stock; telemetry v2, docs/TELEMETRY-V2-SPEC.md section 7).
 *
 *   kLaneSlow     the stock 48 ms change-driven round-robin, its keep-alive
 *                 and its busy refresh. Every stock row, every Benz row.
 *   kLanePrio     sent ONLY when queued by svc::hu::requestPriority(), one
 *                 frame per 8 ms pass, ahead of the 48 ms boundary. The
 *                 round-robin, the keep-alive and the refresh never touch it.
 *   kLaneRequest  sent ONLY on svc::hu::requestSend() (the 1 Hz heartbeat);
 *                 also skipped by the round-robin and the keep-alive, so it
 *                 is not sent twice a second.
 *
 * `cmp` is the number of leading payload bytes the round-robin compares for
 * change detection; 0 means all `len`. Stock 0x32 uses 2 (the gear byte and
 * the one before it) so a moving rpm does not claim a 48 ms slot every pass;
 * the whole payload still goes out whenever the row is sent.
 *
 * Both members have defaults, so a row written { cmd, len, buf, shadow } --
 * every Benz row -- is a slow row compared in full, exactly as before.
 * ------------------------------------------------------------------------- */
inline constexpr std::uint8_t kLaneSlow    = 0U;
inline constexpr std::uint8_t kLanePrio    = 1U;
inline constexpr std::uint8_t kLaneRequest = 2U;

struct TxDesc {
    std::uint8_t cmd;
    std::uint8_t len;
    BufFn        buf;     /* live payload source                            */
    BufFn        shadow;  /* last-sent copy, for change detection           */
    std::uint8_t lane = kLaneSlow;
    std::uint8_t cmp  = 0U;   /* bytes compared; 0 = len                    */
};

namespace detail {

/* Defined in head_unit.cpp, called from BOTH parsers. It is dialect-neutral
 * because both parsers reassemble into ram::AppRam::uartRxFrame in the same
 * layout -- RXF[2] = LEN, RXF[3] = CMD, RXF[4..] = payload -- so it is shared
 * verbatim instead of being written twice and drifting. */
void rxDispatch() noexcept;

}  // namespace detail

/* ---- the stock Hiworld dialect, hu_hiworld.cpp -------------------------- */
namespace hiworld {

/* Thirteen: the six stock slots from the flash table at 0x08009F2C, plus the
 * seven of the custom telemetry block v2 (docs/TELEMETRY-V2-SPEC.md,
 * docs/TPMS-POLL-SPEC.md for 0xB7). A
 * static_assert in head_unit.cpp couples this to cfg::kTxSlotCount, which is
 * what ram::AppRam::uartSlotState is sized from -- change one and the build
 * stops rather than the scheduler corrupting its own index. */
inline constexpr std::uint8_t kSlots = 13U;

extern const TxDesc kTxTable[];

void emit(std::uint8_t cmd, const volatile std::uint8_t* payload,
          std::uint8_t len) noexcept;
void rxParse() noexcept;

}  // namespace hiworld

/* ---- the Raise/Mercedes dialect, hu_raise_benz.cpp ---------------------- */
namespace raisebenz {

inline constexpr std::uint8_t kSlots = 5U;

extern const TxDesc kTxTable[];

void emit(std::uint8_t cmd, const volatile std::uint8_t* payload,
          std::uint8_t len) noexcept;
void rxParse() noexcept;

}  // namespace raisebenz

}  // namespace hu
}  // namespace svc

#endif /* HU_DIALECT_HPP */
