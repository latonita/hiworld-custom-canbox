/* ============================================================================
 * ram_map.hpp -- the application's runtime state, as one typed structure.
 *
 * WHAT THIS IS
 * Every byte of state the firmware keeps between one main-loop pass and the
 * next. It is an ordinary object: the linker allocates it in .bss, the startup
 * code zeroes it like any other .bss, and nothing here depends on where it
 * lands.
 *
 * WHAT IT USED TO BE, AND WHY THAT CHANGED
 * The stock firmware keeps this state at fixed absolute SRAM addresses
 * (0x20001000..0x20003028), and the first two generations of this rebuild
 * reproduced them exactly -- first as ~110 macros like
 *     #define g_status_block ((volatile uint8_t *)0x20001B49U)
 * and then as this struct with a static_assert per field pinning its offset.
 *
 * Nothing ever required that. The only hardware constraint on SRAM is the
 * stack (the bootloader sets MSP = 0x20000600 and it grows down), and the only
 * thing that read a fixed app address was our own flashing tool. The stock
 * addresses were fidelity, not necessity, and holding runtime data at them cost
 * ~5.5 KB of reserved padding, a reinterpret_cast, and a map that had to be
 * maintained in lockstep with a decompile.
 *
 * So the addresses are gone and the padding with them. What is kept is the
 * PROVENANCE: each field still carries the stock address it corresponds to, in
 * a `stock @0x...` comment, because that is how any of this gets diffed against
 * Ghidra or against the notes in docs/. The comments are documentation now, not
 * a layout contract.
 *
 * The one datum that is still addressable from outside is the version string,
 * and it is deliberately NOT in here -- see `g_versionString` below.
 *
 * WHAT IS STILL LOAD-BEARING
 * Offsets WITHIN a block, where the stock firmware overlaps two views of the
 * same bytes. `VehicleState` is the important one: four different head-unit
 * messages are overlapping windows onto one run of bytes, and those relative
 * offsets are protocol, not layout. They keep their static_asserts.
 *
 * VOLATILE
 * Every field is volatile. Only the ISR-shared ones strictly need it; the
 * blanket qualifier is kept so this cannot change code generation on a field
 * someone mis-classified. See docs/misra-cpp.md, deviation D-3.
 *
 * ISR-shared regions (written by an interrupt, read by the main loop):
 *   tickDiv*, sched, softClock, miscTimers, softTimers   <- TIM2_IRQHandler
 *   canRing, canRxRing, canErrFlags                      <- CAN_RX0_IRQHandler
 *   uartFrame (tx cursor + rx ring)                      <- USART2_IRQHandler
 * ==========================================================================*/

#ifndef RAM_MAP_HPP
#define RAM_MAP_HPP

#include <cstddef>
#include <cstdint>

#include "app_config.hpp"

namespace ram {

/* ===========================================================================
 * Soft real-time clock
 *
 * The stock code reads the flags byte with a 32-bit read-modify-write whose
 * mask preserves the bytes above it, so a byte here has an identical memory
 * effect. See docs/misra-cpp.md, deviation D-7.     stock @0x200010FC
 * ========================================================================= */
struct SoftClock {
    volatile std::uint8_t hour24;
    volatile std::uint8_t minute;
    volatile std::uint8_t second;
    volatile std::uint8_t tenths;
    volatile std::uint8_t flags;    /* bit0 changed, bit1 PM                  */
    volatile std::uint8_t hour12;
};

inline constexpr std::uint8_t kSoftClockChanged = 0x01U;  /* minute rollover  */
inline constexpr std::uint8_t kSoftClockPm      = 0x02U;  /* hour24 >= 12     */

/* Blink / heartbeat bits.                                stock @0x20001104 */
inline constexpr std::uint8_t kBlinkState   = 0x01U;  /* toggled every 500 ms */
inline constexpr std::uint8_t kBlinkPrev    = 0x02U;  /* edge detector shadow */
inline constexpr std::uint8_t kBlinkChanged = 0x04U;  /* one-shot pulse       */

/* ===========================================================================
 * 1 ms scheduler                                        stock @0x20001108
 *
 * counter is a genuine 32-bit free-running millisecond counter: the cadence
 * tests mask it with 0x3FF and 0x7FF, so an 8-bit counter would make the
 * 1024 ms and 2048 ms flags unreachable and hang phase 1.
 * ========================================================================= */
struct Scheduler {
    volatile std::uint32_t counter;      /* free-running milliseconds         */
    volatile std::uint8_t  flagFast;     /*    4 ms                           */
    volatile std::uint8_t  flagMed;      /*    8 ms                           */
    volatile std::uint8_t  flagDiv16;    /*   16 ms                           */
    volatile std::uint8_t  flagDiv32;    /*   32 ms                           */
    volatile std::uint8_t  flagDiv64;    /*   64 ms                           */
    volatile std::uint8_t  flagDiv128;   /*  128 ms                           */
    volatile std::uint8_t  flagDiv256;   /*  256 ms                           */
    volatile std::uint8_t  flagDiv512;   /*  512 ms                           */
    volatile std::uint8_t  flagSlow;     /* 1024 ms                           */
    volatile std::uint8_t  flagAutobaud; /* 2048 ms                           */
};

/* ===========================================================================
 * CAN rings                                             stock @0x20001118
 *
 * rxHead is what phase 1 waits on ("3 frames seen"); txTail = txHead is the
 * queue flush the bus-down probe performs.
 * ========================================================================= */
struct CanRingIndices {
    /* THE TWO RINGS HAVE DIFFERENT CONCURRENCY RULES. Do not read this as four
     * symmetric ISR<->main handovers.
     *
     * RX is a genuine single-producer/single-consumer queue across the
     * interrupt boundary: rxHead is written ONLY by CAN_RX0_IRQHandler, rxTail
     * ONLY by the decoder. That works because both are naturally-aligned 32-bit
     * fields, so each access is one instruction and cannot tear, and because the
     * ISR refuses to push with fewer than two free slots -- which is what stops
     * it overwriting the entry the decoder is currently reading.
     *
     * TX is NOT a cross-boundary queue. Both ends are main loop: txHead from
     * txEnqueue(), txTail from txDrain() and txFlush(). It is safe because it is
     * single-threaded, not because it is SPSC.
     *
     * So: never call svc::can::txEnqueue() from an interrupt. Its update of
     * txHead is a read-modify-write with a 16-byte payload copy in the middle;
     * an ISR landing inside a main-loop txEnqueue would lose one frame and
     * corrupt the other. The obvious future change -- answering a frame straight
     * from onRxInterrupt() -- is exactly the one this forbids. */
    volatile std::uint32_t rxHead;   /* CAN_RX0_IRQHandler only               */
    volatile std::uint32_t rxTail;   /* the decoder only                      */
    volatile std::uint32_t txHead;   /* txEnqueue()  -- main loop             */
    volatile std::uint32_t txTail;   /* txDrain()/txFlush() -- main loop      */
};

/* Ring geometry. 50 entries x 16 bytes, both directions. The entry layout is
 * fixed by the stock ISR and the drain path and is still load-bearing:
 *   [0..1] u16 id bits 31..16   [2..3] u16 id bits 15..0
 *   [4] IDE flag   [5] DLC   [6..13] data[0..7]   [14..15] pad             */
inline constexpr std::size_t kCanRingEntries = 50U;
inline constexpr std::size_t kCanRingStride  = 16U;
inline constexpr std::size_t kCanRingBytes   = kCanRingEntries * kCanRingStride;
inline constexpr std::size_t kCanEntryIdHi   = 0U;
inline constexpr std::size_t kCanEntryIdLo   = 2U;
inline constexpr std::size_t kCanEntryIde    = 4U;
inline constexpr std::size_t kCanEntryDlc    = 5U;
inline constexpr std::size_t kCanEntryData   = 6U;
static_assert(kCanEntryData + 8U <= kCanRingStride,
              "a ring entry must hold eight data bytes after its header");

/* Bits in AppRam::canErrFlags.                          stock @0x20001138 */
inline constexpr std::uint8_t kCanErrRxRingFull  = 0x01U;  /* < 2 free slots  */
inline constexpr std::uint8_t kCanErrTxNoMailbox = 0x02U;  /* all 3 MBs busy  */
inline constexpr std::uint8_t kCanErrTxRingFull  = 0x04U;  /* enqueue dropped */

/* ===========================================================================
 * Steering-wheel-control / node addressing              stock @0x2000117F
 * ========================================================================= */
struct SwcState {
    volatile std::uint8_t keycode;      /* low 6 bits of the CAN id           */
    volatile std::uint8_t keydata;      /* d[1] of the key report             */
    volatile std::uint8_t keycodePrev;
    volatile std::uint8_t keydataRef;   /* index the soft-timer scan won      */
};

/* ===========================================================================
 * Short countdowns                                      stock @0x2000118A
 *
 * Two different floors, and both are load-bearing: miscTimer runs down to 0 (a
 * plain "expired" test) while the three SWC timers stop at 1, so an "== 1" test
 * in the consumer fires on exactly one pass and then latches.
 * ========================================================================= */
struct MiscTimers {
    volatile std::uint8_t swcRespMode;  /* floors at 1 */
    volatile std::uint8_t swcFlag;      /* floors at 1 */
    volatile std::uint8_t swcTimer;     /* floors at 1 */
    volatile std::uint8_t miscTimer;    /* floors at 0 */
};

/* ===========================================================================
 * The software-timer pool                               stock @0x200018F8
 *
 * 64 slots, aged once per 10 ms, armed to 150 counts (1.5 s). Index 0x16 is
 * both the last slot a scan examines and the "nothing held" sentinel; scans
 * start at 0x17 so high key codes win over low ones.
 * ========================================================================= */
inline constexpr std::size_t  kSoftTimerSlots     = 64U;
inline constexpr std::int32_t kSoftTimerReload    = 150;    /* 1.5 s          */
inline constexpr std::uint8_t kSoftTimerIdle      = 0x16U;  /* sentinel index */
inline constexpr std::uint8_t kSoftTimerScanStart = 0x17U;

/* ===========================================================================
 * Vehicle state                                         stock @0x20001B49
 *
 * The block the decoders fill and the head-unit link reports. Four different
 * UART messages are OVERLAPPING WINDOWS onto one run of bytes:
 *
 *   offset 0x00 len 14  -> UART cmd 0x72   (flags, SWC key, illum, steering)
 *   offset 0x13 len 10  -> UART cmd 0x12   (doors, volume, tone)
 *   offset 0x1D len 14  -> UART cmd 0x32   (gear, rpm)
 *   offset 0x2C len  8  -> UART cmd 0xA6   (amplifier level + tone alias)
 *
 * Those relative offsets are PROTOCOL, not layout: the head unit is told where
 * each field sits inside its message. They keep their asserts even though the
 * block's absolute address no longer matters.
 *
 * Storage stays a raw byte array because the UART layer hands a pointer and a
 * length straight to the framer, exactly as the stock descriptor table does;
 * the named accessors are the intended way in.
 * ========================================================================= */
class VehicleState {
public:
    static constexpr std::size_t kSize = 0x34U;

    /* ---- byte offsets, verified against reference/decompile/app/can_rx_decode.c --------- */
    static constexpr std::size_t kFlags        = 0x00U;
    static constexpr std::size_t kSwcKey       = 0x02U;  /* plugin key code   */
    static constexpr std::size_t kIllumLevel   = 0x03U;  /* dimmer percent    */
    static constexpr std::size_t kSteerHi      = 0x04U;  /* angle, big endian */
    static constexpr std::size_t kSteerLo      = 0x05U;
    static constexpr std::size_t kChangeCount  = 0x0FU;  /* bumped on a change*/
    static constexpr std::size_t kChangeSource = 0x10U;
    static constexpr std::size_t kChangeShadow = 0x11U;
    static constexpr std::size_t kAccShadow    = 0x12U;  /* whole byte, 0 or 1*/
    static constexpr std::size_t kDoors        = 0x15U;  /* bitfield, below   */
    static constexpr std::size_t kVolume       = 0x17U;
    static constexpr std::size_t kTone         = 0x18U;  /* 5 bytes           */
    static constexpr std::size_t kGear         = 0x1EU;  /* PRNDL, profile 1  */
    static constexpr std::size_t kRpmHi        = 0x1FU;
    static constexpr std::size_t kRpmLo        = 0x20U;
    static constexpr std::size_t kAmpLevel     = 0x2CU;
    static constexpr std::size_t kAmpTone      = 0x2DU;  /* 5 bytes, aliased  */

    /* ---- UART windows: (offset, length) of each message payload --------- */
    static constexpr std::size_t kWin72Off = 0x00U, kWin72Len = 14U;
    static constexpr std::size_t kWin12Off = 0x13U, kWin12Len = 10U;
    static constexpr std::size_t kWin32Off = 0x1DU, kWin32Len = 14U;
    static constexpr std::size_t kWinA6Off = 0x2CU, kWinA6Len =  8U;

    /* ---- bits in byte 0x00 ---------------------------------------------- */
    static constexpr std::uint8_t kFlagAccOn    = 0x01U; /* gates the TX block*/
    static constexpr std::uint8_t kFlagIllumOn  = 0x02U;
    static constexpr std::uint8_t kFlagReverse  = 0x04U;
    static constexpr std::uint8_t kFlagSpare3   = 0x08U;
    static constexpr std::uint8_t kFlagIgnRaw   = 0x10U; /* unidentified      */
    /* Set by the CAN decode path, cleared by the link state machine once its
     * hold timer expires. NOTE the asymmetry, which is stock and deliberate:
     * the hold is ARMED off bit 2 -- the same bit the decoders call "reverse
     * gear" -- and CLEARED here on bit 5. Do not collapse the two. */
    static constexpr std::uint8_t kFlagCanErrLatch = 0x20U;
    static constexpr std::uint8_t kFlagAutobaud = 0x40U; /* phase-1 retrying  */
    static constexpr std::uint8_t kFlagProfileSel = 0x80U; /* latched profile */

    /* ---- bits in byte 0x15 (destination side of the door bitfield) ------
     * The source-to-destination mapping is NOT the identity; the decoder names
     * both sides so a crossed pair is visible. */
    static constexpr std::uint8_t kDoorTailgate   = 0x08U;
    static constexpr std::uint8_t kDoorRightRear  = 0x10U;
    static constexpr std::uint8_t kDoorLeftRear   = 0x20U;
    static constexpr std::uint8_t kDoorRightFront = 0x40U;
    static constexpr std::uint8_t kDoorLeftFront  = 0x80U;

    /* ---- raw access, for the UART framer and for tests ------------------ */
    [[nodiscard]] volatile std::uint8_t* window(std::size_t offset) noexcept
    {
        return &bytes_[offset];
    }
    [[nodiscard]] const volatile std::uint8_t* window(std::size_t offset) const noexcept
    {
        return &bytes_[offset];
    }
    [[nodiscard]] volatile std::uint8_t& at(std::size_t offset) noexcept
    {
        return bytes_[offset];
    }
    [[nodiscard]] std::uint8_t at(std::size_t offset) const noexcept
    {
        return bytes_[offset];
    }

    /* ---- named state ---------------------------------------------------- */
    [[nodiscard]] std::uint8_t flags() const noexcept { return bytes_[kFlags]; }
    [[nodiscard]] bool accOn()     const noexcept { return testFlag(kFlagAccOn); }
    [[nodiscard]] bool illumOn()   const noexcept { return testFlag(kFlagIllumOn); }
    [[nodiscard]] bool reverseOn() const noexcept { return testFlag(kFlagReverse); }

    void setFlag(std::uint8_t mask, bool value) noexcept
    {
        const std::uint8_t cur = bytes_[kFlags];
        bytes_[kFlags] = value ? static_cast<std::uint8_t>(cur | mask)
                               : static_cast<std::uint8_t>(cur & static_cast<std::uint8_t>(~mask));
    }
    [[nodiscard]] bool testFlag(std::uint8_t mask) const noexcept
    {
        return (bytes_[kFlags] & mask) != 0U;
    }

    void setSwcKey(std::uint8_t code) noexcept { bytes_[kSwcKey] = code; }
    [[nodiscard]] std::uint8_t swcKey() const noexcept { return bytes_[kSwcKey]; }

    void setIllumLevel(std::uint8_t percent) noexcept { bytes_[kIllumLevel] = percent; }
    [[nodiscard]] std::uint8_t illumLevel() const noexcept { return bytes_[kIllumLevel]; }

    /* Steering angle is stored big-endian across 0x04/0x05. */
    void setSteeringAngle(std::uint16_t angle) noexcept
    {
        bytes_[kSteerHi] = static_cast<std::uint8_t>(angle >> 8);
        bytes_[kSteerLo] = static_cast<std::uint8_t>(angle & 0xFFU);
    }
    [[nodiscard]] std::uint16_t steeringAngle() const noexcept
    {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes_[kSteerHi]) << 8) | bytes_[kSteerLo]);
    }

    void setDoorBit(std::uint8_t mask, bool open) noexcept
    {
        const std::uint8_t cur = bytes_[kDoors];
        bytes_[kDoors] = open ? static_cast<std::uint8_t>(cur | mask)
                              : static_cast<std::uint8_t>(cur & static_cast<std::uint8_t>(~mask));
    }
    [[nodiscard]] std::uint8_t doors() const noexcept { return bytes_[kDoors]; }

    void setGear(std::uint8_t prndl) noexcept { bytes_[kGear] = prndl; }
    [[nodiscard]] std::uint8_t gear() const noexcept { return bytes_[kGear]; }

    void setRpm(std::uint8_t hi, std::uint8_t lo) noexcept
    {
        bytes_[kRpmHi] = hi;
        bytes_[kRpmLo] = lo;
    }
    /* Engine speed is big-endian across 0x1F/0x20, 1 rpm per LSB. */
    [[nodiscard]] std::uint16_t rpm() const noexcept
    {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes_[kRpmHi]) << 8) | bytes_[kRpmLo]);
    }

    void setAmpLevel(std::uint8_t level) noexcept { bytes_[kAmpLevel] = level; }
    [[nodiscard]] std::uint8_t ampLevel() const noexcept { return bytes_[kAmpLevel]; }

    /* The amplifier run, 0x2C..0x31: volume then the five tone bytes,
     * addressed by the head unit's 1..6 selector. */
    static constexpr std::uint8_t kAmpSelFirst = 1U;
    static constexpr std::uint8_t kAmpSelLast  = 6U;
    [[nodiscard]] volatile std::uint8_t& ampByte(std::uint8_t selector) noexcept
    {
        return bytes_[kAmpLevel + selector - kAmpSelFirst];
    }

    void setAccShadow(std::uint8_t value) noexcept { bytes_[kAccShadow] = value; }
    [[nodiscard]] std::uint8_t accShadow() const noexcept { return bytes_[kAccShadow]; }

private:
    volatile std::uint8_t bytes_[kSize];
};

/* The window geometry IS the head-unit protocol, so it stays proved. */
static_assert(sizeof(VehicleState) == VehicleState::kSize,
              "VehicleState must be exactly its byte window");
static_assert(VehicleState::kWinA6Off + VehicleState::kWinA6Len == VehicleState::kSize,
              "the 0xA6 window is the last thing in the block");
static_assert(VehicleState::kWin12Off + 2U == VehicleState::kDoors,
              "the door bitfield is byte 2 of the 0x12 window");
static_assert(VehicleState::kWin32Off + 1U == VehicleState::kGear,
              "the gear is byte 1 of the 0x32 window");
static_assert(VehicleState::kAmpLevel == VehicleState::kWinA6Off,
              "the amplifier level is byte 0 of the 0xA6 window");
static_assert(VehicleState::kAmpTone == VehicleState::kAmpLevel + 1U,
              "the five tone bytes follow the volume byte");
static_assert(VehicleState::kVolume == VehicleState::kWin12Off + 4U,
              "the volume mirror is byte 4 of the 0x12 window");

/* PRNDL values the profile-1 gear decoder produces.
 *
 * Nibble 9 can never be observed as "leave the gear alone": 9 has bit 3 set,
 * and bit 3 is the reverse test two lines later in the stock decoder, so every
 * nibble from 8 to F reports R. */
inline constexpr std::uint8_t kGearInvalid = 0U;
inline constexpr std::uint8_t kGearPark    = 1U;
inline constexpr std::uint8_t kGearNeutral = 2U;
inline constexpr std::uint8_t kGearReverse = 3U;
inline constexpr std::uint8_t kGearDrive   = 4U;

/* ===========================================================================
 * Head-unit TX shadow copies                            stock @0x20001B98
 *
 * One "last sent" copy per outbound slot; the round-robin scheduler sends a
 * slot only when its live buffer differs from its shadow.
 * ========================================================================= */
struct UartShadows {
    volatile std::uint8_t msg72[14];
    volatile std::uint8_t msg12[10];
    volatile std::uint8_t msg32[14];
    volatile std::uint8_t msgA6[8];
    volatile std::uint8_t msg73[8];
    volatile std::uint8_t msgF0[17];
};

/* ===========================================================================
 * Decoded vehicle signals, in physical units -- NOT a stock structure.
 *
 * ram::VehicleState above is deliberately NOT this: it is the Hiworld wire
 * format, four overlapping message windows over one run of bytes, and its
 * offsets ARE the protocol. Nothing Benz-shaped can be stored in it.
 *
 * So the decoders that exist only to feed the Benz emulation write here, in
 * units that belong to the car rather than to either protocol, and
 * logic::benzPublish() packs these into Benz frames. If a third protocol is
 * ever added it reads this block too, and the decoders do not change.
 *
 * Every field is written ONLY when the car reports a usable value. The WK
 * sends an all-ones sentinel for "unavailable" on several of these ids, and
 * the Benz plugin has no concept of a blanked field -- it renders whatever
 * number arrives and freezes on the last one. Holding the previous reading is
 * therefore strictly better than forwarding 65535, which would display as
 * 65535 rpm or 6553.5 degrees. See logic::benzPublish() for the rest of that
 * reasoning.
 * ========================================================================= */
struct Telemetry {
    volatile std::uint16_t rpm;           /* 1 rpm                 0x002 b0:b1 */
    volatile std::uint16_t speedKph;      /* km/h            0x002 b2:b3 / 128 */
    volatile std::uint32_t odometerKm;    /* km, 24-bit    0x014 b0:b2 / 10    */
    volatile std::uint16_t ambientDeciC;  /* 0.1 degC, >= 0      0x014 b4 * 10 */
    volatile std::uint16_t coolantC;      /* degC, >= 0          0x02C b1 - 40 */
    volatile std::uint16_t deciVolts;     /* 0.1 V                    0x015 b1 */
};

/* ===========================================================================
 * The Benz outbound payloads and their change-detection shadows.
 *
 * Five messages: 0x08 rpm, 0x07 speed, and three pages of the multiplexed
 * 0x41 (page 1 doors, page 2 odometer + ambient, page 3 engine computer).
 * Payload byte 0 of each 0x41 page is the page selector and is written once
 * by logic::benzPublish(); the head unit dispatches on it.
 *
 * Sized exactly, because the head unit reads past what its own length gate
 * enforces: page 2 must be 6 bytes and page 3 must be 11 or the plugin
 * service throws an ArrayIndexOutOfBounds inside the radio.
 * ========================================================================= */
struct BenzFrames {
    volatile std::uint8_t rpm08[2];
    volatile std::uint8_t speed07[2];
    volatile std::uint8_t door41[2];      /* [0]=1, [1]=door bits             */
    volatile std::uint8_t trip41[6];      /* [0]=2, ambient, odometer         */
    volatile std::uint8_t engine41[11];   /* [0]=3, coolant, oil, V, hp, torque*/
};

/* ===========================================================================
 * Head-unit command block                               stock @0x20001BE7
 *
 * The 0xD2 (cluster text) and 0xAD (periodic-TX config) payloads the head unit
 * sends us, each followed by a one-byte "new data" flag the RX dispatcher sets
 * and the consumer clears. The old code reached the 0xAD payload twice, as
 * g_cmd_block[0x0E] and as g_periodic_tx_cfg; they are the same byte and now
 * there is one name for it.
 * ========================================================================= */
struct HeadUnitCommands {
    volatile std::uint8_t cluster[13];      /* cmd 0xD2 payload               */
    volatile std::uint8_t clusterNewData;
    volatile std::uint8_t periodicTxCfg[2]; /* cmd 0xAD payload               */
    volatile std::uint8_t periodicNewData;
};

/* ===========================================================================
 * Everything, in one object.
 *
 * Grouped by what owns it rather than by address, which is a freedom the fixed
 * map did not allow. The `stock @0x...` comments are the thread back to Ghidra
 * and to docs/.
 * ========================================================================= */
struct AppRam {
    /* ---- TIM2 divider chain ------------------------------- stock @0x1000 */
    volatile std::uint8_t tickDiv10ms;
    volatile std::uint8_t tickDiv100ms;
    volatile std::uint8_t tickDiv1s;
    volatile std::uint8_t fatalErrorCode;   /* last trap code  stock @0x1003 */

    /* ---- time --------------------------------------------- stock @0x10FC */
    SoftClock             softClock;
    volatile std::uint8_t blink;            /* kBlink* bits    stock @0x1104 */
    Scheduler             sched;            /*                 stock @0x1108 */

    /* ---- CAN ---------------------------------------------- stock @0x1118 */
    CanRingIndices        canRing;
    volatile std::uint32_t canLinkFlag;     /*                 stock @0x1130 */
    volatile std::uint32_t canRetryCtr;     /*                 stock @0x1134 */
    volatile std::uint8_t canErrFlags;      /* kCanErr* bits   stock @0x1138 */
    volatile std::uint8_t canBusStatus;     /* cfg::kBus*      stock @0x1139 */
    volatile std::uint8_t canActivityCtr;   /* bus-alive       stock @0x113A */
    volatile std::uint32_t busPresentDebounce; /* reloads to 800  stock @0x1194 */
    volatile std::uint32_t canReinitBitrate;   /*                 stock @0x1198 */
    volatile std::uint32_t canTxHoldTimer;     /*                 stock @0x119C */
    volatile std::uint8_t  canErrHoldTimer;    /*                 stock @0x11A0 */

    /* ---- outbound CAN frame templates --------------------- stock @0x1140 */
    volatile std::uint8_t echoBuf516[8];    /* vendor echo -> 0x516           */
    volatile std::uint8_t swcTxBuf[8];      /* SWC reply   -> 0x416           */
    volatile std::uint8_t txFrameAmp[7];    /* -> 0x3D0 / 0x3D9               */
    volatile std::uint8_t txFrameF0[5];     /* -> 0x0F0 / 0x2E9, dlc 4        */
    volatile std::uint8_t txFrame326[8];    /* -> 0x326 / 0x290               */
    volatile std::uint8_t txFrame190[7];    /* -> 0x190 / 0x291               */
    volatile std::uint8_t txFrame18c[8];    /* -> 0x18C / 0x293               */
    volatile std::uint8_t txFrame9f[8];     /* -> 0x09F                       */
    volatile std::uint8_t prndlTx7fe[1];    /* -> 0x7FE, dlc 1 stock @0x118E */

    /* ---- SWC / node addressing ---------------------------- stock @0x117F */
    SwcState              swc;
    volatile std::uint8_t periodicTxPhase;  /* 151-phase table stock @0x1188 */
    MiscTimers            miscTimers;       /*                 stock @0x118A */
    volatile std::uint8_t profileActive;    /* latched profile stock @0x118F */
    volatile std::uint8_t autobaudTries;    /* phase-1 attempts stock @0x1190 */
    volatile std::uint8_t rpmDecimator;     /* 1-in-21 on rpm  stock @0x11CE */
    volatile std::uint8_t d2ClusterState;   /*                 stock @0x10E0 */

    /* ---- head-unit link bookkeeping ----------------------- stock @0x11AA */
    volatile std::uint8_t uartRxOverflow;
    volatile std::uint8_t uartSlotState[cfg::kTxSlotCount]; /* one per TX slot  */
    volatile std::uint8_t uartTxSlotIdx;
    volatile std::uint8_t uartAuxSlotIdx;
    volatile std::uint8_t uartIdleDelay;
    volatile std::uint8_t uartTxTick;
    /* How many bytes of cfg::kIapResetFrame have matched so far. Only ever
     * non-zero in a Benz build, where the Hiworld "reboot into the loader"
     * frame has to be recognised by a side-matcher instead of by the parser
     * -- see the IAP escape hatch note in app_config.hpp. Not a stock field;
     * the stock firmware only ever spoke one dialect. */
    volatile std::uint8_t uartIapMatch;
    volatile std::uint8_t gpioInitialised;  /*                 stock @0x11B5 */
    volatile std::uint8_t cycleCtr;         /* TX slot rotation stock @0x11B7 */
    volatile std::uint8_t cycleCtrMax;

    /* ---- parameter page mirror ---------------------------- stock @0x11D0 */
    volatile std::uint8_t param[32];        /* live config bytes              */
    volatile std::uint8_t paramShadow[32];  /* last saved copy stock @0x1220 */

    /* ---- the two CAN rings -------------------------------- stock @0x1284 */
    volatile std::uint8_t canRxRing[kCanRingBytes];
    volatile std::uint8_t canTxRing[kCanRingBytes];  /*         stock @0x15A4 */
    volatile std::uint8_t canTxBusyBlock[0x14];      /*         stock @0x18E4 */

    /* ---- software timers ---------------------------------- stock @0x18F8 */
    volatile std::int32_t softTimers[kSoftTimerSlots];

    /* ---- head-unit framing buffer ------------------------- stock @0x19F8 */
    volatile std::uint8_t uartFrame[0x109];
    volatile std::uint8_t uartRxFrame[0x3C];/* parsed frame    stock @0x1B01 */
    volatile std::uint8_t uartAckQueue[12]; /*                 stock @0x1B3D */

    /* ---- vehicle state and the head-unit messages --------- stock @0x1B49 */
    VehicleState          vehicle;
    volatile std::uint8_t buf73[8];         /* cmd 0x73, always 0 stock @0x1B7D */
    UartShadows           uartShadows;      /*                 stock @0x1B98 */

    /* ---- the Benz emulation's own state ----------------------------------
     * Present in every build so the structure has one shape, but only
     * written when cfg::kIsBenz -- the Hiworld path never reads them and the
     * decoders that fill Telemetry are compiled out of a jeep image. */
    Telemetry             telemetry;
    BenzFrames            benzFrames;
    BenzFrames            benzShadows;
    HeadUnitCommands      hostCmd;          /*                 stock @0x1BE7 */
};

/* ===========================================================================
 * The state object.
 *
 * On target the linker places this in .bss and app/startup/startup.c zeroes it
 * along with every other static. Under the host test harness it is the same
 * object, which is what lets the decoders, the scheduler and the parameter
 * logic be unit-tested on a PC with no special casing at all -- the host build
 * no longer needs to substitute anything.
 * ========================================================================= */
extern AppRam g_appRam;

inline AppRam& app() noexcept { return g_appRam; }

/* ===========================================================================
 * The version string -- the one datum addressable from OUTSIDE the firmware.
 *
 * tools/iap_upload.py --verify reads these 17 bytes over SWD after a flash to
 * prove the new image actually booted (SRAM is readable under RDP-L1 while the
 * app runs; flash is not). It therefore needs a stable, linker-visible symbol,
 * which is why this is not a member of AppRam: the uploader resolves
 * `g_versionString` out of build/app.elf rather than having an address baked
 * into it.
 *
 * It is `extern "C"` so the name is not mangled and `nm` output is readable.
 * ========================================================================= */
extern "C" volatile std::uint8_t g_versionString[17];

}  // namespace ram

#endif /* RAM_MAP_HPP */
