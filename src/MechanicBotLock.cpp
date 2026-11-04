/*
 * mod-boss-mechanics: the lock multiplier of the "boss mechanics" strategy (STAGE-M2-HANDOFF §3.7, WP-C) and the
 * by-name gate that shares its verdict (STAGE-M4-HANDOFF §3.5, WP-X).
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotLock.h"
#include "AiObjectContext.h"
#include "AttackAction.h"
#include "BossMechanicsConfig.h"
#include "CastAtPositionAction.h"
#include "CastFacingUnitAction.h"
#include "ChooseTargetActions.h"
#include "Engine.h"
#include "GameTime.h"
#include "GenericSpellActions.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "MechanicBotBrain.h"
#include "MechanicBotFacing.h"
#include "MechanicBotState.h"
#include "MechanicLockRules.h"
#include "MovementActions.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ReachTargetActions.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "UseItemAction.h"
#include <string>
#include <typeinfo>
#include <vector>

using namespace BossMechanics;

namespace
{
    // Stock target picks and auto-attack. They derive from MovementAction (through AttackAction) but never move the
    // bot: they pick a target, start the swing and turn to it. Exact types only: the encounter actions that derive
    // from AttackAction (Karazhan, Zul'Aman, Black Temple, ... "position boss") do move, and stay blocked.
    template <class... Types>
    bool IsExactly(Action const* action)
    {
        std::type_info const& type = typeid(*action);
        return ((type == typeid(Types)) || ...);
    }

    bool IsAttackOnly(Action const* action)
    {
        return IsExactly<AttackAction, MeleeAction, AttackMyTargetAction, AttackDuelOpponentAction, DpsAoeAction,
            DpsAssistAction, TankAssistAction, AggressiveTargetAction, AttackAnythingAction, AttackLeastHpTargetAction,
            AttackEnemyPlayerAction, AttackRtiTargetAction, AttackEnemyFlagCarrierAction>(action);
    }

    // Movers that are not MovementActions, by name (the list and its sources: MechanicLockRules.h kNamedMovers).
    bool IsNamedMover(Action* action)
    {
        return dynamic_cast<CastReachTargetSpellAction*>(action) || LockRules::IsNamedMoverName(action->getName());
    }

    // Spells that move the caster: Charge, Intercept, Feral Charge (charge); Blink (leap); Disengage (leap back);
    // Heroic Leap and other jumps; teleports. A mover whose script moves it is in the scripted-mover table instead.
    bool IsMovementSpell(SpellInfo const* info)
    {
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            switch (effect.Effect)
            {
                case SPELL_EFFECT_CHARGE:
                case SPELL_EFFECT_CHARGE_DEST:
                case SPELL_EFFECT_LEAP:
                case SPELL_EFFECT_LEAP_BACK:
                case SPELL_EFFECT_JUMP:
                case SPELL_EFFECT_JUMP_DEST:
                case SPELL_EFFECT_TELEPORT_UNITS:
                    return true;
                default:
                    break;
            }
        }
        return false;
    }

    // How long the bot is busy casting it: cast time, plus the channel (an endless channel counts as forever).
    Milliseconds BusyTime(SpellInfo const* info, Player* bot)
    {
        Milliseconds busy(info->CalcCastTime(bot));
        if (info->IsChanneled())
        {
            int32 const duration = info->GetDuration();
            if (duration < 0)
                return Solver::kForever;
            busy += Milliseconds(duration);
        }
        return busy;
    }

    // Freeze: an item whose spell moves the caster (a rocket, a jump): the item the action would use, by its name.
    bool ItemMoves(PlayerbotAI* botAI, Action* action)
    {
        std::vector<Item*> items = botAI->GetAiObjectContext()->GetValue<std::vector<Item*>>("inventory items",
            action->getName())->Get();
        if (items.empty() || !items.front())
            return false;
        ItemTemplate const* proto = items.front()->GetTemplate();
        if (!proto)
            return false;
        for (auto const& spell : proto->Spells)
            if (spell.SpellId > 0)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId))
                    if (IsMovementSpell(info))
                        return true;
        return false;
    }

    // A spell that moves its caster from a script (MechanicLockRules.h kScriptedMovers); Ambush only from stealth on a
    // target out of melee range, the script's own test. `castTarget` reads the cast's target, only for such a rule.
    template <class TargetFn>
    bool IsScriptedMover(SpellInfo const* info, Player* bot, TargetFn&& castTarget)
    {
        LockRules::ScriptedMover const* mover = LockRules::FindScriptedMover(info->Id);
        if (!mover)
            return false;
        if (mover->rule == LockRules::MoverRule::Always)
            return true;
        Unit* const target = castTarget();
        return LockRules::ScriptedMoverMoves(mover->rule, bot->HasStealthAura(), target != nullptr,
            target && bot->IsWithinMeleeRange(target));
    }

    // The spell the cast action casts. With the fork's CastSpellAction::GetCastSpellId() the rework actions that cast
    // a stored id under a row name report it; without it the name lookup gives 0 for them (STAGE-M4-L-REPORT H1).
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

    enum class LockVerdict : uint8
    {
        Allow,
        BlockMove,      // a movement action or movement spell
        BlockCast,      // a cast-time or channelled spell, an item use
        BlockFacing,    // look-away: the action would turn the bot
    };

    struct Judgement
    {
        LockVerdict verdict = LockVerdict::Allow;
        bool unknownCast = false;   // a cast judged with no spell id
    };

    Judgement Allow(bool unknownCast = false)
    {
        return { LockVerdict::Allow, unknownCast };
    }

    Judgement Blocked(LockVerdict verdict)
    {
        return { verdict, false };
    }

    // The verdict, with no counter touched: the multiplier and the gate count it (CountJudgement), a chat `do` only
    // notes it.
    Judgement JudgeAction(PlayerbotAI* botAI, MechanicBotState& st, Action* action)
    {
        Player* bot = botAI->GetBot();
        if (!bot)
            return Allow();
        AiObjectContext* context = botAI->GetAiObjectContext();
        Milliseconds const now = GameTime::GetGameTimeMS();

        // A released bot (stuck, rooted, past the escape ceiling) is the stock AI's.
        if (BotBrain::Released(botAI, st, now))
            return Allow();

        // Ours. The move action is dropped while frozen (it is not useful then anyway).
        if (dynamic_cast<MechanicBotMoveAction*>(action))
            return st.mode == MechanicBotMode::Freeze ? Blocked(LockVerdict::BlockMove) : Allow();
        if (dynamic_cast<MechanicBotHoldAction*>(action) || dynamic_cast<MechanicBotFaceAction*>(action))
            return Allow();

        // M3, in every mode: tank facing owns the tank (one owner; decision §7.5), and the look-away facing (decision
        // §7.6) is not undone by a facing action, a facing cast or a swing at a target outside the arc.
        if (BotBrain::TankOwned(st) && (typeid(*action) == typeid(TankFaceAction) || action->getName() == "tank face"))
            return Blocked(LockVerdict::BlockMove);
        bool const lookAway = BotFacing::LookAwayActive(st, now);
        if (lookAway)
        {
            if (dynamic_cast<SetFacingTargetAction*>(action) || dynamic_cast<CastFacingUnitAction*>(action))
                return Blocked(LockVerdict::BlockFacing);
            if (IsAttackOnly(action) && BotFacing::WouldTurnAway(bot, st, now, action->GetTarget()))
                return Blocked(LockVerdict::BlockFacing);
        }

        // Watch with an unsafe spot whose solve was deferred (rate limit, per-map budget) locks as Escape
        // (STAGE-M2-REVIEW m5). Just after a cast break, whatever the mode, no cast-time spell and no item use for
        // kNoCastAfterBreak (MechanicBotWaker.cpp), so a cast the lock does not otherwise see (an item, a Watch bot) is
        // not restarted and broken again on every update.
        MechanicBotMode mode = st.mode;
        if (mode == MechanicBotMode::Watch && st.pendingEscape)
            mode = MechanicBotMode::Escape;
        bool const afterBreak = now < st.noCastUntil;
        bool const frozen = mode == MechanicBotMode::Freeze;
        if ((mode == MechanicBotMode::Idle || mode == MechanicBotMode::Watch) && !afterBreak && !lookAway)
            return Allow();
        if (afterBreak && dynamic_cast<UseItemAction*>(action))
            return Blocked(LockVerdict::BlockCast);
        if (frozen && dynamic_cast<UseItemAction*>(action) && ItemMoves(botAI, action))
            return Blocked(LockVerdict::BlockMove);

        // Idle or Watch after a break or under look-away: only casts are judged (cast-time after a break, facing casts
        // under look-away).
        bool const castsOnly = mode == MechanicBotMode::Idle || mode == MechanicBotMode::Watch;

        // Foreign movers (Freeze: every one, decision §7.4).
        if (IsAttackOnly(action))
            return Allow();
        if (!castsOnly && (dynamic_cast<MovementAction*>(action) || IsNamedMover(action)))
            return Blocked(LockVerdict::BlockMove);

        // Spells.
        CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action);
        if (!cast)
            return Allow();     // facing ("set facing" is a plain Action; blocked above under look-away), items, chat
        uint32 const spellId = CastSpellIdOf(botAI, cast);
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        // The cast target, read once: the look-away test and Ambush's stealth rule need it.
        Unit* target = nullptr;
        bool targetRead = false;
        auto castTarget = [&]() -> Unit*
        {
            if (!targetRead)
            {
                target = cast->GetTarget();
                targetRead = true;
            }
            return target;
        };
        if (!castsOnly && info && (IsMovementSpell(info) || IsScriptedMover(info, bot, castTarget)))
            return Blocked(LockVerdict::BlockMove);
        // Look-away: a cast that turns its caster to a target outside the arc (PlayerbotAI::CastSpell, and the mid-cast
        // re-facing in UpdateAI, for SPELL_FACING_FLAG_INFRONT) would sweep the carried cone over the group. Offensive
        // casts on a target outside the arc are blocked whatever their flags and with no spell id (rework actions turn
        // the bot to their target themselves); a ground cast turns the bot to its point, which can be its own feet
        // (world orientation 0); a turning self cast turns it to the current target. Heals on an ally behind the bot
        // need no facing and stay allowed. The rules: MechanicLockRules.h LookAwayBlocksCast.
        if (lookAway)
        {
            Unit* const victim = castTarget();
            LockRules::LookAwayCast c;
            c.groundCast = dynamic_cast<CastAtPositionAction*>(cast) != nullptr;
            c.turnsToCurrentTarget = LockRules::IsTurningSelfCastName(cast->getName());
            if (c.turnsToCurrentTarget)
                c.currentTargetOutside = BotFacing::WouldTurnAway(bot, st, now, AI_VALUE(Unit*, "current target"));
            c.facingInfront = BotFacing::TurnsCaster(info);
            c.offensiveTarget = victim && victim != bot && bot->IsValidAttackTarget(victim);
            c.targetOutside = BotFacing::WouldTurnAway(bot, st, now, victim);
            if (LockRules::LookAwayBlocksCast(c))
                return Blocked(LockVerdict::BlockFacing);
        }
        if (!info)
            return Allow(true);     // unknown spell: allowed (no cast-time or movement test can run)
        Milliseconds const busy = BusyTime(info, bot);
        if (busy <= Milliseconds(0))
            return Allow();     // instant
        // Frozen: the bot cannot leave anyway; casting is what it may do (decision §7.4).
        if (frozen)
            return Allow();
        if (castsOnly)
            return afterBreak ? Blocked(LockVerdict::BlockCast) : Allow();
        if (mode == MechanicBotMode::Escape)
            return Blocked(LockVerdict::BlockCast);     // it would fail while moving (PlayerbotAI::CastSpell)
        // Hold: a cast that ends before the bot must leave. leaveBy already holds SafetyMarginMs once
        // (Solver::LeaveBy); the second margin here is deliberate (STAGE-M2-REVIEW n3): it covers the AI tick between
        // the cast's end and the escape's first step, which the solver's StartDelayMs does not see for a bot that was
        // casting.
        Milliseconds const margin(GetConfig().bots.safetyMarginMs);
        if (!Solver::CastAllowed(now, busy, st.leaveBy, margin))
            return Blocked(LockVerdict::BlockCast);
        return Allow();
    }

    // Counts a judgement: 1 to allow, 0 to drop.
    float CountJudgement(MechanicBotState& st, Judgement const& j)
    {
        if (j.unknownCast)
            ++st.counters.unknownCasts;
        switch (j.verdict)
        {
            case LockVerdict::Allow:
                return 1.f;
            case LockVerdict::BlockMove:
                ++st.counters.lockBlocks;
                ++st.counters.lockMoves;
                return 0.f;
            case LockVerdict::BlockCast:
                ++st.counters.lockBlocks;
                ++st.counters.lockCasts;
                return 0.f;
            case LockVerdict::BlockFacing:
                // Look-away (decision §7.6): an action that would turn the bot off its chosen facing.
                ++st.counters.lockBlocks;
                ++st.counters.facingBlocks;
                return 0.f;
        }
        return 1.f;
    }

#if defined(PLAYERBOTS_HAS_EXECUTE_ACTION_GATE)
    // Engine::SetExecuteActionGate: an action run by name (PlayerbotAI::DoSpecificAction: move to loot from an item
    // use, the pull pre-action and pull spell, the bridge, mod-dungeon-clear dispatches, Lua) gets the lock's verdict,
    // as an engine-queued one does through the multiplier. A bot without the strategy is not ours. Chat `do` is a
    // human order (and the GM test flow): let through, counted when the lock would have refused it (decision §7.4).
    bool SpecificActionGate(PlayerbotAI* botAI, Action* action, Event& event)
    {
        if (!botAI || !action)
            return true;
        if (!botAI->HasStrategy("boss mechanics", BOT_STATE_COMBAT)
            && !botAI->HasStrategy("boss mechanics", BOT_STATE_NON_COMBAT))
            return true;
        MechanicBotState* st = BotBrain::StateOf(botAI);
        if (!st)
            return true;
        Judgement const j = JudgeAction(botAI, *st, action);
        if (event.GetSource() == "do")
        {
            if (j.verdict != LockVerdict::Allow)
                ++st->counters.doBypasses;
            return true;
        }
        if (CountJudgement(*st, j) > 0.f)
            return true;
        ++st->counters.specificBlocks;
        return false;
    }
#endif
}

float BotLock::Verdict(PlayerbotAI* botAI, MechanicBotState& st, Action* action)
{
    if (!botAI || !action)
        return 1.f;
    return CountJudgement(st, JudgeAction(botAI, st, action));
}

void BotLock::InstallSpecificActionGate()
{
#if defined(PLAYERBOTS_HAS_EXECUTE_ACTION_GATE)
    Engine::SetExecuteActionGate(&SpecificActionGate);
#endif
}

float MechanicBotLockMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.f;
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return 1.f;
    return BotLock::Verdict(botAI, *st, action);
}

#endif
