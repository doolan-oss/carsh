// Cover-art memory tests (review L1). The sidecar path must stream the cover file
// (no whole-file buffer), downscale straight out of the decoder (no full-size RGBA
// copy), and reject covers above the dimension cap from the header alone — then
// fall back to the next image in the folder. Allocation sizes are checked with
// HeapProbe's AllocScope, reads with a CountingVfs over the real HostVfs.
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <fstream>
#include <string>
#include <vector>

#include "HeapProbe.h"
#include "Test.h"
#include "fs/HostVfs.h"
#include "fs/MemStream.h"
#include "img/PngCodec.h"
#include "library/Art.h"
#include "library/ArtSource.h"
#include "stb/stb_image_write.h"

using namespace carsh;
using namespace carsh::library;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> solid(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        px[i * 4 + 0] = r; px[i * 4 + 1] = g; px[i * 4 + 2] = b; px[i * 4 + 3] = 255;
    }
    return px;
}

// Busy RGB pattern so the encoded file is large and doesn't compress away.
std::vector<uint8_t> busy(int w, int h) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    uint32_t x = 12345;
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        x = x * 1103515245u + 12345u;
        px[i * 4 + 0] = uint8_t(x >> 16);
        px[i * 4 + 1] = uint8_t((i % size_t(w)) * 255 / size_t(w));
        px[i * 4 + 2] = uint8_t(x >> 24);
        px[i * 4 + 3] = 255;
    }
    return px;
}

std::vector<uint8_t> encodeJpg(const std::vector<uint8_t>& rgba, int w, int h) {
    std::vector<uint8_t> out;
    stbi_write_jpg_to_func(host::pngCollect, &out, w, h, 4, rgba.data(), 92);
    return out;
}

void writeFile(const fs::path& p, const std::vector<uint8_t>& bytes) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

fs::path freshDir(const std::string& sub) {
    fs::path dir = fs::temp_directory_path() / "carsh_artstream" / sub;
    fs::remove_all(dir);
    fs::create_directories(dir);
    writeFile(dir / "01 - track.flac", {1, 2, 3, 4});
    return dir;
}

}  // namespace

TEST(ArtStream, FittedDecodeDownscalesKeepingAspect) {
    auto png = host::encodePng(solid(1000, 500, 200, 40, 90).data(), 1000, 500);
    host::MemStream s(png);
    Image img;
    REQUIRE(decodeImageFitted(s, img));
    CHECK_EQ(img.w, kArtWorkDim);
    CHECK_EQ(img.h, kArtWorkDim / 2);
    REQUIRE(img.valid());
    const uint8_t* p = &img.rgba[(size_t(10) * img.w + 20) * 4];
    CHECK_EQ(int(p[0]), 200);
    CHECK_EQ(int(p[1]), 40);
    CHECK_EQ(int(p[2]), 90);
    CHECK_EQ(int(p[3]), 255);

    // Portrait.
    auto tall = host::encodePng(solid(300, 900, 1, 2, 3).data(), 300, 900);
    host::MemStream st(tall);
    REQUIRE(decodeImageFitted(st, img));
    CHECK_EQ(img.h, kArtWorkDim);
    CHECK_EQ(img.w, 85);  // 300 * 256 / 900 = 85.3
}

// Images already within the working size are kept pixel-for-pixel (same RGB as
// the full decode; alpha is always opaque).
TEST(ArtStream, FittedDecodeKeepsSmallImages) {
    auto rgba = busy(24, 10);
    auto png = host::encodePng(rgba.data(), 24, 10);
    Image full, fitted;
    REQUIRE(decodeImage(png.data(), png.size(), full));
    host::MemStream s(png);
    REQUIRE(decodeImageFitted(s, fitted));
    REQUIRE(fitted.w == 24 && fitted.h == 10);
    bool same = true;
    for (size_t i = 0; i < size_t(24) * 10; ++i)
        for (int c = 0; c < 3; ++c)
            if (full.rgba[i * 4 + size_t(c)] != fitted.rgba[i * 4 + size_t(c)]) same = false;
    CHECK(same);
}

TEST(ArtStream, DimensionCapRejectsBeforeDecoding) {
    // 3001 px wide (a thin strip keeps the fixture tiny) is over the default cap.
    auto wide = host::encodePng(solid(kMaxArtDim + 1, 2, 9, 9, 9).data(), kMaxArtDim + 1, 2);
    Image img;
    CHECK(!decodeImage(wide.data(), wide.size(), img));
    host::MemStream s(wide);
    CHECK(!decodeImageFitted(s, img));

    // Exactly at the cap is fine.
    auto edge = host::encodePng(solid(kMaxArtDim, 2, 9, 9, 9).data(), kMaxArtDim, 2);
    host::MemStream se(edge);
    CHECK(decodeImageFitted(se, img));

    // Caller-supplied caps.
    auto png = host::encodePng(solid(32, 8, 1, 1, 1).data(), 32, 8);
    CHECK(!decodeImage(png.data(), png.size(), img, 16));
    host::MemStream s2(png);
    CHECK(!decodeImageFitted(s2, img, kArtWorkDim, 16));

    // Not an image at all.
    std::vector<uint8_t> junk(300, 0x42);
    host::MemStream sj(junk);
    CHECK(!decodeImageFitted(sj, img));
}

// The old path read the whole cover into a vector, decoded it, and copied the
// full-size RGBA into Image (~4 MiB here, ~10 MiB for a 1000x1000 cover — all lost
// on Circle's stock heap). Now: no allocation above the 256x256 working image, and
// the file is read through the 64 KiB window, never in one piece.
TEST(ArtStream, SidecarJpegHasNoFullSizeBuffers) {
    fs::path dir = freshDir("bigjpg");
    const int n = 1024;
    auto jpg = encodeJpg(busy(n, n), n, n);
    REQUIRE(jpg.size() > 128 * 1024);  // bigger than the read window
    writeFile(dir / "cover.jpg", jpg);

    host::HostVfs hostVfs("/");
    testprobe::IoStats io;
    testprobe::CountingVfs vfs(hostVfs, io);
    AsciiArt art;
    testprobe::AllocStats alloc;
    bool ok = false;
    {
        testprobe::AllocScope scope;
        ok = loadSidecarArt(vfs, (dir / "01 - track.flac").string(), art);
        alloc = scope.stats();
    }
    REQUIRE(ok);
    CHECK_EQ(art.rows, 11);
    CHECK_EQ(art.cols, 22);
    CHECK(alloc.maxBytes <= size_t(kArtWorkDim) * kArtWorkDim * 4);
    CHECK(alloc.maxBytes < jpg.size());               // no whole-file buffer
    CHECK(io.maxRead <= 64 * 1024);                   // windowed reads
    CHECK(io.bytesRead >= jpg.size());                // ...of the whole file
    CHECK(io.bytesRead < jpg.size() + 256 * 1024);    // probe + decode, not re-reads
}

// Camera/scanner JPEGs carry big APPn segments (EXIF, ICC, thumbnails) that stb
// skips while looking for the frame header — beyond the 64 KiB read window, so
// the probe's skip becomes a seek and the rewind for the real decode re-seeks.
TEST(ArtStream, JpegWithLargeAppSegmentsDecodes) {
    auto jpg = encodeJpg(busy(64, 48), 64, 48);
    REQUIRE(jpg.size() > 2 && jpg[0] == 0xFF && jpg[1] == 0xD8);
    std::vector<uint8_t> big(jpg.begin(), jpg.begin() + 2);  // SOI
    for (int seg = 0; seg < 3; ++seg) {  // 3 x ~64 KiB APP1 segments
        const uint16_t len = 0xFFFF;     // includes the two length bytes
        big.push_back(0xFF); big.push_back(0xE1);
        big.push_back(uint8_t(len >> 8)); big.push_back(uint8_t(len & 0xFF));
        for (int i = 0; i < len - 2; ++i) big.push_back(uint8_t(i * 7));
    }
    big.insert(big.end(), jpg.begin() + 2, jpg.end());

    Image direct;
    REQUIRE(decodeImage(jpg.data(), jpg.size(), direct));
    testprobe::IoStats io;
    testprobe::CountingStream s(std::make_unique<host::MemStream>(big), io);
    Image fitted;
    REQUIRE(decodeImageFitted(s, fitted));
    CHECK_EQ(fitted.w, 64);
    CHECK_EQ(fitted.h, 48);
    CHECK(io.seeks >= 2);  // skipped the segments without reading them all
    // Same pixels as decoding the plain JPEG from memory.
    int maxDiff = 0;
    for (size_t i = 0; i < size_t(64) * 48 * 4; i += 4)
        for (int c = 0; c < 3; ++c) {
            const int d = std::abs(int(direct.rgba[i + size_t(c)]) - int(fitted.rgba[i + size_t(c)]));
            if (d > maxDiff) maxDiff = d;
        }
    CHECK_EQ(maxDiff, 0);
}

// PNG goes through the same streamed path (its IDAT is read straight into stb's
// buffer, so only the pixel-copy bound is checked).
TEST(ArtStream, SidecarPngHasNoFullSizeCopy) {
    fs::path dir = freshDir("bigpng");
    const int n = 768;
    writeFile(dir / "folder.png", host::encodePng(busy(n, n).data(), n, n));
    host::HostVfs vfs("/");
    AsciiArt art;
    testprobe::AllocStats alloc;
    bool ok = false;
    {
        testprobe::AllocScope scope;
        ok = loadSidecarArt(vfs, (dir / "01 - track.flac").string(), art);
        alloc = scope.stats();
    }
    REQUIRE(ok);
    CHECK(alloc.maxBytes <= size_t(kArtWorkDim) * kArtWorkDim * 4);
}

// An oversized cover.png (a raw scan) is rejected from its header and the
// next-ranked image (folder.png) is used instead.
TEST(ArtStream, OversizedCoverFallsBackToNextCandidate) {
    fs::path dir = freshDir("fallback");
    writeFile(dir / "cover.png",
              host::encodePng(solid(kMaxArtDim + 1, 4, 0, 0, 255).data(), kMaxArtDim + 1, 4));
    writeFile(dir / "folder.png", host::encodePng(solid(16, 16, 230, 20, 20).data(), 16, 16));
    host::HostVfs vfs("/");
    AsciiArt art;
    REQUIRE(loadSidecarArt(vfs, (dir / "01 - track.flac").string(), art));
    CHECK(art.tintR > art.tintB + 100);  // red folder.png, not the blue scan
}

TEST(ArtStream, OnlyOversizedOrBrokenImagesMeansNoArt) {
    fs::path dir = freshDir("noneusable");
    writeFile(dir / "cover.png",
              host::encodePng(solid(kMaxArtDim + 1, 4, 0, 0, 255).data(), kMaxArtDim + 1, 4));
    writeFile(dir / "front.jpg", std::vector<uint8_t>(500, 0x13));  // not a JPEG
    writeFile(dir / "empty.png", {});
    host::HostVfs vfs("/");
    AsciiArt art;
    CHECK(!loadSidecarArt(vfs, (dir / "01 - track.flac").string(), art));

    // A caller cap applies to every candidate.
    fs::path dir2 = freshDir("capped");
    writeFile(dir2 / "cover.png", host::encodePng(solid(64, 64, 5, 5, 5).data(), 64, 64));
    CHECK(!loadSidecarArt(vfs, (dir2 / "01 - track.flac").string(), art, 22, 11, 32));
    CHECK(loadSidecarArt(vfs, (dir2 / "01 - track.flac").string(), art, 22, 11, 64));
}
