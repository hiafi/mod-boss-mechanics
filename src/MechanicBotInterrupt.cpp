/*
 * mod-boss-mechanics: bots kick a boss's interrupt list (STAGE-M5-HANDOFF §3.3, decision §7.1 #2, WP-I). See
 * MechanicBotInterrupt.h; the pure rules are MechanicKickRules.h and MechanicDescriptor.h.
 *
 * The kick row at INTERRUPT +5, the ranked kick list and the test that a kick stops a cast follow the pattern of
 * mod-playerbots' Magisters' Terrace strategy (src/Ai/Dungeon/MgT: MgTShared.cpp StopsACast, CastInterrupt,
 * InterruptSlot; MgTStrategy.cpp); the pet cast mirrors playerbots' CastPetSpellAction.
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotInterrupt.h"
#include "BossMechanicsConfig.h"
#include "Creature.h"
#include "GameTime.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MechanicBotBrain.h"
#include "MechanicBotFacing.h"
#include "MechanicBotGate.h"
#include "MechanicBotState.h"
#include "MechanicBotTargets.h"
#include "MechanicDescriptorView.h"
#include "MechanicKickRules.h"
#include "MechanicLockRules.h"
#include "MechanicRegistry.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <algorithm>
#include <cstdint>
#include <unordered_map>

using namespace BossMechanics;

namespace
{
    constexpr char const* kLog = "module.bossmechanics.bots";

    // A picked kicker that has not claimed its cast after this long gives way to any eligible bot (Kick::Assign).
    constexpr Milliseconds kDeferAfter{ 600 };
    // Two estimates of one cast's start (end - cast time) differ by a map update at most; casts of one spell by one
    // caster are a cast time apart at least. Capped at half the cast time below.
    constexpr Milliseconds kStartTolerance{ 300 };
    // A claim outlives its cast by this much, so the miss check after the cast's end still sees it.
    constexpr Milliseconds kClaimGrace{ 1000 };
    // A cast gone within this of its end completed (the observation lags by a map update).
    constexpr Milliseconds kMissSlack{ 150 };
    // Once-per-cast marks are kept this long after the cast started.
    constexpr Milliseconds kMarkKeep{ 60000 };
    // The GCD's remaining time is not readable on the game clock; a kick under it is taken as back after this.
    constexpr Milliseconds kGcdGuess{ 750 };
    // A bot that lost control or a pet already casting is taken as able to kick again after this.
    constexpr Milliseconds kBusyGuess{ 500 };
    // An endless channel (negative timer) is taken as ending this far ahead.
    constexpr Milliseconds kEndlessChannel{ 60000 };

    enum MarkKind : uint8
    {
        MarkHold = 1,
        MarkStockDrop = 2,
        MarkMiss = 3
    };

    Milliseconds Now()
    {
        return GameTime::GetGameTimeMS();
    }

    // The bot's Felhunter (or any pet or guardian), alive and on its map.
    Creature* KickPet(Player* bot)
    {
        Creature* pet = bot->GetPet();
        if (!pet)
            pet = bot->GetGuardianPet();
        if (!pet || !pet->IsAlive() || !pet->IsInWorld() || !pet->IsInMap(bot))
            return nullptr;
        return pet;
    }

    // The kick lands on `target`: an interrupt or silence effect it is not immune to (MgT StopsACast, per target).
    // M5 WP-F: nor to the whole spell (a spell-level silence mechanic on a reworked boss's -273 set: Silence, Solar
    // Beam), which CanCastSpell refuses anyway.
    bool KickLands(Unit* target, SpellInfo const* info)
    {
        if (target->IsImmunedToSpell(info, static_cast<Unit const*>(nullptr), info->GetSchoolMask()))
            return false;
        for (uint8 i = EFFECT_0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& e = info->Effects[i];
            bool const stops = e.Effect == SPELL_EFFECT_INTERRUPT_CAST
                || (e.Effect == SPELL_EFFECT_APPLY_AURA && e.ApplyAuraName == SPELL_AURA_MOD_SILENCE);
            if (stops && !target->IsImmunedToSpellEffect(info, i))
                return true;
        }
        return false;
    }

    // M5 WP-F (R M-2): a spell that stops casts: a kick of the table, or a hostile spell with a stopping effect
    // (Kick::StopsCasts: interrupt, silence, stun, confuse, fear). Cached per spell id and map thread (spell data does
    // not change while the server runs).
    bool IsCastStopper(uint32 spellId)
    {
        if (Kick::Find(spellId))
            return true;
        thread_local std::unordered_map<uint32, bool> cache;
        if (auto const it = cache.find(spellId); it != cache.end())
            return it->second;
        bool stops = false;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId); info && !info->IsPositive())
            for (SpellEffectInfo const& e : info->GetEffects())
                stops = stops || Kick::StopsCasts(e.Effect, e.ApplyAuraName);
        if (cache.size() < 4096)
            cache.emplace(spellId, stops);
        return stops;
    }

    // A stopper outside the kick table lands on `target`: it is not immune to the spell (its mechanic, its school)
    // nor to every stopping effect (a reworked boss's stun and silence immunity). Kicks keep KickLands.
    bool StopperLands(Unit* target, SpellInfo const* info)
    {
        if (!target || !info || target->IsImmunedToSpell(info, static_cast<Unit const*>(nullptr),
            info->GetSchoolMask()))
            return false;
        for (uint8 i = EFFECT_0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& e = info->Effects[i];
            if (Kick::StopsCasts(e.Effect, e.ApplyAuraName) && !target->IsImmunedToSpellEffect(info, i))
                return true;
        }
        return false;
    }

    Milliseconds RecoveryOf(SpellInfo const* info)
    {
        return Milliseconds(std::max(info->RecoveryTime, info->CategoryRecoveryTime));
    }

    // The kick's source (the bot, or its pet for Spell Lock) reaches `target`: melee range for melee-range kicks,
    // else the spell's range (edges, as the core measures). Line of sight only when asked (it costs a ray).
    bool Reaches(Unit const* source, SpellInfo const* info, Unit const* target, bool sight)
    {
        if (!source || !target)
            return false;
        bool const melee = info->RangeEntry && (info->RangeEntry->Flags & SPELL_RANGE_MELEE);
        bool const inRange = melee ? source->IsWithinMeleeRange(target)
            : source->IsWithinDistInMap(target, info->GetMaxRange(false));
        return inRange && (!sight || source->IsWithinLOSInMap(target));
    }

    // One interruptible cast of an encounter unit, with the start every bot agrees on.
    struct LiveCast
    {
        Unit* caster = nullptr;
        uint32 spellId = 0;
        Milliseconds start{ 0 };
        Milliseconds end{ 0 };
        bool fresh = false;     // not noted yet (Registry::NoteCastStarted)
        Spell const* spell = nullptr;
    };

    void InterruptibleCasts(Unit* unit, Registry const& reg, Milliseconds now, std::vector<LiveCast>& out)
    {
        for (CurrentSpellTypes slot : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
        {
            Spell* spell = unit->GetCurrentSpell(slot);
            SpellInfo const* info = spell ? spell->GetSpellInfo() : nullptr;
            if (!info)
                continue;
            Kick::CastShape shape;
            shape.channel = slot == CURRENT_CHANNELED_SPELL;
            shape.preparing = spell->getState() == SPELL_STATE_PREPARING;
            shape.casting = spell->getState() == SPELL_STATE_CASTING;
            shape.castTime = spell->GetCastTime();
            shape.silencePrevention = info->PreventionType == SPELL_PREVENTION_TYPE_SILENCE;
            shape.interruptFlag = (info->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) != 0;
            shape.channelInterruptFlag = (info->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT) != 0;
            if (!Kick::Interruptible(shape))
                continue;
            int32 const remaining = spell->GetCastTimeRemaining();
            Milliseconds const end = remaining < 0 ? now + kEndlessChannel : now + Milliseconds(remaining);
            int32 total = shape.channel ? info->GetDuration() : spell->GetCastTime();
            if (total < remaining)
                total = std::max(remaining, 0);
            Milliseconds const estimate = end - Milliseconds(total);
            Desc::CastNote const* note = reg.CastNotes().Find(unit->GetGUID().GetRawValue(), info->Id);
            Milliseconds const tolerance = std::min(kStartTolerance, Milliseconds(std::max(total / 2, 50)));
            Kick::Start const start = Kick::CanonicalStart(note ? note->lastStart : std::nullopt, estimate,
                tolerance);
            out.push_back({ unit, info->Id, start.at, end, start.fresh, spell });
        }
    }

    // What a kick on `target` would stop, judged against the bot's descriptor view; the listed casts' keys too (for the
    // cast hook's claim).
    struct TargetJudgement
    {
        Kick::TargetCast cast;
        std::vector<std::pair<Desc::CastKey, Milliseconds>> listed;     // key, end
        Desc::CastKey first;                                            // the first interruptible cast (marks)
    };

    TargetJudgement JudgeTarget(Player* bot, MechanicBotState& st, Registry const& reg, Unit* target,
        Milliseconds kickCooldown, Milliseconds now)
    {
        TargetJudgement j;
        std::vector<LiveCast> casts;
        InterruptibleCasts(target, reg, now, casts);
        if (casts.empty())
            return j;
        j.cast.casting = true;
        Descriptor const& d = st.descriptor.descriptor;
        Desc::Context const ctx = ContextOf(st.descriptor, bot);
        Desc::UnitFacts const& facts = FactsOf(st.descriptor, reg, target, now);
        uint64 const self = bot->GetGUID().GetRawValue();
        // M5 WP-F (R m-2): only the encounter's own units cast listed casts; a trash mob of a listed entry beside the
        // boss is unlisted (`boss kick` never targets it, so its stock kick must stay).
        bool const encounter = st.descriptor.IsEncounterUnit(target->GetGUID());
        for (LiveCast const& c : casts)
        {
            Desc::CastKey const key{ target->GetGUID().GetRawValue(), c.spellId, c.start };
            if (j.first.caster == 0)
                j.first = key;
            std::optional<std::size_t> const entry = encounter ? Desc::MatchInterrupt(d, target->GetEntry(), c.spellId,
                facts, ctx) : std::nullopt;
            if (!entry)
                continue;
            if (d.interrupts[*entry].priority == 0)
            {
                j.cast.doNotKick = true;
                continue;
            }
            j.cast.listed = true;
            ObjectGuid const holder = reg.CastClaimHolder(key);
            if (!holder.IsEmpty() && holder.GetRawValue() != self)
                j.cast.claimedByOther = true;
            j.listed.emplace_back(key, c.end);
        }
        if (!j.cast.listed && !j.cast.doNotKick)
            j.cast.listedDue = Desc::HoldKickCovered(0, now, kickCooldown, st.kick.due, st.kick.others,
                st.kick.dueReach);
        return j;
    }

    // One bot of the group as a kicker.
    struct MemberKick
    {
        Player* member = nullptr;
        Creature* pet = nullptr;            // Spell Lock's caster
        SpellInfo const* info = nullptr;    // the kick it would use
        bool petKick = false;
        Kick::KickerFacts facts;
    };

    std::optional<MemberKick> KickOf(Player* member, Milliseconds now)
    {
        Creature* pet = KickPet(member);
        std::vector<std::size_t> const known = Kick::KnownKicks([&](Kick::KickSpell const& k)
        {
            return k.caster == Kick::Caster::Pet ? pet && pet->HasSpell(k.id) : member->HasSpell(k.id);
        });
        if (known.empty())
            return std::nullopt;

        bool const lostControl = member->HasUnitState(UNIT_STATE_LOST_CONTROL);
        Milliseconds busyUntil{ 0 };
        if (Spell* cast = member->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (cast->getState() == SPELL_STATE_PREPARING && cast->GetCastTimeRemaining() > 0)
                busyUntil = now + Milliseconds(cast->GetCastTimeRemaining());   // its engine yields until then

        std::vector<Kick::KickOption> options;
        std::vector<SpellInfo const*> infos;
        for (std::size_t index : known)
        {
            Kick::KickSpell const& k = Kick::kTable[index];
            SpellInfo const* info = sSpellMgr->GetSpellInfo(k.id);
            if (!info)
                continue;
            bool const viaPet = k.caster == Kick::Caster::Pet;
            Kick::KickOption o;
            o.index = index;
            // Form or stance, and the gear it asks for (Shield Bash: a shield).
            o.formOk = viaPet || (info->CheckShapeshift(member->GetShapeshiftForm()) == SPELL_CAST_OK
                && member->HasItemFitToSpellRequirements(info));
            o.cooldown = RecoveryOf(info);
            o.readyAt = now + Milliseconds(viaPet ? pet->GetSpellCooldown(k.id) : member->GetSpellCooldownDelay(k.id));
            if (!viaPet && member->GetGlobalCooldownMgr().HasGlobalCooldown(info))
                o.readyAt = std::max(o.readyAt, now + kGcdGuess);
            // Lost control, the pet already casting, or short of the kick's rage, energy, mana or runic power.
            Unit const* source = viaPet ? static_cast<Unit const*>(pet) : member;
            bool const shortOfPower = info->PowerType < MAX_POWERS
                && int32(source->GetPower(Powers(info->PowerType)))
                    < info->CalcPowerCost(source, info->GetSchoolMask());
            if (lostControl || shortOfPower || (viaPet && pet->HasUnitState(UNIT_STATE_CASTING)))
                o.readyAt = std::max(o.readyAt, now + kBusyGuess);
            o.readyAt = std::max(o.readyAt, busyUntil);
            options.push_back(o);
            infos.push_back(info);
        }
        std::optional<std::size_t> const pick = Kick::ChooseKick(options, now);
        if (!pick)
            return std::nullopt;
        MemberKick m;
        m.member = member;
        m.pet = pet;
        m.info = infos[*pick];
        m.petKick = Kick::kTable[options[*pick].index].caster == Kick::Caster::Pet;
        m.facts = { member->GetGUID().GetRawValue(), options[*pick].readyAt, options[*pick].cooldown };
        return m;
    }

    bool HasBossMechanics(Player* member)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
        return ai && (ai->HasStrategy("boss mechanics", BOT_STATE_COMBAT)
            || ai->HasStrategy("boss mechanics", BOT_STATE_NON_COMBAT));
    }

    // The bots of the group on the bot's map with the strategy (the bot first when it is alone).
    void GroupBots(Player* bot, std::vector<Player*>& out)
    {
        out.clear();
        Group* group = bot->GetGroup();
        if (!group)
        {
            out.push_back(bot);
            return;
        }
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (!m || !m->IsInWorld() || !m->IsAlive() || m->GetMap() != bot->GetMap() || !HasBossMechanics(m))
                continue;
            out.push_back(m);
        }
        if (std::find(out.begin(), out.end(), bot) == out.end())
            out.push_back(bot);
    }

    void ClearPlan(Kick::KickState& k)
    {
        k.target = Desc::CastKey();
        k.targetEnd = Milliseconds(0);
        k.priority = 0;
        k.kickSpell = 0;
        k.petKick = false;
    }

    // Spell Lock as the core casts a player's pet spell (WorldSession::HandlePetActionHelper): the pet is the caster,
    // Spell::CheckPetCast, the pet's cooldown, prepare (playerbots' CastPetSpellAction).
    bool CastPetKick(Player* bot, SpellInfo const* info, Unit* target)
    {
        Creature* pet = KickPet(bot);
        if (!pet || !pet->HasSpell(info->Id) || pet->HasSpellCooldown(info->Id)
            || pet->HasUnitState(UNIT_STATE_CASTING))
            return false;
        Spell* spell = new Spell(pet, info, TRIGGERED_NONE);
        spell->m_targets.SetUnitTarget(target);
        spell->LoadScripts();
        SpellCastResult result = spell->CheckPetCast(target);
        if (result == SPELL_FAILED_UNIT_NOT_INFRONT && !pet->isPossessed() && !pet->IsVehicle())
        {
            pet->SetInFront(target);
            result = SPELL_CAST_OK;
        }
        if (result != SPELL_CAST_OK)
        {
            spell->finish(false);
            delete spell;
            return false;
        }
        bool const wasFollowing = pet->HasUnitState(UNIT_STATE_FOLLOW);
        pet->ClearUnitState(UNIT_STATE_FOLLOW);
        if (!info->IsCooldownStartedOnEvent())
            pet->AddSpellCooldown(info->Id, 0, 0);
        bool const started = spell->prepare(&spell->m_targets) == SPELL_CAST_OK;
        if (wasFollowing && !pet->IsInCombat())
            pet->AddUnitState(UNIT_STATE_FOLLOW);
        return started;
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

    char const* VerdictName(Kick::StockVerdict v)
    {
        switch (v)
        {
            case Kick::StockVerdict::DoNotKick: return "do-not-kick";
            case Kick::StockVerdict::Claimed: return "claimed";
            case Kick::StockVerdict::Listed: return "listed";
            case Kick::StockVerdict::Hold: return "hold";
            default: return "allow";
        }
    }
}

void BotKick::Refresh(PlayerbotAI* botAI, MechanicBotState& st, Milliseconds now)
{
    Kick::KickState& k = st.kick;
    if (k.planAt == now)
        return;
    k.planAt = now;
    ClearPlan(k);
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    Registry* reg = bot && bot->IsInWorld() ? Registry::Get(bot->GetMap()) : nullptr;
    RefreshDescriptorView(reg, bot, st.descriptor);
    k.active = reg && st.descriptor.Active() && !st.descriptor.descriptor.interrupts.empty();
    if (!k.active)
    {
        k.due.clear();
        k.others.clear();
        k.dueReach.clear();
        k.seen.clear();
        k.cooldown = Milliseconds(0);
        k.observer = false;
        return;
    }

    Descriptor const& d = st.descriptor.descriptor;
    Desc::Context const ctx = ContextOf(st.descriptor, bot);
    uint64 const self = bot->GetGUID().GetRawValue();

    // The encounter's interruptible casts (owner first, then the tracked units). M5 WP-F (R m-3): not on a unit the
    // ignore list holds now (a hostile hit there costs what the list exists to avoid).
    std::vector<LiveCast> live;
    auto gather = [&](ObjectGuid guid)
    {
        if (Unit* u = ObjectAccessor::GetUnit(*bot, guid); u && u->IsAlive() && u->IsInWorld()
            && !BotTargets::IsIgnoredNow(st, *reg, bot, u, now))
            InterruptibleCasts(u, *reg, now, live);
    };
    gather(st.descriptor.owner);
    for (EncounterUnit const& u : st.descriptor.units)
        gather(u.guid);

    std::vector<Desc::CastFacts> casts;
    casts.reserve(live.size());
    std::vector<Kick::SeenCast> current;
    for (LiveCast const& c : live)
    {
        Desc::UnitFacts const& facts = FactsOf(st.descriptor, *reg, c.caster, now);
        Desc::CastKey const key{ c.caster->GetGUID().GetRawValue(), c.spellId, c.start };
        ObjectGuid const holder = reg->CastClaimHolder(key);
        Desc::CastFacts f;
        f.caster = key.caster;
        f.casterEntry = c.caster->GetEntry();
        f.spellId = c.spellId;
        f.start = c.start;
        f.end = c.end;
        f.claimedByOther = !holder.IsEmpty() && holder.GetRawValue() != self;
        f.facts = &facts;
        casts.push_back(f);
        std::optional<std::size_t> const entry = Desc::MatchInterrupt(d, f.casterEntry, f.spellId, facts, ctx);
        if (!entry || d.interrupts[*entry].priority == 0)
            continue;
        current.push_back({ key, c.end });
        // A listed cast seen starting: its note anchors the `every` cadence and every bot's claim key.
        if (c.fresh)
            reg->NoteCastStarted(c.caster->GetGUID(), f.casterEntry, f.spellId, c.start);
    }

    // Due: what the notes predict, and every unclaimed listed cast in progress (due now). M5 WP-F (R m-2): the notes
    // of the encounter's own units only (the map's notes hold every caster's), and not of an ignored one.
    std::vector<Desc::CastNote> notes;
    notes.reserve(st.descriptor.notes.size());
    for (Desc::CastNote const& n : st.descriptor.notes)
    {
        ObjectGuid const caster(n.caster);
        if (!st.descriptor.IsEncounterUnit(caster))
            continue;
        if (Unit* u = ObjectAccessor::GetUnit(*bot, caster); u && BotTargets::IsIgnoredNow(st, *reg, bot, u, now))
            continue;
        notes.push_back(n);
    }
    k.due = Desc::DueCasts(d, notes, ctx, now, [&](Desc::Key key) -> Desc::UnitFacts const*
    {
        Unit* u = ObjectAccessor::GetUnit(*bot, ObjectGuid(key));
        return u ? &FactsOf(st.descriptor, *reg, u, now) : nullptr;
    });
    std::vector<Desc::KickOption> const order = Desc::OrderInterrupts(d, casts, ctx, now);
    for (Desc::KickOption const& o : order)
        k.due.push_back({ o.priority, now, casts[o.cast].caster });

    // The group's kickers.
    thread_local std::vector<Player*> members;
    GroupBots(bot, members);
    std::vector<MemberKick> kickers;
    uint64 lowest = self;
    for (Player* m : members)
    {
        lowest = std::min(lowest, m->GetGUID().GetRawValue());
        if (std::optional<MemberKick> mk = KickOf(m, now))
            kickers.push_back(*mk);
    }
    k.observer = lowest == self;
    std::size_t selfIndex = kickers.size();
    k.others.clear();
    k.cooldown = Milliseconds(0);
    k.readyAt = Milliseconds(0);
    for (std::size_t i = 0; i < kickers.size(); ++i)
    {
        if (kickers[i].member == bot)
        {
            selfIndex = i;
            k.cooldown = kickers[i].facts.cooldown;
            k.readyAt = kickers[i].facts.readyAt;
        }
        else
            k.others.push_back({ kickers[i].facts.bot, kickers[i].facts.readyAt });
    }
    // M5 WP-F (R m-9): who reaches each due cast's caster, so the hold does not count a kicker out of range of it as
    // cover. A noted caster that does not resolve keeps the plain rule (Desc::DueReaches).
    k.dueReach.clear();
    for (Desc::DueCast const& due : k.due)
    {
        if (!due.caster || std::any_of(k.dueReach.begin(), k.dueReach.end(), [&due](Desc::DueReach const& r)
        {
            return r.caster == due.caster;
        }))
            continue;
        Unit* caster = ObjectAccessor::GetUnit(*bot, ObjectGuid(due.caster));
        if (!caster || !caster->IsInWorld())
            continue;
        Desc::DueReach r;
        r.caster = due.caster;
        for (MemberKick const& m : kickers)
        {
            Unit const* source = m.petKick ? static_cast<Unit const*>(m.pet) : m.member;
            if (KickLands(caster, m.info) && Reaches(source, m.info, caster, false))
                r.kickers.push_back(m.facts.bot);
        }
        k.dueReach.push_back(std::move(r));
    }

    // Misses: a listed cast seen last tick and gone now, at its end, with no claim on it (the group's observer).
    for (Kick::SeenCast const& s : k.seen)
    {
        bool const still = std::any_of(current.begin(), current.end(), [&s](Kick::SeenCast const& c)
        {
            return c.key == s.key;
        });
        if (still)
            continue;
        bool const claimed = !reg->CastClaimHolder(s.key).IsEmpty();
        if (k.observer && Kick::Missed(s.end, now, claimed, kMissSlack)
            && Kick::MarkOnce(k.marks, s.key, MarkMiss, now, kMarkKeep))
        {
            ++st.counters.listedMissed;
            if (GetConfig().bots.debug)
                LOG_INFO(kLog, "Bot {}: listed cast {} of {} completed (missed)", bot->GetName(), s.key.spellId,
                    ObjectGuid(s.key.caster).ToString());
        }
    }
    k.seen = std::move(current);

    if (selfIndex == kickers.size() || order.empty())
        return;

    // Who kicks what.
    std::vector<Kick::KickerFacts> facts;
    facts.reserve(kickers.size());
    for (MemberKick const& m : kickers)
        facts.push_back(m.facts);
    std::vector<Kick::CandidateFacts> candidates;
    candidates.reserve(order.size());
    for (Desc::KickOption const& o : order)
    {
        Desc::CastFacts const& c = casts[o.cast];
        Unit* caster = ObjectAccessor::GetUnit(*bot, ObjectGuid(c.caster));
        Kick::CandidateFacts cand;
        cand.priority = o.priority;
        cand.start = c.start;
        for (MemberKick const& m : kickers)
        {
            Unit const* source = m.petKick ? static_cast<Unit const*>(m.pet) : m.member;
            cand.inRange.push_back(caster && KickLands(caster, m.info) && Reaches(source, m.info, caster, false));
        }
        candidates.push_back(std::move(cand));
    }
    Kick::Assignment const a = Kick::Assign(candidates, facts, selfIndex, k.due, now, kDeferAfter, k.dueReach);
    for (std::size_t held : a.held)
    {
        Desc::CastFacts const& c = casts[order[held].cast];
        Desc::CastKey const key{ c.caster, c.spellId, c.start };
        if (Kick::MarkOnce(k.marks, key, MarkHold, now, kMarkKeep))
        {
            ++st.counters.overloadHolds;
            if (GetConfig().bots.debug)
                LOG_INFO(kLog, "Bot {}: holds its kick on {} (priority {}) for a higher cast due", bot->GetName(),
                    c.spellId, order[held].priority);
        }
    }
    if (!a.cast)
        return;
    Desc::KickOption const& o = order[*a.cast];
    Desc::CastFacts const& c = casts[o.cast];
    k.target = { c.caster, c.spellId, c.start };
    k.targetEnd = c.end;
    k.priority = o.priority;
    k.kickSpell = kickers[selfIndex].info->Id;
    k.petKick = kickers[selfIndex].petKick;
}

void BotKick::OnBotSpellCast(Player* player, Spell const* spell)
{
    if (!player || !spell || spell->IsTriggered() || !spell->GetSpellInfo())
        return;
    SpellInfo const* info = spell->GetSpellInfo();
    if (!IsCastStopper(info->Id) || !BotGate::IsEnabled() || !Registry::Get(player->GetMap()))
        return;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    MechanicBotState* st = botAI ? BotBrain::StateOf(botAI) : nullptr;
    Unit* target = spell->m_targets.GetUnitTarget();
    if (!st || !target || target == player)
        return;
    Milliseconds const now = Now();
    Refresh(botAI, *st, now);
    Registry* reg = Registry::Get(player->GetMap());
    if (!st->kick.active || !reg)
        return;
    // M5 WP-F (R M-2): a stun, silence, incapacitate or fear stops the cast as a kick does, and is judged as one; one
    // the target is immune to stops nothing.
    bool const kick = Kick::Find(info->Id) != nullptr;
    if (!kick && !StopperLands(target, info))
        return;

    TargetJudgement j = JudgeTarget(player, *st, *reg, target, RecoveryOf(info), now);
    bool claimed = false;
    if (j.cast.listed && !j.cast.doNotKick && !j.cast.claimedByOther)
    {
        claimed = true;
        for (auto const& [key, end] : j.listed)
            claimed = reg->ClaimCast(key, player->GetGUID(), end + kClaimGrace) && claimed;
    }
    MechanicBotCounters& c = st->counters;
    Kick::Outcome const outcome = Kick::Classify(j.cast, claimed);
    switch (outcome)
    {
        case Kick::Outcome::Landed:
            ++c.kicksLanded;
            break;
        case Kick::Outcome::Duplicate:
            ++c.duplicateKicks;
            break;
        case Kick::Outcome::DoNotKick:
            ++c.doNotKickKicks;
            break;
        case Kick::Outcome::Wasted:
            ++c.kicksWasted;
            break;
        default:
            break;
    }
    if (!kick && outcome != Kick::Outcome::None && outcome != Kick::Outcome::Unlisted)
        ++c.castStops;
    if (GetConfig().bots.debug && outcome != Kick::Outcome::None)
    {
        static char const* const kNames[] = { "none", "landed", "duplicate", "DO-NOT-KICK", "wasted", "unlisted" };
        LOG_INFO(kLog, "Bot {}: {} {} on {} ({}): {}", player->GetName(), kick ? "kick" : "cast stopper", info->Id,
            target->GetName(), j.first.spellId, kNames[static_cast<std::size_t>(outcome)]);
    }
}

uint64 BotKick::WakeSignature(Player* bot, MechanicBotState const& st)
{
    Kick::KickState const& k = st.kick;
    DescriptorView const& v = st.descriptor;
    if (!bot || !k.active || k.cooldown <= Milliseconds(0) || !v.Active() || v.descriptor.interrupts.empty())
        return 0;
    Registry const* reg = Registry::Get(bot->GetMap());
    if (!reg)
        return 0;
    Milliseconds const now = Now();
    uint64 sig = 0;
    std::vector<LiveCast> casts;
    auto scan = [&](ObjectGuid guid)
    {
        Unit* u = ObjectAccessor::GetUnit(*bot, guid);
        if (!u || !u->IsAlive() || !u->IsInWorld())
            return;
        casts.clear();
        InterruptibleCasts(u, *reg, now, casts);
        for (LiveCast const& c : casts)
        {
            for (InterruptEntry const& e : v.descriptor.interrupts)
            {
                if (e.priority == 0 || e.spellId != c.spellId || (e.casterEntry && e.casterEntry != u->GetEntry()))
                    continue;
                // M5 WP-F (R n-2): the cast's canonical start once a bot noted it (a recycled Spell allocation for
                // the next cast of the spell still changes the signature); the Spell address until then.
                uint64 const instance = c.fresh ? static_cast<uint64>(reinterpret_cast<std::uintptr_t>(c.spell))
                    : static_cast<uint64>(c.start.count());
                sig = LockRules::MixWakeSignature(sig, guid.GetRawValue(), c.spellId, instance);
                break;
            }
        }
    };
    scan(v.owner);
    for (EncounterUnit const& u : v.units)
        scan(u.guid);
    return sig;
}

std::string BotKick::ReportLine(MechanicBotState const& st)
{
    MechanicBotCounters const& c = st.counters;
    Kick::KickState const& k = st.kick;
    std::string plan = "-";
    if (k.target.caster)
        plan = Acore::StringFormat("kick {} on {} cast {} (priority {})", k.kickSpell,
            ObjectGuid(k.target.caster).ToString(), k.target.spellId, k.priority);
    return Acore::StringFormat("  m5 kicks: landed {}, wasted {}, do-not-kick {}, duplicates {}, overload holds {}, "
        "listed missed {}{}, stock kicks dropped {}, stuns/silences judged {} | {}{} | {}", c.kicksLanded,
        c.kicksWasted, c.doNotKickKicks, c.duplicateKicks, c.overloadHolds, c.listedMissed,
        k.observer ? " (observer)" : "", c.stockKicksDropped, c.castStops,
        k.active ? "interrupt list on" : "no interrupt list", k.active && k.cooldown == Milliseconds(0) ? ", no kick"
        : "", plan);
}

Unit* BossInterruptTargetValue::Calculate()
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return nullptr;
    BotKick::Refresh(botAI, *st, Now());
    if (!st->kick.target.caster)
        return nullptr;
    return ObjectAccessor::GetUnit(*bot, ObjectGuid(st->kick.target.caster));
}

bool BossInterruptDueTrigger::IsActive()
{
    return AI_VALUE(Unit*, "boss interrupt target") != nullptr;
}

bool BossKickAction::isUseful()
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return false;
    BotKick::Refresh(botAI, *st, Now());
    return st->kick.target.caster != 0;
}

bool BossKickAction::Execute(Event /*event*/)
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return false;
    Milliseconds const now = Now();
    BotKick::Refresh(botAI, *st, now);
    Kick::KickState& k = st->kick;
    Registry* reg = Registry::Get(bot->GetMap());
    Unit* caster = k.target.caster ? ObjectAccessor::GetUnit(*bot, ObjectGuid(k.target.caster)) : nullptr;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(k.kickSpell);
    if (!reg || !caster || !caster->IsAlive() || !info)
        return false;

    // The bot's own kick: castable now, in range and sight; a kick that turns the bot keeps out of a look-away window.
    if (!k.petKick)
    {
        if (!botAI->CanCastSpell(info->Id, caster, true) || !Reaches(bot, info, caster, true))
            return false;
        if (BotFacing::TurnsCaster(info) && BotFacing::WouldTurnAway(bot, *st, now, caster))
            return false;
    }
    else if (!Reaches(KickPet(bot), info, caster, true))
        return false;

    if (!reg->ClaimCast(k.target, bot->GetGUID(), k.targetEnd + kClaimGrace))
        return false;
    bool const cast = k.petKick ? CastPetKick(bot, info, caster) : botAI->CastSpell(info->Id, caster);
    if (!cast)
    {
        reg->ReleaseCast(k.target, bot->GetGUID());
        return false;
    }
    // The cast hook sees the bot's own casts only.
    if (k.petKick)
        ++st->counters.kicksLanded;
    if (GetConfig().bots.debug)
        LOG_INFO(kLog, "Bot {}: boss kick {} on {} cast {} (priority {}, {} ms left)", bot->GetName(), info->Id,
            caster->GetName(), k.target.spellId, k.priority, (k.targetEnd - now).count());
    k.planAt = Milliseconds(-1);    // re-plan on the next look
    return true;
}

float MechanicBotKickMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.f;
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return 1.f;
    Milliseconds const now = Now();
    BotKick::Refresh(botAI, *st, now);
    if (!st->kick.active || dynamic_cast<BossKickAction*>(action))
        return 1.f;
    CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action);
    if (!cast)
        return 1.f;
    uint32 const spellId = CastSpellIdOf(botAI, cast);
    SpellInfo const* info = spellId && IsCastStopper(spellId) ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!info)
        return 1.f;
    Unit* target = action->GetTarget();
    Registry const* reg = Registry::Get(bot->GetMap());
    if (!target || !reg || target == bot)
        return 1.f;
    // M5 WP-F (R M-2): a stun or silence used as an interrupt (`hammer of justice interrupt`, `bear bash`, a rotation
    // stun on a casting target) is judged as a kick; one the target is immune to stops nothing and stays.
    if (!Kick::Find(spellId) && !StopperLands(target, info))
        return 1.f;
    TargetJudgement const j = JudgeTarget(bot, *st, *reg, target, RecoveryOf(info), now);
    Kick::StockVerdict const verdict = Kick::JudgeStockKick(j.cast);
    if (verdict == Kick::StockVerdict::Allow)
        return 1.f;
    if (Kick::MarkOnce(st->kick.marks, j.first, MarkStockDrop, now, kMarkKeep))
    {
        ++st->counters.stockKicksDropped;
        if (GetConfig().bots.debug)
            LOG_INFO(kLog, "Bot {}: stock `{}` on {} cast {} dropped ({})", bot->GetName(), action->getName(),
                target->GetName(), j.first.spellId, VerdictName(verdict));
    }
    return 0.f;
}

#endif
