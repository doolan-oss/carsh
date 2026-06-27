// FakeH264Decoder — host IH264Decoder that emits one synthetic RGB frame to its
// sink per feed(), recording what it was fed. Lets the projection wiring (video
// media -> decoder.feed -> onFrame -> showFrame) be host-tested without a real
// codec. See tests/aa/AaProjection_test.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "aa/IH264Decoder.h"

namespace carsh::host {

class FakeH264Decoder : public carsh::aa::IH264Decoder {
public:
    explicit FakeH264Decoder(carsh::aa::IH264FrameSink* sink, int w = 8, int h = 8)
        : sink_(sink), w_(w), h_(h), rgb_(static_cast<size_t>(w) * h * 3, 0) {}

    void feed(const uint8_t* annexB, size_t len) override {
        lastFed.assign(annexB, annexB + len);
        ++frames;
        // Paint a value derived from the input so successive frames differ.
        for (auto& b : rgb_) b = static_cast<uint8_t>(len & 0xFF);
        if (sink_ != nullptr) {
            carsh::aa::DecodedFrame f;
            f.width = w_;
            f.height = h_;
            f.pixels = rgb_.data();
            f.pixelsLen = rgb_.size();
            sink_->onFrame(f);
        }
    }

    std::vector<uint8_t> lastFed;
    int frames = 0;

private:
    carsh::aa::IH264FrameSink* sink_;
    int w_, h_;
    std::vector<uint8_t> rgb_;
};

}  // namespace carsh::host
