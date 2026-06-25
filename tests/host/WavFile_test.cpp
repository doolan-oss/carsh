#include "audio/WavFile.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "Test.h"

using carsh::host::WavData;
using carsh::host::WavReader;
using carsh::host::WavWriter;

static std::string tmpPath(const char* name) {
    return std::string("/tmp/carsh_") + name;
}

TEST(WavFile, RoundTrip16BitStereo) {
    const uint32_t rate = 48000;
    const uint16_t ch = 2;
    std::vector<float> in;
    for (int i = 0; i < 480; ++i) {
        float t = float(i) / rate;
        in.push_back(0.5f * std::sin(2 * 3.14159265f * 440 * t));  // L
        in.push_back(0.25f * std::sin(2 * 3.14159265f * 220 * t)); // R
    }
    auto path = tmpPath("rt16.wav");
    REQUIRE(WavWriter::write(path, in.data(), in.size() / ch, ch, rate, 16));

    WavData out;
    REQUIRE(WavReader::read(path, out));
    CHECK_EQ(out.sampleRate, rate);
    CHECK_EQ(out.channels, ch);
    CHECK_EQ(out.bits, uint16_t(16));
    REQUIRE(out.samples.size() == in.size());
    // 16-bit quantization error is < 1/32768.
    double maxErr = 0;
    for (size_t i = 0; i < in.size(); ++i)
        maxErr = std::fmax(maxErr, std::fabs(in[i] - out.samples[i]));
    CHECK(maxErr < 1.0 / 32000.0);
    std::remove(path.c_str());
}

TEST(WavFile, RoundTripFloat32IsExact) {
    const uint32_t rate = 44100;
    std::vector<float> in = {0.0f, 1.0f, -1.0f, 0.123456f, -0.987654f};
    auto path = tmpPath("rtf32.wav");
    REQUIRE(WavWriter::write(path, in.data(), in.size(), 1, rate, 32, true));
    WavData out;
    REQUIRE(WavReader::read(path, out));
    CHECK(out.isFloat);
    REQUIRE(out.samples.size() == in.size());
    for (size_t i = 0; i < in.size(); ++i) CHECK_NEAR(in[i], out.samples[i], 1e-9);
    std::remove(path.c_str());
}

TEST(WavFile, RoundTrip24Bit) {
    const uint32_t rate = 96000;
    std::vector<float> in = {0.5f, -0.5f, 0.999f, -0.999f, 0.0f};
    auto path = tmpPath("rt24.wav");
    REQUIRE(WavWriter::write(path, in.data(), in.size(), 1, rate, 24));
    WavData out;
    REQUIRE(WavReader::read(path, out));
    CHECK_EQ(out.bits, uint16_t(24));
    REQUIRE(out.samples.size() == in.size());
    double maxErr = 0;
    for (size_t i = 0; i < in.size(); ++i)
        maxErr = std::fmax(maxErr, std::fabs(in[i] - out.samples[i]));
    CHECK(maxErr < 1.0 / 8000000.0);
    std::remove(path.c_str());
}
