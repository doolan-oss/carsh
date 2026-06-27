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

# Device entry + backends, plus the platform-independent core objects. These are
# the exact sources the host tests cover — only the backends differ.
OBJS = \
	main.o \
	kernel.o \
	CircleDir.o \
	CircleBluetooth.o \
	CircleAoaTransport.o \
	BtStackGlue.o \
	DeviceCrypto.o \
	$(CARSHTOP)/src/ui/Font.o \
	$(CARSHTOP)/src/library/Database.o \
	$(CARSHTOP)/src/library/Indexer.o \
	$(CARSHTOP)/src/decode/DrDecoders.o \
	$(CARSHTOP)/src/player/PlayerController.o \
	$(CARSHTOP)/src/ota/OtaManager.o \
	$(CARSHTOP)/third_party/dr_libs_impl.o \
	$(CARSHTOP)/third_party/tweetnacl/tweetnacl.o \
	$(CARSHTOP)/third_party/sha256/sha256.o

# ---- Optional: compile BTstack into the image (A2DP sink + AVRCP) ----------
# Build with:  make CARSH_HAVE_BTSTACK=1
# Turns device/BtStackGlue.cpp live and links a curated set of BTstack sources
# (Classic-only: core + classic profiles + bluedroid SBC decoder + the embedded
# run loop), the Circle USB HCI transport, and the Circle BTstack HAL. The
# default build (no flag) links a BT stub and never sees BTstack. The vendored
# tree is third_party/btstack (scripts/fetch_btstack.sh); see device/README.
ifdef CARSH_HAVE_BTSTACK
BTSTACK := $(CARSHTOP)/third_party/btstack
include $(BTSTACK)/src/Makefile.inc            # -> SRC_FILES (core: hci/l2cap/...)
# Curated Classic profile set: the A2DP-sink + AVRCP + SDP + SBC-decoder closure
# only. The full src/classic list pulls in HFP/PAN/PBAP/HID (and their md5/yxml
# deps) we don't ship — keep the BT footprint to the audio path.
BTSTACK_CLASSIC := \
	a2dp.c a2dp_sink.c \
	avdtp.c avdtp_acceptor.c avdtp_initiator.c avdtp_sink.c avdtp_util.c \
	avrcp.c avrcp_controller.c avrcp_target.c avrcp_media_item_iterator.c \
	btstack_sbc_bluedroid.c btstack_sbc_decoder_bluedroid.c btstack_sbc_plc.c \
	btstack_link_key_db_memory.c device_id_server.c \
	sdp_server.c sdp_client.c sdp_util.c
BTSTACK_OBJS := \
	$(addprefix $(BTSTACK)/src/,$(SRC_FILES:.c=.o)) \
	$(addprefix $(BTSTACK)/src/classic/,$(BTSTACK_CLASSIC:.c=.o)) \
	$(patsubst %.c,%.o,$(wildcard $(BTSTACK)/3rd-party/bluedroid/decoder/srce/*.c)) \
	$(patsubst %.c,%.o,$(wildcard $(BTSTACK)/3rd-party/bluedroid/encoder/srce/*.c)) \
	$(BTSTACK)/3rd-party/rijndael/rijndael.o \
	$(BTSTACK)/chipset/realtek/btstack_chipset_realtek.o \
	$(BTSTACK)/platform/embedded/btstack_run_loop_embedded.o
OBJS += HalCircle.o CircleHciTransport.o BtStackClassicShims.o $(BTSTACK_OBJS)
endif

include $(CIRCLEHOME)/Rules.mk

INCLUDE += -I$(CARSHTOP)/src -I$(CARSHTOP)/third_party
ifdef CARSH_HAVE_BTSTACK
# -I$(CURDIR) finds device/btstack_config.h; the rest are the BTstack headers.
INCLUDE += -I$(CURDIR) \
	-I$(BTSTACK)/src -I$(BTSTACK)/platform/embedded \
	-I$(BTSTACK)/3rd-party/bluedroid/decoder/include \
	-I$(BTSTACK)/3rd-party/bluedroid/encoder/include \
	-I$(BTSTACK)/3rd-party/rijndael \
	-I$(BTSTACK)/chipset/realtek
CPPFLAGS += -DCARSH_HAVE_BTSTACK
CFLAGS += -DCARSH_HAVE_BTSTACK
endif
INCLUDE := $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)
# Circle's sound + USB-gadget libraries are not part of the default circle-stdlib
# lib set; add them ahead of the rest (they depend on libusb/libcircle, which come
# later) so the audio-output backends and the USB-C serial-log gadget link.
LIBS := $(CIRCLEHOME)/lib/sound/libsound.a \
        $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
        $(CIRCLE_STDLIB_LIBS)
CPPFLAGS += -std=c++17

-include $(DEPS)
