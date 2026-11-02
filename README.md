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
| `src/MechanicProviders.h/.cpp` | Reference health, content key and role providers (stubs) |
| `src/EngineBridge.h` | The seam between the registry and the engine glue |
| `src/MechanicTelegraph.h/.cpp` | Marker creatures: outline + fill layers, spawn, size, fill timer, follow, despawn |
| `src/MechanicResolve.cpp` | `spell_boss_mechanics_resolve` and the `EngineBridge` implementation (resolve cast) |
| `src/MechanicWeakened.h/.cpp` | Weakened stacks and threat compensator, `spell_boss_mechanics_weakened` |
| `src/MechanicMarkerAI.h/.cpp` | `npc_boss_mechanic_marker`: passive marker / test owner AI |
| `src/MechanicHooks.cpp` | Config load, map tick, encounter end, Weakened on map leave |
| `src/MechanicCommand.cpp` | `.mech` |
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
Mechanic::BeginEncounter(me, { Colour::Red, Colour::Blue, BOSS_RHAHKZOR });      // optional: palette, boss state
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
| `BeginEncounter(owner, opt)` / `EndEncounter(owner)` | Palette and boss state id; implicit at the first registration |
| `Zone(owner, Shapes::Circle/Donut/Cone/Line/LineFacing(...), warn)` | Any shape on a point or unit anchor |
| `Circle`, `Donut`, `Cone(owner, source, angle, len, warn, exempt)`, `Line` | Zone shortcuts |
| `Mine(owner, pos, r, arm, grace)` | Ring while arming, disc when armed; a player inside for `grace` detonates it |
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
`SelfTrigger(spell, stacks)`, `From(MeasuredFrom)`, `TankExempt()`, `Colour(c)`, `Tag(n)`, `NoTelegraph()`,
`OnHit(fn)`, `OnResolve(fn)`.

Damage is a percent of the reference health (`ReferenceHealthProvider`; M1 ships the config stub), one flat number per
target set by the resolve spell, so boss buffs and the creature damage rate never change it
(`SPELL_ATTR3_IGNORE_CASTER_MODIFIERS`). Physical mechanics are reduced by armor; no crit, miss, reflect or partial
resist. A hit fires the victim's "when hit" procs (`SPELL_ATTR3_NOT_A_PROC` lets the triggered cast proc: Reckoning,
Blood Craze, Vengeance-style talents), like a boss ability would.

### Kinds

| Kind | Server resolve (M1) | Marker | Failure |
| --- | --- | --- | --- |
| Zone | resolve spell on everyone eligible inside | the shape's pieces; a mine: ring, then disc | Hit |
| Frontal | as Zone, the owner's victim is skipped | cone | Hit |
| Spread | a circle on each carrier hits every other player inside | ring on each carrier | Hit |
| RunOut | as Spread on one target; `From(Owner)`: hit within r of the owner | ring on the target (*) | Hit |
| Stack | none (split damage via `onResolve`) | disc on the anchor | MissedStack |
| Soak | none; `onResolve` gets the count | disc on the anchor | MissedSoak (group) |
| Bait | hit on the farthest eligible player | none | MissedBait |
| HoldStill | none | none | Moved (> 0.3 yd) |
| Spot | none | disc on each spot | OffSpot |

(*) `From(Owner)`: the ring of r sits on the owner (the hitbox) and a small ring
(`Rules::kRunOutTargetMarkerRadiusYd`, 1.5 yd, same colour) follows the target, so the target knows it must run.

Every unit hit posts `Hit`. Pets take damage but never reach the ledger or Weakened. Only alive player-controlled units
can be hit (GMs never). `ResolveResult.hits` lists the units the filter kept, so an immune target (Divine Shield,
Ice Block) is listed although it took nothing. Get-out kinds draw in the encounter's get-out colour (red by
default), get-in kinds in its get-in colour (blue by default); cones, donuts and lines exist in red and yellow only.

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
passive, never evading, faction/level/phase copied from the owner, gravity off, `Telegraph.Lift` above the anchor.
Display and size come from the ladder pick (scale = radius / length / width; the marker shapes are 1 yd at scale 1)
and are set **before** the creature enters the map: `SummonMarker` builds the TempSummon itself instead of calling
`Map::SummonCreature`, whose `AddToMap` sends the create packet with the template display at scale 1. The client
blends every scale change in, so a marker resized after the create visibly grew from 1 yd (the first build's bug).

Every telegraph draws two layers:

| Layer | Look | Size |
| --- | --- | --- |
| Outline | rim (0.9) + faint interior (0.2) | the full hitbox from the first frame; never resized by the timer |
| Fill | rim (0.9) + solid interior (0.35) | the timer: grows into the outline over the warning, full at the resolve |

The fill is stepped every `Telegraph.FillStepMs` (150 ms) from the registration to the resolve, computed
`Telegraph.FillLeadMs` (150 ms) ahead so the client's scale blend lands it on the outline at the resolve, not after;
it is never bigger than the outline. Per shape:

- Circle, Stack/Soak/Spot discs, Spread/RunOut rings: a disc grows from the centre.
- Cone: grows from the apex (uniform scale keeps the angle).
- Donut: a rim-only timer ring (90668+c) shrinks from the outer rim to the drawn hole, its band always on the danger
  side; once live (a persistent donut), the filled donut.
- Line: grows from the source end (after `ExcludeSourceEnd`) at the outline's width: ladder display swaps at
  scale = width, so the length never blends; a square of its length while shorter than the width; a multi-piece
  line fills piece by piece.
- Travelling projectile lines: no fill (the projectile is the timer).
- Mines: the fill grows while the mine arms and is full while it is armed (the old ring-then-disc, now with a timer).
- Persistent zones: full fill from the resolve to the end (no regrowth per tick).
- RunOut `From(Owner)`: the owner's ring gets the fill; the small pointer rings on the targets do not.

`Telegraph.Fill = 0` drops the timer: the outline is drawn alone until the danger is live (an armed mine, a persistent
zone after its resolve), which still shows the full fill.

Cost: two creatures per drawn piece (outline + fill), so a circle, cone, donut or one-piece line is 2 markers, a
Spread on 5 players 10, an n-piece line 2n; projectile lines and the RunOut pointer rings stay single.
`MaxActivePerMap` (256) caps mechanics, not markers: 256 one-piece mechanics are about 512 marker creatures. Fill
resizes send one value update per fill marker per step (~7 per second), like a moving creature.

- Cone angles round up to the ladder (30, 45, 60, 90, 120, 150, 180, 270); a cone wider than 270 degrees draws a full
  disc of its length (the hitbox stays the cone; a WARN names it).
- Donut ratios round **down** to the ladder (0.25, 0.33, 0.40, 0.50, 0.60, 0.67, 0.75): the drawn safe hole is never
  bigger than the real one (`Rules::kDonutRatioRoundUp = false`, harness ruling 2026-11-02). A ratio below 0.25 draws
  a full disc (no hole shown, a WARN names it) rather than a hole bigger than the real one.
- Lines use aspect steps 2^(i/8), 1 to 8, never shorter than the hitbox and at most 9% longer
  (`Rules::kLineNeverShorter`); above 8 several abutting pieces. Known limit: a line shorter than its width
  (aspect < 1) draws the 1:1 piece, so it can be more than 9% too long (accepted for M1).
- Both layers use the same rounding, so the full fill is exactly the outline.
- The textures keep a transparent margin (1/16 of the side) at every mip level, so no edge texel is stretched along
  the quad edge (the first build's "lines at the cardinals").
- The resolve caster is the mechanic's first outline marker (the carrier's ring for Spread/RunOut, the owner's ring
  for RunOut `From(Owner)`); a mechanic with no marker (`NoTelegraph`, Bait) casts from a hidden marker summoned for
  that cast only.

### Follow modes (`BossMechanics.Telegraph.FollowMode`)

Markers (both layers) on a unit anchor, on a `Line(from, to)` with a unit end, and on Spread/RunOut carriers are moved
every `FollowIntervalMs` when the target moved more than 0.25 yd or turned more than 2 degrees. The hitbox always reads
the live anchor; the marker trails by up to one interval.

| Mode | How | Notes |
| --- | --- | --- |
| 0 (default) | one flying spline per interval, move + face together, speed = distance / interval | turns cones |
| 1 | `MotionMaster::MoveFollow(unit, 0, 0)` once | circles/rings on a unit only; cones and lines still use mode 0 |
| 2 | `NearTeleportTo` every interval | jumpy, always correct |

`ShrinkProjectileLines = 1` shortens a single-pass projectile line's marker behind the projectile's tail (cosmetic;
pieces are re-picked and respawned when their count changes).

## `.mech` (GM, in game)

```
.mech test <shape> <size> <ms> [key=value ...]
.mech clear [all] | .mech list | .mech ledger [name] | .mech weakened <0-7> | .mech palette <red|yellow> <blue|purple>
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
| mine | radius | `grace=` (500), `cap=` (0), `life=` (0 = until triggered or cleared); arms after `<ms>` |
| lanes | lane width | `count=` (3), `gap=` (6), `axis=x/y`, `frame=` (40x40), `safe=1`, `cross=1`, `burn=` (6000) |
| spread | radius | `count=` (0 = everyone) |
| runout | radius | target = selected unit; `from=owner` measures from the test owner |
| stack | radius | anchored on the selected unit (else the point) |
| soak | radius | `need=` (2) |

`safe=1` fires every lane but one; `cross=1` adds lanes on the other axis and a burning cell at every crossing (for
`burn=` ms, ticking every `tick=`, `pct=` per tick). The frame is centred on the anchor point, X along the GM's facing.

Common keys: `dmg=` flat (0 = telegraph only), `pct=` (20, percent of reference health), `tankpct=`,
`school=physical|holy|fire|nature|frost|shadow|arcane` (physical), `weak=0/1` (0), `once=1`, `persist=<ms>`,
`tick=<ms>` (1000), `anchor=point|me|target` (point: the selected unit's position, else the GM's; facing = the
GM's), `colour=red|yellow|blue|purple`, `repeat=` (1, max 50), `every=` (1000 ms; repeats are registered from the test
owner's scheduler), `scatter=` (0 yd: random offset per repeat, point anchors), `tele=0` (no marker). Damage keys do
not apply to stack and soak, nor do `weak=`/`once=` (Weakened lands only from resolve hits). Keys that do not fit
the shape are listed back as ignored.

- `.mech list`: id, kind, shape, colour, tag, time to resolve and to expiry, marker count.
- `.mech ledger [name]`: per encounter, per-player failure counts by tag; with a name, also that player's last ten
  events.
- `.mech weakened n`: sets n stacks on the selected player (yourself when none), for the threat check.
- `.mech palette out in`: the test encounter's get-out and get-in colours for new test mechanics.
- `.mech clear`: ends the `.mech test` encounters only (markers, mechanics, ledger, the Weakened they stacked), drops
  combat with the test owner and despawns it. A real boss fight on the map keeps its mechanics.
- `.mech clear all`: ends **every** encounter on the map, real bosses included (the boss's next registration then
  runs as a bare implicit encounter: default palette, no boss state binding, no failure listener).
- A mine without `life=` never expires, so it keeps the test owner from its idle despawn: `.mech clear` it.

## Config

`conf/mod_boss_mechanics.conf.dist`, read at startup and on `.reload config`:

| Key | Default | What |
| --- | --- | --- |
| `BossMechanics.Enable` | 1 | 0 refuses every registration |
| `BossMechanics.Debug` | 0 | logs every resolve, failure and the map tick diff |
| `BossMechanics.ReferenceHealth.Rows` | `20:1000:1500 60:5000:8000` | stub `level:nonTank:tank` rows |
| `BossMechanics.Telegraph.FollowMode` | 0 | 0 spline step, 1 MoveFollow, 2 teleport |
| `BossMechanics.Telegraph.FollowIntervalMs` | 200 | follow interval (min 50) |
| `BossMechanics.Telegraph.Lift` | 0.1 | marker height above the anchor (yd) |
| `BossMechanics.Telegraph.ShrinkProjectileLines` | 0 | shorten projectile markers behind the projectile |
| `BossMechanics.Telegraph.Fill` | 1 | 0: no fill timer (the fill still shows an armed mine / a live zone) |
| `BossMechanics.Telegraph.FillStepMs` | 150 | fill resize interval (min 50) |
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
| 90500-90669 | Marker displays, fill: disc 90500+c, donut 90508+2j+c, cone 90522+2j+c, line 90538+2i+c |
| | outline: ring 90504+c, line 90588+2i+c, cone 90638+2j+c, donut 90654+2j+c; timer ring 90668+c |
| 90670-90699 | spare (90690-90695: the asset script's opt-in diagnostic displays) |

The spell and creature rows come from dbc-tools (`apps/dbc-tools/source/npcs/boss_mechanics.py`), the displays and
models from `apps/dbc-tools/patch_bossmechanics_markers.py`. A dungeon that wants its own combat-log name declares a
copy of the resolve spell bound to `spell_boss_mechanics_resolve` and passes it with `.Spell(id)`.

## Known limits (accepted for M1)

- Open world: player-picking kinds iterate the map's player list on each evaluation and keep those within
  `OpenWorld.CandidateRadius` of the owner. Correct, but on a continent with thousands of bots a long-lived mine still
  walks the whole list every map update (a grid search around the anchor would fix it if open-world bosses ever use
  mines).
- Short wide lines draw more than 9% long (above).
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
| S3 | Marker on the floor: no z-fighting, not hidden by bumps | raise `Telegraph.Lift`; else flag 0x8 |
| S4 | `circle 2`, `circle 10`, `circle 40` match 2/10/40 yd (`.gps` at the edge) | fix the model radius |
| S8 | The outline appears at full size at once; the fill grows and meets it as the hit lands | `Telegraph.FillLeadMs` |
| S5 | `cone 20 3000` points where the GM faces | flip the texture orientation |
| S6 | No blob shadow, nameplate, name or selection circle on markers | CreatureModelData flags; unit flags |
| S7 | `circle 5 8000 anchor=target` on a moving bot is smooth; a cone turns with it | `FollowMode` 1, then 2 |
