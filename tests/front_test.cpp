// Standalone tests for the rolling fronts (Blackrock Spire build WP-RF; src/MechanicFront.h, the projectile launch
// offset in src/MechanicMath.h, the solver's radial s, slice exits and ParkTogether in src/MechanicSolver.h, the wide
// cone and arc band displays in src/BossMechanicsIds.h): the spec rules, the gaps and wall segments, the placement
// rules, the marker pieces, single solves (slice, gap, tank park), fights in the brain model (Emberseer's pulses, also
// with the user's rulings and the ring orbit of WP-SF; Rend's sweeps) and the cost of four rings at once against the
// solver budget (M5: 2,000 us).
// Build: tests/run.sh.

#include "check.h"
#include "brain_model.h"
#include "MechanicDrill.h"
#include "MechanicFront.h"
#include "MechanicSolver.h"
#include <algorithm>
#include <array>
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
using MarkerShape = Ids::MarkerShape;

namespace
{
    constexpr float kMargin = 0.75f;            // BossMechanics.Bots.SafetyMarginYd
    constexpr float kDeg = Geo::kPi / 180.f;
    Ms const kNow{ 100000 };

    struct StraightOracle : Solver::PathOracle
    {
        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            out = { from, to };
            return true;
        }
    };

    // Straight paths inside an axis-aligned room (the navmesh's walls): a point outside it is not walkable, and a path
    // toward one ends at the wall (the room's nearest point).
    struct BoxOracle : Solver::PathOracle
    {
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;

        BoxOracle(float ax, float ay, float bx, float by) : x0(ax), y0(ay), x1(bx), y1(by) { }

        Vec3 Clamp(Vec3 p) const
        {
            p.x = std::clamp(p.x, x0, x1);
            p.y = std::clamp(p.y, y0, y1);
            return p;
        }

        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            out = { from, Clamp(to) };
            return true;
        }

        bool Walkable(Vec3& p) override
        {
            return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1;
        }
    };

    Vec3 Polar(float bearing, float r)
    {
        return { std::cos(bearing) * r, std::sin(bearing) * r, 0.f };
    }

    // A cone front as the bot view builds it: the Zone's shape at its apex pose, the projectile timing.
    Hazard ConeHazard(std::uint32_t id, ConeFrontSpec const& spec, Ms launch, Vec3 apex = {})
    {
        Hazard h;
        h.id = id;
        h.shape = Front::ConeShape(spec);
        h.poses = { Pose{ apex.x, apex.y, apex.z, Front::ConeFacing(spec) } };
        h.marginYd = kMargin;
        h.timing = Solver::MakeProjectile(Front::ConeParams(spec), launch);
        h.front = true;
        return h;
    }

    // A line front as the bot view builds it: one hazard per wall segment between the gaps.
    std::vector<Hazard> LineHazards(std::uint32_t id, LineFrontSpec const& spec, Ms launch, Vec3 start)
    {
        std::vector<Hazard> out;
        Pose const pose{ start.x, start.y, start.z, spec.facing };
        for (Front::Segment const& seg : Front::Segments(Front::LineShape(spec), pose, spec.gaps))
        {
            Hazard h;
            h.id = id;
            h.shape = seg.shape;
            h.poses = { seg.pose };
            h.marginYd = kMargin;
            h.timing = Solver::MakeProjectile(Front::LineParams(spec), launch);
            h.front = true;
            out.push_back(h);
        }
        return out;
    }

    Hazard Nova(std::uint32_t id, float r, Ms at, Vec3 apex = {})
    {
        Hazard h;
        h.id = id;
        h.shape = Shape::Circle(r);
        h.poses = { Pose{ apex.x, apex.y, apex.z, 0.f } };
        h.marginYd = kMargin;
        h.timing = Solver::MakeInstant(at, Ms(500));
        return h;
    }

    // Does the exact band of the front (no margin) cover p at time t?
    bool ExactHot(Hazard const& h, Vec3 const& p, Ms t)
    {
        Hazard exact = h;
        exact.marginYd = 0.f;
        for (HotWindow const& w : Solver::WindowsAt(exact, p))
            if (w.from <= t && t <= w.to)
                return true;
        return false;
    }

    // ---- the projectile launch offset ------------------------------------------------------------------------------

    void TestLaunchOffset()
    {
        ProjectileParams p;
        p.length = 45.f;
        p.speed = 8.f;
        p.halfLength = 2.f;
        p.start = 9.f;              // the band [7, 11] at launch
        CHECK(ProjectileDuration(p) == MsOf((45.f + 2.f - 9.f) / 8.f));     // the inner edge reaches 45
        CHECK_NEAR(ProjectileCentre(p, 0.f), 9.f, 1e-5);
        CHECK_NEAR(ProjectileCentre(p, 1.f), 17.f, 1e-5);
        // Behind the launch: never hot (the nova's disc).
        CHECK(ProjectileHotWindows(p, 3.f).empty());
        CHECK(ProjectileHotWindows(p, 6.9f).empty());
        // On the launched band: hot from the launch.
        std::vector<HotWindow> w = ProjectileHotWindows(p, 8.f);
        CHECK(w.size() == 1 && w[0].from == Ms(0) && w[0].to == MsOf((8.f + 2.f - 9.f) / 8.f));
        // Ahead: from when the outer edge arrives to when the inner edge leaves.
        w = ProjectileHotWindows(p, 20.f);
        CHECK(w.size() == 1 && w[0].from == MsOf((20.f - 2.f - 9.f) / 8.f) && w[0].to == MsOf((20.f + 2.f - 9.f)
            / 8.f));
        // The sweep: the bands cover what the body passed, starting at the launch position.
        std::vector<SweepBand> b = SweptBands(p, Ms(0), Ms(500));
        CHECK(b.size() == 1 && std::fabs(b[0].lo - 7.f) < 1e-4f && std::fabs(b[0].hi - 15.f) < 1e-4f);
        // start 0 is the old projectile exactly (out-and-back ignores start).
        ProjectileParams q = p;
        q.start = 0.f;
        w = ProjectileHotWindows(q, 20.f);
        CHECK(w.size() == 1 && w[0].from == MsOf(18.f / 8.f) && w[0].to == MsOf(22.f / 8.f));
        q.outAndBack = true;
        q.start = 9.f;
        CHECK(ProjectileStart(q) == 0.f);
        // The projectile coordinate: radial for a cone, along the axis for a line.
        Shape const cone = Shape::Cone(300.f, 45.f);
        Shape const line = Shape::Line(40.f, 10.f);
        Pose const pose{ 1.f, 2.f, 0.f, 0.5f };
        Vec3 const pt{ 4.f, -2.f, 0.f };
        CHECK_NEAR(ProjectileS(cone, pose, pt), 5.f, 1e-5);
        CHECK_NEAR(ProjectileS(line, pose, pt), Geo::ToLineCoords(line, pose, pt).s, 1e-5);
        CHECK_NEAR(ProjectileS(cone, 3.f, -4.f, 1.f, 0.f), 5.f, 1e-5);
        CHECK_NEAR(ProjectileS(line, 3.f, -4.f, 0.f, 1.f), -4.f, 1e-5);
    }

    // ---- the spec rules ---------------------------------------------------------------------------------------------

    void TestConeSpec()
    {
        ConeFrontSpec s;
        s.sliceBearing = 90.f * kDeg;
        s.sliceDeg = 60.f;
        s.startYd = 7.f;
        s.endYd = 45.f;
        CHECK(Front::Valid(s));
        Shape const shape = Front::ConeShape(s);
        CHECK(shape.type == ShapeType::Cone && shape.angleDeg == 300.f && shape.length == 45.f);
        CHECK_NEAR(Front::ConeFacing(s), -90.f * kDeg, 1e-5);
        ProjectileParams const p = Front::ConeParams(s);
        CHECK(p.start == 9.f && p.halfLength == 2.f && p.length == 45.f && p.speed == 8.f);
        CHECK(Front::Duration(p) == Ms(4750));          // (45 - 7) / 8: about 4 s to the walls (SP EM3)
        Pose const apex{ 0.f, 0.f, 0.f, Front::ConeFacing(s) };
        // The slice is outside the hitbox, the rest inside.
        CHECK(!Front::Contains(shape, apex, {}, Polar(90.f * kDeg, 20.f)));
        CHECK(!Front::Contains(shape, apex, {}, Polar(115.f * kDeg, 20.f)));
        CHECK(Front::Contains(shape, apex, {}, Polar(125.f * kDeg, 20.f)));
        CHECK(Front::Contains(shape, apex, {}, Polar(-90.f * kDeg, 20.f)));
        CHECK(!Front::Contains(shape, apex, {}, Polar(0.f, 46.f)));
        // The band at 1 s: [15, 19]; a point there is hit, one in the slice is not, one behind it is not.
        CHECK(Front::HitsAt(shape, apex, {}, p, 1.f, Polar(0.f, 17.f)));
        CHECK(!Front::HitsAt(shape, apex, {}, p, 1.f, Polar(90.f * kDeg, 17.f)));
        CHECK(!Front::HitsAt(shape, apex, {}, p, 1.f, Polar(0.f, 12.f)));
        CHECK(!Front::HitsAt(shape, apex, {}, p, -0.1f, Polar(0.f, 8.f)));
        // Heroic 315 (a 45 degree slice); a full ring; invalid specs.
        s.sliceDeg = 45.f;
        CHECK(Front::ConeShape(s).angleDeg == 315.f);
        s.sliceDeg = 0.f;
        CHECK(Front::Valid(s) && Front::ConeShape(s).angleDeg == 360.f);
        CHECK(Front::Contains(Front::ConeShape(s), apex, {}, Polar(90.f * kDeg, 20.f)));
        ConeFrontSpec bad = s;
        bad.endYd = 5.f;
        CHECK(!Front::Valid(bad));
        bad = s;
        bad.bandYd = 0.f;
        CHECK(!Front::Valid(bad));
        bad = s;
        bad.sliceDeg = 340.f;
        CHECK(!Front::Valid(bad));
        // The ring sequence: each slice turned clockwise (EM3: N 45, H 60).
        CHECK_NEAR(Front::SliceBearing(0.f, 0, 45.f), 0.f, 1e-6);
        CHECK_NEAR(Front::SliceBearing(0.f, 1, 45.f), -45.f * kDeg, 1e-5);
        CHECK_NEAR(Front::SliceBearing(0.f, 3, 60.f), Geo::kPi, 1e-5);
    }

    void TestLineSpec()
    {
        LineFrontSpec s;
        s.facing = Geo::kPi;        // north to south along -x (Rend's arena: portcullis x 194 to the way-in door x 108)
        s.travelYd = 86.f;
        s.widthYd = 40.f;
        s.gaps = { { 10.f, 8.f } };
        CHECK(Front::Valid(s));
        ProjectileParams const p = Front::LineParams(s);
        CHECK(p.start == 2.f && p.halfLength == 2.f && p.length == 86.f);
        CHECK(Front::Duration(p) == MsOf(86.f / 8.f));    // about 10 s end to end (SP RG6)
        Shape const shape = Front::LineShape(s);
        Pose const start{ 190.f, -420.f, 111.f, s.facing };
        // Gaps: t is left of the facing; facing -x, left is -y.
        auto at = [&](float along, float t)
        {
            float const f = s.facing;
            return Vec3{ start.x + std::cos(f) * along - std::sin(f) * t, start.y + std::sin(f) * along
                + std::cos(f) * t, start.z };
        };
        CHECK(Front::Contains(shape, start, s.gaps, at(20.f, 0.f)));
        CHECK(!Front::Contains(shape, start, s.gaps, at(20.f, 10.f)));
        CHECK(!Front::Contains(shape, start, s.gaps, at(20.f, 13.9f)));
        CHECK(Front::Contains(shape, start, s.gaps, at(20.f, 14.1f)));
        CHECK(!Front::Contains(shape, start, s.gaps, at(20.f, 21.f)));    // past the wall's end
        CHECK(Front::HitsAt(shape, start, s.gaps, p, 2.f, at(17.f, -5.f)));
        CHECK(!Front::HitsAt(shape, start, s.gaps, p, 2.f, at(17.f, 9.f)));
        CHECK(!Front::HitsAt(shape, start, s.gaps, p, 2.f, at(25.f, -5.f)));
        // Segments: the wall minus the gap, as two lines on their own centrelines.
        std::vector<Front::Segment> const seg = Front::Segments(shape, start, s.gaps);
        CHECK(seg.size() == 2);
        CHECK_NEAR(seg[0].lo, -20.f, 1e-5);
        CHECK_NEAR(seg[0].hi, 6.f, 1e-5);
        CHECK_NEAR(seg[1].lo, 14.f, 1e-5);
        CHECK_NEAR(seg[1].hi, 20.f, 1e-5);
        CHECK_NEAR(seg[0].shape.width, 26.f, 1e-5);
        CHECK_NEAR(seg[1].shape.width, 6.f, 1e-5);
        // The segments together hold exactly the gapped wall: sample the room.
        bool same = true;
        for (float along = -2.f; along <= 90.f; along += 1.3f)
            for (float t = -23.f; t <= 23.f; t += 0.37f)
            {
                Vec3 const q = at(along, t);
                bool inSeg = false;
                for (Front::Segment const& g : seg)
                    inSeg = inSeg || Geo::Contains(g.shape, g.pose, q);
                if (inSeg != Front::Contains(shape, start, s.gaps, q))
                    same = false;
                // ...with the same s (the segments share the wall's timing).
                for (Front::Segment const& g : seg)
                    if (Geo::Contains(g.shape, g.pose, q)
                        && std::fabs(Geo::ToLineCoords(g.shape, g.pose, q).s - Geo::ToLineCoords(shape, start, q).s)
                        > 1e-3f)
                        same = false;
            }
        CHECK(same);
        // Gaps at the wall's ends, overlapping gaps, slivers, and no gap.
        CHECK(Front::Segments(shape, start, { { -18.f, 8.f } }).size() == 1);
        CHECK(Front::Segments(shape, start, { { -5.f, 8.f }, { 0.f, 8.f } }).size() == 2);
        CHECK(Front::Segments(shape, start, { { -4.1f, 8.f }, { 4.f, 8.f } }).size() == 2);   // 0.1 yd sliver dropped
        CHECK(Front::Segments(shape, start, {}).size() == 1);
        LineFrontSpec bad = s;
        bad.gaps = { { 0.f, 0.f } };
        CHECK(!Front::Valid(bad));
        bad = s;
        bad.travelYd = 0.f;
        CHECK(!Front::Valid(bad));
    }

    void TestPlaceGap()
    {
        // 40 yd wall, 8 yd gap: centres in [-16, 16].
        CHECK_NEAR(*Front::PlaceGap(40.f, 8.f, {}, 0.f), -16.f, 1e-4);
        CHECK_NEAR(*Front::PlaceGap(40.f, 8.f, {}, 0.5f), 0.f, 1e-3);
        CHECK(*Front::PlaceGap(40.f, 8.f, {}, 0.f, 1) >= 0.f);
        CHECK(*Front::PlaceGap(40.f, 8.f, {}, 0.99f, -1) <= 0.f);
        CHECK_NEAR(*Front::PlaceGap(40.f, 8.f, {}, 0.f, 0, 2.f), -14.f, 1e-4);
        // Off live pools: every u lands clear of the blocked intervals.
        std::vector<std::pair<float, float>> const pools{ { -12.f, -8.f }, { 3.f, 9.f } };
        bool clear = true;
        for (float u = 0.f; u < 1.f; u += 0.01f)
        {
            std::optional<float> c = Front::PlaceGap(40.f, 8.f, pools, u);
            clear = clear && c && Front::GapClear(*c, 8.f, pools) && *c >= -16.f - 1e-3f && *c <= 16.f + 1e-3f;
        }
        CHECK(clear);
        // The heroic second wall: the other side of the first gap's.
        float const first = *Front::PlaceGap(40.f, 8.f, pools, 0.9f);
        int const other = -Front::SideOf(first);
        std::optional<float> second = Front::PlaceGap(40.f, 8.f, pools, 0.3f, other);
        CHECK(second && Front::SideOf(*second) == other && Front::GapClear(*second, 8.f, pools));
        // Nothing fits.
        CHECK(!Front::PlaceGap(40.f, 8.f, { { -20.f, 20.f } }, 0.5f));
        CHECK(!Front::PlaceGap(6.f, 8.f, {}, 0.5f));
        // A pool's lateral extent on the wall's axis (only when it reaches the wall's path).
        Shape const line = Shape::Line(86.f, 40.f);
        Pose const pose{ 0.f, 0.f, 0.f, 0.f };
        std::optional<std::pair<float, float>> b = Front::BlockedBy(line, pose, { 30.f, 5.f, 0.f }, 6.f);
        CHECK(b && std::fabs(b->first + 1.f) < 1e-4f && std::fabs(b->second - 11.f) < 1e-4f);
        CHECK(!Front::BlockedBy(line, pose, { -10.f, 0.f, 0.f }, 6.f));
    }

    // ---- displays ---------------------------------------------------------------------------------------------------

    void TestDisplays()
    {
        using Ids::MarkerDisplay;
        // The wide shapes sit after the dashed ring in every color block, before the diagnostics.
        CHECK(MarkerDisplay(0, MarkerShape::WideCone, 0) == 91034);
        CHECK(MarkerDisplay(0, MarkerShape::WideConeOutline, 2) == 91039);
        CHECK(MarkerDisplay(0, MarkerShape::ArcBand, 0) == 91040);
        CHECK(MarkerDisplay(8, MarkerShape::ArcBand, Ids::kArcBandSteps - 1) == 91893);
        CHECK(MarkerDisplay(2, MarkerShape::ArcBand, Ids::ArcBandStep(2, 4)) == 91200 + 40 + 2 * 9 + 4);
        CHECK(MarkerDisplay(0, MarkerShape::ArcBand, 999) == 91093);         // clamped
        bool roundTrip = true;
        for (std::uint8_t c = 0; c < Ids::COLOR_COUNT; ++c)
        {
            std::uint32_t expected = Ids::DISPLAY_FIRST + Ids::DISPLAY_COLOR_STRIDE * c + Ids::kWideConeOffset;
            for (MarkerShape shape : { MarkerShape::WideCone, MarkerShape::WideConeOutline, MarkerShape::ArcBand })
                for (std::uint32_t k = 0; k < Ids::WideShapeSteps(shape); ++k)
                {
                    std::uint32_t const id = MarkerDisplay(c, shape, k);
                    Ids::MarkerDisplayParts const parts = Ids::DecodeMarkerDisplay(id);
                    if (id != expected++ || !parts.valid || parts.color != c || parts.shape != shape || parts.step != k)
                        roundTrip = false;
                }
            if (expected != Ids::DISPLAY_FIRST + Ids::DISPLAY_COLOR_STRIDE * c + 94)
                roundTrip = false;
        }
        CHECK(roundTrip);
        CHECK(!Ids::DecodeMarkerDisplay(91094).valid && !Ids::DecodeMarkerDisplay(91194).valid);
        // The arc band: angle up, ratio down (the drawn band is never narrower or thinner than the hitbox's band).
        Ids::MarkerDisplayParts parts = Ids::DecodeMarkerDisplay(Front::ArcBandDisplay(0, 300.f, 16.f, 20.f));
        CHECK(parts.valid && parts.shape == MarkerShape::ArcBand && parts.step == Ids::ArcBandStep(2, 4));
        parts = Ids::DecodeMarkerDisplay(Front::ArcBandDisplay(1, 310.f, 41.f, 45.f));     // 0.911 -> 0.9, 310 -> 315
        CHECK(parts.color == 1 && parts.step == Ids::ArcBandStep(3, 7));
        for (float outer = 8.5f; outer <= 49.f; outer += 0.25f)
            for (float angle : { 250.f, 300.f, 315.f, 360.f })
            {
                float const inner = outer - 4.f;
                parts = Ids::DecodeMarkerDisplay(Front::ArcBandDisplay(0, angle, inner, outer));
                CHECK(parts.valid);
                if (parts.shape == MarkerShape::ArcBand)
                {
                    float const ratio = Ids::kArcBandRatios[parts.step % Ids::kArcBandRatios.size()];
                    float const drawnAngle = Ids::kArcBandAngles[parts.step / Ids::kArcBandRatios.size()];
                    CHECK(ratio * outer <= inner + 1e-4f && drawnAngle >= angle - 1e-4f);
                    CHECK(outer * (1.f - ratio) <= 4.f + 0.1f * outer);      // never much thicker either
                }
            }
        // Thicker than half the radius: the filled cone of the outer radius.
        parts = Ids::DecodeMarkerDisplay(Front::ArcBandDisplay(0, 300.f, 1.f, 5.f));
        CHECK(parts.shape == MarkerShape::WideCone);
        // The step: 0.8 yd at 8 yd/s, in [50, follow interval].
        CHECK(Front::MarkerStepMs(8.f, 200) == 100);
        CHECK(Front::MarkerStepMs(100.f, 200) == 50);
        CHECK(Front::MarkerStepMs(1.f, 200) == 200);
    }

    void TestConePieces()
    {
        ConeFrontSpec s;
        s.sliceBearing = 0.3f;
        s.sliceDeg = 60.f;
        s.startYd = 7.f;
        FrontInfo const f = Front::InfoOf(s);
        Shape const shape = Front::ConeShape(s);
        ProjectileParams const p = Front::ConeParams(s);
        Pose const apex{ 150.f, -260.f, 92.f, 0.f };
        Front::Colors const colors{ 0, 2 };
        std::vector<Front::Piece> before = Front::ConePieces(f, shape, p, apex, -1.f, colors);
        CHECK(before.size() == 3);
        CHECK(before[0].display == Ids::MarkerDisplay(0, MarkerShape::WideConeOutline, 0) && before[0].scale == 45.f);
        CHECK_NEAR(before[0].pose.o, Front::ConeFacing(s), 1e-5);
        CHECK(before[1].display == Ids::MarkerDisplay(2, MarkerShape::Cone, 2) && before[1].scale == 45.f);  // 60
        CHECK_NEAR(before[1].pose.o, 0.3f, 1e-5);
        CHECK(before[2].display == 0 && before[2].moving);
        // At the launch and on the way: the band from inner to outer; never drawn smaller than the hitbox's band.
        for (float e = 0.f; e <= 4.75f; e += 0.1f)
        {
            std::vector<Front::Piece> const now = Front::ConePieces(f, shape, p, apex, e, colors);
            CHECK(now.size() == 3 && now[0].display == before[0].display && now[1].display == before[1].display);
            Front::Band const b = Front::BandAt(p, e);
            Ids::MarkerDisplayParts const parts = Ids::DecodeMarkerDisplay(now[2].display);
            CHECK(parts.valid && (parts.shape == MarkerShape::ArcBand || parts.shape == MarkerShape::WideCone));
            CHECK_NEAR(now[2].scale, std::min(b.outer, 45.f), 1e-4);
            CHECK(now[2].pose.x == apex.x && now[2].pose.y == apex.y);
        }
        CHECK(Front::ConePieces(f, shape, p, apex, 4.8f, colors)[2].display == 0);     // over
        // A narrow slice below the ladder draws none; the full ring's outline is a disc.
        ConeFrontSpec ring = s;
        ring.sliceDeg = 20.f;
        CHECK(Front::ConePieces(Front::InfoOf(ring), Front::ConeShape(ring), p, apex, 0.f, colors)[1].display == 0);
        ring.sliceDeg = 0.f;
        std::vector<Front::Piece> const full = Front::ConePieces(Front::InfoOf(ring), Front::ConeShape(ring), p, apex,
            1.f, colors);
        CHECK(full[0].display == Ids::MarkerDisplay(0, MarkerShape::Ring) && full[1].display == 0);
        CHECK(Ids::DecodeMarkerDisplay(full[2].display).step / Ids::kArcBandRatios.size() == 5);  // the 360 band
        // Heroic: 45 degree slice (exact on the ladder).
        ring.sliceDeg = 45.f;
        CHECK(Front::ConePieces(Front::InfoOf(ring), Front::ConeShape(ring), p, apex, 0.f, colors)[1].display
            == Ids::MarkerDisplay(2, MarkerShape::Cone, 1));
    }

    void TestLinePieces()
    {
        LineFrontSpec s;
        s.facing = Geo::kPi;
        s.travelYd = 86.f;
        s.widthYd = 40.f;
        s.gaps = { { 10.f, 8.f } };
        FrontInfo const f = Front::InfoOf(s);
        Shape const shape = Front::LineShape(s);
        ProjectileParams const p = Front::LineParams(s);
        Pose const start{ 190.f, -420.f, 111.f, s.facing };
        Front::Colors const colors{ 0, 2 };
        std::size_t const slots = 2 * Front::kMaxLanePieces + 1 + 2;
        std::vector<Front::Piece> const before = Front::LinePieces(f, shape, p, start, -0.5f, colors);
        CHECK(before.size() == slots);
        // Lanes (outline, the danger color) on each segment's centreline, gap lane (get-in), no wall yet.
        CHECK(Ids::DecodeMarkerDisplay(before[0].display).shape == MarkerShape::LineOutline);
        CHECK(Ids::DecodeMarkerDisplay(before[Front::kMaxLanePieces].display).shape == MarkerShape::LineOutline);
        Ids::MarkerDisplayParts const gap = Ids::DecodeMarkerDisplay(before[2 * Front::kMaxLanePieces].display);
        CHECK(gap.valid && gap.shape == MarkerShape::Line && gap.color == 2);
        CHECK_NEAR(before[2 * Front::kMaxLanePieces].scale, 8.f, 1e-5);
        CHECK(before[slots - 1].display == 0 && before[slots - 2].display == 0);
        // The gap lane's centreline: 10 yd left of the facing (-x facing: left is -y).
        CHECK_NEAR(before[2 * Front::kMaxLanePieces].pose.y, -430.f, 1e-3);
        // On the way: each wall piece spans its segment across the facing at the band's middle.
        for (float e = 0.f; e <= 10.7f; e += 0.5f)
        {
            std::vector<Front::Piece> const now = Front::LinePieces(f, shape, p, start, e, colors);
            CHECK(now.size() == slots);
            Front::Band const b = Front::BandAt(p, e);
            for (std::size_t k = slots - 2; k < slots; ++k)
            {
                Ids::MarkerDisplayParts const parts = Ids::DecodeMarkerDisplay(now[k].display);
                CHECK(parts.valid && parts.shape == MarkerShape::Line && now[k].moving);
                CHECK_NEAR(now[k].scale, b.outer - b.inner, 1e-4);                   // the band's thickness
                CHECK_NEAR(now[k].pose.x, 190.f - (b.inner + b.outer) * 0.5f, 1e-3); // along -x
                CHECK_NEAR(Geo::WrapPi(now[k].pose.o - (s.facing + Geo::kPi * 0.5f)), 0.f, 1e-4);
            }
            // The lanes shrink behind the wall; the gap lane stays.
            CHECK(now[2 * Front::kMaxLanePieces].display == before[2 * Front::kMaxLanePieces].display);
        }
        CHECK(Front::LinePieces(f, shape, p, start, 11.f, colors)[slots - 1].display == 0);
    }

    // ---- single solves ----------------------------------------------------------------------------------------------

    Input BaseInput(Vec3 bot)
    {
        Input in;
        in.bot = bot;
        in.now = kNow;
        in.speed = 7.f;
        return in;
    }

    void TestSliceSeeking()
    {
        // Emberseer EM2 + EM3 at 7 stacks: nova 9.1 yd erupting in 2.5 s, the ring launching with it, 60 degree slice
        // at bearing 90. Bots all round the apex at 12, 18 and 25 yd find a spot clean through the ring, in time.
        ConeFrontSpec s;
        s.sliceBearing = 90.f * kDeg;
        s.sliceDeg = 60.f;
        s.startYd = 9.1f;
        Ms const launch = kNow + Ms(2500);
        StraightOracle oracle;
        std::uint32_t moved = 0;
        std::uint32_t total = 0;
        for (float r : { 12.f, 18.f, 25.f })
            for (float b = 0.f; b < 360.f; b += 30.f)
            {
                Input in = BaseInput(Polar(b * kDeg, r));
                in.hazards = { Nova(1, s.startYd, launch), ConeHazard(2, s, launch) };
                Plan const plan = Solver::Solve(in, oracle);
                ++total;
                bool const ok = plan.outcome == Outcome::Safe || plan.outcome == Outcome::Move;
                CHECK(ok);
                if (!ok)
                    std::printf("  slice seeking: r %.0f bearing %.0f -> outcome %d\n", r, b,
                        static_cast<int>(plan.outcome));
                if (plan.outcome == Outcome::Move)
                {
                    ++moved;
                    // The target is out of the band's way over the ring's whole life, and the walk never meets it.
                    CHECK(Solver::PointSafe(in, plan.target, plan.arriveAt, launch + Ms(6000)));
                    CHECK(Solver::CheckPath(in, plan.path, kNow, false).ok);
                }
            }
        CHECK(moved >= total - 6);      // only the bots already in the slice stay
        // A bot already in the slice stays.
        Input in = BaseInput(Polar(90.f * kDeg, 15.f));
        in.hazards = { Nova(1, s.startYd, launch), ConeHazard(2, s, launch) };
        CHECK(Solver::Solve(in, oracle).outcome == Outcome::Safe);
        // The slice candidates: a bot across from the slice gets exits inside it at its own distance.
        Input across = BaseInput(Polar(-90.f * kDeg, 15.f));
        across.hazards = { ConeHazard(2, s, launch) };
        std::vector<Vec3> const cands = Solver::Candidates(across);
        bool inSlice = false;
        for (Vec3 const& c : cands)
        {
            float const d = std::hypot(c.x, c.y);
            float const off = std::fabs(Geo::WrapPi(std::atan2(c.y, c.x) - 90.f * kDeg));
            inSlice = inSlice || (std::fabs(d - 15.f) < 0.1f && off < 30.f * kDeg);
        }
        CHECK(inSlice);
        // The solver's windows (no margin) and the judge's band agree away from the edges (radial s, the launch
        // offset, the slice).
        Hazard const ring = ConeHazard(2, s, launch);
        Pose const apex{ 0.f, 0.f, 0.f, Front::ConeFacing(s) };
        std::uint32_t disagree = 0;
        for (float b = 0.f; b < 360.f; b += 7.f)
            for (float r = 1.f; r < 47.f; r += 0.9f)
                for (float e = 0.05f; e < 5.f; e += 0.25f)
                {
                    Vec3 const q = Polar(b * kDeg, r);
                    Front::Band const band = Front::BandAt(Front::ConeParams(s), e);
                    float const off = std::fabs(Geo::WrapPi(b * kDeg - 90.f * kDeg));
                    if (std::fabs(r - band.inner) < 0.1f || std::fabs(r - band.outer) < 0.1f
                        || std::fabs(off - 30.f * kDeg) < 0.01f || std::fabs(r - 45.f) < 0.1f)
                        continue;
                    Ms const t = launch + Ms(static_cast<std::int64_t>(e * 1000.f));
                    if (ExactHot(ring, q, t) != Front::HitsAt(Front::ConeShape(s), apex, {}, Front::ConeParams(s),
                        e, q))
                        ++disagree;
                }
        CHECK(disagree == 0);
    }

    // Rend's arena (handoff §3.8: x 108.8 to 193.8, 40 yd wide on the main strip, y -440 to -400) as hard spot bounds:
    // the walls the navmesh would give. The wall starts at the portcullis (x 194) and runs 86 yd to the way-in door.
    Solver::Bounds RendStrip()
    {
        Solver::Bounds b;
        Solver::BoundsArea a;
        a.polygon = { { 108.f, -440.f, 0.f }, { 194.f, -440.f, 0.f }, { 194.f, -400.f, 0.f }, { 108.f, -400.f, 0.f } };
        b.areas.push_back(a);
        return b;
    }

    void TestGapSeeking()
    {
        // Rend RG6: a 40 yd wall from x 194 toward x 108 at 8 yd/s, an 8 yd gap 10 yd left of the facing, 3 s warning.
        LineFrontSpec s;
        s.facing = Geo::kPi;
        s.travelYd = 86.f;
        s.widthYd = 40.f;
        s.gaps = { { 10.f, 8.f } };
        Vec3 const start{ 194.f, -420.f, 0.f };
        Ms const launch = kNow + Ms(3000);
        StraightOracle oracle;
        for (float x : { 180.f, 160.f, 130.f })
            for (float y : { -438.f, -425.f, -420.f, -410.f, -402.f })
            {
                Input in = BaseInput({ x, y, 0.f });
                in.hazards = LineHazards(1, s, launch, start);
                in.bounds = { RendStrip() };
                Plan const plan = Solver::Solve(in, oracle);
                bool const ok = plan.outcome == Outcome::Safe || plan.outcome == Outcome::Move;
                CHECK(ok);
                if (plan.outcome == Outcome::Move)
                {
                    // In the gap (y in [-434, -426]) and clear of the wall from its arrival over the horizon (a far bot
                    // whose wall comes past the horizon waits: Safe).
                    bool const inGap = plan.target.y <= -426.f + 1e-3f && plan.target.y >= -434.f - 1e-3f;
                    CHECK(inGap);
                    CHECK(Solver::PointSafe(in, plan.target, plan.arriveAt, kNow + in.params.horizon));
                    if (!inGap)
                        std::printf("  gap seeking: (%.0f, %.0f) -> (%.1f, %.1f)\n", x, y, plan.target.x,
                            plan.target.y);
                }
                else if (ok)
                    CHECK((y <= -426.f && y >= -434.f) || x < 194.f - 8.f * 10.f + 30.f);
                if (!ok)
                    std::printf("  gap seeking: (%.0f, %.0f) -> outcome %d\n", x, y, static_cast<int>(plan.outcome));
            }
    }

    void TestParkTogether()
    {
        // The tank holds the boss (a 5 yd chase stop) at the apex; the slice at bearing 0, 60 degrees, out to 45 yd.
        Solver::Park pk;
        pk.present = true;
        pk.inside = true;
        pk.together = true;
        pk.boss = { 0.f, 0.f, 0.f };
        pk.stopYd = 3.f;
        pk.triggerYd = 5.f;
        pk.depthYd = 1.f;
        pk.zone.id = 9;
        pk.zone.shape = Shape::Cone(60.f, 45.f);
        pk.zone.poses = { Pose{ 0.f, 0.f, 0.f, 0.f } };
        pk.zone.marginYd = kMargin;
        pk.zone.from = kNow + Ms(2500);
        pk.zone.to = kNow + Ms(7000);
        ConeFrontSpec s;
        s.sliceBearing = 0.f;
        s.sliceDeg = 60.f;
        s.startYd = 9.1f;
        Ms const launch = kNow + Ms(2500);
        StraightOracle oracle;
        for (float b : { -150.f, -90.f, 120.f, 180.f })
        {
            Input in = BaseInput(Polar(b * kDeg, 4.f));    // the tank in melee of the boss, off the slice
            in.hazards = { Nova(1, s.startYd, launch), ConeHazard(2, s, launch) };
            in.park = pk;
            Plan const plan = Solver::Solve(in, oracle);
            CHECK(plan.outcome == Outcome::Move);
            // The tank in the slice, out of the nova; the boss's stop (stopYd short of the tank) in the slice too.
            float const d = std::hypot(plan.target.x, plan.target.y);
            float const k = pk.stopYd / std::max(d, 1e-3f);
            Vec3 const stop{ plan.target.x - plan.target.x * k, plan.target.y - plan.target.y * k, 0.f };
            CHECK(Solver::Detail::ParkStopOk(in.park, stop));
            CHECK(std::fabs(std::atan2(plan.target.y, plan.target.x)) < 30.f * kDeg && d > s.startYd);
            CHECK(Solver::PointSafe(in, plan.target, plan.arriveAt, launch + Ms(6000)));
        }
        // Without `together` (a Park on a danger zone) the tank must stand outside the zone: no plan puts it inside.
        Input in = BaseInput(Polar(180.f * kDeg, 4.f));
        in.hazards = { Nova(1, s.startYd, launch), ConeHazard(2, s, launch) };
        in.park = pk;
        in.park.together = false;
        Plan const apart = Solver::Solve(in, oracle);
        CHECK(apart.outcome != Outcome::Move
            || Solver::SignedDistance(pk.zone.shape, pk.zone.poses.front(), apart.target) > 0.f);
    }

    // ---- fights in the brain model ----------------------------------------------------------------------------------

    struct PulseSpec
    {
        char const* name;
        std::uint32_t rings;
        float sliceDeg;
        float turnDeg;
        Ms ringGap;
        float stacks;
        std::uint32_t bots;
        float rangedYd;         // where the ranged half stands from the boss (the melee half at 3 yd)
        // Blackrock Spire WP-SF, the user's rolling front rulings (HANDOFF section 10, 2026-11-15): ring 1 shown 5 s
        // ahead (the nova keeps its 2.5 s), rings 2-4 from the apex. `hint`: every ring but the last carries the
        // turn to the next (ConeFrontSpec::nextTurnDeg).
        bool ruled = false;
        bool hint = false;
    };

    // One Furnace Pulse per 15 s (12 s heroic) for 60 s: the nova's 2.5 s warning, then `rings` rings `ringGap` apart,
    // each slice turned `turnDeg` clockwise, ring k registered as ring k - 1 launches (SP EM3: "its slice appears as
    // the ring before it launches"). The group stands round the boss at the apex (melee 3 yd, ranged 15-25 yd) and
    // drifts back there between pulses (stock follow). Hits: the exact band (no margin) over a bot. Ruled (WP-SF): the
    // first ring shown 5 s ahead and the nova 2.5 s before it erupts with that ring, the later rings from the apex.
    struct PulseResult
    {
        BrainModel::Counters c;
        std::uint32_t novaHits = 0;
        std::uint32_t ringHits = 0;
        std::array<std::uint32_t, 4> byRing{};
        std::uint32_t pulses = 0;
    };

    PulseResult RunPulses(PulseSpec const& spec, std::uint32_t seed)
    {
        BrainModel::Config cfg;
        cfg.params.marginMs = Ms(300);
        BrainModel::World world;
        world.cfg = cfg;
        std::vector<Vec3> slots;
        for (std::uint32_t k = 0; k < spec.bots; ++k)
        {
            float const a = Geo::kTwoPi * static_cast<float>(k) / static_cast<float>(spec.bots) + 0.37f * seed;
            float const r = k < spec.bots / 2 ? 3.f : spec.rangedYd + 2.f * static_cast<float>(k % 3);
            slots.push_back(Polar(a, r));
            BrainModel::Bot b;
            b.pos = slots.back();
            world.bots.push_back(b);
        }
        world.follow = [&slots](std::size_t i, Vec3& to)
        {
            to = slots[i];
            return true;
        };
        // Emberseer's hall (handoff §3.8: 59.4 x 59 yd round the dais, the apex at its middle) as hard spot bounds.
        Solver::Bounds hall;
        Solver::BoundsArea area;
        area.polygon = { { -29.7f, -29.5f, 0.f }, { 29.7f, -29.5f, 0.f }, { 29.7f, 29.5f, 0.f },
            { -29.7f, 29.5f, 0.f } };
        hall.areas.push_back(area);
        world.kinds = [&hall](std::size_t, Input& in, Ms, std::vector<BrainModel::Task>&)
        {
            in.bounds = { hall };
        };
        BoxOracle walls(-29.7f, -29.5f, 29.7f, 29.5f);
        world.oracle = &walls;
        float const nova = 7.f + 0.3f * spec.stacks;
        Ms const pulseEvery = spec.sliceDeg < 50.f ? Ms(12000) : Ms(15000);
        std::uint32_t id = 1;
        Ms const start{ 100000 };
        Ms const end = start + Ms(60000);
        struct Pending
        {
            Ms at;
            Hazard h;
        };
        std::vector<Pending> pending;
        std::vector<std::uint32_t> novas;
        std::vector<std::uint32_t> ringOf(1, 0);
        std::uint32_t pulsesRun = 0;
        std::uint32_t rng = 0x9e3779b9u * (seed + 1);
        Ms const ahead = spec.ruled ? Ms(5000) : Ms(2500);
        for (Ms pulse = start; pulse + ahead + Ms(5500) < end; pulse += pulseEvery)
        {
            rng = rng * 1664525u + 1013904223u;
            float const first = static_cast<float>(rng >> 8) / 16777216.f * Geo::kTwoPi;
            Ms const launch = pulse + ahead;
            ++pulsesRun;
            novas.push_back(id);
            pending.push_back({ launch - Ms(2500), Nova(id++, nova, launch) });
            for (std::uint32_t k = 0; k < spec.rings; ++k)
            {
                ConeFrontSpec ring;
                ring.sliceBearing = Front::SliceBearing(first, k, spec.turnDeg);
                ring.sliceDeg = spec.sliceDeg;
                ring.startYd = spec.ruled && k > 0 ? 0.f : nova;
                ring.nextTurnDeg = spec.hint && k + 1 < spec.rings ? spec.turnDeg : 0.f;
                Ms const ringLaunch = launch + spec.ringGap * k;
                Ms const shown = k == 0 ? pulse : ringLaunch - spec.ringGap;
                Hazard h = ConeHazard(id, ring, ringLaunch);
                h.nextTurn = -Geo::DegToRad(ring.nextTurnDeg);     // as the bot view sets it
                pending.push_back({ shown, h });
                ringOf.resize(id + 1, 0);
                ringOf[id++] = k;
            }
        }
        for (Ms now = start; now < end; now += cfg.step)
        {
            for (Pending& p : pending)
                if (p.h.id != 0 && p.at <= now)
                {
                    world.AddHazard(p.h);
                    p.h.id = 0;
                }
            world.Step(now);
        }
        PulseResult out;
        out.c = world.Total();
        out.pulses = pulsesRun;
        for (BrainModel::Bot const& b : world.bots)
            for (std::uint32_t hid : b.hitBy)
                if (std::find(novas.begin(), novas.end(), hid) != novas.end())
                    ++out.novaHits;
                else
                {
                    ++out.ringHits;
                    ++out.byRing[std::min<std::size_t>(ringOf[hid], out.byRing.size() - 1)];
                }
        return out;
    }

    void TestPulses()
    {
        // Normal: 1 / 2 / 3 rings at 0 / 7 / 14 stacks, 2 s apart, 60 degree slices turned 45. Heroic: 2 / 3 / 4 rings,
        // 1.5 s apart, 45 degree slices turned 60.
        // The group stands close (the ranged half within 5-9 yd of the boss: SP's "the rings teach the group to stand
        // close"), or the ranged keep their usual 15-19 yd. Gated with the group close: no bot is hit by a ring (at
        // most 1% of the ring passes). Reported only: the nova and the first ring together (SP EM2/EM3: a random
        // first slice shown 2.5 s before the nova erupts) are out of reach for whoever stands across the boss from
        // the slice beyond a few yards, or inside a nova of 10 yd and more (13 or more stacks): the bot takes the
        // nova or the ring, as a player would (the report's designer question).
        PulseSpec const specs[] = {
            { "normal 1 ring, 5 close", 1, 60.f, 45.f, Ms(2000), 3.f, 5, 5.f },
            { "normal 2 rings, 5 close", 2, 60.f, 45.f, Ms(2000), 10.f, 5, 5.f },
            { "normal 3 rings, 5 close", 3, 60.f, 45.f, Ms(2000), 16.f, 5, 5.f },
            { "heroic 2 rings, 5 close", 2, 45.f, 60.f, Ms(1500), 3.f, 5, 5.f },
            { "heroic 4 rings, 5 close", 4, 45.f, 60.f, Ms(1500), 16.f, 5, 5.f },
            { "heroic 4 rings, 10 close", 4, 45.f, 60.f, Ms(1500), 16.f, 10, 5.f },
            { "normal 1 ring, 5 at 15 yd", 1, 60.f, 45.f, Ms(2000), 3.f, 5, 15.f },
            { "heroic 4 rings, 10 at 15 yd", 4, 45.f, 60.f, Ms(1500), 16.f, 10, 15.f },
            // WP-SF: the user's rulings (ring 1 shown 5 s ahead, rings 2-4 from the apex): consecutive heroic slices
            // 15 degrees apart with 1.0 s between one band passing and the next arriving (the B10 report's ~38%
            // ring hits before the ring orbit). Gated (gate %, hint % with the turn hint, nova %).
            { "ruled normal 3 rings, 5 close", 3, 60.f, 45.f, Ms(2000), 16.f, 5, 5.f, true, false },
            { "ruled heroic 3 rings, 5 close", 3, 45.f, 60.f, Ms(1500), 10.f, 5, 5.f, true, false },
            { "ruled heroic 4 rings, 5 close", 4, 45.f, 60.f, Ms(1500), 16.f, 5, 5.f, true, false },
            { "ruled heroic 4 rings, 5 hinted", 4, 45.f, 60.f, Ms(1500), 16.f, 5, 5.f, true, true },
            { "ruled heroic 4 rings, 10 hinted", 4, 45.f, 60.f, Ms(1500), 16.f, 10, 5.f, true, true },
            { "ruled heroic 4 rings, 10 at 15 yd", 4, 45.f, 60.f, Ms(1500), 16.f, 10, 15.f, true, true },
        };
        for (PulseSpec const& spec : specs)
        {
            BrainModel::Counters sum;
            std::uint32_t novaHits = 0;
            std::uint32_t ringHits = 0;
            std::uint32_t pulses = 0;
            std::array<std::uint32_t, 4> byRing{};
            for (std::uint32_t seed = 0; seed < 4; ++seed)
            {
                PulseResult const r = RunPulses(spec, seed);
                sum.Add(r.c);
                novaHits += r.novaHits;
                ringHits += r.ringHits;
                pulses += r.pulses;
                for (std::size_t k = 0; k < byRing.size(); ++k)
                    byRing[k] += r.byRing[k];
            }
            std::uint32_t const passes = pulses * spec.bots * spec.rings;  // seeds x pulses x bots x rings
            std::uint32_t const novaPasses = pulses * spec.bots;
            std::printf("front pulses %-33s solves %5u, move %5u, fallback %4u, nova hits %3u of %3u, ring hits %3u "
                "of %4u (by ring %u/%u/%u/%u), solve us max %5lld mean %4lld\n", spec.name, sum.solves,
                sum.outcomes[1] + sum.outcomes[2], sum.Fallbacks(), novaHits, novaPasses, ringHits, passes, byRing[0],
                byRing[1], byRing[2], byRing[3], static_cast<long long>(sum.solveUsMax),
                static_cast<long long>(sum.solves ? sum.solveUsTotal / sum.solves : 0));
            if (spec.rangedYd >= 10.f)
                continue;
            if (!spec.ruled)
                CHECK(ringHits * 100 <= passes);
            else if (spec.rings < 4)
                CHECK(ringHits * 100 <= 3 * passes);                // the B10 gate: heroic up to 3 rings <= 3%
            else
                CHECK(ringHits * 100 <= (spec.hint ? 10u : 20u) * passes);
            if (spec.ruled)
                CHECK(novaHits * 100 <= 15 * novaPasses);           // the B10 gate on the nova
        }
    }

    // Rend RG6: one wall (normal) or two 3 s apart with their gaps on opposite sides (heroic), every 30 s (10 s
    // heroic), 3 s warning, through the 40 yd main strip; the group spread over the arena, back to its spots after.
    BrainModel::Counters RunSweeps(bool heroic, std::uint32_t bots, std::uint32_t seed, float maxApart = -1.f)
    {
        BrainModel::Config cfg;
        BrainModel::World world;
        world.cfg = cfg;
        std::vector<Vec3> slots;
        std::uint32_t rng = 0x85ebca6bu * (seed + 3);
        auto roll = [&rng]()
        {
            rng = rng * 1664525u + 1013904223u;
            return static_cast<float>(rng >> 8) / 16777216.f;
        };
        for (std::uint32_t k = 0; k < bots; ++k)
        {
            slots.push_back({ 135.f + 50.f * roll(), -436.f + 32.f * roll(), 0.f });
            BrainModel::Bot b;
            b.pos = slots.back();
            world.bots.push_back(b);
        }
        world.follow = [&slots](std::size_t i, Vec3& to)
        {
            to = slots[i];
            return true;
        };
        Solver::Bounds const strip = RendStrip();
        world.kinds = [&strip](std::size_t, Input& in, Ms, std::vector<BrainModel::Task>&)
        {
            in.bounds = { strip };
        };
        BoxOracle walls(108.f, -440.f, 194.f, -400.f);
        world.oracle = &walls;
        Vec3 const start{ 194.f, -420.f, 0.f };
        Ms const t0{ 100000 };
        Ms const end = t0 + Ms(60000);
        Ms const every = heroic ? Ms(20000) : Ms(30000);
        struct Pending
        {
            Ms at;
            std::vector<Hazard> hs;
        };
        std::vector<Pending> pending;
        std::uint32_t id = 1;
        for (Ms call = t0; call + Ms(14000) < end; call += every)
        {
            LineFrontSpec s;
            s.facing = Geo::kPi;
            s.travelYd = 86.f;
            s.widthYd = 40.f;
            float const g1 = *Front::PlaceGap(40.f, 8.f, {}, roll());
            s.gaps = { { g1, 8.f } };
            pending.push_back({ call, LineHazards(id++, s, call + Ms(3000), start) });
            if (heroic)
            {
                LineFrontSpec s2 = s;
                s2.gaps = { { *Front::PlaceGap(40.f, 8.f, {}, roll(), -Front::SideOf(g1), 0.f, g1, maxApart), 8.f } };
                pending.push_back({ call, LineHazards(id++, s2, call + Ms(6000), start) });
            }
        }
        for (Ms now = t0; now < end; now += cfg.step)
        {
            for (Pending& p : pending)
                if (!p.hs.empty() && p.at <= now)
                {
                    for (Hazard& h : p.hs)
                        world.AddHazard(h);
                    p.hs.clear();
                }
            world.Step(now);
        }
        return world.Total();
    }

    void TestSweeps()
    {
        // Normal: one wall a call. Heroic: two walls 3 s apart, the second gap on the other side, anywhere there (up to
        // 32 yd from the first: reported) or within 20 yd of the first (the placement the README recommends: gated).
        struct Case
        {
            char const* name;
            bool heroic;
            float maxApart;
            bool gated;
        };
        for (Case const& c : { Case{ "normal one wall,            ", false, -1.f, true },
                 Case{ "heroic two walls, any apart, ", true, -1.f, false },
                 Case{ "heroic two walls, <= 20 apart,", true, 20.f, true } })
            for (std::uint32_t bots : { 5u, 10u })
            {
                BrainModel::Counters sum;
                for (std::uint32_t seed = 0; seed < 4; ++seed)
                    sum.Add(RunSweeps(c.heroic, bots, seed, c.maxApart));
                std::uint32_t const passes = 4 * 3 * bots * (c.heroic ? 2 : 1);
                std::printf("front sweeps %s %2u bots: solves %5u, move %5u, fallback %4u, hits %3u of %3u wall "
                    "passes, solve us max %5lld\n", c.name, bots, sum.solves, sum.outcomes[1] + sum.outcomes[2],
                    sum.Fallbacks(), sum.hits, passes, static_cast<long long>(sum.solveUsMax));
                if (c.gated)
                    CHECK(sum.hits * 50 <= passes);     // at most 2% of the wall passes
            }
    }

    // ---- the drill --------------------------------------------------------------------------------------------------

    float KeyOf(std::string_view line, std::string_view key, float def)
    {
        std::optional<std::string_view> v = Drill::Value(line, key);
        return v ? Drill::Number(*v).value_or(-1.f) : def;
    }

    void TestDrill()
    {
        Drill::Def const* d = Drill::Find("rolling-fronts");
        CHECK(d != nullptr);
        if (!d)
            return;
        CHECK(d->cycleMs == 40000 && d->defaultCycles == 3);
        auto launch = [](Drill::Step const& st)
        {
            std::vector<std::string_view> const t = Drill::Tokens(st.line);
            return static_cast<float>(st.atMs) + Drill::Number(t[2]).value_or(0.f);
        };
        std::vector<Drill::Step> const& st = d->steps;
        CHECK(st.size() == 10);
        if (st.size() != 10)
            return;
        auto ring = [](Drill::Step const& step) { return Drill::Tokens(step.line)[0] == "conefront"; };
        // The rolling front rulings (as G2's row 18): ring 1 shown 5 s ahead, the nova registered 2.5 s later with its
        // 2.5 s warning, erupting as ring 1 launches; each later ring shown as the ring before it launches.
        // Normal (7 stacks): ring 1, nova 9.1, ring 2 two seconds later turned 45 (60 degree slices).
        // Heroic (14 stacks): ring 1, nova 11.2, rings 2-4 1.5 s apart turned 60 (45 degree slices).
        struct Pulse { std::size_t first; std::size_t rings; float nova; float slice; float turn; float spacing; };
        for (Pulse const& p : { Pulse{ 0, 2, 9.1f, 60.f, 45.f, 2000.f }, Pulse{ 3, 4, 11.2f, 45.f, 60.f, 1500.f } })
        {
            Drill::Step const& r1 = st[p.first];
            Drill::Step const& nova = st[p.first + 1];
            CHECK(ring(r1) && Drill::Tokens(nova.line)[0] == "circle");
            CHECK_NEAR(Drill::Number(Drill::Tokens(nova.line)[1]).value_or(0.f), p.nova, 1e-4);
            CHECK(launch(r1) - static_cast<float>(r1.atMs) == 5000.f);
            CHECK(launch(nova) - static_cast<float>(nova.atMs) == 2500.f && launch(nova) == launch(r1));
            // Ring 1 random, from the nova's edge, at the creature (it comes before the nova).
            CHECK(!Drill::Value(r1.line, "at") && !Drill::Value(r1.line, "turn") && !Drill::Value(r1.line, "bearing"));
            CHECK_NEAR(KeyOf(r1.line, "start", -1.f), p.nova, 1e-4);
            for (std::size_t k = 0; k < p.rings; ++k)
            {
                Drill::Step const& r = st[k == 0 ? p.first : p.first + 1 + k];
                CHECK(ring(r));
                CHECK(KeyOf(r.line, "slice", 0.f) == p.slice);
                CHECK(Drill::Value(r.line, "park") == std::optional<std::string_view>("1"));
                CHECK_NEAR(KeyOf(r.line, "until", 0.f), p.nova + 12.f, 1e-4);
                // The hint on every ring that has a next one (WP-SF), none on the last.
                CHECK(KeyOf(r.line, "next", 0.f) == (k + 1 < p.rings ? p.turn : 0.f));
                if (k == 0)
                    continue;
                Drill::Step const& prev = st[k == 1 ? p.first : p.first + k];
                // Later rings: from the apex pinned at the nova (ruling 2, at=prev), turned, shown as the one before
                // launches.
                CHECK(Drill::Value(r.line, "at") == std::optional<std::string_view>("prev"));
                CHECK(KeyOf(r.line, "start", -1.f) == 0.f && KeyOf(r.line, "turn", 0.f) == p.turn);
                CHECK(static_cast<float>(r.atMs) == launch(prev) && launch(r) - launch(prev) == p.spacing);
            }
        }
        // RG6: 8 yd gap, the second wall 3 s after the first, the other side, within 20 yd (ruling 3).
        CHECK(launch(st[9]) - launch(st[8]) == 3000.f && KeyOf(st[8].line, "gap", 0.f) == 8.f);
        CHECK(Drill::Value(st[9].line, "side") == std::optional<std::string_view>("other")
            && KeyOf(st[9].line, "apart", -1.f) == 20.f);
        // The walls start after the last heroic ring has rolled out.
        CHECK(static_cast<float>(st[8].atMs) >= launch(st[7]) + Drill::FrontRollMs(st[7].line));
        for (Drill::Step const& step : st)
        {
            CHECK(Drill::FrontRollMs(step.line) >= 0.f);
            if (Drill::Tokens(step.line)[0] != "circle")
                CHECK(KeyOf(step.line, "band", 4.f) == 4.f && KeyOf(step.line, "speed", 8.f) == 8.f);
            CHECK(static_cast<float>(step.atMs) + Drill::Number(Drill::Tokens(step.line)[2]).value_or(0.f)
                + Drill::FrontRollMs(step.line) <= static_cast<float>(d->cycleMs));
        }
        CHECK_NEAR(Drill::FrontRollMs(st[0].line), (45.f - 9.1f) / 8.f * 1000.f, 1e-1);
        CHECK_NEAR(Drill::FrontRollMs(st[2].line), 45.f / 8.f * 1000.f, 1e-1);
        CHECK_NEAR(Drill::FrontRollMs(st[8].line), 50.f / 8.f * 1000.f, 1e-2);
        CHECK(Drill::FrontBarTag(3) && Drill::FrontBarTag(6) && Drill::FrontBarTag(8) && !Drill::FrontBarTag(2));
    }

    // ---- cost -------------------------------------------------------------------------------------------------------

    void Benchmark()
    {
        // Emberseer at heroic 14+ stacks: the nova and four 315 degree rings, 1.5 s apart, each slice turned 60, all in
        // the view at once (the worst moment: ring 1 rolling, rings 2-4 registered). The bot at 12 to 30 yd round the
        // apex; plus a heroic Rend second wall and Shadowflame-sized pools as extra load.
        ConeFrontSpec s;
        s.sliceDeg = 45.f;
        s.startYd = 7.f + 0.3f * 16.f;
        Ms const launch = kNow + Ms(200);
        std::vector<Hazard> hz;
        hz.push_back(Nova(1, s.startYd, launch));
        for (std::uint32_t k = 0; k < 4; ++k)
        {
            ConeFrontSpec ring = s;
            ring.sliceBearing = Front::SliceBearing(0.4f, k, 60.f);
            hz.push_back(ConeHazard(2 + k, ring, launch + Ms(1500) * k));
        }
        StraightOracle oracle;
        double worstMean = 0.0;
        double worstMax = 0.0;
        std::uint32_t sink = 0;
        for (float r : { 12.f, 18.f, 24.f, 30.f })
        {
            Input in = BaseInput(Polar(2.f, r));
            in.hazards = hz;
            in.role.present = true;
            in.role.anchor = { 0.f, 0.f, 0.f };
            in.role.minYd = r - 3.f;
            in.role.maxYd = r + 3.f;
            Plan const first = Solver::Solve(in, oracle);
            int const runs = 100;
            double worst = 0.0;
            auto const t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < runs; ++i)
            {
                auto const s0 = std::chrono::steady_clock::now();
                sink += Solver::Solve(in, oracle).candidates;
                worst = std::max(worst, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()
                    - s0).count());
            }
            double const mean = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count()
                / runs;
            worstMean = std::max(worstMean, mean);
            worstMax = std::max(worstMax, worst);
            std::printf("front cost, 4 rings + nova, bot at %2.0f yd: %u candidates, %u path checks, outcome %d: mean "
                "%.0f us, max %.0f us (limit 2000 us)%s\n", r, first.candidates, first.pathChecks,
                static_cast<int>(first.outcome), mean, worst, mean > 2000.0 ? "  ** OVER **" : "");
        }
        // The route search over the same view (the brain's re-route): one grid round the four rings.
        Input in = BaseInput(Polar(2.f, 20.f));
        in.hazards = hz;
        Solver::RouteParams rp;
        Solver::RouteStats rs;
        auto const r0 = std::chrono::steady_clock::now();
        Solver::Route(in, Polar(-1.f, 20.f), oracle, rp, &rs);
        double const routeUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - r0).count();
        std::printf("front cost, route round 4 rings: %s, %u cells, %u blocked, %u expansions, %.0f us\n",
            rs.found ? "found" : "none", rs.cells, rs.blocked, rs.expansions, routeUs);
        CHECK(sink > 0);
        CHECK(worstMean < 2000.0);
        std::printf("front cost, worst case: mean %.0f us, max %.0f us\n", worstMean, worstMax);
    }
}

int main()
{
    TestLaunchOffset();
    TestConeSpec();
    TestLineSpec();
    TestPlaceGap();
    TestDisplays();
    TestConePieces();
    TestLinePieces();
    TestSliceSeeking();
    TestGapSeeking();
    TestParkTogether();
    TestPulses();
    TestSweeps();
    TestDrill();
    Benchmark();
    return Report("front_test");
}
