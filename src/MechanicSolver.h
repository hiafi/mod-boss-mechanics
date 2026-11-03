/*
 * mod-boss-mechanics: the bot solver (dungeon-mechanics STAGE-M2-HANDOFF §3.9).
 *
 * Pure header: no AzerothCore or playerbots includes (tests/solver_test.cpp compiles it with the host compiler). The
 * view (MechanicBotView) turns the registry into a Solver::Input for one bot; the brain (MechanicBotBrain) calls
 * Solve, PointSafe, LeaveBy and CheckPath; the lock calls CastAllowed. Bots use the registry's exact hitboxes (the
 * Shape and poses here), never the markers.
 *
 * Path validation checks every leg of the walked path, densified, against each hazard at the time the bot is there,
 * not only the corners or the end point; the target is then validated against the same safety band the brain
 * triggers on, so a bot never retreats into a spot it would immediately flee again. Both rules are adapted from
 * mod-dungeon-clear (Jared Wright, AGPL-3.0-or-later), Util/DcHazard.h, commit 7f67cb5.
 *
 * Times are absolute GameTime milliseconds. Distances are yards; z only matters through a shape's zTolerance.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_SOLVER_H
#define MOD_BOSS_MECHANICS_MECHANIC_SOLVER_H

#include "MechanicGeometry.h"
#include "MechanicMath.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace BossMechanics::Solver
{
    using Ms = std::chrono::milliseconds;                  // absolute GameTime ms
    constexpr Ms kForever{ std::numeric_limits<std::int64_t>::max() / 4 };

    // How a hazard is hot over time (see the Make* helpers below for what each type means).
    enum class TimingType : std::uint8_t
    {
        Instant,        // [from, to]: resolves at from, held live to `to` (resolveAt + PostResolveHoldMs)
        Interval,       // [from, to]: persistent zone, tracked projectile; to may be kForever
        Projectile,     // computed projectile launched at `from`: per point, ProjectileHotWindows(projectile, s)
        MineArming,     // unarmed mine: detonates on anyone inside at `from` (armAt) or, when the arming update is
                        // late, at any moment up to `strictTo`; armed after, up to `to`
        MineArmed       // armed mine: [from, to], crossing allowed under `grace`
    };

    struct Timing
    {
        TimingType type = TimingType::Instant;
        Ms from{ 0 }, to{ 0 };
        ProjectileParams projectile;
        Ms grace{ 0 };
        Ms strictTo{ 0 };       // MineArming: the no-grace window is [from, max(from, strictTo)]
    };

    struct Hazard
    {
        std::uint32_t id = 0;
        Shape shape;
        std::vector<Geo::Pose> poses;       // one, or three for a turning anchor (facing -slack, 0, +slack)
        float marginYd = 0.f;               // SafetyMarginYd + drift of a moving anchor
        Timing timing;
    };

    struct LaneFrame
    {
        Frame frame;
        std::vector<LaneRect> rects, freeLanes;
    };

    struct Weights
    {
        float path = 1.f, role = 2.f, edge = 1.5f, crowd = 1.f, slackPerMs = 0.01f, stay = -1.f;
    };

    struct RoleBand
    {
        bool present = false;
        Geo::Vec3 anchor;
        float minYd = 0.f, maxYd = 0.f;
    };

    struct Params
    {
        Ms startDelay{ 150 }, marginMs{ 300 }, horizon{ 10000 }, graceMargin{ 250 }, holdCap{ 3000 };
        float gridStepYd = 2.f;
        std::uint32_t maxCandidates = 400, maxPathChecks = 8;
        Weights w;
    };

    struct Input
    {
        Geo::Vec3 bot;
        float speed = 7.f;                  // yd/s
        Ms now{ 0 };
        std::vector<Hazard> hazards;
        std::vector<LaneFrame> lanes;
        RoleBand role;
        std::vector<Geo::Vec3> crowd;       // other bots' targets
        Params params;
        // The bot's spot is safe but its current motion (follow, chase) is not: solve anyway, without the bot's own
        // spot as a candidate, so the role anchor (the motion's end) pulls it around the hazard.
        bool reroute = false;
    };

    // Walkable path from `from` to `to`: `out` gets the corner points, from the start (or its first corner) to the
    // end with its real z. false = no complete path (the candidate is skipped).
    class PathOracle
    {
    public:
        virtual ~PathOracle() = default;
        virtual bool Find(Geo::Vec3 const& from, Geo::Vec3 const& to, std::vector<Geo::Vec3>& out) = 0;
    };

    enum class Outcome : std::uint8_t
    {
        Safe,           // the bot's spot is safe over the horizon
        Move,           // a clean path to a safe target
        GraceMove,      // as Move, but crossing armed mines under their grace
        Fallback,       // nothing clean: the least bad target, by the oracle's path
        NoPath          // nothing clean and no oracle path: straight to the nearest exit
    };

    struct Plan
    {
        Outcome outcome = Outcome::Safe;
        Geo::Vec3 target;
        std::vector<Geo::Vec3> path;
        Ms arriveAt{ 0 };
        Ms holdUntil{ 0 };
        float score = 0.f;
        std::vector<std::uint32_t> answers;
        std::uint32_t candidates = 0, pathChecks = 0;
    };

    struct PathCheck
    {
        bool ok = false;
        Ms arriveAt{ 0 };
        Ms timeInsideHot{ 0 };
        std::uint32_t firstHit = 0;
    };

    // --------------------------------------------------------------------------------------------------------------
    // Timing helpers (what the view builds)
    // --------------------------------------------------------------------------------------------------------------

    // Plain Zone: hot from resolveAt, held live for `hold` (PostResolveHoldMs) because it resolves on a later tick.
    inline Timing MakeInstant(Ms resolveAt, Ms hold)
    {
        Timing t;
        t.type = TimingType::Instant;
        t.from = resolveAt;
        t.to = resolveAt + std::max(Ms(0), hold);
        return t;
    }

    // Persistent zone or tracked projectile: hot over [from, to].
    inline Timing MakeInterval(Ms from, Ms to)
    {
        Timing t;
        t.type = TimingType::Interval;
        t.from = from;
        t.to = to;
        return t;
    }

    // Computed projectile launched at resolveAt.
    inline Timing MakeProjectile(ProjectileParams const& params, Ms resolveAt)
    {
        Timing t;
        t.type = TimingType::Projectile;
        t.from = resolveAt;
        t.projectile = params;
        t.to = resolveAt + ProjectileDuration(params);
        return t;
    }

    // Unarmed mine: anyone inside at armAt detonates it; afterwards it is an armed mine up to expireAt.
    inline Timing MakeMineArming(Ms armAt, Ms expireAt, Ms grace)
    {
        Timing t;
        t.type = TimingType::MineArming;
        t.from = armAt;
        t.to = expireAt;
        t.grace = grace;
        return t;
    }

    // Armed mine.
    inline Timing MakeMineArmed(Ms armAt, Ms expireAt, Ms grace)
    {
        Timing t;
        t.type = TimingType::MineArmed;
        t.from = armAt;
        t.to = expireAt;
        t.grace = grace;
        return t;
    }

    // --------------------------------------------------------------------------------------------------------------
    // Internals
    // --------------------------------------------------------------------------------------------------------------
    namespace Detail
    {
        constexpr float kInf = std::numeric_limits<float>::infinity();
        constexpr float kStepYd = 0.5f;             // path densification and candidate dedup
        constexpr float kExitPushYd = 0.5f;         // exits sit this far beyond marginYd
        constexpr float kArriveSlackMs = 400.f;     // slack term: penalise arriving closer than this to a window
        constexpr float kEdgeComfortYd = 2.f;
        constexpr float kCrowdComfortYd = 2.5f;
        constexpr float kNoPendingReachYd = 20.f;
        constexpr float kMinReachYd = 3.f;
        constexpr float kFreeLaneStepYd = 2.f;
        constexpr std::array<float, 6> kRingRadii{ 3.f, 6.f, 9.f, 12.f, 16.f, 20.f };
        constexpr std::uint32_t kRingBearings = 16;
        constexpr std::uint32_t kFallbackScans = 32;     // fallback: straight-line time-inside scans per solve

        inline float Speed(Input const& in)
        {
            return std::max(0.1f, in.speed);
        }

        inline Ms TravelMs(float yd, float speed)
        {
            return Ms(static_cast<std::int64_t>(std::lround(yd / std::max(0.1f, speed) * 1000.f)));
        }

        inline float Dist2(Geo::Vec3 const& a, Geo::Vec3 const& b)
        {
            return Geo::Dist2d(a.x, a.y, b.x, b.y);
        }

        inline float Dist3(Geo::Vec3 const& a, Geo::Vec3 const& b)
        {
            return std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z));
        }

        inline float SegmentDistance(float px, float py, float ax, float ay, float bx, float by)
        {
            float const vx = bx - ax;
            float const vy = by - ay;
            float const l2 = vx * vx + vy * vy;
            float u = l2 > 0.f ? ((px - ax) * vx + (py - ay) * vy) / l2 : 0.f;
            u = std::clamp(u, 0.f, 1.f);
            return std::hypot(px - (ax + u * vx), py - (ay + u * vy));
        }

        inline float PathLength(std::vector<Geo::Vec3> const& path)
        {
            float len = 0.f;
            for (std::size_t i = 1; i < path.size(); ++i)
                len += Dist3(path[i - 1], path[i]);
            return len;
        }

        // Projectile params as the solver sees them: the body is widened by the hazard's margin.
        inline ProjectileParams WidenedProjectile(Hazard const& h)
        {
            ProjectileParams p = h.timing.projectile;
            p.halfLength += h.marginYd;
            return p;
        }

        // Latest moment the hazard is hot anywhere (window end, before marginMs).
        inline Ms HazardEnd(Hazard const& h)
        {
            switch (h.timing.type)
            {
                case TimingType::Projectile:
                    return h.timing.from + ProjectileDuration(WidenedProjectile(h));
                default:
                    return std::max(h.timing.from, h.timing.to);
            }
        }

        // Persistent and armed hazards: their hold is capped by holdCap.
        inline bool IsLasting(Hazard const& h)
        {
            return h.timing.type == TimingType::Interval || h.timing.type == TimingType::MineArming
                || h.timing.type == TimingType::MineArmed;
        }

        inline bool Meets(HotWindow const& w, Ms widen, Ms lo, Ms hi)
        {
            return w.from - widen <= hi && w.to + widen >= lo;
        }
    }

    // --------------------------------------------------------------------------------------------------------------
    // Geometry
    // --------------------------------------------------------------------------------------------------------------

    // Signed horizontal distance from p to the shape's boundary: < 0 inside, > 0 outside. +inf when p is on another
    // floor (|dz| > zTolerance).
    inline float SignedDistance(Shape const& shape, Geo::Pose const& pose, Geo::Vec3 const& p)
    {
        if (std::fabs(p.z - pose.z) > shape.zTolerance + Geo::kEdgeEpsilon)
            return Detail::kInf;

        float const dx = p.x - pose.x;
        float const dy = p.y - pose.y;
        float const d = std::hypot(dx, dy);

        switch (shape.type)
        {
            case ShapeType::Circle:
                return d - shape.radius;
            case ShapeType::Donut:
            {
                if (d >= shape.radius)
                    return d - shape.radius;
                if (d <= shape.inner)
                    return shape.inner - d;
                return -std::min(d - shape.inner, shape.radius - d);
            }
            case ShapeType::Cone:
            {
                float const len = shape.length;
                if (shape.angleDeg >= 360.f)
                    return d - len;
                float const f = Geo::ShapeFacing(shape, pose);
                float const half = Geo::DegToRad(shape.angleDeg) * 0.5f;
                float const ax = std::cos(f + half) * len;
                float const ay = std::sin(f + half) * len;
                float const bx = std::cos(f - half) * len;
                float const by = std::sin(f - half) * len;
                float const edges = std::min(Detail::SegmentDistance(dx, dy, 0.f, 0.f, ax, ay),
                    Detail::SegmentDistance(dx, dy, 0.f, 0.f, bx, by));
                bool const withinAngle = d < 1e-6f || std::fabs(Geo::WrapPi(std::atan2(dy, dx) - f)) <= half;
                if (withinAngle && d <= len)
                    return -std::min(len - d, edges);
                if (withinAngle)
                    return std::min(edges, d - len);
                return edges;
            }
            case ShapeType::Line:
            {
                if (shape.length <= shape.excludeStart || shape.width <= 0.f)
                    return Detail::kInf;
                Geo::LineCoords const c = Geo::ToLineCoords(shape, pose, p);
                float const cs = (shape.excludeStart + shape.length) * 0.5f;
                float const hs = (shape.length - shape.excludeStart) * 0.5f;
                float const qs = std::fabs(c.s - cs) - hs;
                float const qt = std::fabs(c.t) - shape.width * 0.5f;
                float const outside = std::hypot(std::max(qs, 0.f), std::max(qt, 0.f));
                return outside + std::min(std::max(qs, qt), 0.f);
            }
        }
        return Detail::kInf;
    }

    namespace Detail
    {
        // Does any pose of h put p within marginYd? Returns the smallest signed distance (minus margin when inside).
        inline bool Within(Hazard const& h, Geo::Vec3 const& p, float* sdOut = nullptr)
        {
            float const reach = Geo::Bounds(h.shape) + h.marginYd;
            float best = kInf;
            for (Geo::Pose const& pose : h.poses)
            {
                float const dx = p.x - pose.x;
                float const dy = p.y - pose.y;
                if (dx * dx + dy * dy > (reach + 0.01f) * (reach + 0.01f))
                    continue;
                best = std::min(best, SignedDistance(h.shape, pose, p));
            }
            if (sdOut)
                *sdOut = best;
            return best <= h.marginYd;
        }

        // Calls f(HotWindow) for every window of h at p (absolute times, not widened). Nothing when p is not within
        // any pose (plus margin). A projectile gives the windows of every pose that holds p, each with its own s.
        // knownWithin: the caller already checked Within(h, p) (skips the repeat for non-projectiles).
        template <class F>
        void ForEachWindow(Hazard const& h, Geo::Vec3 const& p, F&& f, bool knownWithin = false)
        {
            Timing const& t = h.timing;
            if (t.type != TimingType::Projectile)
            {
                if (!knownWithin && !Within(h, p))
                    return;
                switch (t.type)
                {
                    case TimingType::Instant:
                    case TimingType::Interval:
                        f(HotWindow{ t.from, std::max(t.from, t.to), 0, false });
                        return;
                    case TimingType::MineArming:
                        f(HotWindow{ t.from, std::max(t.from, t.strictTo), 0, false });
                        if (t.to > t.from)
                            f(HotWindow{ t.from, t.to, 0, true });
                        return;
                    case TimingType::MineArmed:
                        f(HotWindow{ t.from, std::max(t.from, t.to), 0, true });
                        return;
                    default:
                        return;
                }
            }

            ProjectileParams const params = WidenedProjectile(h);
            float const reach = Geo::Bounds(h.shape) + h.marginYd;
            for (Geo::Pose const& pose : h.poses)
            {
                float const dx = p.x - pose.x;
                float const dy = p.y - pose.y;
                if (dx * dx + dy * dy > (reach + 0.01f) * (reach + 0.01f))
                    continue;
                if (SignedDistance(h.shape, pose, p) > h.marginYd)
                    continue;
                float const s = Geo::ToLineCoords(h.shape, pose, p).s;
                for (HotWindow w : ProjectileHotWindows(params, s))
                {
                    w.from += t.from;
                    w.to += t.from;
                    f(w);
                }
            }
        }

        // Is h hot at p at some moment of [lo, hi] (windows widened by `widen`)? Sets *grace when every meeting window
        // allows a grace crossing, *openSince to the earliest meeting window's start.
        inline bool HotOver(Hazard const& h, Geo::Vec3 const& p, Ms lo, Ms hi, Ms widen, bool* graceOnly = nullptr,
            Ms* openSince = nullptr, bool knownWithin = false)
        {
            bool hot = false;
            bool onlyGrace = true;
            Ms since = kForever;
            ForEachWindow(h, p, [&](HotWindow const& w)
            {
                if (!Meets(w, widen, lo, hi))
                    return;
                hot = true;
                onlyGrace = onlyGrace && w.graceAllowed;
                since = std::min(since, w.from);
            }, knownWithin);
            if (graceOnly)
                *graceOnly = hot && onlyGrace;
            if (openSince)
                *openSince = since;
            return hot;
        }
    }

    // The windows of h at p, absolute, not widened. Empty = never hot there.
    inline std::vector<HotWindow> WindowsAt(Hazard const& h, Geo::Vec3 const& p)
    {
        std::vector<HotWindow> out;
        Detail::ForEachWindow(h, p, [&out](HotWindow const& w) { out.push_back(w); });
        return out;
    }

    // Is p safe over [from, to]? Unsafe when some hazard holds p (within its margin) during a window widened by
    // marginMs on both ends. `firstHit` gets the first offending hazard id.
    inline bool PointSafe(Input const& in, Geo::Vec3 const& p, Ms from, Ms to, std::uint32_t* firstHit = nullptr)
    {
        for (Hazard const& h : in.hazards)
        {
            if (Detail::HotOver(h, p, from, to, in.params.marginMs))
            {
                if (firstHit)
                    *firstHit = h.id;
                return false;
            }
        }
        if (firstHit)
            *firstHit = 0;
        return true;
    }

    // The latest moment the bot may start leaving p: for each hazard holding p with a window that has not ended, the
    // window start − marginMs − startDelay − the straight-line time to its edge (plus margin). kForever if none.
    // ≤ now means "leave now" (a window is already open or the escape no longer fits).
    inline Ms LeaveBy(Input const& in, Geo::Vec3 const& p)
    {
        Ms best = kForever;
        for (Hazard const& h : in.hazards)
        {
            float sd = Detail::kInf;
            if (!Detail::Within(h, p, &sd))
                continue;
            Ms const escape = in.params.startDelay
                + Detail::TravelMs(std::max(0.f, h.marginYd - sd) + Detail::kExitPushYd, Detail::Speed(in));
            Detail::ForEachWindow(h, p, [&](HotWindow const& w)
            {
                if (w.to + in.params.marginMs < in.now)
                    return;
                best = std::min(best, w.from - in.params.marginMs - escape);
            });
        }
        return best;
    }

    // Does a cast of castTime started at now finish before leaveBy − marginMs?
    inline bool CastAllowed(Ms now, Ms castTime, Ms leaveBy, Ms marginMs)
    {
        if (leaveBy >= kForever)
            return true;
        return now + castTime <= leaveBy - marginMs;
    }

    // --------------------------------------------------------------------------------------------------------------
    // Path check
    // --------------------------------------------------------------------------------------------------------------

    namespace Detail
    {
        constexpr float kDeeperEpsYd = 0.05f;       // start-hazard exemption: "deeper" beyond this
        constexpr float kExitSlackYd = 1.f;         // start-hazard exemption: dwell cap beyond the straight exit

        // The walk behind CheckPath. measure = keep walking after a failing sample and count its dwell too (the
        // Fallback ranks candidates by their full time inside, so the walk must not stop at the first hit).
        inline PathCheck WalkPath(Input const& in, std::vector<Geo::Vec3> const& path, Ms start, bool allowGrace,
            bool measure)
        {
            PathCheck res;
            res.ok = true;
            res.arriveAt = start + in.params.startDelay;
            if (path.empty())
                return res;

            float const speed = Speed(in);
            Ms const stepMs = TravelMs(kStepYd, speed);
            Ms const widen = in.params.marginMs + stepMs;
            Ms const moveStart = start + in.params.startDelay;
            std::size_t const n = in.hazards.size();

            // Start hazards (their region holds path[0]) are exempt only while the path is leaving them: the
            // exemption ends for good at the first sample that is deeper than the one before (by more than
            // kDeeperEpsYd), at the first sample outside, and once the walk has taken longer than the straight exit
            // from path[0] plus kExitSlackYd. A leg across or into a hot region is therefore never accepted.
            struct Track
            {
                bool near = true;           // the hazard's bounds reach the current leg
                bool inStart = false;       // start-hazard exemption still running
                float lastSd = kInf;
                Ms exemptUntil{ 0 };
                Ms graceUsed{ 0 };
            };
            std::vector<Track> track(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                float sd = kInf;
                if (!Within(in.hazards[i], path.front(), &sd))
                    continue;
                track[i].inStart = true;
                track[i].lastSd = sd;
                track[i].exemptUntil = moveStart
                    + TravelMs(std::max(0.f, in.hazards[i].marginYd - sd) + kExitPushYd + kExitSlackYd, speed);
            }

            // Hazards whose bounds reach the current leg (the rest cannot hold any of its samples).
            auto nearLeg = [&](Geo::Vec3 const& a, Geo::Vec3 const& b)
            {
                for (std::size_t i = 0; i < n; ++i)
                {
                    Hazard const& h = in.hazards[i];
                    float const reach = Geo::Bounds(h.shape) + h.marginYd + 0.01f;
                    Track& tr = track[i];
                    tr.near = false;
                    for (Geo::Pose const& pose : h.poses)
                        if (SegmentDistance(pose.x, pose.y, a.x, a.y, b.x, b.y) <= reach)
                            tr.near = true;
                    if (!tr.near)
                        tr.inStart = false;
                }
            };

            auto visit = [&](Geo::Vec3 const& p, Ms lo, Ms hi, Ms dwell) -> bool
            {
                bool clean = true;
                for (std::size_t i = 0; i < n; ++i)
                {
                    Track& tr = track[i];
                    if (!tr.near)
                        continue;
                    Hazard const& h = in.hazards[i];
                    float sd = kInf;
                    if (!Within(h, p, &sd))
                    {
                        tr.inStart = false;
                        continue;
                    }
                    if (tr.inStart && (sd < tr.lastSd - kDeeperEpsYd || lo > tr.exemptUntil))
                        tr.inStart = false;
                    tr.lastSd = sd;
                    bool graceOnly = false;
                    Ms openSince{ 0 };
                    if (!HotOver(h, p, lo, hi, widen, &graceOnly, &openSince, true))
                        continue;
                    if (tr.inStart && openSince <= start)
                    {
                        res.timeInsideHot += dwell;
                        continue;
                    }
                    if (allowGrace && graceOnly)
                    {
                        res.timeInsideHot += dwell;
                        tr.graceUsed += dwell;
                        if (tr.graceUsed <= h.timing.grace - in.params.graceMargin)
                            continue;
                    }
                    else if (measure)
                        res.timeInsideHot += dwell;
                    if (res.ok)
                        res.firstHit = h.id;
                    res.ok = false;
                    clean = false;
                    if (!measure)
                        return false;
                }
                return clean || measure;
            };

            // Sample 0: standing at the start through the start delay.
            if (!visit(path.front(), start, moveStart, in.params.startDelay + stepMs))
                return res;

            float dist = 0.f;
            for (std::size_t k = 1; k < path.size(); ++k)
            {
                Geo::Vec3 const& a = path[k - 1];
                Geo::Vec3 const& b = path[k];
                float const len = Dist3(a, b);
                nearLeg(a, b);
                std::uint32_t const steps = std::max<std::uint32_t>(1,
                    static_cast<std::uint32_t>(std::ceil(len / kStepYd)));
                Ms const dwell = TravelMs(len / steps, speed);
                for (std::uint32_t j = 1; j <= steps; ++j)
                {
                    float const u = static_cast<float>(j) / steps;
                    Geo::Vec3 const p{ a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u };
                    Ms const t = moveStart + TravelMs(dist + len * u, speed);
                    if (!visit(p, t, t, dwell))
                    {
                        res.arriveAt = moveStart + TravelMs(PathLength(path), speed);
                        return res;
                    }
                }
                dist += len;
            }
            res.arriveAt = moveStart + TravelMs(dist, speed);
            return res;
        }
    }

    // Walks `path` (corner points; path[0] = where the bot stands) from `start`: the bot stays at path[0] for
    // startDelay, then moves at in.speed. Every 0.5 yd sample at time t fails the path when a hazard holds it during
    // a window widened by marginMs plus one step of time. Exceptions: (1) start hazards: a hazard that holds path[0]
    // only adds timeInsideHot, for windows already open at `start`, while the path is leaving it (never deeper,
    // within the straight exit time plus a yard; see Detail::WalkPath); (2) allowGrace: armed-mine windows add up per
    // mine, ok while each sum ≤ grace − graceMargin.
    inline PathCheck CheckPath(Input const& in, std::vector<Geo::Vec3> const& path, Ms start, bool allowGrace)
    {
        return Detail::WalkPath(in, path, start, allowGrace, false);
    }

    // --------------------------------------------------------------------------------------------------------------
    // Candidates
    // --------------------------------------------------------------------------------------------------------------
    namespace Detail
    {
        // Earliest window start at the bot's spot among windows not yet over; kForever when nothing threatens it.
        inline Ms Deadline(Input const& in)
        {
            Ms best = kForever;
            Ms const horizonEnd = in.now + in.params.horizon;
            for (Hazard const& h : in.hazards)
                ForEachWindow(h, in.bot, [&](HotWindow const& w)
                {
                    if (w.to + in.params.marginMs >= in.now && w.from - in.params.marginMs <= horizonEnd)
                        best = std::min(best, w.from);
                });
            return best;
        }

        // How far the bot can get before its deadline (+2 yd), or 20 yd when nothing is pending or it is already
        // too late; at least 3 yd.
        inline float ReachRadius(Input const& in)
        {
            Ms const deadline = Deadline(in);
            Ms const budget = deadline - in.now - in.params.startDelay;
            if (deadline >= kForever || budget <= Ms(0))
                return kNoPendingReachYd;
            float const reach = Speed(in) * SecondsOf(budget) + 2.f;
            return std::max(kMinReachYd, reach);
        }

        inline float MaxMargin(Input const& in)
        {
            float m = 0.f;
            for (Hazard const& h : in.hazards)
                m = std::max(m, h.marginYd);
            return m > 0.f ? m : 0.75f;
        }

        inline void PushPolar(std::vector<Geo::Vec3>& out, Geo::Vec3 const& c, float bearing, float r, float z)
        {
            out.push_back({ c.x + std::cos(bearing) * r, c.y + std::sin(bearing) * r, z });
        }

        // Exit points of hazard h at pose `pose` for a bot at p.
        inline void Exits(Hazard const& h, Geo::Pose const& pose, Geo::Vec3 const& p, std::vector<Geo::Vec3>& out)
        {
            Shape const& s = h.shape;
            float const m = h.marginYd + kExitPushYd;
            Geo::Vec3 const c{ pose.x, pose.y, p.z };
            float const dx = p.x - pose.x;
            float const dy = p.y - pose.y;
            float const d = std::hypot(dx, dy);
            float const bearing = d > 0.01f ? std::atan2(dy, dx) : Geo::ShapeFacing(s, pose);

            auto radial = [&](float r)
            {
                if (d > 0.01f)
                {
                    PushPolar(out, c, bearing, r, p.z);
                    return;
                }
                for (std::uint32_t i = 0; i < 8; ++i)
                    PushPolar(out, c, Geo::kTwoPi * i / 8.f, r, p.z);
            };

            switch (s.type)
            {
                case ShapeType::Circle:
                    radial(s.radius + m);
                    return;
                case ShapeType::Donut:
                    if (s.inner - m > 0.f)
                        radial(s.inner - m);
                    radial(s.radius + m);
                    return;
                case ShapeType::Cone:
                {
                    if (s.angleDeg >= 360.f)
                    {
                        radial(s.length + m);
                        return;
                    }
                    float const f = Geo::ShapeFacing(s, pose);
                    float const half = Geo::DegToRad(s.angleDeg) * 0.5f;
                    PushPolar(out, c, bearing, s.length + m, p.z);         // beyond the arc
                    PushPolar(out, c, f + Geo::kPi, m, p.z);               // behind the apex
                    for (float sign : { 1.f, -1.f })
                    {
                        float const e = f + sign * half;
                        float const ex = std::cos(e);
                        float const ey = std::sin(e);
                        float const proj = std::clamp(dx * ex + dy * ey, 0.f, s.length);
                        // Outward normal: +90 degrees from the left edge, -90 from the right one.
                        float const nx = -sign * ey;
                        float const ny = sign * ex;
                        out.push_back({ pose.x + ex * proj + nx * m, pose.y + ey * proj + ny * m, p.z });
                    }
                    return;
                }
                case ShapeType::Line:
                {
                    float const f = Geo::ShapeFacing(s, pose);
                    float const ux = std::cos(f);
                    float const uy = std::sin(f);
                    Geo::LineCoords const lc = Geo::ToLineCoords(s, pose, p);
                    auto at = [&](float ls, float lt)
                    {
                        out.push_back({ pose.x + ux * ls - uy * lt, pose.y + uy * ls + ux * lt, p.z });
                    };
                    at(lc.s, s.width * 0.5f + m);
                    at(lc.s, -(s.width * 0.5f + m));
                    at(s.excludeStart - m, lc.t);
                    at(s.length + m, lc.t);
                    return;
                }
            }
        }

        // Frame coordinates of a world point.
        inline void ToFrame(Frame const& f, Geo::Vec3 const& p, float& fx, float& fy)
        {
            float const c = std::cos(f.angle);
            float const s = std::sin(f.angle);
            float const dx = p.x - f.x;
            float const dy = p.y - f.y;
            fx = c * dx + s * dy;
            fy = -s * dx + c * dy;
        }

        // Gaps across one axis between the union of that axis's lanes: centres and points m inside each edge.
        inline std::vector<float> GapValues(std::vector<LaneRect> const& rects, Axis axis, float span, float m)
        {
            std::vector<std::pair<float, float>> covered;
            for (LaneRect const& r : rects)
                if (r.axis == axis)
                    covered.emplace_back(r.offset, r.offset + r.width);
            std::vector<float> out;
            if (covered.empty())
                return out;
            std::sort(covered.begin(), covered.end());
            float cursor = 0.f;
            auto gap = [&](float lo, float hi)
            {
                if (hi - lo <= 1e-3f)
                    return;
                out.push_back((lo + hi) * 0.5f);
                if (hi - lo > 2.f * m)
                {
                    out.push_back(lo + m);
                    out.push_back(hi - m);
                }
            };
            for (auto const& [lo, hi] : covered)
            {
                if (lo > cursor)
                    gap(cursor, std::min(lo, span));
                cursor = std::max(cursor, hi);
            }
            if (cursor < span)
                gap(cursor, span);
            return out;
        }

        // Analytic cells and free-lane points go to `out`, the grid to `bulk` (the cap fills `out` first).
        inline void FrameCandidates(Input const& in, LaneFrame const& lf, float reach, float m,
            std::vector<Geo::Vec3>& out, std::vector<Geo::Vec3>& bulk)
        {
            Frame const& f = lf.frame;
            float bfx = 0.f;
            float bfy = 0.f;
            ToFrame(f, in.bot, bfx, bfy);
            float const cfx = std::clamp(bfx, 0.f, f.sizeX);
            float const cfy = std::clamp(bfy, 0.f, f.sizeY);
            auto addTo = [&](std::vector<Geo::Vec3>& to, float fx, float fy)
            {
                Geo::Pose const w = FramePoint(f, fx, fy, 0.f);
                to.push_back({ w.x, w.y, f.z });
            };
            auto add = [&](float fx, float fy) { addTo(out, fx, fy); };

            // Analytic safe cells (X lanes leave gaps across Y; Y lanes across X).
            std::vector<float> const gy = GapValues(lf.rects, Axis::X, f.sizeY, m);
            std::vector<float> const gx = GapValues(lf.rects, Axis::Y, f.sizeX, m);
            for (float fy : gy)
                add(cfx, fy);
            for (float fx : gx)
                add(fx, cfy);
            for (float fx : gx)
                for (float fy : gy)
                    add(fx, fy);

            // Free-lane centrelines.
            for (LaneRect const& r : lf.freeLanes)
            {
                float const mid = r.offset + r.width * 0.5f;
                float const len = r.axis == Axis::X ? f.sizeX : f.sizeY;
                for (float a = 0.f; a <= len + 1e-3f; a += kFreeLaneStepYd)
                {
                    if (r.axis == Axis::X)
                        add(a, mid);
                    else
                        add(mid, a);
                }
            }

            // Grid over the frame within reach.
            float const step = std::max(0.5f, in.params.gridStepYd);
            for (float fx = step * 0.5f; fx < f.sizeX; fx += step)
                for (float fy = step * 0.5f; fy < f.sizeY; fy += step)
                    if (std::hypot(fx - bfx, fy - bfy) <= reach)
                        addTo(bulk, fx, fy);
        }
    }

    // Candidate targets: the bot's spot, exits, rings, the role ring and lane-frame points; deduplicated at 0.5 yd,
    // at most maxCandidates, returned nearest first (ties on x, y) with the bot's spot at [0]. The cap keeps the
    // targeted points (exits, role ring, analytic lane cells, free-lane centrelines) before the blanket ones (rings,
    // frame grid), each nearest first: a large frame grid must not crowd out a free lane 25 yd away.
    inline std::vector<Geo::Vec3> Candidates(Input const& in)
    {
        std::vector<Geo::Vec3> raw;
        std::vector<Geo::Vec3> bulk;
        raw.reserve(256);
        bulk.reserve(512);
        Geo::Vec3 const& bot = in.bot;
        float const reach = Detail::ReachRadius(in);

        // Exits from every hazard that holds the bot.
        for (Hazard const& h : in.hazards)
            for (Geo::Pose const& pose : h.poses)
                if (SignedDistance(h.shape, pose, bot) <= h.marginYd)
                    Detail::Exits(h, pose, bot, raw);

        // Rings around the bot.
        for (float r : Detail::kRingRadii)
        {
            if (r > std::max(reach, Detail::kMinReachYd))
                break;
            for (std::uint32_t i = 0; i < Detail::kRingBearings; ++i)
                Detail::PushPolar(bulk, bot, Geo::kTwoPi * i / Detail::kRingBearings, r, bot.z);
        }

        // Role ring.
        if (in.role.present)
        {
            float const r = (in.role.minYd + in.role.maxYd) * 0.5f;
            for (std::uint32_t i = 0; i < Detail::kRingBearings; ++i)
                Detail::PushPolar(raw, in.role.anchor, Geo::kTwoPi * i / Detail::kRingBearings, r, in.role.anchor.z);
        }

        // Lane frames (D9).
        float const m = Detail::MaxMargin(in) + Detail::kExitPushYd;
        for (LaneFrame const& lf : in.lanes)
            Detail::FrameCandidates(in, lf, reach, m, raw, bulk);

        struct Item
        {
            float d;
            Geo::Vec3 p;
        };
        auto nearer = [](Item const& a, Item const& b)
        {
            if (a.d != b.d)
                return a.d < b.d;
            if (a.p.x != b.p.x)
                return a.p.x < b.p.x;
            return a.p.y < b.p.y;
        };
        auto items = [&bot, &nearer](std::vector<Geo::Vec3> const& pts)
        {
            std::vector<Item> v;
            v.reserve(pts.size());
            for (Geo::Vec3 const& p : pts)
                v.push_back({ Detail::Dist2(bot, p), p });
            std::sort(v.begin(), v.end(), nearer);
            return v;
        };
        std::vector<Item> ordered = items(raw);
        std::vector<Item> const blanket = items(bulk);
        ordered.insert(ordered.end(), blanket.begin(), blanket.end());

        std::vector<Item> out;
        std::uint32_t const cap = std::max<std::uint32_t>(1, in.params.maxCandidates);
        out.reserve(std::min<std::size_t>(cap, ordered.size() + 1));
        out.push_back({ 0.f, bot });

        // Dedup with a 0.5 yd hash grid (3x3 neighbourhood).
        std::unordered_map<std::int64_t, std::vector<std::uint32_t>> cells;
        cells.reserve(cap * 2);
        auto key = [](std::int64_t cx, std::int64_t cy) { return (cx << 32) ^ (cy & 0xffffffffll); };
        auto cellOf = [](float v) { return static_cast<std::int64_t>(std::floor(v / Detail::kStepYd)); };
        auto insert = [&](Geo::Vec3 const& p) -> bool
        {
            std::int64_t const cx = cellOf(p.x);
            std::int64_t const cy = cellOf(p.y);
            for (std::int64_t ix = cx - 1; ix <= cx + 1; ++ix)
                for (std::int64_t iy = cy - 1; iy <= cy + 1; ++iy)
                {
                    auto it = cells.find(key(ix, iy));
                    if (it == cells.end())
                        continue;
                    for (std::uint32_t idx : it->second)
                        if (Detail::Dist2(out[idx].p, p) < Detail::kStepYd)
                            return false;
                }
            cells[key(cx, cy)].push_back(static_cast<std::uint32_t>(out.size() - 1));
            return true;
        };
        insert(bot);
        for (Item const& item : ordered)
        {
            if (out.size() >= cap)
                break;
            out.push_back(item);
            if (!insert(item.p))
                out.pop_back();
        }
        std::sort(out.begin() + 1, out.end(), nearer);
        std::vector<Geo::Vec3> pts;
        pts.reserve(out.size());
        for (Item const& item : out)
            pts.push_back(item.p);
        return pts;
    }

    // --------------------------------------------------------------------------------------------------------------
    // Solve
    // --------------------------------------------------------------------------------------------------------------
    namespace Detail
    {
        struct Scored
        {
            std::uint32_t index = 0;
            float straight = 0.f;
            float score = 0.f;
            Geo::Vec3 p;
        };

        inline bool ScoredLess(Scored const& a, Scored const& b)
        {
            if (a.score != b.score)
                return a.score < b.score;
            if (a.p.x != b.p.x)
                return a.p.x < b.p.x;
            return a.p.y < b.p.y;
        }

        inline float Score(Input const& in, Geo::Vec3 const& c, float dist, Ms arrival, Ms deadline, bool isBot)
        {
            Weights const& w = in.params.w;
            float score = w.path * dist;
            if (in.role.present)
            {
                float const d = Dist2(in.role.anchor, c);
                score += w.role * std::max({ 0.f, in.role.minYd - d, d - in.role.maxYd });
            }
            float edge = kInf;
            for (Hazard const& h : in.hazards)
            {
                if (HazardEnd(h) + in.params.marginMs < arrival)
                    continue;
                float const reach = Geo::Bounds(h.shape) + h.marginYd + kEdgeComfortYd;
                for (Geo::Pose const& pose : h.poses)
                {
                    float const dx = c.x - pose.x;
                    float const dy = c.y - pose.y;
                    if (dx * dx + dy * dy > reach * reach)
                        continue;
                    edge = std::min(edge, SignedDistance(h.shape, pose, c));
                }
            }
            if (edge < kEdgeComfortYd)
                score += w.edge * (kEdgeComfortYd - edge);
            if (!in.crowd.empty())
            {
                float nearest = kInf;
                for (Geo::Vec3 const& q : in.crowd)
                    nearest = std::min(nearest, Dist2(q, c));
                score += w.crowd * std::max(0.f, kCrowdComfortYd - nearest);
            }
            if (deadline < kForever && deadline > in.now)
            {
                float const slack = static_cast<float>((deadline - arrival).count());
                score += w.slackPerMs * std::max(0.f, kArriveSlackMs - slack);
            }
            if (isBot)
                score += w.stay;
            return score;
        }

        // Hazards that threaten the bot's spot or whose region the path crosses, still live at `now`.
        inline std::vector<std::uint32_t> Answers(Input const& in, std::vector<Geo::Vec3> const& path)
        {
            std::vector<std::uint32_t> out;
            Ms const horizonEnd = in.now + in.params.horizon;
            for (Hazard const& h : in.hazards)
            {
                if (HazardEnd(h) + in.params.marginMs < in.now)
                    continue;
                bool hit = HotOver(h, in.bot, in.now, horizonEnd, in.params.marginMs);
                for (std::size_t k = 0; !hit && k < path.size(); ++k)
                {
                    Geo::Vec3 const& a = k ? path[k - 1] : path[k];
                    Geo::Vec3 const& b = path[k];
                    float const len = Dist3(a, b);
                    std::uint32_t const steps = std::max<std::uint32_t>(1,
                        static_cast<std::uint32_t>(std::ceil(len / kStepYd)));
                    for (std::uint32_t j = 0; !hit && j <= steps; ++j)
                    {
                        float const u = static_cast<float>(j) / steps;
                        hit = Within(h, { a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u });
                    }
                }
                if (hit)
                    out.push_back(h.id);
            }
            return out;
        }

        inline Ms HoldUntil(Input const& in, std::vector<std::uint32_t> const& answers, Ms arriveAt)
        {
            Ms until = arriveAt;
            for (Hazard const& h : in.hazards)
            {
                if (std::find(answers.begin(), answers.end(), h.id) == answers.end())
                    continue;
                Ms end = HazardEnd(h);
                end = end >= kForever ? kForever : end + in.params.marginMs;
                if (IsLasting(h))
                    end = std::min(end, in.now + in.params.holdCap);
                until = std::max(until, end);
            }
            return until;
        }

        // Keeps the oracle's polyline starting at the bot.
        inline void AnchorPath(Geo::Vec3 const& from, std::vector<Geo::Vec3>& path)
        {
            if (path.empty() || Dist3(path.front(), from) > 0.1f)
                path.insert(path.begin(), from);
        }
    }

    // Picks where the bot goes (§3.9 "Solve"). Deterministic for a given input and oracle.
    inline Plan Solve(Input const& in, PathOracle& oracle)
    {
        Plan plan;
        plan.target = in.bot;
        plan.arriveAt = in.now;
        plan.holdUntil = in.now;
        Ms const horizonEnd = in.now + in.params.horizon;

        // 1. Already safe (unless re-routing a motion).
        if (!in.reroute && PointSafe(in, in.bot, in.now, horizonEnd))
            return plan;

        std::vector<Geo::Vec3> const cands = Candidates(in);
        plan.candidates = static_cast<std::uint32_t>(cands.size());
        float const speed = Detail::Speed(in);
        Ms const deadline = Detail::Deadline(in);
        Ms const moveStart = in.now + in.params.startDelay;

        // 2. Straight-line pre-filter and score.
        std::vector<Detail::Scored> kept;
        kept.reserve(cands.size());
        for (std::uint32_t i = 0; i < cands.size(); ++i)
        {
            if (i == 0 && in.reroute)
                continue;
            Geo::Vec3 const& c = cands[i];
            float const dist = Detail::Dist3(in.bot, c);
            Ms const arrival = moveStart + Detail::TravelMs(dist, speed);
            if (!PointSafe(in, c, arrival, std::max(arrival, horizonEnd)))
                continue;
            kept.push_back({ i, dist, Detail::Score(in, c, dist, arrival, deadline, i == 0), c });
        }
        std::sort(kept.begin(), kept.end(), Detail::ScoredLess);

        // 3./4. Validate in score order: clean paths first, then grace crossings.
        std::unordered_map<std::uint32_t, std::vector<Geo::Vec3>> paths;
        std::unordered_set<std::uint32_t> unreachable;
        // Oracle calls are capped (maxPathChecks), so candidates whose straight line already fails the path check
        // (typically across a start hazard towards a role anchor on its far side) are tried only after the ones whose
        // straight line passes: the cap must not be spent on them while a clean way out exists (STAGE-M2-REVIEW B1).
        // Each tier runs in score order with the same exact early stop. The tiers only matter while a hazard holding
        // the bot is already hot (the start-hazard exemption is live) or when re-routing a motion (the role anchor sits
        // beyond the hazard); otherwise everything is tier 0 (no extra cost).
        bool startHot = in.reroute;
        for (Hazard const& h : in.hazards)
            startHot = startHot || Detail::HotOver(h, in.bot, in.now, in.now, in.params.marginMs);
        std::vector<std::int8_t> straightOk[2] = { std::vector<std::int8_t>(cands.size(), startHot ? -1 : 1),
            std::vector<std::int8_t>(cands.size(), startHot ? -1 : 1) };
        auto validate = [&](bool allowGrace, Plan& out) -> bool
        {
            bool found = false;
            float bestScore = 0.f;
            Detail::Scored best;
            std::uint32_t checks = 0;
            std::vector<std::int8_t>& straight = straightOk[allowGrace ? 1 : 0];
            for (int tier = 0; tier < 2; ++tier)
            {
                for (Detail::Scored const& s : kept)
                {
                    if (found && bestScore <= s.score)
                        break;
                    if (unreachable.count(s.index))
                        continue;
                    auto it = paths.find(s.index);
                    if (it == paths.end() && checks >= in.params.maxPathChecks)
                        break;
                    if (straight[s.index] < 0)
                        straight[s.index] = CheckPath(in, { in.bot, s.p }, in.now, allowGrace).ok ? 1 : 0;
                    if (straight[s.index] != (tier == 0 ? 1 : 0))
                        continue;
                    if (it == paths.end())
                    {
                        ++checks;
                        ++out.pathChecks;
                        std::vector<Geo::Vec3> path;
                        if (!oracle.Find(in.bot, s.p, path) || path.empty())
                        {
                            unreachable.insert(s.index);
                            continue;
                        }
                        Detail::AnchorPath(in.bot, path);
                        it = paths.emplace(s.index, std::move(path)).first;
                    }
                    std::vector<Geo::Vec3> const& path = it->second;
                    float const len = Detail::PathLength(path);
                    Ms const arrival = moveStart + Detail::TravelMs(len, speed);
                    if (!PointSafe(in, path.back(), arrival, std::max(arrival, horizonEnd)))
                        continue;
                    PathCheck const pc = CheckPath(in, path, in.now, allowGrace);
                    if (!pc.ok)
                        continue;
                    float const real = s.score + in.params.w.path * (len - s.straight);
                    Detail::Scored cand = s;
                    cand.score = real;
                    cand.p = path.back();
                    if (!found || Detail::ScoredLess(cand, best))
                    {
                        found = true;
                        best = cand;
                        bestScore = real;
                        out.path = path;
                        out.target = path.back();
                        out.arriveAt = pc.arriveAt;
                        out.score = real;
                    }
                }
            }
            return found;
        };

        Plan work = plan;
        if (validate(false, work))
            work.outcome = Outcome::Move;
        else if (validate(true, work))
            work.outcome = Outcome::GraceMove;
        else if (in.reroute)
        {
            // No clean way around: the caller keeps the bot where it is (no fallback for a motion).
            plan.candidates = work.candidates;
            plan.pathChecks = work.pathChecks;
            return plan;
        }
        else
        {
            // 5. Fallback: fewest hazards hot from the straight-line arrival, then least time inside on the straight
            // line (scanned for the kFallbackScans nearest such candidates only), then nearest.
            std::vector<std::uint32_t> counts(cands.size(), 0);
            std::uint32_t minCount = std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t i = 0; i < cands.size(); ++i)
            {
                Geo::Vec3 const& c = cands[i];
                Ms const arrival = moveStart + Detail::TravelMs(Detail::Dist3(in.bot, c), speed);
                for (Hazard const& h : in.hazards)
                    if (Detail::HotOver(h, c, arrival, std::max(arrival, horizonEnd), in.params.marginMs))
                        ++counts[i];
                minCount = std::min(minCount, counts[i]);
            }
            std::uint32_t bestIdx = 0;
            Ms bestInside = kForever;
            float bestDist = Detail::kInf;
            std::uint32_t scanned = 0;
            for (std::uint32_t i = 0; i < cands.size() && scanned < Detail::kFallbackScans; ++i)
            {
                if (counts[i] != minCount)
                    continue;
                ++scanned;
                Geo::Vec3 const& c = cands[i];
                float const dist = Detail::Dist3(in.bot, c);
                Ms const inside = Detail::WalkPath(in, { in.bot, c }, in.now, true, true).timeInsideHot;
                if (inside < bestInside || (inside == bestInside && dist < bestDist))
                {
                    bestIdx = i;
                    bestInside = inside;
                    bestDist = dist;
                }
            }
            Geo::Vec3 const target = cands.empty() ? in.bot : cands[bestIdx];
            std::vector<Geo::Vec3> path;
            ++work.pathChecks;
            if (oracle.Find(in.bot, target, path) && !path.empty())
            {
                Detail::AnchorPath(in.bot, path);
                work.outcome = Outcome::Fallback;
            }
            else
            {
                // NoPath: straight to the nearest exit point (else the chosen candidate).
                std::vector<Geo::Vec3> exits;
                for (Hazard const& h : in.hazards)
                    for (Geo::Pose const& pose : h.poses)
                        if (SignedDistance(h.shape, pose, in.bot) <= h.marginYd)
                            Detail::Exits(h, pose, in.bot, exits);
                Geo::Vec3 to = target;
                float best = Detail::kInf;
                for (Geo::Vec3 const& e : exits)
                {
                    float const d = Detail::Dist2(in.bot, e);
                    if (d < best || (d == best && (e.x < to.x || (e.x == to.x && e.y < to.y))))
                    {
                        best = d;
                        to = e;
                    }
                }
                path = { in.bot, to };
                work.outcome = Outcome::NoPath;
            }
            work.path = path;
            work.target = path.back();
            work.arriveAt = moveStart + Detail::TravelMs(Detail::PathLength(path), speed);
            work.score = 0.f;
        }

        // 6. What the plan answers and how long to hold.
        work.answers = Detail::Answers(in, work.path);
        work.holdUntil = Detail::HoldUntil(in, work.answers, work.arriveAt);
        return work;
    }
}

#endif
