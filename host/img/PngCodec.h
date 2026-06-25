// Host PNG encode helper over stb_image_write (impl lives in the third_party TU).
// Used by tests to synthesize cover-art fixtures and by PngRenderTarget to dump
// UI frames.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "stb/stb_image_write.h"

namespace carsh::host {

inline void pngCollect(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    auto* p = static_cast<uint8_t*>(data);
    v->insert(v->end(), p, p + size);
}

inline std::vector<uint8_t> encodePng(const uint8_t* rgba, int w, int h) {
    std::vector<uint8_t> out;
    stbi_write_png_to_func(pngCollect, &out, w, h, 4, rgba, w * 4);
    return out;
}

inline bool writePng(const std::string& path, const uint8_t* rgba, int w, int h) {
    return stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4) != 0;
}

}  // namespace carsh::host
