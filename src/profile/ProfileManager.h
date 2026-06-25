// ProfileManager (PLAN.md §2.11) — maps a connected phone's Classic BT address
// to a driver profile, falling back to a guest profile for unknown devices.
// On connect it makes that profile active; consumers (DSP/UI/controller) read
// active() to apply EQ/theme/source. Persistence is via each Profile's
// serialize(); the manager holds the in-RAM set keyed by address.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "profile/Profile.h"

namespace carsh::profile {

class ProfileManager {
public:
    ProfileManager() {
        guest_.flags |= ProfFlagGuest;
        guest_.displayName = "Guest";
        active_ = &guest_;
    }

    // Insert or replace a known profile.
    void upsert(const Profile& p) {
        profiles_[p.addr.key()] = p;
    }

    bool has(const BdAddr& addr) const {
        return profiles_.count(addr.key()) != 0;
    }

    // On phone connect: activate the matching profile, or clone guest defaults
    // for a new device (caller can rename + save to persist). Returns active.
    Profile& onBtConnected(const BdAddr& addr) {
        auto it = profiles_.find(addr.key());
        if (it != profiles_.end()) {
            active_ = &it->second;
        } else {
            Profile np = guest_;
            np.flags &= ~ProfFlagGuest;  // a real (new) profile
            np.addr = addr;
            np.displayName = addr.str();
            auto res = profiles_.emplace(addr.key(), np);
            active_ = &res.first->second;
            createdOnConnect_ = true;
        }
        return *active_;
    }

    void onBtDisconnected() { active_ = &guest_; }

    Profile& active() { return *active_; }
    const Profile& active() const { return *active_; }
    Profile& guest() { return guest_; }

    // True if the last onBtConnected created a brand-new profile (UI may prompt
    // "remember this device?"). Cleared by acknowledge().
    bool wasCreatedOnConnect() const { return createdOnConnect_; }
    void acknowledgeCreate() { createdOnConnect_ = false; }

    Profile* find(const BdAddr& addr) {
        auto it = profiles_.find(addr.key());
        return it == profiles_.end() ? nullptr : &it->second;
    }

    std::size_t count() const { return profiles_.size(); }

    // Persistence helpers: caller writes/reads each blob to <bdaddr>.prof.
    std::vector<std::pair<std::string, std::vector<uint8_t>>> serializeAll()
        const {
        std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
        for (auto& kv : profiles_)
            out.push_back({kv.first + ".prof", kv.second.serialize()});
        out.push_back({"guest.prof", guest_.serialize()});
        return out;
    }
    bool loadProfile(const uint8_t* data, std::size_t len) {
        Profile p;
        if (!Profile::deserialize(data, len, p)) return false;
        if (p.isGuest())
            guest_ = p;
        else
            profiles_[p.addr.key()] = p;
        return true;
    }

private:
    std::map<std::string, Profile> profiles_;
    Profile guest_;
    Profile* active_ = nullptr;
    bool createdOnConnect_ = false;
};

}  // namespace carsh::profile
