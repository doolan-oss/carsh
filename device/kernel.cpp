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

#include "CircleBluetooth.h"
#include "CircleBtRunLoopTask.h"
#include "CircleDecodeTask.h"
#include "CircleFlashTarget.h"
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
carsh::device::CircleSource g_sdSource(0, carsh::hal::SourceType::Sd, "SD:");
carsh::device::CircleSource g_usbSource(1, carsh::hal::SourceType::Usb, "USB:");
carsh::profile::ProfileManager g_profiles;
carsh::bt::AvrcpMetadata g_btMeta;  // latest AVRCP now-playing (BT source)
}  // namespace

CKernel::CKernel(void)
    : CStdlibApp("carsh"),
      mSerial(&mInterrupt),
      mTimer(&mInterrupt),
      mLogger(mOptions.GetLogLevel(), &mTimer),
      mEMMC(&mInterrupt, &mTimer, &mActLED),
      mUSBHCI(&mInterrupt, &mTimer, TRUE) {}

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
    g_engine.setMasterVolume(0.8f);

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
    mgr.addScreen(&now, "Now");
    mgr.addScreen(&library, "Lib");
    mgr.addScreen(&search, "Find");
    mgr.addScreen(&queue, "Q");
    mgr.addScreen(&eq, "EQ");
    mgr.addScreen(&sources, "Src");
    mgr.addScreen(&settings, "Set");

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
    bt.setCallbacks(btcb);
    if (bt.init()) {
        new carsh::device::CircleBtRunLoopTask();
        mLogger.Write(FROM, LogNotice, "Bluetooth: ready (A2DP/AVRCP)");
    } else {
        mLogger.Write(FROM, LogNotice,
                      "Bluetooth: not built (define CARSH_HAVE_BTSTACK + add BTstack)");
    }

    // Spawn the cooperative decode task (sole ring producer). The scheduler owns
    // it; it runs whenever this (UI) task yields via MsSleep below.
    new carsh::device::CircleDecodeTask(&player);

    mLogger.Write(FROM, LogNotice, "entering UI loop");
    unsigned frame = 0;
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

        mgr.render(canvas);

        // Storage/USB diagnostics panel (toggle: tap the top-right corner).
        // Mirrors the boot/scan log (SD/USB mount, indexed file/track counts)
        // onto the display so storage issues are visible without a serial cable.
        if (mDiagVisible) {
            int n = int(mDiag.size());
            canvas.fillRect(0, 0, 380, n * 10 + 4, 0xC8000000u);
            int y = 2;
            for (const std::string& l : mDiag) {
                canvas.drawText(2, y, l, 0xFF33FF66u, 0, 1);
                y += 10;
            }
        }
        // Toggle affordance: a small marker in the top-right corner (green when
        // the panel is shown, dim when hidden).
        canvas.fillRect(rt.width() - 14, 2, 12, 12,
                        mDiagVisible ? 0xFF33FF66u : 0x80FFFFFFu);

        rt.present();

        // Scheduler-aware sleep: yields to the decode task (and BT/net later)
        // between frames, ~30 fps. The decode task keeps the ring fed meanwhile.
        mScheduler.MsSleep(30);
        ++frame;
    }
    return ShutdownHalt;
}
