// Optional decoders that depend on libraries not always present on the build
// server (or not yet ported to bare-metal). The factory dispatches to these when
// compiled in; otherwise the codec sniffs correctly but createDecoder returns
// nullptr and the indexer records the track as "recognized, no decoder".
#pragma once

#include <memory>

#include "decode/IDecoder.h"

namespace carsh::decode {

#ifdef CARSH_HAVE_OPUS
std::unique_ptr<IDecoder> makeOpusDecoder();
#endif

#ifdef CARSH_HAVE_AAC
// ADTS AAC via faad2. MP4/M4A demux is a separate backlog item.
std::unique_ptr<IDecoder> makeAacDecoder();
#endif

}  // namespace carsh::decode
