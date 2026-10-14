/* ============================================================================
 * board_io.hpp -- the discrete, hardwired lines of THIS box.
 *
 * Everything here is board policy, not a register driver: which pin carries
 * which signal, which way round the CAN transceiver's standby line runs, and
 * what has to be armed before the part is allowed to sleep. The register-level
 * work belongs to app/hal/hal_gpio.c, app/hal/hal_exti.c and app/hal/hal_nvic.c;
 * this unit only composes them.
 *
 * Ported from reference/decompile/app/gpio.c and reference/decompile/app/misc.c (Ghidra output) via the
 * commented C port in the retired app/port/hw_gpio.c (gpio_status_lines_init,
 * exti_pb8_wake_config) and the retired app/port/app_glue.c (gpio_write_status_lines,
 * gpio_static_init_b, gpio_can_xcvr_standby_pb12, gpio_can_xcvr_standby_on_pb12).
 * Behaviour is identical; only the layering moved.
 *
 * THE LINES ON THIS BOARD
 *
 *   PB5   status bit 0                      PC15  status bit 1 = ILLUMINATION
 *   PB6   status bit 2 = REVERSE            PB4   status bit 3
 *   PB2   static input                      PB3   static input (sleep path)
 *   PB8   CAN_RX, doubling as EXTI8 bus wake
 *   PB12  CAN transceiver standby  (HIGH = standby, LOW = normal mode)
 *
 * The four status lines are REAL, hardwired signals the Android head unit reads
 * directly, independent of the CAN and UART protocols. PB6 (reverse / back-up
 * camera trigger) and PC15 (illumination / dimming) are the two that are
 * visibly wrong in the car if they get crossed -- see docs/hiworld-gpio-io.md.
 * ==========================================================================*/

#ifndef BOARD_IO_HPP
#define BOARD_IO_HPP

namespace svc {
namespace io {

/* --- 0x08008458 gpio_status_lines_init (gpio_static_init_a in the decompile) ---
 * Boot-time bring-up of the four head-unit status lines.
 *
 * Order matters: each line is driven low BEFORE its pin becomes an output, so
 * the lines never glitch high while the direction changes. PB2 is then made a
 * plain INPUT -- docs/hiworld-gpio-io.md calls PB2 a "static output forced
 * low", but the image disagrees: the call is gpio_init_pin(GPIOB, 0x4, 0),
 * i.e. PMODE = 00 = input, and the preceding writes never touch PB2.
 *
 * Finally ram::app().gpioInitialised (the stock flag at 0x200011B5) is set to
 * 0xFF. */
void statusLinesInit() noexcept;

/* --- 0x080084F6 gpio_write_status_lines ---
 * The 8 ms fan-out. Copies the low nibble of ram::app().vehicle byte 0 out to
 * the four lines:
 *
 *      kFlagAccOn   (bit 0) -> PB5        kFlagIllumOn (bit 1) -> PC15
 *      kFlagReverse (bit 2) -> PB6        kFlagSpare3  (bit 3) -> PB4
 *
 * The stock code extracts each bit with a mask-then-shift pair
 * ((b & 7) >> 2, b & 1, (b & 0xf) >> 3, (b & 3) >> 1), which is exactly
 * "bit n" for n = 2, 0, 3, 1. The call ORDER is kept as in the image
 * (PB6, PB5, PB4, PC15) in case anything downstream is edge-sensitive. */
void writeStatusLines() noexcept;

/* --- 0x080084BE gpio_static_init_b ---
 * The teardown counterpart of statusLinesInit(), run on the sleep / firmware-
 * update path so nothing is being driven while the box is asleep.
 *
 * Three differences from the init block, ALL deliberate and all reproduced:
 *   - PB5 (0x20) is NOT touched here, only PC15 / PB6 / PB4;
 *   - the pins are driven low but NOT reconfigured as outputs (they already
 *     are), while PB3 (0x8) and PB2 (0x4) ARE reconfigured, to mode 0 = input;
 *   - ram::app().gpioInitialised is left alone. */
void statusLinesShutdown() noexcept;

/* --- 0x08007430 (false) / 0x08007440 (true) ---
 * PB12 drives the CAN transceiver's STB pin. The transceiver (an NSI/TJA-class
 * part) idles in standby with STB high; the CAN bring-up pulls it low to put
 * the transceiver into normal mode, and the phase-1 give-up path puts it back
 * high before the MCU sleeps so the quiescent current drops.
 *
 *      standby == true  -> PB12 HIGH, transceiver asleep   (0x08007440)
 *      standby == false -> PB12 LOW,  normal mode          (0x08007430)
 *
 * The two stock entry points differ only in that one argument, which is why
 * they collapse into one function here. Note the Ghidra listing DROPS the third
 * argument of the 0x08007440 call (`gpio_write_pin(GPIOB,0x1000)`); the
 * instruction stream passes 1, hence "standby ON = PB12 high". */
/* Make PB12 a push-pull output. Called by the CAN bring-up, which owns WHEN
 * the transceiver line is configured but not WHICH pin it is -- that belongs
 * here, with the rest of this board's wiring, so a board respin is one edit.
 *
 * Splitting it the other way is what this replaced: the direction was set in
 * svc::can from its own copy of the pin mask while the level was driven here
 * from this one. Moving the pin then left the old pin configured as an output
 * and the new one never configured at all -- a silent, CAN-dead box. */
void configureCanTransceiverPin() noexcept;      /* 0x08007482 part */

void setCanTransceiverStandby(bool standby) noexcept;

/* --- 0x08003222 update_path_exti_setup ---
 * Arms PB8 (= CAN_RX) as a falling-edge EXTI8 interrupt just before the part
 * is put into STOP. Any CAN bus activity then pulls the line low, EXTI9_5
 * fires, and the handler resets the box through the watchdog -- i.e. "wake"
 * means "reboot into the IAP bootloader", which is exactly what the update
 * path wants.
 *
 * *** THIS IS THE ONLY WAKE SOURCE ARMED BEFORE THE DEEP-SLEEP WFI. ***
 * If it does not do its job the box sleeps and never comes back without a
 * power cycle. */
void configurePb8Wake() noexcept;

}  // namespace io
}  // namespace svc

#endif /* BOARD_IO_HPP */
