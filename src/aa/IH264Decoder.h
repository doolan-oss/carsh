// IH264Decoder — the H.264 decode seam for the Android Auto video channel. The
// connection feeds Annex-B chunks (one per video media message) and the decoder
// emits decoded frames to a sink. Keeping decode behind an interface lets the
// connection/video-routing logic be host-tested with a stub, while the device
// supplies the real software decoder running on a dedicated A72 core.
//
// Status: the device-side software decoder + the on-target FPS measurement are
// AA_PLAN Phase 2 [hw] work (the reference uses a portable SW H.264 decoder that
// can be vendored for both host and device). NAL extraction/classification that
// feeds this is already pure + host-tested (H264Nal.h).
#pragma once

#include <cstddef>
#include <cstdint>

namespace carsh::aa {

// A decoded picture. Pixel layout is implementation-defined (I420 or RGB565);
// the device backend converts to the framebuffer format before the sink blits.
struct DecodedFrame {
    int width = 0;
    int height = 0;
    const uint8_t* pixels = nullptr;
    size_t pixelsLen = 0;
};

class IH264FrameSink {
public:
    virtual ~IH264FrameSink() = default;
    virtual void onFrame(const DecodedFrame& frame) = 0;
};

class IH264Decoder {
public:
    virtual ~IH264Decoder() = default;

    // Feed one Annex-B chunk (a whole video media message's bitstream). Decoded
    // pictures are delivered to the sink during/after the call. Non-picture NALs
    // (SPS/PPS/SEI) update decoder state and produce no frame.
    virtual void feed(const uint8_t* annexB, size_t len) = 0;
};

}  // namespace carsh::aa
