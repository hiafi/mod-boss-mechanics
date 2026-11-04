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
#include "MechanicBotView.h"
#include "MechanicFallibility.h"
#include "MechanicGeometry.h"
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

    // Telemetry (§3.10), printed by `.mech bots`.
    struct MechanicBotCounters
    {
        static constexpr std::size_t kOutcomes = 5;     // Solver::Outcome values

        uint32 solves = 0;
        uint32 pathChecks = 0;
        std::array<uint32, kOutcomes> outcomes{};       // indexed by Solver::Outcome
        uint32 tightMoves = 0;      // Move outcomes found only at the reduced margin (Solver::Params::tightMarginYd)
        uint32 lateArrivals = 0;    // escapes still inside a hazard's hitbox once its window there had started
        uint32 lateFallbacks = 0;   // ...of which the plan was a Fallback or NoPath (late by construction)
        uint32 stuckResolves = 0;   // re-solved because the bot did not move for 500 ms during an escape
        uint32 releases = 0;        // escapes given up (stuck on the same target, rooted, or past the hard ceiling)
        uint32 motionStops = 0;     // foreign motion (follow, chase, ...) stopped because its path was unsafe
        uint32 reroutes = 0;        // ...of which re-routed around the hazard instead of held
        uint32 lockBlocks = 0;      // actions the lock multiplier dropped...
        uint32 lockMoves = 0;       // ...of which movement actions and movement spells
        uint32 lockCasts = 0;       // ...of which cast-time or channelled spells
        uint32 castBreaks = 0;      // casts and channels the waker interrupted
        uint32 wakes = 0;           // AI ticks the waker pulled forward
        uint32 solveUsLast = 0;
        uint32 solveUsMax = 0;
        uint64 solveUsTotal = 0;
        // M3 (STAGE-M3-HANDOFF §3.8 telemetry).
        uint32 carrierSolves = 0;   // solves while the bot carries a shape (Spread/RunOut, a Zone on or aimed at it)
        uint32 claimSolves = 0;     // ...while it claims a Soak
        uint32 pickSolves = 0;      // ...while it is the Bait picker
        uint32 roleSolves = 0;      // role re-positions (healer sight) and region tracking (Input::roleSolve)
        uint32 violatedPlans = 0;   // plans that knowingly break a player kind for a Zone (Plan::violated > 0)
        uint32 freezes = 0;         // HoldStill windows entered
        uint32 facingsApplied = 0;  // look-away / tank facings set by the face action
        uint32 facingBlocks = 0;    // actions the lock dropped because they would turn the bot (look-away)
        uint32 facingBreaks = 0;    // casts the waker broke because they would turn the bot (look-away)
        uint32 tankRefacings = 0;   // tank moves that turn a remembered Frontal source away from the group
        uint32 refacingStops = 0;   // ...stopped on the spot because a Frontal warning started (decision §7.5)
        uint32 parks = 0;           // parking / drag-out moves
        uint32 routesFound = 0;
        uint32 routesFailed = 0;
        uint32 routeUsLast = 0;
        uint32 routeUsMax = 0;
        uint32 sightRays = 0;       // line-of-sight rays spent by healer solves
        // BossMechanics.Bots.Debug = 1 only: why candidates and oracle calls failed, summed over the solves.
        Solver::SolveStats debugSolve;
        NavOracleStats debugNav;
    };

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

        // ---- gate ----
        bool avoidAoeByGate = false;                        // `avoid aoe` is the gate's (strip it when leaving)

        // Back to Idle; keeps the counters and the gate flag.
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
