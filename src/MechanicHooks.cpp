/*
 * mod-boss-mechanics: ScriptMgr hooks (STAGE-M1-HANDOFF §2.3, §3.3 "Lifetime", §3.11). Zero core edits; each hook
 * class enables only the hooks it implements.
 *
 * - Config: OnAfterConfigLoad -> LoadConfig (startup and `.reload config`).
 * - Map tick: OnMapUpdate (full map updates only) -> Registry::Tick. Only maps with a registry pay anything.
 * - Encounter end: owner death (OnUnitDeath, after JustDied), owner evade (OnUnitEnterEvadeMode, after Reset), owner
 *   removed from the world (OnCreatureRemoveWorld, deferred to the next tick: it also fires while a grid unloads, where
 *   a despawn would break Map::UnloadGrid), boss state change to NOT_STARTED/DONE/FAIL (OnBeforeSetBossState, new !=
 *   old; it also fires while the instance loads and for unchanged states; deferred to the next tick so JustDied can
 *   still read the ledger, and dropped if the instance refused the change).
 * - Player leaves the map: OnPlayerLeaveAll drops their Weakened.
 * - Bots (M2): right after each registry Tick, the after-tick hook the bot glue installs (SetBotAfterTick).
 */

#include "AllCreatureScript.h"
#include "AllMapScript.h"
#include "BossMechanicsConfig.h"
#include "BossMechanicsIds.h"
#include "Creature.h"
#include "GameTime.h"
#include "GlobalScript.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MechanicRegistry.h"
#include "MechanicWeakened.h"
#include "Player.h"
#include "UnitScript.h"
#include "WorldScript.h"
#include <atomic>

using namespace BossMechanics;

namespace
{
    constexpr char const* kLog = "module.bossmechanics";

    // Set by the bot glue (startup or the first world tick); read on the map threads, hence atomic.
    std::atomic<BotAfterTickFn> sBotAfterTick{ nullptr };

    // Ends `unit`'s encounter now, when it owns one on its map.
    void EndEncounterOf(Unit* unit)
    {
        if (!unit || !unit->IsInWorld())
            return;
        Registry* reg = Registry::Get(unit->GetMap());
        if (reg && reg->FindEncounter(unit->GetGUID()))
            reg->EndEncounter(unit->GetGUID());
    }
}

void BossMechanics::SetBotAfterTick(BotAfterTickFn fn)
{
    sBotAfterTick.store(fn, std::memory_order_release);
}

class BossMechanicsWorldScript : public WorldScript
{
public:
    BossMechanicsWorldScript() : WorldScript("BossMechanicsWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadConfig(reload);
    }
};

class BossMechanicsMapScript : public AllMapScript
{
public:
    BossMechanicsMapScript() : AllMapScript("BossMechanicsMapScript",
        { ALLMAPHOOK_ON_MAP_UPDATE, ALLMAPHOOK_ON_PLAYER_LEAVE_ALL, ALLMAPHOOK_ON_DESTROY_MAP }) { }

    void OnMapUpdate(Map* map, uint32 diff) override
    {
        Registry* reg = Registry::Get(map);
        if (!reg)
            return;
        if (GetConfig().debug && reg->Size())
            LOG_INFO(kLog, "Map {} instance {}: tick diff {} ms, {} mechanics", map->GetId(), map->GetInstanceId(),
                diff, reg->Size());
        Milliseconds const now = GameTime::GetGameTimeMS();
        reg->Tick(now);
        if (BotAfterTickFn fn = sBotAfterTick.load(std::memory_order_acquire))
            fn(map, *reg, now);
    }

    void OnPlayerLeaveAll(Map* map, Player* player) override
    {
        if (player && player->HasAura(Ids::SPELL_WEAKENED))
            Weakened::Clear(map, player);
    }

    void OnDestroyMap(Map* map) override
    {
        if (GetConfig().debug && Registry::Get(map))
            LOG_INFO(kLog, "Map {} instance {} destroyed with its mechanic registry", map->GetId(),
                map->GetInstanceId());
    }
};

class BossMechanicsUnitScript : public UnitScript
{
public:
    BossMechanicsUnitScript() : UnitScript("BossMechanicsUnitScript", true,
        { UNITHOOK_ON_UNIT_DEATH, UNITHOOK_ON_UNIT_ENTER_EVADE_MODE }) { }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        EndEncounterOf(unit);
    }

    void OnUnitEnterEvadeMode(Unit* unit, uint8 /*evadeReason*/) override
    {
        EndEncounterOf(unit);
    }
};

class BossMechanicsCreatureScript : public AllCreatureScript
{
public:
    BossMechanicsCreatureScript() : AllCreatureScript("BossMechanicsCreatureScript") { }

    void OnCreatureRemoveWorld(Creature* creature) override
    {
        if (!creature || creature->GetEntry() == Ids::NPC_MECHANIC_MARKER)
            return;
        if (Registry* reg = Registry::Get(creature->GetMap()))
            reg->DeferEndEncounter(creature->GetGUID());
    }
};

class BossMechanicsGlobalScript : public GlobalScript
{
public:
    BossMechanicsGlobalScript() : GlobalScript("BossMechanicsGlobalScript", { GLOBALHOOK_ON_BEFORE_SET_BOSS_STATE }) { }

    void OnBeforeSetBossState(uint32 id, EncounterState newState, EncounterState oldState, Map* instance) override
    {
        if (newState == oldState)
            return;
        if (newState != NOT_STARTED && newState != DONE && newState != FAIL)
            return;
        // Deferred to the next map tick: this hook runs before the state is committed, and often inside the boss's
        // own death chain (BossAI::_JustDied, InstanceScript::OnUnitDeath) before its JustDied body reads the ledger.
        if (Registry* reg = Registry::Get(instance))
            reg->DeferEndEncountersForBossState(id);
    }
};

void AddSC_boss_mechanics_hooks()
{
    new BossMechanicsWorldScript();
    new BossMechanicsMapScript();
    new BossMechanicsUnitScript();
    new BossMechanicsCreatureScript();
    new BossMechanicsGlobalScript();
}
