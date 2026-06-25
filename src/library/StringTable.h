// StringTable — deduplicating string store (PLAN.md §4.1 strings.dat).
//
// Interns artist/album/title/path strings to small ids so TrackRecords stay
// fixed-size and repeated names (every track on an album shares an artist) cost
// one copy. id 0 is always the empty string. Serializes to a length-prefixed
// blob for persistence (the on-disk strings.dat).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace carsh::library {

class StringTable {
public:
    StringTable() { intern(""); }  // id 0 == empty

    uint32_t intern(const std::string& s) {
        auto it = index_.find(s);
        if (it != index_.end()) return it->second;
        uint32_t id = uint32_t(strings_.size());
        strings_.push_back(s);
        index_.emplace(s, id);
        return id;
    }

    const std::string& get(uint32_t id) const {
        static const std::string empty;
        return id < strings_.size() ? strings_[id] : empty;
    }

    std::size_t size() const { return strings_.size(); }

    // Serialize: [u32 count] then per string [u32 len][bytes].
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out;
        putU32(out, uint32_t(strings_.size()));
        for (auto& s : strings_) {
            putU32(out, uint32_t(s.size()));
            out.insert(out.end(), s.begin(), s.end());
        }
        return out;
    }

    bool deserialize(const uint8_t* data, std::size_t len) {
        strings_.clear();
        index_.clear();
        std::size_t p = 0;
        uint32_t count;
        if (!getU32(data, len, p, count)) return false;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t n;
            if (!getU32(data, len, p, n)) return false;
            if (p + n > len) return false;
            std::string s(reinterpret_cast<const char*>(data + p), n);
            p += n;
            index_.emplace(s, uint32_t(strings_.size()));
            strings_.push_back(std::move(s));
        }
        return true;
    }

private:
    static void putU32(std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
        v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 24));
    }
    static bool getU32(const uint8_t* d, std::size_t len, std::size_t& p,
                       uint32_t& out) {
        if (p + 4 > len) return false;
        out = uint32_t(d[p]) | (uint32_t(d[p + 1]) << 8) |
              (uint32_t(d[p + 2]) << 16) | (uint32_t(d[p + 3]) << 24);
        p += 4;
        return true;
    }

    std::vector<std::string> strings_;
    std::unordered_map<std::string, uint32_t> index_;
};

}  // namespace carsh::library
