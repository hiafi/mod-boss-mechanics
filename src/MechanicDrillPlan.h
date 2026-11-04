/*
 * mod-boss-mechanics: the pure plans of the drill runner (STAGE-M4-HANDOFF §3.2, §3.3, §3.7, WP-B).
 *
 * No AzerothCore includes: tests/drill_plan_test.cpp compiles this header with the host compiler. The runner
 * (MechanicDrillRunner.h, implemented in MechanicCommand.cpp next to the `.mech test` parser) reads it for:
 * - DrillOptions: what `.mech drill`, the training dummy and the sim pass to RunDrill / StartProgram;
 * - StepSchedule: when each step of a drill registers (cycle-major, the order `.mech drill` always used);
 * - OwnerFollowsBoss: whether the owner is moved onto the boss stand-in before the steps (Bait drills);
 * - programs: a list of drills, barrages, minefields and lane sets run one after the other on a loop
 *   (`ParseProgram`), for the training dummy and the dpssim mechanics mode;
 * - UMinefield: the fixed U of overlapping mines of `.mech minefield layout=u` (§3.7).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DRILL_PLAN_H
#define MOD_BOSS_MECHANICS_MECHANIC_DRILL_PLAN_H

#include "MechanicDrill.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace BossMechanics
{
    // Options of one drill run or program (RunDrill, Sim::StartProgram, the training dummy).
    struct DrillOptions
    {
        std::optional<std::uint32_t> seed;      // every pick the run makes (empty: a fresh random seed)
        std::int32_t dmg = 1;                   // flat damage of every damage step (`dmg=`; < 0 is read as 0)
        std::uint32_t cycles = 0;               // RunDrill: cycles of the drill (0: its default cycles)
        bool includeScopePlayer = false;        // member picks may land on the scope player (`self=1`); the dummy
                                                // and the sim pass true, `.mech drill` only with self=1
        bool loop = true;                       // StartProgram: start the program again after its last entry
    };
}

namespace BossMechanics::Drill
{
    constexpr std::uint32_t kMaxDrillCycles = 10;

    // `cycles=` as `.mech drill` reads it: 0 is the drill's default, then clamped to 1..kMaxDrillCycles.
    inline std::uint32_t ClampCycles(std::uint32_t requested, Def const& def)
    {
        std::uint32_t const n = requested ? requested : def.defaultCycles;
        return std::clamp<std::uint32_t>(n, 1, kMaxDrillCycles);
    }

    // One scheduled step: `step` indexes Def::steps, registered `delayMs` after the run starts.
    struct ScheduledStep
    {
        std::uint32_t delayMs = 0;
        std::uint32_t step = 0;
        std::uint32_t cycle = 0;
    };

    // Every step of `cycles` cycles, cycle-major (in time order, as the steps are sorted by atMs below cycleMs), so a
    // scheduler's idle clock ends at the last step.
    inline std::vector<ScheduledStep> StepSchedule(Def const& def, std::uint32_t cycles)
    {
        std::vector<ScheduledStep> out;
        out.reserve(static_cast<std::size_t>(cycles) * def.steps.size());
        for (std::uint32_t c = 0; c < cycles; ++c)
            for (std::uint32_t i = 0; i < def.steps.size(); ++i)
                out.push_back({ c * def.cycleMs + def.steps[i].atMs, i, c });
        return out;
    }

    // The total time of `cycles` cycles.
    inline std::uint32_t RunLengthMs(Def const& def, std::uint32_t cycles)
    {
        return def.cycleMs * cycles;
    }

    // Bait drills measure from the owner: it is moved onto the boss stand-in at the start and before each step,
    // unless the owner is the stand-in itself (the training dummy, the sim's boss).
    inline bool OwnerFollowsBoss(Def const& def, bool ownerIsBoss)
    {
        return def.ownerAtCreature && !ownerIsBoss;
    }

    // ---- programs --------------------------------------------------------------------------------------------------

    enum class EntryKind : std::uint8_t
    {
        Drill,          // a drill of All() (or Lanes()), `cycles` cycles
        Barrage,        // `.mech barrage secs=<seconds>` (900 ms apart, 2 s warning)
        Minefield,      // `.mech minefield` around the boss stand-in, the mines last the entry
        UMinefield      // `.mech minefield layout=u` ahead of the scope player, the mines last the entry
    };

    struct ProgramEntry
    {
        EntryKind kind = EntryKind::Drill;
        Def const* drill = nullptr;     // EntryKind::Drill
        std::uint32_t cycles = 0;       // EntryKind::Drill
        std::uint32_t seconds = 0;      // the other kinds
        std::uint32_t startMs = 0;      // after the program (or loop) start
        std::uint32_t lengthMs = 0;
    };

    struct Program
    {
        std::vector<ProgramEntry> entries;  // empty: `none` (a baseline run: the encounter and scope, no mechanics)
        std::uint32_t loopMs = 0;           // the next loop starts here (0 for an empty program)
    };

    // The quiet time after each entry, so its last warnings and lasting shapes end before the next one starts.
    constexpr std::uint32_t kProgramGapMs = 4000;
    constexpr std::uint32_t kDefaultSegmentSecs = 30;
    constexpr std::uint32_t kMaxSegmentSecs = 600;

    // A lane set every 10 s (§3.6 cost scenario D9): three 6 yd lanes and three crossing ones, 4 s warning, the
    // crossing cells burn for the default 6 s.
    inline Def const& Lanes()
    {
        static Def const lanes = { "lanes", "three crossing lane pairs (6 yd, 4 s warning), crossings burn 6 s", 10000,
            false, { { 0, "lanes 6 4000 count=3 cross=1 tag=1" } }, 3 };
        return lanes;
    }

    // The training dummy's default loop (handoff §3.2, decision §7.2): every drill, then a barrage and a minefield.
    constexpr char const* kDefaultProgram = "smite,cookie,fenrus,soaks,ras,alexei,barrage:30,minefield:30";

    // `none`, `all` (kDefaultProgram), or comma-separated entries `<name>[:<n>]`: a drill name (n = cycles, default
    // the drill's), `lanes` (n = cycles, default 3), `barrage`, `minefield` or `uminefield` (n = seconds, default 30).
    // Entries follow each other with kProgramGapMs between them. nullopt with `err` set on a bad entry.
    inline std::optional<Program> ParseProgram(std::string_view text, std::string& err)
    {
        Program program;
        if (text == "none")
            return program;
        if (text.empty() || text == "all")
            text = kDefaultProgram;
        std::uint32_t at = 0;
        std::size_t i = 0;
        while (i <= text.size())
        {
            std::size_t const comma = std::min(text.find(',', i), text.size());
            std::string_view const item = text.substr(i, comma - i);
            i = comma + 1;
            std::size_t const colon = item.find(':');
            std::string_view const name = item.substr(0, colon);
            std::optional<std::uint32_t> n;
            if (colon != std::string_view::npos)
            {
                std::optional<float> const v = Number(item.substr(colon + 1));
                if (!v || *v < 1.f || std::floor(*v) != *v)
                {
                    err = "bad count in program entry '" + std::string(item) + "'";
                    return std::nullopt;
                }
                n = static_cast<std::uint32_t>(*v);
            }
            ProgramEntry e;
            if (Def const* d = name == "lanes" ? &Lanes() : Find(name))
            {
                e.kind = EntryKind::Drill;
                e.drill = d;
                e.cycles = ClampCycles(n.value_or(0), *d);
                e.lengthMs = RunLengthMs(*d, e.cycles);
            }
            else if (name == "barrage" || name == "minefield" || name == "uminefield")
            {
                e.kind = name == "barrage" ? EntryKind::Barrage : name == "minefield" ? EntryKind::Minefield
                    : EntryKind::UMinefield;
                e.seconds = std::min(n.value_or(kDefaultSegmentSecs), kMaxSegmentSecs);
                e.lengthMs = e.seconds * 1000;
            }
            else
            {
                err = "unknown program entry '" + std::string(item) + "'";
                return std::nullopt;
            }
            e.startMs = at;
            at += e.lengthMs + kProgramGapMs;
            program.entries.push_back(e);
        }
        program.loopMs = at;
        return program;
    }

    // ---- `.mech minefield layout=u` --------------------------------------------------------------------------------

    // A U of overlapping mines of `radius` opening toward `origin`: two arms from the mouth (at `origin`) to `depth`
    // yd ahead along `facing`, their inner edges `gap` yd apart, joined by a wall at `depth`. Neighbors are at most
    // `radius` apart (each overlaps the next by half), so nothing walks through a wall. The pocket inside is clear:
    // |left| < gap / 2, 0 <= ahead < depth - radius. Corner mines are shared by an arm and the wall.
    inline std::vector<Geo::Vec3> UMinefield(Geo::Vec3 const& origin, float facing, float gap, float depth,
        float radius)
    {
        std::vector<Geo::Vec3> out;
        if (radius <= 0.f || gap <= 0.f || depth <= 0.f)
            return out;
        float const side = gap / 2.f + radius;      // the arms' center lines
        auto spaced = [radius](float length)
        {
            return static_cast<std::uint32_t>(std::ceil(length / radius - 1e-4f)) + 1;   // points, both ends
        };
        std::uint32_t const armPoints = spaced(depth);
        for (float const left : { side, -side })
            for (std::uint32_t k = 0; k < armPoints; ++k)
                out.push_back(Shifted(origin, facing, depth * static_cast<float>(k)
                    / static_cast<float>(armPoints - 1), left));
        std::uint32_t const wallPoints = spaced(2.f * side);
        for (std::uint32_t k = 1; k + 1 < wallPoints; ++k)
            out.push_back(Shifted(origin, facing, depth, side - 2.f * side * static_cast<float>(k)
                / static_cast<float>(wallPoints - 1)));
        return out;
    }
}

#endif
