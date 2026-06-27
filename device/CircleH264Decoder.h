// CircleH264Decoder — the Android Auto video decode backend behind IH264Decoder
// (AA_PLAN rung 5). Software H.264 (Cisco openh264, BSD, Constrained-Baseline +
// NEON) since bare-metal Circle has no path to the VideoCore HW decoder (that needs
// VCHIQ/MMAL, i.e. Linux). feed() pushes one Annex-B access unit per AV media
// message into openh264; each decoded I420 picture is converted to RGB888
// (Yuv420.h) and handed to the frame sink, which blits it into the AA tab.
//
// This unit depends only on openh264 (not Circle), so it compiles into BOTH the
// host test build (system libopenh264) — where an encode->decode->checksum round
// trip proves the decode + conversion path — and the device image (vendored
// openh264 for aarch64, linked via `make CARSH_HAVE_OPENH264=1`). [hw]: on-target
// FPS at 800x480, and running feed() on a dedicated A72 core (CMultiCoreSupport).
#ifndef _carsh_circle_h264_decoder_h
#define _carsh_circle_h264_decoder_h

#include <cstddef>
#include <cstdint>
#include <vector>

#include "aa/IH264Decoder.h"

class ISVCDecoder;  // openh264, forward-declared to keep the header dependency-free

namespace carsh::device {

class CircleH264Decoder : public carsh::aa::IH264Decoder {
public:
    explicit CircleH264Decoder(carsh::aa::IH264FrameSink* sink);
    ~CircleH264Decoder() override;

    CircleH264Decoder(const CircleH264Decoder&) = delete;
    CircleH264Decoder& operator=(const CircleH264Decoder&) = delete;

    // Create + initialise the openh264 decoder. Returns false on failure (leaving
    // feed() a no-op).
    bool init();

    // IH264Decoder: feed one Annex-B access unit; emits 0 or 1 frame to the sink.
    void feed(const uint8_t* annexB, size_t len) override;

    int width() const { return width_; }
    int height() const { return height_; }
    unsigned framesDecoded() const { return framesDecoded_; }

private:
    void emit(unsigned char* plane[3], int w, int h, int strideY, int strideUV);

    carsh::aa::IH264FrameSink* sink_;
    ISVCDecoder* dec_ = nullptr;
    std::vector<uint8_t> rgb_;  // reused RGB888 scratch for the latest frame
    int width_ = 0;
    int height_ = 0;
    unsigned framesDecoded_ = 0;
};

}  // namespace carsh::device

#endif  // _carsh_circle_h264_decoder_h
