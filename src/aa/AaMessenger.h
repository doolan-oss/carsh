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
#include <vector>

#include "aa/AaTypes.h"
#include "aa/IAaTransport.h"

namespace carsh::aa {

// A fully reassembled message handed up to the channel layer.
struct AaMessage {
    ChannelId channel = ChannelId::None;
    MessageType msgType = MessageType::Specific;
    EncryptionType encryption = EncryptionType::Plain;
    uint16_t messageId = 0;            // valid for PLAIN messages; 0 for ENCRYPTED
    std::vector<uint8_t> payload;      // message id stripped (PLAIN) / raw ciphertext (ENCRYPTED)
};

class AaMessenger {
public:
    explicit AaMessenger(IAaTransport& transport);

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
        std::vector<uint8_t> buf;
        size_t totalExpected = 0;
        size_t received = 0;
        MessageType msgType = MessageType::Specific;
        EncryptionType encryption = EncryptionType::Plain;
        bool active = false;
    };

    // Process one complete frame already present at `raw`. Sets produced=true and
    // fills `out` when the frame completes a message.
    void processFrame(const uint8_t* raw, ChannelId channel, FrameType frameType,
                      MessageType msgType, EncryptionType enc,
                      uint16_t framePayloadSize, size_t hdrLen,
                      AaMessage& out, bool& produced);

    IAaTransport& tp_;
    std::array<Reasm, static_cast<size_t>(kMaxChannel) + 1> reasm_{};
    std::vector<uint8_t> rxAcc_;  // un-parsed RX bytes carried across reads
};

}  // namespace carsh::aa
