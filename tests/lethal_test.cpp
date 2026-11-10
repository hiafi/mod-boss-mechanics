// Standalone tests for Lethal() (Stratholme build X4, the kit's LethalWall: The Unforgiven's Cordon, Galford's Pyre
// and north wall): which kinds can be one (Judge::LethalFits), its view timing (ViewRules::LethalTiming), and that
// the solver never plans a way across one: the reduced-margin pass keeps its full margin, no grace crossing, the
// Fallback ranking and its path skip it, NoPath never walks into it and FallbackHolds lets no escape walk on into it.
// Build: tests/run.sh.

#include "check.h"
#include "MechanicJudge.h"
#include "MechanicLanding.h"
#include "MechanicSolver.h"
#include "MechanicViewRules.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

    Kind const kAllKinds[] = { Kind::Zone, Kind::Frontal, Kind::Spread, Kind::RunOut, Kind::Stack, Kind::Soak,
        Kind::Bait, Kind::HoldStill, Kind::Spot };

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
        bool Find(Vec3 const&, Vec3 const&, std::vector<Vec3>&) override { return false; }
    };

    // Every path bends through `via` (a navmesh corridor the straight line does not show).
    struct ViaOracle : Solver::PathOracle
    {
        Vec3 via;

        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            out = { from, via, to };
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

    Hazard MakeHazard(std::uint32_t id, Shape const& shape, Pose const& pose, Solver::Timing const& timing,
        bool lethal)
    {
        Hazard h;
        h.id = id;
        h.shape = shape;
        h.poses = { pose };
        h.marginYd = kMargin;
        h.timing = timing;
        h.lethal = lethal;
        return h;
    }

    // A 60 yd wall along x, 2 yd wide, its center line at y = `y` (from x = -30): a street of the Cordon.
    Hazard Wall(std::uint32_t id, float y, bool lethal)
    {
        return MakeHazard(id, Shape::Line(60.f, 2.f), Pose{ -30.f, y, 0.f, 0.f },
            Solver::MakeInterval(kNow - Ms(1000), Solver::kForever), lethal);
    }

    bool Crosses(Vec3 const& a, Vec3 const& b, float lo, float hi)
    {
        return (a.y < lo && b.y > hi) || (a.y > hi && b.y < lo) || (a.y >= lo && a.y <= hi)
            || (b.y >= lo && b.y <= hi);
    }

    bool PathCrossesBand(std::vector<Vec3> const& path, float lo, float hi)
    {
        for (std::size_t i = 1; i < path.size(); ++i)
            if (Crosses(path[i - 1], path[i], lo, hi))
                return true;
        return false;
    }

    void TestLethalFits()
    {
        for (Kind k : kAllKinds)
        {
            bool const shape = k == Kind::Zone || k == Kind::Frontal;
            CHECK(Judge::LethalFits(k, false, false) == shape);
            CHECK(!Judge::LethalFits(k, true, false));     // a mine's grace is its own crossing rule
            CHECK(!Judge::LethalFits(k, false, true));     // a decoy is never in the bot view
        }
    }

    void TestLethalTiming()
    {
        // The wall arms 2 s after the grip; bots treat it as hot from now, its end kept.
        Solver::Timing const persist = Solver::MakeInterval(kNow + Ms(2000), kNow + Ms(60000));
        Solver::Timing const t = ViewRules::LethalTiming(persist, kNow);
        CHECK(t.type == Solver::TimingType::Interval);
        CHECK(t.from == kNow);
        CHECK(t.to == kNow + Ms(60000));
        // Already armed: unchanged.
        Solver::Timing const armed = Solver::MakeInterval(kNow - Ms(500), Solver::kForever);
        CHECK(ViewRules::LethalTiming(armed, kNow).from == kNow - Ms(500));
        // Instant: opens now, ends where it ended.
        Solver::Timing const instant = Solver::MakeInstant(kNow + Ms(3000), Ms(500));
        Solver::Timing const i = ViewRules::LethalTiming(instant, kNow);
        CHECK(i.from == kNow);
        CHECK(i.to == kNow + Ms(3500));
        // A mine or a projectile keeps its own timing.
        Solver::Timing const mine = Solver::MakeMineArming(kNow + Ms(2000), kNow + Ms(9000), Ms(500));
        CHECK(ViewRules::LethalTiming(mine, kNow).from == kNow + Ms(2000));

        // In the solver: a bot beside the wall in its warning does not walk across it (the wall is hot from now).
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Line(60.f, 2.f), Pose{ -30.f, 3.f, 0.f, 0.f },
            ViewRules::LethalTiming(persist, kNow), true));
        CHECK(!Solver::CheckPath(in, { in.bot, { 0.f, 10.f, 0.f } }, kNow, false).ok);
        // Without it the warning lets the walk through (it is 1.5 s across, the wall lights at 2 s).
        Input warn = in;
        warn.hazards[0].timing = persist;
        warn.hazards[0].lethal = false;
        CHECK(Solver::CheckPath(warn, { warn.bot, { 0.f, 10.f, 0.f } }, kNow, false).ok);
    }

    // Two lethal walls with a 1.2 yd gap between their ends: wider than two reduced margins, narrower than two full
    // ones. Pass 2 (ReducedMargins) would slip a bot through a plain gap; never a lethal one.
    void TestReducedMarginKeepsLethal()
    {
        for (bool lethal : { false, true })
        {
            Input in = MakeInput({ 0.f, -5.f, 0.f });
            Solver::Timing const hot = Solver::MakeInterval(kNow - Ms(1000), Solver::kForever);
            in.hazards.push_back(MakeHazard(1, Shape::Line(19.f, 2.f), Pose{ -19.6f, 0.f, 0.f, 0.f }, hot, lethal));
            in.hazards.push_back(MakeHazard(2, Shape::Line(19.f, 2.f), Pose{ 0.6f, 0.f, 0.f, 0.f }, hot, lethal));
            std::vector<Vec3> const through = { in.bot, { 0.f, 5.f, 0.f } };
            CHECK(!Solver::CheckPath(in, through, kNow, false).ok);
            Input const tight = Solver::ReducedMargins(in);
            CHECK(Solver::CheckPath(tight, through, kNow, false).ok == !lethal);
            for (Hazard const& h : tight.hazards)
                CHECK((h.marginYd == kMargin) == lethal);
        }
    }

    // An armed mine's grace never covers a lethal one (Lethal does not fit a mine; the solver holds the rule anyway).
    void TestNoGrace()
    {
        for (bool lethal : { false, true })
        {
            Input in = MakeInput({ -5.f, 0.f, 0.f });
            in.hazards.push_back(MakeHazard(1, Shape::Circle(2.f), Pose{ 0.f, 0.f, 0.f, 0.f },
                Solver::MakeMineArmed(kNow - Ms(1000), Solver::kForever, Ms(3000)), lethal));
            CHECK(Solver::CheckPath(in, { in.bot, { 5.f, 0.f, 0.f } }, kNow, true).ok == !lethal);
            CHECK(Solver::Detail::AnyGrace(in) == !lethal);
        }
    }

    // The Fallback: a bot in a circle that lights before it can leave, a wall 2 yd north of it. The quickest way out
    // is north, across the wall; a lethal wall is never taken, the bot leaves by a slower side.
    Input FallbackScene(bool lethal)
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Circle(10.f), Pose{ 0.f, -4.f, 0.f, 0.f },
            Solver::MakeInterval(kNow + Ms(300), Solver::kForever), false));
        in.hazards.push_back(Wall(2, 3.f, lethal));
        return in;
    }

    void TestFallbackSkipsLethal()
    {
        {
            StraightOracle oracle;
            Plan const plain = Solver::Solve(FallbackScene(false), oracle);
            CHECK(plain.outcome == Outcome::Fallback);
            CHECK(plain.target.y > 4.f);        // across the plain wall: the least time inside
            CHECK(plain.stats.lethalSkipped == 0);
        }
        {
            StraightOracle oracle;
            Input const in = FallbackScene(true);
            Plan const plan = Solver::Solve(in, oracle);
            CHECK(plan.outcome == Outcome::Fallback);
            CHECK(plan.target.y < 2.f - kMargin);
            CHECK(!PathCrossesBand(plan.path, 2.f, 4.f));
            CHECK(plan.stats.lethalSkipped > 0);
            Solver::Input const lethalIn = Solver::Detail::LethalOnly(in);
            CHECK(!Solver::Detail::CrossesLethal(lethalIn, plan.path, kNow, kNow + in.params.horizon));
        }
    }

    // The ranking reads the straight line; the real path may bend. A path through the wall (the oracle's corridor runs
    // north of it) is refused and the next candidate tried; when every path does, the bot stays (NoPath at its spot).
    void TestFallbackRefusesLethalPath()
    {
        ViaOracle oracle;
        oracle.via = { 0.f, 8.f, 0.f };     // north of the wall: every path crosses it twice
        Input const in = FallbackScene(true);
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(!PathCrossesBand(plan.path, 2.f, 4.f));
        CHECK(plan.outcome == Outcome::NoPath);
        CHECK(plan.stats.lethalSkipped > 0);

        // A corridor south of the wall is fine.
        ViaOracle south;
        south.via = { 1.f, -1.f, 0.f };
        Plan const ok = Solver::Solve(in, south);
        CHECK(ok.outcome == Outcome::Fallback);
        CHECK(!PathCrossesBand(ok.path, 2.f, 4.f));
    }

    // NoPath (no navmesh path anywhere): straight to the nearest exit, never one across a lethal wall. The circle's
    // nearest exit lies north, past the wall: a plain wall is walked through, a lethal one never (another exit, or
    // the bot stays where it is).
    void TestNoPathKeepsOut()
    {
        for (bool lethal : { false, true })
        {
            Input in = MakeInput({ 0.f, 0.5f, 0.f });
            in.hazards.push_back(MakeHazard(1, Shape::Circle(10.f), Pose{ 0.f, -1.f, 0.f, 0.f },
                Solver::MakeInterval(kNow + Ms(300), Solver::kForever), false));
            in.hazards.push_back(Wall(2, 3.f, lethal));
            NoPathOracle oracle;
            Plan const plan = Solver::Solve(in, oracle);
            CHECK(plan.outcome == Outcome::NoPath);
            CHECK(PathCrossesBand(plan.path, 2.f, 4.f) == !lethal);
            CHECK((plan.stats.lethalSkipped > 0) == lethal);
        }
    }

    // FallbackHolds: an escape walks on while the rest of its way is no worse; never into a lethal wall.
    void TestFallbackHoldsRefusesLethal()
    {
        for (bool lethal : { false, true })
        {
            Input in = MakeInput({ 0.f, 0.f, 0.f });
            in.hazards.push_back(Wall(2, 3.f, lethal));
            Plan plan;
            plan.outcome = Outcome::Fallback;
            plan.target = { 0.f, 10.f, 0.f };
            plan.exposure = Ms(5000);           // a hot Fallback: the plain wall's crossing is within it
            std::vector<Vec3> const rest = { in.bot, plan.target };
            CHECK(Solver::FallbackHolds(in, plan, rest, kNow + in.params.horizon, false) == !lethal);
            // A rest that keeps south of the wall holds either way.
            plan.target = { 10.f, 0.f, 0.f };
            CHECK(Solver::FallbackHolds(in, plan, { in.bot, plan.target }, kNow + in.params.horizon, false));
        }
    }

    // A bot inside a lethal wall's margin band (not in the wall) may still step back out on its own side.
    void TestLeavingTheBand()
    {
        Input in = MakeInput({ 0.f, 1.5f, 0.f });
        in.hazards.push_back(Wall(2, 3.f, true));
        Input const lethal = Solver::Detail::LethalOnly(in);
        CHECK(!Solver::Detail::CrossesLethal(lethal, { in.bot, { 0.f, -3.f, 0.f } }, kNow, kNow + Ms(10000)));
        CHECK(Solver::Detail::CrossesLethal(lethal, { in.bot, { 0.f, 8.f, 0.f } }, kNow, kNow + Ms(10000)));
        // Ending in it counts, however it got there.
        CHECK(Solver::Detail::CrossesLethal(lethal, { in.bot, { 0.f, 3.f, 0.f } }, kNow, kNow + Ms(10000)));
        // The solve leaves south (the clean pass at the full margin).
        StraightOracle oracle;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(plan.outcome == Outcome::Move);
        CHECK(plan.target.y < 2.f - kMargin);
    }

    // Stratholme F-2b (R-2-FW m-4): a follow pet stands PET_FOLLOW_DIST (2 yd) beside its owner at pi/2. A bot with a
    // pet keeps a lethal wall the follow distance + 1 yd off, counted as drift so the reduced-margin pass and the
    // Fallback's lethal screen keep it too. The bot faces along the wall (o = 0): the pet stands 2 yd to its north.
    void TestPetMargin()
    {
        CHECK_NEAR(ViewRules::LethalPetExtraYd(kMargin, true), 2.25, 1e-4);
        CHECK(ViewRules::LethalPetExtraYd(kMargin, false) == 0.f);
        CHECK(ViewRules::LethalPetExtraYd(4.f, true) == 0.f);         // a wider margin already holds the pet
        auto petY = [](Vec3 const& bot) { return bot.y + ViewRules::kPetFollowYd; };
        for (bool pet : { false, true })
        {
            // The view's hazard: the wall's strip is y 2..4; the bot stands 1.5 yd south of it (outside 0.75 yd).
            Input in = MakeInput({ 0.f, 0.5f, 0.f });
            Hazard w = Wall(2, 3.f, true);
            float const extra = ViewRules::LethalPetExtraYd(kMargin, pet);
            w.driftYd += extra;
            w.marginYd += extra;
            in.hazards.push_back(w);
            StraightOracle oracle;
            Plan const plan = Solver::Solve(in, oracle);
            Vec3 const at = plan.outcome == Outcome::Safe ? in.bot : plan.target;
            if (!pet)
            {
                CHECK(plan.outcome == Outcome::Safe);
                CHECK(petY(at) > 2.f);                  // the as-built bug: a pet would stand in the strip
                continue;
            }
            CHECK(plan.outcome == Outcome::Move);
            CHECK(at.y <= 2.f - kMargin - extra + 1e-3f);
            CHECK(petY(at) < 2.f);                      // its pet out of the strip
            // The reduced-margin pass and the Fallback's lethal screen keep the pet's room.
            for (Hazard const& h : Solver::ReducedMargins(in).hazards)
                CHECK(h.marginYd >= extra);
            Input const lethal = Solver::Detail::LethalOnly(in);
            CHECK(lethal.hazards.size() == 1 && lethal.hazards[0].marginYd >= ViewRules::kPetFollowYd);
            CHECK(Solver::Detail::CrossesLethal(lethal, { at, { 0.f, 0.5f, 0.f } }, kNow, kNow + Ms(10000)));
        }
    }

    // Stratholme F-2b (R-2-B for X4): the bots' own movement spells. The lock refuses a gap closer whose way to its
    // landing crosses a lethal wall (a warrior's Charge or Intercept at a boss across the Cordon, Heroic Leap,
    // Shadowstep), and a mover that lands away (Blink, Disengage) whose way does. The wall's strip is y 2..4.
    void TestMoverSpells()
    {
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        in.hazards.push_back(Wall(2, 3.f, true));
        Ms const until = kNow + in.params.horizon;
        Landing::Dest across;
        across.center = { 0.f, 15.f, 0.f };    // a boss north of the wall
        across.combatReachYd = 1.5f;
        Landing::Dest sameSide;
        sameSide.center = { 12.f, -6.f, 0.f };
        sameSide.combatReachYd = 1.5f;
        for (auto spot : { LockRules::LandingSpot::Reach, LockRules::LandingSpot::Center,
            LockRules::LandingSpot::Behind, LockRules::LandingSpot::CenterOrReach })
        {
            // Its landing is safe (past the wall), its way is not: the as-built landing test lets it through.
            CHECK(!Landing::LandingUnsafe(in, spot, across, in.bot, kNow, until)
                || spot == LockRules::LandingSpot::Behind);
            CHECK(Landing::CloserCrossesLethal(in, spot, across, in.bot, kNow, until));
            CHECK(!Landing::CloserCrossesLethal(in, spot, sameSide, in.bot, kNow, until));
        }
        CHECK(!Landing::CloserCrossesLethal(in, LockRules::LandingSpot::None, across, in.bot, kNow, until));
        // A plain wall is not policed here (the landing test covers it).
        Input plain = in;
        plain.hazards[0].lethal = false;
        CHECK(!Landing::CloserCrossesLethal(plain, LockRules::LandingSpot::Reach, across, plain.bot, kNow, until));

        // Away movers: the named actions go away from the current target, the spells along or against the facing.
        CHECK(LockRules::NamedMoverAway("blink back") == LockRules::AwayMove::AwayFromTarget);
        CHECK(LockRules::NamedMoverAway("disengage") == LockRules::AwayMove::AwayFromTarget);
        CHECK(LockRules::NamedMoverAway("hunter disengage") == LockRules::AwayMove::AwayFromTarget);
        CHECK(LockRules::NamedMoverAway("heroic leap") == LockRules::AwayMove::None);
        CHECK(LockRules::SpellEffectAway(true, false) == LockRules::AwayMove::Forward);
        CHECK(LockRules::SpellEffectAway(false, true) == LockRules::AwayMove::Back);
        CHECK(LockRules::SpellEffectAway(false, false) == LockRules::AwayMove::None);
        Vec3 const southBoss{ 0.f, -5.f, 0.f };     // a hunter's target south of it: Disengage carries it north
        Vec3 const awayNorth = Landing::AwayPoint(LockRules::AwayMove::AwayFromTarget, in.bot, 0.f, &southBoss);
        CHECK_NEAR(awayNorth.y, LockRules::kAwayMoverYd, 1e-3);
        CHECK(Landing::CrossesLethalWall(in, in.bot, awayNorth, kNow, until));
        Vec3 const northAdd{ 0.f, 1.f, 0.f };       // a target north of it: Blink back carries it south, clear
        Vec3 const awaySouth = Landing::AwayPoint(LockRules::AwayMove::AwayFromTarget, in.bot, 0.f, &northAdd);
        CHECK(!Landing::CrossesLethalWall(in, in.bot, awaySouth, kNow, until));
        // By the spell: facing north (pi/2) Blink crosses, Disengage (back, south) does not; along the wall neither.
        float const north = 1.5707964f;
        CHECK(Landing::CrossesLethalWall(in, in.bot, Landing::AwayPoint(LockRules::AwayMove::Forward, in.bot, north,
            nullptr), kNow, until));
        CHECK(!Landing::CrossesLethalWall(in, in.bot, Landing::AwayPoint(LockRules::AwayMove::Back, in.bot, north,
            nullptr), kNow, until));
        CHECK(!Landing::CrossesLethalWall(in, in.bot, Landing::AwayPoint(LockRules::AwayMove::Forward, in.bot, 0.f,
            nullptr), kNow, until));
        // On its target: against the facing.
        Vec3 const onIt = Landing::AwayPoint(LockRules::AwayMove::AwayFromTarget, in.bot, north, &in.bot);
        CHECK_NEAR(onIt.y, -LockRules::kAwayMoverYd, 1e-3);
        // No lethal hazard: never refused.
        CHECK(!Landing::CrossesLethalWall(plain, plain.bot, awayNorth, kNow, until));
    }

    // Cost: the Fallback's lethal screen walks every candidate's straight line against the lethal walls. Four streets
    // of the Cordon round a bot caught in a burning circle, 26 more circles about: the screen's share of a solve.
    void TestLethalCost()
    {
        Input in = FallbackScene(true);
        in.hazards.push_back(Wall(3, -9.f, true));
        in.hazards.push_back(MakeHazard(4, Shape::Line(40.f, 2.f), Pose{ 12.f, -20.f, 0.f, 1.5708f },
            Solver::MakeInterval(kNow - Ms(1000), Solver::kForever), true));
        in.hazards.push_back(MakeHazard(5, Shape::Line(40.f, 2.f), Pose{ -12.f, -20.f, 0.f, 1.5708f },
            Solver::MakeInterval(kNow - Ms(1000), Solver::kForever), true));
        for (std::uint32_t k = 0; k < 26; ++k)
        {
            float const a = 0.97f * static_cast<float>(k);
            float const r = 4.f + 0.6f * static_cast<float>(k);
            Pose const at{ r * std::cos(a), r * std::sin(a), 0.f, 0.f };
            in.hazards.push_back(MakeHazard(10 + k, Shape::Circle(3.f), at,
                Solver::MakeInterval(kNow + Ms(400), kNow + Ms(6000)), false));
        }
        in.role.present = true;             // a ranged bot's band round the boss: the role ring's candidates too
        in.role.anchor = { 0.f, -15.f, 0.f };
        in.role.minYd = 8.f;
        in.role.maxYd = 28.f;
        Input plain = in;
        for (Hazard& h : plain.hazards)
            h.lethal = false;
        auto bench = [](Input const& input, Plan& out)
        {
            constexpr int kRuns = 40;
            double total = 0.0;
            for (int r = 0; r < kRuns; ++r)
            {
                StraightOracle oracle;
                auto const t0 = std::chrono::steady_clock::now();
                out = Solver::Solve(input, oracle);
                total += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            }
            return total / kRuns;
        };
        Plan a, b;
        double const withLethal = bench(in, a);
        double const without = bench(plain, b);
        std::printf("lethal cost: %u candidates, %zu hazards (4 lethal), outcome %d, %u skipped: mean %.0f us "
            "(plain walls %.0f us; soft limit 2000 us)\n", a.candidates, in.hazards.size(),
            static_cast<int>(a.outcome), a.stats.lethalSkipped, withLethal, without);
        CHECK(a.outcome == Outcome::Fallback || a.outcome == Outcome::NoPath);
        CHECK(!PathCrossesBand(a.path, 2.f, 4.f));
    }
}

int main()
{
    TestLethalFits();
    TestLethalTiming();
    TestReducedMarginKeepsLethal();
    TestNoGrace();
    TestFallbackSkipsLethal();
    TestFallbackRefusesLethalPath();
    TestNoPathKeepsOut();
    TestFallbackHoldsRefusesLethal();
    TestLeavingTheBand();
    TestPetMargin();
    TestMoverSpells();
    TestLethalCost();
    return Report("lethal_test");
}
