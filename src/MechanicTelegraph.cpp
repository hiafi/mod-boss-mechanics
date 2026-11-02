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
            bool armedShown = false;    // mine: the disc (armed) display is up
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

        // Where a piece sits: `forward` yards along anchor facing + offset, facing that way, lifted.
        Geo::Pose PiecePose(Geo::Pose const& anchor, VisualPiece const& piece)
        {
            float const facing = anchor.o + piece.facingOffset;
            Geo::Pose p = Geo::Advance(anchor, facing, piece.forward);
            p.z += GetConfig().lift;
            return p;
        }

        uint8 ColourIndex(ActiveMechanic const& m)
        {
            return static_cast<uint8>(m.colour);
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
        // projectile line drops the part the projectile's tail has already passed (cosmetic, config).
        Shape DrawnShape(Registry const& reg, ActiveMechanic const& m, Milliseconds now, Geo::Pose& pose)
        {
            Shape shape = reg.EffectiveShape(m, pose);
            if (ShrinksProjectile(m) && now > m.resolveAt)
            {
                float const half = m.projectile->halfLength > 0.f ? m.projectile->halfLength : shape.width * 0.5f;
                float const tail = m.projectile->speed * SecondsOf(now - m.resolveAt) - half;
                if (tail > shape.excludeStart)
                    shape.excludeStart = std::min(tail, shape.length);
            }
            return shape;
        }

        MarkerStyle StyleOf(ActiveMechanic const& m)
        {
            return m.mine && !m.mine->armed ? MarkerStyle::Ring : MarkerStyle::Disc;
        }

        void WarnPick(ActiveMechanic const& m, VisualPick const& pick)
        {
            if (pick.filled)
                LOG_WARN(kLog, "Mechanic {} ({} {}, tag {}): shape is past the end of its marker ladder; drawn as a "
                    "full disc (the hitbox stays exact)", m.id, KindName(m.kind), ShapeName(m.shape.type), m.tag);
            else if (pick.clamped)
                LOG_WARN(kLog, "Mechanic {} ({} {}, tag {}): shape is past the end of its marker ladder; drawn at the "
                    "last step (the hitbox stays exact)", m.id, KindName(m.kind), ShapeName(m.shape.type), m.tag);
            if (pick.colourFallback)
                LOG_WARN(kLog, "Mechanic {} ({} {}, tag {}): {} markers exist in red and yellow only; drawn {}", m.id,
                    KindName(m.kind), ShapeName(m.shape.type), m.tag, ShapeName(m.shape.type),
                    ColourName(static_cast<Colour>(ColourIndex(m) % Ids::COLOUR_COUNT_GET_OUT)));
        }

        Creature* SpawnPiece(Map* map, TelegraphState& state, ActiveMechanic& m, Unit const* owner,
            Geo::Pose const& anchor, VisualPiece const& piece, bool facingMatters,
            ObjectGuid follow = ObjectGuid::Empty)
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
            m.markers.push_back(marker->GetGUID());
            return marker;
        }

        void DespawnMarker(Map* map, TelegraphState& state, ObjectGuid guid)
        {
            if (Creature* c = map->GetCreature(guid))
                if (c->IsInWorld())
                    c->DespawnOrUnsummon();
            state.markers.erase(guid);
        }

        void MoveMarker(Creature* marker, MarkerState& s, Geo::Pose const& target, Unit* followUnit)
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
                    float const interval = static_cast<float>(cfg.followIntervalMs) / 1000.f;
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

        // Shape kinds: spawn or re-sync the pieces for the current drawn shape, then move them.
        void SyncShapePieces(Map* map, TelegraphState& state, Registry const& reg, ActiveMechanic& m, Milliseconds now,
            bool spawning)
        {
            Geo::Pose pose;
            Shape const shape = DrawnShape(reg, m, now, pose);
            VisualPick const pick = PickVisual(shape, ColourIndex(m), StyleOf(m));
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
                if (piece.display != s.piece.display || piece.scale != s.piece.scale)
                {
                    marker->SetDisplayId(piece.display);
                    marker->SetNativeDisplayId(piece.display);
                    marker->SetObjectScale(piece.scale);
                }
                s.piece = piece;
                MoveMarker(marker, s, PiecePose(pose, piece), anchorUnit);
            }
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
            uint8 const colour = ColourIndex(m);
            VisualPick const ring = PickVisual(Shape::Circle(m.shape.radius), colour, MarkerStyle::Ring);
            if (ring.pieces.empty())
                return hosts;
            if (!IsRunOutFromOwner(m))
            {
                for (ObjectGuid const& c : carriers)
                    hosts.push_back({ c, ring.pieces.front() });
                return hosts;
            }
            hosts.push_back({ m.owner, ring.pieces.front() });
            VisualPick const small = PickVisual(Shape::Circle(Rules::kRunOutTargetMarkerRadiusYd), colour,
                MarkerStyle::Ring);
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
            VisualPick const pick = PickVisual(Shape::Circle(m.shape.radius), ColourIndex(m), MarkerStyle::Disc);
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
            MoveMarker(marker, it->second, PiecePose(reg.AnchorPose(m), it->second.piece),
                FindUnit(map, m.anchor.unit));
        }
    }

    Creature* SummonMarker(Map* map, Position const& pos, Unit const* owner, uint32 display, float scale)
    {
        if (!map)
            return nullptr;
        TempSummon* marker = map->SummonCreature(Ids::NPC_MECHANIC_MARKER, pos);
        if (!marker)
            return nullptr;
        marker->SetReactState(REACT_PASSIVE);
        if (owner)
        {
            marker->SetFaction(owner->GetFaction());
            marker->SetLevel(owner->GetLevel(), false);
            if (marker->GetPhaseMask() != owner->GetPhaseMask())
                marker->SetPhaseMask(owner->GetPhaseMask(), true);
        }
        marker->SetDisableGravity(true);
        if (display)
        {
            // SetDisplayId resets the object scale (Unit.cpp), so the scale goes second.
            marker->SetDisplayId(display);
            marker->SetNativeDisplayId(display);
            marker->SetObjectScale(scale);
        }
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
        ms.armedShown = m.mine && m.mine->armed;

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
    }

    void Update(Map* map, ActiveMechanic& m, Milliseconds now)
    {
        Registry const* reg = Registry::Get(map);
        if (!map || !reg || !m.telegraph)
            return;
        TelegraphState& state = StateOf(map);
        MechanicState& ms = state.mechanics[m.id];
        bool const mineSwap = m.mine && m.mine->armed != ms.armedShown;
        if (!mineSwap && (!IsDynamic(m) || now < ms.nextFollowAt))
            return;
        ms.nextFollowAt = now + Milliseconds(GetConfig().followIntervalMs);
        ms.armedShown = m.mine && m.mine->armed;

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

    void Despawn(Map* map, ActiveMechanic& m)
    {
        if (!map)
            return;
        TelegraphState& state = StateOf(map);
        GuidSet all(m.markers.begin(), m.markers.end());
        for (auto const& [carrier, g] : m.carrierMarkers)
            all.insert(g);
        for (ObjectGuid const& g : all)
            DespawnMarker(map, state, g);
        m.markers.clear();
        m.carrierMarkers.clear();
        state.mechanics.erase(m.id);
    }
}
