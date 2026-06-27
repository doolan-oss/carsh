// H.264 decode + YUV->RGB tests (AA_PLAN rung 5). The I420->RGB888 conversion is
// host-tested unconditionally with hand-computed anchors. When the build server has
// openh264 (the same library vendored for the device), the real decode backend
// (device/CircleH264Decoder) is compiled in and exercised end to end: a synthetic
// clip is encoded to Annex-B, fed access-unit by access-unit through the decoder,
// and each decoded RGB frame is compared back to the source (mean abs error), so a
// genuine decode — not just plumbing — is proven on the build server. On-target FPS
// stays [hw].
#include "Test.h"
#include "aa/Yuv420.h"

using carsh::aa::i420ToRgb888;

// ---- Pure converter (no codec required) -------------------------------------
TEST(Yuv420, KnownAnchors) {
    // Solid 2x2 blocks so chroma indexing is trivial.
    auto convert1 = [](uint8_t Y, uint8_t U, uint8_t V, uint8_t& r, uint8_t& g, uint8_t& b) {
        uint8_t y[4] = {Y, Y, Y, Y};
        uint8_t u[1] = {U};
        uint8_t v[1] = {V};
        uint8_t out[2 * 2 * 3];
        i420ToRgb888(y, 2, u, 1, v, 1, 2, 2, out);
        r = out[0];
        g = out[1];
        b = out[2];
    };
    uint8_t r, g, b;
    // BT.601 black: Y=16, neutral chroma -> (0,0,0).
    convert1(16, 128, 128, r, g, b);
    CHECK(r == 0 && g == 0 && b == 0);
    // White: Y=235 -> (255,255,255).
    convert1(235, 128, 128, r, g, b);
    CHECK(r == 255 && g == 255 && b == 255);
    // Mid-grey: Y=128, neutral chroma -> (298*112+128)>>8 = 130 on all channels.
    convert1(128, 128, 128, r, g, b);
    CHECK(r == 130 && g == 130 && b == 130);
    // Strong red chroma (V high) pushes R up and B down from grey.
    convert1(128, 128, 240, r, g, b);
    CHECK(r > 200);
    CHECK(b < 140);
}

#ifdef CARSH_HAVE_OPENH264
#include <wels/codec_api.h>
#include <wels/codec_app_def.h>

#include <cstring>
#include <vector>

#include "CircleH264Decoder.h"

using carsh::aa::DecodedFrame;
using carsh::aa::IH264FrameSink;
using carsh::device::CircleH264Decoder;

namespace {

constexpr int kW = 320;
constexpr int kH = 240;

// One synthetic I420 frame: a diagonal luma gradient that scrolls with `k` plus a
// moving bright box (so frames differ -> real inter-frame coding), mild constant
// chroma. Layout: Y (kW*kH) then U then V (each kW/2*kH/2).
std::vector<uint8_t> makeFrame(int k) {
    std::vector<uint8_t> f(static_cast<size_t>(kW) * kH * 3 / 2);
    uint8_t* y = f.data();
    uint8_t* u = y + static_cast<size_t>(kW) * kH;
    uint8_t* v = u + static_cast<size_t>(kW / 2) * (kH / 2);
    for (int j = 0; j < kH; ++j)
        for (int i = 0; i < kW; ++i)
            y[j * kW + i] = static_cast<uint8_t>((i + j + k * 4) & 0xFF);
    const int bx = 20 + k * 10, by = 30 + k * 6;
    for (int j = by; j < by + 40 && j < kH; ++j)
        for (int i = bx; i < bx + 40 && i < kW; ++i) y[j * kW + i] = 235;
    for (int c = 0; c < (kW / 2) * (kH / 2); ++c) {
        u[c] = 110;
        v[c] = 150;
    }
    return f;
}

// Encode `n` synthetic frames to per-frame Annex-B access units (openh264 emits
// start-code framing). Returns one byte-vector per non-skipped frame.
std::vector<std::vector<uint8_t>> encodeClip(int n) {
    ISVCEncoder* enc = nullptr;
    REQUIRE(WelsCreateSVCEncoder(&enc) == 0);
    REQUIRE(enc != nullptr);
    SEncParamBase p;
    std::memset(&p, 0, sizeof(p));
    p.iUsageType = CAMERA_VIDEO_REAL_TIME;
    p.iPicWidth = kW;
    p.iPicHeight = kH;
    p.iTargetBitrate = 5000000;
    p.iRCMode = RC_OFF_MODE;  // constant-QP -> deterministic, high quality
    p.fMaxFrameRate = 30.0f;
    REQUIRE(enc->Initialize(&p) == 0);

    std::vector<std::vector<uint8_t>> units;
    for (int k = 0; k < n; ++k) {
        std::vector<uint8_t> src = makeFrame(k);
        SSourcePicture pic;
        std::memset(&pic, 0, sizeof(pic));
        pic.iColorFormat = videoFormatI420;
        pic.iPicWidth = kW;
        pic.iPicHeight = kH;
        pic.iStride[0] = kW;
        pic.iStride[1] = kW / 2;
        pic.iStride[2] = kW / 2;
        pic.pData[0] = src.data();
        pic.pData[1] = src.data() + static_cast<size_t>(kW) * kH;
        pic.pData[2] = pic.pData[1] + static_cast<size_t>(kW / 2) * (kH / 2);
        pic.uiTimeStamp = k * 33;

        SFrameBSInfo info;
        std::memset(&info, 0, sizeof(info));
        REQUIRE(enc->EncodeFrame(&pic, &info) == 0);
        if (info.eFrameType == videoFrameTypeSkip ||
            info.eFrameType == videoFrameTypeInvalid)
            continue;

        std::vector<uint8_t> au;
        for (int l = 0; l < info.iLayerNum; ++l) {
            const SLayerBSInfo& layer = info.sLayerInfo[l];
            int layerLen = 0;
            for (int nidx = 0; nidx < layer.iNalCount; ++nidx)
                layerLen += layer.pNalLengthInByte[nidx];
            au.insert(au.end(), layer.pBsBuf, layer.pBsBuf + layerLen);
        }
        units.push_back(std::move(au));
    }
    enc->Uninitialize();
    WelsDestroySVCEncoder(enc);
    return units;
}

struct CollectSink : IH264FrameSink {
    std::vector<std::vector<uint8_t>> frames;
    int w = 0, h = 0;
    void onFrame(const DecodedFrame& f) override {
        w = f.width;
        h = f.height;
        frames.emplace_back(f.pixels, f.pixels + f.pixelsLen);
    }
};

// Mean absolute error between two equal-length byte buffers.
double mae(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    long sum = 0;
    for (size_t i = 0; i < a.size(); ++i)
        sum += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
    return static_cast<double>(sum) / static_cast<double>(a.size());
}

}  // namespace

TEST(CircleH264Decoder, DecodesEncodedClipAndConverts) {
    const int n = 8;
    std::vector<std::vector<uint8_t>> units = encodeClip(n);
    REQUIRE(static_cast<int>(units.size()) == n);

    CollectSink sink;
    CircleH264Decoder dec(&sink);
    REQUIRE(dec.init());
    for (const auto& au : units) dec.feed(au.data(), au.size());

    // No-delay decode: one picture out per access unit, at the source resolution.
    CHECK_EQ(static_cast<int>(sink.frames.size()), n);
    CHECK_EQ(sink.w, kW);
    CHECK_EQ(sink.h, kH);
    CHECK_EQ(static_cast<int>(dec.framesDecoded()), n);

    // Each decoded RGB frame must reconstruct its source (low MAE proves a genuine
    // decode; garbage would be ~80+). Compare through the same converter.
    for (int k = 0; k < n; ++k) {
        std::vector<uint8_t> src = makeFrame(k);
        std::vector<uint8_t> srcRgb(static_cast<size_t>(kW) * kH * 3);
        i420ToRgb888(src.data(), kW, src.data() + static_cast<size_t>(kW) * kH, kW / 2,
                     src.data() + static_cast<size_t>(kW) * kH +
                         static_cast<size_t>(kW / 2) * (kH / 2),
                     kW / 2, kW, kH, srcRgb.data());
        CHECK(mae(sink.frames[k], srcRgb) < 25.0);
    }
}

TEST(CircleH264Decoder, ParameterSetUnitsProduceNoFrame) {
    CollectSink sink;
    CircleH264Decoder dec(&sink);
    REQUIRE(dec.init());
    // A bare SPS NAL (start code + nal_unit_type 7) is not a full access unit -> no
    // picture, no crash.
    const uint8_t sps[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x0a};
    dec.feed(sps, sizeof(sps));
    CHECK_EQ(static_cast<int>(sink.frames.size()), 0);
}
#endif  // CARSH_HAVE_OPENH264
