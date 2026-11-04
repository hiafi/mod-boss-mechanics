/*
 * mod-boss-mechanics: role constraints v2 and the tank tasks' inputs (STAGE-M3-HANDOFF §3.7, §3.8, WP-C).
 *
 * - Role band (soft, decision §7.11): melee [0, reach - 0.5] around the victim (the solver adds the slot ring for
 *   Spread carriers); ranged [8, RangedDistanceYd + 6]; healers the same band around the nearest group tank, plus
 *   Sight (the tank at Healer.SightWeight, every other member at half) and Reach (members within HealDistance - 2);
 *   out of combat the follow band around the master.
 * - Tank facing: a ring point around a remembered Frontal source whose front shapes (the tank-exempt cones and lines
 *   that face the victim; rear and side shapes need no facing), posed at the source facing the tank, hold no group
 *   member's planned spot (decision §7.5).
 * - Tank parking: Solver::Park for a Park zone whose parked unit is the bot's victim, or the drag-out of the victim
 *   from a lasting hazard.
 *
 * Every unit is resolved per call; nothing here keeps a pointer.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_ROLE_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_ROLE_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "Duration.h"
#include "MechanicBotState.h"
#include "MechanicSolver.h"

class Creature;
class Player;
class PlayerbotAI;
class Unit;

namespace BossMechanics
{
    class Registry;
    struct BotConfig;

    namespace BotRole
    {
        // The nearest alive group tank on the bot's map (healers anchor on it), or nullptr.
        Player* NearestGroupTank(Player* bot);

        // The role band (§3.8 role band v2). Empty out of combat without a master or group leader on the map.
        Solver::RoleBand RoleBandOf(PlayerbotAI* botAI, Player* bot, BotConfig const& cfg);

        // Healers in combat: Sight (tank weight Healer.SightWeight, other members half) and Reach (members within
        // HealDistance - 2). Everyone else: both cleared.
        void FillSightReach(Player* bot, BotConfig const& cfg, Solver::Input& in);

        // A group member's planned spot as the other bots plan against it: a bot's plan.target while it escapes,
        // holds or freezes (or its position while it owes a look-away facing); `facing` = the look-away facing it will
        // hold, else the plan's, else its orientation. false when the member is not a bot with boss-mechanics state
        // or has no plan (the caller keeps its current position).
        bool PlannedSpotOf(Player const* member, Geo::Vec3& spot, float& facing);

        // Chase geometry of a creature on the bot: where it stops (sum of combat reaches + CONTACT_DISTANCE) and when
        // it starts to chase (its melee range), §2.3.
        float ChaseStopYd(Unit const* chaser, Unit const* target);

        // Tank facing: the remembered Frontal sources this bot still tanks, pruned (gone, dead, another victim, the
        // bot out of combat) and refreshed from the view's tank frontals (front shapes only, every one kept).
        void UpdateFrontalSources(Player* bot, Registry const& reg, MechanicBotState& st, BotConfig const& cfg);

        // Does any of `source`'s remembered front shapes, posed at its position facing the bot standing at `tankAt`,
        // hold a group member's planned spot (margin + FacingSlackDeg)?
        bool FrontalHitsGroup(Player* bot, Unit const* source, MechanicBotState::FrontalSource const& f,
            Geo::Vec3 const& tankAt, float facingSlackDeg);

        // A ring point around `source` (radius clamped to [stop + 0.5, melee - 0.5], bearings every 15 degrees,
        // nearest to the bot first) whose cone holds nobody, that passes TargetOk and a validated path. false = none.
        bool TankRefacePlan(Player* bot, Unit const* source, MechanicBotState::FrontalSource const& f,
            Solver::Input const& in, float facingSlackDeg, Solver::PathOracle& oracle, Solver::Plan& out);

        // Fills in.park (§3.7) for a live Park zone whose parked unit is the bot's victim (Park.HoldCapMs per zone), or
        // a drag-out of the bot's victim from a lasting hazard (the bot's own, or a lasting zone other group members
        // are eligible for); clears it otherwise. Returns in.park.present.
        bool FillPark(Player* bot, Registry const& reg, MechanicBotState& st, BotConfig const& cfg,
            Milliseconds now, Solver::Input& in);
    }
}

#endif

#endif
