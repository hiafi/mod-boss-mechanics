/*
 * mod-boss-mechanics: the four playerbots named-object contexts of the boss-mechanics glue (STAGE-M2-HANDOFF §3.2,
 * §3.3, WP-C). MechanicBotGate appends one fresh set to each of the ten class context lists on the first world tick.
 *
 * Pattern adapted from mod-dungeon-clear (Jared Wright, AGPL-3.0-or-later), DungeonClearStrategyContext.h and
 * DungeonClearValueContext.h: NamedObjectContext<T>(false, false) with creators["name"] = &Ctx::fn.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_CONTEXTS_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_CONTEXTS_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotState.h"
#include "MechanicBotStrategy.h"
#include "NamedObjectContext.h"

class MechanicBotStrategyContext : public NamedObjectContext<Strategy>
{
public:
    MechanicBotStrategyContext() : NamedObjectContext<Strategy>(false, false)
    {
        creators["boss mechanics"] = &MechanicBotStrategyContext::boss_mechanics;
    }

private:
    static Strategy* boss_mechanics(PlayerbotAI* botAI) { return new MechanicBotStrategy(botAI); }
};

class MechanicBotActionContext : public NamedObjectContext<Action>
{
public:
    MechanicBotActionContext() : NamedObjectContext<Action>(false, false)
    {
        creators["boss mechanic move"] = &MechanicBotActionContext::boss_mechanic_move;
        creators["boss mechanic hold"] = &MechanicBotActionContext::boss_mechanic_hold;
    }

private:
    static Action* boss_mechanic_move(PlayerbotAI* botAI) { return new MechanicBotMoveAction(botAI); }
    static Action* boss_mechanic_hold(PlayerbotAI* botAI) { return new MechanicBotHoldAction(botAI); }
};

class MechanicBotTriggerContext : public NamedObjectContext<Trigger>
{
public:
    MechanicBotTriggerContext() : NamedObjectContext<Trigger>(false, false)
    {
        creators["boss mechanic reposition"] = &MechanicBotTriggerContext::boss_mechanic_reposition;
        creators["boss mechanic hold"] = &MechanicBotTriggerContext::boss_mechanic_hold;
    }

private:
    static Trigger* boss_mechanic_reposition(PlayerbotAI* botAI) { return new MechanicBotRepositionTrigger(botAI); }
    static Trigger* boss_mechanic_hold(PlayerbotAI* botAI) { return new MechanicBotHoldTrigger(botAI); }
};

class MechanicBotValueContext : public NamedObjectContext<UntypedValue>
{
public:
    MechanicBotValueContext() : NamedObjectContext<UntypedValue>(false, false)
    {
        creators["boss mechanic state"] = &MechanicBotValueContext::boss_mechanic_state;
    }

private:
    static UntypedValue* boss_mechanic_state(PlayerbotAI* botAI)
    {
        return new BossMechanics::MechanicBotStateValue(botAI);
    }
};

#endif

#endif
