/*
 * mod-boss-mechanics: the lock multiplier of the "boss mechanics" strategy (STAGE-M2-HANDOFF §3.7, WP-C).
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotLock.h"
#include "AiObjectContext.h"
#include "AttackAction.h"
#include "BossMechanicsConfig.h"
#include "CastFacingUnitAction.h"
#include "ChooseTargetActions.h"
#include "GameTime.h"
#include "GenericSpellActions.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "MechanicBotBrain.h"
#include "MechanicBotFacing.h"
#include "MechanicBotState.h"
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
#include <unordered_set>
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

    // Movers that are not MovementActions, found by the WP-C lock audit (STAGE-M2-C-REPORT, "Lock audit"), by the
    // name the action reports (getName(), not always its creator key). Gap-closers, leaps and blinks in every class
    // rotation; a stock CastSpellAction is also caught by its spell effect below, these are named because a rework
    // spell may use another effect or a plain Action casts it:
    // - "heroic leap", "heroic leap loose": WarriorHeroicLeapAction (Action; ground-targeted CastSpell 204400);
    // - "cmbt grappling hook": RogueGrapplingHookAction (Action; CastSpell 203963 pulls the rogue to the target);
    // - "cmbt blade rush", "cmbt pack blade rush": RogueCastAction, spell 203980 (a charge);
    // - "shadowstep" (creator "subt shadowstep"): CastSpellAction, spell 36554 (teleport behind the target);
    // - "blink back": CastBlinkBackAction (turns away, then Blink);
    // - "disengage" (also created as "hunter disengage"): CastDisengageAction (leap back).
    // Charge, Intercept and Feral Charge are CastReachTargetSpellAction and are matched by type.
    bool IsNamedMover(Action* action)
    {
        static std::unordered_set<std::string> const names = { "heroic leap", "heroic leap loose",
            "cmbt grappling hook", "cmbt blade rush", "cmbt pack blade rush", "shadowstep", "subt shadowstep",
            "blink back", "disengage", "hunter disengage" };
        return dynamic_cast<CastReachTargetSpellAction*>(action) || names.count(action->getName());
    }

    // Spells that move the caster: Charge, Intercept, Feral Charge (charge); Blink (leap); Disengage (leap back);
    // Heroic Leap and other jumps; teleports. A custom mover with another effect slips through (M4 class audit).
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

    float Block(MechanicBotState& st, bool move)
    {
        ++st.counters.lockBlocks;
        if (move)
            ++st.counters.lockMoves;
        else
            ++st.counters.lockCasts;
        return 0.f;
    }

    // Look-away (decision §7.6): an action that would turn the bot off its chosen facing.
    float FacingBlock(MechanicBotState& st)
    {
        ++st.counters.lockBlocks;
        ++st.counters.facingBlocks;
        return 0.f;
    }
}

float MechanicBotLockMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.f;
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return 1.f;
    Milliseconds const now = GameTime::GetGameTimeMS();

    // A released bot (stuck, rooted, past the escape ceiling) is the stock AI's.
    if (BotBrain::Released(botAI, *st, now))
        return 1.f;

    // Ours. The move action is dropped while frozen (it is not useful then anyway).
    if (dynamic_cast<MechanicBotMoveAction*>(action))
        return st->mode == MechanicBotMode::Freeze ? Block(*st, true) : 1.f;
    if (dynamic_cast<MechanicBotHoldAction*>(action) || dynamic_cast<MechanicBotFaceAction*>(action))
        return 1.f;

    // M3, in every mode: tank facing owns the tank (one owner; decision §7.5), and the look-away facing (decision
    // §7.6) is not undone by a facing action, a facing cast or a swing at a target outside the arc.
    if (BotBrain::TankOwned(*st) && (typeid(*action) == typeid(TankFaceAction) || action->getName() == "tank face"))
        return Block(*st, true);
    bool const lookAway = BotFacing::LookAwayActive(*st, now);
    if (lookAway)
    {
        if (dynamic_cast<SetFacingTargetAction*>(action) || dynamic_cast<CastFacingUnitAction*>(action))
            return FacingBlock(*st);
        if (IsAttackOnly(action) && BotFacing::WouldTurnAway(bot, *st, now, action->GetTarget()))
            return FacingBlock(*st);
    }

    // Watch with an unsafe spot whose solve was deferred (rate limit, per-map budget) locks as Escape
    // (STAGE-M2-REVIEW m5). Just after a cast break, whatever the mode, no cast-time spell and no item use for
    // kNoCastAfterBreak (MechanicBotWaker.cpp), so a cast the lock does not otherwise see (an item, a Watch bot) is
    // not restarted and broken again on every update.
    MechanicBotMode mode = st->mode;
    if (mode == MechanicBotMode::Watch && st->pendingEscape)
        mode = MechanicBotMode::Escape;
    bool const afterBreak = now < st->noCastUntil;
    bool const frozen = mode == MechanicBotMode::Freeze;
    if ((mode == MechanicBotMode::Idle || mode == MechanicBotMode::Watch) && !afterBreak && !lookAway)
        return 1.f;
    if (afterBreak && dynamic_cast<UseItemAction*>(action))
        return Block(*st, false);
    if (frozen && dynamic_cast<UseItemAction*>(action) && ItemMoves(botAI, action))
        return Block(*st, true);

    // Idle or Watch after a break or under look-away: only casts are judged (cast-time after a break, facing casts
    // under look-away).
    bool const castsOnly = mode == MechanicBotMode::Idle || mode == MechanicBotMode::Watch;

    // Foreign movers (Freeze: every one, decision §7.4).
    if (IsAttackOnly(action))
        return 1.f;
    if (!castsOnly && (dynamic_cast<MovementAction*>(action) || IsNamedMover(action)))
        return Block(*st, true);

    // Spells.
    CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action);
    if (!cast)
        return 1.f;     // facing ("set facing" is a plain Action; blocked above under look-away), items, chat, ...
    uint32 const spellId = AI_VALUE2(uint32, "spell id", cast->getSpell());
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!info)
        return 1.f;     // unknown spell: allowed
    if (!castsOnly && IsMovementSpell(info))
        return Block(*st, true);
    // Look-away: a cast that turns its caster to a target outside the arc (PlayerbotAI::CastSpell, and the mid-cast
    // re-facing in UpdateAI, for SPELL_FACING_FLAG_INFRONT) would sweep the carried cone over the group. Offensive
    // casts on a target outside the arc are blocked whatever their flags: rework actions turn the bot to their target
    // themselves (DkFacingCastAction, MageFrostFrozenOrbAction; WP-C facing audit). Heals on an ally behind the bot
    // need no facing and stay allowed.
    if (lookAway)
    {
        Unit* target = cast->GetTarget();
        bool const turns = BotFacing::TurnsCaster(info)
            || (target && target != bot && bot->IsValidAttackTarget(target));
        if (turns && BotFacing::WouldTurnAway(bot, *st, now, target))
            return FacingBlock(*st);
    }
    Milliseconds const busy = BusyTime(info, bot);
    if (busy <= Milliseconds(0))
        return 1.f;     // instant
    // Frozen: the bot cannot leave anyway; casting is what it may do (decision §7.4).
    if (frozen)
        return 1.f;
    if (castsOnly)
        return afterBreak ? Block(*st, false) : 1.f;
    if (mode == MechanicBotMode::Escape)
        return Block(*st, false);   // it would fail while moving (PlayerbotAI::CastSpell) and waste the tick
    // Hold: a cast that ends before the bot must leave. leaveBy already holds SafetyMarginMs once (Solver::LeaveBy);
    // the second margin here is deliberate (STAGE-M2-REVIEW n3): it covers the AI tick between the cast's end and the
    // escape's first step, which the solver's StartDelayMs does not see for a bot that was casting.
    Milliseconds const margin(GetConfig().bots.safetyMarginMs);
    if (!Solver::CastAllowed(now, busy, st->leaveBy, margin))
        return Block(*st, false);
    return 1.f;
}

#endif
