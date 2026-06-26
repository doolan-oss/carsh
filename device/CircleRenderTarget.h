// CircleRenderTarget — device backend for hal::IRenderTarget over Circle's
// CBcmFrameBuffer (PLAN.md §2.2). The UI toolkit draws into an in-RAM backbuffer
// (so the toolkit sees a tightly-packed width*height ARGB surface regardless of
// the framebuffer's hardware pitch); present() blits it row-by-row into the real
// framebuffer, which also gives tear-free double buffering.
//
// Color order: the Pi's 32bpp framebuffer is commonly BGRA in memory. If colors
// come out with red/blue swapped on hardware, build with -DCARSH_FB_SWAP_RB.
#ifndef _carsh_circle_render_target_h
#define _carsh_circle_render_target_h

#include <circle/bcmframebuffer.h>
#include <cstdint>
#include <vector>

#include "hal/IRenderTarget.h"

namespace carsh::device {

class CircleRenderTarget : public carsh::hal::IRenderTarget {
public:
    explicit CircleRenderTarget(CBcmFrameBuffer* fb)
        : fb_(fb),
          w_(int(fb->GetWidth())),
          h_(int(fb->GetHeight())),
          pitch_(fb->GetPitch()),
          back_(std::size_t(w_) * h_, 0xFF000000u) {
        base_ = reinterpret_cast<volatile std::uint8_t*>(
            std::uintptr_t(fb->GetBuffer()));
    }

    int width() const override { return w_; }
    int height() const override { return h_; }
    std::uint32_t* pixels() override { return back_.data(); }
    const std::uint32_t* pixels() const override { return back_.data(); }

    void present() override {
        for (int y = 0; y < h_; ++y) {
            volatile std::uint32_t* dst =
                reinterpret_cast<volatile std::uint32_t*>(base_ + std::size_t(y) * pitch_);
            const std::uint32_t* src = &back_[std::size_t(y) * w_];
            for (int x = 0; x < w_; ++x) {
#ifdef CARSH_FB_SWAP_RB
                std::uint32_t c = src[x];
                dst[x] = (c & 0xFF00FF00u) | ((c & 0x00FF0000u) >> 16) |
                         ((c & 0x000000FFu) << 16);
#else
                dst[x] = src[x];
#endif
            }
        }
    }

private:
    CBcmFrameBuffer* fb_;
    volatile std::uint8_t* base_ = nullptr;
    int w_, h_;
    std::uint32_t pitch_;
    std::vector<std::uint32_t> back_;
};

}  // namespace carsh::device

#endif
