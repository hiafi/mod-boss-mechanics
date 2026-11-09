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
        uint32 solveUsLast = 0;     // Solver::Solve alone (M5 C4: before, it included the allies' refresh below)
        uint32 solveUsMax = 0;
        uint64 solveUsTotal = 0;
        // M5 C4: the allies' refresh (RefreshAllies) run right before each solve that is not a re-route, timed apart.
        uint32 solveRefreshes = 0;
        uint32 refreshUsLast = 0;
        uint32 refreshUsMax = 0;
        uint64 refreshUsTotal = 0;
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
        // melee chase (no point of the melee ring is safe), sampled by the brain (M5 CD: also in a hold on a blocked
        // band, which refuses the chase too); the times the safety valve (a walk-in since M5 CD)
        // (BossMechanics.Bots.ChaseReleaseMs) let a refused chase through; the recovery role solves run for a refused
        // chase (also in roleSolves).
        uint32 msChaseRefused = 0;
        uint32 chaseReleases = 0;
        uint32 chaseSolves = 0;
        // M5 CD (C5): ms a melee bot or tank spent out of melee range for longer than ChaseReleaseMs while a point of
        // the melee ring stayed clear over the valve's window and the chase was not refused (stranded on its chase
        // line; whole-ring blocks are never counted), sampled by the brain; and the strand routes that asked the
        // route search for a clear ring point.
        uint32 msChaseStranded = 0;
        uint32 strandRoutes = 0;
        // M5 CD (C6): facing-only actions (`set facing`) the lock refused while the brain walked the bot (Escape),
        // also in lockBlocks; facingBlocks stays the look-away count.
        uint32 escapeFacingBlocks = 0;
        // M5 CD (E): waker wakes for the descriptor (its revision moved since the bot's view, a listed cast it can
        // kick started, a listed urgent or deadline aura it can dispel appeared), also in wakes.
        uint32 descriptorWakes = 0;
        // M4 WP-Q3: mover auras (LockRules::kMoverAuras, Killing Spree) cancelled because the brain walked, held or
        // froze the bot while one ran.
        uint32 moverBreaks = 0;
        // BossMechanics.Bots.ForceLeaks: blocked judgements let through (per candidate judged), by the detector each
        // feeds. Many passes of a kind and no leak of that kind over a run point at a blind detector.
        uint32 forcedMoves = 0;
        uint32 forcedCasts = 0;
        uint32 forcedTurns = 0;
        // M5 WP-T (targets and the ignore list, MechanicBotTargets.cpp).
        uint32 focusSwitches = 0;       // `boss focus` attacks that moved the bot onto its pick
        uint32 msLowerTarget = 0;       // ms attacking something else while an engaged pick lived (waker samples)
        uint32 breakOutMsMax = 0;       // longest a prison held its player, as this bot saw it (open ones included)
        uint32 ignoredHits = 0;         // damage events the bot dealt to ignored units (UnitScript OnDamage)...
        uint32 ignoredDamage = 0;       // ...and their damage
        uint32 petIgnoredHits = 0;      // the same for its pets, guardians and totems
        uint32 petIgnoredDamage = 0;
        uint32 ignoreBlocks = 0;        // actions the focus multiplier zeroed because their target is ignored
        uint32 ignoreDrops = 0;         // ignored current targets, melee victims and cast targets dropped
        uint32 petRecalls = 0;          // minions pulled off an ignored victim (onto the bot's target, or recalled)
        // SFK F1 (turned teammates, MechanicBotTargets.cpp).
        uint32 hostileReleases = 0;     // targets, melee victims and casts dropped: a teammate no longer turned
        // M5 WP-P (dispels and purges, MechanicBotDispel.cpp), whichever row cast them. M5 WP-F (R M-1): a dispel
        // removes a random aura of its type, so the cures and purges are counted on the dispeller when the listed
        // aura is removed by its cast (the aura hook, AURA_REMOVE_BY_ENEMY_SPELL), not at the cast; a dispel that
        // only takes a stack off counts nothing.
        uint32 urgentDispels = 0;       // cures of a listed loss-of-control aura (an `urgent` entry)...
        uint32 urgentLatencyMsMax = 0;  // ...the longest time from that aura's application to its removal
        uint32 deadlineDispels = 0;     // cures of a deadline entry inside its deadline (floored at skip + lead)
        uint32 deadlineMisses = 0;      // deadline entries' auras that ran out on this bot (aura hook, by expiry)
        uint32 listedDispels = 0;       // cures of a listed aura that was not urgent (stock rows, our target order)
        uint32 purges = 0;              // listed enemy buffs stripped (Purge, Dispel Magic, Spellsteal, Tranq, Devour)
        uint32 dispelWrongAura = 0;     // cures cast while an urgent or deadline aura was on the member that removed
                                        // another aura (the core's random pick; a second dispel follows)
        // M5 WP-I (interrupts, MechanicBotInterrupt.cpp): kicks are judged at the cast, whichever row cast them (the
        // bot's own by the player cast hook, its Felhunter's Spell Lock by `boss kick`).
        uint32 kicksLanded = 0;         // kicks on a listed cast this bot held the claim of
        uint32 kicksWasted = 0;         // kicks on an unlisted cast while a listed cast was due before it was back
        uint32 doNotKickKicks = 0;      // kicks on a do-not-kick cast (a violation: should stay 0)
        uint32 duplicateKicks = 0;      // kicks on a listed cast another bot had claimed
        uint32 overloadHolds = 0;       // casts this bot could kick but held for a higher one due (once per cast)
        uint32 listedMissed = 0;        // listed casts that completed (counted by one bot per group: sum the group)
        uint32 stockKicksDropped = 0;   // stock kick rows the kick multiplier zeroed (once per cast)
        // M5 WP-F (R M-2): stuns, silences, incapacitates and fears outside the kick table that stopped (or would
        // have stopped) a cast and were judged as kicks (also in kicksLanded, duplicateKicks, doNotKickKicks or
        // kicksWasted).
        uint32 castStops = 0;
        // Stratholme X3 (the Jump kind, MechanicBotBrain.cpp StepJump): hops launched (a spline in flight), launches
        // the motion master refused (noted for the Jump.CountPlanned fallback only), Jumps the bot could not hop for
        // (rooted, stunned, frozen, in a vehicle or water, already falling), and Jumps its brain reached too late (at
        // or after the resolve).
        uint32 hops = 0;
        uint32 hopsRefused = 0;
        uint32 hopsBlocked = 0;
        uint32 hopsLate = 0;
        // BossMechanics.Bots.Debug = 1 only: why candidates and oracle calls failed, summed over the solves.
        Solver::SolveStats debugSolve;
        NavOracleStats debugNav;
    };

    // CostStats (the M4 cost counters, STAGE-M4-HANDOFF §3.6) lives in the pure MechanicCost.h.
}

#endif
