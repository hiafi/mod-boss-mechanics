/*
 * mod-boss-mechanics: pure timelines and geometry of `.mech drill` and the M3 `.mech test` landings
 * (STAGE-M3-HANDOFF §3.11).
 *
 * No AzerothCore includes: the command (MechanicCommand.cpp) runs each drill step through the `.mech test` parser,
 * and tests/drill_test.cpp compiles this header with the host compiler to desk-check every drill against the boss doc
 * rows it reproduces (shapes, timings, the separations the pairs need) and the landing geometry below.
 *
 * A drill is a list of steps per cycle. A step is the argument line of one `.mech test` (shape, size, ms, keys), run
 * at `atMs` after the cycle start from the test owner's scheduler (or, with `after=`, after the previous step: see
 * StepOffsets; `cycle=even|odd|<k>/<m>` skips it in the other cycles), or the drill-only pseudo shape `marker <stacks>`
 * (sets Marked (drill) 205009 on group members, see Step; drill setup never hands out Weakened). The command appends
 * `dmg=<n>` to every damage step (DamageStep). Positions are yards in map coordinates; angles in radians; "ahead" is
 * along the GM's facing, "left" 90 degrees counterclockwise from it.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DRILL_H
#define MOD_BOSS_MECHANICS_MECHANIC_DRILL_H

#include "MechanicGeometry.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace BossMechanics::Drill
{
    // ---- landing geometry ------------------------------------------------------------------------------------------

    // The mean of the points (x, y, z); the origin when empty. `at=group` lands on it.
    inline Geo::Vec3 Centroid(std::vector<Geo::Vec3> const& points)
    {
        Geo::Vec3 c{ 0.f, 0.f, 0.f };
        if (points.empty())
            return c;
        for (Geo::Vec3 const& p : points)
        {
            c.x += p.x;
            c.y += p.y;
            c.z += p.z;
        }
        float const n = static_cast<float>(points.size());
        return { c.x / n, c.y / n, c.z / n };
    }

    // `shift=<ahead>:<left>`: moves `p` by `ahead` yd along `facing` and `left` yd to its left.
    inline Geo::Vec3 Shifted(Geo::Vec3 const& p, float facing, float ahead, float left)
    {
        float const c = std::cos(facing);
        float const s = std::sin(facing);
        return { p.x + ahead * c - left * s, p.y + ahead * s + left * c, p.z };
    }

    // `spot ... ring=`: n spots on a circle of `ring` yd around `center`, the first at bearing `rotation`, then
    // counterclockwise at equal steps (members in guid order take them in turn).
    inline std::vector<Geo::Vec3> RingSpots(Geo::Vec3 const& center, float ring, std::uint32_t n, float rotation)
    {
        std::vector<Geo::Vec3> out;
        out.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i)
        {
            float const a = rotation + Geo::kTwoPi * static_cast<float>(i) / static_cast<float>(n);
            out.push_back({ center.x + ring * std::cos(a), center.y + ring * std::sin(a), center.z });
        }
        return out;
    }

    // The distance between neighboring spots of RingSpots (the chord of one step).
    inline float RingSpacing(float ring, std::uint32_t n)
    {
        return n < 2 ? 0.f : 2.f * ring * std::sin(Geo::kPi / static_cast<float>(n));
    }

    // `shape=line` on stack and soak: the source end of a lane of `length` centered on `center` along `facing`
    // (a line's anchor is its source end; the lane runs from there along the facing).
    inline Geo::Vec3 LaneSource(Geo::Vec3 const& center, float facing, float length)
    {
        return Shifted(center, facing, -length / 2.f, 0.f);
    }

    // ---- drill steps -----------------------------------------------------------------------------------------------

    // One step: a `.mech test` line, or `marker <stacks> [pick=nontank]` (Marked (drill) 205009, the drills' stand-in
    // for an eligibility or order aura; stacks > 0: one random group member below that many stacks, never the selected
    // creature's victim with pick=nontank, gets exactly that many; 0: Marked off every group member). Weakened is for
    // failures only: no step sets or clears it (orchestrator ruling on STAGE-M4-R-REPORT M-2).
    struct Step
    {
        std::uint32_t atMs = 0;
        char const* line = "";
    };

    struct Def
    {
        char const* name = "";
        char const* summary = "";       // one line for the command's reply
        std::uint32_t cycleMs = 0;
        bool ownerAtCreature = false;   // the test owner (the Bait's owner) stands at the selected creature (moved
                                        // there at the start and before each step)
        std::vector<Step> steps;        // in start order (StepOffsets: atMs, or after the previous step with after=)
        std::uint32_t defaultCycles = 3;    // `cycles=` when not given
    };

    // Shapes that carry damage keys (dmg=, weak=); stack, soak, holdstill and spot never deal damage (a bell soak's
    // landing does: DamageStep), nor the drill-only steps marker, walk, leash, chase, holdfacing, blink and cast, nor
    // `crawl` (a soak on a moving unit).
    inline bool DamageShape(std::string_view shape)
    {
        return shape != "stack" && shape != "soak" && shape != "holdstill" && shape != "spot" && shape != "marker"
            && shape != "walk" && shape != "leash" && shape != "chase" && shape != "holdfacing" && shape != "crawl"
            && shape != "blink" && shape != "cast";
    }

    // Alexei's slots: one rule (or heroic pair) per slot. Singles warn 3 s (normal), pairs 4 s (heroic); the window is
    // 6 s; at-close rules resolve at warning + window.
    constexpr std::uint32_t kAlexeiSlotMs = 12000;

    // Deadmines Pilot gap 8 (deadmines-pilot.HANDOFF §3.9): the ledger tags of `deadmines-followups`. First shapes:
    // 1 (dash), 4 (Shockwave), 6 (donut), 8 (Saw Spin), 11 (Cave In). Follow-ups (FollowupTag): 2 and 3 (second and
    // third dash), 5 (both aftershocks), 7 (Haft Slam), 9 (Splinter Spray cone, even cycles), 10 (Runaway line, odd
    // cycles), 12 (Lookout wave).
    constexpr std::uint32_t kFollowupTags[] = { 2, 3, 5, 7, 9, 10, 12 };

    inline bool FollowupTag(std::uint32_t tag)
    {
        for (std::uint32_t t : kFollowupTags)
            if (t == tag)
                return true;
        return false;
    }

    // ---- the chase (`hunt`, `soak bell=`; Shadowfang Keep build §3.9, drill `sfk-hunt`) ----------------------------
    // `hunt <r> <ms> speed=<rate> persist=<ms> [tick=] [pick=]`: a chaser (a Mechanic Marker) starts at the selected
    // unit and fixates one member; a circle of r on the chaser, eligible = that member only, stands through the `ms`
    // warning (the howl), then the chaser walks at the member for `persist` ms at `rate` x player run speed,
    // re-aimed at every pass (`tick`). The first pass that holds the member is a catch: one hit, and the hunt ends.
    // `soak <r> <ms> bell=<n> [apart=15] [land=6] [jump=2000]`: an alarm bell, the soak on one of n fixed points
    // `apart` yd from each other around the selected unit; the live hunt's member is not eligible. Filled at its
    // resolve, it ends the hunt and the chaser bounds to the bell, landing `jump` ms later with a circle of `land`
    // there: tag + 1 for everyone but the ringers, tag + 2 for the ringers (BellLandingTag, BellRingerTag).

    constexpr float kPlayerRunYdS = 7.f;        // a player's base run speed (baseMoveSpeed[MOVE_RUN])
    constexpr float kHuntRateNormal = 0.8f;     // Odo's Hunt, normal: speed_run 0.8 (SK 840-842, HANDOFF §8 #11)
    constexpr float kHuntRateHeroic = 0.95f;    // Relentless Hunt, heroic
    constexpr float kMaxHuntRate = 2.f;         // `speed=` above this is refused
    constexpr std::uint32_t kMinBells = 2;
    constexpr std::uint32_t kMaxBells = 6;

    // Ledger tags of `sfk-hunt` (sweep keys `<Kind>/<tag>`): Zone/1 a catch at 80%, MissedSoak/2 a bell nobody rang,
    // Zone/3 someone other than the ringer in the bell landing, Zone/4 the ringer in it, Zone/5 a catch at 95%.
    constexpr std::uint32_t kHuntTagCatchNormal = 1;
    constexpr std::uint32_t kHuntTagBell = 2;
    constexpr std::uint32_t kHuntTagLanding = 3;
    constexpr std::uint32_t kHuntTagRinger = 4;
    constexpr std::uint32_t kHuntTagCatchHeroic = 5;

    constexpr std::uint32_t BellLandingTag(std::uint32_t bellTag)
    {
        return bellTag + 1;
    }

    constexpr std::uint32_t BellRingerTag(std::uint32_t bellTag)
    {
        return bellTag + 2;
    }

    // A pass of the catch circle is a catch only when it hit the hunted player (Stratholme SWEEP-2 D5: the hunted
    // player's pet on the chaser's start used to end the hunt at the end of the howl). The circle is players only
    // (Eligibility::playersOnly), so nobody else is hit; a hit on anyone else would still not be the catch.
    template <class Id>
    bool HuntCaught(std::vector<Id> const& hits, Id const& hunted)
    {
        return std::find(hits.begin(), hits.end(), hunted) != hits.end();
    }

    // The chaser's speed in yd/s at `rate` x player run speed.
    constexpr float HuntSpeedYdS(float rate)
    {
        return rate * kPlayerRunYdS;
    }

    // The ring radius that puts n points `apart` yd from their neighbors (0 below two points).
    inline float BellRing(float apart, std::uint32_t n)
    {
        return n < 2 ? 0.f : apart / (2.f * std::sin(Geo::kPi / static_cast<float>(n)));
    }

    // The n bell points around `center`, the first at bearing `facing`, neighbors `apart` yd apart.
    inline std::vector<Geo::Vec3> BellPoints(Geo::Vec3 const& center, float facing, std::uint32_t n, float apart)
    {
        return RingSpots(center, BellRing(apart, n), n, facing);
    }

    // One step of the chase as the runtime drives it (a straight walk at the target, re-aimed every step): the chaser
    // moves `speed` x `dtS` yd toward `target`, never past it.
    inline Geo::Vec3 ChaseStep(Geo::Vec3 const& chaser, Geo::Vec3 const& target, float speed, float dtS)
    {
        float const dx = target.x - chaser.x;
        float const dy = target.y - chaser.y;
        float const d = std::sqrt(dx * dx + dy * dy);
        float const step = speed * dtS;
        if (d <= step || d < 1e-4f)
            return { target.x, target.y, chaser.z };
        return { chaser.x + dx / d * step, chaser.y + dy / d * step, chaser.z };
    }

    // A desk model of one hunt (the host test reads the bar's premises with it): the chaser stands through `warnMs`,
    // then walks at `chaserYdS` for `chaseMs`, judged every `tickMs` from the end of the warning. The member starts
    // `startGap` yd away and, from `reactMs` after the hunt starts, walks straight away from the chaser at `fleeYdS`
    // (0: stands). The ms of the first pass with the member within `radius`, or nullopt (never caught).
    struct ChaseModel
    {
        float startGap = 6.f;
        float radius = 3.f;
        float chaserYdS = HuntSpeedYdS(kHuntRateNormal);
        float fleeYdS = kPlayerRunYdS;
        std::uint32_t reactMs = 0;
        std::uint32_t warnMs = 2000;
        std::uint32_t chaseMs = 10000;
        std::uint32_t tickMs = 200;
    };

    inline std::optional<std::uint32_t> CatchMs(ChaseModel const& m)
    {
        constexpr std::uint32_t kStepMs = 50;
        Geo::Vec3 chaser{ 0.f, 0.f, 0.f };
        Geo::Vec3 member{ m.startGap, 0.f, 0.f };
        std::uint32_t const end = m.warnMs + m.chaseMs;
        std::uint32_t nextPass = m.warnMs;
        for (std::uint32_t t = 0; t <= end; t += kStepMs)
        {
            if (t >= nextPass)
            {
                if (Geo::Dist2d(chaser.x, chaser.y, member.x, member.y) <= m.radius)
                    return t;
                nextPass += std::max<std::uint32_t>(m.tickMs, 1);
            }
            float const dt = static_cast<float>(kStepMs) / 1000.f;
            if (t >= m.reactMs && m.fleeYdS > 0.f)
            {
                // Straight away from the chaser.
                float const dx = member.x - chaser.x;
                float const dy = member.y - chaser.y;
                float const d = std::max(std::sqrt(dx * dx + dy * dy), 1e-4f);
                member.x += dx / d * m.fleeYdS * dt;
                member.y += dy / d * m.fleeYdS * dt;
            }
            if (t >= m.warnMs)
                chaser = ChaseStep(chaser, member, m.chaserYdS, dt);
        }
        return std::nullopt;
    }

    // ---- Scholomance shapes (Scholomance build §3.9, drill `scholomance-shapes`) -----------------------------------
    // The short warnings of the Scholomance build that need a drill before its heroic sign-off. Landing keys of
    // `.mech test` this adds (the runtime is MechanicCommand.cpp):
    //   away=<yd>      a point anchor moves exactly that far at a random bearing (MA6: a circle 2 yd off each player;
    //                  GD4: a portal on the room's edge);
    //   at=prev        the centre of the previous point-anchored circle of this command (or drill run); none yet: the
    //                  selected unit (PK2: the next specimen stands inside the last ring; with away=);
    //   at=predict     where a member (pick=) will be `lead=` ms later at its heading and speed, on it while it
    //                  stands (IB1: MoveHeading, PredictSpot);
    //   grid=<n> apart=<yd> decoys=<k>    (circle) n x n circles `apart` yd apart, centred on the anchor point and
    //                  squared to the facing, k of them Decoy() at random (JB1 heroic: GridPoints, PickDistinct);
    //   decoy=1        the shape is a Decoy() (Mechanic::Decoy; lanes safe=1: the free lane drawn as one).

    constexpr std::uint32_t kMaxGrid = 5;               // grid= at most 5 x 5
    constexpr std::uint32_t kDefaultPredictLeadMs = 1500;

    // A grid of circles of radius r leaves no gap between four neighbors when apart <= r x sqrt(2).
    inline bool GridCovers(float r, float apart)
    {
        return apart > 0.f && apart <= r * std::sqrt(2.f) + 1e-4f;
    }

    // n x n points `apart` yd apart centred on `center`, rows along `facing` (row-major from the back left).
    inline std::vector<Geo::Vec3> GridPoints(Geo::Vec3 const& center, float facing, std::uint32_t n, float apart)
    {
        std::vector<Geo::Vec3> out;
        out.reserve(static_cast<std::size_t>(n) * n);
        float const half = (static_cast<float>(n) - 1.f) / 2.f;
        for (std::uint32_t i = 0; i < n; ++i)
            for (std::uint32_t j = 0; j < n; ++j)
                out.push_back(Shifted(center, facing, (static_cast<float>(i) - half) * apart,
                    (half - static_cast<float>(j)) * apart));
        return out;
    }

    // k distinct indices below n, in pick order; `index(m)` returns a random index below m (k is capped at n).
    template <class IndexFn>
    std::vector<std::uint32_t> PickDistinct(std::uint32_t n, std::uint32_t k, IndexFn&& index)
    {
        std::vector<std::uint32_t> pool(n);
        for (std::uint32_t i = 0; i < n; ++i)
            pool[i] = i;
        std::vector<std::uint32_t> out;
        for (std::uint32_t taken = 0; taken < k && !pool.empty(); ++taken)
        {
            std::uint32_t const at = std::min<std::uint32_t>(index(static_cast<std::uint32_t>(pool.size())),
                static_cast<std::uint32_t>(pool.size()) - 1);
            out.push_back(pool[at]);
            pool.erase(pool.begin() + at);
        }
        return out;
    }

    // `away=`: `p` moved `yd` along `bearing`.
    inline Geo::Vec3 Away(Geo::Vec3 const& p, float bearing, float yd)
    {
        return Shifted(p, bearing, yd, 0.f);
    }

    // `at=predict`: the direction a mover goes from its movement keys (forward, back, strafe), relative to its
    // orientation; nullopt when it stands or the keys cancel out. A server-driven move (a bot's spline) sets forward
    // and faces along its path.
    inline std::optional<float> MoveHeading(float orientation, bool forward, bool back, bool left, bool right)
    {
        float const ahead = (forward ? 1.f : 0.f) - (back ? 1.f : 0.f);
        float const side = (left ? 1.f : 0.f) - (right ? 1.f : 0.f);
        if (ahead == 0.f && side == 0.f)
            return std::nullopt;
        return orientation + std::atan2(side, ahead);
    }

    // Where the mover stands `leadMs` from now at `speed` yd/s along `heading`; where it stands now without one.
    inline Geo::Vec3 PredictSpot(Geo::Vec3 const& at, std::optional<float> heading, float speed, std::uint32_t leadMs)
    {
        if (!heading || speed <= 0.f)
            return at;
        return Away(at, *heading, speed * static_cast<float>(leadMs) / 1000.f);
    }

    // Ledger tags of `scholomance-shapes` (sweep keys `Zone/<tag>`). The lead-ins (1, 6) warn 2 s like any first shape
    // and are reported; every other tag is a §3.9 short warning under the sweep's bar (ScholoBarTag).
    constexpr std::uint32_t kScholoTagFuse = 1;         // PK2: the first fuse ring, 2 s
    constexpr std::uint32_t kScholoTagChain = 2;        // PK2: chained rings, 1 s each
    constexpr std::uint32_t kScholoTagBarrage = 3;      // MA6: three offset waves, 1.5 s, 1 s apart
    constexpr std::uint32_t kScholoTagRead = 4;         // IB1: the first marker on the read player, 1.5 s
    constexpr std::uint32_t kScholoTagForesight = 5;    // IB1: the predicted markers, 1.5 s each
    constexpr std::uint32_t kScholoTagOssuary = 6;      // RG4: the first ring, 2 s
    constexpr std::uint32_t kScholoTagOut = 7;          // RG4: rings outward, 1.5 s apart
    constexpr std::uint32_t kScholoTagIn = 8;           // RG4 heroic: rings back inward, 1 s apart
    constexpr std::uint32_t kScholoTagSigil = 9;        // JB1 heroic: the real sigils and the room's edge, 6 s
    constexpr std::uint32_t kScholoTagPortal = 10;      // GD4: portal lanes from the edge through players, 2 s
    constexpr std::uint32_t kScholoBarTags[] = { 2, 3, 4, 5, 7, 8, 9, 10 };

    inline bool ScholoBarTag(std::uint32_t tag)
    {
        for (std::uint32_t t : kScholoBarTags)
            if (t == tag)
                return true;
        return false;
    }

    // JB1 heroic in the drill: a 3 x 3 grid of 8 yd sigils 11 yd apart (one a decoy) and the room's edge, a donut
    // from kSigilEdgeInner (inside the grid's outer rim, about 17.7 yd) to kSigilEdgeOuter (ratio 0.25, a ladder
    // step, so the drawn hole is the real one): the decoy's circle is the only safe ground within a 6 s run.
    constexpr float kSigilRadius = 8.f;
    constexpr float kSigilApart = 11.f;
    constexpr std::uint32_t kSigilGrid = 3;
    constexpr float kSigilEdgeInner = 17.f;
    constexpr float kSigilEdgeOuter = 68.f;

    // ---- Stratholme follow-ups (Stratholme build §3.9, drill `stratholme-followups`) -------------------------------
    // The chained follow-ups and the hidden lines of the Stratholme build that need a drill before their heroic
    // sign-off. `.mech test` keys this adds (the runtime is MechanicCommand.cpp):
    //   cone to=member|target|me       the cone turns to aim at that unit live from its anchor (BZ1: a cone from the
    //                  boss that tracks a moving player); no follow=1 with it;
    //   exempt=target  (cone, line, projectile with to=) the unit the shape aims at is not eligible: every other scope
    //                  member is (Eligibility carriers, AllBut; BZ1: the target leads the cone, nobody else may stand
    //                  in it).
    // ST's "hidden flag" (WY9) is the existing `tele=0` (NoTelegraph): the line resolves from a hidden marker and bots
    // read it like any other.

    // exempt=target: the members that stay eligible (every member but `exempt`, in order).
    template <class Id>
    std::vector<Id> AllBut(std::vector<Id> const& members, Id const& exempt)
    {
        std::vector<Id> out;
        out.reserve(members.size());
        for (Id const& m : members)
            if (!(m == exempt))
                out.push_back(m);
        return out;
    }

    // Ledger tags of `stratholme-followups` (sweep keys `Zone/<tag>`). The first shapes (1, 3, 5, 7) are reported;
    // the follow-ups (2, 4, 6, 8) and the hidden Barrage lines (9) are under the sweep's bar (StratBarTag).
    constexpr std::uint32_t kStratTagRank = 1;          // UF1: the first rank, a 30 x 10 lane on the frame's X axis
    constexpr std::uint32_t kStratTagCrossing = 2;      // UF1 heroic: the crossing rank, 1.5 s after the first passes
    constexpr std::uint32_t kStratTagSmash = 3;         // MZ7: circle 8 on the owner, tank included, 2 s
    constexpr std::uint32_t kStratTagAftershock = 4;    // MZ8 heroic: donut 8 / 14, 1.5 s after the smash
    constexpr std::uint32_t kStratTagWail = 5;          // BA4: circle 12 on the owner, 3 s
    constexpr std::uint32_t kStratTagEcho = 6;          // BA5 heroic: donut from 8 yd, 2 s after the wail
    constexpr std::uint32_t kStratTagSwarm = 7;         // BZ1: a 60 / 30 cone tracking a non-tank, 2.5 s
    constexpr std::uint32_t kStratTagSecondSwarm = 8;   // BZ1 heroic: a second cone at another non-tank, 1.5 s later
    constexpr std::uint32_t kStratTagBarrage = 9;       // WY9 heroic: five hidden half-hall lines, 1.5 s apart
    constexpr std::uint32_t kStratBarTags[] = { 2, 4, 6, 8, 9 };

    inline bool StratBarTag(std::uint32_t tag)
    {
        for (std::uint32_t t : kStratBarTags)
            if (t == tag)
                return true;
        return false;
    }

    // UF1: the rank and the crossing rank, each a 30 x 10 lane through the center of a fixed frame (frame=30x10 on X,
    // 10x30 on Y: one lane fills the frame's width, so it is centered, never placed at random).
    constexpr float kRankLength = 30.f;
    constexpr float kRankWidth = 10.f;
    // BA5: the echo is "beyond 8 yd from her". The drill draws it 8 to 32 yd (ratio 0.25, the ladder's smallest step,
    // so the drawn hole is the real one; the leash is 30 yd). 8 to 60 (ratio 0.13) draws a full disc, no hole shown.
    constexpr float kEchoInner = 8.f;
    constexpr float kEchoOuter = 32.f;
    // WY9: Willey's hall frame is 58 x 20 yd (NAVMESH-REPORT §5 (b)), halves 10 yd wide on either side of its axis.
    // The drill centers the hall on the owner: each half is a 58 x 10 line from 29 yd behind it, 5 yd off the axis.
    constexpr float kHallLength = 58.f;
    constexpr float kHalfWidth = 10.f;
    constexpr std::uint32_t kBarrageBlasts = 5;
    constexpr std::uint32_t kBarrageGapMs = 1500;
    constexpr std::uint32_t kBarrageCallMs = 3000;      // the called order lands 3 s before the first blast

    // ---- Stratholme drop spots (Stratholme build X2, drill `stratholme-drops`) --------------------------------------
    // The drop spot rule (MechanicDropSpot.h, MechanicRef::DropSpot): a shape a member carries lands where the member's
    // bot takes it. `.mech test` keys this adds (the runtime is MechanicCommand.cpp):
    // `drop=<edge,away,behind,prefer,avoid>` on a circle anchor=member or a line to=member, `room=<yd>` (a circle room
    // around the selected unit, default 30), `dwell=<ms>` (an enterable spot opens this long before the landing,
    // default 800); `prefer` makes every live persistent circle of the test owner an enterable preferred spot, `avoid`
    // an avoid point. Ledger tags: the old webs (1, reported: a carrier that steps in for the drop takes a tick), and
    // the carried shapes' hits on the others (2-5, under the bar).
    constexpr std::uint32_t kDropTagWeb = 1;            // NE3: two old webs, 5 yd, near the group
    constexpr std::uint32_t kDropTagGlob = 2;           // NE1: a 5 yd glob onto an old web, else the edge
    constexpr std::uint32_t kDropTagRot = 3;            // BR3: a 6 yd Death and Decay to the edge, away from the group
    constexpr std::uint32_t kDropTagLane = 4;           // HM3: a 6 yd lane from the creature to a member
    constexpr std::uint32_t kDropTagHolyFire = 5;       // GF1: a 4 yd fire behind the group (your facing = its travel)
    constexpr std::uint32_t kDropBarTags[] = { 2, 3, 4, 5 };

    inline bool DropBarTag(std::uint32_t tag)
    {
        for (std::uint32_t t : kDropBarTags)
            if (t == tag)
                return true;
        return false;
    }

    // ---- Stratholme jump (Stratholme build X3, drill `stratholme-jump`) ---------------------------------------------
    // The Jump kind (Mechanic::Jump, MechanicJump.h): `jump <r> <ms>` sweeps a circle of r at the end of the warning
    // and spares whoever is in the air; bots hop for it. Timmy's Jump Rope (TC8, TC9; ST "M4. Jump Rope"): three 2 s
    // casts back to back, the trip 25% plus Weakened; heroic Freeze Tag runs beside it (a 6 yd RunOut on a non-tank
    // that lands in the third skip's hop); and the heroic fallback Double Dutch (ST, Timmy's heroic): five skips of 2,
    // 2, 1.5, 1.5 and 1.5 s. Ledger tags (sweep keys `Jump/<tag>`, `RunOut/<tag>`): the bar is no Jump hit at all.
    constexpr std::uint32_t kJumpTagRope = 1;           // TC8: three 2 s skips back to back
    constexpr std::uint32_t kJumpTagTag = 2;            // TC2: Freeze Tag's RunOut during the rope
    constexpr std::uint32_t kJumpTagDutch = 3;          // Double Dutch: the two 2 s skips
    constexpr std::uint32_t kJumpTagDutchFast = 4;      // Double Dutch: the three 1.5 s skips
    constexpr float kJumpRopeRadius = 40.f;             // the whole square around the selected unit

    // ---- Blackrock Spire follow-ups (Blackrock Spire build §3.9, drill `blackrock-spire-followups`) ----------------
    // The follow-ups of the Blackrock Spire build that gate a heroic sign-off (wave 1: B1 Omokk, B7 Gizrul and
    // Halycon, B9's hard mode, B3 Voone). `.mech test` keys this adds (the runtime is MechanicCommand.cpp, Dice::ends):
    //   at=last:<n>       the far end of the fixed line registered n lines before the last one (`at=last` is
    //                     `at=last:0`); a shape placed after a later line still reads an earlier one (HG3's pounces
    //                     register with their own charge and read `at=last`);
    //   to=last[:<n>]     (line) aims a line from its source at that recorded end, frozen at registration like fix=1
    //                     (reach=1 ends it there). A line aimed this way records no end of its own (it is a recorded
    //                     point already), so the hauls of two chains read last:1 and last.
    // Every command (and drill run) keeps the ends of its last kMaxLineEnds fixed lines (LineEnds); an index past the
    // recorded ones lands where the selected unit stands (as `at=last` with no line yet).
    constexpr std::uint32_t kMaxLineEnds = 8;

    // The index of `at=` / `to=` "last" or "last:<n>" (n below kMaxLineEnds); nullopt for anything else.
    inline std::optional<std::uint32_t> LastIndex(std::string_view v)
    {
        if (v == "last")
            return 0;
        if (v.size() < 6 || v.substr(0, 5) != "last:")
            return std::nullopt;
        std::uint32_t n = 0;
        for (char const ch : v.substr(5))
        {
            if (ch < '0' || ch > '9')
                return std::nullopt;
            n = n * 10 + static_cast<std::uint32_t>(ch - '0');
            if (n >= kMaxLineEnds)
                return std::nullopt;
        }
        return n;
    }

    // The far ends of the last kMaxLineEnds fixed lines, oldest first (P: a position type).
    template <class P>
    class LineEnds
    {
    public:
        void Push(P const& end)
        {
            if (_ends.size() == kMaxLineEnds)
                _ends.erase(_ends.begin());
            _ends.push_back(end);
        }

        // The end `back` lines before the last one (0: the last); nullopt when fewer were recorded.
        std::optional<P> Back(std::uint32_t back) const
        {
            if (back >= _ends.size())
                return std::nullopt;
            return _ends[_ends.size() - 1 - back];
        }

        std::size_t Size() const { return _ends.size(); }

    private:
        std::vector<P> _ends;
    };

    // Ledger tags of `blackrock-spire-followups` (sweep keys `Zone/<tag>`, the Call's `Spot/13`). The first shapes (1,
    // 3, 5; wave 2: 8, 9, 11, 13) are reported; the follow-ups (2, 6, 7: 1.5 s; 4: the pounce, landing 1.5 s after its
    // charge; wave 2: 10 the later rings, 12 the second wall, 14 the challenger in the ring, 15-19 the blows) are under
    // the sweep's bar (BrsBarTag).
    constexpr std::uint32_t kBrsTagHaymaker = 1;        // OM2: a 5 x 30 line toward a non-tank's spot, 2.5 s
    constexpr std::uint32_t kBrsTagOneTwo = 2;          // OM2 heroic: the second line at that player's new spot, 1.5 s
    constexpr std::uint32_t kBrsTagCharge = 3;          // HG2 heroic: two 5 x 30 charges 1 s apart, 3 s
    constexpr std::uint32_t kBrsTagPounce = 4;          // HG3 heroic: circle 8 at each charge's end, with its line
    constexpr std::uint32_t kBrsTagChain = 5;           // WT11 / VO11: a 2 yd chain to each of two axes, 8 s
    constexpr std::uint32_t kBrsTagHaul = 6;            // WT11 hard mode: a 3 yd haul along each chain, 1.5 s taut
    constexpr std::uint32_t kBrsTagRecall = 7;          // VO12 heroic: War Cry's recall lanes, 4 yd, 1.5 s
    // Wave 2 (WP-G2; "Blackrock Spire follow-ups, wave 2" below)
    constexpr std::uint32_t kBrsTagNova = 8;            // EM2 heroic: the nova, 2.5 s
    constexpr std::uint32_t kBrsTagRingFirst = 9;       // EM3 heroic: ring 1, its slice shown 5 s ahead
    constexpr std::uint32_t kBrsTagRingNext = 10;       // EM3 heroic: rings 2-4, 1.5 s apart, turned 60 degrees
    constexpr std::uint32_t kBrsTagWall = 11;           // RG6: the wall, 3 s
    constexpr std::uint32_t kBrsTagWallSecond = 12;     // RG6 heroic: the second wall 3 s later, gap on the other side
    constexpr std::uint32_t kBrsTagCall = 13;           // DR18's Call stand-in: everyone to a spot 7 yd from him
    constexpr std::uint32_t kBrsTagRing = 14;           // DR17: the challenger in the lethal ring's band
    constexpr std::uint32_t kBrsTagSweep = 15;          // DR18's blows (1.5 s each, the challenger only)
    constexpr std::uint32_t kBrsTagCleave = 16;
    constexpr std::uint32_t kBrsTagFireBlast = 17;
    constexpr std::uint32_t kBrsTagRingOfFlame = 18;
    constexpr std::uint32_t kBrsTagThunderclap = 19;
    constexpr std::uint32_t kBrsBarTags[] = { 2, 4, 6, 7, 10, 12, 14, 15, 16, 17, 18, 19 };

    inline bool BrsBarTag(std::uint32_t tag)
    {
        for (std::uint32_t t : kBrsBarTags)
            if (t == tag)
                return true;
        return false;
    }

    constexpr std::uint32_t kBrsFollowupMs = 1500;      // every follow-up's warning (§3.9; §7 #26: 2 s on a fail)
    // HG3's pounce is drawn with its charge line (B7 builds SP's reading) and lands kBrsFollowupMs after the charge:
    // the charge's 3 s plus 1.5 s. Its bar still counts the pounce hits (B7's heroic gate).
    constexpr std::uint32_t kPounceWarnMs = 3000 + kBrsFollowupMs;
    constexpr float kHaymakerLength = 30.f;             // OM2 and HG2: 5 x 30 yd
    constexpr float kHaymakerWidth = 5.f;
    constexpr float kChargeMinYd = 15.f;                // HG2 "at the farthest non-tanks": min=, else the farthest
    constexpr std::uint32_t kChargeGapMs = 1000;        // HG2 heroic: the two charges 1 s apart
    constexpr float kPounceRadius = 8.f;                // HG3
    constexpr float kChainWidth = 2.f;                  // WT11 / VO11
    constexpr std::uint32_t kChainLifeMs = 8000;        // WT11: the haul's taut warning starts as the chain ends
    constexpr std::uint32_t kChainTickMs = 2000;        // a crossing hits once per chain per 2 s
    constexpr float kHaulWidth = 3.f;                   // WT11
    constexpr float kRecallWidth = 4.f;                 // VO7 (VO12 recalls with a 1.5 s warning)
    constexpr float kRecallExclude = 5.f;               // VO7 ExcludeSourceEnd(5): the lanes spare the boss's 5 yd

    // ---- Blackrock Spire follow-ups, wave 2 (Blackrock Spire build §3.9 rows 18-30, WP-G2) --------------------------
    // The Upper Spire rows of `blackrock-spire-followups`, each on its boss's numbers as built (B10, B12, B15). The
    // cycle is 40 s: wave 1's rows at 0-21.5 s every cycle, then one wave 2 row from 22 s, rotating with
    // `cycle=<k>/3` (cycles 0, 3: Emberseer; 1, 4: Rend; 2, 5: the Challenge), so the sweep's 120 s run (3 cycles)
    // runs each once. `.mech test` keys this adds (MechanicCommand.cpp):
    //   next=<deg>        (conefront) ConeFrontSpec::nextTurnDeg: the clockwise turn to the next ring's slice, the hint
    //                     the kit sets on every ring that has a next one (WP-SF: the bots' ring orbit reads it);
    //   edge=<yd>         (linefront) the gap's whole width this far inside the wall's ends (Front::PlaceGap edgeYd):
    //                     Rend's 68 yd wall keeps its gap in the 40 yd main strip with edge=15;
    //   inside=<yd>       Within(a circle of <yd> round the selected unit where it stands at registration): the bots'
    //                     hard bounds (B15's RingInside; Rend's room width);
    //   lethal=1          (circle, donut, cone, line) MechanicRef::Lethal(): bots never plan a way across (the ring);
    //   only=member       eligible to one player only, drawn with pick= (pick=same: the member of the last pick): the
    //                     challenger.
    // And the plan key `cycle=<k>/<m>` (CycleOf): the step runs in cycles k, k + m, k + 2m ... (even = 0/2, odd = 1/2).
    constexpr std::uint32_t kBrsWave2Ms = 22000;        // the wave 2 row's start in its cycle
    constexpr std::uint32_t kBrsWave2Rows = 3;          // cycle=<k>/3: rows 18, 24, 30
    // Row 18, Pyroguard Emberseer EM2 + EM3 heroic at 16 stacks (B10 EmberseerLogic.h through the kit's PlanRings,
    // with the rolling front rulings): ring 1 (45 degree slice, random) shown 5 s ahead, from the nova's edge; the nova
    // (7 + 0.3 x 16 = 11.8 yd) registered 2.5 s in with its 2.5 s warning, erupting as ring 1 launches; rings 2-4 each
    // shown as the one before launches (1.5 s), from the apex (ruling 2) pinned at the nova's centre (at=prev: the
    // kit fixes the apex for the pulse, and the parked tank must not drag it), turned 60 degrees clockwise, each
    // carrying the turn to the next (next=60; none on the last); 4 yd band, 8 yd/s, to 45 yd; the bot tank parks him
    // in each slice until the band passes nova + 12 yd. Every hit 30% x (1 + 4% x 16) = 49.2%, fire, Weakened.
    constexpr std::uint32_t kEmberStacks = 16;
    constexpr float kEmberNovaYd = 11.8f;               // 7 + 0.3 x 16
    constexpr std::uint32_t kEmberRingWarnMs = 5000;    // ring 1 (ruling 1)
    constexpr std::uint32_t kEmberNovaWarnMs = 2500;
    constexpr std::uint32_t kEmberRings = 4;            // heroic, 14+ stacks
    constexpr std::uint32_t kEmberSpacingMs = 1500;     // heroic
    constexpr float kEmberSliceDeg = 45.f;              // heroic
    constexpr float kEmberTurnDeg = 60.f;               // heroic
    constexpr float kEmberEndYd = 45.f;
    constexpr float kEmberSafeExtraYd = 12.f;
    constexpr float kEmberPct = 49.2f;
    // Row 24, Rend RG6 heroic (B12 RendGythLogic.h kSweep*): a 68 yd wall, 4 yd band, 8 yd/s over 86 yd, 3 s after
    // the call; one 8 yd gap in the 40 yd main strip (the kit's span 40 with edge 1 = edge 15 on the 68 yd wall); the
    // second wall 3 s later (both registered at the call), its gap on the other side within 20 yd of the first. 25%,
    // frost, Weakened (Frozen 2 s left out: no drill rider). The room's 40 yd width as inside=20.
    constexpr float kRendTravelYd = 86.f;
    constexpr float kRendWidthYd = 68.f;
    constexpr float kRendSpanYd = 40.f;
    constexpr float kRendGapYd = 8.f;
    constexpr float kRendEdgeYd = 15.f;                 // (68 - 40) / 2 + 1
    constexpr float kRendApartYd = 20.f;
    constexpr std::uint32_t kRendWarnMs = 3000;
    constexpr std::uint32_t kRendSpacingMs = 3000;
    constexpr float kRendRoomYd = 20.f;
    // Row 30, General Drakkisath DR17 + DR18 (B15 DrakkisathHardModeLogic.h): the Call (everyone to a 2 yd spot on a
    // 7 yd ring round him, 4 s: the stand-in for SP's 4 yd call circle 8 yd ahead, so the challenger stands within
    // the 9.5 yd that keeps the ring centred on him), then the Challenge on a non-tank: the lethal ring (12 yd with a 2
    // yd band: a donut 11-13, armed 1.5 s after the close, 10.5 s armed, eligible to the challenger only; its kill is
    // 50% here) and six blows 2 s apart, each telegraphed 1.5 s, eligible to the challenger only, inside=10
    // (RingInside: the band's inner edge less 1 yd). The order is fixed (B15 shuffles): Thunderclap, Ring of Flame,
    // Sweep, Fire Blast, Overhead Cleave, then Ring of Flame again (the sixth is one of the first four). It leaves out
    // B15's one step no bot can take: the Sweep right after the Thunderclap (the challenger 8.75-10 yd out, and the
    // only way out of a 10 yd half disc inside 10 yd bounds is behind him: brs_upper_followups_test, STAGE-G2-REPORT).
    constexpr std::uint32_t kCallMs = 4000;
    constexpr float kCallRingYd = 7.f;
    constexpr float kRingRadiusYd = 12.f;
    constexpr float kRingWidthYd = 2.f;
    constexpr std::uint32_t kRingArmMs = 1500;
    constexpr std::uint32_t kRingLifeMs = 10500;
    constexpr float kRingInsideYd = 10.f;               // 12 - 2 / 2 - 1
    constexpr std::uint32_t kBlows = 6;
    constexpr std::uint32_t kBlowEveryMs = 2000;
    constexpr std::uint32_t kBlowWarnMs = 1500;
    constexpr float kSweepDeg = 180.f;
    constexpr float kSweepYd = 10.f;
    constexpr float kCleaveDeg = 60.f;
    constexpr float kCleaveYd = 15.f;
    constexpr float kFireBlastYd = 4.f;
    constexpr float kFireBlastAroundYd = 5.f;
    constexpr float kRingOfFlameInner = 4.f;
    constexpr float kRingOfFlameOuter = 12.f;
    constexpr float kThunderclapYd = 8.f;

    // ---- Rolling fronts (Blackrock Spire build WP-RF, drill `rolling-fronts`) ---------------------------------------
    // The two rolling front kinds (MechanicFront.h) on the shapes and numbers of their first users, heroic numbers
    // (pct= in the percent mode), round the selected creature (the boss; park=1 has the bot tank lead it into each
    // ring's slice, Mechanic::FrontSafe). `.mech test` keys (MechanicCommand.cpp):
    //   conefront <end> <ms> slice=<deg> [bearing=<deg>|turn=<deg>] start=<yd> band=4 speed=8 [park=1 until=<yd>]
    //       a rolling cone front from the anchor point (at=prev: the last point circle's centre, the nova), its band
    //       from start= out to <end>; the slice random, at bearing= from the GM's facing, or turned turn= degrees
    //       clockwise from the previous conefront of the run (EM3's next ring);
    //   linefront <travel> <ms> width=<yd> gap=<yd> [side=left|right|other|any] [apart=<yd>] band=4 speed=8 facing=0
    //       a wall across the anchor point (the middle of the swept area) along the GM's facing turned by facing=,
    //       with one gap; side=other puts it across the centreline from the previous linefront's, apart= at most that
    //       far from it (RG6 heroic).
    // The Emberseer pulses follow the rolling front rulings (HANDOFF §10, as the kit's PlanRings and G2's row 18):
    // ring 1 (random slice, from the nova's edge) shown 5 s ahead; the nova registered 2.5 s in with its 2.5 s warning,
    // erupting as ring 1 launches; each later ring shown as the one before launches, from the apex (start=0) at the
    // nova's centre (at=prev), and every ring that has a next one carries its turn (next=). 0 s, normal at 7 stacks:
    // the nova (7 + 0.3 x 7 = 9.1 yd) and two rings 2 s apart (60 degree slices, the second turned 45). 12 s, heroic
    // at 14 stacks: the nova (11.2 yd) and four rings 1.5 s apart (45 degree slices turned 60). 27.5 s, Rend RG6: a
    // 30 yd wide wall over 50 yd with an 8 yd gap, then (heroic) a second 3 s later, its gap on the other side and
    // within 20 yd of the first (ruling 3; README "Rolling fronts"). Ledger tags (sweep keys `Zone/<tag>`): 1 and 4
    // the novas, 2 and 5 the first rings, 3 and 6 the later rings (the follow-ups under the bar, FrontBarTag), 7 the
    // first wall, 8 the second.
    constexpr std::uint32_t kFrontTagNova = 1;
    constexpr std::uint32_t kFrontTagRing = 2;
    constexpr std::uint32_t kFrontTagRingNext = 3;
    constexpr std::uint32_t kFrontTagNovaHeroic = 4;
    constexpr std::uint32_t kFrontTagRingHeroic = 5;
    constexpr std::uint32_t kFrontTagRingNextHeroic = 6;
    constexpr std::uint32_t kFrontTagWall = 7;
    constexpr std::uint32_t kFrontTagWallSecond = 8;
    constexpr std::uint32_t kFrontBarTags[] = { 3, 6, 8 };

    inline bool FrontBarTag(std::uint32_t tag)
    {
        for (std::uint32_t t : kFrontBarTags)
            if (t == tag)
                return true;
        return false;
    }

    // ---- Blackrock Depths shapes (Blackrock Depths build §3.9, WP-G1, drill `blackrock-depths-shapes`) ------------
    // The wave 1 shapes no drill had run, on the heroic numbers of their bosses (pct= in the percent mode). Two
    // drill-only steps (the runner handles them like `marker`, MechanicCommand.cpp RunDrillWith; ParseWalk / ParseLeash
    // below):
    //   walk <yd> <ms> [offset=<deg>]   the selected unit walks <yd> along the run's facing (turned by offset=,
    //                                   counterclockwise) over <ms>, relocated every kWalkStepMs (WalkPoint): a boss
    //                                   with combat movement off (Roccor's Restless March). Works on a rooted unit (the
    //                                   sim's boss stand-in); it keeps its orientation.
    //   leash <yd> <ms> [role=tank|healer|nonhealer|any] [anchor=tank] [tick=<ms>] tag=<n>
    //                                   for <ms>, every tick (default 1000), each alive scope member farther than <yd>
    //                                   from the selected unit (2D; role=: only the members the role provider calls
    //                                   tanks / healers / not healers) takes a ledger failure `Zone/<n>` (OffSpot).
    //                                   anchor=tank measures from the member's nearest alive tank of the scope instead
    //                                   (not itself; no such tank: the member is skipped), as a healer's real reach
    //                                   is to the tank (ruling 2026-11-18). Nothing is registered, so the bots never
    //                                   see it: it judges whether they keep up on their own.
    // And on `.mech test` lines: `offset=<deg>` turns a line's fixed facing (and the axes of its shift=) from the run's
    // facing, counterclockwise (not with to= or follow=1): a lane across the strip (offset=90), a Rampage back along
    // the route (offset=180).
    // Hide (D13 Bael'Gar) is WP-HK's row, in cycle 0 from 52.5 s to 59.5 s (kBrdTagHide): `hide <range> <ms>
    // [blockers=3] [apart=8] [r=2.5] [display=3711] [go=148503] [behind=3] [share=2] [eye=<yd>] [linger=3000]` (the
    // Hide kind, MechanicHide.h): game objects summoned on a ring round the selected unit, which is the source.
    constexpr std::uint32_t kWalkStepMs = 250;          // one relocation per 250 ms (1.5 yd/s: 0.375 yd a step)
    constexpr float kMaxWalkYd = 60.f;
    constexpr std::uint32_t kDefaultLeashTickMs = 1000;

    // The relocations of a walk of `ms` after the start (one every kWalkStepMs, the last at `ms`): at least 1.
    constexpr std::uint32_t WalkSteps(std::uint32_t ms)
    {
        return ms < kWalkStepMs ? 1u : (ms + kWalkStepMs - 1) / kWalkStepMs;
    }

    // When relocation k (1..WalkSteps) of a walk of `ms` lands (ms after the walk starts).
    constexpr std::uint32_t WalkStepAt(std::uint32_t k, std::uint32_t ms)
    {
        return k * kWalkStepMs < ms ? k * kWalkStepMs : ms;
    }

    // Where the walker stands `elapsedMs` into a walk of `yd` over `ms` from `start` along `heading` (radians): a
    // straight line at an even pace, held at the end (z is the start's; the runtime snaps it to the floor).
    inline Geo::Vec3 WalkPoint(Geo::Vec3 const& start, float heading, float yd, std::uint32_t ms,
        std::uint32_t elapsedMs)
    {
        float const f = ms ? std::min(1.f, static_cast<float>(elapsedMs) / static_cast<float>(ms)) : 1.f;
        return Shifted(start, heading, yd * f, 0.f);
    }

    // The judge passes of a leash of `ms` (at tick, 2 x tick, ... while within `ms`).
    constexpr std::uint32_t LeashTicks(std::uint32_t ms, std::uint32_t tickMs)
    {
        return tickMs ? ms / tickMs : 0u;
    }

    // Is `member` farther than `yd` from `unit` on the ground plane?
    inline bool OutOfLeash(Geo::Vec3 const& unit, Geo::Vec3 const& member, float yd)
    {
        float const dx = member.x - unit.x;
        float const dy = member.y - unit.y;
        return dx * dx + dy * dy > yd * yd;
    }

    // Who a leash judges (role=) and what it measures from (anchor=; see the step's comment above).
    enum class LeashRole : std::uint8_t
    {
        Any,
        Tank,
        Healer,
        NonHealer
    };

    enum class LeashAnchor : std::uint8_t
    {
        Unit,           // the selected unit (the walking boss)
        Tank,           // anchor=tank: the member's nearest alive tank of the scope
        Self            // anchor=self (Scarlet Monastery G2): where the member itself stood as the leash started
    };

    // Does a leash of `role` judge a member the role provider calls a tank / a healer?
    constexpr bool LeashJudges(LeashRole role, bool tank, bool healer)
    {
        switch (role)
        {
            case LeashRole::Tank:
                return tank;
            case LeashRole::Healer:
                return healer;
            case LeashRole::NonHealer:
                return !healer;
            default:
                return true;
        }
    }

    // anchor=tank: is `member` farther than `yd` from every one of `tanks` (the scope's alive tanks other than the
    // member)? nullopt with no tank: the member is skipped, not failed.
    inline std::optional<bool> OutOfTankLeash(std::vector<Geo::Vec3> const& tanks, Geo::Vec3 const& member, float yd)
    {
        if (tanks.empty())
            return std::nullopt;
        for (Geo::Vec3 const& t : tanks)
            if (!OutOfLeash(t, member, yd))
                return false;
        return true;
    }

    // Ledger tags of `blackrock-depths-shapes` (sweep keys `Zone/<tag>`, the ward's `Stack/11`).
    constexpr std::uint32_t kBrdTagRampage = 1;         // RO3 heroic: 20 x 8 lane from him at each route end, 2.5 s
    constexpr std::uint32_t kBrdTagTankLeash = 2;       // RO1: the tank more than 8 yd from the walking boss
    constexpr std::uint32_t kBrdTagRangeLeash = 3;      // RO1: a non-healer more than 40 yd from him (ranged
                                                        // reposition), a healer more than 40 yd from the tank
    constexpr std::uint32_t kBrdTagLeapLine = 4;        // GR4 heroic: three 3 yd hound-to-landing lines, 1.5 s
    constexpr std::uint32_t kBrdTagLeapLanding = 5;     // GR3 heroic: the 5 yd landing at the marked spot, 1.5 s
    constexpr std::uint32_t kBrdTagAxe = 6;             // GO2 heroic: the 3 yd axe landing on a player's spot, 2 s
    constexpr std::uint32_t kBrdTagAxeShown = 7;        // GO2: the return line shown before its lock (4 s, no damage)
    constexpr std::uint32_t kBrdTagAxeReturn = 8;       // GO2 heroic: the locked return line axe to him, 2 s
    constexpr std::uint32_t kBrdTagRupture = 9;         // OK1 normal: donut from 8 yd to the arena, 3 s
    constexpr std::uint32_t kBrdTagRuptureHeroic = 10;  // OK1 heroic: donut from 6 yd, 3 s
    constexpr std::uint32_t kBrdTagWard = 11;           // LG1: the ward's Stack (Stack/11)
    constexpr std::uint32_t kBrdTagTide = 12;           // LG1 heroic: Fire Tide, donut from the ward's edge, 4 s
    constexpr std::uint32_t kBrdTagEdgeBand = 13;       // IN8 / the necks: 4 yd lethal bands on the strip's edges, 5 s
    constexpr std::uint32_t kBrdTagCrumble = 14;        // IN13 (§7 #14 A): the next 2 yd of the long edges, 5 s
    constexpr std::uint32_t kBrdTagSweep = 15;          // IN11: the 10 yd Arm Sweep band across the strip, 4 s
    constexpr std::uint32_t kBrdTagHide = 16;           // BG4 heroic: Molten Furnace, hide behind 3 blockers, 5 s
    constexpr std::uint32_t kBrdBarTags[] = { 2, 3, 4, 5, 8, 9, 10, 12, 13, 14, 15, 16 };

    inline bool BrdBarTag(std::uint32_t tag)
    {
        for (std::uint32_t t : kBrdBarTags)
            if (t == tag)
                return true;
        return false;
    }

    // Cycle 0 (cycle=0/2): Lord Roccor RO1 + RO3 heroic: a Rampage lane, then 30 yd out at 1.5 yd/s (speed_walk 0.6),
    // a Rampage back along the route, 30 yd back (a 60 yd path), the three leashes on each leg (the tank on him, the
    // non-healers on him, the healers on the tank); then Houndmaster Grebmar GR3 + GR4 heroic twice (a member, then a
    // non-tank, as the double whistle below 30%): three hound lines from 10 yd round him to the marked spot and the 5
    // yd landing there. Cycle 1 (cycle=1/2): Gorosh GO2 heroic (axe, shown return, locked return), Ok'thor OK1
    // normal then heroic, Loregrain LG1-LG3 heroic (three tides at three ward spots hugging him, radius 5, 3, 4),
    // Incendius IN8 / IN13 / IN11 on a 48 x 22 strip round him (long axis along the run's facing).
    constexpr std::uint32_t kBrdCycleMs = 60000;
    constexpr std::uint32_t kBrdCycles = 2;
    constexpr float kRoccorWalkYd = 30.f;
    constexpr std::uint32_t kRoccorWalkMs = 20000;      // 30 yd at 1.5 yd/s
    constexpr float kRoccorWalkYdS = 1.5f;
    constexpr float kRampageLength = 20.f;
    constexpr float kRampageWidth = 8.f;
    constexpr std::uint32_t kRampageMs = 2500;
    constexpr float kTankLeashYd = 8.f;
    constexpr float kRangeLeashYd = 40.f;
    constexpr float kLeapLineWidth = 3.f;
    constexpr float kLeapLandingYd = 5.f;
    constexpr std::uint32_t kLeapMs = 1500;
    constexpr float kHoundAwayYd = 10.f;
    constexpr std::uint32_t kHounds = 3;
    constexpr float kAxeLandingYd = 3.f;
    constexpr std::uint32_t kAxeLandingMs = 2000;
    constexpr std::uint32_t kAxeStuckMs = 6000;         // the return line shows this long, locked for the last 2 s
    constexpr std::uint32_t kAxeLockMs = 2000;
    constexpr float kAxeReturnWidth = 4.f;              // §7 #21 default (S1 846: unset)
    constexpr float kRuptureOuterYd = 45.f;             // §7 #21 default (S1 922: "the whole arena")
    constexpr float kRuptureInnerYd = 8.f;
    constexpr float kRuptureInnerHeroicYd = 6.f;
    constexpr std::uint32_t kRuptureMs = 3000;
    constexpr float kTideOuterYd = 45.f;                // §7 #21 default (S2 182: unset)
    constexpr std::uint32_t kTideMs = 4000;
    constexpr float kStripLength = 48.f;                // §7 #14 A: the Anvil bulge, s -24 to +24
    constexpr float kStripWidth = 22.f;
    constexpr float kEdgeBandYd = 4.f;
    constexpr float kCrumbleBandYd = 2.f;
    constexpr std::uint32_t kBandWarnMs = 5000;
    constexpr float kSweepWidth = 10.f;
    constexpr std::uint32_t kSweepMs = 4000;

    // ---- Blackrock Depths shapes, wave 2 (Blackrock Depths build §3.9, WP-G2, drill `blackrock-depths-shapes-2`) --
    // A second drill rather than more rows in `blackrock-depths-shapes`: both of its 60 s cycles are full (cycle 0 to
    // 59.5 s with the hide, cycle 1 to 58 s with the bands), and a third cycle would retime every wave 1 row, its
    // tests and its 120 s sweep. One sweep per drill (§3.9), so the wave 2 sweep runs on its own. Tags 21-27 never
    // meet the wave 1 tags 1-16. Two keys of `.mech test` added for it (MechanicCommand.cpp):
    //   soak <width> <ms> shape=line to=target|member|me
    //                                   a beam from the anchor to that unit: Shapes::Line(from, to, width), the
    //                                   D23 call. It turns with the unit live; its length is the distance at the
    //                                   registration (BossMechanics.h), so a unit that drifts along the beam leaves
    //                                   it short or long by the drift (BeamLengthError below).
    //   at=target                       the point is where the selected unit stands as the step registers. A drill
    //                                   parses every step at its start, so a plain point stays where a walking boss
    //                                   stood then (Moira's spot, the hall's axis); at=target follows him.
    constexpr std::uint32_t kBrd2TagBeam = 21;          // EM2 heroic: Nemesis, a 2 yd line Soak Moira to him (group)
    constexpr std::uint32_t kBrd2TagLoner = 22;         // TS4 heroic: Seething Cold, 2 others within 6 yd (Stack/22)
    constexpr std::uint32_t kBrd2TagLivewire = 23;      // AR8 heroic: 8 yd round a carrier, 4% every 2 s
    constexpr std::uint32_t kBrd2TagLivewireBurst = 24; // AR8: the burst at the dispel, 30% within 8 yd
    constexpr std::uint32_t kBrd2TagCrossfire = 25;     // MG3 heroic: the 10 yd lane across the hall at his spot
    constexpr std::uint32_t kBrd2TagCrossfireBurn = 26; // MG3: the lane burning on, 15% a second
    constexpr std::uint32_t kBrd2TagFlameWall = 27;     // MG4 heroic: the 12 yd strip along a wall, 15% a second
    // Gated per member: the loner and the three fire shapes. The beam's MissedSoak is a group failure (the sweep's
    // "group" bar); Livewire (AR8, no §3.9 row) is reported.
    constexpr std::uint32_t kBrd2BarTags[] = { 22, 25, 26, 27 };
    constexpr std::uint32_t kBrd2GroupTags[] = { 21 };

    inline bool Brd2BarTag(std::uint32_t tag)
    {
        for (std::uint32_t t : kBrd2BarTags)
            if (t == tag)
                return true;
        return false;
    }

    // Cycle 0 (cycle=0/2): the Emperor EM2 heroic: three Nemesis channels 20 s apart from Moira's spot, rooted 15 yd
    // to the left of his start (§7 #19), the first while the tank drifts him 6 yd along the run's facing, the second
    // while he drifts back, the third standing. Cycle 1 (cycle=1/2): the Seven TS4 heroic (Seething Cold), Argelmach
    // AR8 heroic (Livewire and its burst), then Magmus MG3 + MG4 heroic in a 39 yd hall whose axis is the run's
    // facing: a Crossfire at his spot while the tank walks him 12 yd out of it, a Flame Wall along the left wall, a
    // second Crossfire 12 s after the first that walks him home.
    constexpr std::uint32_t kBrd2CycleMs = 60000;
    constexpr std::uint32_t kBrd2Cycles = 2;
    constexpr float kBeamWidth = 2.f;                   // §7 #19 default (S4: unset; DG34 gap 12)
    constexpr float kMoiraYd = 15.f;                    // §7 #19: Dagran held 12-20 yd from Moira
    constexpr std::uint32_t kBeamWarnMs = 3000;         // from the channel start to the first judged second
    constexpr std::uint32_t kNemesisMs = 8000;          // EM2: the 8 s channel
    constexpr std::uint32_t kNemesisEveryMs = 20000;    // EM2 heroic
    constexpr float kDagranDriftYd = 6.f;               // the tank's drift during one channel
    constexpr float kLonerYd = 6.f;
    constexpr std::uint32_t kLonerOthers = 2;
    constexpr std::uint32_t kLonerCastMs = 3000;        // TS4: the 3 s mood cast
    constexpr std::uint32_t kLonerMs = 10000;
    constexpr float kLivewireYd = 8.f;
    constexpr std::uint32_t kLivewireMs = 12000;        // AR8: the 12 s debuff
    constexpr std::uint32_t kLivewireTickMs = 2000;
    constexpr float kHallWidth = 39.f;                  // NAVMESH §4.5: x 1361.2-1400.2 (spec 34)
    constexpr float kCrossfireWidth = 10.f;
    constexpr std::uint32_t kCrossfireGlowMs = 3000;
    constexpr std::uint32_t kCrossfireBurnMs = 6000;    // the ignition second and five more
    constexpr std::uint32_t kCrossfireEveryMs = 12000;  // MG3 heroic
    constexpr float kMagmusDragYd = 12.f;               // S4 1182: the tank walks him at least 10 yd out
    constexpr float kFlameWallWidth = 12.f;
    constexpr float kFlameWallLength = 40.f;
    constexpr std::uint32_t kFlameWallMs = 8000;
    constexpr std::uint32_t kFlameWallGlowMs = 3000;

    // A beam Shapes::Line(from, to) after `to` drifted: its length is still the distance at the registration, its
    // direction the live one. > 0: it stops that far short of `to`; < 0: it runs that far past it.
    inline float BeamLengthError(Geo::Vec3 const& from, Geo::Vec3 const& toAtStart, Geo::Vec3 const& toNow)
    {
        return Geo::Dist2d(from.x, from.y, toNow.x, toNow.y) - Geo::Dist2d(from.x, from.y, toAtStart.x, toAtStart.y);
    }

    // ---- Scarlet Monastery shapes (Scarlet Monastery build §3.9, WP-G1, drill `scarlet-monastery-shapes`) ----------
    // The wave 1 shapes no drill had run, on the heroic numbers of their bosses (pct= in the percent mode). Grammar
    // added for it (MechanicCommand.cpp):
    //   pick=tank                       the selected unit's victim (the tank; none: anyone): Torment on the tank,
    //                                   whose bot backs off along the beam (EndBacksOff, SM-BM F2).
    //   leash ... near=1                the leash inverted: a member fails while it stands CLOSER than <yd>
    //                                   (LeashFails): the tank that did not back off from a Torment aimed at it.
    //   chase <yd> <ms> [home=1]        drill-only step: for <ms> the selected creature steps after its victim like
    //                                   an unrooted boss (ChaseRelocation, every kChaseStepMs at kChaseSpeedYdS, from
    //                                   its melee range in to its stop distance), never more than <yd> from where the
    //                                   chase started; home=1 puts it back there at the end. The sim's boss stand-in
    //                                   is rooted (SimTankDummyAI), so without it a tank's drag-out (the banner, a
    //                                   rot pool under him) leaves him behind in the zone.
    //   marker <n>                      now also remembers the marked member as the command's last pick, so a later
    //                                   pick=same lands on it (the Mark carrier's trail).
    // Tanks are judged separately (§3.9): tags 2 and 6 judge only the tank, so the sweep bars them on the reference
    // members too (DRILL_BARS "references"); so are the trail's 4 and 5 and the spike's pillar 13 (the carrier is a
    // random member, so the member under test carries one Mark in four and one Spike in five: the F5 signal is the
    // whole group's).
    constexpr std::uint32_t kSmTagTorment = 1;          // VI4 heroic: the beam Soak, 4 others, a group failure (Soak/1)
    constexpr std::uint32_t kSmTagBackOff = 2;          // VI4: the targeted tank still within 8.5 yd of him (OffSpot)
    constexpr std::uint32_t kSmTagScent = 3;            // LK3: the scent (Bait, 5 s) landing, flat, reported
    constexpr std::uint32_t kSmTagTrail = 4;            // LK3 heroic: a trail marker landing on anyone but the carrier
    constexpr std::uint32_t kSmTagTrailCarrier = 5;     // LK3 heroic: a trail marker landing on the carrier
    constexpr std::uint32_t kSmTagBanner = 6;           // RH1: the tank in the banner's 10 yd (tank-only, no damage)
    constexpr std::uint32_t kSmTagRot = 7;              // IR1 heroic: the growing Grave Rot under him, 7 -> 10 yd
    constexpr std::uint32_t kSmTagEyePool = 8;          // IR5: a 7 yd pool under a player (2 s marker)
    constexpr std::uint32_t kSmTagGlare = 9;            // IR6 heroic: facing the source at the end of a 2 s cast
    constexpr std::uint32_t kSmTagWallMark = 10;        // DN3 heroic: an 8 yd burst carried to the wall, on others
    constexpr std::uint32_t kSmTagFirePool = 11;        // DN3a heroic: the 3 yd fire pool 1 s after a burst
    constexpr std::uint32_t kSmTagSpikeCarry = 12;      // TH3: the carried 4 yd marker (no damage), reported
    constexpr std::uint32_t kSmTagSpikePillar = 13;     // TH3 heroic: the pillar 1.5 s after the landing, carrier too
    constexpr std::uint32_t kSmTagSpikePool = 14;       // TH3 heroic: the 4 yd pool, 30 s
    constexpr std::uint32_t kSmBarTags[] = { 2, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14 };
    constexpr std::uint32_t kSmGroupTags[] = { 1 };
    constexpr std::uint32_t kSmReferenceTags[] = { 2, 4, 5, 6, 13 };

    inline bool SmBarTag(std::uint32_t tag)
    {
        for (std::uint32_t t : kSmBarTags)
            if (t == tag)
                return true;
        return false;
    }

    // Cycle 0 (cycle=0/3): Vishas VI4 heroic twice (the beam on the tank, who backs off; then on a non-tank, the tank
    // soaking at his end), Loksey LK3 heroic twice (a non-tank carrier scented for 5 s, walking 22 yd off him, then a
    // 4.5 s trail of nine 5 yd markers 0-2 yd round it every 0.5 s, each landing 1.5 s later). Cycle 1 (cycle=1/3):
    // Ironspine IR1 + IR5 + IR6 heroic (the rot under him growing 7 -> 10 yd over 30 s by re-registration, two eye
    // pools under players with each rot, a Glare every 6 s; the boss chases the tank that drags him off the rot), then
    // Doan DN3 + DN3a heroic (three non-tanks carry an 8 yd burst to the edge of a 19 yd chamber, a fire pool 1 s
    // after). Cycle 2 (cycle=2/3, F-1b: STAGE-R-1-GY MN-1): Thalnos's crypt on a hard mode pull, Galvar's RH1 banner
    // at his feet that only the tank sees for 30 s, the boss chasing the tank off it, and TH3 Flame Spike twice (a
    // random carrier, tank included, walks a 4 yd marker 2 s to the edge; the pillar 1.5 s later, then a 30 s pool).
    // The live numbers are the bosses' Logic headers (CORE mod-dungeon-bosses ScarletMonastery/*Logic.h), mirrored
    // here by hand: keep them in step.
    constexpr std::uint32_t kSmCycleMs = 60000;
    constexpr std::uint32_t kSmCycles = 3;
    constexpr float kTormentWidth = 3.f;                // §3.5 D1: SM-BM's example width (VI4: unset)
    constexpr std::uint32_t kTormentWarnMs = 1500;      // VishasLogic.h kTormentWarnMs: the emote to the first tick
    constexpr std::uint32_t kTormentJudgedMs = 8000;    // VishasLogic.h kTormentMs: 8 judged seconds, a 9.5 s channel
    constexpr std::uint32_t kTormentSoakers = 4;        // a 5-man group: everyone but the target
    constexpr float kTormentBackOffYd = 10.f;           // SM-BM F2 example (D1's call)
    constexpr float kTormentTankNearYd = 4.f;
    constexpr float kTormentBackOffJudgeYd = 8.5f;      // sm_shapes_test: the tank walks back past 8.5 yd in time
    constexpr std::uint32_t kScentMs = 5000;            // LK3: the 5 s Scent debuff
    constexpr float kScentOthersYd = 22.f;              // LokseyLogic.h kScentFarYd: the carrier walks 22 yd off him
    constexpr std::uint32_t kTrailMarkers = 9;          // three hounds and six pups: the longest normal trail
    constexpr std::uint32_t kTrailEveryMs = 500;
    constexpr std::uint32_t kTrailWarnMs = 1500;
    constexpr float kTrailMarkerYd = 5.f;
    constexpr float kTrailScatterYd = 2.f;
    constexpr float kBannerYd = 10.f;                   // RH1: 10 yd round the banner at Galvar's feet
    constexpr std::uint32_t kBannerWarnMs = 3000;
    constexpr std::uint32_t kBannerMs = 30000;          // ThalnosHardModeLogic.h kBannerMs: 3 s warned + 27 s judged
    constexpr float kRotFromYd = 7.f;                   // IR1 heroic: 7 -> 10 yd over 30 s
    constexpr float kRotToYd = 10.f;
    constexpr std::uint32_t kRotStepMs = 10000;         // KT-SM GrowingZone: a step every 10 s, warned 1 s ahead
    constexpr std::uint32_t kRotWarnMs = 1500;          // IR1: the 1.5 s cast
    constexpr std::uint32_t kRotStepWarnMs = 1000;
    constexpr std::uint32_t kEyePoolWarnMs = 2000;      // IR5: the 2 s marker
    constexpr std::uint32_t kGlareMs = 2000;            // IR6: the 2 s cast, every 6 s while the Eye is loose
    constexpr std::uint32_t kGlareEveryMs = 6000;
    constexpr float kGlareRangeYd = 40.f;
    constexpr float kWallMarkYd = 8.f;                  // DN3: 8 yd burst, 5 s mark, three marks on heroic
    constexpr std::uint32_t kWallMarkMs = 5000;
    constexpr std::uint32_t kWallMarks = 3;
    constexpr float kDoanRoomYd = 19.f;                 // the chamber, about 1,180 sq yd: a 19.4 yd circle
    constexpr float kFirePoolYd = 3.f;                  // DN3a: 3 yd, forms 1 s after the burst
    constexpr std::uint32_t kEyePoolsEndMs = 41500;     // IR5: live 60 s; here to the last rung's end (cycle 1)
    constexpr float kSpikeYd = 4.f;                     // ThalnosLogic.h TH3: kSpikeRadius
    constexpr std::uint32_t kSpikeCastMs = 2000;        // the bar (the carry)
    constexpr std::uint32_t kSpikeMarkerMs = 1500;      // landing to the pillar
    constexpr std::uint32_t kSpikePoolMs = 30000;       // heroic
    constexpr std::uint32_t kSpikeFirstMs = 6000;       // kSpikeBeat: first at 6 s, every 15 s
    constexpr std::uint32_t kSpikeEveryMs = 15000;
    constexpr float kCryptRoomYd = 14.f;                // the crypt, 18 x 33 yd: a 13.7 yd circle of its area

    // ---- the chase (drill-only `chase` step) ----
    constexpr std::uint32_t kChaseStepMs = 250;
    constexpr float kChaseSpeedYdS = 7.f;               // a creature's base run speed
    constexpr float kMaxChaseYd = 40.f;

    struct ChaseSpec
    {
        float yd = 0.f;                 // never more than this from where the chase started
        std::uint32_t ms = 0;
        bool home = false;              // home=1: back to the start at the end
    };

    // The relocations of a chase of `ms` (one every kChaseStepMs, the last at `ms`): at least 1.
    constexpr std::uint32_t ChaseSteps(std::uint32_t ms)
    {
        return ms < kChaseStepMs ? 1u : (ms + kChaseStepMs - 1) / kChaseStepMs;
    }

    // One relocation of a chasing unit at `unit` after its victim at `victim`: none while within `triggerYd` (its
    // melee range); else toward the victim by at most `maxStepYd`, stopping `stopYd` short of it (its combat reaches
    // plus a little), and pulled back onto the circle of `maxFromStartYd` round `start`. nullopt: it stays.
    inline std::optional<Geo::Vec3> ChaseRelocation(Geo::Vec3 const& start, Geo::Vec3 const& unit,
        Geo::Vec3 const& victim, float triggerYd, float stopYd, float maxStepYd, float maxFromStartYd)
    {
        float const dx = victim.x - unit.x;
        float const dy = victim.y - unit.y;
        float const d = std::sqrt(dx * dx + dy * dy);
        if (d <= triggerYd || d <= stopYd || d <= 0.f)
            return std::nullopt;
        float const move = std::min(maxStepYd, d - stopYd);
        Geo::Vec3 p{ unit.x + dx / d * move, unit.y + dy / d * move, unit.z };
        float const sx = p.x - start.x;
        float const sy = p.y - start.y;
        float const fromStart = std::sqrt(sx * sx + sy * sy);
        if (fromStart > maxFromStartYd && fromStart > 0.f)
        {
            p.x = start.x + sx / fromStart * maxFromStartYd;
            p.y = start.y + sy / fromStart * maxFromStartYd;
        }
        float const mx = p.x - unit.x;
        float const my = p.y - unit.y;
        if (mx * mx + my * my < 0.0001f)
            return std::nullopt;
        return p;
    }

    // ---- Scarlet Monastery shapes, wave 2 (Scarlet Monastery build §3.9, WP-G2, drill `scarlet-monastery-shapes-2`) -
    // The wave 2 rows: a second drill (wave 1's two 60 s cycles are full), one sweep of its own; heroic numbers in
    // pct= (hard mode for the beams' cycle half). Tags 21-40 never meet wave 1's 1-11. NAVMESH numbers (HANDOFF §2.3,
    // §7 #28-#33 ruled A): Halden's ring 5-11 yd, Herod's pit r 13.1 and 121 yd lap, Crell's storeroom. Grammar
    // added for it (MechanicCommand.cpp):
    //   along=target                    a point's axes (shift=, a line's facing) follow the selected unit's own facing
    //                                   as the repeat registers, not the run's: Thorne's furrow 5 yd ahead of him.
    //   past=<yd>                       (line, reach=1) the fixed line runs that far past the to= unit: FixLine.
    //   shift= with to= fix=1           (line) moves the source before it is aimed (it used to move it after, along
    //                                   the aimed facing; no drill combined them): a blade thrown from the ledge.
    //   leash ... anchor=self           each member judged against where it stood as the leash started (the tank that
    //                                   walks round Thorne to re-face him).
    //   holdfacing <ms>                 drill-only step: Mechanic::HoldTankFacing(the test owner, the selected unit,
    //                                   ms) (ParseHoldFacing; SM-BM F6, D12's call).
    //   repeat= / every= on a step      now honored by the runner (RepeatOffsets above; tag kept per repeat).
    // Tanks are judged separately (§3.9): the tank-only tags (23, 25, 31) and the random-carrier tags (21, 26, 30)
    // are barred on the reference members too (DRILL_BARS "references").
    constexpr std::uint32_t kSm2TagFurrow = 21;         // GD1 heroic: a Sow carrier off the furrow at the debuff end
    constexpr std::uint32_t kSm2TagPrune = 22;          // GD1a heroic: the 120 deg / 8 yd Prune (non-tanks)
    constexpr std::uint32_t kSm2TagTankHold = 23;       // F6: the tank walked > 4 yd while the facing hold stood
    constexpr std::uint32_t kSm2TagHolyWater = 24;      // GD2 heroic: the 12 yd ring at him (2.5 s cast)
    constexpr std::uint32_t kSm2TagBlessedGround = 25;  // GD2a: the tank inside the 8 yd tank-only ground (F14)
    constexpr std::uint32_t kSm2TagBloom = 26;          // GD1c heroic (Bless the Harvest missed): 6 yd at the furrow
    constexpr std::uint32_t kSm2TagCloseRanks = 27;     // MA4 heroic: the 5-11 yd band round Halden, 12% a second
    constexpr std::uint32_t kSm2TagFlame = 29;          // WM5: the Vigil Flame 1-body soak unshared (group)
    constexpr std::uint32_t kSm2TagSeared = 30;         // WM5a: a Seared (or the flame's) player within 6 yd of it
    constexpr std::uint32_t kSm2TagFlameTank = 31;      // F7: the tank left Mograine while a non-tank could share
    constexpr std::uint32_t kSm2TagBladeSpread = 32;    // AR7: the lasting 6 yd spread of the Blade window, reported
    constexpr std::uint32_t kSm2TagStrike = 33;         // AR7 heroic: the first Proving Strike dash (2 s)
    constexpr std::uint32_t kSm2TagStrikeNext = 34;     // AR7 heroic: the four follow-up dashes (1 s each)
    constexpr std::uint32_t kSm2TagLapOut = 35;         // HB1a: the 10 yd lane out of the pit (2.5 s)
    constexpr std::uint32_t kSm2TagLapBand = 36;        // HB1b: the ledge band, closed for the lap
    constexpr std::uint32_t kSm2TagBlade = 37;          // HB1c heroic: a blade of light across the pit (1.5 s)
    constexpr std::uint32_t kSm2TagLapHome = 38;        // HB1d: the 10 yd lane home to the pit's center (1.5 s)
    constexpr std::uint32_t kSm2TagBeam = 39;           // HM3: Ravager's Beacon, two beams stepping 30 deg / 1.5 s
    constexpr std::uint32_t kSm2TagBladeHard = 40;      // HB1c hard mode: two blades at once every 1.5 s (60%)
    constexpr std::uint32_t kSm2BarTags[] = { 21, 22, 23, 24, 25, 26, 27, 30, 31, 33, 34, 35, 36, 37, 38, 39, 40 };
    constexpr std::uint32_t kSm2GroupTags[] = { 29 };
    constexpr std::uint32_t kSm2ReferenceTags[] = { 21, 23, 25, 26, 30, 31 };

    inline bool Sm2BarTag(std::uint32_t tag)
    {
        for (std::uint32_t t : kSm2BarTags)
            if (t == tag)
                return true;
        return false;
    }

    // Cycle 0 (cycle=0/3): Thorne GD1 heroic twice (a Prune every 10 s; a Sow on three carriers whose furrow is 5 yd
    // ahead of him, the tank holding his facing from the Sow to the Prune that cuts), GD2 + GD2a between them (Holy
    // Water, then the tank-only blessed ground he is dragged off), the second Sow's seeds blooming at once (Bless the
    // Harvest missed: the step-out ring inside the hold); then Halden MA4 heroic (the 5-11 yd band round him while he
    // steps 4 yd out and back). Cycle 1 (cycle=1/3): Whitemane WM5 heroic, four flares 10 s apart on four non-tank
    // carriers in turn (each Seared after, Marked (drill) the stand-in), the fourth with only the tank left to share;
    // then Crell AR7 heroic (the Blade window's lasting spread and two five-dash chains 10 s apart). Cycle 2
    // (cycle=2/3): Herod HB1 heroic (the lap: out lane, the ledge band, six blades from the lap, home lane), then HM3
    // (two beams round the planted axe, 12 steps one way, the hard mode lap at the flip, 12 steps back). The pit's
    // center is the planted axe, 3 yd past him along the run's facing (he kneels 3 yd from it, HM2).
    constexpr std::uint32_t kSm2CycleMs = 60000;
    constexpr std::uint32_t kSm2Cycles = 3;
    // Thorne (D12, CATH GD1-GD3, §7 #32: a 14.9 yd dry spot)
    constexpr float kPruneYd = 8.f;
    constexpr float kPruneAngleDeg = 120.f;
    constexpr std::uint32_t kPruneWarnMs = 2000;
    constexpr std::uint32_t kPruneEveryMs = 10000;
    constexpr std::uint32_t kSowCarriers = 3;           // heroic
    constexpr std::uint32_t kSowMs = 5000;              // the Blessed Grain debuff
    constexpr float kFurrowYd = 3.f;
    constexpr float kFurrowAheadYd = 5.f;
    constexpr std::uint32_t kFacingHoldMs = 9000;       // from the Sow to the Prune that cuts (SM-BM's D12 example)
    constexpr float kTankHoldDriftYd = 4.f;             // a re-facing walk round him moves the tank farther
    constexpr float kHolyWaterYd = 12.f;                // heroic
    constexpr std::uint32_t kHolyWaterMs = 2500;
    constexpr float kBlessedGroundYd = 8.f;
    constexpr std::uint32_t kBlessedGroundMs = 8000;
    constexpr std::uint32_t kBlessedGroundWarnMs = 500;
    constexpr float kBloomYd = 6.f;
    constexpr std::uint32_t kBloomWarnMs = 2000;
    // Halden (D10, ARM MA4, §7 #30 A: the ring 5-11 yd in the 21 x 40 yd middle chamber)
    constexpr float kRanksInnerYd = 5.f;
    constexpr float kRanksOuterYd = 11.f;
    constexpr std::uint32_t kRanksWarnMs = 2500;
    constexpr std::uint32_t kRanksMs = 8000;            // heroic: the ring turns 8 s
    constexpr float kRanksStepYd = 4.f;                 // "a boss that steps" (§3.9): out, then back
    constexpr std::uint32_t kRanksStepMs = 2000;
    // Whitemane (D14, CATH WM5, heroic)
    constexpr float kFlameYd = 6.f;
    constexpr std::uint32_t kFlameEveryMs = 10000;
    constexpr std::uint32_t kFlares = 4;                // four non-tank carriers; the fourth flare leaves the tank
    constexpr std::uint32_t kFlameRegisterLagMs = 500;  // each flare's carrier is marked after the last one resolved
    constexpr float kFlameTankLeashYd = 8.f;            // kTankLeashYd: the tank stays on Mograine
    // Crell (D9, ARM AR7, heroic: the Blade 15 s, a chain 2 s after the Issue and every 10 s)
    constexpr float kBladeSpreadYd = 6.f;
    constexpr std::uint32_t kBladeMs = 15000;
    constexpr float kStrikeWidth = 3.f;
    constexpr float kStrikePastYd = 5.f;
    constexpr std::uint32_t kStrikeFirstMs = 2000;
    constexpr std::uint32_t kStrikeNextMs = 1000;
    constexpr std::uint32_t kStrikeDashes = 5;          // every player once
    constexpr std::uint32_t kStrikeEveryMs = 10000;
    // Herod (D11 / D11H, ARM HB1 / HM3; NAVMESH: pit r 13.1, the ledge band out to the walls, lap ~121 yd)
    constexpr float kPitYd = 13.f;
    constexpr float kPitAheadYd = 3.f;                  // the pit's center (the planted axe) past his start
    constexpr float kLapRadiusYd = 19.3f;               // 121 yd / 2 pi
    constexpr float kBandOuterYd = 40.f;                // "out to the walls": the sim's floor has none
    constexpr float kLaneWidth = 10.f;                  // HB1a / HB1d: his 5 yd spin
    constexpr std::uint32_t kOutWarnMs = 2500;
    constexpr std::uint32_t kHomeWarnMs = 1500;
    constexpr float kBladeLaneWidth = 4.f;
    constexpr float kBladeLaneYd = 32.f;                // from the ledge across the pit's center to its far edge
    constexpr std::uint32_t kBladeWarnMs = 1500;
    constexpr std::uint32_t kBladeEveryMs = 1500;
    constexpr float kLapHeroicYdS = 12.f;
    constexpr float kLapHardYdS = 14.f;
    constexpr std::uint32_t kLapBlades = 6;             // heroic: a blade every 1.5 s, each landing inside the lap
    constexpr std::uint32_t kLapBladesHard = 5;         // hard mode: the 8.7 s lap lands five pairs
    constexpr float kBeamInnerYd = 3.f;
    constexpr float kBeamYd = 10.f;                     // 3 -> 13 yd: to the pit's edge
    constexpr float kBeaconWidth = 4.f;
    constexpr float kBeamStepDeg = 30.f;
    constexpr std::uint32_t kBeamStepMs = 1500;
    constexpr std::uint32_t kBeamSteps = 24;            // 12 one way (a full turn), the flip, 12 back
    constexpr float kBeamChaseYd = 12.f;

    // How long one lap of `radiusYd` takes at `ydPerS` (ms).
    inline float LapMs(float radiusYd, float ydPerS)
    {
        return ydPerS > 0.f ? Geo::kTwoPi * radiusYd / ydPerS * 1000.f : 0.f;
    }

    // Where the cyclone is `elapsedMs` into a lap that starts at bearing `startRad` (radians from the run's facing),
    // `dir` +1 counterclockwise, -1 clockwise: the bearing (radians, wrapped to [0, 2 pi)).
    inline float LapBearing(float startRad, int dir, float radiusYd, float ydPerS, float elapsedMs)
    {
        float const b = startRad + static_cast<float>(dir < 0 ? -1 : 1) * ydPerS * elapsedMs / 1000.f / radiusYd;
        float const w = std::fmod(b, Geo::kTwoPi);
        return w < 0.f ? w + Geo::kTwoPi : w;
    }

    // `shift=<ahead>:<left>` for a line turned by `offsetRad` (the run frame turned) whose source sits at `source`
    // (run frame: x ahead, y left of the boss's start): the source in the turned frame.
    inline Geo::Vec3 ShiftInTurnedFrame(Geo::Vec3 const& source, float offsetRad)
    {
        float const c = std::cos(offsetRad);
        float const s = std::sin(offsetRad);
        return { source.x * c + source.y * s, -source.x * s + source.y * c, 0.f };
    }

    // ---- Ragefire Chasm shapes (Ragefire Chasm build §3.9, WP-G1, drill `ragefire-chasm-shapes`) -------------------
    // The eight rows of HANDOFF §3.9 (and the reported ninth's burst half) on the heroic numbers of their bosses (pct=
    // in the percent mode). Each step registers what the boss WP registers (the boss's SP row is named at each tag and
    // step); change both together. Grammar added for it (MechanicCommand.cpp):
    //   mound <r> <arm ms> persist=<ms> [speed=5] [erupt=4] [boom=2000] [pick=]
    //       Oggleflint's Borer: a chaser (a Mechanic Marker) starts at the selected unit and runs at one member (pick=)
    //       at speed= yd/s for <arm> + persist, re-aimed every kMoundAimMs (DungeonKit::Chaser). A circle of <r> on it,
    //       that member only, no damage, no telegraph, Persistent(persist, tick 0): the first pass at <arm> (ledger
    //       tag=). Where it stops, a circle of erupt= with a boom= warning and the step's damage keys, for everyone
    //       (MoundEruptTag: tag= + 1). A hunted member that dies: the mound stops, its ring still erupts (§7 #36).
    //   crawl <r> <life ms> yd=<yd> [bearing=0] [off=4] [speed=3] [need=1] [last=<role>] [pick=]
    //       Jergosh's wisp, registered as DungeonKit::CrawlPickups does: a Soak of <r> and need= bodies on a unit that
    //       starts off= yd from a member (pick=) and crawls at speed= yd/s to the point yd= from the selected unit's
    //       start at bearing= from the run's facing. Telegraph only (MissedSoak Never, one claim per bot, the whole
    //       fight's warning); a member within <r> takes it (polled every kCrawlPollMs); reaching the point (or <life
    //       ms>) is a group failure, `Soak/<tag>`.
    //   pick=fresh
    //       a member, the tank included, the chain has not picked (PickPool): the second mound's own player, the beaten
    //       who is never hunted, the spots' rotation.
    //   inside=<yd>:<ahead>
    //       the Within circle fixed on the selected unit's place at the parse (the drill's start), moved <ahead> along
    //       the run's facing (negative: toward the group): a room, as the bosses' FloorBounds (inside=<yd> alone stays
    //       on the unit).
    //   hide ... of=<n> scale=<s>
    //       the blockers stand on random distinct points of n on the ring; each game object scaled (its collision model
    //       too).
    //   marker 0 one=1
    //       Marked off one random holder only (a dispel in the middle of a cast).
    // And two drill-only steps (ParseBlink, ParseCast below; nothing registers):
    //   blink <yd> 0 [pick=any|fresh|nontank|same]
    //       the selected unit appears <yd> behind a member, facing it; `blink 0 0 at=prev`: on the last point circle's
    //       centre (a leap's landing); `blink 0 0 home=1`: back where it stood before the run's first blink.
    //   cast <spell> <ms> [kick=1] [every=<ms>] tag=<n>
    //       the selected unit casts <spell> at its victim. kick=1: it goes on the encounter's interrupt list
    //       (InterruptEntry, NoteNextCast with every=) and is a group failure when it still runs kCastJudgeLeadMs
    //       before <ms>, or never started. Without kick=1 it is a must-play bar: a group failure when it does not run
    //       to its end (a school lockout, a kick). `Soak/<tag>`, MissedSoak.
    // Tags (sweep keys `<Kind>/<tag>`):
    constexpr std::uint32_t kRfcTagChase = 1;           // OG1: the 3 yd chase circle (no damage), reported
    constexpr std::uint32_t kRfcTagErupt = 2;           // OG2 heroic: the 4 yd eruption (MoundEruptTag of 1)
    constexpr std::uint32_t kRfcTagBlow = 3;            // OG4 heroic: the Beating's 3 yd blows
    constexpr std::uint32_t kRfcTagBlowChase = 4;       // OG1 beside the Beating, reported
    constexpr std::uint32_t kRfcTagBlowErupt = 5;       // OG2 beside the Beating (MoundEruptTag of 4)
    constexpr std::uint32_t kRfcTagBurn = 6;            // JR2 heroic: a 6 yd Conflagrate ring on someone else
    constexpr std::uint32_t kRfcTagWisp = 7;            // JR5: a wisp eaten (group)
    constexpr std::uint32_t kRfcTagFan = 8;             // BZ5: the 8 yd tank-inclusive Fan of Knives
    constexpr std::uint32_t kRfcTagStepFan = 9;         // BZ7 heroic: the 6 yd spin after each Shadowstep
    constexpr std::uint32_t kRfcTagRing = 10;           // TG3 heroic: a tick of the 10 yd Felstorm ring
    constexpr std::uint32_t kRfcTagSpot = 11;           // TG5 heroic: a 4 yd fel spot
    constexpr std::uint32_t kRfcTagHideThree = 12;      // TG6 normal's three spikes, seen at the Nova's end
    constexpr std::uint32_t kRfcTagHideTwo = 13;        // TG6 heroic's opposite pair of spikes, 2 + 3 behind them
    constexpr std::uint32_t kRfcTagKick = 14;           // TG2: the 5 s channel went uninterrupted (group)
    constexpr std::uint32_t kRfcTagBarSame = 15;        // a bar of the kicked school blocked or cut (group, reported)
    constexpr std::uint32_t kRfcTagBarOther = 16;       // a bar of another school blocked or cut (group)
    constexpr std::uint32_t kRfcTagBurst = 17;          // TG9 hard mode: the 6 yd globule bursts, reported (humans)
    constexpr std::uint32_t kRfcTagFiveChase = 18;      // OG1 with five mounds (from 160 s live), reported
    constexpr std::uint32_t kRfcTagFiveErupt = 19;      // OG2 with five mounds (MoundEruptTag of 18), reported first
    constexpr std::uint32_t kRfcTagKickThree = 20;      // JR4's window: the 3 s cast went uninterrupted (group)
    constexpr std::uint32_t kRfcTagKickTwo = 21;        // BZ3, BZ6's window: the 2 s cast went uninterrupted (group)
    constexpr std::uint32_t kRfcLastTag = kRfcTagKickTwo;
    constexpr std::uint32_t kRfcBarTags[] = { 2, 3, 5, 6, 8, 9, 10, 11, 12, 13 };
    constexpr std::uint32_t kRfcGroupTags[] = { 7, 14, 16, 20, 21 };
    // Barred on the reference members too: the pool is every member (§7 #33 A), or the tank must step out.
    constexpr std::uint32_t kRfcReferenceTags[] = { 2, 3, 5, 9, 10, 11 };

    inline bool RfcBarTag(std::uint32_t tag)
    {
        for (std::uint32_t t : kRfcBarTags)
            if (t == tag)
                return true;
        return false;
    }

    // A mound's eruption counts under its chase circle's tag + 1.
    constexpr std::uint32_t MoundEruptTag(std::uint32_t moundTag)
    {
        return moundTag + 1;
    }

    constexpr std::uint32_t kRfcCycleMs = 60000;
    constexpr std::uint32_t kRfcCycles = 3;
    // Oggleflint (D1, SP OG1, OG2, OG4 heroic). The Borer lives 4 s: the circle arms kMoundArmMs after the summon and
    // stands for the rest, so it ends with the chase (Persistent(kMoundLifeMs - kMoundArmMs, 0)).
    constexpr float kMoundYd = 3.f;
    constexpr std::uint32_t kMoundLifeMs = 4000;
    constexpr std::uint32_t kMoundArmMs = 500;
    constexpr float kMoundSpeedYdS = 5.f;               // heroic (normal 4)
    constexpr float kMoundSpeedNormalYdS = 4.f;
    constexpr std::uint32_t kMoundAimMs = 250;          // DungeonKit::ChaserSpec::repath
    constexpr float kEruptYd = 4.f;
    constexpr std::uint32_t kEruptMs = 2000;
    constexpr std::uint32_t kHuntEveryMs = 12000;       // heroic
    constexpr float kBlowYd = 3.f;
    constexpr std::uint32_t kBlowMs = 1500;
    constexpr std::uint32_t kBeatingBlows = 3;
    // The den (§3.8: 527 sq yd, a 13 yd circle of its area; the tank spot at the north wall, so the circle lies 8 yd
    // from him toward the group).
    constexpr float kDenYd = 13.f;
    constexpr float kDenAheadYd = -8.f;
    // Jergosh (D2, SP JR2, JR5 heroic)
    constexpr float kBurnYd = 6.f;
    constexpr std::uint32_t kBurnMs = 2500;
    constexpr float kWispYd = 2.f;
    constexpr float kWispSpeedYdS = 3.f;
    constexpr float kWispGoalYd = 50.f;                 // §7 #21 A
    constexpr float kWispOffYd = 4.f;                   // §7 #29 A
    constexpr float kWispReachYd = 1.5f;                // DungeonKit::CrawlPickupSpec::reach
    constexpr std::uint32_t kWispLifeMs = 25000;        // the drill's safety: 50 yd take 16.7 s, 62 yd 20.7 s
    constexpr std::uint32_t kCrawlPollMs = 250;         // DungeonKit::CrawlPickups::kPollMs
    constexpr std::uint32_t kCrawlRepathMs = 1000;      // ... CrawlPickupSpec::repath
    constexpr float kCrawlZBandYd = 6.f;
    // Bazzalan (D3, SP BZ5, BZ7 heroic; §3.8: his arena ends 4 yd short of the rims, a 15 yd circle)
    constexpr float kFanYd = 8.f;
    constexpr float kStepFanYd = 6.f;
    constexpr std::uint32_t kFanMs = 2000;
    constexpr std::uint32_t kShadowsteps = 3;
    constexpr std::uint32_t kStepEveryMs = 2500;        // landing to landing (kShadowstepEveryMs), never on a resolve
    constexpr std::uint32_t kStepHomeMs = 250;          // the last spin's resolve to the way home
    // No `blink` this soon after the resolve of a ring anchored on the unit: steps run before the resolves of their
    // tick, so one on the instant takes the ring along and hits whoever the unit appears behind.
    constexpr std::uint32_t kBlinkAfterResolveMs = 250;
    constexpr float kStepBehindYd = 2.f;
    constexpr float kRidgeYd = 15.f;
    constexpr float kRidgeAheadYd = -6.f;
    // Taragaman (D4, SP TG3, TG5, TG6 heroic; §3.8: 1,040 sq yd of plateau, an 18 yd circle)
    constexpr float kFelRingYd = 10.f;
    constexpr std::uint32_t kFelCastMs = 2000;
    constexpr std::uint32_t kFelSpinMs = 5000;          // heroic: five ticks
    constexpr float kFelSpotYd = 4.f;
    constexpr float kFelSpotOffYd = 4.f;                // the center 0 to 4 yd from the player
    constexpr std::uint32_t kFelSpotMs = 1500;
    constexpr std::uint32_t kFelSpotEveryMs = 500;
    constexpr std::uint32_t kFelSpots = 10;
    constexpr float kPlateauYd = 18.f;
    constexpr std::uint32_t kNovaMs = 4000;
    constexpr float kSpikeRingYd = 10.f;
    constexpr float kSpikeRadiusYd = 2.f;               // §7 #19 A: HideSpec radius 2.0 and eye 3.05, so the spots lie
    constexpr float kSpikeEyeYd = 3.05f;                // 5 yd behind the spike's centre (the default behind, 3 yd)
    constexpr float kSpikeScale = 2.f;
    constexpr std::uint32_t kSpikeDisplay = 5073;       // Stalagmite01.m2
    constexpr std::uint32_t kSpikePoints = 4;
    // The kick row's stock stand-ins (neither in spell_dbc nor scripted, live and Spell.dbc 2026-11-24): a 5 s Shadow
    // channel (PreventionType 1, CHANNEL_INTERRUPT_FLAG_INTERRUPT), a 2 s Shadow bar and a 2 s Frost one, both
    // PreventionType 0 and InterruptFlags 0.
    constexpr std::uint32_t kKickChannelSpell = 36224;  // Drain Life
    constexpr std::uint32_t kBarSameSpell = 18088;      // Corruption
    constexpr std::uint32_t kBarOtherSpell = 50097;     // Ice Spike Trigger (a dummy on itself)
    constexpr std::uint32_t kKickChannelMs = 5000;
    constexpr std::uint32_t kKickEveryMs = 15000;       // heroic Devour
    // ... and the module's own kickable casts (InterruptFlags 15, PreventionType 1, 50 damage) for the short windows.
    constexpr std::uint32_t kKickThreeSpell = 205012;   // Fireball (drill)
    constexpr std::uint32_t kKickThreeMs = 3000;        // Drain Soul's channel
    constexpr std::uint32_t kKickTwoSpell = 205010;     // Frost Bolt (drill)
    constexpr std::uint32_t kKickTwoMs = 2000;          // Eviscerate and Envenom, Jergosh's Immolate
    constexpr std::uint32_t kKickShortEveryMs = 20000;  // theirs: two short windows are never closer
    constexpr std::uint32_t kBarMs = 2000;

    // `blink`: the spot `yd` behind a member at `pos` facing `o` (Shadowstep), and the facing back at it.
    inline Geo::Vec3 BehindSpot(Geo::Vec3 const& pos, float o, float yd)
    {
        return Shifted(pos, o, -yd, 0.f);
    }

    // `crawl`: where the pickup starts, `off` yd from `from` toward `goal` (never past the goal).
    inline Geo::Vec3 CrawlStart(Geo::Vec3 const& from, Geo::Vec3 const& goal, float off)
    {
        float const dx = goal.x - from.x;
        float const dy = goal.y - from.y;
        float const d = std::sqrt(dx * dx + dy * dy);
        if (d <= 0.01f)
            return from;
        float const u = std::min(1.f, off / d);
        return { from.x + dx * u, from.y + dy * u, from.z };
    }

    // `inside=<yd>[:<ahead>]`: the radius and (with ahead) the fixed centre's offset along the run's facing.
    struct InsideSpec
    {
        float yd = 0.f;
        std::optional<float> ahead;
    };

    // ---- the rows (the drill entry below) ----
    // The 2 s and 3 s kick windows stand in the quiet seconds of cycles 0 and 2, at least 20 s apart.
    // Cycle 0 (0/3), Oggleflint: borer-den (two, three, four mounds on distinct members, tank included, 12 s apart, the
    // unit chasing a hunted tank; five at the cycle's end, reported) and beating-chain (one, then two mounds, and three
    // chained blows on a member none of them hunts; the unit lands on each blow). Cycle 1 (1/3), Jergosh and Bazzalan:
    // burn-spread (2, 3 with one dispelled in the middle of the cast, 5 carriers), wisp-soak (two wisps across the
    // rings), kick-lockout (three 5 s channels 15 s apart, each followed by a bar of another school and one of the
    // same), shadowstep-fans. Cycle 2 (2/3), Taragaman: felstorm-spots three times, spike-hide with three spikes, then
    // an opposite pair, the hard mode bursts.

    inline std::vector<Def> const& All()
    {
        static std::vector<Def> const drills = {
            // Deadmines, Smite (SM2, SM3, SM12): Sweeping Slash, front cone 90 deg / 8 yd, 2 s cast; Backswing, rear
            // cone 1.5 s later; Harpoon Volley, Spread 6 yd on every non-tank, 2.5 s cast.
            { "smite", "front cone 90/8 (2 s), rear cone 1.5 s later, Spread 6 on every non-tank (2.5 s)", 10000,
                false, {
                    { 0, "frontal 8 2000 angle=90 tag=1" },
                    { 1500, "frontal 8 2000 angle=90 rear=1 tag=2" },
                    { 5000, "spread 6 2500 role=nontank tag=3" },
                } },
            // Deadmines, Cookie (CK4-CK6), one dish per course on a random non-tank eater: Fire Pepper Chowder, a
            // 60 deg / 8 yd cone on the eater, 3 breaths 2 s apart from 2 s after the feed (look away); Gut Bomb,
            // RunOut 8 yd 5 s after the feed; Week-Old Fish Stew, RunOut 4 yd on the eater at each of 3 drops 2 s
            // apart, a 4 yd puddle under the eater at each drop (persistent 8 s, shortened from 30 s).
            { "cookie", "chowder cone 60/8 on a member (3 breaths), Gut Bomb RunOut 8, Fish Stew RunOut 4 + 3 "
                "puddles 4", 31000, false, {
                    { 0, "cone 8 2000 angle=60 anchor=member pick=nontank follow=1 persist=5000 tick=2000 tag=4" },
                    { 8000, "runout 8 5000 at=member pick=nontank tag=6" },
                    { 15000, "runout 4 2000 at=member pick=nontank tag=5" },
                    { 17000, "circle 4 1500 at=member pick=same persist=8000 tick=1000 tag=7" },
                    { 17000, "runout 4 2000 at=member pick=same tag=5" },
                    { 19000, "circle 4 1500 at=member pick=same persist=8000 tick=1000 tag=7" },
                    { 19000, "runout 4 2000 at=member pick=same tag=5" },
                    { 21000, "circle 4 1500 at=member pick=same persist=8000 tick=1000 tag=7" },
                } },
            // Shadowfang Keep, Fenrus (FN6): Shadow Leap, Bait on the fewest Baited stacks (205008, the drill's
            // stand-in for the rotation debuff), 6 yd landing circle, 2 s emote, everyone else within 10 yd of him.
            // The rider gives everyone hit a Baited stack, the baiter included, so the next pick rotates; the baiter
            // doing the job gets no Weakened, the others caught in the circle do (user ruling 2026-11-04).
            { "fenrus", "Bait every 10 s: fewest Baited stacks baits, landing r 6, others within 10, Weakened only "
                "for the others hit", 10000, true, {
                    { 0, "bait 10 2000 rule=fewest aura=205008 r=6 rider=205008 weak=1 tag=1" },
                } },
            // Scholomance, Alexei Barov (AB2-AB8): every rule alone (3 s warning, 6 s window), then every heroic pair
            // (4 s warning). Out of bounds: a 7 yd persistent circle on the group for the window. Line up: a 4 x 40 yd
            // lane (shifted 12 yd left when paired with Out of bounds). Desks: 3 yd spots on a 12 yd ring, clear of
            // the Out of bounds circle.
            { "alexei", "every rule, then every heroic pair: hold still, assembly, keep your distance, out of bounds, "
                "line up, desks", 13 * kAlexeiSlotMs, false, {
                    // singles (AB2-AB7)
                    { 0 * kAlexeiSlotMs, "holdstill 6 3000 tol=1 tag=2" },
                    { 1 * kAlexeiSlotMs, "stack 6 9000 cluster=2 tag=3" },
                    { 2 * kAlexeiSlotMs, "spread 8 9000 tag=4" },
                    { 3 * kAlexeiSlotMs, "circle 7 3000 at=group persist=6000 tick=1000 tag=5" },
                    { 4 * kAlexeiSlotMs, "stack 4 9000 shape=line length=40 at=group tag=6" },
                    { 5 * kAlexeiSlotMs, "spot 3 9000 ring=12 at=group tag=7" },
                    // heroic pairs (AB8): Out of bounds with each other rule
                    { 6 * kAlexeiSlotMs, "circle 7 4000 at=group persist=6000 tick=1000 tag=5" },
                    { 6 * kAlexeiSlotMs, "holdstill 6 4000 tol=1 tag=2" },
                    { 7 * kAlexeiSlotMs, "circle 7 4000 at=group persist=6000 tick=1000 tag=5" },
                    { 7 * kAlexeiSlotMs, "stack 6 10000 cluster=2 tag=3" },
                    { 8 * kAlexeiSlotMs, "circle 7 4000 at=group persist=6000 tick=1000 tag=5" },
                    { 8 * kAlexeiSlotMs, "spread 8 10000 tag=4" },
                    { 9 * kAlexeiSlotMs, "circle 7 4000 at=group persist=6000 tick=1000 tag=5" },
                    { 9 * kAlexeiSlotMs, "stack 4 10000 shape=line length=40 at=group shift=0:12 tag=6" },
                    { 10 * kAlexeiSlotMs, "circle 7 4000 at=group persist=6000 tick=1000 tag=5" },
                    { 10 * kAlexeiSlotMs, "spot 3 10000 ring=12 at=group tag=7" },
                    // Assembly with Line up; Line up with Keep your distance
                    { 11 * kAlexeiSlotMs, "stack 4 10000 shape=line length=40 at=group tag=6" },
                    { 11 * kAlexeiSlotMs, "stack 6 10000 cluster=2 tag=3" },
                    { 12 * kAlexeiSlotMs, "stack 4 10000 shape=line length=40 at=group tag=6" },
                    { 12 * kAlexeiSlotMs, "spread 8 10000 tag=4" },
                }, 1 },     // one cycle is 2.6 minutes
            // Soaks: Payday (Deadmines MJ1: 1 body, non-tank, 2 yd, 2.5 s flight, near a random non-tank member, so
            // one stands within reach); an ordered soak (fewest Marked (drill) stacks first, standing in for a boss's
            // soak debuff, players at 2 or more never claim; two members get 1 and 2 stacks first, so the rule
            // decides, not the guid); the Barov fortune (Scholomance AB10: count 0, nobody takes it, 10 yd off the
            // group); a lasting soak on the GM (Rattlegore RG2: 3 yd, 5 s, walk while it lasts).
            { "soaks", "Payday (1 non-tank, 2 yd), ordered soak (Marked asc, cap 2), fortune (need 0), lasting soak "
                "on you (walk)", 26000, false, {
                    { 0, "soak 2 2500 need=1 role=nontank at=member pick=nontank scatter=5 tag=1" },
                    { 2500, "marker 2 pick=nontank" },
                    { 3000, "marker 1 pick=nontank" },
                    { 4000, "soak 3 4000 need=1 order=205009:asc:cap=2 at=group shift=6:0 tag=2" },
                    { 9000, "marker 0" },
                    { 10000, "soak 2 4000 need=0 at=group shift=0:10 tag=3" },
                    { 16000, "soak 3 3000 need=1 anchor=me persist=5000 tick=1000 tag=4" },
                } },
            // Scholomance, Ras Frostwhisper (RF1, RF2, RF5): a member reaches 3 Marked (drill) stacks (standing in
            // for Chill) and runs out 8 yd (self-keyed RunOut) while the rest (0-2 stacks) hold a Stack 8 yd on Ras; a
            // second member reaches 3 stacks during the warning and joins the runners; everyone thaws (0) after.
            { "ras", "self-keyed RunOut 8 at Marked >= 3 while a Stack 8 on the creature holds the rest", 12000,
                false, {
                    { 0, "marker 3 pick=nontank" },
                    { 0, "stack 8 6000 anchor=target aura=205009:0:2 tag=2" },
                    { 0, "runout 8 6000 self=205009:3 tag=5" },
                    { 2000, "marker 3 pick=nontank" },
                    { 9000, "marker 0" },
                } },
            // Deadmines Pilot gap 8 (deadmines-pilot.HANDOFF §3.9): every heroic 1.5 s follow-up, after a 2 s first
            // shape, timed from the first one's resolve (after=). Smite SM4 H: a 3 yd dash line from the creature to a
            // non-tank at least 8 yd away (4 yd excluded at its start), then two more, each from where the last ended
            // (at=last) to a non-tank the chain has not hit (pick=other). SM7 + SM8: Shockwave, a 4 x 25 yd frontal
            // line, then two at 30 degrees either side. SM10 + SM11: Harpoon Sweep, donut 5 / 14, then Haft Slam,
            // circle 6 (in, then out). SN1 then SN2 / SN3 (alternating cycles): Saw Spin, circle 8 on the creature
            // (the tank out too), then Splinter Spray, frontal cone 90 deg / 10 yd, or Runaway, a 4 x 30 yd line
            // through a non-tank's spot (fix=1). RZ1 + RZ10: Cave In, circle 5 under every member (at=each), then
            // Lookout, circle 5 under every member where it stands as the first wave lands.
            { "deadmines-followups", "1.5 s follow-ups: dash chain, aftershock lines, donut then circle, saw then "
                "cone or line, two Cave In waves", 30000, false, {
                    { 0, "line 8 2000 width=3 exclude=4 anchor=target to=member pick=nontank min=8 reach=1 weak=1 "
                        "tag=1" },
                    { 0, "line 8 1500 width=3 exclude=4 at=last to=member pick=other min=8 reach=1 after=resolve "
                        "weak=1 tag=2" },
                    { 0, "line 8 1500 width=3 exclude=4 at=last to=member pick=other min=8 reach=1 after=resolve "
                        "weak=1 tag=3" },
                    { 8000, "frontal 25 2000 line=1 width=4 weak=1 tag=4" },
                    { 0, "frontal 25 1500 line=1 width=4 offset=30 after=resolve weak=1 tag=5" },
                    { 0, "frontal 25 1500 line=1 width=4 offset=-30 after=with weak=1 tag=5" },
                    { 14000, "donut 14 2000 inner=5 anchor=target weak=1 tag=6" },
                    { 0, "circle 6 1500 anchor=target after=resolve weak=1 tag=7" },
                    { 20000, "circle 8 2000 anchor=target weak=1 tag=8" },
                    { 0, "frontal 10 1500 angle=90 after=resolve cycle=even weak=1 tag=9" },
                    { 0, "line 30 1500 width=4 exclude=4 anchor=target to=member pick=nontank fix=1 after=resolve "
                        "cycle=odd weak=1 tag=10" },
                    { 25000, "circle 5 2000 at=each weak=1 tag=11" },
                    { 0, "circle 5 1500 at=each after=resolve weak=1 tag=12" },
                }, 2 },     // two cycles: the cone and the line once each
            // Shadowfang Keep build §3.9 (the chase): Odo's Hunt (OD1), the alarm bell (OD3) and its landing (OD4),
            // and the Echo of Odo (BS5). 0 s: a chaser starts at the creature and fixates a random non-tank; a 3 yd
            // circle on it, eligible = that player only, stands through a 2 s howl, then the chaser walks at the
            // player for 10 s at 80% of player run speed (passes every 200 ms; the first one that holds the player is
            // a catch and ends the hunt). 0.5 s: a bell, a 1-body Soak 2 yd on one of three points 19 yd apart (the
            // room's bells, section 7 #24) around the creature, not the hunted player, not the tank; filled at its 5 s
            // resolve (the walk plus the 1 s ring), it ends the hunt and the chaser bounds to the bell, landing 2 s
            // later with a 6 yd circle (tag 3 for the others, 4 for the ringer). 15 s: the heroic hunt, 95% speed and no bell (spent).
            { "sfk-hunt", "Odo's Hunt: a 3 yd chaser at 80% for 10 s, a bell soak that ends it (6 yd landing), "
                "then a 95% hunt with no bell", 30000, false, {
                    { 0, "hunt 3 2000 speed=0.8 persist=10000 tick=200 pick=nontank pct=35 tag=1" },
                    { 500, "soak 2 5000 need=1 role=nontank bell=3 apart=19 land=6 jump=2000 pct=35 weak=1 tag=2" },
                    { 15000, "hunt 3 2000 speed=0.95 persist=10000 tick=200 pick=nontank pct=35 tag=5" },
                }, 2 },     // two cycles a run; the sweep runs sfk-hunt:4 (120 s)
            // Scholomance build §3.9: the short warnings, heroic numbers (pct= in the percent mode). 0 s, Polkelt PK2:
            // a 6 yd fuse ring 3 yd off a non-tank (2 s), then two chained rings, each 4 yd off the last one's centre
            // (the next specimen inside it), 1 s each. 5 s, Malicia MA6: three waves 1 s apart, a 4 yd circle 2 yd
            // off every member (1.5 s). 10 s, Illucia IB1: a 4 yd marker on any member (1.5 s), then four more, each
            // where that member will be 1.5 s later (at its heading and speed) as the last one lands. 19 s,
            // Rattlegore RG4: rings 0-6, 6-12, 12-18 yd on the creature (2 s, then 1.5 s apart), then back inward
            // 12-18, 6-12, 0-6 (1 s apart). 28 s, Jandice JB1 heroic: a 3 x 3 grid of 8 yd sigils 11 yd apart on the
            // group, one a decoy, and the room's edge (a donut from 17 yd), 6 s: the decoy's circle is the only safe
            // ground. 35 s, Gandling GD4 heroic: three 4 yd lanes from points 20 yd off the group, each through a
            // different non-tank and on 40 yd (2 s).
            { "scholomance-shapes", "chained fuse rings, offset barrage, Foresight chain, Ossuary rings out and in, "
                "decoy sigil grid, portal lanes", 40000, false, {
                    { 0, "circle 6 2000 at=member pick=nontank away=3 school=nature pct=40 weak=1 tag=1" },
                    { 0, "circle 6 1000 at=prev away=4 after=resolve school=nature pct=40 weak=1 tag=2" },
                    { 0, "circle 6 1000 at=prev away=4 after=resolve school=nature pct=40 weak=1 tag=2" },
                    { 5000, "circle 4 1500 at=each away=2 school=shadow pct=30 tag=3" },
                    { 1000, "circle 4 1500 at=each away=2 after=with school=shadow pct=30 tag=3" },
                    { 1000, "circle 4 1500 at=each away=2 after=with school=shadow pct=30 tag=3" },
                    { 10000, "circle 4 1500 at=member pick=any school=shadow pct=40 weak=1 tag=4" },
                    { 0, "circle 4 1500 at=predict pick=same lead=1500 after=resolve school=shadow pct=40 weak=1 "
                        "tag=5" },
                    { 0, "circle 4 1500 at=predict pick=same lead=1500 after=resolve school=shadow pct=40 weak=1 "
                        "tag=5" },
                    { 0, "circle 4 1500 at=predict pick=same lead=1500 after=resolve school=shadow pct=40 weak=1 "
                        "tag=5" },
                    { 0, "circle 4 1500 at=predict pick=same lead=1500 after=resolve school=shadow pct=40 weak=1 "
                        "tag=5" },
                    { 19000, "circle 6 2000 anchor=target pct=50 tag=6" },
                    { 0, "donut 12 1500 inner=6 anchor=target after=resolve pct=50 tag=7" },
                    { 0, "donut 18 1500 inner=12.06 anchor=target after=resolve pct=50 tag=7" },
                    { 0, "donut 18 1000 inner=12.06 anchor=target after=resolve pct=50 tag=8" },
                    { 0, "donut 12 1000 inner=6 anchor=target after=resolve pct=50 tag=8" },
                    { 0, "circle 6 1000 anchor=target after=resolve pct=50 tag=8" },
                    { 28000, "circle 8 6000 at=group grid=3 apart=11 decoys=1 school=shadow pct=60 tag=9" },
                    { 0, "donut 68 6000 inner=17 at=group after=with school=shadow pct=60 tag=9" },
                    { 35000, "line 40 2000 width=4 at=group away=20 to=member pick=nontank fix=1 school=arcane "
                        "pct=40 weak=1 tag=10" },
                    { 0, "line 40 2000 width=4 at=group away=20 to=member pick=other fix=1 after=with "
                        "school=arcane pct=40 weak=1 tag=10" },
                    { 0, "line 40 2000 width=4 at=group away=20 to=member pick=other fix=1 after=with "
                        "school=arcane pct=40 weak=1 tag=10" },
                } },        // three cycles a run (120 s): the sweep runs scholomance-shapes:3
            // Stratholme build §3.9: the chained follow-ups and the hidden Barrage, heroic numbers (pct= in the
            // percent mode). 0 s, The Unforgiven UF1 heroic: a 30 x 10 rank across the frame through the owner (3 s),
            // then the crossing rank on the other axis through the same center, 1.5 s after the first passes. 7 s,
            // Malor MZ7 + MZ8 heroic: circle 8 on the owner, tank included (2 s), then a donut 8 / 14 (1.5 s): out,
            // then back in. 13 s, Baroness BA4 + BA5 heroic: circle 12 on her (3 s), then the echo beyond 8 yd (2 s;
            // kEchoOuter). 20 s, Balnazzar BZ1 heroic: a 60 degree / 30 yd cone from him aimed at a non-tank and
            // tracking it (2.5 s), that player exempt, and a second at another non-tank 1.5 s after the first starts.
            // 25 s, Willey WY9 heroic: five half-hall lines with no marker, all registered at the call, resolving 3 s
            // later and then 1.5 s apart, 3 on one half and 2 on the other (even cycles L R L L R, odd R L R R L).
            // MZ7's 1 s stun and BA4/BA5's Terrified are left out (the bar counts hits; no drill rider stuns).
            { "stratholme-followups", "crossing ranks, smash then aftershock, wail then echo, two tracking cones, the "
                "hidden Barrage", 40000, false, {
                    { 0, "lanes 10 3000 count=1 axis=x frame=30x10 school=shadow pct=45 weak=1 tag=1" },
                    { 0, "lanes 10 1500 count=1 axis=y frame=10x30 after=resolve school=shadow pct=45 weak=1 tag=2" },
                    { 7000, "circle 8 2000 anchor=target pct=30 weak=1 tag=3" },
                    { 0, "donut 14 1500 inner=8 anchor=target after=resolve pct=30 weak=1 tag=4" },
                    { 13000, "circle 12 3000 anchor=target school=shadow pct=50 tag=5" },
                    { 0, "donut 32 2000 inner=8 anchor=target after=resolve school=shadow pct=40 tag=6" },
                    { 20000, "cone 30 2500 angle=60 anchor=target to=member pick=nontank exempt=target school=shadow "
                        "pct=40 tag=7" },
                    { 1500, "cone 30 2500 angle=60 anchor=target to=member pick=other exempt=target after=with "
                        "school=shadow pct=40 tag=8" },
                    { 25000, "line 58 3000 width=10 shift=-29:5 tele=0 cycle=even school=fire pct=30 weak=1 tag=9" },
                    { 0, "line 58 4500 width=10 shift=-29:-5 tele=0 after=with cycle=even school=fire pct=30 weak=1 "
                        "tag=9" },
                    { 0, "line 58 6000 width=10 shift=-29:5 tele=0 after=with cycle=even school=fire pct=30 weak=1 "
                        "tag=9" },
                    { 0, "line 58 7500 width=10 shift=-29:5 tele=0 after=with cycle=even school=fire pct=30 weak=1 "
                        "tag=9" },
                    { 0, "line 58 9000 width=10 shift=-29:-5 tele=0 after=with cycle=even school=fire pct=30 weak=1 "
                        "tag=9" },
                    { 25000, "line 58 3000 width=10 shift=-29:-5 tele=0 cycle=odd school=fire pct=30 weak=1 tag=9" },
                    { 0, "line 58 4500 width=10 shift=-29:5 tele=0 after=with cycle=odd school=fire pct=30 weak=1 "
                        "tag=9" },
                    { 0, "line 58 6000 width=10 shift=-29:-5 tele=0 after=with cycle=odd school=fire pct=30 weak=1 "
                        "tag=9" },
                    { 0, "line 58 7500 width=10 shift=-29:-5 tele=0 after=with cycle=odd school=fire pct=30 weak=1 "
                        "tag=9" },
                    { 0, "line 58 9000 width=10 shift=-29:5 tele=0 after=with cycle=odd school=fire pct=30 weak=1 "
                        "tag=9" },
                } },        // three cycles a run (120 s): the sweep runs stratholme-followups:3
            // Stratholme build X2: the drop spot rule (heroic numbers, pct= in the percent mode). 0 s: two old webs
            // (Nerub'enkan NE3), 5 yd persistent circles 10 yd off the group. 2 s, NE1: a 5 yd glob on a non-tank
            // (3 s), carried onto an old web (it may step in 0.8 s before the landing), else to the room's edge away
            // from the group. 10 s, the Baron BR3: a 6 yd Death and Decay on a non-tank (3 s) to the edge, away from
            // the group. 16 s, the Ultimatum HM3 (S15's rule: Edge + Away + avoid the bone piles, the old webs
            // standing in for them): a 6 yd lane from the creature to a non-tank (3 s), the target exempt, missing
            // everyone else. 22 s, Galford GF1: a 4 yd Holy Fire on a non-tank (6 s), behind the group (your
            // facing is its travel) and at the edge. The room is a 25 yd circle around the creature.
            { "stratholme-drops", "drop spots: a glob onto an old web, rot to the edge, a lane missing the group, "
                "holy fire behind it", 30000, false, {
                    { 0, "circle 5 1000 at=group away=10 persist=24000 tick=1000 school=nature pct=2 tag=1" },
                    { 0, "circle 5 1000 at=group away=10 persist=24000 tick=1000 after=with school=nature pct=2 "
                        "tag=1" },
                    { 2000, "circle 5 3000 anchor=member pick=nontank drop=prefer,edge,away room=25 dwell=800 "
                        "school=nature pct=30 weak=1 tag=2" },
                    { 10000, "circle 6 3000 anchor=member pick=nontank drop=edge,away room=25 school=shadow pct=25 "
                        "weak=1 tag=3" },
                    { 16000, "line 30 3000 width=6 anchor=target to=member pick=nontank exempt=target "
                        "drop=edge,away,avoid room=25 pct=50 weak=1 tag=4" },
                    { 22000, "circle 4 6000 anchor=member pick=nontank drop=behind,edge room=25 school=holy pct=30 "
                        "weak=1 tag=5" },
                }, 4 },     // four cycles a run (120 s): the sweep runs stratholme-drops:4
            // Stratholme build X3: the Jump kind (heroic numbers, pct= in the percent mode). 0 s, Timmy TC8: Jump Rope,
            // three 2 s skips back to back over the square (a 40 yd circle on the creature), each trip 25% plus
            // Weakened. 3.5 s, heroic Freeze Tag beside it (TC2): a 6 yd RunOut on a non-tank (2 s) that resolves in
            // the third skip's hop. 10 s, the heroic fallback Double Dutch: five skips of 2, 2, 1.5, 1.5, 1.5 s.
            { "stratholme-jump", "jump rope: three 2 s skips with a freeze tag beside them, then double dutch",
                20000, false, {
                    { 0, "jump 40 2000 anchor=target pct=25 weak=1 tag=1" },
                    { 2000, "jump 40 2000 anchor=target pct=25 weak=1 tag=1" },
                    { 3500, "runout 6 2000 at=member pick=nontank pct=30 weak=1 tag=2" },
                    { 4000, "jump 40 2000 anchor=target pct=25 weak=1 tag=1" },
                    { 10000, "jump 40 2000 anchor=target pct=25 weak=1 tag=3" },
                    { 12000, "jump 40 2000 anchor=target pct=25 weak=1 tag=3" },
                    { 14000, "jump 40 1500 anchor=target pct=25 weak=1 tag=4" },
                    { 15500, "jump 40 1500 anchor=target pct=25 weak=1 tag=4" },
                    { 17000, "jump 40 1500 anchor=target pct=25 weak=1 tag=4" },
                }, 6 },     // six cycles a run (120 s): the sweep runs stratholme-jump:6
            // Blackrock Spire build §3.9 (WP-G1, wave 1 rows 0-12), heroic numbers (pct= in the percent mode). 0 s,
            // Omokk OM2 heroic one-two: a 5 x 30 yd line from the creature toward a non-tank's spot, frozen there (2.5
            // s), then a second toward where that player stands as the first lands (1.5 s). 6 s, Gizrul and Halycon HG2
            // + HG3 heroic: two charges 1 s apart, 5 x 30 yd lines from the creature through the farthest non-tanks
            // (min=15, else the farthest; reach=1 so a farther one is still reached), 3 s, and an 8 yd pounce at each
            // line's far end, shown with its line (as B7 builds HG3) and landing 1.5 s after its charge: 4.5 s (at=last
            // right after its own charge, ties run in list order). 12 s, the axe chains (WT11 hard mode, VO11 heroic):
            // a 2 yd chain from the creature to the spot of each of two non-tanks (their stuck axes), 8 s (2 s, then 6
            // s with a 10% crossing every 2 s); as they end, a 1.5 s warning and a line along each chain: even cycles
            // the haul (WT11: 3 yd, 35%), odd cycles War Cry's recall (VO12 -> VO7: 4 yd, the creature's 5 yd excluded,
            // 40%). OM2's knockback, HG2's Rabies and VO12's Berserking are left out (the bar counts hits).
            // WP-G2 (wave 2 rows 18-30, one per cycle from 22 s: "Blackrock Spire follow-ups, wave 2" above). Cycles
            // 0/3, Emberseer EM2 + EM3 heroic at 16 stacks: ring 1 shown 5 s ahead from the nova's edge, the nova 2.5 s
            // in, rings 2-4 1.5 s apart from the apex at the nova (at=prev), each turned 60 degrees and carrying the
            // next turn; the hall as inside=29. Cycles 1/3, Rend RG6 heroic: two 68 yd walls 3 s apart over 86 yd, the
            // gaps in the 40 yd strip on opposite sides within 20 yd; the room as inside=20. Cycles 2/3, Drakkisath
            // DR17 + DR18: the Call (a spot on a 7 yd ring for everyone), then a non-tank's Challenge: the lethal ring
            // 11-13 yd and six blows 2 s apart (1.5 s each), all eligible to the challenger only and the blows
            // inside=10.
            { "blackrock-spire-followups", "one-two punch, two charges and their pounces, axe chains then the haul "
                "or the recall; then by turns Emberseer's rings, Rend's two walls, Drakkisath's Challenge", 40000,
                false, {
                    { 0, "line 30 2500 width=5 anchor=target to=member pick=nontank fix=1 pct=50 tag=1" },
                    { 0, "line 30 1500 width=5 anchor=target to=member pick=same fix=1 after=resolve pct=50 tag=2" },
                    { 6000, "line 30 3000 width=5 anchor=target to=member pick=nontank min=15 reach=1 pct=40 weak=1 "
                        "tag=3" },
                    { 6000, "circle 8 4500 at=last pct=30 weak=1 tag=4" },
                    { 7000, "line 30 3000 width=5 anchor=target to=member pick=other min=15 reach=1 pct=40 weak=1 "
                        "tag=3" },
                    { 7000, "circle 8 4500 at=last pct=30 weak=1 tag=4" },
                    { 12000, "line 5 2000 width=2 anchor=target to=member pick=nontank reach=1 persist=6000 "
                        "tick=2000 pct=10 weak=1 tag=5" },
                    { 12000, "line 5 2000 width=2 anchor=target to=member pick=other reach=1 persist=6000 tick=2000 "
                        "pct=10 weak=1 tag=5" },
                    { 20000, "line 5 1500 width=3 anchor=target to=last:1 reach=1 cycle=even pct=35 weak=1 tag=6" },
                    { 20000, "line 5 1500 width=3 anchor=target to=last reach=1 cycle=even pct=35 weak=1 tag=6" },
                    { 20000, "line 5 1500 width=4 exclude=5 anchor=target to=last:1 reach=1 cycle=odd pct=40 weak=1 "
                        "tag=7" },
                    { 20000, "line 5 1500 width=4 exclude=5 anchor=target to=last reach=1 cycle=odd pct=40 weak=1 "
                        "tag=7" },
                    // row 18: Emberseer (cycle=0/3)
                    { 22000, "conefront 45 5000 slice=45 start=11.8 next=60 park=1 until=23.8 inside=29 school=fire "
                        "pct=49.2 weak=1 cycle=0/3 tag=9" },
                    { 24500, "circle 11.8 2500 inside=29 school=fire pct=49.2 weak=1 cycle=0/3 tag=8" },
                    { 27000, "conefront 45 1500 at=prev slice=45 turn=60 start=0 next=60 park=1 until=23.8 "
                        "inside=29 school=fire pct=49.2 weak=1 cycle=0/3 tag=10" },
                    { 28500, "conefront 45 1500 at=prev slice=45 turn=60 start=0 next=60 park=1 until=23.8 "
                        "inside=29 school=fire pct=49.2 weak=1 cycle=0/3 tag=10" },
                    { 30000, "conefront 45 1500 at=prev slice=45 turn=60 start=0 park=1 until=23.8 inside=29 "
                        "school=fire pct=49.2 weak=1 cycle=0/3 tag=10" },
                    // row 24: Rend (cycle=1/3)
                    { 22000, "linefront 86 3000 width=68 gap=8 edge=15 inside=20 school=frost pct=25 weak=1 "
                        "cycle=1/3 tag=11" },
                    { 22000, "linefront 86 6000 width=68 gap=8 edge=15 side=other apart=20 inside=20 school=frost "
                        "pct=25 weak=1 cycle=1/3 tag=12" },
                    // row 30: Drakkisath's Challenge (cycle=2/3)
                    { 22000, "spot 2 4000 ring=7 cycle=2/3 tag=13" },
                    { 26000, "donut 13 1500 inner=11 anchor=target only=member pick=nontank lethal=1 persist=10500 "
                        "tick=250 pct=50 weak=1 cycle=2/3 tag=14" },
                    { 26000, "circle 8 1500 anchor=target only=member pick=same inside=10 pct=40 weak=1 cycle=2/3 "
                        "tag=19" },
                    { 28000, "donut 12 1500 inner=4 anchor=target only=member pick=same inside=10 school=fire "
                        "pct=40 weak=1 cycle=2/3 tag=18" },
                    { 30000, "cone 10 1500 angle=180 anchor=target to=member pick=same fix=1 only=member inside=10 "
                        "pct=40 weak=1 cycle=2/3 tag=15" },
                    { 32000, "circle 4 1500 at=member pick=same only=member inside=10 school=fire pct=30 weak=1 "
                        "cycle=2/3 tag=17" },
                    { 32000, "circle 4 1500 at=member pick=same shift=0:5 only=member inside=10 school=fire pct=30 "
                        "weak=1 cycle=2/3 tag=17" },
                    { 32000, "circle 4 1500 at=member pick=same shift=0:-5 only=member inside=10 school=fire pct=30 "
                        "weak=1 cycle=2/3 tag=17" },
                    { 34000, "cone 15 1500 angle=60 anchor=target to=member pick=same fix=1 only=member inside=10 "
                        "pct=50 cycle=2/3 tag=16" },
                    { 36000, "donut 12 1500 inner=4 anchor=target only=member pick=same inside=10 school=fire "
                        "pct=40 weak=1 cycle=2/3 tag=18" },
                }, 3 },     // three cycles a run (120 s): the sweep runs blackrock-spire-followups:3

            // Blackrock Spire WP-PS (the `pass` descriptor drill): the Rookery's egg clutches as 40 small persistent
            // circles, 2 yd, scattered within 25 yd of the selected unit, laid 50 ms apart and kept 16 s; three 20 s
            // cycles lay them again elsewhere. pct=0 (flat 1 in the dummy's flat mode): no harm, but each tick inside
            // is a ledger hit on tag 60 (DrillPresets::kPassEggTag), so the sweep counts egg contacts. No marker.
            { "pass-eggs", "40 egg circles (2 yd, within 25 yd), 16 s, laid again every 20 s: the pass drill's eggs",
                20000, false, {
                    { 0, "circle 2 500 pct=0 persist=16000 tick=1000 repeat=40 every=50 scatter=25 tele=0 tag=60" },
                } },

            // Blackrock Spire build WP-RF: the rolling fronts (see kFrontTagNova above).
            { "rolling-fronts", "Emberseer's nova and rings, normal then heroic, then Rend's wall and its heroic "
                "second wall", 40000, false, {
                    // normal, 7 stacks
                    { 0, "conefront 45 5000 slice=60 start=9.1 next=45 park=1 until=21.1 school=fire pct=30 weak=1 "
                        "tag=2" },
                    { 2500, "circle 9.1 2500 school=fire pct=30 weak=1 tag=1" },
                    { 5000, "conefront 45 2000 at=prev slice=60 turn=45 start=0 park=1 until=21.1 school=fire pct=30 "
                        "weak=1 tag=3" },
                    // heroic, 14 stacks
                    { 12000, "conefront 45 5000 slice=45 start=11.2 next=60 park=1 until=23.2 school=fire pct=30 "
                        "weak=1 tag=5" },
                    { 14500, "circle 11.2 2500 school=fire pct=30 weak=1 tag=4" },
                    { 17000, "conefront 45 1500 at=prev slice=45 turn=60 start=0 next=60 park=1 until=23.2 "
                        "school=fire pct=30 weak=1 tag=6" },
                    { 18500, "conefront 45 1500 at=prev slice=45 turn=60 start=0 next=60 park=1 until=23.2 "
                        "school=fire pct=30 weak=1 tag=6" },
                    { 20000, "conefront 45 1500 at=prev slice=45 turn=60 start=0 park=1 until=23.2 school=fire "
                        "pct=30 weak=1 tag=6" },
                    // Rend
                    { 27500, "linefront 50 3000 width=30 gap=8 school=frost pct=25 weak=1 tag=7" },
                    { 27500, "linefront 50 6000 width=30 gap=8 side=other apart=20 school=frost pct=25 weak=1 tag=8" },
                }, 3 },     // three cycles a run (120 s): the sweep runs rolling-fronts:3

            // Blackrock Depths build §3.9 (WP-G1, wave 1): see kBrdTagRampage above. Cycle 0 (0/2): Roccor walks 30
            // yd out and back (Rampage lanes at the route ends, on each leg the tank's leash and tag 3's two: the
            // non-healers on him, the healers on the tank, ruling 2026-11-18), then
            // Grebmar's two Sic 'em leaps. Cycle 1 (1/2): Gorosh's Returning Axe, Ok'thor's Arcane Rupture normal and
            // heroic, Loregrain's three Fire Tides round a moving ward, Incendius's edge bands, crumble and Arm Sweeps.
            { "blackrock-depths-shapes", "a walking boss with leashes, hound leap lines, axe and its locked return, "
                "rupture donuts, tides round a moving ward, lethal edge bands and an arm sweep", 60000, false, {
                    // walk: Roccor RO1 + RO3 heroic
                    { 0, "line 20 2500 width=8 anchor=target cycle=0/2 pct=60 tag=1" },
                    { 2500, "walk 30 20000 cycle=0/2" },
                    { 2500, "leash 8 20000 role=tank cycle=0/2 tag=2" },
                    { 2500, "leash 40 20000 role=nonhealer cycle=0/2 tag=3" },
                    { 2500, "leash 40 20000 role=healer anchor=tank cycle=0/2 tag=3" },
                    { 22500, "line 20 2500 width=8 anchor=target offset=180 cycle=0/2 pct=60 tag=1" },
                    { 25000, "walk 30 20000 offset=180 cycle=0/2" },
                    { 25000, "leash 8 20000 role=tank cycle=0/2 tag=2" },
                    { 25000, "leash 40 20000 role=nonhealer cycle=0/2 tag=3" },
                    { 25000, "leash 40 20000 role=healer anchor=tank cycle=0/2 tag=3" },
                    // leap: Grebmar GR3 + GR4 heroic (the first on anyone, the second on a non-tank)
                    { 47000, "line 1 1500 width=3 away=10 to=member pick=any reach=1 pct=25 weak=1 cycle=0/2 tag=4" },
                    { 47000, "line 1 1500 width=3 away=10 to=member pick=same reach=1 pct=25 weak=1 cycle=0/2 tag=4" },
                    { 47000, "line 1 1500 width=3 away=10 to=member pick=same reach=1 pct=25 weak=1 cycle=0/2 tag=4" },
                    { 47000, "circle 5 1500 at=last pct=45 weak=1 cycle=0/2 tag=5" },
                    { 50000, "line 1 1500 width=3 away=10 to=member pick=nontank reach=1 pct=25 weak=1 cycle=0/2 "
                        "tag=4" },
                    { 50000, "line 1 1500 width=3 away=10 to=member pick=same reach=1 pct=25 weak=1 cycle=0/2 tag=4" },
                    { 50000, "line 1 1500 width=3 away=10 to=member pick=same reach=1 pct=25 weak=1 cycle=0/2 tag=4" },
                    { 50000, "circle 5 1500 at=last pct=45 weak=1 cycle=0/2 tag=5" },
                    // hide (WP-HK): Bael'Gar BG4 heroic, the Molten Furnace: three Boulders (display 3711) 10 yd round
                    // him, a 5 s cast, everyone he sees within 40 yd hit for 95%; the Boulders gone by 59.5 s
                    { 52500, "hide 40 5000 blockers=3 apart=10 linger=2000 school=fire pct=95 cycle=0/2 tag=16" },
                    // axe-return: Gorosh GO2 heroic
                    { 0, "circle 3 2000 at=member pick=any pct=25 weak=1 cycle=1/2 tag=6" },
                    { 2000, "line 1 4000 width=4 at=prev to=target reach=1 pct=0 cycle=1/2 tag=7" },
                    { 6000, "line 1 2000 width=4 at=prev to=target reach=1 pct=35 weak=1 cycle=1/2 tag=8" },
                    // rupture: Ok'thor OK1, normal then heroic
                    { 10000, "donut 45 3000 inner=8 anchor=target school=arcane pct=30 weak=1 cycle=1/2 tag=9" },
                    { 15000, "donut 45 3000 inner=6 anchor=target school=arcane pct=45 weak=1 cycle=1/2 tag=10" },
                    // ward: Loregrain LG1-LG3 heroic, the ward moved and its radius locked per tide
                    { 20000, "stack 5 4000 anchor=point shift=0:6 cycle=1/2 tag=11" },
                    { 20000, "donut 45 4000 inner=5 shift=0:6 school=fire pct=45 weak=1 cycle=1/2 tag=12" },
                    { 26000, "stack 3 4000 anchor=point shift=0:-4 cycle=1/2 tag=11" },
                    { 26000, "donut 45 4000 inner=3 shift=0:-4 school=fire pct=45 weak=1 cycle=1/2 tag=12" },
                    { 32000, "stack 4 4000 anchor=point shift=-5:0 cycle=1/2 tag=11" },
                    { 32000, "donut 45 4000 inner=4 shift=-5:0 school=fire pct=45 weak=1 cycle=1/2 tag=12" },
                    // bands: Incendius IN8 (4 yd, all four edges), IN13 (the next 2 yd of the long edges), IN11
                    { 38000, "line 48 5000 width=4 shift=-24:9 lethal=1 persist=15000 tick=500 school=fire pct=50 "
                        "weak=1 cycle=1/2 tag=13" },
                    { 38000, "line 48 5000 width=4 shift=-24:-9 lethal=1 persist=15000 tick=500 school=fire pct=50 "
                        "weak=1 cycle=1/2 tag=13" },
                    { 38000, "line 22 5000 width=4 offset=90 shift=-11:-22 lethal=1 persist=15000 tick=500 "
                        "school=fire pct=50 weak=1 cycle=1/2 tag=13" },
                    { 38000, "line 22 5000 width=4 offset=90 shift=-11:22 lethal=1 persist=15000 tick=500 "
                        "school=fire pct=50 weak=1 cycle=1/2 tag=13" },
                    { 44000, "line 48 5000 width=2 shift=-24:6 lethal=1 persist=9000 tick=500 school=fire pct=50 "
                        "weak=1 cycle=1/2 tag=14" },
                    { 44000, "line 48 5000 width=2 shift=-24:-6 lethal=1 persist=9000 tick=500 school=fire pct=50 "
                        "weak=1 cycle=1/2 tag=14" },
                    { 49000, "line 22 4000 width=10 offset=90 shift=-11:0 lethal=1 school=fire pct=30 cycle=1/2 "
                        "tag=15" },
                    { 54000, "line 22 4000 width=10 offset=90 shift=-11:12 lethal=1 school=fire pct=30 cycle=1/2 "
                        "tag=15" },
                }, 2 },     // two cycles a run (120 s): the sweep runs blackrock-depths-shapes:2

            // Blackrock Depths build §3.9 (WP-G2, wave 2): see kBrd2TagBeam above. Cycle 0 (0/2): the Emperor's
            // Nemesis beam from Moira's fixed spot to him, drifting out, drifting back, standing. Cycle 1 (1/2): the
            // Seven's Seething Cold, Argelmach's Livewire, Magmus's Crossfire (walked out), Flame Wall, Crossfire.
            { "blackrock-depths-shapes-2", "a beam soak to a drifting boss, an anchorless loner stack, a carrier "
                "ring and its burst, crossfire lanes at a walking boss's spot and a wall strip", 60000, false, {
                    // beam-block: the Emperor EM2 heroic (8 s channel: 3 s to step in, judged every second for 5 s)
                    { 0, "soak 2 3000 shape=line to=target shift=0:15 need=1 persist=5000 tick=1000 cycle=0/2 "
                        "tag=21" },
                    { 0, "walk 6 8000 cycle=0/2" },
                    { 20000, "soak 2 3000 shape=line to=target shift=0:15 need=1 persist=5000 tick=1000 cycle=0/2 "
                        "tag=21" },
                    { 20000, "walk 6 8000 offset=180 cycle=0/2" },
                    { 40000, "soak 2 3000 shape=line to=target shift=0:15 need=1 persist=5000 tick=1000 cycle=0/2 "
                        "tag=21" },
                    // loner: the Seven TS4 heroic, Seething Cold (3 s mood cast, 10 s)
                    { 0, "stack 6 3000 cluster=2 persist=10000 tick=1000 cycle=1/2 tag=22" },
                    // Livewire: Argelmach AR8 heroic (12 s on a non-tank, 4% every 2 s round it, the burst at the end)
                    { 15000, "circle 8 2000 anchor=member pick=nontank persist=10000 tick=2000 school=nature pct=4 "
                        "cycle=1/2 tag=23" },
                    { 26000, "circle 8 1000 anchor=member pick=same school=nature pct=30 cycle=1/2 tag=24" },
                    // crossfire: Magmus MG3 + MG4 heroic, the hall's axis along the run's facing through his start
                    { 30000, "line 39 3000 width=10 offset=90 at=target shift=-19.5:0 school=fire pct=45 weak=1 "
                        "cycle=1/2 tag=25" },
                    { 30000, "line 39 4000 width=10 offset=90 at=target shift=-19.5:0 persist=5000 tick=1000 "
                        "school=fire pct=15 cycle=1/2 tag=26" },
                    { 30000, "walk 12 3000 cycle=1/2" },
                    { 38000, "line 40 3000 width=12 at=target shift=-20:13.5 persist=8000 tick=1000 school=fire "
                        "pct=15 cycle=1/2 tag=27" },
                    { 42000, "line 39 3000 width=10 offset=90 at=target shift=-19.5:0 school=fire pct=45 weak=1 "
                        "cycle=1/2 tag=25" },
                    { 42000, "line 39 4000 width=10 offset=90 at=target shift=-19.5:0 persist=5000 tick=1000 "
                        "school=fire pct=15 cycle=1/2 tag=26" },
                    { 42000, "walk 12 3000 offset=180 cycle=1/2" },
                }, 2 },     // two cycles a run (120 s): the sweep runs blackrock-depths-shapes-2:2

            // Scarlet Monastery build §3.9 (WP-G1, wave 1): see kSmTagTorment above. Cycle 0 (0/3): Vishas's Torment
            // on the tank, then on a non-tank; Loksey's Mark twice. Cycle 1 (1/3): Ironspine's growing rot, eye pools
            // and Glares; Doan's Mark of Righteous Fire to the wall and its pool. Cycle 2 (2/3, F-1b): Thalnos's crypt,
            // Galvar's banner dragged off and two Flame Spikes carried to the edge.
            { "scarlet-monastery-shapes", "a beam soak to a player whose tank end backs off, a scent trail behind a "
                "carrier, a growing rot with pools under players, a gaze, marks carried to the wall, a tank-only "
                "banner dragged off, flame spikes carried to the edge", 60000, true, {
                    // torment: Vishas VI4 heroic, the 9.5 s channel (1.5 s to step in, 8 judged seconds; D1's
                    // kTormentWarnMs / kTormentMs) on the tank (he backs off 10 yd), then a non-tank
                    { 0, "soak 3 1500 shape=line anchor=target to=member pick=tank exempt=target need=4 persist=8000 "
                        "tick=1000 backoff=10 tanknear=4 cycle=0/3 tag=1" },
                    { 1500, "leash 8.5 8000 role=tank near=1 cycle=0/3 tag=2" },
                    { 10000, "soak 3 1500 shape=line anchor=target to=member pick=nontank exempt=target need=4 "
                        "persist=8000 tick=1000 backoff=10 tanknear=4 cycle=0/3 tag=1" },
                    // mark-trail: Loksey LK3 heroic, a non-tank carrier (D5's kScentTankExempt) scented 5 s, walking
                    // 22 yd off him (kScentFarYd), then nine markers; the others' circle (tag 4) and the carrier's
                    // (tag 5) lie on the same spot (at=prev)
                    { 20000, "marker 1 pick=nontank cycle=0/3" },
                    { 20000, "bait 22 5000 aura=205009:1 r=1 pct=0 cycle=0/3 tag=3" },
                    { 25000, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 25000, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 25500, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 25500, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 26000, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 26000, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 26500, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 26500, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 27000, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 27000, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 27500, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 27500, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 28000, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 28000, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 28500, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 28500, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 29000, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 29000, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 31000, "marker 0 cycle=0/3" },
                    { 32000, "marker 1 pick=nontank cycle=0/3" },
                    { 32000, "bait 22 5000 aura=205009:1 r=1 pct=0 cycle=0/3 tag=3" },
                    { 37000, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 37000, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 37500, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 37500, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 38000, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 38000, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 38500, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 38500, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 39000, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 39000, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 39500, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 39500, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 40000, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 40000, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 40500, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 40500, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 41000, "circle 5 1500 at=member pick=same scatter=2 aura=205009:0:0 pct=45 weak=1 "
                        "cycle=0/3 tag=4" },
                    { 41000, "circle 5 1500 at=prev aura=205009:1 pct=45 weak=1 cycle=0/3 tag=5" },
                    { 43000, "marker 0 cycle=0/3" },
                    // rot-grow: Ironspine IR1 heroic under him (7, 8, 9, 10 yd, each step warned 1 s ahead and live
                    // as the last one ends), IR5 two eye pools with each rot (the first on anyone; live they last 60 s
                    // and grow like his, so here they stay to the rot's end at 41.5 s, 7 yd: the drill has no growing
                    // pool on a member's spot), IR6 a Glare every 6 s; he chases the tank that drags him off the rot,
                    // and goes home before Doan
                    { 0, "circle 7 1500 persist=10000 tick=1000 school=nature pct=8 cycle=1/3 tag=7" },
                    { 0, "circle 7 2000 at=member pick=any persist=39500 tick=1000 school=nature pct=8 cycle=1/3 "
                        "tag=8" },
                    { 0, "circle 7 2000 at=member pick=other persist=39500 tick=1000 school=nature pct=8 cycle=1/3 "
                        "tag=8" },
                    { 0, "chase 20 42000 home=1 cycle=1/3" },
                    { 4000, "gaze 40 2000 pct=0 cycle=1/3 tag=9" },
                    { 10000, "gaze 40 2000 pct=0 cycle=1/3 tag=9" },
                    { 10500, "circle 8 1000 persist=10000 tick=1000 school=nature pct=8 cycle=1/3 tag=7" },
                    { 14000, "circle 7 2000 at=member pick=any persist=25500 tick=1000 school=nature pct=8 cycle=1/3 "
                        "tag=8" },
                    { 14000, "circle 7 2000 at=member pick=other persist=25500 tick=1000 school=nature pct=8 "
                        "cycle=1/3 tag=8" },
                    { 16000, "gaze 40 2000 pct=0 cycle=1/3 tag=9" },
                    { 20500, "circle 9 1000 persist=10000 tick=1000 school=nature pct=8 cycle=1/3 tag=7" },
                    { 22000, "gaze 40 2000 pct=0 cycle=1/3 tag=9" },
                    { 28000, "gaze 40 2000 pct=0 cycle=1/3 tag=9" },
                    { 30500, "circle 10 1000 persist=10000 tick=1000 school=nature pct=8 cycle=1/3 tag=7" },
                    // wall-mark: Doan DN3 heroic, three non-tanks carry 8 yd to the edge of a 19 yd chamber; DN3a
                    // the 3 yd pool 1 s after the burst (the last carrier's: pick=same remembers one member)
                    { 43000, "circle 8 5000 anchor=member pick=nontank drop=edge,away room=19 school=fire pct=45 "
                        "weak=1 cycle=1/3 tag=10" },
                    { 43000, "circle 8 5000 anchor=member pick=other drop=edge,away room=19 school=fire pct=45 "
                        "weak=1 cycle=1/3 tag=10" },
                    { 43000, "circle 8 5000 anchor=member pick=other drop=edge,away room=19 school=fire pct=45 "
                        "weak=1 cycle=1/3 tag=10" },
                    { 48000, "circle 3 1000 at=member pick=same persist=10000 tick=1000 school=fire pct=10 "
                        "cycle=1/3 tag=11" },
                    // banner-drag: Galvar RH1 (Thalnos hard mode, from 5 s), a 10 yd zone only the tank sees at his
                    // feet for 30 s (3 s warned, 27 s judged: ThalnosHardModeLogic.h kBannerMs); he chases the tank off
                    // and goes home after it
                    { 5000, "circle 10 3000 at=target role=tank persist=27000 tick=1000 pct=0 cycle=2/3 tag=6" },
                    { 5000, "chase 20 30500 home=1 cycle=2/3" },
                    // spike-carry: Thalnos TH3 heroic (from 6 s, every 15 s), a random carrier (tank included) walks a
                    // 4 yd marker for the 2 s bar to the crypt's edge away from the group, off the live pools (the
                    // crypt, 18 x 33 yd: a 14 yd circle of its area); it lands where the carrier stands, the pillar
                    // 1.5 s later (45%, Weakened, the carrier included), then the 4 yd pool 30 s
                    { 6000, "circle 4 2000 anchor=member pick=any drop=edge,away,avoid room=14 pct=0 cycle=2/3 "
                        "tag=12" },
                    { 8000, "circle 4 1500 at=member pick=same school=fire pct=45 weak=1 cycle=2/3 tag=13" },
                    { 8000, "circle 4 2500 at=prev persist=29000 tick=1000 school=fire pct=8 cycle=2/3 tag=14" },
                    { 21000, "circle 4 2000 anchor=member pick=any drop=edge,away,avoid room=14 pct=0 cycle=2/3 "
                        "tag=12" },
                    { 23000, "circle 4 1500 at=member pick=same school=fire pct=45 weak=1 cycle=2/3 tag=13" },
                    { 23000, "circle 4 2500 at=prev persist=29000 tick=1000 school=fire pct=8 cycle=2/3 tag=14" },
                }, 3 },     // three cycles a run (180 s): the sweep runs scarlet-monastery-shapes:3

            // Scarlet Monastery build §3.9 (WP-G2, wave 2): see kSm2TagFurrow above. Cycle 0 (0/3): Thorne's Sow and
            // Prune with the facing hold, Holy Water and the blessed ground, the missed Bless the Harvest; Halden's
            // Close Ranks. Cycle 1 (1/3): Whitemane's Vigil Flame loop; Crell's Blade window. Cycle 2 (2/3): Herod's
            // lap; Ravager's Beacon with the hard mode lap at its flip.
            { "scarlet-monastery-shapes-2", "Spots in a held frontal, a tank-only ground under the boss, a 1-body soak "
                "passed round with the tank last, a lasting spread with a dash chain, a band on a stepping boss, a "
                "ledge lap with lanes into the pit, two turning beams", 60000, false, {
                    // sow-prune: Thorne GD1a heroic, the Prune every 10 s (120 deg, 8 yd, 2 s wind-up; the tank exempt)
                    { 0, "frontal 8 2000 angle=120 pct=45 weak=1 cycle=0/3 tag=22" },
                    // GD1 heroic: three carriers (non-tanks here, so the hold's judge reads the tank alone), the furrow
                    // 5 yd ahead of him; the tank holds his facing from the Sow to the Prune that cuts and stays on its
                    // spot (anchor=self)
                    { 3000, "marker 1 pick=nontank cycle=0/3" },
                    { 3000, "marker 1 pick=nontank cycle=0/3" },
                    { 3000, "marker 1 pick=nontank cycle=0/3" },
                    { 3000, "spot 3 5000 ring=0 at=target along=target shift=5:0 aura=205009:1 cycle=0/3 tag=21" },
                    { 3000, "holdfacing 9000 cycle=0/3" },
                    { 3000, "leash 4 9000 role=tank anchor=self cycle=0/3 tag=23" },
                    { 10000, "frontal 8 2000 angle=120 pct=45 weak=1 cycle=0/3 tag=22" },
                    { 12000, "marker 0 cycle=0/3" },
                    // blessed-ground: GD2 heroic Holy Water, then GD2a, 8 yd under him for 8 s that only the tank sees;
                    // he chases the tank off it (and stays dragged through the second Sow, home at 40 s)
                    { 14000, "circle 12 2500 at=target school=holy pct=45 weak=1 cycle=0/3 tag=24" },
                    { 16500, "circle 8 500 at=target role=tank persist=8000 tick=1000 pct=0 cycle=0/3 tag=25" },
                    { 16500, "chase 20 23500 home=1 cycle=0/3" },
                    { 20000, "frontal 8 2000 angle=120 pct=45 weak=1 cycle=0/3 tag=22" },
                    // the second Sow, carriers tank included; Bless the Harvest missed: every seed blooms as it is
                    // planted (6 yd, 2 s), the step-out ring inside the hold
                    { 23000, "marker 1 cycle=0/3" },
                    { 23000, "marker 1 cycle=0/3" },
                    { 23000, "marker 1 cycle=0/3" },
                    { 23000, "spot 3 5000 ring=0 at=target along=target shift=5:0 aura=205009:1 cycle=0/3 tag=21" },
                    { 23000, "holdfacing 9000 cycle=0/3" },
                    { 28000, "circle 6 2000 at=target along=target shift=5:0 school=holy pct=30 weak=1 cycle=0/3 "
                        "tag=26" },
                    { 30000, "frontal 8 2000 angle=120 pct=45 weak=1 cycle=0/3 tag=22" },
                    { 32000, "marker 0 cycle=0/3" },
                    // close-ranks: Halden MA4 heroic, the 5-11 yd band round him (§7 #30 A) for 8 s while he steps 4 yd
                    // out and back
                    { 42000, "donut 11 2500 inner=5 anchor=target persist=8000 tick=1000 pct=12 cycle=0/3 tag=27" },
                    { 46500, "walk 4 2000 cycle=0/3" },
                    { 49500, "walk 4 2000 offset=180 cycle=0/3" },
                    // vigil-flame: Whitemane WM5 heroic, a flare every 10 s on the next non-tank carrier (Marked
                    // (drill) 205009 is the flame and Seared both); 1-body soak, the unmarked eligible, the tank last;
                    // the Seared kept off it. Live (D14 RegisterFlameSoak) registers each soak at the pass with the
                    // whole time to the next flare as its warning, 10 s; the drill's 9.5 s is that lead less the
                    // kFlameRegisterLagMs that keeps each mark after the last resolve, so it is 0.5 s harsher. Live
                    // Seared lasts 19 s (kSearedMs, fades before the second flare after); the drill's Marked never
                    // fades in the cycle, also harsher (G2 Q2). A short lead is where Ordering::roleLast matters
                    // (STAGE-R-1-FW M-1)
                    { 500, "marker 1 pick=nontank cycle=1/3" },
                    { 500, "soak 6 9500 anchor=member pick=same need=1 aura=205009:0:0 last=tank cycle=1/3 tag=29" },
                    { 500, "circle 6 9500 anchor=member pick=same aura=205009:1 pct=0 cycle=1/3 tag=30" },
                    { 500, "leash 8 29500 role=tank cycle=1/3 tag=31" },
                    { 10500, "marker 1 pick=nontank cycle=1/3" },
                    { 10500, "soak 6 9500 anchor=member pick=same need=1 aura=205009:0:0 last=tank cycle=1/3 tag=29" },
                    { 10500, "circle 6 9500 anchor=member pick=same aura=205009:1 pct=0 cycle=1/3 tag=30" },
                    { 20500, "marker 1 pick=nontank cycle=1/3" },
                    { 20500, "soak 6 9500 anchor=member pick=same need=1 aura=205009:0:0 last=tank cycle=1/3 tag=29" },
                    { 20500, "circle 6 9500 anchor=member pick=same aura=205009:1 pct=0 cycle=1/3 tag=30" },
                    { 30500, "marker 1 pick=nontank cycle=1/3" },
                    { 30500, "soak 6 9500 anchor=member pick=same need=1 aura=205009:0:0 last=tank cycle=1/3 tag=29" },
                    { 30500, "circle 6 9500 anchor=member pick=same aura=205009:1 pct=0 cycle=1/3 tag=30" },
                    { 40500, "marker 0 cycle=1/3" },
                    // blade-window: Crell AR7 heroic, the Blade's lasting 6 yd spread (F15) and a five-dash chain at 2
                    // s and 12 s
                    { 41000, "spread 6 2000 persist=13000 tick=1000 pct=0 cycle=1/3 tag=32" },
                    { 43000, "line 5 2000 width=3 anchor=target to=member pick=nontank reach=1 past=5 pct=45 weak=1 "
                        "cycle=1/3 tag=33" },
                    { 0, "line 5 1000 width=3 at=last to=member pick=other reach=1 past=5 after=resolve pct=30 weak=1 "
                        "cycle=1/3 tag=34" },
                    { 0, "line 5 1000 width=3 at=last to=member pick=other reach=1 past=5 after=resolve pct=30 weak=1 "
                        "cycle=1/3 tag=34" },
                    { 0, "line 5 1000 width=3 at=last to=member pick=other reach=1 past=5 after=resolve pct=30 weak=1 "
                        "cycle=1/3 tag=34" },
                    { 0, "line 5 1000 width=3 at=last to=member pick=tank reach=1 past=5 after=resolve pct=30 weak=1 "
                        "cycle=1/3 tag=34" },
                    { 53000, "line 5 2000 width=3 anchor=target to=member pick=nontank reach=1 past=5 pct=45 weak=1 "
                        "cycle=1/3 tag=33" },
                    { 0, "line 5 1000 width=3 at=last to=member pick=other reach=1 past=5 after=resolve pct=30 weak=1 "
                        "cycle=1/3 tag=34" },
                    { 0, "line 5 1000 width=3 at=last to=member pick=other reach=1 past=5 after=resolve pct=30 weak=1 "
                        "cycle=1/3 tag=34" },
                    { 0, "line 5 1000 width=3 at=last to=member pick=other reach=1 past=5 after=resolve pct=30 weak=1 "
                        "cycle=1/3 tag=34" },
                    { 0, "line 5 1000 width=3 at=last to=member pick=tank reach=1 past=5 after=resolve pct=30 weak=1 "
                        "cycle=1/3 tag=34" },
                    // herod-lap: Herod HB1 heroic round the pit's center (3 yd past him): the ledge band closed from
                    // the yell to the lap's end, the lane out, six blades thrown from the lap (12 yd/s on r 19.3) at
                    // players, the lane home
                    { 0, "donut 40 2500 inner=13 shift=3:0 persist=10100 tick=1000 pct=15 once=1 cycle=2/3 tag=36" },
                    { 0, "line 19.3 2500 width=10 offset=90 shift=0:-3 pct=45 weak=1 cycle=2/3 tag=35" },
                    { 2500, "line 32 1500 width=4 shift=3:19.3 to=member pick=any fix=1 school=holy pct=45 weak=1 "
                        "cycle=2/3 tag=37" },
                    { 4000, "line 32 1500 width=4 shift=-12.5:11.5 to=member pick=any fix=1 school=holy pct=45 weak=1 "
                        "cycle=2/3 tag=37" },
                    { 5500, "line 32 1500 width=4 shift=-15.47:-5.6 to=member pick=any fix=1 school=holy pct=45 weak=1 "
                        "cycle=2/3 tag=37" },
                    { 7000, "line 32 1500 width=4 shift=-3.5:-18.17 to=member pick=any fix=1 school=holy pct=45 weak=1 "
                        "cycle=2/3 tag=37" },
                    { 8500, "line 32 1500 width=4 shift=13.72:-16.05 to=member pick=any fix=1 school=holy pct=45 "
                        "weak=1 cycle=2/3 tag=37" },
                    { 10000, "line 32 1500 width=4 shift=22.28:-0.95 to=member pick=any fix=1 school=holy pct=45 "
                        "weak=1 cycle=2/3 tag=37" },
                    { 12600, "line 19.3 1500 width=10 offset=270 shift=-19.3:3 pct=45 weak=1 cycle=2/3 tag=38" },
                    // turning-beams: Herod HM3, two 4 x 10 yd beams round the planted axe, 30 deg every 1.5 s (each
                    // step marked 1.5 s ahead), 12 steps counterclockwise, the hard mode lap at the flip (14 yd/s, two
                    // blades at once, 60%), 12 back; the tank walks him round the pit ahead of them (chase)
                    { 16000, "line 10 1500 width=4 offset=90 shift=3:-3 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 16000, "line 10 1500 width=4 offset=270 shift=3:3 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 16000, "chase 12 36000 home=1 cycle=2/3" },
                    { 17500, "line 10 1500 width=4 offset=120 shift=1.5:-2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 17500, "line 10 1500 width=4 offset=300 shift=4.5:2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 19000, "line 10 1500 width=4 offset=150 shift=0.4:-1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 19000, "line 10 1500 width=4 offset=330 shift=5.6:1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 20500, "line 10 1500 width=4 offset=180 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 20500, "line 10 1500 width=4 shift=6:0 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 22000, "line 10 1500 width=4 offset=210 shift=0.4:1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 22000, "line 10 1500 width=4 offset=30 shift=5.6:-1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 23500, "line 10 1500 width=4 offset=240 shift=1.5:2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 23500, "line 10 1500 width=4 offset=60 shift=4.5:-2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 25000, "line 10 1500 width=4 offset=270 shift=3:3 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 25000, "line 10 1500 width=4 offset=90 shift=3:-3 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 26500, "line 10 1500 width=4 offset=300 shift=4.5:2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 26500, "line 10 1500 width=4 offset=120 shift=1.5:-2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 28000, "line 10 1500 width=4 offset=330 shift=5.6:1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 28000, "line 10 1500 width=4 offset=150 shift=0.4:-1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 29500, "line 10 1500 width=4 shift=6:0 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 29500, "line 10 1500 width=4 offset=180 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 31000, "line 10 1500 width=4 offset=30 shift=5.6:-1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 31000, "line 10 1500 width=4 offset=210 shift=0.4:1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 32500, "line 10 1500 width=4 offset=60 shift=4.5:-2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 32500, "line 10 1500 width=4 offset=240 shift=1.5:2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 34000, "line 10 1500 width=4 offset=30 shift=5.6:-1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 34000, "line 10 1500 width=4 offset=210 shift=0.4:1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 34000, "donut 40 2500 inner=13 shift=3:0 persist=8700 tick=1000 pct=20 once=1 cycle=2/3 tag=36" },
                    { 34000, "line 19.3 2500 width=10 offset=270 shift=0:3 pct=60 weak=1 cycle=2/3 tag=35" },
                    { 35500, "line 10 1500 width=4 shift=6:0 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 35500, "line 10 1500 width=4 offset=180 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 36500, "line 32 1500 width=4 shift=3:-19.3 to=member pick=any fix=1 school=holy pct=60 weak=1 "
                        "cycle=2/3 tag=40" },
                    { 36500, "line 32 1500 width=4 shift=3:-19.3 to=member pick=other fix=1 school=holy pct=60 weak=1 "
                        "cycle=2/3 tag=40" },
                    { 37000, "line 10 1500 width=4 offset=330 shift=5.6:1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 37000, "line 10 1500 width=4 offset=150 shift=0.4:-1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 38000, "line 32 1500 width=4 shift=-14.09:-8.96 to=member pick=any fix=1 school=holy pct=60 "
                        "weak=1 cycle=2/3 tag=40" },
                    { 38000, "line 32 1500 width=4 shift=-14.09:-8.96 to=member pick=other fix=1 school=holy pct=60 "
                        "weak=1 cycle=2/3 tag=40" },
                    { 38500, "line 10 1500 width=4 offset=300 shift=4.5:2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 38500, "line 10 1500 width=4 offset=120 shift=1.5:-2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 39500, "line 32 1500 width=4 shift=-12.87:10.98 to=member pick=any fix=1 school=holy pct=60 "
                        "weak=1 cycle=2/3 tag=40" },
                    { 39500, "line 32 1500 width=4 shift=-12.87:10.98 to=member pick=other fix=1 school=holy pct=60 "
                        "weak=1 cycle=2/3 tag=40" },
                    { 40000, "line 10 1500 width=4 offset=270 shift=3:3 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 40000, "line 10 1500 width=4 offset=90 shift=3:-3 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 41000, "line 32 1500 width=4 shift=5.36:19.16 to=member pick=any fix=1 school=holy pct=60 weak=1 "
                        "cycle=2/3 tag=40" },
                    { 41000, "line 32 1500 width=4 shift=5.36:19.16 to=member pick=other fix=1 school=holy pct=60 "
                        "weak=1 cycle=2/3 tag=40" },
                    { 41500, "line 10 1500 width=4 offset=240 shift=1.5:2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 41500, "line 10 1500 width=4 offset=60 shift=4.5:-2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 42500, "line 32 1500 width=4 shift=21.06:6.8 to=member pick=any fix=1 school=holy pct=60 weak=1 "
                        "cycle=2/3 tag=40" },
                    { 42500, "line 32 1500 width=4 shift=21.06:6.8 to=member pick=other fix=1 school=holy pct=60 "
                        "weak=1 cycle=2/3 tag=40" },
                    { 43000, "line 10 1500 width=4 offset=210 shift=0.4:1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 43000, "line 10 1500 width=4 offset=30 shift=5.6:-1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 44500, "line 10 1500 width=4 offset=180 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 44500, "line 10 1500 width=4 shift=6:0 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 45200, "line 19.3 1500 width=10 offset=90 shift=-19.3:-3 pct=60 weak=1 cycle=2/3 tag=38" },
                    { 46000, "line 10 1500 width=4 offset=150 shift=0.4:-1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 46000, "line 10 1500 width=4 offset=330 shift=5.6:1.5 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 47500, "line 10 1500 width=4 offset=120 shift=1.5:-2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 47500, "line 10 1500 width=4 offset=300 shift=4.5:2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 49000, "line 10 1500 width=4 offset=90 shift=3:-3 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 49000, "line 10 1500 width=4 offset=270 shift=3:3 school=holy pct=45 weak=1 cycle=2/3 tag=39" },
                    { 50500, "line 10 1500 width=4 offset=60 shift=4.5:-2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                    { 50500, "line 10 1500 width=4 offset=240 shift=1.5:2.6 school=holy pct=45 weak=1 cycle=2/3 "
                        "tag=39" },
                }, 3 },     // three cycles a run (180 s): the sweep runs scarlet-monastery-shapes-2:3

            // Ragefire Chasm build §3.9 (WP-G1): see kRfcTagChase above.
            { "ragefire-chasm-shapes", "mounds hunting distinct players with eruptions, a chained beating, a spread on "
                "burning carriers, crawling wisps to catch, a kickable channel with must-play bars, spins after "
                "teleports, a ring with aimed spots, hides behind spikes", 60000, false, {
                    // borer-den: Oggleflint OG1 + OG2 heroic
                    { 0, "chase 8 33000 home=1 cycle=0/3" },
                    { 0, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=any inside=13:-8 pct=45 tankpct=45 "
                        "weak=1 cycle=0/3 tag=1" },
                    { 0, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=fresh inside=13:-8 pct=45 tankpct=45 "
                        "weak=1 cycle=0/3 tag=1" },
                    { 12000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=any inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=1" },
                    { 12000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=fresh inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=1" },
                    { 12000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=fresh inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=1" },
                    // a 2 s kick window (kick-lockout's short half) between two hunts
                    { 19000, "cast 205010 2000 kick=1 cycle=0/3 tag=21" },
                    { 24000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=any inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=1" },
                    { 24000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=fresh inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=1" },
                    { 24000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=fresh inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=1" },
                    { 24000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=fresh inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=1" },
                    // beating-chain: OG4 heroic, the unit landing on each blow, then home
                    { 36000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=any inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=4" },
                    { 36500, "circle 3 1500 at=member pick=fresh inside=13:-8 pct=35 tankpct=35 weak=1 cycle=0/3 "
                        "tag=3" },
                    { 38000, "blink 0 0 at=prev cycle=0/3" },
                    { 38000, "circle 3 1500 at=member pick=same inside=13:-8 pct=35 tankpct=35 weak=1 cycle=0/3 "
                        "tag=3" },
                    { 39500, "blink 0 0 at=prev cycle=0/3" },
                    { 39500, "circle 3 1500 at=member pick=same inside=13:-8 pct=35 tankpct=35 weak=1 cycle=0/3 "
                        "tag=3" },
                    { 41000, "blink 0 0 at=prev cycle=0/3" },
                    { 42000, "blink 0 0 home=1 cycle=0/3" },
                    // a 3 s kick window between two Beatings
                    { 43000, "cast 205012 3000 kick=1 cycle=0/3 tag=20" },
                    { 48000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=any inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=4" },
                    { 48000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=fresh inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=4" },
                    { 48500, "circle 3 1500 at=member pick=fresh inside=13:-8 pct=35 tankpct=35 weak=1 cycle=0/3 "
                        "tag=3" },
                    { 50000, "blink 0 0 at=prev cycle=0/3" },
                    { 50000, "circle 3 1500 at=member pick=same inside=13:-8 pct=35 tankpct=35 weak=1 cycle=0/3 "
                        "tag=3" },
                    { 51500, "blink 0 0 at=prev cycle=0/3" },
                    { 51500, "circle 3 1500 at=member pick=same inside=13:-8 pct=35 tankpct=35 weak=1 cycle=0/3 "
                        "tag=3" },
                    { 53000, "blink 0 0 at=prev cycle=0/3" },
                    { 54000, "blink 0 0 home=1 cycle=0/3" },
                    // five mounds, every member hunted (reported)
                    { 54000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=any inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=18" },
                    { 54000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=fresh inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=18" },
                    { 54000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=fresh inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=18" },
                    { 54000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=fresh inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=18" },
                    { 54000, "mound 3 500 persist=3500 speed=5 erupt=4 boom=2000 pick=fresh inside=13:-8 pct=45 "
                        "tankpct=45 weak=1 cycle=0/3 tag=18" },
                    // burn-spread (JR2 heroic), wisp-soak (JR5) and kick-lockout over each other, as Jergosh runs them
                    // (0.5 s after cycle 0's last eruption: no two rows in one instant)
                    { 500, "marker 1 pick=any cycle=1/3" },
                    { 500, "marker 1 pick=any cycle=1/3" },
                    { 4000, "spread 6 2500 self=205009:1 school=fire pct=45 tankpct=45 weak=1 cycle=1/3 tag=6" },
                    { 7000, "marker 0 cycle=1/3" },
                    { 8000, "crawl 2 25000 yd=50 bearing=90 off=4 speed=3 need=1 last=healer pick=any cycle=1/3 "
                        "tag=7" },
                    { 9000, "cast 36224 5000 kick=1 every=15000 cycle=1/3 tag=14" },
                    { 14000, "marker 1 pick=any cycle=1/3" },
                    { 14000, "marker 1 pick=any cycle=1/3" },
                    { 14000, "marker 1 pick=any cycle=1/3" },
                    { 14500, "cast 50097 2000 cycle=1/3 tag=16" },
                    { 17000, "cast 18088 2000 cycle=1/3 tag=15" },
                    { 18000, "spread 6 2500 self=205009:1 school=fire pct=45 tankpct=45 weak=1 cycle=1/3 tag=6" },
                    { 19250, "marker 0 one=1 cycle=1/3" },
                    { 21000, "marker 0 cycle=1/3" },
                    { 24000, "cast 36224 5000 kick=1 every=15000 cycle=1/3 tag=14" },
                    { 25000, "crawl 2 25000 yd=50 bearing=270 off=4 speed=3 need=1 last=healer pick=any cycle=1/3 "
                        "tag=7" },
                    { 28000, "marker 1 pick=any cycle=1/3" },
                    { 28000, "marker 1 pick=any cycle=1/3" },
                    { 28000, "marker 1 pick=any cycle=1/3" },
                    { 28000, "marker 1 pick=any cycle=1/3" },
                    { 28000, "marker 1 pick=any cycle=1/3" },
                    { 29500, "cast 50097 2000 cycle=1/3 tag=16" },
                    { 32000, "spread 6 2500 self=205009:1 school=fire pct=45 tankpct=45 weak=1 cycle=1/3 tag=6" },
                    { 32000, "cast 18088 2000 cycle=1/3 tag=15" },
                    { 35000, "marker 0 cycle=1/3" },
                    { 39000, "cast 36224 5000 kick=1 cycle=1/3 tag=14" },
                    { 44500, "cast 50097 2000 cycle=1/3 tag=16" },
                    { 47000, "cast 18088 2000 cycle=1/3 tag=15" },
                    // shadowstep-fans: Bazzalan BZ5, then BZ7 heroic, a landing every 2.5 s (0.5 s after a resolve)
                    { 50000, "circle 8 2000 anchor=target inside=15:-6 pct=35 tankpct=35 weak=1 cycle=1/3 tag=8" },
                    { 52500, "blink 2 0 pick=any cycle=1/3" },
                    { 52500, "circle 6 2000 anchor=target inside=15:-6 pct=45 tankpct=45 weak=1 cycle=1/3 tag=9" },
                    { 55000, "blink 2 0 pick=fresh cycle=1/3" },
                    { 55000, "circle 6 2000 anchor=target inside=15:-6 pct=45 tankpct=45 weak=1 cycle=1/3 tag=9" },
                    { 57500, "blink 2 0 pick=fresh cycle=1/3" },
                    { 57500, "circle 6 2000 anchor=target inside=15:-6 pct=45 tankpct=45 weak=1 cycle=1/3 tag=9" },
                    { 59750, "blink 0 0 home=1 cycle=1/3" },
                    // felstorm-spots: Taragaman TG3 + TG5 heroic
                    { 0, "circle 10 2000 anchor=target persist=5000 tick=1000 inside=18:0 pct=20 tankpct=20 cycle=2/3 "
                        "tag=10" },
                    { 2000, "circle 4 1500 at=member pick=fresh scatter=4 repeat=10 every=500 inside=18:0 school=fire "
                        "pct=35 tankpct=35 weak=1 cycle=2/3 tag=11" },
                    // spike-hide: TG6 with three spikes (normal's count), then heroic's opposite pair
                    { 11500, "hide 60 4000 blockers=3 of=4 apart=10 r=2 eye=3.05 display=5073 scale=2 linger=500 "
                        "school=fire pct=75 tankpct=75 cycle=2/3 tag=12" },
                    // a 2 s kick window, the group coming back from the spikes; the 3 s one below
                    { 17000, "cast 205010 2000 kick=1 cycle=2/3 tag=21" },
                    { 20000, "circle 10 2000 anchor=target persist=5000 tick=1000 inside=18:0 pct=20 tankpct=20 "
                        "cycle=2/3 tag=10" },
                    { 22000, "circle 4 1500 at=member pick=fresh scatter=4 repeat=10 every=500 inside=18:0 school=fire "
                        "pct=35 tankpct=35 weak=1 cycle=2/3 tag=11" },
                    { 31500, "hide 60 4000 blockers=2 apart=10 r=2 eye=3.05 display=5073 scale=2 linger=500 "
                        "school=fire pct=75 tankpct=75 cycle=2/3 tag=13" },
                    { 37000, "cast 205012 3000 kick=1 cycle=2/3 tag=20" },
                    { 40000, "circle 10 2000 anchor=target persist=5000 tick=1000 inside=18:0 pct=20 tankpct=20 "
                        "cycle=2/3 tag=10" },
                    { 42000, "circle 4 1500 at=member pick=fresh scatter=4 repeat=10 every=500 inside=18:0 school=fire "
                        "pct=35 tankpct=35 weak=1 cycle=2/3 tag=11" },
                    // globule-walkers, reported: TG9's bursts where the four die on their way in (the kill order itself
                    // is NearUnit on a TargetEntry, no row)
                    { 52000, "circle 6 1500 shift=12:0 school=shadow pct=30 weak=1 cycle=2/3 tag=17" },
                    { 53000, "circle 6 1500 shift=-12:0 school=shadow pct=30 weak=1 cycle=2/3 tag=17" },
                    { 54000, "circle 6 1500 shift=0:8 school=shadow pct=30 weak=1 cycle=2/3 tag=17" },
                    { 55000, "circle 6 1500 shift=0:-8 school=shadow pct=30 weak=1 cycle=2/3 tag=17" },
                }, 3 },     // three cycles a run (180 s): the sweep runs ragefire-chasm-shapes:3
        };
        return drills;
    }

    inline Def const* Find(std::string_view name)
    {
        for (Def const& d : All())
            if (name == d.name)
                return &d;
        return nullptr;
    }

    // ---- line helpers (the host test reads the lines with them) ---------------------------------------------------

    // The whitespace-separated tokens of a step line.
    inline std::vector<std::string_view> Tokens(std::string_view line)
    {
        std::vector<std::string_view> out;
        std::size_t i = 0;
        while (i < line.size())
        {
            while (i < line.size() && line[i] == ' ')
                ++i;
            std::size_t const start = i;
            while (i < line.size() && line[i] != ' ')
                ++i;
            if (i > start)
                out.push_back(line.substr(start, i - start));
        }
        return out;
    }

    // The value of `key=` in a step line, if present.
    inline std::optional<std::string_view> Value(std::string_view line, std::string_view key)
    {
        for (std::string_view const t : Tokens(line))
        {
            std::size_t const eq = t.find('=');
            if (eq != std::string_view::npos && t.substr(0, eq) == key)
                return t.substr(eq + 1);
        }
        return std::nullopt;
    }

    // A non-negative decimal number token (no sign, optional fraction); nullopt when it is not one.
    inline std::optional<float> Number(std::string_view s)
    {
        if (s.empty())
            return std::nullopt;
        float v = 0.f;
        float scale = 0.f;
        for (char const ch : s)
        {
            if (ch == '.' && scale == 0.f)
                scale = 1.f;
            else if (ch >= '0' && ch <= '9')
            {
                if (scale == 0.f)
                    v = v * 10.f + static_cast<float>(ch - '0');
                else
                {
                    scale /= 10.f;
                    v += scale * static_cast<float>(ch - '0');
                }
            }
            else
                return std::nullopt;
        }
        return v;
    }

    // How long a rolling front step rolls after its warning (ms): (end - start) / speed for a conefront, travel /
    // speed for a linefront (MechanicFront.h ConeParams, LineParams); 0 for any other step.
    inline float FrontRollMs(std::string_view line)
    {
        std::vector<std::string_view> const t = Tokens(line);
        if (t.size() < 2 || (t[0] != "conefront" && t[0] != "linefront"))
            return 0.f;
        float const size = Number(t[1]).value_or(0.f);
        auto key = [&line](std::string_view k, float def)
        {
            std::optional<std::string_view> v = Value(line, k);
            return v ? Number(*v).value_or(def) : def;
        };
        float const speed = key("speed", 8.f);
        float const start = t[0] == "conefront" ? key("start", 0.f) : 0.f;
        return speed > 0.f ? (size - start) / speed * 1000.f : 0.f;
    }

    // Does the runner add `dmg=` to this step? Every damage shape, and a bell soak (`soak ... bell=<n>`: its landing
    // circle carries the damage keys).
    inline bool DamageStep(std::string_view line)
    {
        std::vector<std::string_view> const t = Tokens(line);
        if (t.empty())
            return false;
        if (t[0] == "soak")
        {
            std::optional<std::string_view> const bell = Value(line, "bell");
            return bell && Number(*bell).value_or(0.f) > 0.f;
        }
        return DamageShape(t[0]);
    }

    // ---- step timing (after=, cycle=) ------------------------------------------------------------------------------
    // Two keys of a drill step that the plan reads and the runner strips before the `.mech test` parser
    // (StripPlanKeys): `after=resolve` starts the step `atMs` after the previous step of its cycle resolves (that
    // step's start plus its warning), `after=with` `atMs` after that step starts; without `after=`, `atMs` counts from
    // the cycle start. `cycle=even|odd` runs the step only in even (0, 2, ...) or odd cycles, `cycle=<k>/<m>` (WP-G2)
    // only in cycles k, k + m, k + 2m ... (m from 2 to kMaxCycleEvery, k below m); a skipped step is not "the previous
    // step" of the next one.

    enum class After : std::uint8_t
    {
        CycleStart,
        Resolve,
        With
    };

    // Which cycles a step runs in: those with cycle % every == at (every 1: all of them).
    struct CycleRule
    {
        std::uint32_t every = 1;
        std::uint32_t at = 0;
    };

    constexpr std::uint32_t kMaxCycleEvery = 12;

    // The warning of a step line (its third token, ms); 0 for `marker` and anything unreadable.
    inline std::uint32_t WarnMs(std::string_view line)
    {
        std::vector<std::string_view> const t = Tokens(line);
        if (t.size() < 3 || t[0] == "marker")
            return 0;
        std::optional<float> const ms = Number(t[2]);
        return ms ? static_cast<std::uint32_t>(*ms) : 0;
    }

    // nullopt when the value is not one of the key's words.
    inline std::optional<After> AfterOf(std::string_view line)
    {
        std::optional<std::string_view> const v = Value(line, "after");
        if (!v)
            return After::CycleStart;
        if (*v == "resolve")
            return After::Resolve;
        if (*v == "with")
            return After::With;
        return std::nullopt;
    }

    // A whole number of 1-2 digits (no sign, no fraction); nullopt otherwise.
    inline std::optional<std::uint32_t> SmallCount(std::string_view s)
    {
        if (s.empty() || s.size() > 2)
            return std::nullopt;
        std::uint32_t n = 0;
        for (char const ch : s)
        {
            if (ch < '0' || ch > '9')
                return std::nullopt;
            n = n * 10 + static_cast<std::uint32_t>(ch - '0');
        }
        return n;
    }

    // The step's `cycle=` (absent: every cycle); nullopt when the value is not even, odd or a valid <k>/<m>.
    inline std::optional<CycleRule> CycleOf(std::string_view line)
    {
        std::optional<std::string_view> const v = Value(line, "cycle");
        if (!v)
            return CycleRule{};
        if (*v == "even")
            return CycleRule{ 2, 0 };
        if (*v == "odd")
            return CycleRule{ 2, 1 };
        std::size_t const slash = v->find('/');
        if (slash == std::string_view::npos)
            return std::nullopt;
        std::optional<std::uint32_t> const at = SmallCount(v->substr(0, slash));
        std::optional<std::uint32_t> const every = SmallCount(v->substr(slash + 1));
        if (!at || !every || *every < 2 || *every > kMaxCycleEvery || *at >= *every)
            return std::nullopt;
        return CycleRule{ *every, *at };
    }

    // Are the step's timing keys readable? (The runner refuses a drill whose step fails this.)
    inline bool PlanKeysOk(std::string_view line)
    {
        return AfterOf(line).has_value() && CycleOf(line).has_value();
    }

    inline bool RunsInCycle(std::string_view line, std::uint32_t cycle)
    {
        CycleRule const r = CycleOf(line).value_or(CycleRule{});
        return cycle % r.every == r.at;
    }

    // When each step starts in cycle `cycle`, in ms after the cycle start (nullopt: skipped in this cycle). An
    // after= step with no earlier step in the cycle counts from the cycle start.
    inline std::vector<std::optional<std::uint32_t>> StepOffsets(Def const& def, std::uint32_t cycle)
    {
        std::vector<std::optional<std::uint32_t>> out(def.steps.size());
        std::optional<std::size_t> prev;
        for (std::size_t i = 0; i < def.steps.size(); ++i)
        {
            Step const& step = def.steps[i];
            if (!RunsInCycle(step.line, cycle))
                continue;
            After const after = AfterOf(step.line).value_or(After::CycleStart);
            std::uint32_t base = 0;
            if (prev && after != After::CycleStart)
            {
                base = *out[*prev];
                if (after == After::Resolve)
                    base += WarnMs(def.steps[*prev].line);
            }
            out[i] = base + step.atMs;
            prev = i;
        }
        return out;
    }

    // The line without the plan's keys (after=, cycle=), as the `.mech test` parser reads it.
    inline std::string StripPlanKeys(std::string_view line)
    {
        std::string out;
        for (std::string_view const t : Tokens(line))
        {
            std::string_view const key = t.substr(0, t.find('='));
            if (t.find('=') != std::string_view::npos && (key == "after" || key == "cycle"))
                continue;
            if (!out.empty())
                out += ' ';
            out += t;
        }
        return out;
    }

    // ---- member picks (pick=any|nontank|other|tank, min=) ----------------------------------------------------------

    enum class PickRule : std::uint8_t
    {
        Any,
        NonTank,        // never the selected unit's victim
        Other,          // a non-tank the chain has not picked yet (see PickPool)
        Tank,           // the selected unit's victim (Scarlet Monastery G1: Torment on the tank); none: anyone
        Fresh           // a member, the tank included, the chain has not picked (Ragefire Chasm G1; see PickPool)
    };

    // One member as a pick sees it (members in guid order).
    struct PickCandidate
    {
        bool victim = false;    // the selected unit's victim (the tank)
        bool chained = false;   // picked since the last pick that was not pick=other (the chain)
        bool last = false;      // the previous pick
        float dist = 0.f;       // from the shape's source (min=)
    };

    // The members a pick draws from (indices into `c`, in order; empty: nobody). pick=other: a non-tank outside the
    // chain, else a non-tank other than the previous pick, else any non-tank (a small group runs out of fresh
    // targets). pick=fresh (Ragefire Chasm G1): a member, the tank included, outside the chain; everyone picked: anyone
    // but the previous pick, and the chain starts over (a shuffled rotation; FreshRestarts). min > 0: of those, the
    // ones at least `min` yd from the source, else the single farthest one.
    inline std::vector<std::size_t> PickPool(std::vector<PickCandidate> const& c, PickRule rule, float min)
    {
        std::vector<std::size_t> pool;
        auto collect = [&c, &pool](auto const& keep)
        {
            pool.clear();
            for (std::size_t i = 0; i < c.size(); ++i)
                if (keep(c[i]))
                    pool.push_back(i);
        };
        if (rule == PickRule::Tank)
        {
            collect([](PickCandidate const& m) { return m.victim; });
            if (pool.empty())
                collect([](PickCandidate const&) { return true; });
        }
        else if (rule == PickRule::Any)
            collect([](PickCandidate const&) { return true; });
        else if (rule == PickRule::Fresh)
        {
            collect([](PickCandidate const& m) { return !m.chained; });
            if (pool.empty())
                collect([](PickCandidate const& m) { return !m.last; });
            if (pool.empty())
                collect([](PickCandidate const&) { return true; });
        }
        else
        {
            if (rule == PickRule::Other)
            {
                collect([](PickCandidate const& m) { return !m.victim && !m.chained; });
                if (pool.empty())
                    collect([](PickCandidate const& m) { return !m.victim && !m.last; });
            }
            if (pool.empty())
                collect([](PickCandidate const& m) { return !m.victim; });
        }
        if (min > 0.f && !pool.empty())
        {
            std::vector<std::size_t> far;
            std::size_t farthest = pool.front();
            for (std::size_t i : pool)
            {
                if (c[i].dist >= min)
                    far.push_back(i);
                if (c[i].dist > c[farthest].dist)
                    farthest = i;
            }
            if (far.empty())
                far.push_back(farthest);
            pool = far;
        }
        return pool;
    }

    // pick=fresh with every member in the chain: the pick starts a new round (the runner clears the chain first).
    inline bool FreshRestarts(std::vector<PickCandidate> const& c)
    {
        for (PickCandidate const& m : c)
            if (!m.chained)
                return false;
        return true;
    }

    // ---- fixed lines (fix=1, reach=1, at=last) ---------------------------------------------------------------------

    // A line frozen at registration: from `source` toward `toward`, `size` long, or (reach) ending at `toward` but
    // never shorter than `size`; `past` (reach, Scarlet Monastery G2 `past=<yd>`) runs it that far beyond `toward`
    // (Crell's Proving Strike: "to 5 yd past the target's spot"). A target on the source keeps the given facing.
    struct FixedLine
    {
        Geo::Vec3 source;
        float facing = 0.f;
        float length = 0.f;
    };

    inline FixedLine FixLine(Geo::Vec3 const& source, Geo::Vec3 const& toward, float size, bool reach,
        float facing, float past = 0.f)
    {
        float const dx = toward.x - source.x;
        float const dy = toward.y - source.y;
        float const d = std::sqrt(dx * dx + dy * dy);
        FixedLine out{ source, d > 1e-3f ? std::atan2(dy, dx) : facing, size };
        if (reach)
            out.length = std::max(size, d + std::max(past, 0.f));
        return out;
    }

    // The far end of a line (`at=last` lands the next one there).
    inline Geo::Vec3 LineEnd(Geo::Vec3 const& source, float facing, float length)
    {
        return Shifted(source, facing, length, 0.f);
    }

    // ---- the drill-only walk and leash steps (Blackrock Depths build WP-G1; see kWalkStepMs) -----------------------

    constexpr std::uint32_t kMaxWalkMs = 120000;
    constexpr std::uint32_t kMinLeashTickMs = 250;

    struct WalkStep
    {
        float yd = 0.f;
        std::uint32_t ms = 0;
        float offsetDeg = 0.f;          // counterclockwise from the run's facing
    };

    struct LeashStep
    {
        float yd = 0.f;
        std::uint32_t ms = 0;
        std::uint32_t tickMs = kDefaultLeashTickMs;
        LeashRole role = LeashRole::Any;
        LeashAnchor anchor = LeashAnchor::Unit;
        std::uint32_t ledgerTag = 0;
        bool near = false;              // near=1 (Scarlet Monastery G1): a failure when CLOSER than yd (LeashFails)
    };

    // Does a leash fail a member whose distance test came out `outOfLeash` (farther than yd)? near=1 inverts it: the
    // member fails while it stands within yd (a Torment target that has not backed off).
    constexpr bool LeashFails(bool near, bool outOfLeash)
    {
        return near ? !outOfLeash : outOfLeash;
    }

    // Every token after the third is key=value with one of `keys`, each at most once.
    inline bool OnlyKeys(std::vector<std::string_view> const& t, std::vector<std::string_view> const& keys)
    {
        std::vector<std::string_view> seen;
        for (std::size_t i = 3; i < t.size(); ++i)
        {
            std::size_t const eq = t[i].find('=');
            if (eq == std::string_view::npos || eq == 0 || eq + 1 == t[i].size())
                return false;
            std::string_view const key = t[i].substr(0, eq);
            if (std::find(keys.begin(), keys.end(), key) == keys.end()
                || std::find(seen.begin(), seen.end(), key) != seen.end())
                return false;
            seen.push_back(key);
        }
        return true;
    }

    // `walk <yd> <ms> [offset=<deg>]` (plan keys stripped): yd in (0, kMaxWalkYd], ms from kWalkStepMs to
    // kMaxWalkMs, offset 0-360; nullopt otherwise.
    inline std::optional<WalkStep> ParseWalk(std::string_view line)
    {
        std::vector<std::string_view> const t = Tokens(line);
        if (t.size() < 3 || t[0] != "walk" || !OnlyKeys(t, { "offset" }))
            return std::nullopt;
        std::optional<float> const yd = Number(t[1]);
        std::optional<float> const ms = Number(t[2]);
        if (!yd || *yd <= 0.f || *yd > kMaxWalkYd || !ms || *ms < static_cast<float>(kWalkStepMs)
            || *ms > static_cast<float>(kMaxWalkMs))
            return std::nullopt;
        WalkStep w;
        w.yd = *yd;
        w.ms = static_cast<std::uint32_t>(*ms);
        if (std::optional<std::string_view> const v = Value(line, "offset"))
        {
            std::optional<float> const deg = Number(*v);
            if (!deg || *deg > 360.f)
                return std::nullopt;
            w.offsetDeg = *deg;
        }
        return w;
    }

    // `leash <yd> <ms> [role=tank|healer|nonhealer|any] [anchor=tank|self] [tick=<ms>] [near=0|1] tag=<n>` (plan keys
    // stripped): yd > 0, ms up to kMaxWalkMs, tick from kMinLeashTickMs to ms, a whole tag >= 1; nullopt otherwise.
    inline std::optional<LeashStep> ParseLeash(std::string_view line)
    {
        std::vector<std::string_view> const t = Tokens(line);
        if (t.size() < 3 || t[0] != "leash" || !OnlyKeys(t, { "role", "anchor", "tick", "near", "tag" }))
            return std::nullopt;
        std::optional<float> const yd = Number(t[1]);
        std::optional<float> const ms = Number(t[2]);
        if (!yd || *yd <= 0.f || !ms || *ms <= 0.f || *ms > static_cast<float>(kMaxWalkMs))
            return std::nullopt;
        LeashStep l;
        l.yd = *yd;
        l.ms = static_cast<std::uint32_t>(*ms);
        if (std::optional<std::string_view> const role = Value(line, "role"))
        {
            if (*role == "tank")
                l.role = LeashRole::Tank;
            else if (*role == "healer")
                l.role = LeashRole::Healer;
            else if (*role == "nonhealer")
                l.role = LeashRole::NonHealer;
            else if (*role != "any")
                return std::nullopt;
        }
        if (std::optional<std::string_view> const anchor = Value(line, "anchor"))
        {
            if (*anchor == "tank")
                l.anchor = LeashAnchor::Tank;
            else if (*anchor == "self")
                l.anchor = LeashAnchor::Self;
            else
                return std::nullopt;
        }
        if (std::optional<std::string_view> const v = Value(line, "tick"))
        {
            std::optional<float> const tick = Number(*v);
            if (!tick || *tick < static_cast<float>(kMinLeashTickMs) || *tick > *ms)
                return std::nullopt;
            l.tickMs = static_cast<std::uint32_t>(*tick);
        }
        else if (l.tickMs > l.ms)
            return std::nullopt;
        if (std::optional<std::string_view> const v = Value(line, "near"))
        {
            if (*v != "0" && *v != "1")
                return std::nullopt;
            l.near = *v == "1";
        }
        std::optional<std::string_view> const tag = Value(line, "tag");
        std::optional<float> const n = tag ? Number(*tag) : std::nullopt;
        if (!n || *n < 1.f || *n != std::floor(*n))
            return std::nullopt;
        l.ledgerTag = static_cast<std::uint32_t>(*n);
        return l;
    }

    // `chase <yd> <ms> [home=0|1]` (Scarlet Monastery G1; plan keys stripped): yd in (0, kMaxChaseYd], ms from
    // kChaseStepMs to kMaxWalkMs; nullopt otherwise.
    inline std::optional<ChaseSpec> ParseChase(std::string_view line)
    {
        std::vector<std::string_view> const t = Tokens(line);
        if (t.size() < 3 || t[0] != "chase" || !OnlyKeys(t, { "home" }))
            return std::nullopt;
        std::optional<float> const yd = Number(t[1]);
        std::optional<float> const ms = Number(t[2]);
        if (!yd || *yd <= 0.f || *yd > kMaxChaseYd || !ms || *ms < static_cast<float>(kChaseStepMs)
            || *ms > static_cast<float>(kMaxWalkMs))
            return std::nullopt;
        ChaseSpec c;
        c.yd = *yd;
        c.ms = static_cast<std::uint32_t>(*ms);
        if (std::optional<std::string_view> const v = Value(line, "home"))
        {
            if (*v != "0" && *v != "1")
                return std::nullopt;
            c.home = *v == "1";
        }
        return c;
    }

    // `holdfacing <ms>` (Scarlet Monastery G2; plan keys stripped, no other key): for <ms> the selected unit's bot
    // tank holds its facing (Mechanic::HoldTankFacing(the test owner, the selected unit, ms), SM-BM F6: D12's call
    // from the Sow to the Prune that cuts the seedlings); ms from kChaseStepMs to kMaxWalkMs; nullopt otherwise.
    inline std::optional<std::uint32_t> ParseHoldFacing(std::string_view line)
    {
        std::vector<std::string_view> const t = Tokens(line);
        if (t.size() != 2 || t[0] != "holdfacing")
            return std::nullopt;
        std::optional<float> const ms = Number(t[1]);
        if (!ms || *ms < static_cast<float>(kChaseStepMs) || *ms > static_cast<float>(kMaxWalkMs)
            || *ms != std::floor(*ms))
            return std::nullopt;
        return static_cast<std::uint32_t>(*ms);
    }

    // `blink <yd> 0 [pick=any|fresh|nontank|same] [at=prev] [home=1]` (Ragefire Chasm G1; plan keys stripped; the 0 is
    // the warning column, a blink takes none): exactly one of a pick (yd in (0, kMaxBlinkYd]; default any), at=prev
    // (yd 0) and home=1 (yd 0); nullopt otherwise.
    constexpr float kMaxBlinkYd = 10.f;

    struct BlinkStep
    {
        float yd = 0.f;
        bool atPrev = false;            // at=prev: the last point circle's centre
        bool home = false;              // home=1: where the unit stood before the run's first blink
        PickRule pick = PickRule::Any;
        bool same = false;              // pick=same: the last pick (else as PickRule::Any)
    };

    inline std::optional<BlinkStep> ParseBlink(std::string_view line)
    {
        std::vector<std::string_view> const t = Tokens(line);
        if (t.size() < 3 || t[0] != "blink" || t[2] != "0" || !OnlyKeys(t, { "pick", "at", "home" }))
            return std::nullopt;
        std::optional<float> const yd = Number(t[1]);
        if (!yd || *yd > kMaxBlinkYd)
            return std::nullopt;
        BlinkStep b;
        b.yd = *yd;
        std::optional<std::string_view> const at = Value(line, "at");
        std::optional<std::string_view> const home = Value(line, "home");
        std::optional<std::string_view> const pick = Value(line, "pick");
        if ((at && *at != "prev") || (home && *home != "1"))
            return std::nullopt;
        b.atPrev = at.has_value();
        b.home = home.has_value();
        if (b.atPrev || b.home)
            return (b.atPrev && b.home) || pick || b.yd > 0.f ? std::nullopt : std::optional<BlinkStep>(b);
        if (b.yd <= 0.f)
            return std::nullopt;
        if (pick)
        {
            if (*pick == "fresh")
                b.pick = PickRule::Fresh;
            else if (*pick == "nontank")
                b.pick = PickRule::NonTank;
            else if (*pick == "same")
                b.same = true;
            else if (*pick != "any")
                return std::nullopt;
        }
        return b;
    }

    // `cast <spell> <ms> [kick=1] [every=<ms>] tag=<n>` (Ragefire Chasm G1; plan keys stripped): ms from kMinCastMs to
    // kMaxCastMs, every= (with kick=1 only) longer than ms, a whole tag >= 1; nullopt otherwise. `<ms>` is the cast's
    // own length: the judge reads it kCastJudgeLeadMs before its end.
    constexpr std::uint32_t kMinCastMs = 500;
    constexpr std::uint32_t kMaxCastMs = 30000;
    constexpr std::uint32_t kCastJudgeLeadMs = 150;

    struct CastStep
    {
        std::uint32_t spell = 0;
        std::uint32_t ms = 0;
        std::uint32_t everyMs = 0;      // 0: no cadence noted
        bool kick = false;
        std::uint32_t ledgerTag = 0;
    };

    // Is the cast a group failure? `running`: it still runs kCastJudgeLeadMs before its end (false: it never started,
    // was interrupted, or was cut). A kick=1 cast fails when nobody stopped it, and when it never started (a row that
    // did not run must not read as a pass); a must-play bar when it does not run to its end.
    constexpr bool CastFails(bool kick, bool started, bool running)
    {
        return kick ? !started || running : !running;
    }

    inline std::optional<CastStep> ParseCast(std::string_view line)
    {
        std::vector<std::string_view> const t = Tokens(line);
        if (t.size() < 3 || t[0] != "cast" || !OnlyKeys(t, { "kick", "every", "tag" }))
            return std::nullopt;
        std::optional<float> const ms = Number(t[2]);
        if (!ms || *ms < static_cast<float>(kMinCastMs) || *ms > static_cast<float>(kMaxCastMs))
            return std::nullopt;
        CastStep c;
        for (char const ch : t[1])      // a spell id does not fit a float: whole digits only
        {
            if (ch < '0' || ch > '9' || c.spell > 99999999u)
                return std::nullopt;
            c.spell = c.spell * 10 + static_cast<std::uint32_t>(ch - '0');
        }
        if (!c.spell)
            return std::nullopt;
        c.ms = static_cast<std::uint32_t>(*ms);
        if (std::optional<std::string_view> const v = Value(line, "kick"))
        {
            if (*v != "0" && *v != "1")
                return std::nullopt;
            c.kick = *v == "1";
        }
        if (std::optional<std::string_view> const v = Value(line, "every"))
        {
            std::optional<float> const every = Number(*v);
            if (!every || !c.kick || *every <= *ms)
                return std::nullopt;
            c.everyMs = static_cast<std::uint32_t>(*every);
        }
        std::optional<std::string_view> const tag = Value(line, "tag");
        std::optional<float> const n = tag ? Number(*tag) : std::nullopt;
        if (!n || *n < 1.f || *n != std::floor(*n))
            return std::nullopt;
        c.ledgerTag = static_cast<std::uint32_t>(*n);
        return c;
    }

    // A signed decimal number (Number reads none).
    inline std::optional<float> SignedNumber(std::string_view s)
    {
        bool const minus = !s.empty() && s.front() == '-';
        std::optional<float> const v = Number(minus ? s.substr(1) : s);
        return v && minus ? std::optional<float>(-*v) : v;
    }

    // `inside=<yd>` or `inside=<yd>:<ahead>` (Ragefire Chasm G1): yd > 0, ahead at most 50 yd either way; nullopt
    // otherwise.
    inline std::optional<InsideSpec> ParseInside(std::string_view value)
    {
        std::size_t const colon = value.find(':');
        std::optional<float> const yd = Number(value.substr(0, colon));
        if (!yd || *yd <= 0.f)
            return std::nullopt;
        InsideSpec out;
        out.yd = *yd;
        if (colon != std::string_view::npos)
        {
            out.ahead = SignedNumber(value.substr(colon + 1));
            if (!out.ahead || std::fabs(*out.ahead) > 50.f)
                return std::nullopt;
        }
        return out;
    }

    // ---- repeat= and every= on a drill step (Scarlet Monastery G2: the runner used to drop them) -------------------
    // `.mech test` reads `repeat=` (1, at most kMaxStepRepeat) and `every=` (1000 ms) and registers the repeats every
    // `every` ms, each one tag higher. A drill step does the same but keeps its tag: a drill's ledger tag names its
    // mechanic (`pass-eggs`: 40 eggs on tag 60, DrillPresets::kPassEggTag, which the `pass` descriptor counts).
    // Before this the runner (RunDrillWith) parsed and dropped both, so `pass-eggs` laid one egg per cycle.

    constexpr std::uint32_t kMaxStepRepeat = 50;            // MechanicCommand.cpp kMaxRepeat
    constexpr std::uint32_t kDefaultStepEveryMs = 1000;

    // The start offsets (ms after the step's own start) of a step's repeats: 0, every, 2 * every, ...; `repeat`
    // clamped to 1 .. kMaxStepRepeat.
    inline std::vector<std::uint32_t> RepeatOffsets(std::uint32_t repeat, std::uint32_t everyMs)
    {
        std::uint32_t const n = std::clamp<std::uint32_t>(repeat, 1u, kMaxStepRepeat);
        std::vector<std::uint32_t> out;
        out.reserve(n);
        for (std::uint32_t k = 0; k < n; ++k)
            out.push_back(k * everyMs);
        return out;
    }

    // A step line's repeats as the `.mech test` parser reads them (repeat= whole, default 1, clamped; every= ms,
    // default kDefaultStepEveryMs, never below 0): the offsets the runner schedules.
    inline std::vector<std::uint32_t> StepRepeatOffsets(std::string_view line)
    {
        std::uint32_t repeat = 1;
        std::uint32_t every = kDefaultStepEveryMs;
        if (std::optional<std::string_view> const v = Value(line, "repeat"))
            if (std::optional<float> const n = Number(*v))
                repeat = *n < 1.f ? 1u : static_cast<std::uint32_t>(std::min(*n, static_cast<float>(kMaxStepRepeat)));
        if (std::optional<std::string_view> const v = Value(line, "every"))
            if (std::optional<float> const n = Number(*v))
                every = *n < 0.f ? 0u : static_cast<std::uint32_t>(*n);
        return RepeatOffsets(repeat, every);
    }
}

#endif
