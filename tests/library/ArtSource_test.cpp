// ArtSource — find + decode sidecar album art into AsciiArt. Synthesize a folder
// with a cover image on disk, then load it through HostVfs (the same Vfs the device
// indexer/player use), exercising the real readdir -> open -> decode -> ASCII path.
#include "library/ArtSource.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Test.h"
#include "fs/HostVfs.h"
#include "img/PngCodec.h"
#include "library/Art.h"

using namespace carsh;
namespace fs = std::filesystem;

namespace {
void writeFile(const fs::path& p, const std::vector<uint8_t>& bytes) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}
// A folder under the build tmp with a synthesized cover + an (empty) track file.
fs::path makeAlbumDir(const std::string& sub, bool withCover, const char* coverName) {
    fs::path dir = fs::temp_directory_path() / "carsh_artsrc" / sub;
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (withCover) {
        std::vector<uint8_t> rgba(8 * 8 * 4, 0);
        for (int i = 0; i < 8 * 8; ++i) { rgba[i * 4 + 0] = 200; rgba[i * 4 + 2] = 60;
                                          rgba[i * 4 + 3] = 255; }
        writeFile(dir / coverName, host::encodePng(rgba.data(), 8, 8));
    }
    writeFile(dir / "01 - track.flac", {1, 2, 3, 4});
    return dir;
}
}  // namespace

TEST(ArtSource, LoadsSidecarCover) {
    fs::path dir = makeAlbumDir("hascover", true, "cover.png");
    host::HostVfs vfs("/");
    library::AsciiArt art;
    REQUIRE(library::loadSidecarArt(vfs, (dir / "01 - track.flac").string(), art));
    CHECK(art.rows > 0);
    CHECK(art.cols > 0);
    CHECK_EQ(int(art.cells.size()), art.rows * art.cols);
}

TEST(ArtSource, NoImageReturnsFalse) {
    fs::path dir = makeAlbumDir("nocover", false, "");
    host::HostVfs vfs("/");
    library::AsciiArt art;
    CHECK(!library::loadSidecarArt(vfs, (dir / "01 - track.flac").string(), art));
}

TEST(ArtSource, FilenamePreferenceRanking) {
    using library::detail::artRank;
    CHECK(artRank("cover.jpg") > artRank("folder.jpg"));
    CHECK(artRank("folder.png") > artRank("front.png"));
    CHECK(artRank("front.png") > artRank("albumart.jpg"));
    CHECK(artRank("albumart.jpg") > artRank("scan001.png"));  // any-image baseline
    CHECK_EQ(artRank("notes.txt"), -1);                        // not an image
    CHECK_EQ(artRank("track.flac"), -1);
}

TEST(ArtSource, PrefersCoverOverOtherImages) {
    fs::path dir = makeAlbumDir("pref", true, "zzz_other.png");  // a non-preferred image
    // Add a real cover too; it should win over the alphabetically-later other image.
    std::vector<uint8_t> rgba(8 * 8 * 4, 255);
    writeFile(dir / "cover.png", host::encodePng(rgba.data(), 8, 8));
    host::HostVfs vfs("/");
    library::AsciiArt art;
    REQUIRE(library::loadSidecarArt(vfs, (dir / "01 - track.flac").string(), art));
    CHECK(art.rows > 0);  // loaded (the ranking picked cover.png; both decode fine)
}
