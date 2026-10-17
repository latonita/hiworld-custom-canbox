#!/usr/bin/env python3
"""
build.py -- build the Hiworld canbox APP without needing `make`.

There is no make on the usual Windows dev box here, and the project already depends on
Python for iaptool.py / iap_upload.py, so this is the primary build driver. The Makefile
is kept for anyone who does have make; both produce the same build/app.bin.

    python tools/build.py                 compile + link + objcopy + verify preamble
    python tools/build.py --pack          ... and package to build/canupdate<version>.iap
    python tools/build.py --protocol benz  build the Mercedes-Benz emulation image
    python tools/build.py --clean
    python tools/build.py --verbose

--protocol picks which head-unit dialect the image speaks (see app_config.hpp):

    jeep   (default)  Hiworld JPF2, "AA 55" framing -- what this box IS, and the
                      only variant that is stock-equivalent on the wire.
    benz              Raise Mercedes-Benz, "2E" framing, for the richer gauge UI.
                      The head unit must be switched to a Benz protocol id too.

The two images are NOT interchangeable at runtime, so the packed container is
named after the 17-byte version string the image actually carries --
build/canupdateH1N4JPF20L-171026.iap, build/canupdateH1N4BNZ10L-171026.iap --
which is the same string `iap_upload.py --verify` reads back over SWD after a
flash. The protocol is encoded in that string (JPF2 vs BNZ1), so the two can
never collide, and a file that has been moved or renamed still says what it is.
Object files are kept in per-protocol subdirectories so switching --protocol
cannot link a stale object built for the other dialect.

Entry/veneer layout is checked on every build by tools/verify_preamble.py; the build fails
if the entry veneer is not at 0x08002202 or a dispatched veneer slot has moved, because
either of those produces an image that boots into garbage.
"""

import argparse
import os
import re
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

# The head-unit dialects this firmware can be built for. See the Protocol
# block in app/include/app_config.hpp -- the code selects on cfg::kIsBenz, so
# both variants are always compiled by the type checker, not just the active one.
#
# `stock_equivalent` marks the variant whose wire behaviour must match the
# original firmware. verify_preamble.py's structural compare against the stock
# image is meaningful for that one; for an emulation image the preamble is
# still checked (it must boot) but the payload deliberately diverges.
PROTOCOLS = {
    'jeep': {
        'defines': [],
        'describe': 'Hiworld JPF2, AA 55 framing -- stock-equivalent',
        'stock_equivalent': True,
        'packable': True,
    },
    'benz': {
        'defines': ['CANBOX_PROTOCOL_BENZ'],
        'describe': 'Raise Mercedes-Benz emulation, 2E framing',
        'stock_equivalent': False,
        'packable': True,
    },
}


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


# The version string lives in ONE place -- app/include/app_config.hpp, which picks
# between the two literals with the same CANBOX_PROTOCOL_BENZ macro the compile uses.
# It is parsed out of there rather than duplicated here, because a second copy would
# drift and the .iap filename would then lie about what is inside it.
VERSION_HEADER = os.path.join('app', 'include', 'app_config.hpp')

#   #if defined(CANBOX_PROTOCOL_BENZ)
#   inline constexpr char kVersionString[] = "...";   <- group 1
#   #else
#   inline constexpr char kVersionString[] = "...";   <- group 2
#   #endif
# \s matches the newlines, so the pattern carries no line-ending assumptions.
_VERSION_RE = re.compile(
    r'#if\s+defined\(CANBOX_PROTOCOL_BENZ\)\s*'
    r'inline\s+constexpr\s+char\s+kVersionString\[\]\s*=\s*"([^"]*)"\s*;\s*'
    r'#else\s*'
    r'inline\s+constexpr\s+char\s+kVersionString\[\]\s*=\s*"([^"]*)"\s*;\s*'
    r'#endif')


def version_string(defines):
    """The version string the image built with `defines` will carry.

    Fails loudly rather than guessing: if the #if/#else shape in app_config.hpp ever
    changes, a silent fallback here would name every .iap after the wrong protocol and
    `iap_upload.py --verify` would report a mismatch on a perfectly good flash.
    """
    path = os.path.join(ROOT, VERSION_HEADER)
    with open(path, encoding='utf-8') as f:
        m = _VERSION_RE.search(f.read())
    if not m:
        raise SystemExit(
            """error: cannot find the kVersionString #if/#else block in {}.
       tools/build.py names the packed .iap after that string; update
       _VERSION_RE there to match the new shape.""".format(VERSION_HEADER))
    benz, jeep = m.group(1), m.group(2)
    version = benz if 'CANBOX_PROTOCOL_BENZ' in defines else jeep
    # The string becomes a filename, so it must survive being one.
    if not re.fullmatch(r'[A-Za-z0-9._-]+', version):
        raise SystemExit('error: version string %r is not safe as a filename' % version)
    return version


def flags_for(src):
    if src.endswith('.S'):
        return AFLAGS
    if src.endswith('.cpp'):
        return CXXFLAGS
    return CFLAGS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pack', action='store_true', help='also package to the .iap')
    ap.add_argument('--protocol', choices=sorted(PROTOCOLS), default='jeep',
                    help='head-unit dialect to build for (default: jeep)')
    ap.add_argument('--clean', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    if args.clean:
        import shutil
        shutil.rmtree(BUILD, ignore_errors=True)
        print('cleaned build/')
        return 0

    proto = PROTOCOLS[args.protocol]
    # Per-protocol object directory. Without this, switching --protocol would
    # reuse objects compiled with the other dialect's constants folded in --
    # a link that succeeds and produces an image that is half one protocol.
    outdir = 'build/%s' % args.protocol
    defines = ['-D' + d for d in proto['defines']]
    print('protocol: %s (%s)' % (args.protocol, proto['describe']))

    gcc = CROSS + 'gcc'
    gpp = CROSS + 'g++'
    if subprocess.run([gcc, '--version'], capture_output=True).returncode != 0:
        print('error: cannot run %s\n'
              'Set CROSS, e.g.  CROSS=/path/to/arm-none-eabi-  python tools/build.py' % gcc,
              file=sys.stderr)
        return 1

    os.makedirs(os.path.join(ROOT, outdir), exist_ok=True)
    objs = []
    print('compiling:')
    for s in sources():
        o = os.path.join(outdir, s.replace('/', '_').rsplit('.', 1)[0] + '.o')
        os.makedirs(os.path.dirname(os.path.join(ROOT, o)), exist_ok=True)
        cc = gpp if s.endswith('.cpp') else gcc
        run([cc] + flags_for(s) + defines + ['-c', s, '-o', o], args.verbose)
        print('  %s' % s)
        objs.append(o)

    print('linking:')
    elf = '%s/app.elf' % outdir
    binf = '%s/app.bin' % outdir
    run([gpp] + LDFLAGS + objs + ['-Wl,-Map=%s/app.map' % outdir, '-o', elf],
        args.verbose)
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
        if not proto.get('packable', True):
            print('\nREFUSING to pack a flashable image for protocol %s:\n  %s'
                  % (args.protocol, proto['unpackable_reason']), file=sys.stderr)
            print('The image above compiled and can be unit-tested '
                  '(python tools/test.py --protocol %s);' % args.protocol,
                  file=sys.stderr)
            print('it is only the .iap packaging that is blocked.', file=sys.stderr)
            return 1

        print('packing:')
        version = version_string(proto['defines'])
        # The name is only true if the bytes are really in there. Cheap to check, and
        # it catches a stale .bin or a header edit that did not reach this build.
        with open(os.path.join(ROOT, binf), 'rb') as f:
            if version.encode('ascii') not in f.read():
                raise SystemExit(
                    """error: {} does not contain the version string {} that its .iap
       would be named after. Rebuild, or check app_config.hpp.""".format(
                        binf, version))
        # No separator between 'canupdate' and the version -- that is the name
        # Hiworld's updater expects; a dash there is not recognised.
        iap = 'build/canupdate%s.iap' % version
        run([sys.executable, 'tools/iaptool.py', 'pack', binf, REF_IAP,
             '-o', iap], args.verbose)
        run([sys.executable, 'tools/iaptool.py', 'verify', iap], args.verbose)
        print('  %s ready -- flash with:' % iap)
        print('    python tools/iap_upload.py %s --port COM17 '
              '--reset powercycle --verify' % iap)
        if not proto['stock_equivalent']:
            print('  NOTE: this image speaks %s. The head unit must be set to a'
                  % proto['describe'])
            print('        matching protocol id in Canset or it will see nothing.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
