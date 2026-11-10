// Standalone tests for the drop spot rule (Stratholme build X2): src/MechanicDropSpot.h (room geometry, flags) and its
// solver part in src/MechanicSolver.h (DropScore, DropCandidates, LiftEnterable), plus the `stratholme-drops` drill
// lines (src/MechanicDrill.h). Build: tests/run.sh.

#include "check.h"
#include "MechanicDrill.h"
#include "MechanicDropSpot.h"
#include "MechanicSolver.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace BossMechanics;
using Geo::Pose;
using Geo::Vec3;
using Solver::Ally;
using Solver::Carried;
using Solver::Drop;
using Solver::DropCircle;
using Solver::Hazard;
using Solver::Input;
using Solver::Outcome;
using Solver::Plan;
using Ms = std::chrono::milliseconds;

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

    float D(Vec3 const& a, Vec3 const& b)
    {
        return Geo::Dist2d(a.x, a.y, b.x, b.y);
    }

    // A bot at `bot` carrying a circle of `r` that lands `warnMs` from now; `allies` are counted by it.
    Input Carrier(Vec3 const& bot, float r, std::vector<Vec3> const& allies, std::uint32_t warnMs = 3000)
    {
        Input in;
        in.bot = bot;
        in.now = kNow;
        Carried c;
        c.id = 10;
        c.shape = Shape::Circle(r);
        c.mode = Carried::Mode::OnBot;
        c.marginYd = kMargin;
        c.timing = Solver::MakeInstant(kNow + Ms(warnMs), Ms(500));
        for (std::size_t i = 0; i < allies.size(); ++i)
        {
            in.allies.push_back({ 100 + i, allies[i], 0.5f });
            c.allies.push_back(static_cast<std::uint32_t>(i));
        }
        in.carried.push_back(c);
        return in;
    }

    Drop RuleFor(Input const& in)
    {
        Drop d;
        d.present = true;
        d.id = in.carried.front().id;
        d.landAt = in.carried.front().timing.from;
        d.enterAt = d.landAt - Ms(800);
        return d;
    }

    Plan RoleSolve(Input in)
    {
        in.roleSolve = true;
        StraightOracle oracle;
        return Solver::Solve(in, oracle);
    }

    bool AlliesClear(Input const& in, Vec3 const& p)
    {
        float f = 0.f;
        return Solver::CarriedOk(in, p, &f);
    }

    // ---- MechanicDropSpot.h ----

    void TestFlags()
    {
        std::optional<DropSpot::Flags> f = DropSpot::ParseFlags("prefer,edge,away");
        CHECK(f && f->prefer && f->edge && f->away && !f->behind);
        f = DropSpot::ParseFlags("behind");
        CHECK(f && f->behind && !f->edge);
        CHECK(!DropSpot::ParseFlags(""));
        CHECK(!DropSpot::ParseFlags("edge,,away"));
        CHECK(!DropSpot::ParseFlags("wall"));
        CHECK(!DropSpot::ParseFlags("edge,wall"));
        f = DropSpot::ParseFlags("edge,away,avoid");
        CHECK(f && f->edge && f->away && f->avoid && !f->prefer);
    }

    void TestRoom()
    {
        Drop d;
        CHECK(!DropSpot::HasRoom(d));
        CHECK(std::isinf(DropSpot::RoomDistance(d, { 0.f, 0.f, 0.f })));
        d.roomCenter = { 0.f, 0.f, 0.f };
        d.roomRadius = 15.f;
        CHECK(DropSpot::HasRoom(d));
        CHECK_NEAR(DropSpot::RoomDistance(d, { 5.f, 0.f, 0.f }), 10.f, 1e-4);
        CHECK_NEAR(DropSpot::RoomDistance(d, { 0.f, -18.f, 0.f }), -3.f, 1e-4);
        std::vector<Vec3> const circle = DropSpot::EdgePoints(d, 1.f, 0.f);
        CHECK(circle.size() == DropSpot::kCircleRoomBearings);
        for (Vec3 const& q : circle)
            CHECK_NEAR(DropSpot::RoomDistance(d, q), 1.f, 1e-3);

        // A 20 x 10 box, clockwise: the inset points still land inside.
        Drop box;
        box.room = { { 0.f, 0.f, 0.f }, { 0.f, 10.f, 0.f }, { 20.f, 10.f, 0.f }, { 20.f, 0.f, 0.f } };
        CHECK(DropSpot::HasRoom(box));
        CHECK_NEAR(DropSpot::RoomDistance(box, { 10.f, 5.f, 0.f }), 5.f, 1e-4);
        CHECK_NEAR(DropSpot::RoomDistance(box, { 18.f, 5.f, 0.f }), 2.f, 1e-4);
        CHECK_NEAR(DropSpot::RoomDistance(box, { 25.f, 5.f, 0.f }), -5.f, 1e-4);
        std::vector<Vec3> const edge = DropSpot::EdgePoints(box, 1.f, 0.f);
        CHECK(edge.size() >= 20);
        for (Vec3 const& q : edge)
        {
            CHECK(DropSpot::InsidePolygon(box.room, q));
            CHECK(DropSpot::RoomDistance(box, q) >= 1.f - 1e-3f && DropSpot::RoomDistance(box, q) <= 1.f + 1e-3f);
        }
    }

    void TestPreferAndStage()
    {
        Drop d;
        d.prefer.push_back({ { 10.f, 0.f, 0.f }, 5.f, true });
        CHECK(DropSpot::HasEnterable(d));
        CHECK(DropSpot::InsidePrefer(d, { 10.f, 0.f, 0.f }));
        CHECK(DropSpot::InsidePrefer(d, { 5.6f, 0.f, 0.f }));
        CHECK(!DropSpot::InsidePrefer(d, { 5.2f, 0.f, 0.f }));     // within kPreferInsetYd of the edge
        CHECK_NEAR(DropSpot::StageBonus(d, { 5.f, 0.f, 0.f }), DropSpot::kStageBonus, 1e-3);
        CHECK_NEAR(DropSpot::StageBonus(d, { 3.f, 0.f, 0.f }), DropSpot::kStageBonus * 0.5f, 1e-3);
        CHECK(DropSpot::StageBonus(d, { 0.f, 0.f, 0.f }) == 0.f);
        CHECK(DropSpot::StageBonus(d, { 8.f, 0.f, 0.f }) == 0.f);   // inside: the prefer bonus, not staging
        d.prefer.front().enterable = false;
        CHECK(!DropSpot::HasEnterable(d));
        CHECK(DropSpot::StageBonus(d, { 5.f, 0.f, 0.f }) == 0.f);
        CHECK_NEAR(DropSpot::Ahead({ 6.f, 0.f, 0.f }, 0.f, { 0.f, 3.f, 0.f }, 4.f), -2.f, 1e-4);
    }

    // ---- the solver ----

    // Without a rule a clear carrier stays; with edge + away it walks to the room's edge on the far side from the
    // group, and nobody is inside its circle there.
    void TestEdgeAway()
    {
        std::vector<Vec3> const allies = { { 8.f, 0.f, 0.f }, { -8.f, 0.f, 0.f }, { 0.f, 8.f, 0.f } };
        Input in = Carrier({ 0.f, 0.f, 0.f }, 5.f, allies);
        Plan const stay = RoleSolve(in);
        CHECK(stay.outcome == Outcome::Safe);

        in.drop = RuleFor(in);
        in.drop.roomCenter = { 0.f, 0.f, 0.f };
        in.drop.roomRadius = 15.f;
        in.drop.edge = true;
        in.drop.away = true;
        Plan const p = RoleSolve(in);
        CHECK(p.outcome == Outcome::Move);
        CHECK(DropSpot::RoomDistance(in.drop, p.target) <= DropSpot::kEdgeInsetYd + 0.5f);
        CHECK(DropSpot::RoomDistance(in.drop, p.target) >= 0.f);
        CHECK(p.target.y < -5.f);                   // the group's centroid is at (0, 2.7)
        CHECK(AlliesClear(in, p.target));
        // Every candidate the rule added is within the walk before the landing.
        std::vector<Vec3> cands;
        Solver::Detail::DropCandidates(in, cands);
        CHECK(!cands.empty());
        for (Vec3 const& q : cands)
            CHECK(D(in.bot, q) <= 7.f * 3.f + 2.f + 1e-3f);
    }

    // The role band (a melee carrier) does not hold the drop back; without a rule it would.
    void TestRoleBandIgnored()
    {
        std::vector<Vec3> const allies = { { 8.f, 0.f, 0.f }, { -8.f, 0.f, 0.f }, { 0.f, 8.f, 0.f } };
        Input in = Carrier({ 0.f, 0.f, 0.f }, 5.f, allies);
        in.role.present = true;
        in.role.anchor = { 0.f, 1.f, 0.f };
        in.role.minYd = 0.f;
        in.role.maxYd = 4.f;
        in.drop = RuleFor(in);
        in.drop.roomCenter = { 0.f, 0.f, 0.f };
        in.drop.roomRadius = 15.f;
        in.drop.edge = true;
        Plan const p = RoleSolve(in);
        CHECK(p.outcome == Outcome::Move);
        CHECK(DropSpot::RoomDistance(in.drop, p.target) <= DropSpot::kEdgeInsetYd + 0.5f);
        // The role term is back once the carried shape has landed (the rule is for a live carried shape only).
        Input after = in;
        after.now = kNow + Ms(5000);
        CHECK(Solver::Detail::DropCarried(after) == nullptr);
        CHECK(Solver::Detail::DropScore(after, { 14.f, 0.f, 0.f }) == 0.f);
    }

    // A preferred spot (old fire, not enterable: radius = puddle + reach) beats the edge.
    void TestPrefer()
    {
        std::vector<Vec3> const allies = { { 8.f, 0.f, 0.f }, { -8.f, 0.f, 0.f }, { 0.f, 8.f, 0.f } };
        Input in = Carrier({ 0.f, 0.f, 0.f }, 4.f, allies);
        in.drop = RuleFor(in);
        in.drop.roomCenter = { 0.f, 0.f, 0.f };
        in.drop.roomRadius = 15.f;
        in.drop.edge = true;
        in.drop.away = true;
        in.drop.prefer.push_back({ { 2.f, -8.f, 0.f }, 6.f, false });
        // The old fire itself is a lasting hazard of 4 yd.
        Hazard fire;
        fire.id = 77;
        fire.shape = Shape::Circle(4.f);
        fire.poses = { { 2.f, -8.f, 0.f, 0.f } };
        fire.marginYd = kMargin;
        fire.timing = Solver::MakeInterval(kNow - Ms(5000), kNow + Ms(60000));
        in.hazards.push_back(fire);
        Plan const p = RoleSolve(in);
        CHECK(p.outcome == Outcome::Move);
        CHECK(DropSpot::InsidePrefer(in.drop, p.target));
        CHECK(D(p.target, { 2.f, -8.f, 0.f }) > 4.f + kMargin - 1e-3f);    // landed on its rim, not in the fire
        CHECK(AlliesClear(in, p.target));
    }

    // An enterable spot (an old web, a hazard of its own): the carrier waits next to it until it opens, then steps
    // in so the landing's center is inside it.
    void TestEnterable()
    {
        std::vector<Vec3> const allies = { { -8.f, 0.f, 0.f }, { -8.f, 6.f, 0.f }, { -10.f, -4.f, 0.f } };
        Input in = Carrier({ 0.f, 0.f, 0.f }, 5.f, allies);
        in.drop = RuleFor(in);                      // lands at +3000, opens at +1800
        in.drop.prefer.push_back({ { 12.f, 0.f, 0.f }, 5.f, true });
        Hazard web;
        web.id = 77;
        web.shape = Shape::Circle(5.f);
        web.poses = { { 12.f, 0.f, 0.f, 0.f } };
        web.marginYd = kMargin;
        web.timing = Solver::MakeInterval(kNow - Ms(5000), kNow + Ms(60000));
        in.hazards.push_back(web);
        Hazard other = web;                         // another Zone, never lifted
        other.id = 78;
        other.poses = { { -30.f, 0.f, 0.f, 0.f } };
        in.hazards.push_back(other);

        Solver::LiftEnterable(in, { 77 });
        CHECK(in.hazards[0].timing.to == in.drop.enterAt - in.params.marginMs);
        CHECK(in.hazards[1].timing.to == kNow + Ms(60000));
        Solver::LiftEnterable(in, { 77 });          // idempotent
        CHECK(in.hazards[0].timing.to == in.drop.enterAt - in.params.marginMs);

        // Before it opens: staging, outside the web (and its margin), within kStageYd of its edge.
        Plan const wait = RoleSolve(in);
        CHECK(wait.outcome == Outcome::Move);
        float const out = D(wait.target, { 12.f, 0.f, 0.f });
        CHECK(out > 5.f + kMargin - 1e-3f);
        CHECK(out <= 5.f + DropSpot::kStageYd);
        CHECK(!DropSpot::InsidePrefer(in.drop, wait.target));

        // At the opening (the brain's re-solve): in, its center inside the web, arriving before the landing.
        Input open = in;
        open.bot = wait.target;
        open.now = in.drop.enterAt;
        Plan const step = RoleSolve(open);
        CHECK(step.outcome == Outcome::Move);
        CHECK(DropSpot::InsidePrefer(open.drop, step.target));
        CHECK(step.arriveAt <= in.drop.landAt - in.params.marginMs);

        // A web whose window would end before it starts is emptied (the spot is open at once).
        Input late = in;
        late.hazards[0].timing = Solver::MakeInterval(kNow + Ms(2000), kNow + Ms(60000));
        Solver::LiftEnterable(late, { 77 });
        CHECK(late.hazards[0].timing.from == Ms(0) && late.hazards[0].timing.to == Ms(0));
        // A projectile or a mine is left alone.
        Input mine = in;
        mine.hazards[0].timing = Solver::MakeMineArmed(kNow, kNow + Ms(60000), Ms(500));
        Solver::LiftEnterable(mine, { 77 });
        CHECK(mine.hazards[0].timing.to == kNow + Ms(60000));
        // No rule, nothing lifted.
        Input none = in;
        none.drop = Drop();
        none.hazards[0].timing = web.timing;
        Solver::LiftEnterable(none, { 77 });
        CHECK(none.hazards[0].timing.to == kNow + Ms(60000));
    }

    // Avoid points (bone piles): the landing never covers one when another spot is in reach.
    void TestAvoid()
    {
        std::vector<Vec3> const allies = { { 0.f, 8.f, 0.f }, { 5.f, 8.f, 0.f }, { -5.f, 8.f, 0.f } };
        Input in = Carrier({ 0.f, 0.f, 0.f }, 6.f, allies);
        in.drop = RuleFor(in);
        in.drop.roomCenter = { 0.f, 0.f, 0.f };
        in.drop.roomRadius = 15.f;
        in.drop.edge = true;
        in.drop.away = true;
        Plan const plain = RoleSolve(in);
        CHECK(plain.outcome == Outcome::Move);
        // A pile where the rule would take it, and two more beside it.
        for (Vec3 const& pile : { plain.target, Vec3{ plain.target.x + 4.f, plain.target.y, 0.f },
            Vec3{ plain.target.x - 4.f, plain.target.y, 0.f } })
            in.drop.avoid.push_back({ pile, 1.f, false });
        CHECK(Solver::Detail::DropScore(in, plain.target) > DropSpot::kAvoidBase);
        Plan const p = RoleSolve(in);
        CHECK(p.outcome == Outcome::Move);
        for (DropCircle const& pile : in.drop.avoid)
            CHECK(D(p.target, pile.c) > 6.f + pile.r);
        CHECK(AlliesClear(in, p.target));
    }

    // Behind the travel direction (+x): the landing (radius included) never reaches ahead of the group's centroid.
    void TestBehind()
    {
        std::vector<Vec3> const allies = { { 6.f, 0.f, 0.f }, { 6.f, 4.f, 0.f }, { 6.f, -4.f, 0.f } };
        Input in = Carrier({ 0.f, 0.f, 0.f }, 4.f, allies, 6000);
        in.drop = RuleFor(in);
        in.drop.roomCenter = { 0.f, 0.f, 0.f };
        in.drop.roomRadius = 15.f;
        in.drop.edge = true;
        in.drop.behind = true;
        in.drop.travelFacing = 0.f;
        Plan const p = RoleSolve(in);
        CHECK(p.outcome == Outcome::Move);
        CHECK(p.target.x + 4.f <= 6.f + 1e-3f);
        CHECK(DropSpot::RoomDistance(in.drop, p.target) <= DropSpot::kEdgeInsetYd + 0.5f);
        // Ahead of the group costs: a spot past the centroid scores worse than the bot's own.
        CHECK(Solver::Detail::DropScore(in, { 12.f, 0.f, 0.f }) > Solver::Detail::DropScore(in, { -12.f, 0.f, 0.f }));
    }

    // A lane from the boss to the carrier (AimedAtBot): it misses the pile and the group.
    void TestLane()
    {
        Input in;
        in.bot = { 15.f, 0.f, 0.f };
        in.now = kNow;
        Carried c;
        c.id = 10;
        c.shape = Shape::Line(30.f, 6.f);
        c.mode = Carried::Mode::AimedAtBot;
        c.source = { 0.f, 0.f, 0.f, 0.f };
        c.marginYd = kMargin;
        c.timing = Solver::MakeInstant(kNow + Ms(3000), Ms(500));
        in.allies = { { 101, { -3.f, 4.f, 0.f }, 0.5f }, { 102, { -3.f, -4.f, 0.f }, 0.5f } };
        c.allies = { 0, 1 };
        in.carried.push_back(c);
        in.drop = RuleFor(in);
        in.drop.avoid.push_back({ { 8.f, 0.f, 0.f }, 1.f, false });
        Plan const p = RoleSolve(in);
        CHECK(p.outcome == Outcome::Move);
        Pose const lane = Solver::Detail::CarriedPose(in.carried.front(), p.target, 0.f);
        CHECK(Solver::SignedDistance(c.shape, lane, { 8.f, 0.f, 0.f }) > 1.f);
        CHECK(AlliesClear(in, p.target));
    }

    // maxRangeYd keeps the drop near the owner.
    void TestRange()
    {
        std::vector<Vec3> const allies = { { 8.f, 0.f, 0.f }, { -8.f, 0.f, 0.f }, { 0.f, 8.f, 0.f } };
        Input in = Carrier({ 0.f, 0.f, 0.f }, 5.f, allies);
        in.drop = RuleFor(in);
        in.drop.roomCenter = { 0.f, 0.f, 0.f };
        in.drop.roomRadius = 15.f;
        in.drop.edge = true;
        in.drop.away = true;
        in.drop.maxRangeYd = 8.f;
        in.drop.rangeFrom = { 0.f, 0.f, 0.f };
        Plan const p = RoleSolve(in);
        CHECK(D(p.target, { 0.f, 0.f, 0.f }) <= 8.f + 1.f);
        CHECK(AlliesClear(in, p.target));
    }

    // Outside the room costs more than any edge or away term.
    void TestOutside()
    {
        std::vector<Vec3> const allies = { { 8.f, 0.f, 0.f } };
        Input in = Carrier({ 0.f, 0.f, 0.f }, 5.f, allies);
        in.drop = RuleFor(in);
        in.drop.roomCenter = { 0.f, 0.f, 0.f };
        in.drop.roomRadius = 15.f;
        in.drop.away = true;
        CHECK(Solver::Detail::DropScore(in, { -16.f, 0.f, 0.f }) > Solver::Detail::DropScore(in, { -14.f, 0.f, 0.f }));
        CHECK(Solver::Detail::DropScore(in, { -16.f, 0.f, 0.f }) > DropSpot::kOutsideBase
            - DropSpot::kAwayPerYd * DropSpot::kAwayCapYd);
    }

    // ---- Spot bounds (Stratholme F-1b, R-1-B B-1) ----

    // The Baron's room in miniature: the soft circle room (r 20 round the origin) reaches 5 yd past the south wall
    // (y = -15), where his door is (0, -15). The group stands north, so the edge and away terms pull the carrier south,
    // through the door into the corridor. The hard bounds (the room's real outline, the door kept out) stop that.
    Solver::Bounds BaronRoom(bool pathInside)
    {
        Solver::Bounds b;
        b.areas.push_back({ { { -24.f, -15.f, 0.f }, { 24.f, -15.f, 0.f }, { 24.f, 20.f, 0.f }, { -24.f, 20.f, 0.f } },
            {}, 0.f, -5.f, 5.f });
        b.keepOut.push_back({ { 0.f, -15.f, 0.f }, 6.f, false });
        b.pathInside = pathInside;
        return b;
    }

    void TestBoundsGeometry()
    {
        Solver::Bounds const room = BaronRoom(true);
        CHECK(DropSpot::InBounds(room, { 10.f, 0.f, 0.f }));
        CHECK(!DropSpot::InBounds(room, { 10.f, -16.f, 0.f }));     // past the wall
        CHECK(!DropSpot::InBounds(room, { 2.f, -11.f, 0.f }));      // inside, at the door
        CHECK(!DropSpot::InBounds(room, { 10.f, 0.f, 8.f }));       // another floor
        Solver::Bounds none;
        CHECK(DropSpot::InBounds(none, { 1000.f, 1000.f, 0.f }));
        // A circle area, and the union of two.
        Solver::Bounds two;
        two.areas.push_back({ {}, { 0.f, 0.f, 0.f }, 5.f, -100.f, 100.f });
        two.areas.push_back({ {}, { 20.f, 0.f, 0.f }, 5.f, -100.f, 100.f });
        CHECK(DropSpot::InBounds(two, { 3.f, 0.f, 0.f }) && DropSpot::InBounds(two, { 18.f, 0.f, 0.f }));
        CHECK(!DropSpot::InBounds(two, { 10.f, 0.f, 0.f }));
        // Every set holds.
        CHECK(!DropSpot::InBounds(std::vector<Solver::Bounds>{ room, two }, { 10.f, 0.f, 0.f }));
        CHECK(DropSpot::InBounds(std::vector<Solver::Bounds>{ room, two }, { 3.f, 0.f, 0.f }));

        // pathInside: once in, the walk stays in; a start outside walks in; through the door is refused.
        std::vector<Solver::Bounds> const all{ room };
        CHECK(DropSpot::PathInBounds(all, { { -10.f, 0.f, 0.f }, { 10.f, 0.f, 0.f } }));
        CHECK(!DropSpot::PathInBounds(all, { { -10.f, -10.f, 0.f }, { 0.f, -20.f, 0.f }, { 10.f, -10.f, 0.f } }));
        CHECK(DropSpot::PathInBounds(all, { { 0.f, -25.f, 0.f }, { -10.f, -18.f, 0.f }, { -10.f, 0.f, 0.f } }));
        CHECK(DropSpot::PathInBounds(all, { { -10.f, -10.f, 0.f }, { -12.f, -14.5f, 0.f }, { 10.f, -10.f, 0.f } })
            == false);                                              // grazes the door's keep-out circle
        std::vector<Solver::Bounds> loose{ room };
        loose.front().pathInside = false;
        CHECK(DropSpot::PathInBounds(loose, { { -10.f, -10.f, 0.f }, { 0.f, -20.f, 0.f }, { 10.f, -10.f, 0.f } }));

        CHECK(DropSpot::SameBounds(room, BaronRoom(true)));
        CHECK(!DropSpot::SameBounds(room, BaronRoom(false)));
        CHECK(!DropSpot::SameBounds(room, two));
    }

    void TestBoundsDrop()
    {
        std::vector<Vec3> const allies = { { 8.f, 6.f, 0.f }, { -8.f, 6.f, 0.f }, { 0.f, 12.f, 0.f } };
        Input in = Carrier({ 0.f, -6.f, 0.f }, 6.f, allies);
        in.drop = RuleFor(in);
        in.drop.roomCenter = { 0.f, 0.f, 0.f };
        in.drop.roomRadius = 20.f;
        in.drop.edge = true;
        in.drop.away = true;

        // Soft room only: the carrier lands south of the wall (the B-1 failure).
        Plan const loose = RoleSolve(in);
        CHECK(loose.outcome == Outcome::Move);
        CHECK(loose.target.y < -15.f);

        // Hard bounds: inside the room, off the door, a clean move, and the refused candidates counted.
        in.bounds.push_back(BaronRoom(true));
        Plan const p = RoleSolve(in);
        CHECK(p.outcome == Outcome::Move);
        CHECK(DropSpot::InBounds(in.bounds, p.target));
        CHECK(DropSpot::PathInBounds(in.bounds, p.path));
        CHECK(AlliesClear(in, p.target));
        CHECK(p.stats.outOfBounds > 0u);

        // A bot already outside (in the corridor) and safe there is never refused its own spot.
        Input outside = Carrier({ 0.f, -22.f, 0.f }, 6.f, allies);
        outside.bounds.push_back(BaronRoom(true));
        CHECK(RoleSolve(outside).outcome == Outcome::Safe);
    }

    // An oracle that walks every path through a fixed corner (the navmesh through a closed door).
    struct DoorOracle : Solver::PathOracle
    {
        Vec3 via;
        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            out = { from, via, to };
            return true;
        }
    };

    void TestBoundsPath()
    {
        std::vector<Vec3> const allies = { { 8.f, 6.f, 0.f }, { -8.f, 6.f, 0.f }, { 0.f, 12.f, 0.f } };
        Input in = Carrier({ 0.f, -6.f, 0.f }, 6.f, allies, 6000);    // time for the detour through the door
        in.drop = RuleFor(in);
        in.drop.roomCenter = { 0.f, 0.f, 0.f };
        in.drop.roomRadius = 20.f;
        in.drop.edge = true;
        in.drop.away = true;
        in.roleSolve = true;
        in.bounds.push_back(BaronRoom(true));
        DoorOracle door;
        door.via = { 0.f, -18.f, 0.f };
        Plan const p = Solver::Solve(in, door);
        // Every clean path leaves through the door: refused; whatever the solve ends with, it is not such a move.
        CHECK(p.outcome != Outcome::Move);
        CHECK(p.stats.outOfBounds > 0u);
        in.bounds.front().pathInside = false;
        Plan const q = Solver::Solve(in, door);
        CHECK(q.outcome == Outcome::Move);
        CHECK(DropSpot::InBounds(in.bounds, q.target));
    }

    // A parked boss's stop keeps the bounds (a door's keep-out circle: the stock leash).
    void TestBoundsPark()
    {
        Input in;
        in.bot = { 0.f, 0.f, 0.f };
        in.now = kNow;
        Solver::Park& pk = in.park;
        pk.present = true;
        pk.inside = true;
        pk.boss = { 0.f, 0.f, 0.f };
        pk.stopYd = 3.f;
        pk.triggerYd = 5.f;
        pk.depthYd = 1.f;
        pk.zone.shape = Shape::Circle(4.f);
        pk.zone.poses = { Pose{ 0.f, -12.f, 0.f, 0.f } };
        pk.zone.marginYd = 0.5f;
        Vec3 const tank{ 0.f, -17.5f, 0.f };    // the stop is (0, -14.5), 2.5 yd into the zone
        CHECK(Solver::Detail::ParkOk(in, tank));
        Solver::Bounds far;
        far.keepOut.push_back({ { 20.f, 20.f, 0.f }, 6.f, false });
        in.bounds.push_back(far);
        CHECK(Solver::Detail::ParkOk(in, tank));
        Solver::Bounds door;
        door.keepOut.push_back({ { 0.f, -16.f, 0.f }, 2.f, false });
        in.bounds.push_back(door);
        CHECK(!Solver::Detail::ParkOk(in, tank));
    }

    // The enterable dwell budget (R-1-BM m-2): Nerub'enkan's Stuck counts 2 s in a web.
    void TestDwell()
    {
        static_assert(DropSpot::DwellFits(Ms(800), Ms(2000)));
        static_assert(!DropSpot::DwellFits(Ms(1200), Ms(2000)));
        CHECK(DropSpot::kEnterableExitMs == Ms(1000));
    }

    // ---- the drill ----

    void TestDrill()
    {
        Drill::Def const* d = Drill::Find("stratholme-drops");
        CHECK(d != nullptr);
        if (!d)
            return;
        CHECK(d->cycleMs * d->defaultCycles == 120000);
        std::uint32_t drops = 0;
        std::uint32_t webs = 0;
        std::uint32_t websLiveAt = 0;
        for (Drill::Step const& step : d->steps)
        {
            std::string_view const line = step.line;
            std::optional<std::string_view> const tag = Drill::Value(line, "tag");
            CHECK(tag.has_value());
            if (std::optional<std::string_view> const flags = Drill::Value(line, "drop"))
            {
                ++drops;
                CHECK(DropSpot::ParseFlags(*flags).has_value());
                // A shape a member carries: a circle on a member or a line aimed at one.
                CHECK(Drill::Value(line, "anchor") == std::string_view("member")
                    || Drill::Value(line, "to") == std::string_view("member"));
                CHECK(tag && Drill::DropBarTag(static_cast<std::uint32_t>(std::stoul(std::string(*tag)))));
                if (flags->find("prefer") != std::string_view::npos)
                    CHECK(step.atMs > websLiveAt);  // the old webs are live (past their 1 s warning) by then
                if (Drill::Value(line, "to"))
                {
                    CHECK(Drill::Value(line, "exempt") == std::string_view("target"));
                    // HM3 (R-1-BM m-3): S15's RoomDropRule(true) = Edge + Away + avoid the piles; Away alone is inert.
                    std::optional<DropSpot::Flags> const f = DropSpot::ParseFlags(*flags);
                    CHECK(f && f->edge && f->away && f->avoid);
                }
                // A rule never leans on Away alone (a tie-breaker).
                std::optional<DropSpot::Flags> const f = DropSpot::ParseFlags(*flags);
                CHECK(f && (!f->away || f->edge || f->prefer));
                if (std::optional<std::string_view> const dwell = Drill::Value(line, "dwell"))
                    CHECK(DropSpot::DwellFits(Ms(std::stoul(std::string(*dwell))), Ms(2000)));
            }
            else
            {
                ++webs;
                CHECK(Drill::Value(line, "persist").has_value());
                CHECK(tag == std::string_view("1"));
                CHECK(!Drill::DropBarTag(Drill::kDropTagWeb));
                websLiveAt = std::max(websLiveAt, step.atMs + 1000);
            }
        }
        CHECK(drops == 4);
        CHECK(webs == 2);
        for (std::uint32_t t : { Drill::kDropTagGlob, Drill::kDropTagRot, Drill::kDropTagLane,
            Drill::kDropTagHolyFire })
            CHECK(Drill::DropBarTag(t));
    }
}

int main()
{
    TestFlags();
    TestRoom();
    TestPreferAndStage();
    TestEdgeAway();
    TestRoleBandIgnored();
    TestPrefer();
    TestEnterable();
    TestAvoid();
    TestBehind();
    TestLane();
    TestRange();
    TestOutside();
    TestBoundsGeometry();
    TestBoundsDrop();
    TestBoundsPath();
    TestBoundsPark();
    TestDwell();
    TestDrill();
    return Report("drop_spot_test");
}
