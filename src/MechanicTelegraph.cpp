/*
 * mod-boss-mechanics: telegraph markers (STAGE-M1-HANDOFF §2.5, §3.8). See MechanicTelegraph.h.
 */

#include "MechanicTelegraph.h"
#include "BossMechanicsConfig.h"
#include "BossMechanicsIds.h"
#include "Creature.h"
#include "DataMap.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MechanicGround.h"
#include "MechanicMath.h"
#include "MechanicRegistry.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "StringFormat.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "TemporarySummon.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <tuple>
#include <unordered_map>

namespace BossMechanics::Telegraph
{
    namespace
    {
        constexpr char const* kLog = "module.bossmechanics";
        constexpr char const* kStateKey = "mod-boss-mechanics-telegraph";
        // Spline step (§2.5): move when the target moved more than this or turned more than this.
        constexpr float kMoveThresholdYd = 0.25f;
        constexpr float kTurnThresholdRad = 2.f * Geo::kPi / 180.f;
        constexpr float kMinSplineSpeed = 0.5f;

        struct MarkerState
        {
            uint32 mechanicId = 0;
            ObjectGuid follow;          // carrier kinds: the unit this marker sits on (empty: the mechanic's anchor)
            VisualPiece piece;
            bool facingMatters = false; // cones and lines turn; discs, rings and donuts do not
            Geo::Pose sent;             // last commanded pose (lift included)
            bool followStarted = false; // FollowMode::MoveFollow issued
            bool fillLayer = false;     // a fill marker (SyncFillSlot); else an outline (or a carrier ring)
        };

        struct MechanicState
        {
            Milliseconds nextFollowAt{ 0 };
            Milliseconds nextFillAt{ 0 };
            uint32 fillStepMs = 0;      // this mechanic's fill step (FillStepInterval), set at the spawn
            bool liveShown = false;     // the fill shows the live-danger state (resolved, armed mine)
            bool fillFinal = false;     // a static mechanic whose fill reached its last state: no more fill steps
            // The fill layer (MarkerState entries in TelegraphState::markers too). Shape kinds, Stack/Soak and Spot:
            // slot k belongs to outline piece / spot k (empty guid: nothing drawn there yet). Spread/RunOut: one per
            // host, keyed by the host.
            std::vector<ObjectGuid> fill;
            std::unordered_map<ObjectGuid, ObjectGuid> carrierFill;
            // Ground snap (MechanicGround.h). lineSteps: a static line's terrain split, made once at the spawn (empty:
            // the default layout). outline: the last outline pick with its heights (shape kinds, Stack/Soak), which
            // the fill takes its heights from (AssignFillZ). zCache: a static anchor's piece heights by (display,
            // forward), so a shrinking projectile line samples each new piece once.
            LineSteps lineSteps;
            VisualPick outline;
            std::vector<VisualPiece> zCache;
            uint32 groundQueries = 0;   // height queries made so far (logged once after the spawn under Debug)
        };

        // Per-map follow state; lives in Map::CustomData, freed with the map.
        struct TelegraphState : public DataMap::Base
        {
            std::unordered_map<ObjectGuid, MarkerState> markers;
            std::unordered_map<uint32, MechanicState> mechanics;
        };

        TelegraphState& StateOf(Map* map)
        {
            if (TelegraphState* s = map->CustomData.Get<TelegraphState>(kStateKey))
                return *s;
            TelegraphState* s = new TelegraphState();
            map->CustomData.Set(kStateKey, s);
            return *s;
        }

        Unit* FindUnit(Map* map, ObjectGuid guid)
        {
            if (guid.IsEmpty())
                return nullptr;
            if (guid.IsPlayer())
                return ObjectAccessor::GetPlayer(map, guid);
            if (guid.IsPet())
                return map->GetPet(guid);
            if (guid.IsCreatureOrVehicle())
                return map->GetCreature(guid);
            return nullptr;
        }

        Geo::Pose PoseOf(Position const& p)
        {
            return { p.GetPositionX(), p.GetPositionY(), p.GetPositionZ(), p.GetOrientation() };
        }

        Position PositionOf(Geo::Pose const& p)
        {
            return Position(p.x, p.y, p.z, Geo::Wrap2Pi(p.o));
        }

        char const* MarkerShapeName(Ids::MarkerShape shape)
        {
            static char const* const names[] = { "disc", "ring", "timer-ring", "donut", "donut-outline", "cone",
                "cone-outline", "dashed-ring", "line", "line-outline" };
            std::size_t const i = static_cast<std::size_t>(shape);
            return i < std::size(names) ? names[i] : "?";
        }

        // One line about a live marker (`.mech markers`, the Debug log): what the server set on the creature and what
        // it means to draw. `from`: also the distance and bearing (absolute and relative to from's facing).
        std::string DescribeMarker(Creature const* c, MarkerState const& s, Position const* from)
        {
            Ids::MarkerDisplayParts const parts = Ids::DecodeMarkerDisplay(s.piece.display);
            bool const line = parts.valid && Ids::IsLineShape(parts.shape);
            float const k = line ? LineAspect(parts.step) : 1.f;
            float const scale = c->GetObjectScale();
            float const extent = k * scale;                 // the drawn length (line) or radius, at the real scale
            float const o = c->GetOrientation();
            std::string out = Acore::StringFormat("#{} {} {} disp {} ({} c{} step {}{}) scale {:.3f} (want {:.3f}) "
                "native {} cur {} at {:.2f} {:.2f} {:.2f} o {:.3f}", s.mechanicId, s.fillLayer ? "fill" : "outline",
                parts.valid ? MarkerShapeName(parts.shape) : "?", s.piece.display, parts.valid ? "ok" : "bad",
                parts.color, parts.step, line ? Acore::StringFormat(" k {:.4f}", k) : std::string(), scale,
                s.piece.scale, c->GetNativeDisplayId(), c->GetDisplayId(), c->GetPositionX(), c->GetPositionY(),
                c->GetPositionZ(), o);
            if (from)
            {
                float const dx = c->GetPositionX() - from->GetPositionX();
                float const dy = c->GetPositionY() - from->GetPositionY();
                float const bearing = std::atan2(dy, dx);
                out += Acore::StringFormat(" | {:.2f} yd brg {:.1f} (rel {:+.1f}) deg", std::hypot(dx, dy),
                    Geo::RadToDeg(Geo::Wrap2Pi(bearing)), Geo::RadToDeg(Geo::WrapPi(bearing - from->GetOrientation())));
            }
            if (line)
                out += Acore::StringFormat(" | len {:.2f} x w {:.2f}, far end {:.2f} {:.2f}", extent, scale,
                    c->GetPositionX() + std::cos(o) * extent, c->GetPositionY() + std::sin(o) * extent);
            else
                out += Acore::StringFormat(" | radius {:.2f}", extent);
            if (c->movespline && !c->movespline->Finalized())
                out += " | moving";
            return out;
        }

        void LogMarker(char const* what, Creature const* c, MarkerState const& s)
        {
            if (GetConfig().debug)
                LOG_INFO(kLog, "Marker {}: {}", what, DescribeMarker(c, s, nullptr));
        }

        // Where a piece sits: `forward` yards along anchor facing + offset, facing that way (PieceWorldPose), at its
        // ground height (the ground snap; lift included) or lifted above the anchor.
        Geo::Pose PiecePose(Geo::Pose const& anchor, VisualPiece const& piece)
        {
            Geo::Pose p = PieceWorldPose(anchor, piece);
            p.z = piece.hasZ ? piece.z : p.z + GetConfig().lift;
            return p;
        }

        uint8 ColorIndex(ActiveMechanic const& m)
        {
            return static_cast<uint8>(m.color);
        }

        // Rings on players: Spread/RunOut carriers, and every eligible player of a Stack Cluster(n) (get-in rings).
        bool IsCarrierKind(ActiveMechanic const& m)
        {
            return m.kind == Kind::Spread || m.kind == Kind::RunOut || m.IsCluster();
        }

        // How a mechanic is drawn: Shape = the hitbox's pieces (shape kinds, and a Stack/Soak of any shape but a
        // circle, M3 B1, in the get-in color); Carriers = rings on players; GetIn = a disc on the anchor (Stack/Soak
        // circles) or on each spot (Spot); None = no marker (Bait, HoldStill). A Jump draws its shape like a Zone.
        enum class DrawPath : uint8
        {
            Shape,
            Carriers,
            GetIn,
            None
        };

        DrawPath DrawPathOf(ActiveMechanic const& m)
        {
            if (m.IsShapeKind())
                return DrawPath::Shape;
            if (IsCarrierKind(m))
                return DrawPath::Carriers;
            switch (m.kind)
            {
                case Kind::Stack:
                case Kind::Soak:
                    return m.shape.type == ShapeType::Circle ? DrawPath::GetIn : DrawPath::Shape;
                case Kind::Spot:
                    return DrawPath::GetIn;
                case Kind::Jump:
                    return DrawPath::Shape;     // Stratholme X3: the swept area, its fill the countdown to the jump
                default:
                    return DrawPath::None;
            }
        }

        // The players a carrier-path mechanic draws its rings on.
        std::vector<ObjectGuid> RingHosts(Registry const& reg, ActiveMechanic const& m)
        {
            return m.IsCluster() ? reg.EligiblePlayers(m) : reg.Carriers(m);
        }

        bool IsRunOutFromOwner(ActiveMechanic const& m)
        {
            return m.kind == Kind::RunOut && m.measuredFrom == MeasuredFrom::Owner;
        }

        bool ShrinksProjectile(ActiveMechanic const& m)
        {
            return GetConfig().shrinkProjectileLines && m.projectile && !m.projectile->outAndBack
                && m.projectile->unit.IsEmpty() && m.projectile->speed > 0.f;
        }

        // Markers that may need moving after the spawn.
        bool IsDynamic(ActiveMechanic const& m)
        {
            if (IsCarrierKind(m))
                return true;
            if (m.anchor.IsUnit() || (m.to && m.to->IsUnit()))
                return true;
            return ShrinksProjectile(m);
        }

        // The drawn shape of a shape kind right now (pose = live anchor pose): the hitbox, except that a shrinking
        // projectile line drops the part the projectile's tail has already passed, in whole steps of its width
        // (ShrunkExcludeStart: cosmetic, config), and a line with a draw strip (ExceptOne's last lane) draws only that
        // strip.
        Shape DrawnShape(Registry const& reg, ActiveMechanic const& m, Milliseconds now, Geo::Pose& pose)
        {
            Shape shape = reg.EffectiveShape(m, pose);
            ApplyDrawStrip(shape, pose, m.drawLateral, m.drawWidth);
            if (ShrinksProjectile(m) && now > m.resolveAt)
            {
                float const half = m.projectile->halfLength > 0.f ? m.projectile->halfLength : shape.width * 0.5f;
                float const tail = m.projectile->speed * SecondsOf(now - m.resolveAt) - half;
                shape.excludeStart = ShrunkExcludeStart(tail, shape.excludeStart, shape.length, shape.width);
            }
            return shape;
        }

        // The fill layer applies to every drawn kind except a travelling projectile (the projectile is its own timer).
        bool FillApplies(ActiveMechanic const& m)
        {
            return m.telegraph && !m.projectile && m.kind != Kind::Bait && m.kind != Kind::HoldStill;
        }

        // The danger is live (the fill draws full): a mine once armed, anything else from its resolve on (a
        // persistent zone for the rest of its life; a one-shot resolve only for the tick it despawns in).
        bool DangerLive(ActiveMechanic const& m, Milliseconds now)
        {
            return m.mine ? m.mine->armed : now >= m.resolveAt;
        }

        // An armed mine draws the dashed ring and no fill (PickArmedMine): a solid fill means a hit is coming, the
        // dashed ring that the mine waits for a player.
        bool ArmedMine(ActiveMechanic const& m)
        {
            return m.mine && m.mine->armed;
        }

        // The outline pieces of a shape kind: the hitbox's outline (a line laid out by `lineSteps` when given), or an
        // armed mine's dashed ring.
        VisualPick OutlinePick(ActiveMechanic const& m, Shape const& shape, LineSteps const* lineSteps = nullptr)
        {
            return ArmedMine(m) ? PickArmedMine(shape, ColorIndex(m)) : PickVisual(shape, ColorIndex(m),
                MarkerLayer::Outline, lineSteps);
        }

        LineSteps const* LineStepsOf(MechanicState const& ms)
        {
            return ms.lineSteps.empty() ? nullptr : &ms.lineSteps;
        }

        // ------------------------------------------------------------------------------------------------------------
        // Ground snap (MechanicGround.h)
        // ------------------------------------------------------------------------------------------------------------

        // The walkable floor at or below zFrom: terrain, WMOs (buildings, bridges) and gameobjects in the phase.
        // Map::GetHeight takes the ADT height only when zFrom is at or above it, and casts the vmap ray down at most
        // maxDown; INVALID_HEIGHT (or lower) when there is nothing.
        struct MapProbe
        {
            Map const* map = nullptr;
            uint32 phaseMask = PHASEMASK_NORMAL;

            float operator()(float x, float y, float zFrom, float maxDown) const
            {
                return map->GetHeight(phaseMask, x, y, zFrom, true, maxDown);
            }
        };

        using GroundSampler = Ground::Sampler<MapProbe const>;

        Ground::Settings GroundSettings()
        {
            ModuleConfig const& cfg = GetConfig();
            Ground::Settings st;
            st.sampleYd = cfg.groundSampleYd;
            st.maxSpreadYd = cfg.maxPieceSpreadYd;
            st.lift = cfg.lift;
            return st;
        }

        MapProbe ProbeFor(Map* map, ActiveMechanic const& m)
        {
            Unit* owner = FindUnit(map, m.owner);
            return { map, owner ? owner->GetPhaseMask() : uint32(PHASEMASK_NORMAL) };
        }

        // The anchor itself moves (a unit anchor, a Line(from, to) with a unit end): its markers sample the cheap set.
        bool AnchorMoves(ActiveMechanic const& m)
        {
            return m.anchor.IsUnit() || (m.to && m.to->IsUnit());
        }

        // A static anchor's piece height, sampled once per (display, forward) and cached.
        void CachedPieceZ(MechanicState& ms, GroundSampler& sampler, Geo::Pose const& anchor, VisualPiece& piece)
        {
            for (VisualPiece const& c : ms.zCache)
            {
                if (c.display == piece.display && std::fabs(c.forward - piece.forward) < 1e-3f
                    && std::fabs(c.scale - piece.scale) < 1e-3f)
                {
                    piece.hasZ = true;
                    piece.z = c.z;
                    return;
                }
            }
            piece.z = Ground::PieceZ(sampler, anchor, piece, GroundSettings(), false);
            piece.hasZ = true;
            ms.zCache.push_back(piece);
        }

        // The outline of a shape kind with its heights. A static line is split by the terrain at its spawn (the
        // layout is kept in ms.lineSteps for the fill and the mine swap); a static anchor samples each piece's whole
        // footprint once (cached); a moving anchor samples the cheap set on every follow step. Off: no heights.
        VisualPick PlacedOutline(Map* map, ActiveMechanic const& m, MechanicState& ms, Shape const& shape,
            Geo::Pose const& pose, bool spawning)
        {
            ModuleConfig const& cfg = GetConfig();
            if (!cfg.groundSnap)
            {
                ms.lineSteps.clear();
                ms.outline = OutlinePick(m, shape);
                return ms.outline;
            }
            MapProbe const probe = ProbeFor(map, m);
            GroundSampler sampler(probe, pose.z, shape.zTolerance);
            Ground::Settings const st = GroundSettings();
            VisualPick pick;
            if (spawning && !IsDynamic(m) && shape.type == ShapeType::Line && !ArmedMine(m))
            {
                Ground::LineSplit const split = Ground::SplitLine(shape, pose, sampler, st);
                ms.lineSteps = split.steps;
                pick = OutlinePick(m, shape, LineStepsOf(ms));
                if (pick.pieces.size() == split.z.size())
                {
                    for (std::size_t k = 0; k < pick.pieces.size(); ++k)
                    {
                        pick.pieces[k].hasZ = true;
                        pick.pieces[k].z = split.z[k];
                    }
                }
                else
                    Ground::AssignZ(pick, sampler, pose, st, false);
                if (cfg.debug && split.minLengthStopped)
                    LOG_INFO(kLog, "Mechanic {} (tag {}): line piece still spans more than {:.2f} yd of height at "
                        "its minimum length", m.id, m.tag, st.maxSpreadYd);
            }
            else
            {
                pick = OutlinePick(m, shape, LineStepsOf(ms));
                bool const moving = AnchorMoves(m);
                for (VisualPiece& piece : pick.pieces)
                {
                    if (moving)
                    {
                        piece.z = Ground::PieceZ(sampler, pose, piece, st, true);
                        piece.hasZ = true;
                    }
                    else
                        CachedPieceZ(ms, sampler, pose, piece);
                }
            }
            ms.groundQueries += sampler.Queries();
            ms.outline = pick;
            return pick;
        }

        // One disc-like piece on a point or a unit (Stack/Soak/Spot, carrier rings): its whole footprint (`full`) or
        // the cheap set, in the band of the mechanic's own zTolerance.
        void PlaceRound(Map* map, ActiveMechanic const& m, MechanicState& ms, Geo::Pose const& at, VisualPiece& piece,
            bool full)
        {
            if (!GetConfig().groundSnap)
                return;
            MapProbe const probe = ProbeFor(map, m);
            GroundSampler sampler(probe, at.z, m.shape.zTolerance);
            piece.z = Ground::PieceZ(sampler, at, piece, GroundSettings(), !full);
            piece.hasZ = true;
            ms.groundQueries += sampler.Queries();
        }

        // A fill piece's height from the outline's (AssignFillZ); unchanged without one.
        void FillZFrom(VisualPick const& outline, VisualPiece& piece)
        {
            if (outline.pieces.empty() || !outline.pieces.front().hasZ)
                return;
            VisualPick one;
            one.pieces.push_back(piece);
            Ground::AssignFillZ(one, outline);
            piece.hasZ = one.pieces.front().hasZ;
            piece.z = one.pieces.front().z;
        }

        // The height of the outline marker `guid` (a fill slot's outline), copied onto `piece`.
        void FillZFromMarker(TelegraphState const& state, ObjectGuid const& guid, VisualPiece& piece)
        {
            auto it = state.markers.find(guid);
            if (it == state.markers.end() || !it->second.piece.hasZ)
                return;
            piece.hasZ = true;
            piece.z = it->second.piece.z;
        }

        // Spread: every carrier's ring gets a fill. RunOut From(Owner): only the owner's ring (the hitbox); the small
        // rings on the targets are pointers, not hitboxes.
        bool IsFillHost(ActiveMechanic const& m, ObjectGuid const& host)
        {
            return !IsRunOutFromOwner(m) || host == m.owner;
        }

        void WarnPick(ActiveMechanic const& m, VisualPick const& pick)
        {
            if (pick.filled)
                LOG_WARN(kLog, "Mechanic {} ({} {}, tag {}): shape is past the end of its marker ladder; drawn as a "
                    "full disc (the hitbox stays exact)", m.id, KindName(m.kind), ShapeName(m.shape.type), m.tag);
            else if (pick.clamped)
                LOG_WARN(kLog, "Mechanic {} ({} {}, tag {}): shape is past the end of its marker ladder; drawn at the "
                    "last step (the hitbox stays exact)", m.id, KindName(m.kind), ShapeName(m.shape.type), m.tag);
        }

        // A marker for `piece` with its follow state; not listed in m.markers (SpawnPiece does that for outlines).
        Creature* SpawnMarkerAt(Map* map, TelegraphState& state, ActiveMechanic const& m, Unit const* owner,
            Geo::Pose const& anchor, VisualPiece const& piece, bool facingMatters, ObjectGuid follow)
        {
            Geo::Pose const at = PiecePose(anchor, piece);
            Creature* marker = SummonMarker(map, PositionOf(at), owner, piece.display, piece.scale);
            if (!marker)
            {
                LOG_ERROR(kLog, "Mechanic {}: could not summon a marker (creature {}) at {:.1f} {:.1f} {:.1f}", m.id,
                    Ids::NPC_MECHANIC_MARKER, at.x, at.y, at.z);
                return nullptr;
            }
            MarkerState& s = state.markers[marker->GetGUID()];
            s.mechanicId = m.id;
            s.follow = follow;
            s.piece = piece;
            s.facingMatters = facingMatters;
            s.sent = at;
            LogMarker("spawn", marker, s);
            return marker;
        }

        // An outline piece: also listed in m.markers (the resolve casts from the first one).
        Creature* SpawnPiece(Map* map, TelegraphState& state, ActiveMechanic& m, Unit const* owner,
            Geo::Pose const& anchor, VisualPiece const& piece, bool facingMatters,
            ObjectGuid follow = ObjectGuid::Empty)
        {
            Creature* marker = SpawnMarkerAt(map, state, m, owner, anchor, piece, facingMatters, follow);
            if (marker)
                m.markers.push_back(marker->GetGUID());
            return marker;
        }

        // Display and size of a live marker. SetDisplayId(display, scale) sets both in one go (it resets the scale
        // otherwise); a scale-only change is one field. Either goes out in the next update packet; the client blends
        // a scale change in over a short time (no animation in the model does it) and re-instances the model on a
        // display swap, which flickers: a fill marker only ever changes scale while its timer runs (PickFill). The
        // only swaps left are an outline whose shape changed (a shrinking projectile line) and a mine arming.
        void ApplyPiece(Creature* marker, MarkerState& s, VisualPiece const& piece)
        {
            bool const swap = piece.display != s.piece.display;
            bool const resize = !swap && piece.scale != s.piece.scale;
            if (swap)
            {
                marker->SetDisplayId(piece.display, piece.scale);
                marker->SetNativeDisplayId(piece.display);
            }
            else if (resize)
                marker->SetObjectScale(piece.scale);
            s.piece = piece;       // a new forward / facing offset: the caller moves the marker (MoveMarker)
            if (swap || resize)
                LogMarker(swap ? "display" : "scale", marker, s);
        }

        void DespawnMarker(Map* map, TelegraphState& state, ObjectGuid guid)
        {
            if (Creature* c = map->GetCreature(guid))
                if (c->IsInWorld())
                    c->DespawnOrUnsummon();
            state.markers.erase(guid);
        }

        // Moves a marker toward `target` (a spline over `intervalMs`, the time until the next move; 0 = the follow
        // interval).
        void MoveMarker(Creature* marker, MarkerState& s, Geo::Pose const& target, Unit* followUnit,
            uint32 intervalMs = 0)
        {
            ModuleConfig const& cfg = GetConfig();
            float const dist = std::hypot(Geo::Dist2d(s.sent.x, s.sent.y, target.x, target.y), target.z - s.sent.z);
            float const turn = s.facingMatters ? std::fabs(Geo::WrapPi(target.o - s.sent.o)) : 0.f;
            if (dist <= kMoveThresholdYd && turn <= kTurnThresholdRad)
                return;

            FollowMode mode = cfg.followMode;
            // MoveFollow only carries a circle-like piece sitting on a unit; everything else uses the spline step.
            if (mode == FollowMode::MoveFollow && (s.facingMatters || s.piece.forward != 0.f || !followUnit))
                mode = FollowMode::SplineStep;

            switch (mode)
            {
                case FollowMode::Teleport:
                    marker->NearTeleportTo(target.x, target.y, target.z, Geo::Wrap2Pi(target.o));
                    break;
                case FollowMode::MoveFollow:
                    if (!s.followStarted)
                    {
                        marker->GetMotionMaster()->MoveFollow(followUnit, 0.f, 0.f);
                        s.followStarted = true;
                    }
                    break;
                case FollowMode::SplineStep:
                default:
                {
                    uint32 const ms = intervalMs ? intervalMs : cfg.followIntervalMs;
                    float const interval = static_cast<float>(ms) / 1000.f;
                    Movement::MoveSplineInit init(marker);
                    init.MoveTo(target.x, target.y, target.z, false, true);
                    init.SetFacing(Geo::Wrap2Pi(target.o));
                    init.SetVelocity(std::max(dist / interval, kMinSplineSpeed));
                    init.SetFly();
                    init.Launch();
                    break;
                }
            }
            s.sent = target;
        }

        // Moves the fill markers of slots (shape kinds, Stack/Soak) with the anchor, like their outlines, at the
        // heights of the outline they belong to (ms.outline).
        void MoveFillSlots(Map* map, TelegraphState& state, MechanicState const& ms, Geo::Pose const& pose,
            Unit* anchorUnit)
        {
            for (ObjectGuid const& g : ms.fill)
            {
                if (g.IsEmpty())
                    continue;
                Creature* marker = map->GetCreature(g);
                auto it = state.markers.find(g);
                if (!marker || it == state.markers.end())
                    continue;
                FillZFrom(ms.outline, it->second.piece);
                MoveMarker(marker, it->second, PiecePose(pose, it->second.piece), anchorUnit);
            }
        }

        // Shape kinds: spawn or re-sync the outline pieces for the current drawn shape, then move them (and the fill).
        void SyncShapePieces(Map* map, TelegraphState& state, Registry const& reg, ActiveMechanic& m, Milliseconds now,
            bool spawning)
        {
            Geo::Pose pose;
            Shape const shape = DrawnShape(reg, m, now, pose);
            VisualPick const pick = PlacedOutline(map, m, state.mechanics[m.id], shape, pose, spawning);
            bool const facingMatters = shape.type == ShapeType::Cone || shape.type == ShapeType::Line;
            Unit* owner = FindUnit(map, m.owner);
            if (spawning)
                WarnPick(m, pick);

            if (spawning || pick.pieces.size() != m.markers.size())
            {
                // First spawn, or a shrinking line changed its piece count: replace them all.
                for (ObjectGuid const& g : m.markers)
                    DespawnMarker(map, state, g);
                m.markers.clear();
                for (VisualPiece const& piece : pick.pieces)
                    SpawnPiece(map, state, m, owner, pose, piece, facingMatters);
                return;
            }

            Unit* anchorUnit = m.anchor.IsUnit() ? FindUnit(map, m.anchor.unit) : nullptr;
            for (std::size_t i = 0; i < m.markers.size(); ++i)
            {
                Creature* marker = map->GetCreature(m.markers[i]);
                auto it = state.markers.find(m.markers[i]);
                if (!marker || it == state.markers.end())
                    continue;
                MarkerState& s = it->second;
                VisualPiece const& piece = pick.pieces[i];
                bool const swapped = piece.display != s.piece.display;
                ApplyPiece(marker, s, piece);
                Geo::Pose const target = PiecePose(pose, piece);
                if (swapped && ShrinksProjectile(m))
                {
                    // A shrink step: the new (shorter) model goes out with its new start in the same tick, instead of
                    // drawing from the old start while a spline carries it there (its far end overshooting meanwhile).
                    marker->NearTeleportTo(target.x, target.y, target.z, Geo::Wrap2Pi(target.o));
                    s.sent = target;
                }
                else
                    MoveMarker(marker, s, target, anchorUnit);
            }
            MoveFillSlots(map, state, state.mechanics[m.id], pose, anchorUnit);
        }

        struct CarrierHost
        {
            ObjectGuid unit;
            VisualPiece piece;
        };

        // The units carrier markers sit on, each with its piece: a ring of the radius on each carrier; for a RunOut
        // measured from the owner, the ring of the radius on the owner (the hitbox: the carrier fails while inside
        // it) plus a small ring on each carrier (who must run).
        std::vector<CarrierHost> CarrierHosts(ActiveMechanic const& m, std::vector<ObjectGuid> const& carriers)
        {
            std::vector<CarrierHost> hosts;
            uint8 const color = ColorIndex(m);
            VisualPick const ring = PickVisual(Shape::Circle(m.shape.radius), color, MarkerLayer::Outline);
            if (ring.pieces.empty())
                return hosts;
            if (!IsRunOutFromOwner(m))
            {
                for (ObjectGuid const& c : carriers)
                    hosts.push_back({ c, ring.pieces.front() });
                return hosts;
            }
            hosts.push_back({ m.owner, ring.pieces.front() });
            VisualPick const small = PickVisual(Shape::Circle(Rules::kRunOutTargetMarkerRadiusYd), color,
                MarkerLayer::Outline);
            for (ObjectGuid const& c : carriers)
                if (c != m.owner && !small.pieces.empty())
                    hosts.push_back({ c, small.pieces.front() });
            return hosts;
        }

        void SyncCarriers(Map* map, TelegraphState& state, Registry const& reg, ActiveMechanic& m)
        {
            std::vector<ObjectGuid> const carriers = RingHosts(reg, m);
            std::vector<CarrierHost> const hosts = CarrierHosts(m, carriers);
            Unit* owner = FindUnit(map, m.owner);

            // host -> marker, from what is up now
            std::unordered_map<ObjectGuid, ObjectGuid> byHost;
            for (ObjectGuid const& g : m.markers)
            {
                auto it = state.markers.find(g);
                if (it != state.markers.end())
                    byHost[it->second.follow] = g;
            }

            MechanicState& ms = state.mechanics[m.id];
            std::vector<ObjectGuid> keep;
            for (CarrierHost const& h : hosts)
            {
                ObjectGuid const& host = h.unit;
                Unit* unit = FindUnit(map, host);
                if (!unit || !unit->IsAlive())
                    continue;
                // Carriers move: the cheap set every follow step, on the carrier's own floor.
                VisualPiece piece = h.piece;
                PlaceRound(map, m, ms, PoseOf(unit->GetPosition()), piece, false);
                auto found = byHost.find(host);
                if (found != byHost.end())
                {
                    Creature* marker = map->GetCreature(found->second);
                    auto st = state.markers.find(found->second);
                    if (marker && st != state.markers.end())
                    {
                        st->second.piece.hasZ = piece.hasZ;
                        st->second.piece.z = piece.z;
                        MoveMarker(marker, st->second, PiecePose(PoseOf(unit->GetPosition()), piece), unit);
                        keep.push_back(found->second);
                        byHost.erase(found);
                        continue;
                    }
                }
                std::size_t const before = m.markers.size();
                SpawnPiece(map, state, m, owner, PoseOf(unit->GetPosition()), piece, false, host);
                if (m.markers.size() > before)
                    keep.push_back(m.markers.back());
            }

            // Hosts that are gone (dead, left, lost the self-trigger stacks): despawn their markers.
            for (auto const& [host, g] : byHost)
                DespawnMarker(map, state, g);
            m.markers = keep;

            // Fill markers follow their host at its ring's height; a host without an outline any more loses its fill
            // too.
            std::unordered_map<ObjectGuid, ObjectGuid> hostsUp;     // host -> its outline marker
            for (ObjectGuid const& g : m.markers)
            {
                auto it = state.markers.find(g);
                if (it != state.markers.end())
                    hostsUp[it->second.follow] = g;
            }
            for (auto it = ms.carrierFill.begin(); it != ms.carrierFill.end();)
            {
                auto const up = hostsUp.find(it->first);
                Unit* unit = up != hostsUp.end() ? FindUnit(map, it->first) : nullptr;
                Creature* marker = map->GetCreature(it->second);
                auto st = state.markers.find(it->second);
                if (!unit || !marker || st == state.markers.end())
                {
                    DespawnMarker(map, state, it->second);
                    it = ms.carrierFill.erase(it);
                    continue;
                }
                FillZFromMarker(state, up->second, st->second.piece);
                MoveMarker(marker, st->second, PiecePose(PoseOf(unit->GetPosition()), st->second.piece), unit);
                ++it;
            }

            m.carrierMarkers.clear();
            if (IsRunOutFromOwner(m))
            {
                // The resolve casts from the owner's ring (the hitbox), never from a carrier's small ring.
                for (ObjectGuid const& g : m.markers)
                {
                    auto it = state.markers.find(g);
                    if (it == state.markers.end() || it->second.follow != m.owner)
                        continue;
                    for (ObjectGuid const& c : carriers)
                        m.carrierMarkers[c] = g;
                }
                return;
            }
            for (ObjectGuid const& g : m.markers)
            {
                auto it = state.markers.find(g);
                if (it != state.markers.end())
                    m.carrierMarkers[it->second.follow] = g;
            }
        }

        // Stack/Soak (one disc on the anchor) and Spot (a disc on each spot).
        void SpawnGetIn(Map* map, TelegraphState& state, Registry const& reg, ActiveMechanic& m)
        {
            Unit* owner = FindUnit(map, m.owner);
            VisualPick pick = PickVisual(Shape::Circle(m.shape.radius), ColorIndex(m), MarkerLayer::Outline);
            if (pick.pieces.empty())
                return;
            MechanicState& ms = state.mechanics[m.id];
            if (m.kind == Kind::Spot)
            {
                for (auto const& [player, spot] : m.spots)
                {
                    VisualPiece piece = pick.pieces.front();
                    PlaceRound(map, m, ms, PoseOf(spot), piece, true);
                    SpawnPiece(map, state, m, owner, PoseOf(spot), piece, false);
                }
                return;
            }
            Geo::Pose const pose = reg.AnchorPose(m);
            PlaceRound(map, m, ms, pose, pick.pieces.front(), true);
            ms.outline = pick;
            SpawnPiece(map, state, m, owner, pose, pick.pieces.front(), false);
        }

        void FollowGetIn(Map* map, TelegraphState& state, Registry const& reg, ActiveMechanic& m)
        {
            if (m.kind == Kind::Spot || m.markers.empty() || !m.anchor.IsUnit())
                return;
            Creature* marker = map->GetCreature(m.markers.front());
            auto it = state.markers.find(m.markers.front());
            if (!marker || it == state.markers.end())
                return;
            Geo::Pose const pose = reg.AnchorPose(m);
            Unit* anchorUnit = FindUnit(map, m.anchor.unit);
            MechanicState& ms = state.mechanics[m.id];
            PlaceRound(map, m, ms, pose, it->second.piece, false);
            ms.outline.pieces.assign(1, it->second.piece);
            MoveMarker(marker, it->second, PiecePose(pose, it->second.piece), anchorUnit);
            MoveFillSlots(map, state, ms, pose, anchorUnit);
        }

        // One fill slot: spawn, resize, or despawn its marker so it draws `piece` (display 0: nothing). A line fill
        // piece past the first also moves along the line as the fill grows (PickFill scales its distance from the
        // source); the spline takes one fill step (`stepMs`), so it arrives with the next one.
        void SyncFillSlot(Map* map, TelegraphState& state, ActiveMechanic const& m, Unit const* owner, ObjectGuid& slot,
            Geo::Pose const& anchor, VisualPiece const& piece, bool facingMatters, ObjectGuid follow, uint32 stepMs)
        {
            Creature* marker = slot.IsEmpty() ? nullptr : map->GetCreature(slot);
            auto it = slot.IsEmpty() ? state.markers.end() : state.markers.find(slot);
            if (!piece.display || !marker || it == state.markers.end())
            {
                if (!slot.IsEmpty())
                    DespawnMarker(map, state, slot);
                slot.Clear();
                if (!piece.display)
                    return;
                if (Creature* spawned = SpawnMarkerAt(map, state, m, owner, anchor, piece, facingMatters, follow))
                {
                    slot = spawned->GetGUID();
                    state.markers[slot].fillLayer = true;
                }
                return;
            }
            bool const moved = piece.forward != it->second.piece.forward || piece.hasZ != it->second.piece.hasZ
                || piece.z != it->second.piece.z;
            ApplyPiece(marker, it->second, piece);
            if (moved)
                MoveMarker(marker, it->second, PiecePose(anchor, piece), nullptr, stepMs);
        }

        // An armed mine's outline swaps to the dashed ring (one display swap when it arms; nothing after), at the
        // height its ring had.
        void SyncArmedMineOutline(Map* map, TelegraphState& state, ActiveMechanic const& m, Shape const& shape)
        {
            VisualPick const pick = OutlinePick(m, shape);
            for (std::size_t i = 0; i < m.markers.size() && i < pick.pieces.size(); ++i)
            {
                Creature* marker = map->GetCreature(m.markers[i]);
                auto it = state.markers.find(m.markers[i]);
                if (!marker || it == state.markers.end())
                    continue;
                VisualPiece piece = pick.pieces[i];
                piece.hasZ = it->second.piece.hasZ;
                piece.z = it->second.piece.z;
                ApplyPiece(marker, it->second, piece);
            }
        }

        void ResizeFillSlots(Map* map, TelegraphState& state, MechanicState& ms, std::size_t n)
        {
            for (std::size_t i = n; i < ms.fill.size(); ++i)
                if (!ms.fill[i].IsEmpty())
                    DespawnMarker(map, state, ms.fill[i]);
            ms.fill.resize(n);
        }

        // This mechanic's fill step (round 4b): FillStepInterval over the fill it draws (the drawn shape of a shape
        // kind, the radius's disc for the rest), so a big fill steps often enough to move smoothly and a small one no
        // more often than FillStepMs.
        uint32 FillStepFor(Registry const& reg, ActiveMechanic const& m, Milliseconds now)
        {
            ModuleConfig const& cfg = GetConfig();
            Shape shape = Shape::Circle(m.shape.radius);
            if (DrawPathOf(m) == DrawPath::Shape)
            {
                Geo::Pose pose;
                shape = DrawnShape(reg, m, now, pose);
            }
            return FillStepInterval(m.resolveAt - m.createdAt, FillTravel(shape), cfg.fillStepYd, cfg.fillStepMs);
        }

        // One fill step: the timer fraction from createdAt to resolveAt (BossMechanics.Telegraph.FillLeadMs early),
        // or the full fill once the danger is live (an armed mine: no fill, its outline turns into the dashed ring).
        // Spawns a fill piece the first step it is big enough, so no client ever sees it at another size than the one
        // the step picked.
        void StepFill(Map* map, TelegraphState& state, Registry const& reg, ActiveMechanic& m, MechanicState& ms,
            Milliseconds now)
        {
            ModuleConfig const& cfg = GetConfig();
            bool const live = DangerLive(m, now);
            float const f = FillFraction(m.createdAt, m.resolveAt, now, Milliseconds(cfg.fillLeadMs));
            uint8 const color = ColorIndex(m);
            Unit* owner = FindUnit(map, m.owner);
            bool const timer = cfg.fill && m.fillTimer;
            auto pickFor = [&](Shape const& shape, LineSteps const* lineSteps)
            {
                VisualPick pick = PickFill(shape, color, f, live, lineSteps);
                if ((!timer && !live) || ArmedMine(m))
                    for (VisualPiece& piece : pick.pieces)
                        piece.display = 0;         // timer off: the fill only shows the live danger; armed mine: none
                return pick;
            };

            switch (DrawPathOf(m))
            {
                case DrawPath::Shape:
                {
                    Geo::Pose pose;
                    Shape const shape = DrawnShape(reg, m, now, pose);
                    VisualPick pick = pickFor(shape, LineStepsOf(ms));
                    Ground::AssignFillZ(pick, ms.outline);
                    bool const facingMatters = shape.type == ShapeType::Cone || shape.type == ShapeType::Line;
                    if (ArmedMine(m))
                        SyncArmedMineOutline(map, state, m, shape);
                    ResizeFillSlots(map, state, ms, pick.pieces.size());
                    for (std::size_t k = 0; k < pick.pieces.size(); ++k)
                        SyncFillSlot(map, state, m, owner, ms.fill[k], pose, pick.pieces[k], facingMatters,
                            ObjectGuid::Empty, ms.fillStepMs);
                    break;
                }
                case DrawPath::GetIn:
                {
                    VisualPick const pick = pickFor(Shape::Circle(m.shape.radius), nullptr);
                    if (pick.pieces.empty())
                        break;
                    std::vector<Geo::Pose> poses;
                    if (m.kind == Kind::Spot)
                        for (auto const& [player, spot] : m.spots)  // the order SpawnGetIn spawned the outlines in
                            poses.push_back(PoseOf(spot));
                    else
                        poses.push_back(reg.AnchorPose(m));
                    ResizeFillSlots(map, state, ms, poses.size());
                    for (std::size_t k = 0; k < poses.size(); ++k)
                    {
                        VisualPiece piece = pick.pieces.front();
                        if (k < m.markers.size())
                            FillZFromMarker(state, m.markers[k], piece);    // slot k: outline k's height
                        SyncFillSlot(map, state, m, owner, ms.fill[k], poses[k], piece, false, ObjectGuid::Empty,
                            ms.fillStepMs);
                    }
                    break;
                }
                case DrawPath::Carriers:
                {
                    VisualPick const pick = pickFor(Shape::Circle(m.shape.radius), nullptr);
                    if (pick.pieces.empty())
                        break;
                    GuidSet hosts;
                    for (ObjectGuid const& g : m.markers)
                    {
                        auto it = state.markers.find(g);
                        if (it == state.markers.end() || !IsFillHost(m, it->second.follow))
                            continue;
                        Unit* unit = FindUnit(map, it->second.follow);
                        if (!unit || !unit->IsAlive())
                            continue;
                        hosts.insert(it->second.follow);
                        VisualPiece piece = pick.pieces.front();
                        FillZFromMarker(state, g, piece);                    // the host ring's height
                        SyncFillSlot(map, state, m, owner, ms.carrierFill[it->second.follow],
                            PoseOf(unit->GetPosition()), piece, false, it->second.follow, ms.fillStepMs);
                    }
                    for (auto it = ms.carrierFill.begin(); it != ms.carrierFill.end();)
                    {
                        if (hosts.count(it->first) && !it->second.IsEmpty())
                        {
                            ++it;
                            continue;
                        }
                        if (!it->second.IsEmpty())
                            DespawnMarker(map, state, it->second);
                        it = ms.carrierFill.erase(it);
                    }
                    break;
                }
                case DrawPath::None:
                    break;
            }

            ms.liveShown = live;
            // A static mechanic is done once its live state is drawn (or there is nothing to time: timer off); a
            // moving or reshaping one keeps stepping (cheap: an unchanged piece sends nothing).
            ms.fillFinal = !IsDynamic(m) && (live || !timer);
        }
    }

    Creature* SummonMarker(Map* map, Position const& pos, Unit const* owner, uint32 display, float scale)
    {
        if (!map)
            return nullptr;
        // Map::SummonCreature (Object.cpp) cannot be used: it adds the creature to the map, and AddToMap ->
        // UpdateObjectVisibilityOnCreate builds the create packet for every player in range right there, with the
        // template's display and scale 1. The display and scale set after it reach clients as a value update, and
        // the client blends a scale change in, so every marker visibly grew from 1 yd to its size. This is that
        // function's plain-summon path (no properties, no summoner: UNIT_MASK_SUMMON, a TempSummon), with the
        // display, scale, faction, level and gravity set between Create and AddToMap, so the first packet any client
        // gets already carries the final look. EnsureGridLoaded is private; LoadGrid is its public wrapper. The
        // AIRelocationNotifier pass it ends with is left out: a passive marker gives no creature a reason to look.
        TempSummon* marker = new TempSummon(nullptr, ObjectGuid::Empty);
        map->LoadGrid(pos.GetPositionX(), pos.GetPositionY());
        uint32 const phase = owner ? owner->GetPhaseMask() : uint32(PHASEMASK_NORMAL);
        if (!marker->Create(map->GenerateLowGuid<HighGuid::Unit>(), map, phase, Ids::NPC_MECHANIC_MARKER, 0,
            pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), pos.GetOrientation()))
        {
            delete marker;
            return nullptr;
        }
        marker->SetHomePosition(pos);
        marker->InitStats(0);
        if (owner)
        {
            marker->SetFaction(owner->GetFaction());
            marker->SetLevel(owner->GetLevel(), false);
        }
        marker->SetDisableGravity(true);   // out of world: only the movement flag; the create packet carries it
        if (display)
        {
            marker->SetDisplayId(display, scale);
            marker->SetNativeDisplayId(display);
        }
        if (!map->AddToMap(marker->ToCreature()))
        {
            delete marker;
            return nullptr;
        }
        marker->InitSummon();
        marker->SetReactState(REACT_PASSIVE);
        return marker;
    }

    std::vector<std::string> DescribeMarkers(Map* map, Position const& from, float range)
    {
        std::vector<std::string> lines;
        if (!map)
            return lines;
        TelegraphState* state = map->CustomData.Get<TelegraphState>(kStateKey);
        if (!state)
            return lines;
        struct Row
        {
            uint32 mechanicId;
            bool fill;
            float dist;
            std::string text;
        };
        std::vector<Row> rows;
        for (auto const& [guid, s] : state->markers)
        {
            Creature const* c = map->GetCreature(guid);
            if (!c || !c->IsInWorld())
                continue;
            float const d = from.GetExactDist2d(c);
            if (d > range)
                continue;
            rows.push_back({ s.mechanicId, s.fillLayer, d, DescribeMarker(c, s, &from) });
        }
        std::sort(rows.begin(), rows.end(), [](Row const& a, Row const& b)
        {
            return std::tie(a.mechanicId, a.fill, a.dist) < std::tie(b.mechanicId, b.fill, b.dist);
        });
        for (Row& r : rows)
            lines.push_back(std::move(r.text));
        return lines;
    }

    void Spawn(Map* map, ActiveMechanic& m)
    {
        Registry const* reg = Registry::Get(map);
        if (!map || !reg)
            return;
        TelegraphState& state = StateOf(map);
        Milliseconds const now = GameTime::GetGameTimeMS();
        auto const started = std::chrono::steady_clock::now();
        MechanicState& ms = state.mechanics[m.id];
        ms.nextFollowAt = now + Milliseconds(GetConfig().followIntervalMs);
        ms.fillStepMs = FillStepFor(*reg, m, now);
        ms.nextFillAt = now + Milliseconds(ms.fillStepMs);

        switch (DrawPathOf(m))
        {
            case DrawPath::Shape:
                SyncShapePieces(map, state, *reg, m, now, true);
                break;
            case DrawPath::Carriers:
                SyncCarriers(map, state, *reg, m);
                break;
            case DrawPath::GetIn:
                SpawnGetIn(map, state, *reg, m);
                break;
            case DrawPath::None:
                break;
        }
        if (FillApplies(m))
            StepFill(map, state, *reg, m, ms, now);

        ModuleConfig const& cfg = GetConfig();
        if (cfg.debug && cfg.groundSnap)
        {
            auto const us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()
                - started).count();
            LOG_INFO(kLog, "Mechanic {} ({} {}, tag {}): ground snap {} height queries, {} outline pieces, spawn {} us",
                m.id, KindName(m.kind), ShapeName(m.shape.type), m.tag, ms.groundQueries, m.markers.size(), us);
        }
    }

    void Update(Map* map, ActiveMechanic& m, Milliseconds now)
    {
        Registry const* reg = Registry::Get(map);
        if (!map || !reg || !m.telegraph)
            return;
        TelegraphState& state = StateOf(map);
        ModuleConfig const& cfg = GetConfig();
        if (IsDynamic(m) && now >= state.mechanics[m.id].nextFollowAt)
        {
            state.mechanics[m.id].nextFollowAt = now + Milliseconds(cfg.followIntervalMs);
            switch (DrawPathOf(m))
            {
                case DrawPath::Shape:
                    SyncShapePieces(map, state, *reg, m, now, false);
                    break;
                case DrawPath::Carriers:
                    SyncCarriers(map, state, *reg, m);
                    break;
                case DrawPath::GetIn:
                    FollowGetIn(map, state, *reg, m);     // Stack/Soak on a unit; Spot never moves
                    break;
                case DrawPath::None:
                    break;
            }
        }

        if (!FillApplies(m))
            return;
        // Every fill step (FillStepFor) while the timer runs, and at once when the danger turns live (a mine arms, a
        // persistent zone resolves).
        MechanicState& ms = state.mechanics[m.id];
        if (DangerLive(m, now) != ms.liveShown || (!ms.fillFinal && now >= ms.nextFillAt))
        {
            ms.nextFillAt = now + Milliseconds(ms.fillStepMs ? ms.fillStepMs : cfg.fillStepMs);
            StepFill(map, state, *reg, m, ms, now);
        }
    }

    void Despawn(Map* map, ActiveMechanic& m)
    {
        if (!map)
            return;
        TelegraphState& state = StateOf(map);
        GuidSet all(m.markers.begin(), m.markers.end());
        for (auto const& [carrier, g] : m.carrierMarkers)
            all.insert(g);
        auto ms = state.mechanics.find(m.id);
        if (ms != state.mechanics.end())
        {
            for (ObjectGuid const& g : ms->second.fill)
                if (!g.IsEmpty())
                    all.insert(g);
            for (auto const& [host, g] : ms->second.carrierFill)
                if (!g.IsEmpty())
                    all.insert(g);
        }
        for (ObjectGuid const& g : all)
            DespawnMarker(map, state, g);
        m.markers.clear();
        m.carrierMarkers.clear();
        state.mechanics.erase(m.id);
    }
}
