/*
 * mod-boss-mechanics: the sim seam (STAGE-M4-HANDOFF §3.3, WP-B; names pinned for WP-K, WP-C and WP-S).
 *
 * The one header mod-dpssim includes (behind `__has_include("BossMechanicsSim.h")`, STAGE-M4-0-REPORT answer 11).
 * Always compiled; AzerothCore and pure module headers only, no playerbots types, so the sim builds with this module
 * absent or disabled and this module builds without mod-playerbots (the bot calls are then no-ops returning false).
 *
 * Threads: the sim runs MapUpdate.Threads 0, so its driver and the map updates share one thread. The failure sink is
 * called from the map update (Registry::PostFailure). RegisterBotGlue, ReconcileBot, StartProgram, StopProgram and
 * the stats calls run from the driver between map updates (as the world thread does outside them), never from a
 * map-update hook or a bot's own AI tick.
 *
 * Typical sim iteration:
 *   1. once per process, before any bot exists or later: Sim::RegisterBotGlue(); Sim::SetFailureSink(&MySink);
 *   2. spawn the boss stand-in and the group (all bots in one Group), place them;
 *   3. Sim::StartProgram(boss, actor->GetGUID(), "all" or "none" for the paired baseline, opt, err), with
 *      opt.includeScopePlayer = true (the actor is a member like the others) and opt.seed = the iteration seed;
 *   4. Sim::ReconcileBot(bot) for every bot of the group (installs the strategy: the scope exists now);
 *   5. tick; the sink receives every FailureEvent;
 *   6. Sim::ReadBotStats(bot, stats) per bot, then Sim::StopProgram(boss) and Sim::ResetBotStats(bot).
 */

#ifndef MOD_BOSS_MECHANICS_BOSS_MECHANICS_SIM_H
#define MOD_BOSS_MECHANICS_BOSS_MECHANICS_SIM_H

#include "Define.h"
#include "MechanicBotCounters.h"
#include "MechanicDrillPlan.h"
#include "MechanicRegistry.h"
#include "ObjectGuid.h"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Creature;
class Map;
class Player;

namespace BossMechanics::Sim
{
    // Global failure sink: every FailureEvent of every encounter on every map, right after the encounter's own
    // onFailure callback (also when it has none). One sink: a second call replaces it, nullptr removes it. Called on
    // the map's update thread. MissedSoak events have an empty `player` (a group failure).
    using FailureSink = void (*)(Map const* map, FailureEvent const& e);
    void SetFailureSink(FailureSink fn);

    // The bot glue's first-world-tick registration (contexts into the ten class lists, the waker hook, the
    // BossMechanics.Bots.Enable latch), callable early from the sim's start path (playerbots must have built its
    // shared contexts: after OnBeforeWorldInitialized). Idempotent: the first world tick then does nothing, and a
    // second call returns the latched answer. True when the glue is enabled; false with
    // BossMechanics.Bots.Enable = 0 or without playerbots.
    bool RegisterBotGlue();

    // Runs the strategy gate for one bot now (BotGate::Reconcile: login, map change and the world sweep do it live,
    // none of which happens in a sim). Latches the glue first when RegisterBotGlue was not called. True when the
    // bot has the "boss mechanics" strategy afterwards: in an instance map, or in the open world while the bot is,
    // or its group (raid) holds, a scope player of a live encounter that wants bots (StartProgram makes one). False
    // for a real player, a bot outside every scope, the glue disabled, or a build without playerbots.
    bool ReconcileBot(Player* bot);

    // Start / stop a drill program on `owner` (a creature the sim spawned, the boss stand-in: it owns the encounter
    // and is the creature the drills select), scoped to the group of `scopePlayer`. Program text:
    // Drill::ParseProgram ("all", "none", or "<entry>[:<n>],..."). `none` begins the encounter and scope only (the
    // paired baseline). StartProgram drops the pending steps of an earlier program on the owner first. Returns false
    // with a reason (unknown entry, owner not alive in world, scope player not on the owner's map).
    bool StartProgram(Creature* owner, ObjectGuid scopePlayer, std::string_view program, DrillOptions const& opt,
        std::string& err);
    // Ends the owner's encounter: pending steps, mechanics, markers, ledger, Weakened (the sink already has the
    // failures).
    void StopProgram(Creature* owner);

    // Per-bot counters snapshot (zeros without playerbots), for the report. Reset between iterations.
    // Units and meaning: STAGE-M4-C-REPORT §2. The mode and leak fields are copies of the same-named
    // MechanicBotCounters fields (MechanicBotCounters.h, the leak rules in MechanicLeaks.h); `cost` is CostStats
    // (MechanicCost.h).
    struct BotStats
    {
        MechanicBotCounters counters;
        // Time in each mode, ms, disjoint (a released bot counts as released only); sampled per map update.
        uint32 msIdle = 0, msWatch = 0, msEscape = 0, msHold = 0, msFreeze = 0, msReleased = 0;
        // §3.5 leak detectors: moves, casts and turns the lock should have stopped, seen from the outcome.
        uint32 leakMoves = 0, leakCasts = 0, leakTurns = 0;
        std::vector<std::pair<uint32, uint32>> leakSpells;    // (spell id, count) of leaked casts and movement spells
        CostStats cost;                                       // §3.6
    };
    // `out` is reset first. False for a real player, the glue not registered or disabled, or without playerbots.
    bool ReadBotStats(Player const* bot, BotStats& out);
    void ResetBotStats(Player const* bot);
}

#endif
