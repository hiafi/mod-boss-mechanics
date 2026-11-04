/*
 * mod-boss-mechanics: the lock multiplier of the "boss mechanics" strategy (STAGE-M2-HANDOFF §3.7, WP-C;
 * STAGE-M3-HANDOFF §3.8, WP-C). The one block-list file: every rule about which foreign action may run while a bot
 * escapes, holds or freezes lives here.
 *
 * | Mode         | Blocked                                                       | Allowed                           |
 * | Idle, Watch  | nothing (the brain polices foreign motion itself)             | everything                        |
 * | Escape       | every movement action but ours; movement spells; every spell  | instants (heals too), auto-attack |
 * |              | with a cast time or a channel                                 | and target picks, facing, ours    |
 * | Hold         | every movement action but ours; movement spells; a cast-time  | casts that end in time, instants, |
 * |              | or channelled spell that would end after leaveBy - margin     | auto-attack, facing, ours         |
 * | Freeze       | every movement action, ours too; movement spells; items      | every cast, auto-attack, facing   |
 * |              | whose spell moves the caster                                  |                                   |
 *
 * In every mode (M3):
 * - tank facing owns the tank (a remembered Frontal source): stock `tank face` (TankFaceAction) is blocked;
 * - look-away window open: `set facing` (SetFacingTargetAction), CastFacingUnitAction, any cast with
 *   SPELL_FACING_FLAG_INFRONT on a target outside CAST_ANGLE_IN_FRONT of the chosen facing, and attack-only actions
 *   (auto-attack, target picks) on a target outside it are blocked.
 *
 * M4 (STAGE-M4-HANDOFF §3.5, WP-X, from the STAGE-M4-L-REPORT audit):
 * - look-away also blocks an offensive cast on a target outside the arc when the spell id is unknown, every ground
 *   cast (CastAtPositionAction turns the bot to the point), and the self casts that turn the bot to its current target
 *   first (Frozen Orb, Breath of Sindragosa, Glacial Advance); the pure rules are MechanicLockRules.h;
 * - movers: spells that move the caster from a script (MechanicLockRules.h kScriptedMovers: Death from Above, Killing
 *   Spree, Ambush from stealth out of melee, ...), and the interim names `arms intervene`, `subt death from above`;
 * - the spell id: CastSpellAction::GetCastSpellId() when the fork has it (PLAYERBOTS_HAS_CAST_SPELL_ID), which
 *   knows the ids the rework actions cast by id; else the name lookup, 0 for them;
 * - actions run by name (PlayerbotAI::DoSpecificAction skips the multipliers): when the fork has
 *   Engine::SetExecuteActionGate (PLAYERBOTS_HAS_EXECUTE_ACTION_GATE), InstallSpecificActionGate puts the same verdict
 *   there. Chat `do` (event source "do") is a human order and is let through, counted when the lock would refuse it.
 *
 * M4, WP-X2 (STAGE-M4-L-REPORT §10 X7, X8; M4-SWEEP G8):
 * - an item use in Escape or Hold is judged by its on-use spell's cast and channel time, as a spell (the warlock
 *   stones are 3 s casts); Freeze still allows it unless the spell moves the caster;
 * - in every mode, `pull my target` and `pull rti target` (a human's `pull`) are refused while an encounter is live on
 *   the map: playerbots' PullMultiplier would zero every other action, ours included, for up to 15 s (pullBlocks);
 * - BossMechanics.Bots.ForceLeaks (debug, 0 = off): the blocks the leak detectors can see are let through until each
 *   detector has counted that many leaks since the counters' reset (forcedMoves, forcedCasts, forcedTurns), to
 *   prove the detectors fire.
 *
 * Multipliers are per engine, so the strategy registers this in the combat and the non-combat engine alike.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_LOCK_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_LOCK_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotStrategy.h"
#include "Multiplier.h"

class Action;
class PlayerbotAI;

namespace BossMechanics
{
    struct MechanicBotState;

    namespace BotLock
    {
        // The lock's verdict on one action for this bot: 1 to allow, 0 to drop it (and the block counted). The
        // multiplier's value, and the by-name gate's test.
        float Verdict(PlayerbotAI* botAI, MechanicBotState& st, Action* action);

        // Puts the verdict into playerbots' by-name gate (Engine::SetExecuteActionGate) when the fork has it; a no-op
        // otherwise. Idempotent; world thread, once the glue registers (MechanicBotGate.cpp, LatchBotGlue).
        void InstallSpecificActionGate();
    }
}

class MechanicBotLockMultiplier : public Multiplier
{
public:
    explicit MechanicBotLockMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "boss mechanic lock") { }

    float GetValue(Action* action) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

#endif

#endif
