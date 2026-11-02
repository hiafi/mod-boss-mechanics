/*
 * mod-boss-mechanics: public API for boss scripts and bots (STAGE-M1-HANDOFF §3.5, PLAN §3.1).
 *
 * Boss scripts register mechanics with the Mechanic:: free functions; each returns a MechanicRef, a short-lived
 * builder valid in the calling statement, whose options chain configures the mechanic. At the end of the statement
 * the ref commits (timing, random targets, mine cap, markers). Store the MechanicHandle it converts to, never the ref:
 *
 *     using namespace BossMechanics;
 *     MechanicHandle h = Mechanic::Cone(me, me, 60.f, 20.f, 2500ms, TankExempt::Yes).Damage(35.f).Weakened();
 *     Mechanic::Line(me, me, target, 3.f, 2000ms).Projectile(20.f).OutAndBack().ExcludeSourceEnd(5.f).Damage(30.f);
 *     Mechanic::Mine(me, pos, 4.f, 3000ms, 500ms).Cap(1, 3).Damage(40.f).Weakened();
 *     Mechanic::RoomFrame(me, frame).Lanes(Axis::X, { 3, 6.f, 6.f }, 2000ms)
 *         .Lanes(Axis::Y, { 2, 6.f, 6.f }, 2000ms).BurnCrossings(6000ms, 1000ms, 5.f)
 *         .Each([](MechanicRef& lane) { lane.Damage(25.f); });
 *
 * Boss scripts that call this must be compiled in the `modules` target (handoff §2.9).
 * Bots read the registry: Mechanic::Read(map) -> Registry const* (nullptr: nothing registered on this map).
 *
 * Misuse never asserts: an option that does not fit the kind, a size <= 0, a ladder miss on a marker, or an option
 * after the commit logs `module.bossmechanics` at WARN and is ignored (the hitbox always stays exact).
 */

#ifndef MOD_BOSS_MECHANICS_BOSS_MECHANICS_H
#define MOD_BOSS_MECHANICS_BOSS_MECHANICS_H

#include "BossMechanicsIds.h"
#include "MechanicGeometry.h"
#include "MechanicMath.h"
#include "MechanicProviders.h"
#include "MechanicRegistry.h"
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

class Map;
class Unit;

namespace BossMechanics
{
    // Storable reference to a registered mechanic (a script member). id 0 = none / refused.
    struct MechanicHandle
    {
        uint32 id = 0;

        bool IsValid() const { return id != 0; }
        explicit operator bool() const { return IsValid(); }
        bool operator==(MechanicHandle const& o) const { return id == o.id; }
    };

    // A Zone's anchor and shape: Mechanic::Zone(me, Shapes::Circle(pos, 8.f), 3000ms).
    struct ZoneSpec
    {
        Anchor anchor;
        Shape shape;
        std::optional<Anchor> to;       // Line(from, to)
    };

    namespace Shapes
    {
        ZoneSpec Circle(Anchor const& at, float radius);
        ZoneSpec Donut(Anchor const& at, float inner, float outer);
        // A unit anchor turns with the unit (FollowFacing); a point anchor uses its orientation.
        ZoneSpec Cone(Anchor const& at, float angleDeg, float length);
        // Direction follows to - from live; the length is fixed at registration (distance between them then).
        ZoneSpec Line(Anchor const& from, Anchor const& to, float width);
        // Fixed facing (radians); a unit `from` moves the line but does not turn it.
        ZoneSpec LineFacing(Anchor const& from, float facing, float length, float width);
    }

    // Room-frame lanes: `count` lanes of `width`, gaps between neighbours >= minGap (random placement, uniform over
    // the valid layouts; when they cannot fit, even spacing and a warning).
    struct LaneSpec
    {
        uint32 count = 1;
        float width = 6.f;
        float minGap = 6.f;
    };

    class MechanicRef
    {
    public:
        MechanicRef() = default;                        // invalid (refused registration)
        MechanicRef(Map* map, uint32 id, bool commitOnDestroy = true);
        ~MechanicRef();                                 // commits (Registry::Commit) unless built without it
        MechanicRef(MechanicRef&& other) noexcept;
        MechanicRef(MechanicRef const&) = delete;
        MechanicRef& operator=(MechanicRef const&) = delete;
        MechanicRef& operator=(MechanicRef&&) = delete;

        bool IsValid() const;
        uint32 Id() const { return _id; }
        MechanicHandle Handle() const { return { _id }; }
        operator MechanicHandle() const { return Handle(); }
        ActiveMechanic const* Get() const;

        // damage (Zone, Frontal, Spread, RunOut, Bait)
        MechanicRef& Damage(float pctNonTank);          // % of reference non-tank health
        MechanicRef& TankDamage(float pctTank);         // % of reference tank health, for the tank only
        MechanicRef& FlatDamage(int32 flat);            // overrides the provider (tests)
        MechanicRef& School(uint8 school);              // SpellSchools 0..6 -> resolve spell 205001 + school
        MechanicRef& Spell(uint32 spellId);             // a dungeon's own copy of the resolve spell
        MechanicRef& Rider(uint32 spellId);             // cast on every unit hit
        // penalty (any kind but Soak)
        MechanicRef& Weakened();
        MechanicRef& WeakenedOncePerPlayer();           // implies Weakened()
        // timing (shape kinds)
        MechanicRef& Persistent(Milliseconds duration, Milliseconds tick);  // tick 0: one resolve, then lingers
        MechanicRef& Projectile(float speed);           // Line only: resolves as the body passes (yd/s)
        MechanicRef& ProjectileUnit(Unit const* unit);  // track this unit instead of the computed position
        MechanicRef& OutAndBack(Milliseconds pause = Milliseconds(0));
        MechanicRef& Lifetime(Milliseconds lifetime);   // Mine: fizzles after this long armed-or-not (0 = never)
        MechanicRef& Cap(uint32 group, uint8 n);        // Mine: at most n live per group; the oldest fizzles
        // shape
        MechanicRef& ExcludeSourceEnd(float yd);        // Line
        MechanicRef& FacingOffset(float deg);           // Cone/Line
        MechanicRef& Rear();                            // = FacingOffset(180)
        MechanicRef& AnchorOn(Unit const* unit);        // shape kinds, Stack, Soak
        MechanicRef& FollowFacing();                    // unit anchor turns the cone/line with it
        MechanicRef& ZTolerance(float yd);
        // bot-facing
        MechanicRef& Eligible(Eligibility const& eligibility);
        MechanicRef& Order(Ordering const& ordering);   // Soak, Bait
        MechanicRef& Count(uint8 n);                    // Spread, RunOut: n random eligible targets
        MechanicRef& Targets(GuidSet const& players);   // Spread, RunOut, HoldStill
        MechanicRef& SelfTrigger(uint32 auraSpell, uint8 minStacks);    // Spread, RunOut
        MechanicRef& From(MeasuredFrom from);           // RunOut
        MechanicRef& TankExempt();                      // shape kinds; a cone Zone becomes a Frontal
        // telegraph and bookkeeping
        MechanicRef& Color(BossMechanics::Color color);
        MechanicRef& Tag(uint32 tag);
        MechanicRef& NoTelegraph();
        MechanicRef& OnHit(std::function<void(Unit* hit)> fn);
        MechanicRef& OnResolve(std::function<void(ResolveResult const&)> fn);

    private:
        // The mechanic if `fits` (warns otherwise); also warns when called after the commit and `visual`.
        ActiveMechanic* Edit(char const* option, bool fits, bool visual = false);

        Map* _map = nullptr;
        uint32 _id = 0;
        bool _commit = true;
    };

    class LaneBuilder
    {
    public:
        LaneBuilder() = default;                        // invalid (no owner)
        LaneBuilder(Map* map, ObjectGuid owner, uint32 laneSetId);
        ~LaneBuilder();                                 // commits every lane and cell it made
        LaneBuilder(LaneBuilder&& other) noexcept;
        LaneBuilder(LaneBuilder const&) = delete;
        LaneBuilder& operator=(LaneBuilder const&) = delete;
        LaneBuilder& operator=(LaneBuilder&&) = delete;

        // `count` lanes running along `axis`, random offsets across the frame.
        LaneBuilder& Lanes(Axis axis, LaneSpec const& spec, Milliseconds warn);
        // Lanes of `width` covering the frame across `axis`, all but one random lane (Bloodlines).
        LaneBuilder& ExceptOne(float width, Milliseconds warn, Axis axis = Axis::X);
        // A persistent cell at every crossing of an X lane and a Y lane of this set (L10).
        LaneBuilder& BurnCrossings(Milliseconds duration, Milliseconds tick, float pct);
        // Options on every lane and cell made so far (damage, Weakened, tag, color...).
        LaneBuilder& Each(std::function<void(MechanicRef&)> const& fn);

        uint32 Id() const { return _laneSet; }
        std::vector<MechanicHandle> Handles() const;

    private:
        uint32 AddLine(LanePlacement const& place, Milliseconds warn, Milliseconds resolveAt);

        Map* _map = nullptr;
        ObjectGuid _owner;
        uint32 _laneSet = 0;
        std::vector<uint32> _created;
    };
}

// Script API (PLAN §3.1 sketch). `owner` is the encounter: usually the boss (`me`).
namespace Mechanic
{
    using BossMechanics::Anchor;
    using BossMechanics::MechanicHandle;
    using BossMechanics::MechanicRef;

    // encounter (implicit at the owner's first registration; ends on death, evade, despawn, boss state)
    void BeginEncounter(Unit* owner, BossMechanics::EncounterOptions const& opt = {});     // palette, bossStateId
    void EndEncounter(Unit* owner);

    // zones
    MechanicRef Zone(Unit* owner, BossMechanics::ZoneSpec const& spec, Milliseconds warn);
    MechanicRef Circle(Unit* owner, Anchor const& at, float r, Milliseconds warn);
    MechanicRef Donut(Unit* owner, Anchor const& at, float inner, float outer, Milliseconds warn);
    // Anchored on `source`, turning with it. TankExempt::Yes makes it a Frontal (the owner's victim is never hit).
    MechanicRef Cone(Unit* owner, Unit* source, float angleDeg, float length, Milliseconds warn,
        BossMechanics::TankExempt exempt = BossMechanics::TankExempt::No);
    MechanicRef Line(Unit* owner, Anchor const& from, Anchor const& to, float width, Milliseconds warn);
    MechanicRef Line(Unit* owner, Anchor const& from, float facing, float length, float width, Milliseconds warn);
    // Arms after `arm` (ring -> disc); armed, a player inside for `grace` (or anyone inside at arming) detonates it.
    MechanicRef Mine(Unit* owner, Position const& pos, float r, Milliseconds arm, Milliseconds grace);
    BossMechanics::LaneBuilder RoomFrame(Unit* owner, BossMechanics::RoomFrame const& frame);

    // player kinds (server resolve in M1, bot handling in M3)
    MechanicRef Spread(Unit* owner, float r, Milliseconds warn);      // everyone, or .Count(n) / .Targets()
    MechanicRef RunOut(Unit* owner, Unit* target, float r, Milliseconds warn);
    MechanicRef Stack(Unit* owner, Anchor const& at, float r, Milliseconds warn);
    MechanicRef Soak(Unit* owner, Anchor const& at, float r, uint8 count, Milliseconds warn);
    MechanicRef Bait(Unit* owner, BossMechanics::BaitRule const& rule, float othersRange, Milliseconds warn);
    // The listed players (empty: every eligible player) must not move during [now + warn, now + warn + window].
    MechanicRef HoldStill(Unit* owner, GuidSet const& players, Milliseconds window,
        Milliseconds warn = Milliseconds(0));
    MechanicRef Spot(Unit* owner, std::unordered_map<ObjectGuid, Position> const& spots, Milliseconds warn,
        float r = BossMechanics::Rules::kDefaultSpotRadius);

    // handles
    void Cancel(Unit* owner, MechanicHandle h);
    void Detonate(Unit* owner, MechanicHandle h);
    bool Contains(Unit* owner, MechanicHandle h, Unit const* who);                 // now
    bool ContainsPoint(Unit* owner, MechanicHandle h, Position const& p);
    BossMechanics::PlayerLedger const* Failures(Unit* owner, ObjectGuid player);
    void OnFailure(Unit* owner, std::function<void(BossMechanics::FailureEvent const&)> fn);
    BossMechanics::Registry const* Read(Map const* map);                          // bots (M2)
}

#endif
