#include "library/Art.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "stb/stb_image.h"

namespace carsh::library {

const char* kDensityRamp = " .:-=+*#%@";  // 10 levels, dark -> bright

bool decodeImage(const uint8_t* data, std::size_t len, Image& out) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(data, int(len), &w, &h, &comp, 4);
    if (!px) return false;
    out.w = w;
    out.h = h;
    out.rgba.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
    return true;
}

Image downscale(const Image& src, int dw, int dh) {
    Image out;
    if (!src.valid() || dw <= 0 || dh <= 0) return out;
    out.w = dw;
    out.h = dh;
    out.rgba.assign(size_t(dw) * dh * 4, 0);
    for (int y = 0; y < dh; ++y) {
        int sy0 = y * src.h / dh;
        int sy1 = (y + 1) * src.h / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dw; ++x) {
            int sx0 = x * src.w / dw;
            int sx1 = (x + 1) * src.w / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1 && sy < src.h; ++sy)
                for (int sx = sx0; sx < sx1 && sx < src.w; ++sx) {
                    const uint8_t* p = &src.rgba[(size_t(sy) * src.w + sx) * 4];
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; ++n;
                }
            if (!n) n = 1;
            uint8_t* d = &out.rgba[(size_t(y) * dw + x) * 4];
            d[0] = uint8_t(r / n); d[1] = uint8_t(g / n);
            d[2] = uint8_t(b / n); d[3] = uint8_t(a / n);
        }
    }
    return out;
}

uint32_t dominantColor(const Image& img) {
    if (!img.valid()) return 0xFF000000u;
    // Saturation-weighted average so a vivid accent wins over a gray field.
    double sr = 0, sg = 0, sb = 0, sw = 0;
    for (size_t i = 0; i < size_t(img.w) * img.h; ++i) {
        const uint8_t* p = &img.rgba[i * 4];
        int mx = std::max({p[0], p[1], p[2]});
        int mn = std::min({p[0], p[1], p[2]});
        double sat = mx > 0 ? double(mx - mn) / mx : 0.0;
        double w = 0.15 + sat;  // floor so flat images still yield a color
        sr += p[0] * w; sg += p[1] * w; sb += p[2] * w; sw += w;
    }
    if (sw <= 0) return 0xFF000000u;
    uint8_t r = uint8_t(sr / sw), g = uint8_t(sg / sw), b = uint8_t(sb / sw);
    return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

AsciiArt generateAscii(const Image& img, int cols, int rows) {
    AsciiArt out;
    out.cols = cols;
    out.rows = rows;
    out.cells.assign(size_t(cols) * rows, ' ');
    if (!img.valid()) return out;

    Image small = downscale(img, cols, rows);
    const int ramp = int(std::strlen(kDensityRamp));
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) {
            const uint8_t* p = &small.rgba[(size_t(y) * cols + x) * 4];
            // Rec.601 luma.
            double lum = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
            int idx = int(lum * (ramp - 1) / 255.0 + 0.5);
            if (idx < 0) idx = 0;
            if (idx >= ramp) idx = ramp - 1;
            out.cells[size_t(y) * cols + x] = kDensityRamp[idx];
        }
    uint32_t tint = dominantColor(img);
    out.tintR = uint8_t((tint >> 16) & 0xFF);
    out.tintG = uint8_t((tint >> 8) & 0xFF);
    out.tintB = uint8_t(tint & 0xFF);
    return out;
}

std::vector<uint8_t> AsciiArt::serialize() const {
    std::vector<uint8_t> v = {'A', 'S', 'C', '1'};
    v.push_back(uint8_t(rows)); v.push_back(uint8_t(rows >> 8));
    v.push_back(uint8_t(cols)); v.push_back(uint8_t(cols >> 8));
    v.push_back(tintR); v.push_back(tintG); v.push_back(tintB);
    v.insert(v.end(), cells.begin(), cells.end());
    return v;
}

bool AsciiArt::deserialize(const uint8_t* d, std::size_t len, AsciiArt& out) {
    if (len < 11 || std::memcmp(d, "ASC1", 4) != 0) return false;
    out.rows = d[4] | (d[5] << 8);
    out.cols = d[6] | (d[7] << 8);
    out.tintR = d[8]; out.tintG = d[9]; out.tintB = d[10];
    size_t need = size_t(out.rows) * out.cols;
    if (len < 11 + need) return false;
    out.cells.assign(reinterpret_cast<const char*>(d + 11), need);
    return true;
}

}  // namespace carsh::library
