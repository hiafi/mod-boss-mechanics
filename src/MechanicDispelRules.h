/*
 * mod-boss-mechanics: the pure rules of the bots' boss dispels and purges (STAGE-M5-HANDOFF §3.4, decisions §7.1 #3
 * and #5, WP-P). The bot glue is MechanicBotDispel.cpp.
 *
 * - The class tables: which spells a bot may use, by spell id, to cure a group member (Friendly) or to strip a buff
 *   from an enemy (Hostile). The dispel types each spell removes are read from the live SpellInfo at run time (the
 *   reworks changed several of them); `types` here is the 2026-11-04 data, for the tests and as documentation.
 * - The pick: the first spell of the table, in table order, that the bot knows, that is ready and that removes the
 *   aura's dispel type.
 * - Deadline timing: playerbots' HasAuraToDispel skips auras with less than Playerbots.DispelAuraDuration (700 ms)
 *   left, and a bot needs a tick and maybe a GCD to act. A deadline shorter than that would turn urgent too late,
 *   so the urgent path floors every deadline at skip + lead (EffectiveDeadline) and never applies the skip itself.
 *   Normal listed entries keep the stock skip (NormalSkipped), as the stock cure rows would.
 *
 * No AzerothCore includes: tests/dispel_rules_test.cpp compiles this header with the host compiler.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DISPEL_RULES_H
#define MOD_BOSS_MECHANICS_MECHANIC_DISPEL_RULES_H

#include "MechanicDescriptor.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace BossMechanics::DispelRules
{
    using Ms = std::chrono::milliseconds;

    // SharedDefines.h DispelType values used here.
    constexpr std::uint32_t kMagic = 1;
    constexpr std::uint32_t kCurse = 2;
    constexpr std::uint32_t kDisease = 3;
    constexpr std::uint32_t kPoison = 4;
    constexpr std::uint32_t kEnrage = 9;

    constexpr std::uint32_t Bit(std::uint32_t type)
    {
        return type > 0 && type < 32 ? 1u << type : 0u;
    }

    // SharedDefines.h Classes values.
    constexpr std::uint8_t kPaladin = 2;
    constexpr std::uint8_t kHunter = 3;
    constexpr std::uint8_t kPriest = 5;
    constexpr std::uint8_t kShaman = 7;
    constexpr std::uint8_t kMage = 8;
    constexpr std::uint8_t kWarlock = 9;
    constexpr std::uint8_t kDruid = 11;

    enum class Use : std::uint8_t
    {
        Friendly,   // cure a group member (`boss dispel urgent`)
        Hostile     // strip a buff from an enemy (`boss purge`)
    };

    struct ClassSpell
    {
        std::uint8_t classId;
        std::uint32_t spellId;
        Use use;
        bool pet;               // cast by the bot's pet (the Felhunter), not the bot
        std::uint32_t types;    // Bit() mask of the dispel types (2026-11-04 data; the glue reads SpellInfo)
        char const* name;
    };

    // Checked on 2026-11-04 against core-m5 apps/dbc-tools/source/classes/* and the fork's YAML. Within a class the
    // better spell (or the higher rank) comes first. Not listed, on purpose:
    // - retired for players (untrained by the reworks): Priest Cure Disease 528 and Abolish Disease 552
    //   (dungeon-prereqs S2), Shaman Cure Toxins 526 (shaman_trainers.py), Druid Remove Curse 2782 (Nature's Cure
    //   replaced it);
    // - not a dispel: Druid Soothe Animal 2908 is a detect-range aura (aura 91), it does not remove Enrage in this
    //   core; Tranquilizing Shot is the only player Enrage removal;
    // - not worth a GCD outside its rotation: Warrior Shield Slam 23922 (dispels 1 Magic buff, on its own cooldown);
    //   Rogue Anesthetic Poison 26688 (a weapon proc, not castable);
    // - Mass Dispel 32375 (ground AoE, a cast time).
    inline constexpr ClassSpell kSpells[] = {
        // Priest: Dispel Magic (TARGET_UNIT_TARGET_ANY: friend and foe). 527 is the single-rank bootstrap; 988 is
        // its stock rank 2, used when a character still knows it.
        { kPriest, 988, Use::Friendly, false, Bit(kMagic), "Dispel Magic" },
        { kPriest, 527, Use::Friendly, false, Bit(kMagic), "Dispel Magic" },
        { kPriest, 988, Use::Hostile, false, Bit(kMagic), "Dispel Magic" },
        { kPriest, 527, Use::Hostile, false, Bit(kMagic), "Dispel Magic" },
        // Shaman: Cleanse Spirit is Magic + Curse since dungeon-prereqs S4; Purge (8012 the stock rank 2).
        { kShaman, 51886, Use::Friendly, false, Bit(kMagic) | Bit(kCurse), "Cleanse Spirit" },
        { kShaman, 8012, Use::Hostile, false, Bit(kMagic), "Purge" },
        { kShaman, 370, Use::Hostile, false, Bit(kMagic), "Purge" },
        // Druid: Nature's Cure (M5 WP-N: castable in Cat, Bear and Bestial Fury), then the poison cures. Abolish
        // Poison removes one poison at once (its DISPEL effect) before its periodic part.
        { kDruid, 200604, Use::Friendly, false, Bit(kMagic) | Bit(kPoison), "Nature's Cure" },
        { kDruid, 2893, Use::Friendly, false, Bit(kPoison), "Abolish Poison" },
        { kDruid, 8946, Use::Friendly, false, Bit(kPoison), "Cure Poison" },
        // Paladin: Cleanse is Magic + Disease, Purify Disease only (dungeon-prereqs S3 dropped their Poison effect).
        { kPaladin, 4987, Use::Friendly, false, Bit(kMagic) | Bit(kDisease), "Cleanse" },
        { kPaladin, 1152, Use::Friendly, false, Bit(kDisease), "Purify" },
        // Mage: Remove Curse 475 (stock "Remove Lesser Curse"), still trained; Spellsteal takes a Magic buff.
        { kMage, 475, Use::Friendly, false, Bit(kCurse), "Remove Curse" },
        { kMage, 30449, Use::Hostile, false, Bit(kMagic), "Spellsteal" },
        // Hunter: Tranquilizing Shot removes 1 Enrage and 1 Magic effect.
        { kHunter, 19801, Use::Hostile, false, Bit(kEnrage) | Bit(kMagic), "Tranquilizing Shot" },
        // Warlock: the Felhunter's Devour Magic (TARGET_UNIT_TARGET_ANY), highest stock rank first; 19505 is the
        // single-rank bootstrap.
        { kWarlock, 48011, Use::Hostile, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 27277, Use::Hostile, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 27276, Use::Hostile, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 19736, Use::Hostile, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 19734, Use::Hostile, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 19731, Use::Hostile, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 19505, Use::Hostile, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 48011, Use::Friendly, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 27277, Use::Friendly, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 27276, Use::Friendly, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 19736, Use::Friendly, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 19734, Use::Friendly, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 19731, Use::Friendly, true, Bit(kMagic), "Devour Magic" },
        { kWarlock, 19505, Use::Friendly, true, Bit(kMagic), "Devour Magic" },
    };

    // The class's spells for one use, in table order.
    inline std::vector<ClassSpell> SpellsFor(std::uint8_t classId, Use use)
    {
        std::vector<ClassSpell> out;
        for (ClassSpell const& s : kSpells)
            if (s.classId == classId && s.use == use)
                out.push_back(s);
        return out;
    }

    // Every spell id of the tables (the cast hook's first filter).
    inline bool InTables(std::uint32_t spellId)
    {
        for (ClassSpell const& s : kSpells)
            if (s.spellId == spellId)
                return true;
        return false;
    }

    // One spell as the bot holds it right now.
    struct Owned
    {
        std::uint32_t spellId = 0;
        std::uint32_t types = 0;    // Bit() mask (from SpellInfo)
        bool pet = false;
        bool ready = true;          // known, off cooldown, castable on the target (range, form, GCD)
    };

    // The first ready spell, in table order, that removes `type`.
    inline std::optional<std::size_t> PickSpell(std::vector<Owned> const& owned, std::uint32_t type)
    {
        std::uint32_t const bit = Bit(type);
        if (!bit)
            return std::nullopt;
        for (std::size_t i = 0; i < owned.size(); ++i)
            if (owned[i].ready && (owned[i].types & bit))
                return i;
        return std::nullopt;
    }

    inline std::uint32_t TypesOf(std::vector<Owned> const& owned)
    {
        std::uint32_t mask = 0;
        for (Owned const& o : owned)
            mask |= o.types;
        return mask;
    }

    // ---- deadline timing ----------------------------------------------------------------------------------------

    // Default lead over the stock skip: one bot tick and a running GCD (the deadline must flip while the bot can still
    // act before HasAuraToDispel stops seeing the aura).
    constexpr Ms kDeadlineLead{ 1000 };

    // A deadline entry's urgency threshold: its own deadline, but never under skip + lead. 0 stays 0 (no deadline).
    inline Ms EffectiveDeadline(Ms deadline, Ms skip, Ms lead = kDeadlineLead)
    {
        if (deadline <= Ms(0))
            return Ms(0);
        return std::max(deadline, skip + lead);
    }

    // The descriptor the urgent path orders with: the dispels only, deadlines floored by EffectiveDeadline.
    inline Descriptor UrgentView(Descriptor const& d, Ms skip, Ms lead = kDeadlineLead)
    {
        Descriptor out;
        out.dispels = d.dispels;
        for (DispelEntry& e : out.dispels)
            e.deadline = EffectiveDeadline(e.deadline, skip, lead);
        return out;
    }

    // A normal (not urgent now) listed aura is passed over like the stock rows do: less than `skip` left on an aura
    // that expires. `remaining` < 0: a permanent aura.
    inline bool NormalSkipped(Ms remaining, Ms skip)
    {
        return skip > Ms(0) && remaining >= Ms(0) && remaining < skip;
    }

    // ---- counting a cast ----------------------------------------------------------------------------------------

    enum class CastKind : std::uint8_t
    {
        None,       // no listed aura of the spell's types on the target
        Urgent,     // a loss-of-control entry (urgent = true)
        Deadline,   // a deadline entry inside its (floored) deadline
        Normal      // any other listed entry
    };

    // One listed aura the cast target carries.
    struct TargetAura
    {
        std::uint32_t auraId = 0;
        std::uint32_t type = 0;     // its DispelType
        Ms remaining{ 0 };          // < 0: permanent
    };

    struct CastClass
    {
        CastKind kind = CastKind::None;
        std::size_t aura = 0;       // index into the TargetAura list
    };

    // What a cure of `spellTypes` on a target with `auras` counts as: the best listed aura it can remove (urgent, then
    // deadline, then normal; ties to the higher priority, then the earlier aura). `d` is UrgentView'd by the caller.
    inline CastClass ClassifyCast(Descriptor const& d, std::uint32_t spellTypes, std::vector<TargetAura> const& auras)
    {
        CastClass best;
        int bestRank = -1;
        int bestPriority = -1;
        for (std::size_t i = 0; i < auras.size(); ++i)
        {
            if (!(spellTypes & Bit(auras[i].type)))
                continue;
            std::optional<std::size_t> const k = Desc::FindDispel(d, auras[i].auraId);
            if (!k)
                continue;
            DispelEntry const& e = d.dispels[*k];
            Ms const left = auras[i].remaining < Ms(0) ? Ms::max() : auras[i].remaining;
            CastKind const kind = e.urgent ? CastKind::Urgent
                : Desc::IsUrgent(e, left) ? CastKind::Deadline : CastKind::Normal;
            int const rank = kind == CastKind::Urgent ? 3 : kind == CastKind::Deadline ? 2 : 1;
            if (rank > bestRank || (rank == bestRank && e.priority > bestPriority))
            {
                best = { kind, i };
                bestRank = rank;
                bestPriority = e.priority;
            }
        }
        return best;
    }

    // The purge a cast on an enemy counts as: a listed buff of the spell's types on that unit.
    struct EnemyAura
    {
        std::uint32_t auraId = 0;
        std::uint32_t type = 0;
    };

    inline bool CountsAsPurge(Descriptor const& d, std::uint32_t unitEntry, std::uint32_t spellTypes,
        std::vector<EnemyAura> const& auras)
    {
        for (EnemyAura const& a : auras)
        {
            if (!(spellTypes & Bit(a.type)))
                continue;
            for (PurgeEntry const& e : d.purges)
                if (e.auraId == a.auraId && (!e.unitEntry || e.unitEntry == unitEntry))
                    return true;
        }
        return false;
    }

    // ---- stock member order -------------------------------------------------------------------------------------

    // PartyMemberValue::FindPartyMember's lists: master, healers, tanks, others (a pet right after its owner).
    enum class MemberRank : std::uint8_t
    {
        Master = 0,
        Healer = 1,
        Tank = 2,
        Other = 3
    };

    inline MemberRank RankOf(bool master, bool healer, bool tank)
    {
        if (master)
            return MemberRank::Master;
        if (healer)
            return MemberRank::Healer;
        if (tank)
            return MemberRank::Tank;
        return MemberRank::Other;
    }
}

#endif
