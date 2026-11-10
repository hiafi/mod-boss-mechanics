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
 * M3 (STAGE-M3-HANDOFF §3.2, §3.5) adds the player kinds on top of the hazards: regions (be inside), allies and
 * carried shapes (keep allies out of what the bot carries, picking a look-away facing), FarFrom, Freeze (HoldStill),
 * Sight and Reach (soft role terms), and Park (tank parking). TargetOk is the M3 notion of "safe"; with every M3 input
 * empty, Solve, Candidates and TargetOk give exactly the M2 results. Assignment (who soaks, the cluster leader, spread
 * slots) lives in MechanicAssign.h.
 *
 * Times are absolute GameTime milliseconds. Distances are yards; z only matters through a shape's zTolerance.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_SOLVER_H
#define MOD_BOSS_MECHANICS_MECHANIC_SOLVER_H

#include "MechanicDropSpot.h"
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
        float driftYd = 0.f;                // the drift part of marginYd (kept by the reduced-margin pass)
        // Built by the view from other players' planned spots (M3: a carrier's circle, a Bait landing circle, an
        // avoided Soak, a Zone at its anchor's planned spot). RefreshAllies replaces these before every solve; the
        // solver treats them like any other hazard.
        bool synthetic = false;
        // Stratholme X4 (MechanicRef::Lethal, a lethal wall): never crossed. The reduced-margin pass keeps its full
        // margin, no grace crossing, and the Fallback, NoPath and FallbackHolds never take a way that enters it while
        // hot (Detail::CrossesLethal). The view makes it hot from its registration on (ViewRules::LethalTiming).
        bool lethal = false;
    };

    struct LaneFrame
    {
        Frame frame;
        std::vector<LaneRect> rects, freeLanes;
    };

    // --------------------------------------------------------------------------------------------------------------
    // Player kinds (STAGE-M3-HANDOFF §3.2, §3.5). All empty or absent by default: M2 inputs solve as before.
    // --------------------------------------------------------------------------------------------------------------

    // Be inside `shape` at every pose in `poses`, `marginYd` inside its edge, over [from, to] (Stack, claimed Soak,
    // Spot, lane, Bait range). Hard unless `hard` is false (then a score term). The target must be reached by
    // from − marginMs; once that is out of reach (late, or a lasting region already open) the nearest way in is
    // accepted instead (arrival within the straight time to the region plus Detail::kLateSlackYd). A region whose
    // window ended (to + marginMs < now) or that has no pose is ignored.
    struct Region
    {
        std::uint32_t id = 0;
        Shape shape;
        std::vector<Geo::Pose> poses;
        float marginYd = 0.f;
        Ms from{ 0 }, to{ 0 };
        bool hard = true;
    };

    // Another player's spot at a mechanic's judge time: a bot's planned spot, a human's current one.
    struct Ally
    {
        std::uint64_t guid = 0;
        Geo::Vec3 p;
        float slackYd = 0.f;
    };

    // A shape that moves with the bot. No counted ally may be inside it, posed at the bot's spot, at its windows
    // (allies count within marginYd + their slackYd). The target must be reached by timing.from − marginMs when that
    // is still possible. Ignored once its window ended.
    struct Carried
    {
        enum class Mode : std::uint8_t { OnBot, AimedAtBot };
        std::uint32_t id = 0;
        Shape shape;
        Mode mode = Mode::OnBot;
        Geo::Pose source;                   // AimedAtBot: the line's source; it points at the bot's spot
        bool chooseFacing = false;          // OnBot cone/line with followFacing: the solver picks the facing
        float preferredFacing = 0.f;        // ...nearest to this (radians, the bot's victim)
        float fixedFacing = 0.f;            // OnBot, !chooseFacing: the facing it will have
        float marginYd = 0.f;
        float angleMarginDeg = 10.f;        // LookAway.MarginDeg
        Timing timing;
        std::vector<std::uint32_t> allies;  // indices into Input::allies that count
    };

    struct FarFrom { bool present = false; Geo::Vec3 from; float weightPerYd = 0.5f; };
    struct Freeze { bool present = false; Ms from{ 0 }, to{ 0 }; Ms settle{ 250 }; };
    // Score: weights[i] (1 when missing) per point not in sight; only Sight::maxChecks rays per solve.
    struct Sight { std::vector<Geo::Vec3> points; std::vector<float> weights; std::uint32_t maxChecks = 8; };
    // Score: weightPerYd per yard beyond maxYd from each point.
    struct Reach { std::vector<Geo::Vec3> points; float maxYd = 0.f; float weightPerYd = 0.f; };
    // Tank parking (§3.7): the boss chases the tank from `boss` and stops `stopYd` short of it, once the tank is
    // beyond `triggerYd`; the stop must lie `depthYd` inside `zone` (inside = true) or that far outside every
    // `avoid` shape (inside = false, drag out).
    struct Park
    {
        bool present = false, inside = true;
        Geo::Vec3 boss;
        float stopYd = 0.f, triggerYd = 0.f, depthYd = 1.f;
        Region zone;
        std::vector<Hazard> avoid;
    };

    struct Weights
    {
        float path = 1.f, role = 2.f, edge = 1.5f, crowd = 1.f, slackPerMs = 0.01f, stay = -1.f;
        float region = 1.5f;                // per yard short of 1 yd inside a region (RegionComfortWeight)
        // M5 CD (C3 B): per yard closer than Params::spacingYd to a standing member (Input::standing), for a bot with
        // no melee band while no hard region is live (BossMechanics.Bots.SpacingWeight; 0 = off).
        float spacing = 1.5f;
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
        // Reduced-margin pass (tried after the clean pass at the full margin, before a grace crossing): every hazard
        // margin becomes its drift + this (never below kMinTightMarginYd), so a bot can slip through a gap narrower
        // than two full margins. Time margins (marginMs) are not reduced.
        float tightMarginYd = 0.25f;
        // Walkability probes (PathOracle::Walkable) per solve; past this the oracle is asked for paths unprobed.
        std::uint32_t maxProbes = 64;
        // Re-route: a target must be at least this much closer to the role anchor (the motion's end) than the bot,
        // so a clean pass never settles for a spot that makes no progress while a later pass could pass the hazard.
        float rerouteGainYd = 1.f;
        // The slot ring (M3): with a carried circle and a role band, 16 candidates on the band's outer radius. Only
        // a test switches it off (to prove it is needed).
        bool slotRing = true;
        // Entry points round a circle region's edge (M4 WP-Q, Detail::kRegionEdgeArcDeg). Only a test switches them
        // off (to prove they are needed).
        bool regionEdgeArcs = true;
        // M5 CD (C3 B): the spacing term's radius. The barrage's largest circle and Spread are 6 yd, and a donut's
        // ring reaches its hole from 3-8 yd: two ranged bots inside this of each other are both caught by one shot.
        float spacingYd = 6.f;
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
        // M5 CD (C3 B): where every other group member will stand: its plan target in Escape, Hold or Freeze, else
        // its position (a bot in Idle or Watch, a human). The spacing term (Weights::spacing) keeps a bot with no
        // melee band Params::spacingYd away from them; the crowd term (2.5 yd, escape targets only) stays.
        std::vector<Geo::Vec3> standing;
        Params params;
        // The bot's spot is safe but its current motion (follow, chase) is not: solve anyway, without the bot's own
        // spot as a candidate and only with targets params.rerouteGainYd closer to the role anchor (the motion's
        // end), so the anchor pulls it around the hazard.
        bool reroute = false;

        // Player kinds (M3).
        std::vector<Region> regions;
        std::vector<Ally> allies;
        std::vector<Carried> carried;
        // Stratholme X1: other players' carried shapes at their live spots (a Spread/RunOut carrier still walking to
        // its planned spot). Soft blockers for the route search only (MechanicRoute.h: a crossing cost per cell, never
        // a block) and SoftCrossed; Solve and every other check ignore them.
        std::vector<Hazard> softHazards;
        FarFrom far;
        Freeze freeze;
        Sight sight;
        Reach reach;
        Park park;
        // Stratholme X2: the drop spot rule of a carried shape (MechanicDropSpot.h): score terms and candidates.
        Drop drop;
        // Stratholme F-1b: hard spot bounds (MechanicDropSpot.h, Bounds): every set holds a moved-to target (and, with
        // pathInside, its path); a parked boss's stop too (ParkOk). Empty = anywhere.
        std::vector<Bounds> bounds;
        // Slot ring rotation: slot × 360° / slotCount (Assign::SlotOf among the carriers; 0 = no rotation).
        std::uint32_t slot = 0, slotCount = 0;
        // Solve even when the bot's spot passes TargetOk (a role move: healer sight, reach): the spot competes as an
        // ordinary candidate (with Weights::stay) and wins as Safe. Ignored while frozen and under `reroute`.
        bool roleSolve = false;
    };

    // Walkable path from `from` to `to`: `out` gets the corner points, from the start (or its first corner) to the
    // end with its real z. The end may differ from `to` (the walkable point the oracle found for it): the solver
    // validates and scores the real end. false = no path (the candidate is skipped).
    //
    // Walkable is a cheap probe the solver runs before it spends a path check on a candidate: false = `p` is not on
    // walkable ground (inside a wall or a pillar; skipped, no path check spent); true may move `p` onto the walkable
    // surface (a snap, with its real z). The default knows no geometry: everything is walkable, unchanged.
    class PathOracle
    {
    public:
        virtual ~PathOracle() = default;
        virtual bool Find(Geo::Vec3 const& from, Geo::Vec3 const& to, std::vector<Geo::Vec3>& out) = 0;
        virtual bool Walkable(Geo::Vec3& /*p*/) { return true; }
        // Line of sight between two points (the glue lifts both ends by their collision height). Only Sight asks.
        virtual bool InSight(Geo::Vec3 const& /*from*/, Geo::Vec3 const& /*to*/) { return true; }
    };

    enum class Outcome : std::uint8_t
    {
        Safe,           // the bot's spot is safe over the horizon
        Move,           // a clean path to a safe target
        GraceMove,      // as Move, but crossing armed mines under their grace
        Fallback,       // nothing clean: the least bad target, by the oracle's path
        NoPath          // nothing clean and no oracle path: straight to the nearest exit
    };

    // Why the validation passes rejected what they tried (counts per attempt; a later pass re-tries the paths an
    // earlier one found). Plain counters, kept on every solve; the brain logs them with BossMechanics.Bots.Debug = 1.
    struct SolveStats
    {
        std::uint32_t kept = 0;             // candidates past the straight-line pre-filter (full margin)
        std::uint32_t probes = 0;           // walkability probes run
        std::uint32_t offMesh = 0;          // ...that found no walkable ground (no path check spent)
        std::uint32_t snapped = 0;          // ...that moved the candidate onto the walkable surface
        std::uint32_t noPath = 0;           // oracle path calls that returned no path
        std::uint32_t endUnsafe = 0;        // the snapped point or the path's real end is not safe at its arrival
        std::uint32_t crossStart = 0;       // a path was found, it fails inside a hazard that holds the bot
        std::uint32_t crossOther = 0;       // ...inside another hazard
        std::uint32_t budgetStops = 0;      // validation passes cut short by maxPathChecks
        std::uint32_t outOfBounds = 0;      // F-1b: a target or its path outside the hard bounds (Input::bounds)
        std::uint32_t lethalSkipped = 0;    // X4: Fallback/NoPath candidates or paths dropped for crossing a lethal
        // M4 cost (D9): lane-frame candidates in the candidate list, and the oracle path calls spent on them.
        std::uint32_t laneCandidates = 0;
        std::uint32_t lanePathChecks = 0;
        std::uint8_t pass = 0;              // what found the plan: 1 clean, 2 clean at the reduced margin, 3 grace

        void Add(SolveStats const& o)       // sums the counters (not `pass`)
        {
            kept += o.kept;
            probes += o.probes;
            offMesh += o.offMesh;
            snapped += o.snapped;
            noPath += o.noPath;
            endUnsafe += o.endUnsafe;
            crossStart += o.crossStart;
            crossOther += o.crossOther;
            budgetStops += o.budgetStops;
            outOfBounds += o.outOfBounds;
            lethalSkipped += o.lethalSkipped;
            laneCandidates += o.laneCandidates;
            lanePathChecks += o.lanePathChecks;
        }
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
        SolveStats stats;
        // M3: the facing a chooseFacing carried shape needs at the target (radians, (-pi, pi]).
        bool hasFacing = false;
        float facing = 0.f;
        std::vector<Geo::Vec3> waypoints;   // Route only (MechanicRoute.h)
        std::uint32_t violated = 0;         // hard regions + carried shapes the plan breaks (Fallback only)
        std::uint32_t sightChecks = 0;      // InSight rays spent
        // Fallback and NoPath only (M4 §3.7): time the path spends inside an exact hitbox (drift kept, no yard or time
        // margin) while one of its windows is open, from now. 0 = a "clear" fallback.
        Ms exposure{ 0 };
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
        // A shape posed once with its trigonometry done (STAGE-M3-F: CheckPath and the route grid test many points
        // against the same pose). FrameSd equals SignedDistance up to float rounding; z is the caller's.
        struct PoseFrame
        {
            Geo::Pose pose;
            float ux = 1.f, uy = 0.f;           // cone and line axis (unit)
            float ax = 0.f, ay = 0.f;           // cone edge ends, relative to the apex
            float bx = 0.f, by = 0.f;
            float cosHalf = 1.f;
        };

        inline PoseFrame PrepareFrame(Shape const& s, Geo::Pose const& pose)
        {
            PoseFrame r;
            r.pose = pose;
            float const f = Geo::ShapeFacing(s, pose);
            r.ux = std::cos(f);
            r.uy = std::sin(f);
            if (s.type == ShapeType::Cone && s.angleDeg < 360.f)
            {
                float const half = Geo::DegToRad(s.angleDeg) * 0.5f;
                r.cosHalf = std::cos(half);
                r.ax = std::cos(f + half) * s.length;
                r.ay = std::sin(f + half) * s.length;
                r.bx = std::cos(f - half) * s.length;
                r.by = std::sin(f - half) * s.length;
            }
            return r;
        }

        // Squared distance from (px, py) to the segment (0, 0) - (bx, by).
        inline float SegmentDist2FromOrigin(float px, float py, float bx, float by)
        {
            float const l2 = bx * bx + by * by;
            float const u = l2 > 0.f ? std::clamp((px * bx + py * by) / l2, 0.f, 1.f) : 0.f;
            float const ex = px - u * bx;
            float const ey = py - u * by;
            return ex * ex + ey * ey;
        }

        inline float FrameSd(Shape const& s, PoseFrame const& r, float dx, float dy)
        {
            switch (s.type)
            {
                case ShapeType::Circle:
                    return std::sqrt(dx * dx + dy * dy) - s.radius;
                case ShapeType::Donut:
                {
                    float const d = std::sqrt(dx * dx + dy * dy);
                    if (d >= s.radius)
                        return d - s.radius;
                    if (d <= s.inner)
                        return s.inner - d;
                    return -std::min(d - s.inner, s.radius - d);
                }
                case ShapeType::Cone:
                {
                    float const d = std::sqrt(dx * dx + dy * dy);
                    if (s.angleDeg >= 360.f)
                        return d - s.length;
                    float const edges = std::sqrt(std::min(SegmentDist2FromOrigin(dx, dy, r.ax, r.ay),
                        SegmentDist2FromOrigin(dx, dy, r.bx, r.by)));
                    bool const withinAngle = d < 1e-6f || dx * r.ux + dy * r.uy >= d * r.cosHalf;
                    if (withinAngle && d <= s.length)
                        return -std::min(s.length - d, edges);
                    if (withinAngle)
                        return std::min(edges, d - s.length);
                    return edges;
                }
                case ShapeType::Line:
                {
                    if (s.length <= s.excludeStart || s.width <= 0.f)
                        return kInf;
                    float const ls = dx * r.ux + dy * r.uy;
                    float const lt = -dx * r.uy + dy * r.ux;
                    float const cs = (s.excludeStart + s.length) * 0.5f;
                    float const hs = (s.length - s.excludeStart) * 0.5f;
                    float const qs = std::fabs(ls - cs) - hs;
                    float const qt = std::fabs(lt) - s.width * 0.5f;
                    float const ox = std::max(qs, 0.f);
                    float const oy = std::max(qt, 0.f);
                    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qs, qt), 0.f);
                }
            }
            return kInf;
        }

        // Within (below) over prepared frames of h's poses.
        inline bool WithinFrames(Hazard const& h, PoseFrame const* frames, std::size_t count, Geo::Vec3 const& p,
            float* sdOut)
        {
            float const reach = Geo::Bounds(h.shape) + h.marginYd + 0.01f;
            float best = kInf;
            for (std::size_t k = 0; k < count; ++k)
            {
                Geo::Pose const& pose = frames[k].pose;
                if (std::fabs(p.z - pose.z) > h.shape.zTolerance + Geo::kEdgeEpsilon)
                    continue;
                float const dx = p.x - pose.x;
                float const dy = p.y - pose.y;
                if (dx * dx + dy * dy > reach * reach)
                    continue;
                best = std::min(best, FrameSd(h.shape, frames[k], dx, dy));
            }
            *sdOut = best;
            return best <= h.marginYd;
        }

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
    // Player kinds (M3): regions, carried shapes, freeze, parking
    // --------------------------------------------------------------------------------------------------------------
    namespace Detail
    {
        constexpr float kLateSlackYd = 3.f;         // a late region or carrier: arrival within the straight time + this
        constexpr float kFacingStep = 1e-3f;        // a free facing sits this far (radians) beyond a blocked arc

        inline bool HasPlayerKinds(Input const& in)
        {
            return !in.regions.empty() || !in.carried.empty() || in.freeze.present || in.park.present;
        }

        // The hazard horizon: a pending freeze extends it to the window's end (a Zone landing while frozen counts).
        inline Ms EffectiveHorizon(Input const& in, Ms horizonEnd)
        {
            return in.freeze.present ? std::max(horizonEnd, in.freeze.to) : horizonEnd;
        }

        // Frozen: from freeze.from − settle on, Solve never moves the bot.
        inline bool Frozen(Input const& in)
        {
            return in.freeze.present && in.now >= in.freeze.from - in.freeze.settle && in.now <= in.freeze.to;
        }

        // Largest signed distance over the poses (≤ −margin = inside every pose by the margin). +inf without poses.
        inline float RegionSd(Shape const& shape, std::vector<Geo::Pose> const& poses, Geo::Vec3 const& p)
        {
            if (poses.empty())
                return kInf;
            float worst = -kInf;
            for (Geo::Pose const& pose : poses)
                worst = std::max(worst, SignedDistance(shape, pose, p));
            return worst;
        }

        // Smallest signed distance over the poses (> m = outside every pose by m).
        inline float MinSd(Shape const& shape, std::vector<Geo::Pose> const& poses, Geo::Vec3 const& p)
        {
            float best = kInf;
            for (Geo::Pose const& pose : poses)
                best = std::min(best, SignedDistance(shape, pose, p));
            return best;
        }

        inline bool RegionLive(Input const& in, Region const& r)
        {
            return !r.poses.empty() && r.to + in.params.marginMs >= in.now;
        }

        inline bool InsideRegion(Region const& r, Geo::Vec3 const& p)
        {
            return RegionSd(r.shape, r.poses, p) <= -r.marginYd;
        }

        // The latest arrival a region accepts: from − marginMs, or, once that is out of reach, the straight time to
        // its margin edge from the bot plus kLateSlackYd.
        inline Ms RegionDue(Input const& in, Region const& r)
        {
            float const sd = RegionSd(r.shape, r.poses, in.bot);
            float const way = std::isfinite(sd) ? std::max(0.f, sd + r.marginYd) : 0.f;
            Ms const earliest = in.now + in.params.startDelay + TravelMs(way + kLateSlackYd, Speed(in));
            return std::max(r.from - in.params.marginMs, earliest);
        }

        inline bool RegionOk(Input const& in, Region const& r, Geo::Vec3 const& p, Ms arrival)
        {
            return InsideRegion(r, p) && arrival <= RegionDue(in, r);
        }

        inline Ms TimingEnd(Timing const& t)
        {
            return std::max(t.from, t.to);
        }

        inline bool CarriedLive(Input const& in, Carried const& c)
        {
            return TimingEnd(c.timing) + in.params.marginMs >= in.now;
        }

        inline bool ChoosesFacing(Carried const& c)
        {
            return c.mode == Carried::Mode::OnBot && c.chooseFacing
                && (c.shape.type == ShapeType::Cone || c.shape.type == ShapeType::Line);
        }

        // The latest arrival a carrier accepts: timing.from − marginMs, or once that is out of reach, the straight
        // time for the shape's reach plus margin plus kLateSlackYd.
        inline Ms CarriedDue(Input const& in, Carried const& c)
        {
            Ms const earliest = in.now + in.params.startDelay
                + TravelMs(Geo::Bounds(c.shape) + c.marginYd + kLateSlackYd, Speed(in));
            return std::max(c.timing.from - in.params.marginMs, earliest);
        }

        // The pose of a carried shape with the bot at `at` (OnBot: facing `facing`; AimedAtBot: from its source,
        // pointing at `at`).
        inline Geo::Pose CarriedPose(Carried const& c, Geo::Vec3 const& at, float facing)
        {
            if (c.mode == Carried::Mode::OnBot)
                return { at.x, at.y, at.z, facing };
            float const dx = at.x - c.source.x;
            float const dy = at.y - c.source.y;
            float const o = dx * dx + dy * dy > 1e-6f ? std::atan2(dy, dx) : c.source.o;
            return { c.source.x, c.source.y, c.source.z, o };
        }

        // No counted ally inside the shape posed with the bot at `at` (exact SignedDistance check).
        inline bool CarriedClear(Input const& in, Carried const& c, Geo::Vec3 const& at, float facing)
        {
            Geo::Pose const pose = CarriedPose(c, at, facing);
            for (std::uint32_t idx : c.allies)
            {
                if (idx >= in.allies.size())
                    continue;
                Ally const& a = in.allies[idx];
                if (SignedDistance(c.shape, pose, a.p) <= c.marginYd + a.slackYd)
                    return false;
            }
            return true;
        }

        // Blocked facings of one chooseFacing shape at `at`, as arcs in [0, 2pi] of the bot's facing (the shape's
        // facing offset taken out). false = every facing is blocked (an ally within the margin of the apex, or an
        // arc of a full turn).
        struct Arc
        {
            float lo, hi;
        };

        constexpr float kFacingEdgeDeg = 0.5f;

        inline bool BlockedArcs(Input const& in, Carried const& c, Geo::Vec3 const& at, std::vector<Arc>& arcs)
        {
            bool const line = c.shape.type == ShapeType::Line;
            float const offset = Geo::DegToRad(c.shape.facingOffsetDeg);
            // A hair over the margin (kFacingEdgeDeg): the allies see this cone at the reserved facing ± the same
            // margin (the view's planned pose), so an ally the carrier just clears sees itself clear too, never on
            // the edge (STAGE-M3-R-REPORT m-8).
            float const alpha = (line ? 0.f : Geo::DegToRad(c.shape.angleDeg) * 0.5f)
                + Geo::DegToRad(std::max(0.f, c.angleMarginDeg) + kFacingEdgeDeg);
            float const len = line ? Geo::Bounds(c.shape) : c.shape.length;
            if (!line && c.shape.angleDeg >= 360.f)
            {
                for (std::uint32_t idx : c.allies)
                    if (idx < in.allies.size()
                        && SignedDistance(c.shape, { at.x, at.y, at.z, 0.f }, in.allies[idx].p)
                            <= c.marginYd + in.allies[idx].slackYd)
                        return false;
                return true;
            }
            for (std::uint32_t idx : c.allies)
            {
                if (idx >= in.allies.size())
                    continue;
                Ally const& a = in.allies[idx];
                if (std::fabs(a.p.z - at.z) > c.shape.zTolerance + Geo::kEdgeEpsilon)
                    continue;
                float const pad = c.marginYd + a.slackYd;
                float const d = Dist2(at, a.p);
                if (d > len + pad)
                    continue;
                if (d <= pad)
                    return false;
                float const side = line ? c.shape.width * 0.5f + pad : pad;
                float const half = alpha + std::asin(std::min(1.f, side / d));
                if (half >= Geo::kPi)
                    return false;
                float const beta = std::atan2(a.p.y - at.y, a.p.x - at.x) - offset;
                float const lo = Geo::Wrap2Pi(beta - half);
                float const hi = lo + 2.f * half;
                if (hi > Geo::kTwoPi)
                {
                    arcs.push_back({ lo, Geo::kTwoPi });
                    arcs.push_back({ 0.f, hi - Geo::kTwoPi });
                }
                else
                    arcs.push_back({ lo, hi });
            }
            return true;
        }

        inline bool InArcs(std::vector<Arc> const& arcs, float f)
        {
            for (Arc const& a : arcs)
                if (f >= a.lo - 1e-6f && f <= a.hi + 1e-6f)
                    return true;
            return false;
        }

        // The free facing nearest `preferred` (ties: the smaller angle in [0, 2pi)). false = none.
        inline bool PickFacing(std::vector<Arc> const& arcs, float preferred, float& out)
        {
            float const pref = Geo::Wrap2Pi(preferred);
            if (!InArcs(arcs, pref))
            {
                out = Geo::WrapPi(pref);
                return true;
            }
            bool found = false;
            float best = kInf;
            float bestF = 0.f;
            for (Arc const& a : arcs)
                for (float e : { a.lo - kFacingStep, a.hi + kFacingStep })
                {
                    float const f = Geo::Wrap2Pi(e);
                    if (InArcs(arcs, f))
                        continue;
                    float const d = std::fabs(Geo::WrapPi(f - pref));
                    if (d < best - 1e-6f || (std::fabs(d - best) <= 1e-6f && f < bestF))
                    {
                        best = d;
                        bestF = f;
                        found = true;
                    }
                }
            if (found)
                out = Geo::WrapPi(bestF);
            return found;
        }

        // A shape's middle: circle or donut center, half way along a cone's axis or a line's centerline.
        inline Geo::Vec3 ShapeCenter(Shape const& s, Geo::Pose const& pose)
        {
            float const f = Geo::ShapeFacing(s, pose);
            float along = 0.f;
            if (s.type == ShapeType::Cone && s.angleDeg < 360.f)
                along = s.length * 0.5f;
            else if (s.type == ShapeType::Line)
                along = (s.excludeStart + s.length) * 0.5f;
            return { pose.x + std::cos(f) * along, pose.y + std::sin(f) * along, pose.z };
        }

        // Parking: where the boss stops when the tank stands at p (it does not move while p is within triggerYd).
        inline bool ParkStopOk(Park const& pk, Geo::Vec3 const& s)
        {
            if (pk.inside)
                return RegionSd(pk.zone.shape, pk.zone.poses, s) <= -pk.depthYd;
            for (Hazard const& h : pk.avoid)
                if (MinSd(h.shape, h.poses, s) < pk.depthYd)
                    return false;
            return true;
        }

        inline bool ParkOk(Input const& in, Geo::Vec3 const& p)
        {
            Park const& pk = in.park;
            if (!pk.present)
                return true;
            float const d = Dist2(p, pk.boss);
            if (d <= pk.triggerYd)
                return ParkStopOk(pk, pk.boss);     // stay: the boss is already where it should be, in reach
            if (pk.inside && MinSd(pk.zone.shape, pk.zone.poses, p) <= pk.zone.marginYd)
                return false;                       // the tank stays outside the zone
            float const k = pk.stopYd / std::max(d, 1e-3f);
            Geo::Vec3 const stop{ p.x + (pk.boss.x - p.x) * k, p.y + (pk.boss.y - p.y) * k, pk.boss.z };
            // F-1b: the boss's stop keeps the bounds too (a door's keep-out circle: the stock leash).
            return ParkStopOk(pk, stop) && BossMechanics::DropSpot::InBounds(in.bounds, stop);
        }

        inline bool FreezeOk(Input const& in, Ms arrival)
        {
            if (!in.freeze.present || Frozen(in) || in.now > in.freeze.to)
                return true;
            return arrival <= in.freeze.from - in.freeze.settle;
        }
    }

    // Every live hard region holds p (by its margin), reached in time (Region).
    inline bool RegionsOk(Input const& in, Geo::Vec3 const& p, Ms arrival)
    {
        for (Region const& r : in.regions)
            if (r.hard && Detail::RegionLive(in, r) && !Detail::RegionOk(in, r, p, arrival))
                return false;
        return true;
    }

    // Free facing for one carried shape at `at`: false = no facing clears the allies; true sets `facing`. Only an
    // OnBot cone or line with chooseFacing picks (the free arc nearest preferredFacing, §3.5 "Free facing"); any
    // other shape is checked at its own facing (fixedFacing, or aimed from its source), which `facing` gets.
    inline bool FreeFacing(Input const& in, Carried const& c, Geo::Vec3 const& at, float& facing)
    {
        if (!Detail::ChoosesFacing(c))
        {
            Geo::Pose const pose = Detail::CarriedPose(c, at, c.fixedFacing);
            facing = c.mode == Carried::Mode::OnBot ? c.fixedFacing : pose.o;
            return Detail::CarriedClear(in, c, at, c.fixedFacing);
        }
        std::vector<Detail::Arc> arcs;
        float f = 0.f;
        if (!Detail::BlockedArcs(in, c, at, arcs) || !Detail::PickFacing(arcs, c.preferredFacing, f))
            return false;
        if (!Detail::CarriedClear(in, c, at, f))
            return false;
        facing = f;
        return true;
    }

    // No counted ally inside any live carried shape with the bot at p. Every chooseFacing shape shares one facing
    // (the free arc of all of them nearest the first one's preferredFacing), written to *facing.
    inline bool CarriedOk(Input const& in, Geo::Vec3 const& p, float* facing)
    {
        std::vector<Detail::Arc> arcs;
        Carried const* first = nullptr;
        for (Carried const& c : in.carried)
        {
            if (!Detail::CarriedLive(in, c))
                continue;
            if (!Detail::ChoosesFacing(c))
            {
                if (!Detail::CarriedClear(in, c, p, c.fixedFacing))
                    return false;
                continue;
            }
            if (!first)
                first = &c;
            if (!Detail::BlockedArcs(in, c, p, arcs))
                return false;
        }
        if (!first)
            return true;
        float f = 0.f;
        if (!Detail::PickFacing(arcs, first->preferredFacing, f))
            return false;
        for (Carried const& c : in.carried)
            if (Detail::CarriedLive(in, c) && Detail::ChoosesFacing(c) && !Detail::CarriedClear(in, c, p, f))
                return false;
        if (facing)
            *facing = f;
        return true;
    }

    namespace Detail
    {
        // The hazard part of TargetOk: PointSafe over [arrival, the horizon (a pending freeze's end)].
        inline bool HazardsOk(Input const& in, Geo::Vec3 const& p, Ms arrival, Ms horizonEnd)
        {
            return PointSafe(in, p, arrival, std::max(arrival, EffectiveHorizon(in, horizonEnd)));
        }

        // The player-kind part of TargetOk.
        inline bool PlayerKindsOk(Input const& in, Geo::Vec3 const& p, Ms arrival, float* facing)
        {
            if (!HasPlayerKinds(in))
                return true;
            if (!FreezeOk(in, arrival) || !RegionsOk(in, p, arrival) || !ParkOk(in, p))
                return false;
            for (Carried const& c : in.carried)
                if (CarriedLive(in, c) && arrival > CarriedDue(in, c))
                    return false;
            return CarriedOk(in, p, facing);
        }

        // Hard regions and carried shapes p breaks (the Fallback ranking and Plan::violated).
        inline std::uint32_t Violations(Input const& in, Geo::Vec3 const& p, Ms arrival)
        {
            std::uint32_t n = 0;
            for (Region const& r : in.regions)
                if (r.hard && RegionLive(in, r) && !RegionOk(in, r, p, arrival))
                    ++n;
            for (Carried const& c : in.carried)
            {
                if (!CarriedLive(in, c))
                    continue;
                float f = 0.f;
                if (arrival > CarriedDue(in, c) || !FreeFacing(in, c, p, f))
                    ++n;
            }
            return n;
        }

        inline bool AnyChoosesFacing(Input const& in)
        {
            for (Carried const& c : in.carried)
                if (CarriedLive(in, c) && ChoosesFacing(c))
                    return true;
            return false;
        }
    }

    // PointSafe + RegionsOk + CarriedOk + Freeze + Park: what the brain calls "safe" from M3 on. The hazards are
    // checked over [arrival, horizonEnd] (extended to a pending freeze's end); before a freeze the arrival must be
    // by freeze.from − settle (while frozen that term always holds: Solve never moves a frozen bot). *facing gets
    // the facing a chooseFacing carried shape needs at p (unchanged when there is none).
    inline bool TargetOk(Input const& in, Geo::Vec3 const& p, Ms arrival, Ms horizonEnd, float* facing = nullptr)
    {
        return Detail::HazardsOk(in, p, arrival, horizonEnd) && Detail::PlayerKindsOk(in, p, arrival, facing);
    }

    // --------------------------------------------------------------------------------------------------------------
    // Path check
    // --------------------------------------------------------------------------------------------------------------

    namespace Detail
    {
        constexpr float kDeeperEpsYd = 0.05f;       // start-hazard exemption: "deeper" beyond this
        constexpr float kMinStepYd = 0.1f;          // the path walk's finest sample spacing
        constexpr float kExitSlackYd = 1.f;         // start-hazard exemption: dwell cap beyond the straight exit

        // The walk behind CheckPath. measure = keep walking after a failing sample and count its dwell too (the
        // Fallback ranks candidates by their full time inside, so the walk must not stop at the first hit).
        // measureCap (measure only, M4 WP-Q G7): stop once the time inside exceeds it; timeInsideHot is then some
        // value above the cap, not the full time (the Fallback ranking only needs to know the walk lost).
        inline PathCheck WalkPath(Input const& in, std::vector<Geo::Vec3> const& path, Ms start, bool allowGrace,
            bool measure, Ms measureCap = kForever)
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
                Ms hotFrom{ 0 }, hotTo{ 0 };    // the hazard's hot hull, widened (nearLeg's time test)
            };
            std::vector<Track> track(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                // Every window of a non-projectile lies in [from, max(to, strictTo)]. A projectile's can reach past
                // HazardEnd in its margin band beyond the far end: never skipped.
                Hazard const& h = in.hazards[i];
                bool const projectile = h.timing.type == TimingType::Projectile;
                track[i].hotFrom = projectile ? -kForever : h.timing.from - widen;
                track[i].hotTo = projectile ? kForever : std::max(HazardEnd(h), h.timing.strictTo) + widen;
            }
            // Each hazard pose prepared once, when a sample first tests it (the samples then test it many times).
            std::vector<std::size_t> firstFrame(n + 1, 0);
            for (std::size_t i = 0; i < n; ++i)
                firstFrame[i + 1] = firstFrame[i] + in.hazards[i].poses.size();
            std::vector<PoseFrame> frames(firstFrame[n]);
            std::vector<std::uint8_t> prepared(n, 0);
            auto prepare = [&](std::size_t i)
            {
                if (prepared[i])
                    return;
                prepared[i] = 1;
                for (std::size_t k = 0; k < in.hazards[i].poses.size(); ++k)
                    frames[firstFrame[i] + k] = PrepareFrame(in.hazards[i].shape, in.hazards[i].poses[k]);
            };
            auto within = [&](std::size_t i, Geo::Vec3 const& p, float* sd)
            {
                prepare(i);
                return WithinFrames(in.hazards[i], frames.data() + firstFrame[i], firstFrame[i + 1] - firstFrame[i], p,
                    sd);
            };
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

            // Hazards whose bounds reach the current leg, walked over [lo, hi] (the rest cannot hold any of its
            // samples), and (STAGE-M3-F) whose hot hull meets that time, widened as the samples' windows are: a hazard
            // hot only before or after the leg cannot fail it. A start hazard's exemption state is kept: it stays
            // near while its exemption runs. Returns the smallest margin of the hazards near (kInf: none).
            auto nearLeg = [&](Geo::Vec3 const& a, Geo::Vec3 const& b, Ms lo, Ms hi)
            {
                float smallest = kInf;
                for (std::size_t i = 0; i < n; ++i)
                {
                    Hazard const& h = in.hazards[i];
                    float const reach = Geo::Bounds(h.shape) + h.marginYd + 0.01f;
                    Track& tr = track[i];
                    tr.near = false;
                    for (Geo::Pose const& pose : h.poses)
                        if (SegmentDistance(pose.x, pose.y, a.x, a.y, b.x, b.y) <= reach)
                            tr.near = true;
                    if (tr.near && !tr.inStart)
                    {
                        Ms const end = std::max(HazardEnd(h), h.timing.strictTo);
                        tr.near = h.timing.from - widen <= hi && end + widen >= lo;
                    }
                    if (!tr.near)
                        tr.inStart = false;
                    else
                        smallest = std::min(smallest, h.marginYd);
                }
                return smallest;
            };

            auto visit = [&](Geo::Vec3 const& p, Ms lo, Ms hi, Ms dwell) -> bool
            {
                bool clean = true;
                for (std::size_t i = 0; i < n; ++i)
                {
                    Track& tr = track[i];
                    if (!tr.near)
                        continue;
                    // M4 WP-Q (G7): a sample outside the hazard's hot hull cannot fail on it, and outside its start
                    // exemption the spatial test would only find that out later (nearLeg's leg test, per sample: a
                    // lane set's lines reach every sample of a long leg but are hot for half a second of it).
                    if (!tr.inStart && (lo > tr.hotTo || hi < tr.hotFrom))
                        continue;
                    Hazard const& h = in.hazards[i];
                    float sd = kInf;
                    if (!within(i, p, &sd))
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
                    if (allowGrace && graceOnly && !h.lethal)
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
                if (measure && res.timeInsideHot > measureCap)
                    return false;
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
                Ms const legFrom = k == 1 ? start : moveStart + TravelMs(dist, speed);
                Ms const legTo = moveStart + TravelMs(dist + len, speed);
                // Samples at most one margin apart (STAGE-M3-R-REPORT m-11): a straight leg past a corner then keeps
                // at least cos(30°) of the margin, never touching the hitbox; 0.5 yd as before for full margins.
                float const smallest = nearLeg(a, b, legFrom, legTo);
                float const step = std::clamp(smallest, kMinStepYd, kStepYd);
                std::uint32_t const steps = std::max<std::uint32_t>(1,
                    static_cast<std::uint32_t>(std::ceil(len / step)));
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
            if (!HasPlayerKinds(in))
                return best;
            // M3: a hard region the bot is not in, a carried shape its spot breaks, a pending freeze.
            for (Region const& r : in.regions)
                if (r.hard && RegionLive(in, r) && r.from - in.params.marginMs <= horizonEnd
                    && !InsideRegion(r, in.bot))
                    best = std::min(best, r.from);
            for (Carried const& c : in.carried)
            {
                float f = 0.f;
                if (CarriedLive(in, c) && c.timing.from - in.params.marginMs <= horizonEnd
                    && !FreeFacing(in, c, in.bot, f))
                    best = std::min(best, c.timing.from);
            }
            if (in.freeze.present && !Frozen(in) && in.now <= in.freeze.to)
                best = std::min(best, in.freeze.from - in.freeze.settle);
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

        constexpr float kRegionInsetYd = 0.5f;      // region candidates sit this far inside the margin
        constexpr float kRegionLineStepYd = 2.f;    // line region: centerline points this far apart
        // A circle region the bot is outside: entry points on its inset edge this far round from the nearest one,
        // both ways (M4 WP-Q, G1): when the straight way in is blocked (another player's landing circle on it), the
        // nearest point that is not lies round the edge, and the grid rings and the half-radius ring are too coarse
        // to hit the thin band between the edge and what the bot can reach in time.
        constexpr std::array<float, 6> kRegionEdgeArcDeg{ 15.f, 30.f, 45.f, 60.f, 90.f, 120.f };
        constexpr float kMarchStepYd = 0.25f;       // parking: outward march step

        // The point of the shape nearest p, `inset` inside its edge (the shape's middle when it has no such room).
        inline Geo::Vec3 NearestInside(Shape const& s, Geo::Pose const& pose, Geo::Vec3 const& p, float inset)
        {
            float const dx = p.x - pose.x;
            float const dy = p.y - pose.y;
            float const d = std::hypot(dx, dy);
            float const f = Geo::ShapeFacing(s, pose);
            auto polar = [&](float bearing, float r) -> Geo::Vec3
            {
                return { pose.x + std::cos(bearing) * r, pose.y + std::sin(bearing) * r, pose.z };
            };
            float const bearing = d > 1e-3f ? std::atan2(dy, dx) : f;
            switch (s.type)
            {
                case ShapeType::Circle:
                    return polar(bearing, std::min(d, std::max(0.f, s.radius - inset)));
                case ShapeType::Donut:
                {
                    float const lo = s.inner + inset;
                    float const hi = s.radius - inset;
                    return polar(bearing, lo > hi ? (s.inner + s.radius) * 0.5f : std::clamp(d, lo, hi));
                }
                case ShapeType::Cone:
                {
                    if (s.angleDeg >= 360.f)
                        return polar(bearing, std::min(d, std::max(0.f, s.length - inset)));
                    float const half = Geo::DegToRad(s.angleDeg) * 0.5f;
                    float const lo = inset / std::max(1e-3f, std::sin(std::min(half, Geo::kPi * 0.5f)));
                    float const hi = s.length - inset;
                    if (lo > hi)
                        return ShapeCenter(s, pose);
                    float const r = std::clamp(d, lo, hi);
                    float const room = std::max(0.f, half - std::asin(std::min(1.f, inset / r)));
                    return polar(f + std::clamp(Geo::WrapPi(bearing - f), -room, room), r);
                }
                case ShapeType::Line:
                {
                    Geo::LineCoords const c = Geo::ToLineCoords(s, pose, p);
                    float const s0 = s.excludeStart + inset;
                    float const s1 = s.length - inset;
                    float const t1 = s.width * 0.5f - inset;
                    float const ls = s0 > s1 ? (s.excludeStart + s.length) * 0.5f : std::clamp(c.s, s0, s1);
                    float const lt = t1 < 0.f ? 0.f : std::clamp(c.t, -t1, t1);
                    float const ux = std::cos(f);
                    float const uy = std::sin(f);
                    return { pose.x + ux * ls - uy * lt, pose.y + uy * ls + ux * lt, pose.z };
                }
            }
            return { pose.x, pose.y, pose.z };
        }

        // Region candidates: its middle, the nearest point inside (margin + kRegionInsetYd in), and 8 spread points
        // (circle: a ring at half its usable radius; donut: mid-band; cone: two arcs; line: the bot's projection on
        // the centerline and points every kRegionLineStepYd along it).
        inline void RegionCandidates(Input const& in, Region const& r, std::vector<Geo::Vec3>& out)
        {
            Shape const& s = r.shape;
            Geo::Pose const& pose = r.poses[r.poses.size() / 2];
            float const inset = r.marginYd + kRegionInsetYd;
            Geo::Vec3 const c = ShapeCenter(s, pose);
            out.push_back(c);
            out.push_back(NearestInside(s, pose, in.bot, inset));
            float const f = Geo::ShapeFacing(s, pose);
            switch (s.type)
            {
                case ShapeType::Circle:
                case ShapeType::Donut:
                {
                    float const rr = s.type == ShapeType::Circle ? std::max(0.f, s.radius - r.marginYd) * 0.5f
                        : (s.inner + s.radius) * 0.5f;
                    for (std::uint32_t i = 0; i < 8; ++i)
                        PushPolar(out, { pose.x, pose.y, pose.z }, f + Geo::kTwoPi * i / 8.f, rr, pose.z);
                    // Entry points round the edge (kRegionEdgeArcDeg), for a bot outside a circle.
                    float const edge = s.radius - inset;
                    float const dx = in.bot.x - pose.x;
                    float const dy = in.bot.y - pose.y;
                    if (in.params.regionEdgeArcs && s.type == ShapeType::Circle && edge > 0.f
                        && dx * dx + dy * dy > edge * edge)
                    {
                        float const bearing = std::atan2(dy, dx);
                        for (float deg : kRegionEdgeArcDeg)
                            for (float sign : { 1.f, -1.f })
                                PushPolar(out, { pose.x, pose.y, pose.z }, bearing + sign * Geo::DegToRad(deg), edge,
                                    pose.z);
                    }
                    return;
                }
                case ShapeType::Cone:
                {
                    float const half = s.angleDeg >= 360.f ? Geo::kPi : Geo::DegToRad(s.angleDeg) * 0.5f;
                    for (float k : { 1.f / 3.f, 2.f / 3.f })
                        for (float a : { -0.5f, -0.25f, 0.25f, 0.5f })
                            PushPolar(out, { pose.x, pose.y, pose.z }, f + a * half, s.length * k, pose.z);
                    return;
                }
                case ShapeType::Line:
                {
                    float const ux = std::cos(f);
                    float const uy = std::sin(f);
                    float const s0 = s.excludeStart + inset;
                    float const s1 = s.length - inset;
                    if (s0 > s1)
                        return;
                    float const bs = std::clamp(Geo::ToLineCoords(s, pose, in.bot).s, s0, s1);
                    out.push_back({ pose.x + ux * bs, pose.y + uy * bs, pose.z });
                    for (float a = s0; a <= s1 + 1e-3f; a += kRegionLineStepYd)
                        out.push_back({ pose.x + ux * a, pose.y + uy * a, pose.z });
                    return;
                }
            }
        }

        // Carried-shape candidates: 8 bearings in the half-plane away from the counted allies' centroid, at the
        // shape's reach + margin + {0.5, 3, 6} beyond the farthest of them (so each clears every counted ally). A
        // line aimed at the bot adds its arc around the source (±10..45°) and points beyond its length.
        inline void CarriedCandidates(Input const& in, Carried const& c, std::vector<Geo::Vec3>& out)
        {
            Geo::Vec3 const& bot = in.bot;
            float cx = 0.f;
            float cy = 0.f;
            std::uint32_t n = 0;
            for (std::uint32_t idx : c.allies)
                if (idx < in.allies.size())
                {
                    cx += in.allies[idx].p.x;
                    cy += in.allies[idx].p.y;
                    ++n;
                }
            if (n)
            {
                Geo::Vec3 const centroid{ cx / n, cy / n, bot.z };
                float spread = 0.f;
                for (std::uint32_t idx : c.allies)
                    if (idx < in.allies.size())
                        spread = std::max(spread, Dist2(centroid, in.allies[idx].p) + in.allies[idx].slackYd);
                float const base = Dist2(bot, centroid) > 1e-3f
                    ? std::atan2(bot.y - centroid.y, bot.x - centroid.x) : 0.f;
                float const reach = spread + Geo::Bounds(c.shape) + c.marginYd;
                for (std::uint32_t i = 0; i < 8; ++i)
                {
                    float const bearing = base - Geo::kPi * 0.5f + Geo::kPi * i / 7.f;
                    for (float extra : { 0.5f, 3.f, 6.f })
                        PushPolar(out, centroid, bearing, reach + extra, bot.z);
                }
            }
            if (c.mode != Carried::Mode::AimedAtBot)
                return;
            Geo::Vec3 const src{ c.source.x, c.source.y, bot.z };
            float const d = Dist2(src, bot);
            float const bearing = d > 1e-3f ? std::atan2(bot.y - src.y, bot.x - src.x) : c.source.o;
            for (float deg : { 10.f, 20.f, 30.f, 45.f })
                for (float sign : { 1.f, -1.f })
                    PushPolar(out, src, bearing + sign * Geo::DegToRad(deg), d, bot.z);
            float const beyond = Geo::Bounds(c.shape) + c.marginYd;
            for (float extra : { 0.5f, 3.f })
                PushPolar(out, src, bearing, beyond + extra, bot.z);
        }

        // From `c` along `bearing`, the first point more than `clear` outside every pose of the shape (0.25 yd
        // steps); false when none within the shape's reach + clear + 2 yd.
        inline bool MarchOut(Shape const& s, std::vector<Geo::Pose> const& poses, Geo::Vec3 const& c, float bearing,
            float clear, Geo::Vec3& out)
        {
            float const ux = std::cos(bearing);
            float const uy = std::sin(bearing);
            float const limit = 2.f * Geo::Bounds(s) + clear + 2.f;
            for (float r = 0.f; r <= limit; r += kMarchStepYd)
            {
                Geo::Vec3 const p{ c.x + ux * r, c.y + uy * r, c.z };
                if (MinSd(s, poses, p) > clear)
                {
                    out = p;
                    return true;
                }
            }
            return false;
        }

        // Parking candidates (§3.7): inside = points just outside the zone on 16 bearings around its middle, and on
        // its far side from the boss (+0, +1, +2 yd); drag out = points on 16 bearings around each avoided shape far
        // enough that the boss's stop clears it by depthYd (+0, +1.5 yd).
        inline void ParkCandidates(Input const& in, std::vector<Geo::Vec3>& out)
        {
            Park const& pk = in.park;
            auto ring = [&](Shape const& s, std::vector<Geo::Pose> const& poses, float clear,
                std::initializer_list<float> extras, bool farSide)
            {
                if (poses.empty())
                    return;
                Geo::Vec3 const c = ShapeCenter(s, poses[poses.size() / 2]);
                auto push = [&](float bearing, std::initializer_list<float> list)
                {
                    Geo::Vec3 p;
                    if (!MarchOut(s, poses, c, bearing, clear, p))
                        return;
                    for (float extra : list)
                        out.push_back({ p.x + std::cos(bearing) * extra, p.y + std::sin(bearing) * extra, in.bot.z });
                };
                for (std::uint32_t i = 0; i < kRingBearings; ++i)
                    push(Geo::kTwoPi * i / kRingBearings, extras);
                if (farSide && Dist2(c, pk.boss) > 1e-3f)
                    push(std::atan2(c.y - pk.boss.y, c.x - pk.boss.x), { 0.f, 1.f, 2.f });
            };
            if (pk.inside)
            {
                ring(pk.zone.shape, pk.zone.poses, pk.zone.marginYd + kExitPushYd, { 0.f }, true);
                return;
            }
            for (Hazard const& h : pk.avoid)
                ring(h.shape, h.poses, pk.depthYd + pk.stopYd + kExitPushYd, { 0.f, 1.5f }, false);
        }

        // ---- Drop spot (Stratholme build X2; the terms and their order are in MechanicDropSpot.h) ----

        // The carried shape the drop rule is for, while it lives; nullptr otherwise.
        inline Carried const* DropCarried(Input const& in)
        {
            if (!in.drop.present)
                return nullptr;
            for (Carried const& c : in.carried)
                if (c.id == in.drop.id && CarriedLive(in, c))
                    return &c;
            return nullptr;
        }

        // The centroid of the allies the carried shape counts; false when it counts none.
        inline bool DropCentroid(Input const& in, Carried const& c, Geo::Vec3& out)
        {
            float x = 0.f;
            float y = 0.f;
            std::uint32_t n = 0;
            for (std::uint32_t idx : c.allies)
                if (idx < in.allies.size())
                {
                    x += in.allies[idx].p.x;
                    y += in.allies[idx].p.y;
                    ++n;
                }
            if (!n)
                return false;
            out = { x / n, y / n, in.bot.z };
            return true;
        }

        // The drop rule's score with the bot at p (lower is better): avoid points, range and room always, behind the
        // group always; then a preferred spot (a bonus, nothing else counts), or staging next to an enterable one,
        // else the room's edge and the distance from the group.
        inline float DropScore(Input const& in, Geo::Vec3 const& p)
        {
            namespace DS = BossMechanics::DropSpot;
            Carried const* c = DropCarried(in);
            if (!c)
                return 0.f;
            Drop const& d = in.drop;
            float s = 0.f;
            Geo::Pose const pose = CarriedPose(*c, p, c->fixedFacing);
            for (DropCircle const& a : d.avoid)
            {
                float const gap = a.r + DS::kAvoidPadYd - SignedDistance(c->shape, pose, { a.c.x, a.c.y, pose.z });
                if (gap > 0.f)
                    s += DS::kAvoidBase + DS::kAvoidPerYd * gap;
            }
            if (d.maxRangeYd > 0.f)
                s += DS::kRangePerYd * std::max(0.f, Dist2(d.rangeFrom, p) - d.maxRangeYd);
            float const room = DS::RoomDistance(d, p);
            if (room < 0.f)
                s += DS::kOutsideBase + DS::kOutsidePerYd * -room;
            Geo::Vec3 centroid;
            bool const group = DropCentroid(in, *c, centroid);
            if (d.behind && group)
            {
                float const reach = c->mode == Carried::Mode::OnBot ? Geo::Bounds(c->shape) : 0.f;
                s += DS::kBehindPerYd * std::max(0.f, DS::Ahead(centroid, d.travelFacing, p, reach));
            }
            if (DS::InsidePrefer(d, p))
                return s - DS::kPreferBonus;
            if (float const stage = DS::StageBonus(d, p); stage > 0.f)
                return s - stage;
            if (d.edge && std::isfinite(room))
                s += DS::kEdgePerYd * std::max(0.f, room - DS::kEdgeInsetYd);
            if (d.away && group)
                s -= DS::kAwayPerYd * std::min(Dist2(centroid, p), DS::kAwayCapYd);
            return s;
        }

        // Drop rule candidates within the walk before the landing: in and around each preferred spot (staging points
        // just outside an enterable one), along the room's edge, and away from the group.
        inline void DropCandidates(Input const& in, std::vector<Geo::Vec3>& out)
        {
            namespace DS = BossMechanics::DropSpot;
            Carried const* c = DropCarried(in);
            if (!c)
                return;
            Drop const& d = in.drop;
            Geo::Vec3 const& bot = in.bot;
            float const reach = Speed(in) * SecondsOf(std::max(Ms(0), d.landAt - in.now)) + 2.f;
            auto push = [&](Geo::Vec3 const& q)
            {
                if (Dist2(bot, q) <= reach)
                    out.push_back({ q.x, q.y, bot.z });
            };
            auto polar = [](Geo::Vec3 const& from, float bearing, float r)
            {
                return Geo::Vec3{ from.x + std::cos(bearing) * r, from.y + std::sin(bearing) * r, from.z };
            };
            for (DropCircle const& pr : d.prefer)
            {
                float const base = Dist2(bot, pr.c) > 1e-3f ? std::atan2(bot.y - pr.c.y, bot.x - pr.c.x) : 0.f;
                push(pr.c);
                for (float off : { 0.f, 0.5f, -0.5f, 1.f, -1.f })
                {
                    for (float r : { pr.r - 1.f, pr.r * 0.5f })
                        if (r > 0.f)
                            push(polar(pr.c, base + off, r));
                    if (pr.enterable)
                        push(polar(pr.c, base + off, pr.r + c->marginYd + DS::kStageGapYd));
                }
            }
            if (d.edge)
                for (Geo::Vec3 const& q : DS::EdgePoints(d, DS::kEdgeInsetYd * 0.5f, bot.z))
                    push(q);
            Geo::Vec3 centroid;
            if (d.away && DropCentroid(in, *c, centroid))
            {
                float const base = Dist2(bot, centroid) > 1e-3f ? std::atan2(bot.y - centroid.y, bot.x - centroid.x)
                    : 0.f;
                for (std::uint32_t i = 0; i < DS::kAwayBearings; ++i)
                    for (float r : DS::kAwayRings)
                        push(polar(centroid, base - Geo::kPi * 0.5f
                            + Geo::kPi * static_cast<float>(i) / static_cast<float>(DS::kAwayBearings - 1), r));
            }
        }

        // All M3 candidates (targeted: they go before the blanket rings).
        inline void PlayerKindCandidates(Input const& in, float reach, std::vector<Geo::Vec3>& out)
        {
            for (Region const& r : in.regions)
                if (RegionLive(in, r))
                    RegionCandidates(in, r, out);
            bool carriedCircle = false;
            for (Carried const& c : in.carried)
            {
                if (!CarriedLive(in, c))
                    continue;
                CarriedCandidates(in, c, out);
                carriedCircle = carriedCircle || (c.mode == Carried::Mode::OnBot && c.shape.type == ShapeType::Circle);
            }
            DropCandidates(in, out);   // Stratholme X2: the drop rule's spots
            // Slot ring: 16 bearings on the role band's outer radius, rotated by the bot's slot among the carriers
            // (M2's role ring sits at mid-band, too tight for a spread around a target at reach).
            if (carriedCircle && in.role.present && in.params.slotRing)
            {
                float const r = std::max(in.role.minYd, in.role.maxYd - 0.05f);
                float const rot = in.slotCount ? Geo::kTwoPi * (in.slot % in.slotCount) / in.slotCount : 0.f;
                for (std::uint32_t i = 0; i < kRingBearings; ++i)
                    PushPolar(out, in.role.anchor, rot + Geo::kTwoPi * i / kRingBearings, r, in.role.anchor.z);
            }
            if (in.far.present)
            {
                float const base = Dist2(in.bot, in.far.from) > 1e-3f
                    ? std::atan2(in.bot.y - in.far.from.y, in.bot.x - in.far.from.x) : 0.f;
                for (std::uint32_t i = 0; i < 8; ++i)
                    for (float k : { 0.5f, 1.f })
                        PushPolar(out, in.bot, base - Geo::kPi * 0.5f + Geo::kPi * i / 7.f, reach * k, in.bot.z);
            }
            if (in.park.present)
                ParkCandidates(in, out);
        }
    }

    // Stratholme X2: the Zones of the drop's enterable preferred spots (`ids`: their mechanic ids) stop being hot at
    // drop.enterAt - marginMs (a plain or lasting window; a projectile or mine is left alone), so a target inside one
    // is safe from an arrival at enterAt on and the way in is free after it. A window that would end before it starts
    // is emptied. Idempotent: the view calls it after every player pass, and BuildInput rebuilds the hazards (whole)
    // once the drop's mechanic is gone.
    inline void LiftEnterable(Input& in, std::vector<std::uint32_t> const& ids)
    {
        if (!in.drop.present || ids.empty())
            return;
        Ms const end = in.drop.enterAt - in.params.marginMs;
        for (Hazard& h : in.hazards)
        {
            if (std::find(ids.begin(), ids.end(), h.id) == ids.end())
                continue;
            if (h.timing.type != TimingType::Interval && h.timing.type != TimingType::Instant)
                continue;
            if (end <= h.timing.from)
                h.timing.from = h.timing.to = Ms(0);
            else
                h.timing.to = std::min(h.timing.to, end);
        }
    }

    // Candidate targets: the bot's spot, exits, rings, the role ring, lane-frame points and the M3 points (regions,
    // carried shapes, the slot ring, FarFrom, parking: Detail::PlayerKindCandidates); deduplicated at 0.5 yd,
    // at most maxCandidates, returned nearest first (ties on x, y) with the bot's spot at [0]. The cap keeps the
    // targeted points (exits, role ring, analytic lane cells, free-lane centrelines) before the blanket ones (rings,
    // frame grid), each nearest first: a large frame grid must not crowd out a free lane 25 yd away.
    // fromLanes (M4 cost, optional): (*fromLanes)[i] = 1 when candidate i came from a lane frame.
    inline std::vector<Geo::Vec3> Candidates(Input const& in, std::vector<std::uint8_t>* fromLanes = nullptr)
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
        std::size_t const rawLane0 = raw.size();
        std::size_t const bulkLane0 = bulk.size();
        for (LaneFrame const& lf : in.lanes)
            Detail::FrameCandidates(in, lf, reach, m, raw, bulk);
        std::size_t const rawLane1 = raw.size();
        std::size_t const bulkLane1 = bulk.size();

        // Player kinds (M3): regions, carried shapes, the slot ring, FarFrom, parking.
        Detail::PlayerKindCandidates(in, reach, raw);

        // `lane` (M4 cost) rides along and never takes part in the order: the result is the same with or without it.
        struct Item
        {
            float d;
            Geo::Vec3 p;
            bool lane = false;
        };
        auto nearer = [](Item const& a, Item const& b)
        {
            if (a.d != b.d)
                return a.d < b.d;
            if (a.p.x != b.p.x)
                return a.p.x < b.p.x;
            return a.p.y < b.p.y;
        };
        auto items = [&bot, &nearer](std::vector<Geo::Vec3> const& pts, std::size_t lane0, std::size_t lane1)
        {
            std::vector<Item> v;
            v.reserve(pts.size());
            for (std::size_t i = 0; i < pts.size(); ++i)
                v.push_back({ Detail::Dist2(bot, pts[i]), pts[i], i >= lane0 && i < lane1 });
            std::sort(v.begin(), v.end(), nearer);
            return v;
        };
        std::vector<Item> ordered = items(raw, rawLane0, rawLane1);
        std::vector<Item> const blanket = items(bulk, bulkLane0, bulkLane1);
        ordered.insert(ordered.end(), blanket.begin(), blanket.end());

        std::vector<Item> out;
        std::uint32_t const cap = std::max<std::uint32_t>(1, in.params.maxCandidates);
        out.reserve(std::min<std::size_t>(cap, ordered.size() + 1));
        out.push_back({ 0.f, bot, false });

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
        if (fromLanes)
        {
            fromLanes->assign(out.size(), 0);
            for (std::size_t i = 0; i < out.size(); ++i)
                (*fromLanes)[i] = out[i].lane ? 1 : 0;
        }
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

        // M5 CD (C3 B): the spacing term is for bots with no melee band (melee share the boss), and off while a hard
        // region (a Stack, Cluster, Bait or Soak) is live: grouping mechanics stay as they are.
        inline bool SpacingApplies(Input const& in)
        {
            if (in.role.present && in.role.minYd <= 0.f)
                return false;
            for (Region const& r : in.regions)
                if (r.hard && RegionLive(in, r))
                    return false;
            return true;
        }

        inline float Score(Input const& in, Geo::Vec3 const& c, float dist, Ms arrival, Ms deadline, bool isBot)
        {
            Weights const& w = in.params.w;
            float score = w.path * dist;
            // Stratholme X2: a carrier with a drop rule leaves its role band for the drop (the rule's terms decide).
            bool const dropping = DropCarried(in) != nullptr;
            if (dropping)
                score += DropScore(in, c);
            else if (in.role.present)
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
            if (w.spacing > 0.f && !in.standing.empty() && SpacingApplies(in))
            {
                float nearest = kInf;
                for (Geo::Vec3 const& q : in.standing)
                    nearest = std::min(nearest, Dist2(q, c));
                score += w.spacing * std::max(0.f, in.params.spacingYd - nearest);
            }
            if (deadline < kForever && deadline > in.now)
            {
                float const slack = static_cast<float>((deadline - arrival).count());
                score += w.slackPerMs * std::max(0.f, kArriveSlackMs - slack);
            }
            if (isBot)
                score += w.stay;

            // M3: region comfort (per yard short of 1 yd inside), FarFrom, Reach.
            for (Region const& r : in.regions)
            {
                if (!RegionLive(in, r))
                    continue;
                float const sd = RegionSd(r.shape, r.poses, c);
                if (std::isfinite(sd))
                    score += w.region * std::max(0.f, 1.f - (-sd - r.marginYd));
            }
            if (in.far.present)
                score -= in.far.weightPerYd * Dist2(in.far.from, c);
            if (in.reach.weightPerYd > 0.f)
                for (Geo::Vec3 const& q : in.reach.points)
                    score += in.reach.weightPerYd * std::max(0.f, Dist2(q, c) - in.reach.maxYd);
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
            // M3: the live hard regions and carried shapes (the bot's own mechanics).
            for (Region const& r : in.regions)
                if (r.hard && RegionLive(in, r) && std::find(out.begin(), out.end(), r.id) == out.end())
                    out.push_back(r.id);
            for (Carried const& c : in.carried)
                if (CarriedLive(in, c) && std::find(out.begin(), out.end(), c.id) == out.end())
                    out.push_back(c.id);
            return out;
        }

        // Hold: through the windows of the hazards answered, and (M3) of the live hard regions and carried shapes
        // the target satisfies (capped at now + horizon).
        inline Ms HoldUntil(Input const& in, std::vector<std::uint32_t> const& answers, Ms arriveAt,
            Geo::Vec3 const& target)
        {
            Ms until = arriveAt;
            Ms const cap = in.now + in.params.horizon;
            for (Region const& r : in.regions)
                if (r.hard && RegionLive(in, r) && InsideRegion(r, target))
                    until = std::max(until, std::min(cap, r.to + in.params.marginMs));
            for (Carried const& c : in.carried)
            {
                float f = 0.f;
                if (CarriedLive(in, c) && FreeFacing(in, c, target, f))
                    until = std::max(until, std::min(cap, TimingEnd(c.timing) + in.params.marginMs));
            }
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

        // Does hazard `id` hold the bot's spot (within its margin)?
        inline bool HoldsBot(Input const& in, std::uint32_t id)
        {
            for (Hazard const& h : in.hazards)
                if (h.id == id)
                    return Within(h, in.bot);
            return false;
        }

        // Keeps the oracle's polyline starting at the bot.
        inline void AnchorPath(Geo::Vec3 const& from, std::vector<Geo::Vec3>& path)
        {
            if (path.empty() || Dist3(path.front(), from) > 0.1f)
                path.insert(path.begin(), from);
        }
    }

    namespace Detail
    {
        constexpr float kSightShadowYd = 2.f;       // a candidate this close to a blocked ray's line is cast late

        // Is q in the shadow of `blocked` as seen from the sight point `from`: on the far half of the ray from `from`
        // through `blocked` (the occluder lies somewhere on it), within kSightShadowYd of its line?
        inline bool InShadow(Geo::Vec3 const& from, Geo::Vec3 const& blocked, Geo::Vec3 const& q)
        {
            float const bx = blocked.x - from.x;
            float const by = blocked.y - from.y;
            float const len = std::sqrt(bx * bx + by * by);
            if (len < 1e-3f)
                return false;
            float const qx = q.x - from.x;
            float const qy = q.y - from.y;
            float const along = (qx * bx + qy * by) / len;
            if (along < 0.5f * len)
                return false;
            return std::fabs(qx * by - qy * bx) / len <= kSightShadowYd;
        }

        // Sight (STAGE-M3-R-REPORT M-3): at most Sight::maxChecks rays, the primary point (the heaviest weight: the
        // tank) first, one ray per candidate. (1) Best score first, skipping a candidate in the shadow of one already
        // found blocked, up to the first that sees the primary point: the rays spread out over the candidates instead
        // of all landing behind the same pillar. (2) The shadowed candidates that score better than that one, best
        // first (the shadow is a guess: one may see the point after all). (3) The rays left check the other points of
        // the candidates that see the primary point, best first. Penalties: a candidate that does not see the primary
        // point, or whose primary ray was not cast, counts every point (it ranks after every checked one that sees
        // it); one that sees it counts the other points found blocked (unchecked ones count as seen). Adds each kept
        // candidate's penalty to its score; `penalty` (indexed by candidate) gets it too, for a real path end the
        // oracle moved.
        inline void ScoreSight(Input const& in, PathOracle& oracle, std::vector<Scored>& kept,
            std::size_t candidates, std::vector<float>& penalty, std::uint32_t& raysOut)
        {
            std::sort(kept.begin(), kept.end(), ScoredLess);
            std::size_t const np = in.sight.points.size();
            auto weightOf = [&](std::size_t j) { return j < in.sight.weights.size() ? in.sight.weights[j] : 1.f; };
            std::size_t primary = 0;
            float total = 0.f;
            for (std::size_t j = 0; j < np; ++j)
            {
                total += weightOf(j);
                if (weightOf(j) > weightOf(primary))
                    primary = j;
            }
            Geo::Vec3 const& p0 = in.sight.points[primary];
            std::uint32_t const budget = in.sight.maxChecks;
            std::uint32_t rays = 0;
            std::vector<float> pen(kept.size(), total);
            std::vector<std::uint8_t> cast(kept.size(), 0);
            std::vector<Geo::Vec3> blocked;
            std::vector<std::size_t> seen;
            auto castPrimary = [&](std::size_t i)
            {
                ++rays;
                cast[i] = 1;
                if (oracle.InSight(kept[i].p, p0))
                {
                    pen[i] = 0.f;
                    seen.push_back(i);
                    return true;
                }
                blocked.push_back(kept[i].p);
                return false;
            };
            // (1)
            std::size_t firstSeen = kept.size();
            for (std::size_t i = 0; i < kept.size() && rays < budget; ++i)
            {
                bool shadowed = false;
                for (Geo::Vec3 const& b : blocked)
                    shadowed = shadowed || InShadow(p0, b, kept[i].p);
                if (!shadowed && castPrimary(i))
                {
                    firstSeen = i;
                    break;
                }
            }
            // (2)
            for (std::size_t i = 0; i < firstSeen && i < kept.size() && rays < budget; ++i)
                if (!cast[i])
                    castPrimary(i);
            std::sort(seen.begin(), seen.end());
            // (3)
            for (std::size_t i : seen)
                for (std::size_t j = 0; j < np && rays < budget; ++j)
                {
                    if (j == primary)
                        continue;
                    ++rays;
                    if (!oracle.InSight(kept[i].p, in.sight.points[j]))
                        pen[i] += weightOf(j);
                }
            penalty.assign(candidates, total);
            for (std::size_t i = 0; i < kept.size(); ++i)
            {
                kept[i].score += pen[i];
                penalty[kept[i].index] = pen[i];
            }
            raysOut = rays;
        }
    }

    namespace Detail
    {
        constexpr float kMinTightMarginYd = 0.1f;       // the reduced-margin pass never goes below this
        constexpr float kMovedYd = 0.05f;               // a probe or a path end moved the point by more than this
        constexpr std::uint32_t kFallbackProbes = 8;    // fallback: walkability probes down its ranking
        // ...so at most this many of its top entries are read (the probes, then one with a known path)
        constexpr std::uint32_t kFallbackRanked = kFallbackProbes + 1;
        constexpr std::uint32_t kStraightChecks = 64;   // straight-line checks per validation pass (cost cap)

        // The bounded Fallback ranking (M4 WP-Q G7, exact since STAGE-M4-R-REPORT m-3): with the top
        // kFallbackRanked entries full and `last` its worst, how far a candidate (`violated` broken kinds, `dist`
        // away) must be walked to tell whether it ranks ahead of `last` (time inside, then broken kinds, then
        // distance). `cap`: the most time inside that still ranks ahead; `walk` false when nothing can (it would
        // need less than no time inside). The distance is compared, not assumed: the candidates come sorted by 2D
        // distance and the ranking reads 3D, so a later one can be nearer on stairs or slopes.
        struct FallbackCap
        {
            Ms cap = kForever;
            bool walk = true;
        };

        inline FallbackCap FallbackWalkCap(Ms lastInside, std::uint32_t lastViolated, float lastDist,
            std::uint32_t violated, float dist)
        {
            FallbackCap out;
            if (violated < lastViolated || (violated == lastViolated && dist < lastDist))
                out.cap = lastInside;               // a tie on time inside ranks it ahead
            else if (lastInside > Ms(0))
                out.cap = lastInside - Ms(1);
            else
                out.walk = false;
            return out;
        }

        // The input of the reduced-margin pass: every margin cut to its drift + tightMarginYd. false = no margin
        // changes (the pass would repeat the first one).
        inline bool ReducedMargins(Input const& in, Input& out)
        {
            out = in;
            float const tight = std::max(kMinTightMarginYd, in.params.tightMarginYd);
            bool changed = false;
            for (Hazard& h : out.hazards)
            {
                if (h.lethal)
                    continue;       // X4: a lethal wall keeps its full margin in every pass
                float const m = std::min(h.marginYd, std::max(0.f, h.driftYd) + tight);
                changed = changed || m < h.marginYd - 1e-4f;
                h.marginYd = m;
            }
            return changed;
        }

        // Does any hazard allow a grace crossing (an armed or arming mine with a grace)? Without one the grace pass
        // could only repeat the clean pass.
        inline bool AnyGrace(Input const& in)
        {
            for (Hazard const& h : in.hazards)
                if ((h.timing.type == TimingType::MineArmed || h.timing.type == TimingType::MineArming)
                    && h.timing.grace > in.params.graceMargin && !h.lethal)
                    return true;
            return false;
        }

        // Stratholme X4: is any hazard lethal (MechanicRef::Lethal)? Without one the Fallback runs as before.
        inline bool AnyLethal(Input const& in)
        {
            for (Hazard const& h : in.hazards)
                if (h.lethal)
                    return true;
            return false;
        }

        // The input CrossesLethal checks against: the lethal hazards alone, each at its drift + tightMarginYd (never
        // below kMinTightMarginYd, never above its own margin). The Fallback already gave up on the full margins, so a
        // lethal wall is passed as close as the reduced-margin pass would pass anything, and never entered.
        inline Input LethalOnly(Input const& in)
        {
            Input out;
            out.bot = in.bot;
            out.speed = in.speed;
            out.now = in.now;
            out.params = in.params;
            float const tight = std::max(kMinTightMarginYd, in.params.tightMarginYd);
            for (Hazard const& h : in.hazards)
            {
                if (!h.lethal)
                    continue;
                Hazard c = h;
                c.marginYd = std::min(h.marginYd, std::max(0.f, h.driftYd) + tight);
                out.hazards.push_back(std::move(c));
            }
            return out;
        }

        // Does `path`, walked from `start` as CheckPath times it, enter a lethal hazard of `lethal` (LethalOnly)
        // while it is hot, or end inside one hot from its arrival to `until`? Leaving one the path starts in does not
        // count while the path is leaving (CheckPath's start exemption); its end must still lie outside.
        inline bool CrossesLethal(Input const& lethal, std::vector<Geo::Vec3> const& path, Ms start, Ms until)
        {
            if (lethal.hazards.empty() || path.empty())
                return false;
            PathCheck const pc = CheckPath(lethal, path, start, false);
            if (!pc.ok)
                return true;
            return !PointSafe(lethal, path.back(), pc.arriveAt, std::max(pc.arriveAt, until));
        }
    }

    // The input with every hazard margin cut to its drift + Params::tightMarginYd (what Solve's reduced-margin pass
    // checks paths against). The brain re-checks the rest of a pass-2 plan with it.
    inline Input ReducedMargins(Input const& in)
    {
        Input out;
        Detail::ReducedMargins(in, out);
        return out;
    }

    // Picks where the bot goes (§3.9 "Solve"). Deterministic for a given input and oracle.
    //
    // Validation order (each pass in score order, its own maxPathChecks budget of new oracle calls; paths, probes and
    // unreachable candidates are shared): (1) a clean path at the full margin -> Move; (2) a clean path at the reduced
    // margin (Params::tightMarginYd; the target itself stays safe at the full margin, so the bot only passes through
    // the narrow gap and never holds in it) -> Move, stats.pass 2; (3) armed-mine grace crossings at the full margin
    // -> GraceMove (only when a hazard allows grace); (4) Fallback. Before a candidate costs an oracle call it is
    // probed (PathOracle::Walkable): off the walkable ground it is dropped for free, snapped onto it it is re-checked
    // at its new place.
    //
    // M3: a frozen bot (Freeze, from freeze.from − settle to freeze.to) gets Safe at its spot, always. "Safe" is
    // TargetOk (the pre-filter and both end checks of the validation use it; regions' arrival rule at the real path
    // end). Sight rays (at most Sight::maxChecks) re-rank the best kept candidates. The Fallback ranks exact-hitbox
    // Zone exposure, then the hard regions and carried shapes broken (Plan::violated), then distance. A plan owing a
    // look-away facing carries it (hasFacing, facing), Safe ones included.
    inline Plan Solve(Input const& in, PathOracle& oracle)
    {
        Plan plan;
        plan.target = in.bot;
        plan.arriveAt = in.now;
        plan.holdUntil = in.now;
        Ms const horizonEnd = in.now + in.params.horizon;
        bool const facingOwed = Detail::AnyChoosesFacing(in);

        // 0. Frozen (HoldStill window, decision §7.4): never move, whatever else holds (a look-away facing still
        // comes with it when one is free).
        if (Detail::Frozen(in))
        {
            float f = 0.f;
            if (facingOwed && CarriedOk(in, in.bot, &f))
            {
                plan.hasFacing = true;
                plan.facing = f;
            }
            return plan;
        }

        // 1. Already safe (unless re-routing a motion; a role solve goes on and lets the spot compete).
        float spotFacing = 0.f;
        bool const spotOk = !in.reroute && TargetOk(in, in.bot, in.now, horizonEnd, &spotFacing);
        auto safeHere = [&](Plan p)
        {
            p.outcome = Outcome::Safe;
            p.target = in.bot;
            p.path.clear();
            p.arriveAt = in.now;
            p.holdUntil = in.now;
            p.hasFacing = facingOwed;
            p.facing = spotFacing;
            return p;
        };
        if (spotOk && !in.roleSolve)
            return safeHere(plan);

        // M4 cost (D9): which candidates the lane frames made (only asked when there is a lane frame).
        std::vector<std::uint8_t> fromLanes;
        std::vector<Geo::Vec3> const cands = Candidates(in, in.lanes.empty() ? nullptr : &fromLanes);
        plan.candidates = static_cast<std::uint32_t>(cands.size());
        for (std::uint8_t f : fromLanes)
            plan.stats.laneCandidates += f;
        auto fromLane = [&fromLanes](std::uint32_t i) { return i < fromLanes.size() && fromLanes[i]; };
        float const speed = Detail::Speed(in);
        Ms const deadline = Detail::Deadline(in);
        Ms const moveStart = in.now + in.params.startDelay;

        // 2. Straight-line pre-filter and score. Targets are always safe at the full margin (every pass) and pass
        // the player kinds (TargetOk); hazardFree remembers the hazard part alone for the Fallback.
        std::vector<Detail::Scored> kept;
        kept.reserve(cands.size());
        std::vector<std::int8_t> hazardFree(cands.size(), 0);
        float const rerouteMax = in.reroute && in.role.present
            ? Detail::Dist2(in.bot, in.role.anchor) - in.params.rerouteGainYd : Detail::kInf;
        for (std::uint32_t i = 0; i < cands.size(); ++i)
        {
            if (i == 0 && in.reroute)
                continue;
            Geo::Vec3 const& c = cands[i];
            if (in.reroute && Detail::Dist2(c, in.role.anchor) > rerouteMax)
                continue;
            float const dist = Detail::Dist3(in.bot, c);
            Ms const arrival = moveStart + Detail::TravelMs(dist, speed);
            if (!Detail::HazardsOk(in, c, arrival, horizonEnd))
                continue;
            hazardFree[i] = 1;
            if (!Detail::PlayerKindsOk(in, c, arrival, nullptr))
                continue;
            // F-1b: a moved-to target keeps the hard bounds (the bot's own spot never fails them).
            if (i != 0 && !BossMechanics::DropSpot::InBounds(in.bounds, c))
            {
                ++plan.stats.outOfBounds;
                continue;
            }
            kept.push_back({ i, dist, Detail::Score(in, c, dist, arrival, deadline, i == 0), c });
        }

        // Sight (M3, soft; STAGE-M3-R-REPORT M-3): at most Sight::maxChecks rays per solve, the primary point (the
        // heaviest weight: the tank) first. One ray per candidate, best score first, skipping a candidate in the
        // shadow of one already found blocked (Detail::InShadow), until kSightSeen candidates see the primary point;
        // the rays left then check the other points of those, best first. Penalties: a candidate that does not see
        // the primary point, or whose primary ray was not cast, counts every point (so it ranks after every checked
        // one that sees it); one that sees it counts the other points found blocked (unchecked ones count as seen).
        std::vector<float> sightPenalty;
        if (!in.sight.points.empty() && !kept.empty())
            Detail::ScoreSight(in, oracle, kept, cands.size(), sightPenalty, plan.sightChecks);
        std::sort(kept.begin(), kept.end(), Detail::ScoredLess);
        plan.stats.kept = static_cast<std::uint32_t>(kept.size());

        // 3. Validate in score order. Shared across the passes: oracle paths, unreachable candidates and the probes
        // (point[i] is candidate i after its probe).
        std::unordered_map<std::uint32_t, std::vector<Geo::Vec3>> paths;
        std::unordered_set<std::uint32_t> unreachable;
        std::vector<std::int8_t> probed(cands.size(), 0);      // 0 not yet, 1 walkable
        std::vector<Geo::Vec3> point = cands;
        std::uint32_t probes = 0;
        // Oracle calls are capped (maxPathChecks), so candidates whose straight line already fails the path check
        // (across a start hazard towards a role anchor on its far side, STAGE-M2-REVIEW B1; or across a neighbouring
        // hazard that will be hot by then, the common case under a barrage) are tried only when no candidate whose
        // straight line passes validated: the cap must not be spent on them while a clean way out exists. Tier 0 runs
        // in score order with the exact early stop; a tier-1 candidate could only beat its result by a path that bends
        // around the hazard its straight line crosses, which is not worth up to maxPathChecks more oracle calls.
        // Straight lines that passed the clean full-margin check pass the reduced-margin one too (a smaller region).
        std::vector<std::int8_t> cleanStraight(cands.size(), -1);
        // pin: the input the paths are checked against (the reduced margins in pass 2); targets use `in`.
        auto validate = [&](Input const& pin, bool allowGrace, int pass, Plan& out) -> bool
        {
            bool found = false;
            float bestScore = 0.f;
            Detail::Scored best;
            std::uint32_t checks = 0;
            // -1 unknown, 0 fails, 1 passes. Past kStraightChecks the tier-0 scan ends and the unknown ones are
            // tried in tier 1, in score order like the rest.
            std::vector<std::int8_t> straight(cands.size(), -1);
            if (pass == 2)
                for (std::size_t i = 0; i < cands.size(); ++i)
                    if (cleanStraight[i] == 1 && Detail::Dist3(point[i], cands[i]) <= Detail::kMovedYd)
                        straight[i] = 1;
            std::uint32_t straightRuns = 0;
            auto tierOf = [&](std::uint32_t i) -> int
            {
                if (straight[i] < 0 && straightRuns < Detail::kStraightChecks)
                {
                    ++straightRuns;
                    straight[i] = CheckPath(pin, { in.bot, point[i] }, in.now, allowGrace).ok ? 1 : 0;
                }
                return straight[i] == 1 ? 0 : (straight[i] == 0 ? 1 : -1);
            };
            for (int tier = 0; tier < 2 && !found; ++tier)
            {
                for (Detail::Scored const& s : kept)
                {
                    std::uint32_t const i = s.index;
                    if (found && bestScore <= s.score)
                        break;
                    if (unreachable.count(i))
                        continue;
                    auto it = paths.find(i);
                    if (it == paths.end() && checks >= in.params.maxPathChecks)
                    {
                        ++out.stats.budgetStops;
                        break;
                    }
                    int const t = tierOf(i);
                    if (t < 0 && tier == 0)
                        break;
                    if (t >= 0 && t != tier)
                        continue;
                    if (it == paths.end())
                    {
                        // Probe first: a candidate inside a wall costs no path check.
                        if (!probed[i] && probes < in.params.maxProbes)
                        {
                            ++probes;
                            ++out.stats.probes;
                            Geo::Vec3 q = point[i];
                            if (!oracle.Walkable(q))
                            {
                                ++out.stats.offMesh;
                                unreachable.insert(i);
                                continue;
                            }
                            probed[i] = 1;
                            if (Detail::Dist3(q, point[i]) > Detail::kMovedYd)
                            {
                                ++out.stats.snapped;
                                point[i] = q;
                                straight[i] = -1;
                                if (tier == 0 && tierOf(i) != 0)
                                    continue;   // tier 1 retries it
                            }
                        }
                        if (Detail::Dist3(point[i], cands[i]) > Detail::kMovedYd)
                        {
                            Ms const at = moveStart + Detail::TravelMs(Detail::Dist3(in.bot, point[i]), speed);
                            if (!TargetOk(in, point[i], at, horizonEnd))
                            {
                                ++out.stats.endUnsafe;
                                continue;
                            }
                        }
                        ++checks;
                        ++out.pathChecks;
                        if (fromLane(i))
                            ++out.stats.lanePathChecks;
                        std::vector<Geo::Vec3> path;
                        if (!oracle.Find(in.bot, point[i], path) || path.empty())
                        {
                            ++out.stats.noPath;
                            unreachable.insert(i);
                            continue;
                        }
                        Detail::AnchorPath(in.bot, path);
                        it = paths.emplace(i, std::move(path)).first;
                    }
                    std::vector<Geo::Vec3> const& path = it->second;
                    float const len = Detail::PathLength(path);
                    Ms const arrival = moveStart + Detail::TravelMs(len, speed);
                    if (!TargetOk(in, path.back(), arrival, horizonEnd))
                    {
                        ++out.stats.endUnsafe;
                        continue;
                    }
                    // F-1b: the real end (a snap or the oracle may move it) and, with pathInside, the walk.
                    if (!in.bounds.empty() && Detail::Dist3(path.back(), in.bot) > Detail::kMovedYd
                        && (!BossMechanics::DropSpot::InBounds(in.bounds, path.back())
                            || !BossMechanics::DropSpot::PathInBounds(in.bounds, path)))
                    {
                        ++out.stats.outOfBounds;
                        continue;
                    }
                    PathCheck const pc = CheckPath(pin, path, in.now, allowGrace);
                    if (!pc.ok)
                    {
                        ++(Detail::HoldsBot(pin, pc.firstHit) ? out.stats.crossStart : out.stats.crossOther);
                        continue;
                    }
                    // The real end is scored where it is when the probe or the oracle moved it.
                    float const real = Detail::Dist3(path.back(), s.p) > Detail::kMovedYd
                        ? Detail::Score(in, path.back(), len, arrival, deadline, false)
                            + (sightPenalty.empty() ? 0.f : sightPenalty[i])
                        : s.score + in.params.w.path * (len - s.straight);
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
            if (pass == 1)
                cleanStraight = straight;
            return found;
        };

        Plan work = plan;
        Input tightIn;
        bool found = false;
        if (validate(in, false, 1, work))
        {
            work.outcome = Outcome::Move;
            work.stats.pass = 1;
            found = true;
        }
        else if (Detail::ReducedMargins(in, tightIn) && validate(tightIn, false, 2, work))
        {
            work.outcome = Outcome::Move;
            work.stats.pass = 2;
            found = true;
        }
        else if (Detail::AnyGrace(in) && validate(in, true, 3, work))
        {
            work.outcome = Outcome::GraceMove;
            work.stats.pass = 3;
            found = true;
        }
        if (in.roleSolve && spotOk && (!found || Detail::Dist3(work.target, in.bot) <= Detail::kMovedYd))
        {
            // A role solve that found nothing better than the spot it already holds.
            plan.candidates = work.candidates;
            plan.pathChecks = work.pathChecks;
            plan.stats = work.stats;
            plan.sightChecks = work.sightChecks;
            return safeHere(plan);
        }
        if (!found && in.reroute)
        {
            // No clean way around: the caller keeps the bot where it is (no fallback for a motion).
            plan.candidates = work.candidates;
            plan.pathChecks = work.pathChecks;
            plan.stats = work.stats;
            return plan;
        }
        if (!found)
        {
            // 4. Fallback: fewest hazards hot from the straight-line arrival, then least time inside on the straight
            // line (scanned for the kFallbackScans nearest such candidates only), then nearest. Candidates known to
            // have no path are skipped; down that ranking, the first walkable one is taken (an oracle path already
            // found for it is reused).
            // The pre-filter's hazard-free candidates are exactly the ones with no hazard hot from their arrival on,
            // so the counts are only needed when there is none (or none of them has a path). M3: among equals, the
            // fewest broken hard regions and carried shapes come before the distance (decision §7.2: Zones first).
            // Stratholme X4: a candidate whose straight line enters a lethal hazard while it is hot (or that ends in
            // one) is out of the ranking altogether, like one known to have no path. Screened lazily (lethalBad): only
            // the candidates the ranking reaches pay for the walk.
            bool const lethalOn = Detail::AnyLethal(in);
            Input const lethalIn = lethalOn ? Detail::LethalOnly(in) : Input();
            std::vector<std::int8_t> lethalMemo(lethalOn ? cands.size() : 0, -1);
            auto lethalBad = [&](std::uint32_t i)
            {
                if (!lethalOn)
                    return false;
                if (lethalMemo[i] < 0)
                {
                    lethalMemo[i] = Detail::CrossesLethal(lethalIn, { in.bot, cands[i] }, in.now, horizonEnd) ? 1 : 0;
                    if (lethalMemo[i])
                    {
                        unreachable.insert(i);
                        ++work.stats.lethalSkipped;
                    }
                }
                return lethalMemo[i] == 1;
            };
            std::vector<std::uint32_t> counts(cands.size(), 1);
            std::uint32_t minCount = std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t i = 0; i < cands.size(); ++i)
                if (hazardFree[i] && !unreachable.count(i))
                    counts[i] = 0;
            // The ranking's floor: a hazard-free candidate (X4: one that is lethal-free too).
            for (std::uint32_t i = 0; i < cands.size() && minCount != 0; ++i)
                if (hazardFree[i] && !unreachable.count(i) && !lethalBad(i))
                    minCount = 0;
            if (minCount != 0)
            {
                for (std::uint32_t i = 0; i < cands.size(); ++i)
                {
                    if (unreachable.count(i))
                        continue;
                    Geo::Vec3 const& c = cands[i];
                    Ms const arrival = moveStart + Detail::TravelMs(Detail::Dist3(in.bot, c), speed);
                    counts[i] = 0;
                    for (Hazard const& h : in.hazards)
                        if (Detail::HotOver(h, c, arrival, std::max(arrival, horizonEnd), in.params.marginMs))
                            ++counts[i];
                    if (!lethalOn)
                        minCount = std::min(minCount, counts[i]);
                }
                if (lethalOn)
                {
                    // The fewest hot hazards among the lethal-free candidates, screened fewest first.
                    std::vector<std::uint32_t> order;
                    order.reserve(cands.size());
                    for (std::uint32_t i = 0; i < cands.size(); ++i)
                        if (!unreachable.count(i))
                            order.push_back(i);
                    std::stable_sort(order.begin(), order.end(),
                        [&counts](std::uint32_t a, std::uint32_t b) { return counts[a] < counts[b]; });
                    for (std::uint32_t i : order)
                        if (!lethalBad(i))
                        {
                            minCount = counts[i];
                            break;
                        }
                }
            }
            struct Ranked
            {
                Ms inside;
                std::uint32_t violated;
                float dist;
                std::uint32_t index;
            };
            bool const kinds = Detail::HasPlayerKinds(in);
            // Ranked by the time inside the exact hitboxes (drift kept, no yard or time margin): nothing clean
            // exists at the margins, so the least real exposure wins, not the least time in the margin band.
            Input exact = in;
            exact.params.marginMs = Ms(0);
            for (Hazard& h : exact.hazards)
                h.marginYd = std::max(0.f, h.driftYd);
            auto rankedLess = [](Ranked const& a, Ranked const& b)
            {
                if (a.inside != b.inside)
                    return a.inside < b.inside;
                if (a.violated != b.violated)
                    return a.violated < b.violated;
                return a.dist < b.dist;
            };
            std::vector<Ranked> ranked;
            ranked.reserve(Detail::kFallbackScans);
            // M4 WP-Q (G7, the lane-set Fallbacks of the sweep): only the top kFallbackRanked entries of the ranking
            // are ever read (the probes down it, then one with a known path). Once the top is full, a candidate's walk
            // stops as soon as it cannot rank ahead of the top's worst (WalkPath's measureCap), or is skipped when it
            // cannot at all (Detail::FallbackWalkCap). The top, all this solve reads, stays exact.
            std::vector<Ranked> top;    // the kFallbackRanked best so far, in ranking order
            top.reserve(Detail::kFallbackRanked + 1);
            for (std::uint32_t i = 0; i < cands.size() && ranked.size() < Detail::kFallbackScans; ++i)
            {
                if (counts[i] != minCount || unreachable.count(i) || lethalBad(i))
                    continue;
                Geo::Vec3 const& c = cands[i];
                float const dist = Detail::Dist3(in.bot, c);
                // F-1b: a target outside the hard bounds ranks like one more broken rule.
                std::uint32_t const violated = (kinds
                    ? Detail::Violations(in, c, moveStart + Detail::TravelMs(dist, speed)) : 0)
                    + (i != 0 && !BossMechanics::DropSpot::InBounds(in.bounds, c) ? 1u : 0u);
                Ms cap = kForever;
                bool walk = true;
                if (top.size() >= Detail::kFallbackRanked)
                {
                    Ranked const& last = top.back();
                    Detail::FallbackCap const fc = Detail::FallbackWalkCap(last.inside, last.violated, last.dist,
                        violated, dist);
                    cap = fc.cap;
                    walk = fc.walk;
                }
                Ms const inside = walk
                    ? Detail::WalkPath(exact, { in.bot, c }, in.now, true, true, cap).timeInsideHot : kForever;
                Ranked const r{ inside, violated, dist, i };
                ranked.push_back(r);
                if (walk && inside <= cap)
                {
                    top.insert(std::upper_bound(top.begin(), top.end(), r, rankedLess), r);
                    if (top.size() > Detail::kFallbackRanked)
                        top.pop_back();
                }
            }
            std::stable_sort(ranked.begin(), ranked.end(), rankedLess);

            // Default: the top of the ranking (as before the probes), also when every probe down it fails.
            Geo::Vec3 target = !ranked.empty() ? point[ranked.front().index] : (cands.empty() ? in.bot : cands[0]);
            std::uint32_t targetIdx = !ranked.empty() ? ranked.front().index : 0;
            std::vector<Geo::Vec3> path;
            bool havePath = false;
            std::uint32_t tries = 0;
            if (lethalOn)
            {
                // X4: down the ranking, the first candidate whose real path (known, else the oracle's, at most
                // kFallbackProbes new ones) keeps out of every lethal hazard. None: NoPath below, on a clear line.
                for (Ranked const& r : ranked)
                {
                    std::uint32_t const i = r.index;
                    std::vector<Geo::Vec3> p;
                    if (auto it = paths.find(i); it != paths.end())
                        p = it->second;
                    else
                    {
                        if (tries >= Detail::kFallbackProbes)
                            break;
                        ++tries;
                        Geo::Vec3 q = point[i];
                        if (!probed[i])
                        {
                            ++work.stats.probes;
                            if (!oracle.Walkable(q))
                            {
                                ++work.stats.offMesh;
                                continue;
                            }
                            if (Detail::Dist3(q, point[i]) > Detail::kMovedYd)
                                ++work.stats.snapped;
                        }
                        ++work.pathChecks;
                        if (fromLane(i))
                            ++work.stats.lanePathChecks;
                        if (!oracle.Find(in.bot, q, p) || p.empty())
                        {
                            ++work.stats.noPath;
                            continue;
                        }
                        Detail::AnchorPath(in.bot, p);
                    }
                    if (Detail::CrossesLethal(lethalIn, p, in.now, horizonEnd))
                    {
                        ++work.stats.lethalSkipped;
                        continue;
                    }
                    path = std::move(p);
                    target = path.back();
                    targetIdx = i;
                    havePath = true;
                    break;
                }
                // No lethal-free path down the ranking: NoPath's exits only (a candidate whose path was refused, or
                // never asked for, is not walked to blind).
                if (!havePath)
                    target = in.bot;
            }
            for (Ranked const& r : ranked)
            {
                if (lethalOn)
                    break;
                std::uint32_t const i = r.index;
                if (auto it = paths.find(i); it != paths.end())
                {
                    path = it->second;
                    havePath = true;
                    break;
                }
                if (tries >= Detail::kFallbackProbes)
                    break;
                ++tries;
                Geo::Vec3 q = point[i];
                if (!probed[i])
                {
                    ++work.stats.probes;
                    if (!oracle.Walkable(q))
                    {
                        ++work.stats.offMesh;
                        continue;
                    }
                    if (Detail::Dist3(q, point[i]) > Detail::kMovedYd)
                        ++work.stats.snapped;
                }
                target = q;
                targetIdx = i;
                break;
            }
            if (!havePath && !lethalOn)
            {
                ++work.pathChecks;
                if (fromLane(targetIdx))
                    ++work.stats.lanePathChecks;
                havePath = oracle.Find(in.bot, target, path) && !path.empty();
                if (!havePath)
                    ++work.stats.noPath;
            }
            if (havePath)
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
                // X4: with a lethal hazard about, only a line that keeps out of it; the chosen candidate's too, and
                // when none does the bot stays where it is (a wall is never walked into, whatever else is hot).
                auto lethalFree = [&](Geo::Vec3 const& to)
                {
                    return !lethalOn || !Detail::CrossesLethal(lethalIn, { in.bot, to }, in.now, horizonEnd);
                };
                Geo::Vec3 to = target;
                if (!lethalFree(to))
                {
                    ++work.stats.lethalSkipped;
                    to = in.bot;
                }
                float best = Detail::kInf;
                for (Geo::Vec3 const& e : exits)
                {
                    if (!lethalFree(e))
                    {
                        ++work.stats.lethalSkipped;
                        continue;
                    }
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
            // M4 §3.7: the exposure of the path taken, at the exact hitboxes (a clear or a hot fallback).
            work.exposure = Detail::WalkPath(exact, path, in.now, true, true).timeInsideHot;
            if (kinds)
                work.violated = Detail::Violations(in, work.target, work.arriveAt);
        }

        // 5. The facing a chooseFacing carried shape needs at the target (M3, look away).
        if (facingOwed)
        {
            float f = 0.f;
            bool ok = CarriedOk(in, work.target, &f);
            if (!ok)
            {
                // Fallback: the free facing of the first such shape alone, else its preferred one.
                for (Carried const& c : in.carried)
                    if (Detail::CarriedLive(in, c) && Detail::ChoosesFacing(c))
                    {
                        if (!FreeFacing(in, c, work.target, f))
                            f = Geo::WrapPi(c.preferredFacing);
                        break;
                    }
            }
            work.hasFacing = true;
            work.facing = f;
        }

        // 6. What the plan answers and how long to hold.
        work.answers = Detail::Answers(in, work.path);
        work.holdUntil = Detail::HoldUntil(in, work.answers, work.arriveAt, work.target);
        return work;
    }

    // M4 §3.7 (the barrage's Fallback share): may a Fallback or NoPath escape walk on although the rest of its way
    // fails the margins? Such a path never passed them (that is what made it a Fallback), so the escape re-check
    // measured it against a bar it could not clear and solved again on every view rebuild, nearly always to another
    // Fallback (STAGE-M4-P-REPORT §3). It holds when the rest of the way from where the bot stands now (`rest`, the
    // motion under way: no start delay) spends no more time inside the exact hitboxes while they are hot (drift
    // kept, no yard or time margin: Plan::exposure's measure) than the plan did, and its end lies outside every
    // exact hitbox while hot from the arrival on to `horizonEnd`. `clearOnly`: only a clear plan (exposure 0)
    // holds; a hot one is solved again (a re-solve may still find it a clean way). A plan that breaks a hard region
    // (violated > 0: a Stack, a Cluster, the Bait's others range) never holds (STAGE-M4-R-REPORT m-4): it is solved
    // again on every view change, so a Move that became possible (an ally moved away) is found, and the solve asks
    // for its region route again.
    inline bool FallbackHolds(Input const& in, Plan const& plan, std::vector<Geo::Vec3> const& rest, Ms horizonEnd,
        bool clearOnly)
    {
        if (plan.outcome != Outcome::Fallback && plan.outcome != Outcome::NoPath)
            return false;
        if (plan.violated > 0)
            return false;
        if (rest.empty() || (clearOnly && plan.exposure > Ms(0)))
            return false;
        // X4: the rest of the way enters a lethal hazard (it armed, or moved, since the plan): solve again.
        if (Detail::AnyLethal(in))
        {
            Input lethal = Detail::LethalOnly(in);
            lethal.params.startDelay = Ms(0);
            if (Detail::CrossesLethal(lethal, rest, in.now, horizonEnd))
                return false;
        }
        Input exact;
        exact.bot = in.bot;
        exact.speed = in.speed;
        exact.now = in.now;
        exact.params = in.params;
        exact.params.marginMs = Ms(0);
        exact.params.startDelay = Ms(0);
        exact.hazards = in.hazards;
        for (Hazard& h : exact.hazards)
            h.marginYd = std::max(0.f, h.driftYd);
        PathCheck const pc = Detail::WalkPath(exact, rest, in.now, true, true);
        // One densification step of travel: the rest is sampled from another phase than the plan was.
        if (pc.timeInsideHot > plan.exposure + Detail::TravelMs(Detail::kStepYd, Detail::Speed(in)))
            return false;
        return PointSafe(exact, plan.target, pc.arriveAt, std::max(pc.arriveAt, horizonEnd));
    }
}

#endif
