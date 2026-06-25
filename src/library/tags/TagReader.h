// TagReader — dispatch tag parsing by codec (PLAN.md §2.10).
//
// Coverage: MP3 (ID3v2), FLAC (STREAMINFO + Vorbis comment), Opus (OpusTags).
// WAV carries no standard tags here (props come from the decoder). MP4/M4A
// (AAC/ALAC) tag atoms need the MP4 demuxer and are deferred with that work.
#pragma once

#include "decode/IDecoder.h"
#include "hal/Vfs.h"
#include "library/Model.h"
#include "library/tags/FlacTags.h"
#include "library/tags/Id3v2.h"
#include "library/tags/OpusTags.h"

namespace carsh::library::tags {

inline bool readTags(carsh::hal::IStream* s, carsh::decode::Codec codec,
                     TrackMeta& meta) {
    switch (codec) {
        case carsh::decode::Codec::Mp3: return parseId3v2(s, meta);
        case carsh::decode::Codec::Flac: return parseFlacTags(s, meta);
        case carsh::decode::Codec::Opus: return parseOpusTags(s, meta);
        default: return false;  // WAV/AAC(ADTS)/MP4: no tag parser here
    }
}

}  // namespace carsh::library::tags
