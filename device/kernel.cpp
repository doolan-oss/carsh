// carsh device kernel. Brings up serial + SD/FAT + framebuffer + touch, builds
// the full app (library, player, EQ, profiles) from the same core the host tests
// cover, and runs the UI/input loop. Audio output and touch touch real hardware,
// so they are only activated on a board (not in the QEMU boot demo); everything
// is constructed here so the whole stack is built + linked bare-metal.
#include "kernel.h"

#include <circle/bcmframebuffer.h>
#include <circle/devicenameservice.h>
#include <circle/startup.h>      // reboot() for the OTA-push restart
#include <circle/synchronize.h>  // EnableIRQs() for the panic-flush handler
#include <circle/timer.h>        // CTimer::GetClockTicks() for the flush spin

#include <cstdarg>
#include <cstdio>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <vector>

#include "CircleBluetooth.h"
#include "bt/DeviceRow.h"
#include "CircleBtRunLoopTask.h"
#include "CircleDecodeTask.h"
#include "CircleAaDecodeTask.h"
#include "CircleAaDecodeCore.h"
#include "CircleAaDriver.h"
#include "CircleAoaTransport.h"
#include "aa/AaProjection.h"
#include "aa/AsyncH264Decoder.h"
#include "aa/FrameRelay.h"
#include "audio/JitterBuffer.h"
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
#include "OtaPush.h"
#include "ota/OtaManager.h"
#include "ota/Package.h"
#include "audio/AudioEngine.h"
#include "audio/PcmRing.h"
#include "dsp/Eq.h"
#include "dsp/GraphicEq.h"
#include "library/Database.h"
#include "library/Indexer.h"
#include "library/LibraryBrowser.h"
#include "library/LibraryCache.h"
#include "library/ArtSource.h"
#include "player/PlayerController.h"
#include "player/MediaSession.h"
#include "profile/ProfileManager.h"
#include "ui/Canvas.h"
#include "ui/Screen.h"
#include "ui/AndroidAutoScreen.h"
#include "ui/LibraryScreen.h"
#include "ui/Screens.h"

#define FROM "carsh"

namespace {
// --- Panic flush over USB-C -------------------------------------------------
// On an assert()/exception, Circle writes a LogPanic line + stack trace to the
// current log target (here the USB-C CDC serial gadget) and then spins in for(;;).
// But the gadget's TX is driven by the dwc2 USB IRQ, and the fault often fires
// INSIDE the xHCI ISR (our bulk-IN completion runs in that context) with IRQs
// masked — so the queued bytes never reach /dev/ttyACM0 and the board just goes
// silent. Registered via CLogger::RegisterPanicHandler(), this runs right after the
// panic text is queued: re-enable IRQs so the dwc2 TX can fire, then spin-pump the
// gadget for a few seconds so the whole panic + backtrace flush to the host before
// the halt. (We are dying anyway, so re-enabling IRQs mid-ISR is acceptable.)
CUSBCDCGadget* g_panicGadget = nullptr;

// Serial OTA push (scripts/carsh_ota.py): the CircleUsbLog task feeds inbound
// USB-C bytes into its receiver; the UI loop polls it and reboots on apply.
carsh::device::OtaPushService g_otaPush;
void carshPanicFlush(void) {
    if (g_panicGadget == nullptr) return;
    EnableIRQs();
    const unsigned start = CTimer::GetClockTicks();  // free-running 1 MHz counter
    while (CTimer::GetClockTicks() - start < 4u * 1000000u) {  // ~4 s
        g_panicGadget->UpdatePlugAndPlay();
    }
}

// Long-lived app state at file scope: the PcmRing's buffer is large (~1 s of
// audio) and must not sit on the stack, and these persist across the run loop.
constexpr unsigned kRate = 48000;
constexpr uint8_t kChannels = 2;

carsh::library::Database g_db;
// Track ids of the CURRENT player queue, in queue order. The library browser builds
// queues from arbitrary scopes (album / artist / shuffled / whole library), so the
// now-playing display must resolve metadata via this — NOT via a fixed allTracks()
// list, whose order no longer matches the queue.
std::vector<uint64_t> g_queueIds;
carsh::audio::PcmRing<float> g_ring(kRate * kChannels);  // ~1 s stereo (local)
carsh::audio::PcmRing<float> g_btRing(kRate * kChannels);  // A2DP jitter+decode
// Android Auto media audio (ch=4, 48 kHz stereo PCM from the phone). Filled by the
// AA projection's playAudio hook; consumed by the PWM sink below, same as A2DP.
carsh::audio::PcmRing<float> g_aaRing(kRate * kChannels);  // ~1 s stereo
// Decoded AA video frames handed from the decode core (producer) to the UI core
// (consumer) — lock-free triple buffer, so core 3 never writes the frame core 0 is
// blitting. (With the cooperative-task fallback it's just a same-core handoff.)
carsh::aa::FrameRelay g_aaFrames;

// AA media-audio prebuffer / underrun handler over g_aaRing (the choppy-audio fix).
// AA audio arrives over the dongle's wireless link in BURSTS (bench-measured: clumps
// of frames then 200-600 ms gaps), so g_aaRing needs a big cushion. kAaPrimeMs is the
// startup cushion; kAaResumeMs is the much smaller threshold to resume after a
// transient gap (so a 300 ms gap costs ~300 ms, not 300 ms + a full re-prime). Raise
// kAaPrimeMs if dropouts persist; lower it if AA audio lags video too much.
constexpr unsigned kAaPrimeMs = 480;
constexpr unsigned kAaResumeMs = 60;
carsh::audio::JitterBuffer g_aaJitter(
    g_aaRing, std::size_t(kRate) * kChannels * kAaPrimeMs / 1000,
    std::size_t(kRate) * kChannels * kAaResumeMs / 1000);

carsh::audio::AudioEngine g_engine(&g_ring, kChannels);
carsh::dsp::EqProcessor g_eq(kRate);

// Head-unit master output gain (dB-mapped, from the left volume rail). Applied to AA
// and A2DP here; the local player engine applies the SAME value internally via
// setMasterVolume, so all three sources track the one HU volume control. Android Auto
// has no phone-side volume (unlike A2DP/AVRCP), so the HU gain is the only control.
float g_masterGain = 0.0f;

// True while Bluetooth owns the media session (MediaSession::source()), published
// by the UI loop for the PWM IRQ below. A2DP arrives in chunks, so the BT ring
// routinely holds less than one output block mid-stream; while BT owns the session
// that must be silence, never a block of the local engine spliced into the phone
// audio (review P4).
std::atomic<bool> g_btOwnsOutput{false};
// Running count of A2DP PCM chunks delivered (BT run-loop task). The UI loop turns it
// into "the phone is streaming" (player::StreamActivity) — ring occupancy can't tell,
// a stale sub-block tail would pin it on.
std::atomic<uint32_t> g_btPcmChunks{0};

// PWM output sink. Source priority: Android Auto audio > A2DP > the local player
// engine (in practice only one is active; MediaSession pauses the local player when
// a phone takes over). AA and BT are raw PCM, so the HU master gain is applied here;
// the local engine already applied it internally. The DMA/IRQ callback calls
// render(); each ring is an SPSC consumer here.
class BtMixSink : public carsh::hal::IAudioSink {
public:
    std::size_t render(float* out, std::size_t frames) override {
        const std::size_t want = frames * kChannels;
        // Android Auto (highest priority) via the jitter buffer: it prebuffers, owns
        // the output while a stream is live (emitting silence on a short read rather
        // than dropping to the engine and clicking), and returns false only when no
        // AA stream is engaged — then A2DP / the local engine take over below.
        if (g_aaJitter.pull(out, want, g_masterGain)) return frames;
        // A2DP: a full block plays whenever one is buffered (a stream that starts
        // before the UI loop hands BT the session). While BT OWNS the session, a
        // short read plays what there is and pads with silence (it also drains the
        // sub-block tail a stopped stream leaves behind).
        const std::size_t avail = g_btRing.readable();
        if (avail >= want || g_btOwnsOutput.load(std::memory_order_relaxed)) {
            std::size_t n = avail < want ? avail : want;
            n -= n % kChannels;  // whole frames only: never split an L/R pair
            const std::size_t got = g_btRing.read(out, n);
            for (std::size_t i = 0; i < got; ++i) out[i] *= g_masterGain;
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
// An AVRCP report (playback status or track attribute) arrived since the current
// phone connected: MediaSession then takes the BT play/pause state from AVRCP
// alone, instead of from A2DP activity. Both are reset per connection, so a
// reconnect never shows (or acts on) the previous connection's status. Written
// by the BT run-loop task, read by the UI loop: cooperative tasks on core 0.
bool g_btAvrcpReported = false;
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
        new carsh::device::CircleUsbLog(&mSerialCDC, &mLogger, &mSerial,
                                        &g_otaPush.receiver());
        // Let an assert/exception panic flush its message over USB-C before halting
        // (the normal pump task is dead by then — see carshPanicFlush).
        g_panicGadget = &mSerialCDC;
        CLogger::Get()->RegisterPanicHandler(carshPanicFlush);
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
    g_engine.setMasterVolume(0.0f);  // safe default until the rail is seeded (~30%) below

    // Prefer a USB stick for media; fall back to the SD card. The player must
    // read through the SAME source we index, so logical track paths ("/Music/..")
    // resolve against the right FatFs drive.
    carsh::hal::Source& media =
        mUsbMounted ? static_cast<carsh::hal::Source&>(g_usbSource)
                    : static_cast<carsh::hal::Source&>(g_sdSource);
    diag("media source: %s", mUsbMounted ? "USB:" : "SD:");

    carsh::player::PlayerController player(media.vfs(), g_ring, g_engine,
                                           kChannels);

    // Library: load the cached DB if the media tree is unchanged (fast boot), else
    // scan and write the cache to the SD card for next time. The signature is over
    // the MEDIA source; the cache lives on the writable SD boot card.
    {
        const char* kDbPath = "/carsh/library.db";
        const char* kSigPath = "/carsh/library.sig";
        const uint64_t sig =
            carsh::library::librarySignature(media.vfs(), media.root());
        const bool loaded =
            mSdMounted && carsh::library::tryLoadLibrary(g_sdSource.vfs(), kDbPath,
                                                         kSigPath, sig, g_db);
        if (loaded) {
            diag("library: %u tracks (cache hit)", unsigned(g_db.trackCount()));
        } else {
            scanLibrary(media);
            if (mSdMounted && carsh::library::saveLibrary(g_sdSource.vfs(), kDbPath,
                                                          kSigPath, sig, g_db))
                diag("library: cache saved");
        }
    }
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
    carsh::ui::LibraryScreen library(mgr.contentRect());
    carsh::ui::SearchScreen search(mgr.contentRect());
    carsh::ui::QueueScreen queue(mgr.contentRect());
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

    // Left volume rail -> the one HU master gain, applied to the local engine AND to
    // AA/A2DP (in BtMixSink::render). v is the dB-mapped gain from the rail.
    mgr.setOnVolumeChange([&](float v) {
        g_masterGain = v;
        g_engine.setMasterVolume(v);
    });
    // Boot at 30% on the rail (was muted) so audio is audible without a touch input
    // to raise it — useful for HDMI bring-up; seed the master gain to match.
    mgr.setVolume(0.30f);
    g_masterGain = mgr.volumeGain();
    g_engine.setMasterVolume(g_masterGain);

    // Library tab: hierarchical browse (Artists -> Albums -> Songs) with play /
    // shuffle scoped to the current level (whole library / one artist / one album).
    // The browser is the platform-independent model (host-tested); the screen just
    // renders its rows + forwards intents back into it.
    carsh::library::LibraryBrowser browser(g_db);
    browser.setSeed(CTimer::GetClockTicks() | 1u);  // vary the shuffle per boot

    // Bumped whenever the queue CONTENTS change (not on a within-queue jump), so the
    // Queue tab only rebuilds its rows when needed rather than every frame.
    unsigned queueVersion = 0;

    // Media session: arbitrates Now Playing between the local player, the phone over
    // A2DP+AVRCP and Android Auto, and tells the transport buttons which backend to
    // drive. Fed a fresh snapshot each frame in the UI loop below.
    carsh::player::MediaSession session;
    // Every EXPLICIT local start (a library pick, PLAY/SHUFFLE, a queue row, '>' /
    // next / prev on a local track) that actually started playback calls this: local
    // takes the session back from a latched phone, and a phone still streaming is
    // paused (a source switch pauses the source it leaves). Assigned once the AA
    // projection + BT exist (below).
    std::function<void()> takeLocalSession;

    // Build a player queue from a set of track ids and start playing. Keeps
    // g_queueIds in lockstep with the queue so now-playing can resolve metadata.
    auto startQueue = [&](const carsh::library::LibraryBrowser::PlayRequest& req) {
        if (!req.valid()) return;
        std::vector<std::string> paths;
        paths.reserve(req.trackIds.size());
        for (uint64_t id : req.trackIds) {
            const carsh::library::Track* t = g_db.get(id);
            paths.push_back(t ? g_db.path(*t) : std::string());
        }
        g_queueIds = req.trackIds;
        ++queueVersion;
        player.setQueue(std::move(paths));
        // starts decode; audio device opened on hardware
        if (player.play(req.startIndex) && takeLocalSession) takeLocalSession();
    };

    // Push the browser's current level into the screen (title, rows, BACK visibility,
    // scope-aware PLAY/SHUFFLE captions).
    auto refreshLibrary = [&]() {
        library.setTitle(browser.title());
        if (browser.rows().empty())
            library.setItems({"(insert FAT32 media with music)"});
        else
            library.setItems(browser.rows());
        library.setCanGoBack(browser.canGoBack());
        const std::string sc = browser.scope();
        library.setPlayLabels("PLAY " + sc, "SHUFFLE " + sc);
    };

    library.onSelectRow = [&](int i) {
        if (auto req = browser.select(i)) startQueue(*req);  // a song -> play it
        else refreshLibrary();                              // drilled a level
    };
    library.onBack = [&] { browser.back(); refreshLibrary(); };
    library.onPlay = [&] { startQueue(browser.playAll(false)); };
    library.onShuffle = [&] { startQueue(browser.playAll(true)); };
    library.onJump = [&](char ch) {
        const int i = browser.firstRowForLetter(ch);
        if (i >= 0) library.scrollTo(i);  // jump the list to that letter
    };
    refreshLibrary();

    // Queue tab: tap a row to jump within the current queue; CLEAR stops + empties it.
    queue.onSelect = [&](int i) {
        if (player.play(i) && takeLocalSession) takeLocalSession();
    };
    queue.onClear = [&] {
        player.stop();
        player.clearQueue();
        g_queueIds.clear();
        ++queueVersion;
    };

    // EQ: seed the band layout into BOTH the EQ tab and the engine EQ before the UI
    // runs — the active driver profile's curve if it has one, else the default
    // 10-band graphic EQ (flat, so audio is unchanged until the user edits). Without
    // this the tab was blank and took no taps, and g_eq stayed bypassed (review P5).
    // Re-applied in the UI loop when a BT connect/disconnect switches the profile.
    auto applyEq = [&](const carsh::dsp::EqConfig& cfg) {
        eq.setConfig(cfg);
        g_eq.setConfig(cfg);
    };
    const carsh::profile::Profile* eqProfile = &g_profiles.active();
    applyEq(carsh::dsp::eqForProfile(eqProfile->eq));
    // EQ screen edits feed the live engine EQ, and the curve follows the driver
    // profile (in RAM; profiles aren't persisted yet).
    eq.onBandChange = [&](int, double) {
        g_eq.setConfig(eq.config());
        g_profiles.active().eq = eq.config();
    };
    // (Now Playing transport buttons are wired after Bluetooth is set up below, so
    // they can route play/pause/next/prev to AVRCP when the phone is the source.)

    // Touch (real hardware only; QEMU has no digitizer). fb supplies the coordinate
    // mapping; mTimer's 100 Hz periodic tick pumps the driver from the timer IRQ,
    // and that IRQ reads fb through the driver's display pointer. Circle can't
    // unregister the handler, so ~CircleTouch turns it into a no-op. Keep `touch` a
    // LOCAL declared AFTER fb: locals are destroyed in reverse order, so if Run()
    // ever unwinds, the handler is neutralised before fb (and the driver itself)
    // go away. (A static would never run that destructor and leave the IRQ reading
    // a dead fb.)
    carsh::device::CircleTouch touch;
    bool haveTouch = touch.initialize(&fb, &mTimer);
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
        g_btMeta = carsh::bt::AvrcpMetadata{};  // a new phone reports afresh
        g_btAvrcpReported = false;
        carsh::profile::BdAddr pa; pa.b = a;
        g_profiles.onBtConnected(pa);
    };
    btcb.onDisconnected = [](const carsh::bt::BdAddr&) {
        // The glue does not publish an empty metadata when AVRCP goes away, so drop
        // the last status here: a stale PLAYING would otherwise read as "streaming"
        // and take the session on the next connect.
        g_btMeta = carsh::bt::AvrcpMetadata{};
        g_btAvrcpReported = false;
        g_profiles.onBtDisconnected();
    };
    btcb.onMetadata = [](const carsh::bt::AvrcpMetadata& m) {
        g_btMeta = m;
        g_btAvrcpReported = true;
    };
    btcb.onA2dpPcm = [](const float* p, std::size_t f, uint8_t ch) {
        g_btRing.write(p, f * ch);
        g_btPcmChunks.fetch_add(1, std::memory_order_relaxed);
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

    // (The media session is declared above, with the library wiring. Now Playing
    // transport buttons are wired after the AA projection is set up below, so they
    // can also route play/pause/next/prev to Android Auto.)

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
    // Index of the "AA" tab (must match the addScreen order above: it is the 9th /
    // last screen). Used to auto-show projection without a touch input.
    const int aaTabIndex = 8;
    bool aaAutoShown = false;  // latched when we auto-switch to the AA tab on first frame
    // Set by the decode core, consumed by the UI core -> must be atomic.
    std::atomic<bool> aaAutoShowReq{false};
    carsh::aa::AaProjection::Hooks aaHooks;
    aaHooks.showFrame = [&](const uint8_t* rgb, int w, int h) {
        if (rgb != nullptr && w > 0 && h > 0) {
            // Decode side (dedicated core 3, or the cooperative task): hand the frame
            // to the UI core via the lock-free triple buffer. NEVER touch the screen
            // or the transport from here — the UI core owns AndroidAutoScreen + the
            // framebuffer blit, and VIDEO_FOCUS writes (the auto-show) must stay on it.
            g_aaFrames.publish(rgb, w, h);
            aaAutoShowReq.store(true, std::memory_order_relaxed);
        } else {
            // reset()/clear (UI task): drop to the placeholder.
            androidAuto.setFrame(nullptr, 0, 0);
        }
    };
    aaHooks.setConnected = [&](bool up) {
        androidAuto.setConnected(up);
        // On disconnect (the phone's shutdown, or AaProjection::disconnect() from the
        // driver's teardown on an unplug / re-cycle), drop the latch so the next
        // session auto-shows again, and leave the AA tab if it is showing so the
        // display isn't stuck on a dead projection screen. (Another tab the user is
        // on is left alone.)
        if (!up) {
            aaAutoShown = false;
            aaAutoShowReq.store(false, std::memory_order_relaxed);
            if (mgr.activeIndex() == aaTabIndex) mgr.setActive(0);
        }
    };
    // AA media PCM chunks received (UI task: the AA driver ticks from the UI loop).
    // The UI loop turns it into "AA media is playing" for the media session.
    uint32_t aaMediaChunks = 0;
    aaHooks.playAudio = [&](carsh::aa::ChannelId ch, const uint8_t* pcm, std::size_t len) {
        if (ch == carsh::aa::ChannelId::MediaAudio) ++aaMediaChunks;
        // 16-bit LE PCM -> float, into the global AA ring that the PWM sink drains to
        // the jack (the only AA audio channel opened is MediaAudio = 48 kHz stereo,
        // matching the output, so no rate/format conversion is needed).
        const std::size_t n = len / 2;
        float buf[512];
        for (std::size_t off = 0; off < n;) {
            const std::size_t chunk = n - off < 512 ? n - off : 512;
            for (std::size_t i = 0; i < chunk; ++i) {
                const std::size_t b = (off + i) * 2;
                const int16_t v = static_cast<int16_t>(pcm[b] | (pcm[b + 1] << 8));
                buf[i] = v * (1.0f / 32768.0f);
            }
            g_aaRing.write(buf, chunk);
            off += chunk;
        }
    };
    carsh::aa::AaProjection aaProjection(aaHooks);
#ifdef CARSH_HAVE_OPENH264
    carsh::device::CircleH264Decoder aaDecoder(&aaProjection);
    // Decouple decode from the AA receive loop: the projection feeds encoded AUs to
    // this async wrapper (a cheap enqueue), and a dedicated cooperative task pumps
    // openh264 on its own schedule, so a keyframe decode no longer stalls the loop
    // that refills g_aaRing (AA_AUDIO_INVESTIGATION fix #3). ~512 KB of AU headroom
    // (several keyframes) absorbs a brief decode backlog without dropping AUs.
    carsh::aa::AsyncH264Decoder aaAsync(&aaDecoder, 512 * 1024);
    carsh::device::AaDecodeStats aaDecodeStats;
    if (aaDecoder.init()) {
        aaProjection.setDecoder(&aaAsync);
#ifdef CARSH_AA_SKIP_DECODE
        // A/B instrumentation (investigation step 3): pop AUs but skip the actual
        // openh264 decode. If audio goes smooth with this defined, decode-on-the-
        // refill-path is confirmed as the choppiness cause. Build:
        //   make CARSH_HAVE_OPENH264=1 CPPFLAGS=-DCARSH_AA_SKIP_DECODE
        aaAsync.setDecodeEnabled(false);
        mLogger.Write(FROM, LogWarning, "AA: DECODE SKIPPED (A/B audio test build)");
#endif
#ifdef CARSH_AA_DECODE_CORE
        // Run decode on a dedicated A72 core so it can't starve the core-0 audio
        // refill (the bench-measured cause of choppy AA audio). Requires Circle's
        // ARM_ALLOW_MULTI_CORE (scripts/patches/sysconfig-multicore.patch). Frames
        // reach the UI core via g_aaFrames (FrameRelay). static: it must outlive Run.
        static carsh::device::CircleAaDecodeCore s_decodeCore(
            CMemorySystem::Get(), &aaAsync, &aaDecodeStats);
        if (s_decodeCore.Initialize())
            mLogger.Write(FROM, LogNotice, "AA: H.264 decode on dedicated core %u",
                          carsh::device::CircleAaDecodeCore::kDecodeCore);
        else
            mLogger.Write(FROM, LogError, "AA: decode core init FAILED");
#else
        // Single-core fallback (build CARSH_NO_DECODE_CORE=1): cooperative decode task.
        new carsh::device::CircleAaDecodeTask(&aaAsync, &aaDecodeStats);
        mLogger.Write(FROM, LogNotice, "AA: H.264 decode on cooperative task (core 0)");
#endif
    } else {
        mLogger.Write(FROM, LogError, "AA: openh264 decoder init failed");
    }
#endif

    carsh::device::CircleAaDriver aaDriver;  // drives AA over uaa1 from this loop
    aaDriver.setProjection(&aaProjection);

    // AA input-event timestamps: a continuous 64-bit microsecond clock
    // (CTimer::GetClockTicks64 — the 32-bit GetClockTicks wraps every ~71.6 min,
    // review T10), clamped monotonic across touch + keys. A touch carries its CAPTURE
    // time from the 100 Hz driver pump (touchStampUs, set while the UI loop
    // dispatches that event), so a drag drained in a burst still reaches the phone
    // with its real spacing — the phone derives fling velocity from it.
    uint64_t touchStampUs = 0;  // capture time of the touch being dispatched, else 0
    uint64_t lastAaInputUs = 0;
    auto aaInputStamp = [&](uint64_t us) {
        if (us < lastAaInputUs) us = lastAaInputUs;
        lastAaInputUs = us;
        return us;
    };
    auto sendAaKey = [&](carsh::aa::MediaKey key) {
        aaProjection.sendMediaKey(key, aaInputStamp(CTimer::GetClockTicks64()));
    };

    // AA tab: stream the raw contact on the video to the phone (review T3). The
    // screen claims a Down inside the video (via ScreenManager's raw-touch path, so
    // it bypasses the tap/swipe recognizer) and reports PRESS, a DRAG per move
    // (clamped to the video) and RELEASE at the last point, already remapped to
    // 800x480 source coords — so scrolling, map panning and long-press work. Tab-bar
    // taps and the volume rail never reach this path. Tie the phone's video focus
    // to whether the AA tab is the active one.
    androidAuto.onTouchAction = [&](carsh::aa::TouchAction action, int sx, int sy) {
        const uint64_t ts = touchStampUs ? touchStampUs : CTimer::GetClockTicks64();
        aaProjection.forwardTouch(sx, sy, action, aaInputStamp(ts));
    };
    androidAuto.onFocusChanged = [&](bool focused) { aaProjection.setFocus(focused); };

    takeLocalSession = [&] {
        const carsh::player::MediaSession::PauseRequest r = session.selectLocal();
        if (r.androidAuto) sendAaKey(carsh::aa::MediaKey::Pause);
        if (r.bluetooth) bt.sendAvrcpCommand(carsh::bt::AvrcpCommand::Pause);
        g_btOwnsOutput.store(false, std::memory_order_relaxed);  // local plays now
    };

    // Now Playing transport buttons -> the session owner (MediaSession latches the
    // last phone that played, so a paused AA/BT source keeps the buttons). Android
    // Auto: media keys over the AA input channel; Bluetooth: AVRCP; local: the file
    // player. Wired here so the lambdas can reach aaProjection + bt.
    now.onPlayPause = [&] {
        switch (session.transportTarget()) {
            case carsh::player::Source::AndroidAuto:
                sendAaKey(carsh::aa::MediaKey::PlayPause);
                break;
            case carsh::player::Source::Bluetooth:
                bt.sendAvrcpCommand(session.playPauseCommand());
                break;
            case carsh::player::Source::Local:
                if (player.state().transport == carsh::player::Transport::Playing) {
                    player.pause();
                } else {
                    player.resume();
                    if (player.state().transport == carsh::player::Transport::Playing)
                        takeLocalSession();
                }
                break;
        }
    };
    now.onNext = [&] {
        switch (session.transportTarget()) {
            case carsh::player::Source::AndroidAuto:
                sendAaKey(carsh::aa::MediaKey::Next); break;
            case carsh::player::Source::Bluetooth:
                bt.sendAvrcpCommand(carsh::bt::AvrcpCommand::Next); break;
            case carsh::player::Source::Local:
                if (player.next()) takeLocalSession();
                break;
        }
    };
    now.onPrev = [&] {
        switch (session.transportTarget()) {
            case carsh::player::Source::AndroidAuto:
                sendAaKey(carsh::aa::MediaKey::Previous); break;
            case carsh::player::Source::Bluetooth:
                bt.sendAvrcpCommand(carsh::bt::AvrcpCommand::Prev); break;
            case carsh::player::Source::Local:
                if (player.prev()) takeLocalSession();
                break;
        }
    };

    // A/B boot record: log the running slot; if this boot is an armed OTA
    // trial, getting here (storage + UI init all succeeded) confirms it.
    if (mSdMounted) g_otaPush.confirmBootHealthy(&mLogger);

    mLogger.Write(FROM, LogNotice, "entering UI loop");
    unsigned frame = 0;
    // AA tick() latency window (microseconds), reset each ~1 Hz health log. Long
    // ticks lining up with rising AA underruns is the signature of the prime
    // hypothesis (decode starving the audio refill) — see AA_AUDIO_INVESTIGATION.
    unsigned aaTickMaxUs = 0, aaTickSumUs = 0, aaTickCount = 0;
    // render() time window (us). render() is the one stretch with NO audio refill, so
    // its peak ~= the worst gap g_aaRing faces — the suspected source of the remaining
    // AA stutter (the 720p video blit on core 0, + memory contention with core 3).
    unsigned renderMaxUs = 0, renderSumUs = 0, renderCount = 0;
    unsigned lastQueueVersion = ~0u;  // forces the Queue tab to build its rows once
    // Album art (local source): sidecar covers rendered to ASCII, cached by albumKey
    // and refreshed only when the now-playing album changes (decode is not free).
    std::unordered_map<uint64_t, carsh::library::AsciiArt> artCache;
    uint64_t lastArtAlbumKey = ~0ull;
    // Diagnostic software audio sink: no I2S DAC is wired yet, so nothing pulls
    // the engine — playback position would never advance. Each UI tick we consume
    // ~30 ms of audio here (decode -> ring -> engine DSP), which advances the
    // position and yields the output peak for the on-screen level meter. When a
    // DAC is connected, CircleSoundDevice becomes the sole ring consumer instead.
    std::vector<float> sinkBuf(std::size_t(kRate) / 20 * kChannels);  // 50 ms max
    float audioPeak = 0.0f;  // 0..1, peak-hold with per-frame decay
    // "Is it streaming?" for the media session: a phone is playing while PCM chunks
    // keep arriving (held ~0.8 s to ride out bursty delivery), not while a ring
    // merely holds a leftover tail.
    carsh::player::StreamActivity btActivity(800), aaActivity(800);
    uint32_t lastTouchDrops = 0;  // for the ~1 Hz health log

    // Diagnostics toggle: a touch-down on the small marker drawn in the top-right
    // corner (12x12 at x=786,y=2), within a 20x20 hot zone. It used to consume every
    // Down in the whole 60x60 corner, which swallowed real widgets: the 'A' slot of
    // the Library A-Z bar could never be tapped (review T7). The zone is clear of
    // every control but the far right end of a list's first row. Not on the AA tab:
    // there the corner is the phone's UI, so the marker and panel are hidden.
    constexpr int kDiagHotW = 20, kDiagHotH = 20;

    // Drain the touch events the 100 Hz timer pump has queued (review T4) into the
    // UI. Called at the top of each frame AND between the AA refill ticks below, so a
    // drag reaches the phone every ~10 ms instead of in one burst per rendered frame.
    auto drainTouch = [&] {
        if (!haveTouch) return;
        carsh::hal::TouchEvent ev;
        while (touch.poll(ev)) {
            if (ev.phase == carsh::hal::TouchPhase::Down &&
                mgr.activeIndex() != aaTabIndex && ev.x >= rt.width() - kDiagHotW &&
                ev.y < kDiagHotH) {
                mDiagVisible = !mDiagVisible;
                continue;  // consumed: the rest of this contact is ignored by the UI
            }
            touchStampUs = touch.lastEventUs();  // AA input timestamp (capture time)
            mgr.onTouch(ev);
            touchStampUs = 0;
        }
        // The ring was completely full and lost a Down/Up. Moves stop queueing at
        // 112 events, so this takes a UI stall long enough for that Move backlog
        // (~1.1 s of one finger's drag at 100 Hz) PLUS 16 more Down/Up events (8+
        // taps, or several fingers) before the drain. Rare, but a lost Up would
        // leave ScreenManager's primary-contact latch waiting for it forever,
        // ignoring every later touch: reset the touch routing (cancels the contact
        // in progress like a tab switch — an AA press gets its Release — and drops
        // the latch) so the next Down starts cleanly. Checked after the drain, so
        // every event queued before the loss was delivered first.
        if (touch.lostTransitionSinceLastCheck()) {
            mgr.resetTouch();
            mLogger.Write(FROM, LogWarning,
                          "touch: ring lost a Down/Up (%u total); input reset",
                          unsigned(touch.droppedTransitions()));
        }
    };

    for (;;) {
        // Serial OTA push: when a signed image has been received + staged to the
        // inactive slot, reboot into the chainloader trial. (Cheap state poll.)
        // Reboot from HERE rather than returning ShutdownReboot (review A11):
        // returning would destroy Run()'s locals (the AA decoder + AU queue, the
        // projection and its hooks, the player) while the decode core (core 3) and
        // the cooperative decode tasks still use them, and a heap corrupted mid-
        // shutdown halted the board instead of booting the new slot. A watchdog
        // reset needs no unwinding. Sleep briefly first so the USB-C log task flushes
        // "ota-push: rebooting" to carsh_ota.py (files are already synced + closed).
        if (mSdMounted && g_otaPush.poll(&mLogger)) {
            mScheduler.MsSleep(500);
            reboot();
        }

        drainTouch();
        // Pull the latest decoded video frame from the decode core (UI core owns the
        // blit). Only setFrame on a new frame so AndroidAutoScreen keeps pointing at
        // the consumer-owned triple-buffer slot (stable until the next acquire). A
        // frame left in the relay when the session ended is consumed and dropped, so
        // it can't re-freeze the AA tab after the disconnect cleared it.
        {
            const uint8_t* frgb = nullptr;
            int fw = 0, fh = 0;
            if (g_aaFrames.acquire(frgb, fw, fh) && aaProjection.connected())
                androidAuto.setFrame(frgb, fw, fh);
        }
        // Perform a decode-requested auto-show on the UI task, so the resulting
        // VIDEO_FOCUS goes out the AA transport from the UI thread (not the decode
        // core). Latched so it only yanks focus once per connection, and only while
        // a session is up (a late frame after a shutdown must not pull the user back
        // to a dead AA tab).
        if (aaAutoShowReq.exchange(false, std::memory_order_relaxed) && !aaAutoShown &&
            aaProjection.connected()) {
            mgr.setActive(aaTabIndex);
            aaAutoShown = true;
        }
        // A BT connect/disconnect switched the driver profile: run its EQ curve.
        if (&g_profiles.active() != eqProfile) {
            eqProfile = &g_profiles.active();
            applyEq(carsh::dsp::eqForProfile(eqProfile->eq));
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
        if (g_aaRing.readable() > 0) {
            // Android Auto media audio is streaming (48 kHz stereo PCM) — open the
            // jack at that rate so the PWM sink drains g_aaRing. Highest priority:
            // while projecting, AA owns the output.
            wantRate = kRate;
            wantOpen = true;
        } else if (bt.connected()) {
            uint32_t r = carsh::device::btGlueA2dpSampleRate();
            wantRate = r ? r : 44100;
            wantOpen = true;
        } else if (ps.queueIndex >= 0 && ps.queueIndex < int(g_queueIds.size())) {
            // The rate the player is actually producing (PlayerState::sampleRate:
            // the heard decoder's native rate, which changes on a non-gapless
            // restart at a new rate), so the jack reclocks from the true rate even
            // if the library row says 0 or is stale. The row's rate is only a
            // fallback, then 48 kHz.
            uint32_t r = ps.sampleRate;
            if (r == 0) {
                const carsh::library::Track* t =
                    g_db.get(g_queueIds[std::size_t(ps.queueIndex)]);
                if (t) r = t->sampleRate;
            }
            wantRate = r ? r : kRate;
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

        // Now Playing: arbitrate the local player vs the Bluetooth phone and show
        // whichever currently owns the output. The local snapshot resolves metadata
        // via the actual queue (g_queueIds); the BT side comes from AVRCP (g_btMeta).
        carsh::player::MediaSession::LocalState ls;
        if (ps.queueIndex >= 0 && ps.queueIndex < int(g_queueIds.size())) {
            const carsh::library::Track* t =
                g_db.get(g_queueIds[std::size_t(ps.queueIndex)]);
            if (t) { ls.hasTrack = true; ls.title = g_db.title(*t);
                     ls.artist = g_db.artist(*t); ls.album = g_db.album(*t); }
        }
        ls.playing = ps.transport == carsh::player::Transport::Playing;
        ls.positionMs = ps.positionMs;
        ls.durationMs = ps.durationMs;
        session.setLocal(ls);
        // A phone that STARTS playing takes the session and keeps it while paused
        // (review A2), until the user explicitly starts local playback again
        // (takeLocalSession) or it disconnects — see MediaSession.
        // BT is "streaming" while A2DP PCM keeps arriving or AVRCP says PLAYING (that
        // decides who owns the session). Its '>' / '||' and the play/pause command
        // follow AVRCP once it has reported on this connection: a paused phone keeps
        // streaming PCM for a while (the 800 ms hold + its own silence tail).
        const uint64_t nowUs = CTimer::GetClockTicks64();
        const bool btStreaming =
            btActivity.update(g_btPcmChunks.load(std::memory_order_relaxed), nowUs) ||
            g_btMeta.playing;
        session.setBluetooth(bt.connected(), btStreaming, g_btMeta, g_btAvrcpReported);
        // Android Auto: a projection session is up, and its media audio is playing
        // (media PCM arriving, or the jitter buffer still playing it out). Ownership
        // no longer ends when the jitter buffer disengages on a pause.
        const bool aaStreaming = aaActivity.update(aaMediaChunks, nowUs) ||
                                 g_aaJitter.engaged();
        session.setAndroidAuto(aaProjection.connected(), aaStreaming);
        // A phone just took over while the local player was playing underneath it:
        // pause the local player (review P4). Only an explicit local action resumes
        // it — pausing the phone must never start the USB track by itself.
        if (session.shouldPauseLocal()) {
            player.pause();
            mLogger.Write(FROM, LogNotice, "audio: local player paused (%s took over)",
                          session.source() == carsh::player::Source::AndroidAuto
                              ? "Android Auto" : "Bluetooth");
        }
        g_btOwnsOutput.store(session.source() == carsh::player::Source::Bluetooth,
                             std::memory_order_relaxed);

        const carsh::player::MediaSession::NowPlayingView v = session.view();
        carsh::ui::NowPlayingScreen::State st;
        st.title = v.title;
        st.artist = v.artist;
        st.album = v.album;
        st.positionMs = v.positionMs;
        st.durationMs = v.durationMs;
        st.playing = v.playing;
        st.source = v.source == carsh::player::Source::AndroidAuto ? "ANDROID AUTO"
                  : v.source == carsh::player::Source::Bluetooth   ? "BLUETOOTH"
                                                                   : "";
        now.setState(st);

        // Queue tab: rebuild rows only when the queue contents change; mark the
        // now-playing row every frame (cheap). Only meaningful for the local source.
        if (queueVersion != lastQueueVersion) {
            lastQueueVersion = queueVersion;
            std::vector<std::string> qrows;
            qrows.reserve(g_queueIds.size());
            for (uint64_t id : g_queueIds) {
                const carsh::library::Track* t = g_db.get(id);
                qrows.push_back(t ? g_db.artist(*t) + " - " + g_db.title(*t)
                                  : std::string("?"));
            }
            queue.setItems(std::move(qrows));
        }
        queue.setCurrent(v.source == carsh::player::Source::Local ? ps.queueIndex : -1);

        // Now Playing album art (local source only): update on album change, cached.
        if (v.source == carsh::player::Source::Local && ps.queueIndex >= 0 &&
            ps.queueIndex < int(g_queueIds.size())) {
            const carsh::library::Track* t =
                g_db.get(g_queueIds[std::size_t(ps.queueIndex)]);
            const uint64_t ak = t ? t->albumKey : 0;
            if (ak != lastArtAlbumKey) {
                lastArtAlbumKey = ak;
                auto it = artCache.find(ak);
                if (it == artCache.end()) {
                    carsh::library::AsciiArt a;  // rows==0 => looked, none found
                    if (t) carsh::library::loadSidecarArt(media.vfs(), g_db.path(*t), a);
                    it = artCache.emplace(ak, a).first;
                }
                if (it->second.rows > 0) now.setArt(it->second);
                else now.clearArt();
            }
        } else if (lastArtAlbumKey != ~0ull) {
            now.clearArt();        // BT/AA (or stopped): no local art
            lastArtAlbumKey = ~0ull;  // re-evaluate when local playback resumes
        }

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

        {
            const unsigned t0 = CTimer::GetClockTicks();
            mgr.render(canvas);
            const unsigned dt = CTimer::GetClockTicks() - t0;
            if (dt > renderMaxUs) renderMaxUs = dt;
            renderSumUs += dt;
            ++renderCount;
        }

        // Storage/USB diagnostics panel (toggle: tap the top-right marker).
        // Mirrors the boot/scan log (SD/USB mount, indexed file/track counts)
        // onto the display so storage issues are visible without a serial cable.
        // Hidden on the AA tab, where it would cover the phone's UI.
        const bool diagAllowed = mgr.activeIndex() != aaTabIndex;
        if (mDiagVisible && diagAllowed) {
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
        // the panel is shown, dim when hidden), inside the kDiagHotW x kDiagHotH
        // hot zone the touch drain checks.
        if (diagAllowed)
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
            // u = local-engine underrun frames; au = AA media-audio underruns (jitter
            // buffer). au climbing during projection == choppy AA audio.
            char info[56];
            snprintf(info, sizeof info, "u=%lu au=%lu", (unsigned long)ps.underrunFrames,
                     (unsigned long)g_aaJitter.underruns());
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

        // Drive AA + refill g_aaRing MULTIPLE times across the frame interval, not
        // once. With decode now on core 3, the core-0 limiter is the ~44 ms video
        // blit in render(); refilling audio only once per ~74 ms loop lets the ring
        // dip below the prebuffer on jitter -> underruns. Interleaving cheap ticks
        // (poll() drains USB -> g_aaRing; no decode here anymore) with short sleeps
        // keeps the ring topped across the blit. UI-task driven so it never races
        // with the UpdatePlugAndPlay above. Each tick timed for the health log below.
        auto aaTick = [&] {
            const unsigned t0 = CTimer::GetClockTicks();  // 1 MHz free-running (us)
            aaDriver.tick();
            const unsigned dt = CTimer::GetClockTicks() - t0;
            if (dt > aaTickMaxUs) aaTickMaxUs = dt;
            aaTickSumUs += dt;
            ++aaTickCount;
        };
        aaTick();                          // refill right after the render
        for (int i = 0; i < 3; ++i) {      // ~30 ms total (replaces one MsSleep(30)),
            mScheduler.MsSleep(10);        // refilling ~every 10 ms so the ring
            aaTick();                      // survives the next frame's blit
            drainTouch();                  // + stream AA drags at the pump's pace
        }

        // AA audio/decode health, ~1 Hz. Underruns climbing while tick/decode spikes
        // appear is the prime-hypothesis signature; underruns staying at 0 with the
        // ring occupancy holding near the prime level means the path is healthy.
        // (Reads counters from the UI task — never logs from the PWM IRQ context.)
        if (frame % 33 == 0) {
            const unsigned tickAvg = aaTickCount ? aaTickSumUs / aaTickCount : 0;
            const unsigned renderAvg = renderCount ? renderSumUs / renderCount : 0;
            mLogger.Write(FROM, LogNotice,
                          "aa-audio: ring=%u/%u prime=%u eng=%d under=%lu primes=%lu "
                          "tick(us) max=%u avg=%u render(us) max=%u avg=%u",
                          unsigned(g_aaJitter.occupancy()), unsigned(g_aaRing.capacity()),
                          unsigned(g_aaJitter.primeElements()), g_aaJitter.engaged() ? 1 : 0,
                          (unsigned long)g_aaJitter.underruns(),
                          (unsigned long)g_aaJitter.primeEvents(), aaTickMaxUs, tickAvg,
                          renderMaxUs, renderAvg);
#ifdef CARSH_HAVE_OPENH264
            unsigned decMax = 0, decAvg = 0, decN = 0;
            aaDecodeStats.snapshot(decMax, decAvg, decN);
            mLogger.Write(FROM, LogNotice,
                          "aa-decode: n=%u dec(us) max=%u avg=%u q=%u pushed=%lu "
                          "dropped=%lu",
                          decN, decMax, decAvg, unsigned(aaAsync.queue().pending()),
                          (unsigned long)aaAsync.queue().pushed(),
                          (unsigned long)aaAsync.queue().dropped());
#endif
            // Touch events the ring shed under a backlog (Moves first). Should stay
            // 0: the UI drains ~4x per frame and the ring holds > 1 s of Moves.
            if (haveTouch && touch.dropped() != lastTouchDrops) {
                lastTouchDrops = touch.dropped();
                mLogger.Write(FROM, LogWarning, "touch: ring dropped %u event(s) total",
                              unsigned(lastTouchDrops));
            }
            aaTickMaxUs = aaTickSumUs = aaTickCount = 0;
            renderMaxUs = renderSumUs = renderCount = 0;
        }

        // (The ~30 ms inter-frame sleep is now spread across the aaTick() refills
        // above, so AA audio is topped up several times per frame instead of once.)
        ++frame;
    }
    return ShutdownHalt;
}
