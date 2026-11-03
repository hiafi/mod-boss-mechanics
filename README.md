# mod-boss-mechanics

Boss mechanic registry for AzerothCore (WotLK 3.3.5a): boss scripts register telegraphed mechanics (zones, frontals,
spreads, stacks, soaks, baits, mines, travelling projectiles, room-frame lanes); the module draws the telegraph,
resolves the damage from the same shape the telegraph shows, applies the shared Weakened penalty and keeps a
per-player failure ledger per encounter. Bots read the same registry.

Status: stage M1 (registry, telegraphs, resolve, Weakened, `.mech`). Not yet built or playtested; see "Spike checks".
Design: `.agents/plans/dungeon-mechanics/` in the core repository (PLAN, STAGE-M1-HANDOFF and the WP reports).

No core edits: everything is data (dbc-tools), spell/creature scripts and existing ScriptMgr hooks.

## Layout

| File | What |
| --- | --- |
| `src/BossMechanics.h` | Public API: `Mechanic::` free functions, `MechanicRef` options, `LaneBuilder` |
| `src/MechanicRegistry.h/.cpp` | Data model, per-map registry (`Map::CustomData`), scheduler, ledger |
| `src/MechanicGeometry.h` | Pure shape geometry (`Contains`, `Bounds`), shared by the resolve filter and bots |
| `src/MechanicMath.h` | Pure timing and numbers: projectile, mine, lanes, threat, damage, visual pick, `Rules::` |
| `src/MechanicGround.h` | Pure ground snap: footprint sampling, height band, terrain line split |
| `src/MechanicProviders.h/.cpp` | Reference health, content key and role providers (stubs) |
| `src/EngineBridge.h` | The seam between the registry and the engine glue |
| `src/MechanicTelegraph.h/.cpp` | Marker creatures: outline + fill layers, spawn, size, fill timer, follow, despawn |
| `src/MechanicResolve.cpp` | `spell_boss_mechanics_resolve` and the `EngineBridge` implementation (resolve cast) |
| `src/MechanicWeakened.h/.cpp` | Weakened stacks and threat compensator, `spell_boss_mechanics_weakened` |
| `src/MechanicMarkerAI.h/.cpp` | `npc_boss_mechanic_marker`: passive marker / test owner AI |
| `src/MechanicHooks.cpp` | Config load, map tick, encounter end, Weakened on map leave |
| `src/MechanicCommand.cpp` | `.mech` |
| `src/MechanicBarrage.h` | Pure shot geometry and seeded dice of `.mech barrage` (host-tested) |
| `src/MechanicBotReportHook.h` | `SetBotReport(fn)`: how `.mech bots` reaches the playerbots glue |
| `src/MechanicSolver.h` | Pure bot solver: hazards, hot windows, signed distance, candidates, path check, `Solve` |
| `src/MechanicFallibility.h` | Pure seeded bot mistakes: reaction delay, late and wrong-side picks |
| `src/MechanicBotView.h/.cpp` | Registry -> solver input for one bot; navmesh path oracle (no playerbots types) |
| `src/MechanicBotGuard.h` | `BOSS_MECHANICS_WITH_BOTS`: 1 only with mod-playerbots enabled |
| `src/MechanicBotState.h` | Per-bot state and its playerbots value (`boss mechanic state`) |
| `src/MechanicBotBrain.h/.cpp` | Per-bot update: view, mode machine, solve, leaveBy |
| `src/MechanicBotStrategy.h/.cpp` | `boss mechanics` strategy, its two triggers and two actions |
| `src/MechanicBotLock.h/.cpp` | Lock multiplier: what may run while a bot escapes or holds (the one block list) |
| `src/MechanicBotContexts.h` | The four playerbots named-object contexts |
| `src/MechanicBotGate.h/.cpp` | Context registration, strategy gate, `AddSC_boss_mechanics_bots` |
| `src/MechanicBotWaker.cpp` | After-tick waker and cast break |
| `src/MechanicBotReport.cpp` | `.mech bots` body |
| `src/BossMechanicsConfig.h/.cpp` | Cached config |
| `src/BossMechanicsIds.h` | Pinned spell, creature and display ids |
| `tests/` | Standalone tests for the pure headers: `tests/run.sh` (host g++, no server build) |

File names are prefixed (`Mechanic*`, `BossMechanics*`) because module include directories come before the core's on
the `modules` target: a module `Config.h` or `Geometry.h` would shadow the core header for every module.

## Using it from a boss script

A boss script that calls the API must be compiled in the `modules` target (this module or another module): the
`scripts` target cannot see module headers.

```cpp
#include "BossMechanics.h"
using namespace BossMechanics;

// in the boss AI (me = the boss, the encounter owner); BeginEncounter goes in JustEngagedWith, never in Reset
Mechanic::BeginEncounter(me, { Color::Red, Color::Blue, BOSS_RHAHKZOR });      // optional: palette, boss state
MechanicHandle cleave = Mechanic::Cone(me, me, 60.f, 20.f, 2500ms, TankExempt::Yes).Damage(35.f).Weakened();
Mechanic::Line(me, me, target, 3.f, 2000ms).Projectile(20.f).OutAndBack().ExcludeSourceEnd(5.f).Damage(30.f);
Mechanic::Mine(me, pos, 4.f, 3000ms, 500ms).Cap(1, 3).Damage(40.f).Weakened();
Mechanic::RoomFrame(me, frame).Lanes(Axis::X, { 3, 6.f, 6.f }, 2000ms)
    .Lanes(Axis::Y, { 2, 6.f, 6.f }, 2000ms).BurnCrossings(6000ms, 1000ms, 5.f)
    .Each([](MechanicRef& lane) { lane.Damage(25.f); });
```

Call `BeginEncounter` in `JustEngagedWith` (or later), **never in `Reset()`**: the core runs `Reset()` and then the
evade hook, which ends the encounter, so a palette and boss state id set in `Reset()` are lost after every wipe and the
next pull runs as a bare implicit encounter.

Each registration returns a `MechanicRef`, a builder valid in the registering statement; it commits (timing, random
targets, mine cap, markers) at the end of the statement. Store the `MechanicHandle` it converts to, never the ref.
Misuse never asserts: an option that does not fit the kind, a size <= 0 or an option after the commit logs
`module.bossmechanics` at WARN and is ignored. The hitbox is always exact; only the drawn marker rounds to a ladder.

### Free functions (`namespace Mechanic`)

| Function | What |
| --- | --- |
| `BeginEncounter(owner, opt)` / `EndEncounter(owner)` | Palette (out, in, cross lanes, cells), boss state id |
| `Zone(owner, Shapes::Circle/Donut/Cone/Line/LineFacing(...), warn)` | Any shape on a point or unit anchor |
| `Circle`, `Donut`, `Cone(owner, source, angle, len, warn, exempt)`, `Line` | Zone shortcuts |
| `Mine(owner, pos, r, arm, grace)` | Ring + fill while arming, dashed ring once armed; `grace` inside detonates it |
| `RoomFrame(owner, frame)` -> `LaneBuilder` | `.Lanes`, `.ExceptOne`, `.BurnCrossings`, `.Each` (`BossMechanics.h`) |
| `Spread(owner, r, warn)`, `RunOut(owner, target, r, warn)` | Circle on each carrier; hits the other players inside |
| `Stack(owner, at, r, warn)`, `Soak(owner, at, r, count, warn)` | Get-in circles; results to `onResolve` |
| `Bait(owner, rule, othersRange, warn)` | The hit lands on the farthest player |
| `HoldStill(owner, players, window, warn)`, `Spot(owner, spots, warn, r)` | Movement / position checks |
| `Cancel`, `Detonate`, `Contains`, `ContainsPoint`, `Failures`, `OnFailure` | Handles, ledger, failure callback |
| `Read(map)` | Bots: the map's registry (`nullptr` when none) |

### Options (`MechanicRef`)

`Damage(pct)`, `TankDamage(pct)`, `FlatDamage(n)`, `School(s)`, `Spell(id)`, `Rider(spellId)`, `Weakened()`,
`WeakenedOncePerPlayer()`, `Persistent(duration, tick)`, `Projectile(speed)`, `ProjectileUnit(unit)`,
`OutAndBack(pause)`, `Lifetime(ms)` (mines), `Cap(group, n)`, `ExcludeSourceEnd(yd)`, `FacingOffset(deg)`, `Rear()`,
`AnchorOn(unit)`, `FollowFacing()`, `ZTolerance(yd)`, `Eligible(...)`, `Order(...)`, `Count(n)`, `Targets(set)`,
`SelfTrigger(spell, stacks)`, `From(MeasuredFrom)`, `TankExempt()`, `Color(c)`, `Tag(n)`, `NoTelegraph()`,
`NoFillTimer()`, `OnHit(fn)`, `OnResolve(fn)`.

Damage is a percent of the reference health (`ReferenceHealthProvider`; M1 ships the config stub), one flat number per
target set by the resolve spell, so boss buffs and the creature damage rate never change it
(`SPELL_ATTR3_IGNORE_CASTER_MODIFIERS`). Physical mechanics are reduced by armor; no crit, miss, reflect or partial
resist. A hit fires the victim's "when hit" procs (`SPELL_ATTR3_NOT_A_PROC` lets the triggered cast proc: Reckoning,
Blood Craze, Vengeance-style talents), like a boss ability would.

### Kinds

| Kind | Server resolve (M1) | Marker | Failure |
| --- | --- | --- | --- |
| Zone | resolve spell on everyone eligible inside | the shape's pieces; a mine: ring, then dashed ring | Hit |
| Frontal | as Zone, the owner's victim is skipped | cone | Hit |
| Spread | a circle on each carrier hits every other player inside | ring on each carrier | Hit |
| RunOut | as Spread on one target; `From(Owner)`: hit within r of the owner | ring on the target (*) | Hit |
| Stack | none (split damage via `onResolve`) | disc on the anchor | MissedStack |
| Soak | none; `onResolve` gets the count | disc on the anchor | MissedSoak (group) |
| Bait | hit on the farthest eligible player | none | MissedBait |
| HoldStill | none | none | Moved (> 0.3 yd) |
| Spot | none | disc on each spot | OffSpot |

(*) `From(Owner)`: the ring of r sits on the owner (the hitbox) and a small ring
(`Rules::kRunOutTargetMarkerRadiusYd`, 1.5 yd, same color) follows the target, so the target knows it must run.

Every unit hit posts `Hit`. Pets take damage but never reach the ledger or Weakened. Only alive player-controlled units
can be hit (GMs never). `ResolveResult.hits` lists the units the filter kept, so an immune target (Divine Shield,
Ice Block) is listed although it took nothing. Get-out kinds draw in the encounter's get-out color (red by
default), get-in kinds in its get-in color (blue by default); `Color(c)` overrides one mechanic. Every shape exists
in all nine colors:

| Color | RGB | Reads poorly on |
| --- | --- | --- |
| red | 230, 40, 30 | — |
| yellow | 245, 200, 40 | sand |
| blue | 30, 140, 255 | — (brightened from 40, 120, 240) |
| purple | 185, 90, 250 | — |
| orange | 255, 130, 20 | sand |
| green | 80, 225, 60 | grass |
| teal | 20, 215, 200 | snow |
| white | 245, 245, 245 | snow |
| black | 0, 0, 0 | dark floors (it darkens the floor) |

The textures are white; each marker model's M2 color multiplies them (M1-FIX-COLORS-REPORT.md).

### Weakened (205000)

-10% damage done per stack, 7 stacks max, 30 s, refreshed by every stack, cannot be dispelled, lands through
immunities. A threat aura compensates exactly (+11, 25, 43, 67, 100, 150, 233%) so a Weakened tank keeps its threat
per second. Always self-applied. Removed on leaving the map, on death, and on encounter end from the players that
encounter stacked, unless another encounter still on the map also stacked them (an add or a test owner ending mid
fight does not reset the boss's penalty). `.mech weakened n` stacks belong to no encounter: reset them with
`.mech weakened 0`.

- Only hits the resolve spell lands apply Weakened (`Rules::kWeakenedOnNonHitFailures = false`, harness ruling
  2026-11-02). `MissedStack`, `MissedBait`, `Moved` and `OffSpot` go to the failure ledger only. Flip the constant
  in `src/MechanicMath.h` to make those failures apply a stack too on a `.Weakened()` mechanic.
- `WeakenedOncePerPlayer()`: one stack per player per mechanic (a persistent zone ticking several times).
- `Weakened()` fits only the kinds that cast the resolve spell (Zone, Frontal, Spread, RunOut, Bait); on Stack, Soak,
  HoldStill or Spot it logs "does not fit" and is ignored.
- Known side effect: the threat aura also raises a Weakened healer's healing threat.

### Encounter lifetime

An encounter ends (markers despawned, mechanics dropped, its Weakened removed as above, ledger cleared) on
`EndEncounter(owner)`, owner death (after `JustDied`, so `JustDied` can still read the ledger), owner evade (after
`Reset`), owner removed from the world (at the next map tick), and a boss state change to `NOT_STARTED`, `DONE` or
`FAIL` for an encounter registered with that boss state id. The boss state end lands at the next map tick (the hook
fires inside `BossAI::_JustDied` and `InstanceScript::OnUnitDeath`, before the script's own `JustDied` body), so
`JustDied` can read the ledger with or without a boss state id; a change the instance refuses (DONE while a minion
lives) keeps the encounter.

Outside instances only players within `BossMechanics.OpenWorld.CandidateRadius` (100 yd) of the owner take part in
player-picking kinds (Spread, random RunOut targets, Bait, Stack/Soak, HoldStill, mine triggers); dungeons, raids and
battlegrounds use the whole map.

## Telegraph markers

One Mechanic Marker (300400) per marker piece, summoned with no summoner (so no `JustSummoned` and no zone combat),
passive, never evading, faction/level/phase copied from the owner, gravity off, `Telegraph.Lift` above the ground under
it (see "Ground snap" below).
Display and size come from the ladder pick (scale = radius / length / width; the marker shapes are 1 yd at scale 1)
and are set **before** the creature enters the map: `SummonMarker` builds the TempSummon itself instead of calling
`Map::SummonCreature`, whose `AddToMap` sends the create packet with the template display at scale 1. The client
blends every scale change in, so a marker resized after the create visibly grew from 1 yd (the first build's bug).

Every telegraph draws two layers:

| Layer | Look | Size |
| --- | --- | --- |
| Outline | rim (0.9) + faint interior (0.2) | the full hitbox from the first frame; never resized by the timer |
| Fill | rim (0.9) + solid interior (0.35) | the timer: grows into the outline over the warning, full at the resolve |

The fill is stepped from the registration to the resolve, computed `Telegraph.FillLeadMs` (150 ms) ahead so the
client's scale blend lands it on the outline at the resolve, not after; it is never bigger than the outline. The step
is paced by distance (`FillStepInterval`, round 4b): often enough that the fill's leading edge (a line's far end, a
disc's rim, a cone's arc, the donut timer ring) moves at most `Telegraph.FillStepYd` (0.4 yd) per step, never more
often than every 50 ms and never less often than `Telegraph.FillStepMs` (150 ms). The client's scale blend is shorter
than a long step, so a big fill stepped slowly moved in visible jumps ("comes in segments"); a 30 x 4 line over 4 s
now steps every 50 ms (0.4 yd), a 5 yd circle over 4 s every 150 ms. Keep `FillLeadMs` at least `FillStepMs`: then the
last step before the resolve already asks for the full fill. `FillStepYd = 0` steps every `FillStepMs`. Per shape:

- Circle, Stack/Soak/Spot discs, Spread/RunOut rings: a disc grows from the centre.
- Cone: grows from the apex (uniform scale keeps the angle).
- Donut: a rim-only timer ring shrinks from the outer rim to the drawn hole, its band always on the danger
  side; once live (a persistent donut), the filled donut.
- Line: a copy of the whole drawn line scaled by the timer fraction about its source end (after `ExcludeSourceEnd`, or
  a lane's drawn strip start), on its axis, the way a cone grows from its apex: a sliver at the source that grows
  longer and wider together until it is the outline. Scale only, in the full fill's own display, so the client blends
  it smoothly; nothing swaps a display while the timer runs (M1-FIX-LINE-FILL: the old per-step display swaps
  re-instanced the model and flickered). A multi-piece line (aspect above 64) scales each piece and moves piece k to
  f x its start from the source every step (a spline over one step).
- Travelling projectile lines: no fill (the projectile is the timer).
- Mines: the fill grows while the mine arms. Once armed the fill goes and the outline swaps to a dashed ring (one
  display swap), so an armed mine (waiting for a player) reads differently from an incoming hit (a solid fill).
- Persistent zones: full fill from the resolve to the end (no regrowth per tick).
- `NoFillTimer()` (lane crossing cells): outline only until the danger is live, then the full fill.
- RunOut `From(Owner)`: the owner's ring gets the fill; the small pointer rings on the targets do not.

`Telegraph.Fill = 0` drops the timer: the outline is drawn alone until the danger is live (a persistent zone after its
resolve), which still shows the full fill. An armed mine draws its dashed ring either way.

Cost: two creatures per drawn piece (outline + fill), so a circle, cone, donut or one-piece line is 2 markers, a
Spread on 5 players 10, an n-piece line 2n; projectile lines and the RunOut pointer rings stay single.
`MaxActivePerMap` (256) caps mechanics, not markers: 256 one-piece mechanics are about 512 marker creatures. Fill
resizes send one value update per fill marker per step (~7 per second), like a moving creature.

- Cone angles round up to the ladder (30, 45, 60, 90, 120, 150, 180, 270); a cone wider than 270 degrees draws a full
  disc of its length (the hitbox stays the cone; a WARN names it).
- Donut ratios round **down** to the ladder (0.25, 0.33, 0.40, 0.50, 0.60, 0.67, 0.75): the drawn safe hole is never
  bigger than the real one (`Rules::kDonutRatioRoundUp = false`, harness ruling 2026-11-02). A ratio below 0.25 draws
  a full disc (no hole shown, a WARN names it) rather than a hole bigger than the real one.
- Lines use aspect steps 2^((i-8)/4) from 1/4 to 0.84 and 2^((i-8)/8) from 1 to 64 (`LineAspect`, 57 steps, the same
  for both layers). Most lines are one piece; a longer one is laid end to end: pieces of exactly 64 widths, then one
  last piece rounded up to the ladder (`Rules::kLineNeverShorter`), so pieces never overlap and the only overshoot is
  the far end of the last piece (at most 9%, 19% below 1 width, and up to a quarter width for a line shorter than
  that).
- Line models are one quad, symmetric about the marker (the client draws a model centred on its bounds): x in
  [-k, k] with the source quad's UVs, v growing with +X, on a texture that is transparent on rows v < 1/2. So a line
  runs x in [0, k] from the marker forward, and every aspect shares one texture. Its far end is the quad's edge; its
  start is the texture edge at v = 1/2, soft by half a texel of the active mip (round 4b reverted the two-quad model
  of M1-FIX-LINE-FILL, which drew only part of the line in game).
  `PieceWorldPose` / `MarkerModelToWorld` are the placement the host test pins against `Geo::Contains`.
- Lanes: `ExceptOne`'s last lane is clamped inside the frame and overlaps the one before it; its marker draws only
  the strip the others leave uncovered (`CoverLaneDrawnStrip`, `ApplyDrawStrip`), its hitbox stays the whole lane.
  Lanes laid along the second axis of a set draw in the encounter's `crossLanes` color (yellow) and crossing cells in
  `cells` (purple), outline only until they burn, so a crossing does not stack six layers of one color.
- Both layers use the same rounding, so the full fill is exactly the outline.
- The textures keep a transparent margin (1/16 of the side) at every mip level, so no edge texel is stretched along
  the quad edge (the first build's "lines at the cardinals").
- The resolve caster is the mechanic's first outline marker (the carrier's ring for Spread/RunOut, the owner's ring
  for RunOut `From(Owner)`); a mechanic with no marker (`NoTelegraph`, Bait) casts from a hidden marker summoned for
  that cast only.

### Ground snap (`BossMechanics.Telegraph.GroundSnap`)

Off by default (playtest 2026-11-03). The marker skins carry the projected-texture batch flag, so the client paints
each marker onto the ground around it and a marker at the anchor's height already drapes over rolling terrain; the
notched lanes and chevrons first blamed on terrain were the projected smear of the old line textures' edge row. Indoors
the snap took the highest sample (furniture) and lifted the marker 1.6 yd, where the small growing fill no longer
reached the floor. With `GroundSnap = 1` each marker piece samples the floor under its footprint
(`src/MechanicGround.h`) and stands `Lift` above the highest sample:

- a line piece: a grid along its length and across its width about every `GroundSampleYd` (2 yd), ends and edges
  included; a disc, ring or dashed ring: the centre plus rings out to the radius; a donut: rings from its drawn hole
  out; a cone: the apex plus arcs. At most 160 points per piece (the step grows for big markers).
- Heights come from `Map::GetHeight(phase, x, y, z, vmap, maxDown)` (terrain, WMOs, gameobjects). The first search
  starts 2 yd above the anchor, so a bridge deck or a ceiling overhead is never seen; only when it finds no floor in
  the band (the ground rises: a hill) does a second search start from the band's top. A floor counts only within the
  shape's `zTolerance` (6 yd) of the anchor z, the hitbox's own band; nothing accepted: the anchor z.
- A static line (point anchors, no shrinking projectile) whose ground spans more than `MaxPieceSpreadYd` (0.75 yd)
  under one piece is cut into shorter end-to-end ladder pieces (`SplitLine`), each as long as the limit allows, none
  but the last shorter than one width (at least 2 yd), each at its own height. The pieces abut exactly like the
  default layout (only the last one overshoots, at the far end); flat ground gives the default layout. The fill keeps
  one piece per outline piece, scaled from the source; a fill piece stands at the highest outline piece its own extent
  overlaps (its own piece's height once full). Discs, rings, cones, donuts and dashed mine rings are never cut.
- Moving markers (unit anchors, `Line(from, to)` with a unit end, Spread/RunOut carriers, Stack/Soak on a unit)
  sample a cheap set on every follow step: a line piece's centreline at its ends, quarters and middle plus its two
  edges, a disc's centre and six rim points, a cone's apex, middle and three arc points (up to twice that many queries
  on rising ground).
  Shrinking projectile lines are not cut; each piece samples its whole footprint once (cached per piece).
- Cost: a 40 x 6.67 lane is about 110-120 height queries; `.mech test lanes 6 4000 count=3 cross=1` (6 lanes and 9
  cells) about 900-1000 at the spawn, a 20 yd circle about 120. `GroundSampleYd = 3` roughly halves them. With
  `Debug = 1` every mechanic logs its query count and spawn time once.

### Follow modes (`BossMechanics.Telegraph.FollowMode`)

Markers (both layers) on a unit anchor, on a `Line(from, to)` with a unit end, and on Spread/RunOut carriers are moved
every `FollowIntervalMs` when the target moved more than 0.25 yd or turned more than 2 degrees. The hitbox always reads
the live anchor; the marker trails by up to one interval.

| Mode | How | Notes |
| --- | --- | --- |
| 0 (default) | one flying spline per interval, move + face together, speed = distance / interval | turns cones |
| 1 | `MotionMaster::MoveFollow(unit, 0, 0)` once | circles/rings on a unit only; cones and lines still use mode 0 |
| 2 | `NearTeleportTo` every interval | jumpy, always correct |

`ShrinkProjectileLines = 1` (the default since round 4b) shortens a single-pass projectile line's marker behind the
projectile's tail (cosmetic; pieces are re-picked and respawned when their count changes). The drawn start moves in
steps of the line width, at least 1 yd (`ShrunkExcludeStart`): each step swaps the outline's model, which the client
redraws with a brief flicker, so fewer, bigger steps flicker less. A swapped piece jumps to its new start in the same
tick (`NearTeleportTo`) instead of gliding there while its far end overshoots.

## `.mech` (GM, in game)

```
.mech test <shape> <size> <ms> [key=value ...]
.mech clear [all] | .mech list | .mech ledger [name] | .mech weakened <0-7> | .mech markers [range]
.mech palette <out color> <in color> [cross-lane color] [cell color]
.mech barrage [secs=30] [every=900] [warn=2000] [shapes=circle,donut,cone,line,projectile] [dmg=1] [self=0] [seed=]
.mech minefield [count=10] [radius=3] [spread=15] [arm=3000] [grace=500] [dmg=1] [seed=]
.mech bots [all]
```

GMs are never valid targets: use `.gm off` (the command reminds you). The test encounter's owner is a hostile Mechanic
Test Owner (300401, faction 14, the GM's level) summoned at the GM; one per map, reused; it despawns after 5 minutes
without a live mechanic or on `.mech clear`, which ends its encounter (markers, the Weakened it stacked).

Test mechanics only target, count and hit the GMs who ran `.mech test` and their group (raid) members, and in the open
world only within `BossMechanics.OpenWorld.CandidateRadius` of the test owner: bots and players outside the GM's group
are never touched. Invite the bots you test with.

| shape | size means | keys (defaults) |
| --- | --- | --- |
| circle | radius | — |
| donut | outer radius | `inner=` (size/2) |
| cone | length | `angle=` (90), `offset=` (0) |
| rearcone | length | `angle=` (90); offset 180 |
| line | length | `width=` (4), `exclude=` (0) |
| projectile | length | `width=` (3), `speed=` (10), `back=0/1`, `pause=` (0), `exclude=` (0) |
| mine | radius | `grace=` (500), `cap=` (0 = none), `group=` (0), `life=` (0 = until set off or cleared) |
| lanes | lane width | `count=` (3), `gap=` (6), `axis=x/y`, `frame=` (40x40), `safe=1`, `cross=1`, `burn=` (6000) |
| spread | radius | `count=` (0 = everyone) |
| runout | radius | target = selected unit; `from=owner` measures from the test owner |
| stack | radius | anchored on the selected unit (else the point) |
| soak | radius | `need=` (2) |

A mine arms after `<ms>`. `cap=n` keeps at most n live mines per `group=` (`Cap(group, n)`): the oldest one of that
group fizzles, so mine sets with different groups cap independently. `safe=1` fires every lane but one; `cross=1`
adds lanes on the other axis and a burning cell at every crossing (for `burn=` ms, ticking every `tick=`, `pct=` per
tick). The frame is centred on the anchor point, X along the GM's facing. With `cross=1` the second axis draws
yellow and the cells purple (outline only until they burn) unless `color=` sets one color for everything.

Common keys: `dmg=` flat (0 = telegraph only), `pct=` (20, percent of reference health), `tankpct=`,
`school=physical|holy|fire|nature|frost|shadow|arcane` (physical), `weak=0/1` (0), `once=1`, `persist=<ms>`,
`tick=<ms>` (1000), `anchor=point|me|target` (point: the selected unit's position, else the GM's; facing = the
GM's), `color=red|yellow|blue|purple|orange|green|teal|white|black`, `repeat=` (1, max 50), `every=` (1000 ms;
repeats are registered from the test owner's scheduler), `scatter=` (0 yd: random offset per repeat, point anchors),
`tele=0` (no marker), `at=member` and `seed=<n>` (see below). Damage keys do not apply to stack and soak, nor do
`weak=`/`once=` (Weakened lands only from resolve hits). Keys that do not fit the shape are listed back as ignored.

`at=member` lands every repeat on a random alive member of the GM's group on the map, other than the GM (`self=1`
includes the GM). The member is picked when that repeat registers, so delayed repeats follow the bots; the zone is a
point anchor at the member's position then (facing = the GM's), and `scatter=` offsets it. It needs `anchor=point`
(the default) and fails when the group has nobody to land on. `runout` takes the member as its target. `seed=<n>`
seeds the command's own picks (scatter, member, barrage shapes) so a run can be repeated; without it a random seed is
used and printed. Lane placement keeps the registry's own RNG. In the open world the target scope still applies
(`BossMechanics.OpenWorld.CandidateRadius` around the test owner), so keep the bots near the GM.

`.mech barrage` and `.mech minefield` are the bot acceptance runs (the bots in the GM's group, `.gm off`):

- `.mech barrage [secs=30] [every=900] [warn=2000] [shapes=...] [dmg=1] [self=0] [seed=]`: for `secs` seconds, one
  single Zone every `every` ms (100-60000) on a random group member (`self=1` includes the GM), `warn` ms warning
  (0-60000). Each shot picks a kind from `shapes=` (comma separated, any of the five) and its geometry around that
  member: circle r 3-6 on the member; donut 3/8 centred 0-2 yd off the member (the member stands in the safe hole:
  stay in it); cone 60 or 90 degrees, length 15, apex 8 yd from the member facing it; line length 24, width 4, source
  12 yd out at a random bearing, through the member; projectile the same line, width 3, speed 12. `dmg=1` (flat)
  keeps hits visible in `.mech ledger` without killing; `dmg=0` is telegraph only. Ledger tags: 1 circle, 2 donut,
  3 cone, 4 line, 5 projectile. The first shot registers at once, the rest from the test owner's scheduler, so
  `.mech clear` stops the series. The reply prints the shot count and the seed.
- `.mech minefield [count=10] [radius=3] [spread=15] [arm=3000] [grace=500] [dmg=1] [seed=]`: `count` mines of
  `radius` at random points within `spread` yd of the selected unit (else the GM), snapped to the floor, arming after
  `arm` ms with `grace` ms of grace, all in one cap group with cap = count (they never fizzle each other; a second
  minefield gets its own group), `life` 120 s. Mines are tagged 1..count in the ledger. `.mech clear` removes them.
- `.mech bots [all]`: per bot in the GM's group (`all`: every bot on the map, to check the gate on bot-only groups):
  the boss-mechanics strategy status, mode, target and distance, time left to leave, counters, ledger hits. The report
  is part of the playerbots glue; a build without playerbots prints that bot support is not compiled.

- `.mech list`: id, kind, shape, color, tag, time to resolve and to expiry, marker count.
- `.mech markers [range]`: every live marker within range (100 yd) of the GM, one line each: mechanic id, layer
  (outline / fill), display decoded (shape, color, ladder step, line aspect k), the scale the creature really has and
  the one the server asked for, native and current display, x y z o, distance and bearing from the GM (absolute and
  relative to the GM's facing), and the drawn extent the server intends (a line's length k x scale and its far end).
  With `Debug = 1` the same line is logged for every marker spawn, display swap and scale change.
- `.mech ledger [name]`: per encounter, per-player failure counts by tag; with a name, also that player's last ten
  events.
- `.mech weakened n`: sets n stacks on the selected player (yourself when none), for the threat check.
- `.mech palette out in [cross] [cell]`: the test encounter's get-out and get-in colors for new test mechanics (any
  colors), and optionally the second-axis lane and crossing cell colors (default yellow, purple).
- `.mech clear`: ends the `.mech test` encounters only (markers, mechanics, ledger, the Weakened they stacked), drops
  combat with the test owner and despawns it. A real boss fight on the map keeps its mechanics.
- `.mech clear all`: ends **every** encounter on the map, real bosses included (the boss's next registration then
  runs as a bare implicit encounter: default palette, no boss state binding, no failure listener).
- A mine without `life=` never expires, so it keeps the test owner from its idle despawn: `.mech clear` it.

## Bots

Playerbots dodge every `Zone` (and `Frontal`, unless the tank exemption covers them) from the registry's exact
geometry, never from the markers. The glue (`MechanicBot*`) compiles only when mod-playerbots is enabled
(`MechanicBotGuard.h`: `MOD_PLAYERBOTS` and `__has_include("PlayerbotAI.h")`); without it `AddSC_boss_mechanics_bots`
is empty and `.mech bots` says so. No mod-playerbots or core edits.

### Strategy

| Kind | Name | Relevance |
| --- | --- | --- |
| strategy | `boss mechanics` (generic: combat and non-combat engines) | |
| trigger / action | `boss mechanic reposition` -> `boss mechanic move` (a `MovementAction`, `MOVEMENT_FORCED`) | 96 |
| trigger / action | `boss mechanic hold` -> `boss mechanic hold` (stop, clear the motion master, face) | 95 |
| multiplier | `boss mechanic lock` | |
| value | `boss mechanic state` | |

Both triggers run the brain once per game-time millisecond. It keeps a view of the registry (rebuilt on a registry
revision change, every 200 ms while not Idle, every `MovingAnchorRecheckMs` with unit-anchored hazards, every second
while Idle on a map with mechanics) and runs the mode machine:

- **Idle**: nothing relevant. A hazard is relevant when its windows meet `[now, now + HorizonMs]` and the bot could
  reach its region (bounds + margin) before its first window plus 2 s, plus 3 yd: an open or endless hazard (a
  persistent zone, a 120 s mine) keeps only the bots near it in Watch. A relevant hazard -> Watch.
- **Watch**: relevant hazards; the spot and the current motion are safe. Spot unsafe -> solve -> Escape (while the
  solve waits for the rate limit or the per-map budget, the lock already acts as in Escape). Only the bot's current
  spline (follow, chase) crosses a hazard -> re-route: a solve with the spline's end as the role anchor and without
  the bot's own spot; a clean target at least 1 yd closer to that end -> Escape there, then straight back to Watch,
  where the stock motion resumes (and is re-routed again if it still crosses). No clean way around, or the spline's
  end is itself unsafe -> Hold where it stands (until the hazard ends, at most `HoldCapMs`).
- **Escape**: walking the solver's path. Within 0.6 yd, or the move we sent ended short at a spot that is safe
  anyway -> Hold. Path or target turned unsafe -> re-solve. No progress (0.5 yd) for 500 ms since the escape began or
  the bot last moved (re-sends do not restart this clock; refused sends count as no progress) -> re-solve; the same
  target again -> released. Rooted, stunned or polymorphed with no progress -> released until it can move. One escape
  past 2 x its planned travel + 1.5 s, or an unbroken run of escapes past 10 s -> released.
- **Released** (Watch for 1.5 s, or until a root ends): the stock AI has the bot back. Nothing is locked, no cast is
  broken and an unsafe spot is not solved, so a stuck or rooted bot heals, casts and follows; then it tries again.
- **Hold**: at the safe spot until `holdUntil` (latest window end it answers, capped at `HoldCapMs`) -> Watch. Spot
  unsafe -> re-solve; out of the role band for `RoleSlackMs` -> Watch (stock movement takes it back, policed).

Solves are rate-limited by `MinSolveIntervalMs`, except the first after a registry change, and limited to 8 per map
update across all bots of a map (the rest solve on the next update). Our move clears the pending wait of the last move
(ours or a foreign `MOVEMENT_FORCED` hold) so the escape is never refused; a move to the same target is re-sent at
most every 250 ms (after a refused send, a partial path or a spline that ended short).

### Gate

Installed on every bot, master or not, by login, map change and a sweep every `GateSweepMs` (repairs
`ResetStrategies`): on an instance map `boss mechanics` (both engines) plus `avoid aoe` (combat, when
`AvoidAoeInInstances`); in the open world `boss mechanics` only, while the bot's group holds a GM who ran `.mech test`
on that map (`OpenWorldTestScope`); otherwise stripped. `avoid aoe` is stripped only when the gate added it and the
stock rule (`Playerbots.AutoAvoidAoe` with a real master) would not have. `Bots.Enable` is latched at the first world
tick, when the contexts are appended to the ten class lists.

### Waker and cast break

Right after each registry tick (full map updates) the waker sets `SetNextCheckDelay(0)` for a bot whose view is behind
the registry revision, for which a mechanic hidden by fallibility becomes visible, or that is not Idle and has not run
its brain for `FastTickMs` (bots without a real master otherwise tick every 0.5-3 s). A bot preparing a cast does not
run its engine (`PlayerbotAI::UpdateAI` yields only in `SPELL_STATE_PREPARING`; a channel is `SPELL_STATE_CASTING`
and the brain keeps running through it), so the waker refreshes its `leaveBy` and interrupts a cast or channel that
would end after `leaveBy - SafetyMarginMs`, unless the bot is released. After a break the lock holds back cast-time
spells and item uses for 500 ms, so the cast is not restarted and broken again.

`leaveBy` already holds `SafetyMarginMs` once (the window is widened by it); the cast rule subtracts it a second time
on purpose, for the AI tick between the cast's end and the escape's first step. Casts thus end 300 ms (default)
earlier than the bare margin would allow.

### Lock

- **Idle, Watch, released**: nothing is blocked, except: Watch with an unsafe spot whose solve is deferred locks as
  Escape, and for 500 ms after a cast break cast-time spells and item uses are blocked in every mode.
- **Escape**: blocked: every movement action but ours, movement spells, any spell with a cast time or a channel.
  Allowed: instants (heals too), auto-attack and the stock target picks, `set facing`, ours.
- **Hold**: blocked: every movement action but ours, movement spells, a cast or channel that would end after
  `leaveBy - SafetyMarginMs`. Allowed: casts that end in time, instants, auto-attack, `set facing`, ours.

Movement spells: charge, leap, leap back, jump, jump dest and teleport effects, `CastReachTargetSpellAction`, and the
named rework movers (`heroic leap`, `cmbt grappling hook`, `cmbt blade rush`, `shadowstep`, `blink back`,
`disengage`); see `MechanicBotLock.cpp`. Order: our move (96) and hold (95) > `avoid aoe` (90) > encounter actions
(60s) > rotation.

### Config (`BossMechanics.Bots.*`)

| Key | Default | What |
| --- | --- | --- |
| `Enable` | 1 | 0: register nothing (latched at startup) |
| `AvoidAoeInInstances` | 1 | the gate adds `avoid aoe` to every instance bot |
| `OpenWorldTestScope` | 1 | outside instances: bots grouped with a GM who ran `.mech test` |
| `GateSweepMs` | 1000 | gate sweep interval |
| `FastTickMs` | 100 | waker: longest gap between an engaged bot's AI ticks (0 = every map update) |
| `StartDelayMs` | 150 | solver: assumed delay before a move starts |
| `SafetyMarginYd` / `SafetyMarginMs` | 0.75 / 300 | hazard edge distance (min 0.25) / widening of hot windows |
| `PostResolveHoldMs` | 500 | a plain Zone stays live this long past its resolve |
| `GraceMarginMs` | 250 | armed-mine crossing budget = grace - this |
| `HorizonMs` | 10000 | windows starting later are ignored |
| `HoldCapMs` | 3000 | longest hold for persistent and endless hazards |
| `MaxCandidates` / `MaxPathChecks` | 400 / 8 | per solve (path checks x2 with the grace pass, +1 fallback) |
| `GridStepYd` | 2.0 | lane-frame grid |
| `MinSolveIntervalMs` | 150 | per bot, except the first solve after a registry change |
| `MovingAnchorRecheckMs` | 250 | re-view cadence while a hazard is unit-anchored |
| `RoleSlackMs` | 1000 | out of the role band this long while holding -> release the hold |
| `RangedDistanceYd` | 22 | ranged and healer band [8, this + 6] (melee: melee range - 0.5) |
| `FacingSlackDeg` | 15 | turning cones and lines are checked at +-this |
| `Debug` | 0 | one log line per solve, and the view self-check (`module.bossmechanics.bots`) |
| `Fallibility.Enable` | 0 | D1 human-like mistakes, deterministic under `Fallibility.Seed` (0 = random per load) |

Fallibility keys (all off unless `Enable`): `ReactionMs` 350 + `JitterMs` 0-250 per bot before a mechanic is seen,
`LateChancePct` 5 for `LateExtraMs` 700 more, `WrongSideChancePct` 3 to ignore it until `RealiseMs` 700 before its
first window, `ScaleNormal` / `ScaleHeroic` 1.0. The out-of-combat role band is the follow distance + 2.

Rolls are keyed on (seed, bot guid, registry mechanic id). Mechanic ids keep counting per map, so the same seed
replays the same perception for the same mechanic id, not for a second run of `.mech barrage seed=1` on the same map
(its mechanics get new ids): two runs draw similar, not identical, mistakes.

### `.mech bots [all]`

Per bot in the GM's group (`all`: every bot on the map): the strategies installed, mode, target and distance, time
left before it must leave, its ledger failures, and its counters (solves by outcome, path checks, late arrivals,
stuck re-solves, released escapes, motion stops and how many of them were re-routed, lock blocks by moves and casts,
cast breaks, wakes, solve time last / max / mean in microseconds).

### Limits

- No navmesh on a map: every path check fails, so the solver falls back to `Fallback` / `NoPath` (logged once per bot
  and mechanic).
- Zones anchored on the bot itself (or lines aimed at it) are RunOut-shaped and ignored by their carrier (M3).
- Actions run through `PlayerbotAI::DoSpecificAction` (chat `do`, `move to loot` from item use, pull pre-actions) skip
  every multiplier, so the lock cannot stop them; engine reactions (knockback, teleport acks) are not actions.
- The lock knows a cast by `CastSpellAction` (and items by `UseItemAction`). A plain `Action` that calls `CastSpell`
  itself can still start a cast-time spell in Hold; the waker breaks it, at most once per 500 ms window (M4 class
  audit).
- Re-routing around a hazard is greedy, one hop at a time towards the end of the stock motion. A route that needs to
  go away from the master first (a U-shaped wall of mines) is not found: the bot holds until the hazard ends or
  `HoldCapMs`, then tries again.
- A bot released after a stuck escape stands where it is for up to 1.5 s, inside the hazard if that is where it got
  stuck (no navmesh way out); it heals and casts meanwhile.
- The barrage donut tests "stay in the hole" only while `SafetyMarginYd` < 1 (`MechanicBarrage.h`).

### Credits

The context registration and the strategy gate follow mod-dungeon-clear (Jared Wright, AGPL-3.0-or-later),
`DungeonClearModule` and `DcStrategyGate`; the solver's rule to validate path legs, not corners, and to validate a
target against the same band the trigger fires on is from its `DcHazard`.

## Config

`conf/mod_boss_mechanics.conf.dist`, read at startup and on `.reload config`:

| Key | Default | What |
| --- | --- | --- |
| `BossMechanics.Enable` | 1 | 0 refuses every registration |
| `BossMechanics.Debug` | 0 | logs every resolve, failure and the map tick diff |
| `BossMechanics.ReferenceHealth.Rows` | `20:1000:1500 60:5000:8000` | stub `level:nonTank:tank` rows |
| `BossMechanics.Telegraph.FollowMode` | 0 | 0 spline step, 1 MoveFollow, 2 teleport |
| `BossMechanics.Telegraph.FollowIntervalMs` | 200 | follow interval (min 50) |
| `BossMechanics.Telegraph.Lift` | 0.3 | height above the highest ground sample (yd; GroundSnap 0: above the anchor) |
| `BossMechanics.Telegraph.GroundSnap` | 0 | 1: each marker stands Lift above the highest floor under it |
| `BossMechanics.Telegraph.GroundSampleYd` | 2 | ground sample spacing under a marker (min 0.5) |
| `BossMechanics.Telegraph.MaxPieceSpreadYd` | 0.75 | a static line piece spanning more height than this is cut |
| `BossMechanics.Telegraph.ShrinkProjectileLines` | 1 | shorten projectile markers behind the projectile |
| `BossMechanics.Telegraph.Fill` | 1 | 0: no fill timer (the fill still shows a live zone; mines unchanged) |
| `BossMechanics.Telegraph.FillStepMs` | 150 | longest fill step (min 50) |
| `BossMechanics.Telegraph.FillStepYd` | 0.4 | fill edge moves at most this per step (>= 50 ms; 0 = fixed step) |
| `BossMechanics.Telegraph.FillLeadMs` | 150 | fill computed this far ahead (client scale blend; max 2000) |
| `BossMechanics.MaxActivePerMap` | 256 | registrations above it are refused with a warning |
| `BossMechanics.OpenWorld.CandidateRadius` | 100 | open world: farther from the owner never takes part (0 = map) |

## Ids

| Id | What |
| --- | --- |
| 205000 | Weakened (`spell_boss_mechanics_weakened`) |
| 205001-205007 | Mechanic Impact per school (205001 + school), `spell_boss_mechanics_resolve` |
| 300400 | Mechanic Marker (`npc_boss_mechanic_marker`) |
| 300401 | Mechanic Test Owner (`npc_boss_mechanic_marker`) |
| 91000-91833 | Marker displays: 91000 + 100c + offset + step (`Ids::MarkerDisplay`; c = color 0-8, `Color` order) |
| | offsets: disc 0, ring 1, timer ring 2, donut 3+j, donut outline 10+j, cone 17+j, cone outline 25+j, |
| | dashed ring 33 (34 per color; +34..+99 spare) |
| 92000-92856 | Line (fill) displays: 92000 + 100c + i, i = 0..56 (+57..+99 spare) |
| 93000-93856 | Line (outline) displays: 93000 + 100c + i |
| rest of 91000-93999 | spare (91990-91995: the asset script's opt-in diagnostics); the old slice 90500-90699 is free |

The spell and creature rows come from dbc-tools (`apps/dbc-tools/source/npcs/boss_mechanics.py`), the displays and
models from `apps/dbc-tools/patch_bossmechanics_markers.py`. A dungeon that wants its own combat-log name declares a
copy of the resolve spell bound to `spell_boss_mechanics_resolve` and passes it with `.Spell(id)`.

## Known limits (accepted for M1)

- Open world: player-picking kinds iterate the map's player list on each evaluation and keep those within
  `OpenWorld.CandidateRadius` of the owner. Correct, but on a continent with thousands of bots a long-lived mine still
  walks the whole list every map update (a grid search around the anchor would fix it if open-world bosses ever use
  mines).
- A line shorter than a quarter of its width draws a quarter of its width long (above).
- Combat link: a player hit while already in combat gets a combat reference to the marker that cast the hit and to
  the owner, until the marker despawns (encounter end despawns every marker; the owner is the boss anyway). The resolve
  spell has no initial aggro, so out-of-combat players are never put in combat. `.mech test` caveat: a tester already
  in combat (hitting a dummy) is linked to the test owner, which never evades, so they stay in combat until
  `.mech clear` (which drops it) or the test owner's 5-minute idle despawn.
- Hidden caster per pass: `NoTelegraph` mechanics and every Bait summon a hidden marker per resolve pass and despawn
  it right after the cast (a markerless projectile: one per tick while it passes; in dungeons each one also runs
  mod-autobalance's creature evaluation). Leak-free and fine for M1; the cheap upgrade (one hidden caster per mechanic,
  spawned at the commit and despawned with the markers) waits for the Pilot.
- AutoBalance: mod-autobalance scales damage by its **source**, the boss, and its never-modify list is hard-coded (no
  config key for spell ids), so adding 300400/300401 to `AutoBalance.DisabledID` does not help. Resolve hits inside
  dungeons stay AutoBalance-scaled until difficulties stage E0 turns AutoBalance off for 5-player maps (or a
  mod-autobalance fork edit adds 205001-205007 and the dungeon copies to its never-modify list). Check `dmg=`/`pct=`
  numbers in the open world only; the Pilot's damage percentages are not deterministic until E0.

## Spike checks (first in-game look, before the playtest)

| # | Check | If it fails |
| --- | --- | --- |
| S1 | A disc shows its texture through the display's skin | rerun the marker asset script with `--no-skins` |
| S2 | Blend 2 paints over the floor (not additive glow) | try blend 3 (alpha key) |
| S3 | Marker on the floor: no z-fighting, not hidden by bumps | `Telegraph.GroundSnap`, `GroundSampleYd`, `Lift` |
| S4 | `circle 2`, `circle 10`, `circle 40` match 2/10/40 yd (`.gps` at the edge) | fix the model radius |
| S8 | The outline appears at full size at once; the fill grows and meets it as the hit lands | `Telegraph.FillLeadMs` |
| S5 | `cone 20 3000` points where the GM faces | flip the texture orientation |
| S6 | No blob shadow, nameplate, name or selection circle on markers | CreatureModelData flags; unit flags |
| S7 | `circle 5 8000 anchor=target` on a moving bot is smooth; a cone turns with it | `FollowMode` 1, then 2 |
