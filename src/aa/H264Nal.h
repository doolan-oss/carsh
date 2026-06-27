// H264Nal — extract and classify the H.264 NAL units in an Android Auto video
// media message. The AV video channel (channel 3) delivers Annex-B H.264 in
// MEDIA_DATA (msg id 0x0001) or MEDIA_WITH_TS (0x0000, prefixed by an 8-byte
// presentation timestamp). The decoder backend (IH264Decoder) can consume the
// raw Annex-B directly, but the app needs to recognise parameter sets vs picture
// slices vs keyframes (e.g. to gate "video ready" on the first IDR, or to resync
// after loss). That classification is pure and host-testable; this is it.
//
// Annex-B framing: NAL units are separated by a 3- or 4-byte start code
// (00 00 01 / 00 00 00 01); the byte after the start code is
// forbidden_zero_bit(1) | nal_ref_idc(2) | nal_unit_type(5).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace carsh::aa {

// AV channel media message ids (subset relevant to video framing).
enum : uint16_t {
    kAvMediaWithTs = 0x0000,  // 8-byte timestamp prefix, then Annex-B H.264
    kAvMediaData = 0x0001,    // raw Annex-B H.264
};

// H.264 NAL unit types (the ones we care about).
enum class NalType : uint8_t {
    NonIdrSlice = 1,
    IdrSlice = 5,
    Sei = 6,
    Sps = 7,
    Pps = 8,
    Aud = 9,
    Unknown = 0,
};

struct NalUnit {
    NalType type = NalType::Unknown;
    uint8_t rawHeader = 0;        // the full first byte (incl. nal_ref_idc)
    const uint8_t* data = nullptr;  // points into the source bitstream (incl. header byte)
    size_t len = 0;                 // length excluding the start code
};

inline bool isParameterSet(NalType t) { return t == NalType::Sps || t == NalType::Pps; }
inline bool isPictureSlice(NalType t) { return t == NalType::IdrSlice || t == NalType::NonIdrSlice; }
inline bool isKeyframe(NalType t) { return t == NalType::IdrSlice; }

// Strip the media-message envelope to the bare H.264 Annex-B bitstream. For
// MEDIA_WITH_TS the leading 8-byte timestamp is removed. Returns {ptr, len}; the
// pointer aliases `payload`. Returns {nullptr, 0} if there is nothing left.
inline std::pair<const uint8_t*, size_t> extractH264(uint16_t msgId, const uint8_t* payload,
                                                     size_t len) {
    if (msgId == kAvMediaWithTs) {
        if (len <= 8) return {nullptr, 0};
        return {payload + 8, len - 8};
    }
    if (len == 0) return {nullptr, 0};
    return {payload, len};
}

namespace detail {
// Length of the Annex-B start code at `p` (3 or 4), or 0 if none starts here.
inline size_t startCodeLen(const uint8_t* p, size_t remaining) {
    if (remaining >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 4;
    if (remaining >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return 3;
    return 0;
}
}  // namespace detail

// Invoke `fn` for each NAL unit in an Annex-B bitstream. NAL data spans from just
// after its start code to just before the next start code (or end of buffer).
inline void forEachNal(const uint8_t* bs, size_t len, const std::function<void(const NalUnit&)>& fn) {
    size_t i = 0;
    // Advance to the first start code.
    while (i < len && detail::startCodeLen(bs + i, len - i) == 0) ++i;

    while (i < len) {
        size_t sc = detail::startCodeLen(bs + i, len - i);
        if (sc == 0) break;
        size_t nalStart = i + sc;
        // Scan for the next start code to bound this NAL.
        size_t j = nalStart;
        while (j < len && detail::startCodeLen(bs + j, len - j) == 0) ++j;
        if (nalStart < len) {
            NalUnit u;
            u.rawHeader = bs[nalStart];
            u.type = static_cast<NalType>(bs[nalStart] & 0x1F);
            u.data = bs + nalStart;
            u.len = j - nalStart;
            fn(u);
        }
        i = j;
    }
}

}  // namespace carsh::aa
