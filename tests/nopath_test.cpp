// Standalone tests for the escape of a bot with no clean way out (Ragefire Chasm, Taragaman's plateau: two bots beside
// a notch in the floor were walked blind over its rim into the lava): the Fallback asks for a real path to more than
// one candidate, and NoPath walks only along ground and inside the hard bounds, else the bot stays where it is.
// Build: tests/run.sh.

#include "check.h"
#include "MechanicSolver.h"
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

    float Dist(Vec3 const& a, Vec3 const& b)
    {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    struct Rect
    {
        float x0, y0, x1, y1;

        bool Holds(Vec3 const& p) const { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; }
    };

    // Open floor with pits in it. A path is the straight line, when the floor carries it (a navmesh path round a pit
    // ends short of a far target: none); Straight is the navmesh ray.
    struct PitOracle : Solver::PathOracle
    {
        std::vector<Rect> pits;
        std::vector<Vec3> noPathTo;         // Find fails for these
        std::uint32_t finds = 0, lines = 0;

        bool OnFloor(Vec3 const& p) const
        {
            for (Rect const& r : pits)
                if (r.Holds(p))
                    return false;
            return true;
        }

        bool LineOnFloor(Vec3 const& a, Vec3 const& b) const
        {
            int const steps = std::max(1, static_cast<int>(std::ceil(Dist(a, b) / 0.1f)));
            for (int k = 0; k <= steps; ++k)
            {
                float const u = static_cast<float>(k) / static_cast<float>(steps);
                if (!OnFloor({ a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z }))
                    return false;
            }
            return true;
        }

        bool PathOnFloor(Plan const& plan) const
        {
            for (std::size_t i = 1; i < plan.path.size(); ++i)
                if (!LineOnFloor(plan.path[i - 1], plan.path[i]))
                    return false;
            return true;
        }

        bool Walkable(Vec3& p) override { return OnFloor(p); }

        bool Straight(Vec3 const& from, Vec3& to) override
        {
            ++lines;
            return LineOnFloor(from, to);
        }

        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            ++finds;
            for (Vec3 const& p : noPathTo)
                if (Dist(p, to) <= 0.2f)
                    return false;
            if (!LineOnFloor(from, to))
                return false;
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

    Hazard MakeHazard(std::uint32_t id, Shape const& shape, Pose const& pose, Solver::Timing const& timing,
        float drift = 0.f)
    {
        Hazard h;
        h.id = id;
        h.shape = shape;
        h.poses = { pose };
        h.marginYd = kMargin + drift;
        h.driftYd = drift;
        h.timing = timing;
        return h;
    }

    // A circle of fire the bot cannot leave in time (12 yd, lit in 300 ms), and two Stacks at once, so no spot passes
    // the validation: hazard-free candidates all round, none of them with a path yet when the Fallback ranks them.
    Input Infeasible(Vec3 bot)
    {
        Input in = MakeInput(bot);
        for (float x : { 60.f, -60.f })
        {
            Solver::Region r;
            r.id = x > 0.f ? 7 : 8;
            r.shape = Shape::Circle(4.f);
            r.poses = { Pose{ x, 0.f, 0.f, 0.f } };
            r.from = kNow + Ms(1000);
            r.to = kNow + Ms(9000);
            in.regions.push_back(r);
        }
        in.hazards.push_back(MakeHazard(1, Shape::Circle(12.f), Pose{},
            Solver::MakeInterval(kNow + Ms(300), kNow + Ms(8000))));
        return in;
    }

    // The notch: Felstorm's 8 yd ring on a boss still moving (7.9 yd of drift), the bot between the boss and a pit
    // whose far side is the only spot clear of the ring within reach; the fight's floor stops at the pit.
    Input NotchScene()
    {
        Input in = MakeInput({ 3.4f, -2.1f, 0.f });
        in.hazards.push_back(MakeHazard(22, Shape::Circle(8.f), Pose{},
            Solver::MakeInterval(kNow + Ms(2000), kNow + Ms(8000)), 7.9f));
        Solver::Bounds b;
        Solver::BoundsArea a;
        a.polygon = { { -30.f, -3.f, 0.f }, { 10.f, -3.f, 0.f }, { 10.f, 30.f, 0.f }, { -30.f, 30.f, 0.f } };
        b.areas.push_back(a);
        in.bounds.push_back(b);
        return in;
    }

    void TestNotch()
    {
        // Lava from the rim on; then an island across the lava, at the exit (a walkable end, no way to it).
        for (bool island : { false, true })
        {
            PitOracle oracle;
            oracle.pits = { Rect{ 5.f, -60.f, island ? 11.f : 60.f, 1.f }, Rect{ 2.f, -60.f, 5.f, -4.f } };
            Input const in = NotchScene();
            Plan const plan = Solver::Solve(in, oracle);
            CHECK(plan.outcome == Outcome::NoPath);             // the counters and the log keep their meaning
            CHECK(oracle.PathOnFloor(plan));
            CHECK(Dist(plan.target, in.bot) < 0.01f);           // nothing reachable: it stays
            CHECK(plan.answers.size() == 1 && plan.answers[0] == 22);
        }
        // The boss stopped (the next view): a clean way out over the floor, as before.
        PitOracle oracle;
        oracle.pits = { Rect{ 5.f, -60.f, 60.f, 1.f }, Rect{ 2.f, -60.f, 5.f, -4.f } };
        Input in = NotchScene();
        in.hazards[0].marginYd = kMargin;
        in.hazards[0].driftYd = 0.f;
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(plan.outcome == Outcome::Move);
        CHECK(oracle.PathOnFloor(plan));
    }

    // The ranking's first candidate has no path: the next one that has takes its place, along real ground.
    void TestSecondCandidate()
    {
        PitOracle open;
        Input const in = Infeasible({ 5.f, 0.f, 0.f });
        Plan const first = Solver::Solve(in, open);
        CHECK(first.outcome == Outcome::Fallback);

        PitOracle oracle;
        oracle.noPathTo = { first.target };
        Plan const plan = Solver::Solve(in, oracle);
        CHECK(plan.outcome == Outcome::Fallback);
        CHECK(Dist(plan.target, first.target) > 0.2f);
        CHECK(plan.path.size() == 2);
        CHECK(oracle.lines == 0);
        CHECK(plan.pathChecks == oracle.finds);

        // Two refused, a third taken.
        oracle.noPathTo.push_back(plan.target);
        Plan const third = Solver::Solve(in, oracle);
        CHECK(third.outcome == Outcome::Fallback);
        CHECK(Dist(third.target, plan.target) > 0.2f);

        // Hard bounds hold for the later candidates: one outside costs no path call, a path that ends or (pathInside)
        // runs outside is refused.
        Input bounded = in;
        Solver::Bounds b;
        Solver::BoundsArea a;
        a.polygon = { { -80.f, -2.f, 0.f }, { 80.f, -2.f, 0.f }, { 80.f, 40.f, 0.f }, { -80.f, 40.f, 0.f } };
        b.areas.push_back(a);
        bounded.bounds.push_back(b);
        struct AskedOracle : PitOracle
        {
            std::vector<Vec3> asked;
            Vec3 via, shift;
            bool bend = false;

            bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
            {
                asked.push_back(to);
                if (!PitOracle::Find(from, to, out))
                    return false;
                if (bend)
                    out.insert(out.begin() + 1, via);
                out.back() = { to.x + shift.x, to.y + shift.y, to.z };
                return true;
            }
        };
        Vec3 const top = Solver::Solve(bounded, open).target;
        AskedOracle inside;
        inside.noPathTo = { top };
        Plan const kept = Solver::Solve(bounded, inside);
        CHECK(kept.outcome == Outcome::Fallback);
        CHECK(kept.target.y >= -2.f);
        CHECK(inside.asked.size() >= 2);
        for (std::size_t k = 1; k < inside.asked.size(); ++k)
            CHECK(inside.asked[k].y >= -2.f);
        // Bounds that end short of the Stacks, whose spots rank next: none of them costs a path call.
        Input shorter = bounded;
        for (Vec3& p : shorter.bounds[0].areas[0].polygon)
            p.x *= 0.5f;
        AskedOracle few;
        few.noPathTo = { Solver::Solve(shorter, open).target };
        Solver::Solve(shorter, few);
        CHECK(!few.asked.empty());
        for (Vec3 const& p : few.asked)
            CHECK(std::fabs(p.x) <= 40.f);
        AskedOracle shifted;                    // every path ends 2.5 yd south of what was asked
        shifted.noPathTo = { top };
        shifted.shift = { 0.f, -2.5f, 0.f };
        CHECK(Solver::Solve(bounded, shifted).target.y >= -2.f);
        bounded.bounds[0].pathInside = true;
        AskedOracle round;                      // every path runs round outside
        round.noPathTo = { top };
        round.bend = true;
        round.via = { 5.f, -10.f, 0.f };
        Plan const out = Solver::Solve(bounded, round);
        CHECK(out.outcome == Outcome::NoPath);
        for (Vec3 const& p : out.path)
            CHECK(p.y >= -2.f);

        // A later path that ends 18 yd below its candidate (a partial one, cut at a rim) is no way.
        struct RimOracle : PitOracle
        {
            bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
            {
                if (!PitOracle::Find(from, to, out))
                    return false;
                out.back().z -= 18.f;
                return true;
            }
        } rim;
        rim.noPathTo = { first.target };
        Plan const low = Solver::Solve(in, rim);
        CHECK(low.outcome == Outcome::NoPath);
        CHECK(low.target.z > -1.f);
    }

    // No real path anywhere, some straight ways: the nearest exit the ground reaches, inside the hard bounds.
    void TestNearestGroundExit()
    {
        struct LinesOnly : PitOracle
        {
            bool Find(Vec3 const&, Vec3 const&, std::vector<Vec3>&) override
            {
                ++finds;
                return false;
            }
        };
        // A 6 yd wide lane along x through the bot: exits north and south of it, the nearer one north (over the pit).
        Input in = MakeInput({ 0.f, 1.f, 0.f });
        in.hazards.push_back(MakeHazard(1, Shape::Line(40.f, 6.f), Pose{ -20.f, 0.f, 0.f, 0.f },
            Solver::MakeInterval(kNow + Ms(100), kNow + Ms(8000))));
        in.params.maxPathChecks = 2;
        LinesOnly open;
        Plan const north = Solver::Solve(in, open);
        CHECK(north.outcome == Outcome::NoPath);
        CHECK(north.target.y > 3.f);

        LinesOnly pit;
        pit.pits = { Rect{ -60.f, 2.f, 60.f, 60.f } };
        Plan const south = Solver::Solve(in, pit);
        CHECK(south.outcome == Outcome::NoPath);
        CHECK(south.target.y < -3.f);
        CHECK(pit.PathOnFloor(south));

        // The bounds end on the south side as well: the nearest ground outside them is no exit.
        Solver::Bounds b;
        Solver::BoundsArea a;
        a.polygon = { { -60.f, -3.5f, 0.f }, { 60.f, -3.5f, 0.f }, { 60.f, 60.f, 0.f }, { -60.f, 60.f, 0.f } };
        b.areas.push_back(a);
        Input bounded = in;
        bounded.bounds.push_back(b);
        Plan const kept = Solver::Solve(bounded, pit);
        CHECK(kept.outcome == Outcome::NoPath);
        CHECK(kept.target.y > -3.5f);
        CHECK(pit.PathOnFloor(kept));

        // pathInside: an exit inside the bounds whose straight way leaves them (a U round the bot's south side).
        Input corner = in;
        Solver::Bounds u;
        u.pathInside = true;
        Solver::BoundsArea left = a;
        left.polygon = { { -9.f, -9.f, 0.f }, { 9.f, -9.f, 0.f }, { 9.f, -3.5f, 0.f }, { -9.f, -3.5f, 0.f } };
        Solver::BoundsArea spot = a;
        spot.polygon = { { -9.f, 0.f, 0.f }, { 9.f, 0.f, 0.f }, { 9.f, 2.f, 0.f }, { -9.f, 2.f, 0.f } };
        u.areas = { left, spot };
        corner.bounds.push_back(u);
        LinesOnly gap;
        Plan const stays = Solver::Solve(corner, gap);
        CHECK(stays.outcome == Outcome::NoPath);
        CHECK(Dist(stays.target, in.bot) < 0.01f);
        corner.bounds[0].pathInside = false;
        CHECK(Solver::Solve(corner, gap).target.y < -3.f);

        // A pit on every side: the bot stays.
        pit.pits.push_back(Rect{ -60.f, -60.f, 60.f, -1.f });
        pit.pits.push_back(Rect{ 3.f, -60.f, 60.f, 60.f });
        pit.pits.push_back(Rect{ -60.f, -60.f, -3.f, 60.f });
        pit.lines = 0;
        Plan const hold = Solver::Solve(in, pit);
        CHECK(hold.outcome == Outcome::NoPath);
        CHECK(Dist(hold.target, in.bot) < 0.01f);
        CHECK(pit.lines <= Solver::Detail::kExitLines);
        CHECK(hold.stats.exitLines == pit.lines && hold.stats.exitRefused >= 2);
    }

    // No hazard holds the bot (two Stacks, one within reach): NoPath has no exits, the chosen candidate is its last
    // try.
    void TestChosenCandidate()
    {
        struct Never : PitOracle
        {
            bool Find(Vec3 const&, Vec3 const&, std::vector<Vec3>&) override
            {
                ++finds;
                return false;
            }
        };
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        for (float x : { 4.5f, -60.f })
        {
            Solver::Region r;
            r.id = x > 0.f ? 7 : 8;
            r.shape = Shape::Circle(4.f);
            r.poses = { Pose{ x, 0.f, 0.f, 0.f } };
            r.from = kNow + Ms(3000);
            r.to = kNow + Ms(9000);
            in.regions.push_back(r);
        }
        Never open;
        Plan const walk = Solver::Solve(in, open);
        CHECK(walk.outcome == Outcome::NoPath);
        CHECK(Dist(walk.target, { 4.5f, 0.f, 0.f }) <= 4.f);
        Never pit;
        pit.pits = { Rect{ 0.2f, -60.f, 0.4f, 60.f } };
        Plan const hold = Solver::Solve(in, pit);
        CHECK(hold.outcome == Outcome::NoPath);
        CHECK(Solver::IsNoWayOut(hold, in.bot));
    }

    // Many hazards hold the bot and no exit is on ground: the lines asked are capped.
    void TestExitCap()
    {
        struct Never : PitOracle
        {
            bool Find(Vec3 const&, Vec3 const&, std::vector<Vec3>&) override { return false; }
        } island;
        island.pits = { Rect{ 1.f, -60.f, 60.f, 60.f }, Rect{ -60.f, -60.f, -1.f, 60.f },
            Rect{ -60.f, 1.f, 60.f, 60.f }, Rect{ -60.f, -60.f, 60.f, -1.f } };
        Input in = MakeInput({ 0.f, 0.f, 0.f });
        for (std::uint32_t i = 0; i < 30; ++i)
            in.hazards.push_back(MakeHazard(1 + i, Shape::Circle(8.f), Pose{ std::cos(i * 0.2f) * 2.f,
                std::sin(i * 0.2f) * 2.f, 0.f, 0.f }, Solver::MakeInterval(kNow + Ms(100), kNow + Ms(8000))));
        Plan const plan = Solver::Solve(in, island);
        CHECK(plan.outcome == Outcome::NoPath);
        CHECK(Solver::IsNoWayOut(plan, in.bot));
        CHECK(island.lines == Solver::Detail::kExitLines);
    }

    // The core's PathGenerator: a target over a pit comes back as a two-point path to exactly it (its end is not
    // clamped to the polygon); NavOracle::Find refuses that hop (the mesh does not carry it). Without the refusal the
    // last-chance Fallback (every probe failed) walks off the floor, so it is Find that has to hold.
    void TestCoreFind()
    {
        struct CoreOracle : PitOracle
        {
            bool hopCheck = true;

            bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
            {
                ++finds;
                if (hopCheck && !LineOnFloor(from, to))
                    return false;
                out = { from, to };
                return true;
            }
        };
        for (bool check : { false, true })
        {
            CoreOracle oracle;
            oracle.hopCheck = check;
            oracle.pits = { Rect{ 5.f, -60.f, 60.f, 1.f }, Rect{ 2.f, -60.f, 5.f, -4.f } };
            Plan const plan = Solver::Solve(NotchScene(), oracle);
            CHECK(oracle.PathOnFloor(plan) == check);
            CHECK(plan.outcome == (check ? Outcome::NoPath : Outcome::Fallback));
            CHECK(Solver::IsNoWayOut(plan, NotchScene().bot) == check);
        }
    }

    // A path the passes found (and refused as a clean one) is judged like the rest when it comes after a failed one.
    void TestKnownLaterPath()
    {
        struct OneLow : PitOracle
        {
            bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
            {
                if (++finds > 1)
                    return false;
                out = { from, { to.x, to.y, to.z - 18.f } };
                return true;
            }
        };
        std::uint32_t later = 0;
        for (int k = 0; k < 64; ++k)
        {
            Input in = MakeInput({ 5.f, 0.f, 0.f });
            in.hazards.push_back(MakeHazard(1, Shape::Circle(12.f), Pose{},
                Solver::MakeInterval(kNow + Ms(300), kNow + Ms(8000))));
            in.hazards[0].marginYd = 0.1f;      // one pass, one path call in it: every other Find is the Fallback's
            in.params.maxPathChecks = 1;
            in.params.hazardEdgeArcs = false;
            float const b = Geo::kTwoPi * static_cast<float>(k) / 64.f;
            in.role.present = true;
            in.role.anchor = { std::cos(b) * 18.f, std::sin(b) * 18.f, 0.f };
            in.role.maxYd = 3.f;
            OneLow low;
            Plan const plan = Solver::Solve(in, low);
            if (low.finds < 2)
                continue;       // the known path came first in the ranking: taken as it always was
            ++later;
            CHECK(plan.target.z > -1.f);
        }
        CHECK(later > 0);
    }

    // Path calls: never past the passes' old worst case (three passes and one), however many fail; deterministic.
    void TestBudget()
    {
        for (std::uint32_t cap : { 1u, 3u, 8u })
        {
            Input in = Infeasible({ 5.f, 0.f, 0.f });
            in.params.maxPathChecks = cap;
            struct Never : PitOracle
            {
                bool Find(Vec3 const&, Vec3 const&, std::vector<Vec3>&) override
                {
                    ++finds;
                    return false;
                }
            } none;
            Plan const a = Solver::Solve(in, none);
            CHECK(a.pathChecks == none.finds);
            CHECK(none.finds > 1);      // more than the first candidate was asked
            CHECK(none.finds <= 3 * cap + 1);
            Plan const b = Solver::Solve(in, none);
            CHECK(a.outcome == b.outcome && a.target.x == b.target.x && a.target.y == b.target.y
                && a.pathChecks == b.pathChecks);
        }
    }

    // Plans that need neither are what they were: the new line is never asked.
    void TestUnchanged()
    {
        for (int scene = 0; scene < 3; ++scene)
        {
            Input in = scene == 0 ? Infeasible({ 5.f, 0.f, 0.f }) : MakeInput({ 2.f, 0.f, 0.f });
            if (scene == 1)
                in.hazards.push_back(MakeHazard(1, Shape::Circle(6.f), Pose{},
                    Solver::MakeInterval(kNow + Ms(2500), kNow + Ms(8000))));
            PitOracle oracle;
            Plan const plan = Solver::Solve(in, oracle);
            CHECK(plan.outcome == (scene == 0 ? Outcome::Fallback : (scene == 1 ? Outcome::Move : Outcome::Safe)));
            CHECK(oracle.lines == 0);
            CHECK(plan.stats.exitLines == 0 && plan.stats.noPath == 0);
            CHECK(oracle.finds == (scene == 0 ? 1u : plan.pathChecks));
        }
    }

    // X4's loop is as it was (its own tries, its own path screen), and its NoPath keeps to the ground as well.
    void TestLethal()
    {
        auto scene = []
        {
            Input in = MakeInput({ 0.f, 0.f, 0.f });
            in.hazards.push_back(MakeHazard(1, Shape::Circle(10.f), Pose{ 0.f, -4.f, 0.f, 0.f },
                Solver::MakeInterval(kNow + Ms(300), Solver::kForever)));
            Hazard wall = MakeHazard(2, Shape::Line(60.f, 2.f), Pose{ -30.f, 3.f, 0.f, 0.f },
                Solver::MakeInterval(kNow - Ms(1000), Solver::kForever));
            wall.lethal = true;
            in.hazards.push_back(wall);
            return in;
        };
        PitOracle open;
        Plan const first = Solver::Solve(scene(), open);
        CHECK(first.outcome == Outcome::Fallback);
        CHECK(first.target.y < 2.f);
        CHECK(open.lines == 0);

        struct Never : PitOracle
        {
            bool Find(Vec3 const&, Vec3 const&, std::vector<Vec3>&) override
            {
                ++finds;
                return false;
            }
        };
        Never none;
        Plan const line = Solver::Solve(scene(), none);
        CHECK(line.outcome == Outcome::NoPath);
        CHECK(line.target.y < 2.f);             // never across the wall

        Never pit;                              // a pit all round: it stays
        pit.pits = { Rect{ -60.f, -60.f, 60.f, -1.f }, Rect{ 2.f, -60.f, 60.f, 60.f },
            Rect{ -60.f, -60.f, -2.f, 60.f } };
        Plan const hold = Solver::Solve(scene(), pit);
        CHECK(hold.outcome == Outcome::NoPath);
        CHECK(pit.PathOnFloor(hold));
        CHECK(hold.target.y < 2.f);

        // A fire lane on the bot, the lethal wall well south of it: both exits are free of the wall, the nearer one
        // (the way it went before) lies over a pit.
        Input lane = MakeInput({ 0.f, 1.f, 0.f });
        lane.hazards.push_back(MakeHazard(1, Shape::Line(40.f, 6.f), Pose{ -20.f, 0.f, 0.f, 0.f },
            Solver::MakeInterval(kNow + Ms(100), kNow + Ms(8000))));
        Hazard south = MakeHazard(2, Shape::Line(60.f, 2.f), Pose{ -30.f, -9.f, 0.f, 0.f },
            Solver::MakeInterval(kNow - Ms(1000), Solver::kForever));
        south.lethal = true;
        lane.hazards.push_back(south);
        Never open2;
        CHECK(Solver::Solve(lane, open2).target.y > 3.f);
        Never north;
        north.pits = { Rect{ -60.f, 2.f, 60.f, 60.f } };
        Plan const away = Solver::Solve(lane, north);
        CHECK(away.outcome == Outcome::NoPath);
        CHECK(away.target.y < -3.f && away.target.y > -8.f);
        CHECK(north.PathOnFloor(away));

        // The ground moves the exit into the wall's way: refused (the nearer exit, snapped 1.5 yd south).
        struct SnapOracle : Never
        {
            bool Straight(Vec3 const& from, Vec3& to) override
            {
                if (to.y > 0.f)
                    return false;
                to.y -= 1.5f;
                return Never::Straight(from, to);
            }
        } snap;
        lane.hazards[1].poses[0].y = -6.9f;     // its edge and margin 0.4 yd past the south exit, not past the snap
        Plan const refused = Solver::Solve(lane, snap);
        CHECK(refused.outcome == Outcome::NoPath);
        CHECK(Solver::IsNoWayOut(refused, lane.bot));
    }

    void Benchmark()
    {
        struct Never : PitOracle
        {
            bool Find(Vec3 const&, Vec3 const&, std::vector<Vec3>&) override { return false; }
        } none;
        none.pits = { Rect{ 2.f, -60.f, 60.f, 60.f } };
        Input in = Infeasible({ 0.f, 0.f, 0.f });
        for (std::uint32_t i = 0; i < 20; ++i)
            in.hazards.push_back(MakeHazard(100 + i, Shape::Circle(4.f), Pose{ -10.f + i, 6.f - i * 0.5f, 0.f, 0.f },
                Solver::MakeInterval(kNow + Ms(200), kNow + Ms(6000))));
        int const runs = 200;
        std::uint32_t sink = 0;
        auto const t0 = std::chrono::steady_clock::now();
        Plan last;
        for (int i = 0; i < runs; ++i)
        {
            last = Solver::Solve(in, none);
            sink += last.candidates;
        }
        double const mean = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count()
            / runs;
        CHECK(sink > 0);
        std::printf("no path, %zu hazards: outcome %d, path checks %u, exit lines %u: mean %.0f us (soft limit "
            "2000 us)\n", in.hazards.size(), static_cast<int>(last.outcome), last.pathChecks, last.stats.exitLines,
            mean);
    }
}

int main()
{
    TestNotch();
    TestSecondCandidate();
    TestNearestGroundExit();
    TestChosenCandidate();
    TestExitCap();
    TestCoreFind();
    TestKnownLaterPath();
    TestBudget();
    TestUnchanged();
    TestLethal();
    Benchmark();
    return Report("nopath_test");
}
