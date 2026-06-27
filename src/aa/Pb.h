// Pb — the tiny protobuf (proto2 wire) codec the Android Auto messages use.
// Ported from the dylant9/head_unit reference (proto/aa_proto). Only the subset
// AA needs: varint (wire 0), length-delimited (wire 2: string/bytes/submessage),
// with skip support for 32/64-bit fixed fields. Platform-independent, header-only,
// host-tested (tests/aa/Pb_test).
//
// Wire format: tag = (field_number << 3) | wire_type.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace carsh::aa::pb {

// Encoder. Appends to a caller-owned growable buffer, so submessages are encoded
// into a temporary and written length-prefixed (no reserve/shift games).
class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : buf_(out) {}

    void varint(uint64_t value) {
        do {
            uint8_t byte = static_cast<uint8_t>(value & 0x7F);
            value >>= 7;
            if (value) byte = static_cast<uint8_t>(byte | 0x80);
            buf_.push_back(byte);
        } while (value);
    }

    void tag(uint8_t wireType, uint32_t field) {
        varint((static_cast<uint64_t>(field) << 3) | wireType);
    }

    void uint32(uint32_t field, uint32_t value) {
        tag(0, field);
        varint(value);
    }
    void int32(uint32_t field, int32_t value) {
        tag(0, field);
        // proto2 int32 is varint; negatives sign-extend to 10 bytes.
        varint(static_cast<uint64_t>(static_cast<int64_t>(value)));
    }
    void boolean(uint32_t field, bool value) { uint32(field, value ? 1u : 0u); }

    void string(uint32_t field, const std::string& s) {
        bytes(field, reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    void bytes(uint32_t field, const uint8_t* data, size_t len) {
        tag(2, field);
        varint(len);
        buf_.insert(buf_.end(), data, data + len);
    }

    // Encode a length-delimited submessage: the body writes into a fresh Writer,
    // then the result is emitted length-prefixed under `field`.
    void submessage(uint32_t field, const std::function<void(Writer&)>& body) {
        std::vector<uint8_t> tmp;
        Writer w(tmp);
        body(w);
        bytes(field, tmp.data(), tmp.size());
    }

private:
    std::vector<uint8_t>& buf_;
};

// Decoder over a borrowed buffer.
class Reader {
public:
    Reader(const uint8_t* buf, size_t size) : buf_(buf), size_(size) {}
    explicit Reader(const std::vector<uint8_t>& v) : buf_(v.data()), size_(v.size()) {}

    bool eof() const { return pos_ >= size_; }

    bool varint(uint64_t& value) {
        value = 0;
        int shift = 0;
        for (int i = 0; i < 10; ++i) {
            if (pos_ >= size_) return false;
            uint8_t byte = buf_[pos_++];
            value |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if (!(byte & 0x80)) return true;
            shift += 7;
        }
        return false;  // varint too long
    }

    bool tag(uint32_t& field, uint8_t& wireType) {
        if (pos_ >= size_) return false;
        uint64_t t;
        if (!varint(t)) return false;
        wireType = static_cast<uint8_t>(t & 0x07);
        field = static_cast<uint32_t>(t >> 3);
        return true;
    }

    bool skip(uint8_t wireType) {
        switch (wireType) {
            case 0: {
                uint64_t tmp;
                return varint(tmp);
            }
            case 1:
                return advance(8);
            case 2: {
                uint64_t len;
                if (!varint(len)) return false;
                return advance(len);
            }
            case 5:
                return advance(4);
            default:
                return false;
        }
    }

    bool u32(uint32_t& value) {
        uint64_t v;
        if (!varint(v)) return false;
        if (v > UINT32_MAX) return false;
        value = static_cast<uint32_t>(v);
        return true;
    }

    bool str(std::string& out) {
        std::vector<uint8_t> b;
        if (!bytes(b)) return false;
        out.assign(b.begin(), b.end());
        return true;
    }

    bool bytes(std::vector<uint8_t>& out) {
        uint64_t len;
        if (!varint(len)) return false;
        if (len > remaining()) return false;
        out.assign(buf_ + pos_, buf_ + pos_ + static_cast<size_t>(len));
        pos_ += static_cast<size_t>(len);
        return true;
    }

    // Borrow a length-delimited field as a nested reader.
    bool submessage(Reader& sub) {
        uint64_t len;
        if (!varint(len)) return false;
        if (len > remaining()) return false;
        sub = Reader(buf_ + pos_, static_cast<size_t>(len));
        pos_ += static_cast<size_t>(len);
        return true;
    }

private:
    uint64_t remaining() const { return size_ - pos_; }
    bool advance(uint64_t n) {
        if (n > remaining()) return false;
        pos_ += static_cast<size_t>(n);
        return true;
    }

    const uint8_t* buf_;
    size_t size_;
    size_t pos_ = 0;
};

}  // namespace carsh::aa::pb
