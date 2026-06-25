// carsh device kernel. Brings up serial + SD/FAT + framebuffer + touch, builds
// the full app (library, player, EQ, profiles) from the same core the host tests
// cover, and runs the UI/input loop. Audio output and touch touch real hardware,
// so they are only activated on a board (not in the QEMU boot demo); everything
// is constructed here so the whole stack is built + linked bare-metal.
#include "kernel.h"

#include <circle/bcmframebuffer.h>

#include "CircleRenderTarget.h"
#include "CircleSoundDevice.h"
#include "CircleTouch.h"
#include "CircleVfs.h"
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
carsh::audio::PcmRing<float> g_ring(kRate * kChannels);  // ~1 s stereo
carsh::audio::AudioEngine g_engine(&g_ring, kChannels);
carsh::dsp::EqProcessor g_eq(kRate);
carsh::device::CircleSource g_sdSource(0, carsh::hal::SourceType::Sd, "SD:");
carsh::profile::ProfileManager g_profiles;
}  // namespace

CKernel::CKernel(void)
    : CStdlibApp("carsh"),
      mSerial(&mInterrupt),
      mTimer(&mInterrupt),
      mLogger(mOptions.GetLogLevel(), &mTimer),
      mEMMC(&mInterrupt, &mTimer, &mActLED) {}

bool CKernel::Initialize(void) {
    if (!CStdlibApp::Initialize()) return false;
    if (!mSerial.Initialize(115200)) return false;
    if (!mLogger.Initialize(&mSerial)) return false;
    if (!mTimer.Initialize()) return false;

    if (mEMMC.Initialize() && f_mount(&mFileSystem, "SD:", 1) == FR_OK) {
        mSdMounted = true;
        mLogger.Write(FROM, LogNotice, "SD mounted (FAT)");
    } else {
        mLogger.Write(FROM, LogWarning, "no SD/FAT — running without media");
    }
    return true;
}

void CKernel::scanLibrary(void) {
    if (!mSdMounted) return;
    carsh::library::Indexer idx(g_db);
    idx.beginScan(g_sdSource);
    mLogger.Write(FROM, LogNotice, "indexing %u files...",
                  unsigned(idx.progress().filesTotal));
    while (idx.step(8)) { /* CScheduler::Yield() once the decode task is wired */ }
    mLogger.Write(FROM, LogNotice, "library: %u tracks, %u albums",
                  unsigned(g_db.trackCount()), unsigned(g_db.albums().size()));
}

CStdlibApp::TShutdownMode CKernel::Run(void) {
    mLogger.Write(FROM, LogNotice, "carsh boot — bare-metal automotive head unit");

    // Wire the audio DSP chain (EQ inserted into the engine). Playback is started
    // on demand via the player on hardware; the I2S device is not opened here.
    g_engine.setSampleProcessor(&g_eq);
    g_engine.setMasterVolume(0.8f);
    carsh::player::PlayerController player(g_sdSource.vfs(), g_ring, g_engine,
                                           kChannels);

    scanLibrary();

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
    bool haveTouch = touch.initialize();
    mLogger.Write(FROM, LogNotice, "touch: %s", haveTouch ? "ready" : "absent");

    mLogger.Write(FROM, LogNotice, "entering UI loop");
    unsigned frame = 0;
    for (;;) {
        if (haveTouch) {
            carsh::hal::TouchEvent ev;
            while (touch.poll(ev)) mgr.onTouch(ev);  // driver self-updates
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
        rt.present();

        mActLED.On();
        mTimer.MsDelay(15);
        mActLED.Off();
        mTimer.MsDelay(15);   // ~30 fps; the decode task will share this loop
        ++frame;
    }
    return ShutdownHalt;
}
