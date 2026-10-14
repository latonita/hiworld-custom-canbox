#!/usr/bin/env python3
"""
build.py -- build the Hiworld canbox APP without needing `make`.

There is no make on the usual Windows dev box here, and the project already depends on
Python for iaptool.py / iap_upload.py, so this is the primary build driver. The Makefile
is kept for anyone who does have make; both produce the same build/app.bin.

    python tools/build.py                 compile + link + objcopy + verify preamble
    python tools/build.py --pack          ... and package to build/app.iap
    python tools/build.py --clean
    python tools/build.py --verbose

Entry/veneer layout is checked on every build by tools/verify_preamble.py; the build fails
if the entry veneer is not at 0x08002202 or a dispatched veneer slot has moved, because
either of those produces an image that boots into garbage.
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build')

CROSS = os.environ.get('CROSS', 'D:/IDE/arm-none-eabi-gcc/bin/arm-none-eabi-')

CPU = ['-mcpu=cortex-m4', '-mthumb', '-mfloat-abi=soft']

INCLUDES = ['-I' + os.path.join(ROOT, d) for d in (
    os.path.join('app', 'include'),
    os.path.join('app', 'hal'),
    os.path.join('app', 'services'),
    os.path.join('app', 'logic'),
)]

# Shared by both languages. -fno-common and the section-per-symbol flags let
# --gc-sections drop anything the image does not actually reach.
COMMON = CPU + [
    '-Os',
    '-ffreestanding', '-fno-common', '-ffunction-sections', '-fdata-sections',
    '-fno-builtin', '-fno-strict-aliasing',
    '-Wall', '-Wextra', '-Wno-unused-parameter',
] + INCLUDES

CFLAGS = COMMON + ['-std=gnu11']

# C++ for the services and logic layers.
#   -fno-exceptions / -fno-rtti / -fno-unwind-tables: there is no C++ runtime in
#       this image and no room for one; MISRA C++ bans both features anyway.
#   -fno-threadsafe-statics: guard variables for function-local statics would
#       call __cxa_guard_acquire, which does not exist here.
#   -fno-use-cxa-atexit: destructor registration needs a heap and an exit path;
#       this firmware has neither and never returns.
# A static constructor slipping through anyway is caught at link time by the
# .init_array ASSERT in app/linker/app.ld.
CXXFLAGS = COMMON + [
    '-std=c++17',
    '-fno-exceptions', '-fno-rtti',
    '-fno-threadsafe-statics', '-fno-use-cxa-atexit',
    '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
]

AFLAGS = CPU
LDFLAGS = CPU + [
    '-nostdlib', '-nostartfiles',
    '-T' + os.path.join(ROOT, 'app', 'linker', 'app.ld'),
    '-Wl,--gc-sections',
    '-Wl,--no-warn-rwx-segments',
]

# The startup preamble comes first and must stay first: app/linker/app.ld places
# its .image_entry section at 0x08002000 and the entry veneer must land at
# 0x08002202. Everything else is discovered from the three layer directories.
SOURCES = [
    os.path.join('app', 'startup', 'crt0.S'),
    os.path.join('app', 'startup', 'startup.c'),
]

# Scanned in dependency order, lowest layer first. app/port/ is deliberately not
# here: it is the previous generation, kept as a reference next to reference/decompile/app/.
LAYER_DIRS = [
    os.path.join('app', 'hal'),
    os.path.join('app', 'services'),
    os.path.join('app', 'logic'),
]

REF_IAP = 'reference/2429-H1N4JPF20A-220524.iap'
REF_BIN = 'reference/2429-H1N4JPF20A-220524.app.bin'


def run(cmd, verbose):
    if verbose:
        print('  ' + ' '.join(cmd))
    sys.stdout.flush()
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.stdout.strip():
        print(r.stdout.rstrip())
    if r.stderr.strip():
        print(r.stderr.rstrip(), file=sys.stderr)
    if r.returncode != 0:
        print('\nFAILED: %s' % ' '.join(cmd[:2]), file=sys.stderr)
        sys.exit(r.returncode)
    return r


def sources():
    """Startup first, then each layer bottom-up, alphabetically within a layer."""
    srcs = list(SOURCES)
    for d in LAYER_DIRS:
        full = os.path.join(ROOT, d)
        if not os.path.isdir(full):
            continue
        srcs += [os.path.join(d, f).replace(os.sep, '/')
                 for f in sorted(os.listdir(full))
                 if f.endswith(('.c', '.cpp', '.S'))]
    return srcs


def flags_for(src):
    if src.endswith('.S'):
        return AFLAGS
    if src.endswith('.cpp'):
        return CXXFLAGS
    return CFLAGS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pack', action='store_true', help='also package to build/app.iap')
    ap.add_argument('--clean', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    if args.clean:
        import shutil
        shutil.rmtree(BUILD, ignore_errors=True)
        print('cleaned build/')
        return 0

    gcc = CROSS + 'gcc'
    gpp = CROSS + 'g++'
    if subprocess.run([gcc, '--version'], capture_output=True).returncode != 0:
        print('error: cannot run %s\n'
              'Set CROSS, e.g.  CROSS=/path/to/arm-none-eabi-  python tools/build.py' % gcc,
              file=sys.stderr)
        return 1

    os.makedirs(BUILD, exist_ok=True)
    objs = []
    print('compiling:')
    for s in sources():
        o = os.path.join('build', s.replace('/', '_').rsplit('.', 1)[0] + '.o')
        os.makedirs(os.path.dirname(os.path.join(ROOT, o)), exist_ok=True)
        cc = gpp if s.endswith('.cpp') else gcc
        run([cc] + flags_for(s) + ['-c', s, '-o', o], args.verbose)
        print('  %s' % s)
        objs.append(o)

    print('linking:')
    elf, binf = 'build/app.elf', 'build/app.bin'
    run([gpp] + LDFLAGS + objs + ['-Wl,-Map=build/app.map', '-o', elf], args.verbose)
    run([CROSS + 'objcopy', '-O', 'binary', elf, binf], args.verbose)
    run([CROSS + 'size', elf], args.verbose)

    size = os.path.getsize(os.path.join(ROOT, binf))
    print('  %s  %d bytes (region is 0x1D800 = %d)' % (binf, size, 0x1D800))
    if size > 0x1D800:
        print('ERROR: image overflows the app region', file=sys.stderr)
        return 1

    print('verifying preamble:')
    sys.stdout.flush()
    r = subprocess.run([sys.executable, 'tools/verify_preamble.py', binf,
                        '--compare', REF_BIN], cwd=ROOT, text=True)
    if r.returncode != 0:
        return r.returncode

    # Integration checks the compiler cannot make: an interrupt handler whose name drifted
    # and silently left the crt0 trap in place, weak scaffolding, or one stock function
    # reconstructed twice under two names. All three link cleanly and break the box.
    print('checking integration:')
    sys.stdout.flush()
    r = subprocess.run([sys.executable, 'tools/check_stubs.py', '--elf', elf],
                       cwd=ROOT, text=True, capture_output=True)
    if r.returncode != 0:
        print(r.stdout.rstrip(), file=sys.stderr)
        return r.returncode
    print('  ' + r.stdout.rstrip().splitlines()[-1])

    if args.pack:
        print('packing:')
        run([sys.executable, 'tools/iaptool.py', 'pack', binf, REF_IAP,
             '-o', 'build/app.iap'], args.verbose)
        run([sys.executable, 'tools/iaptool.py', 'verify', 'build/app.iap'], args.verbose)
        print('  build/app.iap ready -- flash with:')
        print('    python tools/iap_upload.py build/app.iap --port COM17 '
              '--reset powercycle --verify')
    return 0


if __name__ == '__main__':
    sys.exit(main())
