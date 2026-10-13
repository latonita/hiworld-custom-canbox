/* ============================================================================
 * app/include/n32l406_hw.h -- real peripheral register layouts for the
 * Nations N32L406CBL7, GENERATED from the vendor SDK header
 * (Nations.N32L40x_Library 2.3.0, firmware/CMSIS/device/n32l40x.h).
 *
 * DO NOT assume STM32F1 layouts here. This part looks F1-ish in places but is
 * not one, and the differences are exactly where a rebuild would silently brick:
 *
 *   GPIO    F4/L4-style, NOT F1's CRL/CRH:
 *             PMODE +0x00  POTYPE +0x04  SR +0x08  PUPD +0x0C
 *             PID   +0x10 (input)  POD +0x14 (output)  PBSC +0x18 (set/clear)
 *             AFL   +0x20  AFH +0x24  PBC +0x28 (bit clear)  DS +0x2C
 *           => pin muxing is per-pin AF numbers in AFL/AFH, NOT AFIO remap.
 *              (stock: PA2/PA3 = AF4 USART2, PB8/PB9 = AF5 CAN -- see gpio.c)
 *   AFIO    only RMP_CFG + EXTI_CFG[4]; there is no F1-style MAPR.
 *   ADC     at 0x40020800 (AHB), not 0x40012400.
 *   IRQs    NOT F1 numbering: CAN_TX=53, CAN_RX0=54, CAN_RX1=55, CAN_SCE=56,
 *           ADC=19, UART4=47, UART5=48, TIM5=50, TIM6=51, TIM7=52.
 *           The app's veneer array is in F103 order and the bootloader's vector
 *           table translates between the two -- see app/startup/crt0.S.
 *
 * Regenerate with:  python <scratch>/gen_hw.py <path-to-SDK>
 * ==========================================================================*/

#ifndef N32L406_HW_H
#define N32L406_HW_H

#include <stdint.h>

#define __IO volatile
#define __I  volatile const
#define __O  volatile

typedef struct {
    __IO  uint32_t PMODE;              /* +0x000 */
    __IO  uint32_t POTYPE;             /* +0x004 */
    __IO  uint32_t SR;                 /* +0x008 */
    __IO  uint32_t PUPD;               /* +0x00C */
    __IO  uint32_t PID;                /* +0x010 */
    __IO  uint32_t POD;                /* +0x014 */
    __IO  uint32_t PBSC;               /* +0x018 */
    __IO  uint32_t PLOCK;              /* +0x01C */
    __IO  uint32_t AFL;                /* +0x020 */
    __IO  uint32_t AFH;                /* +0x024 */
    __IO  uint32_t PBC;                /* +0x028 */
    __IO  uint32_t DS;                 /* +0x02C */
} GPIO_Module;   /* size 0x30 */

typedef struct {
    __IO  uint32_t RMP_CFG;            /* +0x000 */
    __IO  uint32_t EXTI_CFG[4];        /* +0x004 */
} AFIO_Module;   /* size 0x14 */

typedef struct {
    __IO  uint32_t IMASK;              /* +0x000 */
    __IO  uint32_t EMASK;              /* +0x004 */
    __IO  uint32_t RT_CFG;             /* +0x008 */
    __IO  uint32_t FT_CFG;             /* +0x00C */
    __IO  uint32_t SWIE;               /* +0x010 */
    __IO  uint32_t PEND;               /* +0x014 */
    __IO  uint32_t TS_SEL;             /* +0x018 */
} EXTI_Module;   /* size 0x1C */

typedef struct {
    __IO  uint16_t STS;                /* +0x000 */
    __IO  uint16_t RESERVED0;          /* +0x002 */
    __IO  uint16_t DAT;                /* +0x004 */
    __IO  uint16_t RESERVED1;          /* +0x006 */
    __IO  uint16_t BRCF;               /* +0x008 */
    __IO  uint16_t RESERVED2;          /* +0x00A */
    __IO  uint16_t CTRL1;              /* +0x00C */
    __IO  uint16_t RESERVED3;          /* +0x00E */
    __IO  uint16_t CTRL2;              /* +0x010 */
    __IO  uint16_t RESERVED4;          /* +0x012 */
    __IO  uint16_t CTRL3;              /* +0x014 */
    __IO  uint16_t RESERVED5;          /* +0x016 */
    __IO  uint16_t GTP;                /* +0x018 */
    __IO  uint16_t RESERVED6;          /* +0x01A */
} USART_Module;   /* size 0x1C */

typedef struct {
    __IO  uint32_t TMI;                /* +0x000 */
    __IO  uint32_t TMDT;               /* +0x004 */
    __IO  uint32_t TMDL;               /* +0x008 */
    __IO  uint32_t TMDH;               /* +0x00C */
} CAN_TxMailBox_Param;   /* size 0x10 */

typedef struct {
    __IO  uint32_t RMI;                /* +0x000 */
    __IO  uint32_t RMDT;               /* +0x004 */
    __IO  uint32_t RMDL;               /* +0x008 */
    __IO  uint32_t RMDH;               /* +0x00C */
} CAN_FIFOMailBox_Param;   /* size 0x10 */

typedef struct {
    __IO  uint32_t FR1;                /* +0x000 */
    __IO  uint32_t FR2;                /* +0x004 */
} CAN_FilterRegister_Param;   /* size 0x8 */

typedef struct {
    __IO  uint32_t MCTRL;              /* +0x000 */
    __IO  uint32_t MSTS;               /* +0x004 */
    __IO  uint32_t TSTS;               /* +0x008 */
    __IO  uint32_t RFF0;               /* +0x00C */
    __IO  uint32_t RFF1;               /* +0x010 */
    __IO  uint32_t INTE;               /* +0x014 */
    __IO  uint32_t ESTS;               /* +0x018 */
    __IO  uint32_t BTIM;               /* +0x01C */
    __IO  uint32_t RESERVED0[88];      /* +0x020 */
    __IO  CAN_TxMailBox_Param sTxMailBox[3]; /* +0x180 */
    __IO  CAN_FIFOMailBox_Param sFIFOMailBox[2]; /* +0x1B0 */
    __IO  uint32_t RESERVED1[12];      /* +0x1D0 */
    __IO  uint32_t FMC;                /* +0x200 */
    __IO  uint32_t FM1;                /* +0x204 */
    __IO  uint32_t RESERVED2;          /* +0x208 */
    __IO  uint32_t FS1;                /* +0x20C */
    __IO  uint32_t RESERVED3;          /* +0x210 */
    __IO  uint32_t FFA1;               /* +0x214 */
    __IO  uint32_t RESERVED4;          /* +0x218 */
    __IO  uint32_t FA1;                /* +0x21C */
    __IO  uint32_t RESERVED5[8];       /* +0x220 */
    __IO  CAN_FilterRegister_Param sFilterRegister[14]; /* +0x240 */
} CAN_Module;   /* size 0x2B0 */

typedef struct {
    __IO  uint32_t CTRL1;              /* +0x000 */
    __IO  uint32_t CTRL2;              /* +0x004 */
    __IO  uint16_t SMCTRL;             /* +0x008 */
    __IO  uint16_t RESERVED1;          /* +0x00A */
    __IO  uint16_t DINTEN;             /* +0x00C */
    __IO  uint16_t RESERVED2;          /* +0x00E */
    __IO  uint32_t STS;                /* +0x010 */
    __IO  uint16_t EVTGEN;             /* +0x014 */
    __IO  uint16_t RESERVED3;          /* +0x016 */
    __IO  uint16_t CCMOD1;             /* +0x018 */
    __IO  uint16_t RESERVED4;          /* +0x01A */
    __IO  uint16_t CCMOD2;             /* +0x01C */
    __IO  uint16_t RESERVED5;          /* +0x01E */
    __IO  uint32_t CCEN;               /* +0x020 */
    __IO  uint16_t CNT;                /* +0x024 */
    __IO  uint16_t RESERVED6;          /* +0x026 */
    __IO  uint16_t PSC;                /* +0x028 */
    __IO  uint16_t RESERVED7;          /* +0x02A */
    __IO  uint16_t AR;                 /* +0x02C */
    __IO  uint16_t RESERVED8;          /* +0x02E */
    __IO  uint16_t REPCNT;             /* +0x030 */
    __IO  uint16_t RESERVED9;          /* +0x032 */
    __IO  uint16_t CCDAT1;             /* +0x034 */
    __IO  uint16_t RESERVED10;         /* +0x036 */
    __IO  uint16_t CCDAT2;             /* +0x038 */
    __IO  uint16_t RESERVED11;         /* +0x03A */
    __IO  uint16_t CCDAT3;             /* +0x03C */
    __IO  uint16_t RESERVED12;         /* +0x03E */
    __IO  uint16_t CCDAT4;             /* +0x040 */
    __IO  uint16_t RESERVED13;         /* +0x042 */
    __IO  uint16_t BKDT;               /* +0x044 */
    __IO  uint16_t RESERVED14;         /* +0x046 */
    __IO  uint16_t DCTRL;              /* +0x048 */
    __IO  uint16_t RESERVED15;         /* +0x04A */
    __IO  uint16_t DADDR;              /* +0x04C */
    __IO  uint16_t RESERVED16;         /* +0x04E */
    __IO  uint32_t RESERVED17;         /* +0x050 */
    __IO  uint16_t CCMOD3;             /* +0x054 */
    __IO  uint16_t RESERVED18;         /* +0x056 */
    __IO  uint16_t CCDAT5;             /* +0x058 */
    __IO  uint16_t RESERVED19;         /* +0x05A */
    __IO  uint16_t CCDAT6;             /* +0x05C */
    __IO  uint16_t RESERVED20;         /* +0x05E */
} TIM_Module;   /* size 0x60 */

typedef struct {
    __IO  uint32_t CTRL;               /* +0x000 */
    __IO  uint32_t CFG;                /* +0x004 */
    __IO  uint32_t CLKINT;             /* +0x008 */
    __IO  uint32_t APB2PRST;           /* +0x00C */
    __IO  uint32_t APB1PRST;           /* +0x010 */
    __IO  uint32_t AHBPCLKEN;          /* +0x014 */
    __IO  uint32_t APB2PCLKEN;         /* +0x018 */
    __IO  uint32_t APB1PCLKEN;         /* +0x01C */
    __IO  uint32_t LDCTRL;             /* +0x020 */
    __IO  uint32_t CTRLSTS;            /* +0x024 */
    __IO  uint32_t AHBPRST;            /* +0x028 */
    __IO  uint32_t CFG2;               /* +0x02C */
    __IO  uint32_t CFG3;               /* +0x030 */
    __IO  uint32_t RDCTRL;             /* +0x034 */
    __IO  uint32_t Reserve0;           /* +0x038 */
    __IO  uint32_t Reserve1;           /* +0x03C */
    __IO  uint32_t PLLHSIPRE;          /* +0x040 */
    __IO  uint32_t SRAM_CTRLSTS;       /* +0x044 */
} RCC_Module;   /* size 0x48 */

typedef struct {
    __IO  uint32_t AC;                 /* +0x000 */
    __IO  uint32_t KEYR;               /* +0x004 */
    __IO  uint32_t OPTKEY;             /* +0x008 */
    __IO  uint32_t STS;                /* +0x00C */
    __IO  uint32_t CTRL;               /* +0x010 */
    __IO  uint32_t ADD;                /* +0x014 */
    __IO  uint32_t OB2;                /* +0x018 */
    __IO  uint32_t OB;                 /* +0x01C */
    __IO  uint32_t WRP;                /* +0x020 */
    __IO  uint32_t RESERVED0;          /* +0x024 */
    __IO  uint32_t RESERVED1;          /* +0x028 */
    __IO  uint32_t RESERVED2;          /* +0x02C */
    __IO  uint32_t CAHR;               /* +0x030 */
} FLASH_Module;   /* size 0x34 */

typedef struct {
    __IO  uint32_t CTRL1;              /* +0x000 */
    __IO  uint32_t CTRL2;              /* +0x004 */
    __IO  uint32_t CTRL3;              /* +0x008 */
    __IO  uint32_t STS1;               /* +0x00C */
    __IO  uint32_t STS2;               /* +0x010 */
    __IO  uint32_t STSCLR;             /* +0x014 */
} PWR_Module;   /* size 0x18 */

typedef struct {
    __IO  uint32_t CTRL;               /* +0x000 */
    __IO  uint32_t CFG;                /* +0x004 */
    __IO  uint32_t STS;                /* +0x008 */
} WWDG_Module;   /* size 0xC */

/* ---- peripheral instances ------------------------------------------------
 * Base addresses verified against the SDK; they agree with the firmware's own
 * REG_* constants in registers.h. */
#define GPIOA    ((GPIO_Module *)0x40010800U)
#define GPIOB    ((GPIO_Module *)0x40010C00U)
#define GPIOC    ((GPIO_Module *)0x40011000U)
#define GPIOD    ((GPIO_Module *)0x40011400U)
#define AFIO     ((AFIO_Module *)0x40010000U)
#define EXTI     ((EXTI_Module *)0x40010400U)
#define USART1   ((USART_Module *)0x40013800U)
#define USART2   ((USART_Module *)0x40004400U)
#define USART3   ((USART_Module *)0x40004800U)
#define CAN      ((CAN_Module *)0x40006400U)
#define TIM1     ((TIM_Module *)0x40012C00U)
#define TIM2     ((TIM_Module *)0x40000000U)
#define RCC      ((RCC_Module *)0x40021000U)
#define FLASH_R  ((FLASH_Module *)0x40022000U)
#define PWR      ((PWR_Module *)0x40007000U)
#define WWDG     ((WWDG_Module *)0x40002C00U)

/* CAN mailbox / filter arrays (offsets per the vendor header). */
#define CAN_TxMailBox    ((CAN_TxMailBox_Param   *)(0x40006400U + 0x180U))
#define CAN_FIFOMailBox  ((CAN_FIFOMailBox_Param *)(0x40006400U + 0x1B0U))
#define CAN_FilterReg    ((CAN_FilterRegister_Param *)(0x40006400U + 0x240U))

/* ---- Cortex-M4 core blocks ---------------------------------------------- */
#define NVIC_ISER        ((__IO uint32_t *)0xE000E100U)
#define NVIC_ICER        ((__IO uint32_t *)0xE000E180U)
#define NVIC_IPR         ((__IO uint8_t  *)0xE000E400U)
#define SCB_AIRCR        (*(__IO uint32_t *)0xE000ED0CU)
#define SCB_SCR          (*(__IO uint32_t *)0xE000ED10U)

/* ---- IRQ numbers (official N32L40x values -- NOT STM32F1's) -------------- */
typedef enum {
    NonMaskableInt_IRQn = -14, HardFault_IRQn = -13, MemoryManagement_IRQn = -12,
    BusFault_IRQn = -11, UsageFault_IRQn = -10, SVCall_IRQn = -5,
    DebugMonitor_IRQn = -4, PendSV_IRQn = -2, SysTick_IRQn = -1,
    WWDG_IRQn = 0, PVD_IRQn = 1, TAMPER_IRQn = 2, RTC_IRQn = 3, FLASH_IRQn = 4,
    RCC_IRQn = 5, EXTI0_IRQn = 6, EXTI1_IRQn = 7, EXTI2_IRQn = 8, EXTI3_IRQn = 9,
    EXTI4_IRQn = 10, DMA_Channel1_IRQn = 11, DMA_Channel2_IRQn = 12,
    DMA_Channel3_IRQn = 13, DMA_Channel4_IRQn = 14, DMA_Channel5_IRQn = 15,
    DMA_Channel6_IRQn = 16, DMA_Channel7_IRQn = 17, DMA_Channel8_IRQn = 18, ADC_IRQn = 19,
    USB_HP_IRQn = 20, USB_LP_IRQn = 21, COMP_1_2_IRQn = 22, EXTI9_5_IRQn = 23,
    TIM1_BRK_IRQn = 24, TIM1_UP_IRQn = 25, TIM1_TRG_COM_IRQn = 26, TIM1_CC_IRQn = 27,
    TIM2_IRQn = 28, TIM3_IRQn = 29, TIM4_IRQn = 30, I2C1_EV_IRQn = 31, I2C1_ER_IRQn = 32,
    I2C2_EV_IRQn = 33, I2C2_ER_IRQn = 34, SPI1_IRQn = 35, SPI2_IRQn = 36, USART1_IRQn = 37,
    USART2_IRQn = 38, USART3_IRQn = 39, EXTI15_10_IRQn = 40, RTCAlarm_IRQn = 41,
    USBWakeUp_IRQn = 42, TIM8_BRK_IRQn = 43, TIM8_UP_IRQn = 44, TIM8_TRG_COM_IRQn = 45,
    TIM8_CC_IRQn = 46, UART4_IRQn = 47, UART5_IRQn = 48, LPUART_IRQn = 49, TIM5_IRQn = 50,
    TIM6_IRQn = 51, TIM7_IRQn = 52, CAN_TX_IRQn = 53, CAN_RX0_IRQn = 54, CAN_RX1_IRQn = 55,
    CAN_SCE_IRQn = 56, LPUART_WKUP_IRQn = 57, LPTIM_WKUP_IRQn = 58, LCD_IRQn = 59,
    SAC_IRQn = 60, MMU_IRQn = 61, TSC_IRQn = 62, RAMC_PERR_IRQn = 63, TIM9_IRQn = 64,
    UCDR_IRQn = 65
} IRQn_Type;

/* Interrupt masking (the app brackets its whole init block in CPSID/CPSIE). */
static inline void irq_disable(void) { __asm volatile ("cpsid i" ::: "memory"); }
static inline void irq_enable(void)  { __asm volatile ("cpsie i" ::: "memory"); }
static inline void wait_for_irq(void) { __asm volatile ("wfi"); }

#endif /* N32L406_HW_H */
