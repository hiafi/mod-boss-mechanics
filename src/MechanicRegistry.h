/*
 * mod-boss-mechanics: data model and the per-map registry (STAGE-M1-HANDOFF §3.3, §3.5, §3.6).
 *
 * Threading (§2.1): one registry per map, stored in Map::CustomData and touched only on that map's update thread
 * (boss scripts, creature AI, bots, spells, map hooks) or on the world thread while no map updates (`.mech`). No locks.
 * Units are stored as ObjectGuid and resolved at use time; nothing here keeps a Unit/Player/Creature pointer.
 *
 * Writers: scripts through the Mechanic:: API (BossMechanics.h), the C2 hooks and `.mech`.
 * Readers: bots (M2/M3) through Registry const& and the evaluation helpers, which are the same code the resolve
 * filter runs.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_REGISTRY_H
#define MOD_BOSS_MECHANICS_MECHANIC_REGISTRY_H

#include "DataMap.h"
#include "Define.h"
#include "Duration.h"
#include "MechanicGeometry.h"
#include "MechanicJudge.h"
#include "MechanicMath.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

class Map;
class Player;
class Unit;
class WorldObject;

namespace BossMechanics
{
    // ----------------------------------------------------------------------------------------------------------------
    // Enums
    // ----------------------------------------------------------------------------------------------------------------
    // Kind lives in MechanicJudge.h (the pure judging rules the host tests compile).

    // Telegraph colors: the index is the marker display's color block (Ids::MarkerDisplay), so the order is the asset
    // script's COLORS. Every marker shape exists in every color.
    enum class Color : uint8
    {
        Red,
        Yellow,
        Blue,
        Purple,
        Orange,
        Green,
        Teal,
        White,      // reads poorly on snow
        Black       // darkens the floor: reads poorly on dark floors
    };
    static_assert(static_cast<uint8>(Color::Black) + 1 == Ids::COLOR_COUNT, "Color must match the marker palette");

    enum class Family : uint8
    {
        GetOut,     // Zone, Frontal, Spread, RunOut, Bait, HoldStill
        GetIn       // Stack, Soak, Spot
    };

    enum class MeasuredFrom : uint8
    {
        Group,      // a RunOut keeps its distance from the other players
        Owner       // a RunOut keeps its distance from the owner (L15)
    };

    enum class Role : uint8
    {
        None = 0,
        Tank = 1,
        Healer = 2,
        Damage = 4,
        Any = 7
    };

    constexpr Role operator|(Role a, Role b)
    {
        return static_cast<Role>(static_cast<uint8>(a) | static_cast<uint8>(b));
    }

    constexpr bool HasRole(Role set, Role r)
    {
        return (static_cast<uint8>(set) & static_cast<uint8>(r)) != 0;
    }

    enum class TankExempt : uint8
    {
        No,
        Yes
    };

    Family FamilyOf(Kind kind);
    char const* KindName(Kind kind);
    char const* ShapeName(ShapeType type);
    char const* ColorName(Color color);

    // ----------------------------------------------------------------------------------------------------------------
    // Anchors and per-kind state
    // ----------------------------------------------------------------------------------------------------------------

    // A point, or any unit (moving ones included). The live pose is re-read on every evaluation.
    struct Anchor
    {
        enum class Type : uint8
        {
            Point,
            Unit
        };

        Type type = Type::Point;
        Position point;                 // Point: x, y, z, o. Unit: the last known pose; o = the fixed facing
        ObjectGuid unit;                // Unit
        bool followFacing = false;      // Unit: cones/lines turn with the unit; otherwise point.o is used

        Anchor() = default;
        Anchor(Position const& p);      // implicit by design: Mechanic::Circle(me, pos, ...)
        Anchor(::Unit const* u);        // implicit by design: Mechanic::Circle(me, target, ...); stores the guid

        bool IsUnit() const { return type == Type::Unit; }
    };

    struct Projectile
    {
        float speed = 0.f;              // yd/s
        Milliseconds returnPause{ 0 };
        bool outAndBack = false;
        float halfLength = 0.f;         // 0 = width / 2
        ObjectGuid unit;                // set: track this unit's projected position instead of the computed one

        // runtime (registry)
        Milliseconds lastElapsed{ 0 };
        float lastTrackedS = 0.f;
        bool trackedStarted = false;
        uint32 trackedPass = 0;
        uint32 passesReported = 0;      // onResolve calls made (one per finished pass)
    };

    struct MineState : MineTimer<ObjectGuid>     // armAt, grace, armed, spent, insideSince
    {
        uint32 capGroup = 0;
        uint8 cap = 0;                  // 0 = no cap
        Milliseconds lifetime{ 0 };     // 0 = until detonated, fizzled or the encounter ends
    };

    struct Persist
    {
        Milliseconds until{ 0 };        // absolute
        Milliseconds tick{ 0 };         // 0 = one resolve, then the zone just lingers until `until`
        Milliseconds nextTick{ 0 };     // absolute
        Milliseconds duration{ 0 };     // as given (until = resolveAt + duration)
        uint32 ticks = 0;               // resolves done; also the pass id of the next one
    };

    struct Eligibility
    {
        Role roles = Role::Any;
        GuidSet carriers;               // empty = everyone
        uint32 auraSpell = 0;           // 0 = no stack threshold
        uint8 minStacks = 0;
        uint8 maxStacks = 255;
    };

    struct Ordering
    {
        uint32 auraSpell = 0;
        bool ascending = true;
        uint8 capStacks = 0;            // players at or above this many stacks never claim (0 = no cap)
    };

    struct SelfTrigger
    {
        uint32 auraSpell = 0;           // 0 = off
        uint8 minStacks = 0;
    };

    struct BaitRule
    {
        enum class Type : uint8
        {
            FewestStacks,   // the non-tank with the fewest stacks of auraSpell, ties to the lowest guid
            Farthest        // whoever is farthest from the owner
        };

        Type type = Type::Farthest;
        uint32 auraSpell = 0;

        static BaitRule FewestStacks(uint32 spell) { return { Type::FewestStacks, spell }; }
        static BaitRule Farthest() { return { Type::Farthest, 0 }; }
    };

    // A bot's planned spot on one mechanic (M3 handoff §3.6). Registry::Reserve replaces the bot's earlier entry and
    // keeps its `since`; Tick drops it at `until`.
    struct Reservation
    {
        ObjectGuid bot;
        Position point;
        Milliseconds until{ 0 };
        Milliseconds since{ 0 };        // when the bot's current claim (claim, slot) began on this mechanic (0: now)
        float facing = 0.f;             // the facing the bot will hold there (look-away, carried cones)
        uint8 slot = 0;                 // Spread slot ring index; Stack Cluster: slot 0 = the leader
        bool claim = false;             // Soak claimer, Bait picker, Cluster leader
    };

    // What one resolve pass is testing. Set by the registry right before EngineBridge::CastResolve and read by the
    // resolve spell's target filter (Registry::FilterResolveTargets); inactive otherwise.
    struct ResolveContext
    {
        bool active = false;
        uint32 pass = 0;
        Geo::Pose pose;                 // the shape's pose for this pass
        Shape shape;                    // the shape for this pass (Spread/RunOut: a circle on the carrier)
        bool swept = false;             // projectile: also require lo <= s <= hi
        float sweepLo = 0.f;
        float sweepHi = 0.f;
        ObjectGuid exclude;             // Spread/RunOut: the carrier itself is not hit by its own circle
        ObjectGuid only;                // Bait / RunOut from owner: only this unit
        ObjectGuid carrier;             // Spread/RunOut: whose circle this is (C2 casts from that carrier's marker)
        std::vector<ObjectGuid> selected;   // filled by FilterResolveTargets: the units kept this pass

        // Search radius (3D, as the area search measures) for a caster at (x, y, z): ResolveSearchRadius
        // (MechanicMath.h), covering a trailing or off-anchor marker and the shape's zTolerance.
        float SearchRadius(float casterX, float casterY, float casterZ) const;
    };

    struct ResolveResult
    {
        uint32 mechanicId = 0;
        uint32 tag = 0;
        Kind kind = Kind::Zone;
        uint32 pass = 0;
        // Damage kinds: the units the resolve filter kept (recorded at filter time, so an immune target, e.g. Divine
        // Shield, is listed although it took nothing). Stack/Soak: the players inside.
        std::vector<ObjectGuid> hits;
        uint32 count = 0;               // Soak/Stack: eligible players inside; others: hits.size()
        bool fizzled = false;           // mine removed by its cap or lifetime: no damage
        ObjectGuid picked;              // Bait: who took it
    };

    // ----------------------------------------------------------------------------------------------------------------
    // One registered mechanic. Field names and meanings are the contract M2/M3 build on (handoff §3.3).
    // Named ActiveMechanic (not Mechanic) so `using namespace BossMechanics;` never makes the Mechanic:: API
    // namespace ambiguous.
    // ----------------------------------------------------------------------------------------------------------------
    struct ActiveMechanic
    {
        uint32 id = 0;
        ObjectGuid owner;
        Kind kind = Kind::Zone;
        uint32 tag = 0;                 // the script's own mechanic id (RZ1 -> 1, ...)

        Anchor anchor;
        std::optional<Anchor> to;       // Line(from, to): direction and length follow to - from
        Shape shape;
        Family family = Family::GetOut;
        Color color = Color::Red;

        Milliseconds createdAt{ 0 };
        Milliseconds resolveAt{ 0 };    // mines: armAt; HoldStill: window start
        Milliseconds expireAt{ 0 };

        std::optional<Projectile> projectile;
        std::optional<MineState> mine;
        std::optional<Persist> persist;

        // bot-facing (PLAN §3.1)
        Eligibility eligibility;
        Ordering ordering;
        SelfTrigger selfTrigger;
        MeasuredFrom measuredFrom = MeasuredFrom::Group;
        uint8 count = 0;                // Spread/RunOut: how many random targets when none are given
        uint8 soakCount = 0;            // Soak: bodies needed (at least; extra bodies never fail)
        uint8 clusterMinOthers = 0;     // Stack Cluster(n): each eligible player needs n others within the radius
        // Frontal/Zone: one player is never hit: the victim of the anchor when the anchor is a creature, else the
        // owner's victim (ExemptVictim).
        bool tankExempt = false;
        float othersRange = 0.f;        // Bait: everyone else stays within this of the owner
        float holdTolerance = Rules::kHoldStillToleranceYd;    // HoldStill: allowed movement in the window (yd)
        // Shape kinds, Park(unit): the tank should hold `parkUnit` (empty: the owner) inside this shape. Read by the
        // bots only; no server effect.
        bool park = false;
        ObjectGuid parkUnit;
        BaitRule bait;
        GuidSet targets;                // Spread/RunOut/HoldStill: the players (Spread: empty = everyone eligible)
        std::unordered_map<ObjectGuid, Position> spots;     // Spot
        std::vector<Reservation> reservations;

        // penalty and damage
        DamageSpec damage;
        bool weakened = false;
        bool weakenedOncePerPlayer = false;
        GuidSet weakenedApplied;
        std::unordered_map<uint32, GuidSet> hitPerPass;

        // telegraph
        std::vector<ObjectGuid> markers;                            // C2 fills (SpawnMarkers)
        std::unordered_map<ObjectGuid, ObjectGuid> carrierMarkers;  // C2: carrier guid -> its marker
        bool telegraph = true;
        bool fillTimer = true;          // false: outline only until the danger is live (crossing cells)
        // Line markers only (ExceptOne's last lane): draw a strip drawWidth wide whose centreline is drawLateral yd
        // to the left of the hitbox's, instead of the hitbox (ApplyDrawStrip). drawWidth 0 = draw the hitbox.
        float drawLateral = 0.f;
        float drawWidth = 0.f;

        // callbacks (owner scripts; dropped when the encounter ends, never called once the owner left the map)
        std::function<void(Unit* hit)> onHit;
        std::function<void(ResolveResult const&)> onResolve;

        uint32 laneSet = 0;
        bool cancelled = false;

        // runtime (registry; read-only for everyone else)
        bool committed = false;         // timing finalised, targets picked, markers spawned
        bool resolved = false;          // first resolve done
        bool finished = false;          // all resolves done: pruned at the end of this tick
        bool fizzled = false;           // removed without damage (mine cap or lifetime)
        bool detonateRequested = false;
        Milliseconds holdWindow{ 0 };   // HoldStill
        Geo::Pose lastPose;             // last known anchor pose (used when a unit anchor is gone)
        ResolveContext ctx;
        std::unordered_map<ObjectGuid, Position> holdStart;     // HoldStill: positions at the window start
        GuidSet failedPlayers;          // HoldStill/Spot, lasting Stack: one failure per player

        bool IsShapeKind() const { return Judge::IsShapeKind(kind); }
        bool IsCluster() const { return kind == Kind::Stack && clusterMinOthers > 0; }
        // Park: the unit the tank should hold inside the shape.
        ObjectGuid ParkedUnit() const { return parkUnit.IsEmpty() ? owner : parkUnit; }
        bool IsActive(Milliseconds now) const { return !cancelled && !finished && now < expireAt; }
    };

    // A computed projectile's timing parameters (default params when m has no projectile). The registry's sweep and
    // hot windows and the bots' view (M2) all use this, so they agree.
    ProjectileParams ProjectileParamsOf(ActiveMechanic const& m);

    // ----------------------------------------------------------------------------------------------------------------
    // Failures and encounters
    // ----------------------------------------------------------------------------------------------------------------
    struct FailureEvent
    {
        enum class Type : uint8
        {
            Hit,
            MissedSoak,     // group failure: player is empty
            MissedStack,
            MissedBait,
            Moved,
            OffSpot
        };

        ObjectGuid encounter;           // the owner
        uint32 mechanicId = 0;
        uint32 tag = 0;
        Kind kind = Kind::Zone;
        ObjectGuid player;
        Type type = Type::Hit;
        uint32 damage = 0;
        Milliseconds at{ 0 };
    };

    char const* FailureTypeName(FailureEvent::Type type);

    struct PlayerLedger
    {
        uint32 total = 0;
        std::unordered_map<uint32, uint32> byTag;
    };

    // origin = a corner; X runs along `angle` for sizeX, Y along angle + 90 degrees for sizeY.
    struct RoomFrame
    {
        Position origin;
        float angle = 0.f;
        float sizeX = 0.f;
        float sizeY = 0.f;

        Frame ToFrame() const;
    };

    struct LaneSet
    {
        uint32 id = 0;
        ObjectGuid owner;
        RoomFrame frame;
        std::vector<uint32> lanes;      // mechanic ids, parallel to `rects`
        std::vector<LaneRect> rects;
        std::vector<LaneRect> freeLanes;    // ExceptOne: the lane left open, per call
        std::vector<uint32> cells;      // crossing cells (mechanic ids)
    };

    struct EncounterOptions
    {
        Color getOut = Color::Red;    // D5 palette
        Color getIn = Color::Blue;
        std::optional<uint32> bossStateId;
        // Lane sets (M1-FIX-LINES): lanes laid along the second axis of a set, and the crossing cells, draw in their
        // own colors so a crossing does not stack six layers of one color.
        Color crossLanes = Color::Yellow;
        Color cells = Color::Purple;
    };

    struct Encounter
    {
        ObjectGuid owner;
        uint32 bossStateId = 0;
        bool hasBossStateId = false;
        Color getOut = Color::Red;
        Color getIn = Color::Blue;
        Color crossLanes = Color::Yellow;
        Color cells = Color::Purple;
        std::unordered_map<ObjectGuid, PlayerLedger> ledger;
        std::vector<FailureEvent> events;   // capped at Rules::kMaxEventsPerEncounter (oldest dropped)
        uint32 groupFailures = 0;           // MissedSoak count
        std::function<void(FailureEvent const&)> onFailure;
        bool ending = false;                // EndEncounter was called during Tick; finished at the end of it
        // Players this encounter stacked Weakened on. Its end removes Weakened from them only, and not from a player
        // another encounter still on the map has also stacked.
        GuidSet weakenedPlayers;
        // `.mech test` (owner = Mechanic Test Owner): the GMs who ran it. Only they and their group (raid) members
        // are eligible for the encounter's mechanics; empty on a test owner = nobody.
        GuidSet testGms;
    };

    // ----------------------------------------------------------------------------------------------------------------
    // The registry
    // ----------------------------------------------------------------------------------------------------------------
    class Registry : public DataMap::Base
    {
    public:
        static constexpr char const* kKey = "mod-boss-mechanics";
        // "Never": a mine without a lifetime expires at this time; a persistent zone without ticks schedules its next
        // tick here. Bots map it to Solver::kForever.
        static constexpr Milliseconds kForever{ std::numeric_limits<int64>::max() / 4 };

        explicit Registry(Map* map) : _map(map) { }

        static Registry* Get(Map const* map);   // nullptr when the map has none (bots: early-out)
        static Registry& GetOrCreate(Map* map);

        Map* GetMap() const { return _map; }

        // Changes whenever what bots see may have changed: Add, Commit, a timing change, Cancel, Detonate, a mine
        // arming, any prune or fizzle, encounter start and end, a lane set added. Bots compare it with the revision
        // they last planned for (M2 handoff §3.4, §3.6). Never 0, so a fresh bot state (seenRevision 0) always
        // differs.
        uint32 Revision() const { return _revision; }

        // ---- writes (scripts, command, hooks) ----
        // Registers a mechanic (assigns id and createdAt, applies the encounter palette). nullptr when refused
        // (BossMechanics.MaxActivePerMap reached, or the module is disabled).
        ActiveMechanic* Add(ActiveMechanic m);
        // Finalises timing, picks random targets, enforces the mine cap and spawns markers. Idempotent for the
        // markers; timing is recomputed on every call. Called by MechanicRef at the end of the statement, by
        // LaneBuilder, and by Tick for anything still uncommitted.
        void Commit(uint32 id);
        void FinalizeTiming(ActiveMechanic& m);    // also bumps the revision
        void Cancel(uint32 id);         // no damage; pruned (markers despawned) on the next Tick
        void Detonate(uint32 id);       // resolve on the next Tick (mines: then spent)
        Encounter& BeginEncounter(ObjectGuid owner, EncounterOptions const* opt = nullptr);
        // markers, mechanics, ledger; Weakened on the players this encounter stacked (unless another encounter on the
        // map stacked them too)
        void EndEncounter(ObjectGuid owner);
        void EndEncountersForBossState(uint32 bossStateId);
        // The boss-state hook's path: ends the encounters bound to bossStateId at the start of the next Tick, so the
        // boss script's JustDied (and anything else in the same call chain) can still read the ledger. At that tick
        // the state is re-read; a change the instance refused (still IN_PROGRESS / SPECIAL) keeps the encounter.
        void DeferEndEncountersForBossState(uint32 bossStateId);
        // Ends the owner's encounter at the start of the next Tick instead of now. For hooks that can run while the
        // map unloads a grid (AllCreatureScript::OnCreatureRemoveWorld): despawning markers there would queue objects
        // for removal mid-unload (Map::UnloadGrid asserts the remove list is empty). No-op without an encounter.
        void DeferEndEncounter(ObjectGuid owner);
        void ClearAll();                // every encounter (`.mech clear all`)
        void Tick(Milliseconds now);    // from AllMapScript::OnMapUpdate (C2), full map updates only
        void PostFailure(FailureEvent const& e);
        // Replaces the bot's earlier reservation on that mechanic (keeping its `since`; a new one with since 0 starts
        // now). Bumps ReservationRevision when the entry is new or its point, facing, slot or claim changed; a
        // re-reserve that only moves `until` does not.
        void Reserve(uint32 id, Reservation r);
        void Unreserve(uint32 id, ObjectGuid bot);  // bumps ReservationRevision when the bot had an entry
        // Changes whenever another bot's reservations may have changed (Reserve, Unreserve, pruning at `until`, a
        // mechanic holding reservations removed). Separate from Revision() so a reservation never re-plans the bots
        // that only follow hazards. Never 0.
        uint32 ReservationRevision() const { return _reservationRevision; }
        LaneSet& AddLaneSet(ObjectGuid owner, RoomFrame const& frame);
        LaneSet* FindLaneSetMutable(uint32 id);

        // ---- resolve spell (C2's spell_boss_mechanics_resolve calls these) ----
        // OnObjectAreaTargetSelect: keeps the units inside ctx (shape, sweep band, only/exclude), eligible, not
        // tank-exempt, player-controlled, and not yet hit this pass; records them in hitPerPass. A mechanic that is
        // gone or has no active ctx empties the list.
        void FilterResolveTargets(uint32 id, std::list<WorldObject*>& targets);
        // OnEffectLaunchTarget: the flat damage for this target (tank number for the tank when pctTank >= 0).
        int32 ResolveDamageFor(uint32 id, Unit const* target) const;
        // AfterHit: posts Hit for players, applies Weakened when flagged, calls onHit. Ignores units that are not
        // player-controlled (a reflected or otherwise stray hit never reaches the ledger). C2 casts the rider itself.
        void OnResolveHit(uint32 id, Unit* target, uint32 damage);

        // ---- reads (bots, scripts, command): filter by `now`; only Tick() prunes ----
        template <class F>
        void ForEachActive(Milliseconds now, F&& f) const
        {
            for (auto const& [id, m] : _mechanics)
                if (m.IsActive(now))
                    f(m);
        }

        template <class F>
        void ForEachEncounter(F&& f) const
        {
            for (auto const& [owner, e] : _encounters)
                f(e);
        }

        template <class F>
        void ForEachLaneSet(F&& f) const
        {
            for (auto const& [id, ls] : _laneSets)
                f(ls);
        }

        ActiveMechanic const* Find(uint32 id) const;
        ActiveMechanic* FindMutable(uint32 id);
        LaneSet const* FindLaneSet(uint32 id) const;
        Encounter const* FindEncounter(ObjectGuid owner) const;
        Encounter* FindEncounterMutable(ObjectGuid owner);
        std::size_t Size() const { return _mechanics.size(); }

        // ---- evaluation (one code path for the resolve filter, scripts and the M2 solver) ----
        // Live pose of the anchor (unit anchors read the unit now; falls back to the last known pose when it is
        // gone) and the effective shape (Line(from, to): direction and length from the two anchors).
        Geo::Pose AnchorPose(ActiveMechanic const& m) const;
        Shape EffectiveShape(ActiveMechanic const& m, Geo::Pose& pose) const;
        // Shape kinds: inside the hitbox now (false for `who` when the shape is anchored on `who`, decision §7.7).
        // Spread/RunOut: within the radius of a carrier other than `who`. Stack/Soak: inside the shape. Stack
        // Cluster(n): at least n eligible players other than `who` within the radius of the point. Spot: within the
        // radius of who's own spot (other kinds: false).
        bool ContainsPoint(ActiveMechanic const& m, Position const& p, ObjectGuid who = ObjectGuid::Empty) const;
        bool ContainsUnit(ActiveMechanic const& m, Unit const* who) const;
        // When the point is dangerous (empty when it is outside the shape). Times are absolute.
        std::vector<HotWindow> HotWindows(ActiveMechanic const& m, Position const& p) const;
        bool IsEligible(ActiveMechanic const& m, Unit const* who) const;
        bool IsTankExempted(ActiveMechanic const& m, Unit const* who) const;
        // Tank-exempt shapes: the player never hit (B7: the anchor creature's victim, else the owner's victim);
        // empty when not tank-exempt or nobody is the victim.
        ObjectGuid ExemptVictim(ActiveMechanic const& m) const;
        Role RoleOf(ActiveMechanic const& m, Player const* player) const;
        // Bait: who the rule picks now (empty guid when nobody qualifies).
        ObjectGuid BaitPick(ActiveMechanic const& m) const;
        // Spread/RunOut: the carriers right now (targets, all eligible, or self-trigger stacks).
        std::vector<ObjectGuid> Carriers(ActiveMechanic const& m) const;
        // The eligible players of the mechanic right now (candidates: alive, in world, not GM, in test scope and
        // range). Stack Cluster(n): the players who each need n others around them.
        std::vector<ObjectGuid> EligiblePlayers(ActiveMechanic const& m) const;

    private:
        Unit* FindUnit(ObjectGuid guid) const;
        bool OwnerPresent(ActiveMechanic const& m) const;
        // Alive, in world, not GM, eligible by IsInTestScope; on a non-instance map also within
        // BossMechanics.OpenWorld.CandidateRadius of the owner (the anchor's last pose when the owner is gone).
        std::vector<Player*> CandidatePlayers(ActiveMechanic const& m) const;
        // `.mech test` encounters: only the GMs who ran it and their group members. Every other encounter: true.
        bool IsInTestScope(ActiveMechanic const& m, Player const* player) const;
        Color DefaultColor(Encounter const& e, Family f) const;

        void Step(ActiveMechanic& m, Milliseconds now);
        void StepShape(ActiveMechanic& m, Milliseconds now);
        void StepProjectile(ActiveMechanic& m, Milliseconds now);
        void StepMine(ActiveMechanic& m, Milliseconds now);
        void StepPersistent(ActiveMechanic& m, Milliseconds now);
        void StepCarriers(ActiveMechanic& m, Milliseconds now);
        void StepStackSoak(ActiveMechanic& m, Milliseconds now);
        void StepBait(ActiveMechanic& m, Milliseconds now);
        void StepHoldStill(ActiveMechanic& m, Milliseconds now);
        void StepSpot(ActiveMechanic& m, Milliseconds now);
        // One Stack/Soak judgement (a single resolve, or one pass of a lasting one).
        void JudgeStackSoak(ActiveMechanic& m, uint32 pass, Milliseconds now);
        // Shape kinds anchored on a player: that player (never hit by its own shape, decision §7.7); else empty.
        ObjectGuid ShapeExclude(ActiveMechanic const& m) const;

        // One resolve pass: sets ctx, casts through the bridge (or, with no damage, rider or Weakened, only collects
        // who is inside), clears ctx. Returns the units hit this pass.
        std::vector<ObjectGuid> ResolvePass(ActiveMechanic& m, uint32 pass, Geo::Pose const& pose,
            Shape const& shape, ObjectGuid exclude = ObjectGuid::Empty, ObjectGuid only = ObjectGuid::Empty,
            std::optional<SweepBand> band = std::nullopt);
        bool CastsSpell(ActiveMechanic const& m) const;
        bool InsideCtx(ActiveMechanic const& m, Unit const* u) const;
        void Report(ActiveMechanic& m, ResolveResult const& r);
        void PlayerFailure(ActiveMechanic& m, ObjectGuid player, FailureEvent::Type type, uint32 damage = 0);
        void ApplyWeakened(ActiveMechanic& m, Player* player);
        void EnforceCap(ActiveMechanic& m);
        void PickTargets(ActiveMechanic& m);
        void Remove(ActiveMechanic& m);     // despawn markers + fizzle callback; caller erases
        void FinishEncounter(ObjectGuid owner);
        void BumpRevision()
        {
            if (++_revision == 0)
                _revision = 1;
        }

        void BumpReservationRevision()
        {
            if (++_reservationRevision == 0)
                _reservationRevision = 1;
        }

        Map* _map;                      // the map that owns this registry (CustomData): always outlives it
        uint32 _nextId = 1;
        uint32 _nextLaneSetId = 1;
        uint32 _revision = 1;
        uint32 _reservationRevision = 1;
        bool _ticking = false;
        std::map<uint32, ActiveMechanic> _mechanics;
        std::map<uint32, LaneSet> _laneSets;
        std::unordered_map<ObjectGuid, Encounter> _encounters;
        std::vector<ObjectGuid> _pendingEnds;
        std::vector<ObjectGuid> _deferredEnds;     // DeferEndEncounter: ended at the start of the next Tick
        std::vector<uint32> _deferredBossStates;    // DeferEndEncountersForBossState: checked at the next Tick
    };

    // M2 bot waker (handoff §3.6): called by OnMapUpdate right after `reg.Tick(now)`, on the map's update thread, for
    // every map with a registry. The playerbots glue installs it at startup; nullptr (the default) = no call.
    using BotAfterTickFn = void (*)(Map* map, Registry& reg, Milliseconds now);
    void SetBotAfterTick(BotAfterTickFn fn);
}

#endif
