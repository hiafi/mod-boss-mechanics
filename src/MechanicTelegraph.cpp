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
#include "MechanicMath.h"
#include "MechanicRegistry.h"
#include "MotionMaster.h"
#include "MoveSplineInit.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "TemporarySummon.h"
#include <algorithm>
#include <cmath>
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
        };

        struct MechanicState
        {
            Milliseconds nextFollowAt{ 0 };
            Milliseconds nextFillAt{ 0 };
            bool liveShown = false;     // the fill shows the live-danger state (resolved, armed mine)
            bool fillFinal = false;     // a static mechanic whose fill reached its last state: no more fill steps
            // The fill layer (MarkerState entries in TelegraphState::markers too). Shape kinds, Stack/Soak and Spot:
            // slot k belongs to outline piece / spot k (empty guid: nothing drawn there yet). Spread/RunOut: one per
            // host, keyed by the host.
            std::vector<ObjectGuid> fill;
            std::unordered_map<ObjectGuid, ObjectGuid> carrierFill;
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

        // Where a piece sits: `forward` yards along anchor facing + offset, facing that way (PieceWorldPose), lifted.
        Geo::Pose PiecePose(Geo::Pose const& anchor, VisualPiece const& piece)
        {
            Geo::Pose p = PieceWorldPose(anchor, piece);
            p.z += GetConfig().lift;
            return p;
        }

        uint8 ColorIndex(ActiveMechanic const& m)
        {
            return static_cast<uint8>(m.color);
        }

        bool IsCarrierKind(ActiveMechanic const& m)
        {
            return m.kind == Kind::Spread || m.kind == Kind::RunOut;
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
        // projectile line drops the part the projectile's tail has already passed (cosmetic, config), and a line with
        // a draw strip (ExceptOne's last lane) draws only that strip.
        Shape DrawnShape(Registry const& reg, ActiveMechanic const& m, Milliseconds now, Geo::Pose& pose)
        {
            Shape shape = reg.EffectiveShape(m, pose);
            ApplyDrawStrip(shape, pose, m.drawLateral, m.drawWidth);
            if (ShrinksProjectile(m) && now > m.resolveAt)
            {
                float const half = m.projectile->halfLength > 0.f ? m.projectile->halfLength : shape.width * 0.5f;
                float const tail = m.projectile->speed * SecondsOf(now - m.resolveAt) - half;
                if (tail > shape.excludeStart)
                    shape.excludeStart = std::min(tail, shape.length);
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

        // The outline pieces of a shape kind: the hitbox's outline, or an armed mine's dashed ring.
        VisualPick OutlinePick(ActiveMechanic const& m, Shape const& shape)
        {
            return ArmedMine(m) ? PickArmedMine(shape, ColorIndex(m)) : PickVisual(shape, ColorIndex(m),
                MarkerLayer::Outline);
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
            if (piece.display != s.piece.display)
            {
                marker->SetDisplayId(piece.display, piece.scale);
                marker->SetNativeDisplayId(piece.display);
            }
            else if (piece.scale != s.piece.scale)
                marker->SetObjectScale(piece.scale);
            s.piece = piece;       // a new forward / facing offset: the caller moves the marker (MoveMarker)
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

        // Moves the fill markers of slots (shape kinds, Stack/Soak) with the anchor, like their outlines.
        void MoveFillSlots(Map* map, TelegraphState& state, MechanicState const& ms, Geo::Pose const& pose,
            Unit* anchorUnit)
        {
            for (ObjectGuid const& g : ms.fill)
            {
                if (g.IsEmpty())
                    continue;
                Creature* marker = map->GetCreature(g);
                auto it = state.markers.find(g);
                if (marker && it != state.markers.end())
                    MoveMarker(marker, it->second, PiecePose(pose, it->second.piece), anchorUnit);
            }
        }

        // Shape kinds: spawn or re-sync the outline pieces for the current drawn shape, then move them (and the fill).
        void SyncShapePieces(Map* map, TelegraphState& state, Registry const& reg, ActiveMechanic& m, Milliseconds now,
            bool spawning)
        {
            Geo::Pose pose;
            Shape const shape = DrawnShape(reg, m, now, pose);
            VisualPick const pick = OutlinePick(m, shape);
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
                ApplyPiece(marker, s, piece);
                MoveMarker(marker, s, PiecePose(pose, piece), anchorUnit);
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
            std::vector<ObjectGuid> const carriers = reg.Carriers(m);
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

            std::vector<ObjectGuid> keep;
            for (CarrierHost const& h : hosts)
            {
                ObjectGuid const& host = h.unit;
                VisualPiece const& piece = h.piece;
                Unit* unit = FindUnit(map, host);
                if (!unit || !unit->IsAlive())
                    continue;
                auto found = byHost.find(host);
                if (found != byHost.end())
                {
                    Creature* marker = map->GetCreature(found->second);
                    auto st = state.markers.find(found->second);
                    if (marker && st != state.markers.end())
                    {
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

            // Fill markers follow their host; a host without an outline any more loses its fill too.
            MechanicState& ms = state.mechanics[m.id];
            GuidSet hostsUp;
            for (ObjectGuid const& g : m.markers)
            {
                auto it = state.markers.find(g);
                if (it != state.markers.end())
                    hostsUp.insert(it->second.follow);
            }
            for (auto it = ms.carrierFill.begin(); it != ms.carrierFill.end();)
            {
                Unit* unit = hostsUp.count(it->first) ? FindUnit(map, it->first) : nullptr;
                Creature* marker = map->GetCreature(it->second);
                auto st = state.markers.find(it->second);
                if (!unit || !marker || st == state.markers.end())
                {
                    DespawnMarker(map, state, it->second);
                    it = ms.carrierFill.erase(it);
                    continue;
                }
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
            VisualPick const pick = PickVisual(Shape::Circle(m.shape.radius), ColorIndex(m), MarkerLayer::Outline);
            if (pick.pieces.empty())
                return;
            if (m.kind == Kind::Spot)
            {
                for (auto const& [player, spot] : m.spots)
                    SpawnPiece(map, state, m, owner, PoseOf(spot), pick.pieces.front(), false);
                return;
            }
            SpawnPiece(map, state, m, owner, reg.AnchorPose(m), pick.pieces.front(), false);
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
            MoveMarker(marker, it->second, PiecePose(pose, it->second.piece), anchorUnit);
            MoveFillSlots(map, state, state.mechanics[m.id], pose, anchorUnit);
        }

        // One fill slot: spawn, resize, or despawn its marker so it draws `piece` (display 0: nothing). A line fill
        // piece past the first also moves along the line as the fill grows (PickFill scales its distance from the
        // source); the spline takes one fill step, so it arrives with the next one.
        void SyncFillSlot(Map* map, TelegraphState& state, ActiveMechanic const& m, Unit const* owner, ObjectGuid& slot,
            Geo::Pose const& anchor, VisualPiece const& piece, bool facingMatters, ObjectGuid follow)
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
                    slot = spawned->GetGUID();
                return;
            }
            bool const moved = piece.forward != it->second.piece.forward;
            ApplyPiece(marker, it->second, piece);
            if (moved)
                MoveMarker(marker, it->second, PiecePose(anchor, piece), nullptr, GetConfig().fillStepMs);
        }

        // An armed mine's outline swaps to the dashed ring (one display swap when it arms; nothing after).
        void SyncArmedMineOutline(Map* map, TelegraphState& state, ActiveMechanic const& m, Shape const& shape)
        {
            VisualPick const pick = OutlinePick(m, shape);
            for (std::size_t i = 0; i < m.markers.size() && i < pick.pieces.size(); ++i)
            {
                Creature* marker = map->GetCreature(m.markers[i]);
                auto it = state.markers.find(m.markers[i]);
                if (marker && it != state.markers.end())
                    ApplyPiece(marker, it->second, pick.pieces[i]);
            }
        }

        void ResizeFillSlots(Map* map, TelegraphState& state, MechanicState& ms, std::size_t n)
        {
            for (std::size_t i = n; i < ms.fill.size(); ++i)
                if (!ms.fill[i].IsEmpty())
                    DespawnMarker(map, state, ms.fill[i]);
            ms.fill.resize(n);
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
            auto pickFor = [&](Shape const& shape)
            {
                VisualPick pick = PickFill(shape, color, f, live);
                if ((!timer && !live) || ArmedMine(m))
                    for (VisualPiece& piece : pick.pieces)
                        piece.display = 0;         // timer off: the fill only shows the live danger; armed mine: none
                return pick;
            };

            switch (m.kind)
            {
                case Kind::Zone:
                case Kind::Frontal:
                {
                    Geo::Pose pose;
                    Shape const shape = DrawnShape(reg, m, now, pose);
                    VisualPick const pick = pickFor(shape);
                    bool const facingMatters = shape.type == ShapeType::Cone || shape.type == ShapeType::Line;
                    if (ArmedMine(m))
                        SyncArmedMineOutline(map, state, m, shape);
                    ResizeFillSlots(map, state, ms, pick.pieces.size());
                    for (std::size_t k = 0; k < pick.pieces.size(); ++k)
                        SyncFillSlot(map, state, m, owner, ms.fill[k], pose, pick.pieces[k], facingMatters,
                            ObjectGuid::Empty);
                    break;
                }
                case Kind::Stack:
                case Kind::Soak:
                case Kind::Spot:
                {
                    VisualPick const pick = pickFor(Shape::Circle(m.shape.radius));
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
                        SyncFillSlot(map, state, m, owner, ms.fill[k], poses[k], pick.pieces.front(), false,
                            ObjectGuid::Empty);
                    break;
                }
                case Kind::Spread:
                case Kind::RunOut:
                {
                    VisualPick const pick = pickFor(Shape::Circle(m.shape.radius));
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
                        SyncFillSlot(map, state, m, owner, ms.carrierFill[it->second.follow],
                            PoseOf(unit->GetPosition()), pick.pieces.front(), false, it->second.follow);
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
                case Kind::Bait:
                case Kind::HoldStill:
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

    void Spawn(Map* map, ActiveMechanic& m)
    {
        Registry const* reg = Registry::Get(map);
        if (!map || !reg)
            return;
        TelegraphState& state = StateOf(map);
        Milliseconds const now = GameTime::GetGameTimeMS();
        MechanicState& ms = state.mechanics[m.id];
        ms.nextFollowAt = now + Milliseconds(GetConfig().followIntervalMs);
        ms.nextFillAt = now + Milliseconds(GetConfig().fillStepMs);

        switch (m.kind)
        {
            case Kind::Zone:
            case Kind::Frontal:
                SyncShapePieces(map, state, *reg, m, now, true);
                break;
            case Kind::Spread:
            case Kind::RunOut:
                SyncCarriers(map, state, *reg, m);
                break;
            case Kind::Stack:
            case Kind::Soak:
            case Kind::Spot:
                SpawnGetIn(map, state, *reg, m);
                break;
            case Kind::Bait:
            case Kind::HoldStill:
                break;
        }
        if (FillApplies(m))
            StepFill(map, state, *reg, m, ms, now);
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
            switch (m.kind)
            {
                case Kind::Zone:
                case Kind::Frontal:
                    SyncShapePieces(map, state, *reg, m, now, false);
                    break;
                case Kind::Spread:
                case Kind::RunOut:
                    SyncCarriers(map, state, *reg, m);
                    break;
                case Kind::Stack:
                case Kind::Soak:
                    FollowGetIn(map, state, *reg, m);
                    break;
                case Kind::Spot:
                case Kind::Bait:
                case Kind::HoldStill:
                    break;
            }
        }

        if (!FillApplies(m))
            return;
        // Every FillStepMs while the timer runs, and at once when the danger turns live (a mine arms, a persistent
        // zone resolves).
        MechanicState& ms = state.mechanics[m.id];
        if (DangerLive(m, now) != ms.liveShown || (!ms.fillFinal && now >= ms.nextFillAt))
        {
            ms.nextFillAt = now + Milliseconds(cfg.fillStepMs);
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
