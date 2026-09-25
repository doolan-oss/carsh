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
//
// Session end: the phone's SHUTDOWN_REQUEST (or any other path into
// AaState::Shutdown — AaConnection::shutdownReason() says which) arrives as
// onShutdown; a dongle unplug / USB re-cycle / link fault is reported by the owner
// calling disconnect(). Either way the projection
// drops its connection and runs reset() — setConnected(false) + a cleared frame — so
// the UI never sits on a frozen frame and the app's auto-show latch re-arms.
//
// Threading: onFrame() runs on the DECODE side (a dedicated core on the device);
// everything else runs on the UI/AA task. connected_/framePending_ are atomics so
// onFrame can drop frames that finish decoding after the session ended.
#pragma once

#include <atomic>
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

    // The live connection for outbound touch/focus. Rebound across dongle cycles.
    // Prefer disconnect() on teardown: setConnection(nullptr) only unbinds, it does
    // not reset the screen state.
    void setConnection(AaConnection* conn) { conn_ = conn; }

    // The session is gone (dongle unplugged / re-cycled, connection torn down or shut
    // down). Unbinds the connection FIRST — so the reset hooks (which may switch tabs
    // and so try to drop video focus) can't send on the dying link — then, if a
    // session had come up (connected, or a frame reached the screen since the last
    // reset), runs reset(): setConnected(false) + showFrame(nullptr). Idempotent: a
    // second call, or a call when nothing had come up (e.g. unplugged mid-handshake),
    // fires no hooks.
    void disconnect() {
        conn_ = nullptr;
        if (connected_.load(std::memory_order_relaxed) ||
            framePending_.load(std::memory_order_relaxed)) {
            reset();
        }
    }

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
        // Any path into AaState::Shutdown (phone request — already acknowledged —
        // version reject, TLS failure): the session is over.
        cbs.onShutdown = [this] { disconnect(); };
        return cbs;
    }

    // From the AA screen: one touch event inside the video (already remapped to
    // source-frame coords by AaVideoGeometry) -> the AA input channel. Send PRESS on
    // finger-down, DRAG on each move, RELEASE on lift; timestampUs is a monotonic µs
    // clock (64-bit — never a wrapping 32-bit tick). False if not projecting.
    bool forwardTouch(int sx, int sy, TouchAction action, uint64_t timestampUs) {
        if (conn_ == nullptr || sx < 0 || sy < 0) return false;
        return conn_->sendTouch(action, static_cast<uint32_t>(sx),
                                static_cast<uint32_t>(sy), timestampUs);
    }

    // From the player UI: drive AA media playback (play/pause/next/prev) by sending a
    // media key over the input channel. timestampUs is the same monotonic 64-bit µs
    // clock as forwardTouch's; it is deliberately NOT defaulted, so a zero-stamped key
    // (review T10) can't creep back in through a call that forgot it. False if not
    // projecting.
    bool sendMediaKey(MediaKey key, uint64_t timestampUs) {
        if (conn_ == nullptr) return false;
        return conn_->sendMediaKey(key, timestampUs);
    }

    // From the AA screen: the AA tab gained/lost focus -> VIDEO_FOCUS_INDICATION, so
    // the phone only streams while we're showing it. False if not projecting.
    bool setFocus(bool focused) {
        if (conn_ == nullptr) return false;
        return conn_->sendVideoFocus(focused ? VideoFocusMode::Focused
                                             : VideoFocusMode::Unfocused);
    }

    // IH264FrameSink: a decoded frame -> the screen. Runs on the decode side. Frames
    // that finish decoding after the session ended (AUs still queued for the decode
    // core when the phone shut down / the dongle dropped) are discarded: showing one
    // would re-freeze the AA tab on a dead picture and re-trigger the app's auto-show.
    void onFrame(const DecodedFrame& f) override {
        if (!connected_.load(std::memory_order_relaxed)) return;
        framePending_.store(true, std::memory_order_relaxed);
        if (hooks_.showFrame) hooks_.showFrame(f.pixels, f.width, f.height);
    }

    // True between the first channel open / video start and the session's end
    // (onShutdown or disconnect()). Safe to read from any core.
    bool connected() const { return connected_.load(std::memory_order_relaxed); }

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
        connected_.store(v, std::memory_order_relaxed);
        if (hooks_.setConnected) hooks_.setConnected(v);
    }
    void reset() {
        setConnected(false);  // first: gates onFrame before the frame is cleared
        framePending_.store(false, std::memory_order_relaxed);
        if (hooks_.showFrame) hooks_.showFrame(nullptr, 0, 0);  // -> placeholder
    }

    IH264Decoder* video_ = nullptr;
    AaConnection* conn_ = nullptr;
    Hooks hooks_;
    std::atomic<bool> connected_{false};
    // A frame was handed to showFrame since the last reset() — so disconnect() still
    // clears the screen if one raced past the connected_ gate as the session ended.
    std::atomic<bool> framePending_{false};
};

}  // namespace carsh::aa
