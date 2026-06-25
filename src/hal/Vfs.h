// Vfs / IStream / Source — storage abstraction (PLAN.md §2.7).
//
// Decoders and tag parsers read through IStream (seekable bytes). The Vfs opens
// paths and walks directories. A Source pairs a Vfs with an id+type so the
// LibraryService can attribute tracks and the SourceManager can attach/detach.
//
// HW backend: Circle CFATFileSystem over USB/SD. Host backend: HostVfs over a
// real directory of media fixtures, so indexing/decoding run on the build server.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace carsh::hal {

// Seekable, read-only byte stream. Reads never block on the audio path beyond
// the underlying device; the deep PcmRing absorbs stalls (PLAN.md R5).
class IStream {
public:
    virtual ~IStream() = default;

    // Reads up to n bytes; returns bytes read (0 at EOF).
    virtual std::size_t read(void* buf, std::size_t n) = 0;

    // Absolute seek from the start. Returns false past EOF.
    virtual bool seek(std::uint64_t pos) = 0;

    virtual std::uint64_t tell() const = 0;
    virtual std::uint64_t size() const = 0;
    bool eof() const { return tell() >= size(); }
};

struct DirEntry {
    std::string name;          // basename only
    bool isDir = false;
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;   // seconds; used for cache-invalidation hashes
};

class Vfs {
public:
    virtual ~Vfs() = default;

    // Opens a file for reading. Returns nullptr if absent / not a file.
    virtual std::unique_ptr<IStream> open(const std::string& path) = 0;

    // Fills out with one entry for the path itself. Returns false if absent.
    virtual bool stat(const std::string& path, DirEntry& out) = 0;

    // Lists a directory (non-recursive). Returns false if not a directory.
    virtual bool readdir(const std::string& path,
                         std::vector<DirEntry>& out) = 0;
};

enum class SourceType : std::uint8_t { Usb = 0, Sd = 1, Bt = 2 };

// A mounted media source. id is stable for the lifetime of the mount and feeds
// the (sourceId, path) hash that forms a track's stable id (PLAN.md §4.1).
class Source {
public:
    virtual ~Source() = default;
    virtual std::uint8_t id() const = 0;
    virtual SourceType type() const = 0;
    virtual Vfs& vfs() = 0;
    // Root path within the source's Vfs to begin indexing from.
    virtual std::string root() const = 0;
};

}  // namespace carsh::hal
