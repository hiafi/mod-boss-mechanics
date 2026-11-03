/*
 * mod-boss-mechanics: the bots' view of the registry and the navmesh path oracle (STAGE-M2-HANDOFF §3.5, WP-B).
 */

#include "MechanicBotView.h"
#include "BossMechanicsConfig.h"
#include "Log.h"
#include "MechanicRegistry.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Unit.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace BossMechanics
{
    namespace
    {
        constexpr char const* kLog = "module.bossmechanics.bots";

        // A path whose navmesh end lands farther than this from the asked point does not reach it.
        constexpr float kPathEndToleranceYd = 1.f;
        // A path whose first point is this close to `from` already starts there.
        constexpr float kPathStartToleranceYd = 0.1f;
        // Drift: the anchor is assumed to move for at most this long before the hazard's first window.
        constexpr Milliseconds kMaxDriftLead{ 1000 };
        // Hazards whose shape lies farther than (run speed x horizon) + this from the bot are left out.
        constexpr float kReachSlackYd = 5.f;
        // Self-check: windows may differ by this much (rounding).
        constexpr Milliseconds kSelfCheckToleranceMs{ 1 };

        Geo::Vec3 ToVec(Position const& p)
        {
            return { p.GetPositionX(), p.GetPositionY(), p.GetPositionZ() };
        }

        bool UnitMoves(Unit const* u)
        {
            return u->isMoving() || (u->movespline && !u->movespline->Finalized());
        }

        // Run speed of the fastest moving unit the hazard is anchored on (0 when none moves). `unitAnchored` is set
        // when either anchor is a unit at all.
        float AnchorDriftSpeed(Player const* bot, ActiveMechanic const& m, bool& unitAnchored)
        {
            float speed = 0.f;
            auto consider = [&](Anchor const& a)
            {
                if (!a.IsUnit())
                    return;
                unitAnchored = true;
                if (Unit const* u = ObjectAccessor::GetUnit(*bot, a.unit))
                    if (UnitMoves(u))
                        speed = std::max(speed, u->GetSpeed(MOVE_RUN));
            };
            consider(m.anchor);
            if (m.to)
                consider(*m.to);
            return speed;
        }

        bool AnchoredOn(ActiveMechanic const& m, ObjectGuid const& guid)
        {
            return (m.anchor.IsUnit() && m.anchor.unit == guid) || (m.to && m.to->IsUnit() && m.to->unit == guid);
        }

        // Timing of one hazard and the start of its first window (for the drift lead).
        Solver::Timing TimingOf(ActiveMechanic const& m, Milliseconds now, Milliseconds hold, Milliseconds& firstAt)
        {
            Solver::Timing t;
            if (m.mine && m.detonateRequested)
                t = Solver::MakeInstant(now, hold);     // Detonate(): blows up on the next full update, anyone inside
            else if (m.mine)
            {
                Milliseconds const expire = m.expireAt >= Registry::kForever ? Solver::kForever : m.expireAt;
                // Armed only once the registry armed it: the arming update detonates on anyone inside, and it may
                // come a tick or more after armAt, so until then the strict window runs on to now (no grace).
                if (m.mine->armed)
                    t = Solver::MakeMineArmed(m.mine->armAt, expire, m.mine->grace);
                else
                {
                    t = Solver::MakeMineArming(m.mine->armAt, expire, m.mine->grace);
                    t.strictTo = now;
                }
            }
            else if (m.projectile && !m.projectile->unit.IsEmpty())
                t = Solver::MakeInterval(m.resolveAt, m.expireAt);  // tracked: per-point windows unreliable
            else if (m.projectile)
                t = Solver::MakeProjectile(ProjectileParamsOf(m), m.resolveAt);
            else if (m.persist)
                t = Solver::MakeInterval(m.detonateRequested && !m.resolved ? std::min(now, m.resolveAt)
                    : m.resolveAt, m.persist->until);
            else
            {
                // Plain: resolves on the first full map update at or after resolveAt (the next one when detonated),
                // so it stays live `hold` past that.
                t = Solver::MakeInstant(m.detonateRequested ? std::min(now, m.resolveAt) : m.resolveAt, hold);
            }
            firstAt = t.from;
            return t;
        }

        // Up to three interior points of the shape (shape-local layout, then posed), for the Debug self-check.
        std::array<Geo::Vec3, 3> SamplePoints(Shape const& shape, Geo::Pose const& pose)
        {
            float const f = Geo::ShapeFacing(shape, pose);
            auto at = [&](float bearing, float dist)
            {
                return Geo::Vec3{ pose.x + std::cos(bearing) * dist, pose.y + std::sin(bearing) * dist, pose.z };
            };
            switch (shape.type)
            {
                case ShapeType::Circle:
                    return { at(f, 0.f), at(f, shape.radius * 0.5f), at(f + Geo::kPi, shape.radius * 0.5f) };
                case ShapeType::Donut:
                {
                    float const mid = (shape.inner + shape.radius) * 0.5f;
                    return { at(f, mid), at(f + Geo::kTwoPi / 3.f, mid), at(f - Geo::kTwoPi / 3.f, mid) };
                }
                case ShapeType::Cone:
                    return { at(f, shape.length * 0.25f), at(f, shape.length * 0.5f), at(f, shape.length * 0.75f) };
                case ShapeType::Line:
                {
                    float const span = std::max(0.f, shape.length - shape.excludeStart);
                    return { at(f, shape.excludeStart + span * 0.25f), at(f, shape.excludeStart + span * 0.5f),
                        at(f, shape.excludeStart + span * 0.75f) };
                }
            }
            return { at(f, 0.f), at(f, 0.f), at(f, 0.f) };
        }

        bool Near(Milliseconds a, Milliseconds b)
        {
            return (a > b ? a - b : b - a) <= kSelfCheckToleranceMs;
        }

        // Debug: the solver's windows at three interior points must match Registry::HotWindows, transformed the way
        // TimingOf intends (plain: `to` + hold; mines: `from` only; tracked projectiles and pending detonations are
        // not comparable). Returns the mismatches (logged).
        uint32 SelfCheck(Registry const& reg, ActiveMechanic const& m, Solver::Hazard const& h, Milliseconds hold)
        {
            if (m.detonateRequested || (m.projectile && !m.projectile->unit.IsEmpty()) || h.poses.empty())
                return 0;
            // The registry has no margin and one pose: compare against the hazard without either (the solver widens a
            // projectile's body by the margin).
            Solver::Hazard exact = h;
            exact.marginYd = 0.f;
            exact.poses.resize(1);
            Geo::Pose const& pose = exact.poses.front();
            uint32 mismatches = 0;
            for (Geo::Vec3 const& p : SamplePoints(exact.shape, pose))
            {
                if (!Geo::Contains(exact.shape, pose, p))
                    continue;
                std::vector<HotWindow> const expect = reg.HotWindows(m, Position(p.x, p.y, p.z));
                std::vector<HotWindow> const got = Solver::WindowsAt(exact, p);
                bool ok = !expect.empty() && !got.empty();
                if (ok && m.mine)
                    ok = Near(got.front().from, expect.front().from);
                else if (ok)
                {
                    ok = got.size() == expect.size();
                    bool const plain = !m.projectile && !m.persist;
                    for (std::size_t i = 0; ok && i < got.size(); ++i)
                        ok = Near(got[i].from, expect[i].from)
                            && Near(got[i].to, plain ? expect[i].to + hold : expect[i].to);
                }
                if (ok)
                    continue;
                ++mismatches;
                auto const first = [](std::vector<HotWindow> const& w)
                {
                    return w.empty() ? std::pair<int64, int64>(0, 0)
                        : std::pair<int64, int64>(w.front().from.count(), w.front().to.count());
                };
                auto const [gotFrom, gotTo] = first(got);
                auto const [expFrom, expTo] = first(expect);
                LOG_WARN(kLog, "Self-check: mechanic {} ({} {}) at ({:.2f}, {:.2f}): solver {} window(s) from {} to "
                    "{}, registry {} window(s) from {} to {}", m.id, KindName(m.kind), ShapeName(m.shape.type), p.x,
                    p.y, got.size(), gotFrom, gotTo, expect.size(), expFrom, expTo);
            }
            return mismatches;
        }

        // Distance from (x, y) to the frame's rectangle (0 inside).
        float DistToFrame(Frame const& f, float x, float y)
        {
            float const c = std::cos(f.angle);
            float const s = std::sin(f.angle);
            float const dx = x - f.x;
            float const dy = y - f.y;
            float const fx = c * dx + s * dy;
            float const fy = -s * dx + c * dy;
            float const ox = fx < 0.f ? -fx : (fx > f.sizeX ? fx - f.sizeX : 0.f);
            float const oy = fy < 0.f ? -fy : (fy > f.sizeY ? fy - f.sizeY : 0.f);
            return std::hypot(ox, oy);
        }
    }

    BotViewOptions BotViewOptions::FromConfig(BotConfig const& cfg)
    {
        BotViewOptions o;
        o.postResolveHold = Milliseconds(cfg.postResolveHoldMs);
        o.safetyMarginYd = cfg.safetyMarginYd;
        o.facingSlackDeg = cfg.facingSlackDeg;
        o.movingAnchorRecheck = Milliseconds(cfg.movingAnchorRecheckMs);
        o.debug = cfg.debug;
        o.params.startDelay = Milliseconds(cfg.startDelayMs);
        o.params.marginMs = Milliseconds(cfg.safetyMarginMs);
        o.params.horizon = Milliseconds(cfg.horizonMs);
        o.params.graceMargin = Milliseconds(cfg.graceMarginMs);
        o.params.holdCap = Milliseconds(cfg.holdCapMs);
        o.params.gridStepYd = cfg.gridStepYd;
        o.params.maxCandidates = cfg.maxCandidates;
        o.params.maxPathChecks = cfg.maxPathChecks;
        return o;
    }

    BotViewInfo BuildInput(Registry const& reg, Player const* bot, Milliseconds now, BotViewOptions const& opt,
        Solver::Input& out, PerceptionFilter const* perceive)
    {
        BotViewInfo info;
        info.revision = reg.Revision();
        out.hazards.clear();
        out.lanes.clear();
        out.now = now;
        out.params = opt.params;
        if (!bot)
            return info;

        out.bot = ToVec(bot->GetPosition());
        out.speed = bot->GetSpeed(MOVE_RUN);
        ObjectGuid const self = bot->GetGUID();
        float const reachYd = out.speed * std::chrono::duration<float>(opt.params.horizon).count() + kReachSlackYd;
        float const slackRad = Geo::DegToRad(opt.facingSlackDeg);

        std::unordered_set<uint32> kept;     // hazard ids in the view (lane rects are filtered by them)
        reg.ForEachActive(now - opt.postResolveHold, [&](ActiveMechanic const& m)
        {
            if (!m.IsShapeKind())
                return;
            if (AnchoredOn(m, self) || !reg.IsEligible(m, bot) || reg.IsTankExempted(m, bot))
                return;

            Geo::Pose pose;
            Shape const shape = reg.EffectiveShape(m, pose);

            bool unitAnchored = false;
            float const driftSpeed = AnchorDriftSpeed(bot, m, unitAnchored);
            Milliseconds firstAt{ 0 };
            Solver::Timing const timing = TimingOf(m, now, opt.postResolveHold, firstAt);

            // Drift: a moving anchor may carry the shape this far before its first window; once the window is open
            // the view is refreshed every movingAnchorRecheck, so at least that much.
            float drift = 0.f;
            if (driftSpeed > 0.f)
            {
                Milliseconds const lead = std::clamp(firstAt - now, opt.movingAnchorRecheck,
                    std::max(kMaxDriftLead, opt.movingAnchorRecheck));
                drift = driftSpeed * std::chrono::duration<float>(lead).count();
            }
            float const margin = opt.safetyMarginYd + drift;

            if (Geo::Dist2d(pose.x, pose.y, out.bot.x, out.bot.y) > Geo::Bounds(shape) + margin + reachYd)
                return;

            if (perceive && *perceive)
            {
                Milliseconds until = Solver::kForever;
                if (!(*perceive)(m.id, until))
                {
                    ++info.hidden;
                    info.nextPerceptionAt = std::min(info.nextPerceptionAt, until);
                    return;
                }
            }

            Solver::Hazard h;
            h.id = m.id;
            h.shape = shape;
            h.marginYd = margin;
            h.timing = timing;
            h.poses.push_back(pose);
            bool const turns = m.anchor.IsUnit() && m.anchor.followFacing && !m.to && slackRad > 0.f
                && (shape.type == ShapeType::Line || (shape.type == ShapeType::Cone && shape.angleDeg < 360.f));
            if (turns)
            {
                Geo::Pose left = pose;
                left.o = Geo::WrapPi(pose.o - slackRad);
                Geo::Pose right = pose;
                right.o = Geo::WrapPi(pose.o + slackRad);
                h.poses.push_back(left);
                h.poses.push_back(right);
            }
            info.movingAnchors = info.movingAnchors || unitAnchored;

            if (opt.debug)
                info.selfCheckMismatches += SelfCheck(reg, m, h, opt.postResolveHold);
            kept.insert(h.id);
            out.hazards.push_back(std::move(h));
        });

        reg.ForEachLaneSet([&](LaneSet const& ls)
        {
            Frame const frame = ls.frame.ToFrame();
            if (DistToFrame(frame, out.bot.x, out.bot.y) > opt.laneFrameRangeYd)
                return;
            // Only the rects of lanes still in the view (WP-A: the analytic gap cells treat every rect as covered, so
            // the rect of a lane that resolved, or that the bot cannot see, would hide a real gap).
            Solver::LaneFrame lf;
            lf.frame = frame;
            for (std::size_t i = 0; i < ls.rects.size() && i < ls.lanes.size(); ++i)
                if (kept.count(ls.lanes[i]))
                    lf.rects.push_back(ls.rects[i]);
            lf.freeLanes = ls.freeLanes;
            out.lanes.push_back(std::move(lf));
        });
        return info;
    }

    bool NavOracle::Find(Geo::Vec3 const& from, Geo::Vec3 const& to, std::vector<Geo::Vec3>& out)
    {
        ++_calls;
        out.clear();
        if (!_bot || !_bot->IsInWorld())
            return false;

        PathGenerator path(_bot);
        path.CalculatePath(from.x, from.y, from.z, to.x, to.y, to.z, false);
        if (path.GetPathType() != PATHFIND_NORMAL)
            return false;
        Movement::PointsArray const& points = path.GetPath();
        if (points.empty())
            return false;
        G3D::Vector3 const& end = points.back();
        if (Geo::Dist2d(end.x, end.y, to.x, to.y) > kPathEndToleranceYd)
            return false;

        out.reserve(points.size() + 1);
        G3D::Vector3 const& first = points.front();
        if (Geo::Dist2d(first.x, first.y, from.x, from.y) > kPathStartToleranceYd)
            out.push_back(from);
        for (G3D::Vector3 const& p : points)
            out.push_back({ p.x, p.y, p.z });
        return true;
    }
}
