/*
 * mod-boss-mechanics: terrain-aware marker heights (M1-FIX-GROUND). Pure: no AzerothCore includes (tests/math_test.cpp
 * compiles it with the host compiler); the engine side passes a height probe (MechanicTelegraph.cpp: Map::GetHeight).
 *
 * Every marker is a flat quad with gravity off. Placed at the anchor's z, the ground of a rolling field rises above it
 * in places and the depth test hides the quad there (chevrons and notches along the edges, cells that look the wrong
 * size). So each marker piece samples the ground under its footprint and stands BossMechanics.Telegraph.Lift above the
 * highest sample:
 * - a line piece: a grid along its length and across its width, about every GroundSampleYd, ends and edges included;
 * - a disc, ring, dashed ring or donut: the centre plus rings out to the radius (a donut from its drawn hole);
 * - a cone: the apex plus arcs out to its length.
 * A static line whose pieces span too much height (MaxPieceSpreadYd) is cut into shorter end-to-end pieces from the
 * ladder (SplitLine), each with its own height. Moving markers sample a cheap set (CheapFootprint) on every follow
 * step.
 *
 * Heights: the probe returns the highest floor at or below a start height. The first search starts a little above the
 * anchor (kSearchUpYd), so a bridge or a ceiling overhead is never seen; only when there is no floor in the band below
 * it (the ground rises: a hill) does a second search start from the top of the band. A floor is accepted only within
 * the shape's zTolerance of the anchor z (the hitbox's own band), so another level never captures a marker. No accepted
 * sample: the anchor z.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_GROUND_H
#define MOD_BOSS_MECHANICS_MECHANIC_GROUND_H

#include "MechanicMath.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace BossMechanics::Ground
{
    // A probe result at or below this found nothing (the core's INVALID_HEIGHT).
    constexpr float kInvalidHeight = -100000.f;
    // The first search for a floor starts this far above the anchor z (never above its zTolerance).
    constexpr float kSearchUpYd = 2.f;
    // A split line piece is never shorter than its width nor than this (yd), except the last piece.
    constexpr float kMinLinePieceYd = 2.f;
    // A split line has at most about this many pieces (the minimum piece length grows for very long lines).
    constexpr std::uint32_t kMaxLinePieces = 24;
    // Sample points per footprint at most: the sample step grows past GroundSampleYd for bigger footprints.
    constexpr std::size_t kMaxFootprintPoints = 160;
    // Sample points over a whole split line at most (its rows, out to one ladder step past its end).
    constexpr std::size_t kMaxLineSamplePoints = 256;
    // The smallest sample step (yd), whatever GroundSampleYd says.
    constexpr float kMinSampleYd = 0.5f;

    struct Point2
    {
        float x = 0.f;
        float y = 0.f;
    };

    struct Settings
    {
        float sampleYd = 2.f;           // BossMechanics.Telegraph.GroundSampleYd
        float maxSpreadYd = 0.75f;      // BossMechanics.Telegraph.MaxPieceSpreadYd
        float lift = 0.3f;              // BossMechanics.Telegraph.Lift
    };

    // The accepted heights of a set of samples.
    struct Heights
    {
        float minZ = std::numeric_limits<float>::infinity();
        float maxZ = -std::numeric_limits<float>::infinity();
        std::uint32_t accepted = 0;

        void Add(float h)
        {
            minZ = std::min(minZ, h);
            maxZ = std::max(maxZ, h);
            ++accepted;
        }

        void Merge(Heights const& o)
        {
            minZ = std::min(minZ, o.minZ);
            maxZ = std::max(maxZ, o.maxZ);
            accepted += o.accepted;
        }

        float Spread() const
        {
            return accepted ? maxZ - minZ : 0.f;
        }

        // The highest accepted sample, or the anchor z when none was accepted.
        float Top(float anchorZ) const
        {
            return accepted ? maxZ : anchorZ;
        }
    };

    // Floor heights within a band around one anchor. Probe: float(float x, float y, float zFrom, float maxDown), the
    // highest floor at or below zFrom (searching at most maxDown below it), or <= kInvalidHeight when there is none.
    template <class Probe>
    class Sampler
    {
    public:
        Sampler(Probe& probe, float anchorZ, float zTolerance)
            : _probe(probe), _anchorZ(anchorZ), _tolerance(std::max(0.f, zTolerance)) { }

        float AnchorZ() const { return _anchorZ; }
        std::uint32_t Queries() const { return _queries; }

        bool InBand(float h) const
        {
            return h > kInvalidHeight && std::fabs(h - _anchorZ) <= _tolerance + 1e-3f;
        }

        // The floor under (x, y): first searched from a little above the anchor down to the band's bottom; when that
        // finds nothing in the band, from the band's top (the ground rises above the first start). false: none.
        bool FloorAt(float x, float y, float& out)
        {
            float const bottom = _anchorZ - _tolerance;
            float const first = _anchorZ + std::min(kSearchUpYd, _tolerance);
            float h = Query(x, y, first, first - bottom);
            if (InBand(h))
            {
                out = h;
                return true;
            }
            float const top = _anchorZ + _tolerance;
            if (top <= first + 1e-3f)
                return false;
            h = Query(x, y, top, top - bottom);
            if (InBand(h))
            {
                out = h;
                return true;
            }
            return false;
        }

        Heights Sample(std::vector<Point2> const& points)
        {
            Heights out;
            for (Point2 const& p : points)
            {
                float h = 0.f;
                if (FloorAt(p.x, p.y, h))
                    out.Add(h);
            }
            return out;
        }

    private:
        float Query(float x, float y, float zFrom, float maxDown)
        {
            ++_queries;
            float const h = _probe(x, y, zFrom, maxDown);
            return std::isfinite(h) ? h : kInvalidHeight;
        }

        Probe& _probe;
        float _anchorZ;
        float _tolerance;
        std::uint32_t _queries = 0;
    };

    // ------------------------------------------------------------------------------------------------------------
    // Footprints
    // ------------------------------------------------------------------------------------------------------------

    inline std::uint32_t Divisions(float length, float step)
    {
        return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(length / step - 1e-4f)));
    }

    // A grid over a line piece: `start` on the centreline at the piece's source end, facing along it.
    inline void AddLine(std::vector<Point2>& out, Geo::Pose const& start, float length, float width, float step)
    {
        float const c = std::cos(start.o);
        float const s = std::sin(start.o);
        std::uint32_t const ns = length > 0.f ? Divisions(length, step) : 0;
        std::uint32_t const nt = width > 0.f ? Divisions(width, step) : 0;
        for (std::uint32_t i = 0; i <= ns; ++i)
        {
            float const along = ns ? length * i / ns : 0.f;
            for (std::uint32_t j = 0; j <= nt; ++j)
            {
                float const across = nt ? -width * 0.5f + width * j / nt : 0.f;
                out.push_back({ start.x + c * along - s * across, start.y + s * along + c * across });
            }
        }
    }

    // Rings from `inner` (0: the centre point, a cone's apex) out to `radius`, each sampled about every `step` along
    // its arc, over [facing - half, facing + half] with both ends (half >= pi: the whole ring).
    inline void AddArcs(std::vector<Point2>& out, float cx, float cy, float inner, float radius, float facing,
        float half, float step)
    {
        bool const full = half >= Geo::kPi - 1e-4f;
        std::uint32_t const nr = radius > inner ? Divisions(radius - inner, step) : 0;
        for (std::uint32_t i = 0; i <= nr; ++i)
        {
            float const r = nr ? inner + (radius - inner) * i / nr : radius;
            if (r <= 1e-4f)
            {
                out.push_back({ cx, cy });
                continue;
            }
            float const span = full ? Geo::kTwoPi : 2.f * half;
            std::uint32_t const na = std::max<std::uint32_t>(full ? 6 : 2, Divisions(r * span, step));
            for (std::uint32_t j = 0; j < na + (full ? 0 : 1); ++j)
            {
                float const a = full ? span * j / na : facing - half + span * j / na;
                out.push_back({ cx + r * std::cos(a), cy + r * std::sin(a) });
            }
        }
    }

    // What a marker piece draws on the ground, from its display: a line piece's rectangle, a cone's sector, or a disc
    // (a donut from its drawn hole out).
    struct PieceShape
    {
        enum class Type : std::uint8_t
        {
            Disc,
            Cone,
            Line
        };

        Type type = Type::Disc;
        float radius = 0.f;     // Disc, Cone: radius / length
        float inner = 0.f;      // Disc: the drawn hole (donuts)
        float halfAngle = 0.f;  // Cone: half the drawn opening angle (radians)
        float length = 0.f;     // Line
        float width = 0.f;      // Line
    };

    inline PieceShape ShapeOfPiece(VisualPiece const& piece)
    {
        PieceShape out;
        Ids::MarkerDisplayParts const parts = Ids::DecodeMarkerDisplay(piece.display);
        out.radius = piece.scale;
        if (!parts.valid)
            return out;
        switch (parts.shape)
        {
            case Ids::MarkerShape::Line:
            case Ids::MarkerShape::LineOutline:
                out.type = PieceShape::Type::Line;
                out.length = PieceExtent(piece);
                out.width = piece.scale;
                break;
            case Ids::MarkerShape::Cone:
            case Ids::MarkerShape::ConeOutline:
                out.type = PieceShape::Type::Cone;
                out.halfAngle = Geo::DegToRad(Ids::kConeAngles[std::min<std::size_t>(parts.step,
                    Ids::kConeAngles.size() - 1)]) * 0.5f;
                break;
            case Ids::MarkerShape::Donut:
            case Ids::MarkerShape::DonutOutline:
                out.inner = Ids::kDonutRatios[std::min<std::size_t>(parts.step, Ids::kDonutRatios.size() - 1)]
                    * piece.scale;
                break;
            default:
                break;
        }
        return out;
    }

    // The sample points of one marker piece standing at `piecePose` (PieceWorldPose: a line piece's source end, a
    // cone's apex, a disc's centre), about every `step` yd, at most kMaxFootprintPoints (the step grows past that).
    inline std::vector<Point2> Footprint(Geo::Pose const& piecePose, VisualPiece const& piece, float step)
    {
        PieceShape const ps = ShapeOfPiece(piece);
        std::vector<Point2> out;
        step = std::max(step, kMinSampleYd);
        for (int tries = 0; tries < 32; ++tries)
        {
            out.clear();
            switch (ps.type)
            {
                case PieceShape::Type::Line:
                    AddLine(out, piecePose, ps.length, ps.width, step);
                    break;
                case PieceShape::Type::Cone:
                    AddArcs(out, piecePose.x, piecePose.y, 0.f, ps.radius, piecePose.o, ps.halfAngle, step);
                    break;
                case PieceShape::Type::Disc:
                    AddArcs(out, piecePose.x, piecePose.y, ps.inner, ps.radius, 0.f, Geo::kPi, step);
                    break;
            }
            if (out.size() <= kMaxFootprintPoints)
                break;
            step *= 1.25f;
        }
        return out;
    }

    // The cheap set a moving marker samples on every follow step: a line piece's centreline at its ends, quarters and
    // middle plus its two edges at the middle; a cone's apex, its half-length point and three points on its arc; a
    // disc's centre and six points on its rim.
    inline std::vector<Point2> CheapFootprint(Geo::Pose const& piecePose, VisualPiece const& piece)
    {
        PieceShape const ps = ShapeOfPiece(piece);
        std::vector<Point2> out;
        float const c = std::cos(piecePose.o);
        float const s = std::sin(piecePose.o);
        auto at = [&](float along, float across)
        {
            out.push_back({ piecePose.x + c * along - s * across, piecePose.y + s * along + c * across });
        };
        switch (ps.type)
        {
            case PieceShape::Type::Line:
                for (int i = 0; i <= 4; ++i)
                    at(ps.length * i / 4.f, 0.f);
                at(ps.length * 0.5f, ps.width * 0.5f);
                at(ps.length * 0.5f, -ps.width * 0.5f);
                break;
            case PieceShape::Type::Cone:
                at(0.f, 0.f);
                at(ps.radius * 0.5f, 0.f);
                for (int i = -1; i <= 1; ++i)
                    at(ps.radius * std::cos(ps.halfAngle * i), ps.radius * std::sin(ps.halfAngle * i));
                break;
            case PieceShape::Type::Disc:
                out.push_back({ piecePose.x, piecePose.y });
                for (int i = 0; i < 6; ++i)
                {
                    float const a = Geo::kTwoPi * i / 6.f;
                    out.push_back({ piecePose.x + ps.radius * std::cos(a), piecePose.y + ps.radius * std::sin(a) });
                }
                break;
        }
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Marker heights
    // ------------------------------------------------------------------------------------------------------------

    // The world z of a marker piece of the drawn pick anchored at `anchor`: the highest accepted floor under its
    // footprint (cheap: CheapFootprint) + the lift, or the anchor z + the lift when nothing was accepted.
    template <class Probe>
    float PieceZ(Sampler<Probe>& sampler, Geo::Pose const& anchor, VisualPiece const& piece, Settings const& st,
        bool cheap, Heights* heights = nullptr)
    {
        Geo::Pose const at = PieceWorldPose(anchor, piece);
        Heights const h = sampler.Sample(cheap ? CheapFootprint(at, piece) : Footprint(at, piece, st.sampleYd));
        if (heights)
            *heights = h;
        return h.Top(sampler.AnchorZ()) + st.lift;
    }

    // Sets every piece's z (PieceZ). Returns the largest spread of any piece.
    template <class Probe>
    float AssignZ(VisualPick& pick, Sampler<Probe>& sampler, Geo::Pose const& anchor, Settings const& st, bool cheap)
    {
        float spread = 0.f;
        for (VisualPiece& piece : pick.pieces)
        {
            Heights h;
            piece.z = PieceZ(sampler, anchor, piece, st, cheap, &h);
            piece.hasZ = true;
            spread = std::max(spread, h.Spread());
        }
        return spread;
    }

    // The fill layer's heights from the outline's: each fill piece stands at the highest outline piece its own extent
    // along the axis overlaps. A line's fill piece k at fraction f lies between the source and outline piece k's far
    // end, so it never sinks under an earlier, higher piece; at the full fill it overlaps only piece k and stands at
    // its height. Discs, rings, cones and donuts are one piece on one centre: the outline's height.
    inline void AssignFillZ(VisualPick& fill, VisualPick const& outline)
    {
        if (outline.pieces.empty() || !outline.pieces.front().hasZ)
            return;
        for (VisualPiece& piece : fill.pieces)
        {
            float const a = piece.forward;
            float const b = a + PieceExtent(piece);
            bool found = false;
            float z = -std::numeric_limits<float>::infinity();
            for (VisualPiece const& o : outline.pieces)
            {
                float const oa = o.forward;
                float const ob = oa + PieceExtent(o);
                bool const overlaps = b > a + 1e-3f ? (b > oa + 1e-3f && a < ob - 1e-3f) : (a >= oa - 1e-3f
                    && a <= ob + 1e-3f);
                if (overlaps && o.hasZ)
                {
                    z = std::max(z, o.z);
                    found = true;
                }
            }
            piece.hasZ = true;
            piece.z = found ? z : outline.pieces.front().z;
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Line split
    // ------------------------------------------------------------------------------------------------------------

    struct LineSplit
    {
        LineSteps steps;                // ladder step per piece, source end first (PickVisual's lineSteps)
        std::vector<float> z;           // marker z per piece (lift included)
        std::vector<float> spread;      // sampled spread per piece
        bool minLengthStopped = false;  // a piece over the spread limit could not be cut (minimum length)
    };

    // The shortest non-last piece of a split line (yd): its width, at least kMinLinePieceYd, and long enough that the
    // line needs at most about kMaxLinePieces pieces.
    inline float MinLinePieceYd(Shape const& shape)
    {
        float const drawn = shape.length - shape.excludeStart;
        return std::max({ shape.width, kMinLinePieceYd, drawn / static_cast<float>(kMaxLinePieces) });
    }

    // Cuts a static line's drawn shape into end-to-end ladder pieces whose sampled spread is at most st.maxSpreadYd,
    // each as long as possible, none but the last shorter than MinLinePieceYd. The ground is sampled once, in rows
    // across the line about every st.sampleYd along it (lazily, out to the last piece's drawn end); a piece's heights
    // are the rows from the last one at or before its start to the first one at or after its end. Flat ground (or a
    // spread within the limit) gives DefaultLineSteps. Every piece but the last is an exact ladder length, so the
    // pieces abut and only the last one overshoots, at the far end.
    template <class Probe>
    LineSplit SplitLine(Shape const& shape, Geo::Pose const& pose, Sampler<Probe>& sampler, Settings const& st)
    {
        LineSplit out;
        float const w = shape.width;
        float const s0 = shape.excludeStart;
        float const end = shape.length;
        float const drawn = end - s0;
        if (shape.type != ShapeType::Line || drawn <= 0.f || w <= 0.f)
            return out;

        // Sample step: GroundSampleYd, grown so the rows over the line (and one ladder step of overshoot) stay within
        // kMaxLineSamplePoints.
        float step = std::max(st.sampleYd, kMinSampleYd);
        float const reach = drawn * 1.2f + w;
        while ((std::ceil(reach / step) + 2.f) * (std::ceil(w / step) + 1.f)
            > static_cast<float>(kMaxLineSamplePoints))
            step *= 1.25f;

        float const facing = Geo::ShapeFacing(shape, pose);
        Geo::Pose const source = Geo::Advance(pose, facing, 0.f);
        std::uint32_t const nt = Divisions(w, step);
        std::vector<Heights> rows;
        std::vector<bool> sampled;
        auto row = [&](std::size_t i) -> Heights const&
        {
            if (i >= rows.size())
            {
                rows.resize(i + 1);
                sampled.resize(i + 1, false);
            }
            if (!sampled[i])
            {
                sampled[i] = true;
                std::vector<Point2> pts;
                Geo::Pose const at = Geo::Advance(source, facing, s0 + step * static_cast<float>(i));
                float const c = std::cos(facing);
                float const s = std::sin(facing);
                for (std::uint32_t j = 0; j <= nt; ++j)
                {
                    float const across = -w * 0.5f + w * j / nt;
                    pts.push_back({ at.x - s * across, at.y + c * across });
                }
                rows[i] = sampler.Sample(pts);
            }
            return rows[i];
        };
        auto range = [&](float a, float b)
        {
            Heights h;
            std::size_t const i0 = static_cast<std::size_t>(std::max(0.f, std::floor((a - s0) / step + 1e-4f)));
            std::size_t const i1 = static_cast<std::size_t>(std::max(0.f, std::ceil((b - s0) / step - 1e-4f)));
            for (std::size_t i = i0; i <= i1; ++i)
                h.Merge(row(i));
            return h;
        };
        auto push = [&](std::uint32_t stepIndex, float a, Heights const& h)
        {
            out.steps.push_back(stepIndex);
            out.z.push_back(h.Top(sampler.AnchorZ()) + st.lift);
            out.spread.push_back(h.Spread());
            return a + LineAspect(stepIndex) * w;
        };

        float const minLen = MinLinePieceYd(shape);
        std::uint32_t const minStep = LineStepFor(minLen / w);
        float const limit = std::max(0.f, st.maxSpreadYd);
        float a = s0;
        for (std::uint32_t guard = 0; guard < 4 * kMaxLinePieces + 8; ++guard)
        {
            float const rest = end - a;
            // The last piece, rounded up: when it fits in one ladder piece and its spread is within the limit (or it
            // is too short to cut), it ends the line.
            std::uint32_t const lastStep = LineStepFor(rest / w);
            float const lastLen = LineAspect(lastStep) * w;
            bool const fits = lastLen >= rest * (1.f - 1e-5f);
            // The longest non-last piece: a ladder length short of the end.
            std::int32_t jMax = -1;
            for (std::int32_t j = static_cast<std::int32_t>(Ids::kLineSteps) - 1; j >= 0; --j)
                if (LineAspect(static_cast<std::uint32_t>(j)) * w < rest - 1e-3f)
                {
                    jMax = j;
                    break;
                }
            bool const canCut = jMax >= static_cast<std::int32_t>(minStep);
            if (fits)
            {
                Heights const h = range(a, a + lastLen);
                if (h.Spread() <= limit || !canCut)
                {
                    if (h.Spread() > limit)
                        out.minLengthStopped = true;
                    push(lastStep, a, h);
                    return out;
                }
            }
            else if (!canCut)
            {
                // Longer than one ladder piece yet nothing to cut with (only when the ladder runs out): the default.
                break;
            }
            // Binary search the longest piece within the limit (spread only grows with the length).
            std::int32_t lo = static_cast<std::int32_t>(minStep);
            std::int32_t hi = jMax;
            std::int32_t best = -1;
            while (lo <= hi)
            {
                std::int32_t const mid = (lo + hi) / 2;
                if (range(a, a + LineAspect(static_cast<std::uint32_t>(mid)) * w).Spread() <= limit)
                {
                    best = mid;
                    lo = mid + 1;
                }
                else
                    hi = mid - 1;
            }
            if (best < 0)
            {
                best = static_cast<std::int32_t>(minStep);
                out.minLengthStopped = true;
            }
            std::uint32_t const j = static_cast<std::uint32_t>(best);
            a = push(j, a, range(a, a + LineAspect(j) * w));
        }

        // Fallback (never expected): the default layout, each piece at its own sampled height.
        out = LineSplit();
        float at = s0;
        for (std::uint32_t j : DefaultLineSteps(shape))
            at = push(j, at, range(at, at + LineAspect(j) * w));
        return out;
    }
}

#endif
