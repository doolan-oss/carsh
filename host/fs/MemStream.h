// MemStream — hal::IStream over an in-RAM byte buffer.
//
// Handy for tests that craft headers/fixtures, and for decoding/verifying byte
// blobs (e.g. OTA packages) without touching the filesystem.
#pragma once

#include <cstring>
#include <vector>

#include "hal/Vfs.h"

namespace carsh::host {

class MemStream : public carsh::hal::IStream {
public:
    explicit MemStream(std::vector<uint8_t> data) : data_(std::move(data)) {}
    MemStream(const void* p, std::size_t n)
        : data_(static_cast<const uint8_t*>(p),
                static_cast<const uint8_t*>(p) + n) {}

    std::size_t read(void* buf, std::size_t n) override {
        std::size_t avail = data_.size() - pos_;
        std::size_t toRead = n < avail ? n : avail;
        std::memcpy(buf, data_.data() + pos_, toRead);
        pos_ += toRead;
        return toRead;
    }
    bool seek(std::uint64_t pos) override {
        if (pos > data_.size()) { pos_ = data_.size(); return true; }
        pos_ = std::size_t(pos);
        return true;
    }
    std::uint64_t tell() const override { return pos_; }
    std::uint64_t size() const override { return data_.size(); }

private:
    std::vector<uint8_t> data_;
    std::size_t pos_ = 0;
};

}  // namespace carsh::host
