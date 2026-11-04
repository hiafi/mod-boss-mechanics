// A pure model of the bot brain's step cycle (src/MechanicBotBrain.cpp), for host tests that need many bots and many
// brain steps: the U minefield follow cycle (route_test) and the 9-bot barrage crowd (crowd_test), STAGE-M4 §3.7.
//
// What it runs, per map update and per bot in a fixed order (the live waker's order on one map thread):
//   * the view: every live hazard (HazardEnd + marginMs >= now), the crowd (the targets of the other bots in Escape or
//     Hold, FillCrowd), rebuilt when a hazard was added or at kViewRefresh;
//   * the route search a step after a failed re-route (in Hold, Route.RetryMs apart, one budget unit);
//   * Watch: a spot failing TargetOk solves (SolveAndApply); else stock follow (a straight line to World::follow) that
//     fails CheckPath (no start delay) asks TryReroute, else HoldHere with a route pending for the line's end; one
//     whose end fails TargetOk from its arrival (WP-Q2, Config::watchEndCheck) asks TryReroute, else HoldHere;
//   * Escape: waypoint advance (kArriveYd + RouteAdvanceOk), arrival, the hard ceilings (release), and on a rebuilt
//     view the re-check of the rest of the path (+ the end) that re-solves when it fails, unless a clear Fallback
//     still holds (Solver::FallbackHolds);
//   * Hold: holdUntil reached -> Watch in the same step; a spot failing TargetOk solves; optionally (Config::roleSlack)
//     the role-band release back to Watch.
// Optionally (Config::brainCadence) a bot thinks only every so often, or on the update after a hazard was added.
// WP-F2 (STAGE-M4-R2-REPORT M-1, M-2, m-6), with Config::chaseLanding: the lock's melee-ring test on the chase, the
// chase clock, the recovery role solve, the chase valve (a release to the stock AI), and the blocked band; TryReroute's
// band re-route only beyond the band (a healer or a too-close bot toward the motion's end).
// Solves are rate limited per bot (MinSolveIntervalMs) and per update (kMaxSolvesPerMapUpdate); a bot denied a solve
// tries again on the next update. Movement: our escape (after the start delay) or stock follow, straight lines at run
// speed; Hold stands still. Not modeled: perception and fallibility, the stuck clock (straight legs never stall),
// reservations, look-away, roles. Hits are judged on the exact hitboxes: a plain Zone at its resolve time, a
// projectile while its body covers the bot, a mine on entry.
// Player kinds (M4 WP-Q, optional): World::kinds adds what MechanicBotView adds for one bot (regions, synthetic
// hazards, carried shapes, allies) and the bot's tasks; a bot with a live holding task holds its spot (TaskHoldUntil,
// HoldForTasks) once the spot passes TargetOk, and a bot with a task or a player kind is relevant without a hazard.
// The test judges the player kinds itself (tests/fight_test.cpp).
// WP-Q3 (optional, World::moverHop): a mover aura on a bot (Killing Spree, Bot::moverAuraUntil) puts it at the hop
// point every Config::moverHop before its move; Config::moverAuraBreak cancels it once the brain step leaves the bot
// in Escape or Hold (the brain's BreakMoverAuras).
#ifndef MOD_BOSS_MECHANICS_TEST_BRAIN_MODEL_H
#define MOD_BOSS_MECHANICS_TEST_BRAIN_MODEL_H

#include "MechanicLanding.h"
#include "MechanicLockRules.h"
#include "MechanicRoute.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace BrainModel
{
    using namespace BossMechanics;
    using Geo::Vec3;
    using Ms = std::chrono::milliseconds;

    struct Straight : Solver::PathOracle
    {
        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            out = { from, to };
            return true;
        }
    };

    struct Config
    {
        Solver::Params params;                  // the solver input params (MaxPathChecks, margins, hold cap, ...)
        Solver::RouteParams route;
        bool routeEnable = true;
        bool crowd = true;                      // FillCrowd (Weights::crowd scores it)
        Ms minSolveInterval{ 150 };             // BossMechanics.Bots.MinSolveIntervalMs
        Ms routeRetry{ 1000 };                  // BossMechanics.Bots.Route.RetryMs
        Ms viewRefresh{ 200 };                  // kViewRefresh
        Ms step{ 100 };                         // one map update
        std::uint32_t solvesPerUpdate = 8;      // kMaxSolvesPerMapUpdate
        float arriveYd = 0.6f;                  // kArriveYd
        float followSlackYd = 1.f;              // stock follow moves when farther than this from its point
        float speed = 7.f;
        // The Escape re-check of a Fallback escape (Solver::FallbackHolds): 0 = re-solve whenever the rest fails the
        // margins (M3), 1 = keep a clear one (exposure 0) whose rest stays clear (the brain, M4), 2 = keep any one
        // no worse than planned.
        int fallbackKeep = 1;
        // The M4 WP-Q build, before STAGE-M4-R-REPORT m-4: a Fallback that breaks a hard region holds as well (the
        // "as built" arms of fight_test reproduce the sweep's failures with it).
        bool holdViolating = false;
        // M4 WP-Q (G2): a Fallback or NoPath that breaks a hard region asks for Solver::RegionRoute on a later step
        // (Route.RetryMs apart, one budget unit), as the brain does.
        bool regionRoute = true;
        // M4 WP-Q2 (m4-s2 Zone/5): Watch also holds stock motion whose end fails TargetOk from its arrival (the
        // brain's StepWatch); false = the M4 build, the path check alone.
        bool watchEndCheck = true;
        // M4 WP-Q2: the lock's landing test on the melee chase (MechanicBotLock.cpp, ReachMeleeAction): with
        // World::follow as the melee chase, stock follow does not start while its landing fails over the horizon.
        // Off by default: crowd_test's follow is stock follow round the GM, which the lock does not judge.
        bool chaseLanding = false;
        // WP-F2 (STAGE-M4-R2-REPORT M-1): with chaseLanding, the chase is refused only while no point of the melee
        // ring (Landing::RingSafe, radius the bot's role band maxYd round World::chaseTarget) is safe, or the band is
        // blocked (m-6); false = the WP-Q2 build, the target's center alone (PointSafe).
        bool chaseRing = true;
        // WP-F2 (M-1 fix 3): a refused chase gets a role solve toward the band (kChaseSolveEvery, kChaseSolveBackoff).
        bool chaseRecovery = true;
        // WP-F2 (M-1 fix 4): the chase valve, BossMechanics.Bots.ChaseReleaseMs (a tank half of it; 0 = off), only
        // while a ring point stays clear for LockRules::ChaseValveWindowMs (chaseValveWindow; false = the plain valve).
        Ms chaseRelease{ 3000 };
        bool chaseValveWindow = true;
        // WP-F2 (M-2): an unsafe end re-routes toward the role band only beyond its maxYd (and never for a healer);
        // false = the WP-Q2 build, outside the band either way.
        bool bandRerouteBeyondOnly = true;
        // WP-F2 (m-6): a failed band re-route blocks the band until the hazard ends (BlockBand); false = WP-Q2.
        bool bandBlock = true;
        // The brain's role-band release (StepHold, BossMechanics.Bots.RoleSlackMs): a hold that is not a task hold
        // goes back to Watch once the bot has been away from its stock point (follow) this long. 0 = not modeled
        // (holds last to holdUntil).
        Ms roleSlack{ 0 };
        // The brain cadence: a bot thinks only this long after its last brain step, or on the update after a hazard
        // was added (the waker's two rules). 0 = every update. The M4 build ran mechanics bots every 200 ms on a
        // 100 ms map update; LockRules::FastTickDue makes it 100 (WP-Q2).
        Ms brainCadence{ 0 };
        // WP-Q3: a mover aura's hop interval (Killing Spree: one teleport per 500 ms hit), and whether the brain
        // cancels a running one in Escape or Hold (LockRules::MoverAuraBreaks; false = the WP-F2 build).
        Ms moverHop{ 500 };
        bool moverAuraBreak = true;
    };

    enum class Mode { Watch, Escape, Hold };

    struct Counters
    {
        std::uint32_t solves = 0, pathChecks = 0, fallbackClear = 0, fallbackHot = 0, budgetStops = 0;
        std::uint32_t crossStart = 0, crossOther = 0, reroutes = 0, motionStops = 0, releases = 0;
        std::uint32_t routes = 0, routesFound = 0, routesGrown = 0, routeLoops = 0;
        std::uint32_t hits = 0, deniedSolves = 0, fallbackKept = 0, regionRoutes = 0, regionRoutesFound = 0;
        std::uint32_t motionEndStops = 0, roleReleases = 0;
        std::uint32_t msChaseRefused = 0, chaseReleases = 0, chaseSolves = 0, landingBlocks = 0;
        std::uint32_t moverBreaks = 0, moverHops = 0;
        std::array<std::uint32_t, 5> outcomes{};
        std::int64_t solveUsMax = 0, solveUsTotal = 0, routeUsMax = 0;
        // Per Fallback: pass 1 spent its whole MaxPathChecks budget (a budget stop) or not.
        std::uint32_t fallbackBudget = 0;

        void Add(Counters const& o)
        {
            solves += o.solves;
            pathChecks += o.pathChecks;
            fallbackClear += o.fallbackClear;
            fallbackHot += o.fallbackHot;
            budgetStops += o.budgetStops;
            crossStart += o.crossStart;
            crossOther += o.crossOther;
            reroutes += o.reroutes;
            motionStops += o.motionStops;
            releases += o.releases;
            routes += o.routes;
            routesFound += o.routesFound;
            routesGrown += o.routesGrown;
            routeLoops += o.routeLoops;
            hits += o.hits;
            deniedSolves += o.deniedSolves;
            fallbackKept += o.fallbackKept;
            regionRoutes += o.regionRoutes;
            regionRoutesFound += o.regionRoutesFound;
            motionEndStops += o.motionEndStops;
            roleReleases += o.roleReleases;
            msChaseRefused += o.msChaseRefused;
            chaseReleases += o.chaseReleases;
            chaseSolves += o.chaseSolves;
            landingBlocks += o.landingBlocks;
            moverBreaks += o.moverBreaks;
            moverHops += o.moverHops;
            for (std::size_t i = 0; i < outcomes.size(); ++i)
                outcomes[i] += o.outcomes[i];
            solveUsMax = std::max(solveUsMax, o.solveUsMax);
            solveUsTotal += o.solveUsTotal;
            routeUsMax = std::max(routeUsMax, o.routeUsMax);
            fallbackBudget += o.fallbackBudget;
        }

        std::uint32_t Fallbacks() const
        {
            return outcomes[static_cast<std::size_t>(Solver::Outcome::Fallback)];
        }
    };

    struct Bot
    {
        Vec3 pos;
        Mode mode = Mode::Watch;
        Solver::Plan plan;
        std::size_t wp = 0;
        bool reduced = false;
        bool routePending = false;
        Vec3 routeGoal;
        Ms routeAskedAt{ 0 }, lastRouteAt{ -100000 }, lastSolveAt{ -100000 }, handoffAt{ 0 }, lastViewAt{ -100000 };
        Ms moveFrom{ 0 }, escapeCeilingAt{ 0 }, escapeChainSince{ 0 }, releaseUntil{ 0 };
        Vec3 handoffGoal;
        std::uint32_t seenRevision = 0;
        std::vector<std::uint32_t> hitBy;       // hazard ids that hit this bot (once each)
        bool insideMine = false;
        bool moving = false;                    // moved in the last step (SpotOf's slack)
        bool regionRoutePending = false;        // a Fallback broke a hard region: RegionRoute on a later step
        bool regionRoute = false;               // the escape walks a RegionRoute (its end must pass TargetOk)
        Ms regionRouteAskedAt{ 0 }, lastRegionRouteAt{ -100000 };
        std::uint32_t regionRouteGoal = 0;      // the next goal of the last RegionRoute (m-10: one goal per step)
        Ms outOfRoleSince{ 0 };                 // Hold away from the stock point since (Config::roleSlack)
        Ms lastBrainAt{ -100000 };              // Config::brainCadence
        std::uint32_t brainRevision = 0;        // the hazard revision the last brain step saw
        // WP-F2: the chase clock, the recovery solve's clock, the blocked band, the valve's release.
        LockRules::ChaseClock chase;
        Ms lastChaseSolveAt{ -100000 }, nextChaseSolveAt{ 0 }, bandBlockedUntil{ 0 };
        std::uint32_t bandBlockedBy = 0;
        bool releaseForChase = false;
        bool followMoved = false;               // stock follow moved the bot in the last step (a spline in flight)
        // WP-Q3: a mover aura runs until moverAuraUntil; its next hop is due at nextMoverHopAt.
        Ms moverAuraUntil{ 0 }, nextMoverHopAt{ 0 };
        Counters c;
    };

    // A task of the view (BotTask): the bot holds its spot for it until `to` + marginMs unless it only avoids
    // (`holds` false: an Avoider).
    struct Task
    {
        std::uint32_t id = 0;
        Ms to{ 0 };
        bool holds = true;
    };

    struct World
    {
        Config cfg;
        std::vector<Solver::Hazard> hazards;    // absolute times; the model drops nothing (the view filters)
        std::vector<Bot> bots;
        std::uint32_t revision = 1;             // bump when a hazard is added (every view rebuilds)
        // Stock follow: the point the bot's follow motion runs to; false = it stands.
        std::function<bool(std::size_t, Vec3&)> follow;
        Vec3 chaseTarget{};                     // Config::chaseLanding: the center of what the chase runs to
        // WP-F2: bots that are healers (TryReroute's patient anchor) and tanks (the valve at half the time).
        std::function<bool(std::size_t)> isHealer;
        std::function<bool(std::size_t)> isTank;
        // WP-Q3: where bot i's mover aura puts it on each hop (false: no hop).
        std::function<bool(std::size_t, Vec3&)> moverHop;
        // Diagnostics: every solve with its input, its plan and where it ran (the bot's mode before it, and whether
        // it is the Escape re-check's re-solve).
        enum class SolveAt { Spot, ReCheck, Reroute };
        std::function<void(Bot const&, Solver::Input const&, Solver::Plan const&, SolveAt)> onSolve;
        // Player kinds: what the view adds for bot i at `now` (the input already holds the hazards and the crowd).
        std::function<void(std::size_t, Solver::Input&, Ms, std::vector<Task>&)> kinds;

        // Where bot j will stand for the others (ViewRules::PlannedSpot without reservations): its plan target in
        // Escape or Hold, else its spot with kUnplannedBotSlackYd (+ run speed x MovingAnchorRecheckMs moving).
        Vec3 SpotOf(std::size_t j, float& slackYd) const
        {
            Bot const& b = bots[j];
            if (b.mode == Mode::Escape || b.mode == Mode::Hold)
            {
                slackYd = 0.f;
                return b.plan.target;
            }
            slackYd = 0.5f + (b.moving ? cfg.speed * 0.25f : 0.f);
            return b.pos;
        }

        void AddHazard(Solver::Hazard h)
        {
            hazards.push_back(std::move(h));
            ++revision;
        }

        void Step(Ms now)
        {
            std::uint32_t budget = cfg.solvesPerUpdate;
            for (std::size_t i = 0; i < bots.size(); ++i)
            {
                Bot& b = bots[i];
                if (cfg.brainCadence > Ms(0) && now - b.lastBrainAt < cfg.brainCadence && b.brainRevision == revision)
                    continue;
                b.lastBrainAt = now;
                b.brainRevision = revision;
                Brain(i, now, budget);
                BreakMoverAura(b, now);
            }
            for (std::size_t i = 0; i < bots.size(); ++i)
                Move(i, now);
        }

        Counters Total() const
        {
            Counters t;
            for (Bot const& b : bots)
                t.Add(b.c);
            return t;
        }

    private:
        static float Dist(Vec3 const& a, Vec3 const& b)
        {
            return std::hypot(a.x - b.x, a.y - b.y);
        }

        Solver::Input View(std::size_t i, Ms now, std::vector<Task>& tasks) const
        {
            Bot const& b = bots[i];
            Solver::Input in;
            in.bot = b.pos;
            in.now = now;
            in.speed = cfg.speed;
            in.params = cfg.params;
            for (Solver::Hazard const& h : hazards)
                if (Solver::Detail::HazardEnd(h) + cfg.params.marginMs >= now)
                    in.hazards.push_back(h);
            if (cfg.crowd)
                for (std::size_t j = 0; j < bots.size(); ++j)
                    if (j != i && (bots[j].mode == Mode::Escape || bots[j].mode == Mode::Hold))
                        in.crowd.push_back(bots[j].plan.target);
            tasks.clear();
            if (kinds)
                kinds(i, in, now, tasks);
            return in;
        }

        // TaskHoldUntil: the latest end (+ marginMs) of the live holding tasks; 0 = none.
        Ms TaskHoldUntil(std::vector<Task> const& tasks, Ms now) const
        {
            Ms until{ 0 };
            for (Task const& t : tasks)
                if (t.holds && t.to + cfg.params.marginMs >= now)
                    until = std::max(until, t.to + cfg.params.marginMs);
            return until;
        }

        bool CanSolve(Bot& b, Ms now, std::uint32_t& budget, bool bypass = false)
        {
            if (!bypass && now - b.lastSolveAt < cfg.minSolveInterval)
                return false;
            if (budget == 0)
            {
                ++b.c.deniedSolves;
                return false;
            }
            --budget;
            return true;
        }

        Solver::Plan RunSolve(Bot& b, Solver::Input const& in, Ms now, SolveAt at = SolveAt::Spot)
        {
            Straight oracle;
            auto const t0 = std::chrono::steady_clock::now();
            Solver::Plan plan = Solver::Solve(in, oracle);
            std::int64_t const us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            b.lastSolveAt = now;
            ++b.c.solves;
            b.c.pathChecks += plan.pathChecks;
            ++b.c.outcomes[static_cast<std::size_t>(plan.outcome)];
            b.c.budgetStops += plan.stats.budgetStops;
            b.c.crossStart += plan.stats.crossStart;
            b.c.crossOther += plan.stats.crossOther;
            b.c.solveUsMax = std::max(b.c.solveUsMax, us);
            b.c.solveUsTotal += us;
            if (plan.outcome == Solver::Outcome::Fallback)
            {
                ++(plan.exposure > Ms(0) ? b.c.fallbackHot : b.c.fallbackClear);
                b.c.fallbackBudget += plan.stats.budgetStops > 0;
            }
            if (cfg.regionRoute && !in.reroute && plan.violated > 0
                && (plan.outcome == Solver::Outcome::Fallback || plan.outcome == Solver::Outcome::NoPath))
            {
                b.regionRoutePending = true;
                b.regionRouteAskedAt = now;
                b.regionRouteGoal = 0;
            }
            if (onSolve)
                onSolve(b, in, plan, at);
            return plan;
        }

        void BeginEscape(Bot& b, Solver::Plan&& plan, Ms now)
        {
            if (b.mode != Mode::Escape)
                b.escapeChainSince = now;
            Ms const travel = std::max(Ms(0), plan.arriveAt - now);
            b.escapeCeilingAt = now + 2 * travel + Ms(1500);
            b.reduced = plan.stats.pass == 2;
            b.plan = std::move(plan);
            b.mode = Mode::Escape;
            b.wp = 0;
            b.routePending = false;
            b.regionRoute = false;
            b.moveFrom = now + cfg.params.startDelay;
            b.outOfRoleSince = Ms(0);
        }

        void SolveAndApply(Bot& b, Solver::Input const& in, Ms now)
        {
            Solver::Plan plan = RunSolve(b, in, now);
            if (plan.outcome == Solver::Outcome::Safe)
            {
                b.mode = Mode::Watch;
                return;
            }
            BeginEscape(b, std::move(plan), now);
        }

        void HoldHere(Bot& b, Ms now, std::uint32_t hazardId, bool shortOnly)
        {
            Ms until = now + cfg.params.holdCap;
            for (Solver::Hazard const& h : hazards)
                if (h.id == hazardId && h.timing.to < Solver::kForever)
                    until = std::min(until, std::max(now + Ms(250), h.timing.to + cfg.params.marginMs));
            if (shortOnly)
                until = std::min(until, now + Ms(250));
            b.plan = Solver::Plan();
            b.plan.target = b.pos;
            b.plan.path = { b.pos };
            b.plan.arriveAt = now;
            b.plan.holdUntil = until;
            b.mode = Mode::Hold;
            b.outOfRoleSince = Ms(0);
            ++b.c.motionStops;
        }

        static bool OutOfMeleeBand(Solver::Input const& in, Vec3 const& pos)
        {
            return in.role.present && in.role.minYd <= 0.f && Dist(pos, in.role.anchor) > in.role.maxYd;
        }

        std::int64_t ChaseStale() const
        {
            return LockRules::ChaseRunStaleMs(static_cast<std::uint32_t>(cfg.params.holdCap.count()));
        }

        std::int64_t ChaseReleaseFor(std::size_t i) const
        {
            return LockRules::ChaseReleaseMsFor(static_cast<std::uint32_t>(cfg.chaseRelease.count()),
                isTank && isTank(i));
        }

        // The brain's BlockBand (m-6).
        void BlockBand(Bot& b, Ms now, std::uint32_t hazardId)
        {
            Ms until = now + cfg.params.holdCap;
            for (Solver::Hazard const& h : hazards)
                if (h.id == hazardId && h.timing.to < Solver::kForever)
                    until = std::max(until, h.timing.to + cfg.params.marginMs);
            b.bandBlockedUntil = until;
            b.bandBlockedBy = hazardId;
        }

        // The brain's StepChaseValve: released to the stock AI for the release time.
        bool ChaseValve(std::size_t i, Solver::Input const& in, Ms now)
        {
            Bot& b = bots[i];
            if (cfg.chaseRelease <= Ms(0) || !OutOfMeleeBand(in, b.pos))
                return false;
            std::int64_t const release = ChaseReleaseFor(i);
            if (!LockRules::ChaseValveDue(b.chase, now.count(), release, ChaseStale())
                || (cfg.chaseValveWindow && !Landing::RingSafe(in, chaseTarget, b.pos, in.role.maxYd, now,
                    now + Ms(LockRules::ChaseValveWindowMs(release)))))
                return false;
            LockRules::ChaseInReach(b.chase);
            b.bandBlockedUntil = Ms(0);
            b.bandBlockedBy = 0;
            ++b.c.chaseReleases;
            ++b.c.releases;
            b.mode = Mode::Watch;
            b.plan = Solver::Plan();
            b.routePending = false;
            b.releaseUntil = now + Ms(release);
            b.releaseForChase = true;
            return true;
        }

        // The brain's StepChaseRecovery: a role solve toward the band while the chase is refused.
        bool ChaseRecovery(Bot& b, Solver::Input const& in, Ms now, std::uint32_t& budget)
        {
            if (!cfg.chaseRecovery || !LockRules::ChaseRefusedNow(b.chase, now.count()) || !OutOfMeleeBand(in, b.pos)
                || now < b.nextChaseSolveAt || now - b.lastChaseSolveAt < Ms(500))
                return false;
            if (!CanSolve(b, now, budget))
                return false;
            b.lastChaseSolveAt = now;
            ++b.c.chaseSolves;
            float const away = Dist(b.pos, in.role.anchor);
            Solver::Input r = in;
            r.roleSolve = true;
            Solver::Plan plan = RunSolve(b, r, now);
            bool const moving = plan.outcome == Solver::Outcome::Move || plan.outcome == Solver::Outcome::GraceMove;
            if (!moving || Dist(plan.target, in.role.anchor) > away - in.params.rerouteGainYd)
            {
                b.nextChaseSolveAt = now + Ms(2000);
                return false;
            }
            BeginEscape(b, std::move(plan), now);
            return true;
        }

        void StepWatch(std::size_t i, Solver::Input& in, Ms now, std::uint32_t& budget, std::vector<Task> const& tasks)
        {
            Bot& b = bots[i];
            Ms const horizonEnd = now + in.params.horizon;
            bool const released = now < b.releaseUntil;
            if (!Solver::TargetOk(in, b.pos, now, horizonEnd))
            {
                b.mode = Mode::Watch;
                if (released)
                    return;
                if (CanSolve(b, now, budget))
                    SolveAndApply(b, in, now);
                return;
            }
            if (!released)
            {
                // HoldForTasks
                Ms const until = TaskHoldUntil(tasks, now);
                if (until > now)
                {
                    b.plan = Solver::Plan();
                    b.plan.target = b.pos;
                    b.plan.path = { b.pos };
                    b.plan.arriveAt = now;
                    b.plan.holdUntil = until;
                    b.mode = Mode::Hold;
                    return;
                }
            }
            b.mode = Mode::Watch;
            if (!released && ChaseValve(i, in, now))
                return;
            if (released && b.releaseForChase)
                return;
            Vec3 end;
            // With the lock on the chase, a motion exists only once the chase started (the lock judges it before it
            // runs; the brain sees the spline on its next step); a refused chase has none: the recovery solve.
            bool const noMotion = cfg.chaseLanding && !b.followMoved;
            if (noMotion || !follow || !follow(i, end) || Dist(b.pos, end) <= cfg.followSlackYd)
            {
                if (!released)
                    ChaseRecovery(b, in, now, budget);
                return;
            }
            Solver::Input motion = in;
            motion.params.startDelay = Ms(0);
            Solver::PathCheck const pc = Solver::CheckPath(motion, { b.pos, end }, now, false);
            // WP-Q2: the end of the motion passes TargetOk from its arrival as well.
            std::uint32_t endHit = 0;
            Ms const arrival = now + Solver::Detail::TravelMs(Dist(b.pos, end), Solver::Detail::Speed(in));
            bool const endOk = !cfg.watchEndCheck
                || Solver::TargetOk(in, end, arrival, std::max(arrival, horizonEnd));
            if (pc.ok && endOk)
            {
                if (!released)
                    ChaseRecovery(b, in, now, budget);
                return;
            }
            if (!endOk)
            {
                Solver::PointSafe(in, end, arrival, std::max(arrival, horizonEnd), &endHit);
                if (pc.ok)
                    ++b.c.motionEndStops;
            }
            // TryReroute
            bool deferred = false;
            float const toGo = Dist(b.pos, end);
            bool const endSafe = Solver::PointSafe(in, end, now, horizonEnd);
            bool const blocked = cfg.bandBlock && !endOk && now < b.bandBlockedUntil && endHit == b.bandBlockedBy;
            bool const healer = isHealer && isHealer(i);
            // WP-Q2: an end that fails re-routes toward the bot's role band; WP-F2 (M-2): only beyond its maxYd, never
            // for a healer; a healer, or a bot inside its band's minYd, toward the motion's end, which then need not be
            // safe; any other bot holds.
            float const fromAnchor = in.role.present ? Dist(b.pos, in.role.anchor) : 0.f;
            bool const bandReroute = !endOk && in.role.present && (cfg.bandRerouteBeyondOnly
                ? !healer && fromAnchor > in.role.maxYd : fromAnchor > in.role.maxYd || fromAnchor < in.role.minYd);
            bool const tooClose = in.role.present && fromAnchor < in.role.minYd;
            bool const endReroute = toGo >= 2.f
                && (endOk ? endSafe : cfg.bandRerouteBeyondOnly && (healer || tooClose));
            if (!blocked && (bandReroute || endReroute))
            {
                if (!CanSolve(b, now, budget))
                    deferred = true;
                else
                {
                    Solver::Input r = in;
                    bool const toBand = bandReroute;
                    Vec3 const anchor = toBand ? in.role.anchor : end;
                    float const away = toBand ? Dist(b.pos, anchor) : toGo;
                    if (!toBand)
                    {
                        r.role.present = true;
                        r.role.anchor = end;
                        r.role.minYd = 0.f;
                        r.role.maxYd = 3.f;
                    }
                    r.reroute = true;
                    Solver::Plan plan = RunSolve(b, r, now, SolveAt::Reroute);
                    bool const moving = plan.outcome == Solver::Outcome::Move
                        || plan.outcome == Solver::Outcome::GraceMove;
                    if (moving && Dist(plan.target, anchor) <= away - in.params.rerouteGainYd)
                    {
                        plan.holdUntil = plan.arriveAt;
                        BeginEscape(b, std::move(plan), now);
                        ++b.c.motionStops;
                        ++b.c.reroutes;
                        return;
                    }
                }
            }
            HoldHere(b, now, pc.ok ? endHit : pc.firstHit, deferred);
            if (!endOk && !deferred && cfg.bandBlock && OutOfMeleeBand(in, b.pos))
            {
                if (!blocked)
                    BlockBand(b, now, endHit);
                LockRules::NoteChaseRefusal(b.chase, now.count(), ChaseStale());
            }
            if (!pc.ok && !deferred && cfg.routeEnable && endSafe)
            {
                b.routePending = true;
                b.routeGoal = end;
                b.routeAskedAt = now;
            }
        }

        void RunRoute(Bot& b, Solver::Input const& in, Ms now)
        {
            b.lastRouteAt = now;
            ++b.c.routes;
            if (Solver::IsRouteLoop(b.handoffAt, b.handoffGoal, now, b.routeGoal))
                ++b.c.routeLoops;
            Straight oracle;
            Solver::RouteStats rs;
            auto const t0 = std::chrono::steady_clock::now();
            Solver::Plan plan = Solver::Route(in, b.routeGoal, oracle, cfg.route, &rs);
            b.c.routeUsMax = std::max<std::int64_t>(b.c.routeUsMax, std::chrono::duration_cast<
                std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
            b.c.routesGrown += rs.grown;
            if (!rs.found || plan.outcome != Solver::Outcome::Move || plan.waypoints.empty())
                return;
            ++b.c.routesFound;
            Solver::TrimToHandoff(in, plan, b.routeGoal);
            BeginEscape(b, std::move(plan), now);
        }

        void StepEscape(std::size_t i, Solver::Input& in, Ms now, std::uint32_t& budget, bool viewRebuilt)
        {
            Bot& b = bots[i];
            Ms const horizonEnd = now + in.params.horizon;
            std::vector<Vec3> const& w = b.plan.waypoints;
            if (w.size() > 1 && b.wp + 1 < w.size() && Dist(b.pos, w[b.wp]) <= cfg.arriveYd)
            {
                Solver::Input const check = b.reduced ? Solver::ReducedMargins(in) : in;
                if (Solver::RouteAdvanceOk(check, b.pos, w[b.wp + 1], now))
                    ++b.wp;
            }
            bool const spotSafe = Solver::TargetOk(in, b.pos, now, horizonEnd);
            Vec3 const moveTo = w.empty() ? b.plan.target : w[b.wp];
            bool const ended = Dist(b.pos, moveTo) < 0.01f;
            bool const onLast = w.empty() || b.wp + 1 >= w.size();
            if ((Dist(b.pos, b.plan.target) <= cfg.arriveYd && (spotSafe || ended)) || (onLast && ended && spotSafe))
            {
                if (!w.empty())
                {
                    b.handoffAt = now;
                    b.handoffGoal = b.routeGoal;
                }
                b.mode = Mode::Hold;
                return;
            }
            if (now >= b.escapeCeilingAt || now - b.escapeChainSince >= Ms(10000))
            {
                b.mode = Mode::Watch;
                b.plan = Solver::Plan();
                b.releaseUntil = now + Ms(1500);
                b.releaseForChase = false;
                ++b.c.releases;
                return;
            }
            if (!viewRebuilt)
                return;
            // The rest of the way (the escape's own margin) and its end.
            std::vector<Vec3> rest{ b.pos };
            if (!w.empty())
                rest.insert(rest.end(), w.begin() + static_cast<std::ptrdiff_t>(b.wp), w.end());
            else
                rest.push_back(b.plan.target);
            Solver::Input motion = b.reduced ? Solver::ReducedMargins(in) : in;
            motion.params.startDelay = Ms(0);
            Solver::PathCheck const pc = Solver::CheckPath(motion, rest, now, false);
            Ms const hEnd = std::max(pc.arriveAt, horizonEnd);
            bool const endOk = w.empty() || b.regionRoute ? Solver::TargetOk(in, b.plan.target, pc.arriveAt, hEnd)
                : Solver::PointSafe(in, b.plan.target, pc.arriveAt, hEnd);
            if (pc.ok && endOk)
                return;
            Solver::Plan held = b.plan;
            if (cfg.holdViolating)
                held.violated = 0;
            if (cfg.fallbackKeep && Solver::FallbackHolds(in, held, rest, horizonEnd, cfg.fallbackKeep == 1))
            {
                ++b.c.fallbackKept;
                return;
            }
            if (!CanSolve(b, now, budget))
                return;
            Ms const holdUntil = b.plan.holdUntil;
            Solver::Plan plan = RunSolve(b, in, now, SolveAt::ReCheck);
            if (plan.outcome == Solver::Outcome::Safe)
            {
                b.plan.target = b.pos;
                b.plan.path = { b.pos };
                b.plan.waypoints.clear();
                b.plan.holdUntil = std::max(now, holdUntil);
                b.mode = Mode::Hold;
                return;
            }
            BeginEscape(b, std::move(plan), now);
        }

        void Brain(std::size_t i, Ms now, std::uint32_t& budget)
        {
            Bot& b = bots[i];
            std::vector<Task> tasks;
            Solver::Input in = View(i, now, tasks);
            bool const viewRebuilt = b.seenRevision != revision || now - b.lastViewAt >= cfg.viewRefresh;
            if (viewRebuilt)
            {
                b.seenRevision = revision;
                b.lastViewAt = now;
            }
            if (in.hazards.empty() && !Solver::Detail::HasPlayerKinds(in) && tasks.empty())
            {
                // ToIdle: nothing relevant, the plan goes.
                b.mode = Mode::Watch;
                b.plan = Solver::Plan();
                b.routePending = false;
                return;
            }
            Ms const horizonEnd = now + in.params.horizon;
            // WP-F2: in reach (or no melee band) ends a run of chase refusals and a blocked band.
            if (!OutOfMeleeBand(in, b.pos))
            {
                LockRules::ChaseInReach(b.chase);
                b.bandBlockedUntil = Ms(0);
                b.bandBlockedBy = 0;
            }
            if (b.routePending && now > b.routeAskedAt)
            {
                b.routePending = false;
                bool const due = b.mode == Mode::Hold && cfg.routeEnable && now - b.lastRouteAt >= cfg.routeRetry
                    && now >= b.releaseUntil && Solver::PointSafe(in, b.routeGoal, now, horizonEnd);
                if (due && budget > 0)
                {
                    --budget;
                    RunRoute(b, in, now);
                    return;
                }
            }
            if (b.regionRoutePending && now > b.regionRouteAskedAt)
            {
                // As the brain (STAGE-M4-R-REPORT m-4): pending while the Fallback still breaks its region, until the
                // retry clock and the budget allow it.
                bool const wanted = cfg.regionRoute && (b.mode == Mode::Escape || b.mode == Mode::Hold)
                    && (b.plan.outcome == Solver::Outcome::Fallback || b.plan.outcome == Solver::Outcome::NoPath)
                    && b.plan.violated > 0 && now >= b.releaseUntil;
                bool const due = b.regionRouteGoal > 0 || now - b.lastRegionRouteAt >= cfg.routeRetry;
                if (!wanted)
                {
                    b.regionRoutePending = false;
                    b.regionRouteGoal = 0;
                }
                else if (due && budget > 0)
                {
                    b.regionRoutePending = false;
                    --budget;
                    b.lastRegionRouteAt = now;
                    ++b.c.regionRoutes;
                    Straight oracle;
                    Solver::RouteStats rs;
                    std::uint32_t const goal = b.regionRouteGoal;
                    b.regionRouteGoal = 0;
                    Solver::Plan plan = Solver::RegionRoute(in, oracle, cfg.route, &rs, goal);
                    if (plan.outcome == Solver::Outcome::Move)
                    {
                        ++b.c.regionRoutesFound;
                        BeginEscape(b, std::move(plan), now);
                        b.regionRoute = true;
                    }
                    else if (rs.moreGoals)
                    {
                        // The next goal on the next step (the brain's continuation).
                        b.regionRoutePending = true;
                        b.regionRouteAskedAt = now;
                        b.regionRouteGoal = rs.nextGoal;
                    }
                    return;
                }
            }
            switch (b.mode)
            {
                case Mode::Watch:
                    StepWatch(i, in, now, budget, tasks);
                    break;
                case Mode::Escape:
                    StepEscape(i, in, now, budget, viewRebuilt);
                    break;
                case Mode::Hold:
                    if (now >= b.plan.holdUntil)
                    {
                        b.plan = Solver::Plan();
                        StepWatch(i, in, now, budget, tasks);
                    }
                    else if (!Solver::TargetOk(in, b.pos, now, horizonEnd))
                    {
                        if (CanSolve(b, now, budget))
                            SolveAndApply(b, in, now);
                    }
                    else if (TaskHoldUntil(tasks, now) <= now && now < b.bandBlockedUntil && ChaseValve(i, in, now))
                    {
                        // StepHold: the chase valve also ends a hold on a blocked band.
                    }
                    else if (cfg.roleSlack > Ms(0) && TaskHoldUntil(tasks, now) <= now)
                    {
                        // StepHold's role-band release: away from the stock point for roleSlack -> Watch; none while
                        // the band is blocked (WP-F2 m-6).
                        Vec3 f;
                        bool const away = follow && follow(i, f) && Dist(b.pos, f) > cfg.followSlackYd
                            && now >= b.bandBlockedUntil;
                        if (!away)
                            b.outOfRoleSince = Ms(0);
                        else if (b.outOfRoleSince == Ms(0))
                            b.outOfRoleSince = now;
                        else if (now - b.outOfRoleSince >= cfg.roleSlack)
                        {
                            b.plan = Solver::Plan();
                            b.outOfRoleSince = Ms(0);
                            b.mode = Mode::Watch;
                            ++b.c.roleReleases;
                        }
                    }
                    break;
            }
        }

        // The brain's BreakMoverAuras: a running mover aura goes once the bot escapes or holds.
        void BreakMoverAura(Bot& b, Ms now)
        {
            if (!cfg.moverAuraBreak || now >= b.moverAuraUntil
                || !LockRules::MoverAuraBreaks(b.mode == Mode::Escape, b.mode == Mode::Hold, false))
                return;
            b.moverAuraUntil = Ms(0);
            ++b.c.moverBreaks;
        }

        void Move(std::size_t i, Ms now)
        {
            Bot& b = bots[i];
            // A mover aura's hop (a teleport) before the step's movement.
            Vec3 hop;
            if (now < b.moverAuraUntil && now >= b.nextMoverHopAt && moverHop && moverHop(i, hop))
            {
                b.pos = hop;
                b.nextMoverHopAt = now + cfg.moverHop;
                ++b.c.moverHops;
            }
            Ms const end = now + cfg.step;
            Vec3 to = b.pos;
            Ms from = now;
            bool followed = false;
            if (b.mode == Mode::Escape)
            {
                to = b.plan.waypoints.empty() ? b.plan.target : b.plan.waypoints[b.wp];
                from = std::max(now, b.moveFrom);
            }
            else if (b.mode == Mode::Watch && follow)
            {
                Vec3 f;
                if (follow(i, f) && Dist(b.pos, f) > cfg.followSlackYd)
                {
                    if (!ChaseRefused(i, now))
                    {
                        to = f;
                        followed = true;
                    }
                    else
                        b.c.msChaseRefused += static_cast<std::uint32_t>(cfg.step.count());
                }
            }
            b.moving = false;
            b.followMoved = followed;
            if (end > from)
            {
                float const d = Dist(b.pos, to);
                b.moving = d > 0.01f;
                float const stepYd = cfg.speed * static_cast<float>((end - from).count()) / 1000.f;
                if (d > 0.f)
                {
                    float const u = std::min(1.f, stepYd / d);
                    b.pos.x += (to.x - b.pos.x) * u;
                    b.pos.y += (to.y - b.pos.y) * u;
                }
            }
            Judge(b, now, end);
        }

        // Config::chaseLanding: the lock refuses the chase (MechanicBotLock.cpp ChaseRefused): no point of the melee
        // ring is safe, or the band is blocked (chaseRing); the target's center is unsafe (the WP-Q2 build). A released
        // bot is the stock AI's. A refusal goes on the chase clock.
        bool ChaseRefused(std::size_t i, Ms now)
        {
            Bot& b = bots[i];
            if (!cfg.chaseLanding || now < b.releaseUntil)
                return false;
            std::vector<Task> tasks;
            Solver::Input const in = View(i, now, tasks);
            if (in.hazards.empty())
                return false;
            Ms const to = now + in.params.horizon;
            bool refused = false;
            if (cfg.chaseRing && in.role.present)
                refused = (cfg.bandBlock && now < b.bandBlockedUntil)
                    || !Landing::RingSafe(in, chaseTarget, b.pos, in.role.maxYd, now, to);
            else
                refused = !Solver::PointSafe(in, chaseTarget, now, to);
            if (refused)
            {
                ++b.c.landingBlocks;
                if (cfg.chaseRing)
                    LockRules::NoteChaseRefusal(b.chase, now.count(), ChaseStale());
            }
            return refused;
        }

        // Hits on the exact hitboxes in (now, end]: a plain Zone when it resolves, a projectile while its body covers
        // the bot (checked at the end of the step), a mine when the bot enters it.
        void Judge(Bot& b, Ms now, Ms end)
        {
            bool inMine = false;
            for (Solver::Hazard const& h : hazards)
            {
                bool inside = false;
                for (Geo::Pose const& pose : h.poses)
                    inside = inside || Solver::SignedDistance(h.shape, pose, b.pos) <= 0.f;
                if (h.timing.type == Solver::TimingType::MineArmed || h.timing.type == Solver::TimingType::MineArming)
                {
                    inMine = inMine || (inside && end >= h.timing.from && end <= h.timing.to);
                    continue;
                }
                if (!inside || std::find(b.hitBy.begin(), b.hitBy.end(), h.id) != b.hitBy.end())
                    continue;
                bool hit = false;
                if (h.timing.type == Solver::TimingType::Projectile)
                {
                    Solver::Hazard exact = h;
                    exact.marginYd = 0.f;
                    for (HotWindow const& wnd : Solver::WindowsAt(exact, b.pos))
                        hit = hit || (wnd.from <= end && end <= wnd.to);
                }
                else
                    hit = h.timing.from > now && h.timing.from <= end;
                if (hit)
                {
                    b.hitBy.push_back(h.id);
                    ++b.c.hits;
                }
            }
            if (inMine && !b.insideMine)
                ++b.c.hits;
            b.insideMine = inMine;
        }
    };
}

#endif
