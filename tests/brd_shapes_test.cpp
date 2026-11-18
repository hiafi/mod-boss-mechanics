// Standalone tests for the Blackrock Depths shapes of src/MechanicDrill.h (Blackrock Depths build §3.9, WP-G1): the
// `blackrock-depths-shapes` drill (wave 1 rows walk, leap, axe-return, rupture, ward, bands; hide is WP-HK's) against
// the build spec rows it reproduces (Roccor RO1 + RO3, Grebmar GR3 + GR4, Gorosh GO2, Ok'thor OK1, Loregrain LG1-LG3,
// Incendius IN8 / IN11 / IN13 with the §7 #14 A strip), the drill-only steps it adds (`walk`, `leash`: ParseWalk,
// ParseLeash, WalkPoint, OutOfLeash), a line's `offset=` geometry, and the sweep's program. Build: tests/run.sh.

#include "check.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include "MechanicGeometry.h"
#include "MechanicJudge.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
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
    constexpr float kMarginYd = 0.5f;           // BossMechanics.Bots.SafetyMarginYd default (M2)
    constexpr float kRunYdS = 7.f;              // a player's run speed
    constexpr float kReactS = 0.3f;             // the bots' reaction allowance the other drill tests use
    // The sim's layout (SimMechanicsScenario.cpp): the tank 3 yd and the DPS 12 yd from the boss, the healer 18 yd,
    // on the reference's side (the reference faces the boss: the run's facing points from the group to the boss).
    constexpr float kSimTankYd = 3.f;
    constexpr float kSimDpsYd = 12.f;
    constexpr float kSimHealerYd = 18.f;

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

    std::string_view ShapeOf(std::string_view line)
    {
        std::vector<std::string_view> const t = Drill::Tokens(line);
        return t.empty() ? std::string_view() : t[0];
    }

    float Size(std::string_view line)
    {
        std::vector<std::string_view> const t = Drill::Tokens(line);
        return t.size() >= 2 ? Drill::Number(t[1]).value_or(-1.f) : -1.f;
    }

    std::uint32_t Tag(std::string_view line)
    {
        return static_cast<std::uint32_t>(Key(line, "tag", 0.f));
    }

    // shift=<ahead>:<left> (signed).
    std::pair<float, float> ShiftOf(std::string_view line)
    {
        std::optional<std::string_view> const v = Drill::Value(line, "shift");
        if (!v)
            return { 0.f, 0.f };
        std::size_t const colon = v->find(':');
        return { std::stof(std::string(v->substr(0, colon))), std::stof(std::string(v->substr(colon + 1))) };
    }

    Drill::Def const& Shapes()
    {
        static Drill::Def const* d = Drill::Find("blackrock-depths-shapes");
        return *d;
    }

    struct At
    {
        std::size_t index = 0;
        std::string_view line;
        std::uint32_t startMs = 0;
        std::uint32_t endMs = 0;        // resolve plus persist (a walk or leash: its length)
    };

    std::vector<At> Cycle(std::uint32_t cycle)
    {
        std::vector<At> out;
        Drill::Def const& d = Shapes();
        std::vector<std::optional<std::uint32_t>> const offsets = Drill::StepOffsets(d, cycle);
        for (std::size_t i = 0; i < d.steps.size(); ++i)
            if (offsets[i])
            {
                std::string_view const line = d.steps[i].line;
                std::uint32_t const end = *offsets[i] + Drill::WarnMs(line)
                    + static_cast<std::uint32_t>(Key(line, "persist", 0.f));
                out.push_back({ i, line, *offsets[i], end });
            }
        return out;
    }

    std::vector<At> WithTag(std::uint32_t cycle, std::uint32_t tag)
    {
        std::vector<At> out;
        for (At const& a : Cycle(cycle))
            if (Tag(a.line) == tag)
                out.push_back(a);
        return out;
    }

    std::vector<At> WithShape(std::uint32_t cycle, std::string_view shape)
    {
        std::vector<At> out;
        for (At const& a : Cycle(cycle))
            if (ShapeOf(a.line) == shape)
                out.push_back(a);
        return out;
    }

    // The pose of a point line as the runtime builds it: the run's facing turned by offset= (counterclockwise), the
    // source shift= from the selected unit along the turned facing.
    Geo::Pose LinePose(std::string_view line, Vec3 const& boss, float facing)
    {
        float const f = Geo::WrapPi(facing + Geo::DegToRad(Key(line, "offset", 0.f)));
        auto const [ahead, left] = ShiftOf(line);
        Vec3 const src = Drill::Shifted(boss, f, ahead, left);
        return { src.x, src.y, src.z, f };
    }

    bool InLine(std::string_view line, Vec3 const& boss, float facing, Vec3 const& p)
    {
        return Geo::Contains(Shape::Line(Size(line), Key(line, "width", 4.f)), LinePose(line, boss, facing), p);
    }

    // A point at `ahead` along the run's facing and `left` to its left of the boss.
    Vec3 Frame(Vec3 const& boss, float facing, float ahead, float left)
    {
        return Drill::Shifted(boss, facing, ahead, left);
    }

    // ---- the table ------------------------------------------------------------------------------------------------
    void TestTable()
    {
        Drill::Def const* d = Drill::Find("blackrock-depths-shapes");
        CHECK(d != nullptr);
        if (!d)
            return;
        CHECK(d->cycleMs == Drill::kBrdCycleMs && d->defaultCycles == Drill::kBrdCycles);
        CHECK(d->cycleMs * d->defaultCycles == 120000u);
        CHECK(!d->ownerAtCreature);

        std::set<std::uint32_t> tags;
        for (Drill::Step const& s : d->steps)
        {
            if (ShapeOf(s.line) == "walk")
                continue;
            tags.insert(Tag(s.line));
            // every step runs in one of the two cycles
            CHECK(Is(s.line, "cycle", "0/2") || Is(s.line, "cycle", "1/2"));
        }
        CHECK(tags == (std::set<std::uint32_t>{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }));
        CHECK(tags.count(Drill::kBrdTagHide) == 1);     // WP-HK's row (tests/hide_test.cpp)
        // Gated: the leashes, both leap shapes, the locked return, both ruptures, the tide, the three bands. Reported:
        // the Rampage (a first shape with no Weakened), the axe's landing and its shown line, the ward's Stack.
        std::set<std::uint32_t> const bar(std::begin(Drill::kBrdBarTags), std::end(Drill::kBrdBarTags));
        CHECK(bar == (std::set<std::uint32_t>{ 2, 3, 4, 5, 8, 9, 10, 12, 13, 14, 15, 16 }));
        for (std::uint32_t t = 0; t <= 20; ++t)
            CHECK(Drill::BrdBarTag(t) == (bar.count(t) == 1));

        for (std::uint32_t cycle = 0; cycle < 4; ++cycle)
        {
            std::uint32_t last = 0;
            for (At const& a : Cycle(cycle))
            {
                CHECK(a.startMs >= last);
                last = a.startMs;
                CHECK(a.endMs <= d->cycleMs);
                CHECK(Is(a.line, "cycle", cycle % 2 ? "1/2" : "0/2"));
            }
        }
        // Cycle 0 keeps 52.5-59.5 s for WP-HK's hide row alone.
        for (At const& a : Cycle(0))
            CHECK(Tag(a.line) == Drill::kBrdTagHide || a.endMs <= 52500u || a.startMs >= 59500u);
        // Every damage step carries a heroic percent; the get-in Stack none.
        for (Drill::Step const& s : d->steps)
            if (Drill::DamageStep(s.line))
                CHECK(Drill::Value(s.line, "pct").has_value());
    }

    // ---- walk and leash --------------------------------------------------------------------------------------------
    void TestParseWalk()
    {
        std::optional<Drill::WalkStep> w = Drill::ParseWalk("walk 30 20000");
        CHECK(w && w->yd == 30.f && w->ms == 20000u && w->offsetDeg == 0.f);
        w = Drill::ParseWalk("walk 30 20000 offset=180");
        CHECK(w && w->offsetDeg == 180.f);
        CHECK(Drill::ParseWalk("walk 60 250").has_value());
        for (char const* bad : { "walk", "walk 30", "walk 0 20000", "walk 61 20000", "walk 30 249", "walk 30 120001",
            "walk 30 20000 offset=361", "walk 30 20000 offset=-90", "walk 30 20000 speed=2", "walk 30 20000 offset",
            "walk 30 20000 offset=90 offset=90", "leash 30 20000", "walk -3 20000", "walk 30 20000 tag=1" })
            CHECK(!Drill::ParseWalk(bad));
    }

    void TestParseLeash()
    {
        std::optional<Drill::LeashStep> l = Drill::ParseLeash("leash 8 20000 role=tank tag=2");
        CHECK(l && l->yd == 8.f && l->ms == 20000u && l->tanksOnly && l->ledgerTag == 2u
            && l->tickMs == Drill::kDefaultLeashTickMs);
        l = Drill::ParseLeash("leash 40 20000 tag=3");
        CHECK(l && !l->tanksOnly && l->ledgerTag == 3u);
        l = Drill::ParseLeash("leash 40 20000 role=any tick=500 tag=3");
        CHECK(l && !l->tanksOnly && l->tickMs == 500u);
        for (char const* bad : { "leash 8 20000", "leash 8 20000 tag=0", "leash 8 20000 tag=1.5", "leash 0 20000 tag=1",
            "leash 8 0 tag=1", "leash 8 20000 role=healer tag=1", "leash 8 20000 tick=100 tag=1",
            "leash 8 1000 tick=2000 tag=1", "leash 8 500 tag=1", "leash 8 20000 tag=1 pct=10",
            "leash 8 20000 tag=1 tag=2", "walk 8 20000 tag=1" })
            CHECK(!Drill::ParseLeash(bad));
        CHECK(Drill::LeashTicks(20000, 1000) == 20u && Drill::LeashTicks(20000, 3000) == 6u
            && Drill::LeashTicks(5, 0) == 0u);
        Vec3 const o{ 0.f, 0.f, 0.f };
        CHECK(!Drill::OutOfLeash(o, { 8.f, 0.f, 0.f }, 8.f));
        CHECK(Drill::OutOfLeash(o, { 8.01f, 0.f, 0.f }, 8.f));
        CHECK(!Drill::OutOfLeash(o, { 6.f, 5.29f, 30.f }, 8.f));    // the ground plane: height is ignored
    }

    void TestWalkGeometry()
    {
        CHECK(Drill::WalkSteps(20000) == 80u && Drill::WalkSteps(250) == 1u && Drill::WalkSteps(260) == 2u
            && Drill::WalkSteps(100) == 1u);
        CHECK(Drill::WalkStepAt(1, 20000) == 250u && Drill::WalkStepAt(80, 20000) == 20000u
            && Drill::WalkStepAt(2, 260) == 260u);
        Vec3 const s{ 10.f, -5.f, 3.f };
        float const h = 0.6f;
        Vec3 const half = Drill::WalkPoint(s, h, 30.f, 20000, 10000);
        CHECK_NEAR(Geo::Dist2d(s.x, s.y, half.x, half.y), 15.f, kEps);
        Vec3 const end = Drill::WalkPoint(s, h, 30.f, 20000, 20000);
        CHECK_NEAR(end.x, s.x + 30.f * std::cos(h), kEps);
        CHECK_NEAR(end.y, s.y + 30.f * std::sin(h), kEps);
        Vec3 const past = Drill::WalkPoint(s, h, 30.f, 20000, 25000);     // held at the end
        CHECK_NEAR(past.x, end.x, kEps);
        CHECK_NEAR(Geo::Dist2d(s.x, s.y, Drill::WalkPoint(s, h, 30.f, 20000, 0).x,
            Drill::WalkPoint(s, h, 30.f, 20000, 0).y), 0.f, kEps);
        // One relocation never jumps farther than a step's worth at 1.5 yd/s.
        for (std::uint32_t k = 1; k <= Drill::WalkSteps(20000); ++k)
        {
            Vec3 const a = Drill::WalkPoint(s, h, 30.f, 20000, Drill::WalkStepAt(k - 1, 20000));
            Vec3 const b = Drill::WalkPoint(s, h, 30.f, 20000, Drill::WalkStepAt(k, 20000));
            CHECK(Geo::Dist2d(a.x, a.y, b.x, b.y) <= Drill::kRoccorWalkYdS * Drill::kWalkStepMs / 1000.f + kEps);
        }
    }

    // RO1 + RO3: a Rampage, the out leg with both leashes, a Rampage back, the back leg with both leashes, home again.
    void TestWalkRow()
    {
        std::vector<At> const walks = WithShape(0, "walk");
        std::vector<At> const leashes = WithShape(0, "leash");
        std::vector<At> const rampages = WithTag(0, Drill::kBrdTagRampage);
        CHECK(walks.size() == 2 && leashes.size() == 4 && rampages.size() == 2);
        CHECK(WithShape(1, "walk").empty() && WithShape(1, "leash").empty());
        if (walks.size() != 2 || rampages.size() != 2 || leashes.size() != 4)
            return;
        std::optional<Drill::WalkStep> const out = Drill::ParseWalk(Drill::StripPlanKeys(walks[0].line));
        std::optional<Drill::WalkStep> const back = Drill::ParseWalk(Drill::StripPlanKeys(walks[1].line));
        CHECK(out && back);
        if (!out || !back)
            return;
        // 30 yd each way (a 60 yd path) at 1.5 yd/s, the second leg straight back: he ends where he started.
        CHECK(out->yd == Drill::kRoccorWalkYd && back->yd == Drill::kRoccorWalkYd);
        CHECK(out->ms == Drill::kRoccorWalkMs && back->ms == Drill::kRoccorWalkMs);
        CHECK_NEAR(out->yd / (static_cast<float>(out->ms) / 1000.f), Drill::kRoccorWalkYdS, kEps);
        CHECK(out->offsetDeg == 0.f && back->offsetDeg == 180.f);
        Vec3 const home{ 0.f, 0.f, 0.f };
        float const facing = 1.1f;
        Vec3 const far = Drill::WalkPoint(home, facing, out->yd, out->ms, out->ms);
        Vec3 const again = Drill::WalkPoint(far, facing + Geo::kPi, back->yd, back->ms, back->ms);
        CHECK_NEAR(Geo::Dist2d(home.x, home.y, again.x, again.y), 0.f, 1e-2f);

        // The legs start as each Rampage resolves and never overlap a Rampage.
        CHECK(walks[0].startMs == rampages[0].endMs && walks[1].startMs == rampages[1].endMs);
        CHECK(rampages[1].startMs >= walks[0].endMs);
        // Each leg has a tank leash and an everyone leash over exactly its length.
        for (std::size_t i = 0; i < leashes.size(); ++i)
        {
            std::optional<Drill::LeashStep> const l = Drill::ParseLeash(Drill::StripPlanKeys(leashes[i].line));
            CHECK(l.has_value());
            if (!l)
                continue;
            At const& leg = walks[i / 2];
            CHECK(leashes[i].startMs == leg.startMs && l->ms == Drill::ParseWalk(Drill::StripPlanKeys(leg.line))->ms);
            CHECK(l->tanksOnly == (i % 2 == 0));
            CHECK(l->yd == (l->tanksOnly ? Drill::kTankLeashYd : Drill::kRangeLeashYd));
            CHECK(l->ledgerTag == (l->tanksOnly ? Drill::kBrdTagTankLeash : Drill::kBrdTagRangeLeash));
        }
        // The Rampage: a 20 x 8 lane from him, 2.5 s, 60% (heroic), no Weakened; the first along the route out, the
        // second back along it (offset=180), toward the group trailing him.
        for (At const& r : rampages)
        {
            CHECK(ShapeOf(r.line) == "line" && Size(r.line) == Drill::kRampageLength);
            CHECK(Key(r.line, "width", 0.f) == Drill::kRampageWidth && Drill::WarnMs(r.line) == Drill::kRampageMs);
            CHECK(Is(r.line, "anchor", "target") && Key(r.line, "pct", 0.f) == 60.f && !Drill::Value(r.line, "weak"));
        }
        CHECK(!Drill::Value(rampages[0].line, "offset") && Key(rampages[1].line, "offset", 0.f) == 180.f);
        // At the far end the group trails him: a member 5 yd behind him (toward home) stands in the second lane, and a
        // 4.5 yd sidestep (0.64 s at run speed) clears it inside the 2.5 s.
        Geo::Pose const lane{ far.x, far.y, far.z, Geo::WrapPi(facing + Geo::kPi) };
        Shape const shape = Shape::Line(Drill::kRampageLength, Drill::kRampageWidth);
        Vec3 const trailing = Drill::Shifted(far, facing, -5.f, 0.f);
        CHECK(Geo::Contains(shape, lane, trailing));
        CHECK(!Geo::Contains(shape, lane, Drill::Shifted(trailing, facing, 0.f, 4.f + kMarginYd)));
        CHECK(4.5f / kRunYdS + kReactS < Drill::kRampageMs / 1000.f);
        // ... and the sidestep keeps the tank inside its leash.
        Vec3 const aside = Drill::Shifted(trailing, facing, 0.f, 4.f + kMarginYd);
        CHECK(Geo::Dist2d(far.x, far.y, aside.x, aside.y) <= Drill::kTankLeashYd);
    }

    // A walker at 1.5 yd/s and one member: how many leash ticks of a leg it fails, standing still or following at
    // run speed with a re-path every `repathMs` (moves only when farther than `keepYd`).
    std::uint32_t LeashFailures(float startYd, float leashYd, std::uint32_t repathMs, float keepYd, bool follows)
    {
        float boss = 0.f;
        float member = -startYd;    // trailing on the walk axis
        std::uint32_t fails = 0;
        std::uint32_t const ms = Drill::kRoccorWalkMs;
        std::uint32_t sinceRepath = 0;
        bool moving = false;
        for (std::uint32_t t = 50; t <= ms; t += 50)
        {
            boss = Drill::kRoccorWalkYdS * static_cast<float>(t) / 1000.f;
            sinceRepath += 50;
            if (follows && sinceRepath >= repathMs)
            {
                sinceRepath = 0;
                moving = boss - member > keepYd;
            }
            if (moving)
                member = std::min(member + kRunYdS * 0.05f, boss - (keepYd - 1.f));
            if (t % Drill::kDefaultLeashTickMs == 0 && boss - member > leashYd)
                ++fails;
        }
        return fails;
    }

    void TestLeashModel()
    {
        // A tank that re-paths every 0.5 s and closes in at 5 yd keeps the 8 yd leash on a whole leg; one that stands
        // still fails from about 3.3 s in (16-17 ticks of 20).
        CHECK(LeashFailures(kSimTankYd, Drill::kTankLeashYd, 500, 5.f, true) == 0u);
        CHECK(LeashFailures(kSimTankYd, Drill::kTankLeashYd, 500, 5.f, false) >= 16u);
        // A re-path as slow as 2 s still holds it (5 yd + 3 yd of walking).
        CHECK(LeashFailures(kSimTankYd, Drill::kTankLeashYd, 2000, 5.f, true) == 0u);
        // A healer at 18 yd that never moves leaves the 40 yd leash after about 14.7 s: the leash catches it on every
        // leg; one that keeps 30 yd (spell range) never fails.
        std::uint32_t const still = LeashFailures(kSimHealerYd, Drill::kRangeLeashYd, 1000, 30.f, false);
        CHECK(still >= 5u && still <= 6u);
        CHECK(LeashFailures(kSimHealerYd, Drill::kRangeLeashYd, 1000, 30.f, true) == 0u);
        // A DPS at 12 yd that never moves fails only the leg's last 2 ticks (42 yd at 20 s): the 40 yd leash catches
        // the ones left behind, not the ones a few yd out of range (their loss is the sweep's DPS column).
        CHECK(LeashFailures(kSimDpsYd, Drill::kRangeLeashYd, 1000, 30.f, false) == 2u);
    }

    // GR3 + GR4 heroic: three 3 yd lines from 10 yd round him to the marked spot, all with the 5 yd landing there.
    void TestLeap()
    {
        std::vector<At> const lines = WithTag(0, Drill::kBrdTagLeapLine);
        std::vector<At> const landings = WithTag(0, Drill::kBrdTagLeapLanding);
        CHECK(lines.size() == 2 * Drill::kHounds && landings.size() == 2);
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            std::string_view const l = lines[i].line;
            CHECK(ShapeOf(l) == "line" && Key(l, "width", 0.f) == Drill::kLeapLineWidth);
            CHECK(Drill::WarnMs(l) == Drill::kLeapMs && Key(l, "away", 0.f) == Drill::kHoundAwayYd);
            CHECK(Is(l, "to", "member") && Is(l, "reach", "1") && Key(l, "pct", 0.f) == 25.f && Is(l, "weak", "1"));
            // The first hound of each leap picks the player (anyone, then a non-tank); the others follow it.
            std::string_view const pick = i % Drill::kHounds ? "same" : (i ? "nontank" : "any");
            CHECK(Is(l, "pick", pick));
        }
        for (std::size_t k = 0; k < landings.size() && k < 2; ++k)
        {
            std::string_view const c = landings[k].line;
            CHECK(ShapeOf(c) == "circle" && Size(c) == Drill::kLeapLandingYd && Drill::WarnMs(c) == Drill::kLeapMs);
            CHECK(Is(c, "at", "last") && Key(c, "pct", 0.f) == 45.f && Is(c, "weak", "1"));
            // with its three lines, after them in list order (at=last reads the third line's end: the marked spot)
            for (std::size_t h = 0; h < Drill::kHounds; ++h)
            {
                At const& l = lines[k * Drill::kHounds + h];
                CHECK(l.startMs == landings[k].startMs && l.index < landings[k].index);
            }
        }
        // The leaps 3 s apart, after the walk is home.
        if (landings.size() == 2)
            CHECK(landings[1].startMs - landings[0].startMs >= 3000u);
        for (At const& a : lines)
            CHECK(a.startMs >= Drill::kRampageMs * 2 + Drill::kRoccorWalkMs * 2);

        // Reach: a line fixed toward a member and reaching it ends on its spot (FixLine), so at=last is that spot.
        Drill::FixedLine const fl = Drill::FixLine({ 0.f, 0.f, 0.f }, { 14.f, 3.f, 0.f }, 1.f, true, 0.f);
        Vec3 const end = Drill::LineEnd(fl.source, fl.facing, fl.length);
        CHECK_NEAR(end.x, 14.f, kEps);
        CHECK_NEAR(end.y, 3.f, kEps);

        // Escape: from the marked spot, wherever the three hounds stand 10 yd round him, a point clear of the 5 yd
        // landing and of all three lines (with the bots' margin) is within reach in 1.5 s less the reaction.
        float const reach = kRunYdS * (Drill::kLeapMs / 1000.f - kReactS);
        std::uint32_t seed = 12345;
        auto rnd = [&seed]()
        {
            seed = seed * 1103515245u + 12345u;
            return static_cast<float>((seed >> 8) & 0xFFFF) / 65535.f;
        };
        int worst = 0;
        for (int trial = 0; trial < 400; ++trial)
        {
            float const ma = rnd() * Geo::kTwoPi;
            float const md = 2.f + rnd() * 20.f;
            Vec3 const marked{ md * std::cos(ma), md * std::sin(ma), 0.f };
            std::vector<Geo::Pose> poses;
            std::vector<float> lengths;
            for (std::uint32_t h = 0; h < Drill::kHounds; ++h)
            {
                float const a = rnd() * Geo::kTwoPi;
                Vec3 const src{ Drill::kHoundAwayYd * std::cos(a), Drill::kHoundAwayYd * std::sin(a), 0.f };
                Drill::FixedLine const f = Drill::FixLine(src, marked, 1.f, true, 0.f);
                poses.push_back({ src.x, src.y, 0.f, f.facing });
                lengths.push_back(f.length);
            }
            bool found = false;
            for (float r = Drill::kLeapLandingYd + kMarginYd; r <= reach && !found; r += 0.25f)
                for (int s = 0; s < 72 && !found; ++s)
                {
                    float const a = static_cast<float>(s) * Geo::kTwoPi / 72.f;
                    Vec3 const p{ marked.x + r * std::cos(a), marked.y + r * std::sin(a), 0.f };
                    bool clear = true;
                    for (std::size_t h = 0; h < poses.size() && clear; ++h)
                        clear = !Geo::Contains(Shape::Line(lengths[h], Drill::kLeapLineWidth + 2.f * kMarginYd),
                            poses[h], p);
                    found = clear;
                }
            if (!found)
                ++worst;
        }
        CHECK(worst == 0);
    }

    // GO2 heroic: the axe lands (3 yd, 2 s) on a player's spot; the return line, axe to him, shows 6 s and locks 2 s
    // before the axe flies (the shown line: no damage, reported; the locked one: 35%, Weakened).
    void TestAxeReturn()
    {
        std::vector<At> const axe = WithTag(1, Drill::kBrdTagAxe);
        std::vector<At> const shown = WithTag(1, Drill::kBrdTagAxeShown);
        std::vector<At> const locked = WithTag(1, Drill::kBrdTagAxeReturn);
        CHECK(axe.size() == 1 && shown.size() == 1 && locked.size() == 1);
        if (axe.size() != 1 || shown.size() != 1 || locked.size() != 1)
            return;
        CHECK(ShapeOf(axe[0].line) == "circle" && Size(axe[0].line) == Drill::kAxeLandingYd);
        CHECK(Drill::WarnMs(axe[0].line) == Drill::kAxeLandingMs && Is(axe[0].line, "at", "member"));
        CHECK(Key(axe[0].line, "pct", 0.f) == 25.f && Is(axe[0].line, "weak", "1"));
        for (At const* l : { &shown[0], &locked[0] })
        {
            CHECK(ShapeOf(l->line) == "line" && Key(l->line, "width", 0.f) == Drill::kAxeReturnWidth);
            CHECK(Is(l->line, "at", "prev") && Is(l->line, "to", "target") && Is(l->line, "reach", "1"));
        }
        // The line shows from the landing to the return: 6 s in all, the last 2 s locked.
        CHECK(shown[0].startMs == axe[0].endMs && locked[0].startMs == shown[0].endMs);
        CHECK(locked[0].endMs - axe[0].endMs == Drill::kAxeStuckMs);
        CHECK(Drill::WarnMs(locked[0].line) == Drill::kAxeLockMs);
        CHECK(Key(shown[0].line, "pct", -1.f) == 0.f && !Drill::Value(shown[0].line, "weak"));
        CHECK(Key(locked[0].line, "pct", 0.f) == 35.f && Is(locked[0].line, "weak", "1"));
        // at=prev reads the axe's centre: no other point circle registers between the landing and the return.
        for (At const& a : Cycle(1))
            if (a.startMs > axe[0].startMs && a.startMs <= locked[0].startMs)
                CHECK(ShapeOf(a.line) != "circle" && ShapeOf(a.line) != "stack");
        // The locked line ends on him (reach): a player beside the line by width / 2 + margin is clear, and 2 s is
        // enough for 2.5 yd of sidestep.
        CHECK((Drill::kAxeReturnWidth / 2.f + kMarginYd) / kRunYdS + kReactS < Drill::kAxeLockMs / 1000.f);
    }

    // OK1: the donut from 8 yd (normal) and 6 yd (heroic) to 45 yd, 3 s; the sim's healer (18 yd) gets in on time.
    void TestRupture()
    {
        std::vector<At> const normal = WithTag(1, Drill::kBrdTagRupture);
        std::vector<At> const heroic = WithTag(1, Drill::kBrdTagRuptureHeroic);
        CHECK(normal.size() == 1 && heroic.size() == 1);
        if (normal.size() != 1 || heroic.size() != 1)
            return;
        struct Row
        {
            At const* a;
            float inner;
            float pct;
        };
        for (Row const& r : { Row{ &normal[0], Drill::kRuptureInnerYd, 30.f },
            Row{ &heroic[0], Drill::kRuptureInnerHeroicYd, 45.f } })
        {
            CHECK(ShapeOf(r.a->line) == "donut" && Size(r.a->line) == Drill::kRuptureOuterYd);
            CHECK(Key(r.a->line, "inner", 0.f) == r.inner && Drill::WarnMs(r.a->line) == Drill::kRuptureMs);
            CHECK(Is(r.a->line, "anchor", "target") && Key(r.a->line, "pct", 0.f) == r.pct);
            CHECK(Is(r.a->line, "weak", "1"));
            CHECK((kSimHealerYd - r.inner + kMarginYd) / kRunYdS + kReactS < Drill::kRuptureMs / 1000.f);
        }
        CHECK(heroic[0].startMs >= normal[0].endMs + 2000u);
    }

    // LG1-LG3 heroic: three tides, each a Stack on the ward and a donut from its edge, at three spots hugging him
    // (KeepOut: he stands outside it; the tank inside it still reaches him), radius locked per tide: 5, 3, 4.
    void TestWard()
    {
        std::vector<At> const stacks = WithTag(1, Drill::kBrdTagWard);
        std::vector<At> const tides = WithTag(1, Drill::kBrdTagTide);
        CHECK(stacks.size() == 3 && tides.size() == 3);
        if (stacks.size() != 3 || tides.size() != 3)
            return;
        float const radii[] = { 5.f, 3.f, 4.f };
        Vec3 const boss{ 0.f, 0.f, 0.f };
        float const facing = 0.4f;
        std::vector<Vec3> centres;
        for (std::size_t i = 0; i < 3; ++i)
        {
            std::string_view const s = stacks[i].line;
            std::string_view const t = tides[i].line;
            CHECK(ShapeOf(s) == "stack" && Is(s, "anchor", "point") && Size(s) == radii[i]);
            CHECK(ShapeOf(t) == "donut" && Size(t) == Drill::kTideOuterYd && Key(t, "inner", 0.f) == radii[i]);
            CHECK(Drill::WarnMs(s) == Drill::kTideMs && Drill::WarnMs(t) == Drill::kTideMs);
            CHECK(stacks[i].startMs == tides[i].startMs);
            CHECK(Key(t, "pct", 0.f) == 45.f && Is(t, "weak", "1") && Is(t, "school", "fire"));
            CHECK(ShiftOf(s) == ShiftOf(t));
            auto const [ahead, left] = ShiftOf(s);
            Vec3 const c = Frame(boss, facing, ahead, left);
            centres.push_back(c);
            float const d = Geo::Dist2d(boss.x, boss.y, c.x, c.y);
            CHECK(d > radii[i]);                                // he is outside the ward
            CHECK(d - radii[i] + kMarginYd <= 5.f - 1.f);        // a spot in the ward within melee reach of him
            if (i)
                CHECK(stacks[i].startMs >= tides[i - 1].endMs + 2000u);
        }
        // The ward moves every tide, and the walk to the next one fits from the resolve to the next resolve.
        for (std::size_t i = 1; i < 3; ++i)
        {
            float const moved = Geo::Dist2d(centres[i - 1].x, centres[i - 1].y, centres[i].x, centres[i].y);
            CHECK(moved >= 5.f);
            float const s = static_cast<float>(tides[i].endMs - tides[i - 1].endMs) / 1000.f;
            CHECK((moved + radii[i - 1]) / kRunYdS + kReactS < s);
        }
    }

    // IN8 (the outer 4 yd of all four edges), IN13 (§7 #14 A: the next 2 yd of the long edges) and IN11 (a 10 yd band
    // across, twice) on the 48 x 22 strip round him, its long axis along the run's facing.
    void TestBands()
    {
        std::vector<At> const edges = WithTag(1, Drill::kBrdTagEdgeBand);
        std::vector<At> const crumbles = WithTag(1, Drill::kBrdTagCrumble);
        std::vector<At> const sweeps = WithTag(1, Drill::kBrdTagSweep);
        CHECK(edges.size() == 4 && crumbles.size() == 2 && sweeps.size() == 2);
        for (std::vector<At> const* set : { &edges, &crumbles })
            for (At const& a : *set)
            {
                CHECK(Is(a.line, "lethal", "1") && Drill::WarnMs(a.line) == Drill::kBandWarnMs);
                CHECK(Key(a.line, "tick", 0.f) == 500.f && Is(a.line, "weak", "1"));
                CHECK(a.endMs == Drill::kBrdCycleMs - 2000u);   // they stay until the row is over
            }
        for (At const& a : edges)
            CHECK(Key(a.line, "width", 0.f) == Drill::kEdgeBandYd);
        for (At const& a : crumbles)
            CHECK(Key(a.line, "width", 0.f) == Drill::kCrumbleBandYd && a.startMs >= edges[0].endMs - 15000u + 1000u);
        for (At const& a : sweeps)
        {
            CHECK(Is(a.line, "lethal", "1") && Key(a.line, "width", 0.f) == Drill::kSweepWidth);
            CHECK(Drill::WarnMs(a.line) == Drill::kSweepMs && Key(a.line, "pct", 0.f) == 30.f);
            CHECK(Key(a.line, "offset", 0.f) == 90.f);
        }

        Vec3 const boss{ 3.f, -7.f, 0.f };
        for (float facing : { 0.f, 1.3f, -2.4f })
        {
            auto inAny = [&boss, facing](std::vector<At> const& set, Vec3 const& p)
            {
                for (At const& a : set)
                    if (InLine(a.line, boss, facing, p))
                        return true;
                return false;
            };
            float const halfL = Drill::kStripLength / 2.f;
            float const halfW = Drill::kStripWidth / 2.f;
            for (float a = -halfL; a <= halfL; a += 0.5f)
                for (float l = -halfW; l <= halfW; l += 0.5f)
                {
                    Vec3 const p = Frame(boss, facing, a, l);
                    float const fa = std::fabs(a);
                    float const fl = std::fabs(l);
                    float const bandW = halfW - Drill::kEdgeBandYd;
                    float const bandL = halfL - Drill::kEdgeBandYd;
                    bool const edge = fl >= bandW + 0.1f || fa >= bandL + 0.1f;
                    bool const inner = fl <= bandW - 0.1f && fa <= bandL - 0.1f;
                    if (edge)
                        CHECK(inAny(edges, p));
                    if (inner)
                        CHECK(!inAny(edges, p));
                    // the crumble: the next 2 yd of the long edges, not the necks
                    float const lo = halfW - Drill::kEdgeBandYd - Drill::kCrumbleBandYd;
                    if (fl >= lo + 0.1f && fl <= halfW - Drill::kEdgeBandYd - 0.1f)
                        CHECK(inAny(crumbles, p));
                    if (fl <= lo - 0.1f)
                        CHECK(!inAny(crumbles, p));
                }
            // The sweeps cross the whole strip (10 yd along it): the middle (him and the tank) and the group's side.
            CHECK(InLine(sweeps[0].line, boss, facing, Frame(boss, facing, 0.f, 0.f)));
            CHECK(InLine(sweeps[0].line, boss, facing, Frame(boss, facing, -kSimTankYd, 0.f)));
            CHECK(InLine(sweeps[0].line, boss, facing, Frame(boss, facing, 4.9f, -halfW + 0.2f)));
            CHECK(!InLine(sweeps[0].line, boss, facing, Frame(boss, facing, 5.1f + kMarginYd, 0.f)));
            CHECK(InLine(sweeps[1].line, boss, facing, Frame(boss, facing, -kSimDpsYd, 3.f)));
            CHECK(!InLine(sweeps[1].line, boss, facing, Frame(boss, facing, -kSimHealerYd, 0.f)));
            // After both crumbles and with each sweep down, safe ground is left within one sweep's reach of every
            // spot it covers: a 5 yd step along the strip in 4 s.
            for (At const& s : sweeps)
                for (float a = -halfL + 4.f; a <= halfL - 4.f; a += 0.5f)
                {
                    Vec3 const p = Frame(boss, facing, a, 0.f);
                    if (!InLine(s.line, boss, facing, p))
                        continue;
                    bool safe = false;
                    for (float step : { -1.f, 1.f })
                        for (float d = 0.5f; d <= kRunYdS * (Drill::kSweepMs / 1000.f - kReactS) && !safe; d += 0.5f)
                        {
                            Vec3 const q = Frame(boss, facing, a + step * d, 0.f);
                            Vec3 const q1 = Frame(boss, facing, a + step * (d + kMarginYd), 0.f);
                            safe = !InLine(s.line, boss, facing, q1) && !inAny(edges, q) && !inAny(crumbles, q);
                        }
                    CHECK(safe);
                }
            // The sim's group starts on the strip, inside the bands: the tank 3 yd, the DPS 12 yd, the healer 18 yd
            // toward the reference.
            for (float d : { kSimTankYd, kSimDpsYd, kSimHealerYd })
            {
                Vec3 const p = Frame(boss, facing, -d, 0.f);
                CHECK(!inAny(edges, p) && !inAny(crumbles, p));
            }
        }
        // The crumble comes after the edges are down, the sweeps after the crumble is.
        CHECK(crumbles[0].startMs >= edges[0].startMs + Drill::kBandWarnMs);
        CHECK(sweeps[0].startMs >= crumbles[0].startMs + Drill::kBandWarnMs);
        CHECK(sweeps[1].startMs >= sweeps[0].endMs + 1000u);
    }

    // offset= on a line: the facing turns counterclockwise and shift= reads along the turned facing (so a lane across
    // the strip at <ahead> = a takes shift=<-halfW>:<-a>).
    void TestLineOffset()
    {
        Vec3 const boss{ 0.f, 0.f, 0.f };
        float const facing = 0.f;
        Geo::Pose const p = LinePose("line 22 4000 width=10 offset=90 shift=-11:-22", boss, facing);
        CHECK_NEAR(p.o, Geo::kPi / 2.f, kEps);
        CHECK_NEAR(p.x, 22.f, kEps);        // ahead 22 along the run's facing (+x)
        CHECK_NEAR(p.y, -11.f, kEps);       // from the right edge, running left
        Geo::Pose const back = LinePose("line 20 2500 width=8 offset=180", boss, 0.7f);
        CHECK_NEAR(Geo::WrapPi(back.o - 0.7f - Geo::kPi), 0.f, kEps);
    }

    void TestSweepProgram()
    {
        std::string err;
        for (char const* name : { "blackrock-depths-shapes:2", "blackrock-depths-shapes" })
        {
            std::optional<Drill::Program> const p = Drill::ParseProgram(name, err);
            CHECK(p && err.empty());
            if (!p || p->entries.size() != 1)
            {
                CHECK(false);
                continue;
            }
            Drill::ProgramEntry const& e = p->entries[0];
            CHECK(e.kind == Drill::EntryKind::Drill && e.drill == &Shapes());
            CHECK(e.cycles == 2 && e.lengthMs == 120000);
            std::vector<Drill::ScheduledStep> const s = Drill::StepSchedule(*e.drill, e.cycles);
            // every step once: cycle 0's rows, then cycle 1's
            CHECK(s.size() == e.drill->steps.size());
            for (Drill::ScheduledStep const& st : s)
            {
                std::string_view const line = e.drill->steps[st.step].line;
                CHECK(static_cast<float>(st.delayMs + Drill::WarnMs(line)) + Key(line, "persist", 0.f) <= 120000.f);
            }
        }
    }

    // ==== wave 2: `blackrock-depths-shapes-2` (WP-G2) ===============================================================
    Drill::Def const& Shapes2()
    {
        static Drill::Def const* d = Drill::Find("blackrock-depths-shapes-2");
        return *d;
    }

    std::vector<At> Cycle2(std::uint32_t cycle)
    {
        std::vector<At> out;
        Drill::Def const& d = Shapes2();
        std::vector<std::optional<std::uint32_t>> const offsets = Drill::StepOffsets(d, cycle);
        for (std::size_t i = 0; i < d.steps.size(); ++i)
            if (offsets[i])
            {
                std::string_view const line = d.steps[i].line;
                std::uint32_t const end = *offsets[i] + Drill::WarnMs(line)
                    + static_cast<std::uint32_t>(Key(line, "persist", 0.f));
                out.push_back({ i, line, *offsets[i], end });
            }
        return out;
    }

    std::vector<At> WithTag2(std::uint32_t cycle, std::uint32_t tag)
    {
        std::vector<At> out;
        for (At const& a : Cycle2(cycle))
            if (ShapeOf(a.line) != "walk" && Tag(a.line) == tag)
                out.push_back(a);
        return out;
    }

    std::vector<At> WithShape2(std::uint32_t cycle, std::string_view shape)
    {
        std::vector<At> out;
        for (At const& a : Cycle2(cycle))
            if (ShapeOf(a.line) == shape)
                out.push_back(a);
        return out;
    }

    // The judged passes of a lasting step (Judge::TakePass): at the resolve, then every tick while before the end.
    std::uint32_t Passes(std::string_view line)
    {
        float const persist = Key(line, "persist", 0.f);
        float const tick = Key(line, "tick", 1000.f);
        if (persist <= 0.f)
            return 1;
        return static_cast<std::uint32_t>(std::ceil(persist / tick));
    }

    // A beam Shapes::Line(Moira, Dagran) as the registry judges it: from Moira toward where he stands now, as long as
    // the distance was when it registered.
    bool InBeam(Vec3 const& moira, Vec3 const& dagranAtStart, Vec3 const& dagranNow, Vec3 const& p)
    {
        float const length = Geo::Dist2d(moira.x, moira.y, dagranAtStart.x, dagranAtStart.y);
        Geo::Pose const pose{ moira.x, moira.y, moira.z,
            std::atan2(dagranNow.y - moira.y, dagranNow.x - moira.x) };
        return Geo::Contains(Shape::Line(length, Drill::kBeamWidth), pose, p);
    }

    void TestTable2()
    {
        Drill::Def const* d = Drill::Find("blackrock-depths-shapes-2");
        CHECK(d != nullptr);
        if (!d)
            return;
        // a drill of its own: the wave 1 drill keeps its 60 s cycles and its 120 s sweep
        CHECK(d != Drill::Find("blackrock-depths-shapes"));
        CHECK(d->cycleMs == Drill::kBrd2CycleMs && d->defaultCycles == Drill::kBrd2Cycles);
        CHECK(d->cycleMs * d->defaultCycles == 120000u);
        CHECK(!d->ownerAtCreature);

        std::set<std::uint32_t> tags;
        for (Drill::Step const& s : d->steps)
        {
            CHECK(Is(s.line, "cycle", "0/2") || Is(s.line, "cycle", "1/2"));
            if (ShapeOf(s.line) == "walk")
                continue;
            tags.insert(Tag(s.line));
        }
        CHECK(tags == (std::set<std::uint32_t>{ 21, 22, 23, 24, 25, 26, 27 }));
        // never a wave 1 tag (1-16), so a ledger read across both drills cannot mix them
        for (std::uint32_t t : tags)
            CHECK(t > Drill::kBrdTagHide && !Drill::BrdBarTag(t));
        std::set<std::uint32_t> const bar(std::begin(Drill::kBrd2BarTags), std::end(Drill::kBrd2BarTags));
        std::set<std::uint32_t> const group(std::begin(Drill::kBrd2GroupTags), std::end(Drill::kBrd2GroupTags));
        CHECK(bar == (std::set<std::uint32_t>{ Drill::kBrd2TagLoner, Drill::kBrd2TagCrossfire,
            Drill::kBrd2TagCrossfireBurn, Drill::kBrd2TagFlameWall }));
        CHECK(group == (std::set<std::uint32_t>{ Drill::kBrd2TagBeam }));
        for (std::uint32_t t = 0; t <= 30; ++t)
            CHECK(Drill::Brd2BarTag(t) == (bar.count(t) == 1));
        // reported: what is neither gated nor a group tag (Livewire's pulses and burst)
        std::set<std::uint32_t> reported;
        for (std::uint32_t t : tags)
            if (!bar.count(t) && !group.count(t))
                reported.insert(t);
        CHECK(reported == (std::set<std::uint32_t>{ Drill::kBrd2TagLivewire, Drill::kBrd2TagLivewireBurst }));

        for (std::uint32_t cycle = 0; cycle < 4; ++cycle)
        {
            std::uint32_t last = 0;
            for (At const& a : Cycle2(cycle))
            {
                CHECK(a.startMs >= last);
                last = a.startMs;
                CHECK(a.endMs <= d->cycleMs);
                CHECK(Is(a.line, "cycle", cycle % 2 ? "1/2" : "0/2"));
            }
        }
        // Every damage step carries a heroic percent; the get-in shapes none.
        for (Drill::Step const& s : d->steps)
            CHECK(Drill::DamageStep(s.line) == Drill::Value(s.line, "pct").has_value());
    }

    // EM2 heroic: three 8 s Nemesis channels 20 s apart, a 2 yd line Soak of one body from Moira's spot (15 yd to his
    // left, the same spot every time) to him; the tank drifts him 6 yd during the first and back during the second.
    void TestBeamRow()
    {
        std::vector<At> const beams = WithTag2(0, Drill::kBrd2TagBeam);
        std::vector<At> const walks = WithShape2(0, "walk");
        CHECK(beams.size() == 3 && walks.size() == 2);
        CHECK(WithTag2(1, Drill::kBrd2TagBeam).empty());
        if (beams.size() != 3 || walks.size() != 2)
            return;
        for (std::size_t i = 0; i < beams.size(); ++i)
        {
            std::string_view const l = beams[i].line;
            CHECK(ShapeOf(l) == "soak" && Size(l) == Drill::kBeamWidth && !Drill::Value(l, "width"));
            CHECK(Is(l, "shape", "line") && Is(l, "to", "target") && !Drill::Value(l, "length"));
            CHECK(Key(l, "need", 0.f) == 1.f && Drill::WarnMs(l) == Drill::kBeamWarnMs);
            CHECK(Drill::WarnMs(l) + static_cast<std::uint32_t>(Key(l, "persist", 0.f)) == Drill::kNemesisMs);
            CHECK(Key(l, "tick", 0.f) == 1000.f && Passes(l) == 5u);
            // Moira rooted (§7 #19): a plain point (the drill's parse-time spot, his start), never at=target
            CHECK(!Drill::Value(l, "at") && !Drill::Value(l, "anchor"));
            CHECK(ShiftOf(l) == std::make_pair(0.f, Drill::kMoiraYd));
            CHECK(beams[i].startMs == i * Drill::kNemesisEveryMs);
        }
        // The drift: 6 yd along the run's facing over the channel, then back over the second; the third stands.
        std::optional<Drill::WalkStep> const out = Drill::ParseWalk(Drill::StripPlanKeys(walks[0].line));
        std::optional<Drill::WalkStep> const back = Drill::ParseWalk(Drill::StripPlanKeys(walks[1].line));
        CHECK(out && back);
        if (!out || !back)
            return;
        CHECK(walks[0].startMs == beams[0].startMs && walks[1].startMs == beams[1].startMs);
        CHECK(out->yd == Drill::kDagranDriftYd && back->yd == Drill::kDagranDriftYd);
        CHECK(out->ms == Drill::kNemesisMs && back->ms == Drill::kNemesisMs);
        CHECK(out->offsetDeg == 0.f && back->offsetDeg == 180.f);
        CHECK(beams[2].startMs >= walks[1].startMs + back->ms);

        float const f = 0.9f;
        Vec3 const home{ 0.f, 0.f, 0.f };
        Vec3 const moira = Frame(home, f, 0.f, Drill::kMoiraYd);
        Vec3 const drifted = Drill::WalkPoint(home, f, out->yd, out->ms, out->ms);
        // The length stays the registration's: 1.16 yd short of him at the end of the first, 1.16 past him at the end
        // of the second (BeamLengthError); a blocker in the middle third never notices.
        float const err = std::sqrt(Drill::kMoiraYd * Drill::kMoiraYd + Drill::kDagranDriftYd * Drill::kDagranDriftYd)
            - Drill::kMoiraYd;
        CHECK_NEAR(Drill::BeamLengthError(moira, home, drifted), err, kEps);
        CHECK_NEAR(Drill::BeamLengthError(moira, drifted, home), -err, kEps);
        CHECK(!InBeam(moira, home, drifted, Drill::Shifted(drifted, std::atan2(moira.y - drifted.y,
            moira.x - drifted.x), 0.5f, 0.f)));
        for (int leg = 0; leg < 2; ++leg)
        {
            Vec3 const start = leg == 0 ? home : drifted;
            float const heading = leg == 0 ? f : Geo::WrapPi(f + Geo::kPi);
            Vec3 prevMid{};
            for (std::uint32_t t = 0; t <= Drill::kNemesisMs; t += 250)
            {
                Vec3 const now = Drill::WalkPoint(start, heading, Drill::kDagranDriftYd, Drill::kNemesisMs, t);
                float const live = Geo::Dist2d(moira.x, moira.y, now.x, now.y);
                float const dir = std::atan2(now.y - moira.y, now.x - moira.x);
                // a blocker anywhere on the middle of the live beam is judged inside it
                for (float frac : { 0.2f, 0.5f, 0.8f })
                    CHECK(InBeam(moira, start, now, Drill::Shifted(moira, dir, frac * live, 0.f)));
                // the middle moves at most 0.4 yd a second: a blocker re-placed every second stays inside 1 yd
                Vec3 const mid = Drill::Shifted(moira, dir, live / 2.f, 0.f);
                if (t >= 1000 && t % 1000 == 0)
                    CHECK(Geo::Dist2d(mid.x, mid.y, prevMid.x, prevMid.y) <= 0.4f);
                if (t % 1000 == 0)
                    prevMid = mid;
            }
        }
        // Nobody stands in it at the sim's start (the group on the run's axis toward the reference): a body has to
        // step in, and the DPS 12 yd out reaches it inside the 3 s.
        for (float yd : { kSimTankYd, kSimDpsYd, kSimHealerYd })
            CHECK(!InBeam(moira, home, home, Frame(home, f, -yd, 0.f)));
        Vec3 const dps = Frame(home, f, -kSimDpsYd, 0.f);
        Vec3 const nearest = Frame(home, f, 0.f, Drill::kBeamWidth);
        CHECK(InBeam(moira, home, home, nearest));
        CHECK(Geo::Dist2d(dps.x, dps.y, nearest.x, nearest.y) / kRunYdS + kReactS < Drill::kBeamWarnMs / 1000.f);
    }

    // TS4 heroic: Seething Cold, everyone needs 2 others within 6 yd for 10 s after a 3 s cast; no anchor.
    void TestLonerRow()
    {
        std::vector<At> const loner = WithTag2(1, Drill::kBrd2TagLoner);
        CHECK(loner.size() == 1 && WithTag2(0, Drill::kBrd2TagLoner).empty());
        if (loner.size() != 1)
            return;
        std::string_view const l = loner[0].line;
        CHECK(ShapeOf(l) == "stack" && Size(l) == Drill::kLonerYd && Key(l, "cluster", 0.f) == Drill::kLonerOthers);
        CHECK(Drill::WarnMs(l) == Drill::kLonerCastMs && Key(l, "persist", 0.f) == Drill::kLonerMs);
        CHECK(Key(l, "tick", 0.f) == 1000.f);
        CHECK(!Drill::Value(l, "anchor") && !Drill::Value(l, "at") && !Drill::Value(l, "shift"));
        Shape const circle = Shape::Circle(Drill::kLonerYd);
        auto misses = [&circle](std::vector<Vec3> const& players)
        {
            return Judge::ClusterMisses(circle, players, Drill::kLonerOthers);
        };
        // everyone together: no miss; the tank alone: the tank only; a trio and a pair: the pair
        CHECK(misses({ { 0, 0, 0 }, { 2, 0, 0 }, { 0, 2, 0 }, { -2, 0, 0 }, { 0, -2, 0 } }).empty());
        CHECK(misses({ { 0, 0, 0 }, { 10, 0, 0 }, { 11, 0, 0 }, { 10, 1, 0 }, { 12, 1, 0 } })
            == (std::vector<std::size_t>{ 0 }));
        CHECK(misses({ { 0, 0, 0 }, { 2, 0, 0 }, { 0, 2, 0 }, { 15, 0, 0 }, { 16, 0, 0 } })
            == (std::vector<std::size_t>{ 3, 4 }));
        // a chain 5 yd apart: the two ends have one neighbour each
        CHECK(misses({ { 0, 0, 0 }, { 5, 0, 0 }, { 10, 0, 0 }, { 15, 0, 0 }, { 20, 0, 0 } })
            == (std::vector<std::size_t>{ 0, 4 }));
        // the edge counts (6 yd exactly)
        CHECK(misses({ { 0, 0, 0 }, { 3, 0, 0 }, { 6, 0, 0 } }).empty());
        // From the sim's start the healer (18 yd) walks 10 yd to the DPS at 8 yd from the boss, within 6 of the tank
        // at 3: 1.7 s of the 3 s cast.
        CHECK((kSimHealerYd - 8.f) / kRunYdS + kReactS < Drill::kLonerCastMs / 1000.f);
        CHECK(8.f - kSimTankYd <= Drill::kLonerYd);
        // Livewire (keep away from one carrier) never runs while the loner (stay with two) does.
        std::vector<At> const live = WithTag2(1, Drill::kBrd2TagLivewire);
        CHECK(live.size() == 1 && loner[0].endMs <= live[0].startMs);
    }

    // AR8 heroic: Livewire on a non-tank 12 s, everyone else within 8 yd takes 4% every 2 s, the burst (the dispel)
    // at its end hits within 8 yd for 30%.
    void TestLivewireRow()
    {
        std::vector<At> const pulse = WithTag2(1, Drill::kBrd2TagLivewire);
        std::vector<At> const burst = WithTag2(1, Drill::kBrd2TagLivewireBurst);
        CHECK(pulse.size() == 1 && burst.size() == 1);
        if (pulse.size() != 1 || burst.size() != 1)
            return;
        std::string_view const p = pulse[0].line;
        std::string_view const b = burst[0].line;
        CHECK(ShapeOf(p) == "circle" && Size(p) == Drill::kLivewireYd && Is(p, "anchor", "member"));
        CHECK(Is(p, "pick", "nontank") && Key(p, "tick", 0.f) == Drill::kLivewireTickMs);
        CHECK(Drill::WarnMs(p) == Drill::kLivewireTickMs && Key(p, "pct", 0.f) == 4.f && Is(p, "school", "nature"));
        CHECK(!Drill::Value(p, "weak") && !Drill::Value(b, "weak"));
        CHECK(ShapeOf(b) == "circle" && Size(b) == Drill::kLivewireYd && Is(b, "anchor", "member"));
        CHECK(Is(b, "pick", "same") && Key(b, "pct", 0.f) == 30.f && !Drill::Value(b, "persist"));
        // the burst lands 12 s after the debuff, as the pulses end: five pulses and the burst, one every 2 s
        CHECK(burst[0].endMs == pulse[0].startMs + Drill::kLivewireMs && pulse[0].endMs == burst[0].endMs);
        CHECK(Passes(p) + 1 == Drill::kLivewireMs / Drill::kLivewireTickMs);
        // no step between them picks a member, so pick=same is the carrier
        for (At const& a : Cycle2(1))
            if (a.startMs > pulse[0].startMs && a.startMs < burst[0].startMs)
                CHECK(!Drill::Value(a.line, "pick"));
        // a member hugging the carrier is out of the 8 yd ring before the first pulse
        CHECK((Drill::kLivewireYd + kMarginYd) / kRunYdS + kReactS < Drill::kLivewireTickMs / 1000.f);
    }

    // MG3 + MG4 heroic in the Iron Hall (39 yd floor, axis along the run's facing through his start): a 10 yd lane
    // across the hall at his spot (3 s glow, 45% + Weakened, then 15% a second for the rest of 6 s) while the tank
    // walks him 12 yd out of it; a Flame Wall along the left wall (3 s glow, 8 s, 15% a second); a second Crossfire
    // 12 s after the first at his new spot, walking him home.
    void TestCrossfireRow()
    {
        std::vector<At> const fire = WithTag2(1, Drill::kBrd2TagCrossfire);
        std::vector<At> const burn = WithTag2(1, Drill::kBrd2TagCrossfireBurn);
        std::vector<At> const wall = WithTag2(1, Drill::kBrd2TagFlameWall);
        std::vector<At> const walks = WithShape2(1, "walk");
        CHECK(fire.size() == 2 && burn.size() == 2 && wall.size() == 1 && walks.size() == 2);
        if (fire.size() != 2 || burn.size() != 2 || wall.size() != 1 || walks.size() != 2)
            return;
        CHECK(fire[1].startMs - fire[0].startMs == Drill::kCrossfireEveryMs);
        for (std::size_t i = 0; i < 2; ++i)
        {
            std::string_view const f = fire[i].line;
            std::string_view const b = burn[i].line;
            for (std::string_view l : { f, b })
            {
                CHECK(ShapeOf(l) == "line" && Size(l) == Drill::kHallWidth);
                CHECK(Key(l, "width", 0.f) == Drill::kCrossfireWidth && Key(l, "offset", 0.f) == 90.f);
                // at his spot as it registers (the drill's walk moved him), across the hall wall to wall
                CHECK(Is(l, "at", "target") && ShiftOf(l) == std::make_pair(-Drill::kHallWidth / 2.f, 0.f));
                CHECK(Is(l, "school", "fire"));
            }
            CHECK(Drill::WarnMs(f) == Drill::kCrossfireGlowMs && Key(f, "pct", 0.f) == 45.f && Is(f, "weak", "1"));
            CHECK(!Drill::Value(f, "persist"));
            // the burn registers with the ignition, judged from 1 s after it for the rest of the 6 s
            CHECK(burn[i].startMs == fire[i].startMs);
            CHECK(Drill::WarnMs(b) == Drill::kCrossfireGlowMs + 1000u);
            CHECK(burn[i].endMs == fire[i].endMs + Drill::kCrossfireBurnMs);
            CHECK(Passes(b) + 1 == Drill::kCrossfireBurnMs / 1000u);
            CHECK(Key(b, "pct", 0.f) == 15.f && !Drill::Value(b, "weak"));
            // the tank walks him out over the glow
            std::optional<Drill::WalkStep> const w = Drill::ParseWalk(Drill::StripPlanKeys(walks[i].line));
            CHECK(w && walks[i].startMs == fire[i].startMs);
            if (w)
            {
                CHECK(w->yd == Drill::kMagmusDragYd && w->ms == Drill::kCrossfireGlowMs);
                CHECK(w->offsetDeg == (i == 0 ? 0.f : 180.f));
                CHECK(w->yd / (static_cast<float>(w->ms) / 1000.f) < kRunYdS);
            }
        }

        float const facing = 0.4f;
        Vec3 const home{ 0.f, 0.f, 0.f };
        Vec3 const out = Drill::WalkPoint(home, facing, Drill::kMagmusDragYd, Drill::kCrossfireGlowMs,
            Drill::kCrossfireGlowMs);
        Vec3 const back = Drill::WalkPoint(out, Geo::WrapPi(facing + Geo::kPi), Drill::kMagmusDragYd,
            Drill::kCrossfireGlowMs, Drill::kCrossfireGlowMs);
        CHECK_NEAR(Geo::Dist2d(home.x, home.y, back.x, back.y), 0.f, 1e-2f);
        for (int i = 0; i < 2; ++i)
        {
            Vec3 const at = i == 0 ? home : out;
            Vec3 const after = i == 0 ? out : back;
            float const dir = i == 0 ? 0.f : Geo::kPi;      // the walk along the axis
            std::string_view const l = fire[static_cast<std::size_t>(i)].line;
            // through his spot, wall to wall, 10 yd wide
            CHECK(InLine(l, at, facing, at));
            CHECK(InLine(l, at, facing, Frame(at, facing, 0.f, Drill::kHallWidth / 2.f - 0.1f)));
            CHECK(InLine(l, at, facing, Frame(at, facing, 0.f, -Drill::kHallWidth / 2.f + 0.1f)));
            CHECK(!InLine(l, at, facing, Frame(at, facing, Drill::kCrossfireWidth / 2.f + 0.1f, 0.f)));
            // walked 12 yd out: his whole melee ring (5 yd) clears the lane by more than the bots' margin
            CHECK(!InLine(l, at, facing, after));
            Vec3 const nearSide = Drill::Shifted(after, Geo::WrapPi(facing + dir + Geo::kPi), 5.f + kMarginYd, 0.f);
            CHECK(!InLine(l, at, facing, nearSide));
            // a member at the lane's middle steps out in time
            CHECK((Drill::kCrossfireWidth / 2.f + kMarginYd) / kRunYdS + kReactS < Drill::kCrossfireGlowMs / 1000.f);
        }

        // Flame Wall: the 12 yd strip along the left wall (7.5 to 19.5 yd from the axis), 40 yd along it round his
        // spot (12 yd out by then), 27 yd of floor left; it burns through the second Crossfire.
        std::string_view const w = wall[0].line;
        CHECK(ShapeOf(w) == "line" && Size(w) == Drill::kFlameWallLength);
        CHECK(Key(w, "width", 0.f) == Drill::kFlameWallWidth && !Drill::Value(w, "offset") && Is(w, "at", "target"));
        CHECK(ShiftOf(w) == std::make_pair(-Drill::kFlameWallLength / 2.f,
            Drill::kHallWidth / 2.f - Drill::kFlameWallWidth / 2.f));
        CHECK(Drill::WarnMs(w) == Drill::kFlameWallGlowMs && Key(w, "persist", 0.f) == Drill::kFlameWallMs);
        CHECK(Key(w, "pct", 0.f) == 15.f && Is(w, "school", "fire") && !Drill::Value(w, "weak"));
        CHECK(wall[0].startMs >= fire[0].startMs + Drill::kCrossfireGlowMs && wall[0].startMs < fire[1].startMs);
        CHECK(wall[0].endMs > fire[1].endMs && wall[0].startMs + Drill::kFlameWallGlowMs < burn[1].endMs);
        CHECK(Drill::kHallWidth - Drill::kFlameWallWidth >= 22.f);
        CHECK(InLine(w, out, facing, Frame(out, facing, 0.f, Drill::kHallWidth / 2.f - 0.1f)));
        CHECK(InLine(w, out, facing, Frame(out, facing, 0.f, Drill::kHallWidth / 2.f - Drill::kFlameWallWidth + 0.1f)));
        CHECK(!InLine(w, out, facing, Frame(out, facing, 0.f, Drill::kHallWidth / 2.f - Drill::kFlameWallWidth
            - 0.1f)));
        // ... and he is home after the second walk: his melee ring is clear of the second lane and of the wall
        for (float deg = 0.f; deg < 360.f; deg += 15.f)
        {
            Vec3 const ring = Drill::Shifted(back, facing + Geo::DegToRad(deg), 5.f, 0.f);
            CHECK(!InLine(fire[1].line, out, facing, ring) && !InLine(w, out, facing, ring));
        }
    }

    void TestSweepProgram2()
    {
        std::string err;
        for (char const* name : { "blackrock-depths-shapes-2:2", "blackrock-depths-shapes-2" })
        {
            std::optional<Drill::Program> const p = Drill::ParseProgram(name, err);
            CHECK(p && err.empty());
            if (!p || p->entries.size() != 1)
            {
                CHECK(false);
                continue;
            }
            Drill::ProgramEntry const& e = p->entries[0];
            CHECK(e.kind == Drill::EntryKind::Drill && e.drill == &Shapes2());
            CHECK(e.cycles == 2 && e.lengthMs == 120000);
            std::vector<Drill::ScheduledStep> const s = Drill::StepSchedule(*e.drill, e.cycles);
            CHECK(s.size() == e.drill->steps.size());
            for (Drill::ScheduledStep const& st : s)
            {
                std::string_view const line = e.drill->steps[st.step].line;
                CHECK(static_cast<float>(st.delayMs + Drill::WarnMs(line)) + Key(line, "persist", 0.f) <= 120000.f);
            }
        }
        // the wave 1 name still finds the wave 1 drill
        std::optional<Drill::Program> const one = Drill::ParseProgram("blackrock-depths-shapes:2", err);
        CHECK(one && one->entries.size() == 1 && one->entries[0].drill == &Shapes());
    }
}

int main()
{
    TestTable();
    TestParseWalk();
    TestParseLeash();
    TestWalkGeometry();
    TestWalkRow();
    TestLeashModel();
    TestLeap();
    TestAxeReturn();
    TestRupture();
    TestWard();
    TestBands();
    TestLineOffset();
    TestSweepProgram();
    TestTable2();
    TestBeamRow();
    TestLonerRow();
    TestLivewireRow();
    TestCrossfireRow();
    TestSweepProgram2();
    return Report("brd_shapes_test");
}
