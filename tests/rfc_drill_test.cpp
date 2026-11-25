// Standalone tests for the Ragefire Chasm shapes of src/MechanicDrill.h (Ragefire Chasm build §3.9, WP-G1): the
// `ragefire-chasm-shapes` drill (rows borer-den, beating-chain, burn-spread, wisp-soak, shadowstep-fans,
// felstorm-spots, spike-hide, kick-lockout and the reported bursts) against the build spec rows it reproduces (OG1,
// OG2, OG4; JR2, JR5; BZ5, BZ7; TG2, TG3, TG5, TG6, TG9; heroic numbers), the grammar it adds (pick=fresh, inside=
// <yd>:<ahead>, blink, cast, the mound's and the crawl's rules) and the rows' shapes fought in the brain model on the
// live timing (BrainModel::Config::Live). Build: tests/run.sh.

#include "check.h"
#include "brain_model.h"
#include "MechanicAssign.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include "MechanicGeometry.h"
#include "MechanicHide.h"
#include "MechanicViewRules.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace BossMechanics;
using Geo::Pose;
using Geo::Vec3;
using Ms = std::chrono::milliseconds;
namespace VR = BossMechanics::ViewRules;

namespace
{
    constexpr float kEps = 1e-3f;
    constexpr float kMargin = 0.75f;        // BossMechanics.Bots.SafetyMarginYd
    constexpr float kRunYdS = 7.f;
    Ms const kStart{ 100000 };

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

    Drill::Def const& Shapes()
    {
        static Drill::Def const* d = Drill::Find("ragefire-chasm-shapes");
        return *d;
    }

    struct At
    {
        std::string_view line;
        std::uint32_t startMs = 0;
        std::uint32_t resolveMs = 0;
        std::uint32_t endMs = 0;        // resolve, plus persist, the last repeat and a mound's eruption
        std::size_t index = 0;
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
                std::uint32_t const resolve = *offsets[i] + Drill::WarnMs(line);
                std::uint32_t end = resolve + static_cast<std::uint32_t>(Key(line, "persist", 0.f))
                    + Drill::StepRepeatOffsets(line).back();
                if (ShapeOf(line) == "mound")
                    end += static_cast<std::uint32_t>(Key(line, "boom", 0.f));
                out.push_back({ line, *offsets[i], resolve, end, i });
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

    // ---- the table -------------------------------------------------------------------------------------------------
    void TestTable()
    {
        Drill::Def const* found = Drill::Find("ragefire-chasm-shapes");
        CHECK(found != nullptr);
        if (!found)
            return;
        Drill::Def const& d = *found;
        CHECK(d.cycleMs == Drill::kRfcCycleMs && d.defaultCycles == Drill::kRfcCycles && !d.ownerAtCreature);
        CHECK(Drill::kRfcCycleMs * Drill::kRfcCycles == 180000u);

        std::set<std::uint32_t> used;
        for (Drill::Step const& s : d.steps)
        {
            std::optional<std::string_view> const cycle = Drill::Value(s.line, "cycle");
            CHECK(cycle && (*cycle == "0/3" || *cycle == "1/3" || *cycle == "2/3"));
            if (std::uint32_t const tag = Tag(s.line))
            {
                used.insert(tag);
                if (ShapeOf(s.line) == "mound")
                    used.insert(Drill::MoundEruptTag(tag));
            }
        }
        std::set<std::uint32_t> all;
        for (std::uint32_t t = Drill::kRfcTagChase; t <= Drill::kRfcLastTag; ++t)
            all.insert(t);
        CHECK(used == all);
        std::set<std::uint32_t> const bar(std::begin(Drill::kRfcBarTags), std::end(Drill::kRfcBarTags));
        std::set<std::uint32_t> const group(std::begin(Drill::kRfcGroupTags), std::end(Drill::kRfcGroupTags));
        for (std::uint32_t t : all)
        {
            CHECK(Drill::RfcBarTag(t) == (bar.count(t) == 1));
            CHECK(!(bar.count(t) && group.count(t)));
        }
        CHECK(!Drill::RfcBarTag(0) && !Drill::RfcBarTag(Drill::kRfcLastTag + 1));
        for (std::uint32_t t : Drill::kRfcReferenceTags)
            CHECK(bar.count(t) == 1);
        CHECK(Drill::MoundEruptTag(Drill::kRfcTagChase) == Drill::kRfcTagErupt);
        CHECK(Drill::MoundEruptTag(Drill::kRfcTagBlowChase) == Drill::kRfcTagBlowErupt);

        // Every row inside its cycle, steps in time order.
        for (std::uint32_t c = 0; c < Drill::kRfcCycles; ++c)
        {
            std::uint32_t last = 0;
            for (At const& a : Cycle(c))
            {
                CHECK(a.startMs >= last);
                last = a.startMs;
                CHECK(a.endMs <= Drill::kRfcCycleMs);
            }
        }
        // Marked is the only aura a step reads, and every carrier mark is taken off again.
        CHECK(!WithShape(1, "marker").empty());
        CHECK(WithShape(0, "marker").empty() && WithShape(2, "marker").empty());
        // Every hit of the tank-inclusive rows carries a tank percent equal to the others' (rule 16).
        for (Drill::Step const& s : d.steps)
            if (Drill::DamageStep(s.line) && Tag(s.line) != Drill::kRfcTagBurst)
                CHECK(Key(s.line, "tankpct", -1.f) == Key(s.line, "pct", -2.f));
    }

    // ---- the grammar -----------------------------------------------------------------------------------------------
    void TestPickFresh()
    {
        std::vector<Drill::PickCandidate> c(5);
        c[0].victim = true;
        // Nobody picked: anyone, the tank included.
        CHECK(Drill::PickPool(c, Drill::PickRule::Fresh, 0.f).size() == 5);
        CHECK(!Drill::FreshRestarts(c));
        c[0].chained = c[2].chained = true;
        c[2].last = true;
        std::vector<std::size_t> const pool = Drill::PickPool(c, Drill::PickRule::Fresh, 0.f);
        CHECK((pool == std::vector<std::size_t>{ 1, 3, 4 }));
        CHECK(!Drill::FreshRestarts(c));
        // Everyone picked: a new round, never the last one twice running.
        for (Drill::PickCandidate& m : c)
            m.chained = true;
        CHECK(Drill::FreshRestarts(c));
        CHECK((Drill::PickPool(c, Drill::PickRule::Fresh, 0.f) == std::vector<std::size_t>{ 0, 1, 3, 4 }));
        // A group of one is picked again.
        std::vector<Drill::PickCandidate> one(1);
        one[0].chained = one[0].last = true;
        CHECK((Drill::PickPool(one, Drill::PickRule::Fresh, 0.f) == std::vector<std::size_t>{ 0 }));
        CHECK(Drill::PickPool({}, Drill::PickRule::Fresh, 0.f).empty());

        // A shuffled rotation over ten picks of five members: each member exactly twice, never twice running.
        for (std::uint32_t seed = 1; seed <= 20; ++seed)
        {
            std::uint32_t rng = seed * 0x9e3779b9u;
            std::vector<std::size_t> chain;
            std::size_t last = 99;
            std::array<int, 5> picked{};
            for (int k = 0; k < 10; ++k)
            {
                std::vector<Drill::PickCandidate> m(5);
                for (std::size_t i = 0; i < 5; ++i)
                {
                    m[i].chained = std::find(chain.begin(), chain.end(), i) != chain.end();
                    m[i].last = i == last;
                }
                std::vector<std::size_t> const p = Drill::PickPool(m, Drill::PickRule::Fresh, 0.f);
                rng = rng * 1664525u + 1013904223u;
                std::size_t const pick = p[(rng >> 8) % p.size()];
                CHECK(pick != last);
                if (Drill::FreshRestarts(m))
                    chain.clear();
                chain.push_back(pick);
                last = pick;
                ++picked[pick];
            }
            for (int n : picked)
                CHECK(n == 2);
        }
    }

    void TestParsers()
    {
        std::optional<Drill::BlinkStep> b = Drill::ParseBlink("blink 2 0 pick=fresh");
        CHECK(b && b->yd == 2.f && b->pick == Drill::PickRule::Fresh && !b->atPrev && !b->home && !b->same);
        b = Drill::ParseBlink("blink 2 0");
        CHECK(b && b->pick == Drill::PickRule::Any);
        b = Drill::ParseBlink("blink 2 0 pick=same");
        CHECK(b && b->same);
        b = Drill::ParseBlink("blink 0 0 at=prev");
        CHECK(b && b->atPrev && !b->home);
        b = Drill::ParseBlink("blink 0 0 home=1");
        CHECK(b && b->home && !b->atPrev);
        for (char const* bad : { "blink", "blink 2", "blink 2 500", "blink 0 0", "blink 11 0", "blink 2 0 at=prev",
            "blink 0 0 at=prev home=1", "blink 0 0 home=0", "blink 0 0 home=1 pick=any", "blink 2 0 pick=tank",
            "blink 2 0 pick=", "blink 2 0 pick=any pick=any", "blink -2 0", "blink 2 0 tag=1", "chase 2 0" })
            CHECK(!Drill::ParseBlink(bad));

        std::optional<Drill::CastStep> c = Drill::ParseCast("cast 36224 5000 kick=1 every=15000 tag=14");
        CHECK(c && c->spell == 36224u && c->ms == 5000u && c->kick && c->everyMs == 15000u && c->ledgerTag == 14u);
        c = Drill::ParseCast("cast 18088 2000 tag=15");
        CHECK(c && !c->kick && c->everyMs == 0u);
        for (char const* bad : { "cast 36224 5000", "cast 0 5000 tag=1", "cast x 5000 tag=1", "cast 36224 400 tag=1",
            "cast 36224 31000 tag=1", "cast 36224 5000 every=15000 tag=1", "cast 36224 5000 kick=1 every=5000 tag=1",
            "cast 36224 5000 kick=2 tag=1", "cast 36224 5000 tag=0", "cast 36224 5000 tag=1.5",
            "cast 36224 5000 pct=1 tag=1", "cast 999999999999 5000 tag=1" })
            CHECK(!Drill::ParseCast(bad));
        // kick=1 fails when nobody stopped it (or it never started); a bar fails when it did not run to its end.
        CHECK(Drill::CastFails(true, true, true) && !Drill::CastFails(true, true, false));
        CHECK(Drill::CastFails(true, false, false));
        CHECK(!Drill::CastFails(false, true, true) && Drill::CastFails(false, true, false));
        CHECK(Drill::CastFails(false, false, false));

        std::optional<Drill::InsideSpec> in = Drill::ParseInside("13:-8");
        CHECK(in && in->yd == 13.f && in->ahead && *in->ahead == -8.f);
        in = Drill::ParseInside("18:0");
        CHECK(in && in->ahead && *in->ahead == 0.f);
        in = Drill::ParseInside("29");
        CHECK(in && in->yd == 29.f && !in->ahead);
        for (char const* bad : { "", "0", "-3", "13:", "13:x", "13:60", ":4" })
            CHECK(!Drill::ParseInside(bad));
        // No other drill uses the fixed form, and the plain form reads as before.
        for (Drill::Def const& d : Drill::All())
            for (Drill::Step const& s : d.steps)
                if (std::optional<std::string_view> const v = Drill::Value(s.line, "inside"))
                {
                    std::optional<Drill::InsideSpec> const p = Drill::ParseInside(*v);
                    CHECK(p.has_value());
                    CHECK(p && p->ahead.has_value() == (std::string_view(d.name) == "ragefire-chasm-shapes"));
                }

        for (char const* s : { "blink", "cast", "crawl" })
            CHECK(!Drill::DamageShape(s));
        CHECK(Drill::DamageStep("mound 3 500 persist=3500 tag=1"));

        // Behind a player facing +x: 2 yd to its -x.
        Vec3 const p = Drill::BehindSpot({ 10.f, 5.f, 1.f }, 0.f, 2.f);
        CHECK_NEAR(p.x, 8.f, kEps);
        CHECK_NEAR(p.y, 5.f, kEps);
        Vec3 const s = Drill::CrawlStart({ 0.f, 0.f, 0.f }, { 0.f, 50.f, 0.f }, 4.f);
        CHECK_NEAR(s.y, 4.f, kEps);
        Vec3 const past = Drill::CrawlStart({ 0.f, 0.f, 0.f }, { 2.f, 0.f, 0.f }, 4.f);
        CHECK_NEAR(past.x, 2.f, kEps);
        Vec3 const same = Drill::CrawlStart({ 3.f, 3.f, 0.f }, { 3.f, 3.f, 0.f }, 4.f);
        CHECK_NEAR(same.x, 3.f, kEps);
    }

    // ---- the rows against the spec ---------------------------------------------------------------------------------
    void CheckMound(At const& m)
    {
        CHECK(ShapeOf(m.line) == "mound" && Size(m.line) == Drill::kMoundYd);
        CHECK(Drill::WarnMs(m.line) == Drill::kMoundArmMs);
        CHECK(Drill::WarnMs(m.line) + Key(m.line, "persist", 0.f) == static_cast<float>(Drill::kMoundLifeMs));
        CHECK(Key(m.line, "speed", 0.f) == Drill::kMoundSpeedYdS && Drill::kMoundSpeedYdS < kRunYdS);
        CHECK(Key(m.line, "erupt", 0.f) == Drill::kEruptYd);
        CHECK(Key(m.line, "boom", 0.f) == static_cast<float>(Drill::kEruptMs));
        CHECK(Key(m.line, "pct", 0.f) == 45.f && Is(m.line, "weak", "1"));
        CHECK(Is(m.line, "inside", "13:-8"));
    }

    // Oggleflint: two, three, four mounds on distinct members (the first pick=any starts the chain), 12 s apart.
    void TestBorerDen()
    {
        std::vector<At> const mounds = WithTag(0, Drill::kRfcTagChase);
        CHECK(mounds.size() == 9);
        std::array<std::uint32_t, 3> const want{ { 2, 3, 4 } };
        for (std::uint32_t h = 0; h < 3; ++h)
        {
            std::uint32_t const at = h * Drill::kHuntEveryMs;
            std::vector<At> hunt;
            for (At const& m : mounds)
                if (m.startMs == at)
                    hunt.push_back(m);
            CHECK(hunt.size() == want[h]);
            for (std::size_t i = 0; i < hunt.size(); ++i)
            {
                CheckMound(hunt[i]);
                CHECK(Is(hunt[i].line, "pick", i ? "fresh" : "any"));
                CHECK(hunt[i].endMs <= at + Drill::kHuntEveryMs);
            }
        }
        // Five at the cycle's end, each member hunted (reported: the live fifth joins at 160 s).
        std::vector<At> const five = WithTag(0, Drill::kRfcTagFiveChase);
        CHECK(five.size() == 5);
        for (std::size_t i = 0; i < five.size(); ++i)
        {
            CheckMound(five[i]);
            CHECK(Is(five[i].line, "pick", i ? "fresh" : "any") && five[i].startMs == five[0].startMs);
            CHECK(five[i].endMs == Drill::kRfcCycleMs);
        }
        CHECK(Drill::MoundEruptTag(Drill::kRfcTagFiveChase) == Drill::kRfcTagFiveErupt);
        CHECK(!Drill::RfcBarTag(Drill::kRfcTagFiveChase) && !Drill::RfcBarTag(Drill::kRfcTagFiveErupt));
        // The unit follows a hunted tank for the three hunts, then goes home before the Beating.
        std::vector<At> const chases = WithShape(0, "chase");
        CHECK(chases.size() == 1);
        std::optional<Drill::ChaseSpec> const c = Drill::ParseChase(Drill::StripPlanKeys(chases[0].line));
        CHECK(c && c->home && c->yd <= Drill::kDenYd);
        for (At const& m : mounds)
            CHECK(c && m.endMs <= c->ms);
        for (At const& m : WithTag(0, Drill::kRfcTagBlowChase))
            CHECK(c && m.startMs >= c->ms);
        // The den: the whole group's stock spots (tank 3, DPS 12, healer 18 yd) lie inside the circle.
        CHECK(std::fabs(Drill::kDenAheadYd) + 3.f <= Drill::kDenYd);
        CHECK(18.f + Drill::kDenAheadYd <= Drill::kDenYd);
        // Its area is the den's (§3.8: 527 sq yd).
        CHECK(std::fabs(Geo::kPi * Drill::kDenYd * Drill::kDenYd - 527.f) < 10.f);
    }

    // OG4: three 3 yd blows, 1.5 s each, chained on one member none of the mounds hunts; the unit lands on each as it
    // resolves (listed before the next blow's marker), then goes home.
    void TestBeatingChain()
    {
        std::vector<At> const blows = WithTag(0, Drill::kRfcTagBlow);
        CHECK(blows.size() == 2 * Drill::kBeatingBlows);
        std::vector<At> const blinks = WithShape(0, "blink");
        CHECK(blinks.size() == 2 * (Drill::kBeatingBlows + 1));
        for (std::uint32_t k = 0; k < 2; ++k)
        {
            At const* b = &blows[k * Drill::kBeatingBlows];
            CHECK(Is(b[0].line, "pick", "fresh") && Is(b[1].line, "pick", "same") && Is(b[2].line, "pick", "same"));
            for (std::uint32_t i = 0; i < Drill::kBeatingBlows; ++i)
            {
                CHECK(ShapeOf(b[i].line) == "circle" && Is(b[i].line, "at", "member"));
                CHECK(Size(b[i].line) == Drill::kBlowYd && Drill::WarnMs(b[i].line) == Drill::kBlowMs);
                CHECK(Key(b[i].line, "pct", 0.f) == 35.f && Is(b[i].line, "weak", "1"));
                CHECK(i == 0 || b[i].startMs == b[i - 1].resolveMs);
                At const& land = blinks[k * (Drill::kBeatingBlows + 1) + i];
                CHECK(land.startMs == b[i].resolveMs && Is(land.line, "at", "prev"));
                if (i + 1 < Drill::kBeatingBlows)
                    CHECK(land.index < b[i + 1].index);
            }
            CHECK(Is(blinks[k * (Drill::kBeatingBlows + 1) + Drill::kBeatingBlows].line, "home", "1"));
            // Its mounds were picked just before it (so the fresh pick leaves the hunted out) and live beside it.
            std::vector<At> mounds;
            for (At const& m : WithTag(0, Drill::kRfcTagBlowChase))
                if (m.startMs < b[0].startMs && m.startMs + 2000u > b[0].startMs)
                    mounds.push_back(m);
            CHECK(mounds.size() == k + 1);
            for (At const& m : mounds)
            {
                CheckMound(m);
                CHECK(m.startMs + Drill::kMoundLifeMs > b[1].startMs);      // still chasing at the second blow
            }
        }
        // A player that keeps stepping clears a blow: 3 yd + margin in 1.5 s at run speed.
        CHECK((static_cast<float>(Drill::kBlowMs) / 1000.f - 0.3f) * kRunYdS > Drill::kBlowYd + kMargin);
    }

    // JR2: 2, 3 (one dispelled in the middle of the cast) and 5 burning carriers, 6 yd, 2.5 s, self-keyed.
    void TestBurnSpread()
    {
        std::vector<At> const spreads = WithTag(1, Drill::kRfcTagBurn);
        CHECK(spreads.size() == 3);
        std::array<std::uint32_t, 3> const want{ { 2, 3, 5 } };
        std::vector<At> const markers = WithShape(1, "marker");
        std::uint32_t from = 0;
        for (std::size_t i = 0; i < spreads.size(); ++i)
        {
            At const& s = spreads[i];
            CHECK(Size(s.line) == Drill::kBurnYd && Drill::WarnMs(s.line) == Drill::kBurnMs);
            CHECK(Is(s.line, "self", "205009:1") && Is(s.line, "school", "fire") && Is(s.line, "weak", "1"));
            CHECK(Key(s.line, "pct", 0.f) == 45.f);
            std::uint32_t carriers = 0;
            std::uint32_t dispels = 0;
            for (At const& m : markers)
            {
                bool const put = Size(m.line) == 1.f;
                if (put && m.startMs >= from && m.startMs < s.startMs)
                    ++carriers;
                if (!put && m.startMs > s.startMs && m.startMs < s.resolveMs)
                {
                    ++dispels;
                    CHECK(Is(m.line, "one", "1"));
                    CHECK(m.startMs == s.startMs + Drill::kBurnMs / 2);
                }
                if (put)
                    CHECK(Is(m.line, "pick", "any"));   // the tank may burn
            }
            CHECK(carriers == want[i]);
            CHECK(dispels == (i == 1 ? 1u : 0u));
            // Every mark off after the resolve, before the next ones.
            bool cleared = false;
            for (At const& m : markers)
                cleared = cleared || (Size(m.line) == 0.f && !Drill::Value(m.line, "one") && m.startMs > s.resolveMs
                    && (i + 1 == spreads.size() || m.startMs < spreads[i + 1].startMs));
            CHECK(cleared);
            from = s.resolveMs;
        }
    }

    // JR5: the wisp, one body, 3 yd/s to a point 50 yd off, across a Conflagrate; and what a bot does with it.
    void TestWispSoak()
    {
        std::vector<At> const wisps = WithTag(1, Drill::kRfcTagWisp);
        CHECK(wisps.size() == 2);
        std::vector<At> const spreads = WithTag(1, Drill::kRfcTagBurn);
        for (At const& w : wisps)
        {
            CHECK(ShapeOf(w.line) == "crawl" && Size(w.line) == Drill::kWispYd);
            CHECK(Key(w.line, "yd", 0.f) == Drill::kWispGoalYd && Key(w.line, "speed", 0.f) == Drill::kWispSpeedYdS);
            CHECK(Key(w.line, "off", 0.f) == Drill::kWispOffYd && Key(w.line, "need", 0.f) == 1.f);
            CHECK(Is(w.line, "last", "healer") && Is(w.line, "pick", "any"));
            CHECK(!Drill::Value(w.line, "pct") && !Drill::DamageStep(w.line));
            CHECK(Drill::WarnMs(w.line) == Drill::kWispLifeMs);
            // It drifts while a ring set is up.
            std::uint32_t const eaten = w.startMs + static_cast<std::uint32_t>(
                (Drill::kWispGoalYd - Drill::kWispOffYd) / Drill::kWispSpeedYdS * 1000.f);
            bool crosses = false;
            for (At const& s : spreads)
                crosses = crosses || (s.startMs > w.startMs && s.resolveMs < eaten);
            CHECK(crosses);
        }
        // The farthest owner (18 yd from him, across from the goal) still gives the crawl less than the safety life.
        CHECK((18.f + Drill::kWispGoalYd) / Drill::kWispSpeedYdS * 1000.f < static_cast<float>(Drill::kWispLifeMs));
        // A spawn 4 yd off is no catch for its owner standing still.
        CHECK(Drill::kWispOffYd > Drill::kWispYd + 0.5f);

        // The claim: the nearest non-healer, the healer only when nobody else can come. The Soak's warning is the whole
        // fight's, so nobody is ever late.
        auto claimant = [](std::uint64_t guid, float yd, bool last)
        {
            Assign::Claimant c;
            c.guid = guid;
            c.bot = true;
            c.etaMs = static_cast<std::int64_t>(yd / kRunYdS * 1000.f);
            c.last = last;
            return c;
        };
        Assign::SoakRule rule;
        rule.count = 1;
        rule.deadlineMs = 4 * 3600 * 1000;
        std::vector<Assign::Claimant> group{ claimant(1, 14.f, false), claimant(2, 4.f, true), claimant(3, 9.f, false),
            claimant(4, 22.f, false), claimant(5, 16.f, false) };
        CHECK((Assign::SoakClaimers(group, rule) == std::vector<std::uint64_t>{ 3 }));
        CHECK((Assign::SoakClaimers({ group[1] }, rule) == std::vector<std::uint64_t>{ 2 }));

        // The catch: a claimer anywhere in the sim's layout runs the crawl down well before the goal, whoever tore it.
        std::array<Vec3, 5> const spots{ { { -3.f, 0.f, 0.f }, { -15.6f, -9.f, 0.f }, { -11.8f, 2.1f, 0.f },
            { -11.8f, -2.1f, 0.f }, { -10.4f, 6.f, 0.f } } };
        float worst = 0.f;
        for (float bearing : { 90.f, 270.f, 0.f, 180.f })
        {
            float const a = bearing * Geo::kPi / 180.f;
            Vec3 const goal{ Drill::kWispGoalYd * std::cos(a), Drill::kWispGoalYd * std::sin(a), 0.f };
            for (Vec3 const& ownerAt : spots)
                for (Vec3 const& claimer : spots)
                {
                    Vec3 w = Drill::CrawlStart(ownerAt, goal, Drill::kWispOffYd);
                    Vec3 p = claimer;
                    float t = 0.f;
                    bool caught = false;
                    while (Geo::Dist2d(w.x, w.y, goal.x, goal.y) > Drill::kWispReachYd)
                    {
                        t += 0.1f;
                        float const left = Geo::Dist2d(w.x, w.y, goal.x, goal.y);
                        float const u = std::min(1.f, Drill::kWispSpeedYdS * 0.1f / left);
                        w = { w.x + (goal.x - w.x) * u, w.y + (goal.y - w.y) * u, 0.f };
                        float const d = Geo::Dist2d(p.x, p.y, w.x, w.y);
                        if (d <= Drill::kWispYd)
                        {
                            caught = true;
                            break;
                        }
                        float const v = t > 0.5f ? std::min(1.f, kRunYdS * 0.1f / d) : 0.f;     // 0.5 s to react
                        p = { p.x + (w.x - p.x) * v, p.y + (w.y - p.y) * v, 0.f };
                    }
                    CHECK(caught);
                    worst = std::max(worst, t);
                }
        }
        std::printf("wisp: caught after at most %.1f s of about %.1f\n", worst,
            Drill::kWispGoalYd / Drill::kWispSpeedYdS);
        CHECK(worst < 10.f);
    }

    // BZ5, then three spins after three Shadowsteps onto three different members, and home.
    void TestShadowstepFans()
    {
        std::vector<At> const fan = WithTag(1, Drill::kRfcTagFan);
        CHECK(fan.size() == 1);
        CHECK(Size(fan[0].line) == Drill::kFanYd && Drill::WarnMs(fan[0].line) == Drill::kFanMs);
        CHECK(Is(fan[0].line, "anchor", "target") && Key(fan[0].line, "pct", 0.f) == 35.f);
        std::vector<At> const spins = WithTag(1, Drill::kRfcTagStepFan);
        std::vector<At> const steps = WithShape(1, "blink");
        CHECK(spins.size() == Drill::kShadowsteps && steps.size() == Drill::kShadowsteps + 1);
        for (std::size_t i = 0; i < spins.size(); ++i)
        {
            CHECK(Size(spins[i].line) == Drill::kStepFanYd && Drill::WarnMs(spins[i].line) == Drill::kFanMs);
            CHECK(Is(spins[i].line, "anchor", "target") && Is(spins[i].line, "weak", "1"));
            CHECK(Key(spins[i].line, "pct", 0.f) == 45.f && !Drill::Value(spins[i].line, "role"));
            // The ring registers on the landing, at the unit's new place; the next landing is well past its resolve.
            CHECK(steps[i].startMs == spins[i].startMs && steps[i].index + 1 == spins[i].index);
            CHECK(Size(steps[i].line) == Drill::kStepBehindYd);
            CHECK(Is(steps[i].line, "pick", i ? "fresh" : "any"));
            if (i)
            {
                CHECK(spins[i].startMs == spins[i - 1].startMs + Drill::kStepEveryMs);
                CHECK(spins[i].startMs >= spins[i - 1].resolveMs + 500);
            }
        }
        CHECK(Is(steps[3].line, "home", "1") && steps[3].startMs == spins[2].resolveMs + Drill::kStepHomeMs);
        CHECK(spins[0].startMs >= fan[0].resolveMs + 500);
        // The bar casts are over before he moves.
        for (At const& c : WithShape(1, "cast"))
            CHECK(c.resolveMs <= fan[0].startMs);
        CHECK(std::fabs(Drill::kRidgeAheadYd) + 3.f <= Drill::kRidgeYd);
        CHECK(18.f + Drill::kRidgeAheadYd <= Drill::kRidgeYd);
    }

    // Every drill, over its default cycles: no `blink` from the moment a ring on the unit is registered (a blink listed
    // before it at the same time is its landing) until kBlinkAfterResolveMs past its last pass.
    void TestNoBlinkOnResolve()
    {
        std::uint32_t pairs = 0;
        for (Drill::Def const& d : Drill::All())
        {
            std::vector<Drill::ScheduledStep> const all = Drill::StepSchedule(d, d.defaultCycles);
            for (Drill::ScheduledStep const& b : all)
            {
                if (ShapeOf(d.steps[b.step].line) != "blink")
                    continue;
                for (Drill::ScheduledStep const& z : all)
                {
                    std::string_view const line = d.steps[z.step].line;
                    if (!Is(line, "anchor", "target") && ShapeOf(line) != "frontal")
                        continue;
                    bool const before = b.delayMs < z.delayMs || (b.delayMs == z.delayMs && b.step < z.step);
                    std::uint32_t const end = z.delayMs + Drill::WarnMs(line)
                        + static_cast<std::uint32_t>(Key(line, "persist", 0.f)) + Drill::StepRepeatOffsets(line).back();
                    CHECK(before || b.delayMs >= end + Drill::kBlinkAfterResolveMs);
                    ++pairs;
                }
            }
        }
        CHECK(pairs > 0);
        CHECK(Drill::kStepEveryMs >= Drill::kFanMs + Drill::kBlinkAfterResolveMs);
        CHECK(Drill::kStepHomeMs >= Drill::kBlinkAfterResolveMs);
    }

    // TG3 + TG5: the 10 yd ring (2 s cast, five ticks) and ten 4 yd spots from the spin's start, a spot every 0.5 s.
    void TestFelstormSpots()
    {
        std::vector<At> const rings = WithTag(2, Drill::kRfcTagRing);
        std::vector<At> const spots = WithTag(2, Drill::kRfcTagSpot);
        CHECK(rings.size() == 3 && spots.size() == 3);
        for (std::size_t i = 0; i < rings.size(); ++i)
        {
            At const& r = rings[i];
            CHECK(Size(r.line) == Drill::kFelRingYd && Drill::WarnMs(r.line) == Drill::kFelCastMs);
            CHECK(Is(r.line, "anchor", "target"));
            CHECK(Key(r.line, "persist", 0.f) == static_cast<float>(Drill::kFelSpinMs));
            CHECK(Key(r.line, "tick", 0.f) == 1000.f && Key(r.line, "pct", 0.f) == 20.f);
            CHECK(!Drill::Value(r.line, "weak"));           // a lingering tick never weakens (rule 31)
            At const& s = spots[i];
            CHECK(s.startMs == r.resolveMs);                // the spin's start
            CHECK(Size(s.line) == Drill::kFelSpotYd && Drill::WarnMs(s.line) == Drill::kFelSpotMs);
            CHECK(Is(s.line, "at", "member") && Is(s.line, "pick", "fresh"));
            CHECK(Key(s.line, "scatter", 0.f) == Drill::kFelSpotOffYd);
            CHECK(Key(s.line, "repeat", 0.f) == static_cast<float>(Drill::kFelSpots));
            CHECK(Key(s.line, "every", 0.f) == static_cast<float>(Drill::kFelSpotEveryMs));
            std::vector<std::uint32_t> const at = Drill::StepRepeatOffsets(s.line);
            CHECK(at.size() == Drill::kFelSpots && at.back() < Drill::kFelSpinMs);      // all inside the spin
            CHECK(Key(s.line, "pct", 0.f) == 35.f && Is(s.line, "weak", "1"));
        }
        CHECK(Drill::kFelSpotMs / Drill::kFelSpotEveryMs == 3);     // three live at a time
        CHECK(std::fabs(Geo::kPi * Drill::kPlateauYd * Drill::kPlateauYd - 1040.f) < 25.f);
    }

    // TG6: the Hide as D4 registers it (radius 2.0, eye 3.05, range 60, the default behind), three spikes on random
    // points of four, then an opposite pair, at 10 yd; the placement (2 + 3) stands in the shadows, inside the walk.
    void TestSpikeHide()
    {
        std::vector<At> const hides = WithShape(2, "hide");
        CHECK(hides.size() == 2);
        std::array<std::uint32_t, 2> const want{ { 3, 2 } };
        for (std::size_t i = 0; i < hides.size(); ++i)
        {
            At const& h = hides[i];
            CHECK(Tag(h.line) == (i ? Drill::kRfcTagHideTwo : Drill::kRfcTagHideThree));
            CHECK(Size(h.line) == 60.f && Drill::WarnMs(h.line) == Drill::kNovaMs);
            CHECK(Key(h.line, "blockers", 0.f) == static_cast<float>(want[i]));
            // Normal: random points of four. Heroic: an opposite pair (two on their own ring), as D4's PickSpikes.
            CHECK(Key(h.line, "of", 0.f) == (i ? 0.f : static_cast<float>(Drill::kSpikePoints)));
            CHECK(Key(h.line, "apart", 0.f) == Drill::kSpikeRingYd && Key(h.line, "r", 0.f) == Drill::kSpikeRadiusYd);
            CHECK_NEAR(Key(h.line, "eye", 0.f), Drill::kSpikeEyeYd, kEps);
            CHECK(Key(h.line, "display", 0.f) == static_cast<float>(Drill::kSpikeDisplay));
            CHECK(Key(h.line, "scale", 0.f) == Drill::kSpikeScale);
            CHECK(!Drill::Value(h.line, "behind") && !Drill::Value(h.line, "share"));       // the defaults
            CHECK(Key(h.line, "pct", 0.f) == 75.f && !Drill::Value(h.line, "weak"));
            // Apart from the spins: 4 s after the last spot of one, the next ring not before the spikes are gone.
            for (At const& s : WithTag(2, Drill::kRfcTagSpot))
                CHECK(s.endMs <= h.startMs || s.startMs >= h.resolveMs);
        }
        CHECK_NEAR(Drill::kSpikeRadiusYd + Hide::kDefaultBehindYd, 5.f, kEps);      // §7 #19: 5 yd behind the centre

        // Every choice of spikes, the sim's layout and a tight melee group: everyone in a shadow, at most the capacity
        // behind one, side by side inside a 4.5 yd shadow 16 yd out, and a straight walk inside the 4 s less the
        // reaction and the margin (24.85 yd). Two spikes are never neighbors (STAGE-G1 Q1).
        float const reach = (static_cast<float>(Drill::kNovaMs) / 1000.f - 0.45f) * kRunYdS;
        std::uint32_t walks = 0;
        std::vector<std::vector<Vec3>> const groups{
            { { -3.f, 0.f, 0.f }, { -15.6f, 9.f, 0.f }, { -3.4f, 0.6f, 0.f }, { -11.8f, 2.1f, 0.f },
                { -3.f, -1.8f, 0.f } },
            { { -3.f, 0.f, 0.f }, { -3.4f, 0.6f, 0.f }, { -3.2f, -0.8f, 0.f }, { -2.8f, 1.4f, 0.f },
                { -3.f, -1.6f, 0.f } } };
        for (std::uint32_t mask = 0; mask < 16; ++mask)
        {
            std::vector<Hide::Blocker> blockers;
            for (std::uint32_t k = 0; k < Drill::kSpikePoints; ++k)
                if (mask & (1u << k))
                {
                    float const a = Geo::kTwoPi * static_cast<float>(k) / static_cast<float>(Drill::kSpikePoints);
                    Hide::Blocker b;
                    b.c = { std::cos(a) * Drill::kSpikeRingYd, std::sin(a) * Drill::kSpikeRingYd, 0.f };
                    b.r = Drill::kSpikeRadiusYd;
                    blockers.push_back(b);
                }
            if (blockers.size() != 3 && mask != 5 && mask != 10)
                continue;
            for (std::vector<Vec3> const& group : groups)
            {
                std::vector<Hide::Seeker> seekers;
                for (std::size_t i = 0; i < group.size(); ++i)
                    seekers.push_back({ i + 1, group[i] });
                Vec3 const source{ 0.f, 0.f, 0.f };
                std::vector<Hide::Placement> const placed = Hide::Assign(source, blockers, seekers, Hide::Params{},
                    [&](Vec3 const& p)
                {
                    return Hide::InAnyShadow2d(source, blockers, p) ? Hide::SpotBadness::Good : Hide::SpotBadness::Seen;
                });
                CHECK(placed.size() == seekers.size());
                std::vector<std::uint32_t> behind(blockers.size(), 0);
                for (Hide::Placement const& p : placed)
                {
                    CHECK(p.badness == Hide::SpotBadness::Good && p.blocker < blockers.size());
                    if (p.blocker < blockers.size())
                        ++behind[p.blocker];
                    Vec3 const from = group[static_cast<std::size_t>(p.guid - 1)];
                    float const walk = Geo::Dist2d(from.x, from.y, p.spot.x, p.spot.y);
                    ++walks;
                    CHECK(walk <= reach);
                    // Across the shadow at 15 yd: inside half of the 4.5 yd the size 2 spike casts at 16 yd (§7 #19).
                    Vec3 const c = blockers[p.blocker].c;
                    float const across = std::fabs(p.spot.x * c.y - p.spot.y * c.x) / Drill::kSpikeRingYd;
                    CHECK(across <= 2.25f);
                }
                for (std::uint32_t n : behind)
                    CHECK(n <= (blockers.size() == 2 ? 3u : 2u));
            }
        }
        std::printf("spike-hide: %u walks, none longer than %.1f yd\n", walks, reach);
    }

    // Three 5 s channels 15 s apart (listed, the first two noting the next), each followed by a must-play bar of
    // another school and then one of the kicked school.
    void TestKickLockout()
    {
        std::vector<At> const kicks = WithTag(1, Drill::kRfcTagKick);
        std::vector<At> const other = WithTag(1, Drill::kRfcTagBarOther);
        std::vector<At> const same = WithTag(1, Drill::kRfcTagBarSame);
        CHECK(kicks.size() == 3 && other.size() == 3 && same.size() == 3);
        for (std::size_t i = 0; i < kicks.size(); ++i)
        {
            std::optional<Drill::CastStep> const k = Drill::ParseCast(Drill::StripPlanKeys(kicks[i].line));
            std::optional<Drill::CastStep> const o = Drill::ParseCast(Drill::StripPlanKeys(other[i].line));
            std::optional<Drill::CastStep> const s = Drill::ParseCast(Drill::StripPlanKeys(same[i].line));
            CHECK(k && o && s);
            if (!k || !o || !s)
                return;
            CHECK(k->kick && k->spell == Drill::kKickChannelSpell && k->ms == Drill::kKickChannelMs);
            CHECK(k->everyMs == (i + 1 < kicks.size() ? Drill::kKickEveryMs : 0u));
            CHECK(!o->kick && o->spell == Drill::kBarOtherSpell && o->ms == Drill::kBarMs);
            CHECK(!s->kick && s->spell == Drill::kBarSameSpell && s->ms == Drill::kBarMs);
            if (i)
                CHECK(kicks[i].startMs - kicks[i - 1].startMs == Drill::kKickEveryMs);
            // The other school's bar first, right behind the channel (a kick's lockout of 4 to 8 s still runs), then
            // the kicked school's; both over before the next channel.
            CHECK(other[i].startMs >= kicks[i].resolveMs && other[i].startMs < kicks[i].resolveMs + 1000u);
            CHECK(same[i].startMs >= other[i].resolveMs);
            if (i + 1 < kicks.size())
                CHECK(same[i].resolveMs < kicks[i + 1].startMs);
        }
    }

    // The short windows: two 3 s casts (Drain Soul's) and two 2 s ones (the finishers'), each in the quiet seconds
    // between two shapes, 20 s (their live cadence) after the last kick of the run; the first 5 s channel comes no
    // sooner than its own 15 s, less the cycle's turn.
    void TestShortKicks()
    {
        std::optional<std::uint32_t> last;      // the run's previous kick
        std::uint32_t three = 0;
        std::uint32_t two = 0;
        for (std::uint32_t c = 0; c < Drill::kRfcCycles; ++c)
        {
            std::vector<At> const all = Cycle(c);
            for (At const& a : all)
            {
                if (ShapeOf(a.line) != "cast")
                    continue;
                std::optional<Drill::CastStep> const k = Drill::ParseCast(Drill::StripPlanKeys(a.line));
                CHECK(k.has_value());
                if (!k || !k->kick)
                    continue;
                std::uint32_t const at = c * Drill::kRfcCycleMs + a.startMs;
                bool const isThree = Tag(a.line) == Drill::kRfcTagKickThree;
                bool const isTwo = Tag(a.line) == Drill::kRfcTagKickTwo;
                CHECK(!last || at >= *last + (isThree || isTwo ? Drill::kKickShortEveryMs : Drill::kKickEveryMs));
                last = at;
                if (!isThree && !isTwo)
                    continue;
                CHECK(k->spell == (isThree ? Drill::kKickThreeSpell : Drill::kKickTwoSpell));
                CHECK(k->ms == (isThree ? Drill::kKickThreeMs : Drill::kKickTwoMs) && k->everyMs == 0u);
                three += isThree ? 1 : 0;
                two += isTwo ? 1 : 0;
                // Nothing else on the floor meanwhile, nor in the second before it.
                for (At const& o : all)
                    if (ShapeOf(o.line) != "cast" && ShapeOf(o.line) != "chase")
                        CHECK(o.endMs + 1000 <= a.startMs || o.startMs >= a.resolveMs);
            }
        }
        CHECK(three == 2 && two == 2);
    }

    void TestBurstsReported()
    {
        std::vector<At> const bursts = WithTag(2, Drill::kRfcTagBurst);
        CHECK(bursts.size() == 4);
        for (At const& b : bursts)
            CHECK(Size(b.line) == 6.f && Drill::WarnMs(b.line) == 1500u && Key(b.line, "pct", 0.f) == 30.f);
        CHECK(!Drill::RfcBarTag(Drill::kRfcTagBurst));
    }

    void TestSweepProgram()
    {
        std::string err;
        std::optional<Drill::Program> const p = Drill::ParseProgram("ragefire-chasm-shapes:3", err);
        CHECK(p && err.empty() && p->entries.size() == 1);
        if (!p || p->entries.size() != 1)
            return;
        CHECK(p->entries[0].drill == &Shapes() && p->entries[0].cycles == 3 && p->entries[0].lengthMs == 180000);
        std::vector<Drill::ScheduledStep> const s = Drill::StepSchedule(Shapes(), 3);
        CHECK(s.size() == Shapes().steps.size());
    }

    // ---- fights in the brain model, live timing --------------------------------------------------------------------
    // The sim's layout (SimMechanicsScenario, as fight_test's Fight): the boss on the origin, the group toward 180
    // degrees (the run's facing is +x): tank 3 yd, healer 18, the others at 12 (ranged) or beside the tank (melee).
    // Melee chase the boss wherever it is; ranged and the healer go back to their spots.
    float g_speed = kRunYdS;        // the bots' run speed (the controls' bots crawl)

    struct Fight
    {
        BrainModel::World w;
        std::array<bool, 5> melee{ { true, false, true, false, true } };
        std::vector<Vec3> home;
        Vec3 boss{ 0.f, 0.f, 0.f };
        std::optional<Solver::Bounds> room;
        std::uint32_t rng = 1;

        explicit Fight(std::uint32_t seed, bool rangedGroup) : rng(seed * 0x9e3779b9u + 7u)
        {
            w.cfg.params.marginMs = Ms(300);
            w.cfg.Live();
            w.cfg.speed = g_speed;
            w.cfg.chaseLanding = true;
            if (rangedGroup)
                melee = { { true, false, false, true, false } };
            std::array<float, 5> const bearing{ { 180.f, 150.f, 170.f, 190.f, 210.f } };
            for (std::size_t i = 0; i < 5; ++i)
            {
                BrainModel::Bot b;
                b.pos = Polar(i == 0 ? 3.f : melee[i] ? 3.5f : i == 1 ? 18.f : 12.f, bearing[i] + Rand(-6.f, 6.f));
                home.push_back(b.pos);
                w.bots.push_back(b);
            }
            w.isTank = [](std::size_t i) { return i == 0; };
            w.isHealer = [](std::size_t i) { return i == 1; };
            w.follow = [this](std::size_t i, Vec3& to)
            {
                if (!melee[i])
                {
                    to = home[i];
                    return Geo::Dist2d(w.bots[i].pos.x, w.bots[i].pos.y, to.x, to.y) > 1.f;
                }
                float const dx = w.bots[i].pos.x - boss.x;
                float const dy = w.bots[i].pos.y - boss.y;
                float const d = std::hypot(dx, dy);
                if (d <= 4.3f)
                    return false;
                to = { boss.x + dx / d * 3.5f, boss.y + dy / d * 3.5f, 0.f };
                return true;
            };
        }

        static Vec3 Polar(float r, float deg)
        {
            float const a = deg * Geo::kPi / 180.f;
            return { r * std::cos(a), r * std::sin(a), 0.f };
        }

        float Rand(float lo, float hi)
        {
            rng = rng * 1664525u + 1013904223u;
            return lo + (hi - lo) * static_cast<float>(rng >> 8) / 16777216.f;
        }

        std::size_t Pick(std::size_t n)
        {
            return std::min<std::size_t>(static_cast<std::size_t>(Rand(0.f, static_cast<float>(n))), n - 1);
        }

        std::vector<std::size_t> Shuffled()
        {
            std::vector<std::size_t> v{ 0, 1, 2, 3, 4 };
            for (std::size_t i = v.size() - 1; i > 0; --i)
                std::swap(v[i], v[Pick(i + 1)]);
            return v;
        }

        void Room(float yd, float ahead)
        {
            Solver::Bounds b;
            Solver::BoundsArea area;
            area.center = { ahead, 0.f, 0.f };
            area.radius = yd;
            b.areas.push_back(area);
            room = b;
        }

        // The role bands (BotRole::RoleBandOf) and the room's bounds.
        void Common(std::size_t i, Solver::Input& in) const
        {
            in.role.present = true;
            in.role.anchor = i == 1 ? w.bots[0].pos : boss;
            in.role.minYd = melee[i] ? 0.f : 8.f;
            in.role.maxYd = melee[i] ? 3.8f : 28.f;
            if (room)
                in.bounds = { *room };
        }

        bool HitBy(std::size_t i, std::uint32_t id) const
        {
            std::vector<std::uint32_t> const& h = w.bots[i].hitBy;
            return std::find(h.begin(), h.end(), id) != h.end();
        }

        static Solver::Hazard Circle(std::uint32_t id, Vec3 const& c, float r, Ms resolve)
        {
            Solver::Hazard h;
            h.id = id;
            h.shape = Shape::Circle(r);
            h.poses = { Pose{ c.x, c.y, c.z, 0.f } };
            h.marginYd = kMargin;
            h.timing = Solver::MakeInstant(resolve, Ms(500));
            return h;
        }
    };

    struct Mound
    {
        std::size_t hunted = 0;
        Vec3 pos;
        Vec3 aim;
        std::uint32_t eruptId = 0;
        bool erupted = false;
    };

    // The mounds' part of a fight: their circles in the hunted bots' views (the unit anchor's drift as
    // MechanicBotView's: the run speed over the lead to the first window, at least MovingAnchorRecheckMs), their steps
    // and their eruptions.
    struct Mounds
    {
        Fight& f;
        std::vector<Mound> list;
        float speed;
        std::uint32_t nextId = 500;

        void ToView(std::size_t i, Solver::Input& in, Ms now) const
        {
            Ms const from = kStart + Ms(Drill::kMoundArmMs);
            Ms const to = kStart + Ms(Drill::kMoundLifeMs);
            if (now < kStart + f.w.cfg.sightLag || now > to)
                return;
            for (std::size_t k = 0; k < list.size(); ++k)
                if (list[k].hunted == i)
                {
                    Solver::Hazard h = Fight::Circle(400 + static_cast<std::uint32_t>(k), list[k].pos, Drill::kMoundYd,
                        from);
                    h.timing = Solver::MakeInterval(from, to);
                    Ms const lead = std::clamp(from - now, Ms(250), Ms(1000));
                    h.driftYd = speed * std::chrono::duration<float>(lead).count();
                    h.marginYd = kMargin + h.driftYd;
                    in.hazards.push_back(h);
                }
        }

        void Before(Ms now)
        {
            for (Mound& m : list)
            {
                if (now >= kStart + Ms(Drill::kMoundLifeMs) + f.w.cfg.sightLag && !m.erupted)
                {
                    m.erupted = true;
                    m.eruptId = nextId++;
                    f.w.AddHazard(Fight::Circle(m.eruptId, m.pos, Drill::kEruptYd,
                        kStart + Ms(Drill::kMoundLifeMs + Drill::kEruptMs)));
                }
            }
        }

        void After(Ms now)
        {
            if (now < kStart || now >= kStart + Ms(Drill::kMoundLifeMs))
                return;
            for (Mound& m : list)
            {
                if ((now - kStart).count() % Drill::kMoundAimMs < f.w.cfg.step.count())
                    m.aim = f.w.bots[m.hunted].pos;
                float const d = Geo::Dist2d(m.pos.x, m.pos.y, m.aim.x, m.aim.y);
                float const u = d > 0.01f ? std::min(1.f, speed * 0.1f / d) : 0.f;
                m.pos = { m.pos.x + (m.aim.x - m.pos.x) * u, m.pos.y + (m.aim.y - m.pos.y) * u, 0.f };
            }
        }

        std::uint32_t Hits() const
        {
            std::uint32_t n = 0;
            for (Mound const& m : list)
                for (std::size_t i = 0; i < 5; ++i)
                    n += f.HitBy(i, m.eruptId) ? 1u : 0u;
            return n;
        }
    };

    struct Score
    {
        std::uint32_t hits = 0;
        std::uint32_t tries = 0;        // bot x landing
        std::uint32_t hunted = 0;       // of them, on the member the shape was for
        std::uint32_t runs = 0;
        BrainModel::Counters c;

        void Print(char const* name) const
        {
            std::printf("%-46s %4u runs: %4u hits of %5u (%.2f%%), %u on its member; %u solves, %u fallbacks\n", name,
                runs, hits, tries, tries ? 100.0 * hits / tries : 0.0, hunted, c.solves, c.Fallbacks());
        }
    };

    Score RunBorerDen(std::uint32_t mounds, bool rangedGroup, std::uint32_t seeds)
    {
        Score s;
        for (std::uint32_t seed = 1; seed <= seeds; ++seed)
        {
            Fight f(seed, rangedGroup);
            f.Room(Drill::kDenYd, Drill::kDenAheadYd);
            Mounds m{ f, {}, Drill::kMoundSpeedYdS };
            std::vector<std::size_t> const order = f.Shuffled();
            for (std::uint32_t k = 0; k < mounds; ++k)
                m.list.push_back({ order[k], f.boss, f.w.bots[order[k]].pos, 0, false });
            f.w.kinds = [&](std::size_t i, Solver::Input& in, Ms now, std::vector<BrainModel::Task>&)
            {
                f.Common(i, in);
                m.ToView(i, in, now);
            };
            // Oggleflint follows the tank as it steps (the drill's chase, his own chase live).
            for (Ms now = kStart - Ms(500); now < kStart + Ms(7500); now += f.w.cfg.step)
            {
                f.w.chaseTarget = f.boss;
                m.Before(now);
                f.w.Step(now);
                m.After(now);
                Vec3 const t = f.w.bots[0].pos;
                float const d = Geo::Dist2d(f.boss.x, f.boss.y, t.x, t.y);
                if (d > 4.3f)
                {
                    float const u = std::min(0.7f, d - 3.5f) / d;
                    f.boss = { f.boss.x + (t.x - f.boss.x) * u, f.boss.y + (t.y - f.boss.y) * u, 0.f };
                }
            }
            s.hits += m.Hits();
            for (Mound const& one : m.list)
                s.hunted += f.HitBy(one.hunted, one.eruptId) ? 1u : 0u;
            s.tries += 5 * mounds;
            s.c.Add(f.w.Total());
            ++s.runs;
        }
        return s;
    }

    // Three blows on one member, the boss landing on each (the melee go after him), mounds on others.
    Score RunBeating(std::uint32_t mounds, bool rangedGroup, std::uint32_t seeds, Score& erupt)
    {
        Score s;
        for (std::uint32_t seed = 1; seed <= seeds; ++seed)
        {
            Fight f(seed + 1000, rangedGroup);
            f.Room(Drill::kDenYd, Drill::kDenAheadYd);
            Mounds m{ f, {}, Drill::kMoundSpeedYdS };
            std::vector<std::size_t> const order = f.Shuffled();
            for (std::uint32_t k = 0; k < mounds; ++k)
                m.list.push_back({ order[k], f.boss, f.w.bots[order[k]].pos, 0, false });
            std::size_t const beaten = order[mounds];
            f.w.kinds = [&](std::size_t i, Solver::Input& in, Ms now, std::vector<BrainModel::Task>&)
            {
                f.Common(i, in);
                m.ToView(i, in, now);
            };
            Ms const first = kStart + Ms(500);
            std::array<Vec3, 3> at{};
            for (Ms now = kStart - Ms(500); now < kStart + Ms(8000); now += f.w.cfg.step)
            {
                f.w.chaseTarget = f.boss;
                m.Before(now);
                for (std::uint32_t k = 0; k < Drill::kBeatingBlows; ++k)
                {
                    Ms const marked = first + Ms(k * Drill::kBlowMs);
                    if (now == marked)
                    {
                        at[k] = f.w.bots[beaten].pos;
                        if (k)
                            f.boss = at[k - 1];
                    }
                    if (now == marked + f.w.cfg.sightLag)
                        f.w.AddHazard(Fight::Circle(1 + k, at[k], Drill::kBlowYd, marked + Ms(Drill::kBlowMs)));
                }
                if (now == first + Ms(3 * Drill::kBlowMs))
                    f.boss = at[2];
                if (now == first + Ms(3 * Drill::kBlowMs + 1000))
                    f.boss = { 0.f, 0.f, 0.f };
                f.w.Step(now);
                m.After(now);
            }
            for (std::uint32_t k = 0; k < Drill::kBeatingBlows; ++k)
                for (std::size_t i = 0; i < 5; ++i)
                    if (f.HitBy(i, 1 + k))
                    {
                        ++s.hits;
                        s.hunted += i == beaten ? 1u : 0u;
                    }
            s.tries += 5 * Drill::kBeatingBlows;
            s.c.Add(f.w.Total());
            ++s.runs;
            erupt.hits += m.Hits();
            erupt.tries += 5 * mounds;
            ++erupt.runs;
        }
        return s;
    }

    // Conflagrate on `carriers` random members (dispel: one carrier's ring drops 1.25 s in), the view's rules for a
    // Spread on named carriers: a carrier keeps everyone out of its ring, everyone else keeps out of the carriers'.
    Score RunBurn(std::uint32_t carriers, bool dispel, bool rangedGroup, std::uint32_t seeds)
    {
        Score s;
        for (std::uint32_t seed = 1; seed <= seeds; ++seed)
        {
            Fight f(seed + 2000, rangedGroup);
            std::array<bool, 5> burning{};
            std::vector<std::size_t> const order = f.Shuffled();
            for (std::uint32_t k = 0; k < carriers; ++k)
                burning[order[k]] = true;
            Ms const resolve = kStart + Ms(Drill::kBurnMs);
            Ms const to = resolve + Ms(500);
            Solver::Timing const timing = Solver::MakeInstant(resolve, to - resolve);
            std::array<bool, 5> published{};
            f.w.onSolve = [&](BrainModel::Bot const& b, Solver::Input const&, Solver::Plan const&,
                BrainModel::World::SolveAt)
            {
                published[static_cast<std::size_t>(&b - f.w.bots.data())] = true;
            };
            f.w.kinds = [&](std::size_t i, Solver::Input& in, Ms now, std::vector<BrainModel::Task>& tasks)
            {
                f.Common(i, in);
                if (now < kStart + f.w.cfg.sightLag || now > to + in.params.marginMs)
                    return;
                if (burning[i])
                {
                    std::vector<std::uint32_t> allies;
                    for (std::size_t j = 0; j < 5; ++j)
                        if (j != i)
                        {
                            float slack = 0.f;
                            Vec3 const p = f.w.SpotOf(j, slack);
                            in.allies.push_back({ static_cast<std::uint32_t>(j + 1), p, slack });
                            allies.push_back(static_cast<std::uint32_t>(in.allies.size() - 1));
                        }
                    in.carried.push_back(VR::CarriedCircle(3, Drill::kBurnYd, 10.f, kMargin, timing,
                        std::move(allies)));
                }
                bool any = burning[i];
                for (std::size_t j = 0; j < 5; ++j)
                    if (j != i && burning[j])
                    {
                        VR::Spot spot;
                        spot.p = f.w.SpotOf(j, spot.slackYd);
                        spot.source = published[j] ? VR::SpotSource::Reservation : VR::SpotSource::BotNow;
                        if (VR::PlaceOtherCircle(spot))
                        {
                            in.hazards.push_back(VR::CarrierHazard(3, Drill::kBurnYd, 10.f, spot, kMargin, timing));
                            any = true;
                        }
                    }
                if (any)
                    tasks.push_back({ 3, to, burning[i] });
            };
            for (Ms now = kStart - Ms(500); now < kStart + Ms(4000); now += f.w.cfg.step)
            {
                f.w.chaseTarget = f.boss;
                if (now == kStart + f.w.cfg.sightLag)
                    ++f.w.revision;
                if (dispel && now == kStart + Ms(Drill::kBurnMs / 2))
                {
                    burning[order[0]] = false;
                    ++f.w.revision;
                }
                f.w.Step(now);
                if (now < resolve && now + f.w.cfg.step >= resolve)
                    for (std::size_t j = 0; j < 5; ++j)
                        for (std::size_t i = 0; burning[j] && i < 5; ++i)
                            if (i != j)
                            {
                                ++s.tries;
                                if (Geo::Dist2d(f.w.bots[i].pos.x, f.w.bots[i].pos.y, f.w.bots[j].pos.x,
                                    f.w.bots[j].pos.y) <= Drill::kBurnYd)
                                    ++s.hits;
                            }
            }
            s.c.Add(f.w.Total());
            ++s.runs;
        }
        return s;
    }

    // The 8 yd fan, then three Shadowsteps `every` ms apart behind three different members, a 6 yd ring on each
    // landing. A ring is judged at the boss's place (the registry's live anchor) after the tick's blink, as
    // Registry::Tick runs the two.
    Score RunFans(bool rangedGroup, std::uint32_t seeds, Score& fan, std::uint32_t every = Drill::kStepEveryMs)
    {
        Score s;
        for (std::uint32_t seed = 1; seed <= seeds; ++seed)
        {
            Fight f(seed + 3000, rangedGroup);
            f.Room(Drill::kRidgeYd, Drill::kRidgeAheadYd);
            std::vector<std::size_t> const order = f.Shuffled();
            f.w.kinds = [&](std::size_t i, Solver::Input& in, Ms, std::vector<BrainModel::Task>&)
            {
                f.Common(i, in);
            };
            Ms const chain = kStart + Ms(Drill::kFanMs + 500);
            for (Ms now = kStart - Ms(500); now < chain + Ms(8000); now += f.w.cfg.step)
            {
                if (now == kStart + f.w.cfg.sightLag)
                    f.w.AddHazard(Fight::Circle(50, f.boss, Drill::kFanYd, kStart + Ms(Drill::kFanMs)));
                for (std::uint32_t k = 0; k < Drill::kShadowsteps; ++k)
                {
                    Ms const land = chain + Ms(k * every);
                    if (now == land)
                    {
                        Vec3 const p = f.w.bots[order[k]].pos;
                        float const o = std::atan2(f.boss.y - p.y, f.boss.x - p.x);     // the member faces him
                        f.boss = Drill::BehindSpot(p, o, Drill::kStepBehindYd);
                    }
                    if (now == land + f.w.cfg.sightLag)
                        f.w.AddHazard(Fight::Circle(60 + k, f.boss, Drill::kStepFanYd, land + Ms(Drill::kFanMs)));
                }
                for (std::uint32_t k = 0; k < Drill::kShadowsteps; ++k)
                    if (now == chain + Ms(k * every + Drill::kFanMs))
                        for (std::size_t i = 0; i < 5; ++i)
                        {
                            Vec3 const p = f.w.bots[i].pos;
                            bool const hit = Geo::Dist2d(p.x, p.y, f.boss.x, f.boss.y) <= Drill::kStepFanYd;
                            ++s.tries;
                            s.hits += hit ? 1u : 0u;
                            s.hunted += hit && i == order[k] ? 1u : 0u;
                        }
                // Home, on the first model step at or past it.
                Ms const home = chain + Ms((Drill::kShadowsteps - 1) * every + Drill::kFanMs + Drill::kStepHomeMs);
                if (now >= home && now - f.w.cfg.step < home)
                    f.boss = { 0.f, 0.f, 0.f };
                f.w.chaseTarget = f.boss;
                f.w.Step(now);
            }
            for (std::size_t i = 0; i < 5; ++i)
            {
                fan.hits += f.HitBy(i, 50) ? 1u : 0u;
                ++fan.tries;
            }
            s.c.Add(f.w.Total());
            ++s.runs;
            ++fan.runs;
        }
        return s;
    }

    // One Felstorm: the 10 yd ring and ten spots on a shuffled rotation.
    Score RunFelstorm(bool rangedGroup, std::uint32_t seeds, Score& ring)
    {
        Score s;
        for (std::uint32_t seed = 1; seed <= seeds; ++seed)
        {
            Fight f(seed + 4000, rangedGroup);
            f.Room(Drill::kPlateauYd, 0.f);
            f.w.kinds = [&](std::size_t i, Solver::Input& in, Ms, std::vector<BrainModel::Task>&)
            {
                f.Common(i, in);
            };
            Ms const spin = kStart + Ms(Drill::kFelCastMs);
            std::vector<std::size_t> order;
            std::array<Vec3, Drill::kFelSpots> at{};
            std::array<std::size_t, Drill::kFelSpots> forWhom{};
            for (Ms now = kStart - Ms(500); now < spin + Ms(8000); now += f.w.cfg.step)
            {
                f.w.chaseTarget = f.boss;
                if (now == kStart + f.w.cfg.sightLag)
                {
                    Solver::Hazard h = Fight::Circle(90, f.boss, Drill::kFelRingYd, spin);
                    h.timing = Solver::MakeInterval(spin, spin + Ms(Drill::kFelSpinMs));
                    f.w.AddHazard(h);
                }
                for (std::uint32_t k = 0; k < Drill::kFelSpots; ++k)
                {
                    Ms const marked = spin + Ms(k * Drill::kFelSpotEveryMs);
                    if (now == marked)
                    {
                        if (order.empty())
                            order = f.Shuffled();
                        forWhom[k] = order.back();
                        order.pop_back();
                        Vec3 const p = f.w.bots[forWhom[k]].pos;
                        float const r = Drill::kFelSpotOffYd * std::sqrt(f.Rand(0.f, 1.f));
                        Vec3 const off = Fight::Polar(r, f.Rand(0.f, 360.f));
                        at[k] = { p.x + off.x, p.y + off.y, 0.f };
                    }
                    if (now == marked + f.w.cfg.sightLag)
                        f.w.AddHazard(Fight::Circle(100 + k, at[k], Drill::kFelSpotYd, marked + Ms(Drill::kFelSpotMs)));
                }
                f.w.Step(now);
                // The ring's ticks (five: at the resolve and every second while below its end).
                Ms const end = now + f.w.cfg.step;
                for (Ms tick = spin; tick < spin + Ms(Drill::kFelSpinMs); tick += Ms(1000))
                    if (tick > now && tick <= end)
                        for (std::size_t i = 0; i < 5; ++i)
                        {
                            ++ring.tries;
                            if (std::hypot(f.w.bots[i].pos.x, f.w.bots[i].pos.y) <= Drill::kFelRingYd)
                                ++ring.hits;
                        }
            }
            for (std::uint32_t k = 0; k < Drill::kFelSpots; ++k)
                for (std::size_t i = 0; i < 5; ++i)
                {
                    ++s.tries;
                    if (f.HitBy(i, 100 + k))
                    {
                        ++s.hits;
                        s.hunted += i == forWhom[k] ? 1u : 0u;
                    }
                }
            s.c.Add(f.w.Total());
            ++s.runs;
            ++ring.runs;
        }
        return s;
    }

    void TestFights()
    {
        for (bool rangedGroup : { false, true })
        {
            char const* g = rangedGroup ? "ranged group" : "melee group";
            for (std::uint32_t n : { 2u, 3u, 4u })
            {
                Score const s = RunBorerDen(n, rangedGroup, 60);
                s.Print((std::string("borer-den, ") + std::to_string(n) + " mounds, " + g).c_str());
                CHECK(s.hits * 200 <= s.tries);         // at most 0.5% of bot x eruption
            }
            // Reported: five mounds at once (the live fifth from 160 s).
            Score const five = RunBorerDen(5, rangedGroup, 60);
            five.Print((std::string("borer-den, 5 mounds (reported), ") + g).c_str());
            CHECK(five.hits * 50 <= five.tries);
            for (std::uint32_t n : { 1u, 2u })
            {
                Score erupt;
                Score const s = RunBeating(n, rangedGroup, 60, erupt);
                s.Print((std::string("beating-chain, ") + std::to_string(n) + " mounds, " + g).c_str());
                erupt.Print("  its eruptions");
                CHECK(s.hits * 200 <= s.tries);
                CHECK(erupt.hits * 200 <= erupt.tries);
            }
            for (std::uint32_t n : { 2u, 3u, 5u })
            {
                Score const s = RunBurn(n, n == 3, rangedGroup, 60);
                s.Print((std::string("burn-spread, ") + std::to_string(n) + " carriers, " + g).c_str());
                CHECK(s.hits * 100 <= 2 * s.tries);
            }
            Score fan;
            Score const fans = RunFans(rangedGroup, 60, fan);
            fan.Print((std::string("shadowstep-fans, the 8 yd fan, ") + g).c_str());
            fans.Print((std::string("shadowstep-fans, the spins, ") + g).c_str());
            CHECK(fan.hits * 200 <= fan.tries);
            CHECK(fans.hits * 200 <= fans.tries);
            Score ring;
            Score const spots = RunFelstorm(rangedGroup, 60, ring);
            ring.Print((std::string("felstorm-spots, the ring's ticks, ") + g).c_str());
            spots.Print((std::string("felstorm-spots, the spots, ") + g).c_str());
            CHECK(ring.hits * 200 <= ring.tries);
            CHECK(spots.hits * 200 <= spots.tries);
        }
    }

    // The model has teeth: bots that crawl (0.5 yd/s) are hit by every row.
    void TestControls()
    {
        g_speed = 0.5f;
        Score erupt, fan, ring;
        Score const den = RunBorerDen(3, false, 10);
        Score const beating = RunBeating(1, false, 10, erupt);
        Score const burn = RunBurn(5, false, false, 10);
        Score const fans = RunFans(false, 10, fan);
        Score const spots = RunFelstorm(false, 10, ring);
        g_speed = kRunYdS;
        for (Score const& s : { den, beating, erupt, burn, fans, fan, spots, ring })
            CHECK(s.hits * 10 > s.tries);
        // A cadence equal to the warning (a landing on each resolve) hits bots that do everything right.
        Score eightYd;
        Score const coincident = RunFans(false, 60, eightYd, Drill::kFanMs);
        coincident.Print("shadowstep-fans, a landing on each resolve");
        CHECK(coincident.hits * 10 > coincident.tries);
    }
}

int main()
{
    TestTable();
    TestPickFresh();
    TestParsers();
    TestBorerDen();
    TestBeatingChain();
    TestBurnSpread();
    TestWispSoak();
    TestShadowstepFans();
    TestNoBlinkOnResolve();
    TestFelstormSpots();
    TestSpikeHide();
    TestKickLockout();
    TestShortKicks();
    TestBurstsReported();
    TestSweepProgram();
    TestFights();
    TestControls();
    return Report("rfc_drill_test");
}
