// Standalone tests for the Gnomeregan shapes of src/MechanicDrill.h (Gnomeregan build §3.9, WP-G1): the
// `gnomeregan-shapes` drill (10 rows: Techbot's spikes, Rubber Bands, bug echoes and casts, Viscous Fallout's
// drag-out, pools, Half-Life and Meltdown, Electrocutioner 6000's Justice under BM-G's move cost and ranged band, the
// Alert System's turning lights, Crowd Pummeler's called chains, Thermaplugg's plates, bombs, mines, Detonator and The
// Big One, and the Whole Workshop's load) against the build spec rows it reproduces (heroic numbers, §7 and §10
// rulings) and the grammar it adds (pick=marked:<n>, marker pick=unmarked, exempt=member, cast nokick=1 and kill=<n>,
// and the drill-only steps lights, movecost and saturated). Build: tests/run.sh.

#include "check.h"
#include "MechanicDescriptorPresets.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include "MechanicGeometry.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
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
    constexpr float kRunYdS = 7.f;
    constexpr std::uint32_t kMembers = 5;

    float Key(std::string_view line, std::string_view key, float def)
    {
        std::optional<std::string_view> v = Drill::Value(line, key);
        return v ? Drill::SignedNumber(*v).value_or(-1.f) : def;
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

    std::string Plain(std::string_view line)
    {
        return Drill::StripPlanKeys(line);
    }

    Drill::Def const& Shapes()
    {
        static Drill::Def const* d = Drill::Find("gnomeregan-shapes");
        return *d;
    }

    // A step's whole life past its start: the drill-only steps their own length, a mine its arming plus its life, a
    // projectile its warning plus its travel, a mound its chase plus the eruption, every repeat and persist included.
    std::uint32_t LifeMs(std::string_view line)
    {
        std::string_view const shape = ShapeOf(line);
        std::string const plain = Plain(line);
        if (shape == "lights")
            return Drill::ParseLights(plain)->ms;
        if (shape == "movecost")
            return Drill::ParseMoveCost(plain)->ms;
        if (shape == "saturated")
            return Drill::ParseSaturated(plain)->ms;
        if (shape == "marker")
            return 0;
        std::uint32_t life = Drill::WarnMs(line) + static_cast<std::uint32_t>(Key(line, "persist", 0.f))
            + Drill::StepRepeatOffsets(line).back();
        if (shape == "mine")
            life += static_cast<std::uint32_t>(Key(line, "life", 0.f));
        if (shape == "projectile")
            life += static_cast<std::uint32_t>(1000.f * Size(line) / Key(line, "speed", 10.f));
        if (shape == "mound")
            life += static_cast<std::uint32_t>(Key(line, "boom", static_cast<float>(Drill::kEruptMs)));
        return life;
    }

    struct At
    {
        std::string_view line;
        std::uint32_t startMs = 0;
        std::uint32_t resolveMs = 0;
        std::uint32_t endMs = 0;
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
                std::string_view const shape = ShapeOf(line);
                bool const own = shape == "lights" || shape == "movecost" || shape == "saturated" || shape == "marker";
                std::uint32_t const resolve = *offsets[i] + (own ? LifeMs(line) : Drill::WarnMs(line));
                out.push_back({ line, *offsets[i], resolve, *offsets[i] + LifeMs(line) });
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

    std::set<std::uint32_t> Set(std::uint32_t const* first, std::uint32_t const* last)
    {
        return std::set<std::uint32_t>(first, last);
    }

    // ---- a 2D model of the frame shapes (the selected unit at the origin, the run's facing +x, left +y) -------------
    struct Shape2
    {
        std::string_view kind;
        float size = 0.f;
        float inner = 0.f;
        float width = 4.f;
        float facing = 0.f;
        Vec3 at{ 0.f, 0.f, 0.f };   // a circle's center, a line's source
    };

    Shape2 ShapeFrom(std::string_view line)
    {
        Shape2 s;
        s.kind = ShapeOf(line);
        s.size = Size(line);
        s.inner = Key(line, "inner", s.size / 2.f);
        s.width = Key(line, "width", 4.f);
        s.facing = Geo::DegToRad(Key(line, "offset", 0.f));
        float ahead = 0.f;
        float left = 0.f;
        if (std::optional<std::string_view> const v = Drill::Value(line, "shift"))
        {
            std::size_t const colon = v->find(':');
            ahead = Drill::SignedNumber(v->substr(0, colon)).value_or(0.f);
            left = Drill::SignedNumber(v->substr(colon + 1)).value_or(0.f);
        }
        // shift= reads along the line's turned facing (offset=), along the run's for a circle or donut
        s.at = Drill::Shifted({ 0.f, 0.f, 0.f }, s.kind == "line" ? s.facing : 0.f, ahead, left);
        return s;
    }

    bool Contains(Shape2 const& s, float x, float y)
    {
        float const dx = x - s.at.x;
        float const dy = y - s.at.y;
        float const d = std::hypot(dx, dy);
        if (s.kind == "circle")
            return d <= s.size;
        if (s.kind == "donut")
            return d <= s.size && d >= s.inner;
        if (s.kind == "line")
        {
            float const along = dx * std::cos(s.facing) + dy * std::sin(s.facing);
            float const across = -dx * std::sin(s.facing) + dy * std::cos(s.facing);
            return along >= 0.f && along <= s.size && std::fabs(across) <= s.width / 2.f;
        }
        return false;
    }

    // Strikes of a chain registered at `callMs` (every step of the cycle starting then, carrying `tag` in tags):
    // strike time -> shapes.
    std::map<std::uint32_t, std::vector<Shape2>> Strikes(std::uint32_t cycle, std::uint32_t callMs,
        std::set<std::uint32_t> const& tags)
    {
        std::map<std::uint32_t, std::vector<Shape2>> out;
        for (At const& a : Cycle(cycle))
            if (a.startMs == callMs && tags.count(Tag(a.line)))
                out[a.resolveMs].push_back(ShapeFrom(a.line));
        return out;
    }

    // Can a player standing in a disc of `room` yd round the unit survive every strike of the chain in order, moving
    // at run speed (less 0.5 s to react) between two strikes? A grid search over the safe spots of each strike.
    bool ChainSurvivable(std::map<std::uint32_t, std::vector<Shape2>> const& strikes, float room, float stepYd,
        std::uint32_t callMs)
    {
        std::vector<std::pair<float, float>> grid;
        for (float x = -room; x <= room; x += stepYd)
            for (float y = -room; y <= room; y += stepYd)
                if (std::hypot(x, y) <= room)
                    grid.emplace_back(x, y);
        std::vector<bool> reach(grid.size(), true);
        std::uint32_t last = callMs;
        for (auto const& [at, shapes] : strikes)
        {
            float const yd = kRunYdS * (static_cast<float>(at - last) / 1000.f - 0.5f);
            std::vector<bool> next(grid.size(), false);
            bool any = false;
            for (std::size_t i = 0; i < grid.size(); ++i)
            {
                bool safe = true;
                for (Shape2 const& s : shapes)
                    safe = safe && !Contains(s, grid[i].first, grid[i].second);
                if (!safe)
                    continue;
                for (std::size_t j = 0; j < grid.size() && !next[i]; ++j)
                    if (reach[j] && std::hypot(grid[i].first - grid[j].first, grid[i].second - grid[j].second) <= yd)
                        next[i] = true;
                any = any || next[i];
            }
            if (!any)
                return false;
            reach = next;
            last = at;
        }
        return true;
    }

    // ---- the table -------------------------------------------------------------------------------------------------
    void TestTable()
    {
        Drill::Def const* found = Drill::Find("gnomeregan-shapes");
        CHECK(found != nullptr);
        if (!found)
            return;
        Drill::Def const& d = *found;
        CHECK(d.cycleMs == Drill::kGnCycleMs && d.defaultCycles == Drill::kGnCycles && !d.ownerAtCreature);
        CHECK(Drill::kGnCycleMs * Drill::kGnCycles == 360000u);

        std::set<std::uint32_t> used;
        for (Drill::Step const& s : d.steps)
        {
            std::optional<std::string_view> const cycle = Drill::Value(s.line, "cycle");
            CHECK(cycle && cycle->size() == 3 && cycle->substr(1) == "/6" && (*cycle)[0] >= '0' && (*cycle)[0] <= '5');
            CHECK(Drill::PlanKeysOk(s.line));
            if (std::uint32_t const tag = Tag(s.line))
            {
                used.insert(tag);
                if (ShapeOf(s.line) == "lights")
                    used.insert(Drill::LightsFloorTag(tag));
                if (ShapeOf(s.line) == "mound")
                    used.insert(tag + 1);       // the eruption
            }
        }
        std::set<std::uint32_t> const bar = Set(std::begin(Drill::kGnBarTags), std::end(Drill::kGnBarTags));
        std::set<std::uint32_t> const group = Set(std::begin(Drill::kGnGroupTags), std::end(Drill::kGnGroupTags));
        std::set<std::uint32_t> const refs = Set(std::begin(Drill::kGnReferenceTags),
            std::end(Drill::kGnReferenceTags));
        std::set<std::uint32_t> const reported = Set(std::begin(Drill::kGnReportedTags),
            std::end(Drill::kGnReportedTags));
        std::set<std::uint32_t> all;
        all.insert(bar.begin(), bar.end());
        all.insert(group.begin(), group.end());
        all.insert(reported.begin(), reported.end());
        // Every tag the drill posts is listed exactly once, and every listed tag is posted.
        CHECK(used == all);
        CHECK(bar.size() + group.size() + reported.size() == all.size());
        CHECK(*all.rbegin() == Drill::kGnLastTag);
        for (std::uint32_t t : refs)
            CHECK(bar.count(t) == 1);
        CHECK(refs == bar);
        // §3.9: row 10 (the Whole Workshop, tags 80+) is reported, never gated; each boss owns its block.
        for (std::uint32_t t : all)
        {
            if (t >= 80)
                CHECK(reported.count(t) == 1);
            std::string_view const gate = Drill::GnGateOf(t);
            CHECK(gate == (t < 20 ? "D1" : t < 40 ? "D2" : t < 50 ? "D3" : t < 60 ? "D4" : t < 70 ? "D5"
                : t < 80 ? "D6" : "D6Hb"));
        }
        // Each boss WP is gated by at least one tag (D6Hb only reported).
        for (std::string_view const wp : { "D1", "D2", "D3", "D4", "D5", "D6" })
        {
            bool gated = false;
            for (std::uint32_t t : bar)
                gated = gated || Drill::GnGateOf(t) == wp;
            for (std::uint32_t t : group)
                gated = gated || Drill::GnGateOf(t) == wp;
            CHECK(gated);
        }

        // Every row inside its cycle, steps in time order, one boss a cycle.
        for (std::uint32_t c = 0; c < Drill::kGnCycles; ++c)
        {
            std::uint32_t last = 0;
            CHECK(!Cycle(c).empty());
            for (At const& a : Cycle(c))
            {
                CHECK(a.startMs >= last);
                last = a.startMs;
                CHECK(a.endMs <= Drill::kGnCycleMs);
            }
        }
        // Rule 17: every hit that can reach the tank carries a tank percent equal to the others'. The no-damage
        // shapes are the RunOut before the save point, the Half-Life keep-out and the mine rings.
        for (Drill::Step const& s : d.steps)
        {
            if (!Drill::DamageStep(s.line))
                continue;
            if (Key(s.line, "pct", -1.f) == 0.f)
            {
                std::uint32_t const t = Tag(s.line);
                CHECK(t == Drill::kGnTagRubberRunOut || t == Drill::kGnTagHalfLifeOut || t == Drill::kGnTagMineRing);
                continue;
            }
            if (Tag(s.line) == Drill::kGnTagHmPuddles)
                continue;   // the hard mode's moving puddles: points near the group, reported
            CHECK(Key(s.line, "tankpct", -1.f) == Key(s.line, "pct", -2.f));
        }
        // Only Marked is a selector.
        for (Drill::Step const& s : d.steps)
            for (char const* key : { "aura", "self" })
                if (std::optional<std::string_view> v = Drill::Value(s.line, key))
                    CHECK(v->substr(0, 6) == "205009");
        // Every cycle that marks ends with Marked off everyone; the Alert and Pummeler cycles mark nobody.
        for (std::uint32_t c : { 0u, 1u, 2u, 5u })
        {
            std::vector<At> const markers = WithShape(c, "marker");
            CHECK(!markers.empty() && Size(markers.back().line) == 0.f);
            for (At const& a : Cycle(c))
                if (ShapeOf(a.line) != "marker" && ShapeOf(a.line) != "lights" && ShapeOf(a.line) != "movecost")
                    if (Drill::Value(a.line, "aura") || Drill::Value(a.line, "self")
                        || Is(a.line, "pick", "marked") || Drill::Value(a.line, "mark"))
                        CHECK(a.endMs <= markers.back().startMs);
        }
        CHECK(WithShape(3, "marker").empty() && WithShape(4, "marker").empty());
        // RB-29: no walk starts on the instant a shape on (or at) the selected unit resolves.
        for (std::uint32_t c = 0; c < Drill::kGnCycles; ++c)
            for (At const& w : WithShape(c, "walk"))
                for (At const& a : Cycle(c))
                    if (Is(a.line, "anchor", "target") || Is(a.line, "at", "target"))
                        if (a.resolveMs == w.startMs)
                            CHECK(Is(a.line, "anchor", "target") && ShapeOf(a.line) == "stack");
        // Every walk comes back: the stand-in ends each cycle where it began.
        for (std::uint32_t c = 0; c < Drill::kGnCycles; ++c)
        {
            float x = 0.f;
            float y = 0.f;
            for (At const& w : WithShape(c, "walk"))
            {
                std::optional<Drill::WalkStep> const s = Drill::ParseWalk(Plain(w.line));
                CHECK(s.has_value());
                float const o = Geo::DegToRad(Key(w.line, "offset", 0.f));
                x += s->yd * std::cos(o);
                y += s->yd * std::sin(o);
            }
            CHECK_NEAR(x, 0.f, 1e-2f);
            CHECK_NEAR(y, 0.f, 1e-2f);
        }
    }

    // ---- the grammar -----------------------------------------------------------------------------------------------
    void TestParsers()
    {
        CHECK(Drill::ParseMarkedPick("marked:1") == 1u && Drill::ParseMarkedPick("marked:10") == 10u);
        for (char const* bad : { "marked", "marked:", "marked:0", "marked:11", "marked:x", "marked:1.5", "unmarked",
            "Marked:1" })
            CHECK(!Drill::ParseMarkedPick(bad));

        std::optional<Drill::LightsStep> l = Drill::ParseLights("lights 30 42000 n=2 len=30 speed=12 lit=3000 "
            "reverse=15000 stop=2000 faster=10 tag=52");
        CHECK(l && l->angleDeg == 30.f && l->ms == 42000u && l->count == 2u && l->lengthYd == 30.f
            && l->degPerSec == 12.f && l->direction == 1 && !l->startDeg && !l->pickMember && l->litMs == 3000u
            && l->reverseMs == 15000u && l->stopMs == 2000u && l->fasterPct == 10.f && l->latchMs == 3000u
            && l->aheadMs == 3000u && l->stepMs == 500u && l->ledgerTag == 52u);
        l = Drill::ParseLights("lights 40 5000 speed=20 dir=-1 pick=any lit=2000 ahead=5000 tag=50");
        CHECK(l && l->count == 1u && l->direction == -1 && l->pickMember && l->aheadMs == 5000u);
        l = Drill::ParseLights("lights 30 4000 start=-90 tag=1");
        CHECK(l && l->startDeg && *l->startDeg == -90.f);
        for (char const* bad : { "lights 30 4000", "lights 0 4000 tag=1", "lights 181 4000 tag=1",
            "lights 30 100 tag=1", "lights 30 4000 n=0 tag=1", "lights 30 4000 n=5 tag=1",
            "lights 30 4000 len=61 tag=1",
            "lights 30 4000 speed=0 tag=1", "lights 30 4000 speed=181 tag=1", "lights 30 4000 dir=2 tag=1",
            "lights 30 4000 start=10 pick=any tag=1", "lights 30 4000 pick=fresh tag=1",
            "lights 30 4000 lit=4000 tag=1",
            "lights 30 4000 lit=100 tag=1", "lights 30 40000 reverse=2500 stop=2000 tag=1",
            "lights 30 40000 reverse=15100 tag=1", "lights 30 4000 stop=6000 tag=1", "lights 30 4000 faster=51 tag=1",
            "lights 30 4000 latch=100 tag=1", "lights 30 4000 ahead=11000 tag=1", "lights 30 4000 step=100 tag=1",
            "lights 30 4000 step=600 tag=1", "lights 30 4000 tag=0", "lights 30 4000 pct=1 tag=1" })
            CHECK(!Drill::ParseLights(bad));

        std::optional<Drill::MoveCostStep> m = Drill::ParseMoveCost("movecost 4 56000 band=11:12");
        CHECK(m && m->above == 4u && m->ms == 56000u && m->band && m->bandMinYd == 11.f && m->bandMaxYd == 12.f);
        m = Drill::ParseMoveCost("movecost 4 1000 band=0");
        CHECK(m && !m->band);
        m = Drill::ParseMoveCost("movecost 2 1000");
        CHECK(m && m->band && m->bandMinYd == 11.f);
        for (char const* bad : { "movecost 4", "movecost 10 1000", "movecost 4 0", "movecost 4 1000 band=12:11",
            "movecost 4 1000 band=11", "movecost 4 1000 band=0:12", "movecost 4 1000 tag=1", "movecost x 1000" })
            CHECK(!Drill::ParseMoveCost(bad));

        std::optional<Drill::SaturatedStep> st = Drill::ParseSaturated("saturated 1000 36000 tag=21");
        CHECK(st && st->tickMs == 1000u && st->ms == 36000u && st->ledgerTag == 21u);
        for (char const* bad : { "saturated 1000 36000", "saturated 100 36000 tag=1", "saturated 1000 500 tag=1",
            "saturated 1000 36000 tag=0", "saturated 1000 36000 x=1 tag=1" })
            CHECK(!Drill::ParseSaturated(bad));
        CHECK(Drill::InArmedPool(3.f, 7.f, 5000, 5000) && !Drill::InArmedPool(3.f, 7.f, 4999, 5000));
        CHECK(!Drill::InArmedPool(7.1f, 7.f, 9000, 5000));

        std::optional<Drill::CastStep> c = Drill::ParseCast("cast 205013 2500 nokick=1 tag=12");
        CHECK(c && c->noKick && !c->kick && !c->kill);
        c = Drill::ParseCast("cast 205010 2000 kick=1 twin=1 kill=5 tag=10");
        CHECK(c && c->kick && c->twin && c->kill == 5u && !c->noKick);
        for (char const* bad : { "cast 205011 2500 kick=1 nokick=1 tag=1", "cast 205010 2000 kick=1 kill=5 tag=1",
            "cast 205010 2000 twin=1 kill=0 tag=1", "cast 205010 2000 twin=1 kill=11 tag=1",
            "cast 205010 2000 nokick=2 tag=1" })
            CHECK(!Drill::ParseCast(bad));

        for (char const* step : { "lights", "movecost", "saturated" })
            CHECK(!Drill::DamageShape(step));
        // The BM-G preset and this drill's step agree (a sweep that read one and drilled the other would lie).
        CHECK(Drill::kGnStaticAbove == Desc::DrillPresets::kMoveCostDrillAbove);
        CHECK(Drill::kGnBandMinYd == Desc::DrillPresets::kMoveCostDrillBandMinYd);
        CHECK(Drill::kGnBandMaxYd == Desc::DrillPresets::kMoveCostDrillBandMaxYd);
        CHECK(Drill::kGnStaticStacks > Drill::kGnStaticAbove && Drill::kGnStaticStacks <= Drill::kMaxMark);
    }

    void TestPicks()
    {
        std::vector<Drill::PickCandidate> c(5);
        c[1].stacks = 1;
        c[1].marked = true;
        c[3].stacks = 2;
        c[3].marked = true;
        CHECK((Drill::PickPool(c, Drill::PickRule::MarkedAt, 0.f, 1) == std::vector<std::size_t>{ 1 }));
        CHECK((Drill::PickPool(c, Drill::PickRule::MarkedAt, 0.f, 2) == std::vector<std::size_t>{ 3 }));
        // Nobody holds that many: anyone.
        CHECK(Drill::PickPool(c, Drill::PickRule::MarkedAt, 0.f, 7).size() == 5);
        // The other rules are unchanged by the new argument.
        CHECK((Drill::PickPool(c, Drill::PickRule::Marked, 0.f, 9) == std::vector<std::size_t>{ 1, 3 }));
        CHECK(Drill::PickPool(c, Drill::PickRule::Any, 0.f).size() == 5);
    }

    // ---- the lights' clock (KT-G LightClock's twin) ----------------------------------------------------------------
    void TestLightsClock()
    {
        // Dark first: the hold runs down before any turn.
        Drill::LightsClock clock(0.f, 12.f, 1, 3000);
        clock.Advance(2000);
        CHECK_NEAR(clock.Bearing(), 0.f, kEps);
        clock.Advance(2000);                                    // 1 s held, 1 s turned
        CHECK_NEAR(Geo::RadToDeg(clock.Bearing()), 12.f, 1e-2f);
        CHECK(clock.HoldMs() == 0);
        // Two lights stand opposite.
        CHECK_NEAR(std::fabs(Geo::WrapPi(clock.BearingOf(1, 2) - clock.BearingOf(0, 2))), Geo::kPi, 1e-4f);
        // MA4: the reversal stops 2 s, then turns back.
        clock.Reverse(2000);
        CHECK(clock.Direction() == -1);
        clock.Advance(2000);
        CHECK_NEAR(Geo::RadToDeg(clock.Bearing()), 12.f, 1e-2f);
        clock.Advance(1000);
        CHECK_NEAR(Geo::RadToDeg(clock.Bearing()), 0.f, 1e-2f);
        // MA2: +10% per Spotted, no cap but the drill's.
        CHECK_NEAR(Drill::LightsFaster(12.f, 10.f), 13.2f, 1e-4f);
        CHECK(Drill::LightsFaster(170.f, 10.f) == Drill::kLightsMaxDegPerSec);
        clock.SetSpeed(500.f);
        CHECK(clock.DegPerSec() == Drill::kLightsMaxDegPerSec);

        // The swept floor holds every bearing the light takes over the look ahead (the bots' sector), and only them
        // (half the light's width either side).
        for (float speed : { 12.f, 20.f, 40.f })
            for (int dir : { 1, -1 })
                for (std::uint32_t hold : { 0u, 1000u, 4000u })
                {
                    Drill::LightsClock const start(0.7f, speed, dir, hold);
                    for (std::uint32_t ahead : { 0u, 3000u, 5000u })
                    {
                        Drill::LightSector const s = start.Swept(0, 1, 30.f, ahead);
                        Drill::LightsClock walk = start;
                        for (std::uint32_t t = 0; t <= ahead; t += 250)
                        {
                            CHECK(std::fabs(Geo::WrapPi(walk.Bearing() - s.bearing))
                                <= Geo::DegToRad(s.angleDeg - 30.f) / 2.f + 1e-3f);
                            walk.Advance(250);
                        }
                        float const turned = speed * static_cast<float>(ahead > hold ? ahead - hold : 0) / 1000.f;
                        CHECK_NEAR(s.angleDeg, std::min(360.f, 30.f + turned), 1e-2f);
                    }
                }
        // Past a full turn the sector is the whole floor.
        CHECK(Drill::LightsClock(0.f, 180.f, 1, 0).Swept(0, 1, 30.f, 3000).angleDeg == 360.f);

        // The light's own test: 2D length and half the angle either side.
        Vec3 const apex{ 0.f, 0.f, 0.f };
        CHECK(Drill::InLightCone(apex, 0.f, 30.f, 30.f, { 29.f, 0.f, 0.f }));
        CHECK(!Drill::InLightCone(apex, 0.f, 30.f, 30.f, { 31.f, 0.f, 0.f }));
        CHECK(Drill::InLightCone(apex, 0.f, 30.f, 30.f, { 10.f * std::cos(0.26f), 10.f * std::sin(0.26f), 0.f }));
        CHECK(!Drill::InLightCone(apex, 0.f, 30.f, 30.f, { 10.f * std::cos(0.27f), 10.f * std::sin(0.27f), 0.f }));
        CHECK(Drill::InLightCone(apex, 3.1f, 30.f, 30.f, { -10.f, 0.f, 50.f }));   // 2D: height ignored
        CHECK(Drill::InLightCone(apex, 1.f, 30.f, 30.f, apex));

        // Reversals: every `reverse` ms once lit, on the tick grid.
        CHECK(!Drill::LightsReverseDue(3000, 3000, 15000) && Drill::LightsReverseDue(18000, 3000, 15000));
        CHECK(Drill::LightsReverseDue(33000, 3000, 15000) && !Drill::LightsReverseDue(18250, 3000, 15000));
        CHECK(!Drill::LightsReverseDue(18000, 3000, 0));

        // RB-29 hygiene: each step's cone is armed until the next step's is (no gap the bots could read as safe).
        for (std::uint32_t step : { 250u, 500u, 1000u, 2000u })
        {
            std::uint32_t const hotFrom = Drill::kLightsWarnMs;
            std::uint32_t const hotTo = hotFrom + Drill::LightsPersistMs(step);
            CHECK(hotTo >= step + Drill::kLightsWarnMs + Drill::kLightsOverlapMs);
            CHECK(hotTo - (step + hotFrom) <= step);            // at most two cones per light at once
        }
    }

    // ---- rows 1 and 2, Techbot -------------------------------------------------------------------------------------
    void TestTechbot()
    {
        // TB1 normal: 5 yd, 2 s, 40%, Weakened, point-anchored where the pick stood (never a unit anchor).
        std::vector<At> const spikes = WithTag(0, Drill::kGnTagLagSpike);
        CHECK(spikes.size() == 2);
        for (At const& a : spikes)
            CHECK(Size(a.line) == Drill::kGnSpikeYd && Drill::WarnMs(a.line) == 2000u && Key(a.line, "pct", 0.f) == 40.f
                && Is(a.line, "at", "member") && !Drill::Value(a.line, "anchor") && Is(a.line, "weak", "1"));
        CHECK(spikes[1].startMs >= spikes[0].resolveMs + 500);

        // TB6: the two owners are marked 1 and 2 (the second one unmarked first), each one's chain picks its own.
        std::vector<At> const markers = WithShape(0, "marker");
        CHECK(markers.size() == 3);
        CHECK(Size(markers[0].line) == 1.f && Is(markers[0].line, "pick", "any"));
        CHECK(Size(markers[1].line) == 2.f && Is(markers[1].line, "pick", "unmarked"));
        std::uint32_t const band = markers[0].startMs;
        std::vector<At> const runs = WithTag(0, Drill::kGnTagRubberRunOut);
        std::vector<At> const saves = WithTag(0, Drill::kGnTagSavePoint);
        std::vector<At> const chained = WithTag(0, Drill::kGnTagRubberSpike);
        CHECK(runs.size() == 2 && saves.size() == 2 && chained.size() == 4);
        for (std::uint32_t owner : { 1u, 2u })
        {
            std::string const pick = "marked:" + std::to_string(owner);
            int r = 0;
            int s = 0;
            std::vector<std::uint32_t> spikeAt;
            for (At const& a : runs)
                if (Is(a.line, "pick", pick))
                {
                    ++r;
                    CHECK(a.startMs == band && a.resolveMs == band + Drill::kGnSaveSetMs);
                    CHECK(Size(a.line) == Drill::kGnSavePointYd);    // the owner away from the burst's reach
                }
            for (At const& a : saves)
                if (Is(a.line, "pick", pick))
                {
                    ++s;
                    // set at 2 s where the owner stands, bursting at the snap-back (5 s) on everyone else
                    CHECK(a.startMs == band + Drill::kGnSaveSetMs && a.resolveMs == band + Drill::kGnRubberMs);
                    CHECK(Is(a.line, "exempt", "member") && Is(a.line, "at", "member") && Size(a.line) == 6.f);
                    CHECK(Key(a.line, "pct", 0.f) == 60.f && !Drill::Value(a.line, "weak"));
                }
            for (At const& a : chained)
                if (Is(a.line, "pick", pick))
                {
                    spikeAt.push_back(a.startMs);
                    CHECK(Drill::WarnMs(a.line) == Drill::kGnSpikeApartMs && Size(a.line) == Drill::kGnSpikeYd);
                }
            CHECK(r == 1 && s == 1);
            // spikes at 2 s and 3.5 s, the second one landing with the snap-back
            CHECK((spikeAt == std::vector<std::uint32_t>{ band + 2000, band + 3500 }));
            CHECK(spikeAt.back() + Drill::kGnSpikeApartMs == band + Drill::kGnRubberMs);
        }
        // TB7: up to 12 echoes on each spike, 4 yd, 2 s, near members (the bugs melee them).
        std::vector<At> const echoes = WithTag(0, Drill::kGnTagBugEcho);
        CHECK(echoes.size() == 2);
        for (At const& a : echoes)
        {
            CHECK(Key(a.line, "repeat", 0.f) == static_cast<float>(Drill::kGnMaxEchoes));
            CHECK(Size(a.line) == Drill::kGnEchoYd && Drill::WarnMs(a.line) == 2000u);
            bool const onSpike = std::any_of(chained.begin(), chained.end(), [&a](At const& s)
            {
                return s.startMs == a.startMs;
            });
            CHECK(onSpike);
        }
        // Marked comes off once every band shape is over.
        CHECK(markers[2].startMs >= echoes.back().endMs && Size(markers[2].line) == 0.f);
        CHECK(WithTag(0, Drill::kGnTagBattleNet).size() == 2);

        // Row 2: three Duplications (a twin, a kill target while it casts), Patch twice listed (a bug alive) and
        // twice never kicked (no bug); one cast at a time on the stand-in, the same spell 10 s apart at least (the
        // kick's school lockout is 4 to 8 s, §7 #9).
        std::vector<At> const casts = WithShape(0, "cast");
        CHECK(casts.size() == 7);
        std::map<std::uint32_t, std::uint32_t> lastOf;
        std::uint32_t busyUntil = 0;
        int dup = 0;
        int listed = 0;
        int never = 0;
        for (At const& a : casts)
        {
            std::optional<Drill::CastStep> const c = Drill::ParseCast(Plain(a.line));
            CHECK(c.has_value());
            if (!c)
                continue;
            CHECK(a.startMs >= busyUntil + 2000);
            busyUntil = a.startMs + c->ms;
            if (lastOf.count(c->spell))
                CHECK(a.startMs >= lastOf[c->spell] + 10000);
            lastOf[c->spell] = a.startMs;
            if (c->ledgerTag == Drill::kGnTagDuplication)
            {
                ++dup;
                CHECK(c->twin && c->kick && c->kill > 0 && c->spell == Ids::SPELL_DRILL_CAST_LOW);
                CHECK(c->ms >= 2000 && c->ms <= 3000);              // §3.9: casts of 2-3 s
            }
            else if (c->ledgerTag == Drill::kGnTagPatchKick)
            {
                ++listed;
                CHECK(c->kick && !c->twin && c->spell == Ids::SPELL_DRILL_CAST_MID);
            }
            else if (c->ledgerTag == Drill::kGnTagPatchNoKick)
            {
                ++never;
                CHECK(c->noKick && !c->kick && c->spell == Ids::SPELL_DRILL_CAST_NO_KICK);
            }
        }
        CHECK(dup == 3 && listed == 2 && never == 2);
        // Row 1 is over before row 2's casts begin.
        for (At const& a : Cycle(0))
            if (ShapeOf(a.line) != "cast")
                CHECK(a.endMs <= casts.front().startMs);
    }

    // ---- rows 3 and 4, Viscous Fallout -----------------------------------------------------------------------------
    void TestFallout()
    {
        std::vector<At> const chase = WithShape(1, "chase");
        std::vector<At> const sat = WithShape(1, "saturated");
        CHECK(chase.size() == 1 && sat.size() == 1);
        std::optional<Drill::ChaseSpec> const ch = Drill::ParseChase(Plain(chase[0].line));
        std::optional<Drill::SaturatedStep> const st = Drill::ParseSaturated(Plain(sat[0].line));
        CHECK(ch && ch->home && st && st->ledgerTag == Drill::kGnTagSaturated);
        if (!ch || !st)
            return;
        // He follows the tank for the whole drag-out and Saturated is judged over the same window, a tick a second.
        CHECK(chase[0].startMs == sat[0].startMs && ch->ms == st->ms && st->tickMs == 1000u);
        std::uint32_t const dragEnd = chase[0].startMs + ch->ms;
        // 7 to 14 lasting pools (Sloughs under him and the globs' pools), every one ending by the drag-out's end.
        std::vector<At> const pools = WithTag(1, Drill::kGnTagSlough);
        CHECK(pools.size() >= Drill::kGnMinPools && pools.size() <= 14);
        float lastR = 0.f;
        int sloughs = 0;
        for (At const& a : pools)
        {
            CHECK(Key(a.line, "persist", 0.f) > 0.f && a.endMs <= dragEnd);
            if (Is(a.line, "at", "target"))
            {
                ++sloughs;
                CHECK(Size(a.line) >= Drill::kGnSloughYd && Size(a.line) >= lastR);     // growing, never smaller
                lastR = Size(a.line);
            }
        }
        CHECK(sloughs >= 6 && lastR >= Drill::kGnSloughYd + 2.f);
        // §7 #38 b: pools armed after their warning; the first Slough lands under him, so the drag must start
        // within its 2 s: the tank walks 7 yd at most out of it.
        CHECK(Drill::WarnMs(pools[0].line) == 2000u && Drill::kGnSloughYd <= kRunYdS * 2.f);
        // Volley pools under everyone, twice; the globs (two at once), each with a pool where it landed.
        std::vector<At> const volley = WithTag(1, Drill::kGnTagVolleyPool);
        CHECK(volley.size() == 2);
        for (At const& a : volley)
            CHECK(Is(a.line, "at", "each") && Size(a.line) == Drill::kGnVolleyPoolYd && Drill::WarnMs(a.line) == 1500u);
        std::vector<At> const ooze = WithTag(1, Drill::kGnTagOoze);
        CHECK(ooze.size() == 2 && ooze[0].startMs == ooze[1].startMs);
        for (At const& a : ooze)
            CHECK(Size(a.line) == Drill::kGnOozeYd && Key(a.line, "pct", 0.f) == 60.f && Is(a.line, "pick", "fresh"));
        int prevPools = 0;
        for (At const& a : pools)
            prevPools += Is(a.line, "at", "prev") && a.startMs == ooze[0].startMs ? 1 : 0;
        CHECK(prevPools == 2);
        // Leaking: 30 pools at least (a pool under each of 5 members every 3 s).
        std::vector<At> const leaks = WithTag(1, Drill::kGnTagLeaking);
        CHECK(leaks.size() == 1);
        CHECK(Key(leaks[0].line, "repeat", 0.f) * kMembers >= static_cast<float>(Drill::kGnMinLeaks));
        CHECK(Is(leaks[0].line, "at", "each") && Key(leaks[0].line, "every", 0.f) >= 1500.f + 500.f);

        // Row 4: two 20+ carriers (Marked 10) and an 8+ one, set before the spread and Half-Life.
        std::vector<At> const markers = WithShape(1, "marker");
        CHECK(markers.size() == 4);
        CHECK(Size(markers[0].line) == 10.f && Size(markers[1].line) == 10.f
            && Size(markers[2].line) == static_cast<float>(Drill::kGnHalfLifeStacks));
        std::vector<At> const melt = WithTag(1, Drill::kGnTagMeltdown);
        CHECK(melt.size() == 1 && Is(melt[0].line, "self", "205009:10") && Size(melt[0].line) == Drill::kGnMeltdownYd);
        CHECK(Key(melt[0].line, "persist", 0.f) > 0.f && melt[0].startMs > markers[2].startMs);
        std::vector<At> const stack = WithTag(1, Drill::kGnTagHalfLife);
        std::vector<At> const out = WithTag(1, Drill::kGnTagHalfLifeOut);
        CHECK(stack.size() == 2 && out.size() == 2);
        for (std::size_t i = 0; i < 2 && i < stack.size() && i < out.size(); ++i)
        {
            // Stack for 8+, keep-out for 0-7: the same 10 yd on him, the same 3 s cast, every player in exactly one.
            CHECK(stack[i].startMs == out[i].startMs && stack[i].resolveMs == out[i].resolveMs);
            CHECK(Drill::WarnMs(stack[i].line) == 3000u);
            CHECK(Is(stack[i].line, "aura", "205009:8") && Is(out[i].line, "aura", "205009:0:7"));
            CHECK(Size(stack[i].line) == Drill::kGnHalfLifeYd && Size(out[i].line) == Drill::kGnHalfLifeYd);
            CHECK(Is(stack[i].line, "anchor", "target") && Is(out[i].line, "anchor", "target"));
            // the Meltdown spread holds through the Stack: carriers 6 yd apart inside 10 yd of him
            CHECK(melt[0].resolveMs <= stack[i].startMs && melt[0].endMs >= stack[i].resolveMs);
        }
        // Three carriers and the tank 6 yd apart fit inside the 10 yd Stack (a square of side 6 has a 4.2 yd radius).
        CHECK(Drill::kGnMeltdownYd / std::sqrt(2.f) < Drill::kGnHalfLifeYd);
        CHECK(stack[1].startMs >= stack[0].resolveMs + 3000);
    }

    // ---- row 5, Electrocutioner 6000 -------------------------------------------------------------------------------
    void TestElectrocutioner()
    {
        std::vector<At> const cost = WithShape(2, "movecost");
        CHECK(cost.size() == 1);
        std::optional<Drill::MoveCostStep> const m = Drill::ParseMoveCost(Plain(cost[0].line));
        CHECK(m && m->above == Drill::kGnStaticAbove && m->band && m->bandMinYd == Drill::kGnBandMinYd
            && m->bandMaxYd == Drill::kGnBandMaxYd);
        if (!m)
            return;
        // It covers every shape of the row and ends before the cycle does (the other rows never see it).
        for (At const& a : Cycle(2))
            if (ShapeOf(a.line) != "movecost" && ShapeOf(a.line) != "leash")
                CHECK(a.startMs >= cost[0].startMs && a.endMs <= cost[0].startMs + m->ms);
        // Static carriers above the move cost (two at a time, refreshed once).
        int carriers = 0;
        for (At const& a : WithShape(2, "marker"))
            if (Size(a.line) > 0.f)
            {
                ++carriers;
                CHECK(Size(a.line) == static_cast<float>(Drill::kGnStaticStacks));
            }
        CHECK(carriers == 4);
        // Justice: donut then circle, in turn, 4 s casts; a ranged bot in the band reaches the safe side in one step.
        std::vector<std::string_view> order;
        for (At const& a : Cycle(2))
            if (Tag(a.line) == Drill::kGnTagJusticeDonut || Tag(a.line) == Drill::kGnTagJusticeCircle)
            {
                order.push_back(ShapeOf(a.line));
                CHECK(Drill::WarnMs(a.line) == Drill::kGnJusticeMs && Key(a.line, "pct", 0.f) == 75.f);
                if (ShapeOf(a.line) == "donut")
                    CHECK(Key(a.line, "inner", 0.f) == Drill::kGnJusticeInnerYd
                        && Size(a.line) == Drill::kGnJusticeOuterYd);
                else
                    CHECK(Size(a.line) == Drill::kGnJusticeCircleYd);
            }
        CHECK((order == std::vector<std::string_view>{ "donut", "circle", "donut", "circle" }));
        float const stepIn = Drill::kGnBandMaxYd - Drill::kGnJusticeInnerYd;
        float const stepOut = Drill::kGnJusticeCircleYd - Drill::kGnBandMinYd;
        CHECK(stepIn + 0.5f <= kRunYdS * (Drill::kGnJusticeMs / 1000.f - 1.f));
        CHECK(stepOut + 0.5f <= kRunYdS * (Drill::kGnJusticeMs / 1000.f - 1.f));
        // Megavolt: pairs of 40 degree / 30 yd cones aimed once at two different members.
        std::vector<At> const cones = WithTag(2, Drill::kGnTagMegavolt);
        CHECK(cones.size() == 6);
        for (std::size_t i = 0; i < cones.size(); ++i)
        {
            CHECK(Key(cones[i].line, "angle", 0.f) == 40.f && Size(cones[i].line) == 30.f
                && Is(cones[i].line, "fix", "1") && Is(cones[i].line, "to", "member"));
            CHECK(Is(cones[i].line, "pick", i % 2 ? "fresh" : "any"));
        }
        // Chain Bolt: kicks (heroic's expected kick) with the 10 yd spread, normal's 6 yd once.
        std::vector<At> const kicks = WithTag(2, Drill::kGnTagChainBoltKick);
        CHECK(kicks.size() == 2 && kicks[1].startMs >= kicks[0].startMs + 10000);
        CHECK(WithTag(2, Drill::kGnTagChainBolt).size() == 1 && WithTag(2, Drill::kGnTagChainBoltNormal).size() == 1);
        CHECK(Size(WithTag(2, Drill::kGnTagChainBolt)[0].line) == 10.f);
        CHECK(Size(WithTag(2, Drill::kGnTagChainBoltNormal)[0].line) == 6.f);
        // Discharges: a Spread of 10 from the carriers (self= at the carriers' stacks); the dodges on a carrier.
        for (At const& a : WithTag(2, Drill::kGnTagDischarge))
            CHECK(Is(a.line, "self", "205009:6") && Size(a.line) == 10.f);
        CHECK(WithTag(2, Drill::kGnTagDischarge).size() == 2);
        for (At const& a : WithTag(2, Drill::kGnTagCarrierDodge))
            CHECK(Is(a.line, "pick", "marked") && Drill::WarnMs(a.line) == 1500u);
        CHECK(WithTag(2, Drill::kGnTagCarrierDodge).size() == 2);
        // The healer's band, reported: a leash just past the band's far edge, a tick a second.
        std::vector<At> const leash = WithShape(2, "leash");
        CHECK(leash.size() == 1);
        std::optional<Drill::LeashStep> const l = Drill::ParseLeash(Plain(leash[0].line));
        CHECK(l && l->role == Drill::LeashRole::Healer && l->yd > Drill::kGnBandMaxYd
            && l->yd <= Drill::kGnBandMaxYd + 2.f
            && l->ledgerTag == Drill::kGnTagBand);
    }

    // ---- row 6, the Mobile Alert System ----------------------------------------------------------------------------
    void TestAlert()
    {
        std::vector<At> const lights = WithShape(3, "lights");
        CHECK(lights.size() == 3);
        int normal = 0;
        int heroic = 0;
        for (At const& a : lights)
        {
            std::optional<Drill::LightsStep> const l = Drill::ParseLights(Plain(a.line));
            CHECK(l.has_value());
            if (!l)
                continue;
            CHECK(l->lengthYd == Drill::kGnLightYd && l->latchMs == Drill::kGnSpottedLatchMs);
            if (l->ledgerTag == Drill::kGnTagSearchlight)
            {
                ++normal;
                // MA1: locked on a member, 40 degrees, dark 2 s, then 60 degrees clockwise in 3 s (the swept floor
                // drawn: the look ahead covers the whole sweep from the start).
                CHECK(l->count == 1 && l->pickMember && l->direction == -1 && l->angleDeg == Drill::kGnSearchlightDeg);
                CHECK(l->litMs == Drill::kGnSearchlightDimMs && l->degPerSec == Drill::kGnSearchlightDegPerSec);
                CHECK_NEAR(l->degPerSec * static_cast<float>(l->ms - l->litMs) / 1000.f, 60.f, kEps);
                CHECK(l->aheadMs >= l->ms);
                Drill::LightsClock const c(0.f, l->degPerSec, l->direction, l->litMs);
                CHECK_NEAR(c.Swept(0, 1, l->angleDeg, l->aheadMs).angleDeg, 100.f, kEps);    // KT-G's 100 degrees
            }
            else if (l->ledgerTag == Drill::kGnTagLights)
            {
                ++heroic;
                CHECK(l->count == 2 && l->angleDeg == Drill::kGnLightDeg && l->degPerSec == Drill::kGnLightDegPerSec);
                CHECK(l->litMs == Drill::kGnLightDarkMs && l->reverseMs == Drill::kGnLightReverseMs
                    && l->stopMs == Drill::kGnLightStopMs && l->fasterPct == Drill::kGnLightFasterPct);
                // Two reversals in the window (MA4: every 15 to 25 s live).
                int rev = 0;
                for (std::uint32_t t = 0; t < l->ms; t += Drill::kLightsTickMs)
                    rev += Drill::LightsReverseDue(t, l->litMs, l->reverseMs) ? 1 : 0;
                CHECK(rev == 2);
                // A player standing still is lit within 15 s of the lights coming on (180 / 12): the row cannot be
                // stood out.
                Drill::LightsClock c(0.f, l->degPerSec, l->direction, l->litMs);
                Vec3 const apex{ 0.f, 0.f, 0.f };
                for (float bearing : { 0.5f, 1.7f, 3.f, -2.2f })
                {
                    Drill::LightsClock w = c;
                    Vec3 const p{ 10.f * std::cos(bearing), 10.f * std::sin(bearing), 0.f };
                    std::uint32_t t = 0;
                    bool lit = false;
                    for (; t <= l->litMs + 15000 && !lit; t += Drill::kLightsTickMs)
                    {
                        for (std::uint32_t i = 0; i < l->count; ++i)
                            lit = lit || Drill::InLightCone(apex, w.BearingOf(i, l->count), l->angleDeg,
                                l->lengthYd, p);
                        w.Advance(Drill::kLightsTickMs);
                    }
                    CHECK(lit);
                }
                // §7 #21: the 30 yd rule over the same window, every 3 s (the latch).
                std::vector<At> const leash = WithShape(3, "leash");
                CHECK(leash.size() == 1 && leash[0].startMs == a.startMs);
                std::optional<Drill::LeashStep> const cam = Drill::ParseLeash(Plain(leash[0].line));
                CHECK(cam && cam->yd == Drill::kGnLightYd && cam->ms == l->ms
                    && cam->tickMs == Drill::kGnSpottedLatchMs && cam->ledgerTag == Drill::kGnTagCameras
                    && cam->role == Drill::LeashRole::Any);
            }
        }
        CHECK(normal == 2 && heroic == 1);
        CHECK(lights[1].startMs >= lights[0].endMs + 1000);
    }

    // ---- row 7, Crowd Pummeler 9-60 --------------------------------------------------------------------------------
    void TestPummeler()
    {
        std::set<std::uint32_t> const punches = { Drill::kGnTagLeftHook, Drill::kGnTagRightHook, Drill::kGnTagPummel,
            Drill::kGnTagClinch, Drill::kGnTagUppercut };
        std::set<std::uint32_t> calls;
        for (At const& a : Cycle(4))
            if (punches.count(Tag(a.line)))
                calls.insert(a.startMs);
        CHECK(calls.size() == 3);
        std::vector<std::size_t> lengths;
        for (std::uint32_t call : calls)
        {
            std::map<std::uint32_t, std::vector<Shape2>> const strikes = Strikes(4, call, punches);
            std::size_t const n = strikes.size();
            lengths.push_back(n);
            // Heroic timing (KT-G ComboTimeline, Sequential): a 1 s call each, the live marker 0.8 s, 1.2 s apart.
            std::size_t i = 0;
            for (auto const& [at, shapes] : strikes)
            {
                CHECK(at == call + Drill::GnPunchStrikeMs(static_cast<std::uint32_t>(n),
                    static_cast<std::uint32_t>(i)));
                // One-Two pairs only from the fourth punch, a hook with the Pummel or the Clinch.
                CHECK(shapes.size() == 1 || (shapes.size() == 2 && i >= 3));
                if (shapes.size() == 2)
                {
                    bool const hook = shapes[0].kind == "line" && shapes[0].width == 2.f * Drill::kGnHookReachYd / 2.f;
                    bool const close = shapes[1].kind == "circle" || shapes[1].kind == "donut";
                    CHECK(hook && close);
                }
                ++i;
            }
            // Every chain can be lived through in the pocket's 30 yd at run speed.
            CHECK(ChainSurvivable(strikes, Drill::kGnHookReachYd, 1.5f, call));
        }
        CHECK((lengths == std::vector<std::size_t>{ 3, 5, 8 }));
        // The frame's shapes: hooks are the halves either side of the center line to 30 yd (in front and behind,
        // so no free spot behind the stand-in), Pummel 12, Clinch 12 to 40, Uppercut the 8 yd lane 25 yd long.
        for (At const& a : Cycle(4))
        {
            std::uint32_t const t = Tag(a.line);
            if (!punches.count(t))
                continue;
            CHECK(Key(a.line, "pct", 0.f) == 60.f && Is(a.line, "weak", "1") && Is(a.line, "at", "target"));
            Shape2 const s = ShapeFrom(a.line);
            if (t == Drill::kGnTagLeftHook || t == Drill::kGnTagRightHook)
            {
                float const side = t == Drill::kGnTagLeftHook ? 1.f : -1.f;
                CHECK(Contains(s, 29.f, side * 29.f) && Contains(s, -29.f, side * 1.f)
                    && !Contains(s, 5.f, -side * 1.f));
            }
            else if (t == Drill::kGnTagPummel)
                CHECK(s.kind == "circle" && s.size == Drill::kGnPummelYd);
            else if (t == Drill::kGnTagClinch)
                CHECK(s.kind == "donut" && s.inner == Drill::kGnPummelYd && s.size == Drill::kGnClinchYd);
            else
                CHECK(s.kind == "line" && s.width == Drill::kGnUppercutWidthYd && s.size == Drill::kGnUppercutYd);
        }
    }

    // ---- rows 8 and 9, Mekgineer Thermaplugg -----------------------------------------------------------------------
    void TestThermaplugg()
    {
        // Row 9, The Big One: back 15 yd, then home at 3 yd/s under an 8 yd Stack that lands on the arrival.
        std::vector<At> const big = WithTag(4, Drill::kGnTagBigOne);
        CHECK(big.size() == 2);
        for (At const& a : big)
        {
            CHECK(ShapeOf(a.line) == "stack" && Is(a.line, "anchor", "target") && Size(a.line) == Drill::kGnBigOneYd);
            bool walks = false;
            for (At const& w : WithShape(4, "walk"))
                if (w.startMs == a.startMs)
                {
                    std::optional<Drill::WalkStep> const s = Drill::ParseWalk(Plain(w.line));
                    CHECK(s && !Drill::Value(w.line, "offset"));
                    CHECK_NEAR(s->yd / (static_cast<float>(s->ms) / 1000.f), Drill::kGnBigOneSpeedYdS, kEps);
                    CHECK(w.startMs + s->ms == a.resolveMs);
                    walks = true;
                }
            CHECK(walks);
        }

        // Row 8, the plates: on the face bearings 25 yd out, 51 degrees apart with the wide gap behind, each one
        // 21 yd or more from its neighbors; the lockout keeps a soaker off the next (Marked), the tank last.
        std::vector<At> const plates = WithTag(5, Drill::kGnTagPlate);
        CHECK(plates.size() == 5);
        std::vector<Vec3> spots;
        for (At const& a : plates)
        {
            CHECK(ShapeOf(a.line) == "soak" && Is(a.line, "need", "1") && Is(a.line, "aura", "205009:0:0")
                && Is(a.line, "mark", "1") && Is(a.line, "last", "tank") && Is(a.line, "at", "target"));
            Shape2 const s = ShapeFrom(a.line);
            CHECK_NEAR(std::hypot(s.at.x, s.at.y), Drill::kGnPlateRingYd, 0.05f);
            float const deg = Geo::RadToDeg(std::atan2(s.at.y, s.at.x));
            float const slot = (std::fabs(deg) - Drill::kGnPlateApartDeg / 2.f) / Drill::kGnPlateApartDeg;
            CHECK_NEAR(slot, std::round(slot), 0.01f);
            CHECK(std::fabs(deg) < 180.f - Drill::kGnPlateApartDeg);    // none in the gap behind
            for (Vec3 const& o : spots)
                CHECK(std::hypot(o.x - s.at.x, o.y - s.at.y) > 21.f);
            spots.push_back(s.at);
            // a plate is 25 yd out: a member at the stand-in reaches it inside the warning
            CHECK(Drill::kGnPlateRingYd / kRunYdS * 1000.f + 1000.f <= static_cast<float>(Drill::WarnMs(a.line)));
        }
        // Face priority: within a wave, the plates resolve one after another (the registry serves the earliest first),
        // and a wave never asks more bodies than the members not locked out.
        std::map<std::uint32_t, std::vector<std::uint32_t>> waves;
        for (At const& a : plates)
            waves[a.startMs].push_back(a.resolveMs);
        std::uint32_t locked = 0;
        for (auto const& [start, resolves] : waves)
        {
            std::set<std::uint32_t> const distinct(resolves.begin(), resolves.end());
            CHECK(distinct.size() == resolves.size());
            CHECK(locked + resolves.size() <= kMembers);
            locked += static_cast<std::uint32_t>(resolves.size());
        }
        // Walking bombs: 4.6 yd/s (§7 #31 B), an 8 yd blast, a RunOut on the same mark ending with the blast.
        std::vector<At> const bombs = WithTag(5, Drill::kGnTagBombChase);
        std::vector<At> const runs = WithTag(5, Drill::kGnTagBombRunOut);
        CHECK(bombs.size() == 2 && runs.size() == 2);
        for (std::size_t i = 0; i < bombs.size() && i < runs.size(); ++i)
        {
            CHECK(Key(bombs[i].line, "speed", 0.f) == Drill::kGnBombSpeedYdS
                && Key(bombs[i].line, "erupt", 0.f) == Drill::kGnBombBlastYd);
            CHECK(runs[i].startMs == bombs[i].startMs && Is(runs[i].line, "pick", "same")
                && Size(runs[i].line) == Drill::kGnBombBlastYd && runs[i].endMs == bombs[i].endMs);
        }
        // Mines: under everyone, the 3 yd trigger and the 6 yd ring laid together, gone before the hard mode rows.
        std::vector<At> const mines = WithTag(5, Drill::kGnTagMine);
        std::vector<At> const rings = WithTag(5, Drill::kGnTagMineRing);
        CHECK(mines.size() == 1 && rings.size() == 1);
        CHECK(Is(mines[0].line, "at", "each") && Size(mines[0].line) == Drill::kGnMineTriggerYd);
        CHECK(Is(rings[0].line, "at", "each") && Size(rings[0].line) == Drill::kGnMineBlastYd);
        CHECK(mines[0].startMs == rings[0].startMs && mines[0].endMs == rings[0].endMs);
        CHECK(Drill::WarnMs(mines[0].line) == 3000u);           // MT5: arms in 3 s
        // The Detonator: a 3 s kick (Fire, §7 #9).
        std::vector<At> const det = WithTag(5, Drill::kGnTagDetonator);
        CHECK(det.size() == 1);
        std::optional<Drill::CastStep> const c = Drill::ParseCast(Plain(det[0].line));
        CHECK(c && c->kick && c->ms == 3000u && c->spell == Ids::SPELL_DRILL_CAST_HIGH);
    }

    // ---- row 10, the Whole Workshop (reported) ---------------------------------------------------------------------
    void TestHardMode()
    {
        std::vector<At> const mines = WithTag(5, Drill::kGnTagHmMines);
        CHECK(mines.size() == 1 && Key(mines[0].line, "repeat", 0.f) == static_cast<float>(Drill::kGnHmMines));
        CHECK(Key(mines[0].line, "pct", 0.f) == 70.f);          // hard mode >= heroic (60)
        std::vector<At> const puddles = WithTag(5, Drill::kGnTagHmPuddles);
        CHECK(puddles.size() == 1 && Key(puddles[0].line, "repeat", 0.f) == static_cast<float>(Drill::kGnHmPuddles));
        CHECK(Key(puddles[0].line, "speed", 0.f) == Drill::kGnHmPuddleSpeedYdS && Key(puddles[0].line, "width", 0.f)
            == 4.f);
        // Two lights on the walking boss over the whole load.
        std::vector<At> const lights = WithShape(5, "lights");
        CHECK(lights.size() == 1);
        std::optional<Drill::LightsStep> const l = Drill::ParseLights(Plain(lights[0].line));
        CHECK(l && l->count == 2 && l->ledgerTag == Drill::kGnTagHmLights && l->degPerSec == Drill::kGnLightDegPerSec);
        bool walking = false;
        for (At const& w : WithShape(5, "walk"))
            walking = walking || (w.startMs == lights[0].startMs && w.endMs + 2000 >= lights[0].endMs);
        CHECK(walking);
        // The fist: two combinations of three shapes from a face 50 yd out, all registered at once, after a 4 s wind-up
        // and 2.5 s apart; each one can be lived through within 40 yd of him.
        std::set<std::uint32_t> calls;
        for (At const& a : WithTag(5, Drill::kGnTagHmFist))
            calls.insert(a.startMs);
        CHECK(calls.size() == 2);
        for (std::uint32_t call : calls)
        {
            std::map<std::uint32_t, std::vector<Shape2>> const strikes = Strikes(5, call, { Drill::kGnTagHmFist });
            CHECK(strikes.size() == 3);
            std::uint32_t k = 0;
            for (auto const& [at, shapes] : strikes)
            {
                CHECK(at == call + Drill::kGnFistWindUpMs + k * Drill::kGnFistApartMs);
                CHECK(shapes.size() == 1);
                ++k;
            }
            CHECK(ChainSurvivable(strikes, 40.f, 2.f, call));
        }
        // The fist's halves split the room on the face-to-him line; near is the 50 yd circle on the face.
        for (At const& a : WithTag(5, Drill::kGnTagHmFist))
        {
            Shape2 const s = ShapeFrom(a.line);
            if (s.kind == "line")
            {
                CHECK(Contains(s, 0.f, 0.5f) != Contains(s, 0.f, -0.5f));
                CHECK(Contains(s, 30.f, 0.5f) == Contains(s, -30.f, 0.5f));
            }
            else
                CHECK_NEAR(s.at.x, Drill::kGnFistFaceYd, kEps);
        }
        std::vector<At> const justice = WithTag(5, Drill::kGnTagHmJustice);
        CHECK(justice.size() == 1 && ShapeOf(justice[0].line) == "donut"
            && Key(justice[0].line, "inner", 0.f) == Drill::kGnJusticeInnerYd);
        // The load: everything of row 10 overlaps the Justice cast.
        for (At const& a : { mines[0], puddles[0], lights[0] })
            CHECK(a.startMs <= justice[0].startMs && a.endMs >= justice[0].resolveMs);
    }
}

int main()
{
    TestTable();
    TestParsers();
    TestPicks();
    TestLightsClock();
    TestTechbot();
    TestFallout();
    TestElectrocutioner();
    TestAlert();
    TestPummeler();
    TestThermaplugg();
    TestHardMode();
    return Report("gnomer_drill_test");
}
