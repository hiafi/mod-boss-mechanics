/*
 * mod-boss-mechanics: bots trade a boss at a debuff's stack count (Blackrock Spire build, WP-TS). See
 * MechanicBotTauntSwap.h; the rules are MechanicTauntSwap.h.
 *
 * Precedents: mod-playerbots' Gluth swap (src/Ai/Raid/Naxx, the assist tank taunts at 5 Mortal Wound stacks) and
 * Al'ar's tank swap (src/Ai/Raid/TK); both are per raid boss and cast through the class's "taunt spell" node, which
 * the reworked Rogue and Shaman tanks do not have. Here the taunt is picked from the spell id table instead.
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotTauntSwap.h"
#include "BossMechanicsConfig.h"
#include "Creature.h"
#include "GameTime.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MechanicBotBrain.h"
#include "MechanicBotInterrupt.h"
#include "MechanicBotState.h"
#include "MechanicDescriptorView.h"
#include "MechanicKickRules.h"
#include "MechanicRegistry.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <algorithm>
#include <vector>

using namespace BossMechanics;

namespace
{
    constexpr char const* kLog = "module.bossmechanics.bots";
    // A swap boss further than this from the bot is not judged (another room).
    constexpr float kMaxBossYd = 80.f;

    Milliseconds Now()
    {
        return GameTime::GetGameTimeMS();
    }

    Desc::Key KeyOf(Unit const* unit)
    {
        return unit ? unit->GetGUID().GetRawValue() : 0;
    }

    // The debuff's stacks on `tank`: those `boss` put on it, or (AnyCaster) from every caster.
    uint8 StacksOn(Player const* tank, Unit const* boss, TauntSwapEntry const& e)
    {
        if (!e.debuffSpell)
            return 0;
        if (e.anyCaster)
            return static_cast<uint8>(std::min<uint32>(tank->GetAuraCount(e.debuffSpell), 255));
        Aura const* aura = tank->GetAura(e.debuffSpell, boss->GetGUID());
        return aura ? std::max<uint8>(aura->GetStackAmount(), 1) : 0;
    }

    // A taunt the swap would cast is known and off cooldown (any player: the pick asks it of every tank): one at the
    // boss itself (`atVictim` false) or Righteous Defense, cast at the victim (`atVictim` true).
    bool TauntReady(Player const* p, bool tank, bool atVictim)
    {
        for (TauntSwap::TauntSpell const& t : TauntSwap::kTable)
            if ((t.kind == TauntSwap::TauntKind::Friend) == atVictim && TauntSwap::Castable(t, tank)
                && p->HasSpell(t.id) && !p->HasSpellCooldown(t.id))
                return true;
        return false;
    }

    // The caster of the last taunt aura on `boss` that is alive and on its threat list (0: none), as the threat
    // manager picks it (ThreatManager::TauntUpdate): TauntSwap::EffectiveVictim (R-1-FW m-3).
    Desc::Key LastTaunter(Unit* boss)
    {
        Unit::AuraEffectList const& taunts = boss->GetAuraEffectsByType(SPELL_AURA_MOD_TAUNT);
        for (auto it = taunts.rbegin(); it != taunts.rend(); ++it)
            if (Unit* caster = (*it)->GetCaster(); caster && caster->IsInWorld() && caster->IsAlive()
                && boss->IsThreatenedBy(caster))
                return KeyOf(caster);
        return 0;
    }

    // The taunt reaches `target`: melee range for a melee-range spell, else the spell's range; line of sight.
    bool Reaches(Player const* bot, SpellInfo const* info, Unit const* target)
    {
        if (!info || !target)
            return false;
        bool const melee = info->RangeEntry && (info->RangeEntry->Flags & SPELL_RANGE_MELEE);
        bool const inRange = melee ? bot->IsWithinMeleeRange(target)
            : bot->IsWithinDistInMap(target, info->GetMaxRange(info->IsPositive()));
        return inRange && bot->IsWithinLOSInMap(target);
    }

    // The spell a stock cast action casts (the lock's rule: GetCastSpellId when the fork has it, else the name).
    uint32 CastSpellIdOf(PlayerbotAI* botAI, CastSpellAction* cast)
    {
#if defined(PLAYERBOTS_HAS_CAST_SPELL_ID)
        (void)botAI;
        return cast->GetCastSpellId();
#else
        AiObjectContext* context = botAI->GetAiObjectContext();
        return AI_VALUE2(uint32, "spell id", cast->getSpell());
#endif
    }

    std::string Name(Player const* bot, Desc::Key key)
    {
        if (!key)
            return "-";
        if (Unit const* u = ObjectAccessor::GetUnit(*bot, ObjectGuid(key)))
            return u->GetName();
        return ObjectGuid(key).ToString();
    }

    char const* WhyName(TauntSwap::Why why)
    {
        switch (why)
        {
            case TauntSwap::Why::Stacks:
                return "stacks";
            case TauntSwap::Why::Trade:
                return "trade";
            default:
                return "-";
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// BotTauntSwap
// ---------------------------------------------------------------------------------------------------------------------

TauntSwap::Decision const& BotTauntSwap::Refresh(PlayerbotAI* botAI, MechanicBotState& st, Milliseconds now)
{
    TauntSwap::State& ts = st.tauntSwap;
    if (ts.decidedAt == now)
        return ts.decision;
    ts.decidedAt = now;
    ts.decision = TauntSwap::Decision();
    ts.entries = false;

    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !bot->IsInWorld() || !bot->IsAlive())
        return ts.decision;
    Registry const* reg = Registry::Get(bot->GetMap());
    if (!reg)
        return ts.decision;
    RefreshDescriptorView(reg, bot, st.descriptor);
    DescriptorView& v = st.descriptor;
    if (!v.Active() || v.descriptor.tauntSwaps.empty())
        return ts.decision;
    ts.entries = true;
    if (!PlayerbotAI::IsTank(bot) || !bot->IsInCombat())
        return ts.decision;

    // The swap bosses: the encounter's live units in combat an entry names, its `when` holding (on the view's copy,
    // so a Custom condition's result is found).
    Desc::Context const ctx = ContextOf(v, bot);
    std::vector<ObjectGuid> guids{ v.owner };
    for (EncounterUnit const& u : v.units)
        guids.push_back(u.guid);
    std::vector<TauntSwap::BossFacts> bosses;
    std::vector<Unit*> bossUnits;
    std::vector<TauntSwapEntry const*> bossEntries;
    Desc::Key const owner = v.owner.GetRawValue();
    for (ObjectGuid const& g : guids)
    {
        Unit* unit = ObjectAccessor::GetUnit(*bot, g);
        if (!unit || !unit->IsInWorld() || !unit->IsAlive() || !unit->IsInCombat()
            || !bot->IsWithinDistInMap(unit, kMaxBossYd))
            continue;
        for (TauntSwapEntry const& e : v.descriptor.tauntSwaps)
        {
            if (!TauntSwap::Matches(e, g.GetRawValue(), unit->GetEntry(), owner))
                continue;
            if (!Desc::Holds(e.when, FactsOf(v, *reg, unit, now), ctx))
                continue;
            bosses.push_back({ g.GetRawValue(), TauntSwap::EffectiveVictim(KeyOf(unit->GetVictim()),
                LastTaunter(unit)), e.swapAtStacks });
            bossUnits.push_back(unit);
            bossEntries.push_back(&e);
            break;
        }
    }
    if (bosses.empty())
        return ts.decision;

    // The tanks: the bot's group on its map (humans too); alone, the bot.
    std::vector<Player*> members;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (m && m->IsInWorld() && m->GetMap() == bot->GetMap() && PlayerbotAI::IsTank(m))
                members.push_back(m);
        }
    }
    else
        members.push_back(bot);

    std::vector<TauntSwap::TankFacts> tanks;
    tanks.reserve(members.size());
    for (Player* m : members)
    {
        TauntSwap::TankFacts t;
        t.guid = m->GetGUID().GetRawValue();
        t.alive = m->IsAlive();
        t.ready = t.alive && TauntReady(m, true, false);
        t.friendReady = t.alive && TauntReady(m, true, true);
        t.stacks.reserve(bosses.size());
        for (std::size_t i = 0; i < bosses.size(); ++i)
            t.stacks.push_back(StacksOn(m, bossUnits[i], *bossEntries[i]));
        tanks.push_back(std::move(t));
    }

    Desc::Key const self = bot->GetGUID().GetRawValue();
    ts.decision = TauntSwap::Decide(bosses, tanks, self);
    if (TauntSwap::TankFacts const* me = TauntSwap::FindTank(tanks, self))
        for (std::size_t i = 0; i < bosses.size(); ++i)
            if (bosses[i].victim == self)
                ts.maxStacks = std::max(ts.maxStacks, TauntSwap::StacksOf(*me, i));
    return ts.decision;
}

void BotTauntSwap::AfterTick(PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Milliseconds now)
{
    TauntSwap::Decision const& d = Refresh(botAI, st, now);
    if (d.holdOff.empty())
        return;
    TauntSwap::State& ts = st.tauntSwap;
    bool const debug = GetConfig().bots.debug;

    if (Unit* victim = bot->GetVictim(); victim && d.HeldOff(KeyOf(victim)))
    {
        if (debug)
            LOG_INFO(kLog, "Bot {}: keeps off {} (another tank holds it): melee stopped", bot->GetName(),
                victim->GetName());
        bot->AttackStop();
        ++ts.holdOffDrops;
    }
    if (Value<Unit*>* currentValue = botAI->GetAiObjectContext()->GetValue<Unit*>("current target"))
        if (Unit* current = currentValue->Get(); current && d.HeldOff(KeyOf(current)))
        {
            currentValue->Set(nullptr);
            if (bot->GetTarget() == current->GetGUID())
                bot->SetSelection(ObjectGuid::Empty);
        }
    for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
        if (Spell const* spell = bot->GetCurrentSpell(type))
            if (d.HeldOff(KeyOf(spell->m_targets.GetUnitTarget())) && !Kick::Find(spell->GetSpellInfo()->Id))
            {
                botAI->RequestSpellInterrupt();
                ++ts.holdOffDrops;
                break;
            }
}

void BotTauntSwap::AppendExclusions(PlayerbotAI* botAI, GuidSet& exclusions, TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::Aoe)
        return;     // an AoE count is not a pick; the area taunts are the multiplier's
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !bot->IsInWorld())
        return;
    MechanicBotState* st = BotBrain::StateOf(botAI);
    if (!st)
        return;
    for (Desc::Key key : Refresh(botAI, *st, Now()).holdOff)
        exclusions.insert(ObjectGuid(key));
}

std::string BotTauntSwap::ReportLine(MechanicBotState const& st)
{
    TauntSwap::State const& ts = st.tauntSwap;
    TauntSwap::Decision const& d = ts.decision;
    return Acore::StringFormat("ts taunt swap: {} | holds {}, keeps off {}, taunts {} ({}) | swaps {} (trades {}), "
        "refused {}, walked in {}, attacks stopped {}, actions blocked {}, most stacks held {}",
        ts.entries ? "entries" : "no entries", d.holds.size(), d.holdOff.size(),
        d.taunt ? ObjectGuid(d.taunt).ToString() : std::string("-"), WhyName(d.why), ts.swaps, ts.trades,
        ts.swapFails, ts.approaches, ts.holdOffDrops, ts.blocked, uint32(ts.maxStacks));
}

// ---------------------------------------------------------------------------------------------------------------------
// Trigger, action, multiplier
// ---------------------------------------------------------------------------------------------------------------------

bool BossTauntSwapTrigger::IsActive()
{
    MechanicBotState* st = _state.Get(botAI);
    return st && BotTauntSwap::Refresh(botAI, *st, Now()).taunt != 0;
}

bool BossTauntSwapAction::isUseful()
{
    MechanicBotState* st = _state.Get(botAI);
    return st && BotTauntSwap::Refresh(botAI, *st, Now()).taunt != 0;
}

bool BossTauntSwapAction::Execute(Event /*event*/)
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return false;
    TauntSwap::Decision const d = BotTauntSwap::Refresh(botAI, *st, Now());
    if (!d.taunt)
        return false;
    TauntSwap::State& ts = st->tauntSwap;
    Unit* boss = ObjectAccessor::GetUnit(*bot, ObjectGuid(d.taunt));
    Unit* victim = d.tauntVictim ? ObjectAccessor::GetUnit(*bot, ObjectGuid(d.tauntVictim)) : nullptr;
    if (!boss || !boss->IsAlive())
        return false;

    bool const tank = PlayerbotAI::IsTank(bot);
    bool outOfReach = false;
    TauntSwap::TauntSpell const* pick = TauntSwap::PickTaunt(tank,
        [&](TauntSwap::TauntSpell const& t) { return bot->HasSpell(t.id); },
        [&](TauntSwap::TauntSpell const& t)
        {
            bool const atVictim = t.kind == TauntSwap::TauntKind::Friend;
            // Righteous Defense would take every swap boss the victim holds (TauntSwap::FriendTauntAllowed).
            if (atVictim && !d.friendTaunt)
                return false;
            Unit* target = atVictim ? victim : boss;
            if (!target || bot->HasSpellCooldown(t.id))
                return false;
            if (!Reaches(bot, sSpellMgr->GetSpellInfo(t.id), target))
            {
                outOfReach = true;
                return false;
            }
            return botAI->CanCastSpell(t.id, target, true);
        });

    if (!pick)
    {
        // Out of reach: walk in with the boss as the target (the taunt goes once in range).
        Unit* current = AI_VALUE(Unit*, "current target");
        if (outOfReach && current != boss && Attack(boss))
        {
            ++ts.approaches;
            return true;
        }
        return false;
    }

    Unit* target = pick->kind == TauntSwap::TauntKind::Friend ? victim : boss;
    if (!botAI->CastSpell(pick->id, target))
    {
        ++ts.swapFails;
        return false;
    }
    ++ts.swaps;
    if (d.why == TauntSwap::Why::Trade)
        ++ts.trades;
    ts.decidedAt = Milliseconds(-1);    // decide again on the next look (the victim moves to us)
    if (GetConfig().bots.debug)
        LOG_INFO(kLog, "Bot {}: taunt swap {} ({}) on {} from {} ({} stacks, {})", bot->GetName(), pick->name,
            pick->id, boss->GetName(), Name(bot, d.tauntVictim), uint32(d.victimStacks), WhyName(d.why));
    // The boss is now ours: keep it as the target (an instant taunt leaves the bot free to attack at once).
    if (AI_VALUE(Unit*, "current target") != boss)
        Attack(boss);
    return true;
}

float BossTauntSwapMultiplier::GetValue(Action* action)
{
    if (!action || dynamic_cast<BossTauntSwapAction*>(action))
        return 1.f;
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return 1.f;
    TauntSwap::Decision const& d = BotTauntSwap::Refresh(botAI, *st, Now());
    if (d.holdOff.empty())
        return 1.f;

    uint32 spellId = 0;
    if (CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action))
        spellId = CastSpellIdOf(botAI, cast);
    TauntSwap::TauntSpell const* taunt = spellId ? TauntSwap::Find(spellId) : nullptr;
    bool const area = taunt && taunt->kind == TauntSwap::TauntKind::Area;
    bool const kick = spellId && Kick::Find(spellId);

    Desc::Key target = 0;
    if (!area && action->GetTargetName() != "self target" && action->GetTargetValue())
        target = KeyOf(action->GetTarget());
    // Righteous Defense aims at the friend whose attackers it taunts: the victim of a boss we keep off.
    if (taunt && taunt->kind == TauntSwap::TauntKind::Friend && target)
        for (Desc::Key held : d.holdOff)
        {
            Unit* boss = ObjectAccessor::GetUnit(*bot, ObjectGuid(held));
            if (boss && KeyOf(boss->GetVictim()) == target)
                target = held;
        }

    if (!TauntSwap::HoldOffBlocks(d, target, area, kick))
        return 1.f;
    ++st->tauntSwap.blocked;
    return 0.f;
}

#endif
