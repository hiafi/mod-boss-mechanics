/*
 * mod-boss-mechanics: the Weakened debuff (205000) (STAGE-M1-HANDOFF §2.6).
 *
 * Always self-applied (caster = the player), so Aura::SetStackAmount always finds its caster and one aura collects
 * stacks from any boss or stalker. EFFECT_0 is -10% damage done per stack (the engine multiplies by the stack count).
 * EFFECT_1 is MOD_THREAT: the AuraScript pins the engine-calculated amount to 0, and every stack change here sets the
 * exact compensator WeakenedThreatPct(stacks) (11, 25, 43, 67, 100, 150, 233) afterwards.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_WEAKENED_H
#define MOD_BOSS_MECHANICS_MECHANIC_WEAKENED_H

#include "Define.h"

class Map;
class Player;

namespace BossMechanics::Weakened
{
    // One more stack (cap 7, duration refreshed), then the threat amount for the new count.
    void AddStack(Player* player);
    // Exactly `stacks` stacks (0 removes it); `.mech weakened`.
    void SetStacks(Player* player, uint8 stacks);
    uint8 GetStacks(Player const* player);
    // Remove from `only`, or from every player on the map when only is nullptr.
    void Clear(Map* map, Player* only = nullptr);
}

#endif
