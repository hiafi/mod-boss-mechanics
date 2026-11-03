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
#include "MechanicSolver.h"
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
        Hold        // at a safe spot until plan.holdUntil (foreign movers blocked; casts that end in time allowed)
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
        uint32 lateArrivals = 0;    // reached the target after the first window of the spot it left had started
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
