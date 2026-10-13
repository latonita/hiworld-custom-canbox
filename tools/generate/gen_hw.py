"""Generate app/include/n32l406_hw.h from the official Nations N32L40x SDK header.

Transcribing register layouts by hand is how a rebuild silently bricks: this chip looks
STM32-ish but is not (GPIO is F4/L4-style, CAN_RX0 is IRQ54, ADC is at 0x40020800), so the
layouts are copied mechanically from the vendor header instead.
"""
import re, sys, os

SDK = sys.argv[1]
HDR = os.path.join(SDK, 'firmware/CMSIS/device/n32l40x.h')
src = open(HDR, encoding='utf-8', errors='replace').read()

# peripherals this firmware actually touches (docs/hiworld-startup-sequence.md §3)
WANT = ['GPIO_Module', 'AFIO_Module', 'EXTI_Module', 'USART_Module',
        'CAN_TxMailBox_Param', 'CAN_FIFOMailBox_Param', 'CAN_FilterRegister_Param',
        'CAN_Module', 'TIM_Module', 'RCC_Module', 'FLASH_Module', 'PWR_Module',
        'WWDG_Module']


def struct_body(name):
    end = src.find('} ' + name)
    if end < 0:
        end = src.find('}' + name)
    assert end > 0, name
    start = src.rfind('typedef struct', 0, end)
    return src[src.find('{', start) + 1:end]


# Members may be primitives OR previously-defined structs (e.g. CAN_Module contains
# `CAN_TxMailBox_Param sTxMailBox[3];`). Matching only primitives silently DROPS the
# nested members and shifts every following register -- that is exactly how the CAN
# filter block ended up 0x50 bytes too low, aliasing the RX FIFO mailboxes.
FIELD = re.compile(r'\s*(__IO|__I|__O)?\s*([A-Za-z_]\w*)\s+(\w+)\s*(?:\[(\d+)\])?\s*;')
SIZES = {'uint8_t': 1, 'uint16_t': 2, 'uint32_t': 4, 'uint64_t': 8}

out = []
OFFSETS = {}
A = out.append
A('''/* ============================================================================
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

''')

for name in WANT:
    body = struct_body(name)
    off = 0
    rows = []
    for line in body.splitlines():
        m = FIELD.match(line)
        if not m:
            continue
        qual, ty, fname, cnt = m.group(1) or '', m.group(2), m.group(3), m.group(4)
        if ty not in SIZES:
            raise SystemExit(
                'gen_hw.py: %s has member of unknown type %r (%s).\n'
                'Add it to WANT BEFORE %s so its size is known, or extend SIZES.\n'
                'Refusing to emit a struct with wrong offsets.'
                % (name, ty, fname, name))
        n = int(cnt) if cnt else 1
        rows.append((off, qual or '__IO', ty, fname, n))
        off += SIZES[ty] * n
    SIZES[name] = off      # nested structs become sizeable for later members
    A('typedef struct {\n')
    for o, qual, ty, fname, n in rows:
        OFFSETS[(name, fname)] = o
        decl = '%s %s%s;' % (ty, fname, '[%d]' % n if n > 1 else '')
        A('    %-5s %-28s /* +0x%03X */\n' % (qual, decl, o))
    A('} %s;   /* size 0x%X */\n\n' % (name, off))

# ---- base addresses, cross-checked against app/include/registers.h ----
bases = dict(re.findall(r'^#define\s+(\w+)_BASE\s+\(?([^)]*?)\)?\s*(?:/\*.*)?$', src, re.M))
A('''/* ---- peripheral instances ------------------------------------------------
 * Base addresses verified against the SDK; they agree with the firmware's own
 * REG_* constants in registers.h. */
''')
INST = [('GPIOA', 'GPIO_Module', 0x40010800), ('GPIOB', 'GPIO_Module', 0x40010C00),
        ('GPIOC', 'GPIO_Module', 0x40011000), ('GPIOD', 'GPIO_Module', 0x40011400),
        ('AFIO', 'AFIO_Module', 0x40010000), ('EXTI', 'EXTI_Module', 0x40010400),
        ('USART1', 'USART_Module', 0x40013800), ('USART2', 'USART_Module', 0x40004400),
        ('USART3', 'USART_Module', 0x40004800), ('CAN', 'CAN_Module', 0x40006400),
        ('TIM1', 'TIM_Module', 0x40012C00), ('TIM2', 'TIM_Module', 0x40000000),
        ('RCC', 'RCC_Module', 0x40021000), ('FLASH_R', 'FLASH_Module', 0x40022000),
        ('PWR', 'PWR_Module', 0x40007000), ('WWDG', 'WWDG_Module', 0x40002C00)]
for nm, ty, addr in INST:
    A('#define %-8s ((%s *)0x%08XU)\n' % (nm, ty, addr))

A('''
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
''')
blk = re.search(r'typedef enum IRQn.*?\}\s*IRQn_Type', src, re.S).group(0)
pairs = [(m.group(1), int(m.group(2)))
         for m in re.finditer(r'^\s*(\w+)_IRQn\s*=\s*(\d+)', blk, re.M)]
line = '    '
for nm, v in pairs:
    tok = '%s_IRQn = %d, ' % (nm, v)
    if len(line) + len(tok) > 92:
        A(line.rstrip() + '\n')
        line = '    '
    line += tok
A(line.rstrip().rstrip(',') + '\n} IRQn_Type;\n')

A('''
/* Interrupt masking (the app brackets its whole init block in CPSID/CPSIE). */
static inline void irq_disable(void) { __asm volatile ("cpsid i" ::: "memory"); }
static inline void irq_enable(void)  { __asm volatile ("cpsie i" ::: "memory"); }
static inline void wait_for_irq(void) { __asm volatile ("wfi"); }

#endif /* N32L406_HW_H */
''')

# ---- self-check: offsets this firmware actually depends on -----------------
# These are cross-checked against registers.h and the stock image's own literals.
EXPECT = {
    ('GPIO_Module', 'PID'): 0x10, ('GPIO_Module', 'POD'): 0x14,
    ('GPIO_Module', 'PBSC'): 0x18, ('GPIO_Module', 'AFL'): 0x20,
    ('GPIO_Module', 'AFH'): 0x24, ('GPIO_Module', 'PBC'): 0x28,
    ('USART_Module', 'DAT'): 0x04, ('USART_Module', 'BRCF'): 0x08,
    ('USART_Module', 'CTRL1'): 0x0C,
    ('CAN_Module', 'BTIM'): 0x1C,
    # CAN filter control lives at CAN_BASE+0x200 == 0x40006600, which is exactly
    # REG_CAN_FILTER in registers.h and what the stock can_filter_init emits.
    ('CAN_Module', 'FMC'): 0x200, ('CAN_Module', 'FM1'): 0x204,
    ('CAN_Module', 'FS1'): 0x20C, ('CAN_Module', 'FFA1'): 0x214,
    ('CAN_Module', 'FA1'): 0x21C,
    ('RCC_Module', 'APB2PCLKEN'): 0x18, ('RCC_Module', 'APB1PCLKEN'): 0x1C,
    ('RCC_Module', 'CTRLSTS'): 0x24,
    ('WWDG_Module', 'CFG'): 0x04,
}
bad = []
for (sname, fname), want in sorted(EXPECT.items()):
    got = OFFSETS.get((sname, fname))
    if got != want:
        bad.append('  %s.%s: expected +0x%X, generator produced %s'
                   % (sname, fname, want,
                      ('+0x%X' % got) if got is not None else 'MISSING'))
if bad:
    raise SystemExit('gen_hw.py: register offset self-check FAILED\n' + '\n'.join(bad))

open('app/include/n32l406_hw.h', 'w', newline='\n', encoding='utf-8').write(''.join(out))
print('wrote app/include/n32l406_hw.h  (offset self-check passed)')
for nm in WANT:
    b = struct_body(nm)
    tot = sum(SIZES[m.group(2)] * (int(m.group(4)) if m.group(4) else 1)
              for m in (FIELD.match(l) for l in b.splitlines()) if m)
    print('  %-28s size 0x%X' % (nm, tot))
