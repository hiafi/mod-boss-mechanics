// Standalone tests for src/MechanicMath.h (STAGE-M1-HANDOFF §6.1). Build: tests/run.sh.

#include "check.h"
#include "MechanicMath.h"
#include <map>
#include <random>
#include <set>
#include <tuple>

using namespace BossMechanics;

namespace
{
    // Simulates the resolve sweep at a fixed tick and returns, per sampled point, the passes that hit it and the
    // time of each hit (ms since resolveAt). Mirrors Registry: one hit per pass per unit.
    struct SweepResult
    {
        std::map<int, std::map<std::uint32_t, std::int64_t>> hits;     // point index -> pass -> hit time
    };

    SweepResult Sweep(ProjectileParams const& p, std::int64_t tickMs, float step)
    {
        SweepResult r;
        std::int64_t const duration = ProjectileDuration(p).count();
        std::int64_t prev = 0;
        for (std::int64_t now = 0; now <= duration + tickMs; now += tickMs)
        {
            for (SweepBand const& b : SweptBands(p, Ms(prev), Ms(now)))
            {
                int idx = 0;
                for (float s = 0.f; s <= p.length + 1e-4f; s += step, ++idx)
                    if (s >= b.lo && s <= b.hi && !r.hits[idx].count(b.pass))
                        r.hits[idx][b.pass] = now;
            }
            prev = now;
        }
        return r;
    }

    void TestProjectileWindows()
    {
        ProjectileParams p;
        p.length = 30.f;
        p.speed = 10.f;
        p.halfLength = 1.5f;
        CHECK(ProjectileDuration(p) == Ms(3150));
        std::vector<HotWindow> w = ProjectileHotWindows(p, 15.f);
        CHECK(w.size() == 1);
        CHECK(w[0].from == Ms(1350));
        CHECK(w[0].to == Ms(1650));
        w = ProjectileHotWindows(p, 0.5f);
        CHECK(w[0].from == Ms(0));
        CHECK(w[0].to == Ms(200));

        p.outAndBack = true;
        p.pause = Ms(500);
        // tOut = 3 s, return starts at 3.5 s, done at 3.5 + 3.15.
        CHECK(ProjectileDuration(p) == Ms(6650));
        w = ProjectileHotWindows(p, 10.f);
        CHECK(w.size() == 2);
        CHECK(w[0].from == Ms(850));
        CHECK(w[0].to == Ms(1150));
        CHECK(w[0].pass == 0);
        CHECK(w[1].from == Ms(3500 + 1850));
        CHECK(w[1].to == Ms(3500 + 2150));
        CHECK(w[1].pass == 1);
        // Near the far end the point stays hot through the pause.
        w = ProjectileHotWindows(p, 29.f);
        CHECK(w[0].from == Ms(2750));
        CHECK(w[0].to == Ms(3500));
        CHECK(w[1].from == Ms(3500));
        CHECK(w[1].to == Ms(3500 + 250));

        // Centre positions.
        std::uint32_t pass = 9;
        CHECK_NEAR(ProjectileCentre(p, 1.f, &pass), 10.f, 1e-4);
        CHECK(pass == 0);
        CHECK_NEAR(ProjectileCentre(p, 3.2f, &pass), 30.f, 1e-4);
        CHECK(pass == 0);
        CHECK_NEAR(ProjectileCentre(p, 4.5f, &pass), 20.f, 1e-4);
        CHECK(pass == 1);
    }

    void TestProjectileSweep()
    {
        for (bool outAndBack : { false, true })
        {
            for (std::int64_t tick : { 50, 400, 1700 })
            {
                ProjectileParams p;
                p.length = 30.f;
                p.speed = 10.f;
                p.halfLength = 1.5f;
                p.outAndBack = outAndBack;
                p.pause = Ms(500);
                SweepResult const r = Sweep(p, tick, 0.25f);
                bool allHitEveryPass = true;
                bool timely = true;
                for (int idx = 0; idx <= 120; ++idx)
                {
                    float const s = idx * 0.25f;
                    auto it = r.hits.find(idx);
                    std::size_t const expectedPasses = outAndBack ? 2 : 1;
                    if (it == r.hits.end() || it->second.size() != expectedPasses)
                    {
                        allHitEveryPass = false;
                        continue;
                    }
                    // The hit lands no earlier than the body reaches the point and no later than one tick after.
                    std::vector<HotWindow> const w = ProjectileHotWindows(p, s);
                    for (HotWindow const& hw : w)
                    {
                        std::int64_t const at = it->second.at(hw.pass);
                        if (at < hw.from.count() - 1 || at > hw.to.count() + tick)
                            timely = false;
                    }
                }
                CHECK(allHitEveryPass);
                CHECK(timely);
            }
        }

        // A unit stepping in behind the projectile is not in any later band.
        ProjectileParams p;
        p.length = 30.f;
        p.speed = 10.f;
        p.halfLength = 1.5f;
        bool behindHit = false;
        std::int64_t prev = 1000;
        for (std::int64_t now = 1050; now <= 3200; now += 50)
        {
            for (SweepBand const& b : SweptBands(p, Ms(prev), Ms(now)))
                if (5.f >= b.lo && 5.f <= b.hi)
                    behindHit = true;     // s = 5 was passed at 0.35-0.65 s, before this loop starts
            prev = now;
        }
        CHECK(!behindHit);

        // Instant line (speed 0): one band over the whole line.
        ProjectileParams instant;
        instant.length = 10.f;
        instant.halfLength = 1.f;
        std::vector<SweepBand> const b = SweptBands(instant, Ms(0), Ms(0));
        CHECK(b.size() == 1);
        CHECK(b[0].lo <= 0.f && b[0].hi >= 10.f);
    }

    void TestTrackedSweep()
    {
        std::uint32_t pass = 0;
        std::vector<SweepBand> b = TrackedSweep(0.f, 5.f, 1.f, true, pass);
        CHECK(pass == 0);
        CHECK(b.size() == 1);
        CHECK_NEAR(b[0].lo, -1.f, 1e-6);
        CHECK_NEAR(b[0].hi, 6.f, 1e-6);
        b = TrackedSweep(20.f, 17.f, 1.f, true, pass);
        CHECK(pass == 1);
        CHECK(b.size() == 2);
        CHECK(b[0].pass == 0);
        CHECK(b[1].pass == 1);
        CHECK_NEAR(b[1].lo, 16.f, 1e-6);
        std::uint32_t single = 0;
        TrackedSweep(20.f, 17.f, 1.f, false, single);
        CHECK(single == 0);
    }

    void TestMine()
    {
        using Mine = MineTimer<int>;
        using Step = Mine::Step;
        std::vector<int> const none;
        std::vector<int> const one{ 1 };

        // Unit inside at arm: detonates at once.
        Mine m;
        m.armAt = Ms(3000);
        m.grace = Ms(500);
        CHECK(m.Update(Ms(2950), one) == Step::None);
        CHECK(m.Update(Ms(3000), one) == Step::Detonate);
        CHECK(m.spent);
        CHECK(m.Update(Ms(3050), one) == Step::None);

        // A 0.4 s clip spares at 50 ms and at 40 ms ticks.
        for (std::int64_t tick : { 40, 50 })
        {
            Mine c;
            c.armAt = Ms(0);
            c.grace = Ms(500);
            CHECK(c.Update(Ms(0), none) == Step::Armed);
            bool detonated = false;
            for (std::int64_t t = tick; t <= 2000; t += tick)
            {
                bool const in = t >= 1000 && t < 1400;
                if (c.Update(Ms(t), in ? one : none) == Step::Detonate)
                    detonated = true;
            }
            CHECK(!detonated);
        }

        // 0.5 s inside detonates.
        Mine s;
        s.armAt = Ms(0);
        s.grace = Ms(500);
        s.Update(Ms(0), none);
        std::int64_t detonatedAt = -1;
        for (std::int64_t t = 50; t <= 3000 && detonatedAt < 0; t += 50)
            if (s.Update(Ms(t), t >= 1000 ? one : none) == Step::Detonate)
                detonatedAt = t;
        CHECK(detonatedAt == 1500);

        // Leaving resets: 0.3 s in, out, 0.3 s in again -> no detonation.
        Mine r;
        r.armAt = Ms(0);
        r.grace = Ms(500);
        r.Update(Ms(0), none);
        bool det = false;
        for (std::int64_t t = 50; t <= 3000; t += 50)
        {
            bool const in = (t >= 1000 && t < 1300) || (t >= 1400 && t < 1700);
            if (r.Update(Ms(t), in ? one : none) == Step::Detonate)
                det = true;
        }
        CHECK(!det);
        CHECK(r.insideSince.empty());

        // Two units: each has its own timer.
        Mine two;
        two.armAt = Ms(0);
        two.grace = Ms(500);
        two.Update(Ms(0), none);
        CHECK(two.Update(Ms(100), { 1 }) == Step::None);
        CHECK(two.Update(Ms(400), { 1, 2 }) == Step::None);
        CHECK(two.Update(Ms(550), { 2 }) == Step::None);    // 1 left after 0.45 s
        CHECK(two.Update(Ms(900), { 2 }) == Step::Detonate);

        // Cap: FIFO, the oldest fizzles.
        std::vector<std::pair<Ms, std::uint32_t>> live{ { Ms(300), 7 }, { Ms(100), 5 }, { Ms(200), 6 } };
        std::vector<std::uint32_t> const out = CapOverflow(live, 2);
        CHECK(out.size() == 1 && out[0] == 5);
        CHECK(CapOverflow(live, 3).empty());
        CHECK(CapOverflow(live, 0).empty());
        std::vector<std::uint32_t> const out1 = CapOverflow(live, 1);
        CHECK(out1.size() == 2 && out1[0] == 5 && out1[1] == 6);
    }

    void TestLanes()
    {
        // Gaps >= minGap for 1,000 seeds.
        bool gapsOk = true;
        bool inside = true;
        bool varied = false;
        float firstOffset = -1.f;
        for (std::uint32_t seed = 1; seed <= 1000; ++seed)
        {
            std::mt19937 rng(seed);
            std::uniform_real_distribution<float> dist(0.f, 1.f);
            auto rnd = [&]() { return dist(rng); };
            bool fellBack = true;
            std::vector<float> const o = RandomLaneOffsets(40.f, 3, 6.f, 6.f, rnd, &fellBack);
            if (fellBack || o.size() != 3)
                gapsOk = false;
            for (std::size_t i = 0; i < o.size(); ++i)
            {
                if (o[i] < -1e-4f || o[i] + 6.f > 40.f + 1e-4f)
                    inside = false;
                if (i > 0 && o[i] - o[i - 1] - 6.f < 6.f - 1e-4f)
                    gapsOk = false;
            }
            if (firstOffset < 0.f)
                firstOffset = o[0];
            else if (std::fabs(o[0] - firstOffset) > 0.5f)
                varied = true;
        }
        CHECK(gapsOk);
        CHECK(inside);
        CHECK(varied);

        // Infeasible: falls back to even spacing.
        bool fellBack = false;
        auto half = []() { return 0.5f; };
        std::vector<float> const even = RandomLaneOffsets(20.f, 3, 6.f, 6.f, half, &fellBack);
        CHECK(fellBack);
        CHECK(even.size() == 3);
        CHECK_NEAR(even[0], 0.f, 1e-5);
        CHECK_NEAR(even[2], 14.f, 1e-5);

        // ExceptOne: covers the span, exactly one free lane.
        for (std::uint32_t seed = 1; seed <= 200; ++seed)
        {
            std::mt19937 rng(seed);
            std::uniform_real_distribution<float> dist(0.f, 1.f);
            auto rnd = [&]() { return dist(rng); };
            std::uint32_t freeIndex = 99;
            std::vector<float> const cover = CoverLaneOffsets(40.f, 6.f, rnd, freeIndex);
            CHECK(cover.size() == 7);
            CHECK(freeIndex < cover.size());
            CHECK_NEAR(cover.back() + 6.f, 40.f, 1e-4);
            // Every point of the span is covered by some lane.
            bool covered = true;
            for (float y = 0.f; y <= 40.f; y += 0.5f)
            {
                bool any = false;
                for (float o : cover)
                    if (y >= o - 1e-4f && y <= o + 6.f + 1e-4f)
                        any = true;
                covered = covered && any;
            }
            CHECK(covered);
        }

        // Frame placement and crossings: frame at (100, 200), rotated 90 degrees, 40 x 30.
        Frame f;
        f.x = 100.f;
        f.y = 200.f;
        f.angle = Geo::kPi / 2.f;
        f.sizeX = 40.f;
        f.sizeY = 30.f;
        LaneRect const xLane{ Axis::X, 10.f, 6.f };       // runs along X (world +y), centred at fy = 13
        LaneRect const yLane{ Axis::Y, 20.f, 4.f };       // runs along Y (world -x), centred at fx = 22
        LanePlacement const lx = PlaceLane(f, xLane);
        CHECK_NEAR(lx.pose.x, 100.f - 13.f, 1e-4);
        CHECK_NEAR(lx.pose.y, 200.f, 1e-4);
        CHECK_NEAR(lx.length, 40.f, 1e-6);
        CHECK_NEAR(lx.width, 6.f, 1e-6);
        LanePlacement const ly = PlaceLane(f, yLane);
        CHECK_NEAR(ly.pose.x, 100.f, 1e-4);
        CHECK_NEAR(ly.pose.y, 222.f, 1e-4);
        CHECK_NEAR(ly.length, 30.f, 1e-6);
        LanePlacement const cell = PlaceCrossing(f, xLane, yLane);
        Shape cs = Shape::Line(cell.length, cell.width);
        // The cell is exactly the overlap: centre in, corners just inside, just outside each side out.
        Geo::Pose const centre = FramePoint(f, 22.f, 13.f, 0.f);
        CHECK(Geo::Contains(cs, cell.pose, Geo::Vec3{ centre.x, centre.y, 0.f }));
        for (auto [fx, fy, in] : { std::tuple{ 20.1f, 10.1f, true }, std::tuple{ 23.9f, 15.9f, true },
            std::tuple{ 19.5f, 13.f, false }, std::tuple{ 24.5f, 13.f, false }, std::tuple{ 22.f, 9.5f, false },
            std::tuple{ 22.f, 16.5f, false } })
        {
            Geo::Pose const q = FramePoint(f, fx, fy, 0.f);
            CHECK(Geo::Contains(cs, cell.pose, Geo::Vec3{ q.x, q.y, 0.f }) == in);
        }
    }

    void TestWeakened()
    {
        int const expected[] = { 0, 11, 25, 43, 67, 100, 150, 233 };
        for (std::uint8_t s = 0; s <= 7; ++s)
        {
            CHECK(WeakenedThreatPct(s) == expected[s]);
            CHECK_NEAR(WeakenedDamageMultiplier(s), 1.0 - 0.1 * s, 1e-6);
            double const net = WeakenedDamageMultiplier(s) * (1.0 + WeakenedThreatPct(s) / 100.0);
            CHECK(std::fabs(net - 1.0) <= 0.005);
        }
        CHECK(WeakenedThreatPct(9) == 233);     // clamped to the stack cap
        // Harness ruling 2026-11-02: only resolve hits apply Weakened (MissedStack/MissedBait/Moved/OffSpot do not).
        static_assert(!Rules::kWeakenedOnNonHitFailures);
    }

    void TestReferenceHealth()
    {
        std::vector<ReferenceHealthRow> rows;
        CHECK(ParseReferenceHealthRows("20:1000:1500 60:5000:8000", rows));
        CHECK(rows.size() == 2);
        ReferenceHealth h = InterpolateReferenceHealth(rows, 40);
        CHECK(h.nonTank == 3000);
        CHECK(h.tank == 4750);
        h = InterpolateReferenceHealth(rows, 10);
        CHECK(h.nonTank == 1000 && h.tank == 1500);
        h = InterpolateReferenceHealth(rows, 80);
        CHECK(h.nonTank == 5000 && h.tank == 8000);
        h = InterpolateReferenceHealth(rows, 21);
        CHECK(h.nonTank == 1100 && h.tank == 1663);

        CHECK(ParseReferenceHealthRows("60:5000:8000, 20:1000:1500;70:9000:15000", rows));
        CHECK(rows.size() == 3 && rows[0].level == 20 && rows[2].level == 70);
        CHECK(InterpolateReferenceHealth(rows, 65).nonTank == 7000);
        CHECK(!ParseReferenceHealthRows("20:1000", rows));
        CHECK(rows.empty());
        CHECK(!ParseReferenceHealthRows("20:1000:1500x", rows));
        CHECK(!ParseReferenceHealthRows("   ", rows));
        CHECK(InterpolateReferenceHealth(rows, 40).nonTank == 0);

        // Per-target flat damage.
        ReferenceHealth const ref{ 4000, 6000 };
        DamageSpec d;
        d.pctNonTank = 20.f;
        CHECK(FlatDamageFor(d, ref, false) == 800);
        CHECK(FlatDamageFor(d, ref, true) == 800);      // pctTank -1: the tank takes the non-tank number
        d.pctTank = 10.f;
        CHECK(FlatDamageFor(d, ref, true) == 600);
        CHECK(FlatDamageFor(d, ref, false) == 800);
        d.pctTank = 0.f;
        CHECK(FlatDamageFor(d, ref, true) == 0);
        d.flat = 123;
        CHECK(FlatDamageFor(d, ref, true) == 123);
        CHECK(FlatDamageFor(d, ref, false) == 123);
        d.flat = 0;
        CHECK(FlatDamageFor(d, ref, false) == 0);
        DamageSpec none;
        CHECK(!none.HasDamage());
        CHECK(none.ResolveSpell() == 205001);
        none.school = 2;
        CHECK(none.ResolveSpell() == 205003);
        none.spellId = 777;
        CHECK(none.ResolveSpell() == 777);
    }

    // L1: every point Contains accepts is within the 3D search radius of the caster (the spell's area search is 3D),
    // for casters on the anchor, trailing it and on another level.
    void TestSearchRadius()
    {
        Geo::Pose const anchor{ 0.f, 0.f, 0.f, 0.f };
        // The review's case: circle 3, unit at 2.9 yd horizontal and 5 yd vertical, caster on the anchor.
        Shape circle = Shape::Circle(3.f);
        CHECK(Geo::Contains(circle, anchor, Geo::Vec3{ 2.9f, 0.f, 5.f }));
        CHECK(std::hypot(2.9f, 5.f) <= ResolveSearchRadius(circle, anchor, 0.f, 0.f, 0.f));
        CHECK(std::hypot(2.9f, 5.f) > Geo::Bounds(circle) + Rules::kSearchSlackYd);    // the old 2D sum missed it

        Shape cone = Shape::Cone(90.f, 10.f);
        Shape line = Shape::Line(30.f, 4.f);
        line.excludeStart = 5.f;
        Shape donut = Shape::Donut(4.f, 12.f);
        Shape tight = Shape::Circle(2.f);
        tight.zTolerance = 1.f;
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> u(-1.f, 1.f);
        struct Caster { float x, y, z; };
        Caster const casters[] = { { 0.f, 0.f, 0.f }, { -3.f, 1.f, 0.f }, { 0.f, 0.f, 4.f }, { 2.f, -2.f, -5.f } };
        bool covered = true;
        int inside = 0;
        for (Shape const* shape : { &circle, &cone, &line, &donut, &tight })
        {
            float const b = Geo::Bounds(*shape);
            for (Caster const& c : casters)
            {
                float const r = ResolveSearchRadius(*shape, anchor, c.x, c.y, c.z);
                for (int i = 0; i < 4000; ++i)
                {
                    Geo::Vec3 const p{ u(rng) * b, u(rng) * b, u(rng) * (shape->zTolerance + 0.5f) };
                    if (!Geo::Contains(*shape, anchor, p))
                        continue;
                    ++inside;
                    float const d = std::sqrt((p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y)
                        + (p.z - c.z) * (p.z - c.z));
                    if (d > r)
                        covered = false;
                }
            }
        }
        CHECK(covered);
        CHECK(inside > 1000);
        // Caster on the anchor, flat: the horizontal part is Bounds + slack, the vertical zTolerance.
        CHECK_NEAR(ResolveSearchRadius(circle, anchor, 0.f, 0.f, 0.f), std::hypot(5.f, 6.f), 1e-5);
    }

    void TestVisualPick()
    {
        // Circle: fill disc / outline ring, colour offset, scale = radius.
        VisualPick v = PickVisual(Shape::Circle(8.f), 2, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == 90502 && v.pieces[0].scale == 8.f);
        v = PickVisual(Shape::Circle(3.f), 3, MarkerLayer::Outline);
        CHECK(v.pieces[0].display == 90507);

        // Donut (Rules::kDonutRatioRoundUp = false): the nearest ratio <= inner/outer, so the drawn safe hole is never
        // bigger than the real one. 4/12 = 0.333 -> 0.33 (j = 1).
        static_assert(!Rules::kDonutRatioRoundUp);
        v = PickVisual(Shape::Donut(4.f, 12.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces[0].display == 90508 + 2 * 1 && !v.clamped);
        CHECK(v.pieces[0].scale == 12.f);
        v = PickVisual(Shape::Donut(4.5f, 12.f), 0, MarkerLayer::Fill);  // 0.375 -> 0.33, never the larger 0.40
        CHECK(v.pieces[0].display == 90508 + 2 * 1);
        v = PickVisual(Shape::Donut(6.f, 12.f), 1, MarkerLayer::Fill);    // exactly 0.50 -> j = 3, yellow
        CHECK(v.pieces[0].display == 90508 + 2 * 3 + 1);
        // Below the ladder (ratio < 0.25): the smallest drawn hole would be bigger than the real one, so a full disc
        // (danger drawn everywhere, no hole), never a safe-looking area that hits.
        v = PickVisual(Shape::Donut(1.f, 12.f), 0, MarkerLayer::Fill);    // 0.083
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == 90500 && v.clamped && v.filled);
        CHECK(v.pieces[0].scale == 12.f);
        v = PickVisual(Shape::Donut(2.f, 12.f), 1, MarkerLayer::Fill);    // 0.167, yellow
        CHECK(v.pieces[0].display == 90501 && v.filled);
        v = PickVisual(Shape::Donut(2.9f, 12.f), 2, MarkerLayer::Fill);   // 0.242, blue -> red family like donuts
        CHECK(v.pieces[0].display == 90500 && v.filled && v.colourFallback);
        v = PickVisual(Shape::Donut(0.f, 12.f), 0, MarkerLayer::Fill);    // no hole at all
        CHECK(v.pieces[0].display == 90500 && v.filled);
        v = PickVisual(Shape::Donut(3.f, 12.f), 0, MarkerLayer::Fill);    // exactly 0.25: the ladder's first step
        CHECK(v.pieces[0].display == 90508 && !v.clamped && !v.filled);
        v = PickVisual(Shape::Donut(11.f, 12.f), 0, MarkerLayer::Fill);   // past the ladder -> 0.75, a smaller hole
        CHECK(v.pieces[0].display == 90508 + 2 * 6 && !v.clamped);
        for (int inner = 1; inner < 120; ++inner)                         // drawn ratio <= real ratio on the ladder
        {
            float const ratio = inner / 120.f;
            v = PickVisual(Shape::Donut(inner / 10.f, 12.f), 0, MarkerLayer::Fill);
            if (v.filled)
            {
                CHECK(ratio < Ids::kDonutRatios.front() && v.pieces[0].display == 90500);
                continue;
            }
            float const drawn = Ids::kDonutRatios[(v.pieces[0].display - 90508) / 2];
            CHECK(drawn <= ratio + 1e-4f);
        }
        v = PickVisual(Shape::Donut(4.f, 12.f), 2, MarkerLayer::Fill);    // blue asked: red/yellow only
        CHECK(v.colourFallback && v.pieces[0].display == 90508 + 2 * 1);

        // Cone: nearest larger angle, scale = length, facing offset carried.
        v = PickVisual(Shape::Cone(90.f, 20.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces[0].display == 90522 + 2 * 3 && v.pieces[0].scale == 20.f);
        v = PickVisual(Shape::Cone(100.f, 20.f), 1, MarkerLayer::Fill);
        CHECK(v.pieces[0].display == 90522 + 2 * 4 + 1);
        v = PickVisual(Shape::Cone(10.f, 20.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces[0].display == 90522);
        v = PickVisual(Shape::Cone(270.f, 20.f), 0, MarkerLayer::Fill);   // the widest cone on the ladder
        CHECK(v.pieces[0].display == 90522 + 2 * 7 && !v.clamped && !v.filled);
        v = PickVisual(Shape::Cone(300.f, 20.f), 1, MarkerLayer::Fill);   // wider: a full disc of the length
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == 90501 && v.clamped && v.filled);
        CHECK(v.pieces[0].scale == 20.f && v.pieces[0].facingOffset == 0.f && v.pieces[0].forward == 0.f);
        for (float a = 1.f; a <= 360.f; a += 1.f)                         // drawn cone angle >= hitbox angle, or disc
        {
            v = PickVisual(Shape::Cone(a, 20.f), 0, MarkerLayer::Fill);
            if (v.filled)
                CHECK(a > 270.f && v.pieces[0].display == 90500);
            else
                CHECK(Ids::kConeAngles[(v.pieces[0].display - 90522) / 2] >= a - 1e-4f);
        }
        Shape rear = Shape::Cone(90.f, 8.f);
        rear.facingOffsetDeg = 180.f;
        v = PickVisual(rear, 0, MarkerLayer::Fill);
        CHECK_NEAR(v.pieces[0].facingOffset, Geo::kPi, 1e-5);

        // Line: never shorter than the hitbox, at most 9% longer.
        bool neverShorter = true;
        bool atMost9 = true;
        for (float len = 1.f; len <= 128.f; len += 0.37f)
        {
            Shape l = Shape::Line(len, 3.f);
            VisualPick const p = PickVisual(l, 0, MarkerLayer::Fill);
            float drawnEnd = 0.f;
            for (VisualPiece const& piece : p.pieces)
            {
                std::uint32_t const i = (piece.display - 90538) / 2;
                drawnEnd = std::max(drawnEnd, piece.forward + LineAspect(i) * piece.scale);
            }
            float const drawnPiece = LineAspect((p.pieces[0].display - 90538) / 2) * 3.f;
            float const hitPiece = len / p.pieces.size();
            if (drawnEnd < len - 1e-3f)
                neverShorter = false;
            if (hitPiece >= 3.f && drawnPiece > hitPiece * 1.0906f + 1e-3f)
                atMost9 = false;
        }
        CHECK(neverShorter);
        CHECK(atMost9);
        // Excluded start: the drawn part starts at the exclusion.
        Shape ex = Shape::Line(30.f, 6.f);
        ex.excludeStart = 6.f;
        v = PickVisual(ex, 1, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 1);
        CHECK_NEAR(v.pieces[0].forward, 6.f, 1e-6);
        CHECK(v.pieces[0].display == 90538 + 2 * 16 + 1);       // 24/6 = 4 = 2^(16/8)
        // Aspect exactly 8 is one piece; above 8 several abutting pieces.
        v = PickVisual(Shape::Line(16.f, 2.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == 90538 + 2 * 24);
        v = PickVisual(Shape::Line(100.f, 2.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 7);                            // 50 / 8 -> 7 pieces of 14.29 yd
        CHECK_NEAR(v.pieces[1].forward, 100.f / 7.f, 1e-4);
        CHECK(v.pieces[0].display == 90538 + 2 * 23);           // 7.14 -> ceil(8 log2 7.14) = 23
        v = PickVisual(Shape::Line(1000.f, 3.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 42);
        // Short wide line: aspect below 1 draws the 1:1 piece.
        v = PickVisual(Shape::Line(2.f, 6.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == 90538);
        // Degenerate.
        CHECK(PickVisual(Shape::Line(0.f, 6.f), 0, MarkerLayer::Fill).pieces.empty());
        // The id table is dense: fill lines end where outline lines start, the timer ring is the last block.
        CHECK(90538 + 2 * 24 + 1 + 1 == Ids::DISPLAY_LINE_OUTLINE_BASE);
        CHECK(Ids::DISPLAY_LINE_OUTLINE_BASE + 2 * 24 + 1 + 1 == Ids::DISPLAY_CONE_OUTLINE_BASE);
        CHECK(Ids::DISPLAY_CONE_OUTLINE_BASE + 2 * 7 + 1 + 1 == Ids::DISPLAY_DONUT_OUTLINE_BASE);
        CHECK(Ids::DISPLAY_DONUT_OUTLINE_BASE + 2 * 6 + 1 + 1 == Ids::DISPLAY_TIMER_RING_BASE);
        CHECK(Ids::DISPLAY_TIMER_RING_BASE + 1 == Ids::DISPLAY_LAST);
        CHECK(Ids::DISPLAY_LAST + 1 == Ids::DISPLAY_SPARE_FIRST && Ids::DISPLAY_SPARE_LAST == 90699);
    }

    // Every shape the tests sweep: (shape, colour).
    std::vector<std::pair<Shape, std::uint8_t>> FillShapes()
    {
        std::vector<std::pair<Shape, std::uint8_t>> out;
        for (float r : { 0.5f, 2.f, 5.f, 10.f, 40.f })
            out.push_back({ Shape::Circle(r), static_cast<std::uint8_t>(static_cast<int>(r) % 4) });
        for (float inner : { 0.f, 2.f, 3.f, 4.5f, 6.f, 9.f, 11.f })
            out.push_back({ Shape::Donut(inner, 12.f), 1 });
        for (float a : { 10.f, 60.f, 90.f, 100.f, 270.f, 300.f })
            out.push_back({ Shape::Cone(a, 20.f), 0 });
        for (float len : { 0.3f, 2.f, 3.f, 7.f, 24.f, 25.f, 40.f, 100.f, 1000.f })
            out.push_back({ Shape::Line(len, 3.f), 1 });
        Shape ex = Shape::Line(30.f, 6.f);
        ex.excludeStart = 6.f;
        out.push_back({ ex, 0 });
        Shape rear = Shape::Cone(90.f, 8.f);
        rear.facingOffsetDeg = 180.f;
        out.push_back({ rear, 0 });
        return out;
    }

    bool IsLine(std::uint32_t display)
    {
        return display >= Ids::DISPLAY_LINE_BASE && display < Ids::DISPLAY_CONE_OUTLINE_BASE;
    }

    void TestLayers()
    {
        // Outline and fill draw the same geometry: same piece count, scale, facing and placement; only the ids differ.
        for (auto const& [shape, colour] : FillShapes())
        {
            VisualPick const o = PickVisual(shape, colour, MarkerLayer::Outline);
            VisualPick const f = PickVisual(shape, colour, MarkerLayer::Fill);
            CHECK(o.pieces.size() == f.pieces.size());
            CHECK(o.clamped == f.clamped && o.filled == f.filled && o.colourFallback == f.colourFallback);
            for (std::size_t k = 0; k < std::min(o.pieces.size(), f.pieces.size()); ++k)
            {
                CHECK(o.pieces[k].scale == f.pieces[k].scale);
                CHECK(o.pieces[k].forward == f.pieces[k].forward);
                CHECK(o.pieces[k].facingOffset == f.pieces[k].facingOffset);
                CHECK_NEAR(PieceExtent(o.pieces[k]), PieceExtent(f.pieces[k]), 1e-5);
                CHECK(o.pieces[k].display != f.pieces[k].display);
            }
        }
        // The outline ids, shape by shape.
        CHECK(PickVisual(Shape::Circle(8.f), 2, MarkerLayer::Outline).pieces[0].display == 90506);
        CHECK(PickVisual(Shape::Donut(4.f, 12.f), 1, MarkerLayer::Outline).pieces[0].display == 90654 + 2 * 1 + 1);
        CHECK(PickVisual(Shape::Donut(1.f, 12.f), 0, MarkerLayer::Outline).pieces[0].display == 90504);  // filled
        CHECK(PickVisual(Shape::Cone(90.f, 20.f), 0, MarkerLayer::Outline).pieces[0].display == 90638 + 2 * 3);
        CHECK(PickVisual(Shape::Cone(300.f, 20.f), 1, MarkerLayer::Outline).pieces[0].display == 90505);  // filled
        CHECK(PickVisual(Shape::Line(12.f, 3.f), 1, MarkerLayer::Outline).pieces[0].display == 90588 + 2 * 16 + 1);
        // Outline rounding promises: a get-out outline is never smaller than its hitbox; a donut hole never bigger.
        for (float len = 0.5f; len <= 200.f; len += 0.73f)
        {
            VisualPick const o = PickVisual(Shape::Line(len, 2.5f), 0, MarkerLayer::Outline);
            CHECK(o.pieces.back().forward + PieceExtent(o.pieces.back()) >= len - 1e-3f);
        }
        for (int inner = 30; inner < 120; ++inner)
        {
            VisualPick const o = PickVisual(Shape::Donut(inner / 10.f, 12.f), 0, MarkerLayer::Outline);
            float const drawn = Ids::kDonutRatios[(o.pieces[0].display - Ids::DISPLAY_DONUT_OUTLINE_BASE) / 2];
            CHECK(drawn <= inner / 120.f + 1e-4f);
        }
    }

    void TestFillFraction()
    {
        Ms const created(1000);
        Ms const resolve(4000);
        CHECK_NEAR(FillFraction(created, resolve, created, Ms(0)), 0.f, 1e-6);
        CHECK_NEAR(FillFraction(created, resolve, Ms(2500), Ms(0)), 0.5f, 1e-6);
        CHECK_NEAR(FillFraction(created, resolve, resolve, Ms(0)), 1.f, 1e-6);
        CHECK_NEAR(FillFraction(created, resolve, Ms(500), Ms(0)), 0.f, 1e-6);        // clamped below
        CHECK_NEAR(FillFraction(created, resolve, Ms(9000), Ms(0)), 1.f, 1e-6);       // and above
        CHECK_NEAR(FillFraction(created, resolve, Ms(3850), Ms(150)), 1.f, 1e-6);     // full `lead` early
        CHECK_NEAR(FillFraction(created, resolve, Ms(3849), Ms(150)), 2999.f / 3000.f, 1e-5);
        CHECK(FillFraction(created, created, created, Ms(0)) == 1.f);                  // no warning: full at once
        float prev = -1.f;
        for (std::int64_t t = 0; t <= 5000; t += 37)
        {
            float const f = FillFraction(created, resolve, Ms(t), Ms(150));
            CHECK(f >= prev);
            prev = f;
        }

        // The schedule the telegraph runs: one step every FillStepMs from the spawn. With the lead at least one step,
        // the last step before the resolve already asks for the full size, so the fill is full at (not after) the
        // resolve as far as the server goes.
        for (std::int64_t warn : { 400, 1000, 2500, 3000, 7777 })
            for (std::int64_t step : { 50, 100, 150, 200 })
            {
                Ms const c(0);
                Ms const r(warn);
                float last = 0.f;
                bool monotonic = true;
                for (std::int64_t t = 0; t < warn; t += step)
                {
                    float const f = FillFraction(c, r, Ms(t), Ms(step));
                    if (f < last)
                        monotonic = false;
                    last = f;
                }
                CHECK(monotonic);
                CHECK(last == 1.f);
            }
    }

    void TestFillPick()
    {
        for (auto const& [shape, colour] : FillShapes())
        {
            VisualPick const outline = PickVisual(shape, colour, MarkerLayer::Outline);
            VisualPick const full = PickVisual(shape, colour, MarkerLayer::Fill);
            bool const donutRing = shape.type == ShapeType::Donut && !full.filled;

            // Live danger: exactly the full fill.
            VisualPick const live = PickFill(shape, colour, 0.3f, true);
            CHECK(live.pieces.size() == full.pieces.size());
            for (std::size_t k = 0; k < live.pieces.size(); ++k)
                CHECK(live.pieces[k].display == full.pieces[k].display && live.pieces[k].scale == full.pieces[k].scale);

            float prevTotal = -1.f;
            float prevRing = 1e9f;
            bool monotonic = true;
            bool neverAbove = true;
            bool slotsInOrder = true;
            for (int step = 0; step <= 200; ++step)
            {
                float const f = step / 200.f;
                VisualPick const p = PickFill(shape, colour, f, false);
                CHECK(p.pieces.size() == outline.pieces.size());
                float total = 0.f;
                for (std::size_t k = 0; k < p.pieces.size(); ++k)
                {
                    VisualPiece const& piece = p.pieces[k];
                    VisualPiece const& o = outline.pieces[k];
                    float const extent = PieceExtent(piece);
                    if (extent > PieceExtent(o) + 1e-4f)
                        neverAbove = false;
                    if (piece.display && piece.scale > o.scale + 1e-5f)     // a line never wider than its outline
                        neverAbove = false;
                    if (piece.display && (piece.forward != o.forward || piece.facingOffset != o.facingOffset))
                        neverAbove = false;                                 // same anchor point and facing
                    if (shape.type == ShapeType::Line && piece.display)
                    {
                        // From the source end, without gaps: piece k starts only once piece k-1 reaches its start
                        // (outline pieces round up, so neighbours overlap a little and both may be partial there).
                        if (k > 0 && p.pieces[k - 1].forward + PieceExtent(p.pieces[k - 1]) < piece.forward - 1e-3f)
                            slotsInOrder = false;
                        if (k == 0 && piece.forward != outline.pieces[0].forward)
                            slotsInOrder = false;
                        if (!IsLine(piece.display))
                            slotsInOrder = false;
                    }
                    if (piece.display && piece.scale < Rules::kFillMinScaleYd)
                        neverAbove = false;                                 // no near-zero creature
                    total += extent;
                }
                if (donutRing)
                {
                    VisualPiece const& ring = p.pieces[0];
                    CHECK(ring.display == Ids::DISPLAY_TIMER_RING_BASE + colour % 2);
                    // shrinks from the outer rim; its band never enters the drawn hole
                    float const hole = Ids::kDonutRatios[(full.pieces[0].display - Ids::DISPLAY_DONUT_BASE) / 2]
                        * shape.radius;
                    CHECK(ring.scale <= shape.radius + 1e-5f);
                    CHECK(ring.scale * (1.f - Ids::kTimerRingBandFrac) >= hole - 1e-4f);
                    if (ring.scale > prevRing + 1e-6f)
                        monotonic = false;
                    prevRing = ring.scale;
                    if (step == 0)
                        CHECK_NEAR(ring.scale, shape.radius, 1e-5);
                    if (step == 200)
                        CHECK_NEAR(ring.scale * (1.f - Ids::kTimerRingBandFrac), hole, 1e-4);
                    continue;
                }
                if (total < prevTotal - 1e-5f)
                    monotonic = false;
                prevTotal = total;
                if (step == 0)
                    CHECK(total == 0.f);                                    // nothing drawn at the start
                if (step == 200)
                {
                    // Full at f = 1: the outline's own geometry, in the fill displays.
                    for (std::size_t k = 0; k < p.pieces.size(); ++k)
                    {
                        CHECK(p.pieces[k].display == full.pieces[k].display);
                        CHECK_NEAR(p.pieces[k].scale, full.pieces[k].scale, 1e-5);
                    }
                }
            }
            CHECK(monotonic);
            CHECK(neverAbove);
            CHECK(slotsInOrder);
        }

        // Circle: linear in f from the centre.
        VisualPick v = PickFill(Shape::Circle(10.f), 0, 0.25f, false);
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == 90500);
        CHECK_NEAR(v.pieces[0].scale, 2.5f, 1e-5);
        CHECK(PickFill(Shape::Circle(10.f), 0, 0.005f, false).pieces[0].display == 0);      // 0.05 yd: not yet
        // Cone: grows from the apex, same angle display, facing kept.
        v = PickFill(Shape::Cone(90.f, 20.f), 1, 0.5f, false);
        CHECK(v.pieces[0].display == 90522 + 2 * 3 + 1);
        CHECK_NEAR(v.pieces[0].scale, 10.f, 1e-5);
        // Donut 4/12 (drawn 0.33): the ring ends with its inner edge on the 3.96 yd drawn hole.
        v = PickFill(Shape::Donut(4.f, 12.f), 0, 1.f, false);
        CHECK(v.pieces[0].display == 90668);
        CHECK_NEAR(v.pieces[0].scale, 0.33f * 12.f / 0.96f, 1e-4);
        v = PickFill(Shape::Donut(4.f, 12.f), 0, 1.f, true);                                // live: filled donut
        CHECK(v.pieces[0].display == 90508 + 2);
        // A donut drawn as a disc fills like a circle.
        v = PickFill(Shape::Donut(1.f, 12.f), 0, 0.5f, false);
        CHECK(v.pieces[0].display == 90500);
        CHECK_NEAR(v.pieces[0].scale, 6.f, 1e-5);
        // Line 24 x 3 (aspect 8, one piece, step 24): constant width, ladder steps that fit, a square below 3 yd.
        v = PickFill(Shape::Line(24.f, 3.f), 0, 0.0625f, false);                            // 1.5 yd: a square
        CHECK(v.pieces[0].display == 90538 && v.pieces[0].scale == 1.5f);
        v = PickFill(Shape::Line(24.f, 3.f), 0, 0.5f, false);                               // 12 yd = 4 widths
        CHECK(v.pieces[0].display == 90538 + 2 * 16 && v.pieces[0].scale == 3.f);
        v = PickFill(Shape::Line(24.f, 3.f), 0, 0.49f, false);                              // 11.76: step 15
        CHECK(v.pieces[0].display == 90538 + 2 * 15);
        // Line 100 x 2 (7 pieces): at half, pieces 0-2 full, piece 3 partial, 4-6 empty.
        VisualPick const o = PickVisual(Shape::Line(100.f, 2.f), 0, MarkerLayer::Outline);
        v = PickFill(Shape::Line(100.f, 2.f), 0, 0.5f, false);
        CHECK(v.pieces[0].display == 90538 + 2 * 23 && v.pieces[2].display == 90538 + 2 * 23);
        CHECK(v.pieces[3].display != 0 && v.pieces[3].display < 90538 + 2 * 23);
        CHECK(v.pieces[4].display == 0 && v.pieces[6].display == 0);
        CHECK(o.pieces.size() == 7);
        // Excluded start: the fill starts at the exclusion.
        Shape ex = Shape::Line(30.f, 6.f);
        ex.excludeStart = 6.f;
        v = PickFill(ex, 0, 0.5f, false);
        CHECK(v.pieces.size() == 1 && v.pieces[0].forward == 6.f);
        CHECK(v.pieces[0].display == 90538 + 2 * 8);                                        // 12 yd = 2 widths
    }
}

int main()
{
    TestProjectileWindows();
    TestProjectileSweep();
    TestTrackedSweep();
    TestMine();
    TestLanes();
    TestWeakened();
    TestReferenceHealth();
    TestSearchRadius();
    TestVisualPick();
    TestLayers();
    TestFillFraction();
    TestFillPick();
    return Report("math_test");
}
