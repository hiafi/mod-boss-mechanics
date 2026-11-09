/*
 * mod-boss-mechanics: the bots' copy of the boss descriptor (STAGE-M5-HANDOFF §3.2, WP-D). See
 * MechanicDescriptorView.h.
 */

#include "MechanicDescriptorView.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "MechanicProviders.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Unit.h"
#include <limits>
#include <list>

namespace BossMechanics
{
    namespace
    {
        // In the open world the encounter's scope set must reach the bot (as the gate's InTestScope); an empty set
        // (a boss without `.mech`, dummy or sim scoping) reaches everyone.
        bool ScopeReaches(Encounter const& e, Player const* bot, bool instance)
        {
            if (instance || e.testGms.empty())
                return true;
            Group const* group = bot->GetGroup();
            ObjectGuid const self = bot->GetGUID();
            for (ObjectGuid const& g : e.testGms)
                if (g == self || (group && group->IsMember(g)))
                    return true;
            return false;
        }

        void Clear(DescriptorView& view)
        {
            view.owner = ObjectGuid::Empty;
            view.stamp = 0;
            view.descriptor = Descriptor();
            view.refs = Desc::References();
            view.units.clear();
            view.held.clear();
            view.notes.clear();
            view.facts.clear();
            view.factsAt = Milliseconds(-1);
        }
    }

    DescriptorView& DescriptorView::operator=(DescriptorView const& other)
    {
        if (this == &other)
            return *this;
        seenRevision = other.seenRevision;
        mapId = other.mapId;
        instanceId = other.instanceId;
        owner = other.owner;
        stamp = other.stamp;
        descriptor = other.descriptor;
        refs = Desc::Referenced(descriptor);
        units = other.units;
        held = other.held;
        notes = other.notes;
        copies = other.copies;
        facts.clear();
        factsAt = Milliseconds(-1);
        return *this;
    }

    ObjectGuid DescriptorView::PrisonOf(ObjectGuid player) const
    {
        for (HeldPlayer const& h : held)
            if (h.player == player)
                return h.prison;
        return ObjectGuid::Empty;
    }

    ObjectGuid DescriptorView::HeldBy(ObjectGuid prison) const
    {
        for (HeldPlayer const& h : held)
            if (h.prison == prison)
                return h.player;
        return ObjectGuid::Empty;
    }

    bool DescriptorView::IsEncounterUnit(ObjectGuid unit) const
    {
        if (!unit.IsEmpty() && unit == owner)
            return true;
        for (EncounterUnit const& u : units)
            if (u.guid == unit)
                return true;
        return false;
    }

    bool RefreshDescriptorView(Registry const* reg, Player const* bot, DescriptorView& view)
    {
        if (!reg || !bot || !bot->IsInWorld())
        {
            // R n-3: every strategy bot on a map without a registry passes here on every tick.
            if (!view.Active() && view.seenRevision == 0)
                return false;
            Clear(view);
            view.seenRevision = 0;
            return true;
        }
        uint32 const revision = reg->DescriptorRevision();
        Map const* map = bot->GetMap();
        // R m-5: a bot that changed maps (a graveyard run, a teleport, an instance swap) and landed on a registry at
        // the same revision number must not keep the old map's descriptor.
        bool const sameMap = view.mapId == map->GetId() && view.instanceId == map->GetInstanceId();
        if (sameMap && revision == view.seenRevision)
            return false;
        view.seenRevision = revision;
        view.mapId = map->GetId();
        view.instanceId = map->GetInstanceId();
        if (!sameMap)
            view.owner = ObjectGuid::Empty;     // the copy below is retaken, even at the same owner guid and stamp

        bool const instance = bot->GetMap()->IsDungeon();
        Encounter const* best = nullptr;
        float bestDist = std::numeric_limits<float>::max();
        reg->ForEachEncounter([&](Encounter const& e)
        {
            if (e.ending || !e.described || !ScopeReaches(e, bot, instance))
                return;
            Unit const* owner = ObjectAccessor::GetUnit(*bot, e.owner);
            float const dist = owner ? bot->GetExactDist(owner) : std::numeric_limits<float>::max() / 2.f;
            if (!best || dist < bestDist)
            {
                best = &e;
                bestDist = dist;
            }
        });
        if (!best)
        {
            Clear(view);
            return true;
        }
        if (best->owner != view.owner || best->descriptorStamp != view.stamp)
        {
            view.descriptor = best->descriptor;
            view.refs = Desc::Referenced(view.descriptor);
            view.owner = best->owner;
            view.stamp = best->descriptorStamp;
            ++view.copies;
        }
        view.units = best->units;
        view.held = best->held;
        view.notes = reg->CastNotes().All();
        view.facts.clear();
        view.factsAt = Milliseconds(-1);
        return true;
    }

    Desc::UnitFacts const& FactsOf(DescriptorView& view, Registry const& reg, Unit const* unit, Milliseconds now)
    {
        static Desc::UnitFacts const kNone;
        if (!unit)
            return kNone;
        if (view.factsAt != now)
        {
            view.facts.clear();
            view.factsAt = now;
        }
        uint64 const key = unit->GetGUID().GetRawValue();
        auto [it, inserted] = view.facts.try_emplace(key);
        Desc::UnitFacts& f = it->second;
        if (!inserted)
            return f;
        f.guid = key;
        f.entry = unit->GetEntry();
        f.alive = unit->IsAlive();
        f.healthPct = unit->GetHealthPct();
        f.player = unit->IsPlayer();
        f.charmed = unit->IsCharmed();
        for (uint32 aura : view.refs.auras)
            if (unit->HasAura(aura))
                f.auras.push_back(aura);
        for (auto const& [entry, yd] : view.refs.near)
        {
            std::list<Creature*> found;
            unit->GetCreatureListWithEntryInGrid(found, entry, yd);
            float nearest = -1.f;
            for (Creature const* c : found)
            {
                if (c == unit || !c->IsAlive())
                    continue;
                float const d = unit->GetExactDist(c);
                if (nearest < 0.f || d < nearest)
                    nearest = d;
            }
            if (nearest >= 0.f)
                f.near.emplace_back(entry, nearest);
        }
        // Stratholme X1 (NearPlayer): the nearest other live player on the unit's map within the largest range asked.
        if (view.refs.playerRange > 0.f && unit->IsInWorld())
            if (Map* map = unit->GetMap())
                for (MapReference const& ref : map->GetPlayers())
                {
                    Player const* p = ref.GetSource();
                    if (!p || p == unit || !p->IsAlive() || !p->IsInWorld())
                        continue;
                    float const d = unit->GetExactDist(p);
                    if (d <= view.refs.playerRange && (f.nearestPlayerYd < 0.f || d < f.nearestPlayerYd))
                        f.nearestPlayerYd = d;
                }
        for (uint32 id : view.refs.zones)
            if (ActiveMechanic const* m = reg.Find(id))
                if (!m->cancelled && !m->finished && reg.ContainsUnit(*m, unit))
                    f.zones.push_back(id);
        // R m-4: a script's lambda may capture its boss; not once the owner left the world (the encounter ends one
        // registry tick later). A Custom condition not evaluated does not hold.
        if (!view.refs.custom.empty())
        {
            Unit const* owner = ObjectAccessor::GetUnit(*unit, view.owner);
            if (owner && owner->IsInWorld())
                for (Condition const* c : view.refs.custom)
                    f.custom.emplace_back(c, c->fn(unit));
        }
        return f;
    }

    Desc::Context ContextOf(DescriptorView const& view, WorldObject const* near)
    {
        Desc::Context ctx;
        ctx.phase = view.descriptor.phase;
        if (near && view.Active())
            if (Unit const* owner = ObjectAccessor::GetUnit(*near, view.owner))
                ctx.ownerHealthPct = owner->GetHealthPct();
        return ctx;
    }

    Desc::HeldRank HeldRankOf(DescriptorView const& view, WorldObject const* near, ObjectGuid prison)
    {
        ObjectGuid const held = view.HeldBy(prison);
        Player const* player = near && !held.IsEmpty() ? ObjectAccessor::GetPlayer(*near, held) : nullptr;
        if (!player)
            return Desc::HeldRank::None;
        Unit const* owner = ObjectAccessor::GetUnit(*near, view.owner);
        Role const role = GetRoleProvider().Get(owner, player);
        if (HasRole(role, Role::Tank))
            return Desc::HeldRank::Tank;
        if (HasRole(role, Role::Healer))
            return Desc::HeldRank::Healer;
        return Desc::HeldRank::Other;
    }
}
