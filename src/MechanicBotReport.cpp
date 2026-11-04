/*
 * mod-boss-mechanics: the `.mech bots [all]` report (STAGE-M2-HANDOFF §3.10, §3.11, WP-C), registered with
 * SetBotReport by AddSC_boss_mechanics_bots. Runs on the world thread while no map updates, like every `.mech`.
 *
 * Per bot in the GM's group on the GM's map (`all`: every bot on the map, to check the gate on bot-only groups): the
 * gate status (strategies installed), mode, target and distance, leaveBy - now, the counters, and the bot's failures
 * in the registry's ledger (every encounter on the map). M3 adds the player-kind, tank and route counters, the bot's
 * tasks, its live reservations, its look-away facing and its remembered Frontal sources.
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotGate.h"
#include "Chat.h"
#include "GameTime.h"
#include "Group.h"
#include "Map.h"
#include "MechanicBotBrain.h"
#include "MechanicBotView.h"
#include "MechanicRegistry.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StringFormat.h"
#include <string>
#include <vector>

namespace BossMechanics
{
    namespace
    {
        char const* YesNo(bool b)
        {
            return b ? "yes" : "no";
        }

        uint32 LedgerFailures(Registry const* reg, ObjectGuid guid)
        {
            uint32 total = 0;
            if (!reg)
                return total;
            reg->ForEachEncounter([&](Encounter const& e)
            {
                auto it = e.ledger.find(guid);
                if (it != e.ledger.end())
                    total += it->second.total;
            });
            return total;
        }

        std::string LeaveByText(Milliseconds leaveBy, Milliseconds now)
        {
            if (leaveBy >= Solver::kForever)
                return "-";
            return std::to_string((leaveBy - now).count()) + " ms";
        }

        void ReportOne(ChatHandler* handler, Player* bot, Registry const* reg, Milliseconds now)
        {
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                return;
            bool const combat = botAI->HasStrategy("boss mechanics", BOT_STATE_COMBAT);
            bool const nonCombat = botAI->HasStrategy("boss mechanics", BOT_STATE_NON_COMBAT);
            bool const avoidAoe = botAI->HasStrategy("avoid aoe", BOT_STATE_COMBAT);
            uint32 const failures = LedgerFailures(reg, bot->GetGUID());
            MechanicBotState const* st = (combat || nonCombat) ? BotBrain::StateOf(botAI) : nullptr;
            if (!st)
            {
                handler->PSendSysMessage("{}: boss mechanics combat {} non-combat {}, avoid aoe {}; no state; "
                    "ledger {}", bot->GetName(), YesNo(combat), YesNo(nonCombat), YesNo(avoidAoe), failures);
                return;
            }

            Geo::Vec3 const& t = st->plan.target;
            bool const planned = st->mode == MechanicBotMode::Escape || st->mode == MechanicBotMode::Hold;
            std::string const target = planned ? Acore::StringFormat("({:.1f}, {:.1f}) {:.1f} yd", t.x, t.y,
                Geo::Dist2d(bot->GetPositionX(), bot->GetPositionY(), t.x, t.y)) : std::string("-");
            handler->PSendSysMessage("{}: boss mechanics combat {} non-combat {}, avoid aoe {} | {} | target {} | "
                "leave in {} | ledger {}", bot->GetName(), YesNo(combat), YesNo(nonCombat), YesNo(avoidAoe),
                ModeName(st->mode), target, LeaveByText(st->leaveBy, now), failures);

            MechanicBotCounters const& c = st->counters;
            auto outcome = [&c](Solver::Outcome o) { return c.outcomes[static_cast<std::size_t>(o)]; };
            uint64 const mean = c.solves ? c.solveUsTotal / c.solves : 0;
            handler->PSendSysMessage("  solves {} (safe {}, move {} (reduced margin {}), grace {}, fallback {}, "
                "no path {}), path checks {}, late {} (fallback {}), stuck {}, released {}, motion stops {} (rerouted "
                "{}), lock {} (moves {}, casts {}), cast breaks {}, wakes {}, solve us last {} max {} mean {}",
                c.solves, outcome(Solver::Outcome::Safe), outcome(Solver::Outcome::Move), c.tightMoves,
                outcome(Solver::Outcome::GraceMove), outcome(Solver::Outcome::Fallback),
                outcome(Solver::Outcome::NoPath), c.pathChecks, c.lateArrivals, c.lateFallbacks, c.stuckResolves,
                c.releases, c.motionStops, c.reroutes, c.lockBlocks, c.lockMoves, c.lockCasts, c.castBreaks, c.wakes,
                c.solveUsLast, c.solveUsMax, mean);
            // M3: the player kinds, the tank tasks and the route search (STAGE-M3-HANDOFF §3.8 telemetry).
            handler->PSendSysMessage("  m3: carrier solves {}, claim solves {}, pick solves {}, role solves {}, "
                "violated {}, freezes {}, facings {}, facing blocks {}, facing breaks {}, tank re-facings {} "
                "(stopped by a warning {}), parks {}, routes {} ({} found, {} failed, us last {} max {}), "
                "sight rays {}",
                c.carrierSolves, c.claimSolves, c.pickSolves, c.roleSolves, c.violatedPlans, c.freezes,
                c.facingsApplied, c.facingBlocks, c.facingBreaks, c.tankRefacings, c.refacingStops, c.parks,
                c.routesFound + c.routesFailed, c.routesFound, c.routesFailed, c.routeUsLast, c.routeUsMax,
                c.sightRays);
            // M4 (STAGE-M4-HANDOFF §3.5-§3.7, WP-C): mode time, lock leaks (spell x count), the Fallback split by
            // exposure, route loops; then the cost counters (us; p50 and p95 are log2 bucket upper bounds).
            auto secs = [](uint32 ms) { return ms / 1000.0; };
            std::string leakSpells;
            for (auto const& [spell, n] : c.leakSpells)
                leakSpells += Acore::StringFormat("{}{} x{}", leakSpells.empty() ? "" : ", ", spell, n);
            handler->PSendSysMessage("  m4: time (s) idle {:.1f} watch {:.1f} escape {:.1f} hold {:.1f} freeze {:.1f} "
                "released {:.1f} | leaks: moves {}, casts {}, turns {} (spells {}) | fallback clear {} hot {} kept {} "
                "| route loops {}", secs(c.msIdle), secs(c.msWatch), secs(c.msEscape), secs(c.msHold), secs(c.msFreeze),
                secs(c.msReleased), c.leakMoves, c.leakCasts, c.leakTurns, leakSpells.empty() ? "-" : leakSpells,
                c.fallbackClear, c.fallbackHot, c.fallbackKept, c.routeLoops);
            CostStats const& k = st->cost;
            uint32 mechanics = 0;
            uint32 mechanicMax = 0;
            double mechanicMean = 0.0;
            k.PerMechanic(mechanics, mechanicMax, mechanicMean);
            handler->PSendSysMessage("  cost: path calls {} (us mean {} p95 {} max {}), probes {} (us max {}), sight "
                "rays {} (us max {}), lanes: {} candidates, {} path calls in {} solves, path calls per mechanic max {} "
                "mean {:.1f} over {} (+{} unattributed)", k.pathUs.count, k.pathUs.Mean(), k.pathUs.Quantile(0.95),
                k.pathUs.max, k.probeUs.count, k.probeUs.max, k.sightUs.count, k.sightUs.max, k.laneCandidates,
                k.lanePathCalls, k.laneSolves, mechanicMax, mechanicMean, mechanics, k.pathCallsUnattributed);
            handler->PSendSysMessage("  cost us: solve p50 {} p95 {} max {} | route p50 {} p95 {} max {} | brain mean "
                "{} max {} | map update (all bots) mean {} max {} over {}", k.solveUs.Quantile(0.5),
                k.solveUs.Quantile(0.95), k.solveUs.max, k.routeUs.Quantile(0.5), k.routeUs.Quantile(0.95),
                k.routeUs.max, k.brainUs.Mean(), k.brainUs.max,
                k.mapUpdates ? k.mapBrainUsTotal / k.mapUpdates : 0, k.mapBrainUsMax, k.mapUpdates);
            // M4: the lock's by-name gate and id-less casts (STAGE-M4-HANDOFF §3.5, WP-X).
            handler->PSendSysMessage("  m4 lock: by-name blocks {}, chat do let through {}, casts with no spell id {}",
                c.specificBlocks, c.doBypasses, c.unknownCasts);
            std::string tasks;
            for (BotTask const& t : st->viewInfo.tasks)
                tasks += Acore::StringFormat("{}{} {} {} ({} .. {} ms)", tasks.empty() ? "" : "; ", t.mechanicId,
                    KindName(t.kind), BotTaskRoleName(t.role), (t.from - now).count(), (t.to - now).count());
            std::string board;
            if (reg)
                reg->ForEachActive(now, [&](ActiveMechanic const& m)
                {
                    for (Reservation const& r : m.reservations)
                        if (r.bot == bot->GetGUID())
                            board += Acore::StringFormat("{}{} at ({:.1f}, {:.1f}) facing {:.0f} slot {}{} until {} ms",
                                board.empty() ? "" : "; ", m.id, r.point.GetPositionX(), r.point.GetPositionY(),
                                r.facing * 180.f / static_cast<float>(M_PI), r.slot, r.claim ? " claim" : "",
                                (r.until - now).count());
                });
            std::string const look = st->lookOwed ? Acore::StringFormat("{:.0f} deg{}", st->lookFacing * 180.f
                / static_cast<float>(M_PI), st->lookWindow ? " (window)" : "") : std::string("-");
            handler->PSendSysMessage("  tasks: {} | reservations: {} | look-away {} | frontal sources {}{}",
                tasks.empty() ? "-" : tasks, board.empty() ? "-" : board, look, st->frontalSources.size(),
                st->parking ? " | parking" : "");
            // Debug = 1 only (BossMechanics.Bots.Debug): why candidates and navmesh calls failed, summed.
            if (c.debugNav.probes || c.debugNav.calls)
            {
                handler->PSendSysMessage("  debug solve: {}", FormatSolveStats(c.debugSolve));
                handler->PSendSysMessage("  debug nav: {}", FormatNavStats(c.debugNav));
            }
        }
    }

    bool BotReport(ChatHandler* handler, Player* gm, bool allOnMap)
    {
        if (!handler || !gm || !gm->IsInWorld())
            return false;
        if (!BotGate::IsEnabled())
        {
            handler->PSendSysMessage("Bot support is compiled in but disabled (BossMechanics.Bots.Enable was 0 at "
                "startup).");
            return true;
        }

        Map* map = gm->GetMap();
        std::vector<Player*> bots;
        if (allOnMap)
        {
            for (MapReference const& ref : map->GetPlayers())
                if (Player* p = ref.GetSource(); p && p->IsInWorld() && GET_PLAYERBOT_AI(p))
                    bots.push_back(p);
        }
        else
        {
            Group* group = gm->GetGroup();
            if (!group)
            {
                handler->SendErrorMessage("You are not in a group; use .mech bots all for every bot on the map.");
                return false;
            }
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* p = ref->GetSource(); p && p->IsInWorld() && p->GetMap() == map && GET_PLAYERBOT_AI(p))
                    bots.push_back(p);
        }

        Registry const* reg = Registry::Get(map);
        Milliseconds const now = GameTime::GetGameTimeMS();
        handler->PSendSysMessage("Boss-mechanics bots on map {} ({}): {}; registry {} (revision {}, {} mechanics).",
            map->GetId(), allOnMap ? "all" : "group", bots.size(), reg ? "yes" : "no", reg ? reg->Revision() : 0,
            reg ? reg->Size() : 0);
        for (Player* bot : bots)
            ReportOne(handler, bot, reg, now);
        return true;
    }
}

#endif
