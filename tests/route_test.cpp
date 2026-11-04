// Standalone tests for src/MechanicRoute.h (STAGE-M3-HANDOFF §4 WP-E).
// Build: tests/run.sh.

#include "check.h"
#include "MechanicRoute.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

using namespace BossMechanics;
using Geo::Pose;
using Geo::Vec3;
using Solver::Hazard;
using Solver::Input;
using Solver::Outcome;
using Solver::Plan;
using Solver::RouteParams;
using Solver::RouteStats;
using Ms = std::chrono::milliseconds;

namespace
{
    constexpr float kMargin = 0.75f;
    constexpr float kMineRadius = 2.f;
    Ms const kNow{ 100000 };

    struct StraightOracle : Solver::PathOracle
    {
        std::uint32_t calls = 0;

        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            ++calls;
            out = { from, to };
            return true;
        }
    };

    // Rejects any straight segment that crosses one of its walls.
    struct WallOracle : Solver::PathOracle
    {
        std::vector<std::pair<Vec3, Vec3>> walls;
        std::uint32_t calls = 0, rejected = 0;

        static float Cross(Vec3 const& o, Vec3 const& a, Vec3 const& b)
        {
            return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
        }

        static bool Intersects(Vec3 const& p1, Vec3 const& p2, Vec3 const& q1, Vec3 const& q2)
        {
            float const d1 = Cross(q1, q2, p1);
            float const d2 = Cross(q1, q2, p2);
            float const d3 = Cross(p1, p2, q1);
            float const d4 = Cross(p1, p2, q2);
            return ((d1 > 0.f) != (d2 > 0.f)) && ((d3 > 0.f) != (d4 > 0.f));
        }

        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            ++calls;
            for (auto const& [a, b] : walls)
                if (Intersects(from, to, a, b))
                {
                    ++rejected;
                    return false;
                }
            out = { from, to };
            return true;
        }
    };

    Input MakeInput(Vec3 bot)
    {
        Input in;
        in.bot = bot;
        in.now = kNow;
        in.speed = 7.f;
        return in;
    }

    Hazard MakeHazard(std::uint32_t id, Shape const& shape, Pose const& pose, Solver::Timing const& timing)
    {
        Hazard h;
        h.id = id;
        h.shape = shape;
        h.poses = { pose };
        h.marginYd = kMargin;
        h.timing = timing;
        return h;
    }

    void AddMine(Input& in, std::uint32_t id, float x, float y, std::int64_t graceMs = 0)
    {
        in.hazards.push_back(MakeHazard(id, Shape::Circle(kMineRadius), Pose{ x, y, 0.f, 0.f },
            Solver::MakeMineArmed(kNow - Ms(5000), Solver::kForever, Ms(graceMs))));
    }

    float Dist(Vec3 const& a, Vec3 const& b)
    {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    // Smallest signed distance from the path (densified at 0.05 yd) to any hazard with id >= minId.
    float Deepest(Input const& in, std::vector<Vec3> const& path, std::uint32_t minId)
    {
        float deepest = 1e9f;
        for (std::size_t k = 1; k < path.size(); ++k)
        {
            Vec3 const& a = path[k - 1];
            Vec3 const& b = path[k];
            int const steps = std::max(1, static_cast<int>(std::ceil(Dist(a, b) / 0.05f)));
            for (int j = 0; j <= steps; ++j)
            {
                float const u = static_cast<float>(j) / steps;
                Vec3 const p{ a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, 0.f };
                for (Hazard const& h : in.hazards)
                    if (h.id >= minId)
                        for (Pose const& pose : h.poses)
                            deepest = std::min(deepest, Solver::SignedDistance(h.shape, pose, p));
            }
        }
        return deepest;
    }

    // The M2 playtest case: the bot stands in the cup of a U of overlapping armed mines (3 yd apart, radius 2) whose
    // bottom lies between it and its follow target; the U opens away from the target.
    //
    //        goal (5, 16)
    //
    //     o  o  o  o          y = 5   (x = -4.5, -1.5, 1.5, 4.5)
    //     o   bot  o          y = 2   (x = +-4.5); bot at (0, 2)
    //     o        o          y = -1
    void MakeU(Input& in, Vec3& goal, std::int64_t graceMs = 0)
    {
        in = MakeInput({ 0.f, 2.f, 0.f });
        goal = { 5.f, 16.f, 0.f };
        std::uint32_t id = 100;
        for (float x : { -4.5f, -1.5f, 1.5f, 4.5f })
            AddMine(in, id++, x, 5.f, graceMs);
        for (float x : { -4.5f, 4.5f })
            for (float y : { 2.f, -1.f })
                AddMine(in, id++, x, y, graceMs);
    }

    // The greedy one-hop re-route of M2 (Solve with Input::reroute towards the motion's end).
    Plan Greedy(Input in, Vec3 const& goal, Solver::PathOracle& oracle)
    {
        in.reroute = true;
        in.role.present = true;
        in.role.anchor = goal;
        in.role.minYd = 0.f;
        in.role.maxYd = 3.f;
        return Solver::Solve(in, oracle);
    }

    bool SamePoints(std::vector<Vec3> const& a, std::vector<Vec3> const& b)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z)
                return false;
        return true;
    }

    // ----------------------------------------------------------------------------------------------------------
    // Tests
    // ----------------------------------------------------------------------------------------------------------

    void TestUShapedMines()
    {
        Input in;
        Vec3 goal;
        MakeU(in, goal);
        StraightOracle oracle;

        // The greedy re-route has no spot 1 yd closer that it can reach cleanly: it keeps the bot still (the M2
        // playtest: the GM had to lead the bots around).
        CHECK(Greedy(in, goal, oracle).outcome == Outcome::Safe);
        CHECK(!Solver::CheckPath(in, { in.bot, goal }, kNow, false).ok);

        RouteStats st;
        Plan const plan = Solver::Route(in, goal, oracle, RouteParams(), &st);
        CHECK(plan.outcome == Outcome::Move);
        CHECK(st.found);
        CHECK(st.legs >= 2 && st.legs <= RouteParams().maxLegs);
        CHECK(st.oracleCalls == st.legs);
        CHECK(st.retries == 0);
        CHECK(plan.pathChecks == st.oracleCalls);
        CHECK(Dist(plan.target, goal) < 0.01f);
        CHECK(Dist(plan.path.front(), in.bot) < 0.01f);
        CHECK(Dist(plan.path.back(), goal) < 0.01f);
        CHECK(plan.holdUntil == plan.arriveAt);
        CHECK(plan.waypoints.size() == st.legs);
        CHECK(Dist(plan.waypoints.back(), plan.target) < 0.01f);
        for (Vec3 const& w : plan.waypoints)
        {
            bool onPath = false;
            for (Vec3 const& p : plan.path)
                onPath = onPath || Dist(p, w) < 0.01f;
            CHECK(onPath);
        }
        CHECK(plan.arriveAt > kNow + in.params.startDelay);
        // Around, never through: outside every mine at the reduced margin (0.25 yd), the whole way.
        CHECK(Deepest(in, plan.path, 100) >= 0.25f - 1e-3f);
        CHECK(Solver::CheckPath(Solver::ReducedMargins(in), plan.path, kNow, false).ok);
        CHECK(plan.stats.pass == 1 || plan.stats.pass == 2);
        if (plan.stats.pass == 1)
            CHECK(Solver::CheckPath(in, plan.path, kNow, false).ok);
        // The route leaves the cup through its open end (below the arms) before turning for the goal.
        float lowest = 1e9f;
        for (Vec3 const& p : plan.path)
            lowest = std::min(lowest, p.y);
        CHECK(lowest < -1.f - kMineRadius);

        // Stock following can take over at the first waypoint whose straight line to the goal is clean at the full
        // margin (STAGE-M3-R-REPORT m-9: a waypoint, never a corner inside a leg).
        std::size_t corner = 0;
        std::size_t const k = Solver::RouteHandoff(in, plan, goal, &corner);
        CHECK(k < plan.waypoints.size());
        CHECK(corner > 0 && corner < plan.path.size());
        CHECK(Dist(plan.path[corner], plan.waypoints[k]) < 0.01f);
        CHECK(Solver::CheckPath(in, { plan.waypoints[k], goal }, kNow, false).ok);

        // With grace on the mines the route still never crosses one (Route has no grace pass).
        MakeU(in, goal, 1500);
        Plan const g = Solver::Route(in, goal, oracle, RouteParams());
        CHECK(g.outcome == Outcome::Move);
        CHECK(Deepest(in, g.path, 100) >= 0.25f - 1e-3f);
        std::printf("route U: %u legs, %u cells (%u blocked), %u expansions, %zu path points\n", st.legs, st.cells,
            st.blocked, st.expansions, plan.path.size());
    }

    void TestClosedRing()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        for (std::uint32_t i = 0; i < 16; ++i)
        {
            float const b = Geo::kTwoPi * i / 16.f;
            AddMine(in, 100 + i, 7.f * std::cos(b), 7.f * std::sin(b), 1500);
        }
        Vec3 const goal{ 15.f, 0.f, 0.f };
        StraightOracle oracle;
        RouteStats st;
        Plan const plan = Solver::Route(in, goal, oracle, RouteParams(), &st);
        CHECK(plan.outcome == Outcome::Safe);
        CHECK(!st.found);
        CHECK(st.oracleCalls == 0);
        CHECK(oracle.calls == 0);
        CHECK(st.expansions > 0 && st.expansions < st.cells);
        CHECK(plan.path.empty());
        CHECK(plan.waypoints.empty());
        CHECK(Dist(plan.target, in.bot) < 0.01f);

        // The same ring with a gap of three mines on the far side: a way out.
        Input open = MakeInput({ 0.f, 0.f, 0.f });
        for (std::uint32_t i = 0; i < 16; ++i)
        {
            if (i == 15 || i == 0 || i == 1)
                continue;
            float const b = Geo::kTwoPi * i / 16.f;
            AddMine(open, 100 + i, 7.f * std::cos(b), 7.f * std::sin(b));
        }
        Plan const out = Solver::Route(open, goal, oracle, RouteParams());
        CHECK(out.outcome == Outcome::Move);
        CHECK(Deepest(open, out.path, 100) >= 0.25f - 1e-3f);
    }

    void TestCellCap()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        Vec3 const goal{ 100.f, 100.f, 0.f };
        StraightOracle oracle;
        RouteStats st;
        RouteParams rp;
        Plan const plan = Solver::Route(in, goal, oracle, rp, &st);
        CHECK(st.cells <= rp.maxCells);
        CHECK(st.cells > rp.maxCells / 2);
        CHECK(st.cellYd > rp.cellYd);
        CHECK(st.cellYd >= std::sqrt(116.f * 116.f / rp.maxCells) - 1e-3f);
        CHECK(plan.outcome == Outcome::Move);
        CHECK(st.legs == 1);                    // nothing in the way: one straight leg

        // A small box keeps the asked cell.
        RouteStats small;
        Solver::Route(in, { 10.f, 0.f, 0.f }, oracle, rp, &small);
        CHECK(small.cellYd == rp.cellYd);
        CHECK(small.cells == 26u * 16u);

        // A tighter cap.
        rp.maxCells = 100;
        Solver::Route(in, goal, oracle, rp, &st);
        CHECK(st.cells <= 100);
        rp.maxCells = 0;
        CHECK(Solver::Route(in, goal, oracle, rp, &st).outcome == Outcome::Safe);
    }

    // A row of armed mines between the bot and the goal with two gaps; the grid prefers the right one.
    //
    //                        goal (2, 20)
    //   o  .  .  o  o  .  o  o        y = 10   (mines at x = -12, -3, 0, 8, 11; gaps of 2 cells in the box)
    //                bot (0, 0)
    void MakeRow(Input& in, Vec3& goal)
    {
        in = MakeInput({ 0.f, 0.f, 0.f });
        goal = { 2.f, 20.f, 0.f };
        std::uint32_t id = 100;
        for (float x : { -12.f, -3.f, 0.f, 8.f, 11.f })
            AddMine(in, id++, x, 10.f);
    }

    float MinX(std::vector<Vec3> const& path, float x0, float x1)
    {
        float m = 1e9f;
        for (std::size_t k = 1; k < path.size(); ++k)
        {
            Vec3 const& a = path[k - 1];
            Vec3 const& b = path[k];
            if ((a.y - 10.f) * (b.y - 10.f) > 0.f || a.y == b.y)
                continue;
            float const u = (10.f - a.y) / (b.y - a.y);
            float const x = a.x + (b.x - a.x) * u;
            if (x >= x0 && x <= x1)
                m = std::min(m, x);
        }
        return m;
    }

    // Where the path crosses the mine row (y = 10); 1e9 when it does not.
    float CrossingX(std::vector<Vec3> const& path)
    {
        return MinX(path, -1e9f, 1e9f);
    }

    void TestWallRejectsLeg()
    {
        Input in;
        Vec3 goal;
        MakeRow(in, goal);
        StraightOracle straight;
        Plan const first = Solver::Route(in, goal, straight, RouteParams());
        CHECK(first.outcome == Outcome::Move);
        float const right = CrossingX(first.path);
        CHECK(right > 2.f && right < 6.f);                  // the right gap
        CHECK(Deepest(in, first.path, 100) >= 0.25f - 1e-3f);

        // A wall on the navmesh closes the right gap: the grid cannot see it, the oracle rejects the leg through it,
        // the cells around that leg are marked and the second search takes the left gap.
        WallOracle wall;
        wall.walls = { { { 1.f, 10.f, 0.f }, { 8.f, 10.f, 0.f } } };
        RouteStats st;
        Plan const plan = Solver::Route(in, goal, wall, RouteParams(), &st);
        CHECK(plan.outcome == Outcome::Move);
        CHECK(st.found);
        CHECK(st.retries == 1);
        CHECK(wall.rejected == 1);
        CHECK(st.oracleCalls == wall.calls);
        CHECK(st.oracleCalls <= in.params.maxPathChecks);
        float const left = CrossingX(plan.path);
        CHECK(left > -10.f && left < -5.f);                 // the left gap
        CHECK(Deepest(in, plan.path, 100) >= 0.25f - 1e-3f);
        for (std::size_t k = 1; k < plan.path.size(); ++k)
            CHECK(!WallOracle::Intersects(plan.path[k - 1], plan.path[k], wall.walls[0].first, wall.walls[0].second));
        CHECK(Dist(plan.target, goal) < 0.01f);

        // Both gaps walled: one retry only, then no route.
        wall.walls.push_back({ { -11.f, 10.f, 0.f }, { -4.f, 10.f, 0.f } });
        RouteStats none;
        CHECK(Solver::Route(in, goal, wall, RouteParams(), &none).outcome == Outcome::Safe);
        CHECK(none.retries == 1);
        CHECK(!none.found);

        // The oracle budget (Params::maxPathChecks) caps the calls.
        Input u;
        Vec3 ugoal;
        MakeU(u, ugoal);
        u.params.maxPathChecks = 1;
        WallOracle capped;
        RouteStats cs;
        CHECK(Solver::Route(u, ugoal, capped, RouteParams(), &cs).outcome == Outcome::Safe);
        CHECK(cs.oracleCalls == 1);
        CHECK(capped.calls == 1);
    }

    void TestTimedZones()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        Vec3 const goal{ 20.f, 0.f, 0.f };
        // A plain zone across the way that resolves now and is over long before the bot gets there.
        in.hazards.push_back(MakeHazard(1, Shape::Circle(4.f), Pose{ 14.f, 0.f, 0.f, 0.f },
            Solver::MakeInstant(kNow, Ms(500))));
        StraightOracle oracle;
        RouteStats st;
        Plan const plan = Solver::Route(in, goal, oracle, RouteParams(), &st);
        CHECK(plan.outcome == Outcome::Move);
        CHECK(st.legs == 1);
        CHECK(plan.path.size() == 2);
        CHECK(st.blocked > 0);                  // rasterized, but not blocking at the time the bot is there

        // The same zone lasting: the route goes round it.
        in.hazards[0].timing = Solver::MakeInterval(kNow - Ms(1000), Solver::kForever);
        Plan const around = Solver::Route(in, goal, oracle, RouteParams());
        CHECK(around.outcome == Outcome::Move);
        CHECK(Deepest(in, around.path, 1) >= 0.25f - 1e-3f);
        CHECK(around.path.size() > 2);

        // A zone that lands far in the future (beyond T) is not even rasterized.
        in.hazards[0].timing = Solver::MakeInstant(kNow + Ms(60000), Ms(500));
        RouteStats late;
        CHECK(Solver::Route(in, goal, oracle, RouteParams(), &late).outcome == Outcome::Move);
        CHECK(late.blocked == 0);
        CHECK(late.legs == 1);
    }

    void TestDeterminism()
    {
        Input in;
        Vec3 goal;
        MakeU(in, goal);
        StraightOracle o1;
        StraightOracle o2;
        RouteStats s1;
        RouteStats s2;
        Plan const a = Solver::Route(in, goal, o1, RouteParams(), &s1);
        Plan const b = Solver::Route(in, goal, o2, RouteParams(), &s2);
        CHECK(SamePoints(a.path, b.path));
        CHECK(SamePoints(a.waypoints, b.waypoints));
        CHECK(a.arriveAt == b.arriveAt);
        CHECK(s1.expansions == s2.expansions && s1.legs == s2.legs && s1.oracleCalls == s2.oracleCalls);
        CHECK(o1.calls == o2.calls);
    }

    // STAGE-M3-R-REPORT m-9 (the review's handoff repro): leg 1 bends out through a navmesh corner C whose straight
    // line to the goal is clean, but the waypoint W1 at the leg's end is not. The handoff is a waypoint, so it is the
    // goal, never W1 or C.
    void TestHandoffAtWaypoint()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(2.f), Pose{ 1.f, 10.f, 0.f, 0.f },
            Solver::MakeMineArmed(kNow - Ms(5000), Solver::kForever, Ms(0))));
        Vec3 const goal{ 0.f, 20.f, 0.f };
        Vec3 const c{ -4.f, 6.f, 0.f };
        Vec3 const w1{ 6.f, 4.f, 0.f };
        Plan p;
        p.outcome = Outcome::Move;
        p.path = { in.bot, c, w1, goal };
        p.waypoints = { w1, goal };
        CHECK(Solver::CheckPath(in, { c, goal }, kNow, false).ok);
        CHECK(!Solver::CheckPath(in, { w1, goal }, kNow, false).ok);
        std::size_t corner = 0;
        CHECK(Solver::RouteHandoff(in, p, goal, &corner) == 1);
        CHECK(corner == 3);
        // W1 clean: the handoff is W1 (path corner 2).
        p.waypoints[0] = { -6.f, 4.f, 0.f };
        p.path[2] = p.waypoints[0];
        CHECK(Solver::RouteHandoff(in, p, goal, &corner) == 0);
        CHECK(corner == 2);
        Plan empty;
        CHECK(Solver::RouteHandoff(in, empty, goal, &corner) == 0);
    }

    // STAGE-M3-R-REPORT m-10: non-finite or absurd input returns Safe at once (no endless cell growth, no NaN route).
    void TestAbsurdInput()
    {
        StraightOracle oracle;
        float const nan = std::numeric_limits<float>::quiet_NaN();
        float const inf = std::numeric_limits<float>::infinity();
        auto routeTo = [&](Input const& in, Vec3 goal, RouteParams rp = RouteParams())
        {
            RouteStats st;
            Plan const p = Solver::Route(in, goal, oracle, rp, &st);
            return p.outcome == Outcome::Safe && !st.found && st.cells == 0;
        };
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        CHECK(routeTo(in, { nan, 0.f, 0.f }));
        CHECK(routeTo(in, { inf, 0.f, 0.f }));
        CHECK(routeTo(in, { 1e30f, 1e30f, 0.f }));
        CHECK(routeTo(in, { 1e7f, 3.f, 0.f }));
        RouteParams rp;
        rp.padYd = inf;
        CHECK(routeTo(in, { 10.f, 0.f, 0.f }, rp));
        rp = RouteParams();
        rp.cellYd = nan;
        CHECK(routeTo(in, { 10.f, 0.f, 0.f }, rp));
        Input bad = in;
        bad.bot.y = nan;
        CHECK(routeTo(bad, { 10.f, 0.f, 0.f }));
        bad = in;
        bad.speed = nan;
        CHECK(routeTo(bad, { 10.f, 0.f, 0.f }));
        // A non-finite hazard pose is skipped by the grid (the oracle legs still see the others).
        Input pose = in;
        pose.hazards.push_back(MakeHazard(1, Shape::Circle(2.f), Pose{ nan, 0.f, 0.f, 0.f },
            Solver::MakeInterval(kNow - Ms(100), Solver::kForever)));
        RouteStats st;
        CHECK(Solver::Route(pose, { 10.f, 0.f, 0.f }, oracle, RouteParams(), &st).outcome == Outcome::Move);
        CHECK(st.blocked == 0);
    }

    // STAGE-M3-R-REPORT M-5: the row-span rasterizer marks exactly the cells the bounding-square scan marked (each
    // cell's center tested with the exact signed distance), for every shape type, offset and timing.
    void TestRasterizeExact()
    {
        using namespace Solver::RouteDetail;
        std::uint32_t seed = 12345;
        auto rnd = [&seed]()
        {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
        };
        std::uint32_t mismatches = 0;
        std::uint32_t marked = 0;
        for (int round = 0; round < 60; ++round)
        {
            Input in = MakeInput({ 0.f, 0.f, 0.f });
            for (std::uint32_t i = 0; i < 6; ++i)
            {
                Shape shape;
                switch ((round + i) % 6)
                {
                    case 0:
                        shape = Shape::Circle(0.5f + 4.f * rnd());
                        break;
                    case 1:
                        shape = Shape::Donut(1.f + 2.f * rnd(), 4.f + 4.f * rnd());
                        break;
                    case 2:
                        shape = Shape::Cone(20.f + 160.f * rnd(), 5.f + 20.f * rnd());
                        break;
                    case 3:
                        shape = Shape::Cone(181.f + 170.f * rnd(), 4.f + 10.f * rnd());
                        break;
                    case 4:
                        shape = Shape::Line(5.f + 30.f * rnd(), 1.f + 5.f * rnd());
                        shape.excludeStart = (round % 3 == 0) ? 2.f * rnd() : 0.f;
                        break;
                    default:
                        shape = Shape::Line(4.f + 20.f * rnd(), 1.f + 3.f * rnd());
                        shape.facingOffsetDeg = 180.f * rnd();
                        break;
                }
                Pose const pose{ 40.f * rnd() - 5.f, 40.f * rnd() - 5.f, 0.f, Geo::kTwoPi * rnd() };
                Solver::Timing timing = Solver::MakeInterval(kNow + Ms(static_cast<int>(500 * i)), kNow + Ms(9000));
                if (i == 5)
                {
                    ProjectileParams pp;
                    pp.length = shape.type == ShapeType::Line ? shape.length : 20.f;
                    pp.halfLength = 1.f;
                    pp.speed = 10.f;
                    timing.type = Solver::TimingType::Projectile;
                    timing.from = kNow + Ms(200);
                    timing.projectile = pp;
                    shape = Shape::Line(pp.length, 2.f);
                }
                Hazard h = MakeHazard(10 + i, shape, pose, timing);
                h.marginYd = 0.25f + 0.5f * rnd();
                if (round % 4 == 0)
                    h.poses.push_back({ pose.x + 1.f, pose.y - 0.5f, 0.f, pose.o + 0.3f });
                in.hazards.push_back(h);
            }
            Vec3 const goal{ 30.f, 30.f, 0.f };
            Grid fast;
            CHECK(MakeGrid(in.bot, goal, RouteParams(), fast));
            Grid slow = fast;
            Ms const horizonEnd = kNow + Ms(20000);
            marked += Rasterize(in, horizonEnd, fast);
            // The reference: every cell of the grid through ForEachWindow (the pre-M3-F scan, unbounded).
            float const halfDiag = slow.cell * 0.70710678f;
            for (Hazard const& src : in.hazards)
            {
                Hazard wide = src;
                wide.marginYd += halfDiag;
                for (std::uint32_t idx = 0; idx < slow.Size(); ++idx)
                    Solver::Detail::ForEachWindow(wide, slow.Center(idx), [&](HotWindow const& w)
                    {
                        slow.hotFrom[idx] = std::min<std::int64_t>(slow.hotFrom[idx], w.from.count());
                        slow.hotTo[idx] = std::max<std::int64_t>(slow.hotTo[idx], w.to.count());
                    });
            }
            for (std::uint32_t idx = 0; idx < slow.Size(); ++idx)
            {
                if (fast.hotFrom[idx] == slow.hotFrom[idx] && fast.hotTo[idx] == slow.hotTo[idx])
                    continue;
                // Only a center on an edge to float rounding may differ.
                bool edge = false;
                for (Hazard const& src : in.hazards)
                    for (Pose const& pose : src.poses)
                        edge = edge || std::fabs(Solver::SignedDistance(src.shape, pose, slow.Center(idx))
                            - (src.marginYd + halfDiag)) < 1e-3f;
                if (!edge)
                    ++mismatches;
            }
        }
        CHECK(mismatches == 0);
        CHECK(marked > 1000);
        std::printf("route rasterize: 360 hazards, %u cells marked, %u mismatches\n", marked, mismatches);
    }

    // STAGE-M3-R-REPORT M-4 (the review's corner-cut fuzz, bounded and seeded): a walker that goes on to the next
    // waypoint early only when RouteAdvanceOk passes never cuts deeper than the route's margin allows (full for a
    // pass-1 route, reduced for pass 2, both less CheckPath's sampling: at most 13.4% of the margin), never into a
    // mine, and always may go on from the waypoint itself.
    void TestCornerCutFuzz()
    {
        std::uint32_t seed = 7;
        auto rnd = [&seed]()
        {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
        };
        auto clearance = [](Input const& in, Vec3 const& a, Vec3 const& b)
        {
            float m = 1e9f;
            for (Hazard const& h : in.hazards)
                for (Pose const& pose : h.poses)
                    m = std::min(m, Solver::Detail::SegmentDistance(pose.x, pose.y, a.x, a.y, b.x, b.y)
                        - h.shape.radius);
            return m;
        };
        int routes = 0;
        int early = 0;
        int refused = 0;
        int bad = 0;
        int stuck = 0;
        float worstFull = 1e9f;
        float worstReduced = 1e9f;
        for (int it = 0; it < 400; ++it)
        {
            Input in = MakeInput({ 0.f, 0.f, 0.f });
            Vec3 const goal{ 25.f * rnd() - 12.f, 15.f + 15.f * rnd(), 0.f };
            int const n = 10 + static_cast<int>(25.f * rnd());
            for (int i = 0; i < n; ++i)
            {
                float const r = 1.f + 2.f * rnd();
                Pose const p{ 30.f * rnd() - 15.f, 30.f * rnd(), 0.f, 0.f };
                if (std::hypot(p.x, p.y) < r + 1.5f || std::hypot(p.x - goal.x, p.y - goal.y) < r + 1.5f)
                    continue;
                in.hazards.push_back(MakeHazard(100 + i, Shape::Circle(r), p,
                    Solver::MakeMineArmed(kNow - Ms(5000), Solver::kForever, Ms(0))));
            }
            StraightOracle oracle;
            Plan const plan = Solver::Route(in, goal, oracle, RouteParams());
            if (plan.outcome != Outcome::Move)
                continue;
            ++routes;
            bool const reduced = plan.stats.pass == 2;
            Input const check = reduced ? Solver::ReducedMargins(in) : in;
            float const margin = reduced ? 0.25f : kMargin;
            Vec3 prev = in.bot;
            for (std::size_t k = 0; k + 1 < plan.waypoints.size(); ++k)
            {
                Vec3 const& w = plan.waypoints[k];
                Vec3 const& next = plan.waypoints[k + 1];
                float const d = Dist(prev, w);
                // On the waypoint the validated leg itself: always passes.
                if (!Solver::RouteAdvanceOk(check, w, next, kNow))
                    ++stuck;
                for (float back : { 0.6f, 0.35f, 0.1f })
                {
                    float const u = d > back ? (d - back) / d : 0.f;
                    Vec3 const at{ prev.x + (w.x - prev.x) * u, prev.y + (w.y - prev.y) * u, 0.f };
                    if (!Solver::RouteAdvanceOk(check, at, next, kNow))
                    {
                        ++refused;
                        continue;
                    }
                    ++early;
                    float const c = clearance(in, at, next);
                    if (reduced)
                        worstReduced = std::min(worstReduced, c);
                    else
                        worstFull = std::min(worstFull, c);
                    if (c < margin * 0.866f - 1e-3f || c < 0.f)
                        ++bad;
                }
                prev = w;
            }
        }
        CHECK(routes > 200);
        CHECK(early > 100);
        CHECK(bad == 0);
        CHECK(stuck == 0);
        std::printf("route corner cuts: %d routes, %d early advances, %d refused, worst clearance %.3f yd (pass 1), "
            "%.3f yd (pass 2), %d below the margin\n", routes, early, refused, worstFull, worstReduced, bad);
    }

    // STAGE-M3-R-REPORT m-12: a goal walled in by lasting mines is found sealed before A* (no exhaustion).
    void TestGoalSealed()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        Vec3 const goal{ 48.f, 48.f, 0.f };
        for (std::uint32_t i = 0; i < 30; ++i)
        {
            float const b = Geo::kTwoPi * i / 30.f;
            in.hazards.push_back(MakeHazard(100 + i, Shape::Circle(1.5f),
                Pose{ 48.f + 7.f * std::cos(b), 48.f + 7.f * std::sin(b), 0.f, 0.f },
                Solver::MakeMineArmed(kNow - Ms(5000), Solver::kForever, Ms(0))));
        }
        StraightOracle oracle;
        RouteStats st;
        CHECK(Solver::Route(in, goal, oracle, RouteParams(), &st).outcome == Outcome::Safe);
        CHECK(!st.found);
        CHECK(st.expansions == 0);
        CHECK(oracle.calls == 0);
        // Mines that expire before the bot could walk round the box are no walls: A* decides (and finds a way in
        // once they are gone).
        Input brief = in;
        for (Hazard& h : brief.hazards)
            h.timing = Solver::MakeMineArmed(kNow - Ms(5000), kNow + Ms(3000), Ms(0));
        RouteStats bs;
        Solver::Route(brief, goal, oracle, RouteParams(), &bs);
        CHECK(bs.expansions > 0);
    }

    // Benchmark: a 64 x 64 box at 1 yd (4,096 cells) and 30 armed mines in three walls of 10 (4 yd apart, touching)
    // across it, open at alternate ends, so the route snakes between them.
    //
    //   y = 36   o o o o o o o o o o . . .        goal (48, 48)
    //   y = 24   . . . o o o o o o o o o o
    //   y = 12   o o o o o o o o o o . . .
    //            bot (0, 0)
    void MakeBench(Input& in, Vec3& goal)
    {
        in = MakeInput({ 0.f, 0.f, 0.f });
        in.params.maxPathChecks = 8;
        goal = { 48.f, 48.f, 0.f };
        std::uint32_t id = 100;
        for (int i = 0; i < 10; ++i)
        {
            AddMine(in, id++, -6.f + 4.f * i, 12.f);
            AddMine(in, id++, 54.f - 4.f * i, 24.f);
            AddMine(in, id++, -6.f + 4.f * i, 36.f);
        }
    }

    // Times `runs` calls of f(); prints and returns the median (µs). Asserts on the median (the max is printed: a
    // loaded host can stall any single run).
    template <class F>
    std::int64_t Bench(char const* name, F&& f, char const* detail)
    {
        using Clock = std::chrono::steady_clock;
        int const runs = 201;
        std::vector<std::int64_t> us;
        us.reserve(runs);
        for (int r = 0; r < runs; ++r)
        {
            auto const t0 = Clock::now();
            f();
            us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
        }
        std::sort(us.begin(), us.end());
        std::int64_t total = 0;
        for (std::int64_t v : us)
            total += v;
        std::printf("route bench %s: %s: median %lld us, mean %lld us, p95 %lld us, max %lld us\n", name, detail,
            static_cast<long long>(us[runs / 2]), static_cast<long long>(total / runs),
            static_cast<long long>(us[runs * 95 / 100]), static_cast<long long>(us.back()));
        return us[runs / 2];
    }

    void TestBenchmark()
    {
        char detail[160];
        Input in;
        Vec3 goal;
        MakeBench(in, goal);
        CHECK(in.hazards.size() == 30);

        // 1. The snake: 4,096 cells, 30 mines, a clean route.
        StraightOracle straight;
        RouteStats st;
        Plan plan = Solver::Route(in, goal, straight, RouteParams(), &st);
        CHECK(st.cells == 4096);
        CHECK(plan.outcome == Outcome::Move);
        CHECK(Deepest(in, plan.path, 100) >= 0.25f - 1e-3f);
        std::snprintf(detail, sizeof(detail),
            "%u cells (%u blocked), 30 mines, %u expansions, %u legs, %u oracle calls", st.cells, st.blocked,
            st.expansions, st.legs, st.oracleCalls);
        CHECK(Bench("snake", [&] { Solver::Route(in, goal, straight, RouteParams()); }, detail) < 1000);

        // 2. The same with a pillar on the navmesh across the first leg: the leg is rejected, the search runs again
        // and the legs spend the whole oracle budget (8 calls). The most work one route search can do here.
        WallOracle wall;
        wall.walls = { { { 28.f, 7.f, 0.f }, { 28.f, 9.f, 0.f } } };
        plan = Solver::Route(in, goal, wall, RouteParams(), &st);
        CHECK(st.retries == 1);
        CHECK(st.oracleCalls == in.params.maxPathChecks);
        CHECK(wall.calls == in.params.maxPathChecks);
        CHECK(plan.outcome == Outcome::Move || plan.outcome == Outcome::Safe);
        std::snprintf(detail, sizeof(detail),
            "%u cells, 30 mines, %u expansions, %u oracle calls, %u retries, %u pull checks", st.cells, st.expansions,
            st.oracleCalls, st.retries, st.pullChecks);
        CHECK(Bench("snake + pillar", [&] { Solver::Route(in, goal, wall, RouteParams()); }, detail) < 1000);

        // 3. The bot inside a closed ring of 30 mines in a 4,096-cell box: every reachable cell expanded, no oracle
        // call.
        Input ring = MakeInput({ 0.f, 0.f, 0.f });
        for (std::uint32_t i = 0; i < 30; ++i)
        {
            float const b = Geo::kTwoPi * i / 30.f;
            AddMine(ring, 100 + i, 13.f * std::cos(b), 13.f * std::sin(b));
        }
        RouteStats rs;
        Solver::Route(ring, goal, straight, RouteParams(), &rs);
        CHECK(!rs.found);
        CHECK(rs.cells == 4096);
        std::snprintf(detail, sizeof(detail), "%u cells (%u blocked), %u expansions", rs.cells, rs.blocked,
            rs.expansions);
        CHECK(Bench("enclosed", [&] { Solver::Route(ring, goal, straight, RouteParams()); }, detail) < 1000);

        // 4. The goal inside a closed ring of 30 lasting mines (STAGE-M3-R-REPORT m-12): sealed, no A*.
        Input sealed = MakeInput({ 0.f, 0.f, 0.f });
        for (std::uint32_t i = 0; i < 30; ++i)
        {
            float const b = Geo::kTwoPi * i / 30.f;
            sealed.hazards.push_back(MakeHazard(100 + i, Shape::Circle(1.5f),
                Pose{ 48.f + 7.f * std::cos(b), 48.f + 7.f * std::sin(b), 0.f, 0.f },
                Solver::MakeMineArmed(kNow - Ms(5000), Solver::kForever, Ms(0))));
        }
        Solver::Route(sealed, goal, straight, RouteParams(), &rs);
        CHECK(!rs.found);
        std::snprintf(detail, sizeof(detail), "%u cells (%u blocked), %u expansions", rs.cells, rs.blocked,
            rs.expansions);
        CHECK(Bench("goal enclosed", [&] { Solver::Route(sealed, goal, straight, RouteParams()); }, detail) < 1000);

        // 5. 30 Lines (30 x 3 yd) or 30 cones (60°, 25 yd) over the box (STAGE-M3-R-REPORT M-5), hot later than the
        // bot passes: rasterized, then a clean route; and a barrage of 24 projectile lines across the way.
        for (int kind = 0; kind < 3; ++kind)
        {
            Input many = MakeInput({ 0.f, 0.f, 0.f });
            for (int i = 0; i < 30; ++i)
            {
                Pose const p{ 4.f + 40.f * static_cast<float>((i * 7) % 30) / 30.f,
                    4.f + 40.f * static_cast<float>((i * 13) % 30) / 30.f, 0.f, 0.3f * static_cast<float>(i) };
                Shape const shape = kind == 0 ? Shape::Line(30.f, 3.f) : Shape::Cone(60.f, 25.f);
                Solver::Timing timing = Solver::MakeInterval(kNow + Ms(20000), kNow + Ms(30000));
                if (kind == 2)
                {
                    if (i >= 24)
                        break;
                    timing.type = Solver::TimingType::Projectile;
                    timing.from = kNow + Ms(100 * i);
                    timing.projectile.length = 30.f;
                    timing.projectile.halfLength = 1.f;
                    timing.projectile.speed = 12.f;
                    many.hazards.push_back(MakeHazard(100 + i, Shape::Line(30.f, 4.f), p, timing));
                    continue;
                }
                many.hazards.push_back(MakeHazard(100 + i, shape, p, timing));
            }
            Solver::Route(many, goal, straight, RouteParams(), &rs);
            char const* name = kind == 0 ? "30 lines" : (kind == 1 ? "30 cones" : "24 projectile lines");
            std::snprintf(detail, sizeof(detail), "%u cells (%u blocked), found %d, %u expansions, %u legs", rs.cells,
                rs.blocked, static_cast<int>(rs.found), rs.expansions, rs.legs);
            CHECK(rs.blocked > 0);
            CHECK(Bench(name, [&] { Solver::Route(many, goal, straight, RouteParams()); }, detail) < 1000);
        }
    }
}

int main()
{
    TestUShapedMines();
    TestClosedRing();
    TestCellCap();
    TestWallRejectsLeg();
    TestTimedZones();
    TestDeterminism();
    TestHandoffAtWaypoint();
    TestAbsurdInput();
    TestRasterizeExact();
    TestCornerCutFuzz();
    TestGoalSealed();
    TestBenchmark();
    return Report("route_test");
}
