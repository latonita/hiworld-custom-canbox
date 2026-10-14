#!/usr/bin/env python3
"""
test.py -- build and run the host unit tests for the canbox APP.

The firmware is cross-compiled for a Cortex-M4 that is bolted into a car, so the
only equivalence evidence available without a bench is a host build of the
layers that do not touch a register: the decoders, the scheduler, the parameter
logic, the software timers, the head-unit framing. Those compile for x86 with
-DCANBOX_HOST_TEST, where ram::app() is a plain object and app/hal/ is replaced
by the recording fakes in tests/host/fake_hal.cpp.

    python tools/test.py                 build + run everything
    python tools/test.py -k steering     only cases whose name contains "steering"
    python tools/test.py -v              echo the compiler command line
    python tools/test.py --clean

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


def main():
    ap = argparse.ArgumentParser()
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

    test_files = [s for s in srcs if os.path.basename(s).startswith('test_')]
    if not test_files:
        print('error: no tests/host/test_*.cpp found -- nothing to run.', file=sys.stderr)
        return 1

    os.makedirs(OUT, exist_ok=True)
    exe = os.path.join(OUT, 'canbox_tests.exe' if os.name == 'nt' else 'canbox_tests')

    cmd = ([cxx] + CXXFLAGS
           + ['-I' + os.path.join(ROOT, d) for d in INCLUDE_DIRS]
           + [os.path.join(ROOT, s) for s in srcs]
           + ['-o', exe])

    if args.verbose:
        print(' '.join(cmd))
    print('building %d translation unit%s...' % (len(srcs), '' if len(srcs) == 1 else 's'))

    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.stdout.strip():
        print(r.stdout.rstrip())
    if r.stderr.strip():
        print(r.stderr.rstrip(), file=sys.stderr)
    if r.returncode != 0:
        print('\nFAILED to build the host tests', file=sys.stderr)
        return r.returncode

    run_cmd = [exe]
    if args.filter:
        run_cmd += ['-k', args.filter]

    print()
    return subprocess.run(run_cmd, cwd=ROOT).returncode


if __name__ == '__main__':
    sys.exit(main())
