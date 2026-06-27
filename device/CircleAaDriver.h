// CircleAaDriver — drives an AaConnection over the live AOA transport ("uaa1") from
// the UI loop. AA_PLAN rung 4a: the plaintext VERSION_REQUEST/RESPONSE exchange,
// which proves the bulk path is bidirectional and stabilises the dongle (it stops
// resetting once the head unit responds). TLS is stubbed with a logging crypto, so
// the connection advances to SslHandshake (= version exchange OK) and stops there;
// the real mbedTLS IAaCrypto is the next rung.
//
// Driven from the UI task (tick() each frame), NOT a separate task — so it never
// races with USB plug-and-play (also UI-task driven), which is what deletes the
// transport when the dongle re-cycles. It binds/rebinds "uaa1" across those cycles.
#ifndef _carsh_circle_aa_driver_h
#define _carsh_circle_aa_driver_h

#include "aa/AaConnection.h"
#include "aa/AaMessenger.h"
#include "aa/IAaCrypto.h"

namespace carsh::device {

class CircleAaDriver {
public:
    ~CircleAaDriver();
    void tick();

private:
    void teardown();
    void* boundDev_ = nullptr;  // CAndroidAutoDevice* — compared only, never deref'd stale
    carsh::aa::IAaCrypto* crypto_ = nullptr;
    carsh::aa::AaMessenger* msger_ = nullptr;
    carsh::aa::AaConnection* conn_ = nullptr;
    int lastState_ = -1;
    unsigned ticks_ = 0;
};

}  // namespace carsh::device

#endif  // _carsh_circle_aa_driver_h
