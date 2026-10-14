# Makefile -- a thin wrapper around tools/build.py.
#
# The real build driver is Python, because the dev box here has no make and the project
# already depends on Python for iaptool.py / iap_upload.py. This file exists so `make`
# works for anyone who has it, and it DELEGATES rather than duplicating: an independent
# source list here would drift the moment a file is added to app/services/ or app/logic/,
# and the failure mode of that drift is a module silently missing from the image.
#
#   make            build + verify the preamble + integration checks
#   make pack       ... and package to build/app.iap
#   make test       host unit tests
#   make flash      pack and upload over UART  (PORT=COMxx)
#   make clean
#
# Override the toolchain prefix with CROSS=..., which build.py honours via the environment:
#   make CROSS=/path/to/arm-none-eabi-

PYTHON ?= python
PORT   ?= COM17

# Exported so tools/build.py picks it up; it reads $CROSS and otherwise uses its default.
ifdef CROSS
export CROSS
endif

.PHONY: all pack test flash clean verify

all:
	$(PYTHON) tools/build.py

pack:
	$(PYTHON) tools/build.py --pack

test:
	$(PYTHON) tools/test.py

verify:
	$(PYTHON) tools/verify_preamble.py build/app.bin \
	    --compare reference/2429-H1N4JPF20A-220524.app.bin
	$(PYTHON) tools/check_stubs.py --elf build/app.elf

# Flashing is destructive to the running image. Read app/REBUILD.md section 6 first:
# --reset powercycle is the safe default, and the line must be quiet during an update.
flash: pack
	$(PYTHON) tools/iap_upload.py build/app.iap --port $(PORT) \
	    --reset powercycle --verify

clean:
	$(PYTHON) tools/build.py --clean
	$(PYTHON) tools/test.py --clean
