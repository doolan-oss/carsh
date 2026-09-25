// HeapProbe — test-only instrumentation for the "no big transient buffers" checks
// (review L1: Circle's stock heap loses every freed block > 512 KiB, so the tag
// parsers and cover-art path must not allocate in proportion to the file).
//
// AllocScope records every global operator-new allocation made while it is alive
// (HeapProbe.cpp replaces operator new/delete for the whole test binary; outside
// a scope they are plain malloc/free). It sees std::vector/std::string storage —
// exactly the buffers the old code sized from tag/file lengths — but not stb's
// own malloc() calls, which the dimension cap bounds instead.
//
// CountingStream wraps an IStream and records how many bytes were read and the
// largest single read, so a test can prove a parser seeked past a cover instead
// of reading it. CountingVfs hands out CountingStreams that all report into one
// shared IoStats (streams opened inside the code under test die before the check).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hal/Vfs.h"

namespace carsh::testprobe {

struct AllocStats {
    std::size_t count = 0;       // allocations made in the scope
    std::size_t maxBytes = 0;    // largest single allocation
    std::size_t totalBytes = 0;  // sum of all allocation sizes
};

// One scope at a time (not reentrant); tests are single-threaded.
class AllocScope {
public:
    AllocScope();
    ~AllocScope();
    AllocScope(const AllocScope&) = delete;
    AllocScope& operator=(const AllocScope&) = delete;
    AllocStats stats() const;
};

struct IoStats {
    std::uint64_t bytesRead = 0;
    std::size_t maxRead = 0;  // largest single read() request
    std::size_t reads = 0;
    std::size_t seeks = 0;
};

class CountingStream : public carsh::hal::IStream {
public:
    CountingStream(std::unique_ptr<carsh::hal::IStream> inner, IoStats& stats)
        : inner_(std::move(inner)), stats_(stats) {}

    std::size_t read(void* buf, std::size_t n) override {
        ++stats_.reads;
        if (n > stats_.maxRead) stats_.maxRead = n;
        const std::size_t got = inner_->read(buf, n);
        stats_.bytesRead += got;
        return got;
    }
    bool seek(std::uint64_t pos) override {
        ++stats_.seeks;
        return inner_->seek(pos);
    }
    std::uint64_t tell() const override { return inner_->tell(); }
    std::uint64_t size() const override { return inner_->size(); }

private:
    std::unique_ptr<carsh::hal::IStream> inner_;
    IoStats& stats_;
};

class CountingVfs : public carsh::hal::Vfs {
public:
    CountingVfs(carsh::hal::Vfs& inner, IoStats& stats)
        : inner_(inner), stats_(stats) {}

    std::unique_ptr<carsh::hal::IStream> open(const std::string& path) override {
        auto s = inner_.open(path);
        if (!s) return nullptr;
        return std::make_unique<CountingStream>(std::move(s), stats_);
    }
    bool stat(const std::string& path, carsh::hal::DirEntry& out) override {
        return inner_.stat(path, out);
    }
    bool readdir(const std::string& path,
                 std::vector<carsh::hal::DirEntry>& out) override {
        return inner_.readdir(path, out);
    }

private:
    carsh::hal::Vfs& inner_;
    IoStats& stats_;
};

}  // namespace carsh::testprobe
