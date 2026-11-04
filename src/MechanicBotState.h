/*
 * mod-boss-mechanics: per-bot state of the boss-mechanics strategy and its playerbots value (STAGE-M2-HANDOFF §3.4,
 * §3.10, WP-C).
 *
 * One MechanicBotState per bot, owned by the bot's AiObjectContext as the value "boss mechanic state". Written by the
 * brain (from the strategy's triggers), the waker and the gate; read by the actions, the lock multiplier, the other
 * bots of the group (crowd) and `.mech bots`. All of them run on the bot's map thread, or on the world thread while
 * no map updates (gate, `.mech`). Nothing here keeps a Player/Unit/Creature pointer: positions are copies, units are
 * resolved every tick.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_STATE_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_STATE_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "Define.h"
#include "Duration.h"
#include "MechanicBotCounters.h"
#include "MechanicBotView.h"
#include "MechanicCost.h"
#include "MechanicFallibility.h"
#include "MechanicGeometry.h"
#include "MechanicLeaks.h"
#include "MechanicSolver.h"
#include "ObjectGuid.h"
#include "Value.h"
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class PlayerbotAI;

namespace BossMechanics
{
    enum class MechanicBotMode : uint8
    {
        Idle,       // nothing relevant: the strategy costs a revision compare per tick
        Watch,      // a hazard is relevant, the spot and the current motion are safe: the brain polices motion
        Escape,     // moving to plan.target (our move action; foreign movers and casts blocked)
        Hold,       // at a safe spot until plan.holdUntil (foreign movers blocked; casts that end in time allowed)
        Freeze      // HoldStill window (M3): no move at all, ours included; casts and facing allowed
    };

    char const* ModeName(MechanicBotMode mode);

    struct MechanicBotState
    {
        MechanicBotMode mode = MechanicBotMode::Idle;
        Solver::Plan plan;                                  // target, path, arriveAt, holdUntil, outcome
        Milliseconds leaveBy = Solver::kForever;            // latest time to start leaving the current spot
        Milliseconds lastBrainAt{ 0 };
        Milliseconds lastSolveAt{ 0 };
        Milliseconds lastViewAt{ 0 };
        Milliseconds outOfRoleSince{ 0 };                   // 0 = in the role band (or no band)
        Milliseconds escapeIssuedAt{ 0 };                   // 0 = the escape move is not issued yet
        uint32 seenRevision = 0;                            // Registry::Revision() of the current view
        std::vector<uint32> planFor;                        // mechanic ids the plan answers
        bool movingAnchors = false;                         // a hazard in the view is anchored on a unit
        std::unordered_map<uint32, Fallibility::Perception> perception;     // per mechanic, pruned with the registry
        MechanicBotCounters counters;

        // ---- brain internals ----
        bool brainRan = false;                              // lastBrainAt is valid
        bool viewValid = false;                             // `input` holds a view built from a registry
        bool viewRebuilt = false;                           // the last brain update rebuilt the view
        bool solveBypass = false;                           // next solve ignores MinSolveIntervalMs (new revision)
        Solver::Input input;                                // the view: hazards, lanes, role band, crowd
        Milliseconds nextPerceptionAt = Solver::kForever;   // fallibility: a hidden mechanic becomes visible
        Milliseconds escapeDeadline = Solver::kForever;     // first window start at the spot the escape left
        bool lateCounted = false;                           // this escape was already counted late
        bool planReduced = false;                           // the plan came from the reduced-margin pass
        Geo::Vec3 stuckRef;                                 // escape: where the bot was at stuckSince (set when the
        Milliseconds stuckSince{ 0 };                       // escape begins and on progress only, never on a send)
        Geo::Vec3 issuedTarget;                             // the point the move action last tried
        Milliseconds escapeSentAt{ 0 };                     // the last move attempt, sent or refused (0 = none yet)
        Milliseconds escapeCeilingAt = Solver::kForever;    // this escape is given up here (released)
        Milliseconds escapeChainSince{ 0 };                 // start of an unbroken run of escapes (hard ceiling)
        Milliseconds releaseUntil{ 0 };                     // released: an unsafe spot is not solved until then
        bool releaseForRoot = false;                        // ...and the release ends as soon as the bot can move
        bool pendingEscape = false;                         // Watch, spot unsafe, solve deferred: lock as in Escape
        Milliseconds noCastUntil{ 0 };                      // after a cast break: cast-time spells, item uses blocked
        Milliseconds lastCastBreakAt{ 0 };
        std::unordered_set<uint32> loggedFallback;          // mechanics a Fallback/NoPath was logged for

        // ---- M3 (STAGE-M3-HANDOFF §3.7, §3.8) ----
        BotViewInfo viewInfo;                               // tasks, self-trigger auras, tank frontals, parks
        uint32 seenReservationRevision = 0;                 // Registry::ReservationRevision() of the current view
        uint64 auraSignature = 0;                           // stacks of the self-trigger auras at the last view
        bool reserved = false;                              // the bot may hold reservations (drop them on leaving)
        Milliseconds freezeUntil{ 0 };                      // Freeze: left at freeze.to + SafetyMarginMs
        Milliseconds freezeTo{ 0 };                         // ...the window's own end (before it: the mechanic must
                                                            // still be there, STAGE-M3-R-REPORT m-1)
        // Look-away (a cone or line carried with followFacing): the facing to hold over [lookFrom, lookTo].
        bool lookOwed = false;                              // a chooseFacing carried shape is live
        bool lookWindow = false;                            // ...and its window (+- SafetyMarginMs) is open now
        float lookFacing = 0.f;
        Milliseconds lookFrom{ 0 };
        Milliseconds lookTo{ 0 };
        bool lookPublished = false;                         // lookFacing went onto the reservation board
        float lookPublishedFacing = 0.f;
        // The face action's job: turn to faceAngle (look-away, or the tank back to its source after a re-facing).
        bool faceOwed = false;
        float faceAngle = 0.f;
        // Tank facing: Frontal sources (creatures) this bot tanked a tank-exempt cone or line from, this fight, with
        // every distinct shape of theirs that faces the victim (ViewRules::RememberFrontShape; rear and side shapes
        // need no facing, STAGE-M3-R-REPORT M-1).
        struct FrontalSource
        {
            ObjectGuid source;
            std::vector<Shape> shapes;
            float marginYd = 0.f;
        };
        std::vector<FrontalSource> frontalSources;
        bool tankWarning = false;                           // a remembered source's Frontal is pending: hold
        bool refacing = false;                              // the Escape is a tank re-facing walk (stopped by a
                                                            // warning, decision §7.5)
        Milliseconds lastTankCheckAt{ 0 };
        // Parking: when the bot started parking for each zone (Park.HoldCapMs).
        std::unordered_map<uint32, Milliseconds> parkSince;
        bool parking = false;                               // Input::park is filled this step
        // Route search (§3.8): asked by a failed, non-deferred re-route; run on a later brain step.
        bool routePending = false;
        Geo::Vec3 routeGoal;
        Milliseconds routeAskedAt{ 0 };
        Milliseconds lastRouteAt{ 0 };
        std::size_t waypointIdx = 0;                        // plan.waypoints: the leg the move action walks
        Milliseconds lastTrackAt{ 0 };                      // region tracking re-solves (MovingAnchorRecheckMs)
        Milliseconds lastSightAt{ 0 };                      // healer line-of-sight checks
        Milliseconds nextSightAt{ 0 };                      // ...none before this (a role solve that stayed backs off)

        // ---- M4 (STAGE-M4-HANDOFF §3.5-§3.7, WP-C) ----
        Leaks::Tracker leak;                                // leak detectors: references and our own tokens
        CostStats cost;                                     // cost counters (zeroed with the counters)
        uint64 costRegistryKey = 0;                         // (map id << 32 | instance id) cost.livePathCalls is for
        Milliseconds clockAt{ 0 };                          // the mode clocks' last sample (0 = none yet)
        Milliseconds routeHandoffAt{ 0 };                   // a route's escape arrived: stock follow took over
        Geo::Vec3 routeHandoffGoal;                         // ...towards this goal
        // M4 WP-Q (G2): a Fallback broke a hard region; Solver::RegionRoute runs on a later brain step.
        bool regionRoutePending = false;
        Milliseconds regionRouteAskedAt{ 0 };
        Milliseconds lastRegionRouteAt{ 0 };
        uint32 regionRouteGoal = 0;                         // the RegionRoute goal to try next (m-10: one per step)
        bool regionRoute = false;                           // the escape walks a RegionRoute (its end: TargetOk)
        // STAGE-M4-R-REPORT M-3: the brain left Idle and checked for a pending pull (BotBrain::EndPullOnWake); cleared
        // when it goes back to Idle.
        bool pullEndChecked = false;
        Milliseconds bandClockAt{ 0 };                      // G5: the out-of-band clock's last sample (0 = none)

        // ---- gate ----
        bool avoidAoeByGate = false;                        // `avoid aoe` is the gate's (strip it when leaving)

        // Back to Idle; keeps the counters, the cost counters and the gate flag.
        void Reset();
    };

    // The value: one state per bot, never recalculated (ManualSetValue over a member, as LastMovementValue does).
    class MechanicBotStateValue : public ManualSetValue<MechanicBotState&>
    {
    public:
        explicit MechanicBotStateValue(PlayerbotAI* botAI)
            : ManualSetValue<MechanicBotState&>(botAI, _data, "boss mechanic state") { }

        std::string const Format() override { return ModeName(_data.mode); }

    private:
        MechanicBotState _data;
    };
}

#endif

#endif
