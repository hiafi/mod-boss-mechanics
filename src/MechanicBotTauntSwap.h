/*
 * mod-boss-mechanics: bots trade a boss at a debuff's stack count (Blackrock Spire build, WP-TS; designer rulings
 * 62-64, shared doc BS7). The pure rules are MechanicTauntSwap.h; the entry is the descriptor's TauntSwapEntry.
 *
 * | Kind       | Name             | Class                     | Relevance                                         |
 * | trigger    | boss taunt swap  | BossTauntSwapTrigger      | checkInterval 1: this tank is picked to taunt     |
 * | action     | boss taunt swap  | BossTauntSwapAction       | ACTION_RAID + 3: the taunt (walks in when out of  |
 * |            |                  |                           | reach), then the boss as its target               |
 * | multiplier | boss taunt swap  | BossTauntSwapMultiplier   | the hold-off: zeroes a tank's actions at a boss   |
 * |            |                  |                           | another tank holds (kicks excepted) and its area  |
 * |            |                  |                           | taunts while it keeps off one                     |
 *
 * Plus, outside the engine:
 * - AfterTick (from the waker, every map update): a tank that keeps off a boss stops its melee on it, drops it as
 *   its current target and breaks a cast at it (not a kick);
 * - the strategy's target exclusions (tank, dps and debuff targets): the bosses the tank keeps off, so the stock
 *   pickers do not walk it back;
 * - the `boss kill target` focus (MechanicBotTargets.cpp): a tank keeps the swap boss it holds (TauntSwap::HeldFocus)
 *   over the list's picks, and the bosses it keeps off are no candidates.
 *
 * Only tanks decide (PlayerbotAI::IsTank); the tanks are the bot's group members on its map, humans included (a human
 * tank picked to taunt is left to taunt by hand: bots never taunt over it). Taunts are classified and cast by spell id
 * (TauntSwap::kTable), so the reworked classes' taunts (Insult, Demonic Taunt, Earth Shock under Rockbiter) count;
 * playerbots' IsTauntAction knows only the stock action classes. No mod-playerbots edit.
 *
 * The names avoid the part `boss mechanic` (STAGE-M5-C-REPORT): a passive bot does not taunt.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_TAUNT_SWAP_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_TAUNT_SWAP_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "AttackAction.h"
#include "Duration.h"
#include "MechanicBotStrategy.h"
#include "MechanicTauntSwap.h"
#include "Multiplier.h"
#include "Strategy.h"
#include "Trigger.h"
#include <string>

class Player;
class PlayerbotAI;

namespace BossMechanics
{
    struct MechanicBotState;

    namespace BotTauntSwap
    {
        // The bot's decision now: refreshed at most once per game-time ms (brings the descriptor view up to date).
        // Empty unless a descriptor with taunt swap entries applies and the bot is a tank.
        TauntSwap::Decision const& Refresh(PlayerbotAI* botAI, MechanicBotState& st, Milliseconds now);

        // The waker's per-bot step: keeps a held-off tank's melee, target and casts off the bosses it keeps off.
        void AfterTick(PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Milliseconds now);

        // MechanicBotStrategy::AppendTargetExclusions: the bosses the bot keeps off (tank, dps and debuff targets).
        void AppendExclusions(PlayerbotAI* botAI, GuidSet& exclusions, TargetValueExclusionType type);

        // One `.mech bots` line.
        std::string ReportLine(MechanicBotState const& st);
    }
}

class BossTauntSwapTrigger : public Trigger
{
public:
    explicit BossTauntSwapTrigger(PlayerbotAI* botAI) : Trigger(botAI, "boss taunt swap", 1) { }

    bool IsActive() override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class BossTauntSwapAction : public AttackAction
{
public:
    explicit BossTauntSwapAction(PlayerbotAI* botAI) : AttackAction(botAI, "boss taunt swap") { }

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class BossTauntSwapMultiplier : public Multiplier
{
public:
    explicit BossTauntSwapMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "boss taunt swap") { }

    float GetValue(Action* action) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

#endif

#endif
