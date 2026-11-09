/*
 * mod-boss-mechanics: the drop spot rule's data and pure geometry (Stratholme build X2, handoff §3.4 gap (d)).
 *
 * A circle that follows a player and lands where they stand is a Zone anchored on that player; a lane from the boss
 * to a player is a Line whose `to` end is that player. The player carries the shape: the M3 view keeps the allies
 * out of it, and the drop spot rule (MechanicRef::DropSpot, DropSpotRule in MechanicRegistry.h) says where the
 * carrier bot takes it before it lands: onto preferred spots (an old web, old fire), off avoid points (bone piles),
 * at the room's edge, behind the group's travel direction, away from the group. The view turns the rule into a
 * Solver::Drop; the solver scores and adds candidates with it (MechanicSolver.h, "Drop spot"); the brain solves the
 * carrier with its spot competing (MechanicBotBrain.cpp, NeedsDropSolve).
 *
 * Pure header: no AzerothCore includes (tests/drop_spot_test.cpp compiles it with the host compiler).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DROP_SPOT_H
#define MOD_BOSS_MECHANICS_MECHANIC_DROP_SPOT_H

#include "MechanicGeometry.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace BossMechanics::Solver
{
    // A preferred spot or an avoid point (x, y; z is the floor's).
    struct DropCircle
    {
        Geo::Vec3 c;
        float r = 0.f;
        // Prefer only: the spot is a Zone of its own (an old web) the carrier may step into for the landing. The view
        // lifts that Zone from the carrier's hazards from Drop::enterAt (minus the margin) to the landing.
        bool enterable = false;
    };

    // The drop spot rule of the bot's carried shape (one per bot: the earliest landing when it carries several).
    // Soft: score terms and candidates on top of the carried shape's hard rule (no ally inside it).
    struct Drop
    {
        bool present = false;
        std::uint32_t id = 0;                   // the carried shape (Carried::id)
        std::chrono::milliseconds landAt{ 0 };  // when it lands (the carried shape's first window)
        std::chrono::milliseconds enterAt{ 0 }; // enterable spots open (landAt - DropSpotRule::maxDwell)
        // The room: a polygon (3+ points) or a circle (roomRadius > 0); neither = no room.
        std::vector<Geo::Vec3> room;
        Geo::Vec3 roomCenter;
        float roomRadius = 0.f;
        bool edge = false;                      // land at the room's edge (against a wall)
        bool away = false;                      // land away from the group (the counted allies' centroid)
        bool behind = false;                    // land behind the group along travelFacing, never ahead of it
        float travelFacing = 0.f;               // radians
        float maxRangeYd = 0.f;                 // within this of rangeFrom (0 = no limit)
        Geo::Vec3 rangeFrom;
        std::vector<DropCircle> prefer;
        std::vector<DropCircle> avoid;
    };
}

namespace BossMechanics::DropSpot
{
    constexpr float kInf = std::numeric_limits<float>::infinity();

    // Tuning of the rule (yards, score points; a yard of walking costs 1 point). Ordered so the rule reads as written
    // in the build specs: onto a preferred spot first (its bonus beats any edge or distance term within reach), else
    // next to it until it opens (staging), else the edge, away from the group; behind the travel direction and off the
    // avoid points always.
    constexpr float kPreferBonus = 40.f;        // the landing centre inside a preferred spot
    constexpr float kPreferInsetYd = 0.5f;      // ...at least this far inside its edge
    constexpr float kStageBonus = 25.f;         // next to an enterable spot that is not open yet (at its edge; 0 at
    constexpr float kStageYd = 4.f;             // ...this far out)
    constexpr float kStageGapYd = 0.6f;         // staging candidates: this far beyond the spot's edge + the margin
    constexpr float kEdgeInsetYd = 2.f;         // no edge cost within this of the room's edge
    constexpr float kEdgePerYd = 2.f;
    constexpr float kAwayPerYd = 0.6f;          // a reward per yard from the group's centroid...
    constexpr float kAwayCapYd = 25.f;          // ...up to this far
    constexpr float kBehindPerYd = 4.f;         // per yard the landing reaches ahead of the group's centroid
    constexpr float kAvoidBase = 20.f;          // the landing covers an avoid point
    constexpr float kAvoidPerYd = 10.f;
    constexpr float kAvoidPadYd = 0.5f;
    constexpr float kOutsideBase = 20.f;        // the spot is outside the room
    constexpr float kOutsidePerYd = 10.f;
    constexpr float kRangePerYd = 5.f;          // beyond maxRangeYd from the owner
    constexpr float kEdgeSampleYd = 3.f;        // room edge candidates: one every this many yards
    constexpr std::uint32_t kCircleRoomBearings = 24;
    constexpr std::uint32_t kAwayBearings = 8;
    constexpr float kAwayRings[] = { 10.f, 15.f, 20.f };

    // Ray-casting point-in-polygon on (x, y).
    inline bool InsidePolygon(std::vector<Geo::Vec3> const& poly, Geo::Vec3 const& p)
    {
        bool inside = false;
        std::size_t const n = poly.size();
        for (std::size_t i = 0, j = n - 1; i < n; j = i++)
        {
            Geo::Vec3 const& a = poly[i];
            Geo::Vec3 const& b = poly[j];
            if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                inside = !inside;
        }
        return inside;
    }

    inline float SegmentDistance(Geo::Vec3 const& p, Geo::Vec3 const& a, Geo::Vec3 const& b)
    {
        float const vx = b.x - a.x;
        float const vy = b.y - a.y;
        float const l2 = vx * vx + vy * vy;
        float u = l2 > 0.f ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2 : 0.f;
        u = std::clamp(u, 0.f, 1.f);
        return std::hypot(p.x - (a.x + u * vx), p.y - (a.y + u * vy));
    }

    inline bool HasRoom(Solver::Drop const& d)
    {
        return d.room.size() >= 3 || d.roomRadius > 0.f;
    }

    // Distance from p to the room's outline: positive inside, negative outside; +inf without a room.
    inline float RoomDistance(Solver::Drop const& d, Geo::Vec3 const& p)
    {
        if (d.room.size() >= 3)
        {
            float best = kInf;
            for (std::size_t i = 0; i < d.room.size(); ++i)
                best = std::min(best, SegmentDistance(p, d.room[i], d.room[(i + 1) % d.room.size()]));
            return InsidePolygon(d.room, p) ? best : -best;
        }
        if (d.roomRadius > 0.f)
            return d.roomRadius - Geo::Dist2d(d.roomCenter.x, d.roomCenter.y, p.x, p.y);
        return kInf;
    }

    // Points along the room's outline, `inset` yd inside it, about kEdgeSampleYd apart (a circle: kCircleRoomBearings).
    inline std::vector<Geo::Vec3> EdgePoints(Solver::Drop const& d, float inset, float z)
    {
        std::vector<Geo::Vec3> out;
        if (d.room.size() >= 3)
        {
            for (std::size_t i = 0; i < d.room.size(); ++i)
            {
                Geo::Vec3 const& a = d.room[i];
                Geo::Vec3 const& b = d.room[(i + 1) % d.room.size()];
                float const len = Geo::Dist2d(a.x, a.y, b.x, b.y);
                if (len < 1e-3f)
                    continue;
                float const ux = (b.x - a.x) / len;
                float const uy = (b.y - a.y) / len;
                // The inward normal: the left one, unless a point there is outside (a clockwise outline).
                float nx = -uy;
                float ny = ux;
                Geo::Vec3 const mid{ (a.x + b.x) * 0.5f + nx * inset, (a.y + b.y) * 0.5f + ny * inset, z };
                if (!InsidePolygon(d.room, mid))
                {
                    nx = -nx;
                    ny = -ny;
                }
                // From `inset` past one corner to `inset` short of the next (the corner's own inset point).
                float const span = len - 2.f * inset;
                if (span < 0.f)
                    continue;
                std::uint32_t const steps = std::max<std::uint32_t>(1,
                    static_cast<std::uint32_t>(std::ceil(span / kEdgeSampleYd)));
                for (std::uint32_t k = 0; k <= steps; ++k)
                {
                    float const s = inset + span * static_cast<float>(k) / static_cast<float>(steps);
                    Geo::Vec3 const q{ a.x + ux * s + nx * inset, a.y + uy * s + ny * inset, z };
                    if (RoomDistance(d, q) >= inset - 1e-3f)    // a sharp corner's neighbor edge may sit closer
                        out.push_back(q);
                }
            }
            return out;
        }
        if (d.roomRadius > inset)
            for (std::uint32_t i = 0; i < kCircleRoomBearings; ++i)
            {
                float const a = Geo::kTwoPi * static_cast<float>(i) / static_cast<float>(kCircleRoomBearings);
                out.push_back({ d.roomCenter.x + std::cos(a) * (d.roomRadius - inset),
                    d.roomCenter.y + std::sin(a) * (d.roomRadius - inset), z });
            }
        return out;
    }

    // The landing centre is inside a preferred spot (kPreferInsetYd in).
    inline bool InsidePrefer(Solver::Drop const& d, Geo::Vec3 const& p)
    {
        for (Solver::DropCircle const& c : d.prefer)
            if (Geo::Dist2d(c.c.x, c.c.y, p.x, p.y) <= c.r - kPreferInsetYd)
                return true;
        return false;
    }

    inline bool HasEnterable(Solver::Drop const& d)
    {
        for (Solver::DropCircle const& c : d.prefer)
            if (c.enterable)
                return true;
        return false;
    }

    // The staging bonus at p: next to an enterable spot that has not opened yet (kStageBonus at its edge, falling to
    // 0 kStageYd out). 0 inside one, or far from all of them.
    inline float StageBonus(Solver::Drop const& d, Geo::Vec3 const& p)
    {
        float best = 0.f;
        for (Solver::DropCircle const& c : d.prefer)
        {
            if (!c.enterable)
                continue;
            float const out = Geo::Dist2d(c.c.x, c.c.y, p.x, p.y) - c.r;
            if (out < 0.f || out > kStageYd)
                continue;
            best = std::max(best, kStageBonus * (1.f - out / kStageYd));
        }
        return best;
    }

    // How far a landing at p reaches ahead of the group's centroid along `facing` (the shape's reach past its anchor
    // included); <= 0 is behind.
    inline float Ahead(Geo::Vec3 const& centroid, float facing, Geo::Vec3 const& p, float reach)
    {
        return (p.x - centroid.x) * std::cos(facing) + (p.y - centroid.y) * std::sin(facing) + reach;
    }

    // `.mech test drop=<flags>`: a comma list of edge, away, behind and prefer; nullopt on an unknown word or none.
    struct Flags
    {
        bool edge = false;
        bool away = false;
        bool behind = false;
        bool prefer = false;
    };

    inline std::optional<Flags> ParseFlags(std::string_view s)
    {
        Flags f;
        bool any = false;
        while (!s.empty())
        {
            std::size_t const comma = s.find(',');
            std::string_view const word = s.substr(0, comma);
            if (word == "edge")
                f.edge = true;
            else if (word == "away")
                f.away = true;
            else if (word == "behind")
                f.behind = true;
            else if (word == "prefer")
                f.prefer = true;
            else
                return std::nullopt;
            any = true;
            if (comma == std::string_view::npos)
                break;
            s.remove_prefix(comma + 1);
        }
        if (!any)
            return std::nullopt;
        return f;
    }
}

#endif
