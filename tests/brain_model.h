// A pure model of the bot brain's step cycle (src/MechanicBotBrain.cpp), for host tests that need many bots and many
// brain steps: the U minefield follow cycle (route_test) and the 9-bot barrage crowd (crowd_test), STAGE-M4 §3.7.
//
// What it runs, per map update and per bot in a fixed order (the live waker's order on one map thread):
//   * the view: every live hazard (HazardEnd + marginMs >= now), the crowd (the targets of the other bots in Escape or
//     Hold, FillCrowd), rebuilt when a hazard was added or at kViewRefresh;
//   * the route search a step after a failed re-route (in Hold, Route.RetryMs apart, one budget unit);
//   * Watch: a spot failing TargetOk solves (SolveAndApply); else stock follow (a straight line to World::follow) that
//     fails CheckPath (no start delay) asks TryReroute, else HoldHere with a route pending for the line's end;
//   * Escape: waypoint advance (kArriveYd + RouteAdvanceOk), arrival, the hard ceilings (release), and on a rebuilt
//     view the re-check of the rest of the path (+ the end) that re-solves when it fails, unless a clear Fallback
//     still holds (Solver::FallbackHolds);
//   * Hold: holdUntil reached -> Watch in the same step; a spot failing TargetOk solves.
// Solves are rate limited per bot (MinSolveIntervalMs) and per update (kMaxSolvesPerMapUpdate); a bot denied a solve
// tries again on the next update. Movement: our escape (after the start delay) or stock follow, straight lines at run
// speed; Hold stands still. Not modeled: perception and fallibility, the stuck clock (straight legs never stall),
// player kinds, tasks, reservations, look-away, roles. Hits are judged on the exact hitboxes: a plain Zone at its
// resolve time, a projectile while its body covers the bot, a mine on entry.
#ifndef MOD_BOSS_MECHANICS_TEST_BRAIN_MODEL_H
#define MOD_BOSS_MECHANICS_TEST_BRAIN_MODEL_H

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
    };

    enum class Mode { Watch, Escape, Hold };

    struct Counters
    {
        std::uint32_t solves = 0, pathChecks = 0, fallbackClear = 0, fallbackHot = 0, budgetStops = 0;
        std::uint32_t crossStart = 0, crossOther = 0, reroutes = 0, motionStops = 0, releases = 0;
        std::uint32_t routes = 0, routesFound = 0, routesGrown = 0, routeLoops = 0;
        std::uint32_t hits = 0, deniedSolves = 0, fallbackKept = 0;
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
        Counters c;
    };

    struct World
    {
        Config cfg;
        std::vector<Solver::Hazard> hazards;    // absolute times; the model drops nothing (the view filters)
        std::vector<Bot> bots;
        std::uint32_t revision = 1;             // bump when a hazard is added (every view rebuilds)
        // Stock follow: the point the bot's follow motion runs to; false = it stands.
        std::function<bool(std::size_t, Vec3&)> follow;
        // Diagnostics: every solve with its input, its plan and where it ran (the bot's mode before it, and whether
        // it is the Escape re-check's re-solve).
        enum class SolveAt { Spot, ReCheck, Reroute };
        std::function<void(Bot const&, Solver::Input const&, Solver::Plan const&, SolveAt)> onSolve;

        void AddHazard(Solver::Hazard h)
        {
            hazards.push_back(std::move(h));
            ++revision;
        }

        void Step(Ms now)
        {
            std::uint32_t budget = cfg.solvesPerUpdate;
            for (std::size_t i = 0; i < bots.size(); ++i)
                Brain(i, now, budget);
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

        Solver::Input View(std::size_t i, Ms now) const
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
            return in;
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
            b.moveFrom = now + cfg.params.startDelay;
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
            ++b.c.motionStops;
        }

        void StepWatch(std::size_t i, Solver::Input& in, Ms now, std::uint32_t& budget)
        {
            Bot& b = bots[i];
            Ms const horizonEnd = now + in.params.horizon;
            if (!Solver::TargetOk(in, b.pos, now, horizonEnd))
            {
                b.mode = Mode::Watch;
                if (now < b.releaseUntil)
                    return;
                if (CanSolve(b, now, budget))
                    SolveAndApply(b, in, now);
                return;
            }
            b.mode = Mode::Watch;
            Vec3 end;
            if (!follow || !follow(i, end) || Dist(b.pos, end) <= cfg.followSlackYd)
                return;
            Solver::Input motion = in;
            motion.params.startDelay = Ms(0);
            Solver::PathCheck const pc = Solver::CheckPath(motion, { b.pos, end }, now, false);
            if (pc.ok)
                return;
            // TryReroute
            bool deferred = false;
            float const toGo = Dist(b.pos, end);
            bool const endSafe = Solver::PointSafe(in, end, now, horizonEnd);
            if (toGo >= 2.f && endSafe)
            {
                if (!CanSolve(b, now, budget))
                    deferred = true;
                else
                {
                    Solver::Input r = in;
                    r.role.present = true;
                    r.role.anchor = end;
                    r.role.minYd = 0.f;
                    r.role.maxYd = 3.f;
                    r.reroute = true;
                    Solver::Plan plan = RunSolve(b, r, now, SolveAt::Reroute);
                    bool const moving = plan.outcome == Solver::Outcome::Move
                        || plan.outcome == Solver::Outcome::GraceMove;
                    if (moving && Dist(plan.target, end) <= toGo - in.params.rerouteGainYd)
                    {
                        plan.holdUntil = plan.arriveAt;
                        BeginEscape(b, std::move(plan), now);
                        ++b.c.motionStops;
                        ++b.c.reroutes;
                        return;
                    }
                }
            }
            HoldHere(b, now, pc.firstHit, deferred);
            if (!deferred && cfg.routeEnable && endSafe)
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
            bool const endOk = w.empty() ? Solver::TargetOk(in, b.plan.target, pc.arriveAt, hEnd)
                : Solver::PointSafe(in, b.plan.target, pc.arriveAt, hEnd);
            if (pc.ok && endOk)
                return;
            if (cfg.fallbackKeep && Solver::FallbackHolds(in, b.plan, rest, horizonEnd, cfg.fallbackKeep == 1))
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
            Solver::Input in = View(i, now);
            bool const viewRebuilt = b.seenRevision != revision || now - b.lastViewAt >= cfg.viewRefresh;
            if (viewRebuilt)
            {
                b.seenRevision = revision;
                b.lastViewAt = now;
            }
            if (in.hazards.empty())
            {
                // ToIdle: nothing relevant, the plan goes.
                b.mode = Mode::Watch;
                b.plan = Solver::Plan();
                b.routePending = false;
                return;
            }
            Ms const horizonEnd = now + in.params.horizon;
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
            switch (b.mode)
            {
                case Mode::Watch:
                    StepWatch(i, in, now, budget);
                    break;
                case Mode::Escape:
                    StepEscape(i, in, now, budget, viewRebuilt);
                    break;
                case Mode::Hold:
                    if (now >= b.plan.holdUntil)
                    {
                        b.plan = Solver::Plan();
                        StepWatch(i, in, now, budget);
                    }
                    else if (!Solver::TargetOk(in, b.pos, now, horizonEnd) && CanSolve(b, now, budget))
                        SolveAndApply(b, in, now);
                    break;
            }
        }

        void Move(std::size_t i, Ms now)
        {
            Bot& b = bots[i];
            Ms const end = now + cfg.step;
            Vec3 to = b.pos;
            Ms from = now;
            if (b.mode == Mode::Escape)
            {
                to = b.plan.waypoints.empty() ? b.plan.target : b.plan.waypoints[b.wp];
                from = std::max(now, b.moveFrom);
            }
            else if (b.mode == Mode::Watch && follow)
            {
                Vec3 f;
                if (follow(i, f) && Dist(b.pos, f) > cfg.followSlackYd)
                    to = f;
            }
            if (end > from)
            {
                float const d = Dist(b.pos, to);
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
