/*
 * mod-boss-mechanics: the "boss mechanics" bot strategy, its triggers and actions (STAGE-M2-HANDOFF §3.2, §3.4, WP-C).
 *
 * | Kind     | Name                       | Class                         | Relevance            |
 * | strategy | boss mechanics             | MechanicBotStrategy (generic) | combat + non-combat  |
 * | trigger  | boss mechanic reposition   | MechanicBotRepositionTrigger  | checkInterval 1      |
 * | trigger  | boss mechanic hold         | MechanicBotHoldTrigger        | checkInterval 1      |
 * | action   | boss mechanic move         | MechanicBotMoveAction         | ACTION_EMERGENCY + 6 |
 * | action   | boss mechanic hold         | MechanicBotHoldAction         | ACTION_EMERGENCY + 5 |
 * | trigger  | boss mechanic face         | MechanicBotFaceTrigger        | checkInterval 1      |
 * | action   | boss mechanic face         | MechanicBotFaceAction         | ACTION_EMERGENCY + 4 |
 * | (mult.)  | boss mechanic lock         | MechanicBotLockMultiplier     | MechanicBotLock.h    |
 * | trigger  | boss focus                 | MechanicBotFocusTrigger       | M5 WP-T, see         |
 * | action   | boss focus                 | MechanicBotFocusAction        | ACTION_RAID + 2      |
 * | (mult.)  | boss focus                 | MechanicBotFocusMultiplier    | MechanicBotTargets.h |
 *
 * The triggers run the brain (MechanicBotBrain::Update, once per game-time ms) and read the mode it leaves. The
 * move action is a MovementAction so it can use the protected MoveTo; the lock multiplier exempts it by type (and
 * drops it while the bot is frozen). The face trigger and action (M3, STAGE-M3-HANDOFF §3.8) hold the look-away
 * facing.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_STRATEGY_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_STRATEGY_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "Action.h"
#include "MovementActions.h"
#include "Strategy.h"
#include "Trigger.h"

namespace BossMechanics
{
    struct MechanicBotState;

    // Resolves and caches the bot's state value. The cached pointer is to the value object in the same bot's
    // AiObjectContext (it lives as long as the trigger or action that holds it), never to a game object.
    class MechanicBotStateRef
    {
    public:
        MechanicBotState* Get(PlayerbotAI* botAI);

    private:
        MechanicBotState* _state = nullptr;
    };
}

class MechanicBotStrategy : public Strategy
{
public:
    explicit MechanicBotStrategy(PlayerbotAI* botAI) : Strategy(botAI) { }

    std::string const getName() override { return "boss mechanics"; }
    uint32 GetType() const override { return STRATEGY_TYPE_GENERIC; }
    // The base Strategy has no triggers or multipliers of its own: nothing to chain.
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    // M5 WP-T: the ignored encounter units (MechanicBotTargets.h). Init reads HasTargetExclusions once.
    bool HasTargetExclusions() const override { return true; }
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;
};

// Escape: active while the plan's move is not under way (not issued yet, re-planned, or the spline ended short).
class MechanicBotRepositionTrigger : public Trigger
{
public:
    explicit MechanicBotRepositionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "boss mechanic reposition", 1) { }

    bool IsActive() override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

// Hold / Freeze: active while something other than us moves the bot (follow, chase, a stock MovePoint).
class MechanicBotHoldTrigger : public Trigger
{
public:
    explicit MechanicBotHoldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "boss mechanic hold", 1) { }

    bool IsActive() override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

// Face (M3): active while a look-away facing is owed and the bot is off it by more than 5 degrees.
class MechanicBotFaceTrigger : public Trigger
{
public:
    explicit MechanicBotFaceTrigger(PlayerbotAI* botAI) : Trigger(botAI, "boss mechanic face", 1) { }

    bool IsActive() override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class MechanicBotMoveAction : public MovementAction
{
public:
    explicit MechanicBotMoveAction(PlayerbotAI* botAI) : MovementAction(botAI, "boss mechanic move") { }

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class MechanicBotHoldAction : public Action
{
public:
    explicit MechanicBotHoldAction(PlayerbotAI* botAI) : Action(botAI, "boss mechanic hold") { }

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

// Turns the bot to the owed facing (SetOrientation + SendMovementFlagUpdate, as ServerFacade::SetFacingTo).
class MechanicBotFaceAction : public Action
{
public:
    explicit MechanicBotFaceAction(PlayerbotAI* botAI) : Action(botAI, "boss mechanic face") { }

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

#endif

#endif
