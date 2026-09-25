#
# carsh A/B chainloader — the tiny boot kernel that reads boot_slot.bin and
# warm-jumps (Circle chainboot) to SD:/kernel8-A.img or -B.img. Built exactly
# like the app image (circle-stdlib toolchain); produces its own
# kernel8-rpi4.img, which is what goes on the card as the boot kernel.
#
#   cd device/chainloader && make        # -> device/chainloader/kernel8-rpi4.img
#
# See device/chainloader/README.md for the one-time card layout change.
#

CIRCLE_STDLIB := $(realpath $(CURDIR)/../../third_party/circle-stdlib)
CARSHTOP      := $(realpath $(CURDIR)/../..)

include $(CIRCLE_STDLIB)/Config.mk

# circlehooks.o: link-time hooks the carsh-patched circle-stdlib expects (see
# the file). The chainloader must also be built against a libcircle carrying
# scripts/patches/chainboot-multicore.patch (kernel.cpp #errors otherwise).
OBJS = main.o kernel.o circlehooks.o

include $(CIRCLEHOME)/Rules.mk

INCLUDE += -I$(CARSHTOP)/src
INCLUDE := $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)
LIBS := $(CIRCLE_STDLIB_LIBS)
CPPFLAGS += -std=c++17

-include $(DEPS)
