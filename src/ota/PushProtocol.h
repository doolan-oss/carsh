// PushProtocol — framed one-way transfer of a .carup package from the dev host
// into the running kernel over the USB-C CDC serial console (PLAN.md §8.5; the
// "serial push" leg of the OTA dev loop: push -> verify -> stage inactive slot ->
// reboot -> chainloader trial).
//
// The CDC line already carries device->host log text; host->device was unused
// (CircleUsbLog drained and discarded it). This protocol owns that direction.
// Device->host feedback stays out of band: the kernel logs "ota-push: ..." lines
// and the host script greps them from the same serial stream it already reads.
//
// Framing (all little-endian):
//   packet := "CPSH" | type u8 | seq u16 | len u16 | payload[len] | crc32 u32
//   crc32 is over type..payload (not the magic), IEEE reflected (zlib-compatible).
//   BEGIN (type 1, seq 0):  payload = totalSize u32 | blobCrc32 u32
//   DATA  (type 2, seq 1+): payload = the next chunk of the blob (<= 4096 bytes)
//   END   (type 3):         payload empty; seq = last DATA seq + 1
//
// There are no per-chunk acks: USB CDC is reliable transport; the per-packet CRC
// guards against software/framing bugs, and the whole blob is re-verified twice
// after transfer (blobCrc32 here, then Ed25519 + image SHA-256 by OtaManager).
// A failed/aborted transfer is recovered by the host simply resending: a fresh
// BEGIN unconditionally restarts reception, and the parser resynchronizes on the
// magic after dropping garbage (counted in resyncSkips()).
//
// PushReceiver is platform-independent and host-tested; the device feeds it raw
// CDC bytes from the CircleUsbLog task and the kernel main loop polls state().
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace carsh::ota {

// Running CRC-32 (IEEE reflected, poly 0xEDB88320), nibble-table so it's small
// enough to keep header-only. crc32Update(crc32Init(), ...) chains across calls;
// finalize with crc32Final.
inline uint32_t crc32Init() { return 0xFFFFFFFFu; }
inline uint32_t crc32Update(uint32_t crc, const uint8_t* d, std::size_t n) {
    static const uint32_t kNib[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        crc = (crc >> 4) ^ kNib[crc & 0x0F];
        crc = (crc >> 4) ^ kNib[crc & 0x0F];
    }
    return crc;
}
inline uint32_t crc32Final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }
inline uint32_t crc32Of(const uint8_t* d, std::size_t n) {
    return crc32Final(crc32Update(crc32Init(), d, n));
}

namespace push {

inline constexpr uint8_t kMagic[4] = {'C', 'P', 'S', 'H'};
inline constexpr uint8_t kTypeBegin = 1;
inline constexpr uint8_t kTypeData = 2;
inline constexpr uint8_t kTypeEnd = 3;
inline constexpr std::size_t kMaxChunk = 4096;   // DATA payload cap
inline constexpr std::size_t kHeaderLen = 4 + 1 + 2 + 2;  // magic..len
inline constexpr std::size_t kMaxPacket = kHeaderLen + kMaxChunk + 4;

inline void putU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}
inline void putU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

// Encode one packet. Shared by the host-side sender (scripts/carsh_ota.py
// mirrors this byte-for-byte) and the receiver tests, so the framing has one
// C++ definition and the tests pin it.
inline std::vector<uint8_t> encodePacket(uint8_t type, uint16_t seq,
                                         const uint8_t* payload, std::size_t n) {
    std::vector<uint8_t> v(kMagic, kMagic + 4);
    v.push_back(type);
    putU16(v, seq);
    putU16(v, uint16_t(n));
    if (n) v.insert(v.end(), payload, payload + n);
    putU32(v, crc32Of(v.data() + 4, v.size() - 4));  // over type..payload
    return v;
}

inline std::vector<uint8_t> encodeBegin(uint32_t totalSize, uint32_t blobCrc) {
    std::vector<uint8_t> p;
    putU32(p, totalSize);
    putU32(p, blobCrc);
    return encodePacket(kTypeBegin, 0, p.data(), p.size());
}

// Encode a whole blob as BEGIN + DATA* + END (test/reference sender).
inline std::vector<uint8_t> encodeTransfer(const std::vector<uint8_t>& blob,
                                           std::size_t chunk = kMaxChunk) {
    if (chunk == 0 || chunk > kMaxChunk) chunk = kMaxChunk;
    std::vector<uint8_t> out =
        encodeBegin(uint32_t(blob.size()), crc32Of(blob.data(), blob.size()));
    uint16_t seq = 1;
    for (std::size_t off = 0; off < blob.size(); off += chunk, ++seq) {
        const std::size_t n =
            blob.size() - off < chunk ? blob.size() - off : chunk;
        const auto pkt = encodePacket(kTypeData, seq, blob.data() + off, n);
        out.insert(out.end(), pkt.begin(), pkt.end());
    }
    const auto end = encodePacket(kTypeEnd, seq, nullptr, 0);
    out.insert(out.end(), end.begin(), end.end());
    return out;
}

}  // namespace push

// Receives a pushed blob from a raw byte stream. Feed it whatever the serial
// read returns; poll state(). On Complete, takeBlob() hands over the package
// and resets to Idle. Any protocol error is sticky until reset() so the caller
// can log the reason first (the host recovers by resending from BEGIN).
class PushReceiver {
public:
    enum class State : uint8_t { Idle, Receiving, Complete, Failed };
    enum class Error : uint8_t {
        None,
        TooLarge,         // BEGIN announced more than maxBlob
        BadLength,        // BEGIN payload malformed
        BadSeq,           // DATA out of order (a chunk was lost/duplicated)
        SizeOverrun,      // more DATA than BEGIN announced
        SizeMismatch,     // END with fewer bytes than announced
        BlobCrcMismatch,  // END: announced whole-blob CRC doesn't match
    };

    explicit PushReceiver(std::size_t maxBlob = 64u * 1024u * 1024u)
        : maxBlob_(maxBlob) {}

    void reset() {
        state_ = State::Idle;
        err_ = Error::None;
        acc_.clear();
        blob_.clear();
        blob_.shrink_to_fit();  // release a multi-MB image, not just clear it
        total_ = 0;
        announcedCrc_ = 0;
        runCrc_ = crc32Init();
        nextSeq_ = 0;
    }

    // Consume raw serial bytes. Cheap when idle (scans for the magic).
    void feed(const uint8_t* d, std::size_t n) {
        if (state_ == State::Failed || state_ == State::Complete) return;
        acc_.insert(acc_.end(), d, d + n);
        parse();
    }

    State state() const { return state_; }
    Error error() const { return err_; }
    std::size_t bytesReceived() const { return blob_.size(); }
    std::size_t totalSize() const { return total_; }
    unsigned packetsOk() const { return packetsOk_; }
    // Bytes discarded hunting for the magic (noise / aborted-transfer tail).
    unsigned resyncSkips() const { return resyncSkips_; }

    // Valid in Complete: hand the blob over and return to Idle.
    std::vector<uint8_t> takeBlob() {
        std::vector<uint8_t> out;
        if (state_ == State::Complete) {
            out = std::move(blob_);
            reset();
        }
        return out;
    }

    static const char* errorStr(Error e) {
        switch (e) {
            case Error::None: return "none";
            case Error::TooLarge: return "too-large";
            case Error::BadLength: return "bad-length";
            case Error::BadSeq: return "bad-seq";
            case Error::SizeOverrun: return "size-overrun";
            case Error::SizeMismatch: return "size-mismatch";
            case Error::BlobCrcMismatch: return "blob-crc-mismatch";
        }
        return "?";
    }

private:
    void fail(Error e) {
        state_ = State::Failed;
        err_ = e;
        acc_.clear();
    }

    void parse() {
        using namespace push;
        for (;;) {
            // Resync: drop everything before the next possible magic. DATA
            // payloads can contain "CPSH" too, but a false lock fails the CRC
            // below and we resume scanning one byte further on.
            std::size_t start = 0;
            while (start + 4 <= acc_.size() &&
                   !(acc_[start] == kMagic[0] && acc_[start + 1] == kMagic[1] &&
                     acc_[start + 2] == kMagic[2] && acc_[start + 3] == kMagic[3]))
                ++start;
            if (start > 0) {
                resyncSkips_ += unsigned(start);
                acc_.erase(acc_.begin(), acc_.begin() + long(start));
            }
            if (acc_.size() < kHeaderLen) return;  // need a full header

            const uint16_t len = uint16_t(acc_[7]) | uint16_t(acc_[8]) << 8;
            if (len > kMaxChunk) {  // can't be a real header — false magic
                resyncSkips_++;
                acc_.erase(acc_.begin());
                continue;
            }
            const std::size_t pktLen = kHeaderLen + len + 4;
            if (acc_.size() < pktLen) return;  // need the full packet

            const uint32_t want = uint32_t(acc_[pktLen - 4]) |
                                  uint32_t(acc_[pktLen - 3]) << 8 |
                                  uint32_t(acc_[pktLen - 2]) << 16 |
                                  uint32_t(acc_[pktLen - 1]) << 24;
            if (crc32Of(acc_.data() + 4, kHeaderLen - 4 + len) != want) {
                resyncSkips_++;          // corrupt or false magic: skip one byte
                acc_.erase(acc_.begin());
                continue;
            }

            const uint8_t type = acc_[4];
            const uint16_t seq = uint16_t(acc_[5]) | uint16_t(acc_[6]) << 8;
            const uint8_t* payload = acc_.data() + kHeaderLen;
            handlePacket(type, seq, payload, len);
            if (state_ == State::Failed || state_ == State::Complete) return;
            acc_.erase(acc_.begin(), acc_.begin() + long(pktLen));
        }
    }

    void handlePacket(uint8_t type, uint16_t seq, const uint8_t* p,
                      std::size_t n) {
        using namespace push;
        ++packetsOk_;
        if (type == kTypeBegin) {
            // A BEGIN always restarts reception — that's the host's retry path.
            if (n != 8) { fail(Error::BadLength); return; }
            const uint32_t total = uint32_t(p[0]) | uint32_t(p[1]) << 8 |
                                   uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
            const uint32_t crc = uint32_t(p[4]) | uint32_t(p[5]) << 8 |
                                 uint32_t(p[6]) << 16 | uint32_t(p[7]) << 24;
            if (total > maxBlob_) { fail(Error::TooLarge); return; }
            blob_.clear();
            blob_.reserve(total);
            total_ = total;
            announcedCrc_ = crc;
            runCrc_ = crc32Init();
            nextSeq_ = 1;
            state_ = State::Receiving;
            return;
        }
        if (state_ != State::Receiving) return;  // stray DATA/END: ignore
        if (type == kTypeData) {
            if (seq != nextSeq_) { fail(Error::BadSeq); return; }
            ++nextSeq_;
            if (blob_.size() + n > total_) { fail(Error::SizeOverrun); return; }
            blob_.insert(blob_.end(), p, p + n);
            runCrc_ = crc32Update(runCrc_, p, n);
            return;
        }
        if (type == kTypeEnd) {
            if (seq != nextSeq_) { fail(Error::BadSeq); return; }
            if (blob_.size() != total_) { fail(Error::SizeMismatch); return; }
            if (crc32Final(runCrc_) != announcedCrc_) {
                fail(Error::BlobCrcMismatch);
                return;
            }
            state_ = State::Complete;
            acc_.clear();
            return;
        }
        // Unknown type with a valid CRC: ignore (forward compatibility).
    }

    const std::size_t maxBlob_;
    State state_ = State::Idle;
    Error err_ = Error::None;
    std::vector<uint8_t> acc_;   // unparsed stream bytes (bounded by resync)
    std::vector<uint8_t> blob_;  // the accumulating package
    std::size_t total_ = 0;
    uint32_t announcedCrc_ = 0;
    uint32_t runCrc_ = crc32Init();
    uint16_t nextSeq_ = 0;
    unsigned packetsOk_ = 0;
    unsigned resyncSkips_ = 0;
};

}  // namespace carsh::ota
