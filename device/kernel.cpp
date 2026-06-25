// carsh device kernel implementation. Brings up serial logging + the framebuffer,
// then renders the real carsh UI (the same ui/ toolkit proven on the host) to the
// panel — the first visible proof the STL-based core runs on the target.
#include "kernel.h"

#include <circle/bcmframebuffer.h>

#include "CircleRenderTarget.h"
#include "ui/Canvas.h"
#include "ui/Screen.h"
#include "ui/Screens.h"

#define FROM "carsh"

CKernel::CKernel(void)
    : CStdlibApp("carsh"),
      mSerial(&mInterrupt),
      mTimer(&mInterrupt),
      mLogger(mOptions.GetLogLevel(), &mTimer) {}

bool CKernel::Initialize(void) {
    if (!CStdlibApp::Initialize()) return false;
    if (!mSerial.Initialize(115200)) return false;
    if (!mLogger.Initialize(&mSerial)) return false;
    if (!mTimer.Initialize()) return false;
    return true;
}

CStdlibApp::TShutdownMode CKernel::Run(void) {
    mLogger.Write(FROM, LogNotice, "carsh boot — bare-metal automotive head unit");

    // Framebuffer at the official DSI panel's native resolution.
    CBcmFrameBuffer fb(800, 480, 32);
    if (!fb.Initialize()) {
        mLogger.Write(FROM, LogError, "framebuffer init failed");
        return ShutdownHalt;
    }
    mLogger.Write(FROM, LogNotice, "framebuffer %ux%u pitch=%u",
                  fb.GetWidth(), fb.GetHeight(), fb.GetPitch());

    carsh::device::CircleRenderTarget rt(&fb);
    carsh::ui::Canvas canvas(rt);

    // Build the screen stack (same toolkit + screens as the host tests).
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

    carsh::ui::NowPlayingScreen::State st;
    st.title = "carsh online";
    st.artist = "bare-metal head unit";
    st.album = "Step 2 bring-up";
    st.positionMs = 23000;
    st.durationMs = 222000;
    st.playing = true;
    now.setState(st);

    mgr.render(canvas);
    rt.present();
    mLogger.Write(FROM, LogNotice, "rendered carsh UI frame (%dx%d)", rt.width(),
                  rt.height());

    // Idle: heartbeat on the ACT LED so a live board is visible at a glance.
    for (;;) {
        mActLED.On();
        mTimer.MsDelay(250);
        mActLED.Off();
        mTimer.MsDelay(750);
    }
    return ShutdownHalt;
}
