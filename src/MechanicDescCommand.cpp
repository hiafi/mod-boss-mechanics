/*
 * mod-boss-mechanics: `.mech desc` (STAGE-M5-HANDOFF §3.6, WP-K). See MechanicDescCommand.h.
 *
 * Reads the registry only (Registry::FindEncounterOfUnit, Encounter::descriptor / units / held, CastNotes, Claims):
 * what a script or a program's `describe` set, not a bot's view of it (`.mech bots` prints the bots' side).
 */

#include "MechanicDescCommand.h"
#include "Chat.h"
#include "Creature.h"
#include "GameTime.h"
#include "Map.h"
#include "MechanicRegistry.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <string>
#include <vector>

namespace BossMechanics
{
    namespace
    {
        std::string SpellLabel(uint32 id)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            return Acore::StringFormat("{} ({})", id, info && info->SpellName[0] ? info->SpellName[0] : "?");
        }

        std::string UnitLabel(Map* map, uint64 raw)
        {
            ObjectGuid const guid(raw);
            if (Creature const* c = map->GetCreature(guid))
                return Acore::StringFormat("{} {} {:.0f}%{}", c->GetName(), guid.ToString(), c->GetHealthPct(),
                    c->IsAlive() ? "" : " dead");
            if (Player const* p = ObjectAccessor::FindConnectedPlayer(guid))
                return p->GetName();
            return guid.ToString();
        }

        std::string WhoName(Who who)
        {
            switch (who)
            {
                case Who::Any:
                    return "any";
                case Who::Melee:
                    return "melee";
                case Who::Ranged:
                    return "ranged";
                case Who::Tank:
                    return "tank";
                case Who::Healer:
                    return "healer";
                case Who::Dps:
                    return "dps";
            }
            return "?";
        }

        std::string When(Condition const& c);

        // A nested condition's text without its " when" (" always" for Always).
        std::string Inner(Condition const& c)
        {
            std::string const s = When(c);
            return s.empty() ? std::string(" always") : s.substr(5);
        }

        // "" for Always; otherwise " when <condition>".
        std::string When(Condition const& c)
        {
            switch (c.kind)
            {
                case CondKind::Always:
                    return "";
                case CondKind::HasAura:
                    return " when aura " + SpellLabel(c.id);
                case CondKind::LacksAura:
                    return " when no aura " + SpellLabel(c.id);
                case CondKind::NearUnit:
                    return Acore::StringFormat(" when within {:.0f} yd of entry {}", c.value, c.id);
                case CondKind::InZone:
                    return Acore::StringFormat(" when in mechanic #{}", c.id);
                case CondKind::Phase:
                    return Acore::StringFormat(" when phase {}", c.id);
                case CondKind::OwnerBelowPct:
                    return Acore::StringFormat(" when the owner is below {:.0f}%", c.value);
                case CondKind::NearPlayer:
                    return Acore::StringFormat(" when a player is within {:.0f} yd", c.value);
                case CondKind::All:
                {
                    std::string out;
                    for (Condition const& sub : c.all)
                        out += (out.empty() ? "" : " and") + Inner(sub);
                    return out.empty() ? std::string() : " when" + out;
                }
                case CondKind::Not:
                    return c.all.empty() ? std::string() : " when not (" + Inner(c.all.front()) + " )";
                case CondKind::Custom:
                    return " when (script condition)";
            }
            return "";
        }

        std::string UnitOrEntry(Map* map, uint32 entry, uint64 guid)
        {
            if (guid)
                return "unit " + UnitLabel(map, guid);
            return entry ? Acore::StringFormat("entry {}", entry) : std::string("any unit");
        }

        bool BelongsTo(Encounter const& e, uint64 raw)
        {
            if (e.owner.GetRawValue() == raw)
                return true;
            for (EncounterUnit const& u : e.units)
                if (u.guid.GetRawValue() == raw)
                    return true;
            return false;
        }

        void PrintEncounter(ChatHandler* handler, Map* map, Registry const& reg, Encounter const& e)
        {
            Descriptor const& d = e.descriptor;
            Milliseconds const now = GameTime::GetGameTimeMS();
            handler->PSendSysMessage("Descriptor of {}: phase {}, {} interrupt(s), {} dispel(s), {} purge(s), "
                "{} target(s), {} ignore(s), stamp {}", UnitLabel(map, e.owner.GetRawValue()), d.phase,
                d.interrupts.size(), d.dispels.size(), d.purges.size(), d.targets.size(), d.ignores.size(),
                e.descriptorStamp);
            for (InterruptEntry const& i : d.interrupts)
                handler->PSendSysMessage("  kick {} by {}: {}{}{}", SpellLabel(i.spellId), UnitOrEntry(map,
                    i.casterEntry, 0), i.priority ? Acore::StringFormat("priority {}", int32(i.priority))
                    : std::string("DO NOT KICK"), i.every.count() ? Acore::StringFormat(", every {} ms",
                    i.every.count()) : std::string(), When(i.when));
            for (DispelEntry const& x : d.dispels)
                handler->PSendSysMessage("  dispel {}: priority {}{}{}{}", SpellLabel(x.auraId), int32(x.priority),
                    x.urgent ? ", urgent" : "", x.deadline.count() ? Acore::StringFormat(", deadline {} ms",
                    x.deadline.count()) : std::string(), When(x.when));
            for (PurgeEntry const& x : d.purges)
                handler->PSendSysMessage("  purge {} on {}: priority {}", SpellLabel(x.auraId), UnitOrEntry(map,
                    x.unitEntry, 0), int32(x.priority));
            for (TargetEntry const& t : d.targets)
                handler->PSendSysMessage("  target {}: priority {:+}, {}{}{}{}{}", t.hostilePlayers
                    ? std::string("hostile players") : UnitOrEntry(map, t.entry, t.guid),
                    int32(t.priority), WhoName(t.who), t.aoeOk ? ", AoE ok" : "", t.partnerEntry
                    ? Acore::StringFormat(", together with {} within {:.0f}%", t.partnerEntry, t.togetherPct)
                    : std::string(), t.breakOut ? ", break-out" : "", When(t.when));
            for (IgnoreEntry const& x : d.ignores)
                handler->PSendSysMessage("  ignore {}{}", UnitOrEntry(map, x.entry, x.guid), When(x.when));
            for (uint32 aura : d.hostilePlayerAuras)
                handler->PSendSysMessage("  hostile player aura {} (a charmed teammate with it is a target)",
                    SpellLabel(aura));
            for (EncounterUnit const& u : e.units)
                handler->PSendSysMessage("  unit {} (entry {}){}", UnitLabel(map, u.guid.GetRawValue()), u.entry,
                    u.despawnAtEnd ? ", despawns at the end" : "");
            for (HeldPlayer const& h : e.held)
                handler->PSendSysMessage("  held {} by {}{}", UnitLabel(map, h.player.GetRawValue()), UnitLabel(map,
                    h.prison.GetRawValue()), h.aura ? ", aura " + SpellLabel(h.aura) : std::string());
            for (Desc::CastNote const& n : reg.CastNotes().All())
            {
                if (!BelongsTo(e, n.caster))
                    continue;
                handler->PSendSysMessage("  cast note {} by {}: next {}, last start {}", SpellLabel(n.spellId),
                    UnitLabel(map, n.caster), n.nextAt ? Acore::StringFormat("in {} ms", (*n.nextAt - now).count())
                    : std::string("-"), n.lastStart ? Acore::StringFormat("{} ms ago", (now - *n.lastStart).count())
                    : std::string("-"));
            }
            for (Desc::Claim const& c : reg.Claims().All())
            {
                if (!BelongsTo(e, c.cast.caster) || c.expires <= now)
                    continue;
                handler->PSendSysMessage("  claim {} by {} (started {} ms ago): {} for {} ms more",
                    SpellLabel(c.cast.spellId), UnitLabel(map, c.cast.caster), (now - c.cast.start).count(),
                    UnitLabel(map, c.bot), (c.expires - now).count());
            }
        }
    }

    bool HandleMechDescCommand(ChatHandler* handler)
    {
        Player* gm = handler->GetPlayer();
        Map* map = gm->GetMap();
        Registry const* reg = Registry::Get(map);
        if (!reg)
        {
            handler->PSendSysMessage("No boss mechanics registry on this map.");
            return true;
        }
        // The selected creature's encounter (as owner or tracked unit); else every described encounter on the map.
        if (Creature const* selected = handler->getSelectedCreature())
        {
            Encounter const* e = reg->FindEncounterOfUnit(selected->GetGUID());
            if (!e || !e->described)
            {
                handler->PSendSysMessage("{} has no described encounter.", selected->GetName());
                return true;
            }
            PrintEncounter(handler, map, *reg, *e);
            return true;
        }
        uint32 shown = 0;
        reg->ForEachEncounter([&](Encounter const& e)
        {
            if (!e.described || e.ending)
                return;
            ++shown;
            PrintEncounter(handler, map, *reg, e);
        });
        handler->PSendSysMessage("{} described encounter(s) on this map (revision {}).", shown,
            reg->DescriptorRevision());
        return true;
    }
}
