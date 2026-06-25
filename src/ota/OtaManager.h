// OtaManager (PLAN.md §2.12) — orchestrates verify -> version-gate -> write
// inactive slot -> arm A/B trial. The actual flash + reboot are delegated to
// IFlashTarget and the chainloader (BootSlots); everything here is host-testable.
#pragma once

#include <string>
#include <vector>

#include "ota/BootSlots.h"
#include "ota/Crypto.h"
#include "ota/IFlashTarget.h"
#include "ota/Package.h"

namespace carsh::ota {

enum class ApplyStatus {
    Ok,
    BadStructure,
    BadSignature,
    ImageHashMismatch,
    PrevVersionTooOld,   // current < manifest.minPrevVersion
    DowngradeBlocked,    // manifest.version < current
    FlashWriteFailed,
};

struct ApplyResult {
    ApplyStatus status = ApplyStatus::BadStructure;
    BootRecord newRecord;   // valid only when status == Ok
    uint8_t writtenSlot = 0xFF;
    bool ok() const { return status == ApplyStatus::Ok; }
};

const char* applyStatusStr(ApplyStatus s);

class OtaManager {
public:
    OtaManager(ICrypto& crypto, const Ed25519PublicKey& pub,
               IFlashTarget& flash)
        : crypto_(crypto), pub_(pub), flash_(flash) {}

    // Verify a package and, if good, write it to the inactive slot and arm a
    // trial. Does not reboot. `current` is the live boot record; currentVersion
    // is the running image's version (parseVersion form).
    ApplyResult apply(const std::vector<uint8_t>& carup,
                      const BootRecord& current, uint32_t currentVersion,
                      uint8_t triesAllowed = 3);

    // Post-boot health confirmation (commit) and explicit rollback.
    BootRecord commit(const BootRecord& r) { return confirmHealthy(r); }
    BootRecord rollbackTo(const BootRecord& r) { return rollback(r); }

private:
    ICrypto& crypto_;
    Ed25519PublicKey pub_;
    IFlashTarget& flash_;
};

}  // namespace carsh::ota
