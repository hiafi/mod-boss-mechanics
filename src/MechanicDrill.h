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
 * StepOffsets; `cycle=even|odd` skips it in the other cycles), or the drill-only pseudo shape `marker <stacks>`
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
    // landing does: DamageStep).
    inline bool DamageShape(std::string_view shape)
    {
        return shape != "stack" && shape != "soak" && shape != "holdstill" && shape != "spot" && shape != "marker";
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
            // a catch and ends the hunt). 0.5 s: a bell, a 1-body Soak 2 yd on one of three points 15 yd apart around
            // the creature, not the hunted player, not the tank; filled at its 5 s resolve (the walk plus the 1 s
            // ring), it ends the hunt and the chaser bounds to the bell, landing 2 s later with a 6 yd circle (tag 3
            // for the others, 4 for the ringer). 15 s: the heroic hunt, 95% speed and no bell (spent).
            { "sfk-hunt", "Odo's Hunt: a 3 yd chaser at 80% for 10 s, a bell soak that ends it (6 yd landing), "
                "then a 95% hunt with no bell", 30000, false, {
                    { 0, "hunt 3 2000 speed=0.8 persist=10000 tick=200 pick=nontank pct=35 tag=1" },
                    { 500, "soak 2 5000 need=1 role=nontank bell=3 apart=15 land=6 jump=2000 pct=35 weak=1 tag=2" },
                    { 15000, "hunt 3 2000 speed=0.95 persist=10000 tick=200 pick=nontank pct=35 tag=5" },
                }, 2 },     // two cycles a run; the sweep runs sfk-hunt:4 (120 s)
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
    // the cycle start. `cycle=even|odd` runs the step only in even (0, 2, ...) or odd cycles; a skipped step is not
    // "the previous step" of the next one.

    enum class After : std::uint8_t
    {
        CycleStart,
        Resolve,
        With
    };

    enum class Parity : std::uint8_t
    {
        Every,
        Even,
        Odd
    };

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

    inline std::optional<Parity> ParityOf(std::string_view line)
    {
        std::optional<std::string_view> const v = Value(line, "cycle");
        if (!v)
            return Parity::Every;
        if (*v == "even")
            return Parity::Even;
        if (*v == "odd")
            return Parity::Odd;
        return std::nullopt;
    }

    // Are the step's timing keys readable? (The runner refuses a drill whose step fails this.)
    inline bool PlanKeysOk(std::string_view line)
    {
        return AfterOf(line).has_value() && ParityOf(line).has_value();
    }

    inline bool RunsInCycle(std::string_view line, std::uint32_t cycle)
    {
        Parity const p = ParityOf(line).value_or(Parity::Every);
        return p == Parity::Every || (p == Parity::Even) == (cycle % 2 == 0);
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

    // ---- member picks (pick=any|nontank|other, min=) ---------------------------------------------------------------

    enum class PickRule : std::uint8_t
    {
        Any,
        NonTank,        // never the selected unit's victim
        Other           // a non-tank the chain has not picked yet (see PickPool)
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
    // targets). min > 0: of those, the ones at least `min` yd from the source, else the single farthest one.
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
        if (rule == PickRule::Any)
            collect([](PickCandidate const&) { return true; });
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

    // ---- fixed lines (fix=1, reach=1, at=last) ---------------------------------------------------------------------

    // A line frozen at registration: from `source` toward `toward`, `size` long, or (reach) ending at `toward` but
    // never shorter than `size`. A target on the source keeps the given facing.
    struct FixedLine
    {
        Geo::Vec3 source;
        float facing = 0.f;
        float length = 0.f;
    };

    inline FixedLine FixLine(Geo::Vec3 const& source, Geo::Vec3 const& toward, float size, bool reach,
        float facing)
    {
        float const dx = toward.x - source.x;
        float const dy = toward.y - source.y;
        float const d = std::sqrt(dx * dx + dy * dy);
        FixedLine out{ source, d > 1e-3f ? std::atan2(dy, dx) : facing, size };
        if (reach)
            out.length = std::max(size, d);
        return out;
    }

    // The far end of a line (`at=last` lands the next one there).
    inline Geo::Vec3 LineEnd(Geo::Vec3 const& source, float facing, float length)
    {
        return Shifted(source, facing, length, 0.f);
    }
}

#endif
