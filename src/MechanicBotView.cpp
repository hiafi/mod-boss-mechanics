/*
 * mod-boss-mechanics: the bots' view of the registry and the navmesh path oracle (STAGE-M2-HANDOFF §3.5, WP-B), the
 * player kinds, reservations and line of sight (STAGE-M3-HANDOFF §3.4, §3.6, WP-V).
 */

#include "MechanicBotView.h"
#include "BossMechanicsConfig.h"
#include "DetourNavMeshQuery.h"
#include "Log.h"
#include "Map.h"
#include "MapDefines.h"
#include "MechanicAssign.h"
#include "MechanicRegistry.h"
#include "MechanicViewRules.h"
#include "ModelIgnoreFlags.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "StringFormat.h"
#include "Unit.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace BossMechanics
{
    namespace
    {
        constexpr char const* kLog = "module.bossmechanics.bots";

        // A path whose end lands farther than this from the asked point goes somewhere else (the solver validates
        // and scores the real end of every accepted path, so this only keeps a candidate from turning into another).
        constexpr float kMaxEndShiftYd = 3.f;
        // Debug: an accepted end this far from the asked point counts as shifted.
        constexpr float kShiftedYd = 0.5f;
        // Walkability probe: a walkable polygon must lie within this horizontally (the snap distance), and within
        // kProbeUpYd vertically (ramps and stairs: the candidates carry the bot's z; PathGenerator's first search box
        // is as tall).
        constexpr float kProbeSnapYd = 1.f;
        constexpr float kProbeUpYd = 5.f;
        constexpr float kProbeMovedYd = 0.05f;
        // A path whose first point is this close to `from` already starts there.
        constexpr float kPathStartToleranceYd = 0.1f;
        // Drift: the anchor is assumed to move for at most this long before the hazard's first window.
        constexpr Milliseconds kMaxDriftLead{ 1000 };
        // Hazards whose shape lies farther than (run speed x horizon) + this from the bot are left out.
        constexpr float kReachSlackYd = 5.f;
        // Stratholme X1: a walking carrier's soft circle (Input::softHazards) is widened by this (its next steps).
        constexpr float kLiveCarrierSlackYd = 0.5f;
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

        // ------------------------------------------------------------------------------------------------------------
        // Player kinds (M3, §3.4): regions, allies, carried shapes, synthetic hazards, Freeze, tasks
        // ------------------------------------------------------------------------------------------------------------

        // Self-check of a region: the solver's containment (margin 0, the live pose) at three interior points and one
        // point outside must agree with Registry::ContainsPoint for `who`. `outside` = also test the exterior point.
        uint32 RegionSelfCheck(Registry const& reg, ActiveMechanic const& m, Shape const& shape, Geo::Pose const& pose,
            ObjectGuid who, bool outside)
        {
            std::vector<Geo::Vec3> points;
            for (Geo::Vec3 const& p : SamplePoints(shape, pose))
                points.push_back(p);
            if (outside)
            {
                float const back = Geo::ShapeFacing(shape, pose) + Geo::kPi;
                float const d = Geo::Bounds(shape) + 1.f;
                points.push_back({ pose.x + std::cos(back) * d, pose.y + std::sin(back) * d, pose.z });
            }
            uint32 mismatches = 0;
            for (Geo::Vec3 const& p : points)
            {
                bool const got = Solver::SignedDistance(shape, pose, p) <= 0.f;
                if (got != Geo::Contains(shape, pose, p))
                    continue;   // on the edge (the two tests differ by their epsilon): not comparable
                bool const expect = reg.ContainsPoint(m, Position(p.x, p.y, p.z), who);
                if (got == expect)
                    continue;
                ++mismatches;
                LOG_WARN(kLog, "Self-check: {} {} ({}) at ({:.2f}, {:.2f}): solver {}, registry {}", KindName(m.kind),
                    m.id, ShapeName(shape.type), p.x, p.y, got ? "inside" : "outside",
                    expect ? "inside" : "outside");
            }
            return mismatches;
        }

        // The judge window of a player kind (§3.2 "Timing"): [resolveAt, resolveAt + hold] (from = now when a
        // detonation is pending), lasting Stack/Soak [resolveAt, until], HoldStill [resolveAt, expireAt].
        void PlayerWindow(ActiveMechanic const& m, Milliseconds now, Milliseconds hold, Milliseconds& from,
            Milliseconds& to)
        {
            Milliseconds const at = m.detonateRequested && !m.resolved ? std::min(now, m.resolveAt) : m.resolveAt;
            if (m.kind == Kind::HoldStill)
            {
                from = m.resolveAt;
                to = m.expireAt;
            }
            else if (m.persist)
            {
                from = at;
                to = m.persist->until >= Registry::kForever ? Solver::kForever : m.persist->until;
            }
            else
            {
                from = at;
                to = at + hold;
            }
        }

        Solver::Timing WindowTiming(ActiveMechanic const& m, Milliseconds from, Milliseconds to)
        {
            return m.persist ? Solver::MakeInterval(from, to) : Solver::MakeInstant(from, to - from);
        }

        bool Has(std::vector<ObjectGuid> const& list, ObjectGuid const& g)
        {
            return std::find(list.begin(), list.end(), g) != list.end();
        }

        Reservation const* ReservationOf(ActiveMechanic const& m, ObjectGuid const& g, Milliseconds now)
        {
            for (Reservation const& r : m.reservations)
                if (r.bot == g && r.until > now)
                    return &r;
            return nullptr;
        }

        // One pass over the player kinds for one bot (RefreshAllies, and BuildInput through it).
        class PlayerPass
        {
        public:
            PlayerPass(Registry const& reg, Player const* bot, Milliseconds now, BotViewOptions const& opt,
                BotViewHooks const* hooks, Solver::Input& in, BotViewInfo& info)
                : _reg(reg), _bot(bot), _self(bot->GetGUID()), _now(now), _opt(opt), _hooks(hooks), _in(in),
                _info(info) { }

            void Run(PerceptionFilter const* perceive, bool countHidden)
            {
                for (Solver::Hazard const& h : _in.hazards)
                    _baseHazards.emplace(h.id, &h - _in.hazards.data());

                std::vector<Solver::Hazard> extra;
                _reg.ForEachActive(_now - _opt.postResolveHold, [&](ActiveMechanic const& m)
                {
                    if (m.decoy)
                        return;     // a decoy is empty floor to the bots (Judge::DecoyFits)
                    if (m.bounds)
                        BoundsOf(m, *m.bounds);
                    if (m.IsShapeKind())
                    {
                        ShapeTasks(m);
                        ShapeKind(m, extra, perceive, countHidden);
                        return;
                    }
                    if (!Perceived(m, perceive, countHidden))
                        return;
                    switch (m.kind)
                    {
                        case Kind::Spread:
                        case Kind::RunOut:
                            Carriers(m, extra);
                            break;
                        case Kind::Stack:
                            if (m.IsCluster())
                                Cluster(m);
                            else
                                StackOrSoak(m, extra);
                            break;
                        case Kind::Soak:
                            StackOrSoak(m, extra);
                            break;
                        case Kind::Bait:
                            Bait(m, extra);
                            break;
                        case Kind::HoldStill:
                            HoldStill(m);
                            break;
                        case Kind::Spot:
                            SpotKind(m);
                            break;
                        default:
                            break;
                    }
                });
                for (Solver::Hazard& h : extra)
                    _in.hazards.push_back(std::move(h));
                // X2: the drop's enterable spots open for the carrier (Solver::LiftEnterable).
                Solver::LiftEnterable(_in, _dropEnterable);
            }

        private:
            bool Perceived(ActiveMechanic const& m, PerceptionFilter const* perceive, bool countHidden)
            {
                if (!perceive || !*perceive)
                    return true;
                Milliseconds until = Solver::kForever;
                if ((*perceive)(m.id, until))
                    return true;
                if (countHidden)
                    ++_info.hidden;
                _info.nextPerceptionAt = std::min(_info.nextPerceptionAt, until);
                return false;
            }

            void AddTask(ActiveMechanic const& m, BotTaskRole role, Milliseconds from, Milliseconds to, uint8 slot = 0)
            {
                BotTask t;
                t.mechanicId = m.id;
                t.kind = m.kind;
                t.role = role;
                t.from = from;
                t.to = to;
                t.slot = slot;
                _info.tasks.push_back(t);
            }

            std::vector<ObjectGuid> const& Eligible(ActiveMechanic const& m)
            {
                if (_eligibleFor != m.id)
                {
                    _eligible = _reg.EligiblePlayers(m);
                    _eligibleFor = m.id;
                }
                return _eligible;
            }

            bool IsBot(ObjectGuid const& g) const
            {
                return g == _self || (_hooks && _hooks->isBot && _hooks->isBot(g));
            }

            // Where `g` will stand at m's judge time (§3.4). nullopt: no reservation and the unit is not here.
            std::optional<ViewRules::Spot> SpotOf(ActiveMechanic const& m, ObjectGuid const& g)
            {
                ViewRules::SpotInputs s;
                if (Reservation const* r = ReservationOf(m, g, _now))
                {
                    s.hasReservation = true;
                    s.reserved = ToVec(r->point);
                    s.reservedFacing = r->facing;
                    return ViewRules::PlannedSpot(s, _opt.humanSlackYd, _opt.movingAnchorRecheck);
                }
                Unit const* u = ObjectAccessor::GetUnit(*_bot, g);
                if (!u || !u->IsInWorld() || u->GetMap() != _bot->GetMap())
                    return std::nullopt;
                s.isBot = IsBot(g);
                if (s.isBot && g != _self && _hooks && _hooks->plannedSpot)
                    s.hasPlan = _hooks->plannedSpot(g, s.plan, s.planFacing);
                s.now = ToVec(u->GetPosition());
                s.nowFacing = u->GetOrientation();
                s.moving = UnitMoves(u);
                s.speed = u->GetSpeed(MOVE_RUN);
                return ViewRules::PlannedSpot(s, _opt.humanSlackYd, _opt.movingAnchorRecheck);
            }

            // Index of g's ally entry in Input::allies (one per player and spot).
            std::optional<uint32> AllyIndex(ActiveMechanic const& m, ObjectGuid const& g)
            {
                std::optional<ViewRules::Spot> spot = SpotOf(m, g);
                if (!spot)
                    return std::nullopt;
                uint64 const raw = g.GetRawValue();
                for (std::size_t i = 0; i < _in.allies.size(); ++i)
                {
                    Solver::Ally const& a = _in.allies[i];
                    if (a.guid == raw && Geo::Dist2d(a.p.x, a.p.y, spot->p.x, spot->p.y) < 0.01f
                        && a.slackYd == spot->slackYd)
                        return static_cast<uint32>(i);
                }
                _in.allies.push_back({ raw, spot->p, spot->slackYd });
                return static_cast<uint32>(_in.allies.size() - 1);
            }

            std::vector<uint32> AllyIndices(ActiveMechanic const& m, std::vector<ObjectGuid> const& players,
                ObjectGuid const& skip = ObjectGuid::Empty)
            {
                std::vector<uint32> out;
                for (ObjectGuid const& g : players)
                {
                    if (g == _self || (!skip.IsEmpty() && g == skip))
                        continue;
                    if (std::optional<uint32> idx = AllyIndex(m, g))
                        out.push_back(*idx);
                }
                return out;
            }

            // A moving unit's drift before `from` (as the M2 hazards: at least one recheck, at most 1 s).
            float DriftOf(Unit const* u, Milliseconds from) const
            {
                if (!u || !UnitMoves(u))
                    return 0.f;
                Milliseconds const lead = std::clamp(from - _now, _opt.movingAnchorRecheck,
                    std::max(kMaxDriftLead, _opt.movingAnchorRecheck));
                return u->GetSpeed(MOVE_RUN) * std::chrono::duration<float>(lead).count();
            }

            // The poses of a get-in region on m's shape: the anchor's planned spot when the anchor is a group bot
            // with a plan; the live pose and its drift pose when the anchor is a moving unit; else the live pose.
            std::vector<Geo::Pose> RegionPoses(ActiveMechanic const& m, Shape const& shape, Geo::Pose const& live,
                Milliseconds from)
            {
                if (!m.anchor.IsUnit() || m.anchor.unit == _self)
                    return { live };
                if (std::optional<ViewRules::Spot> spot = SpotOf(m, m.anchor.unit); spot && spot->Planned())
                {
                    Geo::Pose p = live;
                    p.x = spot->p.x;
                    p.y = spot->p.y;
                    p.z = spot->p.z;
                    if (m.anchor.followFacing && !m.to)
                        p.o = spot->facing;
                    return { p };
                }
                Unit const* u = ObjectAccessor::GetUnit(*_bot, m.anchor.unit);
                NoteMoving(u);
                float const drift = ViewRules::RegionDriftYd(shape, DriftOf(u, from));
                return ViewRules::MovingPoses(live, u ? u->GetOrientation() : live.o, drift);
            }

            // A region or carried shape follows a moving unit: the brain rebuilds the view every
            // MovingAnchorRecheckMs and tracks the region (STAGE-M3-R-REPORT m-6).
            void NoteMoving(Unit const* u)
            {
                if (u && UnitMoves(u))
                    _info.movingAnchors = true;
            }

            void AddRegion(ActiveMechanic const& m, Shape const& shape, std::vector<Geo::Pose> poses,
                Milliseconds from, Milliseconds to)
            {
                Solver::Region r;
                r.id = m.id;
                r.shape = shape;
                r.poses = std::move(poses);
                r.marginYd = ViewRules::RegionMarginYd(shape, _opt.safetyMarginYd);
                r.from = from;
                r.to = to;
                _in.regions.push_back(std::move(r));
            }

            // ---- shape kinds ----

            void ShapeTasks(ActiveMechanic const& m)
            {
                bool const exempt = m.tankExempt && _reg.ExemptVictim(m) == _self;
                Unit const* victim = _bot->GetVictim();
                bool const parks = m.park && victim && victim->GetGUID() == m.ParkedUnit();
                if (!exempt && !parks)
                    return;
                Milliseconds firstAt{ 0 };
                Solver::Timing const t = TimingOf(m, _now, _opt.postResolveHold, firstAt);
                Milliseconds const to = std::max(t.from, t.to);
                if (exempt)
                {
                    _info.tankFrontals.push_back(m.id);
                    AddTask(m, BotTaskRole::Tank, t.from, to);
                }
                if (parks)
                {
                    _info.parks.push_back(m.id);
                    AddTask(m, BotTaskRole::Parker, t.from, to);
                }
            }

            // Tank and parking tasks are never hidden (they are not dodges); a carried shape goes through the
            // perception filter; the planned-spot pose only copies a hazard BuildInput kept (it already passed it).
            void ShapeKind(ActiveMechanic const& m, std::vector<Solver::Hazard>& extra,
                PerceptionFilter const* perceive, bool countHidden)
            {
                bool const onMe = m.anchor.IsUnit() && m.anchor.unit == _self;
                bool const atMe = !onMe && m.to && m.to->IsUnit() && m.to->unit == _self;
                if (onMe || atMe)
                {
                    if (Perceived(m, perceive, countHidden))
                        CarriedShape(m, onMe);
                    return;
                }
                PlannedPose(m, extra);
            }

            // A Zone anchored on the bot (OnBot) or aimed at it (AimedAtBot): keep the allies out of it (§3.4).
            void CarriedShape(ActiveMechanic const& m, bool onMe)
            {
                Geo::Pose pose;
                Shape const shape = _reg.EffectiveShape(m, pose);
                Milliseconds firstAt{ 0 };
                Solver::Carried c;
                c.id = m.id;
                c.shape = shape;
                c.mode = onMe ? Solver::Carried::Mode::OnBot : Solver::Carried::Mode::AimedAtBot;
                c.source = pose;
                c.marginYd = _opt.safetyMarginYd;
                c.angleMarginDeg = _opt.lookAwayMarginDeg;
                c.timing = TimingOf(m, _now, _opt.postResolveHold, firstAt);
                if (onMe)
                {
                    bool const directed = shape.type == ShapeType::Line
                        || (shape.type == ShapeType::Cone && shape.angleDeg < 360.f);
                    c.chooseFacing = directed && m.anchor.followFacing && !m.to;
                    c.fixedFacing = m.anchor.followFacing ? _bot->GetOrientation() : m.anchor.point.GetOrientation();
                    if (m.to)
                        c.fixedFacing = pose.o;     // Line(me, unit): aimed by the other end
                    Unit const* victim = _bot->GetVictim();
                    c.preferredFacing = victim && victim->IsInWorld() && victim->GetMap() == _bot->GetMap()
                        ? _bot->GetAbsoluteAngle(victim) : _bot->GetOrientation();
                }
                if (!onMe && m.anchor.IsUnit())
                    NoteMoving(ObjectAccessor::GetUnit(*_bot, m.anchor.unit));
                ObjectGuid const exempt = m.tankExempt ? _reg.ExemptVictim(m) : ObjectGuid::Empty;
                c.allies = AllyIndices(m, Eligible(m), exempt);
                _in.carried.push_back(std::move(c));
                AddTask(m, BotTaskRole::Carrier, firstAt, std::max(firstAt, TimingEndOf(_in.carried.back().timing)));
                if (m.dropSpot)
                    DropOf(m, _in.carried.back());
            }

            // Stratholme X2: the drop spot rule of a shape the bot carries (the earliest landing when it carries
            // several), as solver input (MechanicDropSpot.h).
            void DropOf(ActiveMechanic const& m, Solver::Carried const& c)
            {
                DropSpotRule const& rule = *m.dropSpot;
                Milliseconds const landAt = c.timing.from;
                if (_in.drop.present && _in.drop.landAt <= landAt)
                    return;
                Solver::Drop d;
                d.present = true;
                d.id = m.id;
                d.landAt = landAt;
                d.enterAt = landAt - rule.maxDwell;
                if (rule.roomPolygon.size() >= 3)
                    for (Position const& p : rule.roomPolygon)
                        d.room.push_back(ToVec(p));
                d.roomCenter = ToVec(rule.roomCenter);
                d.roomRadius = rule.roomRadius;
                // F-1b: the hard bounds; a single area doubles as the room when the rule names none.
                if (rule.bounds)
                {
                    AddBounds(*rule.bounds);
                    if (d.room.empty() && d.roomRadius <= 0.f && rule.bounds->areas.size() == 1)
                    {
                        SpotBounds::Area const& a = rule.bounds->areas.front();
                        for (Position const& p : a.polygon)
                            d.room.push_back(ToVec(p));
                        if (d.room.size() < 3)
                        {
                            d.room.clear();
                            d.roomCenter = ToVec(a.center);
                            d.roomRadius = a.radius;
                        }
                    }
                }
                d.edge = rule.edge;
                d.away = rule.away;
                d.behind = rule.behind;
                d.travelFacing = rule.travelFacing;
                if (Unit const* owner = ObjectAccessor::GetUnit(*_bot, m.owner); owner && rule.maxRangeYd > 0.f)
                {
                    d.maxRangeYd = rule.maxRangeYd;
                    d.rangeFrom = ToVec(owner->GetPosition());
                }
                _dropEnterable.clear();
                for (DropSpotRule::Circle const& pc : rule.prefer)
                {
                    d.prefer.push_back({ ToVec(pc.center), pc.radius, pc.enterable != 0 });
                    if (pc.enterable)
                        _dropEnterable.push_back(pc.enterable);
                }
                for (DropSpotRule::Circle const& ac : rule.avoid)
                    d.avoid.push_back({ ToVec(ac.center), ac.radius, false });
                _in.drop = std::move(d);
            }

            // Stratholme F-1b: a mechanic's hard bounds (MechanicRef::Within) hold for this bot while the mechanic's
            // owner fights it (another fight's room on the same map never binds it).
            void BoundsOf(ActiveMechanic const& m, SpotBounds const& b)
            {
                Unit const* owner = ObjectAccessor::GetUnit(*_bot, m.owner);
                if (!owner || !owner->IsAlive() || !_bot->IsInCombatWith(owner))
                    return;
                AddBounds(b);
            }

            void AddBounds(SpotBounds const& b)
            {
                Solver::Bounds out;
                for (SpotBounds::Area const& a : b.areas)
                {
                    Solver::BoundsArea area;
                    if (a.polygon.size() >= 3)
                        for (Position const& p : a.polygon)
                            area.polygon.push_back(ToVec(p));
                    area.center = ToVec(a.center);
                    area.radius = a.radius;
                    area.zMin = a.zMin;
                    area.zMax = a.zMax;
                    out.areas.push_back(std::move(area));
                }
                for (SpotBounds::KeepOutCircle const& k : b.keepOut)
                    out.keepOut.push_back({ ToVec(k.center), k.radius, false });
                out.pathInside = b.pathInside;
                for (Solver::Bounds const& have : _in.bounds)
                    if (DropSpot::SameBounds(have, out))
                        return;
                _in.bounds.push_back(std::move(out));
            }

            static Milliseconds TimingEndOf(Solver::Timing const& t)
            {
                return std::max(t.from, t.to);
            }

            // A Zone anchored on (or aimed at) another group bot with a plan: one more pose at its planned spot
            // (§3.4), as a synthetic copy of the registry hazard (only when that hazard is in the view).
            void PlannedPose(ActiveMechanic const& m, std::vector<Solver::Hazard>& extra)
            {
                auto base = _baseHazards.find(m.id);
                if (base == _baseHazards.end())
                    return;
                Solver::Hazard const& h = _in.hazards[base->second];
                if (h.poses.empty())
                    return;
                Geo::Pose planned = h.poses.front();
                bool found = false;
                if (m.to && m.to->IsUnit())
                {
                    std::optional<ViewRules::Spot> spot = SpotOf(m, m.to->unit);
                    if (spot && spot->Planned())
                    {
                        Geo::Pose src = planned;
                        if (m.anchor.IsUnit() && m.anchor.unit != m.to->unit)
                            if (std::optional<ViewRules::Spot> from = SpotOf(m, m.anchor.unit);
                                from && from->Planned())
                            {
                                src.x = from->p.x;
                                src.y = from->p.y;
                                src.z = from->p.z;
                            }
                        if (Geo::Dist2d(src.x, src.y, spot->p.x, spot->p.y) > 0.01f)
                            src.o = std::atan2(spot->p.y - src.y, spot->p.x - src.x);
                        planned = src;
                        found = true;
                    }
                }
                else if (m.anchor.IsUnit())
                {
                    std::optional<ViewRules::Spot> spot = SpotOf(m, m.anchor.unit);
                    if (spot && spot->Planned())
                    {
                        planned.x = spot->p.x;
                        planned.y = spot->p.y;
                        planned.z = spot->p.z;
                        if (m.anchor.followFacing)
                            planned.o = spot->facing;
                        found = true;
                    }
                }
                if (!found)
                    return;
                Solver::Hazard copy = h;
                copy.synthetic = true;
                copy.poses.clear();
                copy.poses.push_back(planned);
                if (h.poses.size() == 3)    // a turning anchor: keep its +-slack around the planned facing
                {
                    // A planned facing is a look-away the carrier holds itself (ViewRules::PlannedFacingSlackDeg).
                    float const slack = Geo::DegToRad(ViewRules::PlannedFacingSlackDeg(
                        Geo::RadToDeg(std::fabs(Geo::WrapPi(h.poses[2].o - h.poses[0].o))), _opt.lookAwayMarginDeg));
                    Geo::Pose left = planned;
                    left.o = Geo::WrapPi(planned.o - slack);
                    Geo::Pose right = planned;
                    right.o = Geo::WrapPi(planned.o + slack);
                    copy.poses.push_back(left);
                    copy.poses.push_back(right);
                }
                extra.push_back(std::move(copy));
            }

            // ---- Spread / RunOut ----

            void Carriers(ActiveMechanic const& m, std::vector<Solver::Hazard>& extra)
            {
                // F-1b (R-1-KIT F2): a hit with no warning (DungeonKit's RegistryHit) is no job for anyone.
                if (ViewRules::Undodgeable(m.createdAt, m.resolveAt, m.persist.has_value()))
                    return;
                std::vector<ObjectGuid> const& eligible = Eligible(m);
                bool const meEligible = Has(eligible, _self);
                if (m.selfTrigger.auraSpell && meEligible
                    && std::find(_info.selfTriggerAuras.begin(), _info.selfTriggerAuras.end(),
                        m.selfTrigger.auraSpell) == _info.selfTriggerAuras.end())
                    _info.selfTriggerAuras.push_back(m.selfTrigger.auraSpell);

                std::vector<ObjectGuid> const carriers = _reg.Carriers(m);
                bool const meCarrier = Has(carriers, _self);
                Milliseconds from{ 0 }, to{ 0 };
                PlayerWindow(m, _now, _opt.postResolveHold, from, to);
                Solver::Timing const timing = WindowTiming(m, from, to);
                float const r = m.shape.radius;
                float const zTol = m.shape.zTolerance;

                if (m.kind == Kind::RunOut && m.measuredFrom == MeasuredFrom::Owner)
                {
                    // Only the carrier is hit, and only within r of the owner: a circle hazard on the owner.
                    if (!meCarrier || !meEligible)
                        return;
                    Unit const* owner = ObjectAccessor::GetUnit(*_bot, m.owner);
                    Geo::Pose const op = owner ? Geo::Pose{ owner->GetPositionX(), owner->GetPositionY(),
                        owner->GetPositionZ(), owner->GetOrientation() } : m.lastPose;
                    float const drift = DriftOf(owner, from);
                    ViewRules::Spot spot;
                    spot.p = { op.x, op.y, op.z };
                    spot.slackYd = drift;
                    extra.push_back(ViewRules::CarrierHazard(m.id, r, zTol, spot, _opt.safetyMarginYd, timing));
                    AddTask(m, BotTaskRole::Carrier, from, to);
                    return;
                }

                if (meCarrier)
                {
                    // My circle hits every eligible player but me (players only, decision §7.8).
                    _in.carried.push_back(ViewRules::CarriedCircle(m.id, r, zTol, _opt.safetyMarginYd, timing,
                        AllyIndices(m, eligible)));
                    std::vector<uint64> raws;
                    raws.reserve(carriers.size());
                    for (ObjectGuid const& c : carriers)
                        raws.push_back(c.GetRawValue());
                    uint32 const slot = Assign::SlotOf(raws, _self.GetRawValue());
                    if (_in.slotCount == 0)
                    {
                        _in.slot = slot;
                        _in.slotCount = static_cast<uint32>(carriers.size());
                    }
                    AddTask(m, BotTaskRole::Carrier, from, to, static_cast<uint8>(std::min<uint32>(slot, 255)));
                }
                if (!meEligible)
                    return;
                // Every other carrier's circle hits me: a hazard at its planned spot (the other side of §3.2).
                for (ObjectGuid const& c : carriers)
                {
                    if (c == _self)
                        continue;
                    Unit const* cu = ObjectAccessor::GetUnit(*_bot, c);
                    if (!cu || !cu->IsAlive())
                        continue;
                    std::optional<ViewRules::Spot> spot = SpotOf(m, c);
                    if (spot && ViewRules::PlaceOtherCircle(*spot))
                    {
                        extra.push_back(ViewRules::CarrierHazard(m.id, r, zTol, *spot, _opt.safetyMarginYd, timing));
                        if (_opt.debug && !spot->Planned())
                            _info.selfCheckMismatches += RegionSelfCheck(_reg, m, Shape::Circle(r),
                                { cu->GetPositionX(), cu->GetPositionY(), cu->GetPositionZ(), 0.f }, _self, false);
                    }
                    // Stratholme X1: a bot carrier also at its live spot, as a soft blocker for the route search (it
                    // may still walk to its planned spot, through the others' way; a human's live spot is the hard
                    // circle above). F-1b (R-1-BM M-1): an aura-keyed carrier's blocker is hot from now on.
                    if (!spot || spot->source != ViewRules::SpotSource::HumanNow)
                    {
                        ViewRules::Spot live;
                        live.p = { cu->GetPositionX(), cu->GetPositionY(), cu->GetPositionZ() };
                        live.slackYd = kLiveCarrierSlackYd;
                        live.source = ViewRules::SpotSource::BotNow;
                        _in.softHazards.push_back(ViewRules::CarrierHazard(m.id, r, zTol, live, _opt.safetyMarginYd,
                            ViewRules::SoftCarrierTiming(timing, _now, m.selfTrigger.auraSpell != 0)));
                    }
                }
                if (!meCarrier)
                    AddTask(m, BotTaskRole::Avoider, from, to);
            }

            // ---- Stack, Soak ----

            void StackOrSoak(ActiveMechanic const& m, std::vector<Solver::Hazard>& extra)
            {
                std::vector<ObjectGuid> const& eligible = Eligible(m);
                if (!Has(eligible, _self))
                    return;
                Milliseconds from{ 0 }, to{ 0 };
                PlayerWindow(m, _now, _opt.postResolveHold, from, to);
                Geo::Pose live;
                Shape const shape = _reg.EffectiveShape(m, live);
                bool const onMe = m.anchor.IsUnit() && m.anchor.unit == _self;
                if (_opt.debug)
                    _info.selfCheckMismatches += RegionSelfCheck(_reg, m, shape, live, _self, true);

                if (m.kind == Kind::Stack)
                {
                    if (onMe)
                    {
                        AddTask(m, BotTaskRole::Leader, from, to);  // the others come to me: hold
                        return;
                    }
                    AddRegion(m, shape, RegionPoses(m, shape, live, from), from, to);
                    AddTask(m, BotTaskRole::Member, from, to);
                    return;
                }

                // Soak: who claims (§3.6), the same answer on every bot.
                std::vector<uint64> const claimers = m.oneClaimPerBot ? GroupClaimers(m)
                    : Assign::SoakClaimers(ClaimantsOf(m, eligible, shape, live), SoakRuleOf(m, from));
                auto const mine = std::find(claimers.begin(), claimers.end(), _self.GetRawValue());
                if (mine != claimers.end())
                {
                    uint8 const rank = static_cast<uint8>(std::min<std::ptrdiff_t>(mine - claimers.begin(), 255));
                    if (!onMe)
                        AddRegion(m, shape, RegionPoses(m, shape, live, from), from, to);
                    AddTask(m, BotTaskRole::Claimer, from, to, rank);
                    return;
                }
                if (onMe)
                    return;     // anchored on me and not mine to take: nothing I can do
                // Not a claimer (or count 0): keep out of it (decision §7.3).
                Solver::Hazard h;
                h.id = m.id;
                h.shape = shape;
                h.poses.push_back(live);
                float drift = 0.f;
                if (m.anchor.IsUnit())
                {
                    std::optional<ViewRules::Spot> spot = SpotOf(m, m.anchor.unit);
                    if (spot && spot->Planned())
                    {
                        Geo::Pose p = live;
                        p.x = spot->p.x;
                        p.y = spot->p.y;
                        p.z = spot->p.z;
                        h.poses.push_back(p);
                    }
                    else
                        drift = DriftOf(ObjectAccessor::GetUnit(*_bot, m.anchor.unit), from);
                }
                h.marginYd = _opt.safetyMarginYd + drift;
                h.driftYd = drift;
                h.timing = WindowTiming(m, from, to);
                h.synthetic = true;
                extra.push_back(std::move(h));
                AddTask(m, BotTaskRole::Avoider, from, to);
            }

            // A Soak's claimants: the eligible players on the map, with their stacks, eta and live claim.
            std::vector<Assign::Claimant> ClaimantsOf(ActiveMechanic const& m, std::vector<ObjectGuid> const& eligible,
                Shape const& shape, Geo::Pose const& live)
            {
                float const margin = ViewRules::RegionMarginYd(shape, _opt.safetyMarginYd);
                std::vector<Assign::Claimant> players;
                for (ObjectGuid const& g : eligible)
                {
                    Player const* p = ObjectAccessor::GetPlayer(_bot->GetMap(), g);
                    if (!p)
                        continue;
                    Assign::Claimant c;
                    c.guid = g.GetRawValue();
                    c.bot = IsBot(g);
                    c.insideNow = _reg.ContainsPoint(m, p->GetPosition(), g);
                    c.stacks = m.ordering.auraSpell ? p->GetAuraCount(m.ordering.auraSpell) : 0;
                    float const sd = Solver::SignedDistance(shape, live, ToVec(p->GetPosition()));
                    float const way = std::isfinite(sd) ? std::max(0.f, sd + margin) : 0.f;
                    c.etaMs = ViewRules::SoakEtaMs(_now, way, p->GetSpeed(MOVE_RUN), _opt.params.startDelay);
                    for (Reservation const& r : m.reservations)
                        if (r.bot == g && r.claim && r.until > _now)
                            c.claimedSince = r.since.count();
                    players.push_back(c);
                }
                return players;
            }

            Assign::SoakRule SoakRuleOf(ActiveMechanic const& m, Milliseconds from) const
            {
                Assign::SoakRule rule;
                rule.count = m.soakCount;
                rule.ordered = m.ordering.auraSpell != 0;
                rule.ascending = m.ordering.ascending;
                rule.capStacks = m.ordering.capStacks;
                rule.deadlineMs = (from - _opt.params.marginMs).count();
                // A lasting soak whose window is open: a claimer trailing its moving circle keeps the claim
                // (STAGE-M3-R-REPORT m-5).
                rule.keepSticky = m.persist && _now >= from - _opt.params.marginMs;
                return rule;
            }

            // OneClaimPerBot: the claimers of `m` within its group (the live flagged Soaks of its tag, earliest resolve
            // first, then the lowest id; Assign::ExclusiveSoakClaimers). Every group member is ranked whether or not
            // this bot perceives it or is eligible for it, so every bot gets the same answer. Cached per pass.
            std::vector<uint64> GroupClaimers(ActiveMechanic const& m)
            {
                auto cached = _groupClaims.find(m.id);
                if (cached != _groupClaims.end())
                    return cached->second;
                std::vector<ActiveMechanic const*> group;
                _reg.ForEachActive(_now - _opt.postResolveHold, [&](ActiveMechanic const& s)
                {
                    if (s.kind == Kind::Soak && s.oneClaimPerBot && !s.decoy && s.tag == m.tag)
                        group.push_back(&s);
                });
                std::sort(group.begin(), group.end(), [](ActiveMechanic const* a, ActiveMechanic const* b)
                {
                    return a->resolveAt != b->resolveAt ? a->resolveAt < b->resolveAt : a->id < b->id;
                });
                std::vector<Assign::SoakInput> inputs;
                inputs.reserve(group.size());
                for (ActiveMechanic const* s : group)
                {
                    Milliseconds from{ 0 }, to{ 0 };
                    PlayerWindow(*s, _now, _opt.postResolveHold, from, to);
                    Geo::Pose live;
                    Shape const shape = _reg.EffectiveShape(*s, live);
                    inputs.push_back({ ClaimantsOf(*s, _reg.EligiblePlayers(*s), shape, live), SoakRuleOf(*s, from) });
                }
                std::vector<std::vector<uint64>> const lists = Assign::ExclusiveSoakClaimers(inputs);
                for (std::size_t i = 0; i < group.size(); ++i)
                    _groupClaims[group[i]->id] = lists[i];
                return _groupClaims[m.id];
            }

            // Stack Cluster(n): a sticky leader (slot 0 claim on the board, else the medoid); the others gather
            // within r/2 of its planned spot (ViewRules::ClusterMemberRadius).
            void Cluster(ActiveMechanic const& m)
            {
                std::vector<ObjectGuid> const& eligible = Eligible(m);
                if (!Has(eligible, _self))
                    return;
                Milliseconds from{ 0 }, to{ 0 };
                PlayerWindow(m, _now, _opt.postResolveHold, from, to);
                std::vector<Assign::Point> points;
                for (ObjectGuid const& g : eligible)
                    if (Player const* p = ObjectAccessor::GetPlayer(_bot->GetMap(), g))
                        points.push_back({ g.GetRawValue(), p->GetPositionX(), p->GetPositionY() });
                uint64 sticky = 0;
                Milliseconds stickySince = Solver::kForever;
                for (Reservation const& r : m.reservations)
                    if (r.claim && r.slot == 0 && r.until > _now && r.since < stickySince && Has(eligible, r.bot))
                    {
                        sticky = r.bot.GetRawValue();
                        stickySince = r.since;
                    }
                uint64 const leader = Assign::ClusterLeader(points, sticky);
                if (!leader)
                    return;
                if (leader == _self.GetRawValue())
                {
                    AddTask(m, BotTaskRole::Leader, from, to, 0);
                    return;
                }
                ObjectGuid const leaderGuid(leader);
                std::optional<ViewRules::Spot> spot = SpotOf(m, leaderGuid);
                if (!spot)
                    return;
                Shape circle = Shape::Circle(ViewRules::ClusterMemberRadius(m.shape.radius));
                circle.zTolerance = m.shape.zTolerance;
                Geo::Pose const center{ spot->p.x, spot->p.y, spot->p.z, 0.f };
                std::vector<Geo::Pose> poses{ center };
                if (!spot->Planned())
                {
                    Unit const* u = ObjectAccessor::GetUnit(*_bot, leaderGuid);
                    NoteMoving(u);
                    poses = ViewRules::MovingPoses(center, u ? u->GetOrientation() : 0.f,
                        ViewRules::RegionDriftYd(circle, DriftOf(u, from)));
                }
                AddRegion(m, circle, std::move(poses), from, to);
                AddTask(m, BotTaskRole::Member, from, to, 1);
            }

            // ---- Bait ----

            void Bait(ActiveMechanic const& m, std::vector<Solver::Hazard>& extra)
            {
                std::vector<ObjectGuid> const& eligible = Eligible(m);
                if (!Has(eligible, _self))
                    return;
                Milliseconds from{ 0 }, to{ 0 };
                PlayerWindow(m, _now, _opt.postResolveHold, from, to);
                Solver::Timing const timing = WindowTiming(m, from, to);

                // The pick: a sticky claim on the board (the picker's), else the registry's live rule (§3.6).
                ObjectGuid pick;
                Milliseconds pickSince = Solver::kForever;
                for (Reservation const& r : m.reservations)
                    if (r.claim && r.until > _now && r.since < pickSince && Has(eligible, r.bot))
                    {
                        pick = r.bot;
                        pickSince = r.since;
                    }
                if (pick.IsEmpty())
                    pick = _reg.BaitPick(m);

                Unit const* owner = ObjectAccessor::GetUnit(*_bot, m.owner);
                Geo::Pose const op = owner ? Geo::Pose{ owner->GetPositionX(), owner->GetPositionY(),
                    owner->GetPositionZ(), owner->GetOrientation() } : m.lastPose;
                float const ownerDrift = DriftOf(owner, from);
                NoteMoving(owner);
                bool const pickOnly = Judge::BaitHitsPickOnly(m.shape.radius);
                float const hitR = Judge::BaitHitRadius(m.shape.radius);
                float const zTol = m.shape.zTolerance;

                if (pick == _self)
                {
                    // Run out past everyone: a circle on the owner beyond othersRange and every human, away from
                    // every other player's landing spot, as far as the solver can reach.
                    std::vector<float> humans;
                    for (ObjectGuid const& g : eligible)
                    {
                        if (g == _self || IsBot(g))
                            continue;
                        if (Unit const* u = ObjectAccessor::GetUnit(*_bot, g))
                            humans.push_back(Geo::Dist2d(op.x, op.y, u->GetPositionX(), u->GetPositionY())
                                + _opt.humanSlackYd);
                    }
                    ViewRules::Spot ownerSpot;
                    ownerSpot.p = { op.x, op.y, op.z };
                    ownerSpot.slackYd = ownerDrift;
                    extra.push_back(ViewRules::CarrierHazard(m.id, ViewRules::BaitPickerRadius(m.othersRange, humans),
                        zTol, ownerSpot, _opt.safetyMarginYd, timing));
                    if (!pickOnly)
                        for (ObjectGuid const& g : eligible)
                            if (g != _self)
                                if (std::optional<ViewRules::Spot> spot = SpotOf(m, g))
                                    extra.push_back(ViewRules::CarrierHazard(m.id, hitR, zTol, *spot,
                                        _opt.safetyMarginYd, timing));
                    _in.far.present = true;
                    _in.far.from = { op.x, op.y, op.z };
                    AddTask(m, BotTaskRole::Picker, from, to);
                    return;
                }

                // Everyone else: within othersRange of the owner, out of the picker's landing circle.
                if (m.othersRange > 0.f)
                {
                    Shape range = Shape::Circle(m.othersRange);
                    range.zTolerance = zTol;
                    AddRegion(m, range, ViewRules::MovingPoses(op, op.o, ViewRules::RegionDriftYd(range, ownerDrift)),
                        from, to);
                }
                if (!pickOnly && !pick.IsEmpty())
                {
                    std::optional<ViewRules::Spot> spot = SpotOf(m, pick);
                    if (spot && ViewRules::PlaceOtherCircle(*spot))
                        extra.push_back(ViewRules::CarrierHazard(m.id, hitR, zTol, *spot, _opt.safetyMarginYd,
                            timing));
                }
                AddTask(m, BotTaskRole::Member, from, to);
            }

            // ---- HoldStill, Spot ----

            void HoldStill(ActiveMechanic const& m)
            {
                bool const listed = m.targets.empty() ? Has(Eligible(m), _self) : m.targets.count(_self) > 0;
                if (!listed)
                    return;
                Milliseconds from{ 0 }, to{ 0 };
                PlayerWindow(m, _now, _opt.postResolveHold, from, to);
                Milliseconds const freezeFrom = from - _opt.params.marginMs;
                ViewRules::MergeFreeze(_in.freeze, freezeFrom, to, _opt.freezeSettle, _now);
                AddTask(m, BotTaskRole::Frozen, freezeFrom, to);
            }

            void SpotKind(ActiveMechanic const& m)
            {
                auto it = m.spots.find(_self);
                if (it == m.spots.end())
                    return;
                Milliseconds from{ 0 }, to{ 0 };
                PlayerWindow(m, _now, _opt.postResolveHold, from, to);
                Shape circle = Shape::Circle(m.shape.radius);
                circle.zTolerance = m.shape.zTolerance;
                Geo::Pose const pose{ it->second.GetPositionX(), it->second.GetPositionY(), it->second.GetPositionZ(),
                    0.f };
                if (_opt.debug)
                    _info.selfCheckMismatches += RegionSelfCheck(_reg, m, circle, pose, _self, true);
                AddRegion(m, circle, { pose }, from, to);
                AddTask(m, BotTaskRole::Member, from, to);
            }

            Registry const& _reg;
            Player const* _bot;
            ObjectGuid const _self;
            Milliseconds const _now;
            BotViewOptions const& _opt;
            BotViewHooks const* _hooks;
            Solver::Input& _in;
            BotViewInfo& _info;
            std::unordered_map<uint32, std::size_t> _baseHazards;   // registry hazard id -> index in _in.hazards
            uint32 _eligibleFor = 0;
            std::vector<ObjectGuid> _eligible;
            std::unordered_map<uint32, std::vector<uint64>> _groupClaims;   // OneClaimPerBot: mechanic id -> claimers
            std::vector<uint32> _dropEnterable;     // X2: the drop's enterable spots (their Zones' mechanic ids)
        };

        void RunPlayerPass(Registry const& reg, Player const* bot, Milliseconds now, BotViewOptions const& opt,
            BotViewHooks const* hooks, Solver::Input& in, BotViewInfo& info, PerceptionFilter const* perceive,
            bool countHidden)
        {
            std::erase_if(in.hazards, [](Solver::Hazard const& h) { return h.synthetic; });
            in.softHazards.clear();
            in.regions.clear();
            in.allies.clear();
            in.carried.clear();
            in.far = Solver::FarFrom();
            in.freeze = Solver::Freeze();
            in.drop = Solver::Drop();
            in.bounds.clear();
            in.slot = 0;
            in.slotCount = 0;
            info.tasks.clear();
            info.selfTriggerAuras.clear();
            info.tankFrontals.clear();
            info.parks.clear();
            info.reservationRevision = reg.ReservationRevision();
            if (!bot)
                return;
            PlayerPass(reg, bot, now, opt, hooks, in, info).Run(perceive, countHidden);
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
        o.humanSlackYd = cfg.humanSlackYd;
        o.lookAwayMarginDeg = cfg.lookAwayMarginDeg;
        o.freezeSettle = Milliseconds(cfg.freezeSettleMs);
        o.params.w.region = cfg.regionComfortWeight;
        o.params.w.spacing = cfg.spacingWeight;
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
        Solver::Input& out, PerceptionFilter const* perceive, BotViewHooks const* hooks)
    {
        BotViewInfo info;
        info.revision = reg.Revision();
        out.hazards.clear();
        out.lanes.clear();
        out.now = now;
        out.params = opt.params;
        if (!bot)
        {
            RunPlayerPass(reg, bot, now, opt, hooks, out, info, perceive, true);
            return info;
        }

        out.bot = ToVec(bot->GetPosition());
        out.speed = bot->GetSpeed(MOVE_RUN);
        ObjectGuid const self = bot->GetGUID();
        float const reachYd = out.speed * std::chrono::duration<float>(opt.params.horizon).count() + kReachSlackYd;
        float const slackRad = Geo::DegToRad(opt.facingSlackDeg);

        std::unordered_set<uint32> kept;     // hazard ids in the view (lane rects are filtered by them)
        reg.ForEachActive(now - opt.postResolveHold, [&](ActiveMechanic const& m)
        {
            if (!m.IsShapeKind() || m.decoy)    // a decoy is empty floor to the bots (Judge::DecoyFits)
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
            h.driftYd = drift;
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

        RunPlayerPass(reg, bot, now, opt, hooks, out, info, perceive, true);
        return info;
    }

    void RefreshAllies(Registry const& reg, Player const* bot, Milliseconds now, BotViewOptions const& opt,
        BotViewHooks const* hooks, Solver::Input& input, BotViewInfo* info, PerceptionFilter const* perceive)
    {
        input.now = now;
        if (bot)
        {
            input.bot = ToVec(bot->GetPosition());
            input.speed = bot->GetSpeed(MOVE_RUN);
        }
        BotViewInfo scratch;
        RunPlayerPass(reg, bot, now, opt, hooks, input, info ? *info : scratch, perceive, false);
    }

    char const* BotTaskRoleName(BotTaskRole role)
    {
        switch (role)
        {
            case BotTaskRole::Carrier: return "carrier";
            case BotTaskRole::Avoider: return "avoider";
            case BotTaskRole::Claimer: return "claimer";
            case BotTaskRole::Leader: return "leader";
            case BotTaskRole::Picker: return "picker";
            case BotTaskRole::Member: return "member";
            case BotTaskRole::Frozen: return "frozen";
            case BotTaskRole::Tank: return "tank";
            case BotTaskRole::Parker: return "parker";
        }
        return "?";
    }

    void PublishReservations(Registry& reg, Player const* bot, BotViewInfo const& info, Solver::Plan const& plan,
        Milliseconds now)
    {
        if (!bot)
            return;
        ObjectGuid const self = bot->GetGUID();
        // The facing matters to the others only for a look-away facing or a shape that turns with the bot; anything
        // else publishes 0, so the bot's turning never bumps the board (STAGE-M3-R-REPORT n-15).
        bool turnsWithMe = plan.hasFacing;
        if (!turnsWithMe)
            reg.ForEachActive(now, [&](ActiveMechanic const& m)
            {
                turnsWithMe = turnsWithMe
                    || (!m.decoy && m.anchor.IsUnit() && m.anchor.unit == self && m.anchor.followFacing);
            });
        float const facing = plan.hasFacing ? plan.facing : (turnsWithMe ? bot->GetOrientation() : 0.f);
        std::unordered_set<uint32> published;
        for (BotTask const& t : info.tasks)
        {
            if (t.role == BotTaskRole::Tank || t.role == BotTaskRole::Parker || t.role == BotTaskRole::Frozen)
                continue;
            // A task ending now is pruned by this same update (STAGE-M3-R-REPORT n-13).
            if (t.to <= now || !reg.Find(t.mechanicId))
                continue;
            Reservation r;
            r.bot = self;
            r.point = Position(plan.target.x, plan.target.y, plan.target.z, facing);
            r.until = std::min(t.to, Registry::kForever);
            r.facing = facing;
            r.slot = t.slot;
            r.claim = t.role == BotTaskRole::Claimer || t.role == BotTaskRole::Picker || t.role == BotTaskRole::Leader;
            reg.Reserve(t.mechanicId, r);
            published.insert(t.mechanicId);
        }
        // Mechanics the bot left (no longer eligible, no longer a claimer, ...): off the board.
        std::vector<uint32> left;
        reg.ForEachActive(Milliseconds(0), [&](ActiveMechanic const& m)
        {
            if (published.count(m.id))
                return;
            for (Reservation const& r : m.reservations)
                if (r.bot == self)
                {
                    left.push_back(m.id);
                    return;
                }
        });
        for (uint32 id : left)
            reg.Unreserve(id, self);
    }

    void DropReservations(Registry& reg, ObjectGuid bot)
    {
        std::vector<uint32> held;
        reg.ForEachActive(Milliseconds(0), [&](ActiveMechanic const& m)
        {
            for (Reservation const& r : m.reservations)
                if (r.bot == bot)
                {
                    held.push_back(m.id);
                    return;
                }
        });
        for (uint32 id : held)
            reg.Unreserve(id, bot);
    }

    NavOracle::NavOracle(Unit const* bot, NavOracleStats* stats, uint32 maxSightChecks)
        : _bot(bot), _stats(stats), _maxSightChecks(maxSightChecks)
    {
        // The polygon filter PathGenerator::CreateFilter gives a bot (a headless session): ground and water, no
        // lava, slime or steep slopes. Other players: everything walkable.
        Player const* player = bot ? bot->ToPlayer() : nullptr;
        if (player && player->GetSession() && player->GetSession()->IsHeadless())
        {
            _includeFlags = NAV_GROUND | NAV_WATER;
            _excludeFlags = NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP;
        }
        else
            _includeFlags = NAV_GROUND | NAV_GROUND_STEEP | NAV_WATER | NAV_MAGMA;
    }

    bool NavOracle::Walkable(Geo::Vec3& p)
    {
        if (_stats)
            ++_stats->probes;
        if (!_bot || !_bot->IsInWorld())
            return true;    // Find decides (and fails)
        dtNavMeshQuery const* query = _bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
        if (!query)
        {
            if (_stats)
                ++_stats->probeNoMesh;
            return true;
        }

        dtQueryFilter filter;
        filter.setIncludeFlags(_includeFlags);
        filter.setExcludeFlags(_excludeFlags);
        // Detour works in (y, z, x).
        float const center[3] = { p.y, p.z, p.x };
        float const extents[3] = { kProbeSnapYd, kProbeUpYd, kProbeSnapYd };
        float nearest[3] = { 0.f, 0.f, 0.f };
        dtPolyRef ref = 0;
        if (dtStatusFailed(query->findNearestPoly(center, extents, &filter, &ref, nearest)) || !ref
            || Geo::Dist2d(nearest[2], nearest[0], p.x, p.y) > kProbeSnapYd)
        {
            if (_stats)
                ++_stats->probeOff;
            return false;
        }
        Geo::Vec3 const snapped{ nearest[2], nearest[0], nearest[1] };
        if (_stats && (Geo::Dist2d(snapped.x, snapped.y, p.x, p.y) > kProbeMovedYd
            || std::fabs(snapped.z - p.z) > kProbeMovedYd))
            ++_stats->probeSnapped;
        p = snapped;
        return true;
    }

    bool NavOracle::Find(Geo::Vec3 const& from, Geo::Vec3 const& to, std::vector<Geo::Vec3>& out)
    {
        ++_calls;
        if (_stats)
            ++_stats->calls;
        out.clear();
        if (!_bot || !_bot->IsInWorld())
            return false;

        PathGenerator path(_bot);
        path.CalculatePath(from.x, from.y, from.z, to.x, to.y, to.z, false);
        uint32 const type = path.GetPathType();
        uint32* reject = nullptr;
        if (type & PATHFIND_NOPATH)
            reject = _stats ? &_stats->noPath : nullptr;
        else if (type & PATHFIND_NOT_USING_PATH)
            reject = _stats ? &_stats->notUsingPath : nullptr;
        else if (type & PATHFIND_SHORT)
            reject = _stats ? &_stats->tooLong : nullptr;
        else if (type & PATHFIND_SHORTCUT)
            reject = _stats ? &_stats->shortcut : nullptr;
        else if (type & PATHFIND_FARFROMPOLY_START)
            reject = _stats ? &_stats->farStart : nullptr;
        else if (!(type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)))
            reject = _stats ? &_stats->otherType : nullptr;
        else
        {
            Movement::PointsArray const& points = path.GetPath();
            if (points.empty())
                reject = _stats ? &_stats->empty : nullptr;
            else if (Geo::Dist2d(points.back().x, points.back().y, to.x, to.y) > kMaxEndShiftYd)
                reject = _stats ? &_stats->endFar : nullptr;
            else
            {
                out.reserve(points.size() + 1);
                G3D::Vector3 const& first = points.front();
                if (Geo::Dist2d(first.x, first.y, from.x, from.y) > kPathStartToleranceYd)
                    out.push_back(from);
                for (G3D::Vector3 const& p : points)
                    out.push_back({ p.x, p.y, p.z });
                if (_stats)
                {
                    ++_stats->ok;
                    if (type & PATHFIND_INCOMPLETE)
                        ++_stats->okIncomplete;
                    if (type & PATHFIND_FARFROMPOLY_END)
                        ++_stats->farEnd;
                    if (Geo::Dist2d(points.back().x, points.back().y, to.x, to.y) > kShiftedYd)
                        ++_stats->okShifted;
                }
                return true;
            }
        }
        if (reject)
            ++*reject;
        return false;
    }

    bool NavOracle::InSight(Geo::Vec3 const& from, Geo::Vec3 const& to)
    {
        if (!_bot || !_bot->IsInWorld())
            return true;
        if (_sightRays >= _maxSightChecks)
        {
            if (_stats)
                ++_stats->sightCapped;
            return true;
        }
        ++_sightRays;
        if (_stats)
            ++_stats->sightRays;
        float const lift = _bot->GetCollisionHeight();
        bool const clear = _bot->GetMap()->isInLineOfSight(from.x, from.y, from.z + lift, to.x, to.y, to.z + lift,
            _bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
        if (!clear && _stats)
            ++_stats->sightBlocked;
        return clear;
    }

    void NavOracleStats::Add(NavOracleStats const& o)
    {
        probes += o.probes;
        probeNoMesh += o.probeNoMesh;
        probeOff += o.probeOff;
        probeSnapped += o.probeSnapped;
        calls += o.calls;
        ok += o.ok;
        okIncomplete += o.okIncomplete;
        okShifted += o.okShifted;
        noPath += o.noPath;
        notUsingPath += o.notUsingPath;
        tooLong += o.tooLong;
        shortcut += o.shortcut;
        farStart += o.farStart;
        otherType += o.otherType;
        empty += o.empty;
        endFar += o.endFar;
        farEnd += o.farEnd;
        sightRays += o.sightRays;
        sightBlocked += o.sightBlocked;
        sightCapped += o.sightCapped;
    }

    std::string FormatNavStats(NavOracleStats const& s)
    {
        return Acore::StringFormat("probes {} (no mesh {}, off {}, snapped {}), paths {} (ok {}, incomplete {}, "
            "end moved {}, far end {}; no path {}, not using path {}, too long {}, shortcut {}, far start {}, "
            "other {}, empty {}, end > {} yd {}), sight rays {} (blocked {}, capped {})", s.probes, s.probeNoMesh,
            s.probeOff, s.probeSnapped, s.calls, s.ok, s.okIncomplete, s.okShifted, s.farEnd, s.noPath,
            s.notUsingPath, s.tooLong, s.shortcut, s.farStart, s.otherType, s.empty, kMaxEndShiftYd, s.endFar,
            s.sightRays, s.sightBlocked, s.sightCapped);
    }

    std::string FormatSolveStats(Solver::SolveStats const& s)
    {
        return Acore::StringFormat("kept {}, probes {} (off {}, snapped {}), no path {}, end unsafe {}, crosses start "
            "{} / other {}, budget stops {}, out of bounds {}", s.kept, s.probes, s.offMesh, s.snapped, s.noPath,
            s.endUnsafe, s.crossStart, s.crossOther, s.budgetStops, s.outOfBounds);
    }
}
