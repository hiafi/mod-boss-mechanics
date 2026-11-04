// Standalone tests for the M3 additions of src/MechanicSolver.h: regions, allies, carried shapes, FarFrom, Freeze,
// Sight, Reach, Park, TargetOk (STAGE-M3-HANDOFF §3.5, §4 WP-A).
// Build: tests/run.sh.

#include "check.h"
#include "MechanicSolver.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

using namespace BossMechanics;
using Geo::Pose;
using Geo::Vec3;
using Solver::Ally;
using Solver::Carried;
using Solver::Hazard;
using Solver::Input;
using Solver::Outcome;
using Solver::Plan;
using Solver::Region;
using Ms = std::chrono::milliseconds;

namespace
{
    constexpr float kMargin = 0.75f;
    Ms const kNow{ 100000 };

    struct Lcg
    {
        std::uint64_t state = 0x7654321ull;

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

    struct NoPathOracle : Solver::PathOracle
    {
        bool Find(Vec3 const&, Vec3 const&, std::vector<Vec3>&) override
        {
            return false;
        }
    };

    // No path anywhere and every probe "inside a wall" (M2's costliest case).
    struct WallsEverywhere : NoPathOracle
    {
        bool Walkable(Vec3&) override
        {
            return false;
        }
    };

    // Paths are straight; line of sight is cut by its walls (2D segments).
    struct SightOracle : Solver::PathOracle
    {
        std::vector<std::pair<Vec3, Vec3>> walls;
        std::uint32_t rays = 0;

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
            out = { from, to };
            return true;
        }

        bool Visible(Vec3 const& from, Vec3 const& to) const
        {
            for (auto const& [a, b] : walls)
                if (Intersects(from, to, a, b))
                    return false;
            return true;
        }

        bool InSight(Vec3 const& from, Vec3 const& to) override
        {
            ++rays;
            return Visible(from, to);
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

    Region MakeRegion(std::uint32_t id, Shape const& shape, Pose const& pose, std::int64_t fromMs, std::int64_t toMs,
        float margin = 0.5f)
    {
        Region r;
        r.id = id;
        r.shape = shape;
        r.poses = { pose };
        r.marginYd = margin;
        r.from = kNow + Ms(fromMs);
        r.to = kNow + Ms(toMs);
        return r;
    }

    // A carried shape counting every ally of the input (call after the allies are in).
    Carried MakeCarried(Input const& in, std::uint32_t id, Shape const& shape, Solver::Timing const& timing,
        float margin = 0.5f)
    {
        Carried c;
        c.id = id;
        c.shape = shape;
        c.timing = timing;
        c.marginYd = margin;
        for (std::uint32_t i = 0; i < in.allies.size(); ++i)
            c.allies.push_back(i);
        return c;
    }

    Ally MakeAlly(std::uint64_t guid, Vec3 p, float slack = 0.f)
    {
        Ally a;
        a.guid = guid;
        a.p = p;
        a.slackYd = slack;
        return a;
    }

    float Dist(Vec3 const& a, Vec3 const& b)
    {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    bool IsMove(Plan const& p)
    {
        return p.outcome == Outcome::Move || p.outcome == Outcome::GraceMove;
    }

    bool TargetClear(Input const& in, Plan const& plan)
    {
        for (Hazard const& h : in.hazards)
            for (Pose const& p : h.poses)
                if (Solver::SignedDistance(h.shape, p, plan.target) <= h.marginYd)
                    return false;
        return true;
    }

    bool InsideBy(Region const& r, Vec3 const& p)
    {
        for (Pose const& pose : r.poses)
            if (Solver::SignedDistance(r.shape, pose, p) > -r.marginYd + 1e-4f)
                return false;
        return true;
    }

    Ms HorizonEnd(Input const& in)
    {
        return in.now + in.params.horizon;
    }

    // ----------------------------------------------------------------------------------------------------------
    // Empty M3 inputs change nothing
    // ----------------------------------------------------------------------------------------------------------

    void TestEmptyInputs()
    {
        Lcg rng;
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        for (std::uint32_t i = 0; i < 12; ++i)
        {
            Pose const p{ rng.Range(-15.f, 15.f), rng.Range(-15.f, 15.f), 0.f, rng.Range(-3.f, 3.f) };
            Shape const s = i % 3 == 0 ? Shape::Circle(rng.Range(2.f, 6.f))
                : (i % 3 == 1 ? Shape::Cone(rng.Range(30.f, 120.f), rng.Range(5.f, 12.f))
                    : Shape::Line(rng.Range(8.f, 20.f), rng.Range(2.f, 5.f)));
            in.hazards.push_back(MakeHazard(i + 1, s, p, InstantIn(static_cast<std::int64_t>(rng.Range(500, 6000)))));
        }
        std::uint32_t mismatches = 0;
        for (std::uint32_t k = 0; k < 2000; ++k)
        {
            Vec3 const p{ rng.Range(-20.f, 20.f), rng.Range(-20.f, 20.f), 0.f };
            Ms const arrival = kNow + Ms(static_cast<std::int64_t>(rng.Range(0, 5000)));
            bool const a = Solver::TargetOk(in, p, arrival, HorizonEnd(in));
            bool const b = Solver::PointSafe(in, p, arrival, std::max(arrival, HorizonEnd(in)));
            mismatches += a != b;
            CHECK(Solver::RegionsOk(in, p, arrival));
            CHECK(Solver::CarriedOk(in, p, nullptr));
        }
        CHECK(mismatches == 0);

        in.hazards.push_back(MakeHazard(99, Shape::Circle(4.f), Pose{}, InstantIn(2000)));
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(!plan.hasFacing);
        CHECK(plan.violated == 0);
        CHECK(plan.sightChecks == 0);
        CHECK(plan.waypoints.empty());
        CHECK(TargetClear(in, plan));
    }

    // ----------------------------------------------------------------------------------------------------------
    // Stack (regions)
    // ----------------------------------------------------------------------------------------------------------

    void TestStack()
    {
        StraightOracle oracle;

        // Outside: into the circle by the margin, before from − marginMs.
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.regions.push_back(MakeRegion(7, Shape::Circle(4.f), Pose{ 10.f, 0.f, 0.f, 0.f }, 3000, 3500));
        CHECK(!Solver::TargetOk(in, in.bot, kNow, HorizonEnd(in)));
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(InsideBy(in.regions[0], plan.target));
        CHECK(plan.arriveAt <= kNow + Ms(3000) - in.params.marginMs);
        CHECK(Solver::TargetOk(in, plan.target, plan.arriveAt, HorizonEnd(in)));
        CHECK(std::find(plan.answers.begin(), plan.answers.end(), 7u) != plan.answers.end());
        CHECK(plan.holdUntil >= kNow + Ms(3500));            // holds through the window
        CHECK(Dist(plan.target, { 10.f, 0.f, 0.f }) > 2.f);  // the nearest way in, not the center

        // Inside already: Safe.
        in.bot = { 10.5f, 0.5f, 0.f };
        CHECK(Solver::Solve(in, oracle).outcome == Outcome::Safe);

        // Half the Stack under a hot Zone (x >= 10 - margin): the free half.
        Input half = MakeInput({ 9.f, 12.f, 0.f });
        half.regions.push_back(MakeRegion(7, Shape::Circle(4.f), Pose{ 10.f, 0.f, 0.f, 0.f }, 3000, 3500));
        half.hazards.push_back(MakeHazard(1, Shape::Line(10.f, 20.f), Pose{ 10.f, 0.f, 0.f, 0.f },
            Solver::MakeInterval(kNow - Ms(100), kNow + Ms(20000))));
        Plan const free = Solver::Solve(half, oracle);
        CHECK(IsMove(free));
        CHECK(InsideBy(half.regions[0], free.target));
        CHECK(TargetClear(half, free));
        CHECK(free.target.x < 10.f - kMargin);

        // A moving anchor (drift poses): inside every pose.
        Input moving = MakeInput({ 0.f, 0.f, 0.f });
        Region r = MakeRegion(8, Shape::Circle(4.f), Pose{ 10.f, 0.f, 0.f, 0.f }, 3000, 3500);
        r.poses = { Pose{ 10.f, 0.f, 0.f, 0.f }, Pose{ 11.f, 0.f, 0.f, 0.f }, Pose{ 12.f, 0.f, 0.f, 0.f } };
        moving.regions.push_back(r);
        Plan const mp = Solver::Solve(moving, oracle);
        CHECK(IsMove(mp));
        CHECK(InsideBy(r, mp.target));
        for (Pose const& pose : r.poses)
            CHECK(Dist(mp.target, { pose.x, pose.y, 0.f }) <= 3.5f + 1e-3f);

        // Late (the window opens before the bot can get there): the nearest way in, best effort.
        Input late = MakeInput({ 0.f, 0.f, 0.f });
        late.regions.push_back(MakeRegion(9, Shape::Circle(3.f), Pose{ 20.f, 0.f, 0.f, 0.f }, 500, 5000));
        Plan const lp = Solver::Solve(late, oracle);
        CHECK(IsMove(lp));
        CHECK(InsideBy(late.regions[0], lp.target));
        CHECK(lp.target.x <= 18.5f);

        // A soft region only scores: the spot stays Safe, and an escape prefers the spot inside it.
        Input soft = MakeInput({ 0.f, 0.f, 0.f });
        soft.regions.push_back(MakeRegion(10, Shape::Circle(3.f), Pose{ 0.f, 8.f, 0.f, 0.f }, 3000, 3500));
        soft.regions[0].hard = false;
        CHECK(Solver::Solve(soft, oracle).outcome == Outcome::Safe);
        soft.hazards.push_back(MakeHazard(1, Shape::Circle(4.f), Pose{}, InstantIn(2000)));
        Plan const sp = Solver::Solve(soft, oracle);
        CHECK(IsMove(sp));
        CHECK(TargetClear(soft, sp));
        CHECK(Dist(sp.target, { 0.f, 8.f, 0.f }) <= 3.f);

        // An ended region is ignored.
        Input ended = MakeInput({ 0.f, 0.f, 0.f });
        ended.regions.push_back(MakeRegion(11, Shape::Circle(3.f), Pose{ 20.f, 0.f, 0.f, 0.f }, -3000, -1000));
        CHECK(Solver::Solve(ended, oracle).outcome == Outcome::Safe);
    }

    // Alexei's "Line up" + "Keep your distance": a lane Stack and a Spread 8, four bots solved one after another,
    // each seeing the earlier ones' planned spots as allies.
    void TestLaneAndSpread()
    {
        StraightOracle oracle;
        Region const lane = MakeRegion(20, Shape::Line(36.f, 4.f), Pose{ 0.f, 0.f, 0.f, 0.f }, 8000, 8500);
        std::vector<Vec3> const starts{ { -5.f, -1.f, 0.f }, { -5.f, 0.f, 0.f }, { -5.f, 1.f, 0.f },
            { -6.f, 0.f, 0.f } };
        std::vector<Vec3> planned;
        for (std::size_t b = 0; b < starts.size(); ++b)
        {
            Input in = MakeInput(starts[b]);
            in.regions.push_back(lane);
            for (std::size_t k = 0; k < planned.size(); ++k)
                in.allies.push_back(MakeAlly(k + 1, planned[k]));
            in.carried.push_back(MakeCarried(in, 21, Shape::Circle(8.f), InstantIn(8000)));
            Plan const plan = Solver::Solve(in, oracle);
            CHECK(IsMove(plan));
            CHECK(InsideBy(lane, plan.target));
            CHECK(plan.arriveAt <= kNow + Ms(8000) - in.params.marginMs);
            planned.push_back(plan.target);
        }
        float closest = 1e9f;
        for (std::size_t i = 0; i < planned.size(); ++i)
            for (std::size_t j = i + 1; j < planned.size(); ++j)
                closest = std::min(closest, Dist(planned[i], planned[j]));
        CHECK(closest >= 8.5f - 1e-3f);
    }

    // ----------------------------------------------------------------------------------------------------------
    // Carriers (Spread, RunOut)
    // ----------------------------------------------------------------------------------------------------------

    void TestCarriers()
    {
        StraightOracle oracle;

        // RunOut among 3 allies: at least r + margin from each.
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.allies = { MakeAlly(1, { 1.f, 1.f, 0.f }), MakeAlly(2, { -1.f, 0.5f, 0.f }), MakeAlly(3, { 0.5f, -1.f, 0.f },
            0.5f) };
        in.carried.push_back(MakeCarried(in, 30, Shape::Circle(8.f), InstantIn(4000)));
        CHECK(!Solver::CarriedOk(in, in.bot, nullptr));
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        for (Ally const& a : in.allies)
            CHECK(Dist(plan.target, a.p) >= 8.5f + a.slackYd - 1e-3f);
        CHECK(plan.arriveAt <= kNow + Ms(4000) - in.params.marginMs);
        CHECK(plan.holdUntil >= kNow + Ms(4500));

        // Two carriers solved in turn (each carries its circle and sees the other's circle as a hazard).
        Vec3 const a0{ 0.f, 0.f, 0.f };
        Vec3 const b0{ 1.f, 0.f, 0.f };
        Input ia = MakeInput(a0);
        ia.allies = { MakeAlly(2, b0) };
        ia.carried.push_back(MakeCarried(ia, 31, Shape::Circle(8.f), InstantIn(4000)));
        ia.hazards.push_back(MakeHazard(31, Shape::Circle(8.f), Pose{ b0.x, b0.y, 0.f, 0.f }, InstantIn(4000), 0.5f));
        Plan const pa = Solver::Solve(ia, oracle);
        CHECK(IsMove(pa));
        Input ib = MakeInput(b0);
        ib.allies = { MakeAlly(1, pa.target) };
        ib.carried.push_back(MakeCarried(ib, 31, Shape::Circle(8.f), InstantIn(4000)));
        ib.hazards.push_back(MakeHazard(31, Shape::Circle(8.f), Pose{ pa.target.x, pa.target.y, 0.f, 0.f },
            InstantIn(4000), 0.5f));
        Plan const pb = Solver::Solve(ib, oracle);
        CHECK(pb.outcome == Outcome::Safe || IsMove(pb));
        Vec3 const bEnd = pb.outcome == Outcome::Safe ? b0 : pb.target;
        CHECK(Dist(pa.target, bEnd) >= 8.5f - 1e-3f);

        // RunOut from the owner (a moving owner: drift poses and margin): beyond every pose.
        Input owner = MakeInput({ 1.f, 1.f, 0.f });
        Hazard h = MakeHazard(32, Shape::Circle(10.f), Pose{}, InstantIn(4000), kMargin + 1.f);
        h.poses = { Pose{ 0.f, 0.f, 0.f, 0.f }, Pose{ 1.f, 0.f, 0.f, 0.f }, Pose{ 2.f, 0.f, 0.f, 0.f } };
        h.driftYd = 1.f;
        owner.hazards.push_back(h);
        Plan const po = Solver::Solve(owner, oracle);
        CHECK(IsMove(po));
        CHECK(TargetClear(owner, po));
        CHECK(Dist(po.target, { 2.f, 0.f, 0.f }) > 10.f);
    }

    // Spread of 4 melee around a target: the slot ring on the band's outer radius keeps all four in reach; without
    // it a carrier leaves reach (or fails).
    struct SpreadLayout
    {
        std::vector<Vec3> spots;
        bool allMoved = true;
    };

    SpreadLayout SolveSpread(bool slotRing)
    {
        StraightOracle oracle;
        std::vector<Vec3> spots{ { 2.5f, 0.f, 0.f }, { 0.f, 2.5f, 0.f }, { -2.5f, 0.f, 0.f }, { 0.f, -2.5f, 0.f } };
        SpreadLayout out;
        for (std::size_t b = 0; b < spots.size(); ++b)
        {
            Input in = MakeInput(spots[b]);
            in.params.slotRing = slotRing;
            in.role.present = true;
            in.role.anchor = { 0.f, 0.f, 0.f };
            in.role.minYd = 0.f;
            in.role.maxYd = 5.f;
            for (std::size_t k = 0; k < spots.size(); ++k)
                if (k != b)
                    in.allies.push_back(MakeAlly(k + 1, spots[k]));
            in.carried.push_back(MakeCarried(in, 40, Shape::Circle(5.f), InstantIn(3000), 0.25f));
            in.slot = static_cast<std::uint32_t>(b);
            in.slotCount = 4;
            Plan const plan = Solver::Solve(in, oracle);
            if (IsMove(plan))
                spots[b] = plan.target;
            else if (plan.outcome != Outcome::Safe)
                out.allMoved = false;
        }
        out.spots = spots;
        return out;
    }

    void TestSpreadSlotRing()
    {
        SpreadLayout const with = SolveSpread(true);
        CHECK(with.allMoved);
        float closest = 1e9f;
        float farthest = 0.f;
        for (std::size_t i = 0; i < with.spots.size(); ++i)
        {
            farthest = std::max(farthest, Dist(with.spots[i], { 0.f, 0.f, 0.f }));
            for (std::size_t j = i + 1; j < with.spots.size(); ++j)
                closest = std::min(closest, Dist(with.spots[i], with.spots[j]));
        }
        CHECK(closest >= 5.25f - 1e-3f);
        CHECK(farthest <= 5.f + 1e-3f);

        SpreadLayout const without = SolveSpread(false);
        float farthestWithout = 0.f;
        for (Vec3 const& s : without.spots)
            farthestWithout = std::max(farthestWithout, Dist(s, { 0.f, 0.f, 0.f }));
        CHECK(!without.allMoved || farthestWithout > 5.f + 1e-3f);
        std::printf("spread of 4 at reach 5: with the slot ring farthest %.2f yd, closest pair %.2f yd; without it "
            "farthest %.2f yd%s\n", farthest, closest, farthestWithout, without.allMoved ? "" : " (a carrier failed)");
    }

    // ----------------------------------------------------------------------------------------------------------
    // Bait
    // ----------------------------------------------------------------------------------------------------------

    void TestBait()
    {
        StraightOracle oracle;
        Vec3 const owner{ 0.f, 0.f, 0.f };
        std::vector<Vec3> const others{ { 3.f, 0.f, 0.f }, { 0.f, 4.f, 0.f }, { -5.f, -2.f, 0.f } };

        // The picker: beyond everyone + 2 (a circle hazard on the owner), Radius + margin from each ally, far.
        Input in = MakeInput({ 1.f, 1.f, 0.f });
        float farthest = 0.f;
        for (std::size_t k = 0; k < others.size(); ++k)
        {
            in.allies.push_back(MakeAlly(k + 1, others[k]));
            in.hazards.push_back(MakeHazard(51, Shape::Circle(6.f), Pose{ others[k].x, others[k].y, 0.f, 0.f },
                InstantIn(3000), 0.5f));
            farthest = std::max(farthest, Dist(owner, others[k]));
        }
        float const othersRange = 10.f;
        in.hazards.push_back(MakeHazard(50, Shape::Circle(std::max(othersRange, farthest) + 2.f), Pose{},
            InstantIn(3000), 0.5f));
        in.far.present = true;
        in.far.from = owner;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(TargetClear(in, plan));
        for (Vec3 const& o : others)
        {
            CHECK(Dist(plan.target, owner) >= Dist(o, owner) + 2.f);
            CHECK(Dist(plan.target, o) >= 6.5f - 1e-3f);
        }
        CHECK(plan.arriveAt <= kNow + Ms(3000) - in.params.marginMs);

        // FarFrom pulls farther than the nearest clean spot.
        Input near = in;
        near.far.present = false;
        Plan const np = Solver::Solve(near, oracle);
        CHECK(IsMove(np));
        CHECK(Dist(plan.target, owner) >= Dist(np.target, owner) - 1e-3f);

        // Everyone else: inside othersRange − margin around the owner, away from the picker's landing circle.
        Vec3 const picker{ -14.f, 0.f, 0.f };
        Input other = MakeInput({ 12.f, 3.f, 0.f });
        other.regions.push_back(MakeRegion(50, Shape::Circle(othersRange), Pose{}, 3000, 3500, 0.5f));
        other.hazards.push_back(MakeHazard(51, Shape::Circle(6.f), Pose{ picker.x, picker.y, 0.f, 0.f },
            InstantIn(3000), 0.5f));
        Plan const op = Solver::Solve(other, oracle);
        CHECK(IsMove(op));
        CHECK(Dist(op.target, owner) <= othersRange - 0.5f + 1e-3f);
        CHECK(Dist(op.target, picker) >= 6.5f);
    }

    // ----------------------------------------------------------------------------------------------------------
    // Look away (carried cones and lines)
    // ----------------------------------------------------------------------------------------------------------

    Carried LookAwayCone(Input const& in)
    {
        Carried c = MakeCarried(in, 60, Shape::Cone(60.f, 8.f),
            Solver::MakeInterval(kNow + Ms(500), kNow + Ms(6000)));
        c.chooseFacing = true;
        c.preferredFacing = 0.f;
        c.angleMarginDeg = 10.f;
        return c;
    }

    bool ConeClear(Input const& in, Carried const& c, Vec3 const& at, float facing)
    {
        for (std::uint32_t idx : c.allies)
            if (Solver::SignedDistance(c.shape, Pose{ at.x, at.y, at.z, facing }, in.allies[idx].p) <= c.marginYd)
                return false;
        return true;
    }

    void TestFreeFacingAnalytic()
    {
        // One ally ahead at 4 yd: blocked arc 30° + 10° (+ the 0.5° edge pad) + asin(0.5 / 4) either side.
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.allies = { MakeAlly(1, { 4.f, 0.f, 0.f }) };
        Carried c = LookAwayCone(in);
        float const pad = Geo::DegToRad(Solver::Detail::kFacingEdgeDeg);
        float const half = Geo::DegToRad(40.f) + pad + std::asin(0.5f / 4.f);
        float f = 0.f;
        CHECK(Solver::FreeFacing(in, c, in.bot, f));
        CHECK_NEAR(std::fabs(f), half + 1e-3f, 1e-4f);
        CHECK(f > 0.f);                                     // tie: the smaller angle in [0, 2pi)
        // Preferred facing already free: kept.
        c.preferredFacing = 2.f;
        CHECK(Solver::FreeFacing(in, c, in.bot, f));
        CHECK_NEAR(f, 2.f, 1e-5f);
        // A second ally at 90°: only the clockwise side is left.
        in.allies.push_back(MakeAlly(2, { 0.f, 4.f, 0.f }));
        c = LookAwayCone(in);
        CHECK(Solver::FreeFacing(in, c, in.bot, f));
        CHECK_NEAR(f, -(half + 1e-3f), 1e-4f);
        CHECK(ConeClear(in, c, in.bot, f));
        // An ally beyond length + margin does not block.
        in.allies = { MakeAlly(1, { 8.6f, 0.f, 0.f }) };
        c = LookAwayCone(in);
        CHECK(Solver::FreeFacing(in, c, in.bot, f));
        CHECK_NEAR(f, 0.f, 1e-6f);
        // Within the margin of the apex: nothing is free.
        in.allies = { MakeAlly(1, { 0.3f, 0.f, 0.f }) };
        c = LookAwayCone(in);
        CHECK(!Solver::FreeFacing(in, c, in.bot, f));

        // A line (width 2) on the bot: half-width asin((1 + 0.5) / d), plus the 10° margin.
        Input li = MakeInput({ 0.f, 0.f, 0.f });
        li.allies = { MakeAlly(1, { 5.f, 0.f, 0.f }) };
        Carried l = MakeCarried(li, 61, Shape::Line(12.f, 2.f), InstantIn(2000));
        l.chooseFacing = true;
        float lf = 0.f;
        CHECK(Solver::FreeFacing(li, l, li.bot, lf));
        CHECK_NEAR(std::fabs(lf), Geo::DegToRad(10.f) + pad + std::asin(1.5f / 5.f) + 1e-3f, 1e-4f);
        // A rear cone (facing offset 180°): the bot faces the ally to keep it out of the rear.
        Carried rear = LookAwayCone(li);
        rear.shape.facingOffsetDeg = 180.f;
        rear.preferredFacing = Geo::kPi;
        CHECK(Solver::FreeFacing(li, rear, li.bot, lf));
        CHECK(std::fabs(Geo::WrapPi(lf - Geo::kPi)) > Geo::DegToRad(40.f));
        CHECK(ConeClear(li, rear, li.bot, lf + Geo::kPi));
    }

    void TestLookAway()
    {
        StraightOracle oracle;
        // Allies ahead and to the left: the bot stays and faces the free arc nearest its victim.
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.allies = { MakeAlly(1, { 4.f, 0.f, 0.f }), MakeAlly(2, { 0.f, 4.f, 0.f }) };
        in.carried.push_back(LookAwayCone(in));
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(plan.outcome == Outcome::Safe);
        CHECK(plan.hasFacing);
        float const half = Geo::DegToRad(40.f + Solver::Detail::kFacingEdgeDeg) + std::asin(0.5f / 4.f);
        CHECK_NEAR(plan.facing, -(half + 1e-3f), 1e-4f);
        CHECK(ConeClear(in, in.carried[0], in.bot, plan.facing));

        // Surrounded within the margin: it moves to a spot with a free arc.
        Input close = MakeInput({ 0.f, 0.f, 0.f });
        close.allies = { MakeAlly(1, { 0.3f, 0.f, 0.f }) };
        close.carried.push_back(LookAwayCone(close));
        Plan const cp = Solver::Solve(close, oracle);
        CHECK(IsMove(cp));
        CHECK(cp.hasFacing);
        float f = 0.f;
        CHECK(Solver::FreeFacing(close, close.carried[0], cp.target, f));
        CHECK(ConeClear(close, close.carried[0], cp.target, cp.facing));

        // A ring of 8 allies at 3 yd: no free facing there; it moves out to one.
        Input ring = MakeInput({ 0.f, 0.f, 0.f });
        for (std::uint32_t i = 0; i < 8; ++i)
            ring.allies.push_back(MakeAlly(i + 1, { 3.f * std::cos(Geo::kTwoPi * i / 8.f),
                3.f * std::sin(Geo::kTwoPi * i / 8.f), 0.f }));
        ring.carried.push_back(LookAwayCone(ring));
        CHECK(!Solver::CarriedOk(ring, ring.bot, nullptr));
        Plan const rp = Solver::Solve(ring, oracle);
        CHECK(IsMove(rp));
        CHECK(rp.hasFacing);
        CHECK(ConeClear(ring, ring.carried[0], rp.target, rp.facing));
        CHECK(Solver::TargetOk(ring, rp.target, rp.arriveAt, HorizonEnd(ring)));

        // A line aimed at the bot with an ally in between: the bot moves so the line clears the ally.
        Input aimed = MakeInput({ 10.f, 0.f, 0.f });
        aimed.allies = { MakeAlly(1, { 5.f, 0.f, 0.f }) };
        Carried line = MakeCarried(aimed, 62, Shape::Line(20.f, 3.f), InstantIn(3000));
        line.mode = Carried::Mode::AimedAtBot;
        line.source = Pose{ 0.f, 0.f, 0.f, 0.f };
        aimed.carried.push_back(line);
        CHECK(!Solver::CarriedOk(aimed, aimed.bot, nullptr));
        Plan const ap = Solver::Solve(aimed, oracle);
        CHECK(IsMove(ap));
        CHECK(!ap.hasFacing);
        float const o = std::atan2(ap.target.y, ap.target.x);
        CHECK(Solver::SignedDistance(line.shape, Pose{ 0.f, 0.f, 0.f, o }, aimed.allies[0].p) > 0.5f);
        CHECK(ap.arriveAt <= kNow + Ms(3000) - aimed.params.marginMs);
    }

    // ----------------------------------------------------------------------------------------------------------
    // Freeze (HoldStill)
    // ----------------------------------------------------------------------------------------------------------

    void TestFreeze()
    {
        StraightOracle oracle;
        // A Zone known to land in the window, beyond the plain horizon: without the freeze the spot is safe.
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(70, Shape::Circle(4.f), Pose{}, InstantIn(12000)));
        CHECK(Solver::Solve(in, oracle).outcome == Outcome::Safe);
        in.freeze.present = true;
        in.freeze.from = kNow + Ms(2000);
        in.freeze.to = kNow + Ms(14000);
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(IsMove(plan));
        CHECK(TargetClear(in, plan));
        CHECK(plan.arriveAt <= in.freeze.from - in.freeze.settle);
        CHECK(!Solver::TargetOk(in, plan.target, in.freeze.from, HorizonEnd(in)));     // too late for the window

        // Inside the window (from − settle on): Safe, never a move, even with a Zone burning on the bot.
        Input frozen = in;
        frozen.now = in.freeze.from - Ms(100);
        frozen.hazards.push_back(MakeHazard(71, Shape::Circle(5.f), Pose{},
            Solver::MakeInterval(frozen.now - Ms(10), frozen.now + Ms(1000))));
        frozen.regions.push_back(MakeRegion(72, Shape::Circle(2.f), Pose{ 20.f, 0.f, 0.f, 0.f }, 2000, 3000));
        StraightOracle counted;
        Plan const fp = Solver::Solve(frozen, counted);
        CHECK(fp.outcome == Outcome::Safe);
        CHECK(Dist(fp.target, frozen.bot) == 0.f);
        CHECK(fp.path.empty());
        CHECK(counted.calls == 0);
        // After the window: back to normal.
        frozen.now = in.freeze.to + Ms(1);
        frozen.hazards.back().timing = Solver::MakeInterval(frozen.now - Ms(10), frozen.now + Ms(1000));
        CHECK(IsMove(Solver::Solve(frozen, oracle)));
    }

    // ----------------------------------------------------------------------------------------------------------
    // Tank parking
    // ----------------------------------------------------------------------------------------------------------

    Vec3 PredictedStop(Solver::Park const& pk, Vec3 const& t)
    {
        float const d = Dist(t, pk.boss);
        float const k = pk.stopYd / d;
        return { t.x + (pk.boss.x - t.x) * k, t.y + (pk.boss.y - t.y) * k, 0.f };
    }

    void CheckParked(Input const& in, Plan const& plan)
    {
        Solver::Park const& pk = in.park;
        CHECK(IsMove(plan));
        CHECK(Dist(plan.target, pk.boss) > pk.triggerYd);
        Vec3 const stop = PredictedStop(pk, plan.target);
        CHECK(Solver::SignedDistance(pk.zone.shape, pk.zone.poses[0], stop) <= -pk.depthYd + 1e-3f);
        CHECK(Solver::SignedDistance(pk.zone.shape, pk.zone.poses[0], plan.target) > pk.zone.marginYd);
    }

    void TestPark()
    {
        StraightOracle oracle;
        // Circle zone (Polkelt): the boss ends inside by depthYd, the tank outside at its edge.
        Input in = MakeInput({ 1.5f, 0.f, 0.f });
        in.park.present = true;
        in.park.boss = { 0.f, 0.f, 0.f };
        in.park.stopYd = 2.5f;
        in.park.triggerYd = 5.f;
        in.park.depthYd = 1.f;
        in.park.zone = MakeRegion(80, Shape::Circle(5.f), Pose{ 8.f, 0.f, 0.f, 0.f }, 0, 8000, 0.5f);
        CHECK(!Solver::TargetOk(in, in.bot, kNow, HorizonEnd(in)));
        Plan const plan = Solver::Solve(in, oracle);
        CheckParked(in, plan);

        // Lane zone (Gilnid): a 6 yd wide strip east of the boss.
        Input lane = in;
        lane.park.zone = MakeRegion(81, Shape::Line(20.f, 6.f), Pose{ 5.f, -10.f, 0.f, Geo::kPi * 0.5f }, 0, 8000,
            0.5f);
        Plan const lp = Solver::Solve(lane, oracle);
        CheckParked(lane, lp);

        // The boss already deep inside, the tank in reach and safe: stay.
        Input stay = in;
        stay.park.boss = { 8.f, 0.f, 0.f };
        stay.bot = { 11.f, 0.f, 0.f };
        CHECK(Solver::Solve(stay, oracle).outcome == Outcome::Safe);

        // Drag out of a lasting zone under the boss: the boss's stop clears the zone by depthYd.
        Input drag = MakeInput({ 1.5f, 0.f, 0.f });
        Hazard const fire = MakeHazard(82, Shape::Circle(6.f), Pose{},
            Solver::MakeInterval(kNow - Ms(500), kNow + Ms(15000)));
        drag.hazards.push_back(fire);
        drag.park.present = true;
        drag.park.inside = false;
        drag.park.boss = { 0.f, 0.f, 0.f };
        drag.park.stopYd = 2.5f;
        drag.park.triggerYd = 5.f;
        drag.park.depthYd = 1.f;
        drag.park.avoid = { fire };
        Plan const dp = Solver::Solve(drag, oracle);
        CHECK(IsMove(dp));
        CHECK(TargetClear(drag, dp));
        Vec3 const stop = PredictedStop(drag.park, dp.target);
        CHECK(Solver::SignedDistance(fire.shape, fire.poses[0], stop) >= 1.f - 1e-3f);
        CHECK(Dist(dp.target, drag.park.boss) > drag.park.triggerYd);
    }

    // ----------------------------------------------------------------------------------------------------------
    // Healer sight, conflicts
    // ----------------------------------------------------------------------------------------------------------

    void TestHealerSight()
    {
        Vec3 const tank{ 0.f, 12.f, 0.f };
        Input in = MakeInput({ -0.5f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(90, Shape::Circle(3.f), Pose{}, InstantIn(2000)));
        SightOracle open;
        Plan const base = Solver::Solve(in, open);
        CHECK(IsMove(base));
        CHECK(base.sightChecks == 0);
        CHECK(open.rays == 0);

        // A pillar between the escape it would pick and the tank.
        SightOracle pillar;
        Vec3 const mid{ (base.target.x + tank.x) * 0.5f, (base.target.y + tank.y) * 0.5f, 0.f };
        float const dx = tank.x - base.target.x;
        float const dy = tank.y - base.target.y;
        float const len = std::hypot(dx, dy);
        Vec3 const nrm{ -dy / len, dx / len, 0.f };
        pillar.walls = { { { mid.x - nrm.x * 1.5f, mid.y - nrm.y * 1.5f, 0.f },
            { mid.x + nrm.x * 1.5f, mid.y + nrm.y * 1.5f, 0.f } } };
        CHECK(!pillar.Visible(base.target, tank));
        in.sight.points = { tank };
        in.sight.weights = { 6.f };
        in.sight.maxChecks = 8;
        Plan const seen = Solver::Solve(in, pillar);
        CHECK(IsMove(seen));
        CHECK(TargetClear(in, seen));
        CHECK(pillar.Visible(seen.target, tank));
        CHECK(pillar.rays <= in.sight.maxChecks);
        CHECK(seen.sightChecks == pillar.rays);

        // Two sight points (tank full, a member half): still at most maxChecks rays.
        in.sight.points = { tank, Vec3{ 8.f, 8.f, 0.f } };
        in.sight.weights = { 6.f, 3.f };
        in.sight.maxChecks = 5;
        SightOracle two = pillar;
        two.rays = 0;
        Plan const tp = Solver::Solve(in, two);
        CHECK(IsMove(tp));
        CHECK(two.rays <= 5);
        CHECK(two.Visible(tp.target, tank));    // the tank point first (STAGE-M3-R-REPORT M-3)

        // A role solve: the healer's spot is safe but out of sight; it moves back into sight.
        Input role = MakeInput({ base.target.x, base.target.y, 0.f });
        role.sight.points = { tank };
        role.sight.weights = { 6.f };
        role.roleSolve = true;
        SightOracle rs = pillar;
        rs.rays = 0;
        Plan const rp = Solver::Solve(role, rs);
        CHECK(IsMove(rp));
        CHECK(pillar.Visible(rp.target, tank));
        // ...and with nothing better than its spot, Safe.
        Input inSight = MakeInput({ 0.f, 0.f, 0.f });
        inSight.sight = role.sight;
        inSight.roleSolve = true;
        SightOracle clear;
        CHECK(Solver::Solve(inSight, clear).outcome == Outcome::Safe);
    }

    // STAGE-M3-R-REPORT M-3 (the review's sight_repro): a healer 7 yd behind a 6 yd wall from the tank, in-sight
    // spots about 6 yd sideways, a role band and a role solve. With the tank alone, a 5-man (4 points) and a raid
    // (10 points), the 8 rays find a spot that sees the tank: the tank ray first, candidates in a known shadow
    // skipped.
    void TestHealerBehindPillar()
    {
        Vec3 const tank{ 0.f, 15.f, 0.f };
        for (int members : { 0, 3, 9 })
        {
            Input in = MakeInput({ 0.f, 0.f, 0.f });
            in.role.present = true;
            in.role.anchor = tank;
            in.role.minYd = 8.f;
            in.role.maxYd = 34.5f;
            // Healer.SightWeight's default (10; members at half).
            in.sight.points = { tank };
            in.sight.weights = { 10.f };
            in.sight.maxChecks = 8;
            in.reach.maxYd = 36.5f;
            in.reach.weightPerYd = 1.f;
            in.reach.points = { tank };
            for (int i = 0; i < members; ++i)
            {
                Vec3 const m{ -8.f + 2.f * static_cast<float>(i), 16.f + static_cast<float>(i % 2), 0.f };
                in.sight.points.push_back(m);
                in.sight.weights.push_back(5.f);
                in.reach.points.push_back(m);
            }
            in.roleSolve = true;
            SightOracle o;
            o.walls = { { { -3.f, 7.f, 0.f }, { 3.f, 7.f, 0.f } } };
            CHECK(!o.Visible(in.bot, tank));
            Plan const p = Solver::Solve(in, o);
            CHECK(IsMove(p));
            CHECK(o.Visible(p.target, tank));
            CHECK(o.rays <= in.sight.maxChecks);
            CHECK(p.sightChecks == o.rays);
            CHECK(p.sightChecks > 0);
            std::printf("healer behind a pillar, %zu sight points: target (%.1f, %.1f), %u rays, sees the tank %d\n",
                in.sight.points.size(), p.target.x, p.target.y, p.sightChecks,
                static_cast<int>(o.Visible(p.target, tank)));
        }
        // The shadow rule: a point on the far half of a blocked ray, near its line, is skipped; one beside it or
        // nearer the sight point is not.
        Vec3 const from{ 0.f, 0.f, 0.f };
        Vec3 const blocked{ 0.f, 10.f, 0.f };
        CHECK(Solver::Detail::InShadow(from, blocked, { 1.5f, 11.f, 0.f }));
        CHECK(Solver::Detail::InShadow(from, blocked, { 0.5f, 6.f, 0.f }));
        CHECK(!Solver::Detail::InShadow(from, blocked, { 3.f, 10.f, 0.f }));
        CHECK(!Solver::Detail::InShadow(from, blocked, { 0.f, 3.f, 0.f }));
        CHECK(!Solver::Detail::InShadow(from, blocked, { 0.f, -10.f, 0.f }));
    }

    // STAGE-M3-R-REPORT m-11: a leg passing 0.15 yd off a Line's corner with a 0.25 yd margin. At the old 0.5 yd
    // step the samples straddled the corner (0.28 yd off each) and the path passed; samples at most one margin apart
    // catch it. Clearing the corner by the margin still passes.
    void TestPathSamplesPerMargin()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.params.startDelay = Ms(0);
        Hazard h = MakeHazard(1, Shape::Line(10.f, 2.f), Pose{ 0.f, 0.f, 0.f, 0.f },
            Solver::MakeInterval(kNow - Ms(1000), Solver::kForever), 0.25f);
        in.hazards.push_back(h);
        float const k = 0.70710678f;
        auto leg = [&](float clear)
        {
            // The corner (10, 1); outward diagonal n = (k, k), the leg runs along d = (k, -k), u in [-3.25, 3.25].
            Vec3 const a{ 10.f + clear * k - 3.25f * k, 1.f + clear * k + 3.25f * k, 0.f };
            Vec3 const b{ 10.f + clear * k + 3.25f * k, 1.f + clear * k - 3.25f * k, 0.f };
            return std::vector<Vec3>{ a, b };
        };
        CHECK(!Solver::CheckPath(in, leg(0.15f), kNow, false).ok);
        CHECK(Solver::CheckPath(in, leg(0.3f), kNow, false).ok);
    }

    // Decision §7.2: a region entirely under a hot Zone. Nothing passes TargetOk; the Fallback ranks Zone exposure
    // first, so the bot stays out of the Zone and breaks the region.
    void TestConflictZoneFirst()
    {
        StraightOracle oracle;
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.regions.push_back(MakeRegion(100, Shape::Circle(3.f), Pose{ 10.f, 0.f, 0.f, 0.f }, 2000, 3000));
        in.hazards.push_back(MakeHazard(101, Shape::Circle(6.f), Pose{ 10.f, 0.f, 0.f, 0.f },
            Solver::MakeInterval(kNow - Ms(100), kNow + Ms(10000))));
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(plan.outcome == Outcome::Fallback);
        CHECK(TargetClear(in, plan));
        CHECK(plan.violated == 1);

        // Two regions that cannot both hold: the Fallback keeps the one it can (fewest violated).
        Input two = MakeInput({ 0.f, 0.f, 0.f });
        two.regions.push_back(MakeRegion(102, Shape::Circle(3.f), Pose{ 6.f, 0.f, 0.f, 0.f }, 3000, 3500));
        two.regions.push_back(MakeRegion(103, Shape::Circle(3.f), Pose{ -20.f, 0.f, 0.f, 0.f }, 3000, 3500));
        Plan const tp = Solver::Solve(two, oracle);
        CHECK(tp.outcome == Outcome::Fallback);
        CHECK(tp.violated == 1);
        CHECK(InsideBy(two.regions[0], tp.target) || InsideBy(two.regions[1], tp.target));
    }

    // ----------------------------------------------------------------------------------------------------------
    // Determinism, caps, cost
    // ----------------------------------------------------------------------------------------------------------

    // 30 hazards (one on the bot) + 4 regions + 2 carried shapes + 19 allies. feasible = the regions nest around
    // one free spot; otherwise they are scattered (no candidate holds them all: the Fallback runs).
    // heavy: M2's barrage mix instead (circles 3-6, lines 24 x 4, cones 60/15, donuts 3/8, 4-8 s) with the zone on
    // the bot already burning (solver_test's costliest inputs).
    Input Busy(std::uint64_t seed, bool feasible, bool heavy = false)
    {
        Lcg rng;
        rng.state = seed;
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(5.f), Pose{}, heavy
            ? Solver::MakeInterval(kNow - Ms(500), kNow + Ms(6000)) : InstantIn(2500)));
        for (std::uint32_t i = 0; heavy && i < 29; ++i)
        {
            Pose const p{ rng.Range(-25.f, 25.f), rng.Range(-25.f, 25.f), 0.f, rng.Range(-3.f, 3.f) };
            Solver::Timing const t = InstantIn(static_cast<std::int64_t>(rng.Range(4000.f, 8000.f)));
            Shape const s = i % 4 == 0 ? Shape::Circle(rng.Range(3.f, 6.f)) : (i % 4 == 1 ? Shape::Line(24.f, 4.f)
                : (i % 4 == 2 ? Shape::Cone(60.f, 15.f) : Shape::Donut(3.f, 8.f)));
            in.hazards.push_back(MakeHazard(2 + i, s, p, t));
        }
        for (std::uint32_t i = 0; !heavy && i < 29; ++i)
        {
            Pose const p{ rng.Range(-25.f, 25.f), rng.Range(-25.f, 25.f), 0.f, rng.Range(-3.f, 3.f) };
            if (std::hypot(p.x, p.y) < 8.f)
                continue;
            Shape const s = i % 3 == 0 ? Shape::Circle(rng.Range(2.f, 4.f))
                : (i % 3 == 1 ? Shape::Cone(rng.Range(30.f, 90.f), rng.Range(4.f, 8.f))
                    : Shape::Line(rng.Range(6.f, 12.f), rng.Range(2.f, 4.f)));
            in.hazards.push_back(MakeHazard(2 + i, s, p, InstantIn(static_cast<std::int64_t>(rng.Range(1000, 6000)))));
        }
        while (in.hazards.size() < 30)
            in.hazards.push_back(MakeHazard(100 + static_cast<std::uint32_t>(in.hazards.size()), Shape::Circle(2.f),
                Pose{ rng.Range(10.f, 25.f), rng.Range(10.f, 25.f), 0.f, 0.f }, InstantIn(4000)));
        for (std::uint32_t i = 0; i < 4; ++i)
        {
            Pose const p = feasible ? Pose{ 9.f + 0.5f * i, 0.f, 0.f, 0.f }
                : Pose{ rng.Range(-20.f, 20.f), rng.Range(-20.f, 20.f), 0.f, 0.f };
            in.regions.push_back(MakeRegion(200 + i, Shape::Circle(4.f + i), p, 4000, 4500));
        }
        for (std::uint32_t i = 0; i < 19; ++i)
        {
            float const a = rng.Range(0.f, Geo::kTwoPi);
            float const r = rng.Range(14.f, 28.f);
            in.allies.push_back(MakeAlly(i + 1, { r * std::cos(a), r * std::sin(a), 0.f }, 1.f));
        }
        in.carried.push_back(MakeCarried(in, 300, Shape::Circle(6.f), InstantIn(4000)));
        Carried cone = MakeCarried(in, 301, Shape::Cone(60.f, 8.f), InstantIn(4000));
        cone.chooseFacing = true;
        in.carried.push_back(cone);
        in.role.present = true;
        in.role.anchor = { 5.f, 5.f, 0.f };
        in.role.minYd = 8.f;
        in.role.maxYd = 30.f;
        in.sight.points = { Vec3{ 5.f, 5.f, 0.f } };
        in.sight.weights = { 6.f };
        // A lane frame with no lanes: its grid fills the candidate cap (as M2's barrage benchmark).
        Solver::LaneFrame lf;
        lf.frame = Frame{ -30.f, -30.f, 0.f, 0.f, 60.f, 60.f };
        in.lanes.push_back(lf);
        return in;
    }

    void TestDeterminismAndCaps()
    {
        for (bool feasible : { true, false })
        {
            Input in = Busy(42, feasible);
            StraightOracle o1;
            StraightOracle o2;
            Plan const a = Solver::Solve(in, o1);
            Plan const b = Solver::Solve(in, o2);
            CHECK(a.outcome == b.outcome);
            CHECK(a.target.x == b.target.x && a.target.y == b.target.y);
            CHECK(a.facing == b.facing);
            CHECK(a.violated == b.violated);
            CHECK(a.candidates <= in.params.maxCandidates);
            CHECK(a.pathChecks <= 3 * in.params.maxPathChecks + 1);
            CHECK(a.sightChecks <= in.sight.maxChecks);
            if (feasible)
            {
                CHECK(IsMove(a));
                CHECK(a.violated == 0);
                CHECK(Solver::TargetOk(in, a.target, a.arriveAt, HorizonEnd(in)));
            }
            else
            {
                CHECK(a.outcome == Outcome::Fallback);
                CHECK(a.violated >= 1);
            }
            CHECK(a.hasFacing);
        }
        // The candidate cap holds with every M3 source live.
        Input capped = Busy(7, false);
        capped.params.maxCandidates = 60;
        capped.far.present = true;
        capped.far.from = { 0.f, 0.f, 0.f };
        StraightOracle oracle;
        CHECK(Solver::Candidates(capped).size() <= 60);
        CHECK(Solver::Solve(capped, oracle).candidates <= 60);
    }

    void Benchmark()
    {
        struct Case
        {
            char const* name;
            bool feasible;
            int oracle;     // 0 straight, 1 no path, 2 no path and every probe off the navmesh
            bool heavy = false;
        };
        std::uint32_t sink = 0;
        double worstMean = 0.0;
        double worstMax = 0.0;
        for (Case const& c : { Case{ "feasible", true, 0 }, Case{ "scattered regions (fallback)", false, 0 },
            Case{ "feasible, no path anywhere", true, 1 }, Case{ "scattered, no path anywhere", false, 1 },
            Case{ "feasible, every probe off the navmesh", true, 2 },
            Case{ "M2 barrage mix, bot's zone burning, no path anywhere", true, 1, true },
            Case{ "M2 barrage mix, bot's zone burning, every probe off the navmesh", true, 2, true } })
        {
            Input const in = Busy(1234, c.feasible, c.heavy);
            StraightOracle straight;
            NoPathOracle none;
            WallsEverywhere walls;
            Solver::PathOracle& oracle = c.oracle == 0 ? static_cast<Solver::PathOracle&>(straight)
                : (c.oracle == 1 ? static_cast<Solver::PathOracle&>(none) : walls);
            Plan const first = Solver::Solve(in, oracle);
            int const runs = 100;
            double worst = 0.0;
            auto const t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < runs; ++i)
            {
                auto const s0 = std::chrono::steady_clock::now();
                sink += Solver::Solve(in, oracle).candidates;
                auto const s1 = std::chrono::steady_clock::now();
                worst = std::max(worst, std::chrono::duration<double, std::micro>(s1 - s0).count());
            }
            auto const t1 = std::chrono::steady_clock::now();
            double const mean = std::chrono::duration<double, std::micro>(t1 - t0).count() / runs;
            worstMean = std::max(worstMean, mean);
            worstMax = std::max(worstMax, worst);
            std::printf("solver m3 cost, %s: %zu hazards + %zu regions + %zu carried + %zu allies, %u candidates, "
                "outcome %d, violated %u, path checks %u, rays %u: mean %.0f us, max %.0f us (limit 2000 us)%s\n",
                c.name, in.hazards.size(), in.regions.size(), in.carried.size(), in.allies.size(), first.candidates,
                static_cast<int>(first.outcome), first.violated, first.pathChecks, first.sightChecks, mean, worst,
                mean > 2000.0 ? "  ** OVER **" : "");
        }
        CHECK(sink > 0);
        CHECK(worstMean < 2000.0);
        std::printf("solver m3 cost, worst case: mean %.0f us, max %.0f us\n", worstMean, worstMax);
    }
}

int main()
{
    TestEmptyInputs();
    TestStack();
    TestLaneAndSpread();
    TestCarriers();
    TestSpreadSlotRing();
    TestBait();
    TestFreeFacingAnalytic();
    TestLookAway();
    TestFreeze();
    TestPark();
    TestHealerSight();
    TestHealerBehindPillar();
    TestPathSamplesPerMargin();
    TestConflictZoneFirst();
    TestDeterminismAndCaps();
    Benchmark();
    return Report("solver_m3_test");
}
