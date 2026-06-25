// Concrete WAV/FLAC/MP3 decoders wrapping dr_libs over hal::IStream.
//
// dr_wav/dr_flac/dr_mp3 all take read/seek/tell callbacks with identical
// semantics (SET/CUR/END relative). We adapt them to hal::IStream so the same
// code reads from USB/SD FAT on the Pi and from HostVfs on the build server.
// All three emit interleaved float32 via *_read_pcm_frames_f32 — exactly the
// engine's working format.
//
// The concrete classes are private to this TU; callers get them via the factory.
#include "decode/DecoderFactory.h"

#include <cctype>
#include <cstring>

#include "dr_libs/dr_flac.h"
#include "dr_libs/dr_mp3.h"
#include "dr_libs/dr_wav.h"

namespace carsh::decode {
namespace {

using carsh::hal::IStream;

// --- Shared IStream <-> dr callback shims. Origin enums are SET=0/CUR=1/END=2
// in all three libraries, so a single integer switch serves all of them. -------

size_t streamRead(void* ud, void* out, size_t n) {
    return static_cast<IStream*>(ud)->read(out, n);
}

bool streamSeekImpl(IStream* s, int offset, int origin) {
    uint64_t base = 0;
    if (origin == 1)        // CUR
        base = s->tell();
    else if (origin == 2)   // END
        base = s->size();
    // SET: base = 0
    int64_t target = int64_t(base) + offset;
    if (target < 0) return false;
    return s->seek(uint64_t(target));
}

// Each library types its cursor as `long long`; write through a template so we
// don't fight `long` vs `long long` across the three.
template <typename T>
bool streamTellImpl(IStream* s, T* cursor) {
    *cursor = T(s->tell());
    return true;
}

// dr_wav shims
drwav_bool32 wavSeek(void* ud, int off, drwav_seek_origin o) {
    return streamSeekImpl(static_cast<IStream*>(ud), off, int(o));
}
drwav_bool32 wavTell(void* ud, drwav_int64* c) {
    return streamTellImpl(static_cast<IStream*>(ud), c);
}
// dr_flac shims
drflac_bool32 flacSeek(void* ud, int off, drflac_seek_origin o) {
    return streamSeekImpl(static_cast<IStream*>(ud), off, int(o));
}
drflac_bool32 flacTell(void* ud, drflac_int64* c) {
    return streamTellImpl(static_cast<IStream*>(ud), c);
}
// dr_mp3 shims
drmp3_bool32 mp3Seek(void* ud, int off, drmp3_seek_origin o) {
    return streamSeekImpl(static_cast<IStream*>(ud), off, int(o));
}
drmp3_bool32 mp3Tell(void* ud, drmp3_int64* c) {
    return streamTellImpl(static_cast<IStream*>(ud), c);
}

// --- WAV --------------------------------------------------------------------

class WavDecoder : public IDecoder {
public:
    ~WavDecoder() override { if (open_) drwav_uninit(&wav_); }

    bool open(IStream* s) override {
        if (!drwav_init(&wav_, streamRead, wavSeek, wavTell, s, nullptr))
            return false;
        open_ = true;
        fmt_.sampleRate = wav_.sampleRate;
        fmt_.channels = uint8_t(wav_.channels);
        fmt_.bits = uint8_t(wav_.bitsPerSample);
        fmt_.totalFrames = wav_.totalPCMFrameCount;
        return true;
    }
    std::size_t decode(float* out, std::size_t maxFrames) override {
        return std::size_t(drwav_read_pcm_frames_f32(&wav_, maxFrames, out));
    }
    bool seek(uint64_t frame) override {
        return drwav_seek_to_pcm_frame(&wav_, frame);
    }
    DecodedFormat format() const override { return fmt_; }

private:
    drwav wav_{};
    DecodedFormat fmt_{};
    bool open_ = false;
};

// --- FLAC -------------------------------------------------------------------

class FlacDecoder : public IDecoder {
public:
    ~FlacDecoder() override { if (flac_) drflac_close(flac_); }

    bool open(IStream* s) override {
        flac_ = drflac_open(streamRead, flacSeek, flacTell, s, nullptr);
        if (!flac_) return false;
        fmt_.sampleRate = flac_->sampleRate;
        fmt_.channels = uint8_t(flac_->channels);
        fmt_.bits = flac_->bitsPerSample;
        fmt_.totalFrames = flac_->totalPCMFrameCount;
        return true;
    }
    std::size_t decode(float* out, std::size_t maxFrames) override {
        return std::size_t(drflac_read_pcm_frames_f32(flac_, maxFrames, out));
    }
    bool seek(uint64_t frame) override {
        return drflac_seek_to_pcm_frame(flac_, frame);
    }
    DecodedFormat format() const override { return fmt_; }

private:
    drflac* flac_ = nullptr;
    DecodedFormat fmt_{};
};

// --- MP3 --------------------------------------------------------------------

class Mp3Decoder : public IDecoder {
public:
    ~Mp3Decoder() override { if (open_) drmp3_uninit(&mp3_); }

    bool open(IStream* s) override {
        if (!drmp3_init(&mp3_, streamRead, mp3Seek, mp3Tell, nullptr, s,
                        nullptr))
            return false;
        open_ = true;
        fmt_.sampleRate = mp3_.sampleRate;
        fmt_.channels = uint8_t(mp3_.channels);
        fmt_.bits = 16;  // MP3 has no native bit depth; we emit float anyway
        // Frame count requires a full scan; dr_mp3 caches it. This walks the
        // file once (seek table build) — acceptable at import/open time.
        fmt_.totalFrames = drmp3_get_pcm_frame_count(&mp3_);
        return true;
    }
    std::size_t decode(float* out, std::size_t maxFrames) override {
        return std::size_t(drmp3_read_pcm_frames_f32(&mp3_, maxFrames, out));
    }
    bool seek(uint64_t frame) override {
        return drmp3_seek_to_pcm_frame(&mp3_, frame);
    }
    DecodedFormat format() const override { return fmt_; }

private:
    drmp3 mp3_{};
    DecodedFormat fmt_{};
    bool open_ = false;
};

}  // namespace

// --- Factory ----------------------------------------------------------------

Codec sniff(IStream* s) {
    if (!s) return Codec::Unknown;
    uint8_t hdr[12] = {0};
    s->seek(0);
    size_t n = s->read(hdr, sizeof(hdr));
    s->seek(0);
    if (n >= 12 && !std::memcmp(hdr, "RIFF", 4) && !std::memcmp(hdr + 8, "WAVE", 4))
        return Codec::Wav;
    if (n >= 4 && !std::memcmp(hdr, "fLaC", 4)) return Codec::Flac;
    // MP3: ID3v2 tag ("ID3") or an MPEG audio sync word 0xFFEx.
    if (n >= 3 && !std::memcmp(hdr, "ID3", 3)) return Codec::Mp3;
    if (n >= 2 && hdr[0] == 0xFF && (hdr[1] & 0xE0) == 0xE0) return Codec::Mp3;
    return Codec::Unknown;
}

std::unique_ptr<IDecoder> createDecoder(Codec c) {
    switch (c) {
        case Codec::Wav: return std::make_unique<WavDecoder>();
        case Codec::Flac: return std::make_unique<FlacDecoder>();
        case Codec::Mp3: return std::make_unique<Mp3Decoder>();
        default: return nullptr;
    }
}

std::unique_ptr<IDecoder> openAuto(IStream* s) {
    auto dec = createDecoder(sniff(s));
    if (!dec) return nullptr;
    if (!dec->open(s)) return nullptr;
    return dec;
}

Codec codecFromExtension(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return Codec::Unknown;
    std::string ext;
    for (size_t i = dot + 1; i < path.size(); ++i)
        ext += char(std::tolower((unsigned char)path[i]));
    if (ext == "wav") return Codec::Wav;
    if (ext == "flac") return Codec::Flac;
    if (ext == "mp3") return Codec::Mp3;
    if (ext == "m4a" || ext == "aac") return Codec::Aac;
    if (ext == "alac") return Codec::Alac;
    if (ext == "opus" || ext == "ogg") return Codec::Opus;
    return Codec::Unknown;
}

}  // namespace carsh::decode
