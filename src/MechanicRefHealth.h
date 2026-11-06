/*
 * mod-boss-mechanics: keyed reference health (dungeon difficulties STAGE-E0-E3-HANDOFF §3.7, decision §7 #5, WP-3P).
 *
 * A mechanic's damage is a percent of reference health; this header picks the row. The content key is the content
 * level plus the difficulty: normal, heroic, or an empowered level (E4 sets it through SetContentKeyProvider; until
 * then it is always 0). No AzerothCore includes: tests/ref_health_test.cpp compiles it with the host compiler.
 *
 * BossMechanics.ReferenceHealth.Rows: entries "<level>[tier]:<nonTank>:<tank>" separated by spaces, commas, semicolons
 * or tabs. The tier suffix is none (normal), `h` (heroic) or `e<N>` (empowered level N, 1-255), any case:
 *   "20:1000:1500 60:5000:8000 60h:6000:9600 60e1:7000:11000"
 * The level-only stub format ("20:1000:1500 60:5000:8000") is a valid table of normal rows. At least one normal row
 * is required (the last fallback); a key repeated within one tier is an error.
 *
 * Lookup: linear interpolation by level within the key's tier, clamped at both ends (InterpolateReferenceHealth).
 * Empowered level N with no `e<N>` rows uses the heroic rows; heroic with no heroic rows uses the normal rows.
 *
 * BossMechanics.ReferenceHealth.MapLevels: entries "<mapId>[h]:<level>" pin the content level of a map (normal, or
 * heroic with `h`) instead of the owner's level: a dungeon's mechanics then use one row whatever the level of the
 * boss (19, 20 or 21) or of the `.mech test` owner (the GM's level). Empty: the owner's level everywhere.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_REF_HEALTH_H
#define MOD_BOSS_MECHANICS_MECHANIC_REF_HEALTH_H

#include "MechanicMath.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace BossMechanics
{
    // What a mechanic's damage is keyed by. empoweredLevel > 0 is an empowered heroic (E4); heroic is then implied.
    struct ContentKey
    {
        std::uint8_t level = 0;
        bool heroic = false;
        std::uint8_t empoweredLevel = 0;
    };

    namespace RefHealth
    {
        enum class Tier : std::uint8_t
        {
            Normal,
            Heroic,
            Empowered
        };

        // The rows of every tier, each sorted by level.
        struct Table
        {
            std::vector<ReferenceHealthRow> normal;
            std::vector<ReferenceHealthRow> heroic;
            std::map<std::uint8_t, std::vector<ReferenceHealthRow>> empowered;     // by empowered level
        };

        // The row a key resolved to: the health and the tier (and empowered level) whose rows gave it.
        struct Resolved
        {
            ReferenceHealth health;
            Tier tier = Tier::Normal;
            std::uint8_t empoweredLevel = 0;
        };

        namespace Detail
        {
            inline std::vector<std::string_view> Tokens(std::string_view text)
            {
                std::vector<std::string_view> out;
                std::size_t i = 0;
                auto const isSep = [](char c)
                {
                    return c == ' ' || c == ',' || c == ';' || c == '\t' || c == '\r' || c == '\n';
                };
                while (i < text.size())
                {
                    while (i < text.size() && isSep(text[i]))
                        ++i;
                    std::size_t const start = i;
                    while (i < text.size() && !isSep(text[i]))
                        ++i;
                    if (i > start)
                        out.push_back(text.substr(start, i - start));
                }
                return out;
            }

            // Decimal digits only (no sign, no blank), at most `max`.
            inline bool ParseUnsigned(std::string_view s, std::uint64_t max, std::uint64_t& out)
            {
                if (s.empty() || s.size() > 19)
                    return false;
                std::uint64_t v = 0;
                for (char c : s)
                {
                    if (c < '0' || c > '9')
                        return false;
                    v = v * 10 + static_cast<std::uint64_t>(c - '0');
                }
                if (v > max)
                    return false;
                out = v;
                return true;
            }

            // Splits "a:b:c" into its fields; false unless exactly `n` fields.
            inline bool SplitColons(std::string_view tok, std::size_t n, std::vector<std::string_view>& out)
            {
                out.clear();
                std::size_t start = 0;
                for (std::size_t i = 0; i <= tok.size(); ++i)
                {
                    if (i == tok.size() || tok[i] == ':')
                    {
                        out.push_back(tok.substr(start, i - start));
                        start = i + 1;
                    }
                }
                return out.size() == n;
            }

            inline void SortRows(std::vector<ReferenceHealthRow>& rows)
            {
                std::sort(rows.begin(), rows.end(), [](ReferenceHealthRow const& a, ReferenceHealthRow const& b)
                {
                    return a.level < b.level;
                });
            }

            inline bool HasLevel(std::vector<ReferenceHealthRow> const& rows, std::uint32_t level)
            {
                for (ReferenceHealthRow const& r : rows)
                    if (r.level == level)
                        return true;
                return false;
            }
        }

        // Parses the key part "<level>[h|e<N>]". Level 0-255 (a content key's level is a byte).
        inline bool ParseRowKey(std::string_view s, std::uint32_t& level, Tier& tier, std::uint8_t& empowered)
        {
            std::size_t digits = 0;
            while (digits < s.size() && s[digits] >= '0' && s[digits] <= '9')
                ++digits;
            std::uint64_t v = 0;
            if (!Detail::ParseUnsigned(s.substr(0, digits), std::numeric_limits<std::uint8_t>::max(), v))
                return false;
            level = static_cast<std::uint32_t>(v);
            std::string_view const suffix = s.substr(digits);
            empowered = 0;
            if (suffix.empty())
            {
                tier = Tier::Normal;
                return true;
            }
            if (suffix == "h" || suffix == "H")
            {
                tier = Tier::Heroic;
                return true;
            }
            if (suffix[0] == 'e' || suffix[0] == 'E')
            {
                std::uint64_t n = 0;
                if (!Detail::ParseUnsigned(suffix.substr(1), std::numeric_limits<std::uint8_t>::max(), n) || n == 0)
                    return false;
                tier = Tier::Empowered;
                empowered = static_cast<std::uint8_t>(n);
                return true;
            }
            return false;
        }

        // Parses BossMechanics.ReferenceHealth.Rows. On failure `out` is left empty and `err` names the bad entry.
        inline bool ParseTable(std::string_view text, Table& out, std::string& err)
        {
            out = Table();
            err.clear();
            std::vector<std::string_view> parts;
            for (std::string_view tok : Detail::Tokens(text))
            {
                std::uint32_t level = 0;
                Tier tier = Tier::Normal;
                std::uint8_t empowered = 0;
                std::uint64_t nonTank = 0;
                std::uint64_t tank = 0;
                std::uint64_t const maxHealth = std::numeric_limits<std::int32_t>::max();
                if (!Detail::SplitColons(tok, 3, parts) || !ParseRowKey(parts[0], level, tier, empowered)
                    || !Detail::ParseUnsigned(parts[1], maxHealth, nonTank)
                    || !Detail::ParseUnsigned(parts[2], maxHealth, tank) || nonTank == 0 || tank == 0)
                {
                    err = "bad entry '" + std::string(tok) + "' (want <level>[h|e<N>]:<nonTank>:<tank>, health > 0)";
                    out = Table();
                    return false;
                }
                std::vector<ReferenceHealthRow>& rows = tier == Tier::Normal ? out.normal
                    : tier == Tier::Heroic ? out.heroic : out.empowered[empowered];
                if (Detail::HasLevel(rows, level))
                {
                    err = "duplicate entry '" + std::string(tok) + "'";
                    out = Table();
                    return false;
                }
                rows.push_back({ level, { static_cast<std::uint32_t>(nonTank), static_cast<std::uint32_t>(tank) } });
            }
            if (out.normal.empty())
            {
                err = "no normal row (an entry without a suffix, e.g. 20:1000:1500)";
                out = Table();
                return false;
            }
            Detail::SortRows(out.normal);
            Detail::SortRows(out.heroic);
            for (auto& [n, rows] : out.empowered)
                Detail::SortRows(rows);
            return true;
        }

        // The canonical text of a table: normal, heroic, then empowered 1..N, each by level.
        inline std::string Format(Table const& t)
        {
            std::string s;
            auto add = [&s](std::vector<ReferenceHealthRow> const& rows, std::string const& suffix)
            {
                for (ReferenceHealthRow const& r : rows)
                {
                    if (!s.empty())
                        s += ' ';
                    s += std::to_string(r.level) + suffix + ':' + std::to_string(r.health.nonTank) + ':'
                        + std::to_string(r.health.tank);
                }
            };
            add(t.normal, "");
            add(t.heroic, "h");
            for (auto const& [n, rows] : t.empowered)
                add(rows, "e" + std::to_string(n));
            return s;
        }

        // The row for a key: empowered N -> its rows, else heroic; heroic -> its rows, else normal; normal rows.
        inline Resolved Lookup(Table const& t, ContentKey const& key)
        {
            if (key.empoweredLevel > 0)
            {
                auto const it = t.empowered.find(key.empoweredLevel);
                if (it != t.empowered.end() && !it->second.empty())
                    return { InterpolateReferenceHealth(it->second, key.level), Tier::Empowered, key.empoweredLevel };
            }
            if ((key.heroic || key.empoweredLevel > 0) && !t.heroic.empty())
                return { InterpolateReferenceHealth(t.heroic, key.level), Tier::Heroic, 0 };
            return { InterpolateReferenceHealth(t.normal, key.level), Tier::Normal, 0 };
        }

        // "level 20 normal", "level 60 heroic", "level 60 empowered 2".
        inline std::string Describe(ContentKey const& key)
        {
            std::string s = "level " + std::to_string(key.level);
            if (key.empoweredLevel > 0)
                return s + " empowered " + std::to_string(key.empoweredLevel);
            return s + (key.heroic ? " heroic" : " normal");
        }

        inline char const* TierName(Tier tier)
        {
            switch (tier)
            {
                case Tier::Heroic:
                    return "heroic";
                case Tier::Empowered:
                    return "empowered";
                default:
                    return "normal";
            }
        }

        // ------------------------------------------------------------------------------------------------------------
        // Content key resolution
        // ------------------------------------------------------------------------------------------------------------

        // (map id, heroic) -> pinned content level.
        using MapLevels = std::map<std::pair<std::uint32_t, bool>, std::uint8_t>;

        // Parses BossMechanics.ReferenceHealth.MapLevels ("36:20 36h:60"). Empty text is valid (no pins). Level 1-255.
        inline bool ParseMapLevels(std::string_view text, MapLevels& out, std::string& err)
        {
            out.clear();
            err.clear();
            std::vector<std::string_view> parts;
            for (std::string_view tok : Detail::Tokens(text))
            {
                bool ok = Detail::SplitColons(tok, 2, parts);
                std::string_view mapPart = ok ? parts[0] : std::string_view();
                bool heroic = false;
                if (!mapPart.empty() && (mapPart.back() == 'h' || mapPart.back() == 'H'))
                {
                    heroic = true;
                    mapPart.remove_suffix(1);
                }
                std::uint64_t mapId = 0;
                std::uint64_t level = 0;
                ok = ok && Detail::ParseUnsigned(mapPart, std::numeric_limits<std::uint32_t>::max(), mapId)
                    && Detail::ParseUnsigned(parts[1], std::numeric_limits<std::uint8_t>::max(), level) && level > 0;
                if (!ok)
                {
                    err = "bad entry '" + std::string(tok) + "' (want <mapId>[h]:<level>, level 1-255)";
                    out.clear();
                    return false;
                }
                if (!out.emplace(std::make_pair(static_cast<std::uint32_t>(mapId), heroic),
                    static_cast<std::uint8_t>(level)).second)
                {
                    err = "duplicate entry '" + std::string(tok) + "'";
                    out.clear();
                    return false;
                }
            }
            return true;
        }

        // What the engine knows about where a mechanic resolves.
        struct ContentInputs
        {
            bool hasMap = false;
            std::uint32_t mapId = 0;
            bool heroic = false;                // Map::IsHeroic(): a heroic dungeon, or a 10/25 heroic raid
            std::uint8_t ownerLevel = 0;        // 0 without an owner
            std::uint8_t empoweredLevel = 0;    // E4's state; 0 until then
        };

        // The default content key: the map's difficulty (the open world and the sim are normal), the map's pinned
        // level for that difficulty, else the owner's level; the empowered level passes through.
        inline ContentKey ResolveContentKey(ContentInputs const& in, MapLevels const& pins)
        {
            ContentKey key;
            key.heroic = in.hasMap && in.heroic;
            key.empoweredLevel = in.empoweredLevel;
            key.level = in.ownerLevel;
            if (in.hasMap)
            {
                auto const it = pins.find({ in.mapId, key.heroic });
                if (it != pins.end())
                    key.level = it->second;
            }
            return key;
        }
    }
}

#endif
