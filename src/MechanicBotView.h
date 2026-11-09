/*
 * mod-boss-mechanics: the bots' view of the registry (STAGE-M2-HANDOFF §3.5, WP-B; STAGE-M3-HANDOFF §3.4, §3.6, WP-V).
 *
 * Turns the map's registry into a Solver::Input for one bot: the Zone and Frontal hazards that concern it (exact
 * registry geometry, never the markers), their timing, the lane frames near it, and a navmesh path oracle. AzerothCore
 * only: no playerbots types, so the module builds without mod-playerbots. The bot glue (WP-C) fills the role band and
 * the crowd, and supplies the fallibility perception through a callback.
 *
 * M3 adds the player kinds (Spread, RunOut, Stack, Soak, Bait, HoldStill, Spot, Zones anchored on or aimed at the bot):
 * regions, allies, carried shapes, synthetic hazards (other players' circles at their planned spots), FarFrom, Freeze
 * and the slot ring, plus the bot's tasks for the brain (Stratholme X3: a Jump is a Jumper task only). The pure rules are in MechanicViewRules.h. The reservation
 * board (Registry::Reserve) is how bots see each other's planned spots: RefreshAllies right before every solve,
 * PublishReservations right after it.
 *
 * Runs on the map's update thread (bots update there), like every registry read.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_VIEW_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_VIEW_H

#include "Define.h"
#include "Duration.h"
#include "MechanicJudge.h"
#include "MechanicSolver.h"
#include "ObjectGuid.h"
#include <functional>
#include <string>
#include <vector>

class Player;
class Unit;

namespace BossMechanics
{
    class Registry;
    struct ActiveMechanic;
    struct BotConfig;

    // Everything BuildInput needs from the config, cached by the caller (built from BotConfig on each config load).
    struct BotViewOptions
    {
        Milliseconds postResolveHold{ 500 };    // a plain Zone stays live this long past resolveAt
        float safetyMarginYd = 0.75f;           // base margin of every hazard
        float facingSlackDeg = 15.f;            // turning cones/lines: extra poses at +-this
        Milliseconds movingAnchorRecheck{ 250 };    // the brain re-views this often while an anchor moves
        float laneFrameRangeYd = 40.f;          // lane frames farther than this from the bot are left out
        bool debug = false;                     // run the self-check (BossMechanics.Bots.Debug)
        float humanSlackYd = 1.f;               // a human's spot is its current one plus this (HumanSlackYd)
        float lookAwayMarginDeg = 10.f;         // carried cones/lines: angle kept clear of allies (LookAway.MarginDeg)
        Milliseconds freezeSettle{ 250 };       // HoldStill: stop moving this long before the window (Freeze.SettleMs)
        Solver::Params params;                  // copied into Input::params

        static BotViewOptions FromConfig(BotConfig const& cfg);
    };

    // Fallibility hook (§3.8), filled by the bot glue: true = the bot perceives the mechanic now and plans with it.
    // false = hidden; the callback then sets `until` (preset to Solver::kForever) to when it should look again (the
    // mechanic becomes visible, or the wrong-side bot realises its mistake).
    using PerceptionFilter = std::function<bool(uint32 mechanicId, Milliseconds& until)>;

    // Filled by the glue: what the view cannot know without playerbots types. Either may be empty (then nobody is a
    // bot but `bot` itself: every other player is placed as a human, at its current spot plus HumanSlackYd).
    struct BotViewHooks
    {
        // A group bot's planned spot (plan.target while it Escapes, Holds or Freezes) and the facing it will hold
        // there (its orientation when it has no chosen facing). false = no plan.
        std::function<bool(ObjectGuid, Geo::Vec3& spot, float& facing)> plannedSpot;
        std::function<bool(ObjectGuid)> isBot;
    };

    // What the bot is asked to do for one mechanic (§3.4).
    enum class BotTaskRole : uint8
    {
        Carrier,    // Spread/RunOut carrier, a Zone anchored on or aimed at the bot (keep allies out of it)
        Avoider,    // eligible non-carrier of a Spread/RunOut, Soak non-claimer (keep out)
        Claimer,    // Soak claimer (be inside; a claim on the board)
        Leader,     // Stack anchored on the bot, or the Cluster leader (hold; the others come)
        Picker,     // Bait picker (run out past everyone; a claim on the board)
        Member,     // Stack, Cluster member, Bait non-picker, Spot (be inside)
        Frozen,     // HoldStill (Freeze over the window)
        Tank,       // the exempt victim of a tank-exempt Frontal/Zone (tank facing, §3.7)
        Parker,     // the victim of a Park zone's parked unit (tank parking, §3.7)
        Jumper      // an eligible player of a Jump (Stratholme X3): hop so it is in the air at the resolve (no hold)
    };

    char const* BotTaskRoleName(BotTaskRole role);

    struct BotTask
    {
        uint32 mechanicId = 0;
        Kind kind = Kind::Zone;
        BotTaskRole role = BotTaskRole::Member;
        Milliseconds from{ 0 }, to{ 0 };        // the judge window (Freeze: [resolveAt - SafetyMarginMs, expireAt])
        uint8 slot = 0;                         // Spread/RunOut carrier: Assign::SlotOf; Soak claimer: claim rank
    };

    struct BotViewInfo
    {
        uint32 revision = 0;                    // Registry::Revision() the view was built from
        bool movingAnchors = false;             // a kept hazard is anchored on a unit (re-view while it matters)
        uint32 hidden = 0;                      // hazards the perception filter left out
        Milliseconds nextPerceptionAt = Solver::kForever;   // earliest `until` among the hidden ones
        uint32 selfCheckMismatches = 0;         // Debug: solver windows that disagree with Registry::HotWindows

        // M3 (player kinds), rebuilt by BuildInput and RefreshAllies.
        std::vector<BotTask> tasks;             // in mechanic id order
        std::vector<uint32> selfTriggerAuras;   // Spread/RunOut SelfTrigger auras the bot is eligible for: poll the
                                                // stack count every step, a change is a view change (§3.4)
        std::vector<uint32> tankFrontals;       // tank-exempt shapes whose exempt victim is the bot (tank facing)
        std::vector<uint32> parks;              // Park shapes whose parked unit is the bot's victim (parking)
        uint32 reservationRevision = 0;         // Registry::ReservationRevision() the player kinds were built from
    };

    // Rebuilds out.hazards and out.lanes and sets out.bot, out.speed, out.now and out.params, then the player kinds
    // (RefreshAllies, below). out.role, out.crowd, out.sight, out.reach, out.park and out.roleSolve are the caller's
    // (left untouched).
    //
    // Hazards: every active Zone/Frontal (ForEachActive(now - postResolveHold)) the bot is eligible for and not
    // tank-exempted from, unless it is anchored on the bot itself or aimed at it (a carried shape instead), or too far
    // away to matter within the horizon. Timing: plain = Instant [resolveAt, resolveAt + hold] (from = now when a
    // detonation is pending); persistent = Interval [resolveAt, until]; computed projectile = Projectile from
    // resolveAt; tracked projectile = Interval [resolveAt, expireAt]; mine = MineArming / MineArmed [armAt, expireAt]
    // with its grace.
    BotViewInfo BuildInput(Registry const& reg, Player const* bot, Milliseconds now, BotViewOptions const& opt,
        Solver::Input& out, PerceptionFilter const* perceive = nullptr, BotViewHooks const* hooks = nullptr);

    // The player-kind part of the view, rebuilt from the registry and the reservation board; call it right before
    // every solve so sequential solves on the map thread see each other (§3.6). Replaces input.regions, allies,
    // carried, far, freeze, slot/slotCount and every synthetic hazard (Hazard::synthetic); leaves the registry hazards,
    // lanes, role, crowd, sight, reach, park, params and roleSolve alone. Sets input.now, bot and speed. With `info`,
    // its tasks, selfTriggerAuras, tankFrontals, parks and reservationRevision are replaced (PublishReservations reads
    // the tasks).
    // `perceive` should be the filter BuildInput got (a hidden player kind stays hidden).
    void RefreshAllies(Registry const& reg, Player const* bot, Milliseconds now, BotViewOptions const& opt,
        BotViewHooks const* hooks, Solver::Input& input, BotViewInfo* info = nullptr,
        PerceptionFilter const* perceive = nullptr);

    // After a solve: publishes Reservation{bot, plan.target, until = the task's window end, facing (plan.facing when
    // the plan has one, else the bot's orientation), slot, claim (Claimer, Picker, Leader)} on every mechanic of
    // info.tasks except Tank, Parker, Frozen and Jumper tasks, and unreserves the bot from every other mechanic it holds
    // a reservation on (it left them). Registry::Reserve keeps `since` and bumps ReservationRevision only on a change.
    void PublishReservations(Registry& reg, Player const* bot, BotViewInfo const& info, Solver::Plan const& plan,
        Milliseconds now);
    // Every reservation of the bot (released, left combat, logged out, the strategy removed).
    void DropReservations(Registry& reg, ObjectGuid bot);

    // Debug counters of one NavOracle, filled only when the brain passes them (BossMechanics.Bots.Debug = 1).
    struct NavOracleStats
    {
        uint32 probes = 0;          // Walkable() calls
        uint32 probeNoMesh = 0;     // ...on a map without a navmesh (left to Find, which then fails)
        uint32 probeOff = 0;        // ...no walkable polygon within kProbeSnapYd: the candidate is dropped
        uint32 probeSnapped = 0;    // ...moved onto the navmesh (more than 0.05 yd)
        uint32 calls = 0;           // Find() calls
        uint32 ok = 0;              // ...accepted
        uint32 okIncomplete = 0;    // ...of which PATHFIND_INCOMPLETE (ends at the reachable point)
        uint32 okShifted = 0;       // ...of which end more than 0.5 yd from the asked point
        uint32 noPath = 0;          // rejected: PATHFIND_NOPATH (no polygon, or a broken corridor)
        uint32 notUsingPath = 0;    // rejected: PATHFIND_NOT_USING_PATH (no tile, or a shortcut for fliers)
        uint32 tooLong = 0;         // rejected: PATHFIND_SHORT (longer than the point limit)
        uint32 shortcut = 0;        // rejected: PATHFIND_SHORTCUT
        uint32 farStart = 0;        // rejected: PATHFIND_FARFROMPOLY_START (the bot is off the navmesh)
        uint32 otherType = 0;       // rejected: neither NORMAL nor INCOMPLETE
        uint32 empty = 0;           // rejected: no points
        uint32 endFar = 0;          // rejected: the end lands more than kMaxEndShiftYd from the asked point
        uint32 farEnd = 0;          // accepted with PATHFIND_FARFROMPOLY_END (the asked point was > 7 yd off)
        uint32 sightRays = 0;       // InSight() rays cast (M3: healer line of sight)
        uint32 sightBlocked = 0;    // ...that found the line blocked
        uint32 sightCapped = 0;     // InSight() calls past the oracle's own cap (answered "in sight", no ray)

        void Add(NavOracleStats const& o);
    };

    // Solver path oracle on the bot's navmesh.
    //
    // Walkable: one Detour findNearestPoly in a small box around the point (the bot's own polygon filter): no walkable
    // polygon within kProbeSnapYd -> false (inside a wall, a pillar, off a ledge), else the point moves onto the
    // polygon (its navmesh z). No navmesh on the map -> true, unchanged.
    //
    // Find: PathGenerator from `from` to `to` (the same generator and start MoveTo uses). Accepted: PATHFIND_NORMAL or
    // PATHFIND_INCOMPLETE without NOPATH, NOT_USING_PATH, SHORT, SHORTCUT or FARFROMPOLY_START, whose end lands within
    // kMaxEndShiftYd of `to`. The result starts at `from` and ends at the path's real end (the solver validates that
    // point, not `to`). Lives for one solve: it keeps the unit pointer only for that call.
    //
    // InSight (M3): Map::isInLineOfSight (vmaps and game objects) between the two points, both lifted by the bot's
    // collision height (as WorldObject::IsWithinLOS lifts a player). The solver spends at most Sight::maxChecks rays
    // per solve (Healer.MaxSightChecks); `maxSightChecks` is a second cap in the oracle itself, past which it answers
    // true without a ray. Not in world: true.
    class NavOracle : public Solver::PathOracle
    {
    public:
        explicit NavOracle(Unit const* bot, NavOracleStats* stats = nullptr, uint32 maxSightChecks = 64);

        bool Find(Geo::Vec3 const& from, Geo::Vec3 const& to, std::vector<Geo::Vec3>& out) override;
        bool Walkable(Geo::Vec3& p) override;
        bool InSight(Geo::Vec3 const& from, Geo::Vec3 const& to) override;

        uint32 Calls() const { return _calls; }
        uint32 SightRays() const { return _sightRays; }

    private:
        Unit const* _bot;
        NavOracleStats* _stats;
        uint32 _calls = 0;
        uint32 _sightRays = 0;
        uint32 _maxSightChecks;
        uint16 _includeFlags = 0;
        uint16 _excludeFlags = 0;
    };

    // One line of NavOracleStats / Solver::SolveStats for the Debug log and `.mech bots`.
    std::string FormatNavStats(NavOracleStats const& s);
    std::string FormatSolveStats(Solver::SolveStats const& s);
}

#endif
