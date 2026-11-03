/*
 * mod-boss-mechanics: the seam between `.mech bots` (MechanicCommand.cpp) and the playerbots glue that knows the bots
 * (MechanicBotReport.cpp, STAGE-M2-HANDOFF §3.11). Always compiled, no playerbots includes: the glue registers its
 * report with SetBotReport(fn) from AddSC_boss_mechanics_bots(); without playerbots nothing registers and the
 * command prints "bot support not compiled".
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_REPORT_HOOK_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_REPORT_HOOK_H

class ChatHandler;
class Player;

namespace BossMechanics
{
    // Prints the report to `handler` (PSendSysMessage). `gm` is the player who ran the command. `allOnMap` false: the
    // bots in the GM's group; true: every bot on the GM's map (checks the gate on bot-only groups). Returns false
    // only when it already sent an error message.
    using BotReportFn = bool (*)(ChatHandler* handler, Player* gm, bool allOnMap);

    inline BotReportFn& BotReportSlot()
    {
        static BotReportFn fn = nullptr;
        return fn;
    }

    inline void SetBotReport(BotReportFn fn)
    {
        BotReportSlot() = fn;
    }

    inline BotReportFn GetBotReport()
    {
        return BotReportSlot();
    }
}

#endif
