// CircleAaDriver — drives an AaConnection over the live AOA transport ("uaa1") from
// the UI loop. Bring-up advances version exchange (rung 4a, plaintext) into the
// real mbedTLS TLS handshake (rung 4b): the crypto is AaCryptoMbedTls, seeded from
// the Pi's hardware RNG. With the placeholder head-unit cert (AaHuCert.h) the phone
// rejects the handshake — swapping in the Google-registered aasdk cert is the [hw]
// step that lets it complete on the bench; the driver logs the handshake state and
// any mbedTLS error so that pass is observable on the serial console.
//
// Driven from the UI task (tick() each frame), NOT a separate task — so it never
// races with USB plug-and-play (also UI-task driven), which is what deletes the
// transport when the dongle re-cycles. It binds/rebinds "uaa1" across those cycles.
#ifndef _carsh_circle_aa_driver_h
#define _carsh_circle_aa_driver_h

#include <circle/bcmrandom.h>

#include "aa/AaConnection.h"
#include "aa/AaMessenger.h"
#include "aa/AaProjection.h"
#include "aa/IAaCrypto.h"

namespace carsh::device {

class CircleAaDriver {
public:
    ~CircleAaDriver();
    void tick();

    // Route the live connection into a projection (video/audio/touch/focus). When
    // set, its callbacks replace the logging fallback and it is (re)bound to each
    // new AaConnection across dongle cycles. Call once before the UI loop.
    void setProjection(carsh::aa::AaProjection* p) { projection_ = p; }

private:
    void teardown();

    // mbedTLS entropy callback — fills buf from the Pi's hardware RNG. ctx is the
    // CBcmRandomNumberGenerator below.
    static int rngEntropy(void* ctx, unsigned char* buf, size_t len);

    CBcmRandomNumberGenerator rng_;  // HW entropy source for the TLS DRBG seed
    carsh::aa::AaProjection* projection_ = nullptr;  // optional rung-6 wiring
    void* boundDev_ = nullptr;  // CAndroidAutoDevice* — compared only, never deref'd stale
    carsh::aa::IAaCrypto* crypto_ = nullptr;
    carsh::aa::AaMessenger* msger_ = nullptr;
    carsh::aa::AaConnection* conn_ = nullptr;
    int lastState_ = -1;
    unsigned ticks_ = 0;
};

}  // namespace carsh::device

#endif  // _carsh_circle_aa_driver_h
