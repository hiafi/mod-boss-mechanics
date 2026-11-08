/*
 * mod-boss-mechanics: the bots' target priority and ignore list (STAGE-M5-HANDOFF §3.5, WP-T). The pure rules are
 * MechanicTargetRules.h on top of Desc::PickTarget / Desc::IsIgnored (MechanicDescriptor.h).
 *
 * | Kind       | Name             | Class                       | What                                              |
 * | value      | attackers        | MechanicBotAttackersValue   | stock attackers minus ignored units (override)    |
 * | value      | boss kill target | MechanicBotKillTargetValue  | the focus (TargetRules::Focus), 100 ms cache      |
 * | trigger    | boss focus       | MechanicBotFocusTrigger     | an engaged focus that is not the current target   |
 * | action     | boss focus       | MechanicBotFocusAction      | ACTION_RAID + 2: Attack() the pick                |
 * | multiplier | boss focus       | MechanicBotFocusMultiplier  | stands the stock pickers down; zeroes actions on  |
 * |            |                  |                             | ignored units                                     |
 *
 * Plus, outside the engine:
 * - the `attackers` override feeds every picker and every AoE count (EnemyCountValues, HasEnoughAoeTargets); the
 *   module's contexts are appended to the class lists on the first world tick, after playerbots built them, and the
 *   last Add of a name wins (NamedObjectContext.h SharedNamedObjectContextList::Add). The waker logs once per bot at
 *   Debug which class its `attackers` value is;
 * - the strategy's target exclusions (MechanicBotStrategy::AppendTargetExclusions): the ignored encounter units;
 * - AttackersValue::SetTargetFilter (fork macro PLAYERBOTS_HAS_TARGET_FILTER): IsPossibleTarget drops ignored units,
 *   so RTI, skull and assist targets are filtered too;
 * - AfterTick (from the waker, once per bot and map update, so also while the bot casts): drops an ignored current
 *   target, melee victim or cast target, pulls the bot's pets, guardians and totems off ignored units, and samples
 *   the lower-priority and break-out clocks;
 * - a UnitScript OnDamage hook counts damage the bot and its minions deal to ignored units.
 *
 * Turned teammates (SFK F1, Descriptor::hostilePlayerAuras): AttackersValue::SetHostileTeammateFilter (fork macro
 * PLAYERBOTS_HAS_HOSTILE_TEAMMATE_FILTER) tells the fork which charmed teammates are hostile: the fork then lists
 * them as attackers and possible targets, accepts them as the current target, and leaves them out of its party
 * member values (buffs, cures, the AoE heal count; PartyMemberToHeal skips every charmed player already). The focus
 * ranks them through TargetEntry::HostilePlayers; the cure walk (MechanicBotDispel) skips them; AfterTick drops a
 * teammate that is no longer hostile (aura gone, charm still on) as a target.
 *
 * Focus pattern adapted from mod-playerbots' Magisters' Terrace strategy (src/Ai/Dungeon/MgT: MgTFocusAction,
 * MgTFocusOrderMultiplier).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_TARGETS_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_TARGETS_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "AttackAction.h"
#include "AttackersValue.h"
#include "Duration.h"
#include "MechanicBotStrategy.h"
#include "MechanicTargetRules.h"
#include "Multiplier.h"
#include "ObjectGuid.h"
#include "Trigger.h"
#include "Value.h"
#include <string>

class Player;
class PlayerbotAI;
class Unit;

namespace BossMechanics
{
    class Registry;
    struct MechanicBotState;

    namespace BotTargets
    {
        // Is `unit` on the active descriptor's ignore list now, for this bot's view? Does not refresh the view.
        bool IsIgnoredNow(MechanicBotState& st, Registry const& reg, Player const* bot, Unit const* unit,
            Milliseconds now);

        // SFK F1: is `unit` a teammate turned against the group for this bot now: a charmed player carrying one of the
        // active descriptor's hostilePlayerAuras (Desc::IsHostilePlayer)? Cheap for anything but a charmed player.
        // Does not refresh the view (the waker does, every map update).
        bool IsHostilePlayer(PlayerbotAI* botAI, Player const* bot, Unit const* unit);
        // The waker's per-bot step (MechanicBotWaker.cpp, every map update, on the map thread).
        void AfterTick(PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Registry const& reg, Milliseconds now);

        // MechanicBotStrategy::AppendTargetExclusions: the owner and tracked units that are ignored now.
        void AppendExclusions(PlayerbotAI* botAI, GuidSet& exclusions);

        // From the glue's latch (world thread): AttackersValue::SetTargetFilter and SetHostileTeammateFilter when the
        // fork has them, else nothing.
        void InstallTargetFilter();

        // One `.mech bots` line.
        std::string ReportLine(MechanicBotState const& st);
    }
}

// The damage hook on ignored units (AddSC_boss_mechanics_bots registers it).
void AddSC_boss_mechanics_bot_targets();

class MechanicBotAttackersValue : public AttackersValue
{
public:
    explicit MechanicBotAttackersValue(PlayerbotAI* botAI) : AttackersValue(botAI) { }

    GuidVector Calculate() override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class MechanicBotKillTargetValue : public CalculatedValue<BossMechanics::TargetRules::Focus>
{
public:
    explicit MechanicBotKillTargetValue(PlayerbotAI* botAI)
        : CalculatedValue<BossMechanics::TargetRules::Focus>(botAI, "boss kill target", 100) { }

    std::string const Format() override;

protected:
    BossMechanics::TargetRules::Focus Calculate() override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class MechanicBotFocusTrigger : public Trigger
{
public:
    explicit MechanicBotFocusTrigger(PlayerbotAI* botAI) : Trigger(botAI, "boss focus", 1) { }

    bool IsActive() override;
};

class MechanicBotFocusAction : public AttackAction
{
public:
    explicit MechanicBotFocusAction(PlayerbotAI* botAI) : AttackAction(botAI, "boss focus") { }

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class MechanicBotFocusMultiplier : public Multiplier
{
public:
    explicit MechanicBotFocusMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "boss focus") { }

    float GetValue(Action* action) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

#endif

#endif
