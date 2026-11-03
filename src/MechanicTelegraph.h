/*
 * mod-boss-mechanics: telegraph markers (STAGE-M1-HANDOFF §2.5, §3.8). Engine side of EngineBridge::SpawnMarkers /
 * UpdateMarkers / DespawnMarkers.
 *
 * One Mechanic Marker creature (300400) per marker piece, summoned with no summoner (so no boss JustSummoned and no
 * zone combat), passive, faction/level/phase copied from the owner, gravity off, lifted BossMechanics.Telegraph.Lift
 * yards above the anchor. Display and size come from PickVisual / PickFill (MechanicMath.h) and are set before the
 * creature enters the map (SummonMarker), so no client ever sees it at another size.
 *
 * Two layers per telegraph (M1 telegraph-fill fix):
 * - Outline (m.markers / m.carrierMarkers): rim + faint interior, the full hitbox from the first frame, never resized
 *   by the timer. The resolve casts from these.
 * - Fill (MechanicState::fill / carrierFill): the timer, stepped from createdAt to resolveAt (FillLeadMs early) and
 *   full while the danger is live (a resolved or persistent zone). Each mechanic's step is FillStepInterval: short
 *   enough that the fill's leading edge moves at most BossMechanics.Telegraph.FillStepYd per step, between 50 ms and
 *   BossMechanics.Telegraph.FillStepMs.
 *   Circle / cone: grows from the centre / apex. Donut: a rim-only ring shrinks from the outer rim to the hole; live,
 *   the filled donut. Line: a scaled copy of the whole line grows from the source end, longer and wider together
 *   (scale only, no display swap while the timer runs). Travelling projectile lines get no fill (the projectile is
 *   the timer). Telegraph.Fill = 0: the fill only shows the live state.
 *
 * What each kind draws:
 * - Zone/Frontal: the hitbox's pieces (circle, donut, cone, line pieces) in both layers; a mine fills while it arms;
 *   once armed its fill goes and its outline turns into the dashed ring (PickArmedMine); a unit anchor (or a
 *   Line(from, to) with a unit end) is followed every FollowIntervalMs.
 * - Spread/RunOut: a ring of the radius on each carrier (+ its fill), following it; self-trigger carriers are
 *   resynced on every follow step. RunOut From(Owner): one ring of the radius on the owner (the danger is "within r
 *   of the owner", + its fill) plus a small ring (Rules::kRunOutTargetMarkerRadiusYd, no fill) on each target, so
 *   the target knows it must run.
 * - Stack/Soak: an outline + fill disc on the anchor (followed when it is a unit). Spot: the same on each spot.
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
#include <string>
#include <vector>

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

        // A Mechanic Marker at `pos` with the owner's faction, level and phase, gravity off, display and scale set
        // before it enters the map (the first create packet carries them). display 0 keeps the template's invisible
        // model (a hidden resolve caster). nullptr when the summon fails.
        Creature* SummonMarker(Map* map, Position const& pos, Unit const* owner, uint32 display, float scale);

        // `.mech markers`: one line per live marker within `range` yd of `from` (2D), nearest mechanic first: the
        // mechanic, layer, decoded display, the scale and displays the creature really has, its pose, distance and
        // bearing from `from`, and the drawn extent the server intends (a line's far end).
        std::vector<std::string> DescribeMarkers(Map* map, Position const& from, float range);
    }
}

#endif
