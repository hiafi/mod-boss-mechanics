/*
 * mod-boss-mechanics: the bots' boss dispels and purges (STAGE-M5-HANDOFF §3.4, WP-P). See MechanicBotDispel.h for
 * the names, MechanicDispelRules.h for the class tables and the deadline timing.
 *
 * Cost: every trigger first brings the descriptor view up to date (a revision compare when nothing changed) and
 * returns at once without an active descriptor that lists dispels (purges), or when the bot has no spell of the
 * table. Otherwise it walks the group (as the stock value does) and looks up the listed auras by id on each member,
 * or on the encounter's tracked units for purges: no scan of the map's units.
 *
 * The pet cast (Devour Magic) mirrors the fork's CastPetSpellAction (CheckCast and Execute): PlayerbotAI::CastSpell
 * does not cast a pet's spell from the pet.
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotDispel.h"
#include "AiObjectContext.h"
#include "BossMechanicsConfig.h"
#include "Creature.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MechanicBotBrain.h"
#include "MechanicBotState.h"
#include "MechanicDescriptorView.h"
#include "MechanicDispelRules.h"
#include "MechanicLockRules.h"
#include "MechanicRegistry.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "UnitScript.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace BossMechanics;
using DispelRules::Ms;

namespace
{
    constexpr char const* kLog = "module.bossmechanics.bots";
    constexpr Ms kPermanent{ std::numeric_limits<int32>::max() };
    constexpr float kPurgeScanYd = 60.f;    // tracked units further than this are not looked at
    // A bot that casts on a listed aura claims it this long (the registry's kick claims, keyed by the afflicted unit
    // and the aura id), so the other bots do not spend a GCD on the same aura; a failed cast releases it at once.
    constexpr Milliseconds kClaimMs{ 1000 };

    // Playerbots.DispelAuraDuration: HasAuraToDispel passes over auras with less than this left.
    Ms StockSkip()
    {
        return Ms(sPlayerbotAIConfig.DispelAuraDuration);
    }

    // The dispel types a spell removes, from its live data (DISPEL, and STEAL_BENEFICIAL_BUFF for Spellsteal).
    uint32 SpellTypes(SpellInfo const* info)
    {
        uint32 mask = 0;
        if (!info)
            return mask;
        for (SpellEffectInfo const& eff : info->GetEffects())
            if (eff.Effect == SPELL_EFFECT_DISPEL || eff.Effect == SPELL_EFFECT_STEAL_BENEFICIAL_BUFF)
                mask |= DispelRules::Bit(static_cast<uint32>(eff.MiscValue));
        return mask;
    }

    bool InTables(uint32 spellId)
    {
        static std::unordered_set<uint32> const ids = []
        {
            std::unordered_set<uint32> out;
            for (DispelRules::ClassSpell const& s : DispelRules::kSpells)
                out.insert(s.spellId);
            return out;
        }();
        return ids.count(spellId) != 0;
    }

    Ms Remaining(Aura const* aura)
    {
        int32 const left = aura->GetDuration();
        return left < 0 ? kPermanent : Ms(left);
    }

    // The bot's descriptor, brought up to date; nullptr when none applies to it.
    Descriptor const* ActiveDescriptor(Player* bot, MechanicBotState& st)
    {
        RefreshDescriptorView(Registry::Get(bot->GetMap()), bot, st.descriptor);
        return st.descriptor.Active() ? &st.descriptor.descriptor : nullptr;
    }

    Creature* FindPet(Player* bot)
    {
        Creature* pet = bot->GetPet();
        if (!pet)
            pet = bot->GetGuardianPet();
        if (!pet || !pet->IsAlive() || !pet->IsInWorld() || !pet->IsInMap(bot))
            return nullptr;
        return pet;
    }

    // The bot's spells of the class table for one use, in table order (not yet checked against a target).
    std::vector<DispelRules::Owned> OwnedSpells(Player* bot, DispelRules::Use use)
    {
        std::vector<DispelRules::Owned> out;
        Creature* pet = nullptr;
        bool petLooked = false;
        for (DispelRules::ClassSpell const& s : DispelRules::kSpells)
        {
            if (s.classId != bot->getClass() || s.use != use)
                continue;
            if (s.pet)
            {
                if (!petLooked)
                {
                    pet = FindPet(bot);
                    petLooked = true;
                }
                if (!pet || !pet->HasSpell(s.spellId))
                    continue;
            }
            else if (!bot->HasSpell(s.spellId))
                continue;
            uint32 const types = SpellTypes(sSpellMgr->GetSpellInfo(s.spellId));
            if (types)
                out.push_back({ s.spellId, types, s.pet, false });
        }
        return out;
    }

    // Can the bot (or its pet) cast this spell on the target now? `mayInterrupt`: a cast in progress is not a reason
    // to wait (the urgent path interrupts it).
    bool Usable(PlayerbotAI* botAI, Player* bot, MechanicBotState const& st, DispelRules::Owned const& o, Unit* target,
        bool hostile, bool mayInterrupt)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(o.spellId);
        if (!info || !target)
            return false;
        if (o.pet)
        {
            Creature* pet = FindPet(bot);
            return pet && pet->HasSpell(o.spellId) && !pet->HasSpellCooldown(o.spellId)
                && !pet->HasUnitState(UNIT_STATE_CASTING) && pet->IsWithinDistInMap(target, info->GetMaxRange(!hostile,
                pet)) && pet->IsWithinLOSInMap(target);
        }
        if (bot->HasUnitState(UNIT_STATE_LOST_CONTROL) || bot->HasSpellCooldown(o.spellId)
            || bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
            return false;
        if (info->CheckShapeshift(bot->GetShapeshiftForm()) != SPELL_CAST_OK)
            return false;
        // A cast-time spell (none of the table's today) waits while the bot moves or the brain owns it.
        if (info->CalcCastTime(bot) > 0 && (bot->isMoving() || st.mode == MechanicBotMode::Escape
            || st.mode == MechanicBotMode::Hold || st.mode == MechanicBotMode::Freeze))
            return false;
        if (target != bot && (!bot->IsWithinDistInMap(target, info->GetMaxRange(!hostile, bot))
            || !bot->IsWithinLOSInMap(target)))
            return false;
        // CanCastSpell builds a Spell and runs CheckCast (power, immunity, ...); a cast in progress would fail it.
        if (bot->IsNonMeleeSpellCast(false, false, true))
            return mayInterrupt;
        return botAI->CanCastSpell(o.spellId, target, true);
    }

    // CastPetSpellAction::CheckCast + Execute (fork), for a unit target.
    bool CastPetSpell(Player* bot, uint32 spellId, Unit* target)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        Creature* pet = FindPet(bot);
        if (!info || info->IsPassive() || !pet || !target || !target->IsInWorld() || !target->IsInMap(bot)
            || !pet->HasSpell(spellId))
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
            pet->AddSpellCooldown(spellId, 0, 0);
        bool const started = spell->prepare(&spell->m_targets) == SPELL_CAST_OK;
        if (wasFollowing && !pet->IsInCombat())
            pet->AddUnitState(UNIT_STATE_FOLLOW);
        return started;
    }

    // The group in PartyMemberValue::FindPartyMember's order (master, healers, tanks, others; each player's pet right
    // after it), with the stock rank. No group: the bot and its pet.
    template <class Fn>
    void ForEachMember(PlayerbotAI* botAI, Player* bot, Fn&& fn)
    {
        auto withPet = [&](Player* player, DispelRules::MemberRank rank)
        {
            fn(player, rank);
            if (Pet* pet = player->GetPet())
                fn(pet, rank);
        };
        Group* group = bot->GetGroup();
        if (!group)
        {
            withPet(bot, DispelRules::MemberRank::Other);
            return;
        }
        std::vector<ObjectGuid> guids;
        std::vector<ObjectGuid> others;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* p = ref->GetSource())
                (ref->getSubGroup() == bot->GetSubGroup() ? guids : others).push_back(p->GetGUID());
        // The stock list puts the bot's own subgroup first.
        guids.insert(guids.end(), others.begin(), others.end());

        Player* master = botAI->GetMaster();
        std::vector<Player*> healers;
        std::vector<Player*> tanks;
        std::vector<Player*> rest;
        for (ObjectGuid const& guid : guids)
        {
            Player* player = botAI->GetPlayer(guid);
            if (!player)
                continue;
            if (PlayerbotAI::IsHeal(player))
                healers.push_back(player);
            else if (PlayerbotAI::IsTank(player))
                tanks.push_back(player);
            else if (player != master)
                rest.push_back(player);
        }
        if (master)
            withPet(master, DispelRules::MemberRank::Master);
        for (Player* p : healers)
            withPet(p, DispelRules::MemberRank::Healer);
        for (Player* p : tanks)
            withPet(p, DispelRules::MemberRank::Tank);
        for (Player* p : rest)
            withPet(p, DispelRules::MemberRank::Other);
    }

    // One listed harmful aura on a member.
    struct Listed
    {
        Unit* unit = nullptr;
        uint32 type = 0;
        Aura const* aura = nullptr;
    };

    // The listed harmful auras of the types in `typeMask` on one member. `stockSkip`: pass over a not-yet-urgent
    // aura with less than the stock skip left, as HasAuraToDispel does.
    void Gather(Descriptor const& view, Unit* member, DispelRules::MemberRank rank, uint32 typeMask, bool stockSkip,
        std::vector<Desc::Affliction>& afflictions, std::vector<Listed>& where)
    {
        for (DispelEntry const& e : view.dispels)
        {
            Aura const* aura = member->GetAura(e.auraId);
            if (!aura || aura->IsRemoved())
                continue;
            SpellInfo const* info = aura->GetSpellInfo();
            if (!info || info->IsPositive() || !(typeMask & DispelRules::Bit(info->Dispel)))
                continue;
            Ms const left = Remaining(aura);
            if (stockSkip && !Desc::IsUrgent(e, left)
                && DispelRules::NormalSkipped(Ms(aura->GetDuration()), StockSkip()))
                continue;
            afflictions.push_back({ member->GetGUID().GetRawValue(), e.auraId, left, static_cast<uint8>(rank) });
            where.push_back({ member, info->Dispel, aura });
        }
    }

    struct Plan
    {
        Unit* target = nullptr;
        uint32 spellId = 0;
        bool pet = false;
        uint32 auraId = 0;
        bool urgentEntry = false;   // loss of control (else a deadline)
    };

    bool AliveHere(Unit* unit, Player* bot)
    {
        return unit && unit->IsAlive() && unit->IsInWorld() && unit->IsInMap(bot);
    }

    Desc::CastKey ClaimKey(Unit const* unit, uint32 auraId)
    {
        return Desc::CastKey{ unit->GetGUID().GetRawValue(), auraId, Ms(0) };
    }

    // Another bot cures (strips) this aura right now.
    bool ClaimedByOther(Player* bot, Unit const* unit, uint32 auraId)
    {
        Registry const* reg = Registry::Get(bot->GetMap());
        if (!reg)
            return false;
        ObjectGuid const holder = reg->CastClaimHolder(ClaimKey(unit, auraId));
        return !holder.IsEmpty() && holder != bot->GetGUID();
    }

    // `boss dispel urgent`: the first urgent pick the bot can cure now.
    std::optional<Plan> PlanUrgent(PlayerbotAI* botAI, Player* bot, MechanicBotState& st)
    {
        Descriptor const* d = ActiveDescriptor(bot, st);
        if (!d || d->dispels.empty())
            return std::nullopt;
        std::vector<DispelRules::Owned> owned = OwnedSpells(bot, DispelRules::Use::Friendly);
        uint32 const mask = DispelRules::TypesOf(owned);
        if (!mask)
            return std::nullopt;

        Descriptor const view = DispelRules::UrgentView(*d, StockSkip());
        std::vector<Desc::Affliction> afflictions;
        std::vector<Listed> where;
        ForEachMember(botAI, bot, [&](Unit* member, DispelRules::MemberRank rank)
        {
            if (AliveHere(member, bot))
                Gather(view, member, rank, mask, false, afflictions, where);
        });
        for (Desc::DispelPick const& pick : Desc::OrderDispels(view, afflictions, true))
        {
            Listed const& l = where[pick.affliction];
            if (ClaimedByOther(bot, l.unit, afflictions[pick.affliction].auraId))
                continue;
            for (DispelRules::Owned& o : owned)
                o.ready = (o.types & DispelRules::Bit(l.type)) && Usable(botAI, bot, st, o, l.unit, false, true);
            if (std::optional<std::size_t> const i = DispelRules::PickSpell(owned, l.type))
                return Plan{ l.unit, owned[*i].spellId, owned[*i].pet, afflictions[pick.affliction].auraId,
                    view.dispels[pick.entry].urgent };
        }
        return std::nullopt;
    }

    // `boss purge`: the best listed buff on a tracked unit the bot can strip now.
    std::optional<Plan> PlanPurge(PlayerbotAI* botAI, Player* bot, MechanicBotState& st)
    {
        if (!bot->IsInCombat())
            return std::nullopt;
        Descriptor const* d = ActiveDescriptor(bot, st);
        if (!d || d->purges.empty())
            return std::nullopt;
        std::vector<DispelRules::Owned> owned = OwnedSpells(bot, DispelRules::Use::Hostile);
        uint32 const mask = DispelRules::TypesOf(owned);
        if (!mask)
            return std::nullopt;

        std::vector<ObjectGuid> guids{ st.descriptor.owner };
        for (EncounterUnit const& u : st.descriptor.units)
            guids.push_back(u.guid);
        std::vector<Desc::EnemyBuff> buffs;
        std::vector<Listed> where;
        for (ObjectGuid const& guid : guids)
        {
            Unit* unit = ObjectAccessor::GetUnit(*bot, guid);
            if (!AliveHere(unit, bot) || !bot->IsWithinDistInMap(unit, kPurgeScanYd) || !bot->IsValidAttackTarget(unit))
                continue;
            for (PurgeEntry const& e : d->purges)
            {
                if (e.unitEntry && e.unitEntry != unit->GetEntry())
                    continue;
                Aura const* aura = unit->GetAura(e.auraId);
                if (!aura || aura->IsRemoved() || !(mask & DispelRules::Bit(aura->GetSpellInfo()->Dispel)))
                    continue;
                buffs.push_back({ guid.GetRawValue(), unit->GetEntry(), e.auraId });
                where.push_back({ unit, aura->GetSpellInfo()->Dispel, aura });
            }
        }
        while (!buffs.empty())
        {
            std::optional<Desc::PurgePick> const pick = Desc::PickPurge(*d, buffs);
            if (!pick)
                break;
            Listed const& l = where[pick->buff];
            bool const claimed = ClaimedByOther(bot, l.unit, buffs[pick->buff].auraId);
            for (DispelRules::Owned& o : owned)
                o.ready = !claimed && (o.types & DispelRules::Bit(l.type)) && Usable(botAI, bot, st, o, l.unit, true,
                    false);
            if (std::optional<std::size_t> const i = DispelRules::PickSpell(owned, l.type))
                return Plan{ l.unit, owned[*i].spellId, owned[*i].pet, buffs[pick->buff].auraId, false };
            buffs.erase(buffs.begin() + static_cast<std::ptrdiff_t>(pick->buff));
            where.erase(where.begin() + static_cast<std::ptrdiff_t>(pick->buff));
        }
        return std::nullopt;
    }

    // Counts one cure or purge cast by `caster` (or its pet) of `spellId` on `target`, before it lands.
    void CountCast(Player* caster, MechanicBotState& st, uint32 spellId, Unit* target)
    {
        if (!target || !target->IsInWorld())
            return;
        Descriptor const* d = ActiveDescriptor(caster, st);
        if (!d)
            return;
        uint32 const types = SpellTypes(sSpellMgr->GetSpellInfo(spellId));
        if (!types)
            return;
        MechanicBotCounters& c = st.counters;
        if (caster->IsFriendlyTo(target))
        {
            if (d->dispels.empty())
                return;
            std::vector<DispelRules::TargetAura> auras;
            std::vector<Aura const*> handles;
            for (DispelEntry const& e : d->dispels)
                if (Aura const* aura = target->GetAura(e.auraId))
                    if (!aura->IsRemoved() && !aura->GetSpellInfo()->IsPositive())
                    {
                        auras.push_back({ e.auraId, aura->GetSpellInfo()->Dispel, Ms(aura->GetDuration()) });
                        handles.push_back(aura);
                    }
            DispelRules::CastClass const cls = DispelRules::ClassifyCast(DispelRules::UrgentView(*d, StockSkip()),
                types, auras);
            switch (cls.kind)
            {
                case DispelRules::CastKind::Urgent:
                {
                    ++c.urgentDispels;
                    Aura const* aura = handles[cls.aura];
                    if (aura->GetMaxDuration() > 0)
                        c.urgentLatencyMsMax = std::max<uint32>(c.urgentLatencyMsMax,
                            static_cast<uint32>(std::max(0, aura->GetMaxDuration() - aura->GetDuration())));
                    break;
                }
                case DispelRules::CastKind::Deadline:
                    ++c.deadlineDispels;
                    break;
                case DispelRules::CastKind::Normal:
                    ++c.listedDispels;
                    break;
                case DispelRules::CastKind::None:
                    return;
            }
            if (GetConfig().bots.debug)
                LOG_INFO(kLog, "Bot {}: dispel {} on {} ({}, aura {})", caster->GetName(), spellId, target->GetName(),
                    cls.kind == DispelRules::CastKind::Urgent ? "urgent" : cls.kind == DispelRules::CastKind::Deadline
                    ? "deadline" : "listed", auras[cls.aura].auraId);
            return;
        }
        if (d->purges.empty())
            return;
        std::vector<DispelRules::EnemyAura> auras;
        for (PurgeEntry const& e : d->purges)
            if (Aura const* aura = target->GetAura(e.auraId))
                if (!aura->IsRemoved())
                    auras.push_back({ e.auraId, aura->GetSpellInfo()->Dispel });
        if (!DispelRules::CountsAsPurge(*d, target->GetEntry(), types, auras))
            return;
        ++c.purges;
        if (GetConfig().bots.debug)
            LOG_INFO(kLog, "Bot {}: purge {} on {}", caster->GetName(), spellId, target->GetName());
    }

    // Claims the aura and casts the plan's spell; the pet's casts are counted here, the bot's own by the cast hook.
    bool CastPlan(PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Plan const& plan, bool interrupt)
    {
        Registry* reg = Registry::Get(bot->GetMap());
        Desc::CastKey const key = ClaimKey(plan.target, plan.auraId);
        if (reg && !reg->ClaimCast(key, bot->GetGUID(), GameTime::GetGameTimeMS() + kClaimMs))
            return false;
        bool ok = false;
        if (plan.pet)
        {
            ok = CastPetSpell(bot, plan.spellId, plan.target);
            if (ok)
                CountCast(bot, st, plan.spellId, plan.target);
        }
        else
        {
            if (interrupt && bot->IsNonMeleeSpellCast(false, false, true))
                bot->InterruptNonMeleeSpells(false);
            ok = botAI->CastSpell(plan.spellId, plan.target);
        }
        if (!ok && reg)
            reg->ReleaseCast(key, bot->GetGUID());
        return ok;
    }

    // The bot's own casts of a table spell (whichever action cast it: ours, a stock cure row, a YAML row).
    class BossMechanicsDispelPlayerScript : public PlayerScript
    {
    public:
        BossMechanicsDispelPlayerScript() : PlayerScript("BossMechanicsDispelPlayerScript",
            { PLAYERHOOK_ON_SPELL_CAST }) { }

        void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
        {
            if (!player || !spell || !spell->GetSpellInfo() || !InTables(spell->GetSpellInfo()->Id))
                return;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
            MechanicBotState* st = botAI ? BotBrain::StateOf(botAI) : nullptr;
            if (st)
                CountCast(player, *st, spell->GetSpellInfo()->Id, spell->m_targets.GetUnitTarget());
        }
    };

    // Deadline misses: a deadline entry's aura that ran out on a bot, counted on that bot.
    class BossMechanicsDispelUnitScript : public UnitScript
    {
    public:
        BossMechanicsDispelUnitScript() : UnitScript("BossMechanicsDispelUnitScript", true,
            { UNITHOOK_ON_AURA_REMOVE }) { }

        void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
        {
            if (mode != AURA_REMOVE_BY_EXPIRE || !unit || !aurApp || !unit->IsPlayer())
                return;
            Player* player = unit->ToPlayer();
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
            MechanicBotState* st = botAI ? BotBrain::StateOf(botAI) : nullptr;
            if (!st || !player->IsInWorld())
                return;
            Descriptor const* d = ActiveDescriptor(player, *st);
            if (!d)
                return;
            uint32 const auraId = aurApp->GetBase()->GetId();
            std::optional<std::size_t> const k = Desc::FindDispel(*d, auraId);
            if (!k || d->dispels[*k].deadline <= Ms(0))
                return;
            ++st->counters.deadlineMisses;
            if (GetConfig().bots.debug)
                LOG_INFO(kLog, "Bot {}: deadline debuff {} expired on it", player->GetName(), auraId);
        }
    };
}

namespace BossMechanics::BotDispel
{
    uint64 WakeSignature(Player* bot, MechanicBotState const& st)
    {
        DescriptorView const& v = st.descriptor;
        if (!bot || !v.Active())
            return 0;
        Descriptor const& d = v.descriptor;
        bool urgentListed = false;
        for (DispelEntry const& e : d.dispels)
            urgentListed = urgentListed || e.urgent || e.deadline > Ms(0);
        if (!urgentListed)
            return 0;
        uint32 mask = 0;
        for (DispelRules::Owned const& o : OwnedSpells(bot, DispelRules::Use::Friendly))
            mask |= o.types;
        if (!mask)
            return 0;
        uint64 sig = 0;
        auto look = [&](Unit* u)
        {
            if (!u || !u->IsAlive() || !u->IsInWorld() || !u->IsInMap(bot))
                return;
            for (DispelEntry const& e : d.dispels)
            {
                if (!e.urgent && e.deadline <= Ms(0))
                    continue;
                Aura const* aura = u->GetAura(e.auraId);
                if (!aura || aura->IsRemoved())
                    continue;
                SpellInfo const* info = aura->GetSpellInfo();
                if (!info || info->IsPositive() || !(mask & DispelRules::Bit(info->Dispel))
                    || !Desc::IsUrgent(e, Remaining(aura)))
                    continue;
                sig = LockRules::MixWakeSignature(sig, u->GetGUID().GetRawValue(), e.auraId,
                    reinterpret_cast<std::uintptr_t>(aura));
            }
        };
        auto withPet = [&](Player* p)
        {
            look(p);
            if (Pet* pet = p->GetPet())
                look(pet);
        };
        Group* group = bot->GetGroup();
        if (!group)
        {
            withPet(bot);
            return sig;
        }
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* p = ref->GetSource())
                withPet(p);
        return sig;
    }

    std::string ReportLine(PlayerbotAI* botAI, MechanicBotState const& st)
    {
        MechanicBotCounters const& c = st.counters;
        // Which value the bot holds (the module's override, or a later context shadowing it).
        bool module = false;
        if (AiObjectContext* context = botAI->GetAiObjectContext())
            module = dynamic_cast<MechanicBotDispelTargetValue*>(context->GetUntypedValue(
                "party member to dispel::" + std::to_string(DISPEL_MAGIC))) != nullptr;
        return Acore::StringFormat("  m5 dispels: urgent {} (latency max {} ms), deadline {} (expired on this bot {}), "
            "listed {}, purges {}; party member to dispel: {}", c.urgentDispels, c.urgentLatencyMsMax,
            c.deadlineDispels, c.deadlineMisses, c.listedDispels, c.purges, module ? "module" : "stock");
    }

    void AddScripts()
    {
        new BossMechanicsDispelPlayerScript();
        new BossMechanicsDispelUnitScript();
    }
}

MechanicBotDispelTargetValue::MechanicBotDispelTargetValue(PlayerbotAI* botAI) : PartyMemberToDispel(botAI)
{
    if (GetConfig().bots.debug && bot)
        LOG_INFO(kLog, "Bot {}: party member to dispel is the boss-mechanics override", bot->GetName());
}

Unit* MechanicBotDispelTargetValue::Calculate()
{
    MechanicBotState* st = BotBrain::StateOf(botAI);
    Descriptor const* d = st && bot ? ActiveDescriptor(bot, *st) : nullptr;
    if (!d || d->dispels.empty())
        return PartyMemberToDispel::Calculate();

    uint32 const type = static_cast<uint32>(std::atoi(qualifier.c_str()));
    Descriptor const view = DispelRules::UrgentView(*d, StockSkip());
    std::vector<Desc::Affliction> afflictions;
    std::vector<Listed> where;
    ForEachMember(botAI, bot, [&](Unit* member, DispelRules::MemberRank rank)
    {
        if (AliveHere(member, bot) && Check(member))
            Gather(view, member, rank, DispelRules::Bit(type), true, afflictions, where);
    });
    std::vector<Desc::DispelPick> const picks = Desc::OrderDispels(view, afflictions);
    if (!picks.empty())
        return where[picks.front().affliction].unit;
    return PartyMemberToDispel::Calculate();
}

bool MechanicBotDispelUrgentTrigger::IsActive()
{
    MechanicBotState* st = _state.Get(botAI);
    return st && bot->IsAlive() && PlanUrgent(botAI, bot, *st).has_value();
}

bool MechanicBotDispelUrgentAction::isUseful()
{
    return _state.Get(botAI) && bot->IsAlive();
}

bool MechanicBotDispelUrgentAction::Execute(Event /*event*/)
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return false;
    std::optional<Plan> const plan = PlanUrgent(botAI, bot, *st);
    if (!plan)
        return false;
    bool const ok = CastPlan(botAI, bot, *st, *plan, true);
    if (GetConfig().bots.debug)
        LOG_INFO(kLog, "Bot {}: boss dispel urgent {} on {} for aura {} ({}) - {}", bot->GetName(), plan->spellId,
            plan->target->GetName(), plan->auraId, plan->urgentEntry ? "loss of control" : "deadline",
            ok ? "OK" : "failed");
    return ok;
}

bool MechanicBotPurgeTrigger::IsActive()
{
    MechanicBotState* st = _state.Get(botAI);
    return st && bot->IsAlive() && PlanPurge(botAI, bot, *st).has_value();
}

bool MechanicBotPurgeAction::isUseful()
{
    return _state.Get(botAI) && bot->IsAlive();
}

bool MechanicBotPurgeAction::Execute(Event /*event*/)
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return false;
    std::optional<Plan> const plan = PlanPurge(botAI, bot, *st);
    if (!plan)
        return false;
    bool const ok = CastPlan(botAI, bot, *st, *plan, false);
    if (GetConfig().bots.debug)
        LOG_INFO(kLog, "Bot {}: boss purge {} on {} for buff {} - {}", bot->GetName(), plan->spellId,
            plan->target->GetName(), plan->auraId, ok ? "OK" : "failed");
    return ok;
}

#endif
