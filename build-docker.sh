#!/usr/bin/env bash
#
# build-docker.sh -- build the canbox firmware in a container, on any machine.
#
# Needs nothing but Docker (or Podman). No cross toolchain, no Python, no
# compiler on the host. The image is built once from docker/Dockerfile and
# reused; the repo is bind-mounted, so the packed .iap appears in build/ just as
# a native build would leave it.
#
#   ./build-docker.sh                      tests, then jeep + benz, packed
#   ./build-docker.sh --protocol jeep      only the stock-equivalent image
#   ./build-docker.sh --no-tests           skip the host unit tests
#   ./build-docker.sh --tests-only         host unit tests and nothing else
#   ./build-docker.sh --clean              remove build/
#   ./build-docker.sh --shell              interactive shell in the builder
#   ./build-docker.sh --rebuild-image      force a fresh image build
#
# Flashing stays on the host and out of the container -- it needs the USB serial
# port and an SWD probe:
#
#   python tools/iap_upload.py build/canupdate<version>.iap --port /dev/ttyUSB0 \
#          --reset powercycle --verify
#
# Windows: use build-docker.ps1. This script also works in Git Bash / MSYS2,
# where it converts the mount path for Docker Desktop itself.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${CANBOX_IMAGE:-canbox-builder}"
TAG="${CANBOX_TAG:-15.2.1-1.1}"

REBUILD_IMAGE=0
SHELL_MODE=0
PASSTHRU=()

usage() {
    cat <<'USAGE'
usage: ./build-docker.sh [options]

Launcher options (handled here):
  --rebuild-image               force a fresh build of the builder image
  --shell                       interactive shell inside the builder
  -h, --help                    this

Build options (passed to the container):
  --protocol <jeep|benz|both>   which head-unit dialect to build (default: both)
  --no-tests                    skip the host unit tests
  --tests-only                  run the host unit tests and stop
  --clean                       remove build/ and stop
  -v, --verbose                 echo the real compiler command lines

Environment:
  DOCKER         container runtime to use (default: docker, else podman)
  CANBOX_IMAGE   image name  (default: canbox-builder)
  CANBOX_TAG     image tag   (default: the pinned toolchain version)
USAGE
}

for arg in "$@"; do
    case "$arg" in
        --rebuild-image) REBUILD_IMAGE=1 ;;
        --shell)         SHELL_MODE=1 ;;
        -h|--help)       usage; exit 0 ;;
        *)               PASSTHRU+=("$arg") ;;
    esac
done

# --- container runtime ----------------------------------------------------
DOCKER="${DOCKER:-}"
if [[ -z "$DOCKER" ]]; then
    if command -v docker >/dev/null 2>&1; then DOCKER=docker
    elif command -v podman >/dev/null 2>&1; then DOCKER=podman
    else
        echo "error: neither docker nor podman is on PATH." >&2
        echo "       Install Docker (https://docs.docker.com/get-docker/) or set DOCKER=..." >&2
        exit 1
    fi
fi

if ! "$DOCKER" info >/dev/null 2>&1; then
    echo "error: '$DOCKER' is installed but its daemon is not reachable." >&2
    echo "       Start Docker Desktop (or the docker service) and try again." >&2
    exit 1
fi

# --- host specifics -------------------------------------------------------
# On Windows shells MSYS rewrites /work into a drive path before docker sees it,
# and the uid from `id -u` means nothing to a Docker Desktop VM, so both are
# handled differently there.
# Ownership of the build output is NOT handled here with --user. The entrypoint
# reads the owner off the bind-mounted tree and becomes them, which is the only
# approach that survives the same working tree being driven from both Windows
# and WSL -- see the long comment at the top of docker/build-all.sh.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # MSYS rewrites /work into a drive path before docker ever sees it, so
        # path conversion is switched off and every path docker is handed -- the
        # bind mount AND the build context -- is spelled the Windows way here.
        MOUNT_SRC="$(cd "$ROOT" && pwd -W 2>/dev/null || cygpath -w "$ROOT")"
        CONTEXT_DIR="$MOUNT_SRC/docker"
        export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
        ;;
    *)
        MOUNT_SRC="$ROOT"
        CONTEXT_DIR="$ROOT/docker"
        ;;
esac

# --- image ----------------------------------------------------------------
# The image is rebuilt on every run, not just when it is missing: the entrypoint
# lives inside it, so an edit to docker/build-all.sh would otherwise go unnoticed
# and silently run the stale copy. A cached rebuild costs about a second.
if [[ $REBUILD_IMAGE -eq 1 ]] || ! "$DOCKER" image inspect "$IMAGE:$TAG" >/dev/null 2>&1; then
    echo ">>> building image $IMAGE:$TAG (one time, a few minutes)"
    "$DOCKER" build -t "$IMAGE:$TAG" -t "$IMAGE:latest" "$CONTEXT_DIR"
    echo
else
    "$DOCKER" build -q -t "$IMAGE:$TAG" -t "$IMAGE:latest" "$CONTEXT_DIR" >/dev/null
fi

# --- run ------------------------------------------------------------------
if [[ $SHELL_MODE -eq 1 ]]; then
    exec "$DOCKER" run --rm -it \
        -v "$MOUNT_SRC:/work" -w /work --entrypoint /bin/bash "$IMAGE:$TAG"
fi

exec "$DOCKER" run --rm \
    -v "$MOUNT_SRC:/work" -w /work "$IMAGE:$TAG" ${PASSTHRU[@]+"${PASSTHRU[@]}"}
