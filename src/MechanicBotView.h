/*
 * mod-boss-mechanics: the bots' view of the registry (STAGE-M2-HANDOFF §3.5, WP-B).
 *
 * Turns the map's registry into a Solver::Input for one bot: the Zone and Frontal hazards that concern it (exact
 * registry geometry, never the markers), their timing, the lane frames near it, and a navmesh path oracle. AzerothCore
 * only: no playerbots types, so the module builds without mod-playerbots. The bot glue (WP-C) fills the role band and
 * the crowd, and supplies the fallibility perception through a callback.
 *
 * Runs on the map's update thread (bots update there), like every registry read.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_VIEW_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_VIEW_H

#include "Define.h"
#include "Duration.h"
#include "MechanicSolver.h"
#include <functional>
#include <string>

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
        Solver::Params params;                  // copied into Input::params

        static BotViewOptions FromConfig(BotConfig const& cfg);
    };

    // Fallibility hook (§3.8), filled by the bot glue: true = the bot perceives the mechanic now and plans with it.
    // false = hidden; the callback then sets `until` (preset to Solver::kForever) to when it should look again (the
    // mechanic becomes visible, or the wrong-side bot realises its mistake).
    using PerceptionFilter = std::function<bool(uint32 mechanicId, Milliseconds& until)>;

    struct BotViewInfo
    {
        uint32 revision = 0;                    // Registry::Revision() the view was built from
        bool movingAnchors = false;             // a kept hazard is anchored on a unit (re-view while it matters)
        uint32 hidden = 0;                      // hazards the perception filter left out
        Milliseconds nextPerceptionAt = Solver::kForever;   // earliest `until` among the hidden ones
        uint32 selfCheckMismatches = 0;         // Debug: solver windows that disagree with Registry::HotWindows
    };

    // Rebuilds out.hazards and out.lanes and sets out.bot, out.speed, out.now and out.params. out.role and out.crowd
    // are the caller's (left untouched).
    //
    // Hazards: every active Zone/Frontal (ForEachActive(now - postResolveHold)) the bot is eligible for and not
    // tank-exempted from, unless it is anchored on the bot itself (RunOut-like; M3), or too far away to matter within
    // the horizon. Timing: plain = Instant [resolveAt, resolveAt + hold] (from = now when a detonation is pending);
    // persistent = Interval [resolveAt, until]; computed projectile = Projectile from resolveAt; tracked projectile =
    // Interval [resolveAt, expireAt]; mine = MineArming / MineArmed [armAt, expireAt] with its grace.
    BotViewInfo BuildInput(Registry const& reg, Player const* bot, Milliseconds now, BotViewOptions const& opt,
        Solver::Input& out, PerceptionFilter const* perceive = nullptr);

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
    class NavOracle : public Solver::PathOracle
    {
    public:
        explicit NavOracle(Unit const* bot, NavOracleStats* stats = nullptr);

        bool Find(Geo::Vec3 const& from, Geo::Vec3 const& to, std::vector<Geo::Vec3>& out) override;
        bool Walkable(Geo::Vec3& p) override;

        uint32 Calls() const { return _calls; }

    private:
        Unit const* _bot;
        NavOracleStats* _stats;
        uint32 _calls = 0;
        uint16 _includeFlags = 0;
        uint16 _excludeFlags = 0;
    };

    // One line of NavOracleStats / Solver::SolveStats for the Debug log and `.mech bots`.
    std::string FormatNavStats(NavOracleStats const& s);
    std::string FormatSolveStats(Solver::SolveStats const& s);
}

#endif
