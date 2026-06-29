// AaProjection — wires a live AaConnection to the video decoder, the AA screen, and
// the audio output (AA_PLAN rung 6). It is platform-independent: the app supplies
// hooks (show a decoded frame, play PCM, set the connected state) and maps its UI
// touch/focus into the typed calls here, so the whole projection is host-tested with
// fakes. The device kernel just provides the real hooks + an IH264Decoder.
//
// Data flow:
//   phone --AaConnection--> onAvMedia(video) --> decoder.feed() --> onFrame() -->
//       showFrame hook --> AndroidAutoScreen::setFrame
//   phone --AaConnection--> onAvMedia(audio) --> playAudio hook --> PcmRing/sound
//   AA screen touch  --> forwardTouch() --> AaConnection::sendTouch (input channel)
//   AA tab focus     --> setFocus()      --> AaConnection::sendVideoFocus
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "aa/AaConnection.h"
#include "aa/AaTypes.h"
#include "aa/IH264Decoder.h"

namespace carsh::aa {

class AaProjection : public IH264FrameSink {
public:
    struct Hooks {
        // Latest decoded video frame (RGB888, w*h*3 bytes), valid until the next
        // call. nullptr/0 dims clears to the placeholder. -> AndroidAutoScreen.
        std::function<void(const uint8_t* rgb, int w, int h)> showFrame;
        // Decoded PCM (16-bit) for an audio channel -> the engine ring / sound dev.
        std::function<void(ChannelId channel, const uint8_t* pcm, size_t len)> playAudio;
        // Projection came up / went down (a channel opened, or the phone shut down)
        // -> AndroidAutoScreen::setConnected (placeholder copy + state).
        std::function<void(bool connected)> setConnected;
    };

    explicit AaProjection(Hooks hooks) : hooks_(std::move(hooks)) {}

    // The video decoder whose sink is THIS projection (set after construction to
    // break the decoder<->sink cycle). Video media is fed here.
    void setDecoder(IH264Decoder* decoder) { video_ = decoder; }

    // The live connection for outbound touch/focus. Rebound across dongle cycles;
    // pass nullptr on teardown.
    void setConnection(AaConnection* conn) { conn_ = conn; }

    // The AaConnection callbacks that route inbound traffic into this projection.
    // Pass to the AaConnection ctor, then call setConnection with the same object.
    AaConnectionCallbacks callbacks() {
        AaConnectionCallbacks cbs;
        cbs.onAvMedia = [this](ChannelId ch, bool ts, uint64_t pts, const uint8_t* d,
                               size_t n) { onAvMedia(ch, ts, pts, d, n); };
        cbs.onChannelOpened = [this](ChannelId) { setConnected(true); };
        cbs.onAvStart = [this](ChannelId ch, bool started) {
            if (ch == ChannelId::Video && started) setConnected(true);
        };
        cbs.onShutdown = [this] { reset(); };
        return cbs;
    }

    // From the AA screen: a touch inside the video (already remapped to source-frame
    // coords by AaVideoGeometry) -> the AA input channel. False if not projecting.
    bool forwardTouch(int sx, int sy, TouchAction action, uint64_t timestamp) {
        if (conn_ == nullptr || sx < 0 || sy < 0) return false;
        return conn_->sendTouch(action, static_cast<uint32_t>(sx),
                                static_cast<uint32_t>(sy), timestamp);
    }

    // From the player UI: drive AA media playback (play/pause/next/prev) by sending a
    // media key over the input channel. False if not projecting.
    bool sendMediaKey(MediaKey key, uint64_t timestamp = 0) {
        if (conn_ == nullptr) return false;
        return conn_->sendMediaKey(key, timestamp);
    }

    // From the AA screen: the AA tab gained/lost focus -> VIDEO_FOCUS_INDICATION, so
    // the phone only streams while we're showing it. False if not projecting.
    bool setFocus(bool focused) {
        if (conn_ == nullptr) return false;
        return conn_->sendVideoFocus(focused ? VideoFocusMode::Focused
                                             : VideoFocusMode::Unfocused);
    }

    // IH264FrameSink: a decoded frame -> the screen.
    void onFrame(const DecodedFrame& f) override {
        if (hooks_.showFrame) hooks_.showFrame(f.pixels, f.width, f.height);
    }

    bool connected() const { return connected_; }

private:
    void onAvMedia(ChannelId ch, bool /*ts*/, uint64_t /*pts*/, const uint8_t* d,
                   size_t n) {
        if (ch == ChannelId::Video) {
            if (video_ != nullptr) video_->feed(d, n);  // -> onFrame -> showFrame
        } else if (hooks_.playAudio) {
            hooks_.playAudio(ch, d, n);
        }
    }
    void setConnected(bool v) {
        connected_ = v;
        if (hooks_.setConnected) hooks_.setConnected(v);
    }
    void reset() {
        setConnected(false);
        if (hooks_.showFrame) hooks_.showFrame(nullptr, 0, 0);  // -> placeholder
    }

    IH264Decoder* video_ = nullptr;
    AaConnection* conn_ = nullptr;
    Hooks hooks_;
    bool connected_ = false;
};

}  // namespace carsh::aa
