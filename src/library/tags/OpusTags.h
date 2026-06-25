// OpusTags reader (PLAN.md §2.10). Opus carries tags as a Vorbis comment block
// (prefixed with the 8-byte "OpusTags" magic) in the second Ogg packet. We do a
// minimal Ogg page walk to reassemble the first two packets, then reuse the
// shared Vorbis-comment parser. No decode, no libopus dependency.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "hal/Vfs.h"
#include "library/Model.h"
#include "library/tags/Vorbis.h"

namespace carsh::library::tags {

inline bool parseOpusTags(carsh::hal::IStream* s, TrackMeta& meta) {
    if (!s) return false;
    s->seek(0);
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> cur;
    std::size_t totalRead = 0;

    while (packets.size() < 2 && totalRead < (256u * 1024)) {
        uint8_t ph[27];
        if (s->read(ph, 27) != 27) break;
        totalRead += 27;
        if (std::memcmp(ph, "OggS", 4) != 0) break;
        uint8_t nseg = ph[26];
        std::vector<uint8_t> segtab(nseg);
        if (nseg && s->read(segtab.data(), nseg) != nseg) break;
        totalRead += nseg;
        for (uint8_t i = 0; i < nseg; ++i) {
            uint8_t lace = segtab[i];
            if (lace) {
                std::size_t before = cur.size();
                cur.resize(before + lace);
                if (s->read(cur.data() + before, lace) != lace) return false;
                totalRead += lace;
            }
            if (lace < 255) {  // packet boundary
                packets.push_back(std::move(cur));
                cur.clear();
                if (packets.size() >= 2) break;
            }
        }
    }
    if (packets.size() < 2) return false;
    const auto& tagsPkt = packets[1];
    if (tagsPkt.size() < 8 || std::memcmp(tagsPkt.data(), "OpusTags", 8) != 0)
        return false;
    return parseVorbisComment(tagsPkt.data() + 8, tagsPkt.size() - 8, meta);
}

}  // namespace carsh::library::tags
