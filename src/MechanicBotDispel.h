/*
 * mod-boss-mechanics: the bots' boss dispels and purges (STAGE-M5-HANDOFF §3.4, decisions §7.1 #3 and #5, WP-P).
 *
 * | Kind    | Name                     | Class                          | Relevance             |
 * | value   | party member to dispel   | MechanicBotDispelTargetValue   | overrides the stock   |
 * | trigger | boss dispel urgent       | MechanicBotDispelUrgentTrigger | checkInterval 1       |
 * | action  | boss dispel urgent       | MechanicBotDispelUrgentAction  | ACTION_DISPEL + 5     |
 * | trigger | boss purge               | MechanicBotPurgeTrigger        | checkInterval 1       |
 * | action  | boss purge               | MechanicBotPurgeAction         | ACTION_INTERRUPT - 1  |
 *
 * - `party member to dispel` (qualifier: the DispelType number, as the stock cure triggers and actions ask for it):
 *   the module's value context registers the same name after playerbots' own (the first world tick, MechanicBotGate),
 *   so the last Add wins for every class. With an active descriptor that lists dispels, a member carrying a listed
 *   aura of that type is returned first (Desc::OrderDispels: urgent, priority, time left, stock rank); otherwise, and
 *   always without a descriptor, the stock PartyMemberToDispel::Calculate answers unchanged.
 * - `boss dispel urgent`: an urgent (loss of control) entry, or a deadline entry inside its deadline, on a group
 *   member, cured with the bot's own spell of the right type from the class table (MechanicDispelRules.h), any class.
 *   Its name matches none of the healer cure gates (Priest/Shaman/Druid STOCK_CURE_NAMES, Paladin's "cleanse" /
 *   "purify" prefixes), so the 50% gate does not hold it (decision §7.1 #3). Normal entries stay on the stock rows.
 * - `boss purge`: a listed buff (PurgeEntry) on the encounter's owner or one of its tracked units, in range, with the
 *   class's purge (Purge, offensive Dispel Magic, Spellsteal, Tranquilizing Shot, the Felhunter's Devour Magic).
 * - Counters (MechanicBotCounters, WP-P block): the cast is noted (a cast hook for the bot's own spells, the action
 *   for its pet's), whichever row cast it, and counted by the aura its dispel removed (an aura hook; the core picks
 *   that aura at random among the target's auras of the type, M5 WP-F); deadline misses on the member whose aura
 *   expired (the same hook).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_DISPEL_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_DISPEL_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "Action.h"
#include "MechanicBotStrategy.h"
#include "ObjectGuid.h"
#include "PartyMemberToDispel.h"
#include "Trigger.h"
#include <string>

namespace BossMechanics
{
    struct MechanicBotState;

    namespace BotDispel
    {
        // The `.mech bots` line: the counters and which `party member to dispel` the bot holds.
        std::string ReportLine(PlayerbotAI* botAI, MechanicBotState const& st);

        // The cast and aura hooks (from AddSC_boss_mechanics_bots).
        void AddScripts();

        // M5 CD (E): the waker's dispel signature (LockRules::MixWakeSignature): the listed auras on the group (and
        // its pets) that are urgent now (an `urgent` entry, or a deadline entry inside its deadline) and of a type the
        // bot can cure; 0 = none, or no such entry, or no cure. Bot's map thread.
        uint64 WakeSignature(Player* bot, MechanicBotState const& st);
    }
}

class MechanicBotDispelTargetValue : public PartyMemberToDispel
{
public:
    explicit MechanicBotDispelTargetValue(PlayerbotAI* botAI);

    Unit* Get() override;

protected:
    Unit* Calculate() override;

private:
    ObjectGuid _pick;       // the listed pick of the last Calculate (empty: none, or the stock answer)
    uint32 _pickAura = 0;
};

class MechanicBotDispelUrgentTrigger : public Trigger
{
public:
    explicit MechanicBotDispelUrgentTrigger(PlayerbotAI* botAI) : Trigger(botAI, "boss dispel urgent", 1) { }

    bool IsActive() override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class MechanicBotDispelUrgentAction : public Action
{
public:
    explicit MechanicBotDispelUrgentAction(PlayerbotAI* botAI) : Action(botAI, "boss dispel urgent") { }

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class MechanicBotPurgeTrigger : public Trigger
{
public:
    explicit MechanicBotPurgeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "boss purge", 1) { }

    bool IsActive() override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class MechanicBotPurgeAction : public Action
{
public:
    explicit MechanicBotPurgeAction(PlayerbotAI* botAI) : Action(botAI, "boss purge") { }

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

#endif

#endif
