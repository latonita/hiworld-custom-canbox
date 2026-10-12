/* ============================================================================
 * sys.c -- fatal-error path, ported from app/src/sys.c (Ghidra).
 * ==========================================================================*/

#include "port.h"

/* --- 0x08002e64 fatal_error_reset ---
 * Record the code where a debugger / the next boot can find it, re-arm the
 * watchdog (in case the fault happened before wwdg_init ran), then force a
 * reset. Never returns: the reset drops back into the IAP bootloader, which is
 * why a wedged app can always still be re-flashed.
 *
 * Codes are the stock ones -- 100..164, assigned per interrupt veneer slot in
 * app/startup/crt0.S, so a trap tells you which vector fired. */
void fatal_error_reset(uint8_t code)
{
    for (;;) {
        g_fatal_error_code = code;
        wwdg_init();
        wwdg_force_reset();
    }
}
