/*
 * mod-boss-mechanics: telegraph markers (STAGE-M1-HANDOFF §2.5, §3.8). Engine side of EngineBridge::SpawnMarkers /
 * UpdateMarkers / DespawnMarkers.
 *
 * One Mechanic Marker creature (300400) per marker piece, summoned with no summoner (so no boss JustSummoned and no
 * zone combat), passive, faction/level/phase copied from the owner, gravity off, lifted BossMechanics.Telegraph.Lift
 * yards above the anchor. Display and size come from PickVisual (MechanicMath.h): SetDisplayId first, then
 * SetObjectScale (SetDisplayId resets the scale).
 *
 * What each kind draws:
 * - Zone/Frontal: the hitbox's pieces (circle disc, donut, cone, line pieces); a mine is a ring until it arms, then a
 *   disc; a unit anchor (or a Line(from, to) with a unit end) is followed every FollowIntervalMs.
 * - Spread/RunOut: a ring of the radius on each carrier, following it; self-trigger carriers are resynced on every
 *   follow step. RunOut From(Owner): one ring of the radius on the owner (the danger is "within r of the owner") plus
 *   a small ring (Rules::kRunOutTargetMarkerRadiusYd) on each target, so the target knows it must run.
 * - Stack/Soak: a disc on the anchor (followed when it is a unit). Spot: a disc on each spot.
 * - Bait/HoldStill: nothing (the API registers them with telegraph = false).
 *
 * Per-marker follow state lives in a per-map DataMap entry (Map::CustomData), keyed by marker guid; nothing stores a
 * creature pointer.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_TELEGRAPH_H
#define MOD_BOSS_MECHANICS_MECHANIC_TELEGRAPH_H

#include "Define.h"
#include "Duration.h"
#include "ObjectGuid.h"

class Creature;
class Map;
struct Position;
class Unit;

namespace BossMechanics
{
    struct ActiveMechanic;

    namespace Telegraph
    {
        void Spawn(Map* map, ActiveMechanic& m);
        void Update(Map* map, ActiveMechanic& m, Milliseconds now);
        void Despawn(Map* map, ActiveMechanic& m);

        // A Mechanic Marker at `pos` with the owner's faction, level and phase, gravity off. display 0 keeps the
        // template's invisible model (a hidden resolve caster). nullptr when the summon fails.
        Creature* SummonMarker(Map* map, Position const& pos, Unit const* owner, uint32 display, float scale);
    }
}

#endif
