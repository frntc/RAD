# Top-level convenience wrapper around tools/build.sh -- lets you run
# plain `make` instead of `tools/build.sh`. See dev/hw_testing.md for
# the full build/deploy story and toolchain requirements.
#
# Usage:
#   make                        # build kernel8.img
#   make libs                   # also (re)build the Circle libraries
#   make clean                  # remove the Circle app/Firmware build tree
#   make SDCARD=/media/you/RAD  # build + deploy to a mounted SD card
#   make examples               # assemble + simulate every example and game
#   make tests                  # the conformance suite, on the PC model
#   make copy                   # copy last build onto the RAD SD card + unmount it
#
# PREFIX64/RASPPI/AARCH/JOBS are forwarded to tools/build.sh the same way
# (see that script for what each one does / defaults to).

SDCARD   ?=
PREFIX64 ?=
RASPPI   ?=
AARCH    ?=
JOBS     ?=

BUILD_ENV = SDCARD="$(SDCARD)" PREFIX64="$(PREFIX64)" RASPPI="$(RASPPI)" AARCH="$(AARCH)" JOBS="$(JOBS)"

# `make copy` targets the RAD SD card directly -- config.txt on it boots
# whatever file "kernel=" names, which is kernel_rad.img, not Circle's
# default kernel8.img -- see dev/hw_testing.md.
KERNEL_IMG = external/Circle/app/Firmware/kernel8.img
SD_MOUNT  ?= /media/$(USER)/SIDEKICK
SD_KERNEL ?= kernel_rad.img

.PHONY: all libs clean copy examples tests

# PC-side checks: assemble and simulate. No hardware needed.
examples:
	@tools/examples.sh

tests:
	@tools/testprg.sh

all:
	@$(BUILD_ENV) tools/build.sh

libs:
	@$(BUILD_ENV) tools/build.sh --libs

clean:
	rm -rf external/Circle/app/Firmware

copy:
	@test -f "$(KERNEL_IMG)" || { echo "error: $(KERNEL_IMG) not built yet -- run make first" >&2; exit 1; }
	@test -d "$(SD_MOUNT)" || { echo "error: SD_MOUNT=$(SD_MOUNT) is not mounted" >&2; exit 1; }
	cp "$(KERNEL_IMG)" "$(SD_MOUNT)/$(SD_KERNEL)"
	sync
	@dev="$$(findmnt -no SOURCE --target "$(SD_MOUNT)")"; \
	if [ -z "$$dev" ]; then echo "error: could not resolve block device for $(SD_MOUNT)" >&2; exit 1; fi; \
	echo "==> unmounting $$dev ($(SD_MOUNT))"; \
	udisksctl unmount -b "$$dev"
