// WavSinkDevice — host backend for ISoundDevice (PLAN.md working method).
//
// Stands in for the I2S DAC on the build server. There is no real clock, so the
// test driver explicitly pumps a number of frames; the device pulls them from
// the bound IAudioSink (the AudioEngine), records them, and writes a .wav on
// flush/close. Decode/DSP correctness is then verifiable by diffing the WAV
// against a golden reference.
//
// It also counts underruns (short returns from render) so tests can assert the
// engine kept the pipe fed.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "audio/WavFile.h"
#include "hal/ISoundDevice.h"

namespace carsh::host {

class WavSinkDevice : public carsh::hal::ISoundDevice {
public:
    explicit WavSinkDevice(std::string outPath) : path_(std::move(outPath)) {}
    ~WavSinkDevice() override { close(); }

    bool open(const carsh::audio::AudioFormat& fmt,
              carsh::hal::IAudioSink* sink) override {
        if (!fmt.valid() || sink == nullptr) return false;
        fmt_ = fmt;
        sink_ = sink;
        open_ = true;
        captured_.clear();
        underrunFrames_ = 0;
        return true;
    }

    void close() override {
        if (open_ && !captured_.empty())
            flush();
        open_ = false;
        sink_ = nullptr;
    }

    carsh::audio::AudioFormat format() const override { return fmt_; }
    bool isOpen() const override { return open_; }

    // Pulls `frames` interleaved frames from the sink into the capture buffer.
    // A short render() counts as underrun and the gap is zero-filled (mirrors
    // the DMA zero-filling a starved half-buffer).
    void pump(std::size_t frames) {
        if (!open_ || !sink_) return;
        const std::size_t ch = fmt_.channels;
        std::vector<float> block(frames * ch, 0.0f);
        std::size_t got = sink_->render(block.data(), frames);
        if (got < frames) underrunFrames_ += (frames - got);
        captured_.insert(captured_.end(), block.begin(), block.end());
    }

    // Writes captured audio to the WAV path. Uses the device bit depth; the
    // sink delivered float, so we write 16-bit PCM by default for stable diffs.
    bool flush(uint16_t bits = 16, bool asFloat = false) {
        if (captured_.empty()) return false;
        return WavWriter::write(path_, captured_.data(),
                                captured_.size() / fmt_.channels,
                                fmt_.channels, fmt_.sampleRate, bits, asFloat);
    }

    const std::vector<float>& captured() const { return captured_; }
    std::size_t underrunFrames() const { return underrunFrames_; }
    std::size_t capturedFrames() const {
        return fmt_.channels ? captured_.size() / fmt_.channels : 0;
    }

private:
    std::string path_;
    carsh::audio::AudioFormat fmt_;
    carsh::hal::IAudioSink* sink_ = nullptr;
    std::vector<float> captured_;
    std::size_t underrunFrames_ = 0;
    bool open_ = false;
};

}  // namespace carsh::host
