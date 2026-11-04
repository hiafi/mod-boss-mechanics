/*
 * mod-boss-mechanics: registration and the strategy gate of the playerbots glue (STAGE-M2-HANDOFF §3.3, WP-C).
 *
 * Pattern adapted from mod-dungeon-clear (Jared Wright, AGPL-3.0-or-later), DcStrategyGate and DungeonClearModule.
 *
 * Registration: on the first world tick (playerbots has built its shared contexts by then), one fresh set of the four
 * MechanicBot*Context objects goes into each of the ten class context lists (they are public statics; the base list
 * only serves class 0 and is skipped). BossMechanics.Bots.Enable is latched there: 0 registers nothing. The sim, which
 * runs before any world tick, latches it earlier through Sim::RegisterBotGlue; the first tick then does nothing.
 *
 * Gate invariant, per bot (master or not, PLAN §9 "every role may be a bot"):
 * - on an instance map (Map::IsDungeon()): "boss mechanics" in the combat and non-combat engines, and "avoid aoe" in
 *   the combat engine when BossMechanics.Bots.AvoidAoeInInstances (D6);
 * - elsewhere, with BossMechanics.Bots.OpenWorldTestScope, when the bot is, or its group (raid) holds, a scope player
 *   (Encounter::testGms) of a live encounter on its map that wants bots (Encounter::wantsBots: `.mech test`, the
 *   training dummy, the sim; M4): "boss mechanics" only;
 * - otherwise none of them. "avoid aoe" is stripped only when the gate added it and the stock rule
 *   (Playerbots.AutoAvoidAoe with a real master) would not have.
 * Drivers: OnPlayerLogin, OnPlayerMapChanged and a world-tick sweep every BossMechanics.Bots.GateSweepMs that repairs
 * PlayerbotAI::ResetStrategies (it rebuilds every engine from the defaults). All on the world thread. The sim has none
 * of them: it calls Sim::RegisterBotGlue and Sim::ReconcileBot (BossMechanicsSim.h).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_GATE_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_GATE_H

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "Duration.h"

class ChatHandler;
class Map;
class Player;
class Spell;

namespace BossMechanics
{
    class Registry;

    namespace BotGate
    {
        // BossMechanics.Bots.Enable as latched on the first world tick or Sim::RegisterBotGlue (false before).
        bool IsEnabled();
        // Brings one bot into the invariant above. Idempotent, a few HasStrategy reads when compliant, no-op for
        // real players. World thread only (never from a bot's own engine tick).
        void Reconcile(Player* player);
        // Reconcile for every online player (the sweep).
        void ReconcileAll();
    }

    // MechanicBotWaker.cpp: the registry's after-tick hook (SetBotAfterTick), on the map's update thread.
    void BotAfterTick(Map* map, Registry& reg, Milliseconds now);
    // MechanicBotWaker.cpp (M4 leak detectors): a player cast a spell (PlayerScript::OnPlayerSpellCast, the caster's
    // map thread); a bot's own non-triggered cast is remembered so a leaked move can name its spell.
    void BotOnSpellCast(Player* player, Spell const* spell);
    // MechanicBotReport.cpp: the `.mech bots [all]` body (SetBotReport), on the world thread.
    bool BotReport(ChatHandler* handler, Player* gm, bool allOnMap);
}

#endif

#endif
