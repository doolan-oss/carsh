// Decoder factory + format sniffing (PLAN.md §2.8).
//
// openAuto() peeks the stream's magic bytes, constructs the matching decoder,
// and opens it — the single entry point the PlayerController/decode task uses.
#pragma once

#include <memory>

#include "decode/IDecoder.h"
#include "hal/Vfs.h"

namespace carsh::decode {

// Peeks magic bytes (rewinding the stream) to identify the codec.
Codec sniff(carsh::hal::IStream* stream);

// Constructs an unopened decoder for the codec (nullptr if unsupported).
std::unique_ptr<IDecoder> createDecoder(Codec c);

// Sniff -> create -> open. Returns nullptr on unknown/failed open. The returned
// decoder borrows `stream`; keep it alive for the decoder's lifetime.
std::unique_ptr<IDecoder> openAuto(carsh::hal::IStream* stream);

// Extension-based hint (used by the indexer to skip non-audio files cheaply).
Codec codecFromExtension(const std::string& path);

}  // namespace carsh::decode
