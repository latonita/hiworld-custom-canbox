#!/bin/sh
#
# canbox-build -- the container entrypoint.
#
# Runs the host unit tests, then builds and packs every requested protocol,
# leaving the flashable containers in the bind-mounted working tree:
#
#     build/canupdateH1N4JPF20L-<date>.iap    jeep  (stock-equivalent)
#     build/canupdateH1N4BNZ10L-<date>.iap    benz  (Mercedes emulation)
#
# Flashing is deliberately NOT done here: tools/iap_upload.py needs a USB serial
# port and an SWD probe, which do not belong in a build container. Run it on the
# host against the .iap this produced.
#
# Invoked by build-docker.sh / build-docker.ps1; usable directly as
#     docker run --rm -v "$PWD:/work" canbox-builder [options]

set -eu

# ---------------------------------------------------------------------------
# Run the build as whoever owns the bind-mounted tree.
#
# The container starts as root, which would leave every object file and .iap in
# the working tree owned by root. That is merely untidy on a Linux host, but it
# actively breaks the Windows+WSL combination: a Windows-path bind mount reports
# every file as 0:0 and writes 0:0 metadata, while the same tree reached through
# WSL (/mnt/d/...) reports the real uid -- so a build/ left behind by
# build-docker.ps1 is unwritable by build-docker.sh, and vice versa.
#
# So: read the owner off /work and become them. On a Windows-path mount the
# owner reads back as root and nothing changes (Windows has no use for the uid
# anyway); everywhere else -- native Linux, macOS, WSL -- the output comes back
# owned by the caller. Rootless podman maps the caller to root already and is
# likewise left alone.
#
# CANBOX_UID / CANBOX_GID override the detection; an explicit `docker run
# --user` skips it entirely, since there is then nothing to drop from.
# ---------------------------------------------------------------------------
if [ "$(id -u)" = "0" ] && [ -z "${CANBOX_DROPPED:-}" ] && [ -d /work ]; then
    owner_uid="${CANBOX_UID:-$(stat -c %u /work)}"
    owner_gid="${CANBOX_GID:-$(stat -c %g /work)}"
    if [ "${owner_uid}" != "0" ]; then
        # Repair a build/ left behind by a run from the other side, or the first
        # write below fails with a bare EACCES. It has to be a delete, not a
        # chown: these shares report a fixed owner per mount path and silently
        # ignore chown (it returns success and changes nothing). Deleting is
        # safe -- build/ is generated output, ignored by git, and this costs one
        # full rebuild exactly once per direction switch.
        # The test is recursive, not just on build/ itself: a run from the other
        # side can create a fresh subdirectory (build/benz/, build/host/) as root
        # with umask 022 inside an otherwise fine build/, and that one directory
        # is enough to fail the link.
        intruder="$(find /work/build ! -uid "${owner_uid}" -print -quit 2>/dev/null || true)"
        if [ -n "${intruder}" ]; then
            echo "note: build/ holds files owned by another uid; this build runs as ${owner_uid}."
            echo "      Removing it -- it is generated output, and this share ignores chown."
            rm -rf /work/build
        fi
        export CANBOX_DROPPED=1
        exec setpriv --reuid "${owner_uid}" --regid "${owner_gid}" --clear-groups "$0" "$@"
    fi
fi

PROTOCOLS="jeep benz"
RUN_TESTS=1
BUILD_FIRMWARE=1
VERBOSE=""

usage() {
    cat <<'USAGE'
usage: canbox-build [options]

  --protocol <jeep|benz|both>   which head-unit dialect to build (default: both)
  --no-tests                    skip the host unit tests
  --tests-only                  run the host unit tests and stop
  --clean                       remove build/ and stop
  -v, --verbose                 echo the real compiler command lines
  -h, --help                    this
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --protocol)   shift; [ $# -gt 0 ] || { echo "error: --protocol needs a value" >&2; exit 2; }; PROTOCOLS="$1" ;;
        --protocol=*) PROTOCOLS="${1#*=}" ;;
        --no-tests|--skip-tests) RUN_TESTS=0 ;;
        --tests-only) BUILD_FIRMWARE=0 ;;
        --clean)      CLEAN=1 ;;
        -v|--verbose) VERBOSE="-v" ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "error: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# NB: written as an `if`, not `[ ... ] && ...` -- under `set -e` a false test as
# the last command of a line exits the script.
if [ "${PROTOCOLS}" = "both" ]; then
    PROTOCOLS="jeep benz"
fi

for proto in ${PROTOCOLS}; do
    case "${proto}" in
        jeep|benz) ;;
        *) echo "error: unknown protocol '${proto}' (want jeep, benz or both)" >&2; exit 2 ;;
    esac
done

# Set by the image; defaulted so the script also works in a plain shell session.
CROSS="${CROSS:-arm-none-eabi-}"

# A missing or wrong bind mount is the one failure mode that would otherwise
# show up as a confusing "No such file" from python.
if [ ! -f tools/build.py ] || [ ! -f app/linker/app.ld ]; then
    echo "error: /work does not contain the canbox repo (no tools/build.py)." >&2
    echo "       Mount it, e.g.  docker run --rm -v \"\$PWD:/work\" canbox-builder" >&2
    exit 2
fi

if [ -n "${CLEAN:-}" ]; then
    python3 tools/build.py --clean
    python3 tools/test.py --clean
    exit 0
fi

echo "=============================================================="
echo " canbox builder"
echo "   cross : $("${CROSS}gcc" -dumpversion) ($("${CROSS}ld" --version | head -1))"
echo "   host  : $(g++ -dumpversion) / $(python3 --version)"
if [ "${BUILD_FIRMWARE}" -eq 1 ]; then
    echo "   build : ${PROTOCOLS}"
else
    echo "   build : (host unit tests only)"
fi
echo "=============================================================="
echo

if [ "${RUN_TESTS}" -eq 1 ]; then
    echo "--- host unit tests ------------------------------------------"
    python3 tools/test.py
    echo
fi

[ "${BUILD_FIRMWARE}" -eq 1 ] || exit 0

for proto in ${PROTOCOLS}; do
    echo "--- firmware: ${proto} ---------------------------------------"
    # --pack is the point of the exercise: the .iap is what the radio's upgrade
    # screen accepts. build.py verifies the preamble and the integration
    # invariants on the way, and fails the container if either breaks.
    python3 tools/build.py --protocol "${proto}" --pack ${VERBOSE}
    echo
done

echo "--- output ---------------------------------------------------"
# sha256 so two machines can confirm they produced the same image.
for f in build/*.iap; do
    [ -e "${f}" ] || { echo "error: no .iap was produced" >&2; exit 1; }
    printf '  %-44s %10d bytes\n' "${f}" "$(wc -c < "${f}")"
    printf '    sha256 %s\n' "$(sha256sum "${f}" | cut -d' ' -f1)"
done
echo
echo "Flash from the HOST (not from this container):"
echo "  python tools/iap_upload.py build/canupdate<version>.iap --port COM17 --reset powercycle --verify"
