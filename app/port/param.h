/* ============================================================================
 * param.h -- on-chip flash driver + the parameter (emulated EEPROM) page.
 *
 * Ported from app/src/flash.c (the FLASH primitives) and app/src/sys.c /
 * app/src/can.c (the param page load / save / validate routines).
 *
 * SAFETY: the only flash this firmware may ever erase or program at runtime is
 * the LAST 2 KB page, 0x0801F800..0x0801FFFF. Everything below it is the IAP
 * bootloader (0x08000000..0x08001FFF, write-protected, and the only way to
 * reflash the box) and the app image itself (0x08002000..0x0801F7FF). The
 * erase and program entry points below enforce that in code; see param.c.
 * ==========================================================================*/

#ifndef PARAM_H
#define PARAM_H

#include <stdint.h>

/* ---- the parameter page ------------------------------------------------- */
#define PARAM_PAGE_BASE     0x0801F800U   /* literal at 0x08002e60 in stock   */
#define PARAM_PAGE_SIZE     0x00000800U   /* 2 KB = one N32L406 flash page    */
#define PARAM_PAGE_LAST     0x0801FFFFU   /* inclusive end of the page        */

/* 32 config bytes, stored ONE BYTE PER 32-BIT WORD (so they occupy the first
 * 128 bytes of the page); mirrored to RAM at g_param_struct / g_param_shadow. */
#define PARAM_COUNT         32U

/* Byte meanings, as far as they are known (see param.c for the full table). */
#define PARAM_MAGIC         0x00U         /* must read 0xAA, else defaults    */
#define PARAM_PROFILE       0x01U         /* CAN profile / bit rate, 0 or 1   */
#define PARAM_VOLUME        0x10U         /* stored amplifier volume          */
#define PARAM_TONE_FIRST    0x11U         /* BAL, FAD, BASS, MID, TRE (5 B)   */
#define PARAM_TONE_COUNT    5U
#define PARAM_MAGIC_VALUE   0xAAU

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

/* ---- flash primitives (app/src/flash.c) --------------------------------- */
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

/* ---- the parameter page ------------------------------------------------- */
void param_page_load(void);        /* 0x08002dbc  flash -> RAM + shadow      */
void param_page_save(void);        /* 0x08002d46  RAM -> flash, if dirty     */
void param_validate_defaults(void);/* 0x08007b6c  magic/clamp + prime blocks */

#endif /* PARAM_H */
