#!/usr/bin/env python3
"""
check_stubs.py -- integration checks the compiler cannot make.

Two failure modes that produce a FULLY GREEN BUILD and a broken box:

1. AN INTERRUPT HANDLER THAT IS STILL THE crt0 DEFAULT.
   app/startup/crt0.S defines a weak handler for all 59 veneer slots. Five of them must be
   overridden by a real definition; the other 54 are stock defaults that are SUPPOSED to
   stay weak. A handler whose C++ name got mangled, or whose spelling drifted, links
   perfectly and silently leaves the weak trap in place -- the box then resets with a trap
   code instead of working. An overridden weak symbol shows as 'T' in the linked ELF; one
   that was never overridden is still 'W'.

   (The earlier generation in app/port/ also used weak no-op stubs as cross-subsystem
   scaffolding, which is what this tool was originally written to catch. That scaffolding
   is gone -- docs/ARCHITECTURE.md section 1 rule 5 bans it -- so this check is now a
   regression guard rather than a progress report.)

2. TWO SYMBOLS CLAIMING THE SAME STOCK FLASH ADDRESS.
   Every reconstructed function carries the address of the stock routine it reproduces in a
   /* 0x0800xxxx */ comment. Two different symbols carrying the same address means the same
   stock function was reconstructed twice under two names; the link will not connect them,
   even though both sides compile.

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
# The reconstructed firmware, bottom layer first. app/port/ is gone; reference/decompile/app/ is the Ghidra
# spec and is deliberately not scanned (it carries the stock addresses by definition).
LAYER_DIRS = [os.path.join(ROOT, 'app', d) for d in ('hal', 'services', 'logic', 'startup')]
SRC_EXT = ('.c', '.h', '.cpp', '.hpp')

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


def is_cxx_inline(name):
    """A C++ `inline` function is emitted as a WEAK (COMDAT) symbol by the language rules,
    so the linker can merge the copy every including translation unit emits. `nm` reports
    it as 'W', identically to a hand-written weak stub.

    They are told apart by name mangling: a weak symbol with a mangled name can only be an
    inline or a template instantiation, because nothing in the rebuilt layers declares a
    C++ function weak. That ban is checked at the source level by no_weak_scaffolding()
    below rather than guessed at from the ELF."""
    return name.startswith('_Z')


def no_weak_scaffolding():
    """The source-level half of the check: no explicit weak definitions in the rebuild.

    This is the rule the ELF cannot express, and it is the one that matters. A weak no-op
    stub lets a half-finished subsystem link, and then the firmware boots and quietly does
    not do that job. docs/ARCHITECTURE.md section 1 rule 5 bans them outright."""
    pattern = re.compile(r'__attribute__\s*\(\(\s*weak')
    hits = []
    for d in LAYER_DIRS:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(SRC_EXT):
                continue
            path = os.path.join(d, fn)
            for n, line in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
                if pattern.search(line):
                    rel = os.path.relpath(path, ROOT).replace(os.sep, '/')
                    hits.append('%s:%d' % (rel, n))
    return hits


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
    """stock flash address -> {symbol names that claim it}, over the rebuilt layers"""
    m = collections.defaultdict(set)
    files = []
    for d in LAYER_DIRS:
        if not os.path.isdir(d):
            continue
        files += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith(SRC_EXT)]
    for path in files:
        for line in open(path, encoding='utf-8', errors='replace'):
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
        scaffold = [n for n in w if not is_crt0_default(n) and n not in MUST_BE_STRONG
                    and not is_cxx_inline(n)]
        defaults = [n for n in w if is_crt0_default(n)]

        print('=== real interrupt handlers still unimplemented ===')
        if dead_isr:
            for n in sorted(dead_isr):
                print('  DEAD ISR  %-22s %s' % (n, MUST_BE_STRONG[n]))
            print('\n  This interrupt fires into a fatal trap instead of its handler.')
            problems += len(dead_isr)
        else:
            print('  none -- all %d real handlers have strong definitions' % len(MUST_BE_STRONG))

        print('\n=== weak scaffolding in the rebuilt layers ===')
        weak_src = no_weak_scaffolding()
        if weak_src:
            for h in weak_src:
                print('  WEAK  %s' % h)
            print('\n  %d weak definition(s) in app/hal, app/services or app/logic.' % len(weak_src))
            print('  A weak no-op links happily and silently does nothing at run time.')
            problems += len(weak_src)
        else:
            print('  none -- no __attribute__((weak)) in the rebuilt layers')

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
