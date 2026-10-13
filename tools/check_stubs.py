#!/usr/bin/env python3
"""
check_stubs.py -- find scaffolding that silently survived the link.

The subsystems in app/port/ were reconstructed independently, so each one declares the
cross-subsystem functions it calls as `__attribute__((weak))` no-op stubs. That lets every
file compile and link on its own -- and it means a FULLY GREEN BUILD CAN STILL BE INERT:
if nobody ever provides the strong definition, the call links happily to a stub that does
nothing, and the firmware boots and quietly does half its job.

A weak symbol that was overridden by a real definition shows up as 'T' (text) in the linked
ELF. One that was never overridden is still 'W'. So every 'W' in the final image is an
unimplemented hook.

This also cross-references the ORIGINAL FLASH ADDRESS comments in app/port/*, because the
independent agents named the same stock function differently (e.g. 0x08007a30 is
`tim2_sched_rate_flags` to one subsystem and something else to another). Two different
symbols carrying the same stock address means the link will NOT connect them, even though
both sides compile.

    python tools/check_stubs.py
    python tools/check_stubs.py --elf build/app.elf
"""

import argparse
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CROSS = os.environ.get('CROSS', 'D:/IDE/arm-none-eabi-gcc/bin/arm-none-eabi-')
PORT = os.path.join(ROOT, 'app', 'port')

ADDR_RE = re.compile(r'0x080[0-9a-fA-F]{5}')

# Only treat a line as naming a function if it actually looks like a C declaration or
# definition -- i.e. a return type followed by an identifier and '('. Matching every
# "word(" on the line pulls in prose from comments ("pulse (bit 24)") and attribute
# syntax, which produced noisy false clashes.
#   matches:  void tim2_init(void);            /* 0x080027f8 */
#             static void foo(TIM_Module *t)   /* 0x08006140 */
#             __attribute__((weak)) void bar(void)  /* 0x08007a30 */
DECL_RE = re.compile(
    r'(?:^|\)|;)\s*(?:static\s+|inline\s+|extern\s+|__attribute__\s*\(\([^)]*\)\)\s*)*'
    r'(?:void|int|char|long|short|float|double|bool|signed|unsigned'
    r'|u?int(?:8|16|32|64)_t|size_t|[A-Z]\w*_Module)'
    r'[\s*]+([A-Za-z_]\w*)\s*\(')

# words that are never a reconstructed function name
# a line that documents only PART of a stock routine, not the whole of it
PART_RE = re.compile(r'0x080[0-9a-fA-F]{5}[^\n]*\bpart\b')

NOT_A_NAME = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'defined',
              '__attribute__', 'volatile', 'const'}


# The five veneer slots that carry REAL work in the stock image. If any of these is still
# weak, that interrupt fires into a fatal trap instead of its handler -- the firmware is
# broken even though it linked. Everything else in crt0.S is a stock default that is
# SUPPOSED to stay weak (49 fatal traps + 5 bare `bx lr` stubs for unused peripherals).
MUST_BE_STRONG = {
    'EXTI2_IRQHandler':    'slot 17, IRQ8  -- EXTI line 2 (PB2)',
    'CAN_RX0_IRQHandler':  'slot 29, IRQ54 -- CAN RX FIFO0 -> RX ring',
    'EXTI9_5_IRQHandler':  'slot 32, IRQ23 -- EXTI line 8 (PB8)',
    'TIM2_IRQHandler':     'slot 37, IRQ28 -- 1 ms scheduler tick',
    'USART2_IRQHandler':   'slot 47, IRQ38 -- head-unit UART',
}

# Weak symbols that crt0.S defines on purpose and that correctly stay weak.
def is_crt0_default(name):
    return (name.endswith('_IRQHandler') or name.endswith('_Handler')) \
        and name not in MUST_BE_STRONG


def unresolved_weaks(elf):
    out = subprocess.run([CROSS + 'nm', elf], capture_output=True, text=True, cwd=ROOT)
    if out.returncode != 0:
        print(out.stderr, file=sys.stderr)
        sys.exit(1)
    weak, strong = [], set()
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        typ, name = parts[-2], parts[-1]
        if typ in ('T', 't'):
            strong.add(name)
        elif typ in ('W', 'w'):
            weak.append(name)
    return [w for w in weak if w not in strong]


def addr_to_symbols():
    """stock flash address -> {symbol names that claim it}, scanned from app/port/*"""
    m = collections.defaultdict(set)
    if not os.path.isdir(PORT):
        return m
    for fn in sorted(os.listdir(PORT)):
        if not fn.endswith(('.c', '.h')):
            continue
        for line in open(os.path.join(PORT, fn), encoding='utf-8', errors='replace'):
            addrs = ADDR_RE.findall(line)
            if not addrs:
                continue
            # A line annotated "part" documents a sub-function of a larger stock
            # routine (e.g. rcc_set_bus_prescalers_div4 is half of 0x080032d2,
            # whose whole is clock_tim2_pwr_init). That is deliberate
            # decomposition, not two reconstructions of one function.
            if PART_RE.search(line):
                continue
            names = [n for n in DECL_RE.findall(line) if n not in NOT_A_NAME]
            for a in addrs:
                for n in names:
                    m[a.lower()].add(n)
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', default='build/app.elf')
    args = ap.parse_args()

    elf = os.path.join(ROOT, args.elf)
    problems = 0

    if os.path.exists(elf):
        w = unresolved_weaks(elf)
        dead_isr = [n for n in w if n in MUST_BE_STRONG]
        scaffold = [n for n in w if not is_crt0_default(n) and n not in MUST_BE_STRONG]
        defaults = [n for n in w if is_crt0_default(n)]

        print('=== real interrupt handlers still unimplemented ===')
        if dead_isr:
            for n in sorted(dead_isr):
                print('  DEAD ISR  %-22s %s' % (n, MUST_BE_STRONG[n]))
            print('\n  This interrupt fires into a fatal trap instead of its handler.')
            problems += len(dead_isr)
        else:
            print('  none -- all %d real handlers have strong definitions' % len(MUST_BE_STRONG))

        print('\n=== cross-subsystem hooks still linking to a do-nothing stub ===')
        if scaffold:
            for n in sorted(scaffold):
                print('  STUB  %s' % n)
            print('\n  %d hook(s) link to scaffolding. The image builds, boots, and' % len(scaffold))
            print('  silently does not do these things.')
            problems += len(scaffold)
        else:
            print('  none -- every cross-subsystem hook has a real definition')

        print('\n(%d crt0.S default trap/empty handlers remain weak, as intended)'
              % len(defaults))
    else:
        print('  (no ELF yet: %s)' % args.elf)

    print('\n=== same stock address claimed under different symbol names ===')
    clashes = {a: n for a, n in addr_to_symbols().items() if len(n) > 1}
    if clashes:
        for a in sorted(clashes):
            print('  %s  ->  %s' % (a, ', '.join(sorted(clashes[a]))))
        print('\n  These are the SAME stock function reconstructed under different names.')
        print('  The linker will not connect them; reconcile to one name.')
        problems += len(clashes)
    else:
        print('  none')

    print()
    if problems:
        print('%d integration issue(s).' % problems)
        return 1
    print('No integration issues found.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
