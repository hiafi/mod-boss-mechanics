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
| `src/MechanicRoute.h` | Pure route search around hazards (grid A*, string-pulled legs): `Route`, `RouteHandoff` |
| `src/MechanicFallibility.h` | Pure seeded bot mistakes: reaction delay, late and wrong-side picks |
| `src/MechanicBotView.h/.cpp` | Registry -> solver input and tasks for one bot; reservations; navmesh, sight oracle |
| `src/MechanicViewRules.h` | Pure player-kind rules of the view: planned spots, margins, separation (host-tested) |
| `src/MechanicBotGuard.h` | `BOSS_MECHANICS_WITH_BOTS`: 1 only with mod-playerbots enabled |
| `src/MechanicBotState.h` | Per-bot state and its playerbots value (`boss mechanic state`) |
| `src/MechanicBotBrain.h/.cpp` | Per-bot update: view, mode machine, solve, leaveBy, reservations, tasks, routes |
| `src/MechanicBotRole.h/.cpp` | Role band v2 (healer sight and reach), tank facing ring, tank parking input |
| `src/MechanicBotFacing.h/.cpp` | Look-away facing: the owed facing, the lock's and the waker's facing checks |
| `src/MechanicBotStrategy.h/.cpp` | `boss mechanics` strategy, its three triggers and three actions |
| `src/MechanicBotLock.h/.cpp` | Lock multiplier: what may run while a bot escapes, holds or freezes (one block list) |
| `src/MechanicBotContexts.h` | The four playerbots named-object contexts |
| `src/MechanicBotGate.h/.cpp` | Context registration, strategy gate, `AddSC_boss_mechanics_bots` |
| `src/MechanicBotWaker.cpp` | After-tick waker and cast break; mode clocks and lock leak detectors (M4) |
| `src/MechanicLeaks.h` | Pure lock leak detectors: what counts as a leaked move, cast or turn, and our own tokens |
| `src/MechanicCost.h` | Pure cost counters: microsecond histograms, per-mechanic path calls (`CostStats`) |
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
| `Stack(owner, spec, warn)`, `Soak(owner, spec, count, warn)` | Any shape (`ZoneSpec`; a lane: `Shapes::LineFacing`) |
| `Bait(owner, rule, othersRange, warn)` | The hit lands on the farthest player; that player taking it is the job |
| `HoldStill(owner, players, window, warn)`, `Spot(owner, spots, warn, r)` | Movement / position checks |
| `Cancel`, `Detonate`, `Contains`, `ContainsPoint`, `Failures`, `OnFailure` | Handles, ledger, failure callback |
| `Read(map)` | Bots: the map's registry (`nullptr` when none) |
| `RefFlat(owner, pct, tank)` | Script spells: `pct`% of the owner's reference row ([below](#reference-health)) |

### Options (`MechanicRef`)

`Damage(pct)`, `TankDamage(pct)`, `FlatDamage(n)`, `School(s)`, `Spell(id)`, `Rider(spellId)`, `Weakened()`,
`WeakenedOncePerPlayer()`, `Persistent(duration, tick)`, `Projectile(speed)`, `ProjectileUnit(unit)`,
`OutAndBack(pause)`, `Lifetime(ms)` (mines), `Cap(group, n)`, `ExcludeSourceEnd(yd)`, `FacingOffset(deg)`, `Rear()`,
`AnchorOn(unit)`, `FollowFacing()`, `ZTolerance(yd)`, `Eligible(...)`, `Order(...)`, `Count(n)`, `Targets(set)`,
`SelfTrigger(spell, stacks)`, `From(MeasuredFrom)`, `TankExempt()`, `Color(c)`, `Tag(n)`, `NoTelegraph()`,
`NoFillTimer()`, `OnHit(fn)`, `OnResolve(fn)`.

M3 options:

| Option | Fits | What |
| --- | --- | --- |
| `Persistent(duration, tick)` | + Stack, Soak | judged at every tick (tick 0: once, then it lingers) |
| `Cluster(n)` | circle Stack | no anchor: each eligible player needs `n` others within the radius (Assembly) |
| `Radius(yd)` | Bait | the landing circle (Fenrus 6 yd); default: only the picked player |
| `Tolerance(yd)` | HoldStill | movement allowed in the window (default 0.3 yd; Alexei 1 yd) |
| `Park(unit)` | shape kinds | the tank holds `unit` (default the owner) inside this shape; bots only |

`Cluster(n)` must be set in the statement that registers the Stack; once the Stack has committed it is refused.

Damage is a percent of the reference health (`ReferenceHealthProvider`; keyed rows from the config, see
[Reference health](#reference-health)), one flat number per target set by the resolve spell, so boss buffs and the creature damage rate never change it
(`SPELL_ATTR3_IGNORE_CASTER_MODIFIERS`). Physical mechanics are reduced by armor; no crit, miss, reflect or partial
resist. A hit fires the victim's "when hit" procs (`SPELL_ATTR3_NOT_A_PROC` lets the triggered cast proc: Reckoning,
Blood Craze, Vengeance-style talents), like a boss ability would.

### Reference health

`BossMechanics.ReferenceHealth.Rows` holds one row per content key: `<level>[tier]:<nonTank>:<tank>`, the tier
suffix none (normal), `h` (heroic) or `e<N>` (empowered level N). The shipped default
`20:1300:1290 60:3670:3710 60h:4120:4110` is the E3 sim measurement (2026-11-05); mod-dpssim `tools/refhealth.py`
prints a fresh string to paste after a gear or stat change. The pure rules are
`src/MechanicRefHealth.h` (tests: `tests/ref_health_test.cpp`).

- **Content key** (`ContentKeyProvider`, default `DefaultContentKey`): heroic = `Map::IsHeroic()` (the open world and
  the sim are normal); level = the map's `BossMechanics.ReferenceHealth.MapLevels` pin for that difficulty
  (`36:20`, `36h:60`: one pin per difficulty, so `36:20` alone leaves heroic on the owner's level), else the owner's
  level; empowered level 0. The empowered work (E4) installs its own provider that calls `DefaultContentKey` and sets
  the empowered level.
- **Lookup**: within the key's tier, interpolated by level and clamped at both ends. Empowered N with no `e<N>` rows
  reads the heroic rows, heroic with no `h` rows the normal rows. At least one normal row is required; a repeated key
  in one tier, a zero or signed number, or a level above 255 is malformed (a warning, then the default).
- The level-only stub format (`20:1000:1500 60:5000:8000`) is a valid table of normal rows: a heroic map then reads
  the normal rows by level, as before.
- `.mech test` prints the key and row it resolved (`Reference health (level 20 normal): non-tank 1000, tank 1500.`).
- **A script's own spells** (a SpellScript's hit damage, a custom DoT or chain): `Mechanic::RefFlat(owner, pct, tank)`
  returns `pct`% of the row the registry would pick for `owner` (same content key provider, same reference health
  provider), non-tank health or, with `tank`, tank health. The rounding and clamp are the registry's
  (`RefFlatFor` -> `FlatDamageFor`, `MechanicMath.h`), so `RefFlat(me, 15.f)` equals what `.Damage(15)` deals a non-tank
  and `RefFlat(me, 15.f, true)` what `.Damage(15).TankDamage(15)` deals the tank: Deadmines 195 / 194 normal, 618 /
  617 heroic. Call it while the owner is on its map (at cast or hit time); a null owner or one not in the world
  returns 0 (DEBUG log). It is not scaled by DD's heroic spell hook (custom ids are never scaled): the heroic row is
  the heroic number.

### Kinds

| Kind | Server resolve (M1) | Marker | Failure |
| --- | --- | --- | --- |
| Zone | resolve spell on everyone eligible inside | the shape's pieces; a mine: ring, then dashed ring | Hit |
| Frontal | as Zone, the source's victim is skipped (**) | cone | Hit |
| Spread | a circle on each carrier hits every other player inside | ring on each carrier | Hit |
| RunOut | as Spread on one target; `From(Owner)`: hit within r of the owner | ring on the target (*) | Hit |
| Stack | none (split damage via `onResolve`) | disc on the anchor (***) | MissedStack |
| Soak | none; `onResolve` gets the count | disc on the anchor (***) | MissedSoak (group) |
| Bait | hit on the farthest eligible player (`Radius`: everyone within it) | none | MissedBait |
| HoldStill | none | none | Moved (> 0.3 yd, or `Tolerance`) |
| Spot | none | disc on each spot | OffSpot |

(*) `From(Owner)`: the ring of r sits on the owner (the hitbox) and a small ring
(`Rules::kRunOutTargetMarkerRadiusYd`, 1.5 yd, same color) follows the target, so the target knows it must run.

(**) The victim of the cone's source when the source is a creature (the boss, an add, the `.mech test` mob), else the
owner's victim (M3 B7).

(***) A Stack or Soak of any other shape draws its pieces like a Zone, in the get-in color; a `Cluster` Stack draws a
get-in ring of its radius on each eligible player, like Spread rings.

Rules added in M3 (the pure rules live in `src/MechanicJudge.h`, tested by `tests/judge_test.cpp`):

- **A shape anchored on a player never hits that player** (decision 7): Zones and Frontals anchored on a player
  (Cookie's chowder cone, a circle under a player) skip that player in every pass (one-shot, persistent, projectile),
  and `Contains`/`ContainsPoint` with that player answer false. A player given as a line's `to` end is still hit.
- **Spread, RunOut and Bait circles hit players only** (decision 8): pets and guardians are skipped. Zones and
  Frontals still hit pets (damage only, never ledgered).
- **Soak needs at least `count` bodies**: fewer posts one `MissedSoak`; extra bodies never fail. Bots claim exactly
  `count` and everyone else keeps out of it. `count` 0 never fails.
- **Lasting Stack/Soak** (`Persistent`): `MissedStack` at most once per player per mechanic; `MissedSoak` once per
  short pass; `onResolve` once per pass. Its bot window is `[resolve, until]`.
- **Reservations** (bots, M3 handoff §3.6): `Reserve(id, {bot, point, until, since, facing, slot, claim})` replaces the
  bot's entry; `since` is when its current claim (claim, and a claim's slot) began, kept while that is unchanged;
  `Unreserve(id, bot)`; `ReservationRevision()` changes on a new entry, a changed point (> 0.1 yd), facing, slot or
  claim, an unreserve, an expiry or a mechanic with reservations removed, never on a re-reserve that only moves `until`,
  and never touches `Revision()`.

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
- Doing the mechanic is never a failure (user ruling 2026-11-04): the Bait landing on the player it chose is the job,
  so that player gets no ledger `Hit` and no Weakened, though the damage and any `Rider(spellId)` still land
  (`Judge::HitIsJob`). Anyone else inside the landing circle failed: a `Hit`, plus Weakened on a `.Weakened()` Bait.
  A bait rotation therefore counts its own debuff, never Weakened (the drill's stand-in: Baited 205008).
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
.mech minefield layout=u [gap=8] [depth=12] [radius=3] [arm=3000] [grace=500] [dmg=1]
.mech drill <smite|cookie|fenrus|alexei|soaks|ras|deadmines-followups|sfk-hunt> [cycles=3 (alexei 1)] [seed=] [dmg=1]
            [self=0]
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
| spread | radius | `count=` (0 = everyone), `self=<spell>:<min>` |
| runout | radius | target = selected unit; `from=owner` measures from the test owner; `count=`, `self=` |
| stack | radius | anchored on the selected unit (else the point); `cluster=`, `shape=line`, `persist=` |
| soak | radius | `need=` (2; 0 = nobody takes it), `order=`, `shape=line`, `persist=` |
| frontal | length | on the selected creature, its victim exempt; `angle=` (90), `rear=1`, `line=1` `width=` (4) |
| bait | others' range | `rule=fewest\|farthest` (fewest), `aura=` (205000), `r=` (6, the landing circle), `order=` |
| | | the baiter gets no Weakened, so `rule=fewest` needs `aura=` + `rider=` of a rotation debuff (205008) |
| holdstill | window (s) | `tol=` (0.3 yd); every eligible player |
| spot | radius | `ring=` (10): one spot per group member on a ring around the anchor point |
| hunt | catch radius | `speed=` (0.8, x player run speed), `persist=` (the chase, required), `tick=`, `pick=` |

A mine arms after `<ms>`. `cap=n` keeps at most n live mines per `group=` (`Cap(group, n)`): the oldest one of that
group fizzles, so mine sets with different groups cap independently. `safe=1` fires every lane but one; `cross=1`
adds lanes on the other axis and a burning cell at every crossing (for `burn=` ms, ticking every `tick=`, `pct=` per
tick). The frame is centred on the anchor point, X along the GM's facing. With `cross=1` the second axis draws
yellow and the cells purple (outline only until they burn) unless `color=` sets one color for everything.

Common keys: `dmg=` flat (0 = telegraph only), `pct=` (20, percent of reference health), `tankpct=`,
`school=physical|holy|fire|nature|frost|shadow|arcane` (physical), `weak=0/1` (0), `once=1`, `rider=<spell>` (cast on
every player hit, a Bait's job hit included; only the module's own spells 205008-205099, so a GM cannot cast arbitrary
spells through it, nor Weakened or a resolve spell; a spell of the block with no `spell_dbc` row is refused with its own
message), `persist=<ms>`, `tick=<ms>` (1000), `anchor=point|me|target|member` (point: the selected unit's
position, else the GM's; facing = the GM's), `color=red|yellow|blue|purple|orange|green|teal|white|black`, `repeat=` (1,
max 50), `every=` (1000 ms; repeats are registered from the test owner's scheduler), `scatter=` (0 yd: random offset per
repeat, point anchors), `tele=0` (no marker), `at=member` and `seed=<n>` (see below). Damage keys do not apply to stack,
soak, holdstill and spot, nor do `weak=`/`once=`/`rider=` (Weakened and riders land only from resolve hits). Keys that
do not fit the shape are listed back as ignored, except the placement keys `anchor=`, `at=`, `shift=`, `scatter=` and
`pick=`: they are read for every shape and simply have no effect where the shape does not use them (spread, runout, bait
and holdstill take their players from the group, frontal is always on the selected creature).

M3 keys (STAGE-M3-HANDOFF §3.11):

- `role=tank|healer|dps|nontank|any` and `aura=<spell>:<min>[:<max>]` (a stack band) set the eligibility, on any kind.
  On `bait`, `aura=<spell>` without a colon is the rule's aura instead.
- `self=<spell>:<min>` (spread, runout): the carriers are whoever holds that many stacks when it resolves (a stack
  change mid-warning counts). `self=0/1` (no colon) still means "the GM may be landed on".
- `order=<spell>[:asc|desc][:cap=<n>]` (soak, bait): claimers by stacks, players at `cap` or more never claim.
- `count=` on runout: that many random eligible targets instead of the selected unit.
- `persist=`/`tick=` on stack and soak: a lasting get-in shape judged at every tick.
- `shape=line` `length=` (30) `width=` (the size) on stack and soak: a lane centered on the anchor point along the
  GM's facing (on a unit anchor it starts at the unit). `cluster=<n>` on stack: no anchor, each eligible player needs
  n others within the radius (Assembly).
- `park=1` (circle, donut, cone, rearcone, line): the tank should hold the selected unit inside the zone; with
  `at=gm` the zone lands on you.
- `at=gm` (a point at you, the selection kept for `park=` and frontal sources) and `at=group` (the group's centroid,
  snapped to the floor). `shift=<ahead>:<left>` moves a point anchor that many yd along and left of your facing.
- `anchor=member`: a unit anchor on a random group member, picked as each repeat registers; `follow=1` makes a line turn
  with its unit anchor (a cone on a unit anchor always turns with it, `follow=` or not). `to=member|target|me` (line,
  projectile): aimed at that unit live, the length stays `size`. `pick=any|nontank|same` (with `at=member`,
  `anchor=member`, `to=member`): `nontank` skips the selected unit's victim, `same` reuses the member the previous pick
  of this command (or drill) landed on (when it is gone: a member other than the victim). Note the two meanings of
  "tank" in tests: `pick=nontank` (and the drills' `marker pick=nontank`) skip whoever the selected creature attacks,
  while `role=nontank` (and the Bait rule) go by the role provider, which in tests has no boss and falls back to the
  tank spec. Let a tank-spec bot hold the creature, so the two agree.
- `tag=<n>` (1): the ledger tag; repeats count up from it.
- Deadmines Pilot gap 8 keys: `at=last` lands on the far end of the last fixed line this command (or drill run)
  registered (none yet: the selected unit); `at=each` puts one shape under every member, where each stands now;
  `pick=other` (with `at=member`, `anchor=member`, `to=member`) picks a non-tank the chain has not picked since the
  last pick that was not `other` (none left: a non-tank other than the previous pick, then any non-tank), so a chain
  of three hits three members; on line and projectile, `min=<yd>` makes a `to=member` pick prefer members at least
  that far from the line's source (none: the farthest), `fix=1` freezes a `to=` line at registration (from where its
  source is now toward where the unit stands now, `size` long), and `reach=1` also ends it there (at least `size`
  long). `offset=<deg>` turns a frontal (not with `rear=1`). Drill steps also take `after=resolve` (start `atMs`
  after the previous step of the cycle resolves), `after=with` (`atMs` after it starts) and `cycle=even|odd` (only in
  those cycles); the runner strips them before the `.mech test` parser (`src/MechanicDrill.h`, `StepOffsets`).
- Shadowfang Keep build §3.9 (the chase, drill `sfk-hunt`): `hunt <r> <ms> speed=<rate> persist=<ms>` summons a
  chaser (a Mechanic Marker: invisible, not selectable or attackable) at the selected unit and fixates a member
  (`pick=`); a circle of r on the chaser, eligible for that member only, stands through the `<ms>` warning, then the
  chaser walks at the member for `persist` ms at `rate` x a player's 7 yd/s (its run speed is set to the rate, so the
  bots read the drift), re-aimed at every pass (`tick=`). The first pass that holds the member is a catch (its hit is
  the ledger entry) and ends the hunt; so does the member dying. `soak <r> <ms> bell=<n> [apart=15] [land=6]
  [jump=2000]` is an alarm bell: the soak lands on one of n points `apart` yd from their neighbors on a ring around
  the selected unit (the first at your facing), and the member of the command's (or drill run's) live hunt is not
  eligible. Filled at its resolve, it ends that hunt, the chaser bounds to the bell, and `jump` ms later a circle of
  `land` lands there with the soak line's damage keys: tag + 1 for everyone but the ringers, tag + 2 (no marker of its
  own) for the ringers. A hunt caught before the bell resolves leaves no landing; a bell with no hunt still lands.
- `bait` moves the test owner to the selected unit (else you): the bait lands on whoever is farthest from it.
- Stack thresholds use the Weakened aura: `.mech weakened <0-7>` sets stacks on the selected player (recorded on the
  test encounter, so `.mech clear` takes them off).

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
- `.mech minefield layout=u [gap=8] [depth=12] [radius=3] [arm=3000] [grace=500] [dmg=1]` (M4): a fixed U of
  overlapping mines (neighbors at most `radius` apart) opening toward the GM along their facing: two arms whose inner
  edges are `gap` yd apart, `depth` yd long, joined by a wall at the far end (`src/MechanicDrillPlan.h`, desk-checked
  by `tests/drill_plan_test.cpp`). No dice. Lead the bots into the pocket, then walk round the closed end: a bot must
  leave by the mouth (the M3 playtest's "bot left behind" repro). Same cap group, life and tags as above.
- `.mech drill <name> [cycles=3 (alexei 1)] [seed=] [dmg=1] [self=0]`: a boss doc's mechanics scripted on the **selected
  creature** (the boss stand-in; a bot should tank it for the frontals and the stack on it) and the group, one cycle
  after another. Each step is a `.mech test` line (`src/MechanicDrill.h`, desk-checked by `tests/drill_test.cpp`) run
  from the test owner's scheduler, so `.mech clear` stops it; damage steps get `dmg=` (flat, default 1), `self=1` lets
  member picks land on the GM, `cycles` 1-10. The drill-only step `marker <n> [pick=nontank]` sets exactly n stacks of
  Marked (drill) (205009, a harmless stand-in for a boss's eligibility or order aura) on one random member below n;
  `marker 0` takes it off every member. Drill setup never hands out Weakened, and never clears the Weakened members
  earned from real failures (M4 WP-F). The training dummy and the dpssim mechanics mode run the same drills
  through the runner (`src/MechanicDrillRunner.h`: `RunDrill`, `StartProgram`; the sim seam is
  `src/BossMechanicsSim.h`), with their scope player in place of the GM.
  - `smite` (Deadmines SM2, SM3, SM12; 10 s): frontal cone 90 / 8 yd (2 s), a rear cone registered 1.5 s later
    (resolves 1.5 s after the first), Spread 6 on every non-tank (2.5 s). Tags 1, 2, 3.
  - `cookie` (CK4-CK6; 31 s): chowder cone 60 / 8 yd on a random non-tank (follows its facing, 3 breaths 2 s apart
    from 2 s); Gut Bomb RunOut 8 on a non-tank (5 s); Fish Stew: RunOut 4 on a non-tank at three drops 2 s apart, a
    4 yd puddle (1.5 s warning, 8 s) where the eater stands at each drop. Tags 4, 6, 5, 7 (puddles).
  - `fenrus` (FN6; 10 s): Bait, fewest Baited (205008, the drill's rotation debuff) stacks, landing r 6, everyone
    else within 10 yd. `rider=205008` gives everyone the landing hits a Baited stack, the baiter included, so the next
    pick rotates; `weak=1` gives Weakened only to the others caught in the circle (the baiter doing the job gets
    none). The test owner (the Bait's reference) moves to the creature before each bait, so it follows the
    creature when the tank drags it. Tag 1.
  - `alexei` (AB2-AB8; 13 slots of 12 s): each rule alone with a 3 s warning and a 6 s window (No running: HoldStill
    6 s, 1 yd tolerance; Assembly: cluster 2, 6 yd; Keep your distance: Spread 8; Out of bounds: a 7 yd circle on
    the group for the window; Line up: a 4 x 40 yd lane through the group; desks: Spot 3 yd on a 12 yd ring), then
    each heroic pair with a 4 s warning (Out of bounds with each other rule, the lane then shifted 12 yd left;
    Assembly + Line up; Line up + Keep your distance). Tags: 2 still, 3 assembly, 4 distance, 5 bounds, 6 line up,
    7 desks. One cycle by default (2.6 minutes). The desks keep 8 yd apart for up to 9 members (ring 12).
  - `soaks` (26 s): Payday (1 body, non-tank, 2 yd, 2.5 s, near a random non-tank member); two non-tanks get 2
    and 1 Marked (drill) stacks, then an ordered soak (Marked ascending, `order=205009:asc:cap=2`) 6 yd ahead of the
    group (everyone back to 0 at 9 s); the fortune (need 0, 10 yd left of the group: nobody steps on it); a lasting
    3 yd soak on you for 5 s from 19 s into the cycle (walk). Tags 1-4.
  - `ras` (RF1, RF2, RF5; 12 s): a random non-tank gets 3 Marked (drill) stacks (standing in for Chill); a self-keyed
    RunOut 8 (`self=205009:3`) while a Stack 8 on the creature holds the players at 0-2 stacks; a second non-tank
    reaches 3 stacks 2 s in and joins the runners; everyone back to 0 at 9 s (the encounter records them, so its end
    and `.mech clear` also remove them). Tags 2 (stack), 5 (runout). Marked, not fenrus's Baited 205008: in one
    program slice the Baited stacks would decide who runs.
  - `deadmines-followups` (Deadmines Pilot gap 8; 30 s, two cycles by default, every damage step `weak=1`): each
    heroic 1.5 s follow-up after its 2 s first shape, timed from the first one's resolve. 0 s: a 3 yd dash line from
    the creature to a non-tank at least 8 yd away (4 yd excluded at its start), then two more 1.5 s lines, each from
    the last one's end to a non-tank the chain has not hit (SM4). 8 s: a 4 x 25 yd frontal line, then two at
    +-30 degrees (SM7, SM8). 14 s: donut 5 / 14 on the creature, then circle 6 on it (SM10, SM11). 20 s: circle 8 on
    the creature (the tank out too), then a frontal cone 90 / 10 yd (even cycles) or a 4 x 30 yd line through a
    non-tank's spot (odd cycles) (SN1, SN2, SN3). 25 s: circle 5 under every member, then circle 5 under every member
    where it stands as the first wave lands (RZ1, RZ10). Tags: first shapes 1, 4, 6, 8, 11; follow-ups 2 and 3 (second
    and third dash), 5 (aftershocks), 7 (Haft Slam), 9 (cone), 10 (line), 12 (Lookout).
  - `sfk-hunt` (Shadowfang Keep build §3.9: Odo's Hunt OD1, the bell OD3 and its landing OD4, the Echo of Odo BS5;
    30 s, two cycles by default): 0 s, a hunt on a random non-tank, 3 yd catch circle, 2 s howl, then 10 s at 80% run
    speed (5.6 yd/s), passes every 200 ms; 0.5 s, a bell, 1-body Soak 2 yd on one of three points 15 yd apart around
    the creature (not the hunted, `role=nontank`), resolving at 5.5 s: rung, the hunt ends and a 6 yd landing follows
    2 s later (`weak=1`); 15 s, the heroic hunt at 95% (6.65 yd/s) with no bell. `pct=35` (Maul, the landing) in the
    percent mode. Tags: Zone/1 catch at 80%, MissedSoak/2 bell not rung, Zone/3 others in the landing, Zone/4 the
    ringer in it, Zone/5 catch at 95%. The sweep runs `sfk-hunt:4` (120 s, a program runs once per sim iteration).
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

## Training dummy (300402, M4)

The Mechanics Training Dummy (`npc_boss_mechanics_dummy`, `src/MechanicDummyAI.h/.cpp`) runs a drill program on
itself as the boss stand-in, so players (no GM needed) and their bots can practice. Spawn it with `.npc add 300402`.

- **Engage**: hit it. It begins its own encounter, scoped to the player who engaged it (a pet's owner; a GM's hit
  picks a fighting non-GM player), and runs the program for that player's group (raid) on a loop. It never dies,
  moves or attacks: hits leave 1 health, a **40% floor** is put back every second (above every execute range), it
  turns in place to face the top of its threat list (the tank of the frontal exemption and `pick=nontank`). Its level
  follows the scope player (percent damage reads it).
- **Scope**: only the scope player's group and the players fighting it are hit (`Registry::IsInTestScope`), never a
  bystander. Every second the scope set becomes the scope player plus the players in combat with it (within 60 yd):
  a player who walks off drops out. When the scope player's group has left the fight while others still fight, the
  program restarts for one of them (and the last group leaves the scope).
- **Gossip** (talk to it): the program (every drill then barrage and minefield, one drill, or barrage only), the
  damage (none: failures still count; 1 flat per hit, the default; or **percent**: each mechanic's own `pct=` of
  reference health, 20% for most, as a dungeon boss would hit), "whisper me the failure report" (the ledger by player
  and tag, or the last fight's once it ended), and "stop". A choice made while it is engaged restarts the program at
  once (the ledger is kept); otherwise the program starts on the next engage. **While a program runs, only the scope
  player's group (raid) or a GM may change or stop it**; anyone may start one on an idle dummy and read the report.
- **`.mech dummy <program|stop|report> [dmg=<n>|dmg=pct]`** (GM, the dummy selected): the same, from the command line.
- **Programs** (`Drill::ParseProgram`): `all` (the default: `smite,cookie,fenrus,soaks,ras,alexei,barrage:30,
  minefield:30`), `none` (the encounter and scope only), or comma-separated entries `<name>[:<n>]`: a drill name (n =
  cycles, default the drill's), `lanes` (n = cycles, default 3), `barrage`, `minefield` or `uminefield` (n = seconds,
  default 30). Entries run one after another, 4 s apart; barrage and minefields use the `.mech barrage` /
  `.mech minefield` defaults around the scope player.
- **A failed entry** (a step that no longer parses, for example a spell row missing after a deploy, or nobody alive to
  land on) is skipped; it is logged at `LOG_ERROR`, whispered to the scope player, and recorded on the encounter (the
  sim reads it with `Sim::TakeProgramErrors`).
- **What ends the encounter**: an evade (every player beyond 60 yd or out of combat, checked every second; or 2
  minutes with no valid player in combat with it, e.g. only pets), the gossip stop or `.mech dummy stop`, death (only
  by force), despawn, and `.mech clear all` (the dummy then stays quiet, in combat with no program, until the next
  engage or gossip choice). `.mech clear` leaves it alone. The end drops the pending steps, mechanics, markers,
  ledger, and the Weakened and Marked (drill) stacks it gave.

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
| trigger / action | `boss mechanic face` -> `boss mechanic face` (turn to the look-away facing) | 94 |
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
  the bot's own spot, offered only targets at least 1 yd closer to that end (so a narrow gap is tried at the reduced
  margin instead of settling for a spot that makes no progress); a clean target -> Escape there, then straight back to
  Watch, where the stock motion resumes (and is re-routed again if it still crosses). No clean way around, or the
  spline's end is itself unsafe -> Hold where it stands (until the hazard ends, at most `HoldCapMs`).
- **Escape**: walking the solver's path. Within 0.6 yd at a spot that is safe (or once our spline has ended), or the
  move we sent ended short at a spot that is safe anyway -> Hold. (An exit lies 0.5 yd beyond the margin, so a bot
  stopped 0.6 yd short would still stand in it.) Path or target turned unsafe -> re-solve; a plan from the
  reduced-margin pass is re-checked at the reduced margins. A clear Fallback (its path spends no time inside an exact
  hitbox while hot) never passed the margins, so it walks on while the rest of its way stays clear and its end stays
  outside every exact hitbox; a hot one, or one that breaks a hard region, is solved again (`Solver::FallbackHolds`, M4:
  re-solving a clear Fallback on every view rebuild turned bots round in a crowd and cost hits). No progress (0.5 yd)
  for 500 ms since the escape began or the bot last moved (re-sends do not restart this clock; refused sends count as no
  progress) -> re-solve; the same target again -> released. Rooted, stunned or polymorphed with no progress ->
  released until it can move. One escape past 2 x its planned travel + 1.5 s, or an unbroken run of escapes past 10 s
  -> released.
- **Released** (Watch for 1.5 s, or until a root ends): the stock AI has the bot back. Nothing is locked, no cast is
  broken and an unsafe spot is not solved, so a stuck or rooted bot heals, casts and follows; then it tries again.
- **Hold**: at the safe spot until `holdUntil` (latest window end it answers, capped at `HoldCapMs`) -> Watch. Spot
  unsafe -> re-solve; out of the role band for `RoleSlackMs` -> Watch (stock movement takes it back, policed), never
  while a task holds it (below).
- **Freeze** (M3, HoldStill, decision §7.4): from the window start - `SafetyMarginMs` - `Freeze.SettleMs` to its end
  + `SafetyMarginMs` the bot does not move at all, not even for a new Zone: no solve, every movement action blocked
  (ours too), foreign motion stopped; casts (any length) and facing allowed. Before the window the solver only offers
  targets reached by its start. Released bots are never frozen. A HoldStill that goes before its window ends
  (`.mech clear`, the boss died, a map change) unfreezes the bot at once.

"Safe" means `Solver::TargetOk` from M3 on: hazards (as M2) plus the player kinds (regions to stand in, carried shapes
to keep allies out of, the freeze deadline, the parking rule). `leaveBy` stays hazard-based, plus the latest start to
reach an unmet region, so a heal in Hold finishes only when it ends in time.

### Tasks (M3)

The view gives each bot its tasks (`BotTask`: mechanic, kind, role, window). The brain:

- **Holds for its tasks**: a bot whose spot passes `TargetOk` holds it from registration to the task's window end +
  `SafetyMarginMs` for every role but Avoider (it only keeps out of other players' circles; Watch polices its motion)
  and Frozen (the Freeze mode): a Stack leader stays put while the others come, a claimer stays in its soak, a carrier
  keeps its clear spot, a tank stands still during a Frontal warning so the cone the others dodge does not move.
- **Refreshes allies before every solve** (`RefreshAllies`) and publishes the plan right after it
  (`PublishReservations`: the target, its look-away facing, slot and claim); drops every reservation when it has no
  task left, is released, dies, goes Idle or loses the strategy. A new `ReservationRevision` re-reads the player kinds
  (not the whole view) for every bot that is not Idle. A self-trigger aura (`SelfTrigger`) is polled every step; a
  stack change rebuilds the view and lets the next solve skip the rate limit.
- **Tracks** a hard region on a moving anchor (a soak on a walking player): within `TrackSlackYd` of its inner edge it
  re-solves with `Input::roleSolve` (the spot competes; the region score pulls it back in), at most once per
  `MovingAnchorRecheckMs`.
- **Look-away** (decision §7.6): a carried cone or line with `FollowFacing` gets the free facing nearest the bot's
  victim (`Plan::facing`, or the free arc at its spot). Over each of its windows (+- `SafetyMarginMs`) the face action
  holds that facing and the lock and waker stop whatever would turn the bot (see Lock); a new facing is republished
  when it moved more than 5 degrees. The others pose that cone at the published facing +- `LookAway.MarginDeg` (not
  `FacingSlackDeg`: the carrier holds the facing itself), and the carrier keeps them a hair over that margin out, so
  an ally the carrier cleared never sees itself inside.
- **Tank facing** (decision §7.5, `TankFacing.Enable`): the exempt victim of a tank-exempt Frontal or Zone sourced on
  a creature remembers that source for the fight, with every distinct cone or line of it that faces the victim (facing
  offset within 45 degrees; a rear or side shape needs no facing and is never remembered, so the melee behind the boss
  never make the tank circle it). Between warnings, every 500 ms: when one of those shapes, posed at the source facing
  the tank (+- `FacingSlackDeg`, `SafetyMarginYd`), holds a group member's planned spot, the tank walks to the nearest
  ring point (every 15 degrees, radius within [both combat reaches + 1, melee range - 0.5]) where none does and whose
  spot and path are safe. During a warning it holds (above); a warning that starts while it walks stops the walk on
  the spot. While a source is remembered, the lock blocks stock `tank face`.
- **Tank parking** (`Park.Enable`): for a `Park` zone whose parked unit's victim is the bot, `Input::park` gets the
  unit's spot, its stop distance (combat reaches + 0.5) and chase trigger (its melee range) and `Park.DepthYd`; the
  solver moves the tank to where the chasing unit stops that deep inside the zone, and the tank holds there while the
  zone lives (`Park.HoldCapMs` per zone). Drag-out: the bot's victim stands in, or within 3 yd of, a lasting hazard
  (an interval hot 2 s more) the bot or another group member is eligible for (a `role=nontank` puddle the melee
  stand in) -> the same, with the stop that deep outside every such hazard.
- **Healer sight** (role band v2, soft, decision §7.11): healers in combat score Sight (the nearest group tank at
  `Healer.SightWeight`, other members at half, at most `Healer.MaxSightChecks` rays per solve) and Reach (members
  beyond `HealDistance - 2`). The rays go to the tank first, one per candidate, best score first, skipping candidates
  in the shadow of one already found blocked (within 2 yd of the ray's line beyond it), then to the other members of
  the candidates that see the tank; a candidate not checked counts as out of sight. Once a second, in Watch, a ray to
  the tank; out of sight -> a role solve, which moves the healer only when a better spot exists (else no new one for
  5 s).
- **Route search** (§3.8): when a follow motion crosses hazards and the one-hop re-route finds nothing (and was not
  merely deferred), the next brain step runs `Solver::Route` towards the motion's end (at most once per
  `Route.RetryMs` per bot, one unit of the per-map budget; that step does nothing else, route or not). A route
  becomes an Escape that walks its waypoints (the move action sends one leg at a time): the next leg starts within
  0.6 yd of the waypoint or once the leg's spline ended, and only when the straight line from where the bot stands to
  the next waypoint passes the path check at the route's margin (`RouteAdvanceOk`; else it walks on to the waypoint).
  It ends at `RouteHandoff`'s waypoint, where stock follow takes over. A leg that needed the reduced margin is
  re-checked at the reduced margins. No route: hold, as before.
- **Region route** (M4): a solve whose Fallback breaks a hard region (a Stack, Cluster, Bait range or Spot the bot
  cannot reach straight, because its way in crosses a Zone that is hot by then: the Alexei Assembly member behind the
  Out of bounds circle) asks for `Solver::RegionRoute` on the next brain step, under the route search's rules
  (`Route.Enable`, `Route.RetryMs`, one budget unit, nothing else that step), while the bot still walks or holds that
  Fallback. A found route is an Escape over its waypoints to a spot that passes the full check (the re-check holds its
  end to it, not only to the hazards) and goes onto the reservation board; it holds the region's window there. One
  goal per call (one Route search, so one budget unit costs at most one Route: two cost 1.5 ms in the worst host
  bench, over the 1,000 µs route budget); when that goal's route fails or arrives late, the next goal goes on the next
  brain step, up to two goals. The request stays pending while the Fallback still breaks its region and only the
  retry clock or the budget is missing, and a Fallback that breaks a hard region is never kept by the Escape
  re-check: it is solved again on every view change (STAGE-M4-R-REPORT m-4, m-10).

Role solves, region tracking, tank re-facing searches and routes count in the 8-per-map-update budget.

Solves are rate-limited by `MinSolveIntervalMs`, except the first after a registry change, and limited to 8 per map
update across all bots of a map (the rest solve on the next update). Our move clears the pending wait of the last move
(ours or a foreign `MOVEMENT_FORCED` hold) so the escape is never refused; a move to the same target is re-sent at
most every 250 ms (after a refused send, a partial path or a spline that ended short).

### Solver

Candidates (the bot's spot, exits, rings, the role ring, lane-frame points) are kept when their straight-line arrival
is safe at the full margin, then validated in score order, in passes that each spend at most `MaxPathChecks` new
navmesh path calls (paths found earlier are reused):

1. a clean path at the full margin (`SafetyMarginYd`) -> Move;
2. a clean path at the reduced margin (each hazard's anchor drift + 0.25 yd; `SafetyMarginMs` stays): the bot slips
   through a gap narrower than two full margins, between two mines for instance. The target itself is still safe at
   the full margin, so the bot never holds inside a margin -> Move (counted as "reduced margin");
3. only when an armed or arming mine allows grace: a grace crossing at the full margin -> GraceMove;
4. Fallback: the candidate with the fewest hazards hot from its arrival, then the least time inside the exact hitboxes
   (no margin) on its straight line, then the nearest; candidates known to have no path are skipped and the first
   walkable one down that ranking is taken. No path to it -> NoPath, straight to the nearest exit. Only the top nine
   of the ranking are ever read (eight probes, then one with a known path), so a straight-line walk stops once it
   cannot enter them any more (M4: the same answer, a third less time on a lane-set Fallback).

Path walks skip a sample's test against a hazard that is not hot at that sample's time (outside its window, widened
as the windows are; never a projectile, whose window can run past its end in the margin band, nor a hazard the walk
started in): the same answer as testing it, about 40% less time when a lane set's 40 yd lines reach every sample of a
leg but are hot for half a second of it (M4). A circle region the bot is outside adds twelve entry points on its edge,
15 to 120 degrees round from the nearest one either way (M4): when another player's landing circle blocks the
straight way in, the nearest clear way in lies round the edge, between the edge and what the bot can reach in time,
which the rings and the half-radius points miss (the Fenrus healer that stood still and took the leap).

Within a pass, candidates whose straight line passes the path check are tried first; the others (their straight line
crosses a hazard while it is hot, the common case under a barrage) only when none of the first validated. Before a
candidate costs a path call, a navmesh probe (one Detour `findNearestPoly` within 1 yd around it and 5 yd up and down,
with the bot's polygon filter) drops it when no walkable polygon lies within 1 yd (inside a wall or a pillar), or
snaps it onto the navmesh and checks it again where it landed (up to 64 probes per solve). The path call accepts
`PATHFIND_NORMAL` and `PATHFIND_INCOMPLETE` paths that end within 3 yd of the asked point; the solver validates and
scores the real end, never the asked point.

### Route search

`Solver::Route` (`MechanicRoute.h`, pure) finds a way around hazards towards a goal (the end of a follow motion) when
the greedy one-hop re-route makes no progress, e.g. behind a U-shaped wall of overlapping mines:

- **Grid:** the box around the bot and the goal padded by `Route.PadYd`, cells of `Route.CellYd`, grown so the grid
  stays within `Route.MaxCells`. A cell is blocked when its center lies within a hazard's reduced margin (drift +
  0.25 yd) plus half a cell diagonal at the time the bot would get there (a zone over by then does not block), for the
  hazards hot before the bot could walk twice the way, or the way plus twice `Route.PadYd`. Each hazard pose tests only
  the cells of its own outline, row by row (a circle, and half-planes for a line or a cone of at most 180 degrees), with
  the exact distance. Regions and carried shapes do not count. The bot's and the goal's cells are never blocked, and
  a blocked cell within two cells of the bot is freed when the bot can walk straight to it (the grid's half diagonal
  boxed in a bot standing within a cell of a margin, M4).
- **Grown box (M4):** no route in the box although the search (or the sealed-off goal's cells) reached its edge,
  while hazards cross that edge: the box grows once round the obstacle they form (the hazards whose outline, bounds
  plus margin, crosses the edge, and every hazard within 3 yd of one of them), at most `RouteParams::growYd` (32 yd)
  per side, and the search runs again. A bot deep in a U of mines has its way out behind it, out of the mouth and
  round an arm, which the bot-goal box does not hold (the M3 playtest's bot left behind). A true enclosure never
  reaches the edge and never grows.
- **Search:** a goal walled in by lasting hazards is found by a small flood fill first (no route, no A*); else
  8-connected A*, no corner cutting, octile heuristic, no route longer than the box's perimeter, at most
  `Route.MaxExpansions` expansions. Non-finite input or a goal over 500 yd away: no route.
- **Legs:** the grid path is string-pulled into at most `Route.MaxLegs` straight legs (at the full margin when the
  route fits, else at the reduced one), each walked on the navmesh path and checked like a solve's path (full margin,
  else reduced; never grace). A leg the navmesh rejects blocks the cells around it and the search runs once more.
  Navmesh path calls are capped by `MaxPathChecks`.
- **Result:** `Move` with `path`, `waypoints` (the legs' ends) and `arriveAt`, or `Safe` (no route: hold).
  `RouteHandoff` gives the first waypoint from which the straight line to the goal is clean at the full margin.
- **Cost (host, tests/route_test, medians):** 4,096 cells, 30 mines, 6 legs: about 0.4 ms; the same with a rejected
  leg, a second search and all 8 path calls: about 0.75 ms; bot enclosed: about 0.06 ms; goal enclosed: about
  0.02 ms; 30 lines (30 x 3 yd): about 0.27 ms; 30 cones (60 degrees, 25 yd): about 0.47 ms; 24 projectile lines:
  about 0.56 ms. M4: bot enclosed by a ring the box cuts (grown once, two searches): about 0.14 ms; a bot deep in the
  layout=u minefield (grown box): about 0.15 ms, with a rejected leg and the retry (three searches): about 0.21 ms.
  `Route.MaxCells` and `Route.MaxExpansions` are capped at 16,384 (a larger box costs more).

### Player kinds (view)

`BuildInput` turns every kind into solver input for one bot; `RefreshAllies` rebuilds that part right before every
solve, and `PublishReservations` puts the bot's target on the registry's reservation board right after it, so bots
solving one after another on the map thread never pick colliding spots (decision §7.1: no messaging, no leader).

- **Planned spot** of another player: its live reservation on that mechanic; else, for a bot, its plan target
  (Escape, Hold, Freeze; through `BotViewHooks::plannedSpot`); else a bot's current spot + 0.5 yd (+ its speed × 250 ms
  while moving); a human's current spot + `HumanSlackYd`.
- **Judge window:** `[resolveAt, resolveAt + PostResolveHoldMs]` (a pending Detonate pulls it to now); lasting
  Stack/Soak `[resolveAt, until]`; HoldStill `[resolveAt, expireAt]`. Every eligibility test is
  `Registry::EligiblePlayers` (the registry's own candidates), never a playerbots role.

| Kind | The bot is | Solver input |
| --- | --- | --- |
| Spread, RunOut (group) | a carrier | its circle carried (no other eligible player inside); slot ring by guid rank |
| | eligible | each other live carrier's circle as a hazard at its reservation, or a human's spot<sup>2</sup> |
| RunOut `From(Owner)` | a carrier | a circle hazard on the owner (drift as M2) |
| Stack | anchored on the bot | Leader: nothing to reach (the brain holds) |
| | eligible | a region: the shape at the anchor's planned spot (a bot's plan), or live + a drift pose |
| Stack `Cluster(n)` | the leader (sticky claim, else the medoid) | Leader: hold |
| | eligible | a region of r/2 around the leader's spot (everyone within r of everyone) |
| Soak | a claimer (`Assign::SoakClaimers`) | a region on the shape over its window |
| | eligible, no claim (or count 0) | the shape as a hazard over its window (decision §7.3) |
| Bait | the picker (sticky claim, else `BaitPick`) | hazard on the owner, `max(othersRange, humans) + 2`<sup>1</sup> |
| | eligible, not the picker | a region of `othersRange` around the owner; the picker's landing<sup>1</sup> |
| HoldStill | listed (targets, else eligible) | `Freeze{resolveAt - SafetyMarginMs, expireAt}` |
| Spot | listed | a region of r at its own spot |
| Zone | anchored on the bot | carried shape (OnBot); a turning cone or line picks the facing nearest its victim |
| | aimed at the bot (`to`) | carried shape (AimedAtBot, the registered length) |
| | on another bot with a plan | one more pose at that bot's planned spot (a synthetic copy) |
| Frontal/Zone, tank-exempt | the exempt victim | not a hazard; a Tank task (tank facing) |
| Zone with `Park` | the parked unit's attacker | a Parker task (tank parking) |

<sup>1</sup> The landing circle (`Radius`) around the other players' planned spots (picker), or around the picker's
reservation or a human picker's spot (others)<sup>2</sup>; none when the radius is 0 (the hit lands on the pick only).
The picker also gets `FarFrom` the owner.

<sup>2</sup> M4 (`ViewRules::PlaceOtherCircle`): a group bot's circle goes into the others' view only once it planned
for the mechanic (its reservation on it: every solve with a task publishes one, and a claim role holding its spot
does). Until then it is about to move for it itself, and its own side (the carried circle, the picker's hazards on
the others' planned spots) keeps everyone out. Placing it at its current spot made the others yield first, and the
carrier then saw them clear and stayed: a Ras runner in the middle of the stack pushed the stack out of its region.

A region's margin is `SafetyMarginYd`, at most half its inner radius; a moving anchor's drift (as poses, capped the
same way) shrinks it further. Reservations: `until` = the task's window end, `claim` for claimers, pickers and leaders;
a bot no longer tasked on a mechanic is unreserved. With `Debug = 1` the self-check also compares the solver's
containment of Stack, Soak and Spot regions and of unplanned carrier circles with `Registry::ContainsPoint`.

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
its brain for `FastTickMs` (bots without a real master otherwise tick every 0.5-3 s). The woken bot thinks some time
after the wake, so the fast-tick rule counts that lead in (WP-Q2): on a 100 ms update (the sim, a loaded map) it used
to wake one update late, a brain step every 200 ms. The lead is measured (WP-F2): the smallest delay seen from a wake
to the brain step after it, never more than the full-update gap. In the sim that is the step; live a bot also updates
on the partial updates, one world tick after the wake, and the full-update gap alone woke it up to 1.7x too often on a
loaded realm. An engaged bot so thinks every `FastTickMs` at the most, up to one full-update gap later. A bot
preparing a cast does not run its engine (`PlayerbotAI::UpdateAI` yields only in `SPELL_STATE_PREPARING`; a channel is
`SPELL_STATE_CASTING` and the brain keeps running through it), so the waker refreshes its `leaveBy` and interrupts a
cast or channel that would end after `leaveBy - SafetyMarginMs`, unless the bot is released. After a break the lock
holds back cast-time spells and item uses for 500 ms, so the cast is not restarted and broken again.

M3: a frozen bot is never broken for a hazard (it cannot leave). Under look-away, a cast with
`SPELL_FACING_FLAG_INFRONT` on a target outside the chosen arc is broken when a look-away window opens before it ends:
`PlayerbotAI::UpdateAI` turns a casting bot to its target every update, before any engine runs, so nothing else can
stop that turn.

`leaveBy` already holds `SafetyMarginMs` once (the window is widened by it); the cast rule subtracts it a second time
on purpose, for the AI tick between the cast's end and the escape's first step. Casts thus end 300 ms (default)
earlier than the bare margin would allow.

### Lock

- **Idle, Watch, released**: nothing is blocked, except: Watch with an unsafe spot whose solve is deferred locks as
  Escape, and for 500 ms after a cast break cast-time spells and item uses are blocked in every mode. M4 WP-Q2, Idle
  and Watch (not released): a gap closer whose landing fails the bot's hazards over the horizon is refused, since a
  leap or a charge cannot be stopped once cast (a Heroic Leap at the pull into the boss's warned front cone). Probed
  where it really stops (WP-F2, `MechanicLanding.h`): a charge (Charge, Intercept and the other
  `CastReachTargetSpellAction`s, a CHARGE effect, Blade Rush, Intervene) at the target's reach + 1 yd on the bot's
  side; a jump (a JUMP effect, Heroic Leap, Grappling Hook) on its center; Shadowstep and Ambush behind it by its
  facing; Death from Above and Killing Spree at both the center and the reach. A cast at a position is not judged
  (its point is the action's own). The spell-id part runs only for a bot in Watch or in combat.
- **The melee chase** (`reach melee`, WP-F2): refused only while no point of the melee ring round its target (16
  points at the bot's melee band, its own side first) is safe over the horizon, or while Watch found no way round the
  hazard the last chase ended in (a blocked band, below). A refused melee bot gets a role solve toward the band every
  0.5 s (2 s after one that found nothing closer), and its time out of reach goes to `msChaseRefused`. After
  `ChaseReleaseMs` of refusals out of reach (a tank half of it) the chase valve lets it walk in for as long, so it
  walks into melee and fights there; only while some ring point stays clear for that time + 2 s, so the valve never
  walks the melee into a zone about to resolve (alexei's Out of bounds at its Hold still). The walk-in (M5) is not a
  release: the bot stays in Watch with its horizon cut to that window, so a hazard registered meanwhile (a barrage
  donut on it, a mine on its chase line) is dodged at once, and the lasting hazard behind the refusal is seen again in
  time to leave. A melee bot out of melee range for `ChaseReleaseMs` while some ring point is clear and the chase is
  not refused is stranded (its chase line runs into a lasting hazard on its side): `msChaseStranded` counts it, and
  it routes round the hazard to the first ring point safe over the horizon (the route search; `strandRoutes`).
  Facing-only actions (`set facing`) are refused while the brain walks the bot (Escape). Tanks follow the same rules:
  they are exempt from a Frontal only while they are its source's current victim (not at the pull, nor after an aggro
  loss), and never from a Zone.
- **Watch** also stops a stock motion whose end fails TargetOk from its arrival, not only one whose path does (the
  chase back into alexei's Out of bounds). A melee bot or tank beyond its band re-routes toward the band; every other
  bot (a ranged bot too close to its target, a healer walking to its patient) toward the motion's end, which then
  need not be safe (WP-F2: the band re-route pulled a Hunter's flee back toward the boss, and a healer toward the
  tank). No re-route found: the band is blocked until the hazard ends (`HoldCapMs` when endless): the hold keeps the
  spot past `RoleSlackMs` and Watch holds again without a new solve.
- **Escape**: blocked: every movement action but ours, movement spells, any spell with a cast time or a channel.
  Allowed: instants (heals too), auto-attack and the stock target picks, `set facing`, ours.
- **Hold**: blocked: every movement action but ours, movement spells, a cast or channel that would end after
  `leaveBy - SafetyMarginMs`. Allowed: casts that end in time, instants, auto-attack, `set facing`, ours.
- **Freeze**: blocked: every movement action (ours too), movement spells, items whose spell moves the caster.
  Allowed: every cast, auto-attack, facing.
- **Any mode, tank facing** (a remembered Frontal source): stock `tank face` (`TankFaceAction`) is blocked.
- **Any mode, look-away window open**: blocked: `set facing`, `CastFacingUnitAction`, a cast with
  `SPELL_FACING_FLAG_INFRONT` or on a hostile target outside `CAST_ANGLE_IN_FRONT` of the chosen facing (rework
  actions such as `DkFacingCastAction` turn the bot themselves), attack-only actions on a target outside it. Heals on
  an ally behind the bot stay allowed.

Facing audit (M3): every playerbots and mod-dungeon-clear path that turns the bot without moving it, and what the
look-away lock does with it, is listed in the M3 WP-C report. In short: generic turners (`set facing`,
`CastFacingUnitAction`, facing casts, `AttackAction`) are blocked as above; the mid-cast re-facing is broken by the
waker; movers that turn (`TankFaceAction`, `set behind`, encounter positioning) are blocked by Hold; chat, quest,
trade, gossip and RPG turns are not combat actions and are left alone.

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
| `MaxCandidates` / `MaxPathChecks` | 400 / 8 | per solve / path calls per pass (up to 3 passes, +1 fallback) |
| `GridStepYd` | 2.0 | lane-frame grid |
| `MinSolveIntervalMs` | 150 | per bot, except the first solve after a registry change |
| `MovingAnchorRecheckMs` | 250 | re-view cadence while a hazard is unit-anchored |
| `RoleSlackMs` | 1000 | out of the role band this long while holding -> release the hold |
| `ChaseReleaseMs` | 3000 | chase valve: refused this long, a melee bot walks in as long (a tank: half); strand wait |
| `RangedDistanceYd` | 22 | ranged and healer band [8, this + 6] (melee: melee range - 0.5) |
| `FacingSlackDeg` | 15 | turning cones and lines are checked at +-this |
| `Debug` | 0 | one log line per solve with why it failed, the view self-check (`module.bossmechanics.bots`) |
| `Fallibility.Enable` | 0 | D1 human-like mistakes, deterministic under `Fallibility.Seed` (0 = random per load) |
| `HumanSlackYd` | 1.0 | uncertainty added to a human's current spot (M3) |
| `RegionComfortWeight` | 1.5 | score per yard short of 1 yd inside a get-in region |
| `SpacingWeight` | 1.5 | ranged and healers: score per yard closer than 6 yd to another member (0 = off; M5) |
| `TrackSlackYd` | 0.75 | re-solve a region on a moving unit this close to its inner edge |
| `LookAway.MarginDeg` | 10 | extra angle kept between a carried cone or line and an ally |
| `TankFacing.Enable` | 1 | the tank keeps a Frontal's source faced away from the group |
| `Park.Enable` / `Park.DepthYd` / `Park.HoldCapMs` | 1 / 1.0 / 8000 | tank parking and drag-out; stop depth; hold cap |
| `Freeze.SettleMs` | 250 | stop moving this long before a HoldStill window |
| `Healer.SightWeight` / `Healer.MaxSightChecks` | 10.0 / 8 | per member out of sight (tank full, others half) / rays |
| `Route.Enable` / `Route.RetryMs` | 1 / 1000 | route search around hazard walls; at most once per this per bot |
| `Route.PadYd` / `Route.CellYd` | 8 / 1.0 | search box padding / smallest grid cell |
| `Route.MaxCells` / `Route.MaxExpansions` / `Route.MaxLegs` | 4096 / 4096 / 8 | route search caps |

Fallibility keys (all off unless `Enable`): `ReactionMs` 350 + `JitterMs` 0-250 per bot before a mechanic is seen,
`LateChancePct` 5 for `LateExtraMs` 700 more, `WrongSideChancePct` 3 to ignore it until `RealiseMs` 700 before its
first window, `ScaleNormal` / `ScaleHeroic` 1.0. The out-of-combat role band is the follow distance + 2.

Rolls are keyed on (seed, bot guid, registry mechanic id). Mechanic ids keep counting per map, so the same seed
replays the same perception for the same mechanic id, not for a second run of `.mech barrage seed=1` on the same map
(its mechanics get new ids): two runs draw similar, not identical, mistakes.

### `.mech bots [all]`

Per bot in the GM's group (`all`: every bot on the map): the strategies installed, mode, target and distance, time
left before it must leave, its ledger failures, and its counters (solves by outcome, with the moves found only at the
reduced margin, path checks, late arrivals and how many of them followed a Fallback, stuck re-solves, released
escapes, motion stops and how many of them were re-routed, lock blocks by moves and casts, cast breaks, wakes, solve
time last / max / mean in microseconds). M3 adds a line with the solves made as a carrier, a soak claimer and the
Bait picker, role solves, plans that knowingly break a player kind for a Zone ("violated"), freezes, facings applied,
facing blocks and breaks, tank re-facings (and those a warning stopped), parks, routes found and failed with their
time, and sight rays; and a line with the bot's tasks, its live reservations, its look-away facing and its remembered
Frontal sources. The solve time includes the allies' refresh right before each solve.

M4 adds three lines (the same numbers reach the sim through `Sim::ReadBotStats`):

- `m4`: seconds in each mode (idle, watch, escape, hold, freeze, released; a released bot counts as released only),
  the lock leaks (moves, casts and turns the lock should have stopped, seen from the outcome, with the spell id x
  count of each leaked cast or movement spell; rules in `src/MechanicLeaks.h`), Fallback outcomes split by exposure
  (clear: the path spends no time inside an exact hitbox while it is hot; hot: it does), the Escape re-checks of a
  clear Fallback that walked on (kept), and route loops (a route search for about the same goal within 5 s of a
  route's handoff to stock follow);
- `cost`: navmesh path calls with their mean, p95 and max microseconds, probes and sight rays with their max, the
  lane-frame candidates and the path calls spent on them, path calls per mechanic (max and mean, plus those of plans
  that answer no mechanic);
- `cost us`: solve and route p50 / p95 / max, the bot's brain step mean and max, and the brain time of every bot on
  the map per map update (mean and max). p50 and p95 are log2 bucket bounds (`src/MechanicCost.h`).

A fourth M4 line, `m4 lock`, counts the actions run by name the lock's gate refused, the chat `do` orders it let
through, casts judged with no spell id (only casts that could run: spells the bot never learned reach the lock with id
0 and are not counted), `pull` requests refused while the bot's own encounter is live (in an instance any encounter
on the map; in the open world one whose scope holds the bot's group), pulls ended (a pull pending or running when the
brain left Idle: playerbots' pull would zero every other action for up to 15 s, so the brain runs its `pull end`),
and forced leak passes. A fifth, `m4 routes and bands`, counts the region routes (and those found) and the out-of-band
time (ms; and the part of it inside a task window). A sixth, `m4 landings` (WP-Q2), counts the gap closers and chases
refused for their landing (per candidate judged), the Watch motions stopped for their end, and (WP-F2) the time out
of reach with the chase refused (`msChaseRefused`; M5: a hold on a blocked band too), the chase valve's walk-ins, the
recovery solves and (M5) the stranded time and its strand routes.
`BossMechanics.Bots.ForceLeaks = N` (debug only, never on a live realm) proves the leak detectors fire: the lock lets
the blocks they can see through (a foreign mover, a cast-time spell or item in Escape or Hold, a look-away turn) until
each detector has counted N since the counters' last reset, and every leak is logged as `lock leak in ...`. Passes
are counted per candidate judged, so a kind with many passes and no leak over a whole run points at a blind detector.

With `Debug = 1`, each solve logs the pass that found the plan and why the rest failed: candidates kept, probes (off
the navmesh, snapped), path calls without a path, ends unsafe at arrival, paths crossing a hazard that holds the bot
or another one, passes cut short by `MaxPathChecks`; and for the navmesh calls the accepted ones (INCOMPLETE, end
moved) and the rejected ones by path type flag (NOPATH, NOT_USING_PATH, SHORT, SHORTCUT, FARFROMPOLY_START, other,
empty) and by end distance (more than 3 yd off). `.mech bots` then adds the same counts summed per bot ("debug solve",
"debug nav"). Nothing of this is counted or formatted with `Debug = 0`.

A late arrival is an escape whose bot still stood inside a hazard's exact hitbox (no margin) once one of that hazard's
windows there had started, counted once per escape. Reaching the target after the window opened is not late: the
target often lies well past the edge. Most late arrivals are Fallback escapes, late by construction (nothing on time
existed); a late clean Move points at reaction time (the view seen late, a solve deferred by the per-map budget, a
stuck re-solve).

### Limits

- No navmesh on a map: every path check fails, so the solver falls back to `Fallback` / `NoPath` (logged once per bot
  and mechanic). The probe cannot tell walls apart there and lets every candidate through to the path call.
- A reduced-margin path passes 0.25 yd from a hazard's edge (plus the anchor's drift). Grace crossings are still
  measured at the full margin, so a gap the reduced pass rejects (narrower than about 0.5 yd) stays closed: a bot
  boxed in by overlapping mines holds, or crosses under grace when the mines allow it.
- Fallibility hides a player kind as a whole (the bot does not notice the Spread or the Soak); tank and parking tasks
  are never hidden.
- Actions run through `PlayerbotAI::DoSpecificAction` (chat `do`, `move to loot` from item use, pull pre-actions) skip
  every multiplier, so the lock cannot stop them; engine reactions (knockback, teleport acks) are not actions.
- The lock knows a cast by `CastSpellAction` (and items by `UseItemAction`). A plain `Action` that calls `CastSpell`
  itself can still start a cast-time spell in Hold; the waker breaks it, at most once per 500 ms window (M4 class
  audit).
- Re-routing around a hazard is greedy, one hop at a time towards the end of the stock motion; when that finds
  nothing, the route search (M3) looks for a way around, including one that first leads away from the master (a
  U-shaped wall of mines). Its grid ignores walls until a leg fails on the navmesh (one retry), so a maze-like room
  may still hold a bot; a true enclosure holds it until the hazard ends or `HoldCapMs`, then it tries again.
- Tank facing and parking predict where a chasing unit stops (combat reaches + 0.5); the exact stop on a shortened
  path is not verified, `Park.DepthYd` absorbs it. A boss that turns on its own during a warning (not toward its
  victim) breaks the cone prediction (boss rule, STAGE-M3-HANDOFF §3.12).
- Look-away cannot stop a turn by a plain `Action` the audit did not list, or by `DoSpecificAction`; the face action
  turns the bot back on its next tick.
- Bots never steer their pets; Spread, RunOut and Bait no longer hit pets (decision §7.8), Zones still do. The lock
  does not hold pets either: a hunter's pet may teleport into a telegraph on Kill Command (34026) or charge or leap
  into one on Command I / II (201800 / 201801, its family's commanded spell). Mechanics target players only, so a pet
  in a hazard costs no failure (STAGE-M4-L-REPORT §8).
- Plain `Action`s that cast are not judged as casts (the lock sees `CastSpellAction`s): Paladin seals (20164, 20165,
  20166, 20375, 21084, 31801), auras and the aura swap (465, 7294, 19746, 19876, 32223); Shaman elemental summons
  (2062, 2894, 202272, 202273), imbues (8017, 8024, 8033, 8232, 51730) and attunements (202207-202210); Rogue poisons
  (203701-203706). Every one of them is instant today; a future cast-time one shows as `leakCasts` with its spell id.
- A bot released after a stuck escape stands where it is for up to 1.5 s, inside the hazard if that is where it got
  stuck (no navmesh way out); it heals and casts meanwhile.
- The barrage donut tests "stay in the hole" only while `SafetyMarginYd` < 1 (`MechanicBarrage.h`).

### Credits

The context registration and the strategy gate follow mod-dungeon-clear (Jared Wright, AGPL-3.0-or-later),
`DungeonClearModule` and `DcStrategyGate`; the solver's rule to validate path legs, not corners, and to validate a
target against the same band the trigger fires on is from its `DcHazard` (the route search validates its legs by the
same rule).

## Config

`conf/mod_boss_mechanics.conf.dist`, read at startup and on `.reload config`:

| Key | Default | What |
| --- | --- | --- |
| `BossMechanics.Enable` | 1 | 0 refuses every registration |
| `BossMechanics.Debug` | 0 | logs every resolve, failure and the map tick diff |
| `BossMechanics.ReferenceHealth.Rows` | `20:1300:1290 60:3670:3710 60h:4120:4110` | keyed rows (E3 measurement) |
| `BossMechanics.ReferenceHealth.MapLevels` | `36:20 36h:60` | `mapId[h]:level` content level pins, per difficulty |
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
| 205008 | Baited (drill): harmless dummy debuff, 10 stacks, 2 min; the drills' bait rotation aura (`rider=`, `aura=`) |
| 205009 | Marked (drill): harmless dummy debuff, 10 stacks, 2 min; the drills' selector (`marker` steps) |
| 205010-205099 | spare (the module's block; `rider=` takes 205008-205099) |
| 300400 | Mechanic Marker (`npc_boss_mechanic_marker`) |
| 300401 | Mechanic Test Owner (`npc_boss_mechanic_marker`) |
| 300402 | Mechanics Training Dummy (`npc_boss_mechanics_dummy`, M4) |
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
