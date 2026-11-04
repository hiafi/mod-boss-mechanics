/*
 * mod-boss-mechanics: the per-bot telemetry counters (STAGE-M2-HANDOFF §3.10; moved out of MechanicBotState.h by
 * STAGE-M4 WP-B so the sim seam, BossMechanicsSim.h, can carry them without playerbots types).
 *
 * AzerothCore and pure headers only (no playerbots include): always compiled. The bot glue keeps one
 * MechanicBotCounters in each MechanicBotState; `.mech bots` prints them, Sim::ReadBotStats copies them.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_COUNTERS_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_COUNTERS_H

#include "Define.h"
#include "MechanicBotView.h"
#include "MechanicCost.h"
#include "MechanicSolver.h"
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace BossMechanics
{
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
        // M4 (STAGE-M4-HANDOFF §3.5-§3.7, WP-C).
        // Time in each mode, in ms, sampled by the waker once per map update (a gap over 2 s is not counted). A
        // released bot counts as released, not as Watch; the modes are disjoint and sum to the time observed.
        uint32 msIdle = 0;
        uint32 msWatch = 0;
        uint32 msEscape = 0;
        uint32 msHold = 0;
        uint32 msFreeze = 0;
        uint32 msReleased = 0;
        // Lock leaks (MechanicLeaks.h): moves, casts and turns the lock should have stopped, seen from the outcome.
        uint32 leakMoves = 0;
        uint32 leakCasts = 0;
        uint32 leakTurns = 0;
        std::vector<std::pair<uint32, uint32>> leakSpells;  // (spell id, count) of leaked casts and movement spells
        // Fallback outcomes split by exposure (§3.7): the path spends no time inside an exact hitbox (no margin)
        // while one of its windows is open (clear), or it does (hot). fallbackClear + fallbackHot = Fallback outcomes.
        uint32 fallbackClear = 0;
        uint32 fallbackHot = 0;
        // Escape re-checks of a clear Fallback whose rest failed the margins but stayed clear: walked on, not solved
        // again (Solver::FallbackHolds, WP-P).
        uint32 fallbackKept = 0;
        // Route searches for (about) the same goal within 5 s of a route's handoff to stock follow (§3.7, U minefield).
        uint32 routeLoops = 0;
        // M4 WP-Q (G2): Solver::RegionRoute searches after a Fallback that broke a hard region, and the ones that
        // found a way round the hazards into it.
        uint32 regionRoutes = 0;
        uint32 regionRoutesFound = 0;
        // M4 WP-Q (G5): ms in Escape, Hold or Freeze with the bot outside its role band (melee reach of its target,
        // the ranged band, follow distance), sampled by the brain; ...of them while a task holds it (a Stack, Bait,
        // Spread or Spot window). What a re-position inside a task window could win back is at most msOutOfBandTask.
        // A melee loses to the lock msOutOfBand plus msChaseRefused (below).
        uint32 msOutOfBand = 0;
        uint32 msOutOfBandTask = 0;
        // M4 lock (STAGE-M4-HANDOFF §3.5, WP-X; MechanicBotLock.cpp).
        uint32 specificBlocks = 0;  // actions run by name (DoSpecificAction) the gate refused (also in lockBlocks)
        uint32 doBypasses = 0;      // chat `do` orders the lock would have refused, let through (a human's order)
        uint32 unknownCasts = 0;    // possible casts judged with no known spell id (no cast-time or movement test)
        uint32 pullBlocks = 0;      // X8: `pull` requests refused while an encounter is live (also in lockBlocks)
        uint32 pullsEnded = 0;      // M-3: a pull pending or running when the brain left Idle, ended (`pull end`)
        // M4 WP-Q2: Idle/Watch gap closers and melee chases refused because they would land in a hazard (also in
        // lockBlocks; one per candidate judged, not per refusal), and Watch motions whose end fails TargetOk (each one
        // is then re-routed or held, so it is also in motionStops).
        uint32 landingBlocks = 0;
        uint32 motionEndStops = 0;
        // M4 WP-F2 (STAGE-M4-R2-REPORT M-1, m-3): ms in Idle or Watch out of the melee band while the lock refuses the
        // melee chase (no point of the melee ring is safe), sampled by the brain; the times the safety valve
        // (BossMechanics.Bots.ChaseReleaseMs) let a refused chase through; the recovery role solves run for a refused
        // chase (also in roleSolves).
        uint32 msChaseRefused = 0;
        uint32 chaseReleases = 0;
        uint32 chaseSolves = 0;
        // M4 WP-Q3: mover auras (LockRules::kMoverAuras, Killing Spree) cancelled because the brain walked, held or
        // froze the bot while one ran.
        uint32 moverBreaks = 0;
        // BossMechanics.Bots.ForceLeaks: blocked judgements let through (per candidate judged), by the detector each
        // feeds. Many passes of a kind and no leak of that kind over a run point at a blind detector.
        uint32 forcedMoves = 0;
        uint32 forcedCasts = 0;
        uint32 forcedTurns = 0;
        // BossMechanics.Bots.Debug = 1 only: why candidates and oracle calls failed, summed over the solves.
        Solver::SolveStats debugSolve;
        NavOracleStats debugNav;
    };

    // CostStats (the M4 cost counters, STAGE-M4-HANDOFF §3.6) lives in the pure MechanicCost.h.
}

#endif
