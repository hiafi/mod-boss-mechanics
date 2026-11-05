// Standalone tests for src/MechanicDrillPlan.h (STAGE-M4-HANDOFF §3.2, §3.7, WP-B): the drill runner's placement and
// timing rules (step schedule, cycles, the owner moved onto the boss stand-in), the program grammar and timeline of
// the training dummy and the sim (M5: the descriptor entries), and the fixed U of `.mech minefield layout=u`. Build:
// tests/run.sh.

#include "BossMechanicsIds.h"
#include "check.h"
#include "MechanicDrillPlan.h"
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace BossMechanics;
using Geo::Vec3;

namespace
{
    constexpr float kEps = 1e-3f;

    std::set<std::string_view> const kTestShapes = { "circle", "donut", "cone", "rearcone", "line", "projectile",
        "mine", "lanes", "spread", "runout", "stack", "soak", "frontal", "bait", "holdstill", "spot" };

    float Dist2d(Vec3 const& a, Vec3 const& b)
    {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    float Nearest(std::vector<Vec3> const& mines, Vec3 const& p, std::size_t skip = static_cast<std::size_t>(-1))
    {
        float best = 1e9f;
        for (std::size_t i = 0; i < mines.size(); ++i)
            if (i != skip)
                best = std::min(best, Dist2d(mines[i], p));
        return best;
    }

    // The schedule `.mech drill` used before the runner: for each cycle, for each step, c * cycleMs + atMs.
    void TestStepSchedule()
    {
        for (Drill::Def const& d : Drill::All())
        {
            for (std::uint32_t cycles = 1; cycles <= 3; ++cycles)
            {
                std::vector<Drill::ScheduledStep> const s = Drill::StepSchedule(d, cycles);
                CHECK(s.size() == cycles * d.steps.size());
                std::size_t k = 0;
                for (std::uint32_t c = 0; c < cycles; ++c)
                    for (std::uint32_t i = 0; i < d.steps.size(); ++i, ++k)
                    {
                        if (k >= s.size())
                            break;
                        CHECK(s[k].step == i);
                        CHECK(s[k].cycle == c);
                        CHECK(s[k].delayMs == c * d.cycleMs + d.steps[i].atMs);
                    }
                // In time order (the test owner's idle clock ends at the last step).
                for (std::size_t j = 1; j < s.size(); ++j)
                    CHECK(s[j - 1].delayMs <= s[j].delayMs);
                CHECK(s.empty() || s.back().delayMs < Drill::RunLengthMs(d, cycles));
            }
        }
    }

    void TestCyclesAndOwner()
    {
        Drill::Def const* smite = Drill::Find("smite");
        Drill::Def const* fenrus = Drill::Find("fenrus");
        Drill::Def const* alexei = Drill::Find("alexei");
        CHECK(smite && fenrus && alexei);
        if (!smite || !fenrus || !alexei)
            return;
        CHECK(Drill::ClampCycles(0, *smite) == smite->defaultCycles);
        CHECK(Drill::ClampCycles(0, *alexei) == 1);
        CHECK(Drill::ClampCycles(1, *smite) == 1);
        CHECK(Drill::ClampCycles(7, *smite) == 7);
        CHECK(Drill::ClampCycles(99, *smite) == Drill::kMaxDrillCycles);

        // Bait drills: the owner stands at the boss stand-in, unless it is the stand-in (dummy, sim).
        CHECK(Drill::OwnerFollowsBoss(*fenrus, false));
        CHECK(!Drill::OwnerFollowsBoss(*fenrus, true));
        CHECK(!Drill::OwnerFollowsBoss(*smite, false));
        CHECK(!Drill::OwnerFollowsBoss(*smite, true));

        // Defaults of the options the runner reads.
        DrillOptions const opt;
        CHECK(!opt.seed);
        CHECK(opt.dmg == 1);
        CHECK(opt.cycles == 0);
        CHECK(!opt.includeScopePlayer);
        CHECK(opt.loop);
    }

    void TestPrograms()
    {
        std::string err;
        std::optional<Drill::Program> none = Drill::ParseProgram("none", err);
        CHECK(none && none->entries.empty() && none->loopMs == 0);

        // `all` and the empty text are the default loop: every drill, then a 30 s barrage and a 30 s minefield.
        for (std::string_view text : { "all", "" })
        {
            std::optional<Drill::Program> all = Drill::ParseProgram(text, err);
            CHECK(all && all->entries.size() == 8);
            if (!all || all->entries.size() != 8)
                continue;
            char const* const drills[] = { "smite", "cookie", "fenrus", "soaks", "ras", "alexei" };
            std::uint32_t at = 0;
            for (std::size_t i = 0; i < all->entries.size(); ++i)
            {
                Drill::ProgramEntry const& e = all->entries[i];
                CHECK(e.startMs == at);
                if (i < 6)
                {
                    CHECK(e.kind == Drill::EntryKind::Drill);
                    CHECK(e.drill && std::string_view(e.drill->name) == drills[i]);
                    CHECK(e.drill && e.cycles == e.drill->defaultCycles);
                    CHECK(e.drill && e.lengthMs == e.drill->cycleMs * e.drill->defaultCycles);
                }
                at += e.lengthMs + Drill::kProgramGapMs;
            }
            CHECK(all->entries[6].kind == Drill::EntryKind::Barrage && all->entries[6].seconds == 30);
            CHECK(all->entries[7].kind == Drill::EntryKind::Minefield && all->entries[7].lengthMs == 30000);
            CHECK(all->loopMs == at);
            // About 8.5 minutes: smite 30, cookie 93, fenrus 30, soaks 78, ras 36, alexei 156, 30 + 30, 8 gaps of 4.
            CHECK(all->loopMs == 483000 + 8 * Drill::kProgramGapMs);
        }

        std::optional<Drill::Program> p = Drill::ParseProgram("smite:2,barrage:10,lanes,uminefield", err);
        CHECK(p && p->entries.size() == 4);
        if (p && p->entries.size() == 4)
        {
            CHECK(p->entries[0].cycles == 2 && p->entries[0].lengthMs == 20000 && p->entries[0].startMs == 0);
            CHECK(p->entries[1].kind == Drill::EntryKind::Barrage && p->entries[1].startMs == 24000);
            CHECK(p->entries[1].lengthMs == 10000);
            CHECK(p->entries[2].drill == &Drill::Lanes() && p->entries[2].cycles == 3);
            CHECK(p->entries[2].startMs == 38000 && p->entries[2].lengthMs == 30000);
            CHECK(p->entries[3].kind == Drill::EntryKind::UMinefield && p->entries[3].seconds == 30);
            CHECK(p->entries[3].startMs == 72000);
            CHECK(p->loopMs == 106000);
        }
        std::optional<Drill::Program> clamped = Drill::ParseProgram("barrage:9999,smite:50", err);
        CHECK(clamped && clamped->entries.size() == 2);
        if (clamped && clamped->entries.size() == 2)
        {
            CHECK(clamped->entries[0].seconds == Drill::kMaxSegmentSecs);
            CHECK(clamped->entries[1].cycles == Drill::kMaxDrillCycles);
        }

        for (std::string_view bad : { "bogus", "smite:0", "smite:1.5", "smite:x", "smite,", ",smite", "all,smite",
            "barrage:" })
        {
            err.clear();
            CHECK(!Drill::ParseProgram(bad, err));
            CHECK(!err.empty());
        }

        // The lane set is a `.mech test lanes` line with damage (the runner appends dmg=).
        Drill::Def const& lanes = Drill::Lanes();
        CHECK(lanes.steps.size() == 1 && Drill::Find("lanes") == nullptr);    // a program entry, not a `.mech drill`
        std::vector<std::string_view> const t = Drill::Tokens(lanes.steps[0].line);
        CHECK(!t.empty() && kTestShapes.count(t[0]) && Drill::DamageShape(t[0]));
        CHECK(Drill::Value(lanes.steps[0].line, "cross") == std::optional<std::string_view>("1"));
    }

    // M5 (STAGE-M5-HANDOFF §3.2, WP-D): the descriptor entries run on the owner. They take no time (no gap after
    // them) except `wait`; their tokens split on spaces or colons.
    void TestDescriptorEntries()
    {
        std::string err;
        std::optional<Drill::Program> p = Drill::ParseProgram("describe none,add 300404 n=2 ttl=20000 aura=205021,"
            "cast 205010 unit=300404 every=4000,wait:20,debuff 205014 n=2,buff 300404 205021,swap 300404,"
            "hold 300407 n=2 aura=205024 pick=tank,wait", err);
        CHECK(p && p->entries.size() == 9);
        if (p && p->entries.size() == 9)
        {
            std::vector<Drill::ProgramEntry> const& e = p->entries;
            CHECK(e[0].kind == Drill::EntryKind::Describe && e[0].preset == Desc::FindPreset("none"));
            CHECK(e[1].kind == Drill::EntryKind::Add && e[1].unitEntry == 300404 && e[1].count == 2);
            CHECK(e[1].ttlMs == 20000 && e[1].auraId == 205021);
            CHECK(e[2].kind == Drill::EntryKind::Cast && e[2].spellId == 205010 && e[2].unitEntry == 300404);
            CHECK(e[2].everyMs == 4000);
            CHECK(e[3].kind == Drill::EntryKind::Wait && e[3].seconds == 20 && e[3].lengthMs == 20000);
            CHECK(e[4].kind == Drill::EntryKind::Debuff && e[4].spellId == 205014 && e[4].count == 2);
            CHECK(e[5].kind == Drill::EntryKind::Buff && e[5].unitEntry == 300404 && e[5].spellId == 205021);
            CHECK(e[6].kind == Drill::EntryKind::Swap && e[6].unitEntry == 300404);
            CHECK(e[7].kind == Drill::EntryKind::Hold && e[7].unitEntry == 300407 && e[7].count == 2);
            CHECK(e[7].auraId == 205024 && e[7].pickTank && e[7].ttlMs == 0);
            CHECK(e[8].kind == Drill::EntryKind::Wait && e[8].seconds == Drill::kDefaultSegmentSecs);
            // The timeline: instant entries start together, only the waits take time, no gaps.
            for (std::size_t i = 0; i < 4; ++i)
                CHECK(e[i].startMs == 0);
            for (std::size_t i = 4; i < 9; ++i)
                CHECK(e[i].startMs == 20000);
            CHECK(p->loopMs == 50000);
            for (std::size_t i = 0; i < 9; ++i)
                CHECK(Drill::Instant(e[i].kind) == (e[i].kind != Drill::EntryKind::Wait));
            // What they leave running lasts the rest of the loop (an explicit ttl wins).
            CHECK(Drill::EntryWindowMs(*p, e[2]) == 50000);
            CHECK(Drill::EntryTtlMs(*p, e[1]) == 20000);
            CHECK(Drill::EntryWindowMs(*p, e[7]) == 30000 && Drill::EntryTtlMs(*p, e[7]) == 30000);
        }
        // Defaults.
        p = Drill::ParseProgram("add 300404,hold 300407,cast 205010", err);
        CHECK(p && p->entries.size() == 3 && p->loopMs == 0);
        if (p && p->entries.size() == 3)
        {
            CHECK(p->entries[0].count == 1 && p->entries[0].ttlMs == 0 && p->entries[0].auraId == 0);
            CHECK(!p->entries[1].pickTank && p->entries[1].count == 1);
            CHECK(p->entries[2].unitEntry == 0 && p->entries[2].everyMs == 0);
            // A program with no length: open-ended (until StopProgram or the encounter's end).
            CHECK(Drill::EntryWindowMs(*p, p->entries[2]) == 0 && Drill::EntryTtlMs(*p, p->entries[0]) == 0);
        }

        // The colon form (the space-split `.mech dummy` command) parses the same.
        std::optional<Drill::Program> const spaced = Drill::ParseProgram("cast 205010 unit=300404 every=4000", err);
        std::optional<Drill::Program> const colons = Drill::ParseProgram("cast:205010:unit=300404:every=4000", err);
        CHECK(spaced && colons && spaced->entries.size() == 1 && colons->entries.size() == 1);
        if (spaced && colons && spaced->entries.size() == 1 && colons->entries.size() == 1)
        {
            Drill::ProgramEntry const& a = spaced->entries[0];
            Drill::ProgramEntry const& b = colons->entries[0];
            CHECK(a.kind == b.kind && a.spellId == b.spellId && a.unitEntry == b.unitEntry && a.everyMs == b.everyMs);
        }
        CHECK(Drill::EntryTokens(" buff:300404 205021 ") == (std::vector<std::string_view>{ "buff", "300404",
            "205021" }));

        // With drills: the drill keeps its gap; an entry at the loop's end runs with the next loop, a whole loop.
        p = Drill::ParseProgram("smite,cast 205010 every=4000", err);
        CHECK(p && p->entries.size() == 2);
        if (p && p->entries.size() == 2)
        {
            CHECK(p->entries[1].startMs == 30000 + Drill::kProgramGapMs && p->loopMs == 34000);
            CHECK(Drill::EntryWindowMs(*p, p->entries[1]) == 34000);
        }
        p = Drill::ParseProgram("cast 205010 every=4000, smite ", err);
        CHECK(p && p->entries.size() == 2 && p->entries[0].startMs == 0 && p->entries[1].startMs == 0);
        CHECK(p && Drill::EntryWindowMs(*p, p->entries[0]) == 34000);

        // Spaces around entries are ignored.
        p = Drill::ParseProgram(" smite , barrage:10 ", err);
        CHECK(p && p->entries.size() == 2 && p->entries[1].kind == Drill::EntryKind::Barrage);

        // Whole numbers only.
        CHECK(Drill::WholeNumber("205010") == std::optional<std::uint32_t>(205010));
        CHECK(Drill::WholeNumber("4294967295") == std::optional<std::uint32_t>(4294967295u));
        for (std::string_view bad : { "", "1.5", "-1", "x", "4294967296", "12345678901" })
            CHECK(!Drill::WholeNumber(bad));

        for (std::string_view bad : { "cast", "cast x", "cast 0", "cast 205010 205011", "cast 205010 every=100",
            "cast 205010 unit=0", "cast 205010 bogus=1", "cast 205010 n=2", "add 300404 n=0", "add 300404 n=11",
            "add 300404 ttl=700000", "add 300404 aura=0", "add 300404 every=4000", "debuff 205014 ttl=5",
            "debuff 205014 aura=1", "buff 300404", "buff 300404 x", "buff 300404 205021 n=2", "swap", "swap 300404 n=2",
            "hold 300407 pick=healer", "hold 300407 n=", "describe bogus", "describe", "describe none x=1", "wait:0",
            "wait 5 6", "wait x", "wait n=3", "smite,cast" })
        {
            err.clear();
            CHECK(!Drill::ParseProgram(bad, err));
            CHECK(!err.empty());
        }
    }

    // ---- M5 WP-K: the descriptor drills (MechanicDescriptorDrills.h) and their presets ----

    bool M5Spell(std::uint32_t id)
    {
        return (id >= Ids::SPELL_M5_FIRST && id <= Ids::SPELL_M5_LAST) || Ids::RiderAllowed(id);
    }

    bool M5Creature(std::uint32_t entry)
    {
        return entry >= Ids::NPC_DRILL_CASTER && entry <= Ids::NPC_DRILL_PRISON;
    }

    bool PresetIdsAllowed(Descriptor const& d)
    {
        auto cond = [](Condition const& c)
        {
            if (c.kind == CondKind::HasAura || c.kind == CondKind::LacksAura)
                return M5Spell(c.id);
            if (c.kind == CondKind::NearUnit)
                return M5Creature(c.id);
            return c.kind != CondKind::Custom && c.kind != CondKind::InZone;     // programs cannot use those
        };
        bool ok = true;
        for (InterruptEntry const& e : d.interrupts)
            ok = ok && (!e.casterEntry || M5Creature(e.casterEntry)) && M5Spell(e.spellId) && cond(e.when);
        for (DispelEntry const& e : d.dispels)
            ok = ok && M5Spell(e.auraId);
        for (PurgeEntry const& e : d.purges)
            ok = ok && (!e.unitEntry || M5Creature(e.unitEntry)) && M5Spell(e.auraId);
        for (TargetEntry const& e : d.targets)
            ok = ok && M5Creature(e.entry) && !e.guid && (!e.partnerEntry || M5Creature(e.partnerEntry))
                && cond(e.when);
        for (IgnoreEntry const& e : d.ignores)
            ok = ok && M5Creature(e.entry) && !e.guid && cond(e.when);
        return ok;
    }

    // Names: no descriptor drill shadows an M4 drill, the lane set, a program keyword or a descriptor entry kind; no
    // two drills or presets share a name.
    void TestDescriptorDrillNames()
    {
        std::set<std::string_view> names;
        for (Drill::ProgramDef const& d : Drill::ProgramDrills())
        {
            CHECK(names.insert(d.name).second);
            CHECK(!Drill::Find(d.name) && Drill::Lanes().name != std::string_view(d.name));
            CHECK(!Drill::DescriptorEntryKind(d.name));
            for (std::string_view kw : { "barrage", "minefield", "uminefield", "none", "all", "lanes" })
                CHECK(kw != d.name);
            CHECK(Drill::FindProgramDrill(d.name) == &d);
            CHECK(std::string_view(d.summary).size() > 0);
        }
        for (Drill::Def const& d : Drill::All())
            CHECK(!Drill::FindProgramDrill(d.name));
        CHECK(!Drill::FindProgramDrill("bogus"));
        std::set<std::string_view> presets;
        for (Desc::Preset const& p : Desc::Presets())
            CHECK(presets.insert(p.name).second);
        // The drills and presets the handoff names (§3.6), and the sim slice's program.
        for (std::string_view n : { "interrupts", "dispels", "purges", "targets", "ignore", "descriptor" })
        {
            CHECK(Drill::FindProgramDrill(n) && Drill::FindProgramDrill(n)->listed);
            CHECK(Desc::FindPreset(n) != nullptr);
        }
        CHECK(Drill::FindProgramDrill(Drill::kDescriptorProgram));
        CHECK(Drill::FindProgramDrill(Drill::kDescriptorDrillsProgram));
    }

    // Every drill parses, takes time, uses only the M5 stand-ins and the drill auras, and bounds what it leaves
    // running by its own end.
    void TestDescriptorDrillsParse()
    {
        for (Drill::ProgramDef const& d : Drill::ProgramDrills())
        {
            std::string err;
            std::optional<Drill::Program> const p = Drill::ParseProgram(d.name, err);
            CHECK(p && p->loopMs > 0 && !p->entries.empty());
            if (!p)
            {
                std::printf("  drill %s: %s\n", d.name, err.c_str());
                continue;
            }
            // The text alone parses as well (it is what `.mech dummy` would get).
            CHECK(Drill::ParseProgram(d.program, err).has_value());
            bool described = false;
            for (Drill::ProgramEntry const& e : p->entries)
            {
                CHECK(e.endMs > e.startMs && e.endMs <= p->loopMs);
                CHECK(Drill::EntryWindowMs(*p, e) == e.endMs - e.startMs);
                CHECK(e.kind != Drill::EntryKind::Drill && e.kind != Drill::EntryKind::Barrage);
                if (e.kind == Drill::EntryKind::Describe)
                {
                    described = true;
                    CHECK(e.preset && e.preset->build && e.preset->name != std::string_view("none"));
                    CHECK(!e.followEntry || M5Creature(e.followEntry));
                }
                if (e.kind == Drill::EntryKind::Cast || e.kind == Drill::EntryKind::Debuff
                    || e.kind == Drill::EntryKind::Buff)
                    CHECK(M5Spell(e.spellId));
                if (e.auraId)
                    CHECK(M5Spell(e.auraId));
                if (e.kind == Drill::EntryKind::Add || e.kind == Drill::EntryKind::Buff
                    || e.kind == Drill::EntryKind::Swap || e.kind == Drill::EntryKind::Hold
                    || (e.kind == Drill::EntryKind::Cast && e.unitEntry))
                    CHECK(M5Creature(e.unitEntry));
                if (e.kind == Drill::EntryKind::Hold)
                    CHECK(e.unitEntry == Ids::NPC_DRILL_PRISON);
                // A drill's adds and prisons outlive neither the drill nor an explicit ttl.
                if (e.kind == Drill::EntryKind::Add || e.kind == Drill::EntryKind::Hold)
                    CHECK(Drill::EntryTtlMs(*p, e) > 0 && e.startMs + Drill::EntryTtlMs(*p, e) <= e.endMs);
            }
            CHECK(described);
            // The first entry of every drill (a sub-drill's first entry included) runs at 0: the describe or the
            // followed add, never a wait.
            CHECK(p->entries.front().startMs == 0 && p->entries.front().kind != Drill::EntryKind::Wait);
        }
    }

    // The timeline of the nested drills and the sim slice.
    void TestDescriptorDrillTimeline()
    {
        std::string err;
        // interrupts = interrupts-order (36 s) + gap + interrupts-fast (30 s) + gap.
        std::optional<Drill::Program> const p = Drill::ParseProgram("interrupts", err);
        CHECK(p && p->loopMs == 36000 + Drill::kProgramGapMs + 30000 + Drill::kProgramGapMs);
        if (p)
        {
            std::uint32_t const fastStart = 36000 + Drill::kProgramGapMs;
            std::uint32_t chains = 0;
            for (Drill::ProgramEntry const& e : p->entries)
            {
                bool const order = e.startMs < fastStart;
                CHECK(e.endMs == (order ? 36000u : fastStart + 30000u));
                if (e.kind == Drill::EntryKind::Cast)
                {
                    ++chains;
                    CHECK(e.everyMs >= 3000);
                    // The owner's three casts sit 3 s apart inside one 12 s cadence: never on top of each other.
                    if (!e.unitEntry)
                        CHECK(e.everyMs == 12000 && (e.startMs == 0 || e.startMs == 3000 || e.startMs == 6000));
                }
            }
            CHECK(chains == 5);
        }
        // `:n` runs a drill n times in a row, each run bounded by its own end.
        std::optional<Drill::Program> const twice = Drill::ParseProgram("ignore:2", err);
        CHECK(twice && twice->entries.size() == 6 && twice->loopMs == 2 * (30000 + Drill::kProgramGapMs));
        if (twice && twice->entries.size() == 6)
        {
            CHECK(twice->entries[3].startMs == 30000 + Drill::kProgramGapMs);
            CHECK(twice->entries[4].endMs == 2 * 30000 + Drill::kProgramGapMs);
            CHECK(Drill::EntryTtlMs(*twice, twice->entries[1]) == 30000);
        }
        // Mixed with the M4 grammar: a drill then a descriptor drill, the gap after each.
        std::optional<Drill::Program> const mixed = Drill::ParseProgram("smite,purges", err);
        CHECK(mixed && mixed->loopMs == 30000 + Drill::kProgramGapMs + 30000 + Drill::kProgramGapMs);
        CHECK(mixed && mixed->entries[1].startMs == 30000 + Drill::kProgramGapMs && mixed->entries[0].endMs == 0);
        // The sim slice: one loop fits a 120 s iteration with room for a second start.
        std::optional<Drill::Program> const slice = Drill::ParseProgram(Drill::kDescriptorProgram, err);
        CHECK(slice && slice->loopMs == 60000 + Drill::kProgramGapMs);
        if (slice)
        {
            std::set<Drill::EntryKind> kinds;
            for (Drill::ProgramEntry const& e : slice->entries)
                kinds.insert(e.kind);
            for (Drill::EntryKind k : { Drill::EntryKind::Describe, Drill::EntryKind::Add, Drill::EntryKind::Cast,
                Drill::EntryKind::Debuff, Drill::EntryKind::Buff, Drill::EntryKind::Hold, Drill::EntryKind::Wait })
                CHECK(kinds.count(k));
        }
        // Every drill in turn: the sum of the five listed drills.
        std::optional<Drill::Program> const all = Drill::ParseProgram(Drill::kDescriptorDrillsProgram, err);
        std::uint32_t sum = 0;
        for (std::string_view n : { "interrupts", "dispels", "purges", "targets", "ignore" })
            if (std::optional<Drill::Program> const one = Drill::ParseProgram(n, err))
                sum += one->loopMs;
        CHECK(all && all->loopMs == sum && sum > 300000 && sum < 600000);
    }

    // The WP-K grammar additions: describe phase= / follow=, debuff hp=, and the drill expansion's refusals.
    void TestDescriptorDrillGrammar()
    {
        std::string err;
        std::optional<Drill::Program> p = Drill::ParseProgram("describe targets-phase phase=2 follow=300405,"
            "debuff 205014 n=2 hp=40,describe:none:phase=0,wait:5", err);
        CHECK(p && p->entries.size() == 4);
        if (p && p->entries.size() == 4)
        {
            CHECK(p->entries[0].phase == std::optional<std::uint32_t>(2) && p->entries[0].followEntry == 300405);
            CHECK(p->entries[1].woundPct == 40 && p->entries[1].count == 2);
            CHECK(p->entries[2].phase == std::optional<std::uint32_t>(0) && !p->entries[2].followEntry);
            CHECK(p->entries[3].endMs == 0);
        }
        p = Drill::ParseProgram("describe none,debuff 205014", err);
        CHECK(p && !p->entries[0].phase && !p->entries[0].followEntry && p->entries[1].woundPct == 0);
        for (std::string_view bad : { "describe none phase=", "describe none phase=x", "describe none follow=0",
            "describe none phase=1 phase=2", "describe none follow=1 follow=2", "describe none hp=40",
            "debuff 205014 hp=0", "debuff 205014 hp=100", "add 300405 hp=40", "cast 205010 hp=40",
            "interrupts:0", "targets:x", "descriptor-drills:1.5" })
        {
            err.clear();
            CHECK(!Drill::ParseProgram(bad, err));
            CHECK(!err.empty());
        }
        // :n is clamped like drill cycles.
        p = Drill::ParseProgram("ignore:50", err);
        CHECK(p && p->entries.size() == 3 * Drill::kMaxDrillCycles);

        // M5 WP-F (R M-1): debuff apart=1 and with=<aura>.
        p = Drill::ParseProgram("debuff 205018 apart=1,debuff 205018 with=205014 n=2,debuff 205018 apart=0,wait:1",
            err);
        CHECK(p && p->entries.size() == 4);
        if (p && p->entries.size() == 4)
        {
            CHECK(p->entries[0].apart && p->entries[0].withAura == 0);
            CHECK(!p->entries[1].apart && p->entries[1].withAura == 205014 && p->entries[1].count == 2);
            CHECK(!p->entries[2].apart);
        }
        for (std::string_view bad : { "debuff 205018 apart=2", "debuff 205018 with=0", "debuff 205018 apart=x",
            "debuff 205018 with=1 with=2", "add 300405 apart=1", "cast 205010 with=205014", "hold 300407 apart=1" })
        {
            err.clear();
            CHECK(!Drill::ParseProgram(bad, err));
            CHECK(!err.empty());
        }
        // The `dispels` drill keeps the urgent stun and the deadline debuff apart from other Magic debuffs, but for
        // its last case, which puts the stun on a Magic Affliction on purpose (the follow-up dispel).
        p = Drill::ParseProgram("dispels", err);
        CHECK(p.has_value());
        if (p)
        {
            int apart = 0;
            int with = 0;
            for (Drill::ProgramEntry const& e : p->entries)
            {
                if (e.kind != Drill::EntryKind::Debuff || (e.spellId != 205018 && e.spellId != 205019))
                    continue;
                if (e.withAura)
                {
                    ++with;
                    CHECK(e.spellId == 205018 && e.withAura == 205014);
                }
                else
                {
                    ++apart;
                    CHECK(e.apart);
                }
            }
            CHECK(apart == 4 && with == 1);
        }
    }

    // What the presets mean, checked with the descriptor rules (MechanicDescriptor.h).
    void TestDrillPresets()
    {
        for (Desc::Preset const& preset : Desc::Presets())
        {
            CHECK(preset.build != nullptr);
            if (!preset.build)
                continue;
            Descriptor const d = preset.build();
            CHECK(PresetIdsAllowed(d));
            CHECK(std::string_view(preset.name) == "none" || !d.Empty());
        }
        Desc::Context ctx;
        Desc::UnitFacts owner;
        owner.guid = 1;
        owner.entry = Ids::NPC_DESCRIPTOR_DUMMY;
        Desc::UnitFacts caster;
        caster.guid = 2;
        caster.entry = Ids::NPC_DRILL_CASTER;
        Desc::UnitFacts add;
        add.guid = 3;
        add.entry = Ids::NPC_DRILL_ADD;
        Desc::UnitFacts copy;
        copy.guid = 4;
        copy.entry = Ids::NPC_DRILL_COPY;
        Desc::BotFacts const dps;
        Desc::BotFacts melee;
        melee.melee = true;

        // interrupts: high > mid > low; the do-not-kick cast never; the owner's casts listed for any caster.
        Descriptor const in = Desc::FindPreset("interrupts")->build();
        CHECK(Desc::IsDoNotKick(in, owner.entry, Ids::SPELL_DRILL_CAST_NO_KICK, owner, ctx));
        auto prio = [&](Desc::UnitFacts const& u, std::uint32_t spell) -> int
        {
            std::optional<std::size_t> const i = Desc::MatchInterrupt(in, u.entry, spell, u, ctx);
            return i ? in.interrupts[*i].priority : -1;
        };
        CHECK(prio(owner, Ids::SPELL_DRILL_CAST_HIGH) > prio(owner, Ids::SPELL_DRILL_CAST_MID));
        CHECK(prio(owner, Ids::SPELL_DRILL_CAST_MID) > prio(caster, Ids::SPELL_DRILL_CAST_LOW));
        CHECK(prio(caster, Ids::SPELL_DRILL_CAST_LOW) == 1 && prio(caster, Ids::SPELL_DRILL_CAST_HIGH) == 3);
        CHECK(prio(owner, Ids::SPELL_DRILL_CAST_LOW) == -1);    // low is the casters' only

        // dispels: the stun first, the deadline debuff urgent with 4 s left, normal ones after.
        Descriptor const di = Desc::FindPreset("dispels")->build();
        std::vector<Desc::Affliction> aff = {
            { 10, Ids::SPELL_DRILL_DEBUFF_MAGIC, Desc::Ms(20000), 0 },
            { 11, Ids::SPELL_DRILL_DEADLINE, Desc::Ms(6000), 0 },
            { 12, Ids::SPELL_DRILL_STUN, Desc::Ms(6000), 0 },
        };
        std::vector<Desc::DispelPick> picks = Desc::OrderDispels(di, aff, false);
        CHECK(picks.size() == 3 && picks[0].affliction == 2 && picks[0].urgent);
        CHECK(Desc::OrderDispels(di, aff, true).size() == 1);
        aff[1].remaining = Desc::Ms(3500);
        CHECK(Desc::OrderDispels(di, aff, true).size() == 2);

        // purges: the Magic buff before the Enrage.
        Descriptor const pu = Desc::FindPreset("purges")->build();
        std::optional<Desc::PurgePick> const purge = Desc::PickPurge(pu, { { 3, Ids::NPC_DRILL_ADD,
            Ids::SPELL_DRILL_ENRAGE }, { 3, Ids::NPC_DRILL_ADD, Ids::SPELL_DRILL_PURGEABLE } });
        CHECK(purge && purge->buff == 1);

        auto pick = [&](Descriptor const& d, std::vector<Desc::UnitFacts const*> const& units,
            Desc::BotFacts const& bot, Desc::Context const& c) -> Desc::Key
        {
            std::vector<Desc::TargetCandidate> cands;
            for (Desc::UnitFacts const* u : units)
                cands.push_back({ u, Desc::HeldRank::None });
            std::optional<Desc::TargetPick> const t = Desc::PickTarget(d, cands, owner.guid, bot, 0, c);
            return t ? cands[t->candidate].facts->guid : 0;
        };
        // targets: caster, then the add, then the dummy.
        Descriptor const tg = Desc::FindPreset("targets")->build();
        CHECK(pick(tg, { &owner, &add, &caster }, dps, ctx) == caster.guid);
        CHECK(pick(tg, { &owner, &add }, dps, ctx) == add.guid);
        // targets-aim: the aiming add first.
        Desc::UnitFacts aiming = add;
        aiming.guid = 5;
        aiming.auras = { Ids::SPELL_DRILL_AIMING };
        CHECK(pick(Desc::FindPreset("targets-aim")->build(), { &owner, &add, &aiming }, dps, ctx) == aiming.guid);
        // targets-zone: ignored without Marked, first with it.
        Descriptor const zone = Desc::FindPreset("targets-zone")->build();
        CHECK(Desc::IsIgnored(zone, add, ctx) && pick(zone, { &owner, &add }, dps, ctx) == owner.guid);
        Desc::UnitFacts inZone = add;
        inZone.auras = { Ids::SPELL_MARKED_DRILL };
        CHECK(!Desc::IsIgnored(zone, inZone, ctx) && pick(zone, { &owner, &inZone }, dps, ctx) == inZone.guid);
        // targets-phase: the dummy in phase 1, the caster in phase 2.
        Descriptor phase = Desc::FindPreset("targets-phase")->build();
        Desc::Context c1;
        c1.phase = phase.phase;
        CHECK(phase.phase == 1 && pick(phase, { &owner, &caster }, dps, c1) == owner.guid);
        Desc::Context c2;
        c2.phase = 2;
        CHECK(pick(phase, { &owner, &caster }, dps, c2) == caster.guid);
        // targets-follow, as RunDescribe turns it: the followed add by guid, its twin below the dummy.
        Descriptor follow = Desc::FindPreset("targets-follow")->build();
        CHECK(!follow.targets.empty() && follow.targets[0].entry == Ids::NPC_DRILL_ADD);
        CHECK(!follow.targets.empty() && follow.targets[0].priority > 0);
        follow.targets[0].guid = add.guid;
        follow.targets[0].entry = 0;
        Desc::UnitFacts twin = add;
        twin.guid = 6;
        CHECK(pick(follow, { &owner, &twin, &add }, dps, ctx) == add.guid);
        CHECK(pick(follow, { &owner, &twin }, dps, ctx) == owner.guid);
        // targets-together: the one with more health left while they are more than 10% apart.
        Descriptor const tog = Desc::FindPreset("targets-together")->build();
        Desc::UnitFacts hurtCaster = caster;
        hurtCaster.healthPct = 50.f;
        Desc::UnitFacts fullAdd = add;
        fullAdd.healthPct = 90.f;
        CHECK(pick(tog, { &owner, &hurtCaster, &fullAdd }, dps, ctx) == fullAdd.guid);
        // targets-split: ranged on the caster, melee on the add.
        Descriptor const split = Desc::FindPreset("targets-split")->build();
        CHECK(pick(split, { &owner, &caster, &add }, dps, ctx) == caster.guid);
        CHECK(pick(split, { &owner, &caster, &add }, melee, ctx) == add.guid);
        // targets-breakout: the tank's prison before the healer's.
        Descriptor const bo = Desc::FindPreset("targets-breakout")->build();
        Desc::UnitFacts p1;
        p1.guid = 7;
        p1.entry = Ids::NPC_DRILL_PRISON;
        Desc::UnitFacts p2 = p1;
        p2.guid = 8;
        std::vector<Desc::TargetCandidate> const prisons = { { &owner, Desc::HeldRank::None },
            { &p1, Desc::HeldRank::Healer }, { &p2, Desc::HeldRank::Tank } };
        std::optional<Desc::TargetPick> const freed = Desc::PickTarget(bo, prisons, owner.guid, dps, 0, ctx);
        CHECK(freed && prisons[freed->candidate].facts->guid == p2.guid);
        // ignore: the copies.
        CHECK(Desc::IsIgnored(Desc::FindPreset("ignore")->build(), copy, ctx));
        // descriptor: every list, prisons before the aiming add before the casters before the dummy.
        Descriptor const all = Desc::FindPreset("descriptor")->build();
        CHECK(!all.interrupts.empty() && !all.dispels.empty() && !all.purges.empty() && !all.targets.empty()
            && !all.ignores.empty());
        CHECK(Desc::IsIgnored(all, copy, ctx));
        CHECK(pick(all, { &owner, &add, &caster, &aiming }, dps, ctx) == aiming.guid);
        CHECK(pick(all, { &owner, &add, &caster }, dps, ctx) == caster.guid);
        CHECK(pick(all, { &owner, &add }, dps, ctx) == owner.guid);    // the purge adds rank below the dummy
    }

    void CheckU(Vec3 const& origin, float facing, float gap, float depth, float r, std::size_t expected)
    {
        std::vector<Vec3> const mines = Drill::UMinefield(origin, facing, gap, depth, r);
        CHECK(mines.size() == expected);
        float const side = gap / 2.f + r;
        // Overlapping: every mine has a neighbor at most `r` away, and every point of the walls' center lines is
        // within r / 2 of a mine center (nothing walks through).
        for (std::size_t i = 0; i < mines.size(); ++i)
            CHECK(Nearest(mines, mines[i], i) <= r + kEps);
        for (float a = 0.f; a <= depth; a += 0.25f)
            for (float const left : { side, -side })
                CHECK(Nearest(mines, Drill::Shifted(origin, facing, a, left)) <= r / 2.f + kEps);
        for (float l = -side; l <= side; l += 0.25f)
            CHECK(Nearest(mines, Drill::Shifted(origin, facing, depth, l)) <= r / 2.f + kEps);
        // The pocket and the mouth are clear.
        for (float a = -2.f; a < depth - r - 0.01f; a += 0.5f)
            for (float l = -gap / 2.f + 0.01f; l < gap / 2.f; l += 0.5f)
                CHECK(Nearest(mines, Drill::Shifted(origin, facing, a, l)) >= r - kEps);
        // Nothing behind the mouth side or beyond the closed end.
        for (Vec3 const& m : mines)
        {
            float const dx = m.x - origin.x;
            float const dy = m.y - origin.y;
            float const ahead = dx * std::cos(facing) + dy * std::sin(facing);
            float const left = -dx * std::sin(facing) + dy * std::cos(facing);
            CHECK(ahead >= -kEps && ahead <= depth + kEps);
            CHECK(std::fabs(left) <= side + kEps);
            CHECK_NEAR(m.z, origin.z, kEps);
        }
    }

    void TestUMinefield()
    {
        // Defaults: gap 8, depth 12, radius 3 -> 5 mines per arm, 4 more on the closed end.
        CheckU({ 0.f, 0.f, 0.f }, 0.f, 8.f, 12.f, 3.f, 14);
        CheckU({ 100.f, -40.f, 12.f }, 1.3f, 8.f, 12.f, 3.f, 14);
        CheckU({ -5.f, 7.f, 0.f }, Geo::kPi, 6.f, 10.f, 2.f, 16);
        CheckU({ 0.f, 0.f, 0.f }, 0.5f, 5.f, 7.f, 2.5f, 11);    // spacing below r on both walls
        CHECK(Drill::UMinefield({ 0.f, 0.f, 0.f }, 0.f, 8.f, 12.f, 0.f).empty());
        CHECK(Drill::UMinefield({ 0.f, 0.f, 0.f }, 0.f, 0.f, 12.f, 3.f).empty());
        CHECK(Drill::UMinefield({ 0.f, 0.f, 0.f }, 0.f, 8.f, 0.f, 3.f).empty());
        // The same U turned: facing pi/2 maps (ahead, left) onto (+y, -x).
        std::vector<Vec3> const a = Drill::UMinefield({ 0.f, 0.f, 0.f }, 0.f, 8.f, 12.f, 3.f);
        std::vector<Vec3> const b = Drill::UMinefield({ 0.f, 0.f, 0.f }, Geo::kPi / 2.f, 8.f, 12.f, 3.f);
        CHECK(a.size() == b.size());
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        {
            CHECK_NEAR(b[i].x, -a[i].y, kEps);
            CHECK_NEAR(b[i].y, a[i].x, kEps);
        }
    }
}

int main()
{
    TestStepSchedule();
    TestCyclesAndOwner();
    TestPrograms();
    TestDescriptorEntries();
    TestDescriptorDrillNames();
    TestDescriptorDrillsParse();
    TestDescriptorDrillTimeline();
    TestDescriptorDrillGrammar();
    TestDrillPresets();
    TestUMinefield();
    return Report("drill_plan_test");
}
