#include "ota/OtaManager.h"

namespace carsh::ota {

const char* applyStatusStr(ApplyStatus s) {
    switch (s) {
        case ApplyStatus::Ok: return "ok";
        case ApplyStatus::BadStructure: return "bad-structure";
        case ApplyStatus::BadSignature: return "bad-signature";
        case ApplyStatus::ImageHashMismatch: return "image-hash-mismatch";
        case ApplyStatus::PrevVersionTooOld: return "prev-version-too-old";
        case ApplyStatus::DowngradeBlocked: return "downgrade-blocked";
        case ApplyStatus::FlashWriteFailed: return "flash-write-failed";
    }
    return "unknown";
}

ApplyResult OtaManager::apply(const std::vector<uint8_t>& carup,
                              const BootRecord& current, uint32_t currentVersion,
                              uint8_t triesAllowed) {
    ApplyResult res;
    Package pkg;
    if (!parsePackage(carup, pkg)) {
        res.status = ApplyStatus::BadStructure;
        return res;
    }
    switch (verifyPackage(pkg, crypto_, pub_)) {
        case VerifyResult::Ok: break;
        case VerifyResult::BadSignature:
            res.status = ApplyStatus::BadSignature; return res;
        case VerifyResult::ImageHashMismatch:
            res.status = ApplyStatus::ImageHashMismatch; return res;
        case VerifyResult::BadStructure:
        default:
            res.status = ApplyStatus::BadStructure; return res;
    }

    // Version gates: require the running image meets the manifest's minimum, and
    // refuse to install an older version than what is running (anti-rollback).
    uint32_t newVer = parseVersion(pkg.manifest.version);
    uint32_t minPrev = parseVersion(pkg.manifest.minPrevVersion);
    if (!pkg.manifest.minPrevVersion.empty() && currentVersion < minPrev) {
        res.status = ApplyStatus::PrevVersionTooOld;
        return res;
    }
    if (newVer < currentVersion) {
        res.status = ApplyStatus::DowngradeBlocked;
        return res;
    }

    uint8_t slot = inactiveSlot(current);
    if (!flash_.writeSlotImage(slot, pkg.image, pkg.imageLen)) {
        res.status = ApplyStatus::FlashWriteFailed;
        return res;
    }

    res.status = ApplyStatus::Ok;
    res.writtenSlot = slot;
    res.newRecord = stageUpdate(current, newVer, triesAllowed);
    return res;
}

}  // namespace carsh::ota
