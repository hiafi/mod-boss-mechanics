/*
 * mod-boss-mechanics: the Mechanic:: script API, MechanicRef options and LaneBuilder (STAGE-M1-HANDOFF §3.5).
 */

#include "BossMechanics.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "Random.h"
#include "Unit.h"
#include <algorithm>
#include <cmath>

using namespace BossMechanics;

namespace
{
    constexpr char const* kLog = "module.bossmechanics";

    bool IsDamageKind(Kind k)
    {
        return k == Kind::Zone || k == Kind::Frontal || k == Kind::Spread || k == Kind::RunOut || k == Kind::Bait
            || k == Kind::Jump || k == Kind::Hide || k == Kind::Gaze;
    }

    bool IsShapeKind(Kind k)
    {
        return Judge::IsShapeKind(k);
    }

    bool IsStackOrSoak(Kind k)
    {
        return k == Kind::Stack || k == Kind::Soak;
    }

    // Scarlet Monastery F15: Spread and RunOut take Persistent too (each carrier's circle judged at every tick).
    bool IsCarrierKind(Kind k)
    {
        return k == Kind::Spread || k == Kind::RunOut;
    }

    // A Line Stack/Soak (Scarlet Monastery F2: EndBacksOff, TankNearSource).
    bool IsLineGetIn(ActiveMechanic const* m)
    {
        return m && IsStackOrSoak(m->kind) && m->shape.type == ShapeType::Line;
    }

    // A shape's sizes are usable (donut: inner < outer).
    bool ShapeSizeOk(Shape const& s)
    {
        switch (s.type)
        {
            case ShapeType::Circle: return s.radius > 0.f;
            case ShapeType::Donut: return s.radius > 0.f && s.inner >= 0.f && s.inner < s.radius;
            case ShapeType::Cone: return s.length > 0.f && s.angleDeg > 0.f;
            case ShapeType::Line: return s.length > 0.f && s.width > 0.f;
        }
        return false;
    }

    // Weakened lands only from resolve hits (Rules::kWeakenedOnNonHitFailures = false), so only the kinds that cast
    // the resolve spell can apply it; Stack, HoldStill and Spot only post ledger failures. Soak never (group failure).
    bool CanWeaken(Kind k)
    {
        if (k == Kind::Soak)
            return false;
        return IsDamageKind(k) || Rules::kWeakenedOnNonHitFailures;
    }

    Map* MapOf(Unit const* owner, char const* what)
    {
        if (!owner || !owner->IsInWorld())
        {
            LOG_WARN(kLog, "Mechanic::{}: owner is null or not in world; ignored", what);
            return nullptr;
        }
        return owner->GetMap();
    }

    bool SizeOk(bool ok, char const* what, Unit const* owner)
    {
        if (!ok)
            LOG_WARN(kLog, "Mechanic::{} from {}: size must be > 0 (donut: inner < outer); ignored", what,
                owner ? owner->GetGUID().ToString() : "?");
        return ok;
    }

    // Registers with resolveAt = now + warn. The returned ref commits at the end of the caller's statement.
    MechanicRef Register(Map* map, ObjectGuid owner, Kind kind, Anchor const& anchor, Shape const& shape,
        Milliseconds warn, std::function<void(ActiveMechanic&)> const& init = nullptr, bool commitOnDestroy = true)
    {
        if (!map)
            return MechanicRef();
        Milliseconds const now = GameTime::GetGameTimeMS();
        ActiveMechanic m;
        m.owner = owner;
        m.kind = kind;
        m.anchor = anchor;
        m.shape = shape;
        m.createdAt = now;
        m.resolveAt = now + std::max(warn, Milliseconds(0));
        if (init)
            init(m);
        Registry& reg = Registry::GetOrCreate(map);
        ActiveMechanic* added = reg.Add(std::move(m));
        if (!added)
            return MechanicRef();
        reg.FinalizeTiming(*added);
        return MechanicRef(map, added->id, commitOnDestroy);
    }

    MechanicRef Register(Unit* owner, char const* what, Kind kind, Anchor const& anchor, Shape const& shape,
        Milliseconds warn, std::function<void(ActiveMechanic&)> const& init = nullptr)
    {
        Map* map = MapOf(owner, what);
        if (!map)
            return MechanicRef();
        return Register(map, owner->GetGUID(), kind, anchor, shape, warn, init);
    }

    Registry* RegistryOf(Unit const* owner)
    {
        return owner && owner->IsInWorld() ? Registry::Get(owner->GetMap()) : nullptr;
    }
}

// --------------------------------------------------------------------------------------------------------------------
// Shapes
// --------------------------------------------------------------------------------------------------------------------
namespace BossMechanics::Shapes
{
    ZoneSpec Circle(Anchor const& at, float radius)
    {
        return { at, Shape::Circle(radius), std::nullopt };
    }

    ZoneSpec Donut(Anchor const& at, float inner, float outer)
    {
        return { at, Shape::Donut(inner, outer), std::nullopt };
    }

    ZoneSpec Cone(Anchor const& at, float angleDeg, float length)
    {
        ZoneSpec spec{ at, Shape::Cone(angleDeg, length), std::nullopt };
        if (spec.anchor.IsUnit())
            spec.anchor.followFacing = true;
        return spec;
    }

    ZoneSpec Line(Anchor const& from, Anchor const& to, float width)
    {
        float const length = from.point.GetExactDist2d(to.point);
        return { from, Shape::Line(length, width), to };
    }

    ZoneSpec LineFacing(Anchor const& from, float facing, float length, float width)
    {
        ZoneSpec spec{ from, Shape::Line(length, width), std::nullopt };
        spec.anchor.point.SetOrientation(facing);
        spec.anchor.followFacing = false;
        return spec;
    }
}

// --------------------------------------------------------------------------------------------------------------------
// MechanicRef
// --------------------------------------------------------------------------------------------------------------------
namespace BossMechanics
{
    MechanicRef::MechanicRef(Map* map, uint32 id, bool commitOnDestroy) : _map(map), _id(id), _commit(commitOnDestroy)
    {
    }

    MechanicRef::~MechanicRef()
    {
        if (!_commit || !_id || !_map)
            return;
        if (Registry* reg = Registry::Get(_map))
            reg->Commit(_id);
    }

    MechanicRef::MechanicRef(MechanicRef&& other) noexcept : _map(other._map), _id(other._id), _commit(other._commit)
    {
        other._map = nullptr;
        other._id = 0;
    }

    bool MechanicRef::IsValid() const
    {
        return Get() != nullptr;
    }

    ActiveMechanic const* MechanicRef::Get() const
    {
        if (!_id || !_map)
            return nullptr;
        Registry const* reg = Registry::Get(_map);
        return reg ? reg->Find(_id) : nullptr;
    }

    ActiveMechanic* MechanicRef::Edit(char const* option, bool fits, bool visual)
    {
        if (!_id || !_map)
            return nullptr;
        Registry* reg = Registry::Get(_map);
        ActiveMechanic* m = reg ? reg->FindMutable(_id) : nullptr;
        if (!m)
            return nullptr;
        if (!fits)
        {
            LOG_WARN(kLog, "Mechanic {} ({} {}, tag {}): option {} does not fit; ignored", m->id, KindName(m->kind),
                ShapeName(m->shape.type), m->tag, option);
            return nullptr;
        }
        if (visual && m->committed)
            LOG_WARN(kLog, "Mechanic {} ({}): option {} set after the commit; markers already spawned will not "
                "show it (set options in the registering statement)", m->id, KindName(m->kind), option);
        return m;
    }

    MechanicRef& MechanicRef::Damage(float pctNonTank)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("Damage", cur && IsDamageKind(cur->kind)))
            m->damage.pctNonTank = std::max(0.f, pctNonTank);
        return *this;
    }

    MechanicRef& MechanicRef::TankDamage(float pctTank)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("TankDamage", cur && IsDamageKind(cur->kind)))
            m->damage.pctTank = std::max(0.f, pctTank);
        return *this;
    }

    MechanicRef& MechanicRef::FlatDamage(int32 flat)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("FlatDamage", cur && IsDamageKind(cur->kind)))
            m->damage.flat = std::max(0, flat);
        return *this;
    }

    MechanicRef& MechanicRef::School(uint8 school)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("School", cur && IsDamageKind(cur->kind) && school <= 6))
            m->damage.school = school;
        return *this;
    }

    MechanicRef& MechanicRef::Spell(uint32 spellId)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("Spell", cur && IsDamageKind(cur->kind) && spellId != 0))
            m->damage.spellId = spellId;
        return *this;
    }

    MechanicRef& MechanicRef::Rider(uint32 spellId)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("Rider", cur && IsDamageKind(cur->kind)))
            m->damage.riderSpell = spellId;
        return *this;
    }

    MechanicRef& MechanicRef::Weakened()
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("Weakened", cur && CanWeaken(cur->kind)))
            m->weakened = true;
        return *this;
    }

    MechanicRef& MechanicRef::WeakenedOncePerPlayer()
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("WeakenedOncePerPlayer", cur && CanWeaken(cur->kind)))
        {
            m->weakened = true;
            m->weakenedOncePerPlayer = true;
        }
        return *this;
    }

    MechanicRef& MechanicRef::Persistent(Milliseconds duration, Milliseconds tick)
    {
        ActiveMechanic const* cur = Get();
        bool const fits = cur && (IsShapeKind(cur->kind) || IsStackOrSoak(cur->kind) || IsCarrierKind(cur->kind))
            && !cur->mine && !cur->projectile;
        if (ActiveMechanic* m = Edit("Persistent", fits))
        {
            Persist p;
            p.duration = std::max(duration, Milliseconds(0));
            p.tick = std::max(tick, Milliseconds(0));
            m->persist = p;
            if (Registry* reg = Registry::Get(_map))
                reg->FinalizeTiming(*m);
        }
        return *this;
    }

    MechanicRef& MechanicRef::Projectile(float speed)
    {
        ActiveMechanic const* cur = Get();
        bool const fits = cur && IsShapeKind(cur->kind) && cur->shape.type == ShapeType::Line && !cur->mine
            && !cur->persist && speed > 0.f && !cur->resolved && !cur->front;
        if (ActiveMechanic* m = Edit("Projectile", fits))
        {
            if (!m->projectile)
                m->projectile.emplace();
            m->projectile->speed = speed;
            if (Registry* reg = Registry::Get(_map))
                reg->FinalizeTiming(*m);
        }
        return *this;
    }

    MechanicRef& MechanicRef::ProjectileUnit(Unit const* unit)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("ProjectileUnit", cur && cur->projectile && !cur->front && unit))
            m->projectile->unit = unit->GetGUID();
        return *this;
    }

    MechanicRef& MechanicRef::OutAndBack(Milliseconds pause)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("OutAndBack", cur && cur->projectile && !cur->front))
        {
            m->projectile->outAndBack = true;
            m->projectile->returnPause = std::max(pause, Milliseconds(0));
            if (Registry* reg = Registry::Get(_map))
                reg->FinalizeTiming(*m);
        }
        return *this;
    }

    MechanicRef& MechanicRef::Lifetime(Milliseconds lifetime)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("Lifetime", cur && cur->mine))
        {
            m->mine->lifetime = std::max(lifetime, Milliseconds(0));
            if (Registry* reg = Registry::Get(_map))
                reg->FinalizeTiming(*m);
        }
        return *this;
    }

    MechanicRef& MechanicRef::Cap(uint32 group, uint8 n)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("Cap", cur && cur->mine && !cur->committed))
        {
            m->mine->capGroup = group;
            m->mine->cap = n;
        }
        return *this;
    }

    MechanicRef& MechanicRef::ExcludeSourceEnd(float yd)
    {
        ActiveMechanic const* cur = Get();
        bool const fits = cur && IsShapeKind(cur->kind) && cur->shape.type == ShapeType::Line && yd >= 0.f
            && yd < cur->shape.length && !cur->front;
        if (ActiveMechanic* m = Edit("ExcludeSourceEnd", fits, true))
            m->shape.excludeStart = yd;
        return *this;
    }

    MechanicRef& MechanicRef::FacingOffset(float deg)
    {
        ActiveMechanic const* cur = Get();
        bool const fits = cur && IsShapeKind(cur->kind)
            && (cur->shape.type == ShapeType::Cone || cur->shape.type == ShapeType::Line) && !cur->front;
        if (ActiveMechanic* m = Edit("FacingOffset", fits, true))
            m->shape.facingOffsetDeg = deg;
        return *this;
    }

    MechanicRef& MechanicRef::Rear()
    {
        return FacingOffset(180.f);
    }

    MechanicRef& MechanicRef::AnchorOn(Unit const* unit)
    {
        ActiveMechanic const* cur = Get();
        bool const fits = cur && unit && (IsShapeKind(cur->kind) || cur->kind == Kind::Stack
            || cur->kind == Kind::Soak) && !cur->front;
        if (ActiveMechanic* m = Edit("AnchorOn", fits, true))
        {
            bool const follow = m->anchor.followFacing;
            m->anchor = Anchor(unit);
            m->anchor.followFacing = follow;
        }
        return *this;
    }

    MechanicRef& MechanicRef::FollowFacing()
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("FollowFacing", cur && cur->anchor.IsUnit()))
            m->anchor.followFacing = true;
        return *this;
    }

    MechanicRef& MechanicRef::ZTolerance(float yd)
    {
        if (ActiveMechanic* m = Edit("ZTolerance", yd > 0.f))
            m->shape.zTolerance = yd;
        return *this;
    }

    MechanicRef& MechanicRef::Eligible(Eligibility const& eligibility)
    {
        if (ActiveMechanic* m = Edit("Eligible", true))
            m->eligibility = eligibility;
        return *this;
    }

    MechanicRef& MechanicRef::Order(Ordering const& ordering)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("Order", cur && (cur->kind == Kind::Soak || cur->kind == Kind::Bait)))
            m->ordering = ordering;
        return *this;
    }

    MechanicRef& MechanicRef::EndBacksOff(float yd)
    {
        ActiveMechanic const* cur = Get();
        bool const fits = IsLineGetIn(cur) && cur->to && cur->to->IsUnit() && std::isfinite(yd) && yd > 0.f;
        if (ActiveMechanic* m = Edit("EndBacksOff", fits, true))
        {
            m->endBackOffYd = yd;
            m->shape.length = std::max(m->shape.length, yd);    // the hitbox (and marker) reach the backed-off end
        }
        return *this;
    }

    MechanicRef& MechanicRef::TankNearSource(float yd)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("TankNearSource", IsLineGetIn(cur) && std::isfinite(yd) && yd > 0.f))
            m->tankNearYd = yd;
        return *this;
    }

    MechanicRef& MechanicRef::HoldTankFacing(Unit const* source)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("HoldTankFacing", cur && !cur->decoy))
        {
            m->holdTankFacing = true;
            m->holdFacingSource = source ? source->GetGUID() : ObjectGuid::Empty;
        }
        return *this;
    }

    MechanicRef& MechanicRef::OneClaimPerBot()
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("OneClaimPerBot", cur && cur->kind == Kind::Soak))
            m->oneClaimPerBot = true;
        return *this;
    }

    MechanicRef& MechanicRef::MissedSoak(MissedSoakMode mode)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("MissedSoak", cur && cur->kind == Kind::Soak))
            m->missedSoak = mode;
        return *this;
    }

    MechanicRef& MechanicRef::Count(uint8 n)
    {
        ActiveMechanic const* cur = Get();
        bool const fits = cur && (cur->kind == Kind::Spread || cur->kind == Kind::RunOut) && !cur->committed;
        if (ActiveMechanic* m = Edit("Count", fits))
            m->count = n;
        return *this;
    }

    MechanicRef& MechanicRef::Targets(GuidSet const& players)
    {
        ActiveMechanic const* cur = Get();
        bool const fits = cur && (cur->kind == Kind::Spread || cur->kind == Kind::RunOut
            || cur->kind == Kind::HoldStill);
        if (ActiveMechanic* m = Edit("Targets", fits, true))
            m->targets = players;
        return *this;
    }

    MechanicRef& MechanicRef::SelfTrigger(uint32 auraSpell, uint8 minStacks)
    {
        ActiveMechanic const* cur = Get();
        bool const fits = cur && (cur->kind == Kind::Spread || cur->kind == Kind::RunOut) && auraSpell;
        if (ActiveMechanic* m = Edit("SelfTrigger", fits))
        {
            m->selfTrigger.auraSpell = auraSpell;
            m->selfTrigger.minStacks = minStacks;
        }
        return *this;
    }

    MechanicRef& MechanicRef::From(MeasuredFrom from)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("From", cur && cur->kind == Kind::RunOut))
            m->measuredFrom = from;
        return *this;
    }

    MechanicRef& MechanicRef::TankExempt()
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("TankExempt", cur && IsShapeKind(cur->kind)))
        {
            m->tankExempt = true;
            if (m->kind == Kind::Zone && m->shape.type == ShapeType::Cone)
                m->kind = Kind::Frontal;
        }
        return *this;
    }

    MechanicRef& MechanicRef::Cluster(uint8 minOthers)
    {
        ActiveMechanic const* cur = Get();
        bool const fits = cur && cur->kind == Kind::Stack && cur->shape.type == ShapeType::Circle && minOthers > 0
            && !cur->committed;
        if (ActiveMechanic* m = Edit("Cluster", fits))
            m->clusterMinOthers = minOthers;
        return *this;
    }

    MechanicRef& MechanicRef::Radius(float yd)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("Radius", cur && cur->kind == Kind::Bait && yd > 0.f))
            m->shape.radius = yd;
        return *this;
    }

    MechanicRef& MechanicRef::Tolerance(float yd)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("Tolerance", cur && cur->kind == Kind::HoldStill && yd >= 0.f))
            m->holdTolerance = yd;
        return *this;
    }

    MechanicRef& MechanicRef::Park(Unit const* unit)
    {
        ActiveMechanic const* cur = Get();
        // A front parks nothing: its safe slot is Mechanic::FrontSafe (WP-RF).
        if (ActiveMechanic* m = Edit("Park", cur && IsShapeKind(cur->kind) && !cur->front))
        {
            m->park = true;
            m->parkUnit = unit ? unit->GetGUID() : ObjectGuid::Empty;
        }
        return *this;
    }

    MechanicRef& MechanicRef::ParkTogether(Unit const* unit)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("ParkTogether", cur && IsShapeKind(cur->kind) && !cur->front))
        {
            m->park = true;
            m->parkTogether = true;
            m->parkUnit = unit ? unit->GetGUID() : ObjectGuid::Empty;
        }
        return *this;
    }

    MechanicRef& MechanicRef::DropSpot(DropSpotRule const& rule)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("DropSpot", cur && IsShapeKind(cur->kind) && !cur->decoy && !cur->front))
            m->dropSpot = rule;
        return *this;
    }

    MechanicRef& MechanicRef::Within(SpotBounds const& bounds)
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("Within", cur && !cur->decoy))
            m->bounds = bounds;
        return *this;
    }

    MechanicRef& MechanicRef::Color(BossMechanics::Color color)
    {
        if (ActiveMechanic* m = Edit("Color", true, true))
            m->color = color;
        return *this;
    }

    MechanicRef& MechanicRef::Tag(uint32 tag)
    {
        if (ActiveMechanic* m = Edit("Tag", true))
            m->tag = tag;
        return *this;
    }

    MechanicRef& MechanicRef::NoTelegraph()
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("NoTelegraph", cur && !cur->committed))
            m->telegraph = false;
        return *this;
    }

    MechanicRef& MechanicRef::NoFillTimer()
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("NoFillTimer", cur != nullptr, true))
            m->fillTimer = false;
        return *this;
    }

    MechanicRef& MechanicRef::Decoy()
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("Decoy", cur && !cur->committed && !cur->lethal
            && Judge::DecoyFits(cur->kind, cur->mine.has_value())))
            m->decoy = true;
        return *this;
    }

    MechanicRef& MechanicRef::Lethal()
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("Lethal", cur && !cur->committed
            && Judge::LethalFits(cur->kind, cur->mine.has_value(), cur->decoy)))
            m->lethal = true;
        return *this;
    }

    MechanicRef& MechanicRef::SparesPets()
    {
        ActiveMechanic const* cur = Get();
        if (ActiveMechanic* m = Edit("SparesPets", cur && cur->lethal))
            m->sparesPets = true;
        return *this;
    }

    MechanicRef& MechanicRef::OnHit(std::function<void(Unit* hit)> fn)
    {
        if (ActiveMechanic* m = Edit("OnHit", true))
            m->onHit = std::move(fn);
        return *this;
    }

    MechanicRef& MechanicRef::OnResolve(std::function<void(ResolveResult const&)> fn)
    {
        if (ActiveMechanic* m = Edit("OnResolve", true))
            m->onResolve = std::move(fn);
        return *this;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // LaneBuilder
    // ----------------------------------------------------------------------------------------------------------------
    LaneBuilder::LaneBuilder(Map* map, ObjectGuid owner, uint32 laneSetId) : _map(map), _owner(owner),
        _laneSet(laneSetId)
    {
    }

    LaneBuilder::~LaneBuilder()
    {
        if (!_map)
            return;
        if (Registry* reg = Registry::Get(_map))
            for (uint32 id : _created)
                reg->Commit(id);
    }

    LaneBuilder::LaneBuilder(LaneBuilder&& other) noexcept : _map(other._map), _owner(other._owner),
        _laneSet(other._laneSet), _created(std::move(other._created)), _free(std::move(other._free))
    {
        other._map = nullptr;
        other._created.clear();
        other._free.reset();
    }

    std::vector<MechanicHandle> LaneBuilder::Handles() const
    {
        std::vector<MechanicHandle> out;
        for (uint32 id : _created)
            out.push_back({ id });
        return out;
    }

    uint32 LaneBuilder::AddLine(LanePlacement const& place, Milliseconds warn, Milliseconds resolveAt,
        LanePlacement const* drawn)
    {
        Position const at(place.pose.x, place.pose.y, place.pose.z, place.pose.o);
        Shape const shape = Shape::Line(place.length, place.width);
        float lateral = 0.f;
        float drawWidth = 0.f;
        if (drawn && std::fabs(drawn->width - place.width) > 1e-4f)
        {
            lateral = Geo::ToLineCoords(shape, place.pose, { drawn->pose.x, drawn->pose.y, drawn->pose.z }).t;
            drawWidth = drawn->width;
        }
        MechanicRef ref = Register(_map, _owner, Kind::Zone, Anchor(at), shape, warn,
            [this, resolveAt, lateral, drawWidth](ActiveMechanic& m)
            {
                m.laneSet = _laneSet;
                if (resolveAt > Milliseconds(0))
                    m.resolveAt = resolveAt;
                m.drawLateral = lateral;
                m.drawWidth = drawWidth;
            }, false);
        uint32 const id = ref.Id();
        if (id)
            _created.push_back(id);
        return id;
    }

    void LaneBuilder::ColorSecondAxis(Axis axis, std::size_t firstNew)
    {
        Registry* reg = _map ? Registry::Get(_map) : nullptr;
        LaneSet const* ls = reg ? reg->FindLaneSet(_laneSet) : nullptr;
        Encounter const* e = reg ? reg->FindEncounter(_owner) : nullptr;
        if (!ls || !e)
            return;
        bool otherAxis = false;
        for (std::size_t i = 0; i < std::min(firstNew, ls->rects.size()); ++i)
            if (ls->rects[i].axis != axis)
                otherAxis = true;
        if (!otherAxis)
            return;
        for (std::size_t i = firstNew; i < ls->lanes.size(); ++i)
            if (ActiveMechanic* m = reg->FindMutable(ls->lanes[i]))
                m->color = e->crossLanes;
    }

    LaneBuilder& LaneBuilder::Lanes(Axis axis, LaneSpec const& spec, Milliseconds warn)
    {
        Registry* reg = _map ? Registry::Get(_map) : nullptr;
        LaneSet* ls = reg ? reg->FindLaneSetMutable(_laneSet) : nullptr;
        if (!ls)
            return *this;
        if (spec.count == 0 || spec.width <= 0.f)
        {
            LOG_WARN(kLog, "RoomFrame {}: Lanes needs count > 0 and width > 0; ignored", _laneSet);
            return *this;
        }
        Frame const frame = ls->frame.ToFrame();
        std::size_t const firstNew = ls->lanes.size();
        bool fellBack = false;
        std::vector<float> const offsets = RandomLaneOffsets(FrameSpanAcross(frame, axis), spec.count, spec.width,
            std::max(0.f, spec.minGap), []() { return frand(0.f, 1.f); }, &fellBack);
        if (fellBack)
            LOG_WARN(kLog, "RoomFrame {}: {} lanes of {} yd with gaps >= {} do not fit across {} yd; spaced evenly",
                _laneSet, spec.count, spec.width, spec.minGap, FrameSpanAcross(frame, axis));
        for (float o : offsets)
        {
            LaneRect const rect{ axis, o, spec.width };
            uint32 const id = AddLine(PlaceLane(frame, rect), warn, Milliseconds(0));
            if (!id)
                continue;
            if (LaneSet* set = reg->FindLaneSetMutable(_laneSet))
            {
                set->lanes.push_back(id);
                set->rects.push_back(rect);
            }
        }
        ColorSecondAxis(axis, firstNew);
        return *this;
    }

    LaneBuilder& LaneBuilder::ExceptOne(float width, Milliseconds warn, Axis axis)
    {
        Registry* reg = _map ? Registry::Get(_map) : nullptr;
        LaneSet* ls = reg ? reg->FindLaneSetMutable(_laneSet) : nullptr;
        if (!ls)
            return *this;
        if (width <= 0.f)
        {
            LOG_WARN(kLog, "RoomFrame {}: ExceptOne needs width > 0; ignored", _laneSet);
            return *this;
        }
        Frame const frame = ls->frame.ToFrame();
        std::size_t const firstNew = ls->lanes.size();
        uint32 freeIndex = 0;
        std::vector<float> const offsets = CoverLaneOffsets(FrameSpanAcross(frame, axis), width,
            []() { return frand(0.f, 1.f); }, freeIndex);
        for (std::size_t i = 0; i < offsets.size(); ++i)
        {
            LaneRect const rect{ axis, offsets[i], width };
            if (i == freeIndex)
            {
                if (LaneSet* set = reg->FindLaneSetMutable(_laneSet))
                    set->freeLanes.push_back(rect);
                continue;
            }
            // The last lane is clamped inside the frame and overlaps the one before it: its marker draws only the
            // strip the others leave uncovered (the hitbox stays the whole lane).
            LaneStrip const strip = CoverLaneDrawnStrip(offsets, width, static_cast<uint32>(i), freeIndex);
            LanePlacement const drawn = PlaceLane(frame, { axis, strip.offset, strip.width });
            uint32 const id = AddLine(PlaceLane(frame, rect), warn, Milliseconds(0), &drawn);
            if (!id)
                continue;
            if (LaneSet* set = reg->FindLaneSetMutable(_laneSet))
            {
                set->lanes.push_back(id);
                set->rects.push_back(rect);
            }
        }
        ColorSecondAxis(axis, firstNew);
        if (freeIndex < offsets.size())
        {
            LaneStrip const strip = DecoyLaneStrip(offsets, width, freeIndex);
            _free = FreeLane{ PlaceLane(frame, { axis, strip.offset, strip.width }), warn, firstNew };
        }
        return *this;
    }

    LaneBuilder& LaneBuilder::DecoyFree()
    {
        Registry* reg = _map ? Registry::Get(_map) : nullptr;
        LaneSet const* ls = reg ? reg->FindLaneSet(_laneSet) : nullptr;
        if (!ls)
            return *this;
        if (!_free || _free->place.width <= 0.f)
        {
            LOG_WARN(kLog, "RoomFrame {}: DecoyFree needs an ExceptOne before it (once per ExceptOne); ignored",
                _laneSet);
            return *this;
        }
        FreeLane const free = *_free;
        _free.reset();
        // The color of that call's real lanes (the second axis of a set draws in crossLanes).
        std::optional<BossMechanics::Color> color;
        if (free.firstLane < ls->lanes.size())
            if (ActiveMechanic const* lane = reg->Find(ls->lanes[free.firstLane]))
                color = lane->color;
        uint32 const id = AddLine(free.place, free.warn, Milliseconds(0));
        if (!id)
            return *this;
        MechanicRef decoy(_map, id, false);
        decoy.Decoy();
        if (color)
            decoy.Color(*color);
        return *this;
    }

    LaneBuilder& LaneBuilder::BurnCrossings(Milliseconds duration, Milliseconds tick, float pct)
    {
        Registry* reg = _map ? Registry::Get(_map) : nullptr;
        LaneSet* ls = reg ? reg->FindLaneSetMutable(_laneSet) : nullptr;
        if (!ls)
            return *this;
        Frame const frame = ls->frame.ToFrame();
        // Copy: AddLine may touch the registry's containers.
        std::vector<uint32> const lanes = ls->lanes;
        std::vector<LaneRect> const rects = ls->rects;
        std::size_t made = 0;
        for (std::size_t i = 0; i < rects.size(); ++i)
        {
            if (rects[i].axis != Axis::X)
                continue;
            for (std::size_t j = 0; j < rects.size(); ++j)
            {
                if (rects[j].axis != Axis::Y)
                    continue;
                ActiveMechanic const* a = reg->Find(lanes[i]);
                ActiveMechanic const* b = reg->Find(lanes[j]);
                Milliseconds const at = std::max(a ? a->resolveAt : Milliseconds(0),
                    b ? b->resolveAt : Milliseconds(0));
                uint32 const id = AddLine(PlaceCrossing(frame, rects[i], rects[j]), Milliseconds(0), at);
                if (!id)
                    continue;
                MechanicRef cell(_map, id, false);
                cell.Persistent(duration, tick).Damage(pct).NoFillTimer();
                if (Encounter const* e = reg->FindEncounter(_owner))
                    cell.Color(e->cells);
                if (LaneSet* set = reg->FindLaneSetMutable(_laneSet))
                    set->cells.push_back(id);
                ++made;
            }
        }
        if (!made)
            LOG_WARN(kLog, "RoomFrame {}: BurnCrossings found no X/Y lane pair; call Lanes on both axes first",
                _laneSet);
        return *this;
    }

    LaneBuilder& LaneBuilder::Each(std::function<void(MechanicRef&)> const& fn)
    {
        if (!_map || !fn)
            return *this;
        for (uint32 id : _created)
        {
            MechanicRef ref(_map, id, false);
            fn(ref);
        }
        return *this;
    }
}

// --------------------------------------------------------------------------------------------------------------------
// Mechanic:: free functions
// --------------------------------------------------------------------------------------------------------------------
namespace Mechanic
{
    void BeginEncounter(Unit* owner, EncounterOptions const& opt)
    {
        if (Map* map = MapOf(owner, "BeginEncounter"))
            Registry::GetOrCreate(map).BeginEncounter(owner->GetGUID(), &opt);
    }

    void EndEncounter(Unit* owner)
    {
        if (Registry* reg = RegistryOf(owner))
            reg->EndEncounter(owner->GetGUID());
    }

    MechanicRef Zone(Unit* owner, ZoneSpec const& spec, Milliseconds warn)
    {
        Shape const& s = spec.shape;
        if (!SizeOk(ShapeSizeOk(s), "Zone", owner))
            return MechanicRef();
        std::optional<Anchor> const to = spec.to;
        return Register(owner, "Zone", Kind::Zone, spec.anchor, s, warn, [&to](ActiveMechanic& m)
        {
            m.to = to;
        });
    }

    MechanicRef Circle(Unit* owner, Anchor const& at, float r, Milliseconds warn)
    {
        return Zone(owner, Shapes::Circle(at, r), warn);
    }

    MechanicRef Donut(Unit* owner, Anchor const& at, float inner, float outer, Milliseconds warn)
    {
        return Zone(owner, Shapes::Donut(at, inner, outer), warn);
    }

    MechanicRef Cone(Unit* owner, Unit* source, float angleDeg, float length, Milliseconds warn, TankExempt exempt)
    {
        if (!source)
        {
            LOG_WARN(kLog, "Mechanic::Cone: source is null; ignored");
            return MechanicRef();
        }
        MechanicRef ref = Zone(owner, Shapes::Cone(Anchor(source), angleDeg, length), warn);
        if (exempt == TankExempt::Yes)
            ref.TankExempt();
        return ref;
    }

    MechanicRef Line(Unit* owner, Anchor const& from, Anchor const& to, float width, Milliseconds warn)
    {
        return Zone(owner, Shapes::Line(from, to, width), warn);
    }

    MechanicRef Line(Unit* owner, Anchor const& from, float facing, float length, float width, Milliseconds warn)
    {
        return Zone(owner, Shapes::LineFacing(from, facing, length, width), warn);
    }

    // Blackrock Spire WP-RF (rolling fronts, MechanicFront.h).
    MechanicRef ConeFront(Unit* owner, Position const& apex, ConeFrontSpec const& spec, Milliseconds warn)
    {
        if (!Front::Valid(spec))
        {
            LOG_WARN(kLog, "Mechanic::ConeFront from {}: needs band >= {} yd, speed > 0, end > start >= 0 and a slice "
                "of 0-{} degrees; ignored", owner ? owner->GetGUID().ToString() : "?", Front::kMinBandYd,
                Front::kMaxSliceDeg);
            return MechanicRef();
        }
        Position at = apex;
        at.SetOrientation(Front::ConeFacing(spec));
        ProjectileParams const p = Front::ConeParams(spec);
        return Register(owner, "ConeFront", Kind::Zone, Anchor(at), Front::ConeShape(spec), warn,
            [&spec, &p](ActiveMechanic& m)
        {
            m.projectile.emplace();
            m.projectile->speed = p.speed;
            m.projectile->halfLength = p.halfLength;
            m.projectile->start = p.start;
            m.front = Front::InfoOf(spec);
        });
    }

    MechanicRef LineFront(Unit* owner, Position const& start, LineFrontSpec const& spec, Milliseconds warn)
    {
        if (!Front::Valid(spec))
        {
            LOG_WARN(kLog, "Mechanic::LineFront from {}: needs band >= {} yd, speed > 0, travel and width > 0 and "
                "gaps of width > 0; ignored", owner ? owner->GetGUID().ToString() : "?", Front::kMinBandYd);
            return MechanicRef();
        }
        if (spec.gaps.empty())
            LOG_WARN(kLog, "Mechanic::LineFront from {}: a wall with no gap cannot be dodged",
                owner ? owner->GetGUID().ToString() : "?");
        Position at = start;
        at.SetOrientation(spec.facing);
        ProjectileParams const p = Front::LineParams(spec);
        return Register(owner, "LineFront", Kind::Zone, Anchor(at), Front::LineShape(spec), warn,
            [&spec, &p](ActiveMechanic& m)
        {
            m.projectile.emplace();
            m.projectile->speed = p.speed;
            m.projectile->halfLength = p.halfLength;
            m.projectile->start = p.start;
            m.front = Front::InfoOf(spec);
        });
    }

    MechanicRef FrontSafe(Unit* owner, MechanicHandle front, Unit const* parked, float untilYd, std::size_t gap)
    {
        Registry* reg = RegistryOf(owner);
        ActiveMechanic const* f = reg ? reg->Find(front.id) : nullptr;
        if (!f || !f->front || !f->projectile || f->owner != owner->GetGUID())
        {
            LOG_WARN(kLog, "Mechanic::FrontSafe from {}: mechanic {} is not a live front of this owner; ignored",
                owner ? owner->GetGUID().ToString() : "?", front.id);
            return MechanicRef();
        }
        FrontInfo const info = *f->front;
        ProjectileParams const params = ProjectileParamsOf(*f);
        Geo::Pose const apex = reg->AnchorPose(*f);
        Milliseconds const resolveAt = f->resolveAt;
        Milliseconds const expireAt = f->expireAt;
        float const length = f->shape.length;
        float const width = f->shape.width;
        // The safe slot as a Zone: a cone front's slice (a cone on the apex), a line front's gap (a lane along the
        // facing, laterally on the gap's centre).
        Position at(apex.x, apex.y, apex.z, apex.o);
        Shape shape;
        if (info.type == FrontType::Cone)
        {
            if (info.sliceDeg <= 0.f)
            {
                LOG_WARN(kLog, "Mechanic::FrontSafe from {}: front {} is a full ring (no slice); ignored",
                    owner->GetGUID().ToString(), front.id);
                return MechanicRef();
            }
            shape = Shape::Cone(info.sliceDeg, length);
            at.SetOrientation(info.sliceBearing);
        }
        else
        {
            if (gap >= info.gaps.size())
            {
                LOG_WARN(kLog, "Mechanic::FrontSafe from {}: front {} has no gap {}; ignored",
                    owner->GetGUID().ToString(), front.id, gap);
                return MechanicRef();
            }
            FrontGap const& g = info.gaps[gap];
            float const half = width * 0.5f;
            float const a = std::max(-half, g.centre - g.width * 0.5f);
            float const b = std::min(half, g.centre + g.width * 0.5f);
            if (b <= a)
                return MechanicRef();
            float const mid = (a + b) * 0.5f;
            at.Relocate(apex.x - std::sin(apex.o) * mid, apex.y + std::cos(apex.o) * mid, apex.z);
            shape = Shape::Line(length, b - a);
        }
        shape.zTolerance = f->shape.zTolerance;
        // Live from now through the launch until the band has passed untilYd (0: the whole front).
        Milliseconds until = expireAt;
        if (untilYd > 0.f)
        {
            // The band's back edge (centre - h) reaches untilYd: centre = untilYd + h.
            float const travel = std::max(0.f, untilYd + params.halfLength - params.start);
            until = std::min(expireAt, resolveAt + MsOf(travel / params.speed));
        }
        Milliseconds const now = GameTime::GetGameTimeMS();
        Milliseconds const warn = std::max(Milliseconds(0), resolveAt - now);
        MechanicRef ref = Register(owner, "FrontSafe", Kind::Zone, Anchor(at), shape, warn,
            [](ActiveMechanic& m)
        {
            m.eligibility.roles = Role::None;       // nobody: never a hazard, never hits
            m.telegraph = false;                    // the front draws its own slice or gap
        });
        if (!ref.IsValid())
            return ref;
        ref.Persistent(std::max(Milliseconds(1), until - std::max(resolveAt, now)), Milliseconds(0));
        ref.ParkTogether(parked);
        return ref;
    }

    MechanicRef Mine(Unit* owner, Position const& pos, float r, Milliseconds arm, Milliseconds grace)
    {
        if (!SizeOk(r > 0.f, "Mine", owner))
            return MechanicRef();
        return Register(owner, "Mine", Kind::Zone, Anchor(pos), Shape::Circle(r), arm, [grace](ActiveMechanic& m)
        {
            m.mine.emplace();
            m.mine->grace = std::max(grace, Milliseconds(0));
        });
    }

    LaneBuilder RoomFrame(Unit* owner, BossMechanics::RoomFrame const& frame)
    {
        Map* map = MapOf(owner, "RoomFrame");
        if (!map)
            return LaneBuilder();
        if (frame.sizeX <= 0.f || frame.sizeY <= 0.f)
        {
            LOG_WARN(kLog, "Mechanic::RoomFrame from {}: frame size must be > 0; ignored", owner->GetGUID().ToString());
            return LaneBuilder();
        }
        Registry& reg = Registry::GetOrCreate(map);
        reg.BeginEncounter(owner->GetGUID());
        LaneSet const& ls = reg.AddLaneSet(owner->GetGUID(), frame);
        return LaneBuilder(map, owner->GetGUID(), ls.id);
    }

    MechanicRef Spread(Unit* owner, float r, Milliseconds warn)
    {
        if (!SizeOk(r > 0.f, "Spread", owner))
            return MechanicRef();
        return Register(owner, "Spread", Kind::Spread, Anchor(owner), Shape::Circle(r), warn);
    }

    MechanicRef RunOut(Unit* owner, Unit* target, float r, Milliseconds warn)
    {
        if (!SizeOk(r > 0.f, "RunOut", owner))
            return MechanicRef();
        return Register(owner, "RunOut", Kind::RunOut, Anchor(owner), Shape::Circle(r), warn,
            [target](ActiveMechanic& m)
            {
                if (target)
                    m.targets.insert(target->GetGUID());
            });
    }

    MechanicRef Stack(Unit* owner, Anchor const& at, float r, Milliseconds warn)
    {
        if (!SizeOk(r > 0.f, "Stack", owner))
            return MechanicRef();
        return Register(owner, "Stack", Kind::Stack, at, Shape::Circle(r), warn);
    }

    MechanicRef Soak(Unit* owner, Anchor const& at, float r, uint8 count, Milliseconds warn)
    {
        if (!SizeOk(r > 0.f, "Soak", owner))
            return MechanicRef();
        return Register(owner, "Soak", Kind::Soak, at, Shape::Circle(r), warn, [count](ActiveMechanic& m)
        {
            m.soakCount = count;
        });
    }

    MechanicRef Stack(Unit* owner, ZoneSpec const& spec, Milliseconds warn)
    {
        if (!SizeOk(ShapeSizeOk(spec.shape), "Stack", owner))
            return MechanicRef();
        std::optional<Anchor> const to = spec.to;
        return Register(owner, "Stack", Kind::Stack, spec.anchor, spec.shape, warn, [&to](ActiveMechanic& m)
        {
            m.to = to;
        });
    }

    MechanicRef Soak(Unit* owner, ZoneSpec const& spec, uint8 count, Milliseconds warn)
    {
        if (!SizeOk(ShapeSizeOk(spec.shape), "Soak", owner))
            return MechanicRef();
        std::optional<Anchor> const to = spec.to;
        return Register(owner, "Soak", Kind::Soak, spec.anchor, spec.shape, warn, [&to, count](ActiveMechanic& m)
        {
            m.to = to;
            m.soakCount = count;
        });
    }

    MechanicRef Bait(Unit* owner, BaitRule const& rule, float othersRange, Milliseconds warn)
    {
        return Register(owner, "Bait", Kind::Bait, Anchor(owner), Shape::Circle(0.f), warn,
            [&rule, othersRange](ActiveMechanic& m)
            {
                m.bait = rule;
                m.othersRange = std::max(0.f, othersRange);
                m.telegraph = false;
            });
    }

    MechanicRef HoldStill(Unit* owner, GuidSet const& players, Milliseconds window, Milliseconds warn)
    {
        return Register(owner, "HoldStill", Kind::HoldStill, Anchor(owner), Shape::Circle(0.f), warn,
            [&players, window](ActiveMechanic& m)
            {
                m.targets = players;
                m.holdWindow = std::max(window, Milliseconds(0));
                m.telegraph = false;
            });
    }

    MechanicRef Spot(Unit* owner, std::unordered_map<ObjectGuid, Position> const& spots, Milliseconds warn, float r)
    {
        if (!SizeOk(r > 0.f, "Spot", owner))
            return MechanicRef();
        return Register(owner, "Spot", Kind::Spot, Anchor(owner), Shape::Circle(r), warn, [&spots](ActiveMechanic& m)
        {
            m.spots = spots;
        });
    }

    MechanicRef Jump(Unit* owner, ZoneSpec const& spec, Milliseconds warn)
    {
        if (!SizeOk(ShapeSizeOk(spec.shape), "Jump", owner))
            return MechanicRef();
        std::optional<Anchor> const to = spec.to;
        return Register(owner, "Jump", Kind::Jump, spec.anchor, spec.shape, warn, [&to](ActiveMechanic& m)
        {
            m.to = to;
        });
    }

    MechanicRef Jump(Unit* owner, Anchor const& at, float r, Milliseconds warn)
    {
        if (!SizeOk(r > 0.f, "Jump", owner))
            return MechanicRef();
        return Register(owner, "Jump", Kind::Jump, at, Shape::Circle(r), warn);
    }

    MechanicRef Hide(Unit* owner, HideSpec const& spec, Milliseconds warn)
    {
        if (!SizeOk(spec.rangeYd > 0.f, "Hide", owner))
            return MechanicRef();
        HideSpec s = spec;
        if (s.rangeYd > BossMechanics::Hide::kMaxRangeYd)
        {
            LOG_WARN(kLog, "Mechanic::Hide from {}: range {:.1f} yd is past the resolve's search; clamped to {:.0f}",
                owner ? owner->GetGUID().ToString() : "?", s.rangeYd, BossMechanics::Hide::kMaxRangeYd);
            s.rangeYd = BossMechanics::Hide::kMaxRangeYd;
        }
        // No source given (a default point at the origin): the owner's sight.
        Position const& o = s.source.point;
        if (!s.source.IsUnit() && o.GetPositionX() == 0.f && o.GetPositionY() == 0.f && o.GetPositionZ() == 0.f)
            s.source = Anchor(owner);
        s.behindYd = std::max(0.f, s.behindYd);
        s.spotRadiusYd = std::max(0.5f, s.spotRadiusYd);
        s.share = std::max<uint8>(1, s.share);
        s.reachYd = std::max(0.f, s.reachYd);
        Shape range = Shape::Circle(s.rangeYd);
        range.zTolerance = BossMechanics::Hide::kDefaultZToleranceYd;
        Anchor const source = s.source;
        return Register(owner, "Hide", Kind::Hide, source, range, warn, [&s](ActiveMechanic& m)
        {
            m.hide.emplace();
            m.hide->spec = std::move(s);
            m.telegraph = false;    // the blockers are the tell; the script draws its own cast and emote
        });
    }

    MechanicRef Gaze(Unit* owner, GazeSpec const& spec, Milliseconds warn)
    {
        if (!SizeOk(spec.rangeYd > 0.f, "Gaze", owner))
            return MechanicRef();
        GazeSpec s = spec;
        if (s.rangeYd > BossMechanics::Gaze::kMaxRangeYd)
        {
            LOG_WARN(kLog, "Mechanic::Gaze from {}: range {:.1f} yd is past the resolve's search; clamped to {:.0f}",
                owner ? owner->GetGUID().ToString() : "?", s.rangeYd, BossMechanics::Gaze::kMaxRangeYd);
            s.rangeYd = BossMechanics::Gaze::kMaxRangeYd;
        }
        // No source given (a default point at the origin): the owner's gaze.
        Position const& o = s.source.point;
        if (!s.source.IsUnit() && o.GetPositionX() == 0.f && o.GetPositionY() == 0.f && o.GetPositionZ() == 0.f)
            s.source = Anchor(owner);
        s.arcDeg = BossMechanics::Gaze::ClampArcDeg(s.arcDeg);
        s.lead = std::max(s.lead, Milliseconds(0));
        Shape range = Shape::Circle(s.rangeYd);
        range.zTolerance = BossMechanics::Gaze::kDefaultZToleranceYd;
        Anchor const source = s.source;
        return Register(owner, "Gaze", Kind::Gaze, source, range, warn, [&s](ActiveMechanic& m)
        {
            m.gaze.emplace();
            m.gaze->spec = std::move(s);
            m.telegraph = false;    // the cast and the emote are the tell; nothing on the floor to read
        });
    }

    void HoldTankFacing(Unit* owner, Unit const* source, Milliseconds duration)
    {
        Map* map = MapOf(owner, "HoldTankFacing");
        if (!map)
            return;
        ObjectGuid const src = source ? source->GetGUID() : owner->GetGUID();
        Milliseconds const until = duration > Milliseconds(0)
            ? GameTime::GetGameTimeMS() + duration : Registry::kForever;
        Registry::GetOrCreate(map).HoldTankFacing(owner->GetGUID(), src, until);
    }

    void ReleaseTankFacing(Unit* owner, Unit const* source)
    {
        if (Registry* reg = RegistryOf(owner))
            reg->ReleaseTankFacing(owner->GetGUID(), source ? source->GetGUID() : owner->GetGUID());
    }

    void Cancel(Unit* owner, MechanicHandle h)
    {
        if (Registry* reg = RegistryOf(owner))
            reg->Cancel(h.id);
    }

    void Detonate(Unit* owner, MechanicHandle h)
    {
        if (Registry* reg = RegistryOf(owner))
            reg->Detonate(h.id);
    }

    bool Contains(Unit* owner, MechanicHandle h, Unit const* who)
    {
        Registry const* reg = RegistryOf(owner);
        ActiveMechanic const* m = reg ? reg->Find(h.id) : nullptr;
        return m && !m->cancelled && reg->ContainsUnit(*m, who);
    }

    bool ContainsPoint(Unit* owner, MechanicHandle h, Position const& p)
    {
        Registry const* reg = RegistryOf(owner);
        ActiveMechanic const* m = reg ? reg->Find(h.id) : nullptr;
        return m && !m->cancelled && reg->ContainsPoint(*m, p);
    }

    PlayerLedger const* Failures(Unit* owner, ObjectGuid player)
    {
        Registry const* reg = RegistryOf(owner);
        Encounter const* e = reg ? reg->FindEncounter(owner->GetGUID()) : nullptr;
        if (!e)
            return nullptr;
        auto it = e->ledger.find(player);
        return it != e->ledger.end() ? &it->second : nullptr;
    }

    void OnFailure(Unit* owner, std::function<void(FailureEvent const&)> fn)
    {
        if (Map* map = MapOf(owner, "OnFailure"))
            Registry::GetOrCreate(map).BeginEncounter(owner->GetGUID()).onFailure = std::move(fn);
    }

    Registry const* Read(Map const* map)
    {
        return Registry::Get(map);
    }

    void Describe(Unit* owner, Descriptor descriptor)
    {
        if (Map* map = MapOf(owner, "Describe"))
            Registry::GetOrCreate(map).Describe(owner->GetGUID(), std::move(descriptor));
    }

    void EditDescriptor(Unit* owner, std::function<void(Descriptor&)> const& fn)
    {
        if (Map* map = MapOf(owner, "EditDescriptor"))
            Registry::GetOrCreate(map).EditDescriptor(owner->GetGUID(), fn);
    }

    void SetPhase(Unit* owner, uint32 phase)
    {
        if (Map* map = MapOf(owner, "SetPhase"))
            Registry::GetOrCreate(map).SetPhase(owner->GetGUID(), phase);
    }

    void NoteNextCast(Unit* caster, uint32 spellId, Milliseconds at)
    {
        if (Map* map = MapOf(caster, "NoteNextCast"))
            Registry::GetOrCreate(map).NoteNextCast(caster->GetGUID(), caster->GetEntry(), spellId, at);
    }

    void NoteNextCastIn(Unit* caster, uint32 spellId, Milliseconds in)
    {
        NoteNextCast(caster, spellId, GameTime::GetGameTimeMS() + in);
    }

    void TrackUnit(Unit* owner, Unit* unit)
    {
        Map* map = MapOf(owner, "TrackUnit");
        if (!map || !unit || unit->GetMap() != map)
            return;
        Registry::GetOrCreate(map).TrackUnit(owner->GetGUID(), unit->GetGUID(), unit->GetEntry(), false);
    }

    uint32 RefFlat(Unit const* owner, float pct, bool tank)
    {
        if (!owner || !owner->IsInWorld())
        {
            LOG_DEBUG(kLog, "Mechanic::RefFlat: owner is null or not in world; returning 0");
            return 0;
        }
        Map const* map = owner->FindMap();
        if (!map)
        {
            LOG_DEBUG(kLog, "Mechanic::RefFlat from {}: no map; returning 0", owner->GetGUID().ToString());
            return 0;
        }
        ContentKey const key = GetContentKeyProvider().Get(map, owner);
        return RefFlatFor(GetReferenceHealthProvider().Get(key), pct, tank);
    }
}
