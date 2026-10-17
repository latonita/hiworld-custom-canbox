# syntax=docker/dockerfile:1
#
# Build environment for the Hiworld canbox APP (Nations N32L406, Cortex-M4F).
#
# This image exists so the firmware builds identically on any machine without
# anyone having to install a cross toolchain first. It contains exactly what
# `python tools/build.py` and `python tools/test.py` need and nothing else:
#
#   * arm-none-eabi GCC *with C++ support* -- the firmware is C11 + C++17, and
#     Debian's own gcc-arm-none-eabi package famously ships no arm-none-eabi-g++,
#     which is why the toolchain is pulled from xPack instead of apt.
#   * binutils >= 2.39 -- tools/build.py passes -Wl,--no-warn-rwx-segments
#     unconditionally and older ld rejects the option outright.
#   * a host g++ for the x86 unit tests in tests/host/.
#   * python3, stdlib only. The tooling has no third-party dependencies.
#
# The toolchain version is pinned to the one the project was developed against
# (xPack GNU Arm Embedded GCC 15.2.1-1.1) and checked against the publisher's
# sha256, so a rebuild of this image a year from now still produces the same
# firmware bytes. Bump TOOLCHAIN_VERSION and both checksums together.
#
# The repo itself is NOT copied in -- it is bind-mounted at /work when the
# container runs, so the .iap lands in the working tree's build/ exactly where a
# native build would put it, ready for tools/iap_upload.py to flash from the
# host. See docker/build-all.sh and build-docker.{sh,ps1}.

ARG DEBIAN_TAG=bookworm-slim

# --------------------------------------------------------------------------
# Stage 1: fetch, verify and prune the cross toolchain.
# Kept separate so the ~200 MB tarball never becomes a layer of the final image.
# --------------------------------------------------------------------------
FROM debian:${DEBIAN_TAG} AS toolchain

ARG TOOLCHAIN_VERSION=15.2.1-1.1
# Published by the xPack project as <asset>.sha alongside each release asset.
ARG TOOLCHAIN_SHA256_AMD64=da6a49ad4003944b823c6c93702a8787c922ab34bd7e918ec0eaf6933a9b1ff6
ARG TOOLCHAIN_SHA256_ARM64=67980c7990eba7bb7ffdf39699102effd70889f5ac427be19a8c8a6c5fab2972
# Supplied automatically by BuildKit. The fallback keeps the legacy builder working.
ARG TARGETARCH

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    arch="${TARGETARCH:-$(dpkg --print-architecture)}"; \
    case "${arch}" in \
      amd64) suffix=x64;   sha="${TOOLCHAIN_SHA256_AMD64}" ;; \
      arm64) suffix=arm64; sha="${TOOLCHAIN_SHA256_ARM64}" ;; \
      *) echo "unsupported architecture '${arch}' -- xPack publishes linux x64 and arm64 only" >&2; exit 1 ;; \
    esac; \
    file="xpack-arm-none-eabi-gcc-${TOOLCHAIN_VERSION}-linux-${suffix}.tar.gz"; \
    url="https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v${TOOLCHAIN_VERSION}/${file}"; \
    echo "fetching ${url}"; \
    curl -fsSL --retry 3 --retry-delay 5 -o /tmp/toolchain.tar.gz "${url}"; \
    echo "${sha}  /tmp/toolchain.tar.gz" | sha256sum -c -; \
    mkdir -p /opt/toolchain; \
    tar -xzf /tmp/toolchain.tar.gz -C /opt/toolchain --strip-components=1; \
    rm -f /tmp/toolchain.tar.gz

# Prune. The xPack distribution unpacks to 1.1 GB because it carries newlib and
# libstdc++ built for every Arm variant GCC supports, plus a full gdb with an
# embedded CPython. This container compiles for exactly one part and never
# debugs one -- flashing and SWD happen on the host, outside the container.
#
# Everything removed here fails LOUDLY if it is ever needed again (a missing
# library is a link error, never a miscompile), and the smoke test below
# link-tests the real flag set *after* the pruning, so a bad prune breaks the
# image build rather than someone's firmware.
RUN set -eux; \
    rm -f  /opt/toolchain/bin/*gdb*; \
    rm -rf /opt/toolchain/lib/python3.13; \
    rm -f  /opt/toolchain/libexec/libpython3* \
           /opt/toolchain/libexec/libcrypto.so* \
           /opt/toolchain/libexec/libssl.so* \
           /opt/toolchain/libexec/libsqlite3* \
           /opt/toolchain/libexec/libreadline.so* \
           /opt/toolchain/libexec/libncursesw.so* \
           /opt/toolchain/libexec/libpanelw.so* \
           /opt/toolchain/libexec/libmpdec.so*; \
    rm -rf /opt/toolchain/share/doc /opt/toolchain/arm-none-eabi/share/doc; \
    for libdir in /opt/toolchain/arm-none-eabi/lib /opt/toolchain/lib/gcc/arm-none-eabi/*; do \
        [ -d "${libdir}/thumb" ] || continue; \
        find "${libdir}/thumb" -mindepth 1 -maxdepth 1 -type d \
             ! -name 'v7e-m*' ! -name 'nofp' -exec rm -rf {} +; \
        rm -rf "${libdir}/arm"; \
    done; \
    du -sh /opt/toolchain

# The multilib pruning above keeps thumb/v7e-m* -- the N32L406 is a Cortex-M4F,
# so `-mcpu=cortex-m4 -mthumb -mfloat-abi=soft` selects thumb/v7e-m/nofp, and the
# sibling +fp/+dp variants stay so flipping -mfloat-abi still links. Plain
# thumb/nofp stays for a compile with no -mcpu. v6-m, v7-a, v8-m, A-profile and
# the non-thumb arm/ tree cannot be reached from this project's flags at all.

# Smoke test: the exact toolchain invocations tools/build.py makes, so a pruned
# library or a corrupt download is caught here, not six minutes into a build.
RUN set -eux; \
    cd /tmp; \
    printf 'int c_fn(void){return 1;}\n' > t.c; \
    printf 'extern "C" int c_fn(void); int cpp_fn(void){return c_fn();}\n' > t.cpp; \
    export PATH=/opt/toolchain/bin:$PATH; \
    flags="-mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -ffreestanding -ffunction-sections"; \
    arm-none-eabi-gcc ${flags} -std=gnu11 -c t.c -o tc.o; \
    arm-none-eabi-g++ ${flags} -std=c++17 -fno-exceptions -fno-rtti -c t.cpp -o tcpp.o; \
    arm-none-eabi-g++ ${flags} -nostdlib -nostartfiles -Wl,-e,0 \
        -Wl,--gc-sections -Wl,--no-warn-rwx-segments tc.o tcpp.o -o t.elf; \
    arm-none-eabi-objcopy -O binary t.elf t.bin; \
    arm-none-eabi-nm t.elf > /dev/null; \
    arm-none-eabi-size t.elf; \
    rm -f t.c t.cpp tc.o tcpp.o t.elf t.bin; \
    arm-none-eabi-g++ --version | head -1; \
    arm-none-eabi-ld --version | head -1

# --------------------------------------------------------------------------
# Stage 2: the builder image.
# --------------------------------------------------------------------------
FROM debian:${DEBIAN_TAG}

ARG TOOLCHAIN_VERSION=15.2.1-1.1

LABEL org.opencontainers.image.title="canbox-builder" \
      org.opencontainers.image.description="Build environment for the Hiworld canbox N32L406 APP rebuild" \
      canbox.toolchain="xpack-arm-none-eabi-gcc-${TOOLCHAIN_VERSION}"

# g++ is for tests/host/ (tools/test.py looks for $HOST_CXX, then clang++, then
# g++). bash is already in debian-slim and is what `--shell` sessions land in.
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 g++ \
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf /usr/bin/python3 /usr/local/bin/python

COPY --from=toolchain /opt/toolchain /opt/toolchain

# CROSS is the one knob tools/build.py and tools/check_stubs.py read to find the
# cross toolchain; without it they fall back to the author's D:/IDE path.
# PYTHONDONTWRITEBYTECODE keeps the container from littering the bind-mounted
# working tree with __pycache__ owned by whatever uid the container ran as.
ENV CROSS=/opt/toolchain/bin/arm-none-eabi- \
    PATH=/opt/toolchain/bin:$PATH \
    HOME=/tmp \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

COPY build-all.sh /usr/local/bin/canbox-build
RUN chmod 0755 /usr/local/bin/canbox-build

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/canbox-build"]
