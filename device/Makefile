#
# carsh device image — links the platform-independent core (built against the
# HAL interfaces) with the Circle device backends into kernel8-rpi4.img.
#
# Requires the circle-stdlib build (run scripts/fetch_circle.sh + build it, and
# put the aarch64-none-elf toolchain on PATH). See device/README.md.
#

CIRCLE_STDLIB := $(realpath $(CURDIR)/../third_party/circle-stdlib)
CARSHTOP      := $(realpath $(CURDIR)/..)

include $(CIRCLE_STDLIB)/Config.mk

# Device entry + backends, plus the core objects this milestone needs.
OBJS = \
	main.o \
	kernel.o \
	$(CARSHTOP)/src/ui/Font.o

include $(CIRCLEHOME)/Rules.mk

INCLUDE += -I$(CARSHTOP)/src -I$(CARSHTOP)/third_party
INCLUDE := $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)
LIBS := $(CIRCLE_STDLIB_LIBS)
CPPFLAGS += -std=c++17

-include $(DEPS)
