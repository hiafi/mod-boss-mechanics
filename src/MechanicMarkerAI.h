/*
 * mod-boss-mechanics: AI of the Mechanic Marker (300400) and the Mechanic Test Owner (300401), ScriptName
 * `npc_boss_mechanic_marker` (STAGE-M1-HANDOFF §3.8, §3.10).
 *
 * Both are passive and never evade, attack or react (NullCreatureAI). The marker carries no state of its own: the
 * telegraph code (MechanicTelegraph.cpp) owns placement and follow. The test owner adds two things for `.mech test`:
 * delayed registrations (`repeat=`/`every=`) run from its scheduler, and it despawns itself after
 * kTestOwnerIdle with no live mechanic, which ends its encounter (markers, Weakened) through the hooks.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_MARKER_AI_H
#define MOD_BOSS_MECHANICS_MECHANIC_MARKER_AI_H

#include "Duration.h"
#include "PassiveAI.h"
#include <functional>

class Creature;

namespace BossMechanics
{
    // `.mech test`: the test owner despawns after this long without a live mechanic (handoff §3.10).
    constexpr Milliseconds kTestOwnerIdle = Minutes(5);

    class MarkerAI : public NullCreatureAI
    {
    public:
        explicit MarkerAI(Creature* creature);

        void UpdateAI(uint32 diff) override;

        // Test owner only: runs `fn(me)` after `delay` from this creature's update (same map thread). The closure must
        // capture guids, never unit pointers.
        void ScheduleTest(Milliseconds delay, std::function<void(Creature* owner)> fn);

    private:
        bool IsTestOwner() const;
        void CheckIdle();

        Milliseconds _lastBusy{ 0 };
    };
}

#endif
