#include "library/Art.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

#include "stb/stb_image.h"

namespace carsh::library {

const char* kDensityRamp = " .:-=+*#%@";  // 10 levels, dark -> bright

namespace {

// Box-filter px (w x h, `comp` bytes/pixel: 3 = RGB, 4 = RGBA) down to dw x dh
// RGBA. 3-component input gets opaque alpha.
Image downscalePixels(const uint8_t* px, int w, int h, int comp, int dw, int dh) {
    Image out;
    if (!px || w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return out;
    out.w = dw;
    out.h = dh;
    out.rgba.assign(size_t(dw) * dh * 4, 0);
    for (int y = 0; y < dh; ++y) {
        int sy0 = int(int64_t(y) * h / dh);
        int sy1 = int(int64_t(y + 1) * h / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dw; ++x) {
            int sx0 = int(int64_t(x) * w / dw);
            int sx1 = int(int64_t(x + 1) * w / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1 && sy < h; ++sy)
                for (int sx = sx0; sx < sx1 && sx < w; ++sx) {
                    const uint8_t* p = &px[(size_t(sy) * w + sx) * size_t(comp)];
                    r += p[0]; g += p[1]; b += p[2];
                    a += comp == 4 ? p[3] : 255u;
                    ++n;
                }
            if (!n) n = 1;
            uint8_t* d = &out.rgba[(size_t(y) * dw + x) * 4];
            d[0] = uint8_t(r / n); d[1] = uint8_t(g / n);
            d[2] = uint8_t(b / n); d[3] = uint8_t(a / n);
        }
    }
    return out;
}

// hal::IStream -> stb_image callback adapter. stb pulls 128 bytes at a time; on
// the device every IStream read is an f_read, and sub-sector reads cost a USB
// mass-storage command per 512-byte sector, so reads are batched through a
// 64 KiB window (one small heap bucket) rather than loading the whole file.
// Invariant: the underlying stream sits at bufStart_ + len_.
class StbStream {
public:
    static constexpr std::size_t kWindow = 64 * 1024;

    explicit StbStream(carsh::hal::IStream& s) : s_(s), buf_(kWindow) {
        s_.seek(0);
    }

    void rewind() { skipTo(0); }

    int read(char* dst, int n) {
        if (n <= 0) return 0;
        std::size_t want = std::size_t(n), done = 0;
        while (done < want) {
            if (off_ < len_) {
                const std::size_t k = std::min(len_ - off_, want - done);
                std::memcpy(dst + done, buf_.data() + off_, k);
                off_ += k;
                done += k;
                continue;
            }
            bufStart_ += len_;
            len_ = off_ = 0;
            if (want - done >= kWindow) {  // big read (a PNG IDAT): go direct
                const std::size_t got = s_.read(dst + done, want - done);
                bufStart_ += got;
                done += got;
                break;
            }
            len_ = s_.read(buf_.data(), kWindow);
            if (len_ == 0) break;
        }
        return int(done);
    }

    void skip(int n) {
        const int64_t target = int64_t(bufStart_ + off_) + n;
        skipTo(target < 0 ? 0 : uint64_t(target));
    }

    int eof() const { return off_ >= len_ && bufStart_ + len_ >= s_.size(); }

    static const stbi_io_callbacks kCallbacks;

private:
    void skipTo(uint64_t target) {
        if (target > s_.size()) target = s_.size();
        if (target >= bufStart_ && target <= bufStart_ + len_) {
            off_ = std::size_t(target - bufStart_);
            return;
        }
        s_.seek(target);
        bufStart_ = target;
        len_ = off_ = 0;
    }

    carsh::hal::IStream& s_;
    std::vector<uint8_t> buf_;
    uint64_t bufStart_ = 0;  // stream offset of buf_[0]
    std::size_t len_ = 0, off_ = 0;
};

const stbi_io_callbacks StbStream::kCallbacks = {
    [](void* u, char* data, int size) {
        return static_cast<StbStream*>(u)->read(data, size);
    },
    [](void* u, int n) { static_cast<StbStream*>(u)->skip(n); },
    [](void* u) { return static_cast<StbStream*>(u)->eof(); },
};

bool withinCap(int w, int h, int maxDim) {
    return w > 0 && h > 0 && w <= maxDim && h <= maxDim;
}

}  // namespace

bool decodeImage(const uint8_t* data, std::size_t len, Image& out, int maxDim) {
    if (!data || len == 0 || len > std::size_t(INT_MAX)) return false;
    int w = 0, h = 0, comp = 0;
    // Header-only probe: reject oversized images before stb allocates for them.
    if (!stbi_info_from_memory(data, int(len), &w, &h, &comp) ||
        !withinCap(w, h, maxDim))
        return false;
    stbi_uc* px = stbi_load_from_memory(data, int(len), &w, &h, &comp, 4);
    if (!px) return false;
    out.w = w;
    out.h = h;
    out.rgba.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
    return true;
}

bool decodeImageFitted(carsh::hal::IStream& s, Image& out, int workDim,
                       int maxDim) {
    if (workDim <= 0) return false;
    StbStream st(s);
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_callbacks(&StbStream::kCallbacks, &st, &w, &h, &comp) ||
        !withinCap(w, h, maxDim))
        return false;
    st.rewind();
    // RGB, not RGBA: a quarter less transient memory; nothing downstream reads
    // alpha (ASCII luma + tint).
    stbi_uc* px = stbi_load_from_callbacks(&StbStream::kCallbacks, &st, &w, &h,
                                           &comp, 3);
    if (!px) return false;
    if (!withinCap(w, h, maxDim)) {  // header lied between probe and decode
        stbi_image_free(px);
        return false;
    }
    int dw = w, dh = h;
    if (w > workDim || h > workDim) {
        if (w >= h) {
            dw = workDim;
            dh = std::max(1, int((int64_t(h) * workDim + w / 2) / w));
        } else {
            dh = workDim;
            dw = std::max(1, int((int64_t(w) * workDim + h / 2) / h));
        }
    }
    out = downscalePixels(px, w, h, 3, dw, dh);
    stbi_image_free(px);
    return out.valid();
}

Image downscale(const Image& src, int dw, int dh) {
    if (!src.valid()) return Image{};
    return downscalePixels(src.rgba.data(), src.w, src.h, 4, dw, dh);
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
