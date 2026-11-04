/*
 * mod-boss-mechanics: registration, the strategy gate and AddSC_boss_mechanics_bots (STAGE-M2-HANDOFF §3.3, WP-C).
 *
 * Pattern adapted from mod-dungeon-clear (Jared Wright, AGPL-3.0-or-later), DcStrategyGate and DungeonClearModule.
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotGate.h"
#include "BossMechanicsConfig.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MechanicBotBrain.h"
#include "MechanicBotContexts.h"
#include "MechanicBotReportHook.h"
#include "MechanicBotView.h"
#include "MechanicRegistry.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "WorldScript.h"

// The ten class context registries: each owns its own public shared static lists.
#include "DKAiObjectContext.h"
#include "DruidAiObjectContext.h"
#include "HunterAiObjectContext.h"
#include "MageAiObjectContext.h"
#include "PaladinAiObjectContext.h"
#include "PriestAiObjectContext.h"
#include "RogueAiObjectContext.h"
#include "ShamanAiObjectContext.h"
#include "WarlockAiObjectContext.h"
#include "WarriorAiObjectContext.h"

using namespace BossMechanics;

namespace
{
    constexpr char const* kLog = "module.bossmechanics.bots";

    bool sLatched = false;      // the first world tick ran
    bool sEnabled = false;      // BossMechanics.Bots.Enable at that tick

    // Appends a fresh set of our contexts to one class's shared lists. Every per-bot NamedObjectContextList holds the
    // lists by reference, so existing and future bots of that class see the new names at once.
    template <class Ctx>
    void RegisterClassContexts()
    {
        Ctx::sharedStrategyContexts.Add(new MechanicBotStrategyContext());
        Ctx::sharedActionContexts.Add(new MechanicBotActionContext());
        Ctx::sharedTriggerContexts.Add(new MechanicBotTriggerContext());
        Ctx::sharedValueContexts.Add(new MechanicBotValueContext());
    }

    void RegisterAll()
    {
        RegisterClassContexts<WarriorAiObjectContext>();
        RegisterClassContexts<PaladinAiObjectContext>();
        RegisterClassContexts<DruidAiObjectContext>();
        RegisterClassContexts<DKAiObjectContext>();
        RegisterClassContexts<HunterAiObjectContext>();
        RegisterClassContexts<MageAiObjectContext>();
        RegisterClassContexts<PriestAiObjectContext>();
        RegisterClassContexts<RogueAiObjectContext>();
        RegisterClassContexts<ShamanAiObjectContext>();
        RegisterClassContexts<WarlockAiObjectContext>();
    }

    // Open-world test scope: the bot's group (raid) holds a GM who ran `.mech test` on this map.
    bool InTestScope(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;
        Registry const* reg = Registry::Get(bot->GetMap());
        if (!reg)
            return false;
        bool found = false;
        reg->ForEachEncounter([&](Encounter const& e)
        {
            for (ObjectGuid const& gm : e.testGms)
                found = found || group->IsMember(gm);
        });
        return found;
    }
}

bool BotGate::IsEnabled()
{
    return sEnabled;
}

void BotGate::Reconcile(Player* player)
{
    if (!sEnabled || !player || !player->IsInWorld())
        return;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
        return;     // a real player
    Map* map = player->GetMap();
    if (!map)
        return;

    BotConfig const& cfg = GetConfig().bots;
    bool const instance = map->IsDungeon();
    bool const wantStrategy = instance || (cfg.openWorldTestScope && InTestScope(player));
    bool const wantAvoidAoe = instance && cfg.avoidAoeInInstances;
    bool const hasCombat = botAI->HasStrategy("boss mechanics", BOT_STATE_COMBAT);
    bool const hasNonCombat = botAI->HasStrategy("boss mechanics", BOT_STATE_NON_COMBAT);
    bool const hasAvoidAoe = botAI->HasStrategy("avoid aoe", BOT_STATE_COMBAT);
    bool const stockAvoidAoe = sPlayerbotAIConfig.AutoAvoidAoe && botAI->HasGameClientMaster();

    if (wantStrategy)
    {
        if (!hasCombat)
            botAI->ChangeStrategy("+boss mechanics", BOT_STATE_COMBAT);
        if (!hasNonCombat)
            botAI->ChangeStrategy("+boss mechanics", BOT_STATE_NON_COMBAT);
    }
    else if (hasCombat || hasNonCombat)
    {
        if (hasCombat)
            botAI->ChangeStrategy("-boss mechanics", BOT_STATE_COMBAT);
        if (hasNonCombat)
            botAI->ChangeStrategy("-boss mechanics", BOT_STATE_NON_COMBAT);
        if (MechanicBotState* st = BotBrain::StateOf(botAI))
        {
            // M3: its reservations go with it (world thread, no map updates: the registry may be written).
            if (st->reserved && player->IsInWorld())
                if (Registry* reg = Registry::Get(player->GetMap()))
                    DropReservations(*reg, player->GetGUID());
            st->Reset();
        }
    }

    if (wantAvoidAoe)
    {
        if (!hasAvoidAoe)
            botAI->ChangeStrategy("+avoid aoe", BOT_STATE_COMBAT);
        // In an instance, an "avoid aoe" the stock rule would not add is ours (added now, or before a relog or a
        // reset), so leaving strips it.
        if (!stockAvoidAoe)
            if (MechanicBotState* st = BotBrain::StateOf(botAI))
                st->avoidAoeByGate = true;
    }
    else if (hasAvoidAoe)
    {
        MechanicBotState* st = BotBrain::StateOf(botAI);
        if (st && st->avoidAoeByGate)
        {
            if (!stockAvoidAoe)
                botAI->ChangeStrategy("-avoid aoe", BOT_STATE_COMBAT);
            st->avoidAoeByGate = false;
        }
    }
}

void BotGate::ReconcileAll()
{
    if (!sEnabled)
        return;
    // World thread, inside World::Update and outside the map updates: the player container is stable.
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        Reconcile(player);
}

class BossMechanicsBotWorldScript : public WorldScript
{
public:
    BossMechanicsBotWorldScript() : WorldScript("BossMechanicsBotWorldScript",
        { WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnUpdate(uint32 diff) override
    {
        if (!sLatched)
        {
            // First world tick: playerbots built its shared contexts in OnBeforeWorldInitialized, and no bot exists
            // yet. The master switch is latched here (a reload cannot add or remove contexts).
            sLatched = true;
            sEnabled = GetConfig().bots.enable;
            if (!sEnabled)
            {
                LOG_INFO(kLog, "mod-boss-mechanics: bot glue disabled (BossMechanics.Bots.Enable = 0)");
                return;
            }
            RegisterAll();
            SetBotAfterTick(&BotAfterTick);
            LOG_INFO(kLog, "mod-boss-mechanics: registered the boss-mechanics strategy, triggers, actions and value "
                "into all ten playerbots class contexts");
            return;
        }
        if (!sEnabled)
            return;
        _sinceSweep += diff;
        if (_sinceSweep < GetConfig().bots.gateSweepMs)
            return;
        _sinceSweep = 0;
        BotGate::ReconcileAll();
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload && sLatched && GetConfig().bots.enable != sEnabled)
            LOG_WARN(kLog, "BossMechanics.Bots.Enable changed to {}; it is latched at startup and applies after a "
                "restart", GetConfig().bots.enable ? 1 : 0);
    }

private:
    uint32 _sinceSweep = 0;
};

class BossMechanicsBotPlayerScript : public PlayerScript
{
public:
    BossMechanicsBotPlayerScript() : PlayerScript("BossMechanicsBotPlayerScript",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_BEFORE_LOGOUT }) { }

    void OnPlayerLogin(Player* player) override
    {
        BotGate::Reconcile(player);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        BotGate::Reconcile(player);
    }

    // M3: a bot logging out leaves the reservation board of its map (still in world here; the session update runs
    // on the world thread before the maps update, or on that map's own thread).
    void OnPlayerBeforeLogout(Player* player) override
    {
        if (!player || !player->IsInWorld() || !GET_PLAYERBOT_AI(player))
            return;
        if (Registry* reg = Registry::Get(player->GetMap()))
            DropReservations(*reg, player->GetGUID());
    }
};

void AddSC_boss_mechanics_bots()
{
    new BossMechanicsBotWorldScript();
    new BossMechanicsBotPlayerScript();
    SetBotReport(&BotReport);
}

#else

// Built without mod-playerbots: no bot glue (`.mech bots` says so).
void AddSC_boss_mechanics_bots()
{
}

#endif
