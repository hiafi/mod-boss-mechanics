/*
 * mod-boss-mechanics: bots throw and catch a held item or aura (Blackrock Spire build, WP-PS; shared doc BS6, library
 * L26 Hot Potato). The pure rules are MechanicPass.h; the entry is the descriptor's PassEntry.
 *
 * | Kind       | Name             | Class                  | Relevance                                              |
 * | trigger    | boss pass throw  | BossPassThrowTrigger   | checkInterval 1: this bot holds it and a throw is due  |
 * | action     | boss pass throw  | BossPassThrowAction    | ACTION_EMERGENCY + 3: the item on the catcher (or the  |
 * |            |                  |                        | spell, or the aura moved); a plain Action, never a move |
 * | trigger    | boss pass catch  | BossPassCatchTrigger   | checkInterval 1: a ready catcher off its spot           |
 * | action     | boss pass catch  | BossPassCatchAction    | ACTION_RAID + 4: walk to the reserved spot (MoveTo)     |
 * | multiplier | boss pass catch  | BossPassCatchMultiplier| holds a ready catcher on its spot: zeroes the stock     |
 * |            |                  |                        | movement actions (not the brain's, not a taunt swap)    |
 *
 * Plus, outside the engine (AfterTick, from the waker every map update): a holder with a throw due that is casting has
 * its cast broken (the throw outranks it: the burn rises) and is woken; a ready catcher with a walk is woken.
 *
 * The map's book (one per map, Map::CustomData, map thread only) holds one Pass::Slot per entry: the holder, the ready
 * catchers, their reserved spots, the throw due now, and the throw and catch counters. The first bot to look in a game
 * time millisecond brings it up to date (every 100 ms, at once on a change of holder): the holder (the player with the
 * holder aura), its group's catchers (humans too: a throw may go to a human), the FarFrom units and the counted zones.
 * Each bot then reads its own part: the holder bot its throw, a ready catcher bot its spot (probed on the navmesh,
 * against its own hazards and for the holder's sight; reserved on the book so two catchers never take one spot).
 *
 * The throw by item is the pattern of mod-playerbots' Lady Vashj Tainted Core chain (src/Ai/Raid/SSC,
 * LadyVashjPassTheTaintedCoreAction::ThrowCore: PlayerbotAI::ImbueItem on the catcher after a range and sight check,
 * a catcher left out after failed throws), by Crow (pengchengw@me.com), mod-playerbots #1888 and #2892.
 *
 * The names avoid the part `boss mechanic` (STAGE-M5-C-REPORT): a passive bot still throws and walks to catch.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_PASS_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_PASS_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "Action.h"
#include "Duration.h"
#include "MechanicBotStrategy.h"
#include "MechanicPass.h"
#include "MovementActions.h"
#include "Multiplier.h"
#include "Trigger.h"
#include <string>

class Player;
class PlayerbotAI;

namespace BossMechanics
{
    struct MechanicBotState;

    namespace BotPass
    {
        // The bot's decision now: refreshed at most once per game-time ms (brings the descriptor view and the map's
        // book up to date). Empty unless a descriptor with pass entries applies.
        Pass::Decision const& Refresh(PlayerbotAI* botAI, MechanicBotState& st, Milliseconds now);

        // The waker's per-bot step: breaks a holder's cast for a due throw; wakes a holder with a throw due and a
        // ready catcher with a walk.
        void AfterTick(PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Milliseconds now);

        // One `.mech bots` line.
        std::string ReportLine(PlayerbotAI* botAI, MechanicBotState const& st);
    }
}

class BossPassThrowTrigger : public Trigger
{
public:
    explicit BossPassThrowTrigger(PlayerbotAI* botAI) : Trigger(botAI, "boss pass throw", 1) { }

    bool IsActive() override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class BossPassThrowAction : public Action
{
public:
    explicit BossPassThrowAction(PlayerbotAI* botAI) : Action(botAI, "boss pass throw") { }

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class BossPassCatchTrigger : public Trigger
{
public:
    explicit BossPassCatchTrigger(PlayerbotAI* botAI) : Trigger(botAI, "boss pass catch", 1) { }

    bool IsActive() override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class BossPassCatchAction : public MovementAction
{
public:
    explicit BossPassCatchAction(PlayerbotAI* botAI) : MovementAction(botAI, "boss pass catch") { }

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class BossPassCatchMultiplier : public Multiplier
{
public:
    explicit BossPassCatchMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "boss pass catch") { }

    float GetValue(Action* action) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

#endif

#endif
