// AAC decoder (ADTS) wrapping faad2 over hal::IStream (PLAN.md §2.8/§6).
//
// faad2 decodes one AAC frame (1024 samples/channel) per NeAACDecDecode call
// from a byte window. We keep a sliding buffer topped up from the stream and
// carry leftover decoded frames in `pending_` so decode() honours arbitrary
// block sizes (the decode pump uses small blocks). Output is float (FAAD_FMT_FLOAT).
//
// Scope: ADTS transport only. MP4/M4A (.m4a) needs an MP4 demuxer to extract the
// raw AAC stream first — tracked as a backlog item. Seek supports rewind-to-0
// (re-init); arbitrary seek needs an ADTS frame index (backlog).
#ifdef CARSH_HAVE_AAC

#include <neaacdec.h>

#include <cstring>
#include <vector>

#include "decode/OptionalDecoders.h"
#include "hal/Vfs.h"

namespace carsh::decode {
namespace {

using carsh::hal::IStream;

constexpr std::size_t kWindow = 1u << 16;  // 64 KiB sliding buffer

class AacDecoder : public IDecoder {
public:
    ~AacDecoder() override { if (h_) NeAACDecClose(h_); }

    bool open(IStream* s) override {
        s_ = s;
        h_ = NeAACDecOpen();
        if (!h_) return false;
        NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(h_);
        cfg->outputFormat = FAAD_FMT_FLOAT;
        cfg->downMatrix = 0;
        NeAACDecSetConfiguration(h_, cfg);

        buf_.resize(kWindow);
        return reinit();
    }

    std::size_t decode(float* out, std::size_t maxFrames) override {
        std::size_t produced = 0;
        const uint8_t ch = fmt_.channels ? fmt_.channels : 1;
        while (produced < maxFrames) {
            if (pendingPos_ < pending_.size()) {
                std::size_t availFrames = (pending_.size() - pendingPos_) / ch;
                std::size_t want = maxFrames - produced;
                std::size_t take = want < availFrames ? want : availFrames;
                std::memcpy(out + produced * ch, pending_.data() + pendingPos_,
                            take * ch * sizeof(float));
                pendingPos_ += take * ch;
                produced += take;
                continue;
            }
            if (!decodeFrame()) break;  // EOF or error
        }
        return produced;
    }

    bool seek(uint64_t frame) override {
        if (frame != 0) return false;  // only rewind supported for ADTS
        if (!s_->seek(0)) return false;
        NeAACDecClose(h_);
        h_ = NeAACDecOpen();
        NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(h_);
        cfg->outputFormat = FAAD_FMT_FLOAT;
        cfg->downMatrix = 0;
        NeAACDecSetConfiguration(h_, cfg);
        fill_ = 0;
        pending_.clear();
        pendingPos_ = 0;
        return reinit();
    }

    DecodedFormat format() const override { return fmt_; }

private:
    void topUp() {
        if (fill_ < buf_.size()) {
            std::size_t got = s_->read(buf_.data() + fill_, buf_.size() - fill_);
            fill_ += got;
        }
    }
    void consume(std::size_t n) {
        if (n > fill_) n = fill_;
        std::memmove(buf_.data(), buf_.data() + n, fill_ - n);
        fill_ -= n;
        topUp();
    }
    bool reinit() {
        fill_ = 0;
        topUp();
        if (fill_ == 0) return false;
        unsigned long sr = 0;
        unsigned char ch = 0;
        long consumed = NeAACDecInit(h_, buf_.data(), (unsigned long)fill_, &sr, &ch);
        if (consumed < 0) return false;
        consume(std::size_t(consumed));
        fmt_.sampleRate = uint32_t(sr);
        fmt_.channels = uint8_t(ch);
        fmt_.bits = 16;
        fmt_.totalFrames = 0;  // unknown for streamed ADTS
        return fmt_.channels > 0;
    }
    // Decode one AAC frame into pending_. Returns false at EOF.
    bool decodeFrame() {
        if (fill_ == 0) return false;
        NeAACDecFrameInfo info;
        void* samples = NeAACDecDecode(h_, &info, buf_.data(), (unsigned long)fill_);
        if (info.error != 0) {
            // Resync by dropping a byte; bail if we run out.
            if (fill_ <= 1) return false;
            consume(1);
            return fill_ > 0;
        }
        consume(std::size_t(info.bytesconsumed));
        if (samples && info.samples > 0) {
            const float* f = static_cast<const float*>(samples);
            pending_.assign(f, f + info.samples);
            pendingPos_ = 0;
            if (info.channels) fmt_.channels = uint8_t(info.channels);
            if (info.samplerate) fmt_.sampleRate = uint32_t(info.samplerate);
        }
        return true;
    }

    IStream* s_ = nullptr;
    NeAACDecHandle h_ = nullptr;
    std::vector<uint8_t> buf_;
    std::size_t fill_ = 0;
    std::vector<float> pending_;
    std::size_t pendingPos_ = 0;
    DecodedFormat fmt_{};
};

}  // namespace

std::unique_ptr<IDecoder> makeAacDecoder() {
    return std::make_unique<AacDecoder>();
}

}  // namespace carsh::decode

#endif  // CARSH_HAVE_AAC
