/*
 * mod-boss-mechanics: npc_boss_mechanic_marker (Mechanic Marker 300400, Mechanic Test Owner 300401).
 */

#include "MechanicMarkerAI.h"
#include "BossMechanicsIds.h"
#include "Creature.h"
#include "CreatureScript.h"
#include "GameTime.h"
#include "MechanicRegistry.h"

namespace BossMechanics
{
    namespace
    {
        constexpr Milliseconds kIdleCheckInterval = Seconds(5);
    }

    MarkerAI::MarkerAI(Creature* creature) : NullCreatureAI(creature)
    {
        _lastBusy = GameTime::GetGameTimeMS();
        if (IsTestOwner())
            scheduler.Schedule(kIdleCheckInterval, [this](TaskContext ctx)
            {
                CheckIdle();
                ctx.Repeat(kIdleCheckInterval);
            });
    }

    bool MarkerAI::IsTestOwner() const
    {
        return me->GetEntry() == Ids::NPC_MECHANIC_TEST_OWNER;
    }

    void MarkerAI::UpdateAI(uint32 diff)
    {
        scheduler.Update(diff);
    }

    void MarkerAI::ScheduleTest(Milliseconds delay, std::function<void(Creature* owner)> fn)
    {
        if (!fn)
            return;
        _lastBusy = GameTime::GetGameTimeMS() + delay;
        scheduler.Schedule(delay, [this, fn = std::move(fn)](TaskContext /*ctx*/)
        {
            fn(me);
        });
    }

    void MarkerAI::CheckIdle()
    {
        Milliseconds const now = GameTime::GetGameTimeMS();
        if (Registry const* reg = Registry::Get(me->GetMap()))
        {
            bool busy = false;
            ObjectGuid const self = me->GetGUID();
            reg->ForEachActive(now, [&busy, &self](ActiveMechanic const& m)
            {
                if (m.owner == self)
                    busy = true;
            });
            if (busy)
                _lastBusy = now;
        }
        if (now >= _lastBusy + kTestOwnerIdle)
            me->DespawnOrUnsummon();
    }
}

void AddSC_boss_mechanics_marker()
{
    new GenericCreatureScript<BossMechanics::MarkerAI>("npc_boss_mechanic_marker");
}
