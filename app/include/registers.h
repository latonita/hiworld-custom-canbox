// MMIO peripheral base addresses - Nations N32L406 (as used by this firmware).
// NOTE: N32L406 is STM32F1-like but NOT identical; ADC is at 0x40020800 (not 0x40012400).

#ifndef REGISTERS_H
#define REGISTERS_H
#include <stdint.h>

#define REG_TIM2         ((volatile uint32_t *)0x40000000U)
#define REG_WWDG         ((volatile uint32_t *)0x40002C00U)
#define REG_USART2       ((volatile uint32_t *)0x40004400U)
#define REG_USART3       ((volatile uint32_t *)0x40004800U)
#define REG_CAN          ((volatile uint32_t *)0x40006400U)
#define REG_CAN_FILTER   ((volatile uint32_t *)0x40006600U)
#define REG_PWR          ((volatile uint32_t *)0x40007000U)
#define REG_AFIO         ((volatile uint32_t *)0x40010000U)
#define REG_EXTI         ((volatile uint32_t *)0x40010400U)
#define REG_GPIOA        ((volatile uint32_t *)0x40010800U)
#define REG_GPIOB        ((volatile uint32_t *)0x40010C00U)
#define REG_GPIOC        ((volatile uint32_t *)0x40011000U)
#define REG_GPIOD        ((volatile uint32_t *)0x40011400U)
#define REG_TIM1         ((volatile uint32_t *)0x40012C00U)
#define REG_USART1       ((volatile uint32_t *)0x40013800U)
#define REG_DMA1         ((volatile uint32_t *)0x40020000U)
#define REG_DMA1_CH1     ((volatile uint32_t *)0x40020008U)
#define REG_ADC          ((volatile uint32_t *)0x40020800U)
#define REG_RCC          ((volatile uint32_t *)0x40021000U)
#define REG_FLASH        ((volatile uint32_t *)0x40022000U)
#define REG_NVIC_ISER    ((volatile uint32_t *)0xE000E100U)
#define REG_NVIC_ICER    ((volatile uint32_t *)0xE000E180U)
#define REG_NVIC_IPR     ((volatile uint32_t *)0xE000E400U)
#define REG_SCB          ((volatile uint32_t *)0xE000ED00U)
#define REG_SCB_AIRCR    ((volatile uint32_t *)0xE000ED0CU)
#define REG_SCB_SCR      ((volatile uint32_t *)0xE000ED10U)

#endif // REGISTERS_H
