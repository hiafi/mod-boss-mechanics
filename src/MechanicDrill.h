/*
 * mod-boss-mechanics: pure timelines and geometry of `.mech drill` and the M3 `.mech test` landings
 * (STAGE-M3-HANDOFF §3.11).
 *
 * No AzerothCore includes: the command (MechanicCommand.cpp) runs each drill step through the `.mech test` parser,
 * and tests/drill_test.cpp compiles this header with the host compiler to desk-check every drill against the boss doc
 * rows it reproduces (shapes, timings, the separations the pairs need) and the landing geometry below.
 *
 * A drill is a list of steps per cycle. A step is the argument line of one `.mech test` (shape, size, ms, keys), run
 * at `atMs` after the cycle start from the test owner's scheduler, or the drill-only pseudo shape `weaken <stacks>`
 * (sets Weakened on group members, see Step). The command appends `dmg=<n>` to every damage step (DamageShape).
 * Positions are yards in map coordinates; angles in radians; "ahead" is along the GM's facing, "left" 90 degrees
 * counterclockwise from it.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DRILL_H
#define MOD_BOSS_MECHANICS_MECHANIC_DRILL_H

#include "MechanicGeometry.h"
#include <cmath>
#include <cstdint>
#include <optional>
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

    // One step: a `.mech test` line, or `weaken <stacks> [pick=nontank]` (stacks > 0: one random group member below
    // that many stacks, never the selected creature's victim with pick=nontank, gets exactly that many; 0: every group
    // member back to 0).
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
        std::vector<Step> steps;        // sorted by atMs
        std::uint32_t defaultCycles = 3;    // `cycles=` when not given
    };

    // Shapes that carry damage keys (dmg=, weak=); stack, soak, holdstill and spot never deal damage.
    inline bool DamageShape(std::string_view shape)
    {
        return shape != "stack" && shape != "soak" && shape != "holdstill" && shape != "spot" && shape != "weaken";
    }

    // Alexei's slots: one rule (or heroic pair) per slot. Singles warn 3 s (normal), pairs 4 s (heroic); the window is
    // 6 s; at-close rules resolve at warning + window.
    constexpr std::uint32_t kAlexeiSlotMs = 12000;

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
            // Shadowfang Keep, Fenrus (FN6): Shadow Leap, Bait on the fewest Weakened stacks (standing in for FN7),
            // 6 yd landing circle, 2 s emote, everyone else within 10 yd of him; the baiter gains a stack, so the
            // next pick rotates.
            { "fenrus", "Bait every 10 s: fewest Weakened stacks baits, landing r 6, others within 10, weak=1", 10000,
                true, {
                    { 0, "bait 10 2000 rule=fewest aura=205000 r=6 weak=1 tag=1" },
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
            // one stands within reach); an ordered soak (fewest Weakened stacks first, players at 2 or more never
            // claim; two members get 1 and 2 stacks first, so the rule decides, not the guid); the Barov fortune
            // (Scholomance AB10: count 0, nobody takes it, 10 yd off the group); a lasting soak on the GM (Rattlegore
            // RG2: 3 yd, 5 s, walk while it lasts).
            { "soaks", "Payday (1 non-tank, 2 yd), ordered soak (Weakened asc, cap 2), fortune (need 0), lasting soak "
                "on you (walk)", 26000, false, {
                    { 0, "soak 2 2500 need=1 role=nontank at=member pick=nontank scatter=5 tag=1" },
                    { 2500, "weaken 2 pick=nontank" },
                    { 3000, "weaken 1 pick=nontank" },
                    { 4000, "soak 3 4000 need=1 order=205000:asc:cap=2 at=group shift=6:0 tag=2" },
                    { 9000, "weaken 0" },
                    { 10000, "soak 2 4000 need=0 at=group shift=0:10 tag=3" },
                    { 16000, "soak 3 3000 need=1 anchor=me persist=5000 tick=1000 tag=4" },
                } },
            // Scholomance, Ras Frostwhisper (RF1, RF2, RF5): a member reaches 3 Weakened stacks (standing in for
            // Chill) and runs out 8 yd (self-keyed RunOut) while the rest (0-2 stacks) hold a Stack 8 yd on Ras; a
            // second member reaches 3 stacks during the warning and joins the runners; everyone thaws (0) after.
            { "ras", "self-keyed RunOut 8 at Weakened >= 3 while a Stack 8 on the creature holds the rest", 12000,
                false, {
                    { 0, "weaken 3 pick=nontank" },
                    { 0, "stack 8 6000 anchor=target aura=205000:0:2 tag=2" },
                    { 0, "runout 8 6000 self=205000:3 tag=5" },
                    { 2000, "weaken 3 pick=nontank" },
                    { 9000, "weaken 0" },
                } },
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
}

#endif
