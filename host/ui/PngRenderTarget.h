// PngRenderTarget — host IRenderTarget backed by an in-RAM ARGB surface that is
// written to a .png on present() (PLAN.md working method). Lets UI layout be
// verified by image and by structural pixel queries under synthetic touch.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hal/IRenderTarget.h"
#include "img/PngCodec.h"

namespace carsh::host {

class PngRenderTarget : public carsh::hal::IRenderTarget {
public:
    PngRenderTarget(int w, int h, std::string outPath = "")
        : w_(w), h_(h), path_(std::move(outPath)),
          px_(std::size_t(w) * h, 0xFF000000u) {}

    int width() const override { return w_; }
    int height() const override { return h_; }
    std::uint32_t* pixels() override { return px_.data(); }
    const std::uint32_t* pixels() const override { return px_.data(); }

    // On HW this page-flips; on host it snapshots and (if a path was given) saves.
    void present() override {
        ++frames_;
        if (!path_.empty()) save(path_);
    }

    bool save(const std::string& path) const {
        std::vector<uint8_t> rgba(std::size_t(w_) * h_ * 4);
        for (std::size_t i = 0; i < px_.size(); ++i) {
            uint32_t c = px_[i];
            rgba[i * 4 + 0] = uint8_t(c >> 16);  // R
            rgba[i * 4 + 1] = uint8_t(c >> 8);   // G
            rgba[i * 4 + 2] = uint8_t(c);        // B
            rgba[i * 4 + 3] = uint8_t(c >> 24);  // A
        }
        return writePng(path, rgba.data(), w_, h_);
    }

    int frames() const { return frames_; }

private:
    int w_, h_;
    std::string path_;
    std::vector<uint32_t> px_;
    int frames_ = 0;
};

}  // namespace carsh::host
