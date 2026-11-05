/*
 * mod-boss-mechanics: the pure rules of the stub role provider (MechanicProviders.cpp StubRoleProvider), dungeon
 * mechanics STAGE-M5 C4 (STAGE-M5-CD-REPORT §C4).
 *
 * The provider used to call Player::HasTankSpec / HasHealSpec per call. Both go through Player::GetSpec, which walks
 * every row of sTalentStore (60,476 with the custom talents) for each of the class's 3 tabs: about 72 us a call, and a
 * carrier solve made about 8. The provider now reads the player's own learned talents through a spell -> talent rank
 * index built once, keeps the spec per player in a per-thread cache, and decides the role here, with the same result
 * as GetSpec and HasTankSpec / HasHealSpec:
 *
 * - SpecTab: the class tab with the most points in the active spec. Per talent its highest learned rank counts; the
 *   first tab wins a tie; no point in any tab is no spec (0).
 * - Decide: with an owner victim, the victim is the tank and nobody else is; with none, a tank tab is the tank (Feral
 *   only in bear or dire bear form). Then a heal tab is the healer, else damage.
 * - SpecCacheFresh: a cached spec stands while the talent fingerprint is unchanged and for kSpecCacheTtlMs at most.
 *
 * No AzerothCore includes: tests/role_rules_test.cpp compiles this with the host compiler.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_ROLE_RULES_H
#define MOD_BOSS_MECHANICS_MECHANIC_ROLE_RULES_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace BossMechanics::RoleRules
{
    // Core TalentTree (Player.h): the tabs HasTankSpec and HasHealSpec name.
    constexpr uint32_t kTabWarriorProtection = 163;
    constexpr uint32_t kTabPaladinHoly = 382;
    constexpr uint32_t kTabPaladinProtection = 383;
    constexpr uint32_t kTabPriestDiscipline = 201;
    constexpr uint32_t kTabPriestHoly = 202;
    constexpr uint32_t kTabDeathKnightBlood = 398;
    constexpr uint32_t kTabShamanRestoration = 262;
    constexpr uint32_t kTabDruidFeral = 281;
    constexpr uint32_t kTabDruidRestoration = 282;

    // ShapeshiftForm (UnitDefines.h): HasTankSpec's bear forms.
    constexpr uint8_t kFormBear = 0x05;
    constexpr uint8_t kFormDireBear = 0x08;

    enum class Kind : uint8_t
    {
        Damage,
        Tank,
        Healer,
    };

    // One talent rank a learned spell is: the Talent.dbc row, its tab, and the rank (1-based).
    struct TalentRank
    {
        uint32_t talentId = 0;
        uint32_t tabId = 0;
        uint8_t rank = 0;
    };

    // The class tab with the most points among `learned` (every rank of every talent the player has in the active
    // spec; unordered, sorted in place). Same as Player::GetSpec: the highest rank per talent counts, talents of
    // other tabs are ignored, the first tab of classTabs wins a tie, and 0 points everywhere is 0.
    inline uint32_t SpecTab(std::array<uint32_t, 3> const& classTabs, std::vector<TalentRank>& learned)
    {
        std::sort(learned.begin(), learned.end(), [](TalentRank const& a, TalentRank const& b)
        {
            return a.talentId != b.talentId ? a.talentId < b.talentId : a.rank > b.rank;
        });
        std::array<uint32_t, 3> points{};
        for (std::size_t i = 0; i < learned.size(); ++i)
        {
            if (i && learned[i].talentId == learned[i - 1].talentId)
                continue;   // a lower rank of a talent already counted
            for (std::size_t tab = 0; tab < classTabs.size(); ++tab)
                if (learned[i].tabId == classTabs[tab])
                    points[tab] += learned[i].rank;
        }
        uint32_t best = 0;
        uint32_t bestPoints = 0;
        for (std::size_t tab = 0; tab < classTabs.size(); ++tab)
        {
            if (bestPoints < points[tab])
            {
                bestPoints = points[tab];
                best = classTabs[tab];
            }
        }
        return best;
    }

    // Player::HasTankSpec on a spec tab and the current form.
    inline bool IsTankTab(uint32_t specTab, uint8_t form)
    {
        switch (specTab)
        {
            case kTabWarriorProtection:
            case kTabPaladinProtection:
            case kTabDeathKnightBlood:
                return true;
            case kTabDruidFeral:
                return form == kFormBear || form == kFormDireBear;
            default:
                return false;
        }
    }

    // Player::HasHealSpec on a spec tab.
    inline bool IsHealTab(uint32_t specTab)
    {
        switch (specTab)
        {
            case kTabPaladinHoly:
            case kTabPriestDiscipline:
            case kTabPriestHoly:
            case kTabShamanRestoration:
            case kTabDruidRestoration:
                return true;
            default:
                return false;
        }
    }

    // The stub role: ownerHasVictim = the mechanic's owner has a victim, isVictim = this player is it.
    inline Kind Decide(bool ownerHasVictim, bool isVictim, uint32_t specTab, uint8_t form)
    {
        if (ownerHasVictim && isVictim)
            return Kind::Tank;
        if (!ownerHasVictim && IsTankTab(specTab, form))
            return Kind::Tank;
        if (IsHealTab(specTab))
            return Kind::Healer;
        return Kind::Damage;
    }

    // What the cached spec was computed from. Every way the active spec's talents change moves one of these: learning
    // a talent spends a free point, a reset refunds them, a dual spec switch changes activeSpec, a level change
    // re-grants points (and a level drop resets talents), a talent spell taught directly grows the talent map.
    struct TalentFingerprint
    {
        uint32_t talents = 0;       // Player::GetTalentMap().size()
        uint32_t freePoints = 0;    // Player::GetFreeTalentPoints()
        uint8_t activeSpec = 0;
        uint8_t specsCount = 0;
        uint8_t level = 0;

        bool operator==(TalentFingerprint const&) const = default;
    };

    // A backstop for a change the fingerprint misses (a reset and relearn to the same counts in one tick): a cached
    // spec is computed again at least this often. A miss costs a few us.
    constexpr uint64_t kSpecCacheTtlMs = 5000;

    inline bool SpecCacheFresh(TalentFingerprint const& cached, uint64_t cachedAtMs, TalentFingerprint const& current,
        uint64_t nowMs)
    {
        return cached == current && nowMs >= cachedAtMs && nowMs - cachedAtMs < kSpecCacheTtlMs;
    }

    // The per-thread cache is cleared when it grows past this many players (logged-out players are never evicted
    // otherwise).
    constexpr std::size_t kSpecCacheMaxPlayers = 4096;
}

#endif
