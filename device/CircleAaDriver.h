// CircleAaDriver — drives an AaConnection over the live AOA transport ("uaa1") from
// the UI loop. Bring-up advances version exchange (rung 4a, plaintext) into the
// real mbedTLS TLS handshake (rung 4b): the crypto is AaCryptoMbedTls, seeded from
// the Pi's hardware RNG, with the Google-registered aasdk cert (AaHuCert.h); the
// driver logs the handshake state and any mbedTLS error on the serial console.
//
// Driven from the UI task (tick() each frame), NOT a separate task — so it never
// races with USB plug-and-play (also UI-task driven), which is what deletes the
// transport when the dongle re-cycles. It binds/rebinds "uaa1" across those cycles.
//
// Session lifecycle (review A3/A4/A7/A9, contract C2): a session ends when the
// transport's link latches dead (any bulk-IN/OUT fault) or the connection reaches
// AaState::Shutdown (phone SHUTDOWN_REQUEST, version reject, fatal TLS error), or
// when the transport itself disappears. Every end calls AaProjection::disconnect()
// (screen back to the placeholder, auto-show latch re-armed) before the connection
// is freed. A phone Exit (AaShutdownReason::PhoneRequest) stops there: nothing is
// forced, so projection does not relaunch behind the user's back — the dongle
// re-cycles by itself when the phone leaves, or the user replugs it. After a fault,
// if the dongle doesn't re-cycle by itself within a grace period, the driver forces
// a USB re-enumeration (aoaForceReenumerate), bounded by AaLinkSupervisor's retry
// budget; when that is spent it logs "AA link lost — replug dongle" and waits for a
// replug.
#ifndef _carsh_circle_aa_driver_h
#define _carsh_circle_aa_driver_h

#include <circle/bcmrandom.h>
#include <circle/device.h>

#include "aa/AaConnection.h"
#include "aa/AaLinkSupervisor.h"
#include "aa/AaMessenger.h"
#include "aa/AaProjection.h"
#include "aa/IAaCrypto.h"

namespace carsh::device {

class AaCryptoMbedTls;     // typed handle for TLS bring-up diagnostics (lastError)
class CAndroidAutoDevice;  // the uaa1 transport (CircleAoaTransport.h)

class CircleAaDriver {
public:
    ~CircleAaDriver();
    void tick();

    // Route the live connection into a projection (video/audio/touch/focus). When
    // set, its callbacks replace the logging fallback and it is (re)bound to each
    // new AaConnection across dongle cycles. Call once before the UI loop; the
    // projection must outlive the driver.
    void setProjection(carsh::aa::AaProjection* p) { projection_ = p; }

private:
    void bind(CAndroidAutoDevice* aa);
    // Free the session (connection, messenger, crypto). notify: run the projection's
    // disconnect() first (UI reset); false only from the destructor.
    void endSession(bool notify);
    // Stop watching the bound transport without it having been destroyed.
    void unwatchTransport();
    void forceReenumerate();
    void logLinkFault();

    // CDevice removed-handler on the bound transport: runs inside Circle's device
    // teardown (UpdatePlugAndPlay, or our own forced removal) — task context.
    static void transportRemoved(CDevice* pDevice, void* pContext);

    // mbedTLS entropy callback — fills buf from the Pi's hardware RNG. ctx is the
    // CBcmRandomNumberGenerator below.
    static int rngEntropy(void* ctx, unsigned char* buf, size_t len);

    CBcmRandomNumberGenerator rng_;  // HW entropy source for the TLS DRBG seed
    carsh::aa::AaProjection* projection_ = nullptr;  // optional rung-6 wiring
    // The bound transport. Never dangling: the removed-handler clears it the moment
    // Circle destroys the device, so it is safe to dereference whenever non-null.
    CAndroidAutoDevice* bound_ = nullptr;
    CDevice::TRegistrationHandle removedHandle_ = nullptr;
    bool transportGone_ = false;  // set by the removed-handler, consumed by tick()
    carsh::aa::IAaCrypto* crypto_ = nullptr;
    AaCryptoMbedTls* mbed_ = nullptr;  // == crypto_, typed, for lastError() telemetry
    carsh::aa::AaMessenger* msger_ = nullptr;
    carsh::aa::AaConnection* conn_ = nullptr;
    carsh::aa::AaLinkSupervisor sup_;
    bool faultLogged_ = false;  // the bound link's fault was reported
    int lastState_ = -1;
    unsigned ticks_ = 0;
};

}  // namespace carsh::device

#endif  // _carsh_circle_aa_driver_h
