// Standalone tests for the chase of src/MechanicDrill.h (Shadowfang Keep build §3.9, WP-GD): the `sfk-hunt` drill
// against the boss doc rows it reproduces (Odo's Hunt OD1 at 80% / 95%, the bell OD3, the landing OD4), the bell
// points, the runner's dmg= rule for bell soaks, the chase step and the desk model the sweep's bar rests on.
// Build: tests/run.sh.

#include "check.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace BossMechanics;
using Geo::Vec3;

namespace
{
    constexpr float kEps = 1e-3f;

    float Key(std::string_view line, std::string_view key, float def)
    {
        std::optional<std::string_view> v = Drill::Value(line, key);
        return v ? Drill::Number(*v).value_or(-1.f) : def;
    }

    bool Is(std::string_view line, std::string_view key, std::string_view value)
    {
        std::optional<std::string_view> v = Drill::Value(line, key);
        return v && *v == value;
    }

    std::string_view Shape(std::string_view line)
    {
        std::vector<std::string_view> const t = Drill::Tokens(line);
        return t.empty() ? std::string_view() : t[0];
    }

    // The size and warning tokens.
    float Size(std::string_view line)
    {
        std::vector<std::string_view> const t = Drill::Tokens(line);
        return t.size() >= 2 ? Drill::Number(t[1]).value_or(-1.f) : -1.f;
    }

    float Warn(std::string_view line)
    {
        return static_cast<float>(Drill::WarnMs(line));
    }

    // ---- the drill table -------------------------------------------------------------------------------------------
    void TestSfkHuntTable()
    {
        Drill::Def const* found = Drill::Find("sfk-hunt");
        CHECK(found != nullptr);
        if (!found)
            return;
        Drill::Def const& d = *found;
        CHECK(d.cycleMs == 30000 && d.defaultCycles == 2 && !d.ownerAtCreature);
        CHECK(std::string_view(d.summary).size() > 0);
        CHECK(d.steps.size() == 3);
        if (d.steps.size() != 3)
            return;
        std::string_view const hunt = d.steps[0].line;
        std::string_view const bell = d.steps[1].line;
        std::string_view const heroic = d.steps[2].line;

        // OD1 normal: a 3 yd circle on the chaser, a 2 s howl, 10 s at 80%, a random non-tank, passes every 200 ms.
        CHECK(d.steps[0].atMs == 0);
        CHECK(Shape(hunt) == "hunt" && Size(hunt) == 3.f && Warn(hunt) == 2000.f);
        CHECK_NEAR(Key(hunt, "speed", 0.f), Drill::kHuntRateNormal, kEps);
        CHECK(Key(hunt, "persist", 0.f) == 10000.f && Key(hunt, "tick", 0.f) == 200.f);
        CHECK(Is(hunt, "pick", "nontank"));
        CHECK(Key(hunt, "tag", 0.f) == static_cast<float>(Drill::kHuntTagCatchNormal));
        // Maul is a DoT with a stun: no Weakened (SK ruling 17).
        CHECK(!Drill::Value(hunt, "weak"));

        // OD3: a 1-body Soak 2 yd on one of three bells 15 yd apart, not the tank (the hunted is left out at run time),
        // registered after the hunt (so the run's hunt state is set), resolving inside the chase.
        CHECK(d.steps[1].atMs == 500);
        CHECK(Shape(bell) == "soak" && Size(bell) == 2.f && Key(bell, "need", 0.f) == 1.f);
        CHECK(Is(bell, "role", "nontank"));
        CHECK(Key(bell, "bell", 0.f) == 3.f && Key(bell, "apart", 0.f) == 15.f);
        CHECK(Key(bell, "tag", 0.f) == static_cast<float>(Drill::kHuntTagBell));
        float const chaseStart = static_cast<float>(d.steps[0].atMs) + Warn(hunt);
        float const chaseEnd = chaseStart + Key(hunt, "persist", 0.f);
        float const bellResolve = static_cast<float>(d.steps[1].atMs) + Warn(bell);
        CHECK(d.steps[1].atMs > d.steps[0].atMs);
        CHECK(bellResolve > chaseStart && bellResolve < chaseEnd);
        // The walk to a bell and the 1 s ring fit the warning: 3 s of walking (21 yd) leaves 1.5 s at the bell.
        CHECK(Warn(bell) - 1000.f >= 3000.f);
        // OD4: the bound lands 2 s later with a 6 yd circle, Weakened for those caught (the ringer must step clear),
        // still inside the chase window the hunt gave up.
        CHECK(Key(bell, "land", 0.f) == 6.f && Key(bell, "jump", 0.f) == 2000.f && Is(bell, "weak", "1"));
        CHECK(bellResolve + Key(bell, "jump", 0.f) <= chaseEnd);
        CHECK(Drill::BellLandingTag(Drill::kHuntTagBell) == Drill::kHuntTagLanding);
        CHECK(Drill::BellRingerTag(Drill::kHuntTagBell) == Drill::kHuntTagRinger);

        // OD1 heroic: 95%, no bell (spent), after the first hunt is over, over before the cycle ends.
        CHECK(d.steps[2].atMs == 15000);
        CHECK(Shape(heroic) == "hunt" && Size(heroic) == 3.f && Warn(heroic) == 2000.f);
        CHECK_NEAR(Key(heroic, "speed", 0.f), Drill::kHuntRateHeroic, kEps);
        CHECK(Key(heroic, "persist", 0.f) == 10000.f && Key(heroic, "tick", 0.f) == 200.f);
        CHECK(Is(heroic, "pick", "nontank"));
        CHECK(Key(heroic, "tag", 0.f) == static_cast<float>(Drill::kHuntTagCatchHeroic));
        CHECK(chaseEnd + 1000.f <= static_cast<float>(d.steps[2].atMs));
        CHECK(static_cast<float>(d.steps[2].atMs) + Warn(heroic) + Key(heroic, "persist", 0.f)
            <= static_cast<float>(d.cycleMs));
        for (Drill::Step const& s : d.steps)
            if (s.atMs >= d.steps[2].atMs)
                CHECK(Shape(s.line) != "soak");

        // Every step deals damage through the runner's dmg= (the bell's landing too); Maul and the landing are 35%.
        for (Drill::Step const& s : d.steps)
        {
            CHECK(Drill::DamageStep(s.line));
            CHECK(Key(s.line, "pct", 0.f) == 35.f);
            CHECK(!Drill::Value(s.line, "dmg"));
        }

        // The five ledger tags are distinct.
        std::uint32_t const tags[] = { Drill::kHuntTagCatchNormal, Drill::kHuntTagBell, Drill::kHuntTagLanding,
            Drill::kHuntTagRinger, Drill::kHuntTagCatchHeroic };
        for (std::size_t i = 0; i < 5; ++i)
            for (std::size_t j = i + 1; j < 5; ++j)
                CHECK(tags[i] != tags[j]);
    }

    // The sweep's program: `sfk-hunt:4` is one drill entry of 120 s (a sim iteration runs its program once), with
    // eight hunts (four at each speed) and four bells.
    void TestSweepProgram()
    {
        std::string err;
        std::optional<Drill::Program> const p = Drill::ParseProgram("sfk-hunt:4", err);
        CHECK(p && err.empty());
        if (!p)
            return;
        CHECK(p->entries.size() == 1);
        if (p->entries.size() != 1)
            return;
        Drill::ProgramEntry const& e = p->entries[0];
        CHECK(e.kind == Drill::EntryKind::Drill && e.drill == Drill::Find("sfk-hunt"));
        CHECK(e.cycles == 4 && e.lengthMs == 120000);
        std::vector<Drill::ScheduledStep> const s = Drill::StepSchedule(*e.drill, e.cycles);
        CHECK(s.size() == 12);
        std::uint32_t hunts = 0;
        std::uint32_t bells = 0;
        for (Drill::ScheduledStep const& st : s)
        {
            std::string_view const line = e.drill->steps[st.step].line;
            hunts += Shape(line) == "hunt";
            bells += Shape(line) == "soak";
            // The last hunt is over inside 120 s.
            CHECK(static_cast<float>(st.delayMs) + Warn(line) + Key(line, "persist", 0.f) <= 120000.f);
        }
        CHECK(hunts == 8 && bells == 4);
        // The drill's own default (`.mech drill sfk-hunt`, the training dummy): one minute.
        std::optional<Drill::Program> const one = Drill::ParseProgram("sfk-hunt", err);
        CHECK(one && one->entries.size() == 1 && one->entries[0].lengthMs == 60000);
    }

    // ---- bell points -----------------------------------------------------------------------------------------------
    void TestBellPoints()
    {
        Vec3 const center{ -236.7f, 2146.1f, 100.1f };     // Odo's spawn (DB-FACTS)
        // Three bells 15 yd apart: an 8.66 yd ring, every pair 15 yd apart.
        CHECK_NEAR(Drill::BellRing(15.f, 3), 15.f / std::sqrt(3.f), kEps);
        for (float facing : { 0.f, 1.1f, 4.0f })
        {
            std::vector<Vec3> const b = Drill::BellPoints(center, facing, 3, 15.f);
            CHECK(b.size() == 3);
            if (b.size() != 3)
                continue;
            for (std::size_t i = 0; i < 3; ++i)
            {
                CHECK_NEAR(Geo::Dist2d(b[i].x, b[i].y, center.x, center.y), Drill::BellRing(15.f, 3), kEps);
                CHECK_NEAR(b[i].z, center.z, kEps);
                for (std::size_t j = i + 1; j < 3; ++j)
                    CHECK_NEAR(Geo::Dist2d(b[i].x, b[i].y, b[j].x, b[j].y), 15.f, 1e-2);
            }
            // The first bell at the facing bearing (fixed for a drill run).
            CHECK_NEAR(std::atan2(b[0].y - center.y, b[0].x - center.x), std::remainder(facing, Geo::kTwoPi), kEps);
            // The 6 yd landing at one bell never reaches the others: a ringer can always step toward another bell.
            CHECK(15.f > 6.f + 2.f);
        }
        // Neighbors exactly `apart` for every allowed count.
        for (std::uint32_t n = Drill::kMinBells; n <= Drill::kMaxBells; ++n)
            CHECK_NEAR(Drill::RingSpacing(Drill::BellRing(19.f, n), n), 19.f, 1e-2);
        CHECK(Drill::BellRing(15.f, 1) == 0.f && Drill::BellRing(15.f, 0) == 0.f);
    }

    // ---- the runner's dmg= rule ------------------------------------------------------------------------------------
    void TestDamageStep()
    {
        CHECK(Drill::DamageStep("hunt 3 2000 speed=0.8 persist=10000"));
        CHECK(Drill::DamageStep("circle 5 2000 at=each"));
        CHECK(Drill::DamageStep("soak 2 5000 need=1 bell=3"));
        CHECK(!Drill::DamageStep("soak 2 5000 need=1"));
        CHECK(!Drill::DamageStep("soak 2 5000 need=1 bell=0"));
        CHECK(!Drill::DamageStep("stack 8 6000"));
        CHECK(!Drill::DamageStep("spot 3 9000 ring=12"));
        CHECK(!Drill::DamageStep("marker 2 pick=nontank"));
        CHECK(!Drill::DamageStep(""));
        CHECK(Drill::DamageShape("hunt"));
    }

    // ---- the chase -------------------------------------------------------------------------------------------------
    void TestChaseStep()
    {
        Vec3 const a{ 0.f, 0.f, 5.f };
        Vec3 const b{ 10.f, 0.f, 7.f };
        Vec3 const s = Drill::ChaseStep(a, b, Drill::HuntSpeedYdS(Drill::kHuntRateNormal), 0.5f);
        CHECK_NEAR(s.x, 2.8f, kEps);
        CHECK_NEAR(s.y, 0.f, kEps);
        CHECK_NEAR(s.z, 5.f, kEps);         // the chaser keeps its own height (the path finder places it)
        // Never past the target.
        Vec3 const t = Drill::ChaseStep(a, b, 100.f, 1.f);
        CHECK_NEAR(t.x, 10.f, kEps);
        CHECK_NEAR(t.y, 0.f, kEps);
        Vec3 const same = Drill::ChaseStep(b, b, 5.f, 1.f);
        CHECK_NEAR(same.x, b.x, kEps);
        // The speeds the doc names: 80% and 95% of a player's 7 yd/s, both below it.
        CHECK_NEAR(Drill::HuntSpeedYdS(Drill::kHuntRateNormal), 5.6f, kEps);
        CHECK_NEAR(Drill::HuntSpeedYdS(Drill::kHuntRateHeroic), 6.65f, kEps);
        CHECK(Drill::HuntSpeedYdS(Drill::kHuntRateHeroic) < Drill::kPlayerRunYdS);
        CHECK(Drill::kHuntRateHeroic < Drill::kMaxHuntRate);
    }

    // The bar's premises (§3.9): a hunted player who walks away is never caught at 80% and, barely, at 95%; one who
    // stands is caught; the slack a bot has to start walking is (gap - 3) / chaser speed.
    void TestChaseModel()
    {
        Drill::ChaseModel m;
        // Walks away from the end of the howl, starting just outside the circle: never caught at either speed.
        m.startGap = 3.5f;
        m.reactMs = m.warnMs;
        CHECK(!Drill::CatchMs(m));
        m.chaserYdS = Drill::HuntSpeedYdS(Drill::kHuntRateHeroic);
        CHECK(!Drill::CatchMs(m));

        // Stands still: caught at the first pass with the gap closed, (gap - r) / speed after the howl.
        m.chaserYdS = Drill::HuntSpeedYdS(Drill::kHuntRateNormal);
        m.fleeYdS = 0.f;
        m.startGap = 8.6f;
        std::optional<std::uint32_t> const caught = Drill::CatchMs(m);
        CHECK(caught.has_value());
        if (caught)
        {
            CHECK(*caught >= m.warnMs + 1000 && *caught <= m.warnMs + 1000 + m.tickMs);
            CHECK((*caught - m.warnMs) % m.tickMs == 0);
        }
        // Already inside at the end of the howl: caught at the first pass.
        m.startGap = 2.f;
        CHECK(Drill::CatchMs(m) == std::optional<std::uint32_t>(m.warnMs));

        // A late start: from 8 yd at 80%, the slack is (8 - 3) / 5.6 = 0.89 s after the howl.
        m.fleeYdS = Drill::kPlayerRunYdS;
        m.startGap = 8.f;
        m.reactMs = m.warnMs + 600;
        CHECK(!Drill::CatchMs(m));
        m.reactMs = m.warnMs + 1200;
        CHECK(Drill::CatchMs(m).has_value());
        // At 95% the same start leaves (8 - 3) / 6.65 = 0.75 s.
        m.chaserYdS = Drill::HuntSpeedYdS(Drill::kHuntRateHeroic);
        m.reactMs = m.warnMs + 500;
        CHECK(!Drill::CatchMs(m));
        m.reactMs = m.warnMs + 1000;
        CHECK(Drill::CatchMs(m).has_value());

        // What a full 10 s walk-away gains over the chaser: 14 yd at 80%, only 3.5 yd at 95% ("barely").
        float const chaseS = static_cast<float>(m.chaseMs) / 1000.f;
        CHECK_NEAR((Drill::kPlayerRunYdS - Drill::HuntSpeedYdS(Drill::kHuntRateNormal)) * chaseS, 14.f, 1e-2);
        CHECK_NEAR((Drill::kPlayerRunYdS - Drill::HuntSpeedYdS(Drill::kHuntRateHeroic)) * chaseS, 3.5f, 1e-2);

        // A chase that ends before the chaser arrives catches nobody.
        m.fleeYdS = 0.f;
        m.chaserYdS = Drill::HuntSpeedYdS(Drill::kHuntRateNormal);
        m.startGap = 3.f + Drill::HuntSpeedYdS(Drill::kHuntRateNormal) * chaseS + 2.f;
        CHECK(!Drill::CatchMs(m));
    }
}

int main()
{
    TestSfkHuntTable();
    TestSweepProgram();
    TestBellPoints();
    TestDamageStep();
    TestChaseStep();
    TestChaseModel();
    return Report("hunt_test");
}
