/*
 * mod-boss-mechanics: bots throw and catch a held item or aura (Blackrock Spire build, WP-PS). See MechanicBotPass.h;
 * the rules are MechanicPass.h.
 *
 * Throw by item after mod-playerbots' Lady Vashj Tainted Core chain (src/Ai/Raid/SSC/SSCActions.cpp,
 * LadyVashjPassTheTaintedCoreAction::ThrowCore; author Crow, pengchengw@me.com): the item used on the catcher through
 * PlayerbotAI::ImbueItem after a range and line of sight check, and a catcher that does not catch is left out. Here
 * the catcher is picked by the boss's rules instead of a planned chain, and the bookkeeping is per map (MechanicPass.h
 * Slot) instead of per instance.
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotPass.h"
#include "BossMechanicsConfig.h"
#include "Creature.h"
#include "DataMap.h"
#include "GameTime.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MechanicBotBrain.h"
#include "MechanicBotState.h"
#include "MechanicBotTauntSwap.h"
#include "MechanicBotView.h"
#include "MechanicDescriptorView.h"
#include "MechanicRegistry.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include <algorithm>
#include <cmath>
#include <list>
#include <vector>

using namespace BossMechanics;

namespace
{
    constexpr char const* kLog = "module.bossmechanics.bots";
    constexpr char const* kBookKey = "mod-boss-mechanics-pass";
    // The book is brought up to date at most this often (at once on a change of holder or a throw).
    constexpr Milliseconds kRecompute{ 100 };
    // A book not brought up to date for this long starts over (a wipe and a new pull of the same boss).
    constexpr Milliseconds kStaleBook{ 2000 };
    // FarFrom units and catchers farther than this from the holder are not looked at.
    constexpr float kLookYd = 100.f;
    // Ready spots: at most this many probed per pick; a bot that found none waits this long before probing again.
    constexpr std::size_t kMaxSpotProbes = 8;
    constexpr Milliseconds kNoSpotRetry{ 1000 };
    // A probed spot must stay out of the bot's hazards this long from now.
    constexpr Milliseconds kSpotSafeFor{ 3000 };

    Milliseconds Now()
    {
        return GameTime::GetGameTimeMS();
    }

    Desc::Key KeyOf(Unit const* unit)
    {
        return unit ? unit->GetGUID().GetRawValue() : 0;
    }

    Geo::Vec3 PosOf(WorldObject const* o)
    {
        return { o->GetPositionX(), o->GetPositionY(), o->GetPositionZ() };
    }

    // What the book keeps per entry besides the pure slot: the facts the ready catchers read.
    struct SlotFacts
    {
        std::vector<Pass::Disc> zones;
        std::vector<Geo::Vec3> units;
        std::vector<Pass::Candidate> candidates;
        Geo::Vec3 holderPos;
        Milliseconds computedAt{ -1 };
        std::vector<std::pair<Desc::Key, Milliseconds>> noSpotUntil;    // bots that found no spot: retry after
    };

    // One per map (Map::CustomData), read and written on the map's thread only.
    struct PassBook : DataMap::Base
    {
        ObjectGuid owner;
        std::size_t entries = 0;
        Milliseconds updatedAt{ -1 };
        std::vector<Pass::Slot> slots;
        std::vector<SlotFacts> facts;
    };

    PassBook* BookOf(Map* map)
    {
        static std::string const key(kBookKey);
        return map ? map->CustomData.GetDefault<PassBook>(key) : nullptr;
    }

    PassBook const* FindBook(Map const* map)
    {
        static std::string const key(kBookKey);
        return map ? map->CustomData.Get<PassBook>(key) : nullptr;
    }

    // The unit a trigger or FarFrom names: entry 0 is the encounter owner; else every live creature of the entry
    // within kLookYd of `near`.
    void UnitsOf(Player const* near, ObjectGuid owner, uint32 entry, std::vector<Unit*>& out)
    {
        out.clear();
        if (!entry)
        {
            if (Unit* u = ObjectAccessor::GetUnit(*near, owner); u && u->IsInWorld() && u->IsAlive())
                out.push_back(u);
            return;
        }
        std::list<Creature*> list;
        near->GetCreatureListWithEntryInGrid(list, entry, kLookYd);
        for (Creature* c : list)
            if (c && c->IsInWorld() && c->IsAlive())
                out.push_back(c);
    }

    bool CastsNow(Unit const* u, uint32 spellId)
    {
        for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
            if (Spell const* s = u->GetCurrentSpell(type))
                if (s->GetSpellInfo() && s->GetSpellInfo()->Id == spellId)
                    return true;
        return false;
    }

    // The player holding the entry's aura now: the current holder while it still has it, else the last throw's
    // target, else the first one found (lowest guid). nullptr: nobody.
    Player* FindHolder(Map* map, PassEntry const& e, Pass::Slot const& s)
    {
        Player* found = nullptr;
        Player* last = nullptr;
        for (MapReference const& ref : map->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (!p || !p->IsInWorld() || !p->IsAlive() || !p->HasAura(e.holderAura))
                continue;
            Desc::Key const k = KeyOf(p);
            if (k == s.holder)
                return p;
            if (k == s.lastTarget)
                last = p;
            if (!found || k < KeyOf(found))
                found = p;
        }
        return last ? last : found;
    }

    // The counted zones (FewestZones): live mechanics with the tag, as discs (circles and donuts by their outer
    // radius; other shapes are not counted).
    void ZonesOf(Registry const& reg, PassEntry const& e, Milliseconds now, std::vector<Pass::Disc>& out)
    {
        out.clear();
        if (!e.zoneTag)
            return;
        reg.ForEachActive(now, [&](ActiveMechanic const& m)
        {
            if (m.tag != e.zoneTag || m.decoy)
                return;
            Geo::Pose pose;
            Shape const shape = reg.EffectiveShape(m, pose);
            if (shape.type != ShapeType::Circle && shape.type != ShapeType::Donut)
                return;
            out.push_back({ { pose.x, pose.y, pose.z }, shape.radius });
        });
    }

    // Brings one slot up to date: the holder, the catchers, the ready catchers, the throw due now.
    void UpdateSlot(Player* bot, DescriptorView& v, Registry const& reg, PassEntry const& e, Pass::Slot& s,
        SlotFacts& f, Milliseconds now, Desc::Context const& ctx)
    {
        Map* map = bot->GetMap();
        Unit* owner = ObjectAccessor::GetUnit(*bot, v.owner);
        bool const gateOpen = e.active.kind == CondKind::Always
            || (owner && Desc::Holds(e.active, FactsOf(v, reg, owner, now), ctx));
        Player* holder = gateOpen ? FindHolder(map, e, s) : nullptr;
        Desc::Key const holderKey = KeyOf(holder);
        bool const changed = holderKey != s.holder;
        Pass::Observe(s, holderKey, now, e.flight);
        if (!holder)
        {
            s.ready.clear();
            s.spots.clear();
            s.throwTo = 0;
            s.due = Pass::Due();
            f.candidates.clear();
            return;
        }
        if (!changed && f.computedAt >= Milliseconds(0) && now - f.computedAt < kRecompute
            && !(s.lastThrowAt >= Milliseconds(0) && s.lastThrowAt >= f.computedAt))
            return;
        f.computedAt = now;
        f.holderPos = PosOf(holder);

        // FarFrom units and the counted zones.
        f.units.clear();
        std::vector<Unit*> units;
        for (uint32 entry : e.farFrom)
        {
            UnitsOf(holder, v.owner, entry, units);
            for (Unit* u : units)
                f.units.push_back(PosOf(u));
        }
        ZonesOf(reg, e, now, f.zones);

        // The catchers: the holder's group on its map (humans too; alone: none).
        f.candidates.clear();
        Unit* tank = owner ? owner->GetVictim() : nullptr;
        if (Group* group = holder->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* m = ref->GetSource();
                if (!m || m == holder || !m->IsInWorld() || m->GetMap() != map || !m->IsAlive() || m->IsGameMaster()
                    || !holder->IsWithinDistInMap(m, kLookYd))
                    continue;
                Pass::Candidate c;
                c.guid = KeyOf(m);
                c.pos = PosOf(m);
                Desc::UnitFacts const& mf = FactsOf(v, reg, m, now);
                c.eligible = Desc::Holds(e.catchWhen, mf, ctx);
                c.preferred = e.preferSet && Desc::Holds(e.prefer, mf, ctx);
                c.excluded = Pass::Excluded(s, c.guid, now);
                c.holderYd = holder->GetDistance(m);
                c.inSight = !e.sight || !c.eligible || c.holderYd > e.range + 20.f || holder->IsWithinLOSInMap(m);
                // Only bots walk to a ready spot, and never the boss's tank (it keeps the boss where it is).
                c.bot = GET_PLAYERBOT_AI(m) != nullptr && m != tank;
                f.candidates.push_back(c);
            }
        }

        std::vector<Pass::Ranked> const ready = Pass::RankReady(e, f.candidates, f.zones, f.units);
        s.ready = Pass::PickReady(ready, s.ready, e.readyCatchers);
        Pass::PruneSpots(s);

        // The throw: which triggers fire now (HeldFor is judged by ThrowDue).
        std::vector<bool> fired(e.triggers.size(), false);
        for (std::size_t i = 0; i < e.triggers.size(); ++i)
        {
            PassTrigger const& t = e.triggers[i];
            if (t.kind == PassTrigger::Kind::UnitNear || t.kind == PassTrigger::Kind::Casting)
            {
                UnitsOf(holder, v.owner, t.entry, units);
                for (Unit* u : units)
                {
                    float const yd = holder->GetDistance(u);
                    if (t.kind == PassTrigger::Kind::UnitNear)
                    {
                        if (yd <= t.yd && (t.when.kind == CondKind::Always
                            || Desc::Holds(t.when, FactsOf(v, reg, u, now), ctx)))
                            fired[i] = true;
                    }
                    else if ((t.yd <= 0.f || yd <= t.yd) && CastsNow(u, t.id))
                        fired[i] = true;
                    if (fired[i])
                        break;
                }
            }
            else if (t.kind == PassTrigger::Kind::Mechanic)
            {
                reg.ForEachActive(now, [&](ActiveMechanic const& m)
                {
                    if (m.tag == t.id && (t.ms <= Milliseconds(0) || m.resolveAt <= now + t.ms))
                        fired[i] = true;
                });
            }
        }
        Milliseconds minHold{ 0 };
        Desc::UnitFacts const& hf = FactsOf(v, reg, holder, now);
        for (PassHold const& h : e.holds)
            if (h.ms > minHold && Desc::Holds(h.when, hf, ctx))
                minHold = h.ms;
        Milliseconds const held = s.heldSince >= Milliseconds(0) ? now - s.heldSince : Milliseconds(0);
        s.due = Pass::ThrowDue(e, fired, held, minHold, Pass::InFlight(s, holderKey, now, e.flight));
        s.throwTo = s.due.due ? Pass::ThrowTarget(Pass::RankThrow(e, f.candidates, f.zones, f.units), s.ready) : 0;
    }

    // The book for the bot's map, up to date for `now`.
    PassBook* UpdateBook(Player* bot, DescriptorView& v, Registry const& reg, Milliseconds now)
    {
        PassBook* book = BookOf(bot->GetMap());
        if (!book)
            return nullptr;
        std::vector<PassEntry> const& passes = v.descriptor.passes;
        // A new encounter, another descriptor shape, or a book nobody looked at for kStaleBook (the encounter ended
        // and a new pull began: the holder and its time held start over).
        if (book->owner != v.owner || book->entries != passes.size()
            || (book->updatedAt >= Milliseconds(0) && now - book->updatedAt > kStaleBook))
        {
            book->owner = v.owner;
            book->entries = passes.size();
            book->slots.assign(passes.size(), Pass::Slot());
            book->facts.assign(passes.size(), SlotFacts());
            book->updatedAt = Milliseconds(-1);
        }
        if (book->updatedAt == now)
            return book;
        book->updatedAt = now;
        Desc::Context const ctx = ContextOf(v, bot);
        for (std::size_t i = 0; i < passes.size(); ++i)
            UpdateSlot(bot, v, reg, passes[i], book->slots[i], book->facts[i], now, ctx);
        return book;
    }

    bool NoSpotWait(SlotFacts const& f, Desc::Key bot, Milliseconds now)
    {
        for (auto const& [k, until] : f.noSpotUntil)
            if (k == bot && now < until)
                return true;
        return false;
    }

    void SetNoSpot(SlotFacts& f, Desc::Key bot, Milliseconds until)
    {
        for (auto& [k, u] : f.noSpotUntil)
            if (k == bot)
            {
                u = until;
                return;
            }
        f.noSpotUntil.emplace_back(bot, until);
    }

    Pass::Candidate const* FindCandidate(SlotFacts const& f, Desc::Key k)
    {
        for (Pass::Candidate const& c : f.candidates)
            if (c.guid == k)
                return &c;
        return nullptr;
    }

    // A ready catcher's spot: hold where it stands when that is fit, else walk to its reserved spot, else pick one.
    void DecideSpot(Player* bot, MechanicBotState& st, PassEntry const& e, Pass::Slot& s, SlotFacts& f,
        Milliseconds now, Pass::Decision& d)
    {
        Desc::Key const self = KeyOf(bot);
        Pass::Candidate const* me = FindCandidate(f, self);
        if (!me || !me->bot)
            return;
        Geo::Vec3 const here = PosOf(bot);
        std::vector<Geo::Vec3> const others = Pass::OtherSpots(s, self);
        bool apartHere = true;
        for (Geo::Vec3 const& o : others)
            if (Geo::Dist2d(o.x, o.y, here.x, here.y) < Pass::kSpotApartYd)
                apartHere = false;
        Pass::Reserved* r = Pass::FindSpot(s, self);

        // At the reserved spot, or fit where it stands: hold (up to kSpotHoldMs from the arrival).
        bool const atSpot = r && Geo::Dist2d(r->spot.x, r->spot.y, here.x, here.y) <= Pass::kSpotArriveYd;
        bool const fitHere = Pass::SpotFit(e, here, f.holderPos, f.zones, f.units) && me->inSight && apartHere;
        if ((atSpot && Pass::SpotFit(e, r->spot, f.holderPos, f.zones, f.units)) || fitHere)
        {
            if (!r || !atSpot)
            {
                s.spots.erase(std::remove_if(s.spots.begin(), s.spots.end(), [self](Pass::Reserved const& x)
                {
                    return x.bot == self;
                }), s.spots.end());
                s.spots.push_back({ self, here, now, now });
                r = &s.spots.back();
            }
            if (r->arrivedAt < Milliseconds(0))
                r->arrivedAt = now;
            d.holdSpot = now - r->arrivedAt < Pass::kSpotHoldMs;
            d.spot = r->spot;
            return;
        }

        // On the way to a reserved spot that is still fit.
        if (r && now - r->at < Pass::kSpotKeepMs && Pass::SpotFit(e, r->spot, f.holderPos, f.zones, f.units)
            && (!st.viewValid || Solver::PointSafe(st.input, r->spot, now, now + kSpotSafeFor)))
        {
            d.move = true;
            d.spot = r->spot;
            return;
        }

        // Pick a new one: the cheapest candidates, probed on the navmesh, against the bot's hazards and for the
        // holder's sight.
        if (NoSpotWait(f, self, now))
            return;
        std::vector<Geo::Vec3> const ranked = Pass::RankSpots(e, here, f.holderPos, f.zones, f.units, others);
        // A spot is worth the walk only when it costs less than standing here (zones, KeepFrom, apart; out of the
        // holder's reach here costs everything).
        float const costHere = Pass::SpotCost(e, here, here, f.holderPos, f.zones, f.units, others)
            + (me->inSight ? 0.f : Pass::kSightPenaltyYd);
        NavOracle oracle(bot);
        Geo::Vec3 const holderEye{ f.holderPos.x, f.holderPos.y, f.holderPos.z };
        std::size_t probes = 0;
        for (Geo::Vec3 p : ranked)
        {
            if (probes++ >= kMaxSpotProbes)
                break;
            if (!oracle.Walkable(p))
                continue;
            // Snapped onto the navmesh: judged again where it landed (another floor, out of reach, no better).
            float const cost = Pass::SpotCost(e, p, here, f.holderPos, f.zones, f.units, others);
            if (!std::isfinite(cost) || cost >= costHere - 1.f)
                continue;
            if (st.viewValid && !Solver::PointSafe(st.input, p, now, now + kSpotSafeFor))
                continue;
            if (e.sight && !oracle.InSight(holderEye, p))
                continue;
            // A spot across a pit or on a ledge of its own is no spot: MoveTo would run a partial path to the rim.
            std::vector<Geo::Vec3> path;
            if (!oracle.Find(here, p, path) || path.empty())
                continue;
            s.spots.erase(std::remove_if(s.spots.begin(), s.spots.end(), [self](Pass::Reserved const& x)
            {
                return x.bot == self;
            }), s.spots.end());
            s.spots.push_back({ self, p, now, Milliseconds(-1) });
            ++st.pass.spotsPicked;
            d.move = true;
            d.spot = p;
            return;
        }
        ++st.pass.noSpot;
        SetNoSpot(f, self, now + kNoSpotRetry);
    }

    std::string Name(Player const* bot, Desc::Key key)
    {
        if (!key)
            return "-";
        if (Unit const* u = ObjectAccessor::GetUnit(*bot, ObjectGuid(key)))
            return u->GetName();
        return ObjectGuid(key).ToString();
    }

    // The spell an item uses on its target (the first on-use spell).
    uint32 ItemSpellOf(Item const* item)
    {
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            if (item->GetTemplate()->Spells[i].SpellId > 0)
                return uint32(item->GetTemplate()->Spells[i].SpellId);
        return 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// BotPass
// ---------------------------------------------------------------------------------------------------------------------

Pass::Decision const& BotPass::Refresh(PlayerbotAI* botAI, MechanicBotState& st, Milliseconds now)
{
    Pass::BotState& ps = st.pass;
    if (ps.decidedAt == now)
        return ps.decision;
    ps.decidedAt = now;
    ps.decision = Pass::Decision();
    ps.entries = false;

    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !bot->IsInWorld() || !bot->IsAlive())
        return ps.decision;
    Registry const* reg = Registry::Get(bot->GetMap());
    if (!reg)
        return ps.decision;
    RefreshDescriptorView(reg, bot, st.descriptor);
    DescriptorView& v = st.descriptor;
    if (!v.Active() || v.descriptor.passes.empty())
        return ps.decision;
    ps.entries = true;
    PassBook* book = UpdateBook(bot, v, *reg, now);
    if (!book)
        return ps.decision;

    Desc::Key const self = KeyOf(bot);
    for (std::size_t i = 0; i < book->slots.size() && i < v.descriptor.passes.size(); ++i)
    {
        Pass::Slot& s = book->slots[i];
        if (s.holder == self)
        {
            ps.decision.role = Pass::Role::Holder;
            ps.decision.slot = i;
            ps.decision.throwTo = s.throwTo;
            ps.decision.why = s.due.why;
            return ps.decision;
        }
    }
    for (std::size_t i = 0; i < book->slots.size() && i < v.descriptor.passes.size(); ++i)
    {
        Pass::Slot& s = book->slots[i];
        if (!s.holder || std::find(s.ready.begin(), s.ready.end(), self) == s.ready.end())
            continue;
        ps.decision.role = Pass::Role::Ready;
        ps.decision.slot = i;
        DecideSpot(bot, st, v.descriptor.passes[i], s, book->facts[i], now, ps.decision);
        return ps.decision;
    }
    return ps.decision;
}

void BotPass::AfterTick(PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Milliseconds now)
{
    bool const moving = st.pass.decision.move;
    Pass::Decision const& d = Refresh(botAI, st, now);
    if (d.role == Pass::Role::Holder && d.throwTo)
    {
        // The throw outranks a cast in progress (the burn rises while it casts).
        if (Spell const* spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            spell && spell->getState() == SPELL_STATE_PREPARING)
        {
            botAI->RequestSpellInterrupt();
            ++st.pass.castBreaks;
        }
        botAI->SetNextCheckDelay(0);
    }
    else if (d.role == Pass::Role::Ready && d.move && !moving)
        botAI->SetNextCheckDelay(0);
}

std::string BotPass::ReportLine(PlayerbotAI* botAI, MechanicBotState const& st)
{
    Pass::BotState const& ps = st.pass;
    Pass::Decision const& d = ps.decision;
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return "ps pass: no bot";
    char const* role = d.role == Pass::Role::Holder ? "holder" : d.role == Pass::Role::Ready ? "ready" : "-";
    std::string book;
    if (PassBook const* b = FindBook(bot->GetMap()))
        for (Pass::Slot const& s : b->slots)
            book += Acore::StringFormat(" [holder {}, ready {}, throws {}, catches {}, failed {}, other {}, drops {}]",
                Name(bot, s.holder), s.ready.empty() ? std::string("-") : Name(bot, s.ready.front()), s.throws,
                s.catches, s.failed, s.otherChanges, s.drops);
    return Acore::StringFormat("ps pass: {} | {} {}{}{} | throws {}, refused {}, casts broken {}, walks {}, spots "
        "picked {}, no spot {}, moves held {} |{}", ps.entries ? "entries" : "no entries", role,
        d.throwTo ? "throws to " + Name(bot, d.throwTo) + " (" + Pass::WhyName(d.why) + ")" : std::string(""),
        d.move ? "walking" : "", d.holdSpot ? "holding its spot" : "", ps.throws, ps.refused, ps.castBreaks,
        ps.walks, ps.spotsPicked, ps.noSpot, ps.blocked, book.empty() ? std::string(" no book") : book);
}

// ---------------------------------------------------------------------------------------------------------------------
// Triggers, actions, multiplier
// ---------------------------------------------------------------------------------------------------------------------

bool BossPassThrowTrigger::IsActive()
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return false;
    Pass::Decision const& d = BotPass::Refresh(botAI, *st, Now());
    return d.role == Pass::Role::Holder && d.throwTo;
}

bool BossPassThrowAction::isUseful()
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return false;
    Pass::Decision const& d = BotPass::Refresh(botAI, *st, Now());
    return d.role == Pass::Role::Holder && d.throwTo;
}

bool BossPassThrowAction::Execute(Event /*event*/)
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return false;
    Milliseconds const now = Now();
    Pass::Decision const d = BotPass::Refresh(botAI, *st, now);
    if (d.role != Pass::Role::Holder || !d.throwTo || d.slot >= st->descriptor.descriptor.passes.size())
        return false;
    PassEntry const& e = st->descriptor.descriptor.passes[d.slot];
    Player* target = ObjectAccessor::GetPlayer(*bot, ObjectGuid(d.throwTo));
    PassBook* book = BookOf(bot->GetMap());
    if (!target || !target->IsAlive() || !book || d.slot >= book->slots.size())
        return false;
    Pass::Slot& s = book->slots[d.slot];

    bool sent = false;
    switch (e.how)
    {
        case PassEntry::Throw::Item:
        {
            Item* item = e.item ? bot->GetItemByEntry(e.item) : nullptr;
            if (!item || bot->CanUseItem(item) != EQUIP_ERR_OK || !ItemSpellOf(item))
                break;
            uint32 const itemSpell = ItemSpellOf(item);
            if (bot->HasSpellCooldown(itemSpell))
                break;
            if (bot->IsNonMeleeSpellCast(false))
                bot->InterruptNonMeleeSpells(false);
            botAI->ImbueItem(item, target);
            sent = true;
            break;
        }
        case PassEntry::Throw::Spell:
        {
            if (!e.spell)
                break;
            if (bot->IsNonMeleeSpellCast(false))
                bot->InterruptNonMeleeSpells(false);
            if (bot->HasSpell(e.spell))
                sent = botAI->CanCastSpell(e.spell, target, true) && botAI->CastSpell(e.spell, target);
            else
                sent = bot->CastSpell(target, e.spell, false) == SPELL_CAST_OK;
            break;
        }
        case PassEntry::Throw::MoveAura:
        {
            Aura const* held = bot->GetAura(e.holderAura);
            if (!held)
                break;
            uint8 const stacks = std::max<uint8>(held->GetStackAmount(), 1);
            bot->RemoveAurasDueToSpell(e.holderAura);
            if (Aura* caught = target->AddAura(e.holderAura, target))
                caught->SetStackAmount(stacks);
            if (e.afterAura)
                if (Aura* after = bot->AddAura(e.afterAura, bot); after && e.afterMs > Milliseconds(0))
                {
                    after->SetMaxDuration(int32(e.afterMs.count()));
                    after->SetDuration(int32(e.afterMs.count()));
                }
            sent = true;
            break;
        }
    }

    if (!sent)
    {
        ++st->pass.refused;
        if (GetConfig().bots.debug)
            LOG_INFO(kLog, "Bot {}: pass throw to {} refused (no item, cooldown or cast refused)", bot->GetName(),
                target->GetName());
        return false;
    }
    Pass::NoteThrow(s, KeyOf(bot), d.throwTo, now);
    ++st->pass.throws;
    st->pass.decidedAt = Milliseconds(-1);
    if (GetConfig().bots.debug)
        LOG_INFO(kLog, "Bot {}: pass throw to {} ({}, held {} ms)", bot->GetName(), target->GetName(),
            Pass::WhyName(d.why), s.heldSince >= Milliseconds(0) ? (now - s.heldSince).count() : 0);
    return true;
}

bool BossPassCatchTrigger::IsActive()
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return false;
    Pass::Decision const& d = BotPass::Refresh(botAI, *st, Now());
    return d.role == Pass::Role::Ready && d.move;
}

bool BossPassCatchAction::isUseful()
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return false;
    Pass::Decision const& d = BotPass::Refresh(botAI, *st, Now());
    return d.role == Pass::Role::Ready && d.move;
}

bool BossPassCatchAction::Execute(Event /*event*/)
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return false;
    Pass::Decision const d = BotPass::Refresh(botAI, *st, Now());
    if (d.role != Pass::Role::Ready || !d.move)
        return false;
    // Already walking there: let the spline run.
    if (Movement::MoveSpline const* ms = bot->movespline; ms && ms->Initialized() && !ms->Finalized())
    {
        G3D::Vector3 const end = ms->FinalDestination();
        if (Geo::Dist2d(end.x, end.y, d.spot.x, d.spot.y) <= 1.f)
            return false;
    }
    if (!MoveTo(bot->GetMapId(), d.spot.x, d.spot.y, d.spot.z, false, false, false, false,
        MovementPriority::MOVEMENT_COMBAT))
        return false;
    ++st->pass.walks;
    if (GetConfig().bots.debug)
        LOG_INFO(kLog, "Bot {}: walks to a pass catch spot ({:.1f}, {:.1f})", bot->GetName(), d.spot.x, d.spot.y);
    return true;
}

float BossPassCatchMultiplier::GetValue(Action* action)
{
    if (!action || dynamic_cast<BossPassCatchAction*>(action) || dynamic_cast<BossPassThrowAction*>(action)
        || dynamic_cast<MechanicBotMoveAction*>(action) || dynamic_cast<MechanicBotHoldAction*>(action)
        || dynamic_cast<MechanicBotFaceAction*>(action) || dynamic_cast<BossTauntSwapAction*>(action))
        return 1.f;
    if (!dynamic_cast<MovementAction*>(action))
        return 1.f;
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return 1.f;
    Pass::Decision const& d = BotPass::Refresh(botAI, *st, Now());
    if (d.role != Pass::Role::Ready || !d.holdSpot)
        return 1.f;
    ++st->pass.blocked;
    return 0.f;
}

#endif
