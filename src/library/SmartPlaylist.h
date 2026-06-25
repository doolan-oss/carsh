// Smart playlists — stored as rules, evaluated against the in-RAM index, never
// materialized (PLAN.md §4.1). A rule is (field, op, value); a group ANDs or ORs
// its rules, with an optional sort and limit. This covers the common cases
// ("genre is Jazz AND year >= 2000, newest 50") without a full query language.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "library/Database.h"
#include "library/TextNorm.h"

namespace carsh::library {

enum class Field : uint8_t {
    Title, Artist, Album, AlbumArtist, Genre,
    Year, TrackNo, DiscNo, DurationMs, SourceId, Format, RgTrackGain,
};

enum class Op : uint8_t {
    Eq, Ne, Lt, Le, Gt, Ge, Contains, StartsWith,
};

struct Rule {
    Field field = Field::Title;
    Op op = Op::Contains;
    std::string strVal;   // for string fields / Contains / StartsWith
    double numVal = 0;    // for numeric fields
};

enum class MatchMode : uint8_t { All, Any };
enum class SortBy : uint8_t { None, Year, DurationMs, TrackNo, Title, Artist };

struct SmartRules {
    MatchMode match = MatchMode::All;
    std::vector<Rule> rules;
    SortBy sortBy = SortBy::None;
    bool descending = false;
    std::size_t limit = 0;  // 0 = unlimited
};

class SmartPlaylist {
public:
    static bool isNumeric(Field f) {
        switch (f) {
            case Field::Year: case Field::TrackNo: case Field::DiscNo:
            case Field::DurationMs: case Field::SourceId: case Field::Format:
            case Field::RgTrackGain: return true;
            default: return false;
        }
    }

    static double numField(const Database& db, const Track& t, Field f) {
        (void)db;
        switch (f) {
            case Field::Year: return t.year;
            case Field::TrackNo: return t.trackNo;
            case Field::DiscNo: return t.discNo;
            case Field::DurationMs: return t.durationMs;
            case Field::SourceId: return t.sourceId;
            case Field::Format: return t.format;
            case Field::RgTrackGain: return t.rgTrackGain;
            default: return 0;
        }
    }

    static std::string strField(const Database& db, const Track& t, Field f) {
        switch (f) {
            case Field::Title: return db.title(t);
            case Field::Artist: return db.artist(t);
            case Field::Album: return db.album(t);
            case Field::AlbumArtist: return db.albumArtist(t);
            case Field::Genre: return db.genre(t);
            default: return "";
        }
    }

    static bool matchRule(const Database& db, const Track& t, const Rule& r) {
        if (isNumeric(r.field)) {
            double v = numField(db, t, r.field);
            switch (r.op) {
                case Op::Eq: return v == r.numVal;
                case Op::Ne: return v != r.numVal;
                case Op::Lt: return v < r.numVal;
                case Op::Le: return v <= r.numVal;
                case Op::Gt: return v > r.numVal;
                case Op::Ge: return v >= r.numVal;
                default: return false;
            }
        }
        std::string field = normalize(strField(db, t, r.field));
        std::string val = normalize(r.strVal);
        switch (r.op) {
            case Op::Eq: return field == val;
            case Op::Ne: return field != val;
            case Op::Contains: return field.find(val) != std::string::npos;
            case Op::StartsWith: return field.compare(0, val.size(), val) == 0;
            default: return false;
        }
    }

    static std::vector<uint64_t> evaluate(const Database& db,
                                          const SmartRules& rules) {
        std::vector<const Track*> hits;
        db.forEach([&](const Track& t) {
            bool ok = (rules.match == MatchMode::All);
            for (const Rule& r : rules.rules) {
                bool m = matchRule(db, t, r);
                if (rules.match == MatchMode::All && !m) { ok = false; break; }
                if (rules.match == MatchMode::Any && m) { ok = true; break; }
            }
            if (rules.rules.empty()) ok = true;
            if (ok) hits.push_back(&t);
        });

        if (rules.sortBy != SortBy::None) {
            std::sort(hits.begin(), hits.end(),
                      [&](const Track* a, const Track* b) {
                          int c = cmp(db, *a, *b, rules.sortBy);
                          return rules.descending ? c > 0 : c < 0;
                      });
        }
        std::vector<uint64_t> out;
        for (const Track* t : hits) {
            out.push_back(t->trackId);
            if (rules.limit && out.size() >= rules.limit) break;
        }
        return out;
    }

private:
    static int cmp(const Database& db, const Track& a, const Track& b,
                   SortBy s) {
        switch (s) {
            case SortBy::Year: return tri(a.year, b.year);
            case SortBy::DurationMs: return tri(a.durationMs, b.durationMs);
            case SortBy::TrackNo: return tri(a.trackNo, b.trackNo);
            case SortBy::Title:
                return scmp(normalize(db.title(a)), normalize(db.title(b)));
            case SortBy::Artist:
                return scmp(normalize(db.artist(a)), normalize(db.artist(b)));
            default: return 0;
        }
    }
    template <typename T>
    static int tri(T a, T b) { return a < b ? -1 : (a > b ? 1 : 0); }
    static int scmp(const std::string& a, const std::string& b) {
        return a < b ? -1 : (a > b ? 1 : 0);
    }
};

}  // namespace carsh::library
