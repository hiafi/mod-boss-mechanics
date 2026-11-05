// Standalone tests for the M4 bot statistics (STAGE-M4-HANDOFF §3.5-§3.7, WP-C): the leak detectors
// (src/MechanicLeaks.h), the cost counters (src/MechanicCost.h), the route-loop rule (src/MechanicRoute.h) and the
// solver's lane-frame counts and Fallback exposure (src/MechanicSolver.h). Build: tests/run.sh.

#include "check.h"
#include "MechanicCost.h"
#include "MechanicLeaks.h"
#include "MechanicRoute.h"
#include "MechanicSolver.h"
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace BossMechanics;
using Geo::Pose;
using Geo::Vec3;
using Leaks::Lock;
using Leaks::Sample;
using Leaks::Tracker;
using Leaks::Verdict;
using Ms = std::chrono::milliseconds;

namespace
{
    Ms const kNow{ 100000 };
    constexpr Ms kStep{ 100 };

    // A bot standing at the origin facing 0, nothing in flight, at time `at`.
    Sample Standing(Ms at, Lock lock = Lock::None)
    {
        Sample s;
        s.now = at;
        s.lock = lock;
        s.marginMs = Ms(300);
        return s;
    }

    Sample WithSpline(Sample s, std::uint32_t id, float destX, float destY, bool moves = true)
    {
        s.spline = true;
        s.splineId = id;
        s.splineMoves = moves;
        s.destX = destX;
        s.destY = destY;
        return s;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Moves
    // ------------------------------------------------------------------------------------------------------------

    void TestOwnSplinesNeverCount()
    {
        // Our escape: the move action's token (the id), the send in the same update, the move target, our last end.
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow));
            Leaks::OwnSpline(t, 10, 8.f, 0.f);
            Sample s = WithSpline(Standing(kNow + kStep, Lock::Escape), 10, 8.f, 0.f);
            CHECK(!Leaks::Observe(t, s).move);
            // A generator re-launched our move (new id, same end): ours, and adopted as the token.
            s = WithSpline(Standing(kNow + 2 * kStep, Lock::Escape), 11, 8.3f, 0.2f);
            s.x = 0.7f;
            CHECK(!Leaks::Observe(t, s).move);
            CHECK(t.ownSpline == 11);
            // The brain switched to Hold while our spline still runs (arrived within 0.6 yd): ours by id.
            s = WithSpline(Standing(kNow + 3 * kStep, Lock::Hold), 11, 8.3f, 0.2f);
            s.x = 1.4f;
            CHECK(!Leaks::Observe(t, s).move);
        }
        {
            // A partial path (ends short of the target) with no token yet: ours, the move action sent this update.
            Tracker t;
            Leaks::Observe(t, Standing(kNow));
            Sample s = WithSpline(Standing(kNow + kStep, Lock::Escape), 20, 3.f, 0.f);
            s.targetValid = true;
            s.targetX = 8.f;
            s.ourSendNow = true;
            CHECK(!Leaks::Observe(t, s).move);
            CHECK(t.ownSpline == 20);
            // Next update, same spline: ours by id.
            s.now += kStep;
            s.ourSendNow = false;
            CHECK(!Leaks::Observe(t, s).move);
        }
        {
            // Escape, no token, no send: a spline that ends at the move target is ours; elsewhere it is not.
            Tracker t;
            Leaks::Observe(t, Standing(kNow, Lock::Escape));
            Sample s = WithSpline(Standing(kNow + kStep, Lock::Escape), 30, 8.5f, 0.f);
            s.targetValid = true;
            s.targetX = 8.f;
            CHECK(!Leaks::Observe(t, s).move);
            s = WithSpline(Standing(kNow + 2 * kStep, Lock::Escape), 31, -6.f, 4.f);
            s.targetValid = true;
            s.targetX = 8.f;
            CHECK(Leaks::Observe(t, s).move);
        }
    }

    void TestForeignSplines()
    {
        // Stock follow already under way when the lock starts (the hold action stops it): a motion stop, no leak.
        {
            Tracker t;
            Leaks::Observe(t, WithSpline(Standing(kNow, Lock::None), 40, 10.f, 10.f));
            CHECK(!Leaks::Observe(t, WithSpline(Standing(kNow + kStep, Lock::Hold), 40, 10.f, 10.f)).move);
        }
        // A new foreign spline in Hold counts once, however long it runs; a second one counts again.
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow, Lock::Hold));
            Verdict v = Leaks::Observe(t, WithSpline(Standing(kNow + kStep, Lock::Hold), 50, 10.f, 0.f));
            CHECK(v.move);
            CHECK(v.moveSpell == 0);
            CHECK(!Leaks::Observe(t, WithSpline(Standing(kNow + 2 * kStep, Lock::Hold), 50, 10.f, 0.f)).move);
            CHECK(Leaks::Observe(t, WithSpline(Standing(kNow + 3 * kStep, Lock::Freeze), 51, -10.f, 0.f)).move);
        }
        // Not locked: Watch polices motion itself, nothing counts.
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow));
            CHECK(!Leaks::Observe(t, WithSpline(Standing(kNow + kStep), 60, 10.f, 0.f)).move);
        }
        // A facing-only spline (SetFacingTo launches one in place) is not a move.
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow, Lock::Hold));
            CHECK(!Leaks::Observe(t, WithSpline(Standing(kNow + kStep, Lock::Hold), 70, 0.f, 0.f, false)).move);
        }
    }

    void TestControlledAndJumps()
    {
        // A knockback or a fear (controlled slot) with no cast of the bot's: external, not a leak.
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow, Lock::Hold));
            Sample s = WithSpline(Standing(kNow + kStep, Lock::Hold), 80, 12.f, 0.f);
            s.controlled = true;
            CHECK(!Leaks::Observe(t, s).move);
        }
        // Charge: controlled slot too, but the bot cast it since the last check: a leak naming the spell.
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow, Lock::Escape));
            Leaks::SelfCast(t, 11578);
            Sample s = WithSpline(Standing(kNow + kStep, Lock::Escape), 81, 20.f, 0.f);
            s.controlled = true;
            Verdict const v = Leaks::Observe(t, s);
            CHECK(v.move);
            CHECK(v.moveSpell == 11578);
            CHECK(t.selfCast == 0);     // consumed
        }
        // Blink: no spline, the bot is 20 yd away 100 ms later, after its own cast.
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow, Lock::Hold));
            Leaks::SelfCast(t, 1953);
            Sample s = Standing(kNow + kStep, Lock::Hold);
            s.x = 20.f;
            Verdict const v = Leaks::Observe(t, s);
            CHECK(v.move);
            CHECK(v.moveSpell == 1953);
        }
        // The same jump with no cast of the bot's (a boss teleport): external.
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow, Lock::Hold));
            Sample s = Standing(kNow + kStep, Lock::Hold);
            s.x = 20.f;
            CHECK(!Leaks::Observe(t, s).move);
        }
        // A cast while running normally: a run step is no jump.
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow, Lock::Hold));
            Leaks::SelfCast(t, 48441);
            Sample s = Standing(kNow + kStep, Lock::Hold);
            s.x = 0.7f;
            CHECK(!Leaks::Observe(t, s).move);
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Casts
    // ------------------------------------------------------------------------------------------------------------

    Sample Casting(Sample s, std::uintptr_t key, std::uint32_t spell, Ms remaining)
    {
        s.casting = true;
        s.castKey = key;
        s.castSpell = spell;
        s.castRemaining = remaining;
        return s;
    }

    void TestCasts()
    {
        // Escape: any cast or channel that starts counts, once, with its spell id.
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow, Lock::Escape));
            Verdict v = Leaks::Observe(t, Casting(Standing(kNow + kStep, Lock::Escape), 0x1000, 42650, Ms(4000)));
            CHECK(v.cast);
            CHECK(v.castSpell == 42650);
            CHECK(!Leaks::Observe(t, Casting(Standing(kNow + 2 * kStep, Lock::Escape), 0x1000, 42650, Ms(3900)))
                .cast);
        }
        // Hold: a cast that ends before leaveBy - margin is allowed; one that ends after it leaks.
        {
            Tracker t;
            Sample s = Standing(kNow, Lock::Hold);
            s.leaveBy = kNow + Ms(3000);
            Leaks::Observe(t, s);
            s.now = kNow + kStep;
            CHECK(!Leaks::Observe(t, Casting(s, 0x2000, 585, Ms(2000))).cast);    // ends at 2100 <= 2700
            s.now = kNow + 2 * kStep;
            Verdict const v = Leaks::Observe(t, Casting(s, 0x2100, 686, Ms(3000)));    // ends at 3200 > 2700
            CHECK(v.cast);
            CHECK(v.castSpell == 686);
            // Within the rounding slack: not a leak.
            s.now = kNow + 3 * kStep;
            CHECK(!Leaks::Observe(t, Casting(s, 0x2200, 585, Ms(2400 + 40))).cast);
            // An endless channel against a finite leaveBy leaks; against none it does not.
            s.now = kNow + 4 * kStep;
            CHECK(Leaks::Observe(t, Casting(s, 0x2300, 5143, Solver::kForever)).cast);
            s.leaveBy = Solver::kForever;
            s.now = kNow + 5 * kStep;
            CHECK(!Leaks::Observe(t, Casting(s, 0x2400, 5143, Solver::kForever)).cast);
        }
        // Freeze: casting is what the bot may do.
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow, Lock::Freeze));
            CHECK(!Leaks::Observe(t, Casting(Standing(kNow + kStep, Lock::Freeze), 0x3000, 686, Ms(3000))).cast);
        }
        // A cast started in Watch is legitimate; the mode turning to Escape mid-cast (the waker breaks it) is no leak.
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow));
            CHECK(!Leaks::Observe(t, Casting(Standing(kNow + kStep), 0x4000, 686, Ms(3000))).cast);
            CHECK(!Leaks::Observe(t, Casting(Standing(kNow + 2 * kStep, Lock::Escape), 0x4000, 686, Ms(2900))).cast);
            // The cast ends; a new one in Escape is a leak.
            Leaks::Observe(t, Standing(kNow + 3 * kStep, Lock::Escape));
            CHECK(Leaks::Observe(t, Casting(Standing(kNow + 4 * kStep, Lock::Escape), 0x4000, 686, Ms(3000))).cast);
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Turns
    // ------------------------------------------------------------------------------------------------------------

    void TestTurns()
    {
        float const look = 1.5f;
        // Our face action turns the bot to the look-away facing: ours. A foreign turn away from it: a leak. Turning
        // back to our facing: ours again.
        {
            Tracker t;
            Sample s = Standing(kNow, Lock::Hold);
            s.lookAway = true;
            Leaks::Observe(t, s);
            Leaks::OwnFacing(t, look);
            s.now += kStep;
            s.orientation = look;
            CHECK(!Leaks::Observe(t, s).turn);
            s.now += kStep;
            s.orientation = -2.f;
            CHECK(Leaks::Observe(t, s).turn);
            s.now += kStep;
            s.orientation = look + Geo::kTwoPi;     // normalized differently, same facing
            CHECK(!Leaks::Observe(t, s).turn);
            // Small jitter within the tolerance: nothing.
            s.now += kStep;
            s.orientation = look + Geo::DegToRad(3.f);
            CHECK(!Leaks::Observe(t, s).turn);
        }
        // Outside a look-away window turning is allowed (the lock allows facing in every mode).
        {
            Tracker t;
            Leaks::Observe(t, Standing(kNow, Lock::Hold));
            Sample s = Standing(kNow + kStep, Lock::Hold);
            s.orientation = 2.f;
            CHECK(!Leaks::Observe(t, s).turn);
        }
        // Moving: a spline turns the bot along its path (ours, or a foreign one already counted as a move). The
        // first standing check after it does not count the turn the spline made either.
        {
            Tracker t;
            Sample s = Standing(kNow, Lock::Escape);
            s.lookAway = true;
            Leaks::Observe(t, s);
            Leaks::OwnSpline(t, 90, 6.f, 6.f);
            Sample m = WithSpline(s, 90, 6.f, 6.f);
            m.now += kStep;
            m.orientation = 0.8f;
            CHECK(!Leaks::Observe(t, m).turn);
            Sample h = Standing(kNow + 2 * kStep, Lock::Hold);
            h.lookAway = true;
            h.orientation = 0.9f;
            CHECK(!Leaks::Observe(t, h).turn);
            // Then a foreign turn while standing counts.
            h.now += kStep;
            h.orientation = -1.f;
            CHECK(Leaks::Observe(t, h).turn);
        }
        // Controlled (stunned, feared): not the bot's doing.
        {
            Tracker t;
            Sample s = Standing(kNow, Lock::Hold);
            s.lookAway = true;
            Leaks::Observe(t, s);
            s.now += kStep;
            s.orientation = 2.f;
            s.controlled = true;
            CHECK(!Leaks::Observe(t, s).turn);
        }
        // A look-away window in Watch (not locked) still counts turns.
        {
            Tracker t;
            Sample s = Standing(kNow);
            s.lookAway = true;
            Leaks::Observe(t, s);
            s.now += kStep;
            s.orientation = 2.f;
            CHECK(Leaks::Observe(t, s).turn);
        }
    }

    void TestLeakSpells()
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> spells;
        Leaks::AddSpell(spells, 686);
        Leaks::AddSpell(spells, 0);
        Leaks::AddSpell(spells, 42650);
        Leaks::AddSpell(spells, 686);
        CHECK(spells.size() == 2);
        CHECK(spells[0].first == 686 && spells[0].second == 2);
        CHECK(spells[1].first == 42650 && spells[1].second == 1);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Cost counters
    // ------------------------------------------------------------------------------------------------------------

    void TestHistogram()
    {
        CHECK(UsHistogram::BucketOf(0) == 0);
        CHECK(UsHistogram::BucketOf(1) == 0);
        CHECK(UsHistogram::BucketOf(2) == 1);
        CHECK(UsHistogram::BucketOf(3) == 1);
        CHECK(UsHistogram::BucketOf(4) == 2);
        CHECK(UsHistogram::BucketOf(1023) == 9);
        CHECK(UsHistogram::BucketOf(1024) == 10);
        CHECK(UsHistogram::BucketOf(32767) == 14);
        CHECK(UsHistogram::BucketOf(32768) == 15);
        CHECK(UsHistogram::BucketOf(4000000000u) == 15);
        for (std::uint32_t us : { 0u, 1u, 2u, 7u, 100u, 1999u, 2000u, 30000u })
        {
            std::size_t const b = UsHistogram::BucketOf(us);
            CHECK(us < UsHistogram::BucketUpper(b));
            CHECK(b == 0 || us >= UsHistogram::BucketUpper(b - 1));
        }

        UsHistogram h;
        CHECK(h.Quantile(0.5) == 0 && h.Mean() == 0);
        for (int i = 0; i < 90; ++i)
            h.Add(25);          // bucket 4: [16, 32)
        for (int i = 0; i < 10; ++i)
            h.Add(1500);        // bucket 10: [1024, 2048)
        CHECK(h.count == 100);
        CHECK(h.max == 1500);
        CHECK(h.total == 90u * 25u + 10u * 1500u);
        CHECK(h.Mean() == (90 * 25 + 10 * 1500) / 100);
        CHECK(h.Quantile(0.5) == 32);
        CHECK(h.Quantile(0.9) == 32);
        CHECK(h.Quantile(0.95) == 1500);    // the bucket bound 2048, capped at the exact max
        CHECK(h.Quantile(1.0) == 1500);
    }

    void TestPerMechanic()
    {
        CostStats c;
        c.AddMechanicPathCalls(7, 10);
        c.AddMechanicPathCalls(7, 6);
        c.AddMechanicPathCalls(8, 4);
        c.AddMechanicPathCalls(9, 30);
        std::uint32_t n = 0;
        std::uint32_t max = 0;
        double mean = 0.0;
        c.PerMechanic(n, max, mean);
        CHECK(n == 3 && max == 30);
        CHECK_NEAR(mean, 50.0 / 3.0, 1e-9);
        // 9 and 7 leave the registry: folded, still in the summary; 8 stays live.
        c.FoldGone([](std::uint32_t id) { return id != 8; });
        CHECK(c.livePathCalls.size() == 1);
        CHECK(c.mechanics == 2 && c.mechanicPathCallsMax == 30 && c.mechanicPathCallsTotal == 46);
        c.PerMechanic(n, max, mean);
        CHECK(n == 3 && max == 30);
        CHECK_NEAR(mean, 50.0 / 3.0, 1e-9);
        // A later mechanic with the same id after the fold starts fresh.
        c.AddMechanicPathCalls(7, 2);
        c.PerMechanic(n, max, mean);
        CHECK(n == 4);
        CHECK_NEAR(mean, 52.0 / 4.0, 1e-9);

        c.AddMapUpdate(100);
        c.AddMapUpdate(300);
        CHECK(c.mapUpdates == 2 && c.mapBrainUsMax == 300 && c.mapBrainUsTotal == 400);
        CostStats const zero;
        CHECK(zero.pathUs.count == 0 && zero.livePathCalls.empty() && zero.mapUpdates == 0);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Route loops
    // ------------------------------------------------------------------------------------------------------------

    void TestRouteLoop()
    {
        Vec3 const goal{ 10.f, 0.f, 0.f };
        CHECK(!Solver::IsRouteLoop(Ms(0), goal, kNow, goal));                                // no handoff yet
        CHECK(Solver::IsRouteLoop(kNow, goal, kNow + Ms(1000), goal));
        CHECK(Solver::IsRouteLoop(kNow, goal, kNow + Solver::kRouteLoopWindow, { 18.f, 4.f, 0.f }));
        CHECK(!Solver::IsRouteLoop(kNow, goal, kNow + Solver::kRouteLoopWindow + Ms(1), goal));  // too late
        CHECK(!Solver::IsRouteLoop(kNow, goal, kNow + Ms(1000), { 25.f, 0.f, 0.f }));           // another goal
        CHECK(!Solver::IsRouteLoop(kNow, goal, kNow - Ms(1), goal));
    }

    // ------------------------------------------------------------------------------------------------------------
    // Solver: lane-frame counts and the Fallback exposure
    // ------------------------------------------------------------------------------------------------------------

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
        std::uint32_t calls = 0;

        bool Find(Vec3 const&, Vec3 const&, std::vector<Vec3>&) override
        {
            ++calls;
            return false;
        }
    };

    Solver::Hazard Circle(std::uint32_t id, float r, float margin, Solver::Timing const& timing)
    {
        Solver::Hazard h;
        h.id = id;
        h.shape = Shape::Circle(r);
        h.poses = { Pose{} };
        h.marginYd = margin;
        h.timing = timing;
        return h;
    }

    Solver::Input InCircle(float r, float margin, std::int64_t inMs)
    {
        Solver::Input in;
        in.bot = { 0.f, 0.f, 0.f };
        in.now = kNow;
        in.speed = 7.f;
        in.hazards.push_back(Circle(1, r, margin, Solver::MakeInstant(kNow + Ms(inMs), Ms(500))));
        return in;
    }

    void TestLaneCounts()
    {
        // No lane frame: nothing counted, and Candidates leaves the flags alone when not asked.
        Solver::Input in = InCircle(5.f, 0.75f, 3000);
        StraightOracle o;
        Solver::Plan const plain = Solver::Solve(in, o);
        CHECK(plain.outcome == Solver::Outcome::Move);
        CHECK(plain.stats.laneCandidates == 0 && plain.stats.lanePathChecks == 0);

        // A lane frame with no lanes around the bot: its grid fills candidates; the path calls on them are a part
        // of the plan's.
        Solver::LaneFrame lf;
        lf.frame.x = -20.f;
        lf.frame.y = -20.f;
        lf.frame.sizeX = 40.f;
        lf.frame.sizeY = 40.f;
        in.lanes.push_back(lf);
        std::vector<std::uint8_t> flags;
        std::vector<Vec3> const cands = Solver::Candidates(in, &flags);
        CHECK(flags.size() == cands.size());
        CHECK(flags.empty() || flags[0] == 0);      // the bot's own spot
        std::uint32_t lanes = 0;
        for (std::uint8_t f : flags)
            lanes += f;
        CHECK(lanes > 0 && lanes < cands.size());
        std::vector<Vec3> const unflagged = Solver::Candidates(in);    // the flags change nothing
        bool same = unflagged.size() == cands.size();
        for (std::size_t i = 0; same && i < cands.size(); ++i)
            same = unflagged[i].x == cands[i].x && unflagged[i].y == cands[i].y && unflagged[i].z == cands[i].z;
        CHECK(same);

        StraightOracle o2;
        Solver::Plan const plan = Solver::Solve(in, o2);
        CHECK(plan.stats.laneCandidates == lanes);
        CHECK(plan.stats.lanePathChecks <= plan.pathChecks);
        CHECK(plan.pathChecks == o2.calls);

        // No path anywhere: the validation spends its budget down the score order, mostly on the lane grid, and the
        // NoPath answer's path call is counted too.
        NoPathOracle none;
        Solver::Plan const fb = Solver::Solve(in, none);
        CHECK(fb.outcome == Solver::Outcome::NoPath);
        CHECK(fb.pathChecks == none.calls);
        CHECK(fb.stats.lanePathChecks >= 1);
        CHECK(fb.stats.lanePathChecks <= fb.pathChecks);
        Solver::SolveStats sum;
        sum.Add(plan.stats);
        sum.Add(fb.stats);
        CHECK(sum.laneCandidates == plan.stats.laneCandidates + fb.stats.laneCandidates);
        CHECK(sum.lanePathChecks == plan.stats.lanePathChecks + fb.stats.lanePathChecks);
    }

    void TestFallbackExposure()
    {
        // Hot: a 20 yd circle that lands in 1 s on the bot at its center; nothing gets out in time.
        {
            Solver::Input in = InCircle(20.f, 0.75f, 1000);
            StraightOracle o;
            Solver::Plan const plan = Solver::Solve(in, o);
            CHECK(plan.outcome == Solver::Outcome::Fallback);
            CHECK(plan.exposure > Ms(0));
            CHECK(plan.exposure <= Ms(1000));       // the 500 ms window, widened by the walk's time step
        }
        // Clear: a 4 yd circle with a 3 yd margin, landing in 1 s. The margins (full and reduced) cannot be cleared
        // in time, so the outcome is a Fallback, but the exact hitbox is left before the window opens.
        {
            Solver::Input in = InCircle(4.f, 3.f, 1000);
            StraightOracle o;
            Solver::Plan const plan = Solver::Solve(in, o);
            CHECK(plan.outcome == Solver::Outcome::Fallback);
            CHECK(plan.exposure == Ms(0));
        }
        // Not a Fallback: no exposure measured.
        {
            Solver::Input in = InCircle(5.f, 0.75f, 3000);
            StraightOracle o;
            Solver::Plan const plan = Solver::Solve(in, o);
            CHECK(plan.outcome == Solver::Outcome::Move);
            CHECK(plan.exposure == Ms(0));
        }
    }
}

int main()
{
    TestOwnSplinesNeverCount();
    TestForeignSplines();
    TestControlledAndJumps();
    TestCasts();
    TestTurns();
    TestLeakSpells();
    TestHistogram();
    TestPerMechanic();
    TestRouteLoop();
    TestLaneCounts();
    TestFallbackExposure();
    return Report("bot_stats_test");
}
