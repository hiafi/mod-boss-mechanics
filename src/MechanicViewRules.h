/*
 * mod-boss-mechanics: the pure rules the bots' view (MechanicBotView) uses to turn the player kinds into solver input
 * (dungeon-mechanics STAGE-M3-HANDOFF §3.2, §3.4, §3.6, WP-V).
 *
 * No AzerothCore includes: the view resolves units into the plain values below, and tests/view_rules_test.cpp compiles
 * these with the host compiler (and runs them through the solver). Distances are yards, times absolute GameTime ms.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_VIEW_RULES_H
#define MOD_BOSS_MECHANICS_MECHANIC_VIEW_RULES_H

#include "MechanicSolver.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace BossMechanics::ViewRules
{
    using Ms = std::chrono::milliseconds;

    // A bot with no plan yet: its current spot plus this (it is about to plan; others re-plan on its reservation).
    constexpr float kUnplannedBotSlackYd = 0.5f;
    // Bait: the picker stays this much beyond the farthest other player (§3.4).
    constexpr float kBaitLeadYd = 2.f;
    // A region's margin and drift never take more than this share of its inner radius, so it never vanishes.
    constexpr float kRegionShrinkShare = 0.5f;

    // Radius of the largest circle inside the shape (how deep "inside" can go).
    inline float InRadius(Shape const& s)
    {
        switch (s.type)
        {
            case ShapeType::Circle:
                return std::max(0.f, s.radius);
            case ShapeType::Donut:
                return std::max(0.f, (s.radius - s.inner) * 0.5f);
            case ShapeType::Cone:
            {
                if (s.angleDeg >= 360.f)
                    return std::max(0.f, s.length);
                float const half = Geo::DegToRad(std::max(0.f, s.angleDeg)) * 0.5f;
                if (half >= Geo::kPi * 0.5f)
                    return std::max(0.f, s.length * 0.5f);
                float const sn = std::sin(half);
                return std::max(0.f, s.length * sn / (1.f + sn));
            }
            case ShapeType::Line:
                return std::max(0.f, std::min(s.width * 0.5f, (s.length - s.excludeStart) * 0.5f));
        }
        return 0.f;
    }

    // A region's margin: the base safety margin, at most kRegionShrinkShare of its inner radius.
    inline float RegionMarginYd(Shape const& s, float baseYd)
    {
        return std::clamp(baseYd, 0.f, InRadius(s) * kRegionShrinkShare);
    }

    // How far a region may drift with its moving anchor (the second pose, below), capped the same way.
    inline float RegionDriftYd(Shape const& s, float driftYd)
    {
        return std::clamp(driftYd, 0.f, InRadius(s) * kRegionShrinkShare);
    }

    // A region on a moving anchor: the live pose and the pose `driftYd` along its heading. Regions intersect their
    // poses, so the bot stands where it stays inside while the anchor walks on (WP-A: drift as poses, not margin).
    inline std::vector<Geo::Pose> MovingPoses(Geo::Pose const& live, float headingRad, float driftYd)
    {
        std::vector<Geo::Pose> poses{ live };
        if (driftYd > 0.01f)
        {
            Geo::Pose ahead = live;
            ahead.x += std::cos(headingRad) * driftYd;
            ahead.y += std::sin(headingRad) * driftYd;
            poses.push_back(ahead);
        }
        return poses;
    }

    // Where another player will stand at a mechanic's judge time (§3.4 "planned spot").
    enum class SpotSource : std::uint8_t
    {
        Reservation,    // its live reservation on this mechanic
        Plan,           // a bot's plan target (Escape, Hold, Freeze), from the glue
        BotNow,         // a bot with no plan: its current spot, slack for its motion
        HumanNow        // a human: its current spot plus HumanSlackYd (decision §7.1)
    };

    struct SpotInputs
    {
        bool hasReservation = false;
        Geo::Vec3 reserved;
        float reservedFacing = 0.f;
        bool isBot = false;
        bool hasPlan = false;
        Geo::Vec3 plan;
        float planFacing = 0.f;
        Geo::Vec3 now;
        float nowFacing = 0.f;
        bool moving = false;
        float speed = 0.f;              // run speed (yd/s)
    };

    struct Spot
    {
        Geo::Vec3 p;
        float slackYd = 0.f;
        float facing = 0.f;
        SpotSource source = SpotSource::HumanNow;

        bool Planned() const { return source == SpotSource::Reservation || source == SpotSource::Plan; }
    };

    // Precedence: reservation, plan (bots), current spot. A moving bot without a plan gets its speed x `recheck`
    // on top of kUnplannedBotSlackYd (it is re-read that often).
    inline Spot PlannedSpot(SpotInputs const& s, float humanSlackYd, Ms recheck)
    {
        Spot out;
        if (s.hasReservation)
        {
            out.p = s.reserved;
            out.facing = s.reservedFacing;
            out.source = SpotSource::Reservation;
            return out;
        }
        if (s.isBot && s.hasPlan)
        {
            out.p = s.plan;
            out.facing = s.planFacing;
            out.source = SpotSource::Plan;
            return out;
        }
        out.p = s.now;
        out.facing = s.nowFacing;
        if (s.isBot)
        {
            out.source = SpotSource::BotNow;
            out.slackYd = kUnplannedBotSlackYd;
            if (s.moving)
                out.slackYd += std::max(0.f, s.speed) * std::chrono::duration<float>(recheck).count();
            return out;
        }
        out.source = SpotSource::HumanNow;
        out.slackYd = std::max(0.f, humanSlackYd);
        return out;
    }

    // Spread/RunOut separation, the avoider's side (§3.2): the carrier's circle at its planned spot, as a hazard.
    // The spot's slack widens the margin and counts as drift (the reduced-margin pass keeps it).
    inline Solver::Hazard CarrierHazard(std::uint32_t id, float radius, float zTolerance, Spot const& carrier,
        float marginYd, Solver::Timing const& timing)
    {
        Solver::Hazard h;
        h.id = id;
        h.shape = Shape::Circle(radius);
        h.shape.zTolerance = zTolerance;
        h.poses.push_back({ carrier.p.x, carrier.p.y, carrier.p.z, 0.f });
        h.marginYd = marginYd + carrier.slackYd;
        h.driftYd = carrier.slackYd;
        h.timing = timing;
        h.synthetic = true;
        return h;
    }

    // ...and the carrier's side: its own circle, no counted ally inside (indices into Input::allies).
    inline Solver::Carried CarriedCircle(std::uint32_t id, float radius, float zTolerance, float marginYd,
        Solver::Timing const& timing, std::vector<std::uint32_t> allies)
    {
        Solver::Carried c;
        c.id = id;
        c.shape = Shape::Circle(radius);
        c.shape.zTolerance = zTolerance;
        c.mode = Solver::Carried::Mode::OnBot;
        c.marginYd = marginYd;
        c.timing = timing;
        c.allies = std::move(allies);
        return c;
    }

    // Bait picker (§3.4): the radius of the hazard circle on the owner. Every other bot keeps inside othersRange (its
    // region), humans stand where they stand: `humanDistsYd` are their distances from the owner, slack included.
    inline float BaitPickerRadius(float othersRange, std::vector<float> const& humanDistsYd)
    {
        float far = std::max(0.f, othersRange);
        for (float d : humanDistsYd)
            far = std::max(far, d);
        return far + kBaitLeadYd;
    }

    // Stack Cluster(n): the members' region around the leader. Everyone within r/2 of the leader stands within r of
    // everyone else, so each has every other member (n <= members - 1) inside its circle.
    inline float ClusterMemberRadius(float r)
    {
        return std::max(0.f, r) * 0.5f;
    }

    // Soak eta (Assign::Claimant::etaMs): straight time to the region's margin edge, absolute.
    inline std::int64_t SoakEtaMs(Ms now, float distYd, float speed, Ms startDelay)
    {
        float const v = speed > 0.1f ? speed : 0.1f;
        return (now + startDelay).count() + static_cast<std::int64_t>(std::max(0.f, distYd) / v * 1000.f);
    }

    // A cone or line another bot carries with a chosen facing (look-away): the others pose it at its planned facing
    // ± this. The carrier holds that facing itself (within the face action's 5° and the board's 5° republish step),
    // so its own LookAway.MarginDeg covers it, not the slack of a boss turning at will; and the carrier keeps every
    // ally just over that margin out (Solver::Detail::BlockedArcs), so the two sides agree on the edge
    // (STAGE-M3-R-REPORT m-8).
    inline float PlannedFacingSlackDeg(float turnSlackDeg, float lookAwayMarginDeg)
    {
        return std::max(0.f, std::min(turnSlackDeg, lookAwayMarginDeg));
    }

    // ---- Tank facing (§3.7, decision §7.5; STAGE-M3-R-REPORT M-1) ----

    // A Frontal shape needs the tank to turn its source only when it points at the victim: a cone or line whose
    // facing offset lies within this of straight ahead. A rear or side shape is aimed away from the tank by the
    // boss's own facing, so turning the boss cannot clear it (a tank chasing one would circle the boss forever after
    // the melee standing behind it).
    constexpr float kFrontArcDeg = 45.f;

    inline bool FacesVictim(Shape const& s)
    {
        if (s.type != ShapeType::Cone && s.type != ShapeType::Line)
            return false;
        return std::fabs(Geo::WrapPi(Geo::DegToRad(s.facingOffsetDeg))) <= Geo::DegToRad(kFrontArcDeg) + 1e-4f;
    }

    inline bool SameShape(Shape const& a, Shape const& b)
    {
        return a.type == b.type && a.radius == b.radius && a.inner == b.inner && a.length == b.length
            && a.angleDeg == b.angleDeg && a.width == b.width && a.excludeStart == b.excludeStart
            && a.facingOffsetDeg == b.facingOffsetDeg && a.zTolerance == b.zTolerance;
    }

    // Remembers a source's tank-exempt shape when it faces the victim, once per distinct shape (every front shape is
    // kept: a source with a cone and a line needs both clear). Returns true when it was added.
    inline bool RememberFrontShape(std::vector<Shape>& shapes, Shape const& s)
    {
        if (!FacesVictim(s))
            return false;
        for (Shape const& known : shapes)
            if (SameShape(known, s))
                return false;
        shapes.push_back(s);
        return true;
    }

    // Does any remembered shape, posed at the source facing the tank at `tankAt` (and ±slackDeg), hold one of
    // `members` (their planned spots) within `marginYd`?
    inline bool FrontalHits(std::vector<Shape> const& shapes, Geo::Vec3 const& source, Geo::Vec3 const& tankAt,
        std::vector<Geo::Vec3> const& members, float marginYd, float slackDeg)
    {
        float const o = std::atan2(tankAt.y - source.y, tankAt.x - source.x);
        float const slack = Geo::DegToRad(slackDeg);
        for (Shape const& shape : shapes)
            for (Geo::Vec3 const& p : members)
                for (float d : { -slack, 0.f, slack })
                    if (Solver::SignedDistance(shape, { source.x, source.y, source.z, o + d }, p) <= marginYd)
                        return true;
        return false;
    }

    // HoldStill (§3.4): Freeze{resolveAt − marginMs, expireAt}. Several windows: the earliest one not over yet, merged
    // with any that overlaps it.
    inline void MergeFreeze(Solver::Freeze& f, Ms from, Ms to, Ms settle, Ms now)
    {
        if (to < now)
            return;
        if (!f.present || from > f.to || to < f.from)
        {
            if (f.present && f.to >= now && f.from <= from)
                return;     // an earlier window that does not overlap: keep it
            f.present = true;
            f.from = from;
            f.to = to;
            f.settle = settle;
            return;
        }
        f.from = std::min(f.from, from);
        f.to = std::max(f.to, to);
    }
}

#endif
