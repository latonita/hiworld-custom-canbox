#!/usr/bin/env python3
"""
test.py -- build and run the host unit tests for the canbox APP.

The firmware is cross-compiled for a Cortex-M4 that is bolted into a car, so the
only equivalence evidence available without a bench is a host build of the
layers that do not touch a register: the decoders, the scheduler, the parameter
logic, the software timers, the head-unit framing. Those compile for x86 with
-DCANBOX_HOST_TEST, where ram::app() is a plain object and app/hal/ is replaced
by the recording fakes in tests/host/fake_hal.cpp.

    python tools/test.py                 build + run everything, BOTH protocols
    python tools/test.py --protocol benz only the Benz-dialect configuration
    python tools/test.py -k steering     only cases whose name contains "steering"
    python tools/test.py -v              echo the compiler command line
    python tools/test.py --clean

The head-unit dialect is a compile-time constant (cfg::kIsBenz), so a single
binary can only exercise one of them. The default therefore builds and runs the
suite TWICE, once per protocol: everything that is dialect-independent -- the
decoders, scheduler, params, timers -- is covered in both, and the two framing
suites are swapped in and out per configuration:

    test_head_unit.cpp          Hiworld "AA 55", the stock dialect. jeep only --
                                its assertions are deliberately byte-exact
                                against the original firmware and must not be
                                softened into something dialect-neutral.
    test_head_unit_raise.cpp    Raise/Benz "2E" framing and the IAP escape
                                hatch. benz only.
    test_custom_tx.cpp          the custom 0xB0-0xB8 telemetry block. jeep only
                                -- a benz image has none of those TX slots.
    test_raise_benz_tx.cpp      the Benz message set: WK CAN decode -> frames,
                                against worked examples from the plugin
                                analysis. benz only.

Host compiler: clang++ (or $HOST_CXX / g++). Nothing else is required.
"""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'host')

# Layers under test. app/hal/ is deliberately absent: it is replaced by the fakes.
SRC_DIRS = [
    os.path.join('app', 'services'),
    os.path.join('app', 'logic'),
]

INCLUDE_DIRS = [
    os.path.join('app', 'include'),
    os.path.join('app', 'hal'),
    os.path.join('app', 'services'),
    os.path.join('app', 'logic'),
    os.path.join('tests', 'host'),
]

# main_loop.cpp is the firmware's entry point. Its only external symbol is
# `main_loop`, which does not clash with the runner's main() -- but it never
# returns (phase 2 is an unbounded superloop) and its phases are file-local, so
# there is nothing a host test can call. Excluded until it is restructured.
#
# interrupts.cpp IS built: its five extern "C" handlers are ordinary functions a
# test can call directly, and they are the one place a renamed symbol silently
# leaves a crt0 trap in place.
EXCLUDE = {'main_loop.cpp'}

# Harness files that must be linked in no matter how --units narrows the build.
HARNESS_ALWAYS = {'test_main.cpp', 'fake_hal.cpp'}

# Per-protocol configuration. `defines` mirrors tools/build.py; `exclude` drops
# the framing suite that cannot hold in this configuration. Keeping the split
# here rather than behind #if inside the test sources means neither suite has a
# preprocessor branch in it, and a file left out of both lists fails loudly by
# simply never running.
PROTOCOLS = {
    'jeep': {
        'defines': [],
        'exclude': {'test_head_unit_raise.cpp', 'test_raise_benz_tx.cpp'},
    },
    'benz': {
        'defines': ['CANBOX_PROTOCOL_BENZ'],
        'exclude': {'test_head_unit.cpp', 'test_custom_tx.cpp'},
    },
}

CXXFLAGS = [
    '-std=c++17',
    '-DCANBOX_HOST_TEST',
    '-Wall', '-Wextra', '-Wpedantic',
    '-Wshadow', '-Wconversion', '-Wsign-conversion',
    '-Wno-unused-parameter',
    '-g', '-O1',
]


def find_compiler():
    for name in (os.environ.get('HOST_CXX'), 'clang++', 'g++'):
        if not name:
            continue
        path = shutil.which(name)
        if path:
            return path
    return None


def sources():
    srcs = []
    for d in SRC_DIRS:
        full = os.path.join(ROOT, d)
        if not os.path.isdir(full):
            continue
        for f in sorted(os.listdir(full)):
            if f.endswith('.cpp') and f not in EXCLUDE:
                srcs.append(os.path.join(d, f))

    harness = os.path.join('tests', 'host')
    for f in sorted(os.listdir(os.path.join(ROOT, harness))):
        if f.endswith('.cpp'):
            srcs.append(os.path.join(harness, f))
    return srcs


def build_and_run(cxx, name, proto, args, srcs):
    """Compile and run the suite for one protocol. Returns the process exit code."""
    srcs = [s for s in srcs if os.path.basename(s) not in proto['exclude']]

    test_files = [s for s in srcs if os.path.basename(s).startswith('test_')]
    if not test_files:
        print('error: no tests/host/test_*.cpp selected for protocol %s.' % name,
              file=sys.stderr)
        return 1

    os.makedirs(OUT, exist_ok=True)
    stem = 'canbox_tests_%s' % name
    exe = os.path.join(OUT, stem + ('.exe' if os.name == 'nt' else ''))

    cmd = ([cxx] + CXXFLAGS
           + ['-D' + d for d in proto['defines']]
           + ['-I' + os.path.join(ROOT, d) for d in INCLUDE_DIRS]
           + [os.path.join(ROOT, s) for s in srcs]
           + ['-o', exe])

    if args.verbose:
        print(' '.join(cmd))
    print('[%s] building %d translation unit%s...'
          % (name, len(srcs), '' if len(srcs) == 1 else 's'))

    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.stdout.strip():
        print(r.stdout.rstrip())
    if r.stderr.strip():
        print(r.stderr.rstrip(), file=sys.stderr)
    if r.returncode != 0:
        print('\nFAILED to build the host tests for protocol %s' % name, file=sys.stderr)
        return r.returncode

    run_cmd = [exe]
    if args.filter:
        run_cmd += ['-k', args.filter]

    print()
    return subprocess.run(run_cmd, cwd=ROOT).returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--protocol', choices=sorted(PROTOCOLS) + ['both'], default='both',
                    help='which head-unit dialect to test (default: both)')
    ap.add_argument('-k', dest='filter', help='only run cases matching this substring')
    ap.add_argument('--units', nargs='+', metavar='NAME',
                    help='build only these module basenames (plus their tests and the '
                         'harness) instead of every unit. Lets one module be developed '
                         'and tested before its neighbours exist.')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--clean', action='store_true')
    args = ap.parse_args()

    if args.clean:
        shutil.rmtree(OUT, ignore_errors=True)
        print('cleaned build/host/')
        return 0

    cxx = find_compiler()
    if cxx is None:
        print('error: no host C++ compiler found (tried $HOST_CXX, clang++, g++).\n'
              'Install one, or set HOST_CXX=/path/to/c++.', file=sys.stderr)
        return 1

    srcs = sources()
    if args.units:
        wanted = set(args.units)
        keep = []
        for s in srcs:
            base = os.path.basename(s)
            stem = base[:-4]
            if s.startswith(os.path.join('tests', 'host')):
                # the harness is always in; unit tests only for requested modules
                if base in HARNESS_ALWAYS or not base.startswith('test_'):
                    keep.append(s)
                elif stem[len('test_'):] in wanted:
                    keep.append(s)
            elif stem in wanted:
                keep.append(s)
        present = {os.path.basename(s)[:-4] for s in keep}
        missing = {u for u in wanted if u not in present and ('test_' + u) not in present}
        if missing:
            print('warning: no source found for unit(s): %s' % ', '.join(sorted(missing)),
                  file=sys.stderr)
        srcs = keep

    wanted = sorted(PROTOCOLS) if args.protocol == 'both' else [args.protocol]
    rc = 0
    for name in wanted:
        if len(wanted) > 1:
            print('=== protocol: %s ===' % name)
        one = build_and_run(cxx, name, PROTOCOLS[name], args, srcs)
        # Run every configuration even after one fails: knowing that the Benz
        # build is also broken is worth the extra few seconds.
        rc = rc or one
        if len(wanted) > 1:
            print()
    return rc


if __name__ == '__main__':
    sys.exit(main())
