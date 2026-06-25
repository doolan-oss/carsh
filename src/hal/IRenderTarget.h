// IRenderTarget — framebuffer abstraction for the UI toolkit (PLAN.md §2.2).
//
// HW backend: Circle CBcmFrameBuffer on the 7" DSI panel (800x480), double-
// buffered with vsync page-flip. Host backend: PngRenderTarget, which exposes an
// in-RAM surface and writes it to a .png on present(), so screen layout is
// verifiable by image diff under synthetic touch.
//
// Pixels are 32-bit 0xAARRGGBB, row-major, width*height. The toolkit owns all
// drawing; the target only provides the surface and the flip.
#pragma once

#include <cstdint>

namespace carsh::hal {

// 0xAARRGGBB packed color. Alpha is top byte (opaque = 0xFF).
inline std::uint32_t rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                          std::uint8_t a = 0xFF) {
    return (std::uint32_t(a) << 24) | (std::uint32_t(r) << 16) |
           (std::uint32_t(g) << 8) | std::uint32_t(b);
}

class IRenderTarget {
public:
    virtual ~IRenderTarget() = default;

    virtual int width() const = 0;
    virtual int height() const = 0;

    // Mutable, row-major surface of width*height ARGB pixels.
    virtual std::uint32_t* pixels() = 0;
    virtual const std::uint32_t* pixels() const = 0;

    // Publish the current surface (page-flip on HW; snapshot/save on host).
    virtual void present() = 0;
};

}  // namespace carsh::hal
