/*
 * mod-boss-mechanics: the heal read hook's per-map cache (Zul'Farrak F-1f, R-1-FW m4). Pure: the standard library
 * and MechanicDescriptor.h (HealUntilEntry, Desc::HealReadPct), no server headers, so tests/zf_heal_read_cache_test.cpp
 * builds it on the host.
 *
 * The playerbots fork's heal read hook (ai::health::SetHealReadHook) is called from the reading bot's map thread for
 * any unit the bot asks about, and a focus heal target may stand on another map. Reading that map's registry and the
 * unit's auras from there is a data race. So each map publishes, from its own update thread (the bot after-tick),
 * the healUntil entries whose aura each of its players (and their pets) carries now; the hook reads only this cache,
 * under a shared lock, by the unit's guid. A map's rows are replaced at each of its updates and dropped when it has
 * none; a row older than kStaleMs (a map that stopped updating, a destroyed instance) is ignored and pruned.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_HEAL_READ_CACHE_H
#define MOD_BOSS_MECHANICS_MECHANIC_HEAL_READ_CACHE_H

#include "MechanicDescriptor.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace BossMechanics
{
    // `Owner`: who publishes a row set (the map: `Map const*` live, an int in the tests); hashable.
    template <class Owner>
    class HealReadCache
    {
    public:
        // A row older than this is ignored by Read and dropped by the next Publish: several full map updates.
        static constexpr std::int64_t kStaleMs = 3000;

        using Rows = std::vector<std::pair<std::uint64_t, std::vector<HealUntilEntry>>>;

        // The owner's rows now (unit guid -> the entries whose aura it carries), stamped `nowMs`; replaces every row
        // the owner published before. Takes no lock when the owner publishes nothing and had nothing.
        void Publish(Owner owner, Rows rows, std::int64_t nowMs)
        {
            if (rows.empty())
            {
                std::shared_lock<std::shared_mutex> const read(_mutex);
                if (!_owners.count(owner))
                    return;
            }
            std::unique_lock<std::shared_mutex> const write(_mutex);
            for (auto it = _rows.begin(); it != _rows.end();)
            {
                if (it->second.owner == owner || nowMs - it->second.atMs > kStaleMs)
                    it = _rows.erase(it);
                else
                    ++it;
            }
            for (auto& [guid, entries] : rows)
                if (!entries.empty())
                    _rows[guid] = Row{ owner, nowMs, std::move(entries) };
            _owners.clear();
            for (auto const& [guid, row] : _rows)
                _owners.insert(row.owner);
            _any.store(!_rows.empty(), std::memory_order_relaxed);
        }

        // The percent to read for unit `guid` at `pct` (Desc::HealReadPct over its cached entries); `pct` when the
        // unit has no fresh row. Any thread.
        float Read(std::uint64_t guid, float pct, std::int64_t nowMs) const
        {
            if (!_any.load(std::memory_order_relaxed))
                return pct;
            std::shared_lock<std::shared_mutex> const read(_mutex);
            auto const it = _rows.find(guid);
            if (it == _rows.end() || nowMs - it->second.atMs > kStaleMs)
                return pct;
            return Desc::HealReadPct(it->second.entries, pct, [](std::uint32_t) { return true; });
        }

        std::size_t Size() const
        {
            std::shared_lock<std::shared_mutex> const read(_mutex);
            return _rows.size();
        }

    private:
        struct Row
        {
            Owner owner{};
            std::int64_t atMs = 0;
            std::vector<HealUntilEntry> entries;
        };

        mutable std::shared_mutex _mutex;
        std::unordered_map<std::uint64_t, Row> _rows;
        std::unordered_set<Owner> _owners;
        std::atomic<bool> _any{ false };
    };
}

#endif
