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

#include "MechanicBotDispel.h"
#include "MechanicBotInterrupt.h"
#include "MechanicBotState.h"
#include "MechanicBotStrategy.h"
#include "MechanicBotTargets.h"
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
        creators["boss mechanic face"] = &MechanicBotActionContext::boss_mechanic_face;
        // M5 WP-P: dispels and purges (MechanicBotDispel.h).
        creators["boss dispel urgent"] = &MechanicBotActionContext::boss_dispel_urgent;
        creators["boss purge"] = &MechanicBotActionContext::boss_purge;
        // M5 WP-T (MechanicBotTargets.h).
        creators["boss focus"] = &MechanicBotActionContext::boss_focus;
        // M5 WP-I: interrupts (MechanicBotInterrupt.h).
        creators["boss kick"] = &MechanicBotActionContext::boss_kick;
    }

private:
    static Action* boss_mechanic_move(PlayerbotAI* botAI) { return new MechanicBotMoveAction(botAI); }
    static Action* boss_mechanic_hold(PlayerbotAI* botAI) { return new MechanicBotHoldAction(botAI); }
    static Action* boss_mechanic_face(PlayerbotAI* botAI) { return new MechanicBotFaceAction(botAI); }
    static Action* boss_dispel_urgent(PlayerbotAI* botAI) { return new MechanicBotDispelUrgentAction(botAI); }
    static Action* boss_purge(PlayerbotAI* botAI) { return new MechanicBotPurgeAction(botAI); }
    static Action* boss_focus(PlayerbotAI* botAI) { return new MechanicBotFocusAction(botAI); }
    static Action* boss_kick(PlayerbotAI* botAI) { return new BossKickAction(botAI); }
};

class MechanicBotTriggerContext : public NamedObjectContext<Trigger>
{
public:
    MechanicBotTriggerContext() : NamedObjectContext<Trigger>(false, false)
    {
        creators["boss mechanic reposition"] = &MechanicBotTriggerContext::boss_mechanic_reposition;
        creators["boss mechanic hold"] = &MechanicBotTriggerContext::boss_mechanic_hold;
        creators["boss mechanic face"] = &MechanicBotTriggerContext::boss_mechanic_face;
        // M5 WP-P: dispels and purges (MechanicBotDispel.h).
        creators["boss dispel urgent"] = &MechanicBotTriggerContext::boss_dispel_urgent;
        creators["boss purge"] = &MechanicBotTriggerContext::boss_purge;
        // M5 WP-T (MechanicBotTargets.h).
        creators["boss focus"] = &MechanicBotTriggerContext::boss_focus;
        // M5 WP-I: interrupts (MechanicBotInterrupt.h).
        creators["boss interrupt due"] = &MechanicBotTriggerContext::boss_interrupt_due;
    }

private:
    static Trigger* boss_mechanic_reposition(PlayerbotAI* botAI) { return new MechanicBotRepositionTrigger(botAI); }
    static Trigger* boss_mechanic_hold(PlayerbotAI* botAI) { return new MechanicBotHoldTrigger(botAI); }
    static Trigger* boss_mechanic_face(PlayerbotAI* botAI) { return new MechanicBotFaceTrigger(botAI); }
    static Trigger* boss_dispel_urgent(PlayerbotAI* botAI) { return new MechanicBotDispelUrgentTrigger(botAI); }
    static Trigger* boss_purge(PlayerbotAI* botAI) { return new MechanicBotPurgeTrigger(botAI); }
    static Trigger* boss_focus(PlayerbotAI* botAI) { return new MechanicBotFocusTrigger(botAI); }
    static Trigger* boss_interrupt_due(PlayerbotAI* botAI) { return new BossInterruptDueTrigger(botAI); }
};

class MechanicBotValueContext : public NamedObjectContext<UntypedValue>
{
public:
    MechanicBotValueContext() : NamedObjectContext<UntypedValue>(false, false)
    {
        creators["boss mechanic state"] = &MechanicBotValueContext::boss_mechanic_state;
        // M5 WP-P: overrides the stock value of the same name (this context is added after playerbots' own, so
        // the last Add wins; `.mech bots` says which one a bot holds).
        creators["party member to dispel"] = &MechanicBotValueContext::party_member_to_dispel;
        // M5 WP-T (MechanicBotTargets.h). "attackers" overrides playerbots' own: these contexts are appended to the
        // class lists after playerbots built them, and the last Add of a name wins.
        creators["attackers"] = &MechanicBotValueContext::attackers;
        creators["boss kill target"] = &MechanicBotValueContext::boss_kill_target;
        // M5 WP-I (MechanicBotInterrupt.h).
        creators["boss interrupt target"] = &MechanicBotValueContext::boss_interrupt_target;
    }

private:
    static UntypedValue* boss_mechanic_state(PlayerbotAI* botAI)
    {
        return new BossMechanics::MechanicBotStateValue(botAI);
    }
    static UntypedValue* party_member_to_dispel(PlayerbotAI* botAI) { return new MechanicBotDispelTargetValue(botAI); }
    static UntypedValue* attackers(PlayerbotAI* botAI) { return new MechanicBotAttackersValue(botAI); }
    static UntypedValue* boss_kill_target(PlayerbotAI* botAI) { return new MechanicBotKillTargetValue(botAI); }
    static UntypedValue* boss_interrupt_target(PlayerbotAI* botAI) { return new BossInterruptTargetValue(botAI); }
};

#endif

#endif
