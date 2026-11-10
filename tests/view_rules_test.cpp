// Standalone tests for src/MechanicViewRules.h, the pure rules the bots' view uses for the player kinds
// (STAGE-M3-HANDOFF §3.2, §3.4, §3.6, WP-V), and for the inputs it builds run through the solver: two-sided Spread
// separation solved bot after bot, the Bait picker and the others, the Cluster gather radius.
// Build: tests/run.sh.

#include "check.h"
#include "MechanicJudge.h"
#include "MechanicSolver.h"
#include "MechanicViewRules.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace BossMechanics;
using Geo::Pose;
using Geo::Vec3;
using Solver::Input;
using Solver::Outcome;
using Solver::Plan;
using Ms = std::chrono::milliseconds;
namespace VR = BossMechanics::ViewRules;

namespace
{
    constexpr float kMargin = 0.75f;
    Ms const kNow{ 100000 };

    struct StraightOracle : Solver::PathOracle
    {
        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            out = { from, to };
            return true;
        }
    };

    float Dist(Vec3 const& a, Vec3 const& b)
    {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    Vec3 EndOf(Plan const& p, Vec3 const& start)
    {
        return p.outcome == Outcome::Safe ? start : p.target;
    }

    void TestInRadiusAndMargins()
    {
        CHECK_NEAR(VR::InRadius(Shape::Circle(5.f)), 5.f, 1e-5);
        CHECK_NEAR(VR::InRadius(Shape::Donut(2.f, 6.f)), 2.f, 1e-5);
        float const s45 = std::sin(Geo::DegToRad(45.f));
        CHECK_NEAR(VR::InRadius(Shape::Cone(90.f, 8.f)), 8.f * s45 / (1.f + s45), 1e-4);
        CHECK_NEAR(VR::InRadius(Shape::Cone(180.f, 8.f)), 4.f, 1e-5);
        CHECK_NEAR(VR::InRadius(Shape::Cone(360.f, 8.f)), 8.f, 1e-5);
        Shape lane = Shape::Line(30.f, 4.f);
        CHECK_NEAR(VR::InRadius(lane), 2.f, 1e-5);
        lane.excludeStart = 28.f;
        CHECK_NEAR(VR::InRadius(lane), 1.f, 1e-5);

        // The margin never eats more than half the inner radius; a big circle keeps the base margin.
        CHECK_NEAR(VR::RegionMarginYd(Shape::Circle(6.f), kMargin), kMargin, 1e-6);
        CHECK_NEAR(VR::RegionMarginYd(Shape::Circle(1.f), kMargin), 0.5f, 1e-6);
        CHECK_NEAR(VR::RegionMarginYd(Shape::Line(30.f, 2.f), kMargin), 0.5f, 1e-6);
        CHECK_NEAR(VR::RegionMarginYd(Shape::Circle(6.f), -1.f), 0.f, 1e-6);
        CHECK_NEAR(VR::RegionDriftYd(Shape::Circle(3.f), 7.f), 1.5f, 1e-6);
        CHECK_NEAR(VR::RegionDriftYd(Shape::Circle(3.f), 0.4f), 0.4f, 1e-6);

        // Drift poses: none when still, one ahead along the heading when moving; the intersection still has room.
        Pose const live{ 1.f, 2.f, 3.f, 0.5f };
        CHECK(VR::MovingPoses(live, 0.f, 0.f).size() == 1);
        std::vector<Pose> const moving = VR::MovingPoses(live, Geo::kPi * 0.5f, 1.5f);
        CHECK(moving.size() == 2);
        CHECK_NEAR(moving[1].x, 1.f, 1e-5);
        CHECK_NEAR(moving[1].y, 3.5f, 1e-5);
        CHECK_NEAR(moving[1].o, 0.5f, 1e-6);
        Solver::Region r;
        r.shape = Shape::Circle(3.f);
        r.poses = VR::MovingPoses(live, 0.f, VR::RegionDriftYd(r.shape, 7.f));
        r.marginYd = VR::RegionMarginYd(r.shape, kMargin);
        Vec3 const mid{ live.x + 0.75f, live.y, live.z };
        CHECK(Solver::Detail::InsideRegion(r, mid));
    }

    void TestPlannedSpot()
    {
        Ms const recheck{ 250 };
        VR::SpotInputs s;
        s.now = { 1.f, 1.f, 0.f };
        s.nowFacing = 0.3f;
        s.plan = { 5.f, 5.f, 0.f };
        s.planFacing = 1.f;
        s.reserved = { 9.f, 9.f, 0.f };
        s.reservedFacing = 2.f;
        s.speed = 7.f;

        // A human: current spot + HumanSlackYd (a plan from the glue is ignored for a non-bot).
        s.hasPlan = true;
        VR::Spot h = VR::PlannedSpot(s, 1.f, recheck);
        CHECK(h.source == VR::SpotSource::HumanNow);
        CHECK(!h.Planned());
        CHECK_NEAR(h.slackYd, 1.f, 1e-6);
        CHECK_NEAR(h.p.x, 1.f, 1e-6);

        // A bot with a plan: the plan, no slack.
        s.isBot = true;
        VR::Spot const p = VR::PlannedSpot(s, 1.f, recheck);
        CHECK(p.source == VR::SpotSource::Plan);
        CHECK(p.Planned());
        CHECK_NEAR(p.slackYd, 0.f, 1e-6);
        CHECK_NEAR(p.p.x, 5.f, 1e-6);
        CHECK_NEAR(p.facing, 1.f, 1e-6);

        // A bot without a plan: current spot, unplanned slack, plus its motion while moving.
        s.hasPlan = false;
        VR::Spot const still = VR::PlannedSpot(s, 1.f, recheck);
        CHECK(still.source == VR::SpotSource::BotNow);
        CHECK_NEAR(still.slackYd, VR::kUnplannedBotSlackYd, 1e-6);
        s.moving = true;
        VR::Spot const walking = VR::PlannedSpot(s, 1.f, recheck);
        CHECK_NEAR(walking.slackYd, VR::kUnplannedBotSlackYd + 7.f * 0.25f, 1e-5);

        // A reservation beats everything.
        s.hasReservation = true;
        s.hasPlan = true;
        VR::Spot const r = VR::PlannedSpot(s, 1.f, recheck);
        CHECK(r.source == VR::SpotSource::Reservation);
        CHECK_NEAR(r.p.x, 9.f, 1e-6);
        CHECK_NEAR(r.facing, 2.f, 1e-6);
        CHECK_NEAR(r.slackYd, 0.f, 1e-6);

        // M4 WP-Q (G3): another player's Spread/RunOut circle or Bait landing goes into the view at its reservation
        // or at a human's spot; a group bot that has not planned for the mechanic yet (its plan is for something
        // else, or it has none) is left to move for it itself (tests/fight_test.cpp TestRas, TestFenrus).
        CHECK(VR::PlaceOtherCircle(r));
        CHECK(VR::PlaceOtherCircle(h));
        CHECK(!VR::PlaceOtherCircle(p));
        CHECK(!VR::PlaceOtherCircle(still));
        CHECK(!VR::PlaceOtherCircle(walking));
    }

    void TestBuilders()
    {
        VR::Spot spot;
        spot.p = { 3.f, 4.f, 1.f };
        spot.slackYd = 1.f;
        Solver::Timing const t = Solver::MakeInstant(kNow + Ms(3000), Ms(500));
        Solver::Hazard const h = VR::CarrierHazard(7, 6.f, 4.f, spot, kMargin, t);
        CHECK(h.id == 7);
        CHECK(h.synthetic);
        CHECK(h.shape.type == ShapeType::Circle);
        CHECK_NEAR(h.shape.radius, 6.f, 1e-6);
        CHECK_NEAR(h.shape.zTolerance, 4.f, 1e-6);
        CHECK_NEAR(h.marginYd, kMargin + 1.f, 1e-6);
        CHECK_NEAR(h.driftYd, 1.f, 1e-6);
        CHECK(h.poses.size() == 1 && h.poses[0].x == 3.f && h.poses[0].y == 4.f);
        CHECK(h.timing.from == t.from && h.timing.to == t.to);

        Solver::Carried const c = VR::CarriedCircle(7, 6.f, 4.f, kMargin, t, { 0, 2 });
        CHECK(c.mode == Solver::Carried::Mode::OnBot);
        CHECK(!c.chooseFacing);
        CHECK(c.allies.size() == 2 && c.allies[1] == 2);
        CHECK_NEAR(c.shape.radius, 6.f, 1e-6);

        CHECK_NEAR(VR::BaitPickerRadius(10.f, {}), 12.f, 1e-6);
        CHECK_NEAR(VR::BaitPickerRadius(10.f, { 4.f, 13.5f }), 15.5f, 1e-6);
        CHECK_NEAR(VR::BaitPickerRadius(-1.f, {}), VR::kBaitLeadYd, 1e-6);

        CHECK(VR::SoakEtaMs(kNow, 7.f, 7.f, Ms(150)) == kNow.count() + 150 + 1000);
        CHECK(VR::SoakEtaMs(kNow, -3.f, 7.f, Ms(150)) == kNow.count() + 150);
        CHECK(VR::SoakEtaMs(kNow, 1.f, 0.f, Ms(0)) == kNow.count() + 10000);
    }

    void TestMergeFreeze()
    {
        Ms const settle{ 250 };
        Solver::Freeze f;
        VR::MergeFreeze(f, kNow - Ms(5000), kNow - Ms(1), settle, kNow);     // over: ignored
        CHECK(!f.present);
        VR::MergeFreeze(f, kNow + Ms(1000), kNow + Ms(4000), settle, kNow);
        CHECK(f.present && f.from == kNow + Ms(1000) && f.to == kNow + Ms(4000) && f.settle == settle);
        VR::MergeFreeze(f, kNow + Ms(3000), kNow + Ms(6000), settle, kNow);  // overlaps: merged
        CHECK(f.from == kNow + Ms(1000) && f.to == kNow + Ms(6000));
        VR::MergeFreeze(f, kNow + Ms(9000), kNow + Ms(10000), settle, kNow); // later, apart: the earlier stays
        CHECK(f.from == kNow + Ms(1000) && f.to == kNow + Ms(6000));
        VR::MergeFreeze(f, kNow + Ms(100), kNow + Ms(500), settle, kNow);    // earlier, apart: it comes first
        CHECK(f.from == kNow + Ms(100) && f.to == kNow + Ms(500));
    }

    // Everyone within r/2 of the leader has every other member within r: no Cluster miss for any n < members.
    void TestClusterRadius()
    {
        float const r = 6.f;
        float const g = VR::ClusterMemberRadius(r);
        CHECK_NEAR(g, 3.f, 1e-6);
        std::vector<Vec3> players{ { 0.f, 0.f, 0.f } };
        for (int i = 0; i < 8; ++i)
        {
            float const a = Geo::kTwoPi * static_cast<float>(i) / 8.f;
            players.push_back({ std::cos(a) * g * 0.999f, std::sin(a) * g * 0.999f, 0.f });
        }
        Shape circle = Shape::Circle(r);
        CHECK(Judge::ClusterMisses(circle, players, static_cast<std::uint32_t>(players.size() - 1)).empty());
    }

    Input MakeInput(Vec3 bot)
    {
        Input in;
        in.bot = bot;
        in.speed = 7.f;
        in.now = kNow;
        return in;
    }

    // Spread 6 on 4 bots standing together, solved one after another the way the view feeds them: each sees the
    // earlier bots at their reservations and the later ones at their current spots plus the unplanned slack; each
    // carries its circle (allies) and avoids the others' circles (hazards). Both sides off: the layout collides.
    std::vector<Vec3> SolveSpread(bool carried, bool hazards)
    {
        StraightOracle oracle;
        float const r = 6.f;
        Solver::Timing const window = Solver::MakeInstant(kNow + Ms(3500), Ms(500));
        std::vector<Vec3> start{ { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 1.f, 1.f, 0.f } };
        std::vector<Vec3> end = start;
        std::vector<bool> planned(start.size(), false);
        for (std::size_t i = 0; i < start.size(); ++i)
        {
            Input in = MakeInput(start[i]);
            std::vector<std::uint32_t> idx;
            for (std::size_t j = 0; j < start.size(); ++j)
            {
                if (j == i)
                    continue;
                VR::SpotInputs s;
                s.isBot = true;
                s.hasReservation = planned[j];
                s.reserved = end[j];
                s.now = start[j];
                VR::Spot const spot = VR::PlannedSpot(s, 1.f, Ms(250));
                idx.push_back(static_cast<std::uint32_t>(in.allies.size()));
                in.allies.push_back({ 100 + j, spot.p, spot.slackYd });
                if (hazards)
                    in.hazards.push_back(VR::CarrierHazard(9, r, 6.f, spot, kMargin, window));
            }
            if (carried)
                in.carried.push_back(VR::CarriedCircle(9, r, 6.f, kMargin, window, idx));
            Plan const p = Solver::Solve(in, oracle);
            end[i] = EndOf(p, start[i]);
            planned[i] = true;
        }
        return end;
    }

    float MinPair(std::vector<Vec3> const& v)
    {
        float best = 1e9f;
        for (std::size_t i = 0; i < v.size(); ++i)
            for (std::size_t j = i + 1; j < v.size(); ++j)
                best = std::min(best, Dist(v[i], v[j]));
        return best;
    }

    void TestSpreadTwoSided()
    {
        std::vector<Vec3> const both = SolveSpread(true, true);
        CHECK(MinPair(both) > 6.f);                     // the registry hits inside r: everyone clear
        CHECK(MinPair(both) >= 6.f + kMargin - 1e-3f);  // by the margin
        // Either side alone also separates bots solved in turn (the later ones see the earlier ones); the two-sided
        // input is what keeps a human's or an unplanned bot's spot clear from both ends.
        std::vector<Vec3> const none = SolveSpread(false, false);
        CHECK(MinPair(none) < 6.f);
        std::vector<Vec3> const onlyHazards = SolveSpread(false, true);
        CHECK(MinPair(onlyHazards) > 6.f);
        std::vector<Vec3> const onlyCarried = SolveSpread(true, false);
        CHECK(MinPair(onlyCarried) > 6.f);
    }

    // Bait (othersRange 10, landing circle 6): the picker runs past othersRange + 2 and out of every landing circle;
    // a non-picker standing 15 yd out comes back inside othersRange and keeps out of the picker's circle. Then the
    // registry's farthest player (who takes the hit) is the picker.
    void TestBait()
    {
        StraightOracle oracle;
        Vec3 const owner{ 0.f, 0.f, 0.f };
        float const othersRange = 10.f;
        float const radius = 6.f;
        Solver::Timing const window = Solver::MakeInstant(kNow + Ms(4000), Ms(500));
        Vec3 const pickerStart{ 3.f, 0.f, 0.f };
        Vec3 const otherStart{ 15.f, 0.f, 0.f };

        Input pk = MakeInput(pickerStart);
        VR::Spot ownerSpot;
        ownerSpot.p = owner;
        pk.hazards.push_back(VR::CarrierHazard(5, VR::BaitPickerRadius(othersRange, {}), 6.f, ownerSpot, kMargin,
            window));
        VR::Spot otherSpot;
        otherSpot.p = otherStart;
        otherSpot.slackYd = VR::kUnplannedBotSlackYd;
        pk.hazards.push_back(VR::CarrierHazard(5, Judge::BaitHitRadius(radius), 6.f, otherSpot, kMargin, window));
        pk.far.present = true;
        pk.far.from = owner;
        Plan const pp = Solver::Solve(pk, oracle);
        Vec3 const picker = EndOf(pp, pickerStart);
        CHECK(Dist(picker, owner) >= othersRange + VR::kBaitLeadYd + kMargin - 1e-3f);
        CHECK(Dist(picker, otherStart) >= radius + kMargin - 1e-3f);

        Input ot = MakeInput(otherStart);
        Solver::Region range;
        range.id = 5;
        range.shape = Shape::Circle(othersRange);
        range.poses = { Pose{ owner.x, owner.y, owner.z, 0.f } };
        range.marginYd = VR::RegionMarginYd(range.shape, kMargin);
        range.from = window.from;
        range.to = window.to;
        ot.regions.push_back(range);
        VR::Spot pickSpot;
        pickSpot.p = picker;
        pickSpot.source = VR::SpotSource::Reservation;
        ot.hazards.push_back(VR::CarrierHazard(5, Judge::BaitHitRadius(radius), 6.f, pickSpot, kMargin, window));
        Plan const op = Solver::Solve(ot, oracle);
        Vec3 const other = EndOf(op, otherStart);
        CHECK(Dist(other, owner) <= othersRange - kMargin + 1e-3f);
        CHECK(Dist(other, picker) >= radius + kMargin - 1e-3f);
        CHECK(Dist(picker, owner) > Dist(other, owner));
    }
}

namespace
{
    // STAGE-M3-R-REPORT M-1: the smite drill's rear cone (Rear(), TankExempt) is never remembered, so the melee
    // standing behind the boss never make the tank walk; the front cone is, and holds a member standing in front.
    void TestTankFacingShapes()
    {
        Shape front = Shape::Cone(90.f, 8.f);
        Shape rear = Shape::Cone(90.f, 8.f);
        rear.facingOffsetDeg = 180.f;
        Shape side = Shape::Line(10.f, 4.f);
        side.facingOffsetDeg = 90.f;
        Shape line = Shape::Line(20.f, 4.f);
        CHECK(VR::FacesVictim(front));
        CHECK(!VR::FacesVictim(rear));
        CHECK(!VR::FacesVictim(side));
        CHECK(VR::FacesVictim(line));
        CHECK(!VR::FacesVictim(Shape::Circle(5.f)));

        std::vector<Shape> shapes;
        CHECK(!VR::RememberFrontShape(shapes, rear));
        CHECK(shapes.empty());
        // Boss at the origin, tank 3 yd east, melee 3 yd west (behind it).
        Vec3 const boss{ 0.f, 0.f, 0.f };
        Vec3 const tank{ 3.f, 0.f, 0.f };
        std::vector<Vec3> const behind{ { -3.f, 0.f, 0.f }, { -2.5f, 1.f, 0.f } };
        CHECK(!VR::FrontalHits(shapes, boss, tank, behind, kMargin, 15.f));
        // Even a rear cone that somehow got in would hold the melee: the reason it must not be remembered.
        CHECK(VR::FrontalHits({ rear }, boss, tank, behind, kMargin, 15.f));

        CHECK(VR::RememberFrontShape(shapes, front));
        CHECK(!VR::RememberFrontShape(shapes, front));      // once per distinct shape
        CHECK(VR::RememberFrontShape(shapes, line));        // both front shapes of one source are kept
        CHECK(shapes.size() == 2);
        CHECK(!VR::FrontalHits(shapes, boss, tank, behind, kMargin, 15.f));
        // A member standing beside the tank, in front of the boss: the cone (or the line) holds it.
        std::vector<Vec3> const inFront{ { 4.f, 1.f, 0.f } };
        CHECK(VR::FrontalHits(shapes, boss, tank, inFront, kMargin, 15.f));
        // Only the line reaches 15 yd out.
        std::vector<Vec3> const far{ { 15.f, 0.f, 0.f } };
        CHECK(VR::FrontalHits(shapes, boss, tank, far, kMargin, 15.f));
        CHECK(!VR::FrontalHits({ front }, boss, tank, far, kMargin, 15.f));
    }
}

namespace
{
    // STAGE-M3-R-REPORT m-8: the carrier picks its look-away facing with LookAway.MarginDeg (10°); each ally poses
    // the carrier's cone at that facing ± PlannedFacingSlackDeg (FacingSlackDeg 15° capped at 10°). An ally the
    // carrier has just cleared sees itself clear: no re-solve.
    void TestLookAwayBothSides()
    {
        Solver::Input in;
        in.bot = { 0.f, 0.f, 0.f };
        in.now = kNow;
        in.allies = { { 1, { 4.f, 0.5f, 0.f }, 0.f }, { 2, { -1.f, 5.f, 0.f }, 0.f } };
        Solver::Carried c;
        c.id = 70;
        c.shape = Shape::Cone(60.f, 8.f);
        c.chooseFacing = true;
        c.preferredFacing = 0.f;       // the victim stands right behind the first ally
        c.marginYd = kMargin;
        c.angleMarginDeg = 10.f;
        c.timing = Solver::MakeInstant(kNow + Ms(2000), Ms(400));
        c.allies = { 0, 1 };
        float f = 0.f;
        CHECK(Solver::FreeFacing(in, c, in.bot, f));
        float const slack = Geo::DegToRad(VR::PlannedFacingSlackDeg(15.f, 10.f));
        CHECK_NEAR(slack, Geo::DegToRad(10.f), 1e-6f);
        for (Solver::Ally const& a : in.allies)
            for (float d : { -slack, 0.f, slack })
                CHECK(Solver::SignedDistance(c.shape, { 0.f, 0.f, 0.f, f + d }, a.p) > kMargin);
        CHECK(VR::PlannedFacingSlackDeg(5.f, 10.f) == 5.f);
    }
}

namespace
{
    // F-1b (R-1-KIT F2): a Spread/RunOut with no warning and no persistence is a script's hit (the kit's RegistryHit),
    // not a job: left out of the view. A warned one, or a persistent one (a continuous threat), stays.
    void TestUndodgeable()
    {
        CHECK(ViewRules::Undodgeable(kNow, kNow, false));
        CHECK(ViewRules::Undodgeable(kNow, kNow - Ms(1), false));
        CHECK(!ViewRules::Undodgeable(kNow, kNow + Ms(1), false));
        CHECK(!ViewRules::Undodgeable(kNow, kNow + Ms(4000), false));   // Barthilas's Contagion RunOut
        CHECK(!ViewRules::Undodgeable(kNow, kNow, true));
    }
}

int main()
{
    TestUndodgeable();
    TestTankFacingShapes();
    TestLookAwayBothSides();
    TestInRadiusAndMargins();
    TestPlannedSpot();
    TestBuilders();
    TestMergeFreeze();
    TestClusterRadius();
    TestSpreadTwoSided();
    TestBait();
    return Report("view_rules_test");
}
