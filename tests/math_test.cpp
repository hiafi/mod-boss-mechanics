// Standalone tests for src/MechanicMath.h (STAGE-M1-HANDOFF §6.1). Build: tests/run.sh.

#include "check.h"
#include "MechanicGround.h"
#include "MechanicMath.h"
#include <array>
#include <functional>
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

            // Drawn strips: the drawn lanes' strips never overlap and cover exactly what their hitboxes cover. The
            // last lane (clamped, overlapping the one before it) draws only the 4 yd the others leave, unless the
            // lane before it is the free lane (then the overlap is danger nobody else draws).
            std::vector<LaneStrip> strips;
            for (std::uint32_t i = 0; i < cover.size(); ++i)
                if (i != freeIndex)
                    strips.push_back(CoverLaneDrawnStrip(cover, 6.f, i, freeIndex));
            bool disjoint = true;
            bool same = true;
            for (float y = 0.05f; y < 40.f; y += 0.1f)
            {
                int drawnHere = 0;
                for (LaneStrip const& st : strips)
                    if (y > st.offset && y < st.offset + st.width)
                        ++drawnHere;
                bool hit = false;
                for (std::uint32_t i = 0; i < cover.size(); ++i)
                    if (i != freeIndex && y > cover[i] && y < cover[i] + 6.f)
                        hit = true;
                disjoint = disjoint && drawnHere <= 1;
                same = same && (drawnHere > 0) == hit;
            }
            CHECK(disjoint);
            CHECK(same);
            LaneStrip const last = CoverLaneDrawnStrip(cover, 6.f, 6, freeIndex);
            if (freeIndex == 5)
                CHECK(last.offset == 34.f && last.width == 6.f);
            else if (freeIndex != 6)
            {
                CHECK_NEAR(last.offset, 36.f, 1e-4);
                CHECK_NEAR(last.width, 4.f, 1e-4);
            }
        }
        // A cover that fits exactly has no overlap: every lane draws whole.
        {
            std::uint32_t freeIndex = 0;
            std::vector<float> const cover = CoverLaneOffsets(36.f, 6.f, []() { return 0.f; }, freeIndex);
            LaneStrip const st = CoverLaneDrawnStrip(cover, 6.f, 5, freeIndex);
            CHECK(cover.size() == 6 && st.offset == 30.f && st.width == 6.f);
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

    using Ids::MarkerShape;

    std::uint32_t D(std::uint8_t color, MarkerShape shape, std::uint32_t step = 0)
    {
        return Ids::MarkerDisplay(color, shape, step);
    }

    // The ladder step of a marker display (donut ratio, cone angle or line aspect index).
    std::uint32_t Step(std::uint32_t display)
    {
        return Ids::DecodeMarkerDisplay(display).step;
    }

    void TestVisualPick()
    {
        // Circle: fill disc / outline ring, color offset, scale = radius.
        VisualPick v = PickVisual(Shape::Circle(8.f), 2, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == D(2, MarkerShape::Disc) && v.pieces[0].scale == 8.f);
        v = PickVisual(Shape::Circle(3.f), 3, MarkerLayer::Outline);
        CHECK(v.pieces[0].display == D(3, MarkerShape::Ring));

        // Donut (Rules::kDonutRatioRoundUp = false): the nearest ratio <= inner/outer, so the drawn safe hole is never
        // bigger than the real one. 4/12 = 0.333 -> 0.33 (j = 1).
        static_assert(!Rules::kDonutRatioRoundUp);
        v = PickVisual(Shape::Donut(4.f, 12.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces[0].display == D(0, MarkerShape::Donut, 1) && !v.clamped);
        CHECK(v.pieces[0].scale == 12.f);
        v = PickVisual(Shape::Donut(4.5f, 12.f), 0, MarkerLayer::Fill);  // 0.375 -> 0.33, never the larger 0.40
        CHECK(v.pieces[0].display == D(0, MarkerShape::Donut, 1));
        v = PickVisual(Shape::Donut(6.f, 12.f), 1, MarkerLayer::Fill);    // exactly 0.50 -> j = 3, yellow
        CHECK(v.pieces[0].display == D(1, MarkerShape::Donut, 3));
        // Below the ladder (ratio < 0.25): the smallest drawn hole would be bigger than the real one, so a full disc
        // (danger drawn everywhere, no hole), never a safe-looking area that hits.
        v = PickVisual(Shape::Donut(1.f, 12.f), 0, MarkerLayer::Fill);    // 0.083
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == D(0, MarkerShape::Disc) && v.clamped && v.filled);
        CHECK(v.pieces[0].scale == 12.f);
        v = PickVisual(Shape::Donut(2.f, 12.f), 1, MarkerLayer::Fill);    // 0.167, yellow
        CHECK(v.pieces[0].display == D(1, MarkerShape::Disc) && v.filled);
        v = PickVisual(Shape::Donut(2.9f, 12.f), 2, MarkerLayer::Fill);   // 0.242, blue: a blue disc
        CHECK(v.pieces[0].display == D(2, MarkerShape::Disc) && v.filled);
        v = PickVisual(Shape::Donut(0.f, 12.f), 0, MarkerLayer::Fill);    // no hole at all
        CHECK(v.pieces[0].display == D(0, MarkerShape::Disc) && v.filled);
        v = PickVisual(Shape::Donut(3.f, 12.f), 0, MarkerLayer::Fill);    // exactly 0.25: the ladder's first step
        CHECK(v.pieces[0].display == D(0, MarkerShape::Donut) && !v.clamped && !v.filled);
        v = PickVisual(Shape::Donut(11.f, 12.f), 0, MarkerLayer::Fill);   // past the ladder -> 0.75, a smaller hole
        CHECK(v.pieces[0].display == D(0, MarkerShape::Donut, 6) && !v.clamped);
        for (int inner = 1; inner < 120; ++inner)                         // drawn ratio <= real ratio on the ladder
        {
            float const ratio = inner / 120.f;
            v = PickVisual(Shape::Donut(inner / 10.f, 12.f), 0, MarkerLayer::Fill);
            if (v.filled)
            {
                CHECK(ratio < Ids::kDonutRatios.front() && v.pieces[0].display == D(0, MarkerShape::Disc));
                continue;
            }
            float const drawn = Ids::kDonutRatios[Step(v.pieces[0].display)];
            CHECK(drawn <= ratio + 1e-4f);
        }
        v = PickVisual(Shape::Donut(4.f, 12.f), 2, MarkerLayer::Fill);    // blue: every shape exists in every color
        CHECK(v.pieces[0].display == D(2, MarkerShape::Donut, 1));

        // Cone: nearest larger angle, scale = length, facing offset carried.
        v = PickVisual(Shape::Cone(90.f, 20.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces[0].display == D(0, MarkerShape::Cone, 3) && v.pieces[0].scale == 20.f);
        v = PickVisual(Shape::Cone(100.f, 20.f), 1, MarkerLayer::Fill);
        CHECK(v.pieces[0].display == D(1, MarkerShape::Cone, 4));
        v = PickVisual(Shape::Cone(10.f, 20.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces[0].display == D(0, MarkerShape::Cone));
        v = PickVisual(Shape::Cone(270.f, 20.f), 0, MarkerLayer::Fill);   // the widest cone on the ladder
        CHECK(v.pieces[0].display == D(0, MarkerShape::Cone, 7) && !v.clamped && !v.filled);
        v = PickVisual(Shape::Cone(300.f, 20.f), 1, MarkerLayer::Fill);   // wider: a full disc of the length
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == D(1, MarkerShape::Disc) && v.clamped && v.filled);
        CHECK(v.pieces[0].scale == 20.f && v.pieces[0].facingOffset == 0.f && v.pieces[0].forward == 0.f);
        for (float a = 1.f; a <= 360.f; a += 1.f)                         // drawn cone angle >= hitbox angle, or disc
        {
            v = PickVisual(Shape::Cone(a, 20.f), 0, MarkerLayer::Fill);
            if (v.filled)
                CHECK(a > 270.f && v.pieces[0].display == D(0, MarkerShape::Disc));
            else
                CHECK(Ids::kConeAngles[Step(v.pieces[0].display)] >= a - 1e-4f);
        }
        Shape rear = Shape::Cone(90.f, 8.f);
        rear.facingOffsetDeg = 180.f;
        v = PickVisual(rear, 0, MarkerLayer::Fill);
        CHECK_NEAR(v.pieces[0].facingOffset, Geo::kPi, 1e-5);

        // Line ladder: quarter octaves from 1/4 below 1, eighth octaves from 1 to 64 (the asset script's line_aspect).
        CHECK(Ids::kLineSteps == 57 && Ids::kLineMaxAspect == 64.f);
        CHECK_NEAR(LineAspect(0), 0.25f, 1e-6);
        CHECK_NEAR(LineAspect(4), 0.5f, 1e-6);
        CHECK_NEAR(LineAspect(Ids::kLineUnitStep), 1.f, 1e-6);
        CHECK_NEAR(LineAspect(16), 2.f, 1e-6);
        CHECK_NEAR(LineAspect(32), 8.f, 1e-5);
        CHECK_NEAR(LineAspect(Ids::kLineSteps - 1), Ids::kLineMaxAspect, 1e-4);
        for (std::uint32_t i = 1; i < Ids::kLineSteps; ++i)
            CHECK(LineAspect(i) > LineAspect(i - 1) && LineAspect(i) <= LineAspect(i - 1) * 1.1893f);
        for (std::uint32_t i = Ids::kLineUnitStep + 1; i < Ids::kLineSteps; ++i)
            CHECK(LineAspect(i) <= LineAspect(i - 1) * 1.0906f);                  // eighth octaves from aspect 1
        CHECK(LineStepFor(0.1f) == 0 && LineStepFor(0.25f) == 0 && LineStepFor(0.26f) == 1);
        CHECK(LineStepFor(1.f) == Ids::kLineUnitStep && LineStepFor(8.f) == 32 && LineStepFor(50.f) == 54);
        CHECK(LineStepFor(64.f) == 56 && LineStepFor(500.f) == 56);
        // Line: end to end. Every piece but the last is exactly 64 widths (top step) and the next one starts where it
        // ends; the last piece covers the rest, rounded up by at most one ladder step. Never shorter than the hitbox.
        bool neverShorter = true;
        bool endToEnd = true;
        bool lastTight = true;
        for (float len = 0.8f; len <= 600.f; len += 0.37f)
        {
            Shape l = Shape::Line(len, 3.f);
            VisualPick const p = PickVisual(l, 0, MarkerLayer::Fill);
            for (std::size_t k = 0; k + 1 < p.pieces.size(); ++k)
            {
                if (Step(p.pieces[k].display) != Ids::kLineSteps - 1)
                    endToEnd = false;
                if (std::fabs(p.pieces[k].forward + PieceExtent(p.pieces[k]) - p.pieces[k + 1].forward) > 1e-3f)
                    endToEnd = false;
            }
            VisualPiece const& last = p.pieces.back();
            float const rest = len - last.forward;
            float const drawnEnd = last.forward + PieceExtent(last);
            if (drawnEnd < len - 1e-3f)
                neverShorter = false;
            float const slack = rest >= 3.f ? 1.0906f : 1.1893f;          // one eighth / quarter octave
            if (rest >= 0.75f && PieceExtent(last) > rest * slack + 1e-3f)
                lastTight = false;
        }
        CHECK(neverShorter);
        CHECK(endToEnd);
        CHECK(lastTight);
        // Excluded start: the drawn part starts at the exclusion.
        Shape ex = Shape::Line(30.f, 6.f);
        ex.excludeStart = 6.f;
        v = PickVisual(ex, 1, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 1);
        CHECK_NEAR(v.pieces[0].forward, 6.f, 1e-6);
        CHECK(v.pieces[0].display == D(1, MarkerShape::Line, 24));       // 24/6 = 4 = 2^(16/8), step 8 + 16
        // Aspect up to 64 is one piece (100 x 2 too: 50 -> 2^(46/8) = 53.8, step 54); above 64 full pieces of 64
        // widths plus the rest.
        v = PickVisual(Shape::Line(16.f, 2.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == D(0, MarkerShape::Line, 32));
        v = PickVisual(Shape::Line(100.f, 2.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == D(0, MarkerShape::Line, 54));
        v = PickVisual(Shape::Line(128.f, 2.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == D(0, MarkerShape::Line, 56));
        v = PickVisual(Shape::Line(300.f, 2.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 3);                            // 2 pieces of 128 yd + 44 yd
        CHECK_NEAR(v.pieces[1].forward, 128.f, 1e-3);
        CHECK_NEAR(v.pieces[2].forward, 256.f, 1e-3);
        CHECK(v.pieces[0].display == D(0, MarkerShape::Line, 56) && v.pieces[1].display == D(0, MarkerShape::Line, 56));
        CHECK(v.pieces[2].display == D(0, MarkerShape::Line, 44));           // 44 / 2 = 22 -> 2^(36/8) = 22.6
        v = PickVisual(Shape::Line(1000.f, 3.f), 0, MarkerLayer::Fill);
        CHECK(v.pieces.size() == 6);
        // Short wide line: aspect below 1 draws a piece shorter than wide (not a square any more).
        v = PickVisual(Shape::Line(2.f, 6.f), 0, MarkerLayer::Fill);     // 1/3 -> 2^(-6/4) = 0.354, step 2
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == D(0, MarkerShape::Line, 2) && !v.clamped);
        CHECK_NEAR(PieceExtent(v.pieces[0]), 6.f * std::exp2(-1.5f), 1e-4);
        v = PickVisual(Shape::Line(1.f, 6.f), 0, MarkerLayer::Fill);     // 1/6: below the ladder, drawn at 1/4
        CHECK(v.pieces[0].display == D(0, MarkerShape::Line, 0) && v.clamped);
        // Degenerate.
        CHECK(PickVisual(Shape::Line(0.f, 6.f), 0, MarkerLayer::Fill).pieces.empty());
        // The id table: one 100-id block per color from 91000 for the quad shapes in a fixed order, and one per color
        // from 92000 (line fill) and 93000 (line outline) for the line ladders (the asset script's display_id; its
        // tests pin the same numbers).
        CHECK(Ids::MarkerDisplay(0, MarkerShape::Disc) == 91000);
        CHECK(D(2, MarkerShape::Disc) == 91200);
        CHECK(D(2, MarkerShape::Ring) == 91201);
        CHECK(D(1, MarkerShape::Donut, 3) == 91106);
        CHECK(D(0, MarkerShape::Cone, 3) == 91020);
        CHECK(D(8, MarkerShape::ConeOutline, 7) == 91832);
        CHECK(D(0, MarkerShape::DashedRing) == 91033 && D(8, MarkerShape::DashedRing) == 91833);
        CHECK(D(3, MarkerShape::LineOutline, 8) == 93308);
        CHECK(D(4, MarkerShape::Line, 0) == 92400);
        CHECK(D(0, MarkerShape::Line, 56) == 92056);
        CHECK(D(8, MarkerShape::LineOutline, 56) == 93856 && Ids::DISPLAY_LAST == 93856);
        std::array<std::uint32_t, 9> const offsets = { 0, 1, 2, 3, 10, 17, 25, 33, 34 };
        CHECK(Ids::kMarkerShapeOffsets == offsets);
        CHECK(Ids::DISPLAY_PER_COLOR == 34 && Ids::DISPLAY_COLOR_STRIDE == 100 && Ids::COLOR_COUNT == 9);
        CHECK(Ids::DISPLAY_SLICE_LAST == 93999);
        // Every shipped id decodes back to its (color, shape, step), and the ids are dense inside each color block.
        bool roundTrip = true;
        for (std::uint8_t c = 0; c < Ids::COLOR_COUNT; ++c)
        {
            std::uint32_t expected = Ids::DISPLAY_FIRST + Ids::DISPLAY_COLOR_STRIDE * c;
            for (std::size_t s = 0; s < Ids::kMarkerShapeSteps.size(); ++s)
            {
                MarkerShape const shape = static_cast<MarkerShape>(s);
                std::uint32_t lineId = (shape == MarkerShape::Line ? Ids::DISPLAY_LINE_FIRST
                    : Ids::DISPLAY_LINE_OUTLINE_FIRST) + Ids::DISPLAY_COLOR_STRIDE * c;
                for (std::uint32_t k = 0; k < Ids::kMarkerShapeSteps[s]; ++k)
                {
                    std::uint32_t const id = D(c, shape, k);
                    Ids::MarkerDisplayParts const parts = Ids::DecodeMarkerDisplay(id);
                    std::uint32_t const want = Ids::IsLineShape(shape) ? lineId++ : expected++;
                    if (id != want || !parts.valid || parts.color != c || parts.shape != shape || parts.step != k)
                        roundTrip = false;
                }
            }
        }
        CHECK(roundTrip);
        for (std::uint32_t bad : { 0u, 90500u, 90669u, 90999u, 91034u, 91066u, 91098u, 91099u, 91199u, 91834u,
                 91899u, 91990u, 91996u, 91999u, 92057u, 92099u, 92857u, 92900u, 93057u, 93857u, 93900u, 94000u })
            CHECK(!Ids::DecodeMarkerDisplay(bad).valid);
        // Out-of-range color or step clamps to a shipped display.
        CHECK(D(200, MarkerShape::Disc) == D(8, MarkerShape::Disc));
        CHECK(D(0, MarkerShape::Cone, 99) == D(0, MarkerShape::Cone, 7));
        // Every color draws the same geometry; only the color block differs.
        for (Shape const& shape : { Shape::Circle(5.f), Shape::Donut(4.f, 12.f), Shape::Cone(90.f, 20.f),
                 Shape::Line(100.f, 2.f) })
        {
            VisualPick const red = PickVisual(shape, 0, MarkerLayer::Fill);
            for (std::uint8_t c = 1; c < Ids::COLOR_COUNT; ++c)
            {
                VisualPick const other = PickVisual(shape, c, MarkerLayer::Fill);
                CHECK(other.pieces.size() == red.pieces.size());
                for (std::size_t k = 0; k < std::min(other.pieces.size(), red.pieces.size()); ++k)
                {
                    CHECK(other.pieces[k].display == red.pieces[k].display + Ids::DISPLAY_COLOR_STRIDE * c);
                    CHECK(other.pieces[k].scale == red.pieces[k].scale);
                    CHECK(other.pieces[k].forward == red.pieces[k].forward);
                }
            }
        }
    }

    // Model point (mx, my) of a marker at `pose` / `scale`, from a world point (the inverse of MarkerModelToWorld).
    std::pair<float, float> WorldToModel(Geo::Pose const& pose, float scale, Geo::Vec3 const& p)
    {
        float const dx = p.x - pose.x;
        float const dy = p.y - pose.y;
        float const c = std::cos(pose.o);
        float const s = std::sin(pose.o);
        return { (c * dx + s * dy) / scale, (-s * dx + c * dy) / scale };
    }

    // Every model point of every drawn line fill piece (x in [0, k], y in [-1/2, 1/2]) lies inside the drawn outline:
    // along the line between the source end and the outline's far end, and within half the width of the axis.
    bool FillInsideOutline(Shape const& shape, Geo::Pose const& anchor, VisualPick const& fill)
    {
        VisualPick const outline = PickVisual(shape, 0, MarkerLayer::Outline);
        if (outline.pieces.empty())
            return fill.pieces.empty();
        float const start = outline.pieces.front().forward;
        float const end = outline.pieces.back().forward + PieceExtent(outline.pieces.back());
        for (VisualPiece const& piece : fill.pieces)
        {
            if (!piece.display)
                continue;
            Geo::Pose const at = PieceWorldPose(anchor, piece);
            float const k = LineAspect(Ids::DecodeMarkerDisplay(piece.display).step);
            for (float mx = 0.f; mx <= k + 1e-6f; mx += k / 16.f)
                for (float my = -0.5f; my <= 0.5f + 1e-6f; my += 0.125f)
                {
                    Geo::LineCoords const c = Geo::ToLineCoords(shape, anchor,
                        MarkerModelToWorld(at, piece.scale, mx, my));
                    if (c.s < start - 1e-3f || c.s > end + 1e-3f || std::fabs(c.t) > shape.width * 0.5f + 1e-3f)
                        return false;
                }
        }
        return true;
    }

    // M1-FIX-LINES: the line marker and the line hitbox agree. Each outline piece stands at PieceWorldPose and draws
    // model x in [0, k] (forward), y in [-1/2, 1/2] at scale = width (MarkerModelToWorld); the hitbox is
    // Geo::Contains. Every drawn point is inside the hitbox (or in the far-end overshoot of the last piece) and every
    // hitbox point is drawn, for any anchor facing and facing offset (180 = a backwards line).
    void TestLinePlacement()
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> u(-1.f, 1.f);
        struct Case
        {
            float length;
            float width;
            float exclude;
            float offsetDeg;
        };
        Case const cases[] = { { 30.f, 4.f, 0.f, 0.f }, { 30.f, 4.f, 6.f, 0.f }, { 40.f, 2.f, 0.f, 0.f },
            { 100.f, 2.f, 5.f, 0.f }, { 2.f, 6.f, 0.f, 0.f }, { 30.f, 3.f, 0.f, 180.f }, { 25.f, 5.f, 3.f, -37.f },
            { 6.f, 6.f, 0.f, 90.f }, { 300.f, 2.f, 0.f, 0.f }, { 400.f, 1.5f, 7.f, 45.f } };
        bool drawnInside = true;
        bool hitboxDrawn = true;
        for (Case const& cs : cases)
            for (int trial = 0; trial < 8; ++trial)
            {
                Geo::Pose const anchor{ 50.f * u(rng), 50.f * u(rng), 3.f, Geo::kPi * u(rng) };
                Shape shape = Shape::Line(cs.length, cs.width);
                shape.excludeStart = cs.exclude;
                shape.facingOffsetDeg = cs.offsetDeg;
                VisualPick const pick = PickVisual(shape, 0, MarkerLayer::Outline);
                float const drawnEnd = pick.pieces.back().forward + PieceExtent(pick.pieces.back());
                for (VisualPiece const& piece : pick.pieces)
                {
                    Geo::Pose const at = PieceWorldPose(anchor, piece);
                    float const k = LineAspect(Step(piece.display));
                    for (float mx = 0.f; mx <= k + 1e-6f; mx += k / 16.f)
                        for (float my = -0.5f; my <= 0.5f + 1e-6f; my += 0.125f)
                        {
                            Geo::Vec3 const w = MarkerModelToWorld(at, piece.scale, mx, my);
                            Geo::LineCoords const c = Geo::ToLineCoords(shape, anchor, w);
                            if (c.s < cs.exclude - 1e-3f || c.s > drawnEnd + 1e-3f
                                || std::fabs(c.t) > cs.width * 0.5f + 1e-3f)
                                drawnInside = false;
                            if (c.s <= cs.length - 1e-3f && !Geo::Contains(shape, anchor, w))
                                drawnInside = false;
                        }
                }
                // Every hitbox point lies on some drawn piece.
                float const f = Geo::ShapeFacing(shape, anchor);
                for (float sAlong = cs.exclude; sAlong <= cs.length; sAlong += 0.25f)
                    for (float t = -cs.width * 0.5f; t <= cs.width * 0.5f + 1e-6f; t += cs.width / 8.f)
                    {
                        Geo::Vec3 const w{ anchor.x + std::cos(f) * sAlong - std::sin(f) * t,
                            anchor.y + std::sin(f) * sAlong + std::cos(f) * t, anchor.z };
                        if (!Geo::Contains(shape, anchor, w))
                            hitboxDrawn = false;
                        bool any = false;
                        for (VisualPiece const& piece : pick.pieces)
                        {
                            auto const [mx, my] = WorldToModel(PieceWorldPose(anchor, piece), piece.scale, w);
                            float const k = LineAspect(Step(piece.display));
                            if (mx >= -1e-3f && mx <= k + 1e-3f && std::fabs(my) <= 0.5f + 1e-3f)
                                any = true;
                        }
                        hitboxDrawn = hitboxDrawn && any;
                    }
            }
        CHECK(drawnInside);
        CHECK(hitboxDrawn);

        // The `.mech test line 30 4000 width=4` case, pinned: GM at the origin facing +x. The one outline piece
        // stands on the GM, faces +x and draws 0..32 yd ahead (aspect 8 at scale 4), where the hitbox is.
        Geo::Pose const gm{ 0.f, 0.f, 0.f, 0.f };
        Shape const line = Shape::Line(30.f, 4.f);
        VisualPick const pick = PickVisual(line, 0, MarkerLayer::Outline);
        CHECK(pick.pieces.size() == 1);
        Geo::Pose const at = PieceWorldPose(gm, pick.pieces[0]);
        CHECK_NEAR(at.x, 0.f, 1e-6);
        CHECK_NEAR(at.o, 0.f, 1e-6);
        float const k0 = LineAspect(Step(pick.pieces[0].display));
        Geo::Vec3 const tip = MarkerModelToWorld(at, pick.pieces[0].scale, k0, 0.f);
        CHECK_NEAR(tip.x, 32.f, 1e-3);
        CHECK(Geo::Contains(line, gm, Geo::Vec3{ 15.f, 1.9f, 0.f }));
        CHECK(!Geo::Contains(line, gm, Geo::Vec3{ -15.f, 0.f, 0.f }));
        // Rear (facing offset 180): marker and hitbox both behind.
        Shape rear = line;
        rear.facingOffsetDeg = 180.f;
        VisualPick const back = PickVisual(rear, 0, MarkerLayer::Outline);
        Geo::Vec3 const backTip = MarkerModelToWorld(PieceWorldPose(gm, back.pieces[0]), back.pieces[0].scale,
            LineAspect(Step(back.pieces[0].display)), 0.f);
        CHECK_NEAR(backTip.x, -32.f, 1e-3);
        CHECK(Geo::Contains(rear, gm, Geo::Vec3{ -15.f, 0.f, 0.f }));
        // The fill of a one-piece line stands where its outline piece stands, in the same display, at f x width.
        VisualPick const fill = PickFill(line, 0, 0.3f, false);
        CHECK(fill.pieces[0].forward == pick.pieces[0].forward && fill.pieces[0].facingOffset == 0.f);
        CHECK(fill.pieces[0].display == PickVisual(line, 0, MarkerLayer::Fill).pieces[0].display);
        CHECK_NEAR(fill.pieces[0].scale, 1.2f, 1e-5);

        // ExceptOne's last lane: the drawn strip (ApplyDrawStrip) stays inside the lane's hitbox and covers only the
        // strip. Frame 40 x 40 rotated 30 degrees, lanes of 6 along X: the last lane is [34, 40], drawn [36, 40].
        Frame fr;
        fr.x = 10.f;
        fr.y = -20.f;
        fr.angle = Geo::DegToRad(30.f);
        fr.sizeX = 40.f;
        fr.sizeY = 40.f;
        for (Axis axis : { Axis::X, Axis::Y })
        {
            LanePlacement const lane = PlaceLane(fr, { axis, 34.f, 6.f });
            LanePlacement const strip = PlaceLane(fr, { axis, 36.f, 4.f });
            Shape hit = Shape::Line(lane.length, lane.width);
            float const lateral = Geo::ToLineCoords(hit, lane.pose, { strip.pose.x, strip.pose.y, 0.f }).t;
            Shape drawnShape = hit;
            Geo::Pose drawnPose = lane.pose;
            ApplyDrawStrip(drawnShape, drawnPose, lateral, strip.width);
            CHECK_NEAR(drawnPose.x, strip.pose.x, 1e-3);
            CHECK_NEAR(drawnPose.y, strip.pose.y, 1e-3);
            CHECK(drawnShape.width == 4.f && hit.width == 6.f);
            // The fill of the drawn strip pivots on the strip's start and stays inside the strip.
            VisualPick const stripFill = PickFill(drawnShape, 0, 0.5f, false);
            Geo::Pose const stripAt = PieceWorldPose(drawnPose, stripFill.pieces[0]);
            CHECK_NEAR(stripAt.x, drawnPose.x, 1e-3);
            CHECK_NEAR(stripAt.y, drawnPose.y, 1e-3);
            CHECK(FillInsideOutline(drawnShape, drawnPose, stripFill));
            bool inside = true;
            for (float fa = 0.5f; fa < 40.f; fa += 0.5f)
                for (float fc = 34.1f; fc < 40.f; fc += 0.1f)
                {
                    Geo::Pose const q = axis == Axis::X ? FramePoint(fr, fa, fc, 0.f) : FramePoint(fr, fc, fa, 0.f);
                    Geo::Vec3 const v{ q.x, q.y, 0.f };
                    if (!Geo::Contains(hit, lane.pose, v))
                        inside = false;                                     // the hitbox is the whole lane
                    if (Geo::Contains(drawnShape, drawnPose, v) != (fc >= 36.f - 1e-3f))
                        inside = false;                                     // the marker only the strip
                }
            CHECK(inside);
        }
    }

    // Every shape the tests sweep: (shape, color).
    std::vector<std::pair<Shape, std::uint8_t>> FillShapes()
    {
        std::vector<std::pair<Shape, std::uint8_t>> out;
        for (float r : { 0.5f, 2.f, 5.f, 10.f, 40.f })
            out.push_back({ Shape::Circle(r), static_cast<std::uint8_t>(static_cast<int>(r) % Ids::COLOR_COUNT) });
        for (float inner : { 0.f, 2.f, 3.f, 4.5f, 6.f, 9.f, 11.f })
            out.push_back({ Shape::Donut(inner, 12.f), 5 });
        for (float a : { 10.f, 60.f, 90.f, 100.f, 270.f, 300.f })
            out.push_back({ Shape::Cone(a, 20.f), 7 });
        for (float len : { 0.3f, 2.f, 3.f, 7.f, 24.f, 25.f, 40.f, 100.f, 192.f, 300.f, 1000.f })
            out.push_back({ Shape::Line(len, 3.f), 8 });
        Shape ex = Shape::Line(30.f, 6.f);
        ex.excludeStart = 6.f;
        out.push_back({ ex, 2 });
        Shape rear = Shape::Cone(90.f, 8.f);
        rear.facingOffsetDeg = 180.f;
        out.push_back({ rear, 4 });
        return out;
    }

    bool IsLine(std::uint32_t display)
    {
        Ids::MarkerDisplayParts const parts = Ids::DecodeMarkerDisplay(display);
        return parts.valid && (parts.shape == MarkerShape::Line || parts.shape == MarkerShape::LineOutline);
    }

    void TestLayers()
    {
        // Outline and fill draw the same geometry: same piece count, scale, facing and placement; only the ids differ.
        for (auto const& [shape, color] : FillShapes())
        {
            VisualPick const o = PickVisual(shape, color, MarkerLayer::Outline);
            VisualPick const f = PickVisual(shape, color, MarkerLayer::Fill);
            CHECK(o.pieces.size() == f.pieces.size());
            CHECK(o.clamped == f.clamped && o.filled == f.filled);
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
        auto outlineId = [](Shape const& shape, std::uint8_t color)
        {
            return PickVisual(shape, color, MarkerLayer::Outline).pieces[0].display;
        };
        CHECK(outlineId(Shape::Circle(8.f), 2) == D(2, MarkerShape::Ring));
        CHECK(outlineId(Shape::Donut(4.f, 12.f), 1) == D(1, MarkerShape::DonutOutline, 1));
        CHECK(outlineId(Shape::Donut(1.f, 12.f), 0) == D(0, MarkerShape::Ring));              // drawn as a disc
        CHECK(outlineId(Shape::Cone(90.f, 20.f), 6) == D(6, MarkerShape::ConeOutline, 3));
        CHECK(outlineId(Shape::Cone(300.f, 20.f), 1) == D(1, MarkerShape::Ring));             // drawn as a disc
        CHECK(outlineId(Shape::Line(12.f, 3.f), 8) == D(8, MarkerShape::LineOutline, 24));
        // Outline rounding promises: a get-out outline is never smaller than its hitbox; a donut hole never bigger.
        for (float len = 0.5f; len <= 200.f; len += 0.73f)
        {
            VisualPick const o = PickVisual(Shape::Line(len, 2.5f), 0, MarkerLayer::Outline);
            CHECK(o.pieces.back().forward + PieceExtent(o.pieces.back()) >= len - 1e-3f);
        }
        for (int inner = 30; inner < 120; ++inner)
        {
            VisualPick const o = PickVisual(Shape::Donut(inner / 10.f, 12.f), 0, MarkerLayer::Outline);
            float const drawn = Ids::kDonutRatios[Step(o.pieces[0].display)];
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
        for (auto const& [shape, color] : FillShapes())
        {
            VisualPick const outline = PickVisual(shape, color, MarkerLayer::Outline);
            VisualPick const full = PickVisual(shape, color, MarkerLayer::Fill);
            bool const donutRing = shape.type == ShapeType::Donut && !full.filled;

            // Live danger: exactly the full fill.
            VisualPick const live = PickFill(shape, color, 0.3f, true);
            CHECK(live.pieces.size() == full.pieces.size());
            for (std::size_t k = 0; k < live.pieces.size(); ++k)
                CHECK(live.pieces[k].display == full.pieces[k].display && live.pieces[k].scale == full.pieces[k].scale);

            float prevTotal = -1.f;
            float prevRing = 1e9f;
            bool monotonic = true;
            bool neverAbove = true;
            bool noSwap = true;
            bool lineScaled = true;
            bool lineInside = true;
            float const source = outline.pieces.empty() ? 0.f : outline.pieces.front().forward;
            for (int step = 0; step <= 200; ++step)
            {
                float const f = step / 200.f;
                VisualPick const p = PickFill(shape, color, f, false);
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
                    if (piece.facingOffset != o.facingOffset)
                        neverAbove = false;                                 // same facing
                    if (shape.type != ShapeType::Line && piece.display && piece.forward != o.forward)
                        neverAbove = false;                                 // same anchor point
                    // No display swap during the timer: a drawn fill piece is always its full-fill display (the
                    // donut's timer ring aside: it is one display from start to end).
                    if (piece.display && !donutRing && piece.display != full.pieces[k].display)
                        noSwap = false;
                    if (shape.type == ShapeType::Line)
                    {
                        // A copy of the whole line scaled by f about the source end: piece k at f x width, standing
                        // f x (start_k - source) from the source.
                        if (std::fabs(piece.scale - f * o.scale) > 1e-4f
                            || std::fabs(piece.forward - (source + f * (o.forward - source))) > 1e-3f)
                            lineScaled = false;
                        if (piece.display && !IsLine(piece.display))
                            lineScaled = false;
                        if (piece.display != 0 && f * o.scale < Rules::kFillMinScaleYd)
                            lineScaled = false;
                        if (piece.display == 0 && f * o.scale >= Rules::kFillMinScaleYd)
                            lineScaled = false;                             // drawn from the first step it is 0.1 yd
                    }
                    if (piece.display && piece.scale < Rules::kFillMinScaleYd)
                        neverAbove = false;                                 // no near-zero creature
                    total += extent;
                }
                if (shape.type == ShapeType::Line && !FillInsideOutline(shape, Geo::Pose{ 3.f, -2.f, 0.f, 0.7f }, p))
                    lineInside = false;
                if (donutRing)
                {
                    VisualPiece const& ring = p.pieces[0];
                    CHECK(ring.display == D(color, MarkerShape::TimerRing));
                    // shrinks from the outer rim; its band never enters the drawn hole
                    float const hole = Ids::kDonutRatios[Step(full.pieces[0].display)]
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
                        CHECK_NEAR(p.pieces[k].forward, full.pieces[k].forward, 1e-4);
                    }
                }
            }
            CHECK(monotonic);
            CHECK(neverAbove);
            CHECK(noSwap);
            CHECK(lineScaled);
            CHECK(lineInside);
        }

        // Circle: linear in f from the centre.
        VisualPick v = PickFill(Shape::Circle(10.f), 0, 0.25f, false);
        CHECK(v.pieces.size() == 1 && v.pieces[0].display == D(0, MarkerShape::Disc));
        CHECK_NEAR(v.pieces[0].scale, 2.5f, 1e-5);
        CHECK(PickFill(Shape::Circle(10.f), 0, 0.005f, false).pieces[0].display == 0);      // 0.05 yd: not yet
        // Cone: grows from the apex, same angle display, facing kept.
        v = PickFill(Shape::Cone(90.f, 20.f), 1, 0.5f, false);
        CHECK(v.pieces[0].display == D(1, MarkerShape::Cone, 3));
        CHECK_NEAR(v.pieces[0].scale, 10.f, 1e-5);
        // Donut 4/12 (drawn 0.33): the ring ends with its inner edge on the 3.96 yd drawn hole.
        v = PickFill(Shape::Donut(4.f, 12.f), 0, 1.f, false);
        CHECK(v.pieces[0].display == D(0, MarkerShape::TimerRing));
        CHECK_NEAR(v.pieces[0].scale, 0.33f * 12.f / 0.96f, 1e-4);
        v = PickFill(Shape::Donut(4.f, 12.f), 0, 1.f, true);                                // live: filled donut
        CHECK(v.pieces[0].display == D(0, MarkerShape::Donut, 1));
        // A donut drawn as a disc fills like a circle.
        v = PickFill(Shape::Donut(1.f, 12.f), 0, 0.5f, false);
        CHECK(v.pieces[0].display == D(0, MarkerShape::Disc));
        CHECK_NEAR(v.pieces[0].scale, 6.f, 1e-5);
        // Line 24 x 3 (aspect 8, one piece, step 32): one display from the first frame to the hit, growing longer and
        // wider together from the source end; nothing below 0.1 yd wide.
        v = PickFill(Shape::Line(24.f, 3.f), 0, 0.01f, false);                              // 0.03 yd: not yet
        CHECK(v.pieces[0].display == 0);
        v = PickFill(Shape::Line(24.f, 3.f), 0, 0.05f, false);                              // 0.15 yd wide
        CHECK(v.pieces[0].display == D(0, MarkerShape::Line, 32));
        CHECK_NEAR(v.pieces[0].scale, 0.15f, 1e-5);
        CHECK(v.pieces[0].forward == 0.f);
        v = PickFill(Shape::Line(24.f, 3.f), 0, 0.5f, false);                               // 12 x 1.5 yd
        CHECK(v.pieces[0].display == D(0, MarkerShape::Line, 32));
        CHECK_NEAR(v.pieces[0].scale, 1.5f, 1e-5);
        CHECK_NEAR(PieceExtent(v.pieces[0]), 12.f, 1e-3);
        // Line 300 x 2 (128 + 128 + 44 yd): at half, three pieces 1 yd wide at 0, 64 and 128 yd, in their own
        // displays (they abut, as the outline's do).
        v = PickFill(Shape::Line(300.f, 2.f), 0, 0.5f, false);
        CHECK(v.pieces.size() == 3);
        CHECK(v.pieces[0].display == D(0, MarkerShape::Line, 56) && v.pieces[2].display == D(0, MarkerShape::Line, 44));
        CHECK_NEAR(v.pieces[1].forward, 64.f, 1e-3);
        CHECK_NEAR(v.pieces[2].forward, 128.f, 1e-3);
        CHECK_NEAR(v.pieces[0].forward + PieceExtent(v.pieces[0]), v.pieces[1].forward, 1e-3);
        CHECK_NEAR(v.pieces[2].scale, 1.f, 1e-5);
        // Short wide line 2 x 6 (outline step 2): a scaled copy, 4.8 yd wide at f = 0.8.
        v = PickFill(Shape::Line(2.f, 6.f), 0, 0.8f, false);
        CHECK(v.pieces[0].display == D(0, MarkerShape::Line, 2));
        CHECK_NEAR(v.pieces[0].scale, 4.8f, 1e-5);
        // Excluded start: the fill pivots on the exclusion (the drawn line's source end).
        Shape ex = Shape::Line(30.f, 6.f);
        ex.excludeStart = 6.f;
        v = PickFill(ex, 0, 0.5f, false);
        CHECK(v.pieces.size() == 1 && v.pieces[0].forward == 6.f);
        CHECK(v.pieces[0].display == D(0, MarkerShape::Line, 24));                          // 24 / 6 = 4: step 24
        CHECK_NEAR(v.pieces[0].scale, 3.f, 1e-5);

        // Armed mine: the outline's ring becomes the dashed ring (same scale); nothing else changes.
        VisualPick const armed = PickArmedMine(Shape::Circle(4.f), 5);
        CHECK(armed.pieces.size() == 1 && armed.pieces[0].display == D(5, MarkerShape::DashedRing));
        CHECK(armed.pieces[0].scale == 4.f && armed.pieces[0].forward == 0.f);
        CHECK(Ids::DecodeMarkerDisplay(armed.pieces[0].display).shape == MarkerShape::DashedRing);
    }

    // Round 4b Q13: the `.mech test lanes 6 4000 count=3 cross=1` layout (frame 40 x 40, lanes 6 wide, gaps >= 6, X
    // lanes plus Y lanes, a cell at every crossing), over many random layouts and a rotated frame. For every lane and
    // cell, both layers (outline, live fill, and the fill mid-timer): the hitbox is the lane / crossing rectangle,
    // and the drawn rectangle (model x in [0, k], y in [-1/2, 1/2] at the piece's scale and pose) is that rectangle
    // too: exactly for a cell (aspect 1 is a ladder step), and for a lane up to its ladder round-up at the far end.
    // So the server draws each cell on its crossing; a cell that looks longer in game is the client's drawing.
    void TestCrossingLayout()
    {
        // The drawn rectangle of one line piece, in frame coordinates: (fx lo, fx hi, fy lo, fy hi).
        auto drawnRect = [](Frame const& f, Geo::Pose const& anchor, VisualPiece const& piece)
        {
            Geo::Pose const at = PieceWorldPose(anchor, piece);
            float const k = PieceExtent(piece) / piece.scale;
            std::array<float, 4> r{ 1e9f, -1e9f, 1e9f, -1e9f };
            float const c = std::cos(f.angle);
            float const sn = std::sin(f.angle);
            for (float mx : { 0.f, k })
                for (float my : { -0.5f, 0.5f })
                {
                    Geo::Vec3 const w = MarkerModelToWorld(at, piece.scale, mx, my);
                    float const fx = c * (w.x - f.x) + sn * (w.y - f.y);
                    float const fy = -sn * (w.x - f.x) + c * (w.y - f.y);
                    r[0] = std::min(r[0], fx);
                    r[1] = std::max(r[1], fx);
                    r[2] = std::min(r[2], fy);
                    r[3] = std::max(r[3], fy);
                }
            return r;
        };
        auto inRect = [](Frame const& f, Shape const& shape, Geo::Pose const& pose, float fx0, float fx1, float fy0,
            float fy1)
        {
            // Points just inside the rectangle are in the hitbox, points just outside are not.
            bool ok = true;
            for (float a = 0.05f; a < 1.f; a += 0.1f)
                for (float b = 0.05f; b < 1.f; b += 0.1f)
                {
                    Geo::Pose const q = FramePoint(f, fx0 + a * (fx1 - fx0), fy0 + b * (fy1 - fy0), 0.f);
                    ok = ok && Geo::Contains(shape, pose, Geo::Vec3{ q.x, q.y, f.z });
                }
            float const midX = (fx0 + fx1) * 0.5f;
            float const midY = (fy0 + fy1) * 0.5f;
            for (auto [qx, qy] : { std::pair{ fx0 - 0.05f, midY }, std::pair{ fx1 + 0.05f, midY },
                std::pair{ midX, fy0 - 0.05f }, std::pair{ midX, fy1 + 0.05f } })
            {
                Geo::Pose const q = FramePoint(f, qx, qy, 0.f);
                ok = ok && !Geo::Contains(shape, pose, Geo::Vec3{ q.x, q.y, f.z });
            }
            return ok;
        };

        bool cellsExact = true;
        bool cellsHit = true;
        bool lanesExact = true;
        bool lanesHit = true;
        bool fillsInside = true;
        int cells = 0;
        for (std::uint32_t seed = 1; seed <= 200; ++seed)
        {
            std::mt19937 rng(seed);
            std::uniform_real_distribution<float> dist(0.f, 1.f);
            auto rnd = [&]() { return dist(rng); };
            Frame f;
            f.x = 300.f * dist(rng) - 150.f;
            f.y = 300.f * dist(rng) - 150.f;
            f.z = 12.f;
            f.angle = Geo::kTwoPi * dist(rng);
            f.sizeX = 40.f;
            f.sizeY = 40.f;
            std::vector<LaneRect> rects;
            for (Axis axis : { Axis::X, Axis::Y })
                for (float o : RandomLaneOffsets(FrameSpanAcross(f, axis), 3, 6.f, 6.f, rnd))
                    rects.push_back({ axis, o, 6.f });
            CHECK(rects.size() == 6);

            for (LaneRect const& lane : rects)
            {
                LanePlacement const p = PlaceLane(f, lane);
                Shape const shape = Shape::Line(p.length, p.width);
                bool const x = lane.axis == Axis::X;
                float const along0 = 0.f;
                float const along1 = x ? f.sizeX : f.sizeY;
                // The hitbox: [0, 40] along, [offset, offset + 6] across, in frame coordinates.
                lanesHit = lanesHit && (x ? inRect(f, shape, p.pose, along0, along1, lane.offset, lane.offset + 6.f)
                    : inRect(f, shape, p.pose, lane.offset, lane.offset + 6.f, along0, along1));
                for (MarkerLayer layer : { MarkerLayer::Outline, MarkerLayer::Fill })
                {
                    VisualPick const pick = PickVisual(shape, 0, layer);
                    CHECK(pick.pieces.size() == 1);
                    std::array<float, 4> const r = drawnRect(f, p.pose, pick.pieces[0]);
                    float const lo = x ? r[0] : r[2];
                    float const hi = x ? r[1] : r[3];
                    float const acrossLo = x ? r[2] : r[0];
                    float const acrossHi = x ? r[3] : r[1];
                    // Drawn from the frame edge to 40 yd, rounded up at most one ladder step (9%) at the far end.
                    lanesExact = lanesExact && std::fabs(lo) < 1e-3f && hi >= 40.f - 1e-3f && hi <= 40.f * 1.0906f
                        && std::fabs(acrossLo - lane.offset) < 1e-3f && std::fabs(acrossHi - lane.offset - 6.f) < 1e-3f;
                }
            }

            for (LaneRect const& xl : rects)
                for (LaneRect const& yl : rects)
                {
                    if (xl.axis != Axis::X || yl.axis != Axis::Y)
                        continue;
                    ++cells;
                    LanePlacement const c = PlaceCrossing(f, xl, yl);
                    Shape const shape = Shape::Line(c.length, c.width);
                    cellsHit = cellsHit && inRect(f, shape, c.pose, yl.offset, yl.offset + 6.f, xl.offset,
                        xl.offset + 6.f);
                    VisualPick const outline = PickVisual(shape, 3, MarkerLayer::Outline);
                    VisualPick const live = PickFill(shape, 3, 0.f, true);
                    CHECK(outline.pieces.size() == 1 && live.pieces.size() == 1);
                    CHECK(outline.pieces[0].display == D(3, MarkerShape::LineOutline, Ids::kLineUnitStep));   // 93308
                    CHECK(live.pieces[0].display == D(3, MarkerShape::Line, Ids::kLineUnitStep));            // 92308
                    for (VisualPiece const& piece : { outline.pieces[0], live.pieces[0] })
                    {
                        CHECK_NEAR(PieceExtent(piece), 6.f, 1e-5);
                        CHECK_NEAR(piece.scale, 6.f, 1e-6);
                        std::array<float, 4> const r = drawnRect(f, c.pose, piece);
                        cellsExact = cellsExact && std::fabs(r[0] - yl.offset) < 1e-3f
                            && std::fabs(r[1] - yl.offset - 6.f) < 1e-3f && std::fabs(r[2] - xl.offset) < 1e-3f
                            && std::fabs(r[3] - xl.offset - 6.f) < 1e-3f;
                    }
                    // The timer fill (a cell never runs one: NoFillTimer; checked for completeness) stays inside.
                    for (float fr : { 0.1f, 0.5f, 0.9f })
                        fillsInside = fillsInside && FillInsideOutline(shape, c.pose, PickFill(shape, 3, fr, false));
                }
        }
        CHECK(cells == 200 * 9);
        CHECK(cellsExact);
        CHECK(cellsHit);
        CHECK(lanesExact);
        CHECK(lanesHit);
        CHECK(fillsInside);
    }

    // Round 4b Q8 / Q15: fill pacing by distance.
    void TestFillPacing()
    {
        // How far the leading edge travels.
        CHECK_NEAR(FillTravel(Shape::Line(30.f, 4.f)), 32.f, 1e-4);              // one piece, aspect 8 at width 4
        CHECK_NEAR(FillTravel(Shape::Line(6.f, 6.f)), 6.f, 1e-4);
        Shape ex = Shape::Line(30.f, 3.f);
        ex.excludeStart = 5.f;
        CHECK_NEAR(FillTravel(ex), PieceExtent(PickVisual(ex, 0, MarkerLayer::Fill).pieces[0]), 1e-4);   // from 5 yd
        Shape const multi = Shape::Line(100.f, 1.5f);                             // 96 + 4 yd
        VisualPick const mp = PickVisual(multi, 0, MarkerLayer::Fill);
        CHECK(mp.pieces.size() == 2);
        CHECK_NEAR(FillTravel(multi), mp.pieces[1].forward + PieceExtent(mp.pieces[1]), 1e-3);
        CHECK(FillTravel(multi) >= 100.f - 1e-3f);
        CHECK_NEAR(FillTravel(Shape::Circle(5.f)), 5.f, 1e-6);
        CHECK_NEAR(FillTravel(Shape::Cone(90.f, 20.f)), 20.f, 1e-6);
        CHECK_NEAR(FillTravel(Shape::Cone(300.f, 20.f)), 20.f, 1e-6);           // drawn as a disc
        CHECK_NEAR(FillTravel(Shape::Donut(6.f, 12.f)), 12.f - DonutTimerEnd(12.f, 3), 1e-5);          // ratio 0.50
        CHECK_NEAR(FillTravel(Shape::Donut(1.f, 12.f)), 12.f, 1e-6);            // below the ladder: a disc
        CHECK(FillTravel(Shape::Line(0.f, 4.f)) == 0.f);

        // The interval: 0.4 yd per step, between 50 ms and FillStepMs.
        CHECK(FillStepInterval(Ms(4000), 32.f, 0.4f, 150) == 50);                // 30 x 4 line: 50 ms (Q15's feel)
        CHECK(FillStepInterval(Ms(4000), 100.f, 0.4f, 150) == 50);               // capped at 50 ms
        CHECK(FillStepInterval(Ms(4000), 5.f, 0.4f, 150) == 150);                // 5 yd circle: FillStepMs
        CHECK(FillStepInterval(Ms(4000), 16.f, 0.4f, 150) == 100);
        CHECK(FillStepInterval(Ms(6000), 32.f, 0.4f, 150) == 75);
        CHECK(FillStepInterval(Ms(4000), 32.f, 0.f, 150) == 150);                // pacing off
        CHECK(FillStepInterval(Ms(0), 32.f, 0.4f, 150) == 150);                  // no warning
        CHECK(FillStepInterval(Ms(4000), 0.f, 0.4f, 150) == 150);                // nothing to time
        CHECK(FillStepInterval(Ms(4000), 32.f, 0.4f, 50) == 50);                 // FillStepMs 50: always 50
        CHECK(FillStepInterval(Ms(4000), 32.f, 0.4f, 300) == 50);

        // The schedule the telegraph runs (one step per interval from the spawn, FillLeadMs 150): the leading edge
        // never moves more than 0.4 yd per step unless the 50 ms floor holds it, and the last step before the resolve
        // asks for the full fill.
        std::vector<Shape> shapes;
        for (float len : { 2.f, 6.f, 12.f, 30.f, 60.f, 100.f, 300.f })
            shapes.push_back(Shape::Line(len, 4.f));
        for (float r : { 1.f, 5.f, 20.f, 40.f })
            shapes.push_back(Shape::Circle(r));
        shapes.push_back(Shape::Cone(90.f, 30.f));
        shapes.push_back(Shape::Donut(4.f, 30.f));
        bool paced = true;
        bool full = true;
        for (Shape const& shape : shapes)
            for (std::int64_t warn : { 1000, 2500, 4000, 8000 })
            {
                float const travel = FillTravel(shape);
                std::uint32_t const step = FillStepInterval(Ms(warn), travel, 0.4f, 150);
                CHECK(step >= 50 && step <= 150);
                float const floorMove = travel * 50.f / static_cast<float>(warn);
                float last = -1.f;
                float lastF = 0.f;
                for (std::int64_t t = 0; t < warn; t += step)
                {
                    float const f = FillFraction(Ms(0), Ms(warn), Ms(t), Ms(150));
                    if (last >= 0.f && (f - last) * travel > std::max(0.4f, floorMove) + 1e-3f)
                        paced = false;
                    last = f;
                    lastF = f;
                }
                full = full && lastF == 1.f;
            }
        CHECK(paced);
        CHECK(full);
    }

    // Round 4b Q16: the projectile shrink moves the drawn start in whole widths (at least 1 yd), never past the tail.
    void TestProjectileShrink()
    {
        CHECK(ShrunkExcludeStart(-2.f, 0.f, 30.f, 4.f) == 0.f);                  // tail not on the line yet
        CHECK(ShrunkExcludeStart(3.9f, 0.f, 30.f, 4.f) == 0.f);
        CHECK(ShrunkExcludeStart(4.f, 0.f, 30.f, 4.f) == 4.f);
        CHECK(ShrunkExcludeStart(11.f, 0.f, 30.f, 4.f) == 8.f);
        CHECK(ShrunkExcludeStart(40.f, 0.f, 30.f, 4.f) == 30.f);                 // past the end: capped at the length
        CHECK(ShrunkExcludeStart(31.f, 0.f, 30.f, 3.f) == 30.f);                 // 30: nothing left to draw
        CHECK(ShrunkExcludeStart(9.f, 5.f, 30.f, 3.f) == 8.f);                   // steps from the excluded start
        CHECK(ShrunkExcludeStart(2.7f, 0.f, 30.f, 0.5f) == 2.f);                 // thin line: 1 yd steps
        // Never past the tail, never before the real start, and a 30 x 4 projectile at 10 yd/s re-picks its outline
        // only when the drawn start moves (once per 4 yd: at most 8 starts over the line, not one per follow step).
        std::set<float> starts;
        bool ok = true;
        for (float tail = -2.f; tail <= 32.f; tail += 2.f)          // the tail every 200 ms at 10 yd/s
        {
            float const st = ShrunkExcludeStart(tail, 0.f, 30.f, 4.f);
            ok = ok && st >= 0.f && (tail <= 0.f || st <= tail + 1e-4f) && st <= 30.f;
            starts.insert(st);
        }
        CHECK(ok);
        CHECK(starts.size() <= 9);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Ground snap (MechanicGround.h)
    // ------------------------------------------------------------------------------------------------------------

    // A synthetic world: a ground field plus an optional ceiling layer (a bridge or a roof) and a lower floor. The
    // probe returns the highest layer at or below zFrom (within maxDown), like Map::GetHeight.
    struct World
    {
        std::function<float(float, float)> ground;
        std::function<float(float, float)> ceiling;     // empty: none; NaN at a point: none there
        uint32_t calls = 0;

        float operator()(float x, float y, float zFrom, float maxDown)
        {
            ++calls;
            float best = Ground::kInvalidHeight - 1.f;
            auto consider = [&](float h)
            {
                if (std::isfinite(h) && h <= zFrom + 1e-3f && h >= zFrom - maxDown - 1e-3f)
                    best = std::max(best, h);
            };
            consider(ground(x, y));
            if (ceiling)
                consider(ceiling(x, y));
            return best;
        }
    };

    float Slope(float x, float y)
    {
        return 0.12f * x - 0.05f * y;
    }

    float Bumps(float x, float y)
    {
        auto g = [](float dx, float dy, float a, float sigma)
        {
            return a * std::exp(-(dx * dx + dy * dy) / (2.f * sigma * sigma));
        };
        return g(x - 12.f, y - 3.f, 1.6f, 4.f) - g(x - 28.f, y + 6.f, 1.2f, 5.f) + g(x + 5.f, y - 15.f, 2.f, 4.5f);
    }

    float StepField(float x, float /*y*/)
    {
        return x < 17.f ? 0.f : 1.5f;
    }

    float Flat(float, float)
    {
        return 0.f;
    }

    // Points every `d` yd over a marker piece's drawn area (line rectangle, cone sector, disc), for the dense check.
    std::vector<Ground::Point2> Dense(Geo::Pose const& at, VisualPiece const& piece, float d)
    {
        Ground::PieceShape const ps = Ground::ShapeOfPiece(piece);
        std::vector<Ground::Point2> out;
        float const c = std::cos(at.o);
        float const s = std::sin(at.o);
        if (ps.type == Ground::PieceShape::Type::Line)
        {
            for (float a = 0.f; a <= ps.length + 1e-4f; a += d)
                for (float t = -ps.width * 0.5f; t <= ps.width * 0.5f + 1e-4f; t += d)
                    out.push_back({ at.x + c * a - s * t, at.y + s * a + c * t });
            return out;
        }
        for (float dx = -ps.radius; dx <= ps.radius + 1e-4f; dx += d)
            for (float dy = -ps.radius; dy <= ps.radius + 1e-4f; dy += d)
            {
                float const r = std::hypot(dx, dy);
                if (r > ps.radius || r < ps.inner)
                    continue;
                if (ps.type == Ground::PieceShape::Type::Cone && r > 1e-4f
                    && std::fabs(Geo::WrapPi(std::atan2(dy, dx) - at.o)) > ps.halfAngle)
                    continue;
                out.push_back({ at.x + dx, at.y + dy });
            }
        return out;
    }

    // The worst (ground - (z - lift)) over a pick's pieces: dense points and the piece's own footprint.
    struct Clearance
    {
        float dense = -1e9f;
        float footprint = -1e9f;
    };

    Clearance Check(VisualPick const& pick, Geo::Pose const& anchor, std::function<float(float, float)> const& f,
        float lift, float step)
    {
        Clearance out;
        for (VisualPiece const& piece : pick.pieces)
        {
            Geo::Pose const at = PieceWorldPose(anchor, piece);
            for (Ground::Point2 const& p : Dense(at, piece, 0.25f))
                out.dense = std::max(out.dense, f(p.x, p.y) - (piece.z - lift));
            for (Ground::Point2 const& p : Ground::Footprint(at, piece, step))
                out.footprint = std::max(out.footprint, f(p.x, p.y) - (piece.z - lift));
        }
        return out;
    }

    // The split laid out: abutting pieces from excludeStart, only the last one past the end.
    bool CoversExactly(Shape const& shape, VisualPick const& pick)
    {
        if (pick.pieces.empty())
            return false;
        float at = shape.excludeStart;
        for (std::size_t k = 0; k < pick.pieces.size(); ++k)
        {
            VisualPiece const& p = pick.pieces[k];
            if (std::fabs(p.forward - at) > 1e-3f)
                return false;
            at += PieceExtent(p);
            bool const last = k + 1 == pick.pieces.size();
            if (!last && at >= shape.length - 1e-4f)
                return false;
            if (last && at < shape.length - 1e-3f)
                return false;
        }
        return true;
    }

    VisualPick SplitPick(Shape const& shape, Geo::Pose const& pose, World& world, Ground::Settings const& st,
        Ground::LineSplit* outSplit = nullptr, uint32_t* queries = nullptr)
    {
        Ground::Sampler<World> sampler(world, pose.z, shape.zTolerance);
        Ground::LineSplit const split = Ground::SplitLine(shape, pose, sampler, st);
        VisualPick pick = PickVisual(shape, 0, MarkerLayer::Outline, &split.steps);
        for (std::size_t k = 0; k < pick.pieces.size() && k < split.z.size(); ++k)
        {
            pick.pieces[k].hasZ = true;
            pick.pieces[k].z = split.z[k];
        }
        if (outSplit)
            *outSplit = split;
        if (queries)
            *queries = sampler.Queries();
        return pick;
    }

    void TestGroundFlat()
    {
        Ground::Settings st;
        World world{ Flat, {} };
        // Flat ground: the default layout (one piece, or 64-width pieces then the rest), every piece at lift.
        for (auto [len, w] : { std::pair{ 40.f, 6.667f }, { 6.f, 6.f }, { 48.f, 4.f }, { 200.f, 2.f }, { 1.f, 6.f } })
        {
            Shape shape = Shape::Line(len, w);
            Geo::Pose const pose{ 3.f, -4.f, 0.f, 0.7f };
            Ground::LineSplit split;
            uint32_t queries = 0;
            VisualPick const pick = SplitPick(shape, pose, world, st, &split, &queries);
            VisualPick const old = PickVisual(shape, 0, MarkerLayer::Outline);
            CHECK(split.steps == DefaultLineSteps(shape));
            CHECK(pick.pieces.size() == old.pieces.size());
            bool same = pick.pieces.size() == old.pieces.size();
            for (std::size_t k = 0; same && k < pick.pieces.size(); ++k)
                same = pick.pieces[k].display == old.pieces[k].display && pick.pieces[k].scale == old.pieces[k].scale
                    && pick.pieces[k].forward == old.pieces[k].forward;
            CHECK(same);
            for (float z : split.z)
                CHECK_NEAR(z, st.lift, 1e-5);
            CHECK(!split.minLengthStopped);
            if (len == 40.f)
                CHECK(queries <= 140);          // a 40 x 6.67 lane: 21+ rows x 5 columns, one query each on flat ground
        }
        // A layout made for another shape is ignored (the default is drawn).
        Shape a = Shape::Line(40.f, 4.f);
        LineSteps const wrong = { 3 };
        VisualPick const pa = PickVisual(a, 0, MarkerLayer::Outline, &wrong);
        CHECK(pa.pieces.size() == 1
            && pa.pieces[0].display == PickVisual(a, 0, MarkerLayer::Outline).pieces[0].display);
        // A disc on flat ground: lift, its centre included.
        Ground::Sampler<World> sampler(world, 0.f, 6.f);
        VisualPiece disc = PickVisual(Shape::Circle(8.f), 0, MarkerLayer::Outline).pieces.front();
        CHECK_NEAR(Ground::PieceZ(sampler, Geo::Pose{ 1.f, 1.f, 0.f, 0.f }, disc, st, false), st.lift, 1e-6);
    }

    void TestGroundFields()
    {
        Ground::Settings st;
        struct Field
        {
            char const* name;
            float (*f)(float, float);
            bool exact;         // planar or piecewise constant: the footprint's own samples clear exactly
        };
        Field const fields[] = { { "slope", Slope, true }, { "bumps", Bumps, false }, { "step", StepField, true } };
        struct LineCase
        {
            float len, w, exclude, facing, x, y;
        };
        LineCase const lines[] = { { 40.f, 6.667f, 0.f, 0.f, -20.f, 0.f }, { 40.f, 6.667f, 0.f, 1.5708f, 5.f, -20.f },
            { 60.f, 3.f, 0.f, 0.3f, -10.f, -8.f }, { 30.f, 4.f, 6.f, -0.8f, 0.f, 10.f }, { 120.f, 1.f, 0.f, 0.1f,
            -50.f, 0.f }, { 8.f, 8.f, 0.f, 2.f, 14.f, 0.f } };
        bool covered = true;
        bool cleared = true;
        bool clearedExact = true;
        bool spreadOk = true;
        bool order = true;
        std::size_t splitCount = 0;
        for (Field const& field : fields)
        {
            World world{ field.f, {} };
            for (LineCase const& lc : lines)
            {
                Shape shape = Shape::Line(lc.len, lc.w);
                shape.excludeStart = lc.exclude;
                shape.zTolerance = 30.f;            // the whole field is in band (the band itself: TestGroundBand)
                Geo::Pose const pose{ lc.x, lc.y, field.f(lc.x, lc.y), lc.facing };
                Ground::LineSplit split;
                VisualPick const pick = SplitPick(shape, pose, world, st, &split);
                covered = covered && CoversExactly(shape, pick) && LineStepsCover(shape, split.steps);
                Clearance const c = Check(pick, pose, field.f, st.lift, st.sampleYd);
                cleared = cleared && c.dense <= st.lift + 1e-4f && c.dense <= 0.15f;
                if (field.exact)
                    clearedExact = clearedExact && c.footprint <= 1e-3f && c.dense <= 1e-3f;
                // Spread limit: within it, or a non-last piece at the minimum length, or a last piece too short
                // to cut.
                float const minLen = LineAspect(LineStepFor(Ground::MinLinePieceYd(shape) / shape.width)) * shape.width;
                for (std::size_t k = 0; k < split.spread.size(); ++k)
                {
                    float const len = PieceExtent(pick.pieces[k]);
                    bool const last = k + 1 == split.spread.size();
                    if (split.spread[k] > st.maxSpreadYd + 1e-5f)
                        spreadOk = spreadOk && split.minLengthStopped && (std::fabs(len - minLen) < 1e-3f
                            || (last && len < 2.f * minLen + 1e-3f));
                }
                for (std::size_t k = 1; k < pick.pieces.size(); ++k)
                    order = order && pick.pieces[k].forward > pick.pieces[k - 1].forward;
                if (pick.pieces.size() > DefaultLineSteps(shape).size())
                    ++splitCount;
            }
        }
        CHECK(covered);
        CHECK(cleared);
        CHECK(clearedExact);
        CHECK(spreadOk);
        CHECK(order);
        CHECK(splitCount >= 6);                 // the fields really cut lines

        // Steep ground (0.5 rise per yd across a 6.67 yd lane): every piece is at the minimum length and flagged.
        World steep{ [](float x, float) { return 0.5f * x; }, {} };
        Shape lane = Shape::Line(40.f, 6.667f);
        lane.zTolerance = 30.f;
        Ground::LineSplit split;
        VisualPick const pick = SplitPick(lane, Geo::Pose{ 0.f, 0.f, 0.f, 0.f }, steep, st, &split);
        CHECK(split.minLengthStopped);
        CHECK(CoversExactly(lane, pick));
        CHECK(pick.pieces.size() >= 6 && pick.pieces.size() <= Ground::kMaxLinePieces + 1);
        CHECK_NEAR(PieceExtent(pick.pieces.front()), 6.667f, 1e-3);      // aspect 1: one width
        // A narrow line along the contour of the same slope never splits (0.5 yd across its 1 yd width).
        Shape contour = Shape::Line(40.f, 1.f);
        contour.zTolerance = 30.f;
        VisualPick const along = SplitPick(contour, Geo::Pose{ 0.f, 0.f, 0.f, Geo::kPi * 0.5f }, steep, st, &split);
        CHECK(along.pieces.size() == 1);

        // Discs and cones: one height, the footprint's highest + lift, nothing above it.
        for (Field const& field : fields)
        {
            World world{ field.f, {} };
            for (Shape const& sh : { Shape::Circle(10.f), Shape::Donut(4.f, 12.f), Shape::Cone(90.f, 20.f),
                Shape::Circle(3.f) })
            {
                Geo::Pose const pose{ 10.f, 2.f, field.f(10.f, 2.f), 0.4f };
                Ground::Sampler<World> sampler(world, pose.z, 30.f);
                VisualPick pick = PickVisual(sh, 0, MarkerLayer::Outline);
                Ground::AssignZ(pick, sampler, pose, st, false);
                Clearance const c = Check(pick, pose, field.f, st.lift, st.sampleYd);
                CHECK(c.footprint <= 1e-4f);
                CHECK(c.dense <= 0.15f);
                CHECK(pick.pieces.front().z >= pose.z + st.lift - 1e-4f || sh.type == ShapeType::Donut);
            }
        }
    }

    void TestGroundBand()
    {
        Ground::Settings st;
        Geo::Pose const pose{ 0.f, 0.f, 0.f, 0.f };
        VisualPiece const disc = PickVisual(Shape::Circle(6.f), 0, MarkerLayer::Outline).pieces.front();

        // A ceiling (a bridge deck, a roof) 4 yd up, inside the 6 yd band: never captures the marker.
        World roofed{ Flat, [](float, float) { return 4.f; } };
        Ground::Sampler<World> a(roofed, 0.f, 6.f);
        CHECK_NEAR(Ground::PieceZ(a, pose, disc, st, false), st.lift, 1e-6);
        Shape lane = Shape::Line(40.f, 6.f);
        VisualPick const under = SplitPick(lane, pose, roofed, st);
        CHECK(under.pieces.size() == 1 && std::fabs(under.pieces[0].z - st.lift) < 1e-6f);

        // A hill rising above the first search start (2 yd) but inside the band: found from the band's top.
        World hill{ [](float x, float) { return x > 3.f ? 3.5f : 0.f; }, {} };
        Ground::Sampler<World> b(hill, 0.f, 6.f);
        CHECK_NEAR(Ground::PieceZ(b, pose, disc, st, false), 3.5f + st.lift, 1e-6);

        // Ground out of band (a cliff 10 yd down, a wall 9 yd up): ignored; nothing accepted -> the anchor z.
        World pit{ [](float x, float) { return x > 2.f ? -10.f : 0.f; }, {} };
        Ground::Sampler<World> c(pit, 0.f, 6.f);
        CHECK_NEAR(Ground::PieceZ(c, pose, disc, st, false), st.lift, 1e-6);
        World wall{ [](float, float) { return 9.f; }, {} };
        Ground::Sampler<World> d(wall, 0.f, 6.f);
        Ground::Heights h;
        CHECK_NEAR(Ground::PieceZ(d, pose, disc, st, false, &h), st.lift, 1e-6);
        CHECK(h.accepted == 0);

        // The anchor standing on a bridge deck (z 4) over ground at 0 (out of a 3 yd band): the deck.
        World deck{ Flat, [](float, float) { return 4.f; } };
        Ground::Sampler<World> e(deck, 4.f, 3.f);
        CHECK_NEAR(Ground::PieceZ(e, Geo::Pose{ 0.f, 0.f, 4.f, 0.f }, disc, st, false), 4.f + st.lift, 1e-6);

        // The cheap follow set: a handful of queries, on a slope its highest point.
        World slope{ Slope, {} };
        Ground::Sampler<World> f(slope, 0.f, 30.f);
        float const z = Ground::PieceZ(f, pose, disc, st, true);
        CHECK(f.Queries() <= 14);
        CHECK_NEAR(z, 0.12f * 6.f + st.lift, 1e-4);
    }

    void TestGroundFill()
    {
        Ground::Settings st;
        World world{ StepField, {} };
        Shape lane = Shape::Line(40.f, 4.f);
        lane.zTolerance = 30.f;
        Geo::Pose const pose{ 0.f, 0.f, 0.f, 0.f };
        Ground::LineSplit split;
        VisualPick const outline = SplitPick(lane, pose, world, st, &split);
        CHECK(outline.pieces.size() >= 2);
        bool ok = true;
        for (float f : { 0.1f, 0.35f, 0.6f, 0.9f, 1.f })
        {
            VisualPick fill = PickFill(lane, 0, f, false, &split.steps);
            CHECK(fill.pieces.size() == outline.pieces.size());
            Ground::AssignFillZ(fill, outline);
            for (std::size_t k = 0; k < fill.pieces.size(); ++k)
            {
                // Never below the ground it covers (the highest outline piece it overlaps); at the full fill,
                // exactly its own outline piece's height.
                if (fill.pieces[k].display)
                {
                    Geo::Pose const at = PieceWorldPose(pose, fill.pieces[k]);
                    for (Ground::Point2 const& p : Dense(at, fill.pieces[k], 0.25f))
                        ok = ok && StepField(p.x, p.y) <= fill.pieces[k].z - st.lift + 1e-4f;
                }
                if (f == 1.f)
                    ok = ok && std::fabs(fill.pieces[k].z - outline.pieces[k].z) < 1e-5f;
            }
        }
        CHECK(ok);
        // The ExceptOne draw strip keeps working: the strip is split like any line and covers itself exactly.
        Shape strip = lane;
        Geo::Pose stripPose = pose;
        ApplyDrawStrip(strip, stripPose, 1.f, 2.f);
        VisualPick const sp = SplitPick(strip, stripPose, world, st, &split);
        CHECK(CoversExactly(strip, sp) && sp.pieces.front().scale == 2.f);
    }
}

int main()
{
    TestProjectileWindows();
    TestProjectileSweep();
    TestTrackedSweep();
    TestMine();
    TestLanes();
    TestLinePlacement();
    TestWeakened();
    TestReferenceHealth();
    TestSearchRadius();
    TestVisualPick();
    TestLayers();
    TestFillFraction();
    TestFillPick();
    TestCrossingLayout();
    TestFillPacing();
    TestProjectileShrink();
    TestGroundFlat();
    TestGroundFields();
    TestGroundBand();
    TestGroundFill();
    return Report("math_test");
}
