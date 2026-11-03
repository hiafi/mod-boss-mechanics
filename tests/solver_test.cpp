// Standalone tests for src/MechanicSolver.h and src/MechanicFallibility.h (STAGE-M2-HANDOFF §4 WP-A).
// Build: tests/run.sh.

#include "check.h"
#include "MechanicFallibility.h"
#include "MechanicSolver.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace BossMechanics;
using Geo::Pose;
using Geo::Vec3;
using Solver::Hazard;
using Solver::Input;
using Solver::Outcome;
using Solver::Plan;
using Ms = std::chrono::milliseconds;

namespace
{
    constexpr float kMargin = 0.75f;
    Ms const kNow{ 100000 };

    // Deterministic LCG for the random tests (no <random> so runs match across hosts).
    struct Lcg
    {
        std::uint64_t state = 0x1234567ull;

        float Next01()
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<float>(state >> 40) / 16777216.f;
        }

        float Range(float lo, float hi)
        {
            return lo + (hi - lo) * Next01();
        }
    };

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
        std::uint32_t calls = 0;

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
                    return false;
            out = { from, to };
            return true;
        }
    };

    struct NoPathOracle : Solver::PathOracle
    {
        std::uint32_t calls = 0;

        bool Find(Vec3 const&, Vec3 const&, std::vector<Vec3>&) override
        {
            ++calls;
            return false;
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

    Hazard MakeHazard(std::uint32_t id, Shape const& shape, Pose const& pose, Solver::Timing const& timing,
        float margin = kMargin)
    {
        Hazard h;
        h.id = id;
        h.shape = shape;
        h.poses = { pose };
        h.marginYd = margin;
        h.timing = timing;
        return h;
    }

    Solver::Timing InstantIn(std::int64_t ms)
    {
        return Solver::MakeInstant(kNow + Ms(ms), Ms(500));
    }

    float Dist(Vec3 const& a, Vec3 const& b)
    {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    // Is the plan's target clear of every hazard pose (beyond margin)?
    bool TargetClear(Input const& in, Plan const& plan)
    {
        for (Hazard const& h : in.hazards)
            for (Pose const& p : h.poses)
                if (Solver::SignedDistance(h.shape, p, plan.target) <= h.marginYd)
                    return false;
        return true;
    }

    bool IsMove(Plan const& p)
    {
        return p.outcome == Outcome::Move;
    }

    // ----------------------------------------------------------------------------------------------------------
    // SignedDistance
    // ----------------------------------------------------------------------------------------------------------

    void TestSignedDistanceAgreesWithContains()
    {
        Shape rear = Shape::Cone(90.f, 12.f);
        rear.facingOffsetDeg = 180.f;
        Shape excl = Shape::Line(20.f, 4.f);
        excl.excludeStart = 3.f;
        Shape back = Shape::Line(16.f, 6.f);
        back.facingOffsetDeg = 180.f;
        std::vector<std::pair<Shape, Pose>> const cases = {
            { Shape::Circle(8.f), Pose{ 0.f, 0.f, 0.f, 0.f } },
            { Shape::Donut(3.f, 9.f), Pose{ 10.f, -4.f, 0.f, 0.3f } },
            { Shape::Cone(60.f, 15.f), Pose{ -5.f, 2.f, 0.f, 1.1f } },
            { Shape::Cone(270.f, 10.f), Pose{ 0.f, 0.f, 0.f, -2.f } },
            { Shape::Cone(360.f, 10.f), Pose{ 0.f, 0.f, 0.f, 0.f } },
            { Shape::Cone(179.f, 10.f), Pose{ 0.f, 0.f, 0.f, 2.f } },
            { rear, Pose{ 3.f, 3.f, 0.f, 0.5f } },
            { Shape::Line(24.f, 4.f), Pose{ 0.f, 0.f, 0.f, 0.7f } },
            { excl, Pose{ 1.f, 1.f, 0.f, -1.f } },
            { back, Pose{ 0.f, 0.f, 0.f, 2.5f } },
        };
        Lcg rng;
        int disagreements = 0;
        int compared = 0;
        for (auto const& [shape, pose] : cases)
        {
            float const b = Geo::Bounds(shape) + 5.f;
            for (int i = 0; i < 10000; ++i)
            {
                Vec3 const p{ pose.x + rng.Range(-b, b), pose.y + rng.Range(-b, b), 0.f };
                float const sd = Solver::SignedDistance(shape, pose, p);
                if (std::fabs(sd) < 0.01f)
                    continue;
                ++compared;
                if ((sd < 0.f) != Geo::Contains(shape, pose, p))
                    ++disagreements;
            }
        }
        CHECK(compared > 90000);
        CHECK(disagreements == 0);

        // Another floor.
        CHECK(std::isinf(Solver::SignedDistance(Shape::Circle(5.f), Pose{}, Vec3{ 0.f, 0.f, 7.f })));
    }

    void TestSignedDistanceAnalytic()
    {
        Pose const o{};
        Shape const circle = Shape::Circle(8.f);
        CHECK_NEAR(Solver::SignedDistance(circle, o, { 3.f, 0.f, 0.f }), -5.f, 1e-4);
        CHECK_NEAR(Solver::SignedDistance(circle, o, { 0.f, -10.f, 0.f }), 2.f, 1e-4);

        Shape const donut = Shape::Donut(3.f, 9.f);
        CHECK_NEAR(Solver::SignedDistance(donut, o, { 0.f, 0.f, 0.f }), 3.f, 1e-4);
        CHECK_NEAR(Solver::SignedDistance(donut, o, { 2.f, 0.f, 0.f }), 1.f, 1e-4);
        CHECK_NEAR(Solver::SignedDistance(donut, o, { 5.f, 0.f, 0.f }), -2.f, 1e-4);
        CHECK_NEAR(Solver::SignedDistance(donut, o, { 0.f, 8.f, 0.f }), -1.f, 1e-4);
        CHECK_NEAR(Solver::SignedDistance(donut, o, { 12.f, 0.f, 0.f }), 3.f, 1e-4);

        Shape const cone = Shape::Cone(60.f, 15.f);
        CHECK_NEAR(Solver::SignedDistance(cone, o, { 5.f, 0.f, 0.f }), -2.5f, 1e-4);   // 5 sin 30
        CHECK_NEAR(Solver::SignedDistance(cone, o, { 14.f, 0.f, 0.f }), -1.f, 1e-4);
        CHECK_NEAR(Solver::SignedDistance(cone, o, { 20.f, 0.f, 0.f }), 5.f, 1e-4);
        CHECK_NEAR(Solver::SignedDistance(cone, o, { -3.f, 0.f, 0.f }), 3.f, 1e-4);
        CHECK_NEAR(Solver::SignedDistance(cone, o, { 0.f, 4.f, 0.f }), 4.f * std::cos(Geo::DegToRad(30.f)), 1e-4);

        Shape line = Shape::Line(20.f, 4.f);
        line.excludeStart = 3.f;
        CHECK_NEAR(Solver::SignedDistance(line, o, { 10.f, 0.f, 0.f }), -2.f, 1e-4);
        CHECK_NEAR(Solver::SignedDistance(line, o, { 10.f, 5.f, 0.f }), 3.f, 1e-4);
        CHECK_NEAR(Solver::SignedDistance(line, o, { 1.f, 0.f, 0.f }), 2.f, 1e-4);
        CHECK_NEAR(Solver::SignedDistance(line, o, { 25.f, 0.f, 0.f }), 5.f, 1e-4);
        CHECK_NEAR(Solver::SignedDistance(line, o, { 23.f, 5.f, 0.f }), std::hypot(3.f, 3.f), 1e-4);
        CHECK_NEAR(Solver::SignedDistance(line, o, { 19.f, 0.5f, 0.f }), -1.f, 1e-4);
    }

    // ----------------------------------------------------------------------------------------------------------
    // Single shapes
    // ----------------------------------------------------------------------------------------------------------

    void TestCircleAtFeet()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(5.f), Pose{}, InstantIn(2000)));
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(Dist(plan.target, Vec3{}) >= 5.f + kMargin);
        CHECK(plan.arriveAt < kNow + Ms(2000) - in.params.marginMs);
        CHECK(plan.answers.size() == 1 && plan.answers[0] == 1);
        CHECK(plan.holdUntil >= kNow + Ms(2500));
        CHECK(plan.path.size() >= 2 && Dist(plan.path.front(), in.bot) < 0.01f);

        // Off-centre: leaves radially (the nearest way out).
        in.bot = { 3.f, 0.f, 0.f };
        Plan const p2 = Solver::Solve(in, oracle);
        CHECK(IsMove(p2));
        CHECK(p2.target.x > 5.f && std::fabs(p2.target.y) < 1.f);
    }

    void TestDonut()
    {
        StraightOracle oracle;
        Input in = MakeInput({ 5.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Donut(4.f, 9.f), Pose{}, InstantIn(2000)));
        Plan const hole = Solver::Solve(in, oracle);
        CHECK(IsMove(hole));
        CHECK(Dist(hole.target, Vec3{}) <= 4.f - kMargin);     // into the hole: 2.25 yd vs 5.25 yd out
        CHECK(TargetClear(in, hole));

        in.bot = { 0.f, 8.f, 0.f };
        Plan const out = Solver::Solve(in, oracle);
        CHECK(IsMove(out));
        CHECK(Dist(out.target, Vec3{}) >= 9.f + kMargin);      // out: 1.75 yd vs 4.75 yd in
    }

    void TestConeSideways()
    {
        StraightOracle oracle;
        Input in = MakeInput({ 4.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Cone(90.f, 15.f), Pose{}, InstantIn(2000)));
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(TargetClear(in, plan));
        CHECK(std::fabs(plan.target.y) > 2.f && plan.target.x < 15.f);      // sideways, not past the arc
        CHECK(Dist(plan.target, in.bot) < 5.f);

        // Rear cone: points backwards from a pose facing +x.
        Shape rear = Shape::Cone(90.f, 15.f);
        rear.facingOffsetDeg = 180.f;
        Input r = MakeInput({ -4.f, 0.f, 0.f });
        r.hazards.push_back(MakeHazard(2, rear, Pose{}, InstantIn(2000)));
        Plan const rp = Solver::Solve(r, oracle);
        CHECK(IsMove(rp));
        CHECK(TargetClear(r, rp));
        CHECK(std::fabs(rp.target.y) > 2.f && rp.target.x > -15.f);
        CHECK(Dist(rp.target, r.bot) < 5.f);
    }

    void TestLineExcludedEnd()
    {
        StraightOracle oracle;
        Shape line = Shape::Line(24.f, 4.f);
        line.excludeStart = 4.f;
        Input in = MakeInput({ 2.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, line, Pose{}, InstantIn(2000)));
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(plan.outcome == Outcome::Safe);
        CHECK(oracle.calls == 0);

        // Inside the hot part: steps sideways.
        in.bot = { 12.f, 0.5f, 0.f };
        Plan const p2 = Solver::Solve(in, oracle);
        CHECK(IsMove(p2));
        CHECK(std::fabs(p2.target.y) >= 2.f + kMargin);
    }

    // ----------------------------------------------------------------------------------------------------------
    // Lanes
    // ----------------------------------------------------------------------------------------------------------

    void AddLanes(Input& in, Frame const& f, std::vector<LaneRect> const& rects, Solver::Timing const& t,
        std::uint32_t firstId)
    {
        Solver::LaneFrame lf;
        lf.frame = f;
        lf.rects = rects;
        for (LaneRect const& r : rects)
        {
            LanePlacement const lp = PlaceLane(f, r);
            in.hazards.push_back(MakeHazard(firstId++, Shape::Line(lp.length, lp.width), lp.pose, t));
        }
        in.lanes.push_back(lf);
    }

    void FrameCoords(Frame const& f, Vec3 const& p, float& fx, float& fy)
    {
        float const c = std::cos(f.angle);
        float const s = std::sin(f.angle);
        fx = c * (p.x - f.x) + s * (p.y - f.y);
        fy = -s * (p.x - f.x) + c * (p.y - f.y);
    }

    Vec3 World(Frame const& f, float fx, float fy)
    {
        Pose const p = FramePoint(f, fx, fy, 0.f);
        return { p.x, p.y, f.z };
    }

    void TestLanesGap()
    {
        Frame const f{ 0.f, 0.f, 0.f, 0.f, 40.f, 30.f };
        Input in = MakeInput(World(f, 20.f, 15.f));
        AddLanes(in, f, { { Axis::X, 0.f, 6.f }, { Axis::X, 12.f, 6.f }, { Axis::X, 24.f, 6.f } }, InstantIn(2500), 1);
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        float fx = 0.f;
        float fy = 0.f;
        FrameCoords(f, plan.target, fx, fy);
        bool const inGap = (fy >= 6.f + kMargin && fy <= 12.f - kMargin)
            || (fy >= 18.f + kMargin && fy <= 24.f - kMargin);
        CHECK(inGap);
    }

    void TestCrossfire()
    {
        Frame const f{ 100.f, 50.f, 0.f, 0.5f, 40.f, 30.f };
        std::vector<LaneRect> const xs = { { Axis::X, 2.f, 4.f }, { Axis::X, 10.f, 4.f }, { Axis::X, 18.f, 4.f },
            { Axis::X, 25.f, 4.f } };
        std::vector<LaneRect> const ys = { { Axis::Y, 8.f, 4.f }, { Axis::Y, 26.f, 4.f } };
        std::vector<LaneRect> all = xs;
        all.insert(all.end(), ys.begin(), ys.end());
        Input in = MakeInput(World(f, 10.f, 12.f));        // on the crossing of X lane [10, 14] and Y lane [8, 12]
        AddLanes(in, f, all, InstantIn(2500), 1);
        // Burning crossings (persistent cells).
        std::uint32_t id = 100;
        for (LaneRect const& x : xs)
            for (LaneRect const& y : ys)
            {
                LanePlacement const lp = PlaceCrossing(f, x, y);
                in.hazards.push_back(MakeHazard(id++, Shape::Line(lp.length, lp.width), lp.pose,
                    Solver::MakeInterval(kNow + Ms(2500), kNow + Ms(8500))));
            }
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(TargetClear(in, plan));
        CHECK(Solver::PointSafe(in, plan.target, plan.arriveAt, kNow + in.params.horizon));
        float fx = 0.f;
        float fy = 0.f;
        FrameCoords(f, plan.target, fx, fy);
        CHECK(fx >= -0.01f && fx <= 40.01f && fy >= -0.01f && fy <= 30.01f);
    }

    void TestOneSafeLane()
    {
        Frame const f{ 0.f, 0.f, 0.f, 0.f, 60.f, 48.f };
        std::vector<LaneRect> lanes;
        for (int i = 0; i < 7; ++i)
            lanes.push_back({ Axis::X, 6.f * i, 6.f });
        Input in = MakeInput(World(f, 30.f, 21.f));        // lane 3; the free lane is [42, 48], 21+ yd away
        AddLanes(in, f, lanes, InstantIn(4000), 1);
        in.lanes[0].freeLanes.push_back({ Axis::X, 42.f, 6.f });
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        float fx = 0.f;
        float fy = 0.f;
        FrameCoords(f, plan.target, fx, fy);
        CHECK(fy >= 42.f + kMargin && fy <= 48.f);

        // Without the frame candidates (bearings and exits alone) nothing clean is found.
        Input bare = in;
        bare.lanes.clear();
        Plan const p2 = Solver::Solve(bare, oracle);
        CHECK(p2.outcome != Outcome::Move && p2.outcome != Outcome::GraceMove);

        // Only the free-lane centreline (no rects, no grid reach): still found.
        Input freeOnly = in;
        freeOnly.lanes[0].rects.clear();
        freeOnly.params.gridStepYd = 1000.f;
        Plan const p3 = Solver::Solve(freeOnly, oracle);
        CHECK(IsMove(p3));
    }

    // ----------------------------------------------------------------------------------------------------------
    // Projectiles
    // ----------------------------------------------------------------------------------------------------------

    ProjectileParams Bolt(bool outAndBack)
    {
        ProjectileParams p;
        p.length = 30.f;
        p.speed = 10.f;
        p.halfLength = 1.5f;
        p.outAndBack = outAndBack;
        p.pause = Ms(500);
        return p;
    }

    void TestProjectile()
    {
        // Launched 2 s ago: the body is at s = 20.
        Input in = MakeInput({ 5.f, -5.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Line(30.f, 3.f), Pose{},
            Solver::MakeProjectile(Bolt(false), kNow - Ms(2000))));
        Solver::PathCheck const behind = Solver::CheckPath(in, { { 5.f, -5.f, 0.f }, { 5.f, 5.f, 0.f } }, kNow, false);
        CHECK(behind.ok);
        Solver::PathCheck const ahead = Solver::CheckPath(in, { { 28.f, -5.f, 0.f }, { 28.f, 5.f, 0.f } }, kNow, false);
        CHECK(!ahead.ok);
        CHECK(ahead.firstHit == 1);

        // The windows equal ProjectileHotWindows at the point's s, widened by the margin along the line.
        std::vector<HotWindow> const w = Solver::WindowsAt(in.hazards[0], { 12.f, 0.f, 0.f });
        CHECK(w.size() == 1);
        if (w.size() == 1)
        {
            ProjectileParams wide = Bolt(false);
            wide.halfLength += kMargin;
            std::vector<HotWindow> const ref = ProjectileHotWindows(wide, 12.f);
            CHECK(w[0].from == kNow - Ms(2000) + ref[0].from);
            CHECK(w[0].to == kNow - Ms(2000) + ref[0].to);
        }
        CHECK(Solver::WindowsAt(in.hazards[0], { 12.f, 5.f, 0.f }).empty());

        // Out and back from now: a point at s = 10 is hot on the way out (0.78-1.23 s) and back (5.28-5.73 s).
        Input ob = MakeInput({ 10.f, 0.f, 0.f });
        ob.hazards.push_back(MakeHazard(2, Shape::Line(30.f, 3.f), Pose{},
            Solver::MakeProjectile(Bolt(true), kNow)));
        Vec3 const p{ 10.f, 0.f, 0.f };
        CHECK(Solver::WindowsAt(ob.hazards[0], p).size() == 2);
        CHECK(Solver::PointSafe(ob, p, kNow + Ms(3000), kNow + Ms(3000)));      // between the passes
        CHECK(!Solver::PointSafe(ob, p, kNow + Ms(5500), kNow + Ms(5500)));
        CHECK(!Solver::PointSafe(ob, p, kNow + Ms(3000), kNow + Ms(10000)));
        CHECK(Solver::PointSafe(ob, p, kNow + Ms(6100), kNow + Ms(10000)));     // after both
        StraightOracle oracle;
        Plan const plan = Solver::Solve(ob, oracle);
        CHECK(IsMove(plan));
        CHECK(TargetClear(ob, plan));
    }

    void TestStartDelay()
    {
        // A circle that resolves 1.2 s from now on the bot's path: walking 6 yd out of it takes ~0.86 s.
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(5.f), Pose{}, InstantIn(1600)));
        std::vector<Vec3> const path = { { 0.f, 0.f, 0.f }, { 7.f, 0.f, 0.f } };
        in.params.startDelay = Ms(0);
        Solver::PathCheck const quick = Solver::CheckPath(in, path, kNow, false);
        CHECK(quick.ok);
        CHECK(quick.arriveAt == kNow + Ms(1000));
        in.params.startDelay = Ms(600);
        Solver::PathCheck const slow = Solver::CheckPath(in, path, kNow, false);
        CHECK(!slow.ok);
        in.params.startDelay = Ms(150);
        CHECK(Solver::CheckPath(in, path, kNow, false).arriveAt == kNow + Ms(1150));
    }

    // ----------------------------------------------------------------------------------------------------------
    // Mines
    // ----------------------------------------------------------------------------------------------------------

    Solver::Timing ArmedSince(std::int64_t agoMs, std::int64_t graceMs)
    {
        return Solver::MakeMineArmed(kNow - Ms(agoMs), Solver::kForever, Ms(graceMs));
    }

    void TestMineArmingUnderBot()
    {
        Input in = MakeInput({ 0.5f, 0.f, 0.f });
        Ms const armAt = kNow + Ms(3000);
        in.hazards.push_back(MakeHazard(1, Shape::Circle(3.f), Pose{},
            Solver::MakeMineArming(armAt, Solver::kForever, Ms(500))));
        CHECK(Solver::WindowsAt(in.hazards[0], in.bot).size() == 2);
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(plan.arriveAt < armAt - in.params.marginMs);
        CHECK(Dist(plan.target, Vec3{}) >= 3.f + kMargin);
        // Lasting hazard: the hold is capped.
        CHECK(plan.holdUntil <= kNow + in.params.holdCap);
    }

    void AddMineRing(Input& in, float ringRadius, std::uint32_t count, float skipBearing, std::uint32_t firstId)
    {
        for (std::uint32_t i = 0; i < count; ++i)
        {
            float const b = Geo::kTwoPi * i / count;
            if (skipBearing >= 0.f && std::fabs(Geo::WrapPi(b - skipBearing)) < 0.5f)
                continue;
            Pose const p{ ringRadius * std::cos(b), ringRadius * std::sin(b), 0.f, 0.f };
            in.hazards.push_back(MakeHazard(firstId + i, Shape::Circle(2.f), p, ArmedSince(5000, 1500)));
        }
    }

    bool PathTouches(Input const& in, std::vector<Vec3> const& path, std::uint32_t minId)
    {
        for (std::size_t k = 1; k < path.size(); ++k)
            for (int j = 0; j <= 100; ++j)
            {
                float const u = j / 100.f;
                Vec3 const p{ path[k - 1].x + (path[k].x - path[k - 1].x) * u,
                    path[k - 1].y + (path[k].y - path[k - 1].y) * u, 0.f };
                for (Hazard const& h : in.hazards)
                    if (h.id >= minId && Solver::SignedDistance(h.shape, h.poses[0], p) <= 0.f)
                        return true;
            }
        return false;
    }

    void TestMineFieldWithGap()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(5.f), Pose{}, InstantIn(3000)));
        AddMineRing(in, 7.f, 16, 3.f * Geo::kPi / 4.f, 100);    // gap of three mines around 135 degrees
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(TargetClear(in, plan));
        CHECK(!PathTouches(in, plan.path, 100));
        float const bearing = std::atan2(plan.target.y, plan.target.x);
        CHECK(std::fabs(Geo::WrapPi(bearing - 3.f * Geo::kPi / 4.f)) < 0.5f);
    }

    void TestMineFieldGrace()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(4.f), Pose{}, InstantIn(3000)));
        AddMineRing(in, 7.f, 16, -1.f, 100);                    // closed ring
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(plan.outcome == Outcome::GraceMove);
        CHECK(TargetClear(in, plan));
        CHECK(!Solver::CheckPath(in, plan.path, kNow, false).ok);
        Solver::PathCheck const pc = Solver::CheckPath(in, plan.path, kNow, true);
        CHECK(pc.ok);
        CHECK(pc.timeInsideHot > Ms(0));
        CHECK(pc.timeInsideHot <= Ms(2 * (1500 - 250)));        // at most two overlapping mines, each under budget
        CHECK(oracle.calls <= 2 * in.params.maxPathChecks);

        // Grace too short for the crossing: not a GraceMove.
        Input tight = in;
        for (Hazard& h : tight.hazards)
            if (h.id >= 100)
                h.timing.grace = Ms(400);
        Plan const p2 = Solver::Solve(tight, oracle);
        CHECK(p2.outcome == Outcome::Fallback);
    }

    void TestInsideArmedMine()
    {
        Input in = MakeInput({ 0.5f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(3.f), Pose{}, ArmedSince(5000, 500)));
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(TargetClear(in, plan));
        Solver::PathCheck const pc = Solver::CheckPath(in, plan.path, kNow, false);
        CHECK(pc.ok);
        CHECK(pc.timeInsideHot > Ms(0));                        // the start-hazard exemption, not a clean path
        CHECK(Solver::LeaveBy(in, in.bot) <= kNow);
    }

    // Deepest signed distance to hazard h along a path (densified every 0.1 yd).
    float DeepestAlong(Hazard const& h, std::vector<Vec3> const& path)
    {
        float deepest = 1e9f;
        for (std::size_t k = 0; k < path.size(); ++k)
        {
            Vec3 const& a = k ? path[k - 1] : path[k];
            Vec3 const& b = path[k];
            int const steps = std::max(1, static_cast<int>(std::ceil(Dist(a, b) / 0.1f)));
            for (int j = 0; j <= steps; ++j)
            {
                float const u = static_cast<float>(j) / steps;
                Vec3 const p{ a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u };
                for (Pose const& pose : h.poses)
                    deepest = std::min(deepest, Solver::SignedDistance(h.shape, pose, p));
            }
        }
        return deepest;
    }

    // STAGE-M2-REVIEW B1: the start-hazard exemption covers a way out only, never a leg across or into the region,
    // however much the role anchor on the far side pulls.
    void TestStartHazardExitOnly()
    {
        auto withRole = [](Vec3 bot, float anchorX)
        {
            Input in = MakeInput(bot);
            in.role.present = true;
            in.role.anchor = { anchorX, 0.f, 0.f };
            in.role.minYd = 0.f;
            in.role.maxYd = 3.f;
            return in;
        };
        StraightOracle oracle;

        // Case 1: 0.5 yd outside a burning circle (inside the margin), the role anchor beyond it.
        {
            Input in = withRole({ 6.5f, 0.f, 0.f }, -9.f);
            in.hazards.push_back(MakeHazard(1, Shape::Circle(6.f), Pose{},
                Solver::MakeInterval(kNow - Ms(1000), kNow + Ms(20000))));
            CHECK(!Solver::CheckPath(in, { in.bot, { -7.9f, -1.1f, 0.f } }, kNow, false).ok);
            CHECK(!Solver::CheckPath(in, { in.bot, { -7.9f, -1.1f, 0.f } }, kNow, true).ok);
            Plan const plan = Solver::Solve(in, oracle);
            CHECK(IsMove(plan));
            CHECK(TargetClear(in, plan));
            CHECK(DeepestAlong(in.hazards[0], plan.path) >= 0.5f - 0.06f);
        }

        // Case 2: knocked into the circle: out by the near side, not through the rest of it.
        {
            Input in = withRole({ 5.f, 0.f, 0.f }, -9.f);
            in.hazards.push_back(MakeHazard(1, Shape::Circle(6.f), Pose{},
                Solver::MakeInterval(kNow - Ms(1000), kNow + Ms(20000))));
            CHECK(!Solver::CheckPath(in, { in.bot, { -7.9f, 0.f, 0.f } }, kNow, false).ok);
            CHECK(Solver::CheckPath(in, { in.bot, { 7.5f, 0.f, 0.f } }, kNow, false).ok);
            Plan const plan = Solver::Solve(in, oracle);
            CHECK(IsMove(plan));
            CHECK(TargetClear(in, plan));
            CHECK(plan.target.x > 0.f);
            CHECK(DeepestAlong(in.hazards[0], plan.path) >= -1.f - 0.06f);
        }

        // Case 3: inside an armed mine, the role anchor on the far side: never across the mine.
        {
            Input in = withRole({ 2.5f, 0.f, 0.f }, -6.f);
            in.hazards.push_back(MakeHazard(1, Shape::Circle(3.f), Pose{}, ArmedSince(5000, 500)));
            CHECK(!Solver::CheckPath(in, { in.bot, { -4.5f, 0.f, 0.f } }, kNow, false).ok);
            CHECK(!Solver::CheckPath(in, { in.bot, { -4.5f, 0.f, 0.f } }, kNow, true).ok);
            Plan const plan = Solver::Solve(in, oracle);
            CHECK(IsMove(plan));
            CHECK(TargetClear(in, plan));
            CHECK(DeepestAlong(in.hazards[0], plan.path) >= -0.5f - 0.06f);
        }

        // A walk along a hot line that never gets deeper is still capped at the straight exit time.
        {
            Input in = MakeInput({ 5.f, 1.5f, 0.f });
            in.hazards.push_back(MakeHazard(1, Shape::Line(30.f, 4.f), Pose{},
                Solver::MakeInterval(kNow - Ms(1000), kNow + Ms(20000))));
            CHECK(!Solver::CheckPath(in, { in.bot, { 25.f, 1.5f, 0.f } }, kNow, false).ok);
            CHECK(Solver::CheckPath(in, { in.bot, { 5.f, 3.5f, 0.f } }, kNow, false).ok);
        }

        // STAGE-M2-REVIEW m1: a mine whose arming update is late is still strict (no grace) up to strictTo.
        {
            Input in = MakeInput({ 4.f, 0.f, 0.f });
            Solver::Timing t = Solver::MakeMineArming(kNow - Ms(200), Solver::kForever, Ms(1500));
            t.strictTo = kNow;
            in.hazards.push_back(MakeHazard(1, Shape::Circle(2.f), Pose{}, t));
            CHECK(!Solver::CheckPath(in, { in.bot, { -4.f, 0.f, 0.f } }, kNow, true).ok);
            Solver::Timing armed = Solver::MakeMineArmed(kNow - Ms(200), Solver::kForever, Ms(1500));
            in.hazards[0].timing = armed;
            CHECK(Solver::CheckPath(in, { in.bot, { -4.f, 0.f, 0.f } }, kNow, true).ok);
        }

        // The Fallback ranks by the whole time inside: with nothing clean anywhere it still leaves by the near side.
        {
            Input in = MakeInput({ 5.f, 0.f, 0.f });
            in.hazards.push_back(MakeHazard(1, Shape::Circle(6.f), Pose{},
                Solver::MakeInterval(kNow - Ms(1000), kNow + Ms(20000))));
            in.hazards.push_back(MakeHazard(2, Shape::Circle(40.f), Pose{}, InstantIn(500)));
            NoPathOracle none;
            Plan const plan = Solver::Solve(in, none);
            CHECK(plan.outcome == Outcome::NoPath);
            CHECK(plan.target.x > 0.f);
        }
    }

    // ----------------------------------------------------------------------------------------------------------
    // Persistent, moving and turning
    // ----------------------------------------------------------------------------------------------------------

    void TestPersistent()
    {
        Input in = MakeInput({ 2.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(6.f), Pose{},
            Solver::MakeInterval(kNow - Ms(1000), kNow + Ms(5000))));
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        // The shortest way out (+x); the edge term may push it up to a yard past the exit point.
        CHECK(Dist(plan.target, in.bot) >= 6.f + kMargin - 2.f);
        CHECK(Dist(plan.target, in.bot) <= 6.f + kMargin + 0.5f - 2.f + 1.f);
        CHECK(plan.target.x > 6.f + kMargin && std::fabs(plan.target.y) < 1.5f);
        CHECK(plan.holdUntil <= kNow + in.params.holdCap);
        CHECK(plan.holdUntil >= plan.arriveAt);

        // A later window on the shortest exit: the target avoids it.
        in.hazards.push_back(MakeHazard(2, Shape::Circle(3.f), Pose{ 7.5f, 0.f, 0.f, 0.f }, InstantIn(3000)));
        Plan const p2 = Solver::Solve(in, oracle);
        CHECK(IsMove(p2));
        CHECK(TargetClear(in, p2));
        CHECK(Solver::PointSafe(in, p2.target, p2.arriveAt, kNow + in.params.horizon));
    }

    // STAGE-M2-REVIEW m2: a follow motion across a lasting zone is re-routed around it, not only stopped.
    void TestReroute()
    {
        Input in = MakeInput({ -10.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(4.f), Pose{}, Solver::MakeInterval(kNow - Ms(1000),
            Solver::kForever)));
        Vec3 const end{ 10.f, 0.f, 0.f };
        in.role.present = true;
        in.role.anchor = end;
        in.role.minYd = 0.f;
        in.role.maxYd = 3.f;
        StraightOracle oracle;
        CHECK(Solver::Solve(in, oracle).outcome == Outcome::Safe);     // the spot is safe: no reroute asked
        in.reroute = true;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(TargetClear(in, plan));
        CHECK(Dist(plan.target, end) < Dist(in.bot, end) - 1.f);
        CHECK(DeepestAlong(in.hazards[0], plan.path) > kMargin);
        CHECK(Solver::CheckPath(in, plan.path, kNow, false).ok);

        // Walled in: no clean way, no fallback (Safe = keep still).
        NoPathOracle none;
        CHECK(Solver::Solve(in, none).outcome == Outcome::Safe);
    }

    void TestMovingAnchorMargin()
    {
        Input in = MakeInput({ 1.f, 0.f, 0.f });
        float const drift = 3.f;
        in.hazards.push_back(MakeHazard(1, Shape::Circle(4.f), Pose{}, InstantIn(3000), kMargin + drift));
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(Dist(plan.target, Vec3{}) >= 4.f + kMargin + drift);
    }

    void TestTurningCone()
    {
        float const slack = Geo::DegToRad(15.f);
        Hazard h = MakeHazard(1, Shape::Cone(60.f, 15.f), Pose{}, InstantIn(2500));
        h.poses = { Pose{ 0.f, 0.f, 0.f, -slack }, Pose{ 0.f, 0.f, 0.f, 0.f }, Pose{ 0.f, 0.f, 0.f, slack } };
        float const b = Geo::DegToRad(35.f);                    // outside the 0 pose, inside the +15 one
        Input in = MakeInput({ 6.f * std::cos(b), 6.f * std::sin(b), 0.f });
        in.hazards.push_back(h);
        CHECK(!Solver::PointSafe(in, in.bot, kNow, kNow + Ms(10000)));
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(TargetClear(in, plan));
    }

    // ----------------------------------------------------------------------------------------------------------
    // Degenerate input, determinism and caps
    // ----------------------------------------------------------------------------------------------------------

    void TestInfeasible()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(20.f), Pose{}, InstantIn(1000)));
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(plan.outcome == Outcome::Fallback);
        CHECK(!plan.path.empty());
        CHECK(plan.answers.size() == 1);

        NoPathOracle none;
        Plan const p2 = Solver::Solve(in, none);
        CHECK(p2.outcome == Outcome::NoPath);
        CHECK(p2.path.size() == 2);
        CHECK(Dist(p2.target, Vec3{}) >= 20.f + kMargin);
        CHECK(none.calls <= 2 * in.params.maxPathChecks + 1);

        Input empty = MakeInput({ 1.f, 2.f, 3.f });
        Plan const p3 = Solver::Solve(empty, oracle);
        CHECK(p3.outcome == Outcome::Safe);
        CHECK(Solver::LeaveBy(empty, empty.bot) == Solver::kForever);
        CHECK(Solver::Candidates(empty).size() >= 1);
    }

    Input Barrage(std::uint64_t seed, std::uint32_t hazards)
    {
        Lcg rng;
        rng.state = seed;
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        for (std::uint32_t i = 0; i < hazards; ++i)
        {
            Pose const p{ rng.Range(-25.f, 25.f), rng.Range(-25.f, 25.f), 0.f, rng.Range(-3.f, 3.f) };
            Solver::Timing const t = InstantIn(static_cast<std::int64_t>(rng.Range(4000.f, 8000.f)));
            switch (i % 4)
            {
                case 0:
                    in.hazards.push_back(MakeHazard(i + 1, Shape::Circle(rng.Range(3.f, 6.f)), p, t));
                    break;
                case 1:
                    in.hazards.push_back(MakeHazard(i + 1, Shape::Line(24.f, 4.f), p, t));
                    break;
                case 2:
                    in.hazards.push_back(MakeHazard(i + 1, Shape::Cone(60.f, 15.f), p, t));
                    break;
                default:
                    in.hazards.push_back(MakeHazard(i + 1, Shape::Donut(3.f, 8.f), p, t));
                    break;
            }
        }
        // One circle on the bot so a solve is needed.
        in.hazards.push_back(MakeHazard(999, Shape::Circle(5.f), Pose{ 1.f, 1.f, 0.f, 0.f }, InstantIn(4000)));
        in.role.present = true;
        in.role.anchor = { 10.f, 0.f, 0.f };
        in.role.minYd = 8.f;
        in.role.maxYd = 28.f;
        in.crowd = { { -6.f, 0.f, 0.f } };
        Solver::LaneFrame lf;
        lf.frame = Frame{ -30.f, -30.f, 0.f, 0.f, 60.f, 60.f };
        in.lanes.push_back(lf);
        return in;
    }

    void TestDeterminismAndCaps()
    {
        Input const in = Barrage(42, 30);
        StraightOracle o1;
        StraightOracle o2;
        Plan const a = Solver::Solve(in, o1);
        Plan const b = Solver::Solve(in, o2);
        CHECK(a.outcome == b.outcome);
        CHECK(a.target.x == b.target.x && a.target.y == b.target.y);
        CHECK(a.score == b.score);
        CHECK(a.path.size() == b.path.size());
        CHECK(a.answers == b.answers);
        CHECK(a.candidates == b.candidates);
        CHECK(o1.calls == o2.calls);

        CHECK(a.candidates <= in.params.maxCandidates);
        Input capped = in;
        capped.params.maxCandidates = 50;
        std::vector<Vec3> const c = Solver::Candidates(capped);
        CHECK(c.size() == 50);
        CHECK(c[0].x == in.bot.x && c[0].y == in.bot.y);
        for (std::size_t i = 1; i < c.size(); ++i)
            CHECK(Dist(c[i], in.bot) >= Dist(c[i - 1], in.bot) - 1e-4f);

        // Every path rejected: the oracle is asked at most maxPathChecks per pass (three passes) plus the fallback.
        Input few = in;
        few.params.maxPathChecks = 3;
        NoPathOracle none;
        Plan const p = Solver::Solve(few, none);
        CHECK(p.outcome == Outcome::NoPath);
        CHECK(none.calls <= 3 * 3 + 1);
        CHECK(p.pathChecks == none.calls);

        // A wall that blocks the best directions: the solve still finds a validated path elsewhere.
        WallOracle wall;
        wall.walls.push_back({ { 6.5f, -40.f, 0.f }, { 6.5f, 40.f, 0.f } });
        Input w = MakeInput({ 4.f, 0.f, 0.f });
        w.hazards.push_back(MakeHazard(1, Shape::Circle(5.f), Pose{ 2.f, 0.f, 0.f, 0.f }, InstantIn(2500)));
        Plan const wp = Solver::Solve(w, wall);
        CHECK(IsMove(wp));
        CHECK(wp.target.x < 6.5f);
        CHECK(TargetClear(w, wp));
    }

    // ----------------------------------------------------------------------------------------------------------
    // LeaveBy, CastAllowed
    // ----------------------------------------------------------------------------------------------------------

    void TestLeaveByAndCast()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        CHECK(Solver::LeaveBy(in, in.bot) == Solver::kForever);
        in.hazards.push_back(MakeHazard(1, Shape::Circle(5.f), Pose{}, InstantIn(3000)));
        Ms const lb = Solver::LeaveBy(in, in.bot);
        // window start − marginMs − startDelay − time to walk 5 + 0.75 + 0.5 yd.
        Ms const walk{ static_cast<std::int64_t>(std::lround(6.25f / 7.f * 1000.f)) };
        Ms const expected = kNow + Ms(3000) - Ms(300) - Ms(150) - walk;
        CHECK(lb == expected);
        CHECK(Solver::LeaveBy(in, { 20.f, 0.f, 0.f }) == Solver::kForever);
        // At the edge the escape is short: later leaveBy.
        CHECK(Solver::LeaveBy(in, { 4.9f, 0.f, 0.f }) > lb);
        // Window over (past the hold + margin): nothing to leave.
        Input late = in;
        late.now = kNow + Ms(3000 + 500 + 301);
        CHECK(Solver::LeaveBy(late, late.bot) == Solver::kForever);
        // Window open now: leave now.
        Input open = in;
        open.now = kNow + Ms(3100);
        CHECK(Solver::LeaveBy(open, open.bot) <= open.now);

        Ms const m{ 300 };
        CHECK(Solver::CastAllowed(kNow, Ms(2500), Solver::kForever, m));
        CHECK(Solver::CastAllowed(kNow, Ms(0), Solver::kForever, m));
        CHECK(Solver::CastAllowed(kNow, Ms(1000), kNow + Ms(1300), m));     // ends exactly at leaveBy − margin
        CHECK(!Solver::CastAllowed(kNow, Ms(1001), kNow + Ms(1300), m));
        CHECK(!Solver::CastAllowed(kNow, Ms(0), kNow - Ms(1), m));          // already late
        CHECK(Solver::CastAllowed(kNow, Ms(0), kNow + m, m));               // instant at the limit
    }

    // ----------------------------------------------------------------------------------------------------------
    // Fallibility
    // ----------------------------------------------------------------------------------------------------------

    void TestFallibility()
    {
        using namespace Fallibility;
        Config c;
        c.enable = true;
        c.seed = 0xC0FFEEull;
        float const scale = ScaleFor(c, false);
        CHECK_NEAR(scale, 1.f, 1e-6);

        // Same seed, same rolls; another seed, other rolls.
        CHECK(Mix(1, 2, 3, 4) == Mix(1, 2, 3, 4));
        CHECK(Mix(1, 2, 3, 4) != Mix(2, 2, 3, 4));
        CHECK(Roll01(7, 8, 9, kSaltLate) == Roll01(7, 8, 9, kSaltLate));
        Perception const a = Perceive(c, scale, 77, 1234, Ms(1000), Ms(4000));
        Perception const b = Perceive(c, scale, 77, 1234, Ms(1000), Ms(4000));
        CHECK(a.visibleAt == b.visibleAt && a.late == b.late && a.wrongSide == b.wrongSide);

        // The jitter is a per-bot trait: the same for every mechanic (when not late).
        Ms const r1 = ReactionDelay(c, scale, 77);
        CHECK(r1 >= Ms(350) && r1 <= Ms(600));
        for (std::uint32_t m = 1; m < 50; ++m)
        {
            Perception const p = Perceive(c, scale, 77, m, Ms(0), Ms(5000));
            CHECK(p.visibleAt == r1 + (p.late ? Ms(700) : Ms(0)));
        }

        // 100,000 rolls within ±1% of the configured chance.
        int late = 0;
        int wrong = 0;
        double sum = 0.0;
        int const n = 100000;
        for (int i = 0; i < n; ++i)
        {
            std::uint64_t const bot = 1000 + static_cast<std::uint64_t>(i % 40);
            std::uint32_t const mech = static_cast<std::uint32_t>(i / 40 + 1);
            Perception const p = Perceive(c, scale, bot, mech, Ms(0), Ms(5000));
            late += p.late ? 1 : 0;
            wrong += p.wrongSide ? 1 : 0;
            sum += Roll01(c.seed, bot, mech, 99);
        }
        CHECK(std::fabs(late / static_cast<double>(n) - 0.05) < 0.01);
        CHECK(std::fabs(wrong / static_cast<double>(n) - 0.03) < 0.01);
        CHECK(std::fabs(sum / n - 0.5) < 0.01);

        // Wrong side: not seen until firstWindowStart − RealiseMs.
        Perception w;
        w.visibleAt = Ms(400);
        w.wrongSide = true;
        w.realiseAt = Ms(5000) - Ms(700);
        Ms until{ 0 };
        CHECK(!Sees(w, Ms(1000), &until));
        CHECK(until == Ms(4300));
        CHECK(Sees(w, Ms(4300)));

        // Disabled: no delay, no late, no wrong side.
        Config off = c;
        off.enable = false;
        float const zero = ScaleFor(off, true);
        CHECK(zero == 0.f);
        for (std::uint32_t m = 1; m < 1000; ++m)
        {
            Perception const p = Perceive(off, zero, 5, m, Ms(1234), Ms(3000));
            CHECK(p.visibleAt == Ms(1234) && !p.late && !p.wrongSide);
            CHECK(Sees(p, Ms(1234)));
        }

        // Scale doubles delays and chances.
        Config hc = c;
        hc.scaleHeroic = 2.f;
        float const hs = ScaleFor(hc, true);
        CHECK_NEAR(hs, 2.f, 1e-6);
        CHECK(ReactionDelay(hc, hs, 77).count() == std::lround(2.f * (350.f
            + 250.f * Roll01(c.seed, 77, 0, kSaltJitter))));
    }

    // ----------------------------------------------------------------------------------------------------------
    // M2 playtest fixes: walkability probe, path tiers, reduced-margin pass
    // ----------------------------------------------------------------------------------------------------------

    // A floor plan: walkable inside `rooms` (axis-aligned rectangles); `walls` are segments no straight path may
    // cross. Walkable() drops points off the plan, or snaps the ones within snapYd of a room onto it (the navmesh
    // probe); Find() fails for an end off the plan or a segment across a wall.
    struct FloorOracle : Solver::PathOracle
    {
        struct Rect
        {
            float x0, y0, x1, y1;
        };
        std::vector<Rect> rooms;
        std::vector<std::pair<Vec3, Vec3>> walls;
        bool probe = true;
        float snapYd = 0.f;
        std::uint32_t calls = 0, probes = 0;

        bool OnPlan(Vec3 const& p) const
        {
            for (Rect const& r : rooms)
                if (p.x >= r.x0 - 1e-3f && p.x <= r.x1 + 1e-3f && p.y >= r.y0 - 1e-3f && p.y <= r.y1 + 1e-3f)
                    return true;
            return false;
        }

        bool Walkable(Vec3& p) override
        {
            if (!probe)
                return true;
            ++probes;
            if (OnPlan(p))
                return true;
            float best = 1e9f;
            Vec3 snapped = p;
            for (Rect const& r : rooms)
            {
                Vec3 const q{ std::clamp(p.x, r.x0, r.x1), std::clamp(p.y, r.y0, r.y1), p.z };
                if (Dist(p, q) < best)
                {
                    best = Dist(p, q);
                    snapped = q;
                }
            }
            if (best > snapYd)
                return false;
            p = snapped;
            return true;
        }

        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            ++calls;
            if (!OnPlan(to))
                return false;
            for (auto const& [a, b] : walls)
                if (WallOracle::Intersects(from, to, a, b))
                    return false;
            out = { from, to };
            return true;
        }
    };

    // Indoors: the best-scored candidates (towards a role anchor behind a wall) lie inside the wall. The probe drops
    // them without a path check, so a walkable exit is found with the first path checks; without it the budget is
    // spent on the wall (the M2 playtest's fallbacks with 17 path checks).
    void TestProbeSkipsWalls()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(4.f), Pose{}, InstantIn(2000)));
        in.role.present = true;
        in.role.anchor = { 0.f, -10.f, 0.f };
        in.role.minYd = 0.f;
        in.role.maxYd = 3.f;
        FloorOracle floor;
        floor.rooms = { { -30.f, -1.f, 30.f, 30.f } };      // the wall is everything south of y = -1

        Plan const plan = Solver::Solve(in, floor);
        CHECK(IsMove(plan));
        CHECK(plan.stats.pass == 1);
        CHECK(TargetClear(in, plan));
        CHECK(floor.OnPlan(plan.target));
        CHECK(plan.stats.offMesh >= 8);
        CHECK(plan.pathChecks <= 2);
        CHECK(plan.stats.noPath == 0);
        CHECK(plan.stats.probes == floor.probes);

        // Without the probe the same solve spends its path checks on the wall first.
        FloorOracle blind = floor;
        blind.probe = false;
        blind.calls = 0;
        Plan const old = Solver::Solve(in, blind);
        CHECK(old.pathChecks > plan.pathChecks);
        CHECK(old.stats.noPath >= in.params.maxPathChecks);
        CHECK(old.stats.offMesh == 0);
    }

    // A candidate just off the floor is snapped onto it and checked again where it landed: a snap that puts it in a
    // hazard's margin rejects it (the plan never targets an unsafe spot), one that keeps it safe is used.
    void TestProbeSnap()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(4.f), Pose{ 0.f, 3.f, 0.f, 0.f }, InstantIn(2500)));
        FloorOracle floor;
        floor.rooms = { { -30.f, -1.75f, 30.f, 30.f } };    // the radial exit (0, -2.25) lies 0.5 yd in the wall
        floor.snapYd = 1.f;
        Plan const plan = Solver::Solve(in, floor);
        CHECK(IsMove(plan));
        CHECK(TargetClear(in, plan));
        CHECK(floor.OnPlan(plan.target));
        CHECK(plan.stats.snapped >= 1);
        CHECK(plan.stats.endUnsafe >= 1);                   // the snapped exit sits on the margin: rejected
        CHECK(Solver::CheckPath(in, plan.path, kNow, false).ok);
    }

    // The oracle's path may end away from the asked point (a navmesh end, an INCOMPLETE path): the solver validates
    // and scores that real end, never the asked one.
    void TestShiftedPathEnd()
    {
        struct ShiftOracle : Solver::PathOracle
        {
            float dx = 0.f, dy = 0.f;

            bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
            {
                out = { from, { to.x + dx, to.y + dy, to.z } };
                return true;
            }
        };
        Input in = MakeInput({ 1.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(4.f), Pose{}, InstantIn(3000)));
        ShiftOracle away;
        away.dx = 0.8f;
        Plan const plan = Solver::Solve(in, away);
        CHECK(IsMove(plan));
        CHECK(TargetClear(in, plan));
        CHECK(plan.target.x == plan.path.back().x && plan.target.y == plan.path.back().y);

        // Every end pulled 1 yd towards the centre: the nearest exits land inside the margin and are rejected.
        ShiftOracle in1;
        in1.dx = -1.f;
        Plan const p2 = Solver::Solve(in, in1);
        CHECK(p2.stats.endUnsafe >= 1);
        CHECK(p2.outcome != Outcome::Move || TargetClear(in, p2));
    }

    // Candidates whose straight line crosses a neighbouring hazard while it is hot are tried after the ones whose
    // straight line is clean, even when nothing holding the bot is hot yet (a barrage: the next shot lands across the
    // way to the role anchor). Before, the budget went to them and the solve fell back.
    void TestTiersUnderBarrage()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(4.f), Pose{}, InstantIn(2500)));
        // A strip x in [3, 7], |y| <= 30, hot from 600 to 1100 ms: the way east crosses it while it burns, the spots
        // beyond it are safe on arrival.
        Pose const strip{ 3.f, 0.f, 0.f, 0.f };
        in.hazards.push_back(MakeHazard(2, Shape::Line(4.f, 60.f), strip, InstantIn(600)));
        in.role.present = true;
        in.role.anchor = { 15.f, 0.f, 0.f };
        in.role.minYd = 0.f;
        in.role.maxYd = 3.f;
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(plan.stats.pass == 1);
        CHECK(plan.pathChecks <= 2);
        CHECK(TargetClear(in, plan));
        CHECK(Solver::CheckPath(in, plan.path, kNow, false).ok);
    }

    // Two armed mines with a 1.2 yd gap between their edges, the bot walled into a pocket below them with a zone on
    // it: no clean path at the full margin (the gap is narrower than two 0.75 yd margins), so the reduced-margin pass
    // takes the gap, the target beyond it is safe at the full margin, and the path never enters a mine.
    void MakePocket(Input& in, FloorOracle& floor, float gap)
    {
        in = MakeInput({ 0.f, -5.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(8.f), Pose{ 0.f, -5.f, 0.f, 0.f }, InstantIn(3000)));
        float const cx = 3.f + gap * 0.5f;
        in.hazards.push_back(MakeHazard(100, Shape::Circle(3.f), Pose{ -cx, 0.f, 0.f, 0.f }, ArmedSince(5000, 500)));
        in.hazards.push_back(MakeHazard(101, Shape::Circle(3.f), Pose{ cx, 0.f, 0.f, 0.f }, ArmedSince(5000, 500)));
        floor = FloorOracle();
        floor.rooms = { { -30.f, -10.f, 30.f, 30.f } };
        floor.walls = { { { -6.f, -10.f, 0.f }, { -6.f, 0.f, 0.f } }, { { 6.f, -10.f, 0.f }, { 6.f, 0.f, 0.f } },
            { { -6.f, -10.f, 0.f }, { 6.f, -10.f, 0.f } } };
        in.params.maxPathChecks = 16;   // the wall oracle has no probe for the far side of the walls
    }

    void TestReducedMarginGap()
    {
        Input in;
        FloorOracle floor;
        MakePocket(in, floor, 1.2f);
        Plan const plan = Solver::Solve(in, floor);
        CHECK(IsMove(plan));
        CHECK(plan.stats.pass == 2);
        CHECK(TargetClear(in, plan));
        CHECK(plan.target.y > 3.f);                         // beyond the mines
        CHECK(!PathTouches(in, plan.path, 100));            // never inside an armed mine
        CHECK(DeepestAlong(in.hazards[1], plan.path) >= 0.25f - 1e-3f);
        CHECK(DeepestAlong(in.hazards[2], plan.path) >= 0.25f - 1e-3f);
        CHECK(!Solver::CheckPath(in, plan.path, kNow, false).ok);                         // not clean at 0.75
        CHECK(Solver::CheckPath(Solver::ReducedMargins(in), plan.path, kNow, false).ok);  // clean at 0.25
        CHECK(plan.pathChecks <= 3 * in.params.maxPathChecks + 1);

        // A wide gap: the full margin is enough, the first pass takes it.
        MakePocket(in, floor, 2.5f);
        Plan const wide = Solver::Solve(in, floor);
        CHECK(IsMove(wide));
        CHECK(wide.stats.pass == 1);
        CHECK(!PathTouches(in, wide.path, 100));

        // A re-route (a following bot's motion crossing the mines, its spot safe) at the mouth of the gap (no clean
        // spot 1 yd closer to the motion's end is left on this side) slips through the same gap.
        MakePocket(in, floor, 1.2f);
        in.hazards.erase(in.hazards.begin());               // no zone on the bot
        in.bot = { 0.f, -1.6f, 0.f };
        in.reroute = true;
        in.role.present = true;
        in.role.anchor = { 0.f, 10.f, 0.f };
        in.role.minYd = 0.f;
        in.role.maxYd = 3.f;
        Plan const re = Solver::Solve(in, floor);
        CHECK(IsMove(re));
        CHECK(re.stats.pass == 2);
        CHECK(re.target.y > 3.f);
        CHECK(!PathTouches(in, re.path, 100));
    }

    // A true enclosure (gap 0.4 yd, narrower than two reduced margins; or a closed ring) still has no clean way out:
    // the reduced pass finds nothing, and the outcome is what it was before (grace crossing, fallback, or no plan for a
    // re-route).
    void TestTrueEnclosure()
    {
        Input in;
        FloorOracle floor;
        MakePocket(in, floor, 0.4f);
        Plan const plan = Solver::Solve(in, floor);
        CHECK(plan.outcome != Outcome::Move);
        CHECK(plan.stats.pass != 2);
        CHECK(plan.outcome == Outcome::Fallback || plan.outcome == Outcome::NoPath
            || plan.outcome == Outcome::GraceMove);

        // No grace either (grace <= GraceMarginMs): Fallback, as before.
        for (Hazard& h : in.hazards)
            if (h.id >= 100)
                h.timing.grace = Ms(200);
        Plan const none = Solver::Solve(in, floor);
        CHECK(none.outcome == Outcome::Fallback || none.outcome == Outcome::NoPath);

        // Closed ring of overlapping mines: grace crossing as before, never a reduced-margin Move.
        Input ring = MakeInput({ 0.f, 0.f, 0.f });
        ring.hazards.push_back(MakeHazard(1, Shape::Circle(4.f), Pose{}, InstantIn(3000)));
        AddMineRing(ring, 7.f, 16, -1.f, 100);
        StraightOracle oracle;
        Plan const g = Solver::Solve(ring, oracle);
        CHECK(g.outcome == Outcome::GraceMove);
        CHECK(g.stats.pass == 3);

        // A re-route boxed in by the ring, at its inner edge towards the motion's end: no plan, the bot holds.
        Input boxed = ring;
        boxed.hazards.erase(boxed.hazards.begin());
        boxed.bot = { 4.2f, 0.f, 0.f };
        boxed.reroute = true;
        boxed.role.present = true;
        boxed.role.anchor = { 15.f, 0.f, 0.f };
        boxed.role.maxYd = 3.f;
        for (Hazard& h : boxed.hazards)
            h.timing.grace = Ms(0);
        CHECK(Solver::Solve(boxed, oracle).outcome == Outcome::Safe);
    }

    // ----------------------------------------------------------------------------------------------------------
    // Cost
    // ----------------------------------------------------------------------------------------------------------

    void Benchmark()
    {
        Input const in = Barrage(7, 29);                         // 29 + the one on the bot = 30 hazards
        StraightOracle oracle;
        Plan const first = Solver::Solve(in, oracle);
        CHECK(in.hazards.size() == 30);
        CHECK(first.candidates == in.params.maxCandidates);
        int const runs = 200;
        auto const t0 = std::chrono::steady_clock::now();
        std::uint32_t sink = 0;
        double worst = 0.0;
        for (int i = 0; i < runs; ++i)
        {
            auto const s0 = std::chrono::steady_clock::now();
            Plan const p = Solver::Solve(in, oracle);
            auto const s1 = std::chrono::steady_clock::now();
            worst = std::max(worst, std::chrono::duration<double, std::micro>(s1 - s0).count());
            sink += p.candidates;
        }
        auto const t1 = std::chrono::steady_clock::now();
        double const mean = std::chrono::duration<double, std::micro>(t1 - t0).count() / runs;
        std::printf("solver cost: %u candidates x %zu hazards, outcome %d, path checks %u: mean %.0f us, max %.0f us "
            "(soft limit 2000 us)%s\n", first.candidates, in.hazards.size(), static_cast<int>(first.outcome),
            first.pathChecks, mean, worst, mean > 2000.0 ? "  ** OVER **" : "");
        CHECK(sink > 0);

        // Worst case: every oracle call fails, so both validation passes run dry and the fallback scans everything.
        NoPathOracle none;
        auto const w0 = std::chrono::steady_clock::now();
        Plan const worstPlan = Solver::Solve(in, none);
        auto const w1 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i)
            sink += Solver::Solve(in, none).candidates;
        auto const w2 = std::chrono::steady_clock::now();
        std::printf("solver cost, no path anywhere: outcome %d, oracle calls %u: first %.0f us, mean %.0f us\n",
            static_cast<int>(worstPlan.outcome), worstPlan.pathChecks,
            std::chrono::duration<double, std::micro>(w1 - w0).count(),
            std::chrono::duration<double, std::micro>(w2 - w1).count() / 20);
        CHECK(worstPlan.outcome == Outcome::NoPath);

        // Same, but the zone on the bot is already burning: the straight-line tiers of the validation are live.
        Input hot = in;
        hot.hazards.back().timing = Solver::MakeInterval(kNow - Ms(500), kNow + Ms(6000));
        auto h0 = std::chrono::steady_clock::now();
        Plan const hotPlan = Solver::Solve(hot, none);
        for (int i = 0; i < 20; ++i)
            sink += Solver::Solve(hot, none).candidates;
        auto const h1 = std::chrono::steady_clock::now();
        std::printf("solver cost, no path anywhere, bot in a burning zone: outcome %d, oracle calls %u: mean %.0f us\n",
            static_cast<int>(hotPlan.outcome), hotPlan.pathChecks,
            std::chrono::duration<double, std::micro>(h1 - h0).count() / 21);
        CHECK(hotPlan.outcome == Outcome::NoPath);
        StraightOracle straight;
        auto s0 = std::chrono::steady_clock::now();
        Plan const hotMove = Solver::Solve(hot, straight);
        auto const s1 = std::chrono::steady_clock::now();
        std::printf("solver cost, bot in a burning zone: outcome %d, oracle calls %u: %.0f us\n",
            static_cast<int>(hotMove.outcome), hotMove.pathChecks,
            std::chrono::duration<double, std::micro>(s1 - s0).count());

        // All three passes run dry: the barrage plus 30 armed mines (the grace pass is live), no path anywhere.
        Input mined = hot;
        Lcg mrng;
        mrng.state = 99;
        for (std::uint32_t i = 0; i < 30; ++i)
        {
            Pose const p{ mrng.Range(-20.f, 20.f), mrng.Range(-20.f, 20.f), 0.f, 0.f };
            mined.hazards.push_back(MakeHazard(2000 + i, Shape::Circle(3.f), p, ArmedSince(1000, 500)));
        }
        auto const m0 = std::chrono::steady_clock::now();
        Plan const minedPlan = Solver::Solve(mined, none);
        for (int i = 0; i < 20; ++i)
            sink += Solver::Solve(mined, none).candidates;
        auto const m1 = std::chrono::steady_clock::now();
        double const minedMean = std::chrono::duration<double, std::micro>(m1 - m0).count() / 21;
        std::printf("solver cost, no path anywhere, %zu hazards with 30 armed mines (three passes): outcome %d, oracle "
            "calls %u: mean %.0f us%s\n", mined.hazards.size(), static_cast<int>(minedPlan.outcome),
            minedPlan.pathChecks, minedMean, minedMean > 2000.0 ? "  ** OVER **" : "");
        CHECK(minedPlan.outcome == Outcome::NoPath);
        CHECK(minedPlan.pathChecks <= 3 * mined.params.maxPathChecks + 1);

        // Every probe says "inside a wall": the probes run to their cap, then the path checks as before.
        struct WallsEverywhere : NoPathOracle
        {
            std::uint32_t probes = 0;

            bool Walkable(Vec3&) override
            {
                ++probes;
                return false;
            }
        };
        WallsEverywhere walls;
        auto const p0 = std::chrono::steady_clock::now();
        Plan const wallPlan = Solver::Solve(hot, walls);
        for (int i = 0; i < 20; ++i)
            sink += Solver::Solve(hot, walls).candidates;
        auto const p1 = std::chrono::steady_clock::now();
        double const wallMean = std::chrono::duration<double, std::micro>(p1 - p0).count() / 21;
        std::printf("solver cost, every probe off the navmesh: outcome %d, probes %u, oracle calls %u: "
            "mean %.0f us%s\n",
            static_cast<int>(wallPlan.outcome), wallPlan.stats.probes, wallPlan.pathChecks, wallMean,
            wallMean > 2000.0 ? "  ** OVER **" : "");
        CHECK(wallPlan.stats.probes <= hot.params.maxProbes + 8);
        CHECK(wallPlan.stats.offMesh == wallPlan.stats.probes);
        CHECK(sink > 0);
    }
}

int main()
{
    TestSignedDistanceAgreesWithContains();
    TestSignedDistanceAnalytic();
    TestCircleAtFeet();
    TestDonut();
    TestConeSideways();
    TestLineExcludedEnd();
    TestLanesGap();
    TestCrossfire();
    TestOneSafeLane();
    TestProjectile();
    TestStartDelay();
    TestMineArmingUnderBot();
    TestMineFieldWithGap();
    TestMineFieldGrace();
    TestInsideArmedMine();
    TestStartHazardExitOnly();
    TestPersistent();
    TestReroute();
    TestMovingAnchorMargin();
    TestTurningCone();
    TestInfeasible();
    TestDeterminismAndCaps();
    TestLeaveByAndCast();
    TestFallibility();
    TestProbeSkipsWalls();
    TestProbeSnap();
    TestShiftedPathEnd();
    TestTiersUnderBarrage();
    TestReducedMarginGap();
    TestTrueEnclosure();
    Benchmark();
    return Report("solver_test");
}
