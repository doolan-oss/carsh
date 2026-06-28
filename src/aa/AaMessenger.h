// AaMessenger — Android Auto message framing, fragmentation, and reassembly over
// an IAaTransport byte stream. Ported from the dylant9/head_unit reference
// (aa_messenger). Platform-independent and host-tested (tests/aa).
//
// Wire format (aasdk):
//   [1B channel][1B flags][2B frame_size BE]([4B total_size BE] only on FIRST)[payload]
//   flags: bits 1:0 frame type · bit 2 message type · bit 3 encryption
//   For a PLAIN message the first 2 bytes of the reassembled payload are the
//   big-endian message id; for an ENCRYPTED message the id is inside the
//   ciphertext (so message_id is reported as 0 until decrypted upstream).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "aa/AaTypes.h"
#include "aa/IAaTransport.h"

namespace carsh::aa {

// A fully reassembled message handed up to the channel layer.
struct AaMessage {
    ChannelId channel = ChannelId::None;
    MessageType msgType = MessageType::Specific;
    EncryptionType encryption = EncryptionType::Plain;
    uint16_t messageId = 0;            // valid for PLAIN, or ENCRYPTED once decrypted
    std::vector<uint8_t> payload;      // message id stripped, unless still ciphertext
    bool decrypted = false;            // true if the messenger already decrypted (per-frame)
};

class AaMessenger {
public:
    explicit AaMessenger(IAaTransport& transport);

    // Per-frame decryptor (cipher payload -> plaintext). When set, ENCRYPTED frames are
    // decrypted INDIVIDUALLY and their plaintext concatenated — this is how aasdk does
    // it; a multi-frame message is NOT one big ciphertext blob, each frame is its own
    // self-contained TLS record(s). Without this a fragmented (large keyframe) message
    // fails to decrypt and desyncs the stream. Returns empty on a decrypt error.
    void setDecryptor(std::function<std::vector<uint8_t>(const uint8_t*, size_t)> d) {
        decryptor_ = std::move(d);
    }

    // Receive and reassemble exactly one complete message. Blocks (via the
    // transport's own timeout) until a whole message is available. Returns true
    // and fills `out` on success; false on transport timeout/error.
    bool receive(AaMessage& out, uint32_t timeoutMs);

    // Send a message, fragmenting into FIRST/MIDDLE/LAST when it exceeds one USB
    // frame. The 2-byte message id is prepended to the payload on the wire.
    bool send(ChannelId channel, EncryptionType enc, MessageType msgType,
              uint16_t messageId, const uint8_t* payload, size_t len);

    // Convenience wrappers mirroring the reference API.
    bool sendControl(uint16_t messageId, EncryptionType enc,
                     const uint8_t* payload, size_t len);
    bool sendSpecific(ChannelId channel, EncryptionType enc, uint16_t messageId,
                      const uint8_t* payload, size_t len);
    // Send a payload that is already complete (no message id prepended) — used for
    // encrypted frames where the id lives inside the ciphertext.
    bool sendRaw(ChannelId channel, EncryptionType enc, MessageType msgType,
                 const uint8_t* payload, size_t len);

private:
    struct Reasm {
        std::vector<uint8_t> buf;       // accumulated payload (plaintext if decrypted)
        MessageType msgType = MessageType::Specific;
        EncryptionType encryption = EncryptionType::Plain;
        bool active = false;
        bool decrypted = false;
    };

    // Process one complete frame already present at `raw`. Sets produced=true and
    // fills `out` when the frame completes a message.
    void processFrame(const uint8_t* raw, ChannelId channel, FrameType frameType,
                      MessageType msgType, EncryptionType enc,
                      uint16_t framePayloadSize, size_t hdrLen,
                      AaMessage& out, bool& produced);

    IAaTransport& tp_;
    std::function<std::vector<uint8_t>(const uint8_t*, size_t)> decryptor_;
    std::array<Reasm, static_cast<size_t>(kMaxChannel) + 1> reasm_{};
    std::vector<uint8_t> rxAcc_;  // un-parsed RX bytes carried across reads
};

}  // namespace carsh::aa
