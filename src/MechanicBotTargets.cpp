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
#include "GenericSpellActions.h"
#include "HealthUtils.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MechanicBotBrain.h"
#include "MechanicBotGate.h"
#include "MechanicBotState.h"
#include "MechanicBotTauntSwap.h"
#include "MechanicDescriptorView.h"
#include "MechanicHealReadCache.h"
#include "MechanicRegistry.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "PetDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "UnitScript.h"
#include <algorithm>
#include <optional>
#include <vector>

using namespace BossMechanics;

namespace
{
    constexpr char const* kLog = "module.bossmechanics.bots";
    // A listed unit further than this from the bot is not a candidate (another room, a far tracked add).
    constexpr float kMaxPickYd = 80.f;
    // Picks out of the bot's line of sight are skipped (Attack refuses them): at most this many re-picks per value.
    constexpr int kMaxRepicks = 3;
    // While the bot walks into sight of an unseen pick, the walk is planned again at most this often.
    constexpr Milliseconds kSightRetry{ 1000 };

    Milliseconds Now()
    {
        return GameTime::GetGameTimeMS();
    }

    Desc::Key KeyOf(Unit const* unit)
    {
        return unit ? unit->GetGUID().GetRawValue() : 0;
    }

    // The first point of the bot's path to `target` that sees it (TargetRules::SightSamples, at the bot's eye height),
    // else the path's end. False: no path.
    bool SightPoint(Player* bot, Unit* target, TargetRules::PathPoint& out)
    {
        PathGenerator path(bot);
        path.CalculatePath(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);
        PathType const type = path.GetPathType();
        if ((type & PATHFIND_NOPATH) || !(type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)))
            return false;
        std::vector<TargetRules::PathPoint> points;
        points.reserve(path.GetPath().size());
        for (G3D::Vector3 const& p : path.GetPath())
            points.push_back({ p.x, p.y, p.z });
        std::vector<TargetRules::PathPoint> const samples = TargetRules::SightSamples(points);
        if (samples.empty())
            return false;
        // IsWithinLOS adds the target's own height to the point: this lands it at the bot's eye.
        float const eye = bot->GetCollisionHeight() - target->GetCollisionHeight();
        out = samples.back();
        for (TargetRules::PathPoint const& sample : samples)
            if (target->IsWithinLOS(sample.x, sample.y, sample.z + eye))
            {
                out = sample;
                break;
            }
        return true;
    }

    // WP-DS: an Attacks hold is an ignore entry too (Desc::IsIgnored).
    bool HasIgnores(DescriptorView const& v)
    {
        return v.Active() && (!v.descriptor.ignores.empty()
            || Desc::HasHolds(v.descriptor, HoldEntry::Action::Attacks));
    }

    // Zul'Farrak F33: an ignore entry for one role (IgnoreEntry::who): only then does the ignore check need the bot's
    // role (PlayerbotAI::IsTank and the rest are not free; the target filter runs for many units per tick).
    bool HasRoleIgnores(DescriptorView const& v)
    {
        return std::any_of(v.descriptor.ignores.begin(), v.descriptor.ignores.end(),
            [](IgnoreEntry const& e) { return e.who != Who::Any; });
    }

    // The bot's role as the target rules read it (Desc::BotFacts).
    Desc::BotFacts BotFactsOf(Player const* bot)
    {
        Player* const p = const_cast<Player*>(bot);     // the playerbots role reads take a non-const player
        Desc::BotFacts facts;
        facts.tank = PlayerbotAI::IsTank(p);
        facts.healer = PlayerbotAI::IsHeal(p);
        facts.melee = PlayerbotAI::IsMelee(p);
        facts.mainTank = facts.tank && PlayerbotAI::IsMainTank(p);    // WP-TS: Who::MainTank / Who::AssistTank
        facts.guid = bot->GetGUID().GetRawValue();
        return facts;
    }

    bool HasDamageHolds(DescriptorView const& v)
    {
        return v.Active() && Desc::HasHolds(v.descriptor, HoldEntry::Action::Damage);
    }

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

    // WP-DS: does the spell deal damage (direct, weapon, leech, power burn, or a periodic damage or leech aura)? A
    // Damage hold keeps only these off its unit: a kick, a slow or a curse with no damage still goes.
    bool DealsDamage(SpellInfo const* info)
    {
        for (SpellEffectInfo const& eff : info->GetEffects())
        {
            switch (eff.Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                case SPELL_EFFECT_ENVIRONMENTAL_DAMAGE:
                case SPELL_EFFECT_HEALTH_LEECH:
                case SPELL_EFFECT_WEAPON_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                case SPELL_EFFECT_POWER_BURN:
                    return true;
                case SPELL_EFFECT_APPLY_AURA:
                    if (eff.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE
                        || eff.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH
                        || eff.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE_PERCENT)
                        return true;
                    break;
                default:
                    break;
            }
        }
        return false;
    }

    // The schools the bot's cast of `info` hits with: a wand's Shoot by the wand's damage school (the spell itself is
    // physical), anything else by its own school mask.
    uint32 HitSchoolMask(Player const* bot, SpellInfo const* info)
    {
        if (info->Id == 5019)   // Shoot (wands)
            if (Item const* wand = bot->GetWeaponForAttack(RANGED_ATTACK, true))
                return 1u << wand->GetTemplate()->Damage[0].DamageType;
        return info->GetSchoolMask();
    }

    // A damaging cast of `info` by the bot on `target` is held now (a Damage or Attacks hold whose schools it meets).
    bool CastHeld(MechanicBotState& st, Registry const& reg, Player const* bot, Unit const* target,
        SpellInfo const* info, Milliseconds now)
    {
        if (!info || info->IsPositive() || !DealsDamage(info))
            return false;
        uint32 const held = BotTargets::HeldDamageMaskNow(st, reg, bot, target, now);
        return held && (held & HitSchoolMask(bot, info));
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

    // WP-DS: a damaging cast or channel in progress whose target a Damage hold now covers (the shell came up mid-cast)
    // is stopped, as an ignored target's is.
    void DropHeldCasts(PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Registry const& reg, Milliseconds now)
    {
        for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
            if (Spell const* spell = bot->GetCurrentSpell(type))
            {
                Unit const* target = spell->m_targets.GetUnitTarget();
                if (!target || target == bot || !target->IsCreature()
                    || !CastHeld(st, reg, bot, target, spell->GetSpellInfo(), now))
                    continue;
                botAI->RequestSpellInterrupt();
                ++st.counters.holdDrops;
                if (GetConfig().bots.debug)
                    LOG_INFO(kLog, "Bot {}: stopped {} on {} (damage hold)", bot->GetName(),
                        spell->GetSpellInfo()->Id, target->GetName());
                break;
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
    // Zul'Farrak F33: the bot's role only when an entry is for one role (no bot: only the Any entries apply).
    std::optional<Desc::BotFacts> facts;
    if (bot && HasRoleIgnores(v))
        facts = BotFactsOf(bot);
    return Desc::IsIgnored(v.descriptor, FactsOf(v, reg, unit, now), ContextOf(v, bot), facts ? &*facts : nullptr);
}

bool BotTargets::KicksHeldNow(MechanicBotState& st, Registry const& reg, Player const* bot, Unit const* unit,
    Milliseconds now)
{
    DescriptorView& v = st.descriptor;
    if (!unit || !v.Active() || !Desc::HasHolds(v.descriptor, HoldEntry::Action::Interrupts))
        return false;
    return Desc::KicksHeld(v.descriptor, FactsOf(v, reg, unit, now), ContextOf(v, bot));
}

uint32 BotTargets::HeldDamageMaskNow(MechanicBotState& st, Registry const& reg, Player const* bot, Unit const* unit,
    Milliseconds now)
{
    DescriptorView& v = st.descriptor;
    if (!unit || !v.Active() || v.descriptor.holds.empty())
        return 0;
    return Desc::HeldDamageMask(v.descriptor, FactsOf(v, reg, unit, now), ContextOf(v, bot));
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
    if (HasDamageHolds(v))
        DropHeldCasts(botAI, bot, st, reg, now);
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

namespace
{
    // Zul'Farrak F-1f (R-1-FW m4): what the heal read hook reads. Published per map from its own update thread.
    HealReadCache<Map const*> sHealReads;
}

void BotTargets::PublishHealReads(Map* map, Registry const& reg, Milliseconds now)
{
    if (!map)
        return;
    std::vector<HealUntilEntry> entries;
    reg.ForEachEncounter([&](Encounter const& e)
    {
        if (!e.described || e.ending)
            return;
        for (HealUntilEntry const& h : e.descriptor.healUntil)
            if (h.auraId)
                entries.push_back(h);
    });
    HealReadCache<Map const*>::Rows rows;
    if (!entries.empty())
    {
        auto consider = [&](Unit const* u)
        {
            if (!u || !u->IsInWorld() || !u->IsAlive())
                return;
            std::vector<HealUntilEntry> hit;
            for (HealUntilEntry const& h : entries)
                if (u->HasAura(h.auraId))
                    hit.push_back(h);
            if (!hit.empty())
                rows.emplace_back(u->GetGUID().GetRawValue(), std::move(hit));
        };
        for (MapReference const& ref : map->GetPlayers())
            if (Player* player = ref.GetSource())
            {
                consider(player);
                consider(player->GetPet());
            }
    }
    sHealReads.Publish(map, std::move(rows), static_cast<std::int64_t>(now.count()));
}

float BotTargets::HealReadPct(Unit const* unit, float pct)
{
    // Any thread (the reading bot's map thread; the unit may be on another map): the cache only, never the unit's
    // map registry or auras. The guid is fixed for the unit's life.
    if (!unit)
        return pct;
    return sHealReads.Read(unit->GetGUID().GetRawValue(), pct, static_cast<std::int64_t>(Now().count()));
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
#if defined(PLAYERBOTS_HAS_HEAL_READ_HOOK)
    ai::health::SetHealReadHook(&BotTargets::HealReadPct);
    LOG_INFO(kLog, "mod-boss-mechanics: installed the heal read hook (ai::health::SetHealReadHook)");
#else
    LOG_INFO(kLog, "mod-boss-mechanics: this mod-playerbots has no ai::health::SetHealReadHook; bot healers stop at "
        "their usual line, whatever the boss descriptor's healUntil entries say");
#endif
}

std::string BotTargets::ReportLine(MechanicBotState const& st)
{
    TargetRules::Focus const& f = st.targets.focus;
    MechanicBotCounters const& c = st.counters;
    std::string const focus = f.target ? Acore::StringFormat("{} prio {}{}{}", ObjectGuid(f.target).ToString(),
        f.priority, f.Engaged() ? " engaged" : "", f.aoeOk ? " aoe ok" : "") + (f.swapHold ? " (swap hold)" : "")
        + (f.unseen ? " (out of sight)" : "")
        : std::string("-");
    return Acore::StringFormat("m5 targets: focus {} | switches {}, sight walks {}, on lower targets {} ms, break-out "
        "max {} ms | ignored: bot hits {} ({} dmg), pet hits {} ({} dmg), actions blocked {}, targets dropped {}, "
        "minions pulled off {} | turned teammates released {} | holds: damage casts blocked {}, stopped {}", focus,
        c.focusSwitches, c.focusSightMoves, c.msLowerTarget, c.breakOutMsMax, c.ignoredHits, c.ignoredDamage,
        c.petIgnoredHits, c.petIgnoredDamage, c.ignoreBlocks, c.ignoreDrops, c.petRecalls, c.hostileReleases,
        c.holdBlocks, c.holdDrops);
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
    // WP-TS: the taunt swap's bosses (a copy: the decision is taken before the facts below, and a refresh in it would
    // not copy the view again within this ms). A boss the bot keeps off is no candidate; one it holds is its focus.
    TauntSwap::Decision const swap = BotTauntSwap::Refresh(botAI, *st, now);

    std::vector<ObjectGuid> guids;
    auto addGuid = [&](ObjectGuid const& g)
    {
        if (!swap.HeldOff(g.GetRawValue()) && std::find(guids.begin(), guids.end(), g) == guids.end())
            guids.push_back(g);
    };
    addGuid(v.owner);
    for (EncounterUnit const& u : v.units)
        addGuid(u.guid);
    for (ObjectGuid const& g : attackers)
        addGuid(g);

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

    Desc::BotFacts const facts = BotFactsOf(bot);
    Desc::Context const ctx = ContextOf(v, bot);
    auto unitOf = [&candidates, &units](Desc::Key key) -> Unit*
    {
        for (std::size_t i = 0; i < candidates.size(); ++i)
            if (candidates[i].facts->guid == key)
                return units[i];
        return nullptr;
    };
    // Picks out of the bot's sight are ranked again without them (Attack refuses them); one that outranks the boss
    // nearby stays the focus, `unseen`, and the focus action walks the bot into sight (DecideFocusInSight).
    TargetRules::Focus f = TargetRules::DecideFocusInSight(v.descriptor, candidates, v.owner.GetRawValue(), facts,
        KeyOf(current), ctx, kMaxRepicks, [&](Desc::Key key)
        {
            Unit* unit = unitOf(key);
            return !unit || bot->IsWithinLOSInMap(unit);
        }, [&](Desc::Key key)
        {
            Unit* unit = unitOf(key);
            return unit ? bot->GetDistance(unit) : 0.f;
        });
    // WP-TS: a tank keeps the swap boss it holds, whatever the list ranks above it (the assist tank's add entry).
    if (Desc::Key const held = TauntSwap::HeldFocus(swap, KeyOf(current)); held && f.target != held)
    {
        f = TargetRules::Focus();
        f.target = held;
        f.swapHold = true;
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
    if (f.unseen && !bot->IsWithinLOSInMap(target))
    {
        // Out of sight (Attack refuses it): walk to the first point of the path that sees it, and keep the tick
        // while the walk runs (a cast on the old target would stop it).
        Milliseconds const now = Now();
        if (bot->isMoving() && _sightTarget == target->GetGUID() && now < _sightRetryAt)
            return true;
        TargetRules::PathPoint to;
        if (!SightPoint(bot, target, to) || !MoveTo(target->GetMapId(), to.x, to.y, to.z))
            return bot->isMoving() && _sightTarget == target->GetGUID();
        _sightTarget = target->GetGUID();
        _sightRetryAt = now + kSightRetry;
        if (MechanicBotState* st = _state.Get(botAI))
            ++st->counters.focusSightMoves;
        if (GetConfig().bots.debug)
            LOG_INFO(kLog, "Bot {}: focus {} out of sight at {:.1f} yd, walking {:.1f} yd into sight (priority {})",
                bot->GetName(), target->GetName(), bot->GetDistance(target), bot->GetExactDist(to.x, to.y, to.z),
                f.priority);
        return true;
    }
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

    // The ignore list: an action aimed at an ignored unit (an unknown target value name has no target). WP-DS: a
    // damaging cast whose schools a Damage hold on the target covers.
    bool const ignores = HasIgnores(st->descriptor);
    bool const damageHolds = HasDamageHolds(st->descriptor);
    if ((!ignores && !damageHolds) || action->GetTargetName() == "self target" || !action->GetTargetValue())
        return 1.f;
    Unit* target = action->GetTarget();
    if (!target || target == bot || !target->IsCreature() || bot->IsFriendlyTo(target))
        return 1.f;
    if (damageHolds)
        if (CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action))
            if (Registry const* reg = Registry::Get(bot->GetMap()))
            {
                uint32 const spellId = CastSpellIdOf(botAI, cast);
                if (spellId && CastHeld(*st, *reg, bot, target, sSpellMgr->GetSpellInfo(spellId), Now()))
                {
                    ++st->counters.holdBlocks;
                    return 0.f;
                }
            }
    if (!ignores)
        return 1.f;
    Registry const* reg = Registry::Get(bot->GetMap());
    if (!reg || !BotTargets::IsIgnoredNow(*st, *reg, bot, target, Now()))
        return 1.f;
    ++st->counters.ignoreBlocks;
    return 0.f;
}

#endif
