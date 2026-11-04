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
 * Multipliers are per engine, so the strategy registers this in the combat and the non-combat engine alike.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_LOCK_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_LOCK_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotStrategy.h"
#include "Multiplier.h"

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
