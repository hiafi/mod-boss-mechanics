/*
 * mod-boss-mechanics: the sim seam (BossMechanicsSim.h, STAGE-M4-HANDOFF §3.3, WP-B).
 *
 * RegisterBotGlue and ReconcileBot live in MechanicBotGate.cpp (the latch is file-local there); the drill program
 * in MechanicCommand.cpp (it shares the `.mech test` parser). This file forwards and reads the bot counters (WP-C: the
 * mode clocks and leak counts live in MechanicBotCounters, the cost counters in MechanicBotState::cost).
 */

#include "BossMechanicsSim.h"
#include "MechanicBotGuard.h"
#include "MechanicDrillRunner.h"

#if BOSS_MECHANICS_WITH_BOTS
#include "MechanicBotBrain.h"
#include "MechanicBotGate.h"
#include "MechanicBotState.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#endif

namespace BossMechanics::Sim
{
    void SetFailureSink(FailureSink fn)
    {
        BossMechanics::SetFailureSink(fn);
    }

    bool StartProgram(Creature* owner, ObjectGuid scopePlayer, std::string_view program, DrillOptions const& opt,
        std::string& err)
    {
        return BossMechanics::StartProgram(owner, scopePlayer, program, opt, err);
    }

    void StopProgram(Creature* owner)
    {
        BossMechanics::StopProgram(owner);
    }

#if BOSS_MECHANICS_WITH_BOTS
    namespace
    {
        MechanicBotState* StateOfBot(Player const* bot)
        {
            if (!bot || !BotGate::IsEnabled())
                return nullptr;
            // GET_PLAYERBOT_AI takes a non-const Player; nothing here writes the player.
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(const_cast<Player*>(bot));
            return botAI ? BotBrain::StateOf(botAI) : nullptr;
        }
    }

    bool ReadBotStats(Player const* bot, BotStats& out)
    {
        out = BotStats();
        MechanicBotState const* st = StateOfBot(bot);
        if (!st)
            return false;
        MechanicBotCounters const& c = st->counters;
        out.counters = c;
        out.msIdle = c.msIdle;
        out.msWatch = c.msWatch;
        out.msEscape = c.msEscape;
        out.msHold = c.msHold;
        out.msFreeze = c.msFreeze;
        out.msReleased = c.msReleased;
        out.leakMoves = c.leakMoves;
        out.leakCasts = c.leakCasts;
        out.leakTurns = c.leakTurns;
        out.leakSpells = c.leakSpells;
        out.cost = st->cost;
        return true;
    }

    // Counts only: the leak detectors' references and tokens (MechanicBotState::leak) and the mode clock's last
    // sample stay, so the first check after a reset judges against the real previous state.
    void ResetBotStats(Player const* bot)
    {
        if (MechanicBotState* st = StateOfBot(bot))
        {
            st->counters = MechanicBotCounters();
            st->cost = CostStats();
        }
    }
#else
    bool ReadBotStats(Player const* /*bot*/, BotStats& out)
    {
        out = BotStats();
        return false;
    }

    void ResetBotStats(Player const* /*bot*/)
    {
    }
#endif
}
