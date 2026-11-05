/*
 * mod-boss-mechanics: `.mech desc` (STAGE-M5-HANDOFF §3.6, WP-K): prints the active boss descriptor of the selected
 * creature's encounter (or of every described encounter on the map), its tracked units, held players, cast notes and
 * kick claims.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DESC_COMMAND_H
#define MOD_BOSS_MECHANICS_MECHANIC_DESC_COMMAND_H

class ChatHandler;

namespace BossMechanics
{
    bool HandleMechDescCommand(ChatHandler* handler);
}

#endif
