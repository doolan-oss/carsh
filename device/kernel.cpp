// carsh device kernel. Brings up serial + SD/FAT + framebuffer + touch, builds
// the full app (library, player, EQ, profiles) from the same core the host tests
// cover, and runs the UI/input loop. Audio output and touch touch real hardware,
// so they are only activated on a board (not in the QEMU boot demo); everything
// is constructed here so the whole stack is built + linked bare-metal.
#include "kernel.h"

#include <circle/bcmframebuffer.h>
#include <circle/devicenameservice.h>

#include <cstdarg>
#include <cstdio>

#include <algorithm>
#include <cmath>
#include <vector>

#include "CircleBluetooth.h"
#include "bt/DeviceRow.h"
#include "CircleBtRunLoopTask.h"
#include "CircleDecodeTask.h"
#include "CircleAaDriver.h"
#include "CircleAoaTransport.h"
#include "aa/AaProjection.h"
#ifdef CARSH_HAVE_OPENH264
#include "CircleH264Decoder.h"
#endif
#include "CircleUsbLog.h"
#include "CircleFlashTarget.h"
#include "CirclePwmSoundDevice.h"
#include "CircleRenderTarget.h"
#include "CircleSoundDevice.h"
#include "CircleTouch.h"
#include "CircleVfs.h"
#include "DeviceCrypto.h"
#include "ota/OtaManager.h"
#include "ota/Package.h"
#include "audio/AudioEngine.h"
#include "audio/PcmRing.h"
#include "dsp/Eq.h"
#include "library/Database.h"
#include "library/Indexer.h"
#include "player/PlayerController.h"
#include "profile/ProfileManager.h"
#include "ui/Canvas.h"
#include "ui/Screen.h"
#include "ui/AndroidAutoScreen.h"
#include "ui/Screens.h"

#define FROM "carsh"

namespace {
// Long-lived app state at file scope: the PcmRing's buffer is large (~1 s of
// audio) and must not sit on the stack, and these persist across the run loop.
constexpr unsigned kRate = 48000;
constexpr uint8_t kChannels = 2;

carsh::library::Database g_db;
carsh::audio::PcmRing<float> g_ring(kRate * kChannels);  // ~1 s stereo (local)
carsh::audio::PcmRing<float> g_btRing(kRate * kChannels);  // A2DP jitter+decode
carsh::audio::AudioEngine g_engine(&g_ring, kChannels);
carsh::dsp::EqProcessor g_eq(kRate);

// PWM output sink: plays decoded A2DP PCM when the BT ring has data, otherwise the
// local player engine. The DMA/IRQ callback calls render(); the BT ring is the
// SPSC consumer here (producer is the A2DP decode callback). BT bypasses the
// engine's volume/EQ for now (the phone controls volume).
class BtMixSink : public carsh::hal::IAudioSink {
public:
    std::size_t render(float* out, std::size_t frames) override {
        const std::size_t want = frames * kChannels;
        if (g_btRing.readable() >= want) {
            std::size_t got = g_btRing.read(out, want);
            for (std::size_t i = got; i < want; ++i) out[i] = 0.0f;
            return frames;
        }
        return g_engine.render(out, frames);
    }
};
BtMixSink g_outSink;
carsh::device::CircleSource g_sdSource(0, carsh::hal::SourceType::Sd, "SD:");
carsh::device::CircleSource g_usbSource(1, carsh::hal::SourceType::Usb, "USB:");
carsh::profile::ProfileManager g_profiles;
carsh::bt::AvrcpMetadata g_btMeta;  // latest AVRCP now-playing (BT source)
std::vector<carsh::bt::DiscoveredDevice> g_btDevices;  // scan results for the BT tab
}  // namespace

CKernel::CKernel(void)
    : CStdlibApp("carsh"),
      mSerial(&mInterrupt),
      mTimer(&mInterrupt),
      mLogger(mOptions.GetLogLevel(), &mTimer),
      mEMMC(&mInterrupt, &mTimer, &mActLED),
      mUSBHCI(&mInterrupt, &mTimer, TRUE),
      mSerialCDC(&mInterrupt) {}

void CKernel::diag(const char* fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    mLogger.Write(FROM, LogNotice, "%s", buf);
    mDiag.emplace_back(buf);
    if (mDiag.size() > 16) mDiag.pop_front();
}

bool CKernel::Initialize(void) {
    if (!CStdlibApp::Initialize()) return false;
    if (!mSerial.Initialize(115200)) return false;
    if (!mLogger.Initialize(&mSerial)) return false;
    if (!mTimer.Initialize()) return false;

    // Bring the USB-C serial-log gadget up and retarget the logger to it BEFORE the
    // xHCI host initializes, so Circle's per-device USB enumeration log — including
    // UNBOUND devices (e.g. the AA dongle, which binds no Circle driver) and any
    // enumeration errors — is captured over USB-C, instead of scrolling past on the
    // GPIO UART before the console exists. The dwc2 USB-C gadget is independent of
    // the xHCI host, so it can come up first. (CScheduler is the kernel's mScheduler
    // member, valid here in Initialize().)
    mUsbGadgetOk = mSerialCDC.Initialize();
    if (mUsbGadgetOk) {
        new carsh::device::CircleUsbLog(&mSerialCDC, &mLogger, &mSerial);
        for (unsigned i = 0; i < 400; ++i) {
            CScheduler::Get()->Yield();  // let the gadget PnP task run + host attach
            if (CDeviceNameService::Get()->GetDevice("utty1", FALSE)) break;
            mTimer.MsDelay(5);
        }
    }
    diag("USB-C: serial-log gadget %s", mUsbGadgetOk ? "ready" : "FAILED");

    if (!mUSBHCI.Initialize()) diag("USB: host controller init FAILED");

    mountStorage();
    return true;
}

void CKernel::mountStorage(void) {
    // SD (EMMC) — the boot card; may also hold media.
    if (mEMMC.Initialize() && f_mount(&mFileSystem, "SD:", 1) == FR_OK) {
        mSdMounted = true;
        diag("SD: mounted (FAT)");
    } else {
        diag("SD: not mounted");
    }

    // USB mass storage. With plug-and-play, a stick connected at power-on is
    // enumerated during mUSBHCI.Initialize(); give it a few PnP cycles in case
    // it is slow to settle, then mount the first device (umsd1) as USB:.
    for (int attempt = 0; attempt < 20 && !mUsbMounted; ++attempt) {
        if (CDeviceNameService::Get()->GetDevice("umsd1", TRUE)) {
            FRESULT r = f_mount(&mUsbFs, "USB:", 1);
            if (r == FR_OK) {
                mUsbMounted = true;
                diag("USB: mounted (FAT) on umsd1");
            } else {
                diag("USB: umsd1 found, mount failed (%d) — FAT32?", int(r));
            }
            return;
        }
        mUSBHCI.UpdatePlugAndPlay();
        mTimer.MsDelay(50);
    }
    diag("USB: no mass-storage device detected");
}

void CKernel::scanLibrary(carsh::hal::Source& src) {
    carsh::library::Indexer idx(g_db);
    idx.beginScan(src);
    diag("indexing %u files...", unsigned(idx.progress().filesTotal));
    while (idx.step(8)) { /* CScheduler::Yield() once the decode task is wired */ }
    diag("library: %u tracks, %u albums", unsigned(g_db.trackCount()),
         unsigned(g_db.albums().size()));
}

void CKernel::checkOta(void) {
    if (!mSdMounted) return;
    carsh::device::CircleVfs vfs("SD:");
    auto s = vfs.open("/carsh/ota/update.carup");
    if (!s) { mLogger.Write(FROM, LogNotice, "OTA: no staged package"); return; }

    std::vector<uint8_t> blob(std::size_t(s->size()));
    s->read(blob.data(), blob.size());

    carsh::device::DeviceCrypto crypto;
    carsh::device::CircleFlashTarget flash("SD:");
    // PLACEHOLDER signing key — provision the real Ed25519 public key at build
    // time (compiled into the image + recovery image). See OPEN_QUESTIONS Q5.
    static const carsh::ota::Ed25519PublicKey kOtaPubKey{};
    carsh::ota::OtaManager ota(crypto, kOtaPubKey, flash);

    carsh::ota::Package pkg;
    if (!carsh::ota::parsePackage(blob, pkg)) {
        mLogger.Write(FROM, LogWarning, "OTA: malformed package");
        return;
    }
    auto vr = carsh::ota::verifyPackage(pkg, crypto, kOtaPubKey);
    mLogger.Write(FROM, LogNotice,
                  "OTA: staged %s, verify=%d (confirm in Settings to apply)",
                  pkg.manifest.version.c_str(), int(vr));
    (void)ota;  // apply()/commit() are user-confirmed (PLAN §8.5); not automatic
}

CStdlibApp::TShutdownMode CKernel::Run(void) {
    mLogger.Write(FROM, LogNotice, "carsh boot — bare-metal automotive head unit");

    // Wire the audio DSP chain (EQ inserted into the engine). Playback is started
    // on demand via the player on hardware; the I2S device is not opened here.
    g_engine.setSampleProcessor(&g_eq);
    g_engine.setMasterVolume(0.0f);  // boot muted; the left volume rail raises it

    // Prefer a USB stick for media; fall back to the SD card. The player must
    // read through the SAME source we index, so logical track paths ("/Music/..")
    // resolve against the right FatFs drive.
    carsh::hal::Source& media =
        mUsbMounted ? static_cast<carsh::hal::Source&>(g_usbSource)
                    : static_cast<carsh::hal::Source&>(g_sdSource);
    diag("media source: %s", mUsbMounted ? "USB:" : "SD:");

    carsh::player::PlayerController player(media.vfs(), g_ring, g_engine,
                                           kChannels);

    scanLibrary(media);
    checkOta();

    // Framebuffer + UI.
    CBcmFrameBuffer fb(800, 480, 32);
    if (!fb.Initialize()) {
        mLogger.Write(FROM, LogError, "framebuffer init failed");
        return ShutdownHalt;
    }
    carsh::device::CircleRenderTarget rt(&fb);
    carsh::ui::Canvas canvas(rt);
    carsh::ui::ScreenManager mgr(rt.width(), rt.height());

    carsh::ui::NowPlayingScreen now(mgr.contentRect());
    carsh::ui::ListScreen library(mgr.contentRect(), "Lib");
    carsh::ui::SearchScreen search(mgr.contentRect());
    carsh::ui::ListScreen queue(mgr.contentRect(), "Q");
    carsh::ui::EqScreen eq(mgr.contentRect());
    carsh::ui::ListScreen sources(mgr.contentRect(), "Src");
    carsh::ui::SettingsScreen settings(mgr.contentRect());
    carsh::ui::DevicesScreen devices(mgr.contentRect());
    carsh::ui::AndroidAutoScreen androidAuto(mgr.contentRect());
    mgr.addScreen(&now, "Now");
    mgr.addScreen(&library, "Lib");
    mgr.addScreen(&search, "Find");
    mgr.addScreen(&queue, "Q");
    mgr.addScreen(&eq, "EQ");
    mgr.addScreen(&sources, "Src");
    mgr.addScreen(&settings, "Set");
    mgr.addScreen(&devices, "BT");
    // Android Auto projection tab. Shows the placeholder until the USB-AOA dongle
    // transport + H.264 decoder land (AA_PLAN rungs 3-6); the screen blits frames
    // into the content rect, leaving the volume rail + tab bar visible.
    mgr.addScreen(&androidAuto, "AA");

    // Left volume rail -> engine master volume. Boots muted (setMasterVolume(0)
    // above); dragging the rail raises it.
    mgr.setOnVolumeChange([&](float v) { g_engine.setMasterVolume(v); });

    // Library list -> tap queues + plays that track.
    std::vector<uint64_t> ids = g_db.allTracks();
    std::vector<std::string> rows;
    std::vector<std::string> paths;
    for (uint64_t id : ids) {
        const carsh::library::Track* t = g_db.get(id);
        if (!t) continue;
        rows.push_back(g_db.artist(*t) + " - " + g_db.title(*t));
        paths.push_back(g_db.path(*t));
    }
    if (rows.empty()) rows.push_back("(insert FAT32 media with music)");
    library.setItems(rows);
    library.setOnSelect([&](int i) {
        if (i >= 0 && i < int(paths.size())) {
            player.setQueue(paths);
            player.play(i);  // starts decode; audio device opened on hardware
        }
    });

    // EQ screen edits feed the live engine EQ.
    eq.onBandChange = [&](int, double) { g_eq.setConfig(eq.config()); };
    now.onPlayPause = [&] {
        if (player.state().transport == carsh::player::Transport::Playing)
            player.pause();
        else
            player.resume();
    };
    now.onNext = [&] { player.next(); };
    now.onPrev = [&] { player.prev(); };

    // Touch (real hardware only; QEMU has no digitizer).
    carsh::device::CircleTouch touch;
    bool haveTouch = touch.initialize(&fb);  // fb supplies touch coord mapping
    mLogger.Write(FROM, LogNotice, "touch: %s", haveTouch ? "ready" : "absent");

    // (The USB-C serial-log console is already up — it's brought up before the xHCI
    // host in Initialize() so the enumeration log is captured.)

    // AOA Rung 1 — list every registered USB device, now that the USB-C console is
    // up so it is captured over serial (the host's own enumeration log goes out the
    // GPIO UART before this point). Tells us how the CA525 dongle powers up: a bound
    // driver name (uhubN / uttyN / umsdN / venXXXX-YYYY), or absent if it enumerated
    // unbound. Pump plug-and-play first to let a slow dongle settle. See AA_PLAN.
    for (unsigned i = 0; i < 25; ++i) {
        mUSBHCI.UpdatePlugAndPlay();
        CScheduler::Get()->Yield();
        mTimer.MsDelay(20);
    }
    mLogger.Write(FROM, LogNotice, "AOA Rung-1: registered USB/devices --");
    CDeviceNameService::Get()->EnumerateDevices(
        [](CDevice*, const char* name, boolean bBlock, void* arg) -> boolean {
            static_cast<CLogger*>(arg)->Write("rung1", LogNotice, "  dev: %s%s", name,
                                              bBlock ? " [block]" : "");
            return TRUE;  // keep enumerating
        },
        &mLogger);
    mLogger.Write(FROM, LogNotice, "AOA Rung-1: end device list");

    // Bluetooth (A2DP sink + AVRCP). Connect switches the driver profile (keyed
    // on the stable Classic address); AVRCP metadata feeds the BT now-playing;
    // decoded A2DP PCM flows into the BT ring. Live only when BTstack is compiled
    // in (CARSH_HAVE_BTSTACK) and the controller comes up.
    static carsh::device::CircleBluetooth bt;  // outlives Run for the glue callbacks
    carsh::bt::BluetoothCallbacks btcb;
    btcb.onConnected = [](const carsh::bt::BdAddr& a) {
        carsh::profile::BdAddr pa; pa.b = a;
        g_profiles.onBtConnected(pa);
    };
    btcb.onDisconnected = [](const carsh::bt::BdAddr&) { g_profiles.onBtDisconnected(); };
    btcb.onMetadata = [](const carsh::bt::AvrcpMetadata& m) { g_btMeta = m; };
    btcb.onA2dpPcm = [](const float* p, std::size_t f, uint8_t ch) {
        g_btRing.write(p, f * ch);
    };
    // Scan results feed the BT tab's device cache (upsert by address).
    btcb.onDeviceDiscovered = [](const carsh::bt::DiscoveredDevice& d) {
        for (auto& e : g_btDevices)
            if (e.address == d.address) { e = d; return; }
        g_btDevices.push_back(d);
    };
    btcb.onConnectFailed = [this](const carsh::bt::BdAddr&, int reason) {
        mLogger.Write(FROM, LogWarning, "BT connect failed: 0x%x", reason);
    };
    bt.setCallbacks(btcb);
    if (bt.init()) {
        new carsh::device::CircleBtRunLoopTask();
        mLogger.Write(FROM, LogNotice, "Bluetooth: ready (A2DP/AVRCP)");
    } else {
        mLogger.Write(FROM, LogNotice,
                      "Bluetooth: not built (define CARSH_HAVE_BTSTACK + add BTstack)");
    }

    // BT tab intents -> the radio. `bt` (static) and g_btDevices (file scope) have
    // static storage, so the lambdas reference them without capture (no dangling
    // capture of `devices`). connect() itself stops any in-progress inquiry.
    devices.onScan = [] {
        if (bt.isScanning()) bt.stopScan();
        else { g_btDevices.clear(); bt.startScan(); }
    };
    devices.onConnect = [](int i) {
        if (i < 0 || i >= int(g_btDevices.size())) return;
        bt.connect(g_btDevices[std::size_t(i)].address);
    };
    devices.onDisconnect = [] { bt.disconnect(); };

    // Spawn the cooperative decode task (sole ring producer). The scheduler owns
    // it; it runs whenever this (UI) task yields via MsSleep below.
    new carsh::device::CircleDecodeTask(&player);

    // (USB-C serial-log gadget was brought up above, before Bluetooth init.)

    // Audio output: drive the Pi's onboard 3.5mm jack via PWM (no extra hardware).
    // Opened LAZILY at the first track's sample rate in the loop below (the core
    // has no resampler, so the device must clock at the track's native rate, and
    // opening from a closed state avoids tearing down a running DMA). Once open,
    // its DMA/IRQ callback is the sole ring consumer, so playback advances and
    // sound comes out the jack. A USB/I2S DAC would slot in here in future.
    carsh::device::CirclePwmSoundDevice sound(&mInterrupt);
    unsigned curRate = 0;  // 0 = not opened yet

    // Android Auto projection wiring (rung 6): decoded video -> the AA tab, AA audio
    // -> a ring, content-rect touch -> the AA input channel, tab focus -> VIDEO_FOCUS.
    // The decoder is linked only with CARSH_HAVE_OPENH264; without it the AA tab
    // stays on the placeholder but TLS/channels/audio/touch still run. The projection
    // is declared BEFORE aaDriver so it outlives the driver (whose teardown clears
    // the projection's connection pointer).
    carsh::audio::PcmRing<float> aaAudioRing(kRate);  // ~1s; drained in rung 7 (audio focus)
    carsh::aa::AaProjection::Hooks aaHooks;
    aaHooks.showFrame = [&](const uint8_t* rgb, int w, int h) {
        androidAuto.setFrame(rgb, w, h);
    };
    aaHooks.setConnected = [&](bool up) { androidAuto.setConnected(up); };
    aaHooks.playAudio = [&](carsh::aa::ChannelId, const uint8_t* pcm, std::size_t len) {
        // 16-bit LE PCM -> float, into the AA ring. Draining it to the jack with
        // audio focus / mixing vs the music engine is rung 7.
        const std::size_t n = len / 2;
        float buf[512];
        for (std::size_t off = 0; off < n;) {
            const std::size_t chunk = n - off < 512 ? n - off : 512;
            for (std::size_t i = 0; i < chunk; ++i) {
                const std::size_t b = (off + i) * 2;
                const int16_t v = static_cast<int16_t>(pcm[b] | (pcm[b + 1] << 8));
                buf[i] = v * (1.0f / 32768.0f);
            }
            aaAudioRing.write(buf, chunk);
            off += chunk;
        }
    };
    carsh::aa::AaProjection aaProjection(aaHooks);
#ifdef CARSH_HAVE_OPENH264
    carsh::device::CircleH264Decoder aaDecoder(&aaProjection);
    if (aaDecoder.init())
        aaProjection.setDecoder(&aaDecoder);
    else
        mLogger.Write(FROM, LogError, "AA: openh264 decoder init failed");
#endif

    carsh::device::CircleAaDriver aaDriver;  // drives AA over uaa1 from this loop
    aaDriver.setProjection(&aaProjection);

    // AA tab: forward content-rect touches (already remapped to 800x480 source
    // coords by AndroidAutoScreen) to the phone as a press+release; tie the phone's
    // video focus to whether the AA tab is the active one.
    androidAuto.onTouch = [&](int sx, int sy, carsh::ui::GestureType) {
        const uint64_t ts = CTimer::GetClockTicks();
        aaProjection.forwardTouch(sx, sy, carsh::aa::TouchAction::Press, ts);
        aaProjection.forwardTouch(sx, sy, carsh::aa::TouchAction::Release, ts);
    };
    androidAuto.onFocusChanged = [&](bool focused) { aaProjection.setFocus(focused); };

    mLogger.Write(FROM, LogNotice, "entering UI loop");
    unsigned frame = 0;
    // Diagnostic software audio sink: no I2S DAC is wired yet, so nothing pulls
    // the engine — playback position would never advance. Each UI tick we consume
    // ~30 ms of audio here (decode -> ring -> engine DSP), which advances the
    // position and yields the output peak for the on-screen level meter. When a
    // DAC is connected, CircleSoundDevice becomes the sole ring consumer instead.
    std::vector<float> sinkBuf(std::size_t(kRate) / 20 * kChannels);  // 50 ms max
    float audioPeak = 0.0f;  // 0..1, peak-hold with per-frame decay
    for (;;) {
        if (haveTouch) {
            carsh::hal::TouchEvent ev;
            while (touch.poll(ev)) {
                // Diagnostics panel toggle: a touch-down in the top-right corner
                // (clear of the panel at top-left and the tab bar at the bottom).
                // Consume it so it does not reach the UI.
                if (ev.phase == carsh::hal::TouchPhase::Down &&
                    ev.x >= rt.width() - 60 && ev.y < 60) {
                    mDiagVisible = !mDiagVisible;
                    continue;
                }
                mgr.onTouch(ev);
            }
        }
        // Refresh now-playing from the player each frame.
        carsh::player::PlayerState ps = player.state();

        // Open / adapt the PWM output to the current track's sample rate, lazily.
        // The core has no resampler, so the device must clock at the track's native
        // rate (a fixed rate would shift pitch). Opening from closed is clean, and
        // close() drains the DMA so a rate-change reopen is safe; a single-rate
        // library opens once and never reopens.
        // When a phone is connected over A2DP, clock the output at the SBC stream
        // rate (typically 44.1 kHz); otherwise at the local track's rate. The output
        // device always renders g_outSink, which mixes BT audio over the local
        // engine. Reopen only on a rate change (close() drains the DMA safely).
        unsigned wantRate = curRate;
        bool wantOpen = false;
        if (bt.connected()) {
            uint32_t r = carsh::device::btGlueA2dpSampleRate();
            wantRate = r ? r : 44100;
            wantOpen = true;
        } else if (ps.queueIndex >= 0 && ps.queueIndex < int(ids.size())) {
            const carsh::library::Track* t =
                g_db.get(ids[std::size_t(ps.queueIndex)]);
            wantRate = (t && t->sampleRate) ? t->sampleRate : kRate;
            wantOpen = true;
        }
        if (wantOpen && (wantRate != curRate || !sound.isOpen())) {
            curRate = wantRate;
            if (sound.open({curRate, 16, kChannels}, &g_outSink))
                diag("audio: PWM jack @ %u Hz%s", unsigned(curRate),
                     bt.connected() ? " (BT)" : "");
            else
                diag("audio: PWM open FAILED");
        }

        // Output level for the meter. With a real device open, its DMA/IRQ
        // callback is the ring consumer, so we just read its peak (do NOT render
        // here — a second consumer would violate the ring's SPSC contract).
        // Without a device, fall back to the diagnostic software sink that pulls
        // ~one UI tick of audio so the pipeline still runs and position advances.
        if (sound.isOpen()) {
            audioPeak = std::max(sound.peak(), audioPeak * 0.80f);
        } else if (ps.transport == carsh::player::Transport::Playing) {
            std::size_t framesTick = std::size_t(kRate) * 30 / 1000;  // ~30 ms
            if (framesTick * kChannels > sinkBuf.size())
                framesTick = sinkBuf.size() / kChannels;
            std::size_t framesOut = g_engine.render(sinkBuf.data(), framesTick);
            float peak = 0.0f;
            for (std::size_t i = 0; i < framesOut * kChannels; ++i)
                peak = std::max(peak, std::fabs(sinkBuf[i]));
            audioPeak = std::max(peak, audioPeak * 0.80f);  // fast attack, slow decay
        } else {
            audioPeak *= 0.80f;
        }

        carsh::ui::NowPlayingScreen::State st;
        if (ps.queueIndex >= 0 && ps.queueIndex < int(ids.size())) {
            const carsh::library::Track* t = g_db.get(ids[std::size_t(ps.queueIndex)]);
            if (t) { st.title = g_db.title(*t); st.artist = g_db.artist(*t);
                     st.album = g_db.album(*t); }
        } else {
            st.title = "carsh"; st.artist = "select a track"; st.album = "";
        }
        st.positionMs = ps.positionMs;
        st.durationMs = ps.durationMs;
        st.playing = ps.transport == carsh::player::Transport::Playing;
        now.setState(st);

        // BT tab: reflect scan state + the discovered/connected device list. The
        // connected device is upserted into the cache so it always shows, even on
        // a passive auto-reconnect (its address may not be a scan result).
        devices.setScanning(bt.isScanning());
        {
            int connIdx = -1;
            if (bt.connected()) {
                carsh::bt::BdAddr ca = bt.connectedAddress();
                for (std::size_t i = 0; i < g_btDevices.size(); ++i)
                    if (g_btDevices[i].address == ca) { connIdx = int(i); break; }
                if (connIdx < 0) {
                    carsh::bt::DiscoveredDevice d; d.address = ca; d.name = "(connected)";
                    connIdx = int(g_btDevices.size());
                    g_btDevices.push_back(d);
                }
            }
            std::vector<std::string> rows;
            rows.reserve(g_btDevices.size());
            for (std::size_t i = 0; i < g_btDevices.size(); ++i)
                rows.push_back(carsh::bt::deviceRow(g_btDevices[i], int(i) == connIdx));
            devices.setDevices(rows, connIdx);
        }

        mgr.render(canvas);

        // Storage/USB diagnostics panel (toggle: tap the top-right corner).
        // Mirrors the boot/scan log (SD/USB mount, indexed file/track counts)
        // onto the display so storage issues are visible without a serial cable.
        if (mDiagVisible) {
            int dx = mgr.contentRect().x;  // clear of the left volume rail
            int n = int(mDiag.size());
            canvas.fillRect(dx, 0, 380, n * 10 + 4, 0xC8000000u);
            int y = 2;
            for (const std::string& l : mDiag) {
                canvas.drawText(dx + 2, y, l, 0xFF33FF66u, 0, 1);
                y += 10;
            }
        }
        // Toggle affordance: a small marker in the top-right corner (green when
        // the panel is shown, dim when hidden).
        canvas.fillRect(rt.width() - 14, 2, 12, 12,
                        mDiagVisible ? 0xFF33FF66u : 0x80FFFFFFu);

        // Output level meter (diagnostic): peak of the engine output this tick,
        // in the gap just above the tab bar. Green / amber / red near clip. The
        // underrun counter (u=) climbing means the decoder isn't feeding the ring
        // fast enough; 0 with a moving bar means the audio pipeline is healthy.
        {
            int my = mgr.contentRect().h - 16, mx = mgr.contentRect().x + 4,
                mw = 280, mh = 12;
            canvas.drawText(mx, my + 2, "OUT", 0xFFFFFFFFu, 0, 1);
            int bx = mx + 28;
            canvas.rectOutline(bx, my, mw, mh, 0xFF666666u);
            int fill = int(std::min(audioPeak, 1.0f) * float(mw - 2));
            uint32_t col = audioPeak > 0.9f ? 0xFFFF3030u
                         : audioPeak > 0.7f ? 0xFFFFD030u : 0xFF30FF30u;
            if (fill > 0) canvas.fillRect(bx + 1, my + 1, fill, mh - 2, col);
            char info[40];
            snprintf(info, sizeof info, "u=%lu", (unsigned long)ps.underrunFrames);
            canvas.drawText(bx + mw + 6, my + 2, info, 0xFFAAAAAAu, 0, 1);
        }

        rt.present();

        // AOA Rung 1 (continuous) — keep pumping USB plug-and-play so a slow-booting
        // dongle gets enumerated whenever it finally presents. The CA525 powers an
        // internal SoC and can take several seconds to bring up its USB interface —
        // well after the boot enumeration window — so a one-shot scan at boot misses
        // it. When it appears (or is hot-replugged), Circle logs "Device venXXXX
        // found" over USB-C. ~1 Hz is plenty and cheap.
        if (frame % 33 == 0) mUSBHCI.UpdatePlugAndPlay();

        // Drive the Android Auto connection over the live AOA transport (uaa1):
        // version exchange now, TLS next (AA_PLAN rung 4). UI-task driven so it never
        // races with the UpdatePlugAndPlay above (which deletes the transport when
        // the dongle re-cycles).
        aaDriver.tick();

        // Scheduler-aware sleep: yields to the decode task (and BT/net later)
        // between frames, ~30 fps. The decode task keeps the ring fed meanwhile.
        mScheduler.MsSleep(30);
        ++frame;
    }
    return ShutdownHalt;
}
