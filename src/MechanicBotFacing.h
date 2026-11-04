/*
 * mod-boss-mechanics: look-away facing (STAGE-M3-HANDOFF §3.7, decision §7.6, WP-C).
 *
 * A bot that carries a cone or line with followFacing (Cookie's chowder) keeps it off every ally: the solver picks the
 * free facing nearest its victim (Solver::CarriedOk / Plan::facing). Over each window of that shape (+- SafetyMarginMs)
 * the bot stands still (a move turns it) and holds that facing:
 * - the face action applies it (SetOrientation + SendMovementFlagUpdate, as playerbots' ServerFacade::SetFacingTo);
 * - the lock drops `set facing`, CastFacingUnitAction, any cast with SPELL_FACING_FLAG_INFRONT whose target lies
 *   outside CAST_ANGLE_IN_FRONT of the chosen facing, and attack-only actions on a victim outside it;
 * - the waker breaks a cast in progress on a target outside the arc when a window starts before the cast ends (the
 *   mid-cast re-facing in PlayerbotAI::UpdateAI cannot be stopped otherwise).
 *
 * Pure functions of the bot's state and live units; nothing is kept.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_FACING_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_FACING_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "Duration.h"
#include "MechanicBotState.h"

class Player;
class Spell;
class SpellInfo;
class Unit;
class WorldObject;

namespace BossMechanics
{
    namespace BotFacing
    {
        // The face trigger turns the bot when it is off the owed facing by more than this.
        constexpr float kFaceToleranceDeg = 5.f;

        // Recomputes lookOwed, lookWindow, lookFacing, lookFrom/lookTo and faceOwed/faceAngle from st.input (the
        // chooseFacing carried shapes) and the plan. Called on every brain step and by the waker for casting bots.
        void UpdateLookAway(Player const* bot, MechanicBotState& st, Milliseconds now);

        // A look-away window (widened by SafetyMarginMs) is open at `now`.
        bool LookAwayActive(MechanicBotState const& st, Milliseconds now);

        // `target` lies within CAST_ANGLE_IN_FRONT of `facing` seen from the bot.
        bool InArc(Player const* bot, float facing, WorldObject const* target);

        // Under look-away: would turning to `target` (a cast, a swing) break the chosen facing?
        bool WouldTurnAway(Player const* bot, MechanicBotState const& st, Milliseconds now, Unit const* target);

        // The spell turns its caster to its target (PlayerbotAI::CastSpell and UpdateAI re-face these).
        bool TurnsCaster(SpellInfo const* info);

        // A cast in progress that the look-away forbids: the window opens before the cast ends and the cast's target
        // lies outside the arc (the bot would be turned to it every update).
        bool CastBreaksLookAway(Player const* bot, MechanicBotState const& st, Spell const* spell, Milliseconds now,
            Milliseconds remaining);

        // Off the angle by more than kFaceToleranceDeg.
        bool FacingOff(Player const* bot, float angle);

        // Turns the bot (ServerFacade::SetFacingTo without a target).
        void Apply(Player* bot, float angle);
    }
}

#endif

#endif
