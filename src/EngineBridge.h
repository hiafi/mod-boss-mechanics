/*
 * mod-boss-mechanics: the seam between the registry (C1: scheduling, geometry, ledger) and the engine glue (C2:
 * creatures and spells). STAGE-M1-HANDOFF §3.6. Implemented by EngineGlue in MechanicResolve.cpp (on top of
 * MechanicTelegraph.cpp and MechanicWeakened.cpp), installed from the loader (InstallEngineBridge -> SetEngineBridge).
 * Every call happens on the map's update thread or on the world thread while no map updates (§2.1).
 *
 * Contract, per call:
 * - SpawnMarkers(map, m): once per mechanic, at Registry::Commit (end of the registering statement), only when
 *   m.telegraph. Spawn the marker creatures (Ids::NPC_MECHANIC_MARKER, no summoner) and push their guids into
 *   m.markers (the outline layer only); for player-attached markers (Spread/RunOut carriers) also fill
 *   m.carrierMarkers. Picks displays with PickVisual (MechanicMath.h): circle/donut/cone/line from
 *   Registry::EffectiveShape, Spread/RunOut a ring of m.shape.radius on each carrier (Registry::Carriers; RunOut
 *   From(Owner): on the owner, plus a small ring on each target), Stack/Soak a disc, Spot a disc on each spot.
 *   The fill layer (PickFill, the timer) is the telegraph's own state, never in m.markers. Color: m.color.
 * - UpdateMarkers(map, m, now): every Tick while the mechanic lives (follow unit anchors and carriers, step the fill
 *   timer, show the full fill once a mine arms or a persistent zone resolves, optional projectile shrink). C2
 *   throttles to its follow and fill intervals.
 * - DespawnMarkers(map, m): when the mechanic finishes, is cancelled or fizzles, and on encounter end. Clear
 *   m.markers / m.carrierMarkers. Must tolerate markers that are already gone.
 * - CastResolve(map, m, pass): one AoE cast of m.damage.ResolveSpell() from a marker (m.ctx.carrier's marker for
 *   Spread/RunOut, else the first marker; a hidden 300400 at m.ctx.pose for NoTelegraph mechanics), triggered,
 *   original caster = m.owner when it is a live creature in the map, SPELLVALUE_RADIUS_MOD from
 *   m.ctx.SearchRadius(caster x, y, z), MISCVALUE0 = m.id, MISCVALUE1 = pass. m.ctx is active and m.ctx.pass == pass
 *   for the whole call; the hits must happen inside the call (instant, speed 0). The spell script calls
 *   Registry::FilterResolveTargets / ResolveDamageFor / OnResolveHit, and casts m.damage.riderSpell itself.
 * - AddWeakenedStack(player): one more stack (self-applied, cap 7, refresh), threat amount per WeakenedThreatPct.
 * - ClearWeakened(map, only): remove Weakened from `only`, or from every player on the map when only is nullptr.
 *   An encounter end calls it per player the encounter stacked (Registry::FinishEncounter), never with nullptr.
 */

#ifndef MOD_BOSS_MECHANICS_ENGINE_BRIDGE_H
#define MOD_BOSS_MECHANICS_ENGINE_BRIDGE_H

#include "Define.h"
#include "Duration.h"

class Map;
class Player;

namespace BossMechanics
{
    struct ActiveMechanic;

    class EngineBridge
    {
    public:
        virtual ~EngineBridge() = default;

        virtual void SpawnMarkers(Map* map, ActiveMechanic& m) = 0;                         // fills m.markers
        virtual void UpdateMarkers(Map* map, ActiveMechanic& m, Milliseconds now) = 0;      // follow, mine swap
        virtual void DespawnMarkers(Map* map, ActiveMechanic& m) = 0;
        virtual void CastResolve(Map* map, ActiveMechanic& m, uint32 pass) = 0;             // one AoE cast, filtered
        virtual void AddWeakenedStack(Player* player) = 0;
        virtual void ClearWeakened(Map* map, Player* only = nullptr) = 0;
    };

    // C2 calls it from the loader (the bridge object lives for the process). nullptr uninstalls.
    void SetEngineBridge(EngineBridge* bridge);
    EngineBridge* GetEngineBridge();
}

#endif
