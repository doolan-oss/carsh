// Single translation unit that compiles the dr_libs implementations.
// NO_STDIO forces the custom-callback IO path (we feed bytes via hal::IStream),
// which is also what bare-metal needs — no FILE* on the Pi.
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO

#include "dr_libs/dr_wav.h"
#include "dr_libs/dr_flac.h"
#include "dr_libs/dr_mp3.h"
