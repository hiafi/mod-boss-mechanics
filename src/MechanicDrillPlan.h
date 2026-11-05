/*
 * mod-boss-mechanics: the pure plans of the drill runner (STAGE-M4-HANDOFF §3.2, §3.3, §3.7, WP-B).
 *
 * No AzerothCore includes: tests/drill_plan_test.cpp compiles this header with the host compiler. The runner
 * (MechanicDrillRunner.h, implemented in MechanicCommand.cpp next to the `.mech test` parser) reads it for:
 * - DrillOptions: what `.mech drill`, the training dummy and the sim pass to RunDrill / StartProgram;
 * - StepSchedule: when each step of a drill registers (cycle-major, the order `.mech drill` always used);
 * - OwnerFollowsBoss: whether the owner is moved onto the boss stand-in before the steps (Bait drills);
 * - programs: a list of drills, barrages, minefields and lane sets run one after the other on a loop
 *   (`ParseProgram`), for the training dummy and the dpssim mechanics mode; M5 (STAGE-M5-HANDOFF §3.2, WP-D) adds the
 *   descriptor entries (casts, adds, debuffs, buffs, swaps, prisons, a descriptor preset) run on the owner, so the
 *   same program works live on the dummy and in the sim on SimTarget;
 * - UMinefield: the fixed U of overlapping mines of `.mech minefield layout=u` (§3.7).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DRILL_PLAN_H
#define MOD_BOSS_MECHANICS_MECHANIC_DRILL_PLAN_H

#include "MechanicDescriptorDrills.h"
#include "MechanicDescriptorPresets.h"
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
        bool percent = false;                   // M4 WP-F: no flat damage; every damage step deals its own `pct=`
                                                // of reference health (20% when the line has none), barrage shots
                                                // and mines too (the training dummy's "percent" choice)
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
        UMinefield,     // `.mech minefield layout=u` ahead of the scope player, the mines last the entry
        // M5 descriptor entries. Wait holds the timeline; the others take no time: they start together with the entry
        // that follows them (no gap), and what they leave running lasts the rest of the loop (EntryWindowMs).
        Wait,           // `wait[:<secs>]`: nothing for `seconds` (default 30), no gap after it
        Cast,           // `cast <spell> [unit=<entry>] [every=<ms>]`: the owner (or each live add of the entry) casts
                        // the spell at its victim, interruptible; `every` repeats it for the rest of the loop and
                        // notes each next cast (Mechanic::NoteNextCast)
        Add,            // `add <entry> [n=<k>] [ttl=<ms>] [aura=<id>]`: k adds around the owner, tracked on the
                        // encounter (despawned at its end), each with the aura; ttl 0: the rest of the loop
        Debuff,         // `debuff <spell> [n=<k>] [hp=<pct>]`: the aura on k random scope group members (hp=: each
                        // of them wounded down to that percent of health, a drill's "someone is under 50%")
        Buff,           // `buff <unit> <spell>`: the aura on every live add of entry <unit> (the owner's entry: the
                        // owner)
        Swap,           // `swap <entry>`: the owner trades places with a random live add of the entry
        Hold,           // `hold <entry> [n=<k>] [ttl=<ms>] [aura=<id>] [pick=tank|any]`: a prison unit on each of k
                        // members (pick=tank: the owner's victim, else the role provider's tank, first), the aura on
                        // the held player until the prison dies or despawns
        Describe        // `describe <name> [phase=<n>] [follow=<entry>]`: the owner's descriptor becomes the preset
                        // (MechanicDescriptorPresets.h); phase= sets its phase; follow= turns the preset's first
                        // target entry of that creature into its first live add's guid (WP-K)
    };

    struct ProgramEntry
    {
        EntryKind kind = EntryKind::Drill;
        Def const* drill = nullptr;     // EntryKind::Drill
        std::uint32_t cycles = 0;       // EntryKind::Drill
        std::uint32_t seconds = 0;      // the other kinds
        std::uint32_t startMs = 0;      // after the program (or loop) start
        std::uint32_t lengthMs = 0;
        // M5 descriptor entries (EntryKind::Cast and after)
        std::uint32_t spellId = 0;      // Cast, Debuff, Buff
        std::uint32_t unitEntry = 0;    // Cast unit= (0: the owner), Add, Buff, Swap, Hold
        std::uint32_t everyMs = 0;      // Cast (0: once)
        std::uint32_t count = 1;        // Add, Debuff, Hold n=
        std::uint32_t ttlMs = 0;        // Add, Hold (0: the rest of the loop; the encounter when the program has no
                                        // length)
        std::uint32_t auraId = 0;       // Add, Hold aura=
        bool pickTank = false;          // Hold pick=tank
        Desc::Preset const* preset = nullptr;   // Describe
        // M5 WP-K
        std::uint32_t woundPct = 0;     // Debuff hp= (0: no wound)
        std::optional<std::uint32_t> phase;     // Describe phase=
        std::uint32_t followEntry = 0;  // Describe follow=
        std::uint32_t endMs = 0;        // set inside a descriptor drill (MechanicDescriptorDrills.h): what the entry
                                        // leaves running ends here (after the program start), not at the loop's end
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
    // M5 descriptor entries: at most this many units per entry, casts at most this often, lifetimes up to the longest
    // segment.
    constexpr std::uint32_t kMaxEntryUnits = 10;
    constexpr std::uint32_t kMinEveryMs = 500;
    constexpr std::uint32_t kMaxTtlMs = kMaxSegmentSecs * 1000;

    // Does this kind take no time (it starts with the next entry, no gap after it)?
    constexpr bool Instant(EntryKind k)
    {
        return k >= EntryKind::Cast;
    }

    // How long what an entry leaves running lasts (a `cast every=` chain, an add or prison without ttl): the rest of
    // the loop after its start, so the next loop's run of the entry never overlaps it (an entry at the very end of
    // the loop runs with the next loop: a whole loop). 0 = open-ended (the program has no length: until StopProgram
    // or the encounter's end).
    inline std::uint32_t EntryWindowMs(Program const& program, ProgramEntry const& e)
    {
        if (e.endMs > e.startMs)
            return e.endMs - e.startMs;     // a descriptor drill's entry: the rest of the drill
        if (!program.loopMs)
            return 0;
        return program.loopMs > e.startMs ? program.loopMs - e.startMs : program.loopMs;
    }

    // An add's or prison's lifetime: its ttl, else the entry's window (0 = until the encounter ends).
    inline std::uint32_t EntryTtlMs(Program const& program, ProgramEntry const& e)
    {
        return e.ttlMs ? e.ttlMs : EntryWindowMs(program, e);
    }

    // The tokens of a descriptor entry: separated by spaces or colons, so the space-split `.mech dummy` command can
    // pass `cast:205010:every=4000` for `cast 205010 every=4000`.
    inline std::vector<std::string_view> EntryTokens(std::string_view item)
    {
        std::vector<std::string_view> out;
        std::size_t i = 0;
        while (i < item.size())
        {
            while (i < item.size() && (item[i] == ' ' || item[i] == ':'))
                ++i;
            std::size_t const start = i;
            while (i < item.size() && item[i] != ' ' && item[i] != ':')
                ++i;
            if (i > start)
                out.push_back(item.substr(start, i - start));
        }
        return out;
    }

    // A whole number token (digits only, at most 4294967295).
    inline std::optional<std::uint32_t> WholeNumber(std::string_view s)
    {
        if (s.empty() || s.size() > 10)
            return std::nullopt;
        std::uint64_t v = 0;
        for (char const ch : s)
        {
            if (ch < '0' || ch > '9')
                return std::nullopt;
            v = v * 10 + static_cast<std::uint64_t>(ch - '0');
        }
        if (v > 0xFFFFFFFFull)
            return std::nullopt;
        return static_cast<std::uint32_t>(v);
    }

    inline std::optional<EntryKind> DescriptorEntryKind(std::string_view name)
    {
        if (name == "wait")
            return EntryKind::Wait;
        if (name == "cast")
            return EntryKind::Cast;
        if (name == "add")
            return EntryKind::Add;
        if (name == "debuff")
            return EntryKind::Debuff;
        if (name == "buff")
            return EntryKind::Buff;
        if (name == "swap")
            return EntryKind::Swap;
        if (name == "hold")
            return EntryKind::Hold;
        if (name == "describe")
            return EntryKind::Describe;
        return std::nullopt;
    }

    // One descriptor entry (`kind` from its first token). The ids are only checked to be > 0 here; the runner
    // checks that the spells and creatures exist (CheckProgram) before anything runs. false with `err` set.
    inline bool ParseDescriptorEntry(std::string_view item, EntryKind kind, ProgramEntry& e, std::string& err)
    {
        std::vector<std::string_view> const t = EntryTokens(item);
        auto fail = [&err, item](std::string const& why)
        {
            err = why + " in program entry '" + std::string(item) + "'";
            return false;
        };
        e.kind = kind;
        // Positional arguments, then key=value options.
        std::size_t positional = 0;
        while (1 + positional < t.size() && t[1 + positional].find('=') == std::string_view::npos)
            ++positional;
        std::size_t const wanted = kind == EntryKind::Wait ? 0 : kind == EntryKind::Buff ? 2 : 1;
        if (kind == EntryKind::Wait)
        {
            if (positional > 1 || t.size() > 1 + positional)
                return fail("bad arguments");
            std::optional<std::uint32_t> const secs = positional ? WholeNumber(t[1])
                : std::optional<std::uint32_t>(kDefaultSegmentSecs);
            if (!secs || *secs < 1)
                return fail("bad count");
            e.seconds = std::min(*secs, kMaxSegmentSecs);
            e.lengthMs = e.seconds * 1000;
            return true;
        }
        if (positional != wanted)
            return fail(wanted == 2 ? "expected <unit> <spell>" : "expected one id or name");
        if (kind == EntryKind::Describe)
        {
            e.preset = Desc::FindPreset(t[1]);
            if (!e.preset)
                return fail("unknown descriptor preset '" + std::string(t[1]) + "'");
            for (std::size_t i = 2; i < t.size(); ++i)
            {
                std::size_t const eq = t[i].find('=');
                std::string_view const key = t[i].substr(0, eq);
                std::optional<std::uint32_t> const n = eq == std::string_view::npos ? std::nullopt
                    : WholeNumber(t[i].substr(eq + 1));
                std::uint32_t const v = n.value_or(0);
                if (key == "phase" && n.has_value() && !e.phase)
                    e.phase = v;
                else if (key == "follow" && v && !e.followEntry)
                    e.followEntry = v;
                else
                    return fail("unexpected options");
            }
            return true;
        }
        std::optional<std::uint32_t> const first = WholeNumber(t[1]);
        if (!first || !*first)
            return fail("bad id '" + std::string(t[1]) + "'");
        switch (kind)
        {
            case EntryKind::Cast:
            case EntryKind::Debuff:
                e.spellId = *first;
                break;
            case EntryKind::Buff:
            {
                std::optional<std::uint32_t> const spell = WholeNumber(t[2]);
                if (!spell || !*spell)
                    return fail("bad id '" + std::string(t[2]) + "'");
                e.unitEntry = *first;
                e.spellId = *spell;
                break;
            }
            default:
                e.unitEntry = *first;
                break;
        }
        for (std::size_t i = 1 + positional; i < t.size(); ++i)
        {
            std::size_t const eq = t[i].find('=');
            std::string_view const key = t[i].substr(0, eq);
            std::string_view const value = t[i].substr(eq + 1);
            if (key == "pick" && kind == EntryKind::Hold)
            {
                if (value != "tank" && value != "any")
                    return fail("bad pick '" + std::string(value) + "'");
                e.pickTank = value == "tank";
                continue;
            }
            std::optional<std::uint32_t> const n = WholeNumber(value);
            if (!n)
                return fail("bad value of " + std::string(key));
            bool const unitsKind = kind == EntryKind::Add || kind == EntryKind::Debuff || kind == EntryKind::Hold;
            bool const lifeKind = kind == EntryKind::Add || kind == EntryKind::Hold;
            if (key == "unit" && kind == EntryKind::Cast && *n)
                e.unitEntry = *n;
            else if (key == "every" && kind == EntryKind::Cast && *n >= kMinEveryMs)
                e.everyMs = *n;
            else if (key == "n" && unitsKind && *n >= 1 && *n <= kMaxEntryUnits)
                e.count = *n;
            else if (key == "ttl" && lifeKind && *n <= kMaxTtlMs)
                e.ttlMs = *n;
            else if (key == "aura" && lifeKind && *n)
                e.auraId = *n;
            else if (key == "hp" && kind == EntryKind::Debuff && *n >= 1 && *n <= 99)
                e.woundPct = *n;
            else
                return fail("bad option '" + std::string(t[i]) + "'");
        }
        return true;
    }

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
    // Entries follow each other with kProgramGapMs between them. M5: the descriptor entries of EntryKind (Wait to
    // Describe; their tokens split on spaces or colons) take no gap, and only `wait` takes time. Spaces around an
    // entry are ignored. A descriptor drill's name (MechanicDescriptorDrills.h, `<name>[:<runs>]`) expands in place
    // (ExpandProgramDrill). nullopt with `err` set on a bad entry.
    inline std::optional<Program> ParseProgramText(std::string_view text, std::string& err, std::uint32_t depth);

    // A descriptor drill `runs` times at `at`: its entries shifted to `at`, each bounded by the end of its run's
    // content (an inner drill's own bound kept), then kProgramGapMs (an inner drill's trailing gap is not doubled).
    // false with `err` when its program does not parse, takes no time, or has an entry at its very end (nothing would
    // bound it).
    inline bool ExpandProgramDrill(ProgramDef const& def, std::uint32_t runs, std::uint32_t depth, Program& program,
        std::uint32_t& at, std::string& err)
    {
        if (depth >= kMaxProgramDrillDepth)
        {
            err = "descriptor drill '" + std::string(def.name) + "' nests too deep";
            return false;
        }
        std::optional<Program> const sub = ParseProgramText(def.program, err, depth + 1);
        if (!sub)
            return false;
        // Where its content ends: the last wait, drill or inner drill (the loop length without a trailing gap).
        std::uint32_t contentMs = 0;
        for (ProgramEntry const& e : sub->entries)
            contentMs = std::max({ contentMs, e.endMs, e.startMs + e.lengthMs });
        if (!contentMs)
        {
            err = "descriptor drill '" + std::string(def.name) + "' takes no time (no wait)";
            return false;
        }
        for (ProgramEntry const& e : sub->entries)
            if (e.startMs >= contentMs)
            {
                err = "descriptor drill '" + std::string(def.name) + "' has an entry at its end";
                return false;
            }
        for (std::uint32_t r = 0; r < runs; ++r)
        {
            for (ProgramEntry e : sub->entries)
            {
                e.endMs = at + (e.endMs ? e.endMs : contentMs);
                e.startMs += at;
                program.entries.push_back(e);
            }
            at += contentMs + kProgramGapMs;
        }
        return true;
    }

    inline std::optional<Program> ParseProgram(std::string_view text, std::string& err)
    {
        if (text == "none")
            return Program{};
        if (text.empty() || text == "all")
            text = kDefaultProgram;
        return ParseProgramText(text, err, 0);
    }

    inline std::optional<Program> ParseProgramText(std::string_view text, std::string& err, std::uint32_t depth)
    {
        Program program;
        std::uint32_t at = 0;
        std::size_t i = 0;
        while (i <= text.size())
        {
            std::size_t const comma = std::min(text.find(',', i), text.size());
            std::string_view item = text.substr(i, comma - i);
            i = comma + 1;
            while (!item.empty() && item.front() == ' ')
                item.remove_prefix(1);
            while (!item.empty() && item.back() == ' ')
                item.remove_suffix(1);
            std::string_view const head = item.substr(0, item.find_first_of(" :"));
            if (std::optional<EntryKind> const kind = DescriptorEntryKind(head))
            {
                ProgramEntry e;
                if (!ParseDescriptorEntry(item, *kind, e, err))
                    return std::nullopt;
                e.startMs = at;
                at += e.lengthMs;
                program.entries.push_back(e);
                continue;
            }
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
            if (ProgramDef const* pd = FindProgramDrill(name))
            {
                if (!ExpandProgramDrill(*pd, std::min(n.value_or(1), kMaxDrillCycles), depth, program, at, err))
                    return std::nullopt;
                continue;
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
