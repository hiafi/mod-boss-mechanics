/*
 * mod-boss-mechanics: the per-bot brain of the boss-mechanics strategy (STAGE-M2-HANDOFF §3.4, §3.5, §3.8, WP-C;
 * STAGE-M3-HANDOFF §3.7, §3.8, WP-C).
 *
 * Update() runs from the strategy's triggers (the "cached value behind the trigger" of the playerbots cost rules), at
 * most once per game-time millisecond, on the bot's map thread. It keeps the bot's view of the registry current
 * (MechanicBotView), runs the mode machine Idle -> Watch -> Escape -> Hold (+ Freeze for HoldStill), and solves
 * (MechanicSolver) when the spot fails TargetOk (hazards and the player kinds) or the bot's current motion is unsafe.
 * Around every solve it refreshes the allies and publishes the plan on the registry's reservation board. It also runs
 * the tank tasks (facing, parking), the healer sight check, region tracking, the route search and the look-away
 * facing. It early-outs in O(1) while the registry did not change and nothing is relevant.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_BRAIN_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_BRAIN_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "Duration.h"
#include "MechanicBotState.h"

class Player;
class PlayerbotAI;
class Unit;

namespace BossMechanics
{
    class Registry;

    namespace BotBrain
    {
        // The bot's state value; nullptr when the bot's class context has no "boss mechanic state" (glue disabled).
        MechanicBotState* StateOf(PlayerbotAI* botAI);

        // One brain step at `now` (GameTime ms). Idempotent within one game-time millisecond.
        void Update(PlayerbotAI* botAI, MechanicBotState& state, Milliseconds now);

        // Waker, bot casting or channelling: refreshes the view when it is stale (no candidate search, no paths) and
        // returns leaveBy for the bot's current spot (Solver::kForever when nothing threatens it).
        Milliseconds CastLeaveBy(PlayerbotAI* botAI, MechanicBotState& state, Registry const& reg, Milliseconds now);

        // The bot gave up an escape (stuck on the same target, rooted, or past the hard ceiling) and the stock AI has
        // it back until state.releaseUntil: no solve for its unsafe spot, no cast break, nothing locked. A release
        // for a root ends as soon as the bot can move again (cleared here).
        bool Released(PlayerbotAI* botAI, MechanicBotState& state, Milliseconds now);

        // The bot moves by something that is not ours: a spline in flight, or a generator in the active slot.
        bool HasForeignMotion(Player const* bot);

        // Hold action: the unit to face (the victim in combat), or nullptr.
        Unit* FacingTarget(Player* bot);

        // Tank facing owns the tank (stock `tank face` is blocked): a remembered Frontal source (§3.7, decision §7.5).
        bool TankOwned(MechanicBotState const& st);

        // Where the move action goes: the current waypoint of a route, else the plan's target.
        Geo::Vec3 MoveTarget(MechanicBotState const& st);
    }
}

#endif

#endif
