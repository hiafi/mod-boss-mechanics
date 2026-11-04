/*
 * mod-boss-mechanics: the drill runner (STAGE-M4-HANDOFF §3.2, WP-B).
 *
 * `.mech drill`, the training dummy and the dpssim mechanics mode all run drills through RunDrill: every step is a
 * `.mech test` line parsed against a ParseCtx (the reference unit that stands in for the GM: `at=gm`, `anchor=me`,
 * `to=me` and the facing of lanes, lines and `shift=`) and registered later from a scheduler. Implemented in
 * MechanicCommand.cpp, next to the `.mech test` parser it shares.
 *
 * Scope: `scopePlayer` is added to the owner encounter's scope set (Encounter::testGms), which also marks the
 * encounter as one that wants bots (the open-world bot gate, MechanicBotGate.cpp). Member picks land on the scope
 * player's group (raid) on the owner's map, in guid order; the scope player itself only with
 * DrillOptions::includeScopePlayer. Registry targeting: the Mechanic Test Owner's (300401) and the training dummy's
 * (300402) mechanics are limited to the scope set and its groups (Registry::IsInTestScope); any other owner (the
 * sim's boss stand-in) reaches every candidate (instance: the map, open world:
 * BossMechanics.OpenWorld.CandidateRadius).
 *
 * Programs (StartProgram): an entry that fails at its start is skipped and reported (LOG_ERROR, a whisper to the
 * training dummy's scope player, and Encounter::programErrors, which the sim takes with Sim::TakeProgramErrors).
 *
 * Scheduler: an owner with a MarkerAI (the Mechanic Test Owner) schedules on its own TaskScheduler, exactly as
 * `.mech drill` always did (its idle clock and `.mech clear` despawn end it). Any other owner schedules on its
 * registry encounter (Registry::Schedule), so the steps go when the encounter ends (evade, death, despawn,
 * StopProgram) and need no particular AI on the owner.
 *
 * Threads: the owner's map thread, or the world thread while no map updates (commands, the sim's start path).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DRILL_RUNNER_H
#define MOD_BOSS_MECHANICS_MECHANIC_DRILL_RUNNER_H

#include "Define.h"
#include "MechanicDrillPlan.h"
#include "ObjectGuid.h"
#include <string>
#include <string_view>

class Creature;
class Unit;

namespace BossMechanics
{
    // What a run did, for the command's reply.
    struct DrillResult
    {
        uint32 seed = 0;        // the seed of the picks (repeat it with DrillOptions::seed)
        uint32 cycles = 0;
        uint32 steps = 0;       // steps per cycle
    };

    // Runs `def` on `owner` (the encounter owner and, for Bait, the measuring point) with `boss` as the stand-in the
    // steps select (frontal source, `anchor=target`, `pick=nontank`'s victim, `park=`). `owner` may be `boss`. The
    // reference of the lines is the scope player (it must be on the owner's map). Every step is parsed before
    // anything registers: false with `err` set (the command's wording) when a step is bad, the scope player is gone,
    // or the scope group has no alive member to land on.
    bool RunDrill(Creature* owner, Unit* boss, ObjectGuid scopePlayer, Drill::Def const& def, DrillOptions const& opt,
        DrillResult& out, std::string& err);

    // Starts `program` (Drill::ParseProgram) on `owner`, which is also the boss stand-in, scoped to `scopePlayer`'s
    // group: begins the owner's encounter, adds the scope player, then schedules the entries on the encounter (and
    // the next loop with DrillOptions::loop). Drill entries run through RunDrill at their start. A program already
    // running on `owner` has its pending steps dropped; its encounter, live mechanics and ledger are kept (StopProgram
    // ends them). `none` begins the encounter and scope only.
    bool StartProgram(Creature* owner, ObjectGuid scopePlayer, std::string_view program, DrillOptions const& opt,
        std::string& err);

    // Ends `owner`'s encounter: pending steps, mechanics, markers, ledger and the Weakened it stacked.
    void StopProgram(Creature* owner);
}

#endif
