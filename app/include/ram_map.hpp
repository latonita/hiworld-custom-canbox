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

    /* The custom 0xB0-0xB8 telemetry block (docs/TELEMETRY-V2-SPEC.md). NOT
     * stock -- there is no `stock @` address for these because the stock
     * firmware has no such slots. They are APPENDED rather than interleaved so
     * the six above keep the stock RAM order a reader can diff against Ghidra.
     * Written only in a jeep build; present in every build so the structure
     * has one shape. msgB3 is kept although 0xB3 is sent every 100 ms
     * whatever it holds: the row machinery stays uniform. */
    volatile std::uint8_t msgB0[4];
    volatile std::uint8_t msgB2[7];
    volatile std::uint8_t msgB3[11];
    volatile std::uint8_t msgB4[5];
    volatile std::uint8_t msgB5[3];
    volatile std::uint8_t msgB7[5];
    volatile std::uint8_t msgB8[6];
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
    volatile std::uint16_t ambientDeciC;  /* 0.1 degC, >= 0      0x014 b4:b5, shared */
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
 * THE CUSTOM 0xB0-0xB8 TELEMETRY BLOCK -- NOT a stock structure.
 *
 * Layout v2: docs/TELEMETRY-V2-SPEC.md is the contract (section 14 of it is
 * the wire summary a decoder is written from). The evidence for every CAN
 * byte is docs/JEEP-WK-CAN-ID-byte-map.md.
 *
 * TWO STRUCTS, FOR THE SAME REASON THE BENZ PATH HAS TWO. CustomSignals is what
 * the CAN decoders write: the LATEST RAW READING of each source plus the
 * timestamp of its last accepted frame. CustomFrames is the packed big-endian
 * payload the TX descriptor table points at. logic::customTick100() and
 * customTick1000() turn the first into the second, and that is the one place
 * a dead source becomes a sentinel.
 *
 * ---------------------------------------------------------------------------
 * THE SENTINEL RULE -- read this before touching a default.
 *
 * Every field has its own "no value" code (spec section 8) and there is no
 * group validity mask any more:
 *   - unsigned fields: ALL ONES (0xFF / 0xFFFF / 0xFFFFFF).
 *   - the one signed field, outside air: 0x8000, the most negative s16.
 *   - the ignition enum: 7. The 4WD enum: 0.
 *   - single bits cannot hold a sentinel, so 0xB2 carries a source-alive byte
 *     (S6) and a bit whose source is dead is written 0.
 *
 * STALENESS LIVES HERE NOW. Each CAN source has one u32 stamp (sched.counter,
 * 1 kHz, wrap-safe subtraction) written by its decoder on every ACCEPTED
 * frame, and a "seen since boot" bit. A source is alive while
 *     seen && (now - lastMs) <= limit          (3 x nominal period)
 * and a dead source's fields go to their sentinels at the next 100 ms tick.
 * ========================================================================= */

/* The "no value" encodings. */
inline constexpr std::uint8_t  kCustomUnknown8    = 0xFFU;
inline constexpr std::uint16_t kCustomUnknown16   = 0xFFFFU;
/* The 24-bit odometer. The car's own "unavailable" code for 0x014 b0:b1:b2 is
 * also 0xFFFFFF, so source and wire encoding coincide and the copy is
 * verbatim. */
inline constexpr std::uint32_t kCustomUnknown24   = 0x00FFFFFFU;
inline constexpr std::int16_t  kCustomUnknownS16  = static_cast<std::int16_t>(-32768);
inline constexpr std::uint8_t  kIgnEnumUnknown    = 7U;
inline constexpr std::uint8_t  kTransferUnknown   = 0U;

/* The CAN sources the block ages, as indexes into CustomSignals::lastMs. The
 * limits (3 x nominal period, spec section 6) are in custom_decode.cpp. */
inline constexpr std::uint8_t kSrc000   = 0U;   /* ignition ladder          */
inline constexpr std::uint8_t kSrc002   = 1U;   /* rpm, speed               */
inline constexpr std::uint8_t kSrc003   = 2U;   /* telltales, gear, brake   */
inline constexpr std::uint8_t kSrc006   = 3U;   /* lights, RAP              */
inline constexpr std::uint8_t kSrc012   = 4U;   /* key in                   */
inline constexpr std::uint8_t kSrc013   = 5U;   /* doors, belt              */
inline constexpr std::uint8_t kSrc014   = 6U;   /* odometer, outside air    */
inline constexpr std::uint8_t kSrc015   = 7U;   /* battery volts            */
inline constexpr std::uint8_t kSrc02C   = 8U;   /* coolant, oil pressure    */
inline constexpr std::uint8_t kSrc0D0   = 9U;   /* parking brake            */
inline constexpr std::uint8_t kSrc159   = 10U;  /* fuel, minutes since RUN  */
inline constexpr std::uint8_t kSrc1A7   = 11U;  /* fuel flow                */
inline constexpr std::uint8_t kSrc1B2   = 12U;  /* transfer case            */
inline constexpr std::uint8_t kSrc1B4   = 13U;  /* compass                  */
inline constexpr std::uint8_t kSrc2C0   = 14U;  /* boost / MAP              */
inline constexpr std::uint8_t kSrcCount = 15U;

/* 0xB7 tyre positions, as the WCM orders them in its 0x213B reply:
 * b3=LF b4=RF b5=RR b6=LR. TRIANGULATED, NOT PROVEN -- three independent
 * sources agree and a fourth swaps the rears, and no deflation test has ever
 * been done. If the head unit shows the wrong corner, change these four lines
 * and nothing else. */
inline constexpr std::uint8_t kTyrePosLeftFront  = 0U;
inline constexpr std::uint8_t kTyrePosRightFront = 1U;
inline constexpr std::uint8_t kTyrePosRightRear  = 2U;
inline constexpr std::uint8_t kTyrePosLeftRear   = 3U;
inline constexpr std::uint8_t kTyrePosSpare      = 4U;   /* deferred, not sent */
inline constexpr std::uint8_t kTyreCount         = 4U;   /* the spare is extra */

/* Bits of the 0xB7 flags byte. Battery and leak come from the per-wheel
 * 21 2n read, low-pressure warning from 21 25+n, no signal from the pressure
 * poll itself. docs/TPMS-POLL-SPEC.md. */
inline constexpr std::uint8_t kTyreFlagBatteryLow = 0x01U;
inline constexpr std::uint8_t kTyreFlagLeak       = 0x02U;
inline constexpr std::uint8_t kTyreFlagNoSignal   = 0x04U;
inline constexpr std::uint8_t kTyreFlagLowPressure = 0x08U;

/* 0xB0 byte 0. Diagnostics only: no consumer branches on it (one protocol
 * version, firmware and decoder change together). */
inline constexpr std::uint8_t kCustomLayoutVersion = 0x02U;

/* Indicator debounce state bits (CustomSignals::indActive) and the matching
 * indexes into CustomSignals::indUntil. */
inline constexpr std::uint8_t kIndLeft   = 0x01U;
inline constexpr std::uint8_t kIndRight  = 0x02U;
inline constexpr std::uint8_t kIndHazard = 0x04U;
inline constexpr std::uint8_t kIndTimers = 3U;

struct CustomSignals {
    /* ---- source ages (spec section 6) ----------------------------------- */
    volatile std::uint32_t lastMs[kSrcCount];  /* sched.counter at last accept */
    volatile std::uint16_t srcSeen;            /* bit n: source n seen since boot */
    volatile std::uint16_t srcAlive;           /* bit n: alive at the last tick */

    /* ---- latest raw readings, as the car sent them ---------------------- */
    volatile std::uint8_t  ign;             /* 0x000 b0                       */
    volatile std::uint16_t rpm;             /* 0x002 b0:b1, 0xFFFF n/a        */
    volatile std::uint16_t speedRaw;        /* 0x002 b2:b3, km/h x128         */
    volatile std::uint8_t  lamps0;          /* 0x003 b0                       */
    volatile std::uint8_t  lamps1;          /* 0x003 b1                       */
    volatile std::uint8_t  lever;           /* 0x003 b2                       */
    volatile std::uint8_t  brake;           /* 0x003 b3                       */
    volatile std::uint8_t  prndl;           /* 0x003 b4, ASCII                */
    volatile std::uint8_t  lights;          /* 0x006 b0                       */
    volatile std::uint8_t  rapByte;         /* 0x006 b1                       */
    volatile std::uint8_t  keyByte;         /* 0x012 b2                       */
    volatile std::uint8_t  beltByte;        /* 0x013 b0                       */
    volatile std::uint8_t  doorByte;        /* 0x013 b2                       */
    volatile std::uint32_t odometerDeciKm;  /* 0x014 b0:b2, verbatim          */
    volatile std::int16_t  outsideDeciC;    /* 0x014 b4:b5, HELD over FFFF    */
    volatile std::uint8_t  volts;           /* 0x015 b1, 0.1 V                */
    volatile std::uint8_t  oil;             /* 0x02C b0                       */
    volatile std::uint8_t  coolant;         /* 0x02C b1, degC+40              */
    volatile std::uint8_t  pedal;           /* 0x02C b2, 0.4 %/count          */
    volatile std::uint8_t  parkByte;        /* 0x0D0 b0                       */
    volatile std::uint8_t  minutesOff;      /* 0x159 b0                       */
    volatile std::uint8_t  fuelPct;         /* 0x159 b5, HELD on the gate     */
    volatile std::uint16_t fuelDecilitres;  /* 0x159 b5 x5, HELD on the gate  */
    volatile std::uint8_t  transfer;        /* 0x1B2 b1                       */
    volatile std::uint8_t  compassSector;   /* 0x1B4 b0, last accepted        */
    volatile std::uint16_t headingDeg;      /* 0x1B4 b1 x2, last accepted     */
    volatile std::uint8_t  boost;           /* 0x2C0 b4, 8 hPa/count          */

    /* ---- engine-running state machine (spec section 5) ------------------ */
    volatile std::uint8_t  runStreak;
    volatile std::uint8_t  engineRun;

    /* ---- 0x1A7 fuel flow: a sample-and-hold integral over each 100 ms
     * window. flowQ is the held Q (0xFFFF = none), flowMarkMs the time up to
     * which it has been integrated. Each counted interval is <= 1200 ms and a
     * window spans ~100 ms, so sumQdt stays far inside u32 (the packer also
     * halves both sums if a stalled loop ever lets them grow). ------------ */
    volatile std::uint16_t flowQ;
    volatile std::uint32_t flowMarkMs;
    volatile std::uint32_t flowSumQdt;
    volatile std::uint32_t flowSumDt;
    volatile std::uint16_t flowCentiLph;    /* last window, 0.01 L/h          */
    volatile std::uint8_t  loadPct;         /* last window, 0xFF n/a          */

    /* ---- indicator debounce (spec section 3.2.1) ----------------------- */
    volatile std::uint32_t indUntil[kIndTimers]; /* left, right, hazard      */
    volatile std::uint8_t  indActive;       /* kInd* bits                     */

    /* ---- the 100 ms producer --------------------------------------------- */
    volatile std::uint32_t nextTickMs;      /* drift-free: += 100             */
    volatile std::uint8_t  tickCount;       /* 0..9, the 1 s task on 0        */
    volatile std::uint8_t  stateAge;        /* ticks since 0xB2 was queued    */
    volatile std::uint8_t  stateLast[7];    /* 0xB2 payload last queued       */
    volatile std::uint8_t  heartbeat;       /* 0xB0 rolling counter           */

    /* ---- TPMS, polled from the WCM. docs/TPMS-POLL-SPEC.md ---------------
     * NOT broadcast on this car: these come from a KWP request/response
     * exchange the box initiates, which is the only place this firmware
     * transmits a frame nobody asked it for. Order LF, RF, RR, LR. */
    volatile std::uint16_t tyreKpa[4];      /* kPa, 0xFFFF unknown  0x500 b3..b6 */
    volatile std::uint16_t tpmsPollDiv;     /* 8 ms passes since the last poll   */
    volatile std::uint16_t tpmsRotateDiv;   /* 8 ms passes since the last rotate */
    volatile std::uint8_t  tpmsPosition;    /* which corner 0xB7 is carrying     */
    volatile std::uint8_t  tpmsPending;     /* a request is outstanding          */
    volatile std::uint8_t  tpmsDeadline;    /* 8 ms passes left to answer        */
    volatile std::uint8_t  tpmsFailures;    /* consecutive failed 21 3B polls    */
    volatile std::uint8_t  tpmsLid;         /* local id of the request in flight */
    volatile std::uint8_t  tpmsStep;        /* next request: 0=21 3B 1=21 2n 2=21 25+n */
    volatile std::uint8_t  tpmsWheel;       /* wheel n of the next 21 2n / 21 25+n */
    /* Per-wheel extras, held between reads. tyreTemp is the raw reply byte
     * (degC + 50), 0xFF = none. tyreExtra holds 0xB7 flag bits 0, 1 (from
     * 21 2n) and 3 (from 21 25+n). Miss counters: consecutive failed reads. */
    volatile std::uint8_t  tyreTemp[4];
    volatile std::uint8_t  tyreExtra[4];
    volatile std::uint8_t  tyreMissTemp[4];
    volatile std::uint8_t  tyreMissDiag[4];
};

/* The packed payloads the TX descriptor table points at; their change-detection
 * shadows are in UartShadows above. Sized EXACTLY: a row's length is copied out
 * of here with no bounds check, and hu_hiworld.cpp static_asserts every one of
 * these against both its table row and its shadow. */
struct CustomFrames {
    volatile std::uint8_t b0[4];   /* layout version, capability u16, counter  */
    volatile std::uint8_t b2[7];   /* STATE S0..S6                             */
    volatile std::uint8_t b3[11];  /* speed, rpm, load, flow, oil, boost, pedal */
    volatile std::uint8_t b4[5];   /* coolant, volts, fuel %, fuel litres      */
    volatile std::uint8_t b5[3];   /* outside air s16, minutes since RUN       */
    volatile std::uint8_t b7[5];   /* ONE tyre: position, kPa, temp, flags     */
    volatile std::uint8_t b8[6];   /* odometer u24, sector, heading            */
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

    /* ---- ACC sources (NOT stock; cfg::kAccFollowsRap) ----------------------
     * kFlagAccOn is recomputed from these by logic::shared::recomputeAcc().
     * accIgnOn: 0x000 b0 ladder says ACC/RUN/CRANK. accRapOn: 0x006 b1 bit 1.
     * The *SeenMs stamps are sched.counter at the last frame of each id. */
    volatile std::uint8_t  accIgnOn;
    volatile std::uint8_t  accRapOn;
    volatile std::uint32_t accIgnSeenMs;
    volatile std::uint32_t accRapSeenMs;

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
    /* NOT a stock field (REQUIREMENTS FR-144): 1024 ms ticks the periodic save
     * still has to wait out after the last 0xAD amp change. 0 = settled, save
     * allowed -- which is also the .bss value, so a boot behaves as stock. */
    volatile std::uint8_t paramSettleTicks;

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

    /* ---- the custom 0xB0-0xB8 telemetry block's own state (v2) -----------
     * The mirror image of the Benz pair above: present in every build so the
     * structure has one shape, written only when !cfg::kIsBenz. The shadows
     * live in uartShadows beside the stock six, because the TX scheduler
     * reaches them through the same descriptor table. */
    CustomSignals         customSignals;
    CustomFrames          customFrames;

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
