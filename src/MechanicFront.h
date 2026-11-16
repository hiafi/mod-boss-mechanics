/*
 * mod-boss-mechanics: rolling fronts (Blackrock Spire build WP-RF, shared doc BS5 and library L25).
 *
 * A rolling front is a band of a set thickness that rolls across the floor at a set speed and hits whoever it passes
 * over. Two shapes:
 * - a rolling cone front: the band expands from an apex (a ring), over every bearing but a safe slice (a 300 degree
 *   front with a 60 degree slice), from a start radius to an end radius (Emberseer's Furnace Pulse rings);
 * - a rolling line front: a wall across the room that sweeps along a facing, with one or more gaps (Rend's Freezing
 *   Sweep).
 *
 * In the registry both are Zones (Kind::Zone) with a single-pass projectile (MechanicMath.h ProjectileParams, with a
 * launch offset `start`): the cone front is a Cone shape whose projectile coordinate s is the radial distance from the
 * apex (ProjectileS), the line front a Line shape (s along the facing) whose gaps carve lateral strips out of the
 * hitbox. So the resolve, the hot windows, the solver, the route search and the brain model all reuse the projectile
 * code; this header holds what is front-specific: the spec to shape and timing, the gaps, the wall segments the bot
 * view turns a gapped wall into, the gap placement and slice turn rules the scripts and drills share, and the marker
 * pieces.
 *
 * Pure: no AzerothCore includes (tests/front_test.cpp compiles it with the host compiler).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_FRONT_H
#define MOD_BOSS_MECHANICS_MECHANIC_FRONT_H

#include "BossMechanicsIds.h"
#include "MechanicGeometry.h"
#include "MechanicMath.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace BossMechanics
{
    // One gap of a line front: its centre's lateral offset from the wall's centreline (Geo::ToLineCoords t: left of
    // the facing is positive) and its width, yd. Inside a gap the wall does not hit.
    struct FrontGap
    {
        float centre = 0.f;
        float width = 0.f;
    };

    enum class FrontType : std::uint8_t
    {
        Cone,
        Line
    };

    // A rolling cone front (Mechanic::ConeFront): the band rolls out from the apex.
    struct ConeFrontSpec
    {
        float sliceBearing = 0.f;   // radians: the middle of the safe slice, seen from the apex (0 = +x)
        float sliceDeg = 60.f;      // the safe slice's width; the band covers the other 360 - sliceDeg (0: a full ring)
        float startYd = 0.f;        // the band's inner edge at launch (Emberseer: the nova's radius)
        float endYd = 45.f;         // the front is over once the band's inner edge passes this (the walls)
        float bandYd = 4.f;         // the band's thickness
        float speed = 8.f;          // yd/s
        // Blackrock Spire WP-SF, bots only: a ring of a sequence whose next ring is coming, that ring's slice turned
        // this many degrees clockwise from this one (Front::SliceBearing's turn; 0: none or not known). The bots' orbit
        // (MechanicSolver.h Detail::RingOrbit) waits at the slice's side toward it; players learn the turn.
        float nextTurnDeg = 0.f;
    };

    // A rolling line front (Mechanic::LineFront): a wall across the room sweeping along `facing`. The start point is
    // the middle of the wall's start line; the wall reaches widthYd / 2 to each side.
    struct LineFrontSpec
    {
        float facing = 0.f;         // radians: the way the wall travels
        float travelYd = 0.f;       // the front is over once the wall's back edge passes this far from the start line
        float widthYd = 0.f;        // the wall's length across (the room's width)
        float bandYd = 4.f;         // the wall's thickness
        float speed = 8.f;          // yd/s
        std::vector<FrontGap> gaps; // where the wall does not hit (at least one, or the wall cannot be dodged)
    };

    // What the registry keeps for a front (ActiveMechanic::front), beside its projectile timing.
    struct FrontInfo
    {
        FrontType type = FrontType::Cone;
        float bandYd = 4.f;
        float startYd = 0.f;        // cone: the band's inner edge at launch
        float sliceBearing = 0.f;   // cone: the safe slice (radians) ...
        float sliceDeg = 0.f;       // ... and its width (0: none)
        float nextTurnDeg = 0.f;    // cone: ConeFrontSpec::nextTurnDeg
        std::vector<FrontGap> gaps; // line
    };
}

namespace BossMechanics::Front
{
    constexpr float kMinBandYd = 0.5f;
    constexpr float kMaxSliceDeg = 330.f;       // the front must cover at least 30 degrees
    // A wall segment narrower than this (between two gaps, or a gap's edge and the wall's end) is dropped: a sliver
    // is not a wall a player has to dodge.
    constexpr float kMinSegmentYd = 0.25f;
    // The marker step of a moving front (the band moves or grows at most this far per step), within
    // [kMinStepMs, the follow interval].
    constexpr float kStepYd = 0.8f;
    constexpr std::uint32_t kMinStepMs = 50;

    // ---- spec -> shape, facing, timing ------------------------------------------------------------------------------

    inline bool Valid(ConeFrontSpec const& s)
    {
        return s.bandYd >= kMinBandYd && s.speed > 0.f && s.startYd >= 0.f && s.endYd > s.startYd
            && s.sliceDeg >= 0.f && s.sliceDeg <= kMaxSliceDeg && std::isfinite(s.sliceBearing)
            && std::isfinite(s.nextTurnDeg);
    }

    inline bool Valid(LineFrontSpec const& s)
    {
        if (s.bandYd < kMinBandYd || s.speed <= 0.f || s.travelYd <= 0.f || s.widthYd <= 0.f
            || !std::isfinite(s.facing))
            return false;
        for (FrontGap const& g : s.gaps)
            if (g.width <= 0.f || !std::isfinite(g.centre))
                return false;
        return true;
    }

    // The cone's hitbox: every bearing but the slice, out to endYd. Its facing (the anchor's orientation) points away
    // from the slice (ConeFacing).
    inline Shape ConeShape(ConeFrontSpec const& s)
    {
        return Shape::Cone(360.f - s.sliceDeg, s.endYd);
    }

    inline float ConeFacing(ConeFrontSpec const& s)
    {
        return Geo::WrapPi(s.sliceBearing + Geo::kPi);
    }

    // Single pass: the band's centre starts at startYd + band / 2 (its inner edge on startYd) and the front is over
    // once its inner edge passes endYd: (endYd - startYd) / speed after the launch.
    inline ProjectileParams ConeParams(ConeFrontSpec const& s)
    {
        ProjectileParams p;
        p.length = s.endYd;
        p.speed = s.speed;
        p.halfLength = s.bandYd * 0.5f;
        p.start = s.startYd + s.bandYd * 0.5f;
        return p;
    }

    inline Shape LineShape(LineFrontSpec const& s)
    {
        return Shape::Line(s.travelYd, s.widthYd);
    }

    // The wall's back edge starts on the start line and the front is over once it passes travelYd.
    inline ProjectileParams LineParams(LineFrontSpec const& s)
    {
        ProjectileParams p;
        p.length = s.travelYd;
        p.speed = s.speed;
        p.halfLength = s.bandYd * 0.5f;
        p.start = s.bandYd * 0.5f;
        return p;
    }

    inline FrontInfo InfoOf(ConeFrontSpec const& s)
    {
        FrontInfo f;
        f.type = FrontType::Cone;
        f.bandYd = s.bandYd;
        f.startYd = s.startYd;
        f.sliceBearing = Geo::WrapPi(s.sliceBearing);
        f.sliceDeg = s.sliceDeg;
        f.nextTurnDeg = s.nextTurnDeg;
        return f;
    }

    inline FrontInfo InfoOf(LineFrontSpec const& s)
    {
        FrontInfo f;
        f.type = FrontType::Line;
        f.bandYd = s.bandYd;
        f.gaps = s.gaps;
        return f;
    }

    // How long after its launch a front is over.
    inline Ms Duration(ProjectileParams const& p)
    {
        return ProjectileDuration(p);
    }

    // ---- judging ----------------------------------------------------------------------------------------------------

    inline bool InGap(std::vector<FrontGap> const& gaps, float t)
    {
        for (FrontGap const& g : gaps)
            if (std::fabs(t - g.centre) < g.width * 0.5f - Geo::kEdgeEpsilon)
                return true;
        return false;
    }

    // Is p inside the front's hitbox (the whole path it will sweep, gaps carved out)? The band's position in time is
    // the projectile's (the registry's sweep, ProjectileHotWindows).
    inline bool Contains(Shape const& shape, Geo::Pose const& pose, std::vector<FrontGap> const& gaps,
        Geo::Vec3 const& p)
    {
        if (!Geo::Contains(shape, pose, p))
            return false;
        if (shape.type != ShapeType::Line || gaps.empty())
            return true;
        return !InGap(gaps, Geo::ToLineCoords(shape, pose, p).t);
    }

    // The band at `e` seconds after the launch: s from inner to outer (inner clamped at 0).
    struct Band
    {
        float inner = 0.f;
        float outer = 0.f;
    };

    inline Band BandAt(ProjectileParams const& p, float e)
    {
        float const c = ProjectileCentre(p, e);
        return { std::max(0.f, c - p.halfLength), c + p.halfLength };
    }

    // Is the front's band over p at `e` seconds after the launch (its hitbox and the band, no time margin)?
    inline bool HitsAt(Shape const& shape, Geo::Pose const& pose, std::vector<FrontGap> const& gaps,
        ProjectileParams const& p, float e, Geo::Vec3 const& q)
    {
        if (e < 0.f || !Contains(shape, pose, gaps, q))
            return false;
        float const s = ProjectileS(shape, pose, q);
        Band const b = BandAt(p, e);
        return s >= b.inner - Geo::kEdgeEpsilon && s <= b.outer + Geo::kEdgeEpsilon
            && e <= SecondsOf(ProjectileDuration(p)) + 1e-3f;
    }

    // ---- wall segments (the bot view: a gapped wall is one Line hazard per segment) --------------------------------

    struct Segment
    {
        Shape shape;
        Geo::Pose pose;
        float lo = 0.f;     // the segment's lateral extent (t) on the whole wall
        float hi = 0.f;
    };

    // The parts of a line front's wall between its gaps, each a Line shape of the segment's width posed on its own
    // centreline (moved sideways, same facing, same s). Gaps are clipped to the wall; slivers under kMinSegmentYd are
    // dropped. A wall with no gap is one segment: the shape itself.
    inline std::vector<Segment> Segments(Shape const& line, Geo::Pose const& pose, std::vector<FrontGap> const& gaps)
    {
        std::vector<Segment> out;
        if (line.type != ShapeType::Line || line.width <= 0.f)
            return out;
        float const half = line.width * 0.5f;
        std::vector<std::pair<float, float>> cut;
        for (FrontGap const& g : gaps)
        {
            float const a = std::max(-half, g.centre - g.width * 0.5f);
            float const b = std::min(half, g.centre + g.width * 0.5f);
            if (b > a)
                cut.push_back({ a, b });
        }
        std::sort(cut.begin(), cut.end());
        float const f = Geo::ShapeFacing(line, pose);
        float const nx = -std::sin(f);      // the left normal (t grows along it)
        float const ny = std::cos(f);
        auto add = [&](float lo, float hi)
        {
            if (hi - lo < kMinSegmentYd)
                return;
            Segment s;
            s.shape = line;
            s.shape.width = hi - lo;
            float const mid = (lo + hi) * 0.5f;
            s.pose = pose;
            s.pose.x += nx * mid;
            s.pose.y += ny * mid;
            s.lo = lo;
            s.hi = hi;
            out.push_back(s);
        };
        float at = -half;
        for (auto const& [a, b] : cut)
        {
            if (a > at)
                add(at, a);
            at = std::max(at, b);
        }
        if (at < half)
            add(at, half);
        return out;
    }

    // ---- placement rules the scripts and the drill share ------------------------------------------------------------

    // A slice turned `ring` times by `turnDeg` clockwise from `first` (Emberseer EM3: each extra ring's slice turned
    // 45 degrees on normal, 60 on heroic). Clockwise is a negative turn in the map's counter-clockwise angles.
    inline float SliceBearing(float first, std::uint32_t ring, float turnDeg)
    {
        return Geo::WrapPi(first - Geo::DegToRad(turnDeg) * static_cast<float>(ring));
    }

    // Is a gap centred at `centre` (width `width`) clear of every blocked lateral interval [lo, hi] (a live pool's
    // extent across the wall, Rend RG6: "a gap is never placed on a live pool")?
    inline bool GapClear(float centre, float width, std::vector<std::pair<float, float>> const& blocked)
    {
        float const a = centre - width * 0.5f;
        float const b = centre + width * 0.5f;
        for (auto const& [lo, hi] : blocked)
            if (b > lo && a < hi)
                return false;
        return true;
    }

    // Where to put a gap of `gapWidth` on a wall of `wallWidth` (lateral t in [-wallWidth / 2, wallWidth / 2]): its
    // whole width `edgeYd` or more inside the wall's ends, clear of every blocked interval, on the side `side` asks
    // (+1: the gap's centre left of the centreline, t >= 0; -1: right, t <= 0; 0: anywhere). `u` in [0, 1) picks
    // uniformly over the free centres (a script's roll). nullopt when no centre fits.
    // `near` / `within` (optional): the centre also within `within` yd of `near` (RG6 heroic: the second wall's gap
    // close enough to the first's for a player to cross behind the first wall; see README "Rolling fronts").
    inline std::optional<float> PlaceGap(float wallWidth, float gapWidth, std::vector<std::pair<float, float>> blocked,
        float u, int side = 0, float edgeYd = 0.f, float near = 0.f, float within = -1.f)
    {
        float lo = -wallWidth * 0.5f + edgeYd + gapWidth * 0.5f;
        float hi = wallWidth * 0.5f - edgeYd - gapWidth * 0.5f;
        if (within >= 0.f)
        {
            lo = std::max(lo, near - within);
            hi = std::min(hi, near + within);
        }
        if (side > 0)
            lo = std::max(lo, 0.f);
        else if (side < 0)
            hi = std::min(hi, 0.f);
        if (hi < lo)
            return std::nullopt;
        // Free centre intervals: [lo, hi] minus each blocked interval grown by half the gap.
        std::vector<std::pair<float, float>> free{ { lo, hi } };
        for (auto const& [blo, bhi] : blocked)
        {
            float const a = blo - gapWidth * 0.5f;
            float const b = bhi + gapWidth * 0.5f;
            std::vector<std::pair<float, float>> next;
            for (auto const& [fl, fh] : free)
            {
                if (b <= fl || a >= fh)
                {
                    next.push_back({ fl, fh });
                    continue;
                }
                if (a > fl)
                    next.push_back({ fl, a });
                if (b < fh)
                    next.push_back({ b, fh });
            }
            free = std::move(next);
        }
        float total = 0.f;
        for (auto const& [fl, fh] : free)
            total += fh - fl;
        if (free.empty())
            return std::nullopt;
        if (total <= 1e-4f)
            return free.front().first;      // a single free point (the gap fits exactly)
        float pick = std::clamp(u, 0.f, 0.9999f) * total;
        for (auto const& [fl, fh] : free)
        {
            if (pick <= fh - fl)
                return fl + pick;
            pick -= fh - fl;
        }
        return free.back().second;
    }

    // The side of the centreline a gap's centre is on (+1 left, -1 right, 0 on it), and the opposite one (RG6 heroic:
    // the second wall's gap on the other side).
    inline int SideOf(float centre)
    {
        return centre > 1e-3f ? 1 : (centre < -1e-3f ? -1 : 0);
    }

    // The lateral interval a circle (a pool) covers on the wall's axis, when it reaches into the wall's path at all.
    inline std::optional<std::pair<float, float>> BlockedBy(Shape const& line, Geo::Pose const& pose,
        Geo::Vec3 const& c, float radius)
    {
        Geo::LineCoords const lc = Geo::ToLineCoords(line, pose, c);
        if (lc.s < -radius || lc.s > line.length + radius)
            return std::nullopt;
        return std::make_pair(lc.t - radius, lc.t + radius);
    }

    // ---- markers ----------------------------------------------------------------------------------------------------

    // One marker piece of a front at absolute pose `pose` (the lift is the caller's): its display (0 = nothing drawn
    // in this slot now) and scale. `moving`: the piece moves or grows with the band (stepped every MarkerStepMs).
    struct Piece
    {
        std::uint32_t display = 0;
        float scale = 1.f;
        Geo::Pose pose;
        bool moving = false;
    };

    struct Colors
    {
        std::uint8_t danger = 0;    // the front (get-out)
        std::uint8_t safe = 2;      // the slice or the gaps (get-in)
    };

    // How often a moving front's markers step: the band moves kStepYd per step, within [kMinStepMs, maxMs].
    inline std::uint32_t MarkerStepMs(float speed, std::uint32_t maxMs)
    {
        if (speed <= 0.f)
            return maxMs;
        auto const ms = static_cast<std::uint32_t>(kStepYd / speed * 1000.f);
        return std::clamp<std::uint32_t>(ms, std::min(kMinStepMs, maxMs), std::max(kMinStepMs, maxMs));
    }

    // The arc band's display for a band from inner to outer (> inner) on a cone of `angleDeg`: the angle rounds up
    // (the band never drawn narrower than the hitbox), the hole's ratio rounds down (never drawn thinner). A band
    // thicker than half its outer radius draws the filled cone (or the wide cone, or a disc) of the outer radius.
    inline std::uint32_t ArcBandDisplay(std::uint8_t color, float angleDeg, float inner, float outer)
    {
        float const ratio = outer > 0.f ? inner / outer : 0.f;
        if (ratio < Ids::kArcBandRatios.front() - 1e-4f)
        {
            VisualPick const pick = PickVisual(Shape::Cone(angleDeg, outer), color, MarkerLayer::Fill);
            return pick.pieces.empty() ? 0 : pick.pieces.front().display;
        }
        std::size_t const a = std::min(LadderAtLeast(Ids::kArcBandAngles, angleDeg), Ids::kArcBandAngles.size() - 1);
        std::size_t const r = LadderAtMost(Ids::kArcBandRatios, ratio);
        return Ids::MarkerDisplay(color, Ids::MarkerShape::ArcBand, Ids::ArcBandStep(a, r));
    }

    // Every marker piece of a cone front at `e` seconds after its launch (negative: before it), in a fixed slot order:
    //   0  the outline: the cone of the front's hitbox (get-out, outline layer: a wide cone past 270 degrees);
    //   1  the safe slice (get-in, fill layer), its angle rounded down so the drawn slice is never wider than the real
    //      one; display 0 without a slice or below the ladder's 30 degrees;
    //   2  the band (get-out): an arc band of the front's angle from its inner to its outer edge, scale = the outer
    //      radius; display 0 before the launch and once the front is over.
    // `apex` carries the anchor's z; its facing is ignored (the front's facing is ConeFacing).
    inline std::vector<Piece> ConePieces(FrontInfo const& f, Shape const& shape, ProjectileParams const& p,
        Geo::Pose const& apex, float e, Colors const& colors)
    {
        std::vector<Piece> out(3);
        float const facing = Geo::WrapPi(f.sliceBearing + Geo::kPi);
        Geo::Pose at = apex;
        at.o = facing;
        VisualPick const outline = PickVisual(shape, colors.danger, MarkerLayer::Outline);
        if (!outline.pieces.empty())
            out[0] = { outline.pieces.front().display, outline.pieces.front().scale, at, false };
        if (f.sliceDeg >= Ids::kConeAngles.front() - 1e-4f)
        {
            std::size_t const j = LadderAtMost(Ids::kConeAngles, f.sliceDeg);
            Geo::Pose slice = apex;
            slice.o = f.sliceBearing;
            out[1] = { Ids::MarkerDisplay(colors.safe, Ids::MarkerShape::Cone, static_cast<std::uint32_t>(j)),
                shape.length, slice, false };
        }
        if (e >= 0.f && e <= SecondsOf(ProjectileDuration(p)))
        {
            Band const b = BandAt(p, e);
            float const outer = std::min(b.outer, shape.length);
            if (outer > b.inner + 0.05f)
                out[2] = { ArcBandDisplay(colors.danger, shape.angleDeg, b.inner, outer), outer, at, true };
        }
        out[2].pose = at;
        out[2].moving = true;
        return out;
    }

    // Every marker piece of a line front at `e` seconds after its launch, in a fixed slot order for a given segment
    // count (the gaps never change): per wall segment its lane (the path the wall still has to sweep, get-out outline;
    // its drawn start follows the wall's back edge in steps of the segment's width, ShrunkExcludeStart, so the ground
    // behind the wall reads clear), per gap its lane (get-in fill, the whole path), then per wall segment the wall
    // itself (get-out fill: a line across, `band` thick, from the segment's right end to its left end; display 0
    // before the launch and once the front is over). A segment lane longer than one ladder piece takes several slots
    // (kMaxLanePieces per segment, the unused ones display 0).
    constexpr std::size_t kMaxLanePieces = 4;

    inline std::vector<Piece> LinePieces(FrontInfo const& f, Shape const& shape, ProjectileParams const& p,
        Geo::Pose const& start, float e, Colors const& colors)
    {
        std::vector<Segment> const segments = Segments(shape, start, f.gaps);
        float const facing = Geo::ShapeFacing(shape, start);
        float const nx = -std::sin(facing);
        float const ny = std::cos(facing);
        bool const live = e >= 0.f && e <= SecondsOf(ProjectileDuration(p));
        float const tail = live ? ProjectileCentre(p, e) - p.halfLength : 0.f;
        std::vector<Piece> out;
        out.reserve(segments.size() * (kMaxLanePieces + 1) + f.gaps.size());
        // Lanes (the path still to sweep).
        for (Segment const& s : segments)
        {
            Shape lane = s.shape;
            lane.excludeStart = live ? ShrunkExcludeStart(tail, 0.f, lane.length, lane.width) : 0.f;
            Geo::Pose pose = s.pose;
            pose.o = facing;
            VisualPick const pick = PickVisual(lane, colors.danger, MarkerLayer::Outline);
            for (std::size_t k = 0; k < kMaxLanePieces; ++k)
            {
                Piece piece;
                piece.pose = pose;
                if (k < pick.pieces.size())
                {
                    piece.display = pick.pieces[k].display;
                    piece.scale = pick.pieces[k].scale;
                    piece.pose = Geo::Advance(pose, facing, pick.pieces[k].forward);
                    piece.pose.z = pose.z;
                }
                out.push_back(piece);
            }
        }
        // Gaps (the safe lanes).
        for (FrontGap const& g : f.gaps)
        {
            Geo::Pose pose = start;
            pose.o = facing;
            pose.x += nx * g.centre;
            pose.y += ny * g.centre;
            float const half = shape.width * 0.5f;
            float const a = std::max(-half, g.centre - g.width * 0.5f);
            float const b = std::min(half, g.centre + g.width * 0.5f);
            Piece piece;
            piece.pose = pose;
            if (b > a)
            {
                VisualPick const pick = PickVisual(Shape::Line(shape.length, b - a), colors.safe, MarkerLayer::Fill);
                if (pick.pieces.size() == 1)
                {
                    piece.display = pick.pieces.front().display;
                    piece.scale = pick.pieces.front().scale;
                    piece.pose.x += nx * ((a + b) * 0.5f - g.centre);
                    piece.pose.y += ny * ((a + b) * 0.5f - g.centre);
                }
            }
            out.push_back(piece);
        }
        // The wall: per segment a line across, from its right end (lo) to its left end (hi).
        Band const b = BandAt(p, std::max(0.f, e));
        float const along = (b.inner + b.outer) * 0.5f;
        for (Segment const& s : segments)
        {
            Piece piece;
            piece.moving = true;
            Geo::Pose right = start;
            right.x += std::cos(facing) * along + nx * s.lo;
            right.y += std::sin(facing) * along + ny * s.lo;
            right.o = Geo::WrapPi(facing + Geo::kPi * 0.5f);
            piece.pose = right;
            if (live)
            {
                VisualPick const pick = PickVisual(Shape::Line(s.hi - s.lo, b.outer - b.inner), colors.danger,
                    MarkerLayer::Fill);
                if (pick.pieces.size() == 1)
                {
                    piece.display = pick.pieces.front().display;
                    piece.scale = pick.pieces.front().scale;
                }
            }
            out.push_back(piece);
        }
        return out;
    }
}

#endif
