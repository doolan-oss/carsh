// OtaPush — the device side of the serial OTA dev loop (PLAN.md §8.5): the
// CircleUsbLog task feeds host->device CDC bytes into a PushReceiver; the UI
// loop polls this service, and when a complete .carup arrives it is verified
// (Ed25519 + image SHA-256 via OtaManager), written to the inactive A/B slot,
// boot_slot.bin is re-armed for a trial, and the kernel reboots through the
// chainloader (device/chainloader/). A bad image exhausts its tries and the
// chainloader rolls back — the push loop cannot brick the card.
//
// Status is reported as "ota-push: ..." log lines on the same USB-C serial the
// host script (scripts/carsh_ota.py) is already reading; the strings here are
// its protocol, keep them in sync.
//
// Health confirmation is dev-loop pragmatic: reaching CKernel::Run() with
// storage up counts as healthy (confirmBootHealthy below). The user-confirmed
// flow for real end-user OTA (PLAN §8.5) stays separate in checkOta().
#ifndef _carsh_device_ota_push_h
#define _carsh_device_ota_push_h

#include <circle/logger.h>
#include <fatfs/ff.h>

#include <vector>

#include "CircleFlashTarget.h"
#include "DeviceCrypto.h"
#include "OtaPushKey.h"
#include "ota/BootSlots.h"
#include "ota/OtaManager.h"
#include "ota/PushProtocol.h"

namespace carsh::device {

class OtaPushService {
public:
    static constexpr const char* kRecordPath = "SD:/boot_slot.bin";
    static constexpr uint8_t kTriesAllowed = 3;

    carsh::ota::PushReceiver& receiver() { return rx_; }

    // Once at startup (storage mounted, core init done): log the running slot
    // and, if this boot is an armed A/B trial, mark it healthy — getting this
    // far IS the dev-loop health check. No record file = pre-chainloader boot
    // or fresh card; that's fine, apply() bootstraps one.
    void confirmBootHealthy(CLogger* log) {
        carsh::ota::BootRecord r;
        if (!loadRecord(r)) {
            log->Write(From, LogNotice,
                       "ota: no boot_slot.bin (pre-A/B boot) — pushes bootstrap it");
            return;
        }
        if (r.trying) {
            const carsh::ota::BootRecord n = carsh::ota::confirmHealthy(r);
            if (saveRecord(n))
                log->Write(From, LogNotice,
                           "ota: slot %c v%u trial CONFIRMED healthy",
                           'A' + n.activeSlot, n.activeVersion());
            else
                log->Write(From, LogError, "ota: confirm write failed");
            r = n;
        }
        log->Write(From, LogNotice, "ota: running slot %c v%u%s",
                   'A' + r.activeSlot, r.activeVersion(),
                   r.healthy ? " (healthy)" : "");
    }

    // Poll from the UI loop (cheap unless a transfer just finished). Returns
    // true when an update was applied and the caller should ShutdownReboot.
    bool poll(CLogger* log) {
        using RxState = carsh::ota::PushReceiver::State;
        const RxState s = rx_.state();
        if (s == lastState_ && s != RxState::Complete) return false;
        lastState_ = s;

        switch (s) {
            case RxState::Idle:
                return false;
            case RxState::Receiving:
                log->Write(From, LogNotice, "ota-push: receiving %u bytes",
                           unsigned(rx_.totalSize()));
                return false;
            case RxState::Failed:
                log->Write(From, LogWarning,
                           "ota-push: recv failed err=%s (resend to retry)",
                           carsh::ota::PushReceiver::errorStr(rx_.error()));
                rx_.reset();
                lastState_ = RxState::Idle;
                return false;
            case RxState::Complete:
                break;
        }

        const std::vector<uint8_t> carup = rx_.takeBlob();
        lastState_ = RxState::Idle;
        log->Write(From, LogNotice, "ota-push: recv ok (%u bytes) — verifying",
                   unsigned(carup.size()));

        bool keyed = false;
        for (uint8_t b : kOtaPushPubKey) keyed |= b != 0;
        if (!keyed) {
            log->Write(From, LogError,
                       "ota-push: push failed: no key provisioned "
                       "(scripts/carsh_ota.py keygen + rebuild the image)");
            return false;
        }

        carsh::ota::BootRecord current;
        if (!loadRecord(current)) {
            // Bootstrap: the running (pre-chainloader) image counts as a
            // healthy slot A; the push lands in B and the chainloader takes
            // over from the next boot (see device/chainloader/README.md).
            current = {};
            current.activeSlot = 0;
            current.healthy = true;
        }

        DeviceCrypto crypto;
        CircleFlashTarget flash("SD:");
        carsh::ota::OtaManager ota(crypto, kOtaPushPubKey, flash);
        const carsh::ota::ApplyResult res =
            ota.apply(carup, current, current.activeVersion(), kTriesAllowed);
        if (res.status != carsh::ota::ApplyStatus::Ok) {
            log->Write(From, LogError, "ota-push: apply failed: %s",
                       carsh::ota::applyStatusStr(res.status));
            return false;
        }
        if (!saveRecord(res.newRecord)) {
            log->Write(From, LogError,
                       "ota-push: apply failed: boot_slot.bin write");
            return false;
        }
        log->Write(From, LogNotice,
                   "ota-push: apply=ok slot=%c v%u tries=%u",
                   'A' + res.newRecord.activeSlot,
                   res.newRecord.activeVersion(), unsigned(kTriesAllowed));
        log->Write(From, LogNotice, "ota-push: rebooting");
        return true;
    }

private:
    static constexpr const char* From = "ota";

    bool loadRecord(carsh::ota::BootRecord& r) {
        FIL fil;
        if (f_open(&fil, kRecordPath, FA_READ) != FR_OK) return false;
        uint8_t buf[16];
        UINT br = 0;
        const FRESULT res = f_read(&fil, buf, sizeof buf, &br);
        f_close(&fil);
        return res == FR_OK && br == sizeof buf &&
               carsh::ota::deserializeBootRecord(buf, br, r);
    }

    bool saveRecord(const carsh::ota::BootRecord& r) {
        const std::vector<uint8_t> v = carsh::ota::serializeBootRecord(r);
        FIL fil;
        if (f_open(&fil, kRecordPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
            return false;
        UINT bw = 0;
        const FRESULT res = f_write(&fil, v.data(), UINT(v.size()), &bw);
        f_sync(&fil);
        f_close(&fil);
        return res == FR_OK && bw == v.size();
    }

    carsh::ota::PushReceiver rx_;
    carsh::ota::PushReceiver::State lastState_ =
        carsh::ota::PushReceiver::State::Idle;
};

}  // namespace carsh::device

#endif  // _carsh_device_ota_push_h
