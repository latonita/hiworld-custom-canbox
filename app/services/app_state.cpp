/* ============================================================================
 * app_state.cpp -- the definition of the firmware's runtime state.
 *
 * One object, in .bss, zeroed by app/startup/startup.c like any other static.
 * Declared in app/include/ram_map.hpp; see the header for what lives in it and
 * for the history of why it used to sit at fixed absolute SRAM addresses.
 *
 * This is deliberately a translation unit of its own. It is the only place the
 * state is DEFINED, so "who owns this memory" has a one-file answer, and the
 * host test build links the same object as the firmware rather than
 * substituting one of its own.
 * ==========================================================================*/

#include "ram_map.hpp"

#include "app_config.hpp"

namespace ram {

/* Zero-initialised: it lands in .bss, which startup.c clears before main_loop.
 * Nothing here may rely on a non-zero initial value -- a constructor would not
 * run (this image never executes .init_array) and an initialiser would put the
 * object in .data, which costs flash to no purpose. */
AppRam g_appRam{};

/* The 17-byte version string, outside AppRam so the flashing tool can resolve
 * it by symbol name. See the note in ram_map.hpp.
 *
 * Also .bss, not .rodata: the head-unit link COPIES the string in at init, and
 * tools/iap_upload.py --verify reads the copy back over SWD. Reading it is how
 * you know the image booted, so a value that was merely linked into flash
 * would prove nothing.
 *
 * NOTE the `= {}`: inside a linkage-specification a declaration is treated as
 * though it carried `extern`, so `extern "C" uint8_t x[17];` would declare and
 * never define. The empty initialiser makes this the definition, and since it
 * is all zeroes the object still lands in .bss rather than costing flash. */
extern "C" {
volatile std::uint8_t g_versionString[cfg::kVersionLength] = {};
}
static_assert(sizeof(g_versionString) == cfg::kVersionLength,
              "the uploader reads exactly this many bytes back over SWD");

}  // namespace ram
