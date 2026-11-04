// Standalone tests for src/MechanicDrillPlan.h (STAGE-M4-HANDOFF §3.2, §3.7, WP-B): the drill runner's placement and
// timing rules (step schedule, cycles, the owner moved onto the boss stand-in), the program grammar and timeline of
// the training dummy and the sim, and the fixed U of `.mech minefield layout=u`. Build: tests/run.sh.

#include "check.h"
#include "MechanicDrillPlan.h"
#include <cmath>
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
    TestUMinefield();
    return Report("drill_plan_test");
}
