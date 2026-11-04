/*
 * mod-boss-mechanics: the Mechanics Training Dummy (300402), ScriptName `npc_boss_mechanics_dummy`
 * (STAGE-M4-HANDOFF §3.2, WP-K).
 *
 * A player (no GM needed) or a GM spawns it with `.npc add 300402`. When a player engages it, it begins its own
 * encounter and runs a drill program on itself as the boss stand-in (StartProgram, MechanicDrillRunner.h), scoped to
 * that player's group, on a loop. The default program is every drill, then a 30 s barrage and a 30 s minefield
 * (Drill::kDefaultProgram), 1 flat damage per hit. It never dies, moves or attacks:
 * - every hit is clamped to leave 1 health, and a 40% floor is restored every second;
 * - NullCreatureAI never chases or swings; the template has DamageModifier 0, immunities -362 (control, knockback,
 *   pull, taunt) and is neutral (faction 31), so players can both attack it and talk to it.
 *
 * Lifetime (the existing hooks in MechanicHooks.cpp end the owner's encounter on evade, death and despawn):
 * - every second the dummy drops combat with players beyond kDummyLeashYd, adds the players it is in combat with to
 *   its encounter's scope set, and evades as soon as it is in combat with nobody;
 * - 2 minutes with no valid player in combat with it (only pets left, say) also evades;
 * - when the scope player's group has left the fight (the scope player is off the map, or neither it nor a member
 *   of its group is still fighting) while other players stay, the program restarts scoped to one of them;
 * - the top of the threat list is its victim (the tank of the frontal exemption and `pick=nontank`), and it turns
 *   in place to face it.
 * Evade ends the encounter: pending steps, mechanics, markers, ledger and Weakened go (StopProgram semantics). The
 * dummy keeps the last ledger as text so the gossip report still works after the fight. `.mech clear` leaves the
 * dummy alone (it ends the test owner's encounters only); `.mech clear all` ends its encounter too, and the dummy
 * then stays quiet (in combat, no program) until the next engage or gossip choice.
 *
 * Gossip: pick the program (all, one drill, barrage only), the damage (0 or 1 flat), whisper the report, stop the
 * program. A choice made while engaged restarts the program at once; otherwise it starts on the next engage. GMs also
 * have `.mech dummy <program|stop|report> [dmg=<n>]` on the selected dummy (HandleMechDummyCommand).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DUMMY_AI_H
#define MOD_BOSS_MECHANICS_MECHANIC_DUMMY_AI_H

#include "Duration.h"
#include "ObjectGuid.h"
#include "PassiveAI.h"
#include <string>
#include <vector>

class ChatHandler;
class Creature;
class Player;
class Unit;

namespace Acore::ChatCommands
{
    struct Tail;
}

namespace BossMechanics
{
    constexpr Milliseconds kDummyCheckInterval = Seconds(1);
    constexpr Milliseconds kDummyNoPlayerTimeout = Minutes(2);
    constexpr float kDummyLeashYd = 60.f;       // beyond every spell range: a player this far has left the fight
    constexpr int32 kDummyHealthFloorPct = 40;  // above every execute threshold (20 / 25 / 35%)
    constexpr int32 kDummyDefaultDamage = 1;
    constexpr float kDummyFacingSlop = 0.15f;  // rad: smaller turns toward the tank are not sent

    class MechanicDummyAI : public NullCreatureAI
    {
    public:
        explicit MechanicDummyAI(Creature* creature);

        void Reset() override;
        void JustEnteredCombat(Unit* who) override;
        void JustStartedThreateningMe(Unit* who) override;
        void JustEngagedWith(Unit* who) override;
        void EnterEvadeMode(EvadeReason why) override;
        void DamageTaken(Unit* attacker, uint32& damage, DamageEffectType damageType,
            SpellSchoolMask damageSchoolMask) override;
        void UpdateAI(uint32 diff) override;

        // The program text (Drill::ParseProgram) and the flat damage per hit. Restarts the program now when engaged
        // (scoped to the current scope player, else `by`); otherwise it starts on the next engage. False with `err`
        // when the program does not parse.
        bool Configure(std::string const& program, int32 damage, Player* by, std::string& err);
        // Ends the running program (the encounter with it) until the next engage.
        void Stop();
        // The live ledger (or the last fight's), one line each, as `.mech ledger` prints it.
        std::vector<std::string> ReportLines() const;

        std::string const& Program() const { return _program; }
        int32 Damage() const { return _damage; }
        bool Running() const;
        ObjectGuid ScopePlayer() const { return _scopePlayer; }

    private:
        // The engaged players who count (alive, on this map, within the leash, not GMs), in combat order.
        std::vector<Player*> FightingPlayers() const;
        bool StartFor(Player* scope, std::string& err);
        void Check();
        void FaceVictim();
        std::vector<std::string> LedgerLines() const;

        std::string _program;
        int32 _damage = kDummyDefaultDamage;
        ObjectGuid _scopePlayer;
        bool _started = false;                  // a program was started and not stopped since
        bool _stopped = false;                  // Stop(), a failed start or `.mech clear all`: until the next engage
        Milliseconds _noPlayerFor{ 0 };
        std::vector<std::string> _lastReport;   // the ledger of the last ended fight
    };

    // `.mech dummy <program|stop|report> [dmg=<n>]` on the selected training dummy (MechanicCommand.cpp's table).
    bool HandleMechDummyCommand(ChatHandler* handler, Acore::ChatCommands::Tail args);
}

#endif
