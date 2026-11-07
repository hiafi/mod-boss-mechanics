/*
 * mod-boss-mechanics: the bots' target priority and ignore list (STAGE-M5-HANDOFF §3.5, WP-T). See
 * MechanicBotTargets.h.
 *
 * Focus pattern adapted from mod-playerbots' Magisters' Terrace strategy (src/Ai/Dungeon/MgT, GPLv2: Crow, jrad7,
 * Keleborn): an attack of the picked unit at ACTION_RAID + 2 and a multiplier that stands the stock pickers down.
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotTargets.h"
#include "BossMechanicsConfig.h"
#include "ChooseTargetActions.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MechanicBotBrain.h"
#include "MechanicBotGate.h"
#include "MechanicBotState.h"
#include "MechanicDescriptorView.h"
#include "MechanicRegistry.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "PetDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "StringFormat.h"
#include "UnitScript.h"
#include <algorithm>
#include <vector>

using namespace BossMechanics;

namespace
{
    constexpr char const* kLog = "module.bossmechanics.bots";
    // A listed unit further than this from the bot is not a candidate (another room, a far tracked add).
    constexpr float kMaxPickYd = 80.f;
    // Picks out of the bot's line of sight are skipped (Attack refuses them): at most this many re-picks per value.
    constexpr int kMaxRepicks = 3;

    Milliseconds Now()
    {
        return GameTime::GetGameTimeMS();
    }

    Desc::Key KeyOf(Unit const* unit)
    {
        return unit ? unit->GetGUID().GetRawValue() : 0;
    }

    bool HasIgnores(DescriptorView const& v)
    {
        return v.Active() && !v.descriptor.ignores.empty();
    }

    bool HasHostilePlayers(DescriptorView const& v)
    {
        return v.Active() && !v.descriptor.hostilePlayerAuras.empty();
    }

    // The registry and the state with the view brought up to date; false when no descriptor applies to the bot.
    bool RefreshedView(PlayerbotAI* botAI, Player* bot, MechanicBotStateRef& ref, MechanicBotState*& st,
        Registry const*& reg)
    {
        st = ref.Get(botAI);
        if (!st || !bot->IsInWorld())
            return false;
        reg = Registry::Get(bot->GetMap());
        RefreshDescriptorView(reg, bot, st->descriptor);
        return reg && st->descriptor.Active();
    }

    TargetRules::Focus CurrentFocus(PlayerbotAI* botAI)
    {
        Value<TargetRules::Focus>* v = botAI->GetAiObjectContext()->GetValue<TargetRules::Focus>("boss kill target");
        return v ? v->Get() : TargetRules::Focus();
    }

    TargetRules::Picker PickerOf(Action* action)
    {
        if (dynamic_cast<DpsAssistAction*>(action))
            return TargetRules::Picker::Assist;
        if (dynamic_cast<DpsAoeAction*>(action))
            return TargetRules::Picker::Aoe;
        if (dynamic_cast<TankAssistAction*>(action))
            return TargetRules::Picker::TankAssist;
        return TargetRules::Picker::None;
    }

#if defined(PLAYERBOTS_HAS_TARGET_FILTER)
    // AttackersValue::IsPossibleTarget's veto: runs for many units per tick, so it reads the view as the bot's last
    // refresh left it (the waker refreshes it every map update) and builds facts only for a unit an ignore entry
    // names.
    bool TargetFilter(Unit* unit, Player* bot)
    {
        if (!unit || !bot || !unit->IsCreature())
            return true;
        Registry const* reg = Registry::Get(bot->GetMap());
        if (!reg)
            return true;
        MechanicBotState* st = BotBrain::StateOf(GET_PLAYERBOT_AI(bot));
        if (!st || !HasIgnores(st->descriptor))
            return true;
        return !BotTargets::IsIgnoredNow(*st, *reg, bot, unit, Now());
    }
#endif

#if defined(PLAYERBOTS_HAS_HOSTILE_TEAMMATE_FILTER)
    // SFK F1: the fork asks this for a charmed teammate only (AttackersValue::IsHostileTeammate).
    bool HostileTeammateFilter(Player* teammate, Player* bot)
    {
        return bot && BotTargets::IsHostilePlayer(GET_PLAYERBOT_AI(bot), bot, teammate);
    }
#endif

    // SFK F1: a teammate the bot attacked as turned is not any more (the aura is gone, or the charm with it): drop it
    // as the current target, the melee victim and the cast target. The core's release (Unit::RemoveCharmedBy,
    // StopAttackingInvalidTarget) covers the melee swing when the charm ends; this covers the aura ending first and
    // a cast in flight.
    void DropReleased(PlayerbotAI* botAI, Player* bot, MechanicBotState& st)
    {
        auto released = [&](Unit const* u)
        {
            return u && u != bot && u->IsPlayer() && u->ToPlayer()->GetTeamId(true) == bot->GetTeamId(true)
                && !BotTargets::IsHostilePlayer(botAI, bot, u);
        };
        MechanicBotCounters& c = st.counters;
        Value<Unit*>* currentValue = botAI->GetAiObjectContext()->GetValue<Unit*>("current target");
        Unit* current = currentValue ? currentValue->Get() : nullptr;
        if (released(current))
        {
            if (GetConfig().bots.debug)
                LOG_INFO(kLog, "Bot {}: dropped {} as its target (no longer turned)", bot->GetName(),
                    current->GetName());
            currentValue->Set(nullptr);
            if (bot->GetTarget() == current->GetGUID())
                bot->SetSelection(ObjectGuid::Empty);
            ++c.hostileReleases;
        }
        if (released(bot->GetVictim()))
        {
            bot->AttackStop();
            ++c.hostileReleases;
        }
        for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
            if (Spell const* spell = bot->GetCurrentSpell(type))
                if (released(spell->m_targets.GetUnitTarget()) && !spell->GetSpellInfo()->IsPositive())
                {
                    botAI->RequestSpellInterrupt();
                    ++c.hostileReleases;
                    break;
                }
    }

    // Pulls one minion (pet, guardian, totem) off an ignored victim: onto the owner's target when it can, else back.
    void PullOff(PlayerbotAI* botAI, Player* bot, Unit* minion, Unit* ownerTarget)
    {
        CharmInfo* charm = minion->GetCharmInfo();
        minion->AttackStop();
        minion->CastStop();
        if (ownerTarget)
        {
            // As PetAttackAction (PetsAction.cpp).
            if (charm)
            {
                charm->SetIsCommandAttack(true);
                charm->SetIsAtStay(false);
                charm->SetIsFollowing(false);
                charm->SetIsCommandFollow(false);
                charm->SetIsReturning(false);
            }
            if (Creature* c = minion->ToCreature(); c && c->IsAIEnabled && c->AI())
                c->AI()->AttackStart(ownerTarget);
            return;
        }
        if (minion == bot->GetPet())
        {
            botAI->PetFollow();
            return;
        }
        if (charm)
        {
            charm->SetCommandState(COMMAND_FOLLOW);
            charm->SetIsCommandAttack(false);
            charm->SetIsCommandFollow(true);
            charm->SetIsReturning(true);
        }
        if (!minion->IsTotem())
            minion->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, minion->GetFollowAngle());
    }

    // The ignore list on the bot's own attack: current target, melee victim, the cast in progress, and its minions.
    void DropIgnored(PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Registry const& reg, Milliseconds now)
    {
        auto ignored = [&](Unit const* u) { return u && BotTargets::IsIgnoredNow(st, reg, bot, u, now); };
        MechanicBotCounters& c = st.counters;
        bool const debug = GetConfig().bots.debug;

        Value<Unit*>* currentValue = botAI->GetAiObjectContext()->GetValue<Unit*>("current target");
        Unit* current = currentValue ? currentValue->Get() : nullptr;
        if (ignored(current))
        {
            if (debug)
                LOG_INFO(kLog, "Bot {}: dropped its current target {} (ignore list)", bot->GetName(),
                    current->GetName());
            currentValue->Set(nullptr);
            if (bot->GetTarget() == current->GetGUID())
                bot->SetSelection(ObjectGuid::Empty);
            current = nullptr;
            ++c.ignoreDrops;
        }
        if (ignored(bot->GetVictim()))
        {
            bot->AttackStop();
            ++c.ignoreDrops;
        }
        for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
            if (Spell const* spell = bot->GetCurrentSpell(type))
                if (ignored(spell->m_targets.GetUnitTarget()))
                {
                    botAI->RequestSpellInterrupt();
                    ++c.ignoreDrops;
                    break;
                }

        if (bot->m_Controlled.empty())
            return;
        Unit* const ownerTarget = current && current->IsAlive() && bot->IsValidAttackTarget(current) ? current
            : nullptr;
        // Copied: PullOff must not run while the set is being walked.
        std::vector<Unit*> minions(bot->m_Controlled.begin(), bot->m_Controlled.end());
        for (Unit* m : minions)
        {
            if (!m || !m->IsInWorld() || !m->IsAlive())
                continue;
            Unit* const victim = m->GetVictim();
            if (!victim)
                continue;
            switch (TargetRules::DecideMinion(ignored(victim), ownerTarget != nullptr))
            {
                case TargetRules::MinionStep::Keep:
                    continue;
                case TargetRules::MinionStep::Redirect:
                    PullOff(botAI, bot, m, ownerTarget);
                    break;
                case TargetRules::MinionStep::Recall:
                    PullOff(botAI, bot, m, nullptr);
                    break;
            }
            ++c.petRecalls;
            if (debug)
                LOG_INFO(kLog, "Bot {}: pulled {} off {} (ignore list){}", bot->GetName(), m->GetName(),
                    victim->GetName(), ownerTarget ? Acore::StringFormat(", onto {}", ownerTarget->GetName())
                    : std::string(", recalled"));
        }
    }

    // Damage the bot and its minions deal to ignored units (the WP-S counters). OnDamage runs for every damage event
    // on the server: a creature victim and a player behind the attacker are tested before anything is looked up.
    class BossMechanicsBotTargetsUnitScript : public UnitScript
    {
    public:
        BossMechanicsBotTargetsUnitScript() : UnitScript("BossMechanicsBotTargetsUnitScript", true,
            { UNITHOOK_ON_DAMAGE }) { }

        void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
        {
            if (!damage || !attacker || !victim || attacker == victim || !victim->IsCreature()
                || !BotGate::IsEnabled())
                return;
            Player* owner = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (!owner || owner->GetMap() != victim->GetMap())
                return;
            // M5 WP-F (R m-10): the map's registry first (most maps have none), then the bot lookups.
            Registry const* reg = Registry::Get(victim->GetMap());
            if (!reg)
                return;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(owner);
            if (!botAI)
                return;
            MechanicBotState* st = BotBrain::StateOf(botAI);
            if (!st || !HasIgnores(st->descriptor) || !BotTargets::IsIgnoredNow(*st, *reg, owner, victim, Now()))
                return;
            MechanicBotCounters& c = st->counters;
            if (attacker == owner)
            {
                ++c.ignoredHits;
                c.ignoredDamage += damage;
            }
            else
            {
                ++c.petIgnoredHits;
                c.petIgnoredDamage += damage;
            }
            if (GetConfig().bots.debug)
                LOG_INFO(kLog, "Bot {}: {} hit ignored {} for {}", owner->GetName(),
                    attacker == owner ? std::string("it") : std::string(attacker->GetName()), victim->GetName(),
                    damage);
        }
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// BotTargets
// ---------------------------------------------------------------------------------------------------------------------

bool BotTargets::IsHostilePlayer(PlayerbotAI* botAI, Player const* bot, Unit const* unit)
{
    if (!botAI || !bot || !unit || !unit->IsPlayer() || !unit->IsCharmed() || !bot->IsInWorld()
        || !unit->IsInMap(bot))
        return false;
    MechanicBotState* st = BotBrain::StateOf(botAI);
    if (!st || !HasHostilePlayers(st->descriptor))
        return false;
    Registry const* reg = Registry::Get(bot->GetMap());
    if (!reg)
        return false;
    DescriptorView& v = st->descriptor;
    return Desc::IsHostilePlayer(v.descriptor, FactsOf(v, *reg, unit, Now()));
}

bool BotTargets::IsIgnoredNow(MechanicBotState& st, Registry const& reg, Player const* bot, Unit const* unit,
    Milliseconds now)
{
    DescriptorView& v = st.descriptor;
    if (!unit || !HasIgnores(v) || !TargetRules::MayBeIgnored(v.descriptor, KeyOf(unit), unit->GetEntry()))
        return false;
    return Desc::IsIgnored(v.descriptor, FactsOf(v, reg, unit, now), ContextOf(v, bot));
}

void BotTargets::AfterTick(PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Registry const& reg,
    Milliseconds now)
{
    TargetRules::State& ts = st.targets;
    if (!ts.attackersLogged)
    {
        // Risk §6: the override must win the name. Logged once per bot (until the state resets).
        ts.attackersLogged = true;
        UntypedValue* value = botAI->GetAiObjectContext()->GetUntypedValue("attackers");
        LOG_DEBUG(kLog, "Bot {}: value `attackers` is {}", bot->GetName(),
            dynamic_cast<MechanicBotAttackersValue*>(value) ? "mod-boss-mechanics' (ignore list)"
            : value ? "not mod-boss-mechanics' (ignore list off for its pickers)" : "missing");
    }

    RefreshDescriptorView(&reg, bot, st.descriptor);
    DescriptorView const& v = st.descriptor;
    Milliseconds const gap = TargetRules::SampleGap(ts.sampledAt, now);
    ts.sampledAt = now;
    MechanicBotCounters& c = st.counters;
    if (!v.Active())
    {
        if (ts.breakOut.Open())
            ts.breakOut.Sample({}, now);
        ts.focus = TargetRules::Focus();
        return;
    }

    // Break-out: how long each prison of the encounter held its player.
    std::vector<Desc::Key> prisons;
    prisons.reserve(v.held.size());
    for (HeldPlayer const& h : v.held)
        prisons.push_back(h.prison.GetRawValue());
    if (!prisons.empty() || ts.breakOut.Open())
    {
        ts.breakOut.Sample(prisons, now);
        c.breakOutMsMax = std::max(c.breakOutMsMax, ts.breakOut.MaxMs());
    }

    // Lower priority: the bot attacks something else while its engaged pick lives.
    if (gap > Milliseconds(0) && bot->IsInCombat() && now - ts.focusAt <= TargetRules::kFocusStale)
    {
        Unit* victim = bot->GetVictim();
        if (!victim)
            if (Value<Unit*>* cur = botAI->GetAiObjectContext()->GetValue<Unit*>("current target"))
                victim = cur->Get();
        if (TargetRules::OnLowerTarget(ts.focus, KeyOf(victim)))
            c.msLowerTarget += static_cast<uint32>(gap.count());
    }

    if (HasIgnores(v))
        DropIgnored(botAI, bot, st, reg, now);
    if (HasHostilePlayers(v))
        DropReleased(botAI, bot, st);
}

void BotTargets::AppendExclusions(PlayerbotAI* botAI, GuidSet& exclusions)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !bot->IsInWorld())
        return;
    MechanicBotState* st = BotBrain::StateOf(botAI);
    Registry const* reg = Registry::Get(bot->GetMap());
    if (!st || !reg || !HasIgnores(st->descriptor))
        return;
    Milliseconds const now = Now();
    auto add = [&](ObjectGuid guid)
    {
        Unit* unit = ObjectAccessor::GetUnit(*bot, guid);
        if (unit && IsIgnoredNow(*st, *reg, bot, unit, now))
            exclusions.insert(guid);
    };
    // Copied first: IsIgnoredNow does not refresh the view, but the guids are cheap to copy.
    std::vector<ObjectGuid> guids{ st->descriptor.owner };
    for (EncounterUnit const& u : st->descriptor.units)
        guids.push_back(u.guid);
    for (ObjectGuid const& g : guids)
        add(g);
}

void BotTargets::InstallTargetFilter()
{
#if defined(PLAYERBOTS_HAS_TARGET_FILTER)
    AttackersValue::SetTargetFilter(&TargetFilter);
    LOG_INFO(kLog, "mod-boss-mechanics: installed the ignore-list target filter (AttackersValue::SetTargetFilter)");
#else
    LOG_INFO(kLog, "mod-boss-mechanics: this mod-playerbots has no AttackersValue::SetTargetFilter; RTI, skull and "
        "assist targets are not filtered by boss ignore lists");
#endif
#if defined(PLAYERBOTS_HAS_HOSTILE_TEAMMATE_FILTER)
    AttackersValue::SetHostileTeammateFilter(&HostileTeammateFilter);
    LOG_INFO(kLog, "mod-boss-mechanics: installed the turned-teammate filter "
        "(AttackersValue::SetHostileTeammateFilter)");
#else
    LOG_INFO(kLog, "mod-boss-mechanics: this mod-playerbots has no AttackersValue::SetHostileTeammateFilter; bots "
        "never attack a charmed teammate, whatever the boss descriptor's hostilePlayerAuras say");
#endif
}

std::string BotTargets::ReportLine(MechanicBotState const& st)
{
    TargetRules::Focus const& f = st.targets.focus;
    MechanicBotCounters const& c = st.counters;
    std::string const focus = f.target ? Acore::StringFormat("{} prio {}{}{}", ObjectGuid(f.target).ToString(),
        f.priority, f.Engaged() ? " engaged" : "", f.aoeOk ? " aoe ok" : "") : std::string("-");
    return Acore::StringFormat("m5 targets: focus {} | switches {}, on lower targets {} ms, break-out max {} ms | "
        "ignored: bot hits {} ({} dmg), pet hits {} ({} dmg), actions blocked {}, targets dropped {}, minions pulled "
        "off {} | turned teammates released {}", focus, c.focusSwitches, c.msLowerTarget, c.breakOutMsMax,
        c.ignoredHits, c.ignoredDamage, c.petIgnoredHits, c.petIgnoredDamage, c.ignoreBlocks, c.ignoreDrops,
        c.petRecalls, c.hostileReleases);
}

void AddSC_boss_mechanics_bot_targets()
{
    new BossMechanicsBotTargetsUnitScript();
}

// ---------------------------------------------------------------------------------------------------------------------
// Values, trigger, action, multiplier
// ---------------------------------------------------------------------------------------------------------------------

GuidVector MechanicBotAttackersValue::Calculate()
{
    GuidVector result = AttackersValue::Calculate();
    if (result.empty())
        return result;
    MechanicBotState* st = nullptr;
    Registry const* reg = nullptr;
    if (!RefreshedView(botAI, bot, _state, st, reg) || !HasIgnores(st->descriptor))
        return result;
    Milliseconds const now = Now();
    result.erase(std::remove_if(result.begin(), result.end(), [&](ObjectGuid const& guid)
    {
        Unit* unit = botAI->GetUnit(guid);
        return unit && BotTargets::IsIgnoredNow(*st, *reg, bot, unit, now);
    }), result.end());
    return result;
}

std::string const MechanicBotKillTargetValue::Format()
{
    return value.target ? ObjectGuid(value.target).ToString() : std::string("-");
}

TargetRules::Focus MechanicBotKillTargetValue::Calculate()
{
    TargetRules::Focus none;
    // Read before the view is refreshed and the facts are taken: recalculating `attackers` refreshes the view, and a
    // refresh that copies drops the facts cache the candidates point into.
    GuidVector const attackers = AI_VALUE(GuidVector, "attackers");
    Unit* const current = AI_VALUE(Unit*, "current target");
    MechanicBotState* st = nullptr;
    Registry const* reg = nullptr;
    bool const active = RefreshedView(botAI, bot, _state, st, reg);
    if (!st)
        return none;
    Milliseconds const now = Now();
    st->targets.focusAt = now;
    st->targets.focus = none;
    DescriptorView& v = st->descriptor;
    if (!active || v.descriptor.targets.empty() || !bot->IsInCombat())
        return none;

    std::vector<ObjectGuid> guids{ v.owner };
    for (EncounterUnit const& u : v.units)
        guids.push_back(u.guid);
    for (ObjectGuid const& g : attackers)
        if (std::find(guids.begin(), guids.end(), g) == guids.end())
            guids.push_back(g);

    std::vector<Desc::TargetCandidate> candidates;
    std::vector<Unit*> units;
    candidates.reserve(guids.size());
    units.reserve(guids.size());
    for (ObjectGuid const& g : guids)
    {
        Unit* unit = botAI->GetUnit(g);
        if (!unit || !unit->IsInWorld() || !bot->IsWithinDistInMap(unit, kMaxPickYd))
            continue;
        Desc::TargetCandidate c;
        c.facts = &FactsOf(v, *reg, unit, now);
        if (!v.HeldBy(g).IsEmpty())
            c.held = HeldRankOf(v, bot, g);
        candidates.push_back(c);
        units.push_back(unit);
    }

    Desc::BotFacts facts;
    facts.tank = PlayerbotAI::IsTank(bot);
    facts.healer = PlayerbotAI::IsHeal(bot);
    facts.melee = PlayerbotAI::IsMelee(bot);
    Desc::Context const ctx = ContextOf(v, bot);
    TargetRules::Focus f;
    for (int i = 0; i <= kMaxRepicks; ++i)
    {
        f = TargetRules::DecideFocus(v.descriptor, candidates, v.owner.GetRawValue(), facts, KeyOf(current), ctx);
        if (!f.target)
            break;
        std::size_t const at = static_cast<std::size_t>(std::distance(candidates.begin(),
            std::find_if(candidates.begin(), candidates.end(), [&f](Desc::TargetCandidate const& c)
            {
                return c.facts->guid == f.target;
            })));
        if (at >= units.size() || bot->IsWithinLOSInMap(units[at]))
            break;
        candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(at));
        units.erase(units.begin() + static_cast<std::ptrdiff_t>(at));
        f = TargetRules::Focus();
    }
    st->targets.focus = f;
    return f;
}

bool MechanicBotFocusTrigger::IsActive()
{
    TargetRules::Focus const f = AI_VALUE(TargetRules::Focus, "boss kill target");
    if (!f.Engaged())
        return false;
    Unit* current = AI_VALUE(Unit*, "current target");
    return KeyOf(current) != f.target;
}

bool MechanicBotFocusAction::isUseful()
{
    return AI_VALUE(TargetRules::Focus, "boss kill target").Engaged();
}

bool MechanicBotFocusAction::Execute(Event /*event*/)
{
    TargetRules::Focus const f = AI_VALUE(TargetRules::Focus, "boss kill target");
    if (!f.Engaged())
        return false;
    Unit* target = botAI->GetUnit(ObjectGuid(f.target));
    Unit* current = AI_VALUE(Unit*, "current target");
    if (!target || current == target)
        return false;
    if (!Attack(target))
        return false;
    if (MechanicBotState* st = _state.Get(botAI))
        ++st->counters.focusSwitches;
    if (GetConfig().bots.debug)
        LOG_INFO(kLog, "Bot {}: focus {} -> {} (priority {}{})", bot->GetName(),
            current ? current->GetName() : std::string("-"), target->GetName(), f.priority,
            f.outranksBoss ? ", above the boss" : "");
    return true;
}

float MechanicBotFocusMultiplier::GetValue(Action* action)
{
    if (dynamic_cast<MechanicBotFocusAction*>(action))
        return 1.f;
    MechanicBotState* st = _state.Get(botAI);
    if (!st || !st->descriptor.Active())
        return 1.f;

    TargetRules::Picker const picker = PickerOf(action);
    if (picker != TargetRules::Picker::None)
    {
        if (!bot->IsInCombat() || st->descriptor.descriptor.targets.empty())
            return 1.f;
        return TargetRules::SuppressPicker(picker, CurrentFocus(botAI)) ? 0.f : 1.f;
    }

    // The ignore list: an action aimed at an ignored unit (an unknown target value name has no target).
    if (!HasIgnores(st->descriptor) || action->GetTargetName() == "self target" || !action->GetTargetValue())
        return 1.f;
    Unit* target = action->GetTarget();
    if (!target || target == bot || !target->IsCreature() || bot->IsFriendlyTo(target))
        return 1.f;
    Registry const* reg = Registry::Get(bot->GetMap());
    if (!reg || !BotTargets::IsIgnoredNow(*st, *reg, bot, target, Now()))
        return 1.f;
    ++st->counters.ignoreBlocks;
    return 0.f;
}

#endif
