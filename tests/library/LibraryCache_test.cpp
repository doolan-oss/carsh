// LibraryCache — the boot-time fast path: a cheap media-tree signature + a
// save/load round-trip of the serialized DB, exercised over a real on-disk tree
// via HostVfs (same Vfs the device uses) including HostVfs::writeFile.
#include "library/LibraryCache.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "Test.h"
#include "fs/HostVfs.h"
#include "library/Database.h"

using namespace carsh;
namespace fs = std::filesystem;

namespace {
void touch(const fs::path& p, const char* bytes = "x") {
    std::ofstream f(p, std::ios::binary);
    f << bytes;
}
fs::path makeTree(const std::string& sub) {
    fs::path root = fs::temp_directory_path() / "carsh_libcache" / sub;
    fs::remove_all(root);
    fs::create_directories(root / "ArtistA" / "Album1");
    touch(root / "ArtistA" / "Album1" / "01.flac");
    touch(root / "ArtistA" / "Album1" / "02.mp3");
    touch(root / "ArtistA" / "Album1" / "cover.png");  // non-audio: ignored
    touch(root / "ArtistA" / "Album1" / "notes.txt");  // non-audio: ignored
    return root;
}

library::Database tinyDb() {
    library::Database db;
    library::TrackMeta m;
    m.title = "T"; m.artist = "A"; m.album = "Al"; m.durationMs = 1000;
    m.sampleRate = 44100; m.channels = 2; m.bitsPerSample = 16;
    db.addTrack(m, 1, "/A/Al/01.flac", 1, "/A/Al");
    db.buildIndexes();
    return db;
}
}  // namespace

TEST(LibraryCache, SignatureStableAndChangeSensitive) {
    fs::path root = makeTree("sig");
    host::HostVfs vfs(root.string());
    const uint64_t s1 = library::librarySignature(vfs, "/");
    CHECK_EQ(s1, library::librarySignature(vfs, "/"));  // stable

    // A non-audio file change does NOT move the signature.
    touch(root / "ArtistA" / "Album1" / "cover.png", "different bytes");
    CHECK_EQ(s1, library::librarySignature(vfs, "/"));

    // Adding an audio file DOES.
    touch(root / "ArtistA" / "Album1" / "03.wav");
    const uint64_t s2 = library::librarySignature(vfs, "/");
    CHECK(s2 != s1);

    // Removing it returns to the original signature.
    fs::remove(root / "ArtistA" / "Album1" / "03.wav");
    CHECK_EQ(s1, library::librarySignature(vfs, "/"));
}

TEST(LibraryCache, SaveThenLoadRoundTrips) {
    fs::path root = makeTree("rt");
    host::HostVfs vfs(root.string());
    const uint64_t sig = library::librarySignature(vfs, "/");

    library::Database db = tinyDb();
    REQUIRE(library::saveLibrary(vfs, "/library.db", "/library.sig", sig, db));

    // A fresh DB loads from the cache when the signature matches.
    library::Database loaded;
    REQUIRE(library::tryLoadLibrary(vfs, "/library.db", "/library.sig", sig, loaded));
    CHECK_EQ(loaded.trackCount(), std::size_t(1));
    CHECK_EQ(loaded.allTracks().size(), std::size_t(1));
}

TEST(LibraryCache, WrongSignatureMisses) {
    fs::path root = makeTree("miss");
    host::HostVfs vfs(root.string());
    library::Database db = tinyDb();
    REQUIRE(library::saveLibrary(vfs, "/library.db", "/library.sig", 111, db));

    library::Database loaded;
    CHECK(!library::tryLoadLibrary(vfs, "/library.db", "/library.sig", 222, loaded));
    CHECK_EQ(loaded.trackCount(), std::size_t(0));  // not populated on a miss
}

TEST(LibraryCache, MissingCacheMisses) {
    fs::path root = makeTree("none");
    host::HostVfs vfs(root.string());
    library::Database loaded;
    CHECK(!library::tryLoadLibrary(vfs, "/nope.db", "/nope.sig", 1, loaded));
}
