/*
 * mod-boss-mechanics: the lock multiplier of the "boss mechanics" strategy (STAGE-M2-HANDOFF §3.7, WP-C).
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotLock.h"
#include "AttackAction.h"
#include "BossMechanicsConfig.h"
#include "ChooseTargetActions.h"
#include "GameTime.h"
#include "GenericSpellActions.h"
#include "MechanicBotBrain.h"
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

    float Block(MechanicBotState& st, bool move)
    {
        ++st.counters.lockBlocks;
        if (move)
            ++st.counters.lockMoves;
        else
            ++st.counters.lockCasts;
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

    // Watch with an unsafe spot whose solve was deferred (rate limit, per-map budget) locks as Escape
    // (STAGE-M2-REVIEW m5). Just after a cast break, whatever the mode, no cast-time spell and no item use for
    // kNoCastAfterBreak (MechanicBotWaker.cpp), so a cast the lock does not otherwise see (an item, a Watch bot) is
    // not restarted and broken again on every update.
    MechanicBotMode mode = st->mode;
    if (mode == MechanicBotMode::Watch && st->pendingEscape)
        mode = MechanicBotMode::Escape;
    bool const afterBreak = now < st->noCastUntil;
    if ((mode == MechanicBotMode::Idle || mode == MechanicBotMode::Watch) && !afterBreak)
        return 1.f;
    // A released bot (stuck, rooted, past the escape ceiling) is the stock AI's.
    if (BotBrain::Released(botAI, *st, now))
        return 1.f;
    if (afterBreak && dynamic_cast<UseItemAction*>(action))
        return Block(*st, false);

    // Ours.
    if (dynamic_cast<MechanicBotMoveAction*>(action) || dynamic_cast<MechanicBotHoldAction*>(action))
        return 1.f;

    // Idle or Watch after a break: only cast-time spells are held back.
    bool const castsOnly = mode == MechanicBotMode::Idle || mode == MechanicBotMode::Watch;

    // Foreign movers.
    if (IsAttackOnly(action))
        return 1.f;
    if (!castsOnly && (dynamic_cast<MovementAction*>(action) || IsNamedMover(action)))
        return Block(*st, true);

    // Spells.
    CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action);
    if (!cast)
        return 1.f;     // facing ("set facing" is a plain Action), items, chat, ...
    uint32 const spellId = AI_VALUE2(uint32, "spell id", cast->getSpell());
    SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!info)
        return 1.f;     // unknown spell: allowed
    if (!castsOnly && IsMovementSpell(info))
        return Block(*st, true);
    Milliseconds const busy = BusyTime(info, bot);
    if (busy <= Milliseconds(0))
        return 1.f;     // instant
    if (castsOnly || mode == MechanicBotMode::Escape)
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
