/*
 * mod-boss-mechanics: the per-map registry, scheduler and ledger (STAGE-M1-HANDOFF §3.3-3.6).
 *
 * Reentrancy: Tick runs callbacks (onHit, onResolve, onFailure) and spell casts while iterating the mechanics. A
 * callback may Add (std::map insertion keeps iterators valid), Cancel/Detonate (flags only) or EndEncounter (deferred
 * to the end of the tick while _ticking). Only Tick and FinishEncounter erase, and never inside the iteration.
 */

#include "MechanicRegistry.h"
#include "BossMechanicsConfig.h"
#include "Containers.h"
#include "EngineBridge.h"
#include "GameTime.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MechanicProviders.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "Unit.h"
#include <algorithm>
#include <atomic>
#include <limits>

namespace BossMechanics
{
    namespace
    {
        EngineBridge* sBridge = nullptr;
        std::atomic<bool> sWarnedNoBridge{ false };

        Geo::Vec3 ToVec(Position const& p)
        {
            return { p.GetPositionX(), p.GetPositionY(), p.GetPositionZ() };
        }

        Geo::Pose ToPose(Position const& p)
        {
            return { p.GetPositionX(), p.GetPositionY(), p.GetPositionZ(), p.GetOrientation() };
        }

        ResolveResult BaseResult(ActiveMechanic const& m, uint32 pass)
        {
            ResolveResult r;
            r.mechanicId = m.id;
            r.tag = m.tag;
            r.kind = m.kind;
            r.pass = pass;
            return r;
        }

        void WarnNoBridge()
        {
            if (!sWarnedNoBridge.exchange(true))
                LOG_ERROR("module.bossmechanics", "mod-boss-mechanics: no engine bridge installed; markers and "
                    "resolve casts are skipped");
        }
    }

    ProjectileParams ProjectileParamsOf(ActiveMechanic const& m)
    {
        ProjectileParams p;
        if (!m.projectile)
            return p;
        p.length = m.shape.length;
        p.speed = m.projectile->speed;
        p.halfLength = m.projectile->halfLength > 0.f ? m.projectile->halfLength : m.shape.width * 0.5f;
        p.outAndBack = m.projectile->outAndBack;
        p.pause = m.projectile->returnPause;
        return p;
    }

    void SetEngineBridge(EngineBridge* bridge)
    {
        sBridge = bridge;
    }

    EngineBridge* GetEngineBridge()
    {
        return sBridge;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Names
    // ----------------------------------------------------------------------------------------------------------------
    Family FamilyOf(Kind kind)
    {
        switch (kind)
        {
            case Kind::Stack:
            case Kind::Soak:
            case Kind::Spot:
                return Family::GetIn;
            default:
                return Family::GetOut;
        }
    }

    char const* KindName(Kind kind)
    {
        switch (kind)
        {
            case Kind::Zone: return "Zone";
            case Kind::Frontal: return "Frontal";
            case Kind::Spread: return "Spread";
            case Kind::RunOut: return "RunOut";
            case Kind::Stack: return "Stack";
            case Kind::Soak: return "Soak";
            case Kind::Bait: return "Bait";
            case Kind::HoldStill: return "HoldStill";
            case Kind::Spot: return "Spot";
        }
        return "?";
    }

    char const* ShapeName(ShapeType type)
    {
        switch (type)
        {
            case ShapeType::Circle: return "circle";
            case ShapeType::Donut: return "donut";
            case ShapeType::Cone: return "cone";
            case ShapeType::Line: return "line";
        }
        return "?";
    }

    char const* ColorName(Color color)
    {
        switch (color)
        {
            case Color::Red: return "red";
            case Color::Yellow: return "yellow";
            case Color::Blue: return "blue";
            case Color::Purple: return "purple";
            case Color::Orange: return "orange";
            case Color::Green: return "green";
            case Color::Teal: return "teal";
            case Color::White: return "white";
            case Color::Black: return "black";
        }
        return "?";
    }

    char const* FailureTypeName(FailureEvent::Type type)
    {
        switch (type)
        {
            case FailureEvent::Type::Hit: return "Hit";
            case FailureEvent::Type::MissedSoak: return "MissedSoak";
            case FailureEvent::Type::MissedStack: return "MissedStack";
            case FailureEvent::Type::MissedBait: return "MissedBait";
            case FailureEvent::Type::Moved: return "Moved";
            case FailureEvent::Type::OffSpot: return "OffSpot";
        }
        return "?";
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Small types
    // ----------------------------------------------------------------------------------------------------------------
    Anchor::Anchor(Position const& p) : type(Type::Point), point(p) { }

    Anchor::Anchor(::Unit const* u)
    {
        if (!u)
            return;
        type = Type::Unit;
        unit = u->GetGUID();
        point = u->GetPosition();
    }

    float ResolveContext::SearchRadius(float casterX, float casterY, float casterZ) const
    {
        return ResolveSearchRadius(shape, pose, casterX, casterY, casterZ);
    }

    Frame RoomFrame::ToFrame() const
    {
        Frame f;
        f.x = origin.GetPositionX();
        f.y = origin.GetPositionY();
        f.z = origin.GetPositionZ();
        f.angle = angle;
        f.sizeX = sizeX;
        f.sizeY = sizeY;
        return f;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Registry: storage
    // ----------------------------------------------------------------------------------------------------------------
    Registry* Registry::Get(Map const* map)
    {
        // DataMap looks up by std::string: build the key once, not per call (bots read this every tick).
        static std::string const key(kKey);
        return map ? map->CustomData.Get<Registry>(key) : nullptr;
    }

    Registry& Registry::GetOrCreate(Map* map)
    {
        if (Registry* r = Get(map))
            return *r;
        Registry* r = new Registry(map);
        map->CustomData.Set(kKey, r);
        return *r;
    }

    ActiveMechanic const* Registry::Find(uint32 id) const
    {
        auto it = _mechanics.find(id);
        return it != _mechanics.end() ? &it->second : nullptr;
    }

    ActiveMechanic* Registry::FindMutable(uint32 id)
    {
        auto it = _mechanics.find(id);
        return it != _mechanics.end() ? &it->second : nullptr;
    }

    LaneSet const* Registry::FindLaneSet(uint32 id) const
    {
        auto it = _laneSets.find(id);
        return it != _laneSets.end() ? &it->second : nullptr;
    }

    LaneSet* Registry::FindLaneSetMutable(uint32 id)
    {
        auto it = _laneSets.find(id);
        return it != _laneSets.end() ? &it->second : nullptr;
    }

    Encounter const* Registry::FindEncounter(ObjectGuid owner) const
    {
        auto it = _encounters.find(owner);
        return it != _encounters.end() ? &it->second : nullptr;
    }

    Encounter* Registry::FindEncounterMutable(ObjectGuid owner)
    {
        auto it = _encounters.find(owner);
        return it != _encounters.end() ? &it->second : nullptr;
    }

    LaneSet& Registry::AddLaneSet(ObjectGuid owner, RoomFrame const& frame)
    {
        uint32 const id = _nextLaneSetId++;
        LaneSet& ls = _laneSets[id];
        ls.id = id;
        ls.owner = owner;
        ls.frame = frame;
        BumpRevision();
        return ls;
    }

    Color Registry::DefaultColor(Encounter const& e, Family f) const
    {
        return f == Family::GetIn ? e.getIn : e.getOut;
    }

    Encounter& Registry::BeginEncounter(ObjectGuid owner, EncounterOptions const* opt)
    {
        auto [it, inserted] = _encounters.try_emplace(owner);
        Encounter& e = it->second;
        if (inserted)
        {
            e.owner = owner;
            BumpRevision();
        }
        if (opt)
        {
            e.getOut = opt->getOut;
            e.getIn = opt->getIn;
            e.crossLanes = opt->crossLanes;
            e.cells = opt->cells;
            if (opt->bossStateId)
            {
                e.bossStateId = *opt->bossStateId;
                e.hasBossStateId = true;
            }
        }
        return e;
    }

    ActiveMechanic* Registry::Add(ActiveMechanic m)
    {
        ModuleConfig const& cfg = GetConfig();
        if (!cfg.enable)
            return nullptr;
        if (_mechanics.size() >= cfg.maxActivePerMap)
        {
            LOG_WARN("module.bossmechanics", "Map {} instance {}: {} mechanics active (BossMechanics.MaxActivePerMap); "
                "refusing a {} from {}", _map->GetId(), _map->GetInstanceId(), _mechanics.size(), KindName(m.kind),
                m.owner.ToString());
            return nullptr;
        }
        Encounter& e = BeginEncounter(m.owner);
        if (e.ending)
            return nullptr;

        m.id = _nextId++;
        m.family = FamilyOf(m.kind);
        m.color = DefaultColor(e, m.family);
        m.lastPose = ToPose(m.anchor.point);
        m.lastPose = AnchorPose(m);
        uint32 const id = m.id;
        BumpRevision();
        return &_mechanics.emplace(id, std::move(m)).first->second;
    }

    void Registry::FinalizeTiming(ActiveMechanic& m)
    {
        BumpRevision();
        if (m.mine)
        {
            m.mine->armAt = m.resolveAt;
            m.expireAt = m.mine->lifetime > Milliseconds(0) ? m.resolveAt + m.mine->lifetime : kForever;
        }
        else if (m.projectile)
            m.expireAt = m.resolveAt + ProjectileDuration(ProjectileParamsOf(m));
        else if (m.persist)
        {
            m.persist->until = m.resolveAt + m.persist->duration;
            if (!m.resolved)
                m.persist->nextTick = m.resolveAt;
            m.expireAt = m.persist->until;
        }
        else if (m.kind == Kind::HoldStill)
            m.expireAt = m.resolveAt + m.holdWindow;
        else
            m.expireAt = m.resolveAt;
    }

    void Registry::Commit(uint32 id)
    {
        ActiveMechanic* m = FindMutable(id);
        if (!m || m->cancelled || m->finished)
            return;
        FinalizeTiming(*m);
        if (m->committed)
            return;
        m->committed = true;
        PickTargets(*m);
        EnforceCap(*m);
        if (!m->telegraph)
            return;
        if (EngineBridge* bridge = GetEngineBridge())
            bridge->SpawnMarkers(_map, *m);
        else
            WarnNoBridge();
    }

    void Registry::PickTargets(ActiveMechanic& m)
    {
        if ((m.kind != Kind::Spread && m.kind != Kind::RunOut) || !m.targets.empty() || m.count == 0
            || m.selfTrigger.auraSpell)
            return;
        std::vector<ObjectGuid> pool;
        for (Player* p : CandidatePlayers(m))
            if (IsEligible(m, p))
                pool.push_back(p->GetGUID());
        Acore::Containers::RandomShuffle(pool);
        if (pool.size() > m.count)
            pool.resize(m.count);
        m.targets.insert(pool.begin(), pool.end());
    }

    void Registry::EnforceCap(ActiveMechanic& m)
    {
        if (!m.mine || m.mine->cap == 0)
            return;
        std::vector<std::pair<Milliseconds, uint32>> live;
        for (auto const& [id, other] : _mechanics)
            if (other.mine && other.owner == m.owner && other.mine->capGroup == m.mine->capGroup && !other.cancelled
                && !other.finished && !other.mine->spent)
                live.emplace_back(other.createdAt, id);
        for (uint32 id : CapOverflow(live, m.mine->cap))
        {
            ActiveMechanic* old = FindMutable(id);
            if (!old)
                continue;
            old->fizzled = true;
            old->finished = true;
            BumpRevision();
        }
    }

    void Registry::Cancel(uint32 id)
    {
        if (ActiveMechanic* m = FindMutable(id))
        {
            m->cancelled = true;
            BumpRevision();
        }
    }

    void Registry::Detonate(uint32 id)
    {
        if (ActiveMechanic* m = FindMutable(id))
        {
            m->detonateRequested = true;
            BumpRevision();
        }
    }

    void Registry::Reserve(uint32 id, Reservation r)
    {
        ActiveMechanic* m = FindMutable(id);
        if (!m)
            return;
        for (Reservation& existing : m->reservations)
        {
            if (existing.bot == r.bot)
            {
                existing = r;
                return;
            }
        }
        m->reservations.push_back(r);
    }

    void Registry::EndEncounter(ObjectGuid owner)
    {
        if (_ticking)
        {
            for (auto& [id, m] : _mechanics)
            {
                if (m.owner != owner)
                    continue;
                m.cancelled = true;
                m.onHit = nullptr;
                m.onResolve = nullptr;
            }
            if (Encounter* e = FindEncounterMutable(owner))
            {
                e->ending = true;
                e->onFailure = nullptr;
            }
            _pendingEnds.push_back(owner);
            BumpRevision();
            return;
        }
        FinishEncounter(owner);
    }

    void Registry::FinishEncounter(ObjectGuid owner)
    {
        std::vector<uint32> ids;
        for (auto const& [id, m] : _mechanics)
            if (m.owner == owner)
                ids.push_back(id);
        bool const hadEncounter = _encounters.count(owner) != 0;
        if (!hadEncounter && ids.empty())
            return;
        BumpRevision();

        EngineBridge* bridge = GetEngineBridge();
        for (uint32 id : ids)
        {
            auto it = _mechanics.find(id);
            if (it == _mechanics.end())
                continue;
            if (bridge)
                bridge->DespawnMarkers(_map, it->second);
            _mechanics.erase(it);
        }
        for (auto it = _laneSets.begin(); it != _laneSets.end();)
        {
            if (it->second.owner == owner)
                it = _laneSets.erase(it);
            else
                ++it;
        }
        // Weakened: only the players this encounter stacked, and not one another encounter on the map also stacked
        // (an add's or a test owner's end must not reset the boss fight's penalty).
        GuidSet weakened;
        if (auto it = _encounters.find(owner); it != _encounters.end())
            weakened.swap(it->second.weakenedPlayers);
        _encounters.erase(owner);
        if (!bridge)
            return;
        for (ObjectGuid const& g : weakened)
        {
            bool const heldElsewhere = std::any_of(_encounters.begin(), _encounters.end(), [&g](auto const& kv)
            {
                return kv.second.weakenedPlayers.count(g) != 0;
            });
            if (heldElsewhere)
                continue;
            if (Player* player = ObjectAccessor::GetPlayer(_map, g))
                bridge->ClearWeakened(_map, player);
        }
    }

    void Registry::EndEncountersForBossState(uint32 bossStateId)
    {
        std::vector<ObjectGuid> owners;
        for (auto const& [owner, e] : _encounters)
            if (e.hasBossStateId && e.bossStateId == bossStateId)
                owners.push_back(owner);
        for (ObjectGuid const& owner : owners)
            EndEncounter(owner);
    }

    void Registry::DeferEndEncountersForBossState(uint32 bossStateId)
    {
        bool const bound = std::any_of(_encounters.begin(), _encounters.end(), [bossStateId](auto const& kv)
        {
            return kv.second.hasBossStateId && kv.second.bossStateId == bossStateId;
        });
        if (bound && std::find(_deferredBossStates.begin(), _deferredBossStates.end(), bossStateId)
            == _deferredBossStates.end())
            _deferredBossStates.push_back(bossStateId);
    }

    void Registry::DeferEndEncounter(ObjectGuid owner)
    {
        if (!_encounters.count(owner))
            return;
        if (std::find(_deferredEnds.begin(), _deferredEnds.end(), owner) == _deferredEnds.end())
            _deferredEnds.push_back(owner);
    }

    void Registry::ClearAll()
    {
        GuidSet owners;
        for (auto const& [owner, e] : _encounters)
            owners.insert(owner);
        for (auto const& [id, m] : _mechanics)
            owners.insert(m.owner);
        for (ObjectGuid const& owner : owners)
            EndEncounter(owner);
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Registry: failures
    // ----------------------------------------------------------------------------------------------------------------
    void Registry::PostFailure(FailureEvent const& e)
    {
        Encounter* enc = FindEncounterMutable(e.encounter);
        if (!enc)
            return;
        if (!e.player.IsEmpty())
        {
            PlayerLedger& l = enc->ledger[e.player];
            ++l.total;
            ++l.byTag[e.tag];
        }
        else
            ++enc->groupFailures;

        enc->events.push_back(e);
        if (enc->events.size() > Rules::kMaxEventsPerEncounter)
            enc->events.erase(enc->events.begin());

        if (GetConfig().debug)
            LOG_INFO("module.bossmechanics", "Failure {} mechanic {} (tag {}, {}) player {} damage {}",
                FailureTypeName(e.type), e.mechanicId, e.tag, KindName(e.kind), e.player.ToString(), e.damage);

        if (enc->onFailure && FindUnit(e.encounter))
        {
            auto const fn = enc->onFailure;     // the callback may end the encounter
            fn(e);
        }
    }

    void Registry::PlayerFailure(ActiveMechanic& m, ObjectGuid player, FailureEvent::Type type, uint32 damage)
    {
        FailureEvent e;
        e.encounter = m.owner;
        e.mechanicId = m.id;
        e.tag = m.tag;
        e.kind = m.kind;
        e.player = player;
        e.type = type;
        e.damage = damage;
        e.at = GameTime::GetGameTimeMS();
        PostFailure(e);
        if (Rules::kWeakenedOnNonHitFailures && type != FailureEvent::Type::Hit && m.weakened)
            if (Player* p = ObjectAccessor::GetPlayer(_map, player))
                ApplyWeakened(m, p);
    }

    void Registry::ApplyWeakened(ActiveMechanic& m, Player* player)
    {
        if (!m.weakened || !player || m.kind == Kind::Soak)
            return;
        if (m.weakenedOncePerPlayer && !m.weakenedApplied.insert(player->GetGUID()).second)
            return;
        if (!m.weakenedOncePerPlayer)
            m.weakenedApplied.insert(player->GetGUID());
        if (Encounter* e = FindEncounterMutable(m.owner))
            e->weakenedPlayers.insert(player->GetGUID());
        if (EngineBridge* bridge = GetEngineBridge())
            bridge->AddWeakenedStack(player);
        else
            WarnNoBridge();
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Registry: evaluation
    // ----------------------------------------------------------------------------------------------------------------
    Unit* Registry::FindUnit(ObjectGuid guid) const
    {
        if (guid.IsEmpty())
            return nullptr;
        if (guid.IsPlayer())
            return ObjectAccessor::GetPlayer(_map, guid);
        if (guid.IsPet())
            return _map->GetPet(guid);
        if (guid.IsCreatureOrVehicle())
            return _map->GetCreature(guid);
        return nullptr;
    }

    bool Registry::OwnerPresent(ActiveMechanic const& m) const
    {
        return FindUnit(m.owner) != nullptr;
    }

    std::vector<Player*> Registry::CandidatePlayers(ActiveMechanic const& m) const
    {
        // Outside instances the map is a whole continent: only players near the owner (the anchor's last pose when
        // the owner is gone) take part, so an open-world mechanic or `.mech test` never reaches the whole continent.
        float const radius = _map->Instanceable() ? 0.f : GetConfig().openWorldCandidateRadius;
        Geo::Pose centre = m.lastPose;
        if (radius > 0.f)
            if (Unit const* owner = FindUnit(m.owner))
                centre = ToPose(owner->GetPosition());

        std::vector<Player*> out;
        for (MapReference const& ref : _map->GetPlayers())
        {
            Player* player = ref.GetSource();
            if (!player || !player->IsInWorld() || !player->IsAlive() || player->IsGameMaster())
                continue;
            if (radius > 0.f && player->GetExactDist2d(centre.x, centre.y) > radius)
                continue;
            if (!IsInTestScope(m, player))
                continue;
            out.push_back(player);
        }
        return out;
    }

    bool Registry::IsInTestScope(ActiveMechanic const& m, Player const* player) const
    {
        if (!m.owner.IsCreature() || m.owner.GetEntry() != Ids::NPC_MECHANIC_TEST_OWNER)
            return true;
        Encounter const* e = FindEncounter(m.owner);
        if (!e)
            return false;
        for (ObjectGuid const& g : e->testGms)
        {
            if (g == player->GetGUID())
                return true;
            if (Player const* gm = ObjectAccessor::GetPlayer(_map, g))
                if (gm->IsInSameRaidWith(player))
                    return true;
        }
        return false;
    }

    Geo::Pose Registry::AnchorPose(ActiveMechanic const& m) const
    {
        if (!m.anchor.IsUnit())
            return ToPose(m.anchor.point);
        Unit const* u = FindUnit(m.anchor.unit);
        if (!u)
            return m.lastPose;
        Geo::Pose p = ToPose(u->GetPosition());
        if (!m.anchor.followFacing)
            p.o = m.anchor.point.GetOrientation();
        return p;
    }

    Shape Registry::EffectiveShape(ActiveMechanic const& m, Geo::Pose& pose) const
    {
        pose = AnchorPose(m);
        if (m.to)
        {
            Geo::Pose to = ToPose(m.to->point);
            if (m.to->IsUnit())
                if (Unit const* u = FindUnit(m.to->unit))
                    to = ToPose(u->GetPosition());
            if (Geo::Dist2d(pose.x, pose.y, to.x, to.y) > 0.01f)
                pose.o = std::atan2(to.y - pose.y, to.x - pose.x);
        }
        return m.shape;
    }

    std::vector<ObjectGuid> Registry::Carriers(ActiveMechanic const& m) const
    {
        std::vector<ObjectGuid> out;
        if (m.selfTrigger.auraSpell)
        {
            for (Player* p : CandidatePlayers(m))
                if (IsEligible(m, p) && p->GetAuraCount(m.selfTrigger.auraSpell) >= m.selfTrigger.minStacks)
                    out.push_back(p->GetGUID());
            return out;
        }
        if (!m.targets.empty())
            return { m.targets.begin(), m.targets.end() };
        if (m.kind == Kind::Spread && m.count == 0)
            for (Player* p : CandidatePlayers(m))
                if (IsEligible(m, p))
                    out.push_back(p->GetGUID());
        return out;
    }

    bool Registry::ContainsPoint(ActiveMechanic const& m, Position const& p, ObjectGuid who) const
    {
        Geo::Vec3 const v = ToVec(p);
        switch (m.kind)
        {
            case Kind::Zone:
            case Kind::Frontal:
            case Kind::Stack:
            case Kind::Soak:
            {
                Geo::Pose pose;
                Shape const shape = EffectiveShape(m, pose);
                return Geo::Contains(shape, pose, v);
            }
            case Kind::Spread:
            case Kind::RunOut:
            {
                Shape circle = Shape::Circle(m.shape.radius);
                circle.zTolerance = m.shape.zTolerance;
                if (m.kind == Kind::RunOut && m.measuredFrom == MeasuredFrom::Owner)
                {
                    Unit const* owner = FindUnit(m.owner);
                    return owner && Geo::Contains(circle, ToPose(owner->GetPosition()), v);
                }
                for (ObjectGuid const& c : Carriers(m))
                {
                    if (c == who)
                        continue;
                    if (Unit const* cu = FindUnit(c))
                        if (Geo::Contains(circle, ToPose(cu->GetPosition()), v))
                            return true;
                }
                return false;
            }
            case Kind::Spot:
            {
                auto it = m.spots.find(who);
                if (it == m.spots.end())
                    return false;
                Shape circle = Shape::Circle(m.shape.radius);
                circle.zTolerance = m.shape.zTolerance;
                return Geo::Contains(circle, ToPose(it->second), v);
            }
            case Kind::Bait:
            case Kind::HoldStill:
                return false;
        }
        return false;
    }

    bool Registry::ContainsUnit(ActiveMechanic const& m, Unit const* who) const
    {
        return who && ContainsPoint(m, who->GetPosition(), who->GetGUID());
    }

    std::vector<HotWindow> Registry::HotWindows(ActiveMechanic const& m, Position const& p) const
    {
        std::vector<HotWindow> out;
        if (m.IsShapeKind() && !ContainsPoint(m, p))
            return out;
        if (m.projectile)
        {
            Geo::Pose pose;
            Shape const shape = EffectiveShape(m, pose);
            float const s = Geo::ToLineCoords(shape, pose, ToVec(p)).s;
            out = ProjectileHotWindows(ProjectileParamsOf(m), s);
            for (HotWindow& w : out)
            {
                w.from += m.resolveAt;
                w.to += m.resolveAt;
            }
            return out;
        }
        if (m.mine)
        {
            if (m.mine->armed)
                out.push_back({ m.mine->armAt, m.expireAt, 0, true });
            else
                out.push_back({ m.mine->armAt, m.mine->armAt, 0, false });
            return out;
        }
        if (m.persist)
        {
            out.push_back({ m.resolveAt, m.persist->until, 0, false });
            return out;
        }
        if (m.kind == Kind::HoldStill)
        {
            out.push_back({ m.resolveAt, m.expireAt, 0, false });
            return out;
        }
        out.push_back({ m.resolveAt, m.resolveAt, 0, false });
        return out;
    }

    Role Registry::RoleOf(ActiveMechanic const& m, Player const* player) const
    {
        return GetRoleProvider().Get(FindUnit(m.owner), player);
    }

    bool Registry::IsEligible(ActiveMechanic const& m, Unit const* who) const
    {
        if (!who)
            return false;
        Player const* p = who->GetCharmerOrOwnerPlayerOrPlayerItself();     // pets follow their owner
        if (!p || !IsInTestScope(m, p))
            return false;
        Eligibility const& e = m.eligibility;
        if (e.roles != Role::Any && !HasRole(e.roles, RoleOf(m, p)))
            return false;
        if (!e.carriers.empty() && !e.carriers.count(p->GetGUID()))
            return false;
        if (e.auraSpell)
        {
            uint32 const stacks = p->GetAuraCount(e.auraSpell);
            if (stacks < e.minStacks || stacks > e.maxStacks)
                return false;
        }
        return true;
    }

    bool Registry::IsTankExempted(ActiveMechanic const& m, Unit const* who) const
    {
        if (!m.tankExempt || !who)
            return false;
        Unit const* owner = FindUnit(m.owner);
        Unit const* victim = owner ? owner->GetVictim() : nullptr;
        return victim && victim->GetGUID() == who->GetGUID();
    }

    ObjectGuid Registry::BaitPick(ActiveMechanic const& m) const
    {
        Unit const* owner = FindUnit(m.owner);
        Geo::Pose const from = owner ? ToPose(owner->GetPosition()) : m.lastPose;
        ObjectGuid best;
        float bestDist = -1.f;
        uint32 bestStacks = std::numeric_limits<uint32>::max();
        for (Player* p : CandidatePlayers(m))
        {
            if (!IsEligible(m, p))
                continue;
            ObjectGuid const g = p->GetGUID();
            if (m.bait.type == BaitRule::Type::Farthest)
            {
                float const d = Geo::Dist2d(from.x, from.y, p->GetPositionX(), p->GetPositionY());
                if (d > bestDist || (d == bestDist && g < best))
                {
                    bestDist = d;
                    best = g;
                }
                continue;
            }
            if (RoleOf(m, p) == Role::Tank)
                continue;
            uint32 const stacks = m.bait.auraSpell ? p->GetAuraCount(m.bait.auraSpell) : 0;
            if (best.IsEmpty() || stacks < bestStacks || (stacks == bestStacks && g < best))
            {
                bestStacks = stacks;
                best = g;
            }
        }
        return best;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Registry: resolve spell callbacks
    // ----------------------------------------------------------------------------------------------------------------
    bool Registry::InsideCtx(ActiveMechanic const& m, Unit const* u) const
    {
        ResolveContext const& ctx = m.ctx;
        ObjectGuid const g = u->GetGUID();
        if (!ctx.only.IsEmpty() && g != ctx.only)
            return false;
        if (!ctx.exclude.IsEmpty() && g == ctx.exclude)
            return false;
        Geo::Vec3 const p = ToVec(u->GetPosition());
        if (!Geo::Contains(ctx.shape, ctx.pose, p))
            return false;
        if (ctx.swept)
        {
            float const s = Geo::ToLineCoords(ctx.shape, ctx.pose, p).s;
            return s >= ctx.sweepLo && s <= ctx.sweepHi;
        }
        return true;
    }

    void Registry::FilterResolveTargets(uint32 id, std::list<WorldObject*>& targets)
    {
        ActiveMechanic* m = FindMutable(id);
        if (!m || !m->ctx.active || m->cancelled)
        {
            targets.clear();
            return;
        }
        GuidSet& done = m->hitPerPass[m->ctx.pass];
        targets.remove_if([&](WorldObject* obj)
        {
            Unit* u = obj ? obj->ToUnit() : nullptr;
            if (!u || !u->IsAlive() || !u->GetCharmerOrOwnerPlayerOrPlayerItself())
                return true;    // player-controlled units only (players, pets, guardians)
            if (Player* pl = u->ToPlayer())
                if (pl->IsGameMaster())
                    return true;
            if (!InsideCtx(*m, u) || !IsEligible(*m, u) || IsTankExempted(*m, u))
                return true;
            if (!done.insert(u->GetGUID()).second)
                return true;    // already hit this pass
            m->ctx.selected.push_back(u->GetGUID());
            return false;
        });
    }

    int32 Registry::ResolveDamageFor(uint32 id, Unit const* target) const
    {
        ActiveMechanic const* m = Find(id);
        if (!m || !target)
            return 0;
        Unit const* owner = FindUnit(m->owner);
        ContentKey const key = GetContentKeyProvider().Get(_map, owner);
        ReferenceHealth const ref = GetReferenceHealthProvider().Get(key);
        bool isTank = false;
        if (Player const* p = target->ToPlayer())
            isTank = RoleOf(*m, p) == Role::Tank;
        return FlatDamageFor(m->damage, ref, isTank);
    }

    void Registry::OnResolveHit(uint32 id, Unit* target, uint32 damage)
    {
        ActiveMechanic* m = FindMutable(id);
        // Player-controlled units only (the filter's contract): a hit on anything else (a reflected copy landing on
        // the marker, or any future path past the filter) never reaches the ledger, Weakened or onHit.
        if (!m || !target || !target->GetCharmerOrOwnerPlayerOrPlayerItself())
            return;
        if (Player* p = target->ToPlayer())
        {
            PlayerFailure(*m, p->GetGUID(), FailureEvent::Type::Hit, damage);
            ApplyWeakened(*m, p);
        }
        if (m->onHit && OwnerPresent(*m))
        {
            auto const fn = m->onHit;   // the callback may end the encounter (clears m->onHit)
            fn(target);
        }
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Registry: scheduler
    // ----------------------------------------------------------------------------------------------------------------
    bool Registry::CastsSpell(ActiveMechanic const& m) const
    {
        return m.damage.HasDamage() || m.damage.riderSpell || m.weakened || m.onHit;
    }

    std::vector<ObjectGuid> Registry::ResolvePass(ActiveMechanic& m, uint32 pass, Geo::Pose const& pose,
        Shape const& shape, ObjectGuid exclude, ObjectGuid only, std::optional<SweepBand> band)
    {
        uint32 const id = m.id;
        m.ctx = ResolveContext();
        m.ctx.active = true;
        m.ctx.pass = pass;
        m.ctx.pose = pose;
        m.ctx.shape = shape;
        m.ctx.exclude = exclude;
        m.ctx.only = only;
        m.ctx.carrier = exclude.IsEmpty() ? only : exclude;
        if (band)
        {
            m.ctx.swept = true;
            m.ctx.sweepLo = band->lo;
            m.ctx.sweepHi = band->hi;
        }

        std::vector<ObjectGuid> hits;
        if (CastsSpell(m))
        {
            if (EngineBridge* bridge = GetEngineBridge())
                bridge->CastResolve(_map, m, pass);
            else
                WarnNoBridge();
            // The cast may have run callbacks that added mechanics; `m` stays valid (std::map), re-find anyway.
            if (ActiveMechanic* again = FindMutable(id))
                hits = std::move(again->ctx.selected);
        }
        else
        {
            // Telegraph-only (no damage, rider, Weakened or onHit): nobody is hit and nothing reaches the ledger;
            // the result still lists who stood inside.
            GuidSet& done = m.hitPerPass[pass];
            for (Player* p : CandidatePlayers(m))
                if (InsideCtx(m, p) && IsEligible(m, p) && !IsTankExempted(m, p) && done.insert(p->GetGUID()).second)
                    hits.push_back(p->GetGUID());
        }

        if (ActiveMechanic* again = FindMutable(id))
            again->ctx = ResolveContext();

        if (GetConfig().debug)
            LOG_INFO("module.bossmechanics", "Resolve mechanic {} ({} {}) pass {}: {} hit", id, KindName(m.kind),
                ShapeName(shape.type), pass, hits.size());
        return hits;
    }

    void Registry::Report(ActiveMechanic& m, ResolveResult const& r)
    {
        if (!m.onResolve || !OwnerPresent(m))
            return;
        auto const fn = m.onResolve;    // the callback may end the encounter (clears m.onResolve)
        fn(r);
    }

    void Registry::Tick(Milliseconds now)
    {
        if (_ticking)
            return;

        if (!_deferredEnds.empty())
        {
            std::vector<ObjectGuid> owners;
            owners.swap(_deferredEnds);
            for (ObjectGuid const& owner : owners)
                FinishEncounter(owner);
        }

        if (!_deferredBossStates.empty())
        {
            std::vector<uint32> states;
            states.swap(_deferredBossStates);
            InstanceMap* instance = _map->ToInstanceMap();
            InstanceScript const* script = instance ? instance->GetInstanceScript() : nullptr;
            for (uint32 bossStateId : states)
            {
                // The hook runs before the state is committed and the instance may refuse it (DONE while a minion
                // lives): only end when the state really is NOT_STARTED / DONE / FAIL now.
                EncounterState const state = script ? script->GetBossState(bossStateId) : DONE;
                if (state == NOT_STARTED || state == DONE || state == FAIL)
                    EndEncountersForBossState(bossStateId);
                else if (GetConfig().debug)
                    LOG_INFO("module.bossmechanics", "Boss state {} change refused (now {}); encounters kept",
                        bossStateId, static_cast<uint32>(state));
            }
        }

        _ticking = true;
        EngineBridge* bridge = GetEngineBridge();

        for (auto& [id, m] : _mechanics)
        {
            if (m.cancelled || m.finished)
                continue;
            if (!m.committed)
                Commit(id);
            if (m.cancelled || m.finished)
                continue;
            m.lastPose = AnchorPose(m);
            Step(m, now);
            if (!m.cancelled && !m.finished && bridge)
                bridge->UpdateMarkers(_map, m, now);
        }

        std::vector<uint32> dead;
        for (auto const& [id, m] : _mechanics)
            if (m.cancelled || m.finished)
                dead.push_back(id);
        if (!dead.empty())
            BumpRevision();
        for (uint32 id : dead)
        {
            auto it = _mechanics.find(id);
            if (it == _mechanics.end())
                continue;
            Remove(it->second);
            _mechanics.erase(id);
        }

        for (auto& [id, m] : _mechanics)
            std::erase_if(m.reservations, [now](Reservation const& r) { return r.until <= now; });

        for (auto it = _laneSets.begin(); it != _laneSets.end();)
        {
            LaneSet const& ls = it->second;
            bool const alive = std::any_of(ls.lanes.begin(), ls.lanes.end(), [this](uint32 id)
            {
                return _mechanics.count(id) != 0;
            }) || std::any_of(ls.cells.begin(), ls.cells.end(), [this](uint32 id)
            {
                return _mechanics.count(id) != 0;
            });
            if (alive)
                ++it;
            else
            {
                it = _laneSets.erase(it);
                BumpRevision();
            }
        }

        _ticking = false;
        while (!_pendingEnds.empty())
        {
            std::vector<ObjectGuid> owners;
            owners.swap(_pendingEnds);
            for (ObjectGuid const& owner : owners)
                FinishEncounter(owner);
        }
    }

    void Registry::Remove(ActiveMechanic& m)
    {
        if (EngineBridge* bridge = GetEngineBridge())
            bridge->DespawnMarkers(_map, m);
        if (m.fizzled && !m.cancelled)
        {
            ResolveResult r = BaseResult(m, 0);
            r.fizzled = true;
            Report(m, r);
        }
    }

    void Registry::Step(ActiveMechanic& m, Milliseconds now)
    {
        switch (m.kind)
        {
            case Kind::Zone:
            case Kind::Frontal:
                StepShape(m, now);
                break;
            case Kind::Spread:
            case Kind::RunOut:
                StepCarriers(m, now);
                break;
            case Kind::Stack:
            case Kind::Soak:
                StepStackSoak(m, now);
                break;
            case Kind::Bait:
                StepBait(m, now);
                break;
            case Kind::HoldStill:
                StepHoldStill(m, now);
                break;
            case Kind::Spot:
                StepSpot(m, now);
                break;
        }
    }

    void Registry::StepShape(ActiveMechanic& m, Milliseconds now)
    {
        if (m.mine)
            return StepMine(m, now);
        if (m.projectile)
            return StepProjectile(m, now);
        if (m.persist)
            return StepPersistent(m, now);
        if (now < m.resolveAt && !m.detonateRequested)
            return;
        m.detonateRequested = false;
        Geo::Pose pose;
        Shape const shape = EffectiveShape(m, pose);
        ResolveResult r = BaseResult(m, 0);
        r.hits = ResolvePass(m, 0, pose, shape);
        r.count = r.hits.size();
        m.resolved = true;
        m.finished = true;
        Report(m, r);
    }

    void Registry::StepPersistent(ActiveMechanic& m, Milliseconds now)
    {
        Persist& p = *m.persist;
        if (m.detonateRequested)
        {
            m.detonateRequested = false;
            if (!m.resolved && now < p.nextTick)
                p.nextTick = now;
        }
        if (now >= p.nextTick && (p.nextTick < p.until || p.ticks == 0))
        {
            uint32 const pass = p.ticks++;
            p.nextTick = p.tick > Milliseconds(0) ? p.nextTick + p.tick : kForever;
            m.resolved = true;
            Geo::Pose pose;
            Shape const shape = EffectiveShape(m, pose);
            ResolveResult r = BaseResult(m, pass);
            r.hits = ResolvePass(m, pass, pose, shape);
            r.count = r.hits.size();
            Report(m, r);
        }
        if (m.resolved && now >= m.persist->until)
            m.finished = true;
    }

    void Registry::StepProjectile(ActiveMechanic& m, Milliseconds now)
    {
        if (m.detonateRequested)
        {
            m.detonateRequested = false;
            LOG_WARN("module.bossmechanics", "Detonate on projectile mechanic {} ignored", m.id);
        }
        if (now < m.resolveAt)
            return;

        Projectile& pr = *m.projectile;
        ProjectileParams const params = ProjectileParamsOf(m);
        Milliseconds const elapsed = now - m.resolveAt;
        Geo::Pose pose;
        Shape const shape = EffectiveShape(m, pose);
        std::vector<SweepBand> bands;
        bool done = false;
        uint32 passesDone = 0;

        if (!pr.unit.IsEmpty())
        {
            if (Unit const* u = FindUnit(pr.unit))
            {
                float const s = Geo::ToLineCoords(shape, pose, ToVec(u->GetPosition())).s;
                if (!pr.trackedStarted)
                {
                    pr.lastTrackedS = s;
                    pr.trackedStarted = true;
                }
                bands = TrackedSweep(pr.lastTrackedS, s, params.halfLength, params.outAndBack, pr.trackedPass);
                pr.lastTrackedS = s;
            }
            else
                done = true;
            if (now >= m.expireAt)
                done = true;
            passesDone = pr.trackedPass;
        }
        else
        {
            bands = SweptBands(params, pr.lastElapsed, elapsed);
            pr.lastElapsed = elapsed;
            Milliseconds const duration = ProjectileDuration(params);
            if (params.outAndBack && elapsed >= MsOf(ProjectileReturnStart(params)))
                passesDone = 1;
            if (elapsed >= duration)
                done = true;
        }

        for (SweepBand const& band : bands)
            ResolvePass(m, band.pass, pose, shape, ObjectGuid::Empty, ObjectGuid::Empty, band);
        m.resolved = true;

        uint32 const passesTotal = params.outAndBack ? 2 : 1;
        if (done)
            passesDone = passesTotal;
        while (pr.passesReported < passesDone)
        {
            uint32 const pass = pr.passesReported++;
            ResolveResult r = BaseResult(m, pass);
            GuidSet const& hit = m.hitPerPass[pass];
            r.hits.assign(hit.begin(), hit.end());
            r.count = r.hits.size();
            Report(m, r);
        }
        if (done)
            m.finished = true;
    }

    void Registry::StepMine(ActiveMechanic& m, Milliseconds now)
    {
        MineState& mine = *m.mine;
        if (now >= m.expireAt)
        {
            m.fizzled = true;
            m.finished = true;
            return;
        }
        Geo::Pose pose;
        Shape const shape = EffectiveShape(m, pose);
        bool detonate = false;
        if (m.detonateRequested)
        {
            m.detonateRequested = false;
            mine.armed = true;
            mine.spent = true;
            detonate = true;
        }
        else if (now >= mine.armAt)
        {
            std::vector<ObjectGuid> inside;
            for (Player* p : CandidatePlayers(m))
                if (IsEligible(m, p) && Geo::Contains(shape, pose, ToVec(p->GetPosition())))
                    inside.push_back(p->GetGUID());
            bool const wasArmed = mine.armed;
            detonate = mine.Update(now, inside) == MineState::Step::Detonate;
            if (!wasArmed && mine.armed)
                BumpRevision();     // bots: hot at the arming moment only -> hot until it expires
        }
        if (!detonate)
            return;
        ResolveResult r = BaseResult(m, 0);
        r.hits = ResolvePass(m, 0, pose, shape);
        r.count = r.hits.size();
        m.resolved = true;
        m.finished = true;
        Report(m, r);
    }

    void Registry::StepCarriers(ActiveMechanic& m, Milliseconds now)
    {
        if (now < m.resolveAt && !m.detonateRequested)
            return;
        m.detonateRequested = false;
        ResolveResult r = BaseResult(m, 0);
        Unit const* owner = FindUnit(m.owner);
        Shape circle = Shape::Circle(m.shape.radius);
        circle.zTolerance = m.shape.zTolerance;
        uint32 pass = 0;
        for (ObjectGuid const& c : Carriers(m))
        {
            uint32 const thisPass = pass++;
            Unit const* cu = FindUnit(c);
            if (!cu || !cu->IsAlive())
                continue;
            Geo::Pose const cp = ToPose(cu->GetPosition());
            std::vector<ObjectGuid> hits;
            if (m.kind == Kind::RunOut && m.measuredFrom == MeasuredFrom::Owner)
            {
                // L15: the carrier fails (is hit) when it is closer than r to the owner.
                if (owner && Geo::Contains(circle, ToPose(owner->GetPosition()), ToVec(cu->GetPosition())))
                    hits = ResolvePass(m, thisPass, cp, circle, ObjectGuid::Empty, c);
            }
            else
                hits = ResolvePass(m, thisPass, cp, circle, c);
            r.hits.insert(r.hits.end(), hits.begin(), hits.end());
        }
        r.count = r.hits.size();
        m.resolved = true;
        m.finished = true;
        Report(m, r);
    }

    void Registry::StepStackSoak(ActiveMechanic& m, Milliseconds now)
    {
        if (now < m.resolveAt && !m.detonateRequested)
            return;
        m.detonateRequested = false;
        Geo::Pose pose;
        Shape const shape = EffectiveShape(m, pose);
        ResolveResult r = BaseResult(m, 0);
        std::vector<ObjectGuid> missed;
        for (Player* p : CandidatePlayers(m))
        {
            if (!IsEligible(m, p))
                continue;
            if (Geo::Contains(shape, pose, ToVec(p->GetPosition())))
                r.hits.push_back(p->GetGUID());
            else if (m.kind == Kind::Stack)
                missed.push_back(p->GetGUID());
        }
        r.count = r.hits.size();
        m.resolved = true;
        m.finished = true;
        for (ObjectGuid const& g : missed)
            PlayerFailure(m, g, FailureEvent::Type::MissedStack);
        if (m.kind == Kind::Soak && r.count < m.soakCount)
        {
            FailureEvent e;
            e.encounter = m.owner;
            e.mechanicId = m.id;
            e.tag = m.tag;
            e.kind = m.kind;
            e.type = FailureEvent::Type::MissedSoak;
            e.at = now;
            PostFailure(e);
        }
        Report(m, r);
    }

    void Registry::StepBait(ActiveMechanic& m, Milliseconds now)
    {
        if (now < m.resolveAt && !m.detonateRequested)
            return;
        m.detonateRequested = false;
        Unit const* owner = FindUnit(m.owner);
        Geo::Pose const from = owner ? ToPose(owner->GetPosition()) : m.lastPose;
        ObjectGuid const pick = BaitPick(m);

        Player* farthest = nullptr;
        float farDist = -1.f;
        std::vector<std::pair<ObjectGuid, float>> dists;
        for (Player* p : CandidatePlayers(m))
        {
            if (!IsEligible(m, p))
                continue;
            float const d = Geo::Dist2d(from.x, from.y, p->GetPositionX(), p->GetPositionY());
            dists.emplace_back(p->GetGUID(), d);
            if (d > farDist || (d == farDist && farthest && p->GetGUID() < farthest->GetGUID()))
            {
                farDist = d;
                farthest = p;
            }
        }

        ResolveResult r = BaseResult(m, 0);
        m.resolved = true;
        m.finished = true;
        if (farthest)
        {
            ObjectGuid const target = farthest->GetGUID();
            r.picked = target;
            Geo::Pose const tp = ToPose(farthest->GetPosition());
            Shape circle = Shape::Circle(std::max(m.shape.radius, 0.5f));
            circle.zTolerance = m.shape.zTolerance;
            r.hits = ResolvePass(m, 0, tp, circle, ObjectGuid::Empty,
                m.shape.radius > 0.f ? ObjectGuid::Empty : target);
            for (auto const& [g, d] : dists)
                if (g != target && d > m.othersRange)
                    PlayerFailure(m, g, FailureEvent::Type::MissedBait);
            if (!pick.IsEmpty() && pick != target)
                PlayerFailure(m, target, FailureEvent::Type::MissedBait);
        }
        r.count = r.hits.size();
        Report(m, r);
    }

    void Registry::StepHoldStill(ActiveMechanic& m, Milliseconds now)
    {
        if (now < m.resolveAt)
            return;
        if (!m.resolved)
        {
            m.resolved = true;
            if (m.targets.empty())
            {
                for (Player* p : CandidatePlayers(m))
                    if (IsEligible(m, p))
                        m.holdStart[p->GetGUID()] = p->GetPosition();
            }
            else
            {
                for (ObjectGuid const& g : m.targets)
                    if (Unit const* u = FindUnit(g))
                        m.holdStart[g] = u->GetPosition();
            }
        }
        for (auto const& [g, start] : m.holdStart)
        {
            if (m.failedPlayers.count(g))
                continue;
            Unit const* u = FindUnit(g);
            if (!u || !u->IsAlive())
                continue;
            if (u->GetExactDist2d(start) > Rules::kHoldStillToleranceYd)
            {
                m.failedPlayers.insert(g);
                PlayerFailure(m, g, FailureEvent::Type::Moved);
            }
        }
        if (now < m.expireAt)
            return;
        m.finished = true;
        ResolveResult r = BaseResult(m, 0);
        r.hits.assign(m.failedPlayers.begin(), m.failedPlayers.end());
        r.count = r.hits.size();
        Report(m, r);
    }

    void Registry::StepSpot(ActiveMechanic& m, Milliseconds now)
    {
        if (now < m.resolveAt && !m.detonateRequested)
            return;
        m.detonateRequested = false;
        m.resolved = true;
        m.finished = true;
        ResolveResult r = BaseResult(m, 0);
        Shape circle = Shape::Circle(m.shape.radius);
        circle.zTolerance = m.shape.zTolerance;
        std::vector<ObjectGuid> off;
        for (auto const& [g, spot] : m.spots)
        {
            Unit const* u = FindUnit(g);
            if (!u || !u->IsAlive())
                continue;
            if (Geo::Contains(circle, ToPose(spot), ToVec(u->GetPosition())))
                r.hits.push_back(g);
            else
                off.push_back(g);
        }
        for (ObjectGuid const& g : off)
            PlayerFailure(m, g, FailureEvent::Type::OffSpot);
        r.count = r.hits.size();
        Report(m, r);
    }
}
