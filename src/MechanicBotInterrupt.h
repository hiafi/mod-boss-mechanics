/*
 * mod-boss-mechanics: bots kick a boss's interrupt list (STAGE-M5-HANDOFF §3.3, decision §7.1 #2, WP-I).
 *
 * | Kind     | Name                   | Class                         | Relevance              |
 * | value    | boss interrupt target  | BossInterruptTargetValue      | the caster this bot kicks now (nullptr: none) |
 * | trigger  | boss interrupt due     | BossInterruptDueTrigger       | checkInterval 1        |
 * | action   | boss kick              | BossKickAction                | ACTION_INTERRUPT + 5   |
 * | (mult.)  | boss kick order        | MechanicBotKickMultiplier     | the stock kick rows    |
 *
 * Only while a descriptor with an interrupt list applies to the bot (MechanicDescriptorView); otherwise the value is
 * nullptr, the trigger is off and the multiplier lets every action through, so stock behavior is untouched.
 *
 * - The plan (BotKick::Refresh, once per bot and game-time ms): the interruptible casts of the encounter's units
 *   (owner + tracked units), ordered by Desc::OrderInterrupts (do-not-kick, unlisted and claimed casts left out); the
 *   group's kickers (bots with the strategy and a kick from the table, MechanicKickRules.h) with when their kick is
 *   ready and whether it reaches each caster; Kick::Assign (lowest key first, the overload hold, deferral). A listed
 *   cast seen starting is noted (Registry::NoteCastStarted) so its `every` cadence and its claim key are the same
 *   for every bot.
 * - `boss kick` claims the cast (Registry::ClaimCast until the cast's end plus a grace) and casts the bot's kick: the
 *   bot's own through PlayerbotAI::CastSpell, Spell Lock with the Felhunter as the caster (MechanicKickRules.h).
 * - The multiplier zeroes a stock kick (CastSpellAction whose spell id is in the table) aimed at a unit casting a
 *   do-not-kick cast, a cast another bot claimed, a listed cast (`boss kick` owns those: the order, the hold and the
 *   claim live there), or an unlisted cast while a listed cast is due before the kick is back.
 * - The player cast hook (BotKick::OnBotSpellCast) judges every kick a bot casts, whatever row cast it, and keeps the
 *   counters (MechanicBotCounters, WP-I block); a stock kick on a listed cast also takes the claim, so a second one
 *   is counted as a duplicate.
 *
 * The names avoid the part `boss mechanic` (STAGE-M5-C-REPORT): a passive bot does not kick.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_INTERRUPT_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_INTERRUPT_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "Action.h"
#include "Duration.h"
#include "MechanicBotStrategy.h"
#include "Multiplier.h"
#include "Trigger.h"
#include "Value.h"
#include <string>

class Player;
class PlayerbotAI;
class Spell;
class Unit;

namespace BossMechanics
{
    struct MechanicBotState;

    namespace BotKick
    {
        // Brings st.kick up to date for `now` (idempotent within a game-time ms). Bot's map thread.
        void Refresh(PlayerbotAI* botAI, MechanicBotState& st, Milliseconds now);

        // PlayerScript::OnPlayerSpellCast for bots: a kick from the table judged on what it stops (counters, claim).
        void OnBotSpellCast(Player* player, Spell const* spell);

        // The `.mech bots` line of the interrupt counters and the current plan.
        std::string ReportLine(MechanicBotState const& st);

        // M5 CD (E): the waker's kick signature (LockRules::MixWakeSignature): the casts in progress on the
        // encounter's units that the bot's interrupt list lists with a priority (caster entry and spell; the `when`
        // condition and interruptibility are left to the kick plan), when the bot has a kick (its last plan); 0 =
        // none. Cheap: the owner and the tracked units, two spell slots each. Bot's map thread.
        uint64 WakeSignature(Player* bot, MechanicBotState const& st);
    }
}

class BossInterruptTargetValue : public CalculatedValue<Unit*>
{
public:
    explicit BossInterruptTargetValue(PlayerbotAI* botAI)
        : CalculatedValue<Unit*>(botAI, "boss interrupt target", 1) { }

    Unit* Calculate() override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class BossInterruptDueTrigger : public Trigger
{
public:
    explicit BossInterruptDueTrigger(PlayerbotAI* botAI) : Trigger(botAI, "boss interrupt due", 1) { }

    bool IsActive() override;
};

class BossKickAction : public Action
{
public:
    explicit BossKickAction(PlayerbotAI* botAI) : Action(botAI, "boss kick") { }

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

class MechanicBotKickMultiplier : public Multiplier
{
public:
    explicit MechanicBotKickMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "boss kick order") { }

    float GetValue(Action* action) override;

private:
    BossMechanics::MechanicBotStateRef _state;
};

#endif

#endif
