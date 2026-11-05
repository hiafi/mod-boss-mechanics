/*
 * mod-boss-mechanics: the bots' copy of the boss descriptor (STAGE-M5-HANDOFF §3.2, WP-D).
 *
 * One DescriptorView per bot (MechanicBotState::descriptor). RefreshDescriptorView compares
 * Registry::DescriptorRevision with the one the view was built from and copies only on a change: the active
 * encounter's descriptor (only when its stamp moved), the encounter's units and prisons, and the map's cast notes.
 * Kick claims are not copied; WP-I reads and writes them live (Registry::ClaimCast).
 *
 * FactsOf turns a unit into the Desc::UnitFacts the pure rules read (MechanicDescriptor.h), cached per bot per tick:
 * the referenced auras, the nearest live unit of each NearUnit entry, the referenced zones and every Custom
 * condition, each evaluated once per unit and tick (§6: the `when` lambdas run on the map thread for every bot tick).
 *
 * AzerothCore only: no playerbots types, so WP-I/P/T call it from their values and the module builds without
 * mod-playerbots. Runs on the bot's map thread, like every registry read.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DESCRIPTOR_VIEW_H
#define MOD_BOSS_MECHANICS_MECHANIC_DESCRIPTOR_VIEW_H

#include "Define.h"
#include "Duration.h"
#include "MechanicDescriptor.h"
#include "MechanicRegistry.h"
#include "ObjectGuid.h"
#include <unordered_map>
#include <vector>

class Player;
class Unit;
class WorldObject;

namespace BossMechanics
{
    // `refs` and the cached facts' Custom results point into `descriptor`: a move keeps them (the vectors' buffers
    // move), a copy rebuilds `refs` and drops the cache.
    struct DescriptorView
    {
        DescriptorView() = default;
        DescriptorView(DescriptorView const& other) { *this = other; }
        DescriptorView(DescriptorView&& other) = default;
        DescriptorView& operator=(DescriptorView const& other);
        DescriptorView& operator=(DescriptorView&& other) = default;

        uint32 seenRevision = 0;            // Registry::DescriptorRevision() of the copy (0: never built)
        ObjectGuid owner;                   // the active encounter (empty: no descriptor applies to the bot)
        uint32 stamp = 0;                   // the encounter's descriptorStamp the copy was taken at
        Descriptor descriptor;              // the copy (phase included)
        Desc::References refs;              // what its conditions read (pointers into `descriptor`)
        std::vector<EncounterUnit> units;   // the encounter's adds and tracked units (not the owner)
        std::vector<HeldPlayer> held;       // its prisons
        std::vector<Desc::CastNote> notes;  // the map's cast notes
        uint32 copies = 0;                  // descriptor copies taken (`.mech desc`, the sim's cost check)

        // ---- per-tick cache (FactsOf) ----
        Milliseconds factsAt{ -1 };
        std::unordered_map<uint64, Desc::UnitFacts> facts;

        bool Active() const { return !owner.IsEmpty(); }
        // The prison holding `player` (empty: none), and the player a prison holds (empty: not a prison).
        ObjectGuid PrisonOf(ObjectGuid player) const;
        ObjectGuid HeldBy(ObjectGuid prison) const;
        bool IsEncounterUnit(ObjectGuid unit) const;    // the owner or one of `units`
    };

    // Brings the view up to date with the bot's map and returns true when it changed. The active encounter: a live
    // one with a descriptor whose scope reaches the bot (in an instance every encounter; in the open world one whose
    // scope set holds the bot or a member of its group, or has no scope set), the nearest owner first. A null
    // registry (nothing on the map) clears the view.
    bool RefreshDescriptorView(Registry const* reg, Player const* bot, DescriptorView& view);

    // The unit's facts for the view's descriptor, built at most once per `now` (the cache is dropped when `now`
    // moves or the view is re-copied).
    Desc::UnitFacts const& FactsOf(DescriptorView& view, Registry const& reg, Unit const* unit, Milliseconds now);

    // The shared condition context: the copy's phase and the owner's health now (100 when it is gone). `near`: any
    // object on the bot's map (the bot).
    Desc::Context ContextOf(DescriptorView const& view, WorldObject const* near);

    // Break-out order (§3.5): the role of the player `prison` holds, by the module's role provider (the owner's victim
    // is the tank). None when it holds nobody or the player is gone.
    Desc::HeldRank HeldRankOf(DescriptorView const& view, WorldObject const* near, ObjectGuid prison);
}

#endif
