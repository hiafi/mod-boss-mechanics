/*
 * mod-boss-mechanics loader (modules/CMakeLists.txt calls Add<module>Scripts).
 */

namespace BossMechanics
{
    void InstallEngineBridge();     // MechanicResolve.cpp
}

void AddSC_boss_mechanics_hooks();      // MechanicHooks.cpp: config, map tick, encounter end, Weakened on map leave
void AddSC_boss_mechanics_resolve();    // MechanicResolve.cpp: spell_boss_mechanics_resolve (205001-205007)
void AddSC_boss_mechanics_weakened();   // MechanicWeakened.cpp: spell_boss_mechanics_weakened (205000)
void AddSC_boss_mechanics_marker();     // MechanicMarkerAI.cpp: npc_boss_mechanic_marker (300400, 300401)
void AddSC_boss_mechanics_command();    // MechanicCommand.cpp: .mech

void Addmod_boss_mechanicsScripts()
{
    BossMechanics::InstallEngineBridge();
    AddSC_boss_mechanics_hooks();
    AddSC_boss_mechanics_resolve();
    AddSC_boss_mechanics_weakened();
    AddSC_boss_mechanics_marker();
    AddSC_boss_mechanics_command();
}
