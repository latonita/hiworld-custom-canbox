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
CFLAGS = CPU + [
    '-Os', '-std=gnu11',
    '-ffreestanding', '-fno-common', '-ffunction-sections', '-fdata-sections',
    '-fno-builtin', '-fno-strict-aliasing',
    '-Wall', '-Wextra', '-Wno-unused-parameter',
    '-I' + os.path.join(ROOT, 'app', 'include'),
]
AFLAGS = CPU
LDFLAGS = CPU + [
    '-nostdlib', '-nostartfiles',
    '-T' + os.path.join(ROOT, 'app', 'linker', 'app.ld'),
    '-Wl,--gc-sections',
    '-Wl,--no-warn-rwx-segments',
]

# Sources, in porting order. app/src/*.c is raw Ghidra output and is NOT built --
# subsystems move into app/port/ as they are reconstructed into real C.
SOURCES = [
    'app/startup/crt0.S',
    'app/startup/startup.c',
]
PORT_DIR = os.path.join(ROOT, 'app', 'port')

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
    srcs = list(SOURCES)
    if os.path.isdir(PORT_DIR):
        srcs += sorted('app/port/' + f for f in os.listdir(PORT_DIR)
                       if f.endswith(('.c', '.S')))
    return srcs


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
        flags = AFLAGS if s.endswith('.S') else CFLAGS
        run([gcc] + flags + ['-c', s, '-o', o], args.verbose)
        print('  %s' % s)
        objs.append(o)

    print('linking:')
    elf, binf = 'build/app.elf', 'build/app.bin'
    run([gcc] + LDFLAGS + objs + ['-Wl,-Map=build/app.map', '-o', elf], args.verbose)
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
