/* ============================================================================
 * can_bus.cpp -- see can_bus.hpp.
 *
 * Ported from reference/decompile/app/can.c (Ghidra output). The
 * commentary here IS the reverse-engineering record: where the stock image
 * does something that looks like a bug (an assignment where an OR was meant,
 * a counter that wraps to zero at 100, a function the decompiler misnamed) the
 * behaviour is reproduced and the note explains why.
 *
 * IRQ NUMBER: init() passes IRQn = 0x36 = 54. On the N32L406 CAN_RX0_IRQn
 * really IS 54 -- it is NOT the STM32F1 value 20. Do not "fix" it; see
 * docs/notes/vector-table-slot-map.md.
 * ==========================================================================*/

#include "can_bus.hpp"

#include <cstddef>

#include "app_config.hpp"
#include "board_io.hpp"
#include "bytes.hpp"
#include "param_store.hpp"
#include "ram_map.hpp"

extern "C" {
#include "hal_can.h"
#include "hal_gpio.h"
#include "hal_nvic.h"
#include "hal_rcc.h"
}

/* The ring entries store each identifier half as a 16-bit word, and this file
 * reproduces those stores one byte at a time so no reinterpret_cast is needed
 * (docs/misra-cpp.md allows exactly one, in ram::app()). The byte order of the
 * two halves is therefore load-bearing: it must match what the stock u16 store
 * produced on a little-endian Cortex-M4. */
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "the CAN ring entry layout assumes a little-endian target"
#endif

namespace svc {
namespace can {
namespace {

/* ---- board wiring -------------------------------------------------------
 * PB8 = CAN_RX, PB9 = CAN_TX, both AF5 (the AF number comes from a hard-coded
 * (port, pin) table inside gpio_init_pin; the MODE is the real parameter).
 * PB12 = transceiver standby, driven through svc::io.
 *     0x08002AC6  movs r2,#2 -> PB8  mode 2 (AF_PP)
 *     0x08002AD0  movs r2,#2 -> PB9  mode 2 (AF_PP)
 *     0x0800745E  movs r2,#1 -> PB12 mode 1 (OUTPUT_PP)                    */
/* The CAN pins, spelled with hal_gpio.h's names rather than bare masks so they
 * grep against the rest of the board. NOTE PB8 is dual-role: it is CAN_RX while
 * the bus is up, and svc::io::configurePb8Wake() re-purposes it as the EXTI
 * line 8 wake source on the way into deep sleep. */
constexpr std::uint32_t kPinCanRx = GPIO_PIN_8;
constexpr std::uint32_t kPinCanTx = GPIO_PIN_9;

/* CAN_RX0_IRQn on the N32L406, with the stock priority grouping and priority. */
constexpr std::uint32_t kNvicPriorityGroup = 0x300U;  /* PRIGROUP = 3 */
constexpr std::uint8_t  kCanRx0Irqn   = 54U;
constexpr std::uint8_t  kCanRx0Preempt = 3U;
constexpr std::uint8_t  kCanRx0Sub     = 0U;

/* Bit timing, from the derivation in app/hal/hal_can.h:
 * 1 + (BS1 + 1) + (BS2 + 1) = 8 tq/bit, sample point 62.5 %, SJW 1 tq. */
constexpr std::uint32_t kBrpNumerator = 2000U;  /* BRP = 2000 / baud_kbps   */
constexpr std::uint8_t  kBitTimingSjw = 0U;     /* 1 tq */
constexpr std::uint8_t  kBitTimingBs1 = 3U;     /* 4 tq */
constexpr std::uint8_t  kBitTimingBs2 = 2U;     /* 3 tq */

/* Reloads the two bitrate wrappers plant. 200 x 100 ms = 20 s of grace before
 * the link state machine may declare the bus dead after a re-baud. */
constexpr std::uint8_t kActivityCtrReload = 200U;

/* TX pump limits: at most 10 entries per 4 ms pass, and the "no free mailbox"
 * counter saturates by wrapping to zero at 100 (stock; see txDrain). */
constexpr std::uint8_t  kTxDrainMaxPerPass = 10U;
constexpr std::uint16_t kTxBusyCtrLimit    = 100U;

/* The CAN profile selector. The INDEX comes from param_store.hpp so there is
 * one definition of which byte this is, but the access goes straight to the RAM
 * mirror rather than through svc::params::profile(): the constant is header-only
 * and costs nothing at link time, whereas calling the accessor would drag the
 * parameter store and the flash HAL into every link that only wants the rings.
 * The stock code reads the same byte the same way, as *(p_param_struct + 1). */
constexpr std::size_t kParamProfile = svc::params::kProfile;

/* ---- ring helpers -------------------------------------------------------
 * Both rings are 50 x 16 bytes; the indices are the four 32-bit words in
 * ram::AppRam::canRing. One slot is always left unused so head == tail is
 * unambiguously "empty". */

[[nodiscard]] volatile std::uint8_t* entryAt(volatile std::uint8_t* ring,
                                             std::uint32_t index) noexcept
{
    return ring + static_cast<std::size_t>(index) * ram::kCanRingStride;
}

[[nodiscard]] std::uint32_t nextIndex(std::uint32_t index) noexcept
{
    return (index + 1U) % static_cast<std::uint32_t>(ram::kCanRingEntries);
}

/* id high half at +0, low half at +2, each a little-endian u16 -- the stock
 * code rebuilds the identifier as CONCAT22(u16@+0, u16@+2). */
void storeId(volatile std::uint8_t* entry, std::uint32_t canId) noexcept
{
    entry[ram::kCanEntryIdHi + 0U] = static_cast<std::uint8_t>(canId >> 16);
    entry[ram::kCanEntryIdHi + 1U] = static_cast<std::uint8_t>(canId >> 24);
    entry[ram::kCanEntryIdLo + 0U] = static_cast<std::uint8_t>(canId);
    entry[ram::kCanEntryIdLo + 1U] = static_cast<std::uint8_t>(canId >> 8);
}

[[nodiscard]] std::uint32_t loadId(const volatile std::uint8_t* entry) noexcept
{
    const std::uint32_t hi = static_cast<std::uint32_t>(entry[ram::kCanEntryIdHi + 0U])
                           | (static_cast<std::uint32_t>(entry[ram::kCanEntryIdHi + 1U]) << 8);
    const std::uint32_t lo = static_cast<std::uint32_t>(entry[ram::kCanEntryIdLo + 0U])
                           | (static_cast<std::uint32_t>(entry[ram::kCanEntryIdLo + 1U]) << 8);

    return (hi << 16) | lo;
}

/* The stock payload loop is `for (i = 0; i < dlc && i < 8; i = i + 1 & 0xff)`.
 * The 0xFF mask on the counter is why svc::bytes::copy takes a uint8_t length:
 * a length of 256 would have spun forever in the original. */
[[nodiscard]] std::uint8_t payloadLen(std::uint8_t dlc) noexcept
{
    return (dlc < 8U) ? dlc : static_cast<std::uint8_t>(8U);
}

/* ---- the 16-bit "no free mailbox" counter -------------------------------
 * It lives at 0x200018E6, i.e. bytes 2..3 of ram::AppRam::canTxBusyBlock --
 * a 16-bit field inside a block the decompile never otherwise named. Accessed
 * as two bytes so this file needs no reinterpret_cast; the memory effect is
 * identical on the little-endian target (see the #error guard above). */
[[nodiscard]] std::uint16_t txBusyCtr() noexcept
{
    const volatile std::uint8_t* b = ram::app().canTxBusyBlock;
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[2])
         | static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[3]) << 8));
}

void setTxBusyCtr(std::uint16_t value) noexcept
{
    volatile std::uint8_t* b = ram::app().canTxBusyBlock;
    b[2] = static_cast<std::uint8_t>(value);
    b[3] = static_cast<std::uint8_t>(value >> 8);
}

/* Shared body of the two byte-identical bitrate wrappers below. */
void reinitAtBitrate(int baudKbps) noexcept
{
    initNormal(baudKbps);

    /* PB12 is the transceiver standby line. WHICH pin that is belongs to
     * svc::io (board_io.cpp), which also drives its level -- this module only
     * decides when. The stock code calls gpio_init_pin on it twice in a row;
     * the duplicate is harmless and is reproduced so the instruction stream
     * still matches. */
    svc::io::configureCanTransceiverPin();
    svc::io::configureCanTransceiverPin();

    /* 0x08007430: drive PB12 low = transceiver awake. */
    svc::io::setCanTransceiverStandby(false);

    ram::AppRam& r = ram::app();
    r.canReinitBitrate = static_cast<std::uint32_t>(baudKbps);
    r.canBusStatus     = cfg::kBusSettling;
    r.canActivityCtr   = kActivityCtrReload;
}

}  // namespace

/* ===========================================================================
 * Bring-up
 * ======================================================================== */

/* --- 0x08002A90 --- Order is kept exactly as the stock: NVIC first (global
 * interrupts are still masked at this point), then clocks + pins, then the
 * reset pulse, then the bit timing, then the accept-all filter, then FMPIE0. */
void init(int baudKbps, std::uint8_t mode) noexcept
{
    can_init_t        cfgInit;
    can_filter_init_t filt;

    nvic_set_priority_grouping(kNvicPriorityGroup);
    nvic_init(kCanRx0Irqn, kCanRx0Preempt, kCanRx0Sub, 1);

    /* stock argument 0x09 = AFIO | IOPB */
    rcc_apb2_periph_clock(RCC_APB2_AFIO | RCC_APB2_IOPB, 1);
    gpio_init_pin(GPIOB, kPinCanRx, GPIO_MODE_AF_PP);
    gpio_init_pin(GPIOB, kPinCanTx, GPIO_MODE_AF_PP);

    rcc_apb1_periph_clock(RCC_APB1_CAN, 1);
    can_deinit(CAN);   /* reset pulse: every re-baud starts from known state */

    can_struct_init_defaults(&cfgInit);
    cfgInit.ttcm = 0U;   /* no time-triggered mode   */
    cfgInit.abom = 0U;   /* no auto bus-off recovery */
    cfgInit.awum = 0U;   /* no auto wake-up          */
    cfgInit.nart = 0U;   /* automatic retransmission */
    cfgInit.rflm = 0U;   /* FIFO overwrites when full */
    cfgInit.txfp = 1U;   /* FIFO-priority TX order   */
    cfgInit.sjw  = kBitTimingSjw;
    cfgInit.bs1  = kBitTimingBs1;
    cfgInit.bs2  = kBitTimingBs2;
    cfgInit.prescaler =
        static_cast<std::uint16_t>(kBrpNumerator / static_cast<std::uint32_t>(baudKbps));
    cfgInit.mode = mode;
    (void)can_init_regs(CAN, &cfgInit);   /* stock ignores the result */

    /* One accept-all bank: a 32-bit id/mask pair of zeroes matches every
     * identifier and routes it to FIFO0. ALL real filtering is done in software
     * by the profile decoders. */
    filt.id_high         = 0U;
    filt.id_low          = 0U;
    filt.mask_id_high    = 0U;
    filt.mask_id_low     = 0U;
    filt.fifo_assignment = 0U;   /* FIFO0                 */
    filt.number          = 0U;   /* bank 0                */
    filt.mode            = 0U;   /* identifier / mask     */
    filt.scale           = 1U;   /* one 32-bit filter     */
    filt.activation      = 1U;
    can_filter_init(&filt);

    can_it_config(CAN, CAN_IT_FMP0, 1);
}

/* --- 0x08002B76 --- */
void initNormal(int baudKbps) noexcept
{
    init(baudKbps, CAN_MODE_NORMAL);
}

/* --- 0x08002B84 --- Present in the image but never called by live code. */
void initSilent(int baudKbps) noexcept
{
    init(baudKbps, CAN_MODE_SILENT);
}

/* --- 0x08007482 --- */
void reinitBitrateA(int baudKbps) noexcept
{
    reinitAtBitrate(baudKbps);
}

/* --- 0x0800744E --- Byte-identical twin of _A in the stock image. */
void reinitBitrateB(int baudKbps) noexcept
{
    reinitAtBitrate(baudKbps);
}

/* --- 0x08007B2C --- Baud is locked to the decode profile: profile 1 -> 125
 * kbit/s, profile 0 -> 83.333 kbit/s. Any other stored value is normalised
 * back to profile 0 here, which is the last line of defence against a
 * half-written parameter page picking a bit rate the bus does not use. */
static_assert(cfg::kProfileCount == 2U,
              "setBaudByProfile() and applyBitrateB() are stock two-way "
              "NORMALISERS: the else branch WRITES BACK profile 0 over anything "
              "it does not recognise, and params::save() then commits that to "
              "flash. Adding a third profile without restructuring these two "
              "makes the box erase its own configuration at boot. See the "
              "kProfileCount note in app_config.hpp.");

void setBaudByProfile() noexcept
{
    if (ram::app().param[kParamProfile] == cfg::kProfileFca125k) {
        reinitBitrateA(cfg::kCanBaudFcaKbps);
    } else {
        ram::app().param[kParamProfile] = cfg::kProfileJeep83k;
        reinitBitrateA(cfg::kCanBaudJeepKbps);
    }
}

/* --- 0x08007B4C --- Same test, through the _B wrapper. */
void applyBitrateB() noexcept
{
    if (ram::app().param[kParamProfile] == cfg::kProfileFca125k) {
        reinitBitrateB(cfg::kCanBaudFcaKbps);
    } else {
        ram::app().param[kParamProfile] = cfg::kProfileJeep83k;
        reinitBitrateB(cfg::kCanBaudJeepKbps);
    }
}

/* ===========================================================================
 * TX ring
 * ======================================================================== */

/* --- 0x080074C0 --- */
void txEnqueue(std::uint32_t canId, std::uint8_t ide, std::uint8_t dlc,
               const volatile std::uint8_t* data) noexcept
{
    ram::AppRam&        r    = ram::app();
    const std::uint32_t head = r.canRing.txHead;
    const std::uint32_t next = nextIndex(head);

    if (next == r.canRing.txTail) {
        /* Ring full: the frame is DROPPED. The stock code ORs the flag here
         * (unlike txDrain, which assigns) and does not retry.
         *
         * This OR is a read-modify-write that the CAN RX ISR can land inside,
         * losing whichever bit the other side was setting. It is left
         * unprotected because canErrFlags has NO READER anywhere in the image --
         * it is a write-only diagnostic you inspect over SWD. If anything ever
         * starts acting on these bits, this needs a critical section, and so
         * does the OR in onRxInterrupt(). */
        r.canErrFlags = static_cast<std::uint8_t>(r.canErrFlags | ram::kCanErrTxRingFull);
        return;
    }

    volatile std::uint8_t* e = entryAt(r.canTxRing, head);
    storeId(e, canId);
    e[ram::kCanEntryIde] = ide;
    e[ram::kCanEntryDlc] = dlc;
    svc::bytes::copy(e + ram::kCanEntryData, data, payloadLen(dlc));

    /* Bytes past DLC keep whatever the previous occupant of this slot left
     * there, exactly as in stock; transmitEntry() never reads them. */
    r.canRing.txHead = next;
}

/* --- 0x08002B98 --- true means NO mailbox was free. See the header. */
bool transmitEntry(const volatile std::uint8_t* entry) noexcept
{
    can_tx_msg_t msg;

    const std::uint32_t id = loadId(entry);

    msg.std_id = 0U;
    msg.ext_id = 0U;
    msg.rtr    = 0U;
    msg.dlc    = entry[ram::kCanEntryDlc];

    if (entry[ram::kCanEntryIde] == 0U) {
        msg.std_id = id;
        msg.ide    = CAN_ID_STANDARD;
    } else {
        msg.ide    = CAN_ID_EXTENDED;
        msg.ext_id = id;
    }

    /* The stock loop fills only data[0..dlc-1] and leaves the rest of its stack
     * frame uninitialised; bytes past DLC are never put on the wire, but
     * zeroing them keeps the mailbox write deterministic. */
    const std::size_t len = static_cast<std::size_t>(payloadLen(msg.dlc));
    for (std::size_t i = 0U; i < 8U; ++i) {
        msg.data[i] = (i < len) ? entry[ram::kCanEntryData + i] : static_cast<std::uint8_t>(0U);
    }

    return can_transmit(CAN, &msg) == CAN_TX_NO_MAILBOX;
}

/* --- 0x08007698 --- the 4 ms task. */
void txDrain() noexcept
{
    ram::AppRam& r = ram::app();

    for (std::uint8_t drained = 0U; drained < kTxDrainMaxPerPass; ++drained) {
        const std::uint32_t tail = r.canRing.txTail;
        if (tail == r.canRing.txHead) {
            return;
        }

        if (transmitEntry(entryAt(r.canTxRing, tail))) {
            /* All three mailboxes busy. NOTE: the stock code ASSIGNS here, it
             * does not OR -- a mailbox stall therefore wipes a pending
             * kCanErrRxRingFull / kCanErrTxRingFull. Reproduced deliberately;
             * nothing in the image reads those bits, they are diagnostics. */
            r.canErrFlags = ram::kCanErrTxNoMailbox;

            /* Saturating-by-wrapping stall counter: it counts to 100 and then
             * snaps back to 0 rather than sticking. Also stock. */
            const std::uint16_t busy = txBusyCtr();
            setTxBusyCtr(busy < kTxBusyCtrLimit ? static_cast<std::uint16_t>(busy + 1U)
                                                : static_cast<std::uint16_t>(0U));

            /* The frame is NOT consumed: txTail stays put and the next pass
             * retries it. */
            return;
        }

        r.canRing.txTail = nextIndex(tail);
    }
}

void txFlush() noexcept
{
    ram::AppRam& r = ram::app();
    r.canRing.txTail = r.canRing.txHead;
}

/* ===========================================================================
 * RX ring
 * ======================================================================== */

/* --- 0x08002C0A --- the CAN_RX0 interrupt body. */
void onRxInterrupt() noexcept
{
    ram::AppRam& r = ram::app();
    can_rx_msg_t msg;

    const std::uint32_t head = r.canRing.rxHead;
    const std::uint32_t tail = r.canRing.rxTail;

    const std::uint32_t freeSlots =
        (head < tail) ? (tail - head)
                      : ((tail + static_cast<std::uint32_t>(ram::kCanRingEntries)) - head);

    if (freeSlots < 2U) {
        /* Still pop the message: releasing the FIFO is what clears the FMP0
         * condition, so skipping it would re-enter this handler forever. */
        can_receive_fifo(CAN, 0, &msg);
        r.canErrFlags = static_cast<std::uint8_t>(r.canErrFlags | ram::kCanErrRxRingFull);
        return;
    }

    can_receive_fifo(CAN, 0, &msg);

    /* can_receive_fifo stores the RAW RMI bit in .ide, i.e. 0 or 4, not 0 or 1. */
    const std::uint32_t id = (msg.ide == CAN_ID_STANDARD) ? msg.std_id : msg.ext_id;

    volatile std::uint8_t* e = entryAt(r.canRxRing, head);
    storeId(e, id);
    e[ram::kCanEntryDlc] = msg.dlc;
    svc::bytes::copy(e + ram::kCanEntryData, msg.data, payloadLen(msg.dlc));

    /* NOTE: the stock ISR never writes entry[kCanEntryIde] on the RX side -- it
     * stores only the two id halves, the DLC and the payload. The decoders only
     * ever look at the id and the data, so the stale IDE byte is never read.
     * Left alone on purpose: zeroing it would be a behaviour change. */

    r.canRing.rxHead = nextIndex(head);
}

bool rxAvailable() noexcept
{
    const ram::AppRam& r = ram::app();
    return r.canRing.rxTail != r.canRing.rxHead;
}

std::uint32_t rxId() noexcept
{
    ram::AppRam& r = ram::app();
    return loadId(entryAt(r.canRxRing, r.canRing.rxTail));
}

const volatile std::uint8_t* rxData() noexcept
{
    ram::AppRam& r = ram::app();
    return entryAt(r.canRxRing, r.canRing.rxTail) + ram::kCanEntryData;
}

void rxAdvance() noexcept
{
    ram::AppRam& r = ram::app();
    r.canRing.rxTail = nextIndex(r.canRing.rxTail);
}

/* ===========================================================================
 * State / shutdown
 * ======================================================================== */

/* --- 0x080074B6 --- bit 0 of the vehicle-state block = "vehicle bus alive". */
std::uint8_t busActiveFlag() noexcept
{
    return static_cast<std::uint8_t>(ram::app().vehicle.at(ram::VehicleState::kFlags)
                                     & ram::VehicleState::kFlagAccOn);
}

/* --- 0x080077E8 --- the 100 ms hook. Both counters floor at 0. */
void timersTick() noexcept
{
    ram::AppRam& r = ram::app();

    if (r.canActivityCtr != 0U) {
        r.canActivityCtr = static_cast<std::uint8_t>(r.canActivityCtr - 1U);
    }
    if (r.canErrHoldTimer != 0U) {
        r.canErrHoldTimer = static_cast<std::uint8_t>(r.canErrHoldTimer - 1U);
    }
}

/* --- 0x08002D1A --- misnamed "can_rx0_msg_pending" by the decompiler. */
bool enterSleep() noexcept
{
    return can_sleep(CAN) == 1;
}

}  // namespace can
}  // namespace svc
