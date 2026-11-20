// Standalone tests for src/MechanicHide.h, the Hide kind's pure rules (Blackrock Depths build WP-HK; hub BD1, library
// L31), its resolve filter rule (MechanicJudge.h) and the bots' way to a spot behind a blocker (MechanicSolver.h and
// MechanicRoute.h, as the view builds it: MechanicBotView.cpp HideKind). Build: tests/run.sh.

#include "check.h"
#include "MechanicDrill.h"
#include "MechanicHide.h"
#include "MechanicJudge.h"
#include "MechanicRoute.h"
#include "MechanicSolver.h"
#include "MechanicViewRules.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

using namespace BossMechanics;
using Geo::Pose;
using Geo::Vec3;
using Ms = std::chrono::milliseconds;

namespace
{
    constexpr double kEps = 1e-3;
    constexpr float kMargin = 0.75f;            // BossMechanics.Bots.SafetyMarginYd
    Ms const kNow{ 100000 };
    Ms const kHold{ 500 };                      // PostResolveHoldMs

    struct Lcg
    {
        std::uint64_t state = 0x4b1d3ull;

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

    // Bael'Gar's room as the spec builds it: the boss in the middle, three Boulders on a ring round him.
    std::vector<Hide::Blocker> Ring(Vec3 const& c, std::uint32_t n, float apart, float r, float rotation = 0.f)
    {
        std::vector<Hide::Blocker> out;
        for (std::uint32_t i = 0; i < n; ++i)
        {
            float const a = rotation + Geo::kTwoPi * static_cast<float>(i) / static_cast<float>(n);
            out.push_back({ { c.x + apart * std::cos(a), c.y + apart * std::sin(a), c.z }, r });
        }
        return out;
    }

    // The filter: a Hide hits players the source sees; a hidden player is spared; pets never count; the flag means
    // nothing to the other kinds.
    void TestFilter()
    {
        CHECK(Judge::PlayersOnly(Kind::Hide));
        CHECK(!Judge::IsShapeKind(Kind::Hide));     // never a hazard to dodge: the bots hide, the floor is not hot
        Judge::Candidate seen;
        CHECK(Judge::KeepResolveTarget(Kind::Hide, seen));
        Judge::Candidate hidden;
        hidden.hidden = true;
        CHECK(!Judge::KeepResolveTarget(Kind::Hide, hidden));
        Judge::Candidate pet;
        pet.player = false;
        CHECK(!Judge::KeepResolveTarget(Kind::Hide, pet));
        Judge::Candidate far;
        far.inside = false;                         // out of range
        CHECK(!Judge::KeepResolveTarget(Kind::Hide, far));
        Judge::Candidate other;
        other.eligible = false;
        CHECK(!Judge::KeepResolveTarget(Kind::Hide, other));
        Judge::Candidate gm;
        gm.gameMaster = true;
        CHECK(!Judge::KeepResolveTarget(Kind::Hide, gm));
        for (Kind k : { Kind::Zone, Kind::Frontal, Kind::Spread, Kind::RunOut, Kind::Bait, Kind::Jump })
            CHECK(Judge::KeepResolveTarget(k, hidden));
        Judge::Candidate air;
        air.airborne = true;
        CHECK(Judge::KeepResolveTarget(Kind::Hide, air));   // a jump does not hide anyone
    }

    void TestCapacity()
    {
        CHECK(Hide::Capacity(5, 3, 2) == 2);        // BD1: three Boulders for five, 2 + 2 + 1
        CHECK(Hide::Capacity(5, 2, 2) == 3);        // too few: they crowd behind the ones there are
        CHECK(Hide::Capacity(5, 1, 2) == 5);
        CHECK(Hide::Capacity(3, 3, 2) == 2);        // enough: at most `share`
        CHECK(Hide::Capacity(1, 3, 2) == 2);
        CHECK(Hide::Capacity(5, 6, 1) == 1);
        CHECK(Hide::Capacity(5, 0, 2) == 0);
        CHECK(Hide::Capacity(0, 3, 2) == 2);
    }

    void TestGeometry()
    {
        Vec3 const src{ 0.f, 0.f, 0.f };
        Hide::Blocker const b{ { 10.f, 0.f, 0.f }, 2.5f };
        Vec3 const s = Hide::SpotBehind(src, b, 3.f, 0.f);
        CHECK_NEAR(s.x, 15.5f, kEps);
        CHECK_NEAR(s.y, 0.f, kEps);
        Vec3 const l = Hide::SpotBehind(src, b, 3.f, 1.f);
        CHECK_NEAR(l.x, 15.5f, kEps);
        CHECK_NEAR(l.y, 1.f, kEps);                 // left of the line source -> blocker
        // A blocker north of the source: behind is farther north, left is west.
        Hide::Blocker const n{ { 0.f, 10.f, 0.f }, 2.f };
        Vec3 const ns = Hide::SpotBehind(src, n, 1.f, 1.f);
        CHECK_NEAR(ns.x, -1.f, kEps);
        CHECK_NEAR(ns.y, 13.f, kEps);

        // Slots: centred, at most 1.5 yd apart, within 80% of the blocker's width.
        CHECK_NEAR(Hide::SlotOffset(0, 1, 2.5f), 0.f, kEps);
        CHECK_NEAR(Hide::SlotOffset(0, 2, 2.5f), -0.75f, kEps);
        CHECK_NEAR(Hide::SlotOffset(1, 2, 2.5f), 0.75f, kEps);
        CHECK_NEAR(Hide::SlotOffset(0, 3, 2.5f), -1.5f, kEps);
        CHECK_NEAR(Hide::SlotOffset(1, 3, 2.5f), 0.f, kEps);
        CHECK_NEAR(Hide::SlotOffset(2, 3, 2.5f), 1.5f, kEps);
        CHECK_NEAR(Hide::SlotOffset(0, 5, 2.5f), -2.f, kEps);
        CHECK_NEAR(Hide::SlotOffset(4, 5, 2.5f), 2.f, kEps);
        CHECK_NEAR(Hide::SlotOffset(0, 3, 1.f), -0.8f, kEps);     // a narrow pillar packs them tighter

        // The plane shadow.
        CHECK(Hide::InShadow2d(src, b, { 14.f, 0.f, 0.f }));
        CHECK(Hide::InShadow2d(src, b, { 30.f, 4.f, 0.f }));     // the shadow widens behind the blocker
        CHECK(!Hide::InShadow2d(src, b, { 6.f, 0.f, 0.f }));     // in front of it
        CHECK(!Hide::InShadow2d(src, b, { 14.f, 4.f, 0.f }));    // beside it
        CHECK(!Hide::InShadow2d(src, b, { -14.f, 0.f, 0.f }));   // the other side of the source
        CHECK(!Hide::InShadow2d({ 9.f, 0.f, 0.f }, b, { 14.f, 0.f, 0.f }));   // a source inside the footprint
        // Every slot spot of a full blocker is in its shadow.
        for (std::uint8_t of = 1; of <= 5; ++of)
            for (std::uint8_t slot = 0; slot < of; ++slot)
                CHECK(Hide::InShadow2d(src, b, Hide::SpotBehind(src, b, 3.f, Hide::SlotOffset(slot, of, b.r))));

        // Candidates: the slot's spot first, then closer and farther, then straight behind the centre.
        std::vector<Vec3> const c = Hide::SpotCandidates(src, b, 3.f, 1.f);
        CHECK(c.size() == 6);
        CHECK_NEAR(c[0].x, 15.5f, kEps);
        CHECK_NEAR(c[1].x, 14.f, kEps);
        CHECK_NEAR(c[2].x, 17.f, kEps);
        CHECK_NEAR(c[3].y, 0.f, kEps);
        CHECK(Hide::SpotCandidates(src, b, 3.f, 0.f).size() == 3);

        CHECK(!Hide::NeedsReplace(3, 3, false, 1.9f));
        CHECK(Hide::NeedsReplace(3, 2, false, 0.f));         // a Boulder melted or fell in a crack
        CHECK(Hide::NeedsReplace(3, 3, true, 0.f));          // a player died or came in range
        CHECK(Hide::NeedsReplace(3, 3, false, 2.f));         // the tank dragged the boss
    }

    // Five players round Bael'Gar, three Boulders 8 yd out: everyone is placed, at most two a Boulder (2 + 2 + 1), in
    // the shadow, and the same answer twice.
    void TestAssignSpread()
    {
        Vec3 const src{ 0.f, 0.f, 0.f };
        std::vector<Hide::Blocker> const blockers = Ring(src, 3, 8.f, 2.5f, 0.3f);
        std::vector<Hide::Seeker> const seekers = { { 11, { 4.f, 1.f, 0.f } }, { 12, { -3.f, 3.f, 0.f } },
            { 13, { 2.f, -6.f, 0.f } }, { 14, { 20.f, 5.f, 0.f } }, { 15, { -10.f, -10.f, 0.f } } };
        Hide::Params const params;
        auto bad = Hide::PlaneBadness(src, blockers);
        std::vector<Hide::Placement> const a = Hide::Assign(src, blockers, seekers, params, bad);
        CHECK(a.size() == seekers.size());
        std::vector<std::uint32_t> per(blockers.size(), 0);
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            CHECK(a[i].guid == seekers[i].guid);
            CHECK(a[i].blocker < blockers.size());
            if (a[i].blocker >= blockers.size())
                continue;
            ++per[a[i].blocker];
            CHECK(a[i].badness == Hide::SpotBadness::Good);
            CHECK(Hide::InShadow2d(src, blockers[a[i].blocker], a[i].spot));
        }
        std::sort(per.begin(), per.end());
        CHECK(per[0] == 1 && per[1] == 2 && per[2] == 2);
        // Deterministic.
        std::vector<Hide::Placement> const again = Hide::Assign(src, blockers, seekers, params, bad);
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            CHECK(again[i].blocker == a[i].blocker);
            CHECK(again[i].slot == a[i].slot);
            CHECK_NEAR(again[i].spot.x, a[i].spot.x, kEps);
        }
        // The far player at (20, 5) hides behind the Boulder on its side (bearing 0.3 rad).
        CHECK(a[3].blocker == 0);
        // No two players share a spot.
        for (std::size_t i = 0; i < a.size(); ++i)
            for (std::size_t j = i + 1; j < a.size(); ++j)
                CHECK(Dist(a[i].spot, a[j].spot) > 1.f);
    }

    // Greedy alone can strand the last player: the cheapest pair first leaves it the far blocker. The moves and swaps
    // lower the worst walk first, then the total.
    void TestAssignBlockers()
    {
        std::vector<Hide::Seeker> const two = { { 1, {} }, { 2, {} } };
        // s1: A 1, B 2; s2: A 3, B 100; one each. Greedy: s1 A, s2 B (worst 100); best: s1 B, s2 A (worst 3).
        std::vector<std::uint32_t> const a = Hide::AssignBlockers({ 1.f, 2.f, 3.f, 100.f }, 2, 2, 1, two);
        CHECK(a[0] == 1 && a[1] == 0);
        // Equal worst: the lower total wins (s1 A 1 + s2 B 5 = 6 beats s1 B 5 + s2 A 5 = 10).
        std::vector<std::uint32_t> const b = Hide::AssignBlockers({ 1.f, 5.f, 5.f, 5.f }, 2, 2, 1, two);
        CHECK(b[0] == 0 && b[1] == 1);
        // Two at a time: s1 A 1 B 9, s2 A 2 B 3, s3 A 3 B 50, two a blocker. Greedy fills A with s1 and s2 and
        // strands s3 at B (50); the swaps reach s1 A, s2 B, s3 A (worst 3).
        std::vector<std::uint32_t> const c = Hide::AssignBlockers({ 1.f, 9.f, 2.f, 3.f, 3.f, 50.f }, 3, 2, 2,
            { { 1, {} }, { 2, {} }, { 3, {} } });
        CHECK(c[0] == 0 && c[1] == 1 && c[2] == 0);
    }

    // One Boulder left: all five behind it, side by side, all in its shadow. None: nobody placed.
    void TestAssignCrowd()
    {
        Vec3 const src{ 0.f, 0.f, 0.f };
        std::vector<Hide::Blocker> const one = { { { 8.f, 0.f, 0.f }, 2.5f } };
        std::vector<Hide::Seeker> seekers;
        for (std::uint64_t g = 1; g <= 5; ++g)
            seekers.push_back({ g, { 0.f, -10.f + 4.f * static_cast<float>(g), 0.f } });
        std::vector<Hide::Placement> const a = Hide::Assign(src, one, seekers, Hide::Params(),
            Hide::PlaneBadness(src, one));
        std::set<std::uint8_t> slots;
        for (Hide::Placement const& p : a)
        {
            CHECK(p.blocker == 0);
            CHECK(p.of == 5);
            CHECK(Hide::InShadow2d(src, one[0], p.spot));
            slots.insert(p.slot);
        }
        CHECK(slots.size() == 5);
        // Slots follow where the players come from: the south one (y -6) takes the rightmost slot (y < 0 behind a
        // blocker east of the source), the north one (y 10) the leftmost; nobody crosses.
        CHECK(a[0].spot.y < a[1].spot.y && a[1].spot.y < a[2].spot.y && a[2].spot.y < a[3].spot.y
            && a[3].spot.y < a[4].spot.y);

        std::vector<Hide::Placement> const none = Hide::Assign(src, {}, seekers, Hide::Params(),
            Hide::PlaneBadness(src, one));
        CHECK(none.size() == 5);
        for (Hide::Placement const& p : none)
            CHECK(p.blocker == Hide::kNoBlocker);
    }

    // The engine's verdicts steer the placement: a Boulder whose shadow the source sees over (a giant, a low rock)
    // is left for the others; a slot whose side is seen falls back to the spot straight behind; a spot on a lasting
    // crack moves closer or farther.
    void TestAssignBadness()
    {
        Vec3 const src{ 0.f, 0.f, 0.f };
        std::vector<Hide::Blocker> const blockers = Ring(src, 3, 8.f, 2.5f);
        std::vector<Hide::Seeker> const seekers = { { 1, { 9.f, 0.f, 0.f } }, { 2, { 7.f, 1.f, 0.f } },
            { 3, { -5.f, 5.f, 0.f } } };
        // Blocker 0 (east) is useless: every spot behind it is seen.
        auto seenEast = [&](Vec3 const& p)
        {
            if (p.x > 5.f)
                return Hide::SpotBadness::Seen;
            return Hide::InAnyShadow2d(src, blockers, p) ? Hide::SpotBadness::Good : Hide::SpotBadness::Seen;
        };
        std::vector<Hide::Placement> const a = Hide::Assign(src, blockers, seekers, Hide::Params(), seenEast);
        for (Hide::Placement const& p : a)
        {
            CHECK(p.blocker != 0);
            CHECK(p.badness == Hide::SpotBadness::Good);
        }

        // Only straight behind is hidden (a narrow shadow): both players behind one blocker end up on the centre
        // line, at different depths when they can.
        std::vector<Hide::Blocker> const one = { { { 8.f, 0.f, 0.f }, 2.5f } };
        auto narrow = [](Vec3 const& p)
        {
            return std::fabs(p.y) < 0.1f ? Hide::SpotBadness::Good : Hide::SpotBadness::Seen;
        };
        std::vector<Hide::Seeker> const two = { { 1, { 0.f, -5.f, 0.f } }, { 2, { 0.f, 5.f, 0.f } } };
        std::vector<Hide::Placement> const b = Hide::Assign(src, one, two, Hide::Params(), narrow);
        for (Hide::Placement const& p : b)
        {
            CHECK(p.badness == Hide::SpotBadness::Good);
            CHECK_NEAR(p.spot.y, 0.f, kEps);
        }

        // A lava crack across the 3 yd spot (x 13.5 to 14.5): the spot moves to half depth (x 12) or + 1.5 (x 15).
        auto crack = [&](Vec3 const& p)
        {
            if (p.x > 13.4f && p.x < 14.6f)
                return Hide::SpotBadness::OnLastingZone;
            return Hide::InShadow2d(src, one[0], p) ? Hide::SpotBadness::Good : Hide::SpotBadness::Seen;
        };
        std::vector<Hide::Placement> const c = Hide::Assign(src, one, { { 1, { 0.f, 4.f, 0.f } } }, Hide::Params(),
            crack);
        CHECK(c[0].badness == Hide::SpotBadness::Good);
        CHECK(c[0].spot.x < 13.4f || c[0].spot.x > 14.6f);

        // Everything is bad: the least bad spot is kept and reported.
        auto wall = [](Vec3 const&) { return Hide::SpotBadness::OffFloor; };
        std::vector<Hide::Placement> const d = Hide::Assign(src, one, { { 1, { 0.f, 4.f, 0.f } } }, Hide::Params(),
            wall);
        CHECK(d[0].blocker == 0);
        CHECK(d[0].badness == Hide::SpotBadness::OffFloor);
    }

    // The view's input for one bot: its spot as a region over the judge window, each blocker a small lasting hazard.
    Solver::Input HideInput(Vec3 const& bot, Vec3 const& spot, std::vector<Hide::Blocker> const& blockers,
        Ms resolveAt)
    {
        Solver::Input in;
        in.now = kNow;
        in.bot = bot;
        in.params.marginMs = Ms(300);
        Solver::Region r;
        r.id = 9;
        r.shape = Shape::Circle(Hide::kDefaultSpotRadiusYd);
        r.poses = { Pose{ spot.x, spot.y, spot.z, 0.f } };
        r.marginYd = ViewRules::RegionMarginYd(r.shape, kMargin);
        r.from = resolveAt;
        r.to = resolveAt + kHold;
        in.regions.push_back(r);
        for (Hide::Blocker const& b : blockers)
        {
            Solver::Hazard h;
            h.id = 9;
            h.shape = Shape::Circle(b.r);
            h.poses = { Pose{ b.c.x, b.c.y, b.c.z, 0.f } };
            h.marginYd = Hide::kBlockerMarginYd;
            h.timing = Solver::MakeInterval(kNow, resolveAt + kHold);
            h.synthetic = true;
            in.hazards.push_back(h);
        }
        return in;
    }

    // Where a bot walking `path` at `speed` from `start` stands at `t`.
    Vec3 AlongPath(std::vector<Vec3> const& path, float speed, Ms start, Ms t)
    {
        if (path.empty())
            return {};
        float left = speed * std::chrono::duration<float>(std::max(Ms(0), t - start)).count();
        for (std::size_t i = 1; i < path.size(); ++i)
        {
            float const leg = Dist(path[i - 1], path[i]);
            if (left <= leg && leg > 0.f)
            {
                float const f = left / leg;
                return { path[i - 1].x + (path[i].x - path[i - 1].x) * f,
                    path[i - 1].y + (path[i].y - path[i - 1].y) * f, path[i].z };
            }
            left -= leg;
        }
        return path.back();
    }

    // The closest the walked path comes to a blocker's centre (densified every 0.25 yd).
    float ClosestApproach(std::vector<Vec3> const& path, Hide::Blocker const& b)
    {
        float best = 1e9f;
        for (std::size_t i = 1; i < path.size(); ++i)
        {
            float const leg = Dist(path[i - 1], path[i]);
            std::uint32_t const steps = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(leg / 0.25f));
            for (std::uint32_t k = 0; k <= steps; ++k)
            {
                float const f = static_cast<float>(k) / static_cast<float>(steps);
                Vec3 const p{ path[i - 1].x + (path[i].x - path[i - 1].x) * f,
                    path[i - 1].y + (path[i].y - path[i - 1].y) * f, 0.f };
                best = std::min(best, Dist(p, b.c));
            }
        }
        return best;
    }

    // A bot one step to the source's side of a Boulder: its straight way to the spot behind runs through the rock.
    // The solve refuses that (the blocker is a hazard) and the region route goes round it; the bot arrives in time
    // and never walks into the footprint.
    void TestRouteRound()
    {
        Vec3 const src{ 0.f, 0.f, 0.f };
        std::vector<Hide::Blocker> const one = { { { 10.f, 0.f, 0.f }, 2.5f } };
        Ms const resolveAt = kNow + Ms(5000);
        Vec3 const spot = Hide::SpotBehind(src, one[0], Hide::kDefaultBehindYd, 0.f);
        Solver::Input const in = HideInput({ 6.f, 0.2f, 0.f }, spot, one, resolveAt);
        StraightOracle oracle;
        Solver::Plan plan = Solver::Solve(in, oracle);
        bool routed = false;
        if (plan.outcome != Solver::Outcome::Move)
        {
            CHECK(plan.violated == 1);      // the brain asks for the region route on the next step
            Solver::RouteParams rp;
            Solver::RouteStats rs;
            plan = Solver::RegionRoute(in, oracle, rp, &rs);
            for (std::uint32_t calls = 1; plan.outcome != Solver::Outcome::Move && rs.moreGoals
                && calls < Solver::kRegionRouteMaxGoals; ++calls)
                plan = Solver::RegionRoute(in, oracle, rp, &rs, rs.nextGoal);
            routed = true;
        }
        CHECK(plan.outcome == Solver::Outcome::Move);
        CHECK(plan.arriveAt <= resolveAt);
        CHECK(Dist(plan.target, spot) <= Hide::kDefaultSpotRadiusYd);
        CHECK(Hide::InShadow2d(src, one[0], plan.target));
        CHECK(ClosestApproach(plan.path, one[0]) >= one[0].r);
        std::printf("hide: route round a Boulder: %s, %zu path points, arrives %lld ms before the wave, closest "
            "%.2f yd from its centre\n", routed ? "region route" : "solve", plan.path.size(),
            static_cast<long long>((resolveAt - plan.arriveAt).count()), ClosestApproach(plan.path, one[0]));
    }

    // The whole bot side at once, many times: five bots at random spots within 20 yd of the boss, three Boulders on
    // an 8-12 yd ring, a 5 s cast. Each bot is placed (Assign), solves (and routes when the solve breaks its region),
    // walks its plan at run speed; at the resolve it must stand in its spot's region, in its Boulder's shadow, never
    // having walked through a rock.
    void TestHideRuns()
    {
        Lcg rng;
        constexpr std::uint32_t kRuns = 200;
        std::uint32_t hidden = 0, total = 0, routes = 0, throughRock = 0, late = 0;
        for (std::uint32_t run = 0; run < kRuns; ++run)
        {
            Vec3 const src{ 0.f, 0.f, 0.f };
            std::vector<Hide::Blocker> const blockers = Ring(src, 3, rng.Range(8.f, 12.f), 2.5f,
                rng.Range(0.f, Geo::kTwoPi));
            std::vector<Hide::Seeker> seekers;
            for (std::uint64_t g = 1; g <= 5; ++g)
            {
                Vec3 p;
                do
                {
                    float const a = rng.Range(0.f, Geo::kTwoPi);
                    float const d = rng.Range(2.f, 20.f);
                    p = { d * std::cos(a), d * std::sin(a), 0.f };
                } while (std::any_of(blockers.begin(), blockers.end(), [&p](Hide::Blocker const& b)
                {
                    return Dist(p, b.c) < b.r + 1.f;
                }));
                seekers.push_back({ g, p });
            }
            Ms const resolveAt = kNow + Ms(5000);
            std::vector<Hide::Placement> const a = Hide::Assign(src, blockers, seekers, Hide::Params(),
                Hide::PlaneBadness(src, blockers));
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                ++total;
                Solver::Input const in = HideInput(seekers[i].p, a[i].spot, blockers, resolveAt);
                StraightOracle oracle;
                Solver::Plan plan = Solver::Solve(in, oracle);
                if (plan.outcome != Solver::Outcome::Move && plan.outcome != Solver::Outcome::Safe)
                {
                    Solver::RouteParams rp;
                    Solver::RouteStats rs;
                    Solver::Plan route = Solver::RegionRoute(in, oracle, rp, &rs);
                    for (std::uint32_t calls = 1; route.outcome != Solver::Outcome::Move && rs.moreGoals
                        && calls < Solver::kRegionRouteMaxGoals; ++calls)
                        route = Solver::RegionRoute(in, oracle, rp, &rs, rs.nextGoal);
                    if (route.outcome == Solver::Outcome::Move)
                    {
                        plan = route;
                        ++routes;
                    }
                }
                std::vector<Vec3> path = plan.path;
                if (plan.outcome == Solver::Outcome::Safe || path.empty())
                    path = { seekers[i].p };
                Vec3 const at = AlongPath(path, in.speed, kNow + in.params.startDelay, resolveAt);
                bool const inRegion = Dist(at, a[i].spot) <= Hide::kDefaultSpotRadiusYd;
                bool const shadow = Hide::InAnyShadow2d(src, blockers, at);
                if (inRegion && shadow)
                    ++hidden;
                else
                    std::printf("  miss run %u bot %zu: outcome %d, from (%.1f, %.1f) to spot (%.1f, %.1f) %.1f yd, "
                        "arrives %+lld ms, at (%.1f, %.1f), region %d shadow %d, blocker %u slot %u/%u\n", run, i,
                        static_cast<int>(plan.outcome), seekers[i].p.x, seekers[i].p.y, a[i].spot.x, a[i].spot.y,
                        Dist(seekers[i].p, a[i].spot), static_cast<long long>((plan.arriveAt - resolveAt).count()),
                        at.x, at.y, inRegion, shadow, a[i].blocker, a[i].slot, a[i].of);
                if (plan.arriveAt > resolveAt)
                    ++late;
                for (Hide::Blocker const& b : blockers)
                    if (path.size() > 1 && ClosestApproach(path, b) < b.r - 0.05f)
                        ++throughRock;
            }
        }
        std::printf("hide: %u runs, %u bots: %u hidden at the wave, %u region routes, %u late plans, %u walks "
            "through a rock\n", kRuns, total, hidden, routes, late, throughRock);
        CHECK(hidden == total);
        CHECK(throughRock == 0);
        CHECK(late == 0);
    }

    // The drill row (`blackrock-depths-shapes`, cycle 0): Bael'Gar's heroic Molten Furnace as built in the spec (BG4:
    // a 5 s cast, 95%, no Weakened, three Boulders), in G1's reserved window 52.5-59.5 s with the Boulders gone by
    // then, and gated by the sweep's bar.
    void TestDrillRow()
    {
        Drill::Def const* d = Drill::Find("blackrock-depths-shapes");
        CHECK(d != nullptr);
        if (!d)
            return;
        std::uint32_t rows = 0;
        std::vector<std::optional<std::uint32_t>> const at0 = Drill::StepOffsets(*d, 0);
        std::vector<std::optional<std::uint32_t>> const at1 = Drill::StepOffsets(*d, 1);
        for (std::size_t i = 0; i < d->steps.size(); ++i)
        {
            std::string_view const line = d->steps[i].line;
            std::vector<std::string_view> const t = Drill::Tokens(line);
            if (t.empty() || t[0] != "hide")
                continue;
            ++rows;
            CHECK(Drill::Value(line, "tag") == std::optional<std::string_view>("16"));
            CHECK(Drill::kBrdTagHide == 16u && Drill::BrdBarTag(Drill::kBrdTagHide));
            CHECK(t.size() >= 3 && t[1] == "40" && t[2] == "5000");     // 40 yd of the boss, a 5 s cast
            CHECK(Drill::Value(line, "blockers") == std::optional<std::string_view>("3"));
            CHECK(Drill::Value(line, "pct") == std::optional<std::string_view>("95"));
            CHECK(Drill::Value(line, "school") == std::optional<std::string_view>("fire"));
            CHECK(!Drill::Value(line, "weak"));                    // BG4: a hit over 50% is its own penalty
            CHECK(Drill::DamageStep(line));
            CHECK(at0[i].has_value() && !at1[i].has_value());     // cycle 0 only
            if (!at0[i])
                continue;
            std::uint32_t const linger = static_cast<std::uint32_t>(
                Drill::Number(Drill::Value(line, "linger").value_or("3000")).value_or(0.f));
            CHECK(*at0[i] == 52500u);
            CHECK(*at0[i] + Drill::WarnMs(line) + linger <= 59500u);
            // Its Boulders: 10 yd round the boss, the default footprint, at most two players each for five.
            float const apart = Drill::Number(Drill::Value(line, "apart").value_or("8")).value_or(0.f);
            CHECK(apart - Hide::kDefaultBlockerRadiusYd > 2.f * Hide::kBlockerMarginYd);
            CHECK(Hide::Capacity(5, 3, Hide::kDefaultShare) == 2);
        }
        CHECK(rows == 1);
    }
}

int main()
{
    TestFilter();
    TestCapacity();
    TestGeometry();
    TestAssignSpread();
    TestAssignBlockers();
    TestAssignCrowd();
    TestAssignBadness();
    TestRouteRound();
    TestHideRuns();
    TestDrillRow();
    return Report("hide_test");
}
