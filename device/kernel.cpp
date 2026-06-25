// carsh device kernel implementation. Brings up serial + SD/FAT + framebuffer,
// indexes the SD into the library with the same core the host tests cover, and
// renders the carsh UI to the panel.
#include "kernel.h"

#include <circle/bcmframebuffer.h>

#include "CircleRenderTarget.h"
#include "CircleVfs.h"
#include "library/Database.h"
#include "library/Indexer.h"
#include "ui/Canvas.h"
#include "ui/Screen.h"
#include "ui/Screens.h"

#define FROM "carsh"

// Long-lived app state. Kept at file scope so the framebuffer render and the
// (future) input/audio loop can share it without a huge stack frame.
static carsh::library::Database s_db;

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

    // SD card + FAT. Non-fatal: the UI still runs without media.
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
    carsh::device::CircleSource src(0, carsh::hal::SourceType::Sd, "SD:");
    carsh::library::Indexer idx(s_db);
    idx.beginScan(src);
    mLogger.Write(FROM, LogNotice, "indexing %u files...",
                  unsigned(idx.progress().filesTotal));
    // Chunked scan (the cooperative slice the scheduler task will drive later).
    while (idx.step(8)) { /* Yield() once the scheduler is wired */ }
    mLogger.Write(FROM, LogNotice, "library: %u tracks, %u albums",
                  unsigned(s_db.trackCount()), unsigned(s_db.albums().size()));
}

void CKernel::renderHome(void) {
    CBcmFrameBuffer fb(800, 480, 32);
    if (!fb.Initialize()) {
        mLogger.Write(FROM, LogError, "framebuffer init failed");
        return;
    }
    mLogger.Write(FROM, LogNotice, "framebuffer %ux%u pitch=%u", fb.GetWidth(),
                  fb.GetHeight(), fb.GetPitch());

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

    // Populate the library browse list from the scan.
    std::vector<std::string> rows;
    for (uint64_t id : s_db.allTracks()) {
        const carsh::library::Track* t = s_db.get(id);
        if (t) rows.push_back(s_db.artist(*t) + " - " + s_db.title(*t));
    }
    if (rows.empty()) rows.push_back("(insert a FAT32 USB/SD with music)");
    library.setItems(rows);

    carsh::ui::NowPlayingScreen::State st;
    st.title = "carsh online";
    st.artist = "bare-metal head unit";
    st.album = mSdMounted ? "SD mounted" : "no media";
    st.durationMs = 222000;
    st.playing = false;
    now.setState(st);

    mgr.render(canvas);
    rt.present();
    mLogger.Write(FROM, LogNotice, "rendered carsh UI (%dx%d)", rt.width(),
                  rt.height());
}

CStdlibApp::TShutdownMode CKernel::Run(void) {
    mLogger.Write(FROM, LogNotice, "carsh boot — bare-metal automotive head unit");
    scanLibrary();
    renderHome();

    for (;;) {  // heartbeat; the cooperative task loop lands here next milestone
        mActLED.On();
        mTimer.MsDelay(250);
        mActLED.Off();
        mTimer.MsDelay(750);
    }
    return ShutdownHalt;
}
