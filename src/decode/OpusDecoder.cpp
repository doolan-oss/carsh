// Opus decoder wrapping libopusfile over hal::IStream (PLAN.md §2.8/§6).
//
// libopusfile handles the Ogg demux + Opus decode and always outputs 48 kHz
// float (Opus's internal rate). We feed it bytes through OpusFileCallbacks bound
// to hal::IStream, mirroring the dr_libs adapters. On bare-metal this same
// wrapper applies once libopus/libopusfile/libogg are cross-compiled (tracked in
// the porting backlog); on the host it links the system library and is genuinely
// tested against a real .opus fixture.
#ifdef CARSH_HAVE_OPUS

#include <opus/opusfile.h>

#include <cstdio>

#include "decode/OptionalDecoders.h"
#include "hal/Vfs.h"

namespace carsh::decode {
namespace {

using carsh::hal::IStream;

int opRead(void* ud, unsigned char* ptr, int nbytes) {
    if (nbytes < 0) return -1;
    return int(static_cast<IStream*>(ud)->read(ptr, std::size_t(nbytes)));
}
int opSeek(void* ud, opus_int64 offset, int whence) {
    IStream* s = static_cast<IStream*>(ud);
    opus_int64 base = 0;
    if (whence == SEEK_CUR)
        base = opus_int64(s->tell());
    else if (whence == SEEK_END)
        base = opus_int64(s->size());
    opus_int64 target = base + offset;
    if (target < 0) return -1;
    return s->seek(std::uint64_t(target)) ? 0 : -1;
}
opus_int64 opTell(void* ud) {
    return opus_int64(static_cast<IStream*>(ud)->tell());
}

class OpusDecoder : public IDecoder {
public:
    ~OpusDecoder() override { if (of_) op_free(of_); }

    bool open(IStream* s) override {
        OpusFileCallbacks cb{opRead, opSeek, opTell, nullptr};
        int err = 0;
        of_ = op_open_callbacks(s, &cb, nullptr, 0, &err);
        if (!of_ || err != 0) return false;
        fmt_.sampleRate = 48000;  // Opus is always 48 kHz
        fmt_.channels = uint8_t(op_channel_count(of_, -1));
        fmt_.bits = 16;
        ogg_int64_t total = op_pcm_total(of_, -1);
        fmt_.totalFrames = total > 0 ? uint64_t(total) : 0;
        return fmt_.channels > 0;
    }

    std::size_t decode(float* out, std::size_t maxFrames) override {
        // op_read_float takes a total-float buffer size; returns frames/channel.
        int got = op_read_float(of_, out, int(maxFrames * fmt_.channels), nullptr);
        return got > 0 ? std::size_t(got) : 0;
    }

    bool seek(uint64_t frame) override {
        return op_pcm_seek(of_, ogg_int64_t(frame)) == 0;
    }
    DecodedFormat format() const override { return fmt_; }

private:
    OggOpusFile* of_ = nullptr;
    DecodedFormat fmt_{};
};

}  // namespace

std::unique_ptr<IDecoder> makeOpusDecoder() {
    return std::make_unique<OpusDecoder>();
}

}  // namespace carsh::decode

#endif  // CARSH_HAVE_OPUS
