// AaMessenger — see AaMessenger.h. Faithful C++ port of the reference framing.
#include "aa/AaMessenger.h"

#include <cstring>

namespace carsh::aa {
namespace {

// RX accumulator capacity. A single AA frame's payload size is a uint16, so one frame
// can be up to 8 (max header) + 65535 bytes — far more than one USB transfer (video
// keyframes arrive as large frames). The accumulator must hold the largest legal frame
// plus a fresh read, or it would clear mid-frame and drop bytes — which is fatal on an
// encrypted channel: dropping ciphertext desyncs the TLS stream permanently.
constexpr size_t kRxAccCap = 8 + 0xFFFF + kUsbTransferSize;  // ~82 KB

inline uint16_t readBe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
inline uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
inline void writeBe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}
inline void writeBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

}  // namespace

AaMessenger::AaMessenger(IAaTransport& transport) : tp_(transport) {
    rxAcc_.reserve(kRxAccCap);
}

void AaMessenger::processFrame(const uint8_t* raw, ChannelId channel,
                               FrameType frameType, MessageType msgType,
                               EncryptionType enc, uint16_t framePayloadSize,
                               size_t hdrLen, AaMessage& out, bool& produced) {
    produced = false;

    const int ch = static_cast<int>(channel);
    if (ch > kMaxChannel) return;  // unknown channel — skip

    const bool isFirst = (frameType == FrameType::First);
    const uint8_t* payload = raw + hdrLen;
    Reasm& r = reasm_[static_cast<size_t>(ch)];

    if (isFirst || frameType == FrameType::Bulk) {
        // Start a new message. Reassembly is FRAME-TYPE driven (accumulate until LAST/
        // BULK), like aasdk — NOT size driven, since the FIRST frame's total_size is the
        // ciphertext sum which doesn't match the decrypted plaintext length.
        r.buf.clear();
        r.active = true;
        r.decrypted = false;
        r.msgType = msgType;
        r.encryption = enc;
        if (isFirst) {
            const uint32_t totalSize = readBe32(raw + 4);
            if (totalSize > kMaxMessageSize) {  // implausible — drop
                r.active = false;
                return;
            }
            r.buf.reserve(totalSize);  // hint only
        }
    }

    if (!r.active) return;  // MIDDLE/LAST with no active message — drop

    // Append this frame's payload. ENCRYPTED frames are decrypted INDIVIDUALLY (aasdk
    // model) — each frame is its own complete TLS record(s); reassembling ciphertext and
    // decrypting once corrupts multi-frame (large keyframe) messages and desyncs the TLS
    // stream. The decrypted plaintext chunks are concatenated into the message.
    if (enc == EncryptionType::Encrypted && decryptor_) {
        std::vector<uint8_t> plain = decryptor_(payload, framePayloadSize);
        r.buf.insert(r.buf.end(), plain.begin(), plain.end());
        r.decrypted = true;
    } else {
        r.buf.insert(r.buf.end(), payload, payload + framePayloadSize);
    }

    const bool complete = (frameType == FrameType::Bulk) || (frameType == FrameType::Last);
    if (!complete) return;

    out.channel = channel;
    out.msgType = r.msgType;
    out.encryption = r.encryption;
    out.decrypted = r.decrypted;

    // Plaintext (PLAIN, or ENCRYPTED-and-decrypted) carries the 2-byte message id; a
    // still-ciphertext payload (no decryptor) keeps id=0 for an upstream decrypt.
    const bool havePlaintext = (r.encryption == EncryptionType::Plain) || r.decrypted;
    if (havePlaintext && r.buf.size() >= 2) {
        out.messageId = readBe16(r.buf.data());
        out.payload.assign(r.buf.begin() + 2, r.buf.end());
    } else {
        out.messageId = 0;
        out.payload.assign(r.buf.begin(), r.buf.end());
    }

    r.buf.clear();
    r.active = false;
    produced = true;
}

bool AaMessenger::receive(AaMessage& out, uint32_t timeoutMs) {
    uint8_t readBuf[kUsbTransferSize];
    for (;;) {
        // 1. Extract and process every complete frame the accumulator holds.
        while (rxAcc_.size() >= 4) {
            const uint8_t* f = rxAcc_.data();
            const ChannelId channel = static_cast<ChannelId>(f[0]);
            const FrameType frameType = static_cast<FrameType>(f[1] & 0x03);
            const MessageType msgType = static_cast<MessageType>(f[1] & 0x04);
            const EncryptionType enc = static_cast<EncryptionType>(f[1] & 0x08);

            const size_t hdrLen = (frameType == FrameType::First) ? 8 : 4;
            if (rxAcc_.size() < hdrLen) break;  // need more for the header

            const uint16_t framePayloadSize = readBe16(f + 2);
            const size_t frameTotal = hdrLen + framePayloadSize;

            if (frameTotal > kRxAccCap) {
                // Cannot be a legal frame — the stream is desynced; drop & resync.
                rxAcc_.clear();
                break;
            }
            if (rxAcc_.size() < frameTotal) break;  // need more for the payload

            bool produced = false;
            processFrame(f, channel, frameType, msgType, enc, framePayloadSize, hdrLen,
                         out, produced);

            rxAcc_.erase(rxAcc_.begin(), rxAcc_.begin() + static_cast<long>(frameTotal));

            if (produced) return true;
        }

        // 2. Not enough buffered — read another transfer and append.
        const int n = tp_.read(readBuf, sizeof(readBuf), timeoutMs);
        if (n < 0) return false;  // error/disconnect
        if (n == 0) return false;  // timeout — caller re-polls
        size_t got = static_cast<size_t>(n);
        if (rxAcc_.size() + got > kRxAccCap) {
            // Defensive: a stuck/oversized partial frame is filling the buffer.
            rxAcc_.clear();
            if (got > kRxAccCap) got = kRxAccCap;
        }
        rxAcc_.insert(rxAcc_.end(), readBuf, readBuf + got);
    }
}

bool AaMessenger::send(ChannelId channel, EncryptionType enc, MessageType msgType,
                       uint16_t messageId, const uint8_t* payload, size_t len) {
    if (len > 0 && !payload) return false;

    // Full message payload on the wire is [2B message id][user payload].
    const size_t msgPayloadLen = 2 + len;
    const size_t maxBulk = kUsbTransferSize - 4;   // 2B header + 2B short size
    const size_t maxFirst = kUsbTransferSize - 8;  // + 4B total size
    const size_t maxCont = kUsbTransferSize - 4;

    std::vector<uint8_t> frame;

    if (msgPayloadLen <= maxBulk) {
        frame.resize(4 + msgPayloadLen);
        frame[0] = static_cast<uint8_t>(channel);
        frame[1] = frameFlags(FrameType::Bulk, msgType, enc);
        writeBe16(frame.data() + 2, static_cast<uint16_t>(msgPayloadLen));
        writeBe16(frame.data() + 4, messageId);
        if (len > 0) std::memcpy(frame.data() + 6, payload, len);
        return tp_.write(frame.data(), frame.size(), 5000) >= 0;
    }

    // Multi-frame: assemble the contiguous message payload, then chunk it.
    std::vector<uint8_t> msg(msgPayloadLen);
    writeBe16(msg.data(), messageId);
    if (len > 0) std::memcpy(msg.data() + 2, payload, len);

    size_t off = 0;
    bool first = true;
    while (off < msgPayloadLen) {
        const size_t remaining = msgPayloadLen - off;
        FrameType ft;
        size_t chunk;
        if (first) {
            ft = FrameType::First;
            chunk = remaining < maxFirst ? remaining : maxFirst;
        } else if (remaining <= maxCont) {
            ft = FrameType::Last;
            chunk = remaining;
        } else {
            ft = FrameType::Middle;
            chunk = maxCont;
        }

        const size_t hdrLen = first ? 8u : 4u;
        frame.resize(hdrLen + chunk);
        frame[0] = static_cast<uint8_t>(channel);
        frame[1] = frameFlags(ft, msgType, enc);
        writeBe16(frame.data() + 2, static_cast<uint16_t>(chunk));
        if (first) {
            writeBe32(frame.data() + 4, static_cast<uint32_t>(msgPayloadLen));
            first = false;
        }
        std::memcpy(frame.data() + hdrLen, msg.data() + off, chunk);
        off += chunk;

        if (tp_.write(frame.data(), frame.size(), 5000) < 0) return false;
    }
    return true;
}

bool AaMessenger::sendControl(uint16_t messageId, EncryptionType enc,
                              const uint8_t* payload, size_t len) {
    return send(ChannelId::Control, enc, MessageType::Control, messageId, payload, len);
}

bool AaMessenger::sendSpecific(ChannelId channel, EncryptionType enc, uint16_t messageId,
                               const uint8_t* payload, size_t len) {
    return send(channel, enc, MessageType::Specific, messageId, payload, len);
}

bool AaMessenger::sendRaw(ChannelId channel, EncryptionType enc, MessageType msgType,
                          const uint8_t* payload, size_t len) {
    const size_t maxBulk = kUsbTransferSize - 4;
    std::vector<uint8_t> frame;

    if (len <= maxBulk) {
        frame.resize(4 + len);
        frame[0] = static_cast<uint8_t>(channel);
        frame[1] = frameFlags(FrameType::Bulk, msgType, enc);
        writeBe16(frame.data() + 2, static_cast<uint16_t>(len));
        if (len > 0) std::memcpy(frame.data() + 4, payload, len);
        return tp_.write(frame.data(), frame.size(), 5000) >= 0;
    }

    const size_t maxFirst = kUsbTransferSize - 8;
    const size_t maxCont = kUsbTransferSize - 4;
    size_t off = 0;
    bool first = true;
    while (off < len) {
        const size_t remaining = len - off;
        FrameType ft;
        size_t chunk;
        if (first) {
            ft = FrameType::First;
            chunk = remaining < maxFirst ? remaining : maxFirst;
        } else if (remaining <= maxCont) {
            ft = FrameType::Last;
            chunk = remaining;
        } else {
            ft = FrameType::Middle;
            chunk = maxCont;
        }
        const size_t hdrLen = first ? 8u : 4u;
        frame.resize(hdrLen + chunk);
        frame[0] = static_cast<uint8_t>(channel);
        frame[1] = frameFlags(ft, msgType, enc);
        writeBe16(frame.data() + 2, static_cast<uint16_t>(chunk));
        if (first) {
            writeBe32(frame.data() + 4, static_cast<uint32_t>(len));
            first = false;
        }
        std::memcpy(frame.data() + hdrLen, payload + off, chunk);
        off += chunk;
        if (tp_.write(frame.data(), frame.size(), 5000) < 0) return false;
    }
    return true;
}

}  // namespace carsh::aa
