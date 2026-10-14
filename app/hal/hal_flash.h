/* ============================================================================
 * app/hal/hal_flash.h -- on-chip flash controller driver (HAL layer).
 *
 * LAYER CONTRACT: pure C11, MMIO only. No RAM state, no globals.h, no C++.
 * Every entry point carries the flash address of the stock function it
 * reproduces so the rebuild can be diffed against
 * reference/2429-H1N4JPF20A-220524.app.bin.
 *
 * Ported from the FLASH primitives in the retired app/port/param.c / the retired app/port/param.h,
 * which came from reference/decompile/app/flash.c (Ghidra output, vendor FLASH driver).
 *
 * ---------------------------------------------------------------------------
 * SAFETY: the only flash this firmware may ever erase or program at runtime is
 * the LAST 2 KB page, 0x0801F800..0x0801FFFF. Everything below it is the IAP
 * bootloader (0x08000000..0x08001FFF, write-protected, and the only way to
 * reflash the box) and the app image itself (0x08002000..0x0801F7FF). The
 * erase and program entry points below enforce that in code; see hal_flash.c.
 * ---------------------------------------------------------------------------
 *
 * NOT HERE (parameter *store*, RAM state -- services layer): param_page_load
 * @0x08002dbc, param_page_save @0x08002d46, param_validate_defaults
 * @0x08007b6c, and the PARAM_* byte-index constants. This unit knows the page
 * only as an address range to defend, never as a data structure.
 * ==========================================================================*/

#ifndef HAL_FLASH_H
#define HAL_FLASH_H

#include <stdint.h>

#include "n32l406_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the parameter page -------------------------------------------------
 * These three constants are what the bounds guard is written against. They
 * are not a convenience: changing them changes which flash this firmware is
 * allowed to destroy. */
#define PARAM_PAGE_BASE     0x0801F800U   /* literal at 0x08002e60 in stock   */
#define PARAM_PAGE_SIZE     0x00000800U   /* 2 KB = one N32L406 flash page    */
#define PARAM_PAGE_LAST     0x0801FFFFU   /* inclusive end of the page        */

/* ---- flash status codes -------------------------------------------------
 * Numeric values are taken verbatim from the stock FLASH_GetBank1Status
 * @0x08004716 / the callers; the NAMES for 3, 4, 5 and 7 are inferred from
 * which FLASH_STS bit produces them and are NOT confirmed against the vendor
 * header. Only COMPL (6), TIMEOUT (10) and ALIGN (9) matter to this driver. */
typedef enum {
    FLASH_STS_BUSY      = 1,   /* STS bit0  BUSY                             */
    FLASH_STS_ERR_PG    = 3,   /* STS bit2  -- inferred: program error       */
    FLASH_STS_ERR_PV    = 4,   /* STS bit3  -- inferred: program verify err  */
    FLASH_STS_ERR_WRP   = 5,   /* STS bit4  -- inferred: write-protect err   */
    FLASH_STS_COMPL     = 6,   /* no error bit set -> operation complete     */
    FLASH_STS_ERR_EV    = 7,   /* STS bit6  -- inferred: erase verify err    */
    FLASH_STS_ERR_OB    = 8,   /* option-byte precheck failed (DEAD paths)   */
    FLASH_STS_ERR_ALIGN = 9,   /* address not 4-byte aligned                 */
    FLASH_STS_TIMEOUT   = 10,  /* wait loop ran out                          */
    /* NOT a stock code: this rebuild's bounds check refused the address.
     * Chosen outside the stock 1..10 range so it can never be mistaken for a
     * controller status. */
    FLASH_STS_REFUSED   = 0x7F
} flash_sts_t;

/* ---- flash primitives (reference/decompile/app/flash.c) --------------------------------- */
void flash_set_latency(uint32_t latency);                 /* 0x080045fc */
void flash_set_prefetch(uint32_t prefetch);               /* 0x0800460e */
void flash_unlock(void);                                  /* 0x080046fc */
void flash_lock(void);                                    /* 0x08004708 */
int  flash_get_status(void);                              /* 0x08004716 */
int  flash_wait_for_last_operation(uint32_t timeout);     /* 0x08004762 */
void flash_clear_status_flags(uint32_t flags);            /* 0x08004788 */

/* Both of these REFUSE any address outside PARAM_PAGE_BASE..PARAM_PAGE_LAST
 * and return FLASH_STS_REFUSED without touching the controller. */
int  flash_erase_page(uint32_t page_addr);                /* 0x08004794 */
int  flash_program_word(uint32_t addr, uint32_t data);    /* 0x08004a96 */

/* Read one 32-bit word of flash.
 *
 * The stock code dereferences the parameter page directly. It is routed through
 * a function here for one reason: it is the ONLY read path into the page, so
 * making it an explicit HAL entry point is what lets the parameter store be
 * unit-tested on a host, where 0x0801F800 is not mapped. On target this is a
 * single ldr and costs nothing.  (no stock counterpart -- rebuild only) */
uint32_t flash_read_word(uint32_t addr);


#ifdef __cplusplus
}
#endif

#endif /* HAL_FLASH_H */
