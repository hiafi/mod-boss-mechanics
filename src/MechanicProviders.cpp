/*
 * mod-boss-mechanics: provider stubs and installation (STAGE-M1-HANDOFF §3.7).
 */

#include "MechanicProviders.h"
#include "BossMechanicsConfig.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Map.h"
#include "MechanicRoleRules.h"
#include "Player.h"
#include "Unit.h"
#include <array>
#include <unordered_map>
#include <vector>

namespace BossMechanics
{
    namespace
    {
        ConfigReferenceHealthProvider const sConfigReferenceHealth;
        DefaultContentKeyProvider const sDefaultContentKey;
        StubRoleProvider const sStubRole;

        ReferenceHealthProvider const* sReferenceHealth = &sConfigReferenceHealth;
        ContentKeyProvider const* sContentKey = &sDefaultContentKey;
        RoleProvider const* sRole = &sStubRole;

        // Learned talent spell -> the talent ranks it is (every Talent.dbc row that lists it), built once on first use
        // (the talent store is complete by then: DBC and talent_dbc load at startup). Player::GetSpec walks the whole
        // store (60,476 rows with the custom talents) per tab instead (STAGE-M5-CD-REPORT §C4).
        using TalentIndex = std::unordered_map<uint32, std::vector<RoleRules::TalentRank>>;

        TalentIndex BuildTalentIndex()
        {
            TalentIndex index;
            for (uint32 id = 0; id < sTalentStore.GetNumRows(); ++id)
            {
                TalentEntry const* talent = sTalentStore.LookupEntry(id);
                if (!talent)
                    continue;
                for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
                    if (uint32 const spellId = talent->RankID[rank])
                        index[spellId].push_back({ id, talent->TalentTab, static_cast<uint8>(rank + 1) });
            }
            return index;
        }

        TalentIndex const& GetTalentIndex()
        {
            static TalentIndex const index = BuildTalentIndex();
            return index;
        }

        // Player::GetSpec(), from the player's own talent map: O(learned talents).
        uint32 ComputeSpecTab(Player const* player)
        {
            if (!player->GetSpecsCount())
                return 0;
            uint8 const spec = player->GetActiveSpec();
            uint32 const* pages = GetTalentTabPages(player->getClass());
            std::array<uint32, 3> const classTabs{ pages[0], pages[1], pages[2] };
            TalentIndex const& index = GetTalentIndex();
            thread_local std::vector<RoleRules::TalentRank> learned;
            learned.clear();
            for (auto const& [spellId, talent] : player->GetTalentMap())
            {
                if (talent->State == PLAYERSPELL_REMOVED || !talent->IsInSpec(spec))
                    continue;
                auto const it = index.find(spellId);
                if (it != index.end())
                    learned.insert(learned.end(), it->second.begin(), it->second.end());
            }
            return RoleRules::SpecTab(classTabs, learned);
        }

        struct CachedSpec
        {
            RoleRules::TalentFingerprint fingerprint;
            uint64 atMs = 0;
            uint32 tab = 0;
        };

        // Per map thread, like the brain's other per-thread state: no lock. A player seen on two threads (a map
        // change, a command on the world thread) is computed once on each.
        thread_local std::unordered_map<uint64, CachedSpec> tSpecCache;

        uint32 CachedSpecTab(Player const* player)
        {
            RoleRules::TalentFingerprint fingerprint;
            fingerprint.talents = static_cast<uint32>(player->GetTalentMap().size());
            fingerprint.freePoints = player->GetFreeTalentPoints();
            fingerprint.activeSpec = player->GetActiveSpec();
            fingerprint.specsCount = player->GetSpecsCount();
            fingerprint.level = player->GetLevel();
            uint64 const nowMs = static_cast<uint64>(GameTime::GetGameTimeMS().count());
            uint64 const key = player->GetGUID().GetRawValue();
            auto const it = tSpecCache.find(key);
            if (it != tSpecCache.end() && RoleRules::SpecCacheFresh(it->second.fingerprint, it->second.atMs,
                fingerprint, nowMs))
                return it->second.tab;
            if (it == tSpecCache.end() && tSpecCache.size() >= RoleRules::kSpecCacheMaxPlayers)
                tSpecCache.clear();
            CachedSpec& entry = tSpecCache[key];
            entry.fingerprint = fingerprint;
            entry.atMs = nowMs;
            entry.tab = ComputeSpecTab(player);
            return entry.tab;
        }
    }

    void SetReferenceHealthProvider(ReferenceHealthProvider* provider)
    {
        sReferenceHealth = provider ? provider : &sConfigReferenceHealth;
    }

    void SetContentKeyProvider(ContentKeyProvider* provider)
    {
        sContentKey = provider ? provider : &sDefaultContentKey;
    }

    void SetRoleProvider(RoleProvider* provider)
    {
        sRole = provider ? provider : &sStubRole;
    }

    ReferenceHealthProvider const& GetReferenceHealthProvider()
    {
        return *sReferenceHealth;
    }

    ContentKeyProvider const& GetContentKeyProvider()
    {
        return *sContentKey;
    }

    RoleProvider const& GetRoleProvider()
    {
        return *sRole;
    }

    bool IsConfigReferenceHealthActive()
    {
        return sReferenceHealth == &sConfigReferenceHealth;
    }

    ContentKey DefaultContentKey(Map const* map, Unit const* owner)
    {
        RefHealth::ContentInputs in;
        in.hasMap = map != nullptr;
        in.mapId = map ? map->GetId() : 0;
        in.heroic = map && map->IsHeroic();
        in.ownerLevel = owner ? owner->GetLevel() : 0;
        in.empoweredLevel = 0;
        return RefHealth::ResolveContentKey(in, GetConfig().referenceHealthMapLevels);
    }

    ReferenceHealth ConfigReferenceHealthProvider::Get(ContentKey const& key) const
    {
        return RefHealth::Lookup(GetConfig().referenceHealth, key).health;
    }

    ContentKey DefaultContentKeyProvider::Get(Map const* map, Unit const* owner) const
    {
        return DefaultContentKey(map, owner);
    }

    Role StubRoleProvider::Get(Unit const* owner, Player const* player) const
    {
        if (!player)
            return Role::Damage;
        Unit const* victim = owner ? owner->GetVictim() : nullptr;
        bool const isVictim = victim && victim->GetGUID() == player->GetGUID();
        if (isVictim)
            return Role::Tank;
        uint32 const specTab = CachedSpecTab(player);
        switch (RoleRules::Decide(victim != nullptr, false, specTab, player->GetShapeshiftForm()))
        {
            case RoleRules::Kind::Tank:
                return Role::Tank;
            case RoleRules::Kind::Healer:
                return Role::Healer;
            default:
                return Role::Damage;
        }
    }
}
