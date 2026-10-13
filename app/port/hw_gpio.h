/* ============================================================================
 * hw_gpio.h -- GPIO + EXTI API of the reconstructed canbox APP.
 *
 * Ported from app/src/gpio.c, app/src/core.c and app/src/misc.c (Ghidra).
 * See hw_gpio.c for the full commentary; the short version is that this part
 * is a Nations N32L406, whose GPIO block is F4/L4-shaped (PMODE/POTYPE/PUPD +
 * per-pin AF numbers in AFL/AFH) and NOT STM32F1-shaped (no CRL/CRH, no AFIO
 * remap register). AFIO here is only { RMP_CFG, EXTI_CFG[4] }.
 * ==========================================================================*/

#ifndef HW_GPIO_H
#define HW_GPIO_H

#include <stdint.h>
#include "n32l406_hw.h"

/* ---- pin masks ---------------------------------------------------------- */
#define GPIO_PIN(n)             (1UL << (n))
#define GPIO_PIN_2              0x0004U   /* PB2  static input                */
#define GPIO_PIN_3              0x0008U   /* PB3  static input                */
#define GPIO_PIN_4              0x0010U   /* PB4  status line (state bit 3)   */
#define GPIO_PIN_5              0x0020U   /* PB5  status line (state bit 0)   */
#define GPIO_PIN_6              0x0040U   /* PB6  status line = REVERSE       */
#define GPIO_PIN_8              0x0100U   /* PB8  CAN_RX / EXTI8 bus wake     */
#define GPIO_PIN_9              0x0200U   /* PB9  CAN_TX                      */
#define GPIO_PIN_12             0x1000U   /* PB12 CAN transceiver standby     */
#define GPIO_PIN_15             0x8000U   /* PC15 status line = ILLUM         */

/* ---- cfg.mode -----------------------------------------------------------
 * Low nibble drives PMODE (2 bits) and, through bit 4, POTYPE (0 = push-pull,
 * 1 = open-drain). Bit 28 means "this pin also feeds an EXTI line", and then
 * bits 16/17/20/21 select IMASK / EMASK / RT_CFG / FT_CFG. The encoding is the
 * vendor SDK's GPIO_ModeType; the stock firmware only ever uses the values
 * spelled out below. */
#define GPIO_MODE_INPUT         0x00000000U
#define GPIO_MODE_OUTPUT_PP     0x00000001U
#define GPIO_MODE_AF_PP         0x00000002U
#define GPIO_MODE_ANALOG        0x00000003U
#define GPIO_MODE_OUTPUT_OD     0x00000011U
#define GPIO_MODE_AF_OD         0x00000012U

#define GPIO_MODE_EXTI          0x10000000U   /* "configure EXTI too"        */
#define GPIO_MODE_EXTI_IT       0x00010000U   /* -> EXTI.IMASK               */
#define GPIO_MODE_EXTI_EVT      0x00020000U   /* -> EXTI.EMASK               */
#define GPIO_MODE_EXTI_RISING   0x00100000U   /* -> EXTI.RT_CFG              */
#define GPIO_MODE_EXTI_FALLING  0x00200000U   /* -> EXTI.FT_CFG              */

/* The one composite value the stock image uses, literal 0x10210000 from the
 * pool at 0x0800339c: input pin + EXTI interrupt on the falling edge. */
#define GPIO_MODE_INPUT_EXTI_IT_FALLING \
    (GPIO_MODE_INPUT | GPIO_MODE_EXTI | GPIO_MODE_EXTI_IT | GPIO_MODE_EXTI_FALLING)

/* ---- cfg.pull ----------------------------------------------------------- */
#define GPIO_PULL_NONE          0U
#define GPIO_PULL_UP            1U
#define GPIO_PULL_DOWN          2U

/* ---- cfg.alternate ------------------------------------------------------
 * Per-pin AF number written into AFL (pins 0-7) / AFH (pins 8-15). 0xF is the
 * "no peripheral" code the stock driver uses for plain GPIO. */
#define GPIO_AF0                0U    /* PB10                                */
#define GPIO_AF4                4U    /* PA2/PA3 = USART2                    */
#define GPIO_AF5                5U    /* PB8/PB9 = CAN, PB11                 */
#define GPIO_AF_NONE            15U

/* ---- EXTI line masks ---------------------------------------------------- */
#define EXTI_LINE(n)            (1UL << (n))
#define EXTI_LINE2              0x00000004U   /* PB2                          */
#define EXTI_LINE8              0x00000100U   /* PB8                          */

/* ---- AFIO EXTI port selectors ------------------------------------------- */
#define AFIO_EXTI_PORT_A        0U
#define AFIO_EXTI_PORT_B        1U
#define AFIO_EXTI_PORT_C        2U
#define AFIO_EXTI_PORT_D        3U

/* ----------------------------------------------------------------------------
 * The 16-byte pin-configuration block the stock HAL passes around. Field
 * offsets are load-bearing -- they were read straight out of
 * gpio_apply_pin_config @0x08005172 (ldrh [r1,#0], ldrb [r1,#2], ldrb [r1,#3],
 * ldrb [r1,#4], ldr [r1,#8], ldr [r1,#12]) and match the vendor SDK's
 * GPIO_InitType. Keep them.
 * --------------------------------------------------------------------------*/
typedef struct {
    uint16_t pin;        /* +0x00  bit mask of pins this block applies to     */
    uint8_t  drive;      /* +0x02  DS    field (drive strength, 2 bits)       */
    uint8_t  slew;       /* +0x03  SR    bit  (slew-rate limit)               */
    uint8_t  pull;       /* +0x04  PUPD  field (GPIO_PULL_*)                  */
    uint8_t  reserved[3];/* +0x05  padding; the stock HAL never reads it      */
    uint32_t mode;       /* +0x08  GPIO_MODE_*                                */
    uint32_t alternate;  /* +0x0C  AF number for AFL/AFH                      */
} gpio_pin_cfg_t;

/* ---- gpio --------------------------------------------------------------- */
void     gpio_apply_pin_config(GPIO_Module *port, const gpio_pin_cfg_t *cfg); /* 0x08005172 */
void     gpio_init_pin(GPIO_Module *port, uint32_t pin_mask, uint32_t mode);  /* 0x08002494 */
void     gpio_set_bit(GPIO_Module *port, uint32_t pin_mask);                  /* 0x080053a2 */
void     gpio_clear_bit(GPIO_Module *port, uint32_t pin_mask);                /* 0x080053aa */
void     gpio_write_pin(GPIO_Module *port, uint32_t pin_mask, int value);     /* 0x0800252a */
int      gpio_read_pin(GPIO_Module *port, uint32_t pin_mask);                 /* 0x08002548 */
void     gpio_status_lines_init(void);                                        /* 0x08008458 */

/* ---- exti --------------------------------------------------------------- */
void     exti_pb8_wake_config(void);                                          /* 0x08003222 */
int      exti_get_it_status(uint32_t line);                                   /* 0x080045b6 */
void     exti_clear_it_pending(uint32_t line);                                /* 0x080045d8 */

/* ---- interrupt handlers (override the weak stubs in app/startup/crt0.S) -- */
void     EXTI2_IRQHandler(void);                                              /* 0x08002edc */
void     EXTI9_5_IRQHandler(void);                                            /* 0x08002f3e */

#endif /* HW_GPIO_H */
