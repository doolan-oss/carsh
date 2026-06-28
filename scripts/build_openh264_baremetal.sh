#!/usr/bin/env bash
# Cross-build a DECODER-ONLY openh264 static lib for the Pi 4 bare-metal target
# (AA_PLAN rung 5, device side). The host build links the distro libopenh264; the
# device needs an aarch64 archive built with the SAME ABI/flags Circle uses, so the
# objects link cleanly into kernel8-rpi4.img. We compile the decoder + common
# sources (and their AArch64 NEON .S) directly with the cross compiler instead of
# openh264's hosted-Linux build system, which assumes an OS.
#
# Bare-metal notes:
#   - CPU detect: -DHAVE_NEON_AARCH64 makes WelsCPUFeatureDetect a compile-time
#     constant (NEON+VFPv3, mandatory on ARMv8) — no /proc/cpuinfo, no getauxval.
#   - Threading: the DecodeFrameNoDelay path is single-threaded and never spawns a
#     thread; WelsThreadLib still REFERENCES pthread/sem, so we link a no-op shim
#     (openh264_baremetal_shim.c) for those symbols (newlib has none → no clash).
#   - Headers: openh264 needs newlib's <stdio/stdlib/string.h>, so (unlike Circle
#     code) we do NOT pass -nostdinc; the libc/libstdc++ symbols resolve at the
#     final image link (circle-stdlib bundles newlib).
#
# Output: third_party/openh264/libopenh264.a  (device/Makefile CARSH_HAVE_OPENH264).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OH="$ROOT/third_party/openh264"
SHIM="$ROOT/scripts/openh264_baremetal_shim.c"
TC="${TC:-/opt/arm-toolchain/bin/aarch64-none-elf-}"
CC="${TC}gcc"
CXX="${TC}g++"
AR="${TC}ar"

if [ ! -d "$OH/codec" ]; then
  echo "openh264 source missing — run scripts/fetch_openh264.sh first" >&2
  exit 1
fi

# Match Circle's AArch64 ABI for RASPPI=4 (see circle Rules.mk: cortex-a72,
# AARCH=64, -ffreestanding, signed char, gc-sections). NEON on. No exceptions/rtti
# (Circle is built -fno-exceptions -fno-rtti; the decoder neither throws nor needs
# rtti). NOT -nostdinc: openh264 pulls newlib's hosted headers.
# -D__linux__: aarch64-none-elf predefines no OS, so openh264 picks its BSD path
# (pulls <sys/sysctl.h>, absent here). The linux path needs only sched_getaffinity
# (shimmed to fail->serial) + sysconf (newlib) + pthread (shimmed). For our file set
# this is the ONLY effect: cpu.cpp's __linux__ branches are under 32-bit HAVE_NEON,
# not our HAVE_NEON_AARCH64 path, so they stay inert.
ARCHFLAGS="-mcpu=cortex-a72 -mlittle-endian -DAARCH=64 -DHAVE_NEON_AARCH64 -D__linux__=1"
# NOT -ffreestanding: libstdc++'s <cmath> (pulled via openh264 macros.h -> <math.h>)
# refuses freestanding mode. Dropping it is ABI-safe — it only changes libcall
# assumptions, not the calling convention / struct layout; newlib provides the
# math + mem functions at the final image link.
OPT="-O2 -fsigned-char -ffunction-sections -fdata-sections"
CXXSTD="-std=gnu++14 -fno-exceptions -fno-rtti"
# Shim include dir FIRST: supplies <pthread.h>/<semaphore.h> that newlib bare metal
# lacks (the types come from newlib's <sys/_pthreadtypes.h>; we add prototypes).
INCLUDES="-I$ROOT/scripts/openh264_baremetal_include \
  -I$OH/codec/api/wels -I$OH/codec/common/inc \
  -I$OH/codec/decoder/core/inc -I$OH/codec/decoder/plus/inc"
ASMINC="-I$OH/codec/common/arm64"

BUILD="$OH/baremetal-build"
rm -rf "$BUILD"; mkdir -p "$BUILD"

COMMON_CPP="common_tables copy_mb cpu crt_util_safe_x deblocking_common expand_pic \
  intra_pred_common mc memory_align sad_common utils welsCodecTrace \
  WelsTaskThread WelsThread WelsThreadLib WelsThreadPool"
DEC_CORE_CPP="au_parser bit_stream cabac_decoder deblocking decode_mb_aux decode_slice \
  decoder decoder_core decoder_data_tables error_concealment fmo get_intra_predictor \
  manage_dec_ref memmgr_nal_unit mv_pred parse_mb_syn_cabac parse_mb_syn_cavlc \
  pic_queue rec_mb wels_decoder_thread"
DEC_PLUS_CPP="welsDecoderExt"
COMMON_ASM="copy_mb_aarch64_neon deblocking_aarch64_neon expand_picture_aarch64_neon \
  intra_pred_common_aarch64_neon mc_aarch64_neon"
DEC_ASM="block_add_aarch64_neon intra_pred_aarch64_neon"

objs=()
compile_cpp() { # <srcpath> <tag>
  local out="$BUILD/$2.o"
  echo "  CXX  $2"
  "$CXX" $ARCHFLAGS $OPT $CXXSTD $INCLUDES -c "$1" -o "$out"
  objs+=("$out")
}
compile_asm() { # <srcpath> <tag>
  local out="$BUILD/asm_$2.o"
  echo "  AS   $2"
  "$CC" $ARCHFLAGS $OPT $ASMINC -c "$1" -o "$out"
  objs+=("$out")
}

for f in $COMMON_CPP;  do compile_cpp "$OH/codec/common/src/$f.cpp" "common_$f"; done
for f in $DEC_CORE_CPP; do compile_cpp "$OH/codec/decoder/core/src/$f.cpp" "dec_$f"; done
for f in $DEC_PLUS_CPP; do compile_cpp "$OH/codec/decoder/plus/src/$f.cpp" "decplus_$f"; done
for f in $COMMON_ASM;  do compile_asm "$OH/codec/common/arm64/$f.S" "common_$f"; done
for f in $DEC_ASM;     do compile_asm "$OH/codec/decoder/core/arm64/$f.S" "dec_$f"; done

if [ -f "$SHIM" ]; then
  # Built as C++ (-x c++) so it sees the same newlib pthread-type visibility as the
  # openh264 TUs; the shim header's extern "C" keeps the symbols C-linkage.
  echo "  CXX  baremetal_shim"
  "$CXX" $ARCHFLAGS $OPT $CXXSTD -x c++ -I"$ROOT/scripts/openh264_baremetal_include" \
    -c "$SHIM" -o "$BUILD/shim.o"
  objs+=("$BUILD/shim.o")
fi

echo "  AR   libopenh264.a"
rm -f "$OH/libopenh264.a"
"$AR" rcs "$OH/libopenh264.a" "${objs[@]}"
echo "Done: $OH/libopenh264.a ($(du -h "$OH/libopenh264.a" | cut -f1))"
