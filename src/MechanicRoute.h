/*
 * mod-boss-mechanics: the route search (dungeon-mechanics STAGE-M3-HANDOFF §3.9).
 *
 * Pure header: no AzerothCore or playerbots includes (tests/route_test.cpp compiles it with the host compiler). When a
 * following bot's motion crosses hazards and the greedy one-hop re-route (Solve with Input::reroute) finds no spot that
 * makes progress, e.g. behind a U-shaped wall of overlapping mines, the brain asks Route for a way around: an
 * 8-connected grid A* over the follow box, string-pulled into a few straight legs, each leg then walked on the
 * oracle's real path.
 *
 * Every leg is validated the way Solve validates a path: the oracle's walked path, densified, against each hazard at
 * the time the bot is there (CheckPath), at the full margin and else at the reduced one; never a grace crossing. That
 * per-leg rule is adapted from mod-dungeon-clear (Jared Wright, AGPL-3.0-or-later), Util/DcHazard.h, commit 7f67cb5.
 *
 * Stratholme X1: Input::softHazards (other players' carried shapes at their live spots, Barthilas's Infection
 * carriers walking to the Sick Ward) are soft blockers: a cell they cover at the time the bot gets there costs
 * RouteParams::softCost more to enter, never blocks, and the string-pull never shortcuts through more of them than the
 * grid path it replaces. Without soft hazards every answer is the M4 one.
 *
 * Times are absolute GameTime milliseconds. Distances are yards.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_ROUTE_H
#define MOD_BOSS_MECHANICS_MECHANIC_ROUTE_H

#include "MechanicSolver.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace BossMechanics::Solver
{
    struct RouteParams
    {
        float padYd = 8.f, cellYd = 1.f;
        std::uint32_t maxCells = 4096, maxExpansions = 4096, maxLegs = 8;
        // M4 §3.7 (the U minefield): when the box finds no route and hazards cross its edge, it grows once to hold
        // the whole obstacle they belong to, at most this far past the first box on each side (0 = never).
        float growYd = 32.f;
        // Stratholme X1: the extra cost of entering a cell a soft blocker covers then, in thousandths of a cell's walk
        // (4000: a detour of up to four cells per covered cell is preferred to crossing it). 0 = soft blockers off.
        std::uint32_t softCost = 4000;
    };

    struct RouteStats
    {
        std::uint32_t cells = 0;            // grid cells
        std::uint32_t blocked = 0;          // cells some hazard covers at some time (before any leg marks)
        std::uint32_t expansions = 0;       // A* expansions, all searches (RouteParams::maxExpansions caps each)
        std::uint32_t legs = 0;             // legs of the route found (or of the last string-pull)
        std::uint32_t oracleCalls = 0;      // oracle path calls, all searches (capped by Params::maxPathChecks)
        std::uint32_t retries = 0;          // searches re-run after the navmesh rejected a leg (0 or 1)
        std::uint32_t pullChecks = 0;       // CheckPath calls of the string-pull, all searches
        bool found = false;
        float cellYd = 0.f;                 // the cell size used (grows past RouteParams::cellYd for a large box)
        std::uint32_t grown = 0;            // 1: the first box found no route and grew round the obstacle it clipped
        std::uint32_t softCells = 0;        // cells some soft blocker covers at some time (the last search's box)
        std::uint32_t softCrossed = 0;      // soft cells the found route's grid path enters while they are hot
        std::uint32_t softPullDropped = 0;  // the soft pull needed > maxLegs: 1 relaxed (kSoftPullSlack), 2 dropped
        // RegionRoute only: no route worked and another goal is left (try it with firstGoal = nextGoal).
        bool moreGoals = false;
        std::uint32_t nextGoal = 0;
    };

    namespace RouteDetail
    {
        constexpr std::uint32_t kOrthoCost = 1000;      // integer step costs: octile distance in thousandths
        constexpr std::uint32_t kDiagCost = 1414;
        constexpr std::uint32_t kDiagExtra = kDiagCost - kOrthoCost;
        constexpr std::uint32_t kPullLookahead = 8;     // string-pull: grid points scanned past the last clear one
        constexpr std::uint32_t kPullConfirms = 8;      // string-pull: CheckPath confirmations per leg
        constexpr float kMinCellYd = 0.25f;
        constexpr std::uint32_t kSoftPullSlack = 2;     // X1: soft core cells a relaxed shortcut may cut
        constexpr float kGrowCell = 1.02f;
        constexpr float kMaxRouteYd = 500.f;            // bot-goal distance and padding beyond this: no route
        constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();
        constexpr std::uint32_t kNoParent = std::numeric_limits<std::uint32_t>::max();

        struct Grid
        {
            float x0 = 0.f, y0 = 0.f, cell = 1.f;
            std::uint32_t nx = 0, ny = 0;
            Geo::Vec3 a, b;                                 // bot and goal (z interpolated between them)
            // Per cell, the hull of its hot windows (ms; from > to: never hot). Seal widens it; a cell under a leg the
            // navmesh rejected is blocked for good.
            std::vector<std::int64_t> hotFrom, hotTo;
            std::uint32_t start = 0, goal = 0;              // the bot's and the goal's cells (always free)
            std::int64_t moveStart = 0;                     // ms: the bot starts walking (now + startDelay)
            std::int64_t widen = 0;                         // ms: hot hulls widened by marginMs + a cell's travel
            float msPerYd = 0.f;
            // Stratholme X1: per cell, the hull of the soft blockers' hot windows (empty: none in the input), and what
            // entering one then costs (RouteParams::softCost).
            std::vector<std::int64_t> softFrom, softTo;
            // ...and the cells their shapes themselves touch (no margin): what the string-pull may not cut into.
            std::vector<std::int64_t> softCoreFrom, softCoreTo;
            std::uint32_t softCost = 0;

            std::uint32_t Size() const
            {
                return nx * ny;
            }

            std::uint32_t Index(std::uint32_t ix, std::uint32_t iy) const
            {
                return iy * nx + ix;
            }

            std::uint32_t CellOf(float x, float y) const
            {
                auto clampTo = [](float v, std::uint32_t n)
                {
                    std::int64_t const i = static_cast<std::int64_t>(std::floor(v));
                    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(i, 0, static_cast<std::int64_t>(n) - 1));
                };
                return Index(clampTo((x - x0) / cell, nx), clampTo((y - y0) / cell, ny));
            }

            // Cell center; z interpolated along bot -> goal so a sloped follow box stays on the hazards' floor.
            Geo::Vec3 Center(std::uint32_t idx) const
            {
                float const x = x0 + (static_cast<float>(idx % nx) + 0.5f) * cell;
                float const y = y0 + (static_cast<float>(idx / nx) + 0.5f) * cell;
                float const vx = b.x - a.x;
                float const vy = b.y - a.y;
                float const l2 = vx * vx + vy * vy;
                float const u = l2 > 0.f ? std::clamp(((x - a.x) * vx + (y - a.y) * vy) / l2, 0.f, 1.f) : 0.f;
                return { x, y, a.z + (b.z - a.z) * u };
            }
        };

        struct Box
        {
            float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        };

        // The box around bot and goal padded by `pad`.
        inline Box FollowBox(Geo::Vec3 const& bot, Geo::Vec3 const& goal, float pad)
        {
            pad = std::max(0.f, pad);
            return { std::min(bot.x, goal.x) - pad, std::min(bot.y, goal.y) - pad, std::max(bot.x, goal.x) + pad,
                std::max(bot.y, goal.y) + pad };
        }

        // The grid over `box`; the cell grows past cellYd until the grid fits maxCells.
        inline bool MakeGrid(Box const& box, Geo::Vec3 const& bot, Geo::Vec3 const& goal, RouteParams const& rp,
            Grid& g)
        {
            if (rp.maxCells == 0)
                return false;
            g.x0 = box.x0;
            g.y0 = box.y0;
            float const w = box.x1 - box.x0;
            float const h = box.y1 - box.y0;
            float cell = std::max({ kMinCellYd, rp.cellYd, std::sqrt(w * h / static_cast<float>(rp.maxCells)) });
            auto count = [&](float c)
            {
                return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(w / c)))
                    * std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(h / c)));
            };
            while (count(cell) > rp.maxCells)
                cell *= kGrowCell;
            g.cell = cell;
            g.nx = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(w / cell)));
            g.ny = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(h / cell)));
            g.a = bot;
            g.b = goal;
            g.hotFrom.assign(g.Size(), kNever);
            g.hotTo.assign(g.Size(), -kNever);
            g.start = g.CellOf(bot.x, bot.y);
            g.goal = g.CellOf(goal.x, goal.y);
            return true;
        }

        // The box around bot and goal padded by padYd.
        inline bool MakeGrid(Geo::Vec3 const& bot, Geo::Vec3 const& goal, RouteParams const& rp, Grid& g)
        {
            return MakeGrid(FollowBox(bot, goal, rp.padYd), bot, goal, rp, g);
        }

        // Does hazard `h` count for a route timed to `horizonEnd` (Rasterize's filter)?
        inline bool InRouteTime(Input const& in, Hazard const& h, Ms horizonEnd)
        {
            return !h.poses.empty() && Detail::HazardEnd(h) + in.params.marginMs >= in.now
                && h.timing.from - in.params.marginMs <= horizonEnd;
        }

        // M4 §3.7 (the U minefield): the first box found no route. Was it the box? The hazards whose outline (bounds
        // plus margin) crosses its edge are the seed; every hazard whose outline comes within `ring` of a member's
        // joins (a wall of overlapping mines is one obstacle). The box grows to hold the whole obstacle plus `ring`
        // on every side, by at most `growYd` per side. false: no hazard crosses the edge (a real enclosure: the
        // first answer stands) or the box would not grow.
        inline bool GrowBox(Input const& in, Ms horizonEnd, float ring, float growYd, Box& box)
        {
            struct Disc
            {
                float x = 0.f, y = 0.f, r = 0.f;
                bool in = false;
            };
            std::vector<Disc> discs;
            for (Hazard const& h : in.hazards)
            {
                if (!InRouteTime(in, h, horizonEnd))
                    continue;
                float const r = Geo::Bounds(h.shape) + std::max(0.f, h.marginYd);
                for (Geo::Pose const& pose : h.poses)
                    if (std::isfinite(pose.x) && std::isfinite(pose.y) && std::isfinite(r))
                        discs.push_back({ pose.x, pose.y, r, false });
            }
            std::vector<std::size_t> open;
            for (std::size_t i = 0; i < discs.size(); ++i)
            {
                Disc& d = discs[i];
                bool const meets = d.x + d.r >= box.x0 && d.x - d.r <= box.x1 && d.y + d.r >= box.y0
                    && d.y - d.r <= box.y1;
                bool const inside = d.x - d.r >= box.x0 && d.x + d.r <= box.x1 && d.y - d.r >= box.y0
                    && d.y + d.r <= box.y1;
                if (meets && !inside)
                {
                    d.in = true;
                    open.push_back(i);
                }
            }
            if (open.empty())
                return false;
            Box grown = box;
            while (!open.empty())
            {
                Disc const d = discs[open.back()];
                open.pop_back();
                grown.x0 = std::min(grown.x0, d.x - d.r - ring);
                grown.y0 = std::min(grown.y0, d.y - d.r - ring);
                grown.x1 = std::max(grown.x1, d.x + d.r + ring);
                grown.y1 = std::max(grown.y1, d.y + d.r + ring);
                for (std::size_t j = 0; j < discs.size(); ++j)
                {
                    Disc& o = discs[j];
                    if (!o.in && Geo::Dist2d(d.x, d.y, o.x, o.y) <= d.r + o.r + ring)
                    {
                        o.in = true;
                        open.push_back(j);
                    }
                }
            }
            grown.x0 = std::max(grown.x0, box.x0 - growYd);
            grown.y0 = std::max(grown.y0, box.y0 - growYd);
            grown.x1 = std::min(grown.x1, box.x1 + growYd);
            grown.y1 = std::min(grown.y1, box.y1 + growYd);
            constexpr float kMinGrowYd = 0.5f;
            if (grown.x0 > box.x0 - kMinGrowYd && grown.y0 > box.y0 - kMinGrowYd && grown.x1 < box.x1 + kMinGrowYd
                && grown.y1 < box.y1 + kMinGrowYd)
                return false;
            box = grown;
            return true;
        }

        inline bool Finite(Geo::Vec3 const& p)
        {
            return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
        }

        inline bool Finite(Geo::Pose const& p)
        {
            return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(p.o);
        }

        // One hazard pose prepared for rasterizing (STAGE-M3-R-REPORT M-5): the shape's frame computed once
        // (Detail::PoseFrame, so the exact signed distance of a cell costs no trigonometry), and a conservative outline
        // of the shape grown by the hazard's margin (a circle around the anchor and, for a Line or a cone of at most
        // 180°, half-planes) that gives each grid row the span of cells worth testing.
        struct PoseRaster
        {
            Detail::PoseFrame frame;
            float outer = 0.f;                      // outline circle radius around the anchor
            struct HalfPlane
            {
                float a = 0.f, b = 0.f, c = 0.f;    // a dx + b dy <= c (relative to the anchor)
            };
            std::uint32_t planes = 0;
            HalfPlane plane[4];
            bool skip = false;                      // the shape holds no point (a zero-length line)
        };

        inline PoseRaster PrepareRaster(Shape const& s, Geo::Pose const& pose, float grow)
        {
            PoseRaster r;
            r.frame = Detail::PrepareFrame(s, pose);
            r.outer = Geo::Bounds(s) + grow;
            float const ux = r.frame.ux;
            float const uy = r.frame.uy;
            auto add = [&r](float a, float b, float c) { r.plane[r.planes++] = { a, b, c }; };
            switch (s.type)
            {
                case ShapeType::Circle:
                case ShapeType::Donut:
                    break;
                case ShapeType::Cone:
                {
                    if (s.angleDeg >= 360.f || s.angleDeg > 180.f)
                        break;
                    // The sector lies behind both edge lines (outward unit normals: the edge ends turned a quarter
                    // outwards), so its growth by `grow` lies within `grow` of them.
                    float const len = std::max(s.length, 1e-6f);
                    add(-r.frame.ay / len, r.frame.ax / len, grow);
                    add(r.frame.by / len, -r.frame.bx / len, grow);
                    break;
                }
                case ShapeType::Line:
                {
                    if (s.length <= s.excludeStart || s.width <= 0.f)
                    {
                        r.skip = true;
                        break;
                    }
                    float const hw = s.width * 0.5f + grow;
                    add(ux, uy, s.length + grow);
                    add(-ux, -uy, -(s.excludeStart - grow));
                    add(-uy, ux, hw);
                    add(uy, -ux, hw);
                    break;
                }
            }
            return r;
        }

        // The span [lo, hi] of dx (relative to the anchor) the grown outline covers at row offset dy. false = none.
        inline bool RowSpan(PoseRaster const& r, float dy, float& lo, float& hi)
        {
            float const q = r.outer * r.outer - dy * dy;
            if (q < 0.f)
                return false;
            hi = std::sqrt(q);
            lo = -hi;
            for (std::uint32_t i = 0; i < r.planes; ++i)
            {
                PoseRaster::HalfPlane const& p = r.plane[i];
                float const rhs = p.c - p.b * dy;
                if (std::fabs(p.a) < 1e-6f)
                {
                    if (rhs < -1e-4f)
                        return false;
                    continue;
                }
                float const x = rhs / p.a;
                if (p.a > 0.f)
                    hi = std::min(hi, x);
                else
                    lo = std::max(lo, x);
            }
            return lo <= hi;
        }

        // Each hazard whose windows meet [now, now + T] marks the cells whose center lies within its (reduced) margin
        // plus half a cell diagonal, with the hull of its windows there. Only the cells inside each pose's grown
        // outline are tested, row by row, each with the exact signed distance in the pose's prepared frame (cost
        // scales with the hazard's area, not its bounding square). Non-finite poses are skipped. Returns the cells
        // marked.
        // Rasterize's walk over one list of hazards (the hard ones or, Stratholme X1, the soft ones), into the hulls
        // `hotFrom` / `hotTo` (one per cell).
        inline void RasterizeInto(Input const& tight, std::vector<Hazard> const& list, Ms horizonEnd, Grid const& g,
            std::vector<std::int64_t>& hotFrom, std::vector<std::int64_t>& hotTo)
        {
            float const halfDiag = g.cell * 0.70710678f;
            constexpr float kSpanSlackYd = 1e-3f;
            for (Hazard const& src : list)
            {
                if (!InRouteTime(tight, src, horizonEnd))
                    continue;
                Hazard h = src;
                h.marginYd += halfDiag;
                bool const projectile = h.timing.type == TimingType::Projectile;
                ProjectileParams const params = Detail::WidenedProjectile(h);
                // Every other timing has the same windows wherever it holds a point: their hull, once.
                std::int64_t fixedFrom = kNever;
                std::int64_t fixedTo = -kNever;
                if (!projectile)
                    Detail::ForEachWindow(h, Geo::Vec3{ h.poses.front().x, h.poses.front().y, h.poses.front().z },
                        [&](HotWindow const& w)
                    {
                        fixedFrom = std::min<std::int64_t>(fixedFrom, w.from.count());
                        fixedTo = std::max<std::int64_t>(fixedTo, w.to.count());
                    }, true);
                for (Geo::Pose const& pose : h.poses)
                {
                    if (!Finite(pose))
                        continue;
                    PoseRaster const r = PrepareRaster(h.shape, pose, h.marginYd);
                    if (r.skip)
                        continue;
                    // The cells' z runs between the bot's and the goal's: test it only when that range leaves the
                    // shape's floor tolerance.
                    float const zTolerance = h.shape.zTolerance + Geo::kEdgeEpsilon;
                    bool const checkZ = std::max(std::fabs(g.a.z - pose.z), std::fabs(g.b.z - pose.z)) > zTolerance;
                    auto first = [&g](float v, float origin, std::uint32_t n)
                    {
                        float const c = std::ceil((v - origin) / g.cell - 0.5f);
                        return static_cast<std::int64_t>(std::clamp(c, -1.f, static_cast<float>(n)));
                    };
                    auto last = [&g](float v, float origin, std::uint32_t n)
                    {
                        float const c = std::floor((v - origin) / g.cell - 0.5f);
                        return static_cast<std::int64_t>(std::clamp(c, -1.f, static_cast<float>(n)));
                    };
                    std::int64_t const iy0 = std::max<std::int64_t>(0, first(pose.y - r.outer - kSpanSlackYd, g.y0,
                        g.ny));
                    std::int64_t const iy1 = std::min<std::int64_t>(g.ny - 1, last(pose.y + r.outer + kSpanSlackYd,
                        g.y0, g.ny));
                    for (std::int64_t iy = iy0; iy <= iy1; ++iy)
                    {
                        float const cy = g.y0 + (static_cast<float>(iy) + 0.5f) * g.cell;
                        float lo = 0.f;
                        float hi = 0.f;
                        if (!RowSpan(r, cy - pose.y, lo, hi))
                            continue;
                        std::int64_t const ix0 = std::max<std::int64_t>(0, first(pose.x + lo - kSpanSlackYd, g.x0,
                            g.nx));
                        std::int64_t const ix1 = std::min<std::int64_t>(g.nx - 1, last(pose.x + hi + kSpanSlackYd,
                            g.x0, g.nx));
                        for (std::int64_t ix = ix0; ix <= ix1; ++ix)
                        {
                            std::uint32_t const idx = g.Index(static_cast<std::uint32_t>(ix),
                                static_cast<std::uint32_t>(iy));
                            float const dx = g.x0 + (static_cast<float>(ix) + 0.5f) * g.cell - pose.x;
                            float const dy = cy - pose.y;
                            if (checkZ && std::fabs(g.Center(idx).z - pose.z) > zTolerance)
                                continue;
                            if (Detail::FrameSd(h.shape, r.frame, dx, dy) > h.marginYd)
                                continue;
                            if (!projectile)
                            {
                                hotFrom[idx] = std::min(hotFrom[idx], fixedFrom);
                                hotTo[idx] = std::max(hotTo[idx], fixedTo);
                                continue;
                            }
                            float const s = dx * r.frame.ux + dy * r.frame.uy;
                            for (HotWindow const& w : ProjectileHotWindows(params, s))
                            {
                                std::int64_t const from = (w.from + h.timing.from).count();
                                std::int64_t const to = (w.to + h.timing.from).count();
                                hotFrom[idx] = std::min(hotFrom[idx], from);
                                hotTo[idx] = std::max(hotTo[idx], to);
                            }
                        }
                    }
                }
            }
        }

        inline std::uint32_t Rasterize(Input const& tight, Ms horizonEnd, Grid& g)
        {
            RasterizeInto(tight, tight.hazards, horizonEnd, g, g.hotFrom, g.hotTo);
            std::uint32_t blocked = 0;
            for (std::uint32_t i = 0; i < g.Size(); ++i)
                if (g.hotFrom[i] <= g.hotTo[i])
                    ++blocked;
            return blocked;
        }

        // Stratholme X1: the soft blockers (in.softHazards) into g.softFrom / g.softTo at their own margins, and into
        // g.softCoreFrom / g.softCoreTo at none (the cells the shapes touch), widened like Seal widens the hard hulls
        // (call it after g.widen is set); the start and goal cells are never soft. Returns the soft cells; 0 (and no
        // soft grid) without soft hazards or with `cost` 0.
        inline std::uint32_t RasterizeSoft(Input const& in, Ms horizonEnd, Grid& g, std::uint32_t cost)
        {
            g.softFrom.clear();
            g.softTo.clear();
            g.softCoreFrom.clear();
            g.softCoreTo.clear();
            g.softCost = 0;
            if (in.softHazards.empty() || cost == 0)
                return 0;
            auto layer = [&](std::vector<Hazard> const& list, std::vector<std::int64_t>& from,
                std::vector<std::int64_t>& to)
            {
                from.assign(g.Size(), kNever);
                to.assign(g.Size(), -kNever);
                RasterizeInto(in, list, horizonEnd, g, from, to);
                std::uint32_t marked = 0;
                for (std::uint32_t i = 0; i < g.Size(); ++i)
                    if (from[i] <= to[i])
                    {
                        from[i] -= g.widen;
                        to[i] += g.widen;
                        ++marked;
                    }
                for (std::uint32_t i : { g.start, g.goal })
                {
                    from[i] = kNever;
                    to[i] = -kNever;
                }
                return marked;
            };
            std::uint32_t const soft = layer(in.softHazards, g.softFrom, g.softTo);
            if (!soft)
            {
                g.softFrom.clear();
                g.softTo.clear();
                return 0;
            }
            std::vector<Hazard> core = in.softHazards;
            for (Hazard& h : core)
                h.marginYd = 0.f;
            layer(core, g.softCoreFrom, g.softCoreTo);
            g.softCost = cost;
            return soft;
        }

        // Is the cell under a soft blocker at time t (ms)? `core`: under its shape itself (no margin).
        inline bool SoftAt(Grid const& g, std::uint32_t idx, std::int64_t t, bool core = false)
        {
            std::vector<std::int64_t> const& from = core ? g.softCoreFrom : g.softFrom;
            std::vector<std::int64_t> const& to = core ? g.softCoreTo : g.softTo;
            return !from.empty() && t >= from[idx] && t <= to[idx];
        }

        struct Node
        {
            std::uint32_t f = 0, g = 0, idx = 0;
        };

        // Heap order (pops first: lower f, then the deeper node, larger g, then the lower cell index). True when a
        // pops after b.
        struct HeapAfter
        {
            bool operator()(Node const& a, Node const& b) const
            {
                if (a.f != b.f)
                    return a.f > b.f;
                if (a.g != b.g)
                    return a.g < b.g;
                return a.idx > b.idx;
            }
        };

        inline std::uint32_t Octile(std::int64_t x, std::int64_t y, std::int64_t gx, std::int64_t gy)
        {
            std::uint32_t const dx = static_cast<std::uint32_t>(std::abs(x - gx));
            std::uint32_t const dy = static_cast<std::uint32_t>(std::abs(y - gy));
            return kOrthoCost * std::max(dx, dy) + kDiagExtra * std::min(dx, dy);
        }

        // A cell's state at time t (ms): free unless a rejected leg marked it or t meets its hot hull widened by
        // marginMs plus a cell's travel. The start and goal cells are always free (Seal).
        inline bool FreeAt(Grid const& g, std::uint32_t idx, std::int64_t t)
        {
            return t < g.hotFrom[idx] || t > g.hotTo[idx];
        }

        // After Rasterize: widens every hot hull by g.widen and frees the start and goal cells, so FreeAt is two
        // compares.
        inline void Seal(Grid& g)
        {
            for (std::uint32_t i = 0; i < g.Size(); ++i)
                if (g.hotFrom[i] <= g.hotTo[i])
                {
                    g.hotFrom[i] -= g.widen;
                    g.hotTo[i] += g.widen;
                }
            for (std::uint32_t i : { g.start, g.goal })
            {
                g.hotFrom[i] = kNever;
                g.hotTo[i] = -kNever;
            }
        }

        // The longest route A* considers, in step-cost units: the box's perimeter (a walk round the whole box). Also
        // how long GoalSealed's walls must last.
        inline std::uint32_t MaxRouteCost(Grid const& g)
        {
            return 2 * (g.nx + g.ny) * kOrthoCost;
        }

        // 8-connected A*, no corner cutting, no route longer than MaxRouteCost. A cell must be free at the time the
        // bot reaches it (moveStart + walk / speed). Stratholme X1: entering a cell a soft blocker covers then adds
        // g.softCost to the cost A* ranks by (the walk, which times the cells and caps the length, does not grow).
        // Returns the cells from start to goal; empty = none within maxExpansions. `edge` (M4 §3.7) is set when an
        // empty answer ran out of cells after reaching the box's edge (the way may lie outside the box); never after
        // maxExpansions.
        inline std::vector<std::uint32_t> Search(Grid const& g, std::uint32_t maxExpansions, std::uint32_t& expansions,
            bool* edge = nullptr)
        {
            bool touched = false;
            if (edge)
                *edge = false;
            std::uint32_t const n = g.Size();
            std::uint32_t const maxCost = MaxRouteCost(g);
            std::int64_t const nx = g.nx;
            std::int64_t const ny = g.ny;
            std::int64_t const gx = g.goal % g.nx;
            std::int64_t const gy = g.goal / g.nx;
            float const msPerUnit = g.cell / static_cast<float>(kOrthoCost) * g.msPerYd;
            auto freeAt = [&](std::uint32_t idx, std::uint32_t cost)
            {
                return FreeAt(g, idx, g.moveStart + static_cast<std::int64_t>(cost * msPerUnit + 0.5f));
            };

            std::vector<std::uint32_t> best(n, std::numeric_limits<std::uint32_t>::max());
            std::vector<std::uint32_t> walk(n, 0);      // the walk part of best (best itself without soft blockers)
            std::vector<std::uint32_t> parent(n, kNoParent);
            bool const soft = !g.softFrom.empty();
            std::vector<Node> heap;
            heap.reserve(512);
            HeapAfter const after;
            best[g.start] = 0;
            heap.push_back({ Octile(g.start % g.nx, g.start / g.nx, gx, gy), 0, g.start });
            bool reached = false;
            while (!heap.empty() && expansions < maxExpansions)
            {
                std::pop_heap(heap.begin(), heap.end(), after);
                Node const cur = heap.back();
                heap.pop_back();
                if (cur.g != best[cur.idx])
                    continue;                   // a stale entry: the cell was reached cheaper since
                ++expansions;
                if (cur.idx == g.goal)
                {
                    reached = true;
                    break;
                }
                std::int64_t const cx = cur.idx % g.nx;
                std::int64_t const cy = cur.idx / g.nx;
                touched = touched || cx == 0 || cy == 0 || cx == nx - 1 || cy == ny - 1;
                for (std::int64_t dy = -1; dy <= 1; ++dy)
                {
                    std::int64_t const y = cy + dy;
                    if (y < 0 || y >= ny)
                        continue;
                    for (std::int64_t dx = -1; dx <= 1; ++dx)
                    {
                        std::int64_t const x = cx + dx;
                        if ((!dx && !dy) || x < 0 || x >= nx)
                            continue;
                        std::uint32_t const idx = static_cast<std::uint32_t>(y * nx + x);
                        bool const diag = dx && dy;
                        std::uint32_t const step = diag ? kDiagCost : kOrthoCost;
                        std::uint32_t const walked = walk[cur.idx] + step;
                        std::uint32_t const cost = cur.g + step + (soft && SoftAt(g, idx, g.moveStart
                            + static_cast<std::int64_t>(walked * msPerUnit + 0.5f)) ? g.softCost : 0);
                        if (walked > maxCost || cost >= best[idx] || !freeAt(idx, walked))
                            continue;
                        if (diag && (!freeAt(static_cast<std::uint32_t>(cy * nx + x), walked)
                            || !freeAt(static_cast<std::uint32_t>(y * nx + cx), walked)))
                            continue;
                        best[idx] = cost;
                        walk[idx] = walked;
                        parent[idx] = cur.idx;
                        heap.push_back({ cost + Octile(x, y, gx, gy), cost, idx });
                        std::push_heap(heap.begin(), heap.end(), after);
                    }
                }
            }
            std::vector<std::uint32_t> cells;
            if (!reached)
            {
                if (edge)
                    *edge = touched && heap.empty();
                return cells;
            }
            for (std::uint32_t c = g.goal; c != kNoParent; c = parent[c])
                cells.push_back(c);
            std::reverse(cells.begin(), cells.end());
            return cells;
        }

        inline bool IsWall(Grid const& g, std::uint32_t idx)
        {
            return g.hotFrom[idx] == -kNever && g.hotTo[idx] == kNever;
        }

        // M4 §3.7 (the U minefield's inner corners): the grid blocks a cell by its center plus half a diagonal, so a
        // bot standing within about a cell of a hazard's margin can find every neighbor blocked although it may walk
        // away. Each blocked cell within kStartRing cells of the start whose straight line from the bot passes
        // CheckPath at `tight` (the grid's own margins) is freed. Cells under a rejected leg stay walls. The legs are
        // validated on the oracle's path anyway. Returns the cells freed.
        inline std::uint32_t FreeAroundStart(Grid& g, Input const& tight, Geo::Vec3 const& bot)
        {
            constexpr std::int64_t kStartRing = 2;
            std::int64_t const sx = g.start % g.nx;
            std::int64_t const sy = g.start / g.nx;
            std::uint32_t freed = 0;
            for (std::int64_t y = std::max<std::int64_t>(0, sy - kStartRing);
                y <= std::min<std::int64_t>(g.ny - 1, sy + kStartRing); ++y)
                for (std::int64_t x = std::max<std::int64_t>(0, sx - kStartRing);
                    x <= std::min<std::int64_t>(g.nx - 1, sx + kStartRing); ++x)
                {
                    std::uint32_t const idx = g.Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                    if (idx == g.start || g.hotFrom[idx] > g.hotTo[idx] || IsWall(g, idx))
                        continue;
                    if (!CheckPath(tight, { bot, g.Center(idx) }, tight.now, false).ok)
                        continue;
                    g.hotFrom[idx] = kNever;
                    g.hotTo[idx] = -kNever;
                    ++freed;
                }
            return freed;
        }

        enum class Sight : std::uint8_t
        {
            Clear,          // every cell free when the bot crosses it
            Hot,            // some cell hot then (CheckPath may still pass the exact line)
            Wall            // a cell under a leg the navmesh rejected (never passes)
        };

        // Grid line of sight: the cells the segment a -> b passes through (grid traversal, each cell once) when the
        // bot crosses them, leaving a at `at` (ms; a cell is timed where the segment enters it). Cheap; CheckPath
        // confirms what it does not rule out. `soft` (Stratholme X1): counts the cells entered under a soft blocker's
        // shape (its core: no margin; the start cell is not entered; a wall ends the count).
        inline Sight LineSight(Grid const& g, Geo::Vec3 const& a, Geo::Vec3 const& b, std::int64_t at,
            std::uint32_t* soft = nullptr)
        {
            float const ax = (a.x - g.x0) / g.cell;
            float const ay = (a.y - g.y0) / g.cell;
            float const bx = (b.x - g.x0) / g.cell;
            float const by = (b.y - g.y0) / g.cell;
            std::int64_t const nx = g.nx;
            std::int64_t const ny = g.ny;
            auto clampCell = [](float v, std::int64_t n)
            {
                return std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(v)), 0, n - 1);
            };
            std::int64_t x = clampCell(ax, nx);
            std::int64_t y = clampCell(ay, ny);
            std::int64_t const ex = clampCell(bx, nx);
            std::int64_t const ey = clampCell(by, ny);
            float const dx = bx - ax;
            float const dy = by - ay;
            std::int64_t const sx = dx > 0.f ? 1 : -1;
            std::int64_t const sy = dy > 0.f ? 1 : -1;
            float const tdx = dx != 0.f ? std::fabs(1.f / dx) : Detail::kInf;
            float const tdy = dy != 0.f ? std::fabs(1.f / dy) : Detail::kInf;
            float tx = dx != 0.f ? (dx > 0.f ? (static_cast<float>(x) + 1.f - ax) : (ax - static_cast<float>(x))) * tdx
                : Detail::kInf;
            float ty = dy != 0.f ? (dy > 0.f ? (static_cast<float>(y) + 1.f - ay) : (ay - static_cast<float>(y))) * tdy
                : Detail::kInf;
            float const msPerU = Detail::Dist2(a, b) * g.msPerYd;
            float u = 0.f;
            bool hot = false;
            auto visit = [&](std::int64_t cx, std::int64_t cy) -> bool
            {
                std::uint32_t const idx = static_cast<std::uint32_t>(std::clamp<std::int64_t>(cy, 0, ny - 1) * nx
                    + std::clamp<std::int64_t>(cx, 0, nx - 1));
                if (IsWall(g, idx))
                    return false;
                std::int64_t const t = at + static_cast<std::int64_t>(u * msPerU + 0.5f);
                hot = hot || !FreeAt(g, idx, t);
                if (soft && SoftAt(g, idx, t, true))
                    ++*soft;
                return true;
            };
            std::uint32_t const maxSteps = static_cast<std::uint32_t>(nx + ny + 2);
            for (std::uint32_t step = 0; step <= maxSteps; ++step)
            {
                if (step && !visit(x, y))
                    return Sight::Wall;
                if (x == ex && y == ey)
                    break;
                if (tx == ty)
                {
                    // Through a corner exactly: both side cells count (no cutting between two blocked cells).
                    u = tx;
                    if (!visit(x + sx, y) || !visit(x, y + sy))
                        return Sight::Wall;
                    x += sx;
                    y += sy;
                    tx += tdx;
                    ty += tdy;
                }
                else if (tx < ty)
                {
                    u = tx;
                    x += sx;
                    tx += tdx;
                }
                else
                {
                    u = ty;
                    y += sy;
                    ty += tdy;
                }
                if (x < 0 || y < 0 || x >= nx || y >= ny || u > 1.f + 1e-4f)
                    break;
            }
            return hot ? Sight::Hot : Sight::Clear;
        }

        // String-pull against `pin` (the full or the reduced margins): from each point, the farthest later grid point
        // whose straight segment passes, timed from the cumulative arrival (the leg's start delay is spent before it,
        // so the walk starts on arrival). The grid line of sight scans forward to kPullLookahead points past the last
        // clear one (it is a little stricter than CheckPath: a cell is blocked by its center plus half a diagonal);
        // CheckPath then confirms the farthest clear point and extends past it through the hot ones, never to a point
        // behind a marked cell, at most kPullConfirms calls per leg. Returns the indices of the leg ends; empty = more
        // than maxLegs or a point with no leg. `softPrefix` (Stratholme X1; one per point, the soft core cells the grid
        // path entered up to it): a shortcut i -> j that enters more soft core cells than the grid path between them is
        // treated like one behind a marked cell (the next point, i + 1, is always kept); `softSlack` more are let
        // through per shortcut. The core (the shapes without
        // their margins) lets a leg cut across the margin band round a carrier's circle, never into the circle the
        // grid path kept out of, so an arc round it takes a few legs, not one per cell.
        inline std::vector<std::size_t> Pull(Input const& pin, Grid const& g, std::vector<Geo::Vec3> const& pts,
            std::uint32_t maxLegs, std::uint32_t& confirms, std::vector<std::uint32_t> const* softPrefix = nullptr,
            std::uint32_t softSlack = 0)
        {
            std::vector<std::size_t> ends;
            float const speed = Detail::Speed(pin);
            std::size_t const last = pts.size() - 1;
            std::size_t i = 0;
            Ms at = pin.now + pin.params.startDelay;
            std::vector<std::size_t> scan;
            while (i < last)
            {
                if (ends.size() >= maxLegs)
                    return {};
                // Scan: the points not behind a marked cell, and the farthest clear one.
                scan.assign(1, i + 1);
                std::size_t clear = 0;              // index in scan
                std::uint32_t misses = 0;
                for (std::size_t j = i + 2; j <= last; ++j)
                {
                    std::uint32_t segSoft = 0;
                    Sight sight = LineSight(g, pts[i], pts[j], at.count(), softPrefix ? &segSoft : nullptr);
                    if (softPrefix && sight != Sight::Wall
                        && segSoft > (*softPrefix)[j] - (*softPrefix)[i] + softSlack)
                        sight = Sight::Wall;
                    if (sight != Sight::Wall)
                        scan.push_back(j);
                    if (sight == Sight::Clear)
                    {
                        clear = scan.size() - 1;
                        misses = 0;
                    }
                    else if (++misses > kPullLookahead)
                        break;
                }
                auto passes = [&](std::size_t j)
                {
                    ++confirms;
                    return CheckPath(pin, { pts[i], pts[j] }, at - pin.params.startDelay, false).ok;
                };
                // Confirm the farthest clear point (else the nearer ones, farthest first), then extend past it through
                // the hot ones while CheckPath passes them (two misses in a row end it).
                std::size_t pass = 0;
                std::size_t from = scan.size();
                std::uint32_t tries = 0;
                for (std::size_t k = clear + 1; k-- > 0 && tries < kPullConfirms; ++tries)
                    if (passes(scan[k]))
                    {
                        pass = scan[k];
                        from = k + 1;
                        break;
                    }
                std::uint32_t fails = 0;
                for (std::size_t k = from; k < scan.size() && tries < kPullConfirms && fails < 2; ++k, ++tries)
                {
                    if (passes(scan[k]))
                    {
                        pass = scan[k];
                        fails = 0;
                    }
                    else
                        ++fails;
                }
                if (!pass)
                    return {};
                at += Detail::TravelMs(Detail::Dist3(pts[i], pts[pass]), speed);
                ends.push_back(pass);
                i = pass;
            }
            return ends;
        }

        // Marks the cells under the segment a -> b and their 8 neighbors (a wall the grid cannot see lies somewhere
        // near the segment), except within one cell of the start and goal cells.
        inline void MarkSegment(Grid& g, Geo::Vec3 const& a, Geo::Vec3 const& b)
        {
            auto near = [&g](std::uint32_t p, std::uint32_t q)
            {
                std::int64_t const dx = static_cast<std::int64_t>(p % g.nx) - static_cast<std::int64_t>(q % g.nx);
                std::int64_t const dy = static_cast<std::int64_t>(p / g.nx) - static_cast<std::int64_t>(q / g.nx);
                return std::max(std::abs(dx), std::abs(dy)) <= 1;
            };
            float const len = Detail::Dist2(a, b);
            std::uint32_t const steps = std::max<std::uint32_t>(1,
                static_cast<std::uint32_t>(std::ceil(len / (g.cell * 0.25f))));
            for (std::uint32_t k = 0; k <= steps; ++k)
            {
                float const u = static_cast<float>(k) / steps;
                std::uint32_t const c = g.CellOf(a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u);
                std::int64_t const cx = c % g.nx;
                std::int64_t const cy = c / g.nx;
                std::int64_t const yLo = std::max<std::int64_t>(0, cy - 1);
                std::int64_t const yHi = std::min<std::int64_t>(g.ny - 1, cy + 1);
                std::int64_t const xLo = std::max<std::int64_t>(0, cx - 1);
                std::int64_t const xHi = std::min<std::int64_t>(g.nx - 1, cx + 1);
                for (std::int64_t y = yLo; y <= yHi; ++y)
                    for (std::int64_t x = xLo; x <= xHi; ++x)
                    {
                        std::uint32_t const idx = g.Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                        if (!near(idx, g.start) && !near(idx, g.goal))
                        {
                            g.hotFrom[idx] = -kNever;
                            g.hotTo[idx] = kNever;
                        }
                    }
            }
        }

        // A cell blocked for the whole walk: a lasting hazard (mine, persistent zone) hot from before the bot starts
        // until at least `until`, or a cell under a rejected leg.
        inline bool StaticWall(Grid const& g, std::uint32_t idx, std::int64_t until)
        {
            return g.hotFrom[idx] <= g.moveStart && g.hotTo[idx] >= until;
        }

        // Is the goal sealed off by static walls (STAGE-M3-R-REPORT m-12)? A flood fill from the goal over the cells
        // that are not static walls, with A*'s moves (8-connected, no corner cutting), at most `limit` cells: true
        // when it runs out of cells before it reaches the start (A* would expand every cell it can reach and find
        // nothing). false when it reaches the start or passes the limit (A* decides). `edge` (M4 §3.7): a sealed
        // goal's cells reach the box's edge (the walls may open outside the box).
        inline bool GoalSealed(Grid const& g, std::uint32_t limit, bool* edge = nullptr)
        {
            if (edge)
                *edge = false;
            std::uint32_t const n = g.Size();
            std::int64_t const nx = g.nx;
            std::int64_t const ny = g.ny;
            // Walls last at least as long as the longest route A* considers takes to walk (MaxRouteCost).
            float const longestYd = static_cast<float>(MaxRouteCost(g)) / kOrthoCost * g.cell;
            std::int64_t const until = g.moveStart + static_cast<std::int64_t>(longestYd * g.msPerYd) + 1;
            auto wall = [&](std::int64_t x, std::int64_t y)
            {
                return StaticWall(g, static_cast<std::uint32_t>(y * nx + x), until);
            };
            std::vector<std::uint8_t> seen(n, 0);
            std::vector<std::uint32_t> stack{ g.goal };
            seen[g.goal] = 1;
            std::uint32_t visited = 0;
            while (!stack.empty())
            {
                std::uint32_t const cur = stack.back();
                stack.pop_back();
                if (cur == g.start || ++visited > limit)
                    return false;
                std::int64_t const cx = cur % g.nx;
                std::int64_t const cy = cur / g.nx;
                if (edge && (cx == 0 || cy == 0 || cx == nx - 1 || cy == ny - 1))
                    *edge = true;
                for (std::int64_t dy = -1; dy <= 1; ++dy)
                    for (std::int64_t dx = -1; dx <= 1; ++dx)
                    {
                        std::int64_t const x = cx + dx;
                        std::int64_t const y = cy + dy;
                        if ((!dx && !dy) || x < 0 || y < 0 || x >= nx || y >= ny)
                            continue;
                        std::uint32_t const idx = static_cast<std::uint32_t>(y * nx + x);
                        if (seen[idx] || wall(x, y))
                            continue;
                        if (dx && dy && (wall(cx + dx, cy) || wall(cx, cy + dy)))
                            continue;
                        seen[idx] = 1;
                        stack.push_back(idx);
                    }
            }
            return true;
        }
    }

    // A clean way around the hazards from in.bot towards `goal` (§3.9). Outcome Move with the route: `path` is the
    // whole walked path (the oracle's legs joined), `waypoints` the legs' real ends (the last one is where the oracle
    // put the goal), `target` the last waypoint, `holdUntil` = `arriveAt`, `pathChecks` the oracle calls, `stats.pass`
    // 1 when every leg is clean at the full margin, 2 when one needs the reduced margin. Outcome Safe (the bot stays,
    // empty path) when there is none.
    //
    // Grid: the box around bot and goal padded by padYd, cells of cellYd (grown to fit maxCells). Blocked: a cell
    // whose center lies within a hazard's reduced margin (drift + tightMarginYd) plus half a cell diagonal while the
    // hazard is hot at the time the bot gets there; only hazards whose windows meet [now, now + T], T = startDelay +
    // 2 x |goal - bot| / speed + marginMs. Regions and the bot's own carried shapes do not count (routes are for
    // following); other players' carried shapes at their live spots (Input::softHazards, Stratholme X1) are soft
    // blockers: a cost per covered cell (RouteParams::softCost), never a block; stats.softCrossed counts the covered
    // cells the route still enters. The start and goal cells are never blocked. Search: 8-connected A*, no corner
    // cutting, octile heuristic, at most maxExpansions per search.
    // Legs: string-pulled at the full margin when the route fits maxLegs at it, else at the reduced one; each goes
    // through oracle.Find and CheckPath on the real path (full margin, else reduced; never grace). A leg the oracle
    // rejects (no path, or a path that fails the check) marks the cells around its straight segment and the search runs
    // once more (legs it repeats reuse their oracle path). Oracle calls are capped by in.params.maxPathChecks.
    // No route in the box although the search (or the sealed-off goal's cells) reached its edge, while hazards cross
    // that edge: the box grows once round the obstacle they form, at most growYd per side (RouteDetail::GrowBox,
    // M4 §3.7), and the search runs again in it; stats.grown = 1. A real enclosure never grows.
    // Deterministic for a given input and oracle.
    inline Plan Route(Input const& in, Geo::Vec3 const& goal, PathOracle& oracle, RouteParams const& rp,
        RouteStats* stats = nullptr)
    {
        RouteStats local;
        RouteStats& st = stats ? *stats : local;
        st = RouteStats();

        Plan plan;
        plan.target = in.bot;
        plan.arriveAt = in.now;
        plan.holdUntil = in.now;

        // Absurd input (STAGE-M3-R-REPORT m-10): a non-finite or far goal, bot, speed or grid parameter would loop
        // the cell growth or cast NaN to a cell index. No route.
        if (!RouteDetail::Finite(in.bot) || !RouteDetail::Finite(goal) || !std::isfinite(in.speed)
            || !std::isfinite(rp.padYd) || !std::isfinite(rp.cellYd)
            || Detail::Dist2(in.bot, goal) > RouteDetail::kMaxRouteYd || rp.padYd > RouteDetail::kMaxRouteYd)
            return plan;

        Input tight;
        Detail::ReducedMargins(in, tight);
        float const speed = Detail::Speed(in);
        float const straightYd = Detail::Dist2(in.bot, goal);
        Ms const moveStart = in.now + in.params.startDelay;
        // The hazards that matter: those hot before the bot could walk twice the way, or the way plus a detour of
        // the box padding out and back (STAGE-M3-R-REPORT q-2: a detour round a wall near the goal).
        auto horizonFor = [&](float detourYd)
        {
            float const walkYd = std::max(2.f * straightYd, straightYd + 2.f * std::max(0.f, detourYd));
            return in.now + in.params.startDelay + Detail::TravelMs(walkYd, speed) + in.params.marginMs;
        };

        // Oracle paths by (from, to), kept across the retries: a leg a later search repeats costs no second call.
        struct Known
        {
            Geo::Vec3 from, to;
            bool found = false;
            std::vector<Geo::Vec3> path;
        };
        std::vector<Known> known;
        auto same = [](Geo::Vec3 const& a, Geo::Vec3 const& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };

        RouteDetail::Box box = RouteDetail::FollowBox(in.bot, goal, rp.padYd);
        float detourYd = std::max(0.f, rp.padYd);
        for (std::uint32_t boxPass = 0; boxPass < 2; ++boxPass)
        {
            if (boxPass)
            {
                // M4 §3.7 (the U minefield): no route inside the first box, and the search (or the sealed goal's
                // cells) reached its edge. When hazards cross the edge, the way round them may lie outside it (a bot
                // deep in a U must first walk back out of its mouth, away from the goal): grow the box round that
                // obstacle and search once more.
                float const growYd = std::max(0.f, rp.growYd);
                float const ring = 2.f * std::max(RouteDetail::kMinCellYd, rp.cellYd) + 1.f;
                if (growYd <= 0.f || !std::isfinite(growYd)
                    || !RouteDetail::GrowBox(in, horizonFor(detourYd + growYd), ring, growYd, box))
                    return plan;
                detourYd = std::max({ std::min(in.bot.x, goal.x) - box.x0, std::min(in.bot.y, goal.y) - box.y0,
                    box.x1 - std::max(in.bot.x, goal.x), box.y1 - std::max(in.bot.y, goal.y) });
                st.grown = 1;
            }

            RouteDetail::Grid grid;
            if (!RouteDetail::MakeGrid(box, in.bot, goal, rp, grid))
                return plan;
            st.cells = grid.Size();
            st.cellYd = grid.cell;
            st.blocked = RouteDetail::Rasterize(tight, horizonFor(detourYd), grid);
            grid.moveStart = moveStart.count();
            grid.widen = in.params.marginMs.count() + Detail::TravelMs(grid.cell, speed).count();
            grid.msPerYd = 1000.f / speed;
            RouteDetail::Seal(grid);
            RouteDetail::FreeAroundStart(grid, tight, in.bot);
            st.softCells = RouteDetail::RasterizeSoft(in, horizonFor(detourYd), grid, rp.softCost);
            // A goal walled in by lasting hazards: no A* exhaustion (STAGE-M3-R-REPORT m-12). Walled in up to the box's
            // edge: the walls may open outside the box.
            bool edge = false;
            if (RouteDetail::GoalSealed(grid, std::max<std::uint32_t>(64, std::min(rp.maxExpansions, grid.Size())
                / 4), &edge))
            {
                if (!edge)
                    return plan;
                continue;
            }

            bool noWay = false;
            for (std::uint32_t attempt = 0; attempt < 2; ++attempt)
            {
                if (attempt)
                    ++st.retries;
                std::uint32_t expansions = 0;
                std::vector<std::uint32_t> const cells = RouteDetail::Search(grid, rp.maxExpansions, expansions,
                    &edge);
                st.expansions += expansions;
                if (cells.empty())
                {
                    noWay = edge;
                    break;
                }

                std::vector<Geo::Vec3> pts;
                pts.reserve(cells.size() + 1);
                pts.push_back(in.bot);
                for (std::size_t k = 1; k + 1 < cells.size(); ++k)
                    pts.push_back(grid.Center(cells[k]));
                pts.push_back(goal);

                // Stratholme X1: the soft cells the grid path enters up to each point, so the string-pull never
                // shortcuts through more of them.
                std::vector<std::uint32_t> softPrefix;
                st.softCrossed = 0;
                if (!grid.softFrom.empty() && cells.size() == pts.size())
                {
                    softPrefix.assign(pts.size(), 0);
                    float walkYd = 0.f;
                    for (std::size_t k = 1; k < pts.size(); ++k)
                    {
                        walkYd += Detail::Dist2(pts[k - 1], pts[k]);
                        std::int64_t const t = grid.moveStart + static_cast<std::int64_t>(walkYd * grid.msPerYd);
                        softPrefix[k] = softPrefix[k - 1] + (RouteDetail::SoftAt(grid, cells[k], t, true) ? 1 : 0);
                        st.softCrossed += RouteDetail::SoftAt(grid, cells[k], t) ? 1 : 0;
                    }
                }
                std::vector<std::uint32_t> const* soft = softPrefix.empty() ? nullptr : &softPrefix;

                // Legs at the full margin when the route fits maxLegs at it, else at the reduced one (the grid is
                // blocked at the reduced margin, so a narrow gap may need it).
                std::vector<std::size_t> ends = RouteDetail::Pull(in, grid, pts, rp.maxLegs, st.pullChecks, soft);
                if (ends.empty())
                    ends = RouteDetail::Pull(tight, grid, pts, rp.maxLegs, st.pullChecks, soft);
                // Stratholme X1: a path that bends round several soft blockers may need more than maxLegs to keep
                // off them. Soft never blocks: pulled again letting each leg cut kSoftPullSlack core cells, and at
                // last without the soft rule (the legs may then cross them).
                if (ends.empty() && soft)
                {
                    st.softPullDropped = 1;
                    ends = RouteDetail::Pull(in, grid, pts, rp.maxLegs, st.pullChecks, soft,
                        RouteDetail::kSoftPullSlack);
                    if (ends.empty())
                        ends = RouteDetail::Pull(tight, grid, pts, rp.maxLegs, st.pullChecks, soft,
                            RouteDetail::kSoftPullSlack);
                }
                if (ends.empty() && soft)
                {
                    st.softPullDropped = 2;
                    ends = RouteDetail::Pull(in, grid, pts, rp.maxLegs, st.pullChecks);
                    if (ends.empty())
                        ends = RouteDetail::Pull(tight, grid, pts, rp.maxLegs, st.pullChecks);
                }
                st.legs = static_cast<std::uint32_t>(ends.size());
                if (ends.empty())
                    return plan;

                std::vector<Geo::Vec3> full{ in.bot };
                std::vector<Geo::Vec3> waypoints;
                Geo::Vec3 from = in.bot;
                Ms at = moveStart;
                bool allFull = true;
                bool rejected = false;
                std::size_t legStart = 0;
                for (std::size_t end : ends)
                {
                    auto hit = std::find_if(known.begin(), known.end(), [&](Known const& k)
                    {
                        return same(k.from, from) && same(k.to, pts[end]);
                    });
                    if (hit == known.end())
                    {
                        if (st.oracleCalls >= in.params.maxPathChecks)
                            return plan;
                        ++st.oracleCalls;
                        Known k{ from, pts[end], false, {} };
                        k.found = oracle.Find(from, pts[end], k.path) && !k.path.empty();
                        if (k.found)
                            Detail::AnchorPath(from, k.path);
                        hit = known.insert(known.end(), std::move(k));
                    }
                    std::vector<Geo::Vec3> const& leg = hit->path;
                    bool ok = hit->found;
                    if (ok)
                    {
                        Ms const legStartAt = at - in.params.startDelay;
                        if (!CheckPath(in, leg, legStartAt, false).ok)
                        {
                            allFull = false;
                            ok = CheckPath(tight, leg, legStartAt, false).ok;
                        }
                    }
                    if (!ok)
                    {
                        RouteDetail::MarkSegment(grid, pts[legStart], pts[end]);
                        rejected = true;
                        break;
                    }
                    full.insert(full.end(), leg.begin() + 1, leg.end());
                    at += Detail::TravelMs(Detail::PathLength(leg), speed);
                    from = leg.back();
                    waypoints.push_back(from);
                    legStart = end;
                }
                if (rejected)
                    continue;

                st.found = true;
                plan.outcome = Outcome::Move;
                plan.path = std::move(full);
                plan.target = waypoints.back();
                plan.arriveAt = at;
                plan.holdUntil = at;
                plan.pathChecks = st.oracleCalls;
                plan.stats.pass = allFull ? 1 : 2;
                plan.answers = Detail::Answers(in, plan.path);
                plan.waypoints = std::move(waypoints);
                return plan;
            }
            // The search ran out of cells inside the box (a real enclosure), hit maxExpansions, or both searches
            // ended in a leg the navmesh rejected: a bigger box would not help.
            if (!noWay)
                return plan;
        }
        return plan;
    }

    // Stratholme X1: does `path`, walked from `start` as CheckPath times it, enter a soft blocker (Input::softHazards)
    // while it is hot? Leaving one the path starts in does not count (CheckPath's start exemption). The brain asks it
    // of a solve's Move (a straight crossing past a walking carrier) before it asks for a route round the carriers.
    inline bool SoftCrossed(Input const& in, std::vector<Geo::Vec3> const& path, Ms start)
    {
        if (in.softHazards.empty() || path.size() < 2)
            return false;
        Input probe;
        probe.bot = path.front();
        probe.speed = in.speed;
        probe.now = in.now;
        probe.params = in.params;
        probe.hazards = in.softHazards;
        return !CheckPath(probe, path, start, false).ok;
    }

    // Goals RegionRoute tries per call (each one Route search): one, so one call (one budget unit) never costs more
    // than one Route (STAGE-M4-R-REPORT m-10: two Routes of the snake + pillar bench took 1.5 ms on the host, over
    // the 1,000 µs route budget). The caller tries the next goal on a later step (`firstGoal`, RouteStats::moreGoals),
    // up to kRegionRouteMaxGoals in all.
    constexpr std::uint32_t kRegionRouteGoals = 1;
    constexpr std::uint32_t kRegionRouteMaxGoals = 2;

    // M4 WP-Q (G2, the Alexei Assembly): a way round the hazards into a hard region the bot cannot reach straight.
    // Solve validates only the oracle's path, which knows walls, not hazards, so a member whose cluster lies past a
    // lasting Zone (the Out of bounds circle on the group) that is hot by the time the straight line crosses it gets
    // a Fallback and stands where it is until the judge. RegionRoute runs Route towards a spot that passes TargetOk:
    // the candidates (Candidates, nearest first) that pass TargetOk at their straight arrival, the `firstGoal`-th of
    // them and on (kRegionRouteGoals per call), and keeps the first route whose end passes TargetOk at the route's
    // own arrival (the late rule of the regions included). Outcome Move with the route (`waypoints`, `path`,
    // `target`; answers and holdUntil as Solve gives them, so the bot holds the region for its window); Safe (the bot
    // stays) when the bot already passes TargetOk, no live hard region is unmet, or no route works; then
    // stats->moreGoals says whether a goal below kRegionRouteMaxGoals is left (stats->nextGoal). Hazards only steer
    // the route (Route ignores regions); the goal's checks bring the regions in. Deterministic for a given input and
    // oracle.
    inline Plan RegionRoute(Input const& in, PathOracle& oracle, RouteParams const& rp, RouteStats* stats = nullptr,
        std::uint32_t firstGoal = 0)
    {
        RouteStats local;
        RouteStats& st = stats ? *stats : local;
        st = RouteStats();
        Plan none;
        none.target = in.bot;
        none.arriveAt = in.now;
        none.holdUntil = in.now;
        Ms const horizonEnd = in.now + in.params.horizon;
        if (firstGoal >= kRegionRouteMaxGoals)
            return none;
        if (Detail::Frozen(in) || TargetOk(in, in.bot, in.now, horizonEnd))
            return none;
        bool unmet = false;
        for (Region const& r : in.regions)
            unmet = unmet || (r.hard && Detail::RegionLive(in, r) && !Detail::InsideRegion(r, in.bot));
        if (!unmet)
            return none;
        float const speed = Detail::Speed(in);
        Ms const moveStart = in.now + in.params.startDelay;
        std::uint32_t tried = 0;
        std::uint32_t eligible = 0;
        std::vector<Geo::Vec3> const cands = Candidates(in);
        for (std::size_t i = 1; i < cands.size(); ++i)
        {
            Geo::Vec3 const& goal = cands[i];
            if (!TargetOk(in, goal, moveStart + Detail::TravelMs(Detail::Dist3(in.bot, goal), speed), horizonEnd))
                continue;
            if (eligible++ < firstGoal)
                continue;
            if (tried >= kRegionRouteGoals)
            {
                // One more goal exists: the caller may try it on a later step.
                st.moreGoals = firstGoal + tried < kRegionRouteMaxGoals;
                st.nextGoal = firstGoal + tried;
                break;
            }
            ++tried;
            RouteStats rs;
            Plan plan = Route(in, goal, oracle, rp, &rs);
            st.cells = rs.cells;
            st.blocked = rs.blocked;
            st.expansions += rs.expansions;
            st.legs = rs.legs;
            st.oracleCalls += rs.oracleCalls;
            st.retries += rs.retries;
            st.pullChecks += rs.pullChecks;
            st.cellYd = rs.cellYd;
            st.grown = std::max(st.grown, rs.grown);
            if (!rs.found || plan.outcome != Outcome::Move || plan.waypoints.empty()
                || !TargetOk(in, plan.target, plan.arriveAt, std::max(plan.arriveAt, horizonEnd)))
                continue;
            st.found = true;
            plan.answers = Detail::Answers(in, plan.path);
            plan.holdUntil = Detail::HoldUntil(in, plan.answers, plan.arriveAt, plan.target);
            return plan;
        }
        return none;
    }

    // Where stock following can take over a route (§3.8, STAGE-M3-R-REPORT m-9): the index in route.waypoints of
    // the first waypoint from which the straight line to `goal` passes CheckPath at the full margin, timed from the
    // arrival there; the last waypoint when no earlier one does (0 for a route with none). `pathIndex` gets that
    // waypoint's corner in route.path. At most waypoints - 1 checks.
    inline std::size_t RouteHandoff(Input const& in, Plan const& route, Geo::Vec3 const& goal,
        std::size_t* pathIndex = nullptr)
    {
        constexpr float kMatchYd = 0.01f;
        if (pathIndex)
            *pathIndex = route.path.empty() ? 0 : route.path.size() - 1;
        if (route.waypoints.empty() || route.path.empty())
            return 0;
        float const speed = Detail::Speed(in);
        Ms at = in.now + in.params.startDelay;
        std::size_t k = 0;
        for (std::size_t j = 0; j + 1 < route.waypoints.size(); ++j)
        {
            Geo::Vec3 const& w = route.waypoints[j];
            while (k < route.path.size() && Detail::Dist3(route.path[k], w) > kMatchYd)
            {
                ++k;
                if (k < route.path.size())
                    at += Detail::TravelMs(Detail::Dist3(route.path[k - 1], route.path[k]), speed);
            }
            if (k >= route.path.size())
                break;
            if (CheckPath(in, { w, goal }, at - in.params.startDelay, false).ok)
            {
                if (pathIndex)
                    *pathIndex = k;
                return j;
            }
        }
        return route.waypoints.size() - 1;
    }

    // A found route cut where stock follow takes over (§3.8): at RouteHandoff's waypoint, with `target`, `path` and
    // `arriveAt` re-timed to it and `holdUntil` = `arriveAt` (the brain's TrimRoute; it also drops the answers that
    // are not hazards).
    inline void TrimToHandoff(Input const& in, Plan& plan, Geo::Vec3 const& goal)
    {
        std::size_t cut = 0;
        std::size_t const keep = RouteHandoff(in, plan, goal, &cut) + 1;
        if (keep < plan.waypoints.size() && cut + 1 < plan.path.size())
        {
            plan.waypoints.resize(keep);
            plan.path.resize(cut + 1);
            plan.target = plan.waypoints.back();
            plan.arriveAt = in.now + in.params.startDelay + Detail::TravelMs(Detail::PathLength(plan.path),
                Detail::Speed(in));
        }
        plan.holdUntil = plan.arriveAt;
    }

    // May a bot walking a route go on from `pos` to the next waypoint `next` now (§3.8 waypoint mode,
    // STAGE-M3-R-REPORT M-4)? The move action paths from where the bot stands, not from the waypoint the leg was
    // validated from, so that line must pass CheckPath itself, timed from `at` with the bot already moving. `check`
    // is the input at the margin the route was found at (full, or ReducedMargins for a pass-2 route): the shortcut
    // never cuts deeper than the legs did. A bot on its waypoint passes whenever the validated leg does.
    inline bool RouteAdvanceOk(Input const& check, Geo::Vec3 const& pos, Geo::Vec3 const& next, Ms at)
    {
        if (!RouteDetail::Finite(pos) || !RouteDetail::Finite(next))
            return false;
        return CheckPath(check, { pos, next }, at - check.params.startDelay, false).ok;
    }

    // Route-loop counter (M4 §3.7, the U minefield): a route search within kRouteLoopWindow of a route's handoff to
    // stock follow, for a goal within kRouteLoopGoalYd of that route's goal (the goal is the end of the follow motion,
    // so it moves with the leader), is a loop: found, handed off, re-crossed by the follow, routed again.
    constexpr Ms kRouteLoopWindow{ 5000 };
    constexpr float kRouteLoopGoalYd = 10.f;

    // handoffAt 0 = no handoff yet.
    inline bool IsRouteLoop(Ms handoffAt, Geo::Vec3 const& handoffGoal, Ms now, Geo::Vec3 const& goal)
    {
        if (handoffAt <= Ms(0) || now < handoffAt || now - handoffAt > kRouteLoopWindow)
            return false;
        return Geo::Dist2d(handoffGoal.x, handoffGoal.y, goal.x, goal.y) <= kRouteLoopGoalYd;
    }
}

#endif
