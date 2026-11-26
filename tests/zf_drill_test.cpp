// Standalone tests for the Zul'Farrak shapes of src/MechanicDrill.h (Zul'Farrak build §3.9, WP-G1): the
// `zul-farrak-shapes` drill (13 rows: Theka's swarm lines, Antu'sul's trench bolts and lone-carrier dispel, Zum'rah's
// souls, walkers and the living, the Block's rotation, the stair, the crew's trio, Ukorz's kill order and storm, the
// Creeping Sands, the pool's hot puddles and Icicle, and the hard mode's gather, floes and Twin Call) against the
// build spec rows it reproduces (heroic numbers, §10 rulings) and the grammar it adds (inbox=, crawl from=, mark=,
// pick=marked|unmarked|healer, around=, judge=prefer, cast twin=1, and the drill-only steps afflict, partner, stalk and
// floes). Build: tests/run.sh.

#include "check.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include "MechanicGeometry.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
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
        static Drill::Def const* d = Drill::Find("zul-farrak-shapes");
        return *d;
    }

    struct At
    {
        std::string_view line;
        std::uint32_t startMs = 0;
        std::uint32_t resolveMs = 0;
        std::uint32_t endMs = 0;        // resolve, plus persist and the last repeat
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
                std::uint32_t const end = resolve + static_cast<std::uint32_t>(Key(line, "persist", 0.f))
                    + Drill::StepRepeatOffsets(line).back();
                out.push_back({ line, *offsets[i], resolve, end });
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

    // ---- the table -------------------------------------------------------------------------------------------------
    void TestTable()
    {
        Drill::Def const* found = Drill::Find("zul-farrak-shapes");
        CHECK(found != nullptr);
        if (!found)
            return;
        Drill::Def const& d = *found;
        CHECK(d.cycleMs == Drill::kZfCycleMs && d.defaultCycles == Drill::kZfCycles && !d.ownerAtCreature);
        CHECK(Drill::kZfCycleMs * Drill::kZfCycles == 300000u);

        std::set<std::uint32_t> used;
        for (Drill::Step const& s : d.steps)
        {
            std::optional<std::string_view> const cycle = Drill::Value(s.line, "cycle");
            CHECK(cycle && cycle->size() == 3 && cycle->substr(1) == "/5" && (*cycle)[0] >= '0' && (*cycle)[0] <= '4');
            CHECK(Drill::PlanKeysOk(s.line));
            if (std::uint32_t const tag = Tag(s.line))
            {
                used.insert(tag);
                if (Is(s.line, "judge", "prefer"))
                    used.insert(Drill::DropJudgeTag(tag));
            }
            if (std::optional<std::string_view> const trail = Drill::Value(s.line, "trailtag"))
                used.insert(static_cast<std::uint32_t>(Drill::Number(*trail).value_or(0.f)));
        }
        std::set<std::uint32_t> all;
        for (std::uint32_t t = Drill::kZfTagSwarm; t <= Drill::kZfLastTag; ++t)
            all.insert(t);
        CHECK(used == all);
        std::set<std::uint32_t> const bar = Set(std::begin(Drill::kZfBarTags), std::end(Drill::kZfBarTags));
        std::set<std::uint32_t> const group = Set(std::begin(Drill::kZfGroupTags), std::end(Drill::kZfGroupTags));
        std::set<std::uint32_t> const refs = Set(std::begin(Drill::kZfReferenceTags),
            std::end(Drill::kZfReferenceTags));
        std::set<std::uint32_t> const reported = Set(std::begin(Drill::kZfReportedTags),
            std::end(Drill::kZfReportedTags));
        // Every tag is exactly one of gated, a group bar or reported.
        for (std::uint32_t t : all)
        {
            CHECK(bar.count(t) + group.count(t) + reported.count(t) == 1);
            CHECK(Drill::ZfBarTag(t) == (bar.count(t) == 1));
        }
        CHECK(!Drill::ZfBarTag(0) && !Drill::ZfBarTag(Drill::kZfLastTag + 1));
        for (std::uint32_t t : refs)
            CHECK(bar.count(t) == 1);
        CHECK(Drill::DropJudgeTag(Drill::kZfTagIcicle) == Drill::kZfTagIcicleDrop);
        // §7 #58 A: the hard mode rows (9, 10, 13) are reported, never gated.
        for (std::uint32_t t : { Drill::kZfTagGather, Drill::kZfTagConductive, Drill::kZfTagFloe, Drill::kZfTagStrike,
            Drill::kZfTagChosen, Drill::kZfTagTwinCall })
            CHECK(reported.count(t) == 1);

        // Every row inside its cycle, steps in time order.
        for (std::uint32_t c = 0; c < Drill::kZfCycles; ++c)
        {
            std::uint32_t last = 0;
            CHECK(!Cycle(c).empty());
            for (At const& a : Cycle(c))
            {
                CHECK(a.startMs >= last);
                last = a.startMs;
                CHECK(a.endMs <= Drill::kZfCycleMs);
            }
        }
        // Rule 17: every hit that can reach the tank carries a tank percent equal to the others'; the no-damage
        // circles (pct=0) are the reported water, pits' slow and Icicle, and the gated pit landings (Sunk).
        for (Drill::Step const& s : d.steps)
        {
            if (!Drill::DamageStep(s.line))
                continue;
            if (Key(s.line, "pct", -1.f) == 0.f)
            {
                std::uint32_t const t = Tag(s.line);
                CHECK(t == Drill::kZfTagWater || t == Drill::kZfTagPit || t == Drill::kZfTagPitSlow
                    || t == Drill::kZfTagIcicle);
                continue;
            }
            if (Tag(s.line) == Drill::kZfTagStrike)
                continue;   // Storm Strikes: random points near a player, the hard mode's (reported)
            CHECK(Key(s.line, "tankpct", -1.f) == Key(s.line, "pct", -2.f));
        }
        // Only Marked is a selector; the dispel stand-in is the M5 Magic debuff.
        for (Drill::Step const& s : d.steps)
            if (std::optional<std::string_view> v = Drill::Value(s.line, "aura"))
                CHECK(v->substr(0, 6) == "205009");
        CHECK(Drill::kZfDispelSpell == Ids::SPELL_DRILL_DEBUFF_MAGIC);
        CHECK(Drill::kZfTwinCallSpell == Ids::SPELL_DRILL_CAST_LOW);
        CHECK(Drill::kZfTwinCasterEntry == Ids::NPC_DRILL_CASTER);
        CHECK(Drill::kZfPartnerEntry == Ids::NPC_DRILL_ADD);
        // Every marker cycle ends with Marked off everyone.
        for (std::uint32_t c = 0; c < 3; ++c)
        {
            std::vector<At> const markers = WithShape(c, "marker");
            CHECK(!markers.empty() && Size(markers.back().line) == 0.f);
        }
        CHECK(WithShape(3, "marker").empty() && WithShape(4, "marker").empty());
    }

    // ---- the grammar -----------------------------------------------------------------------------------------------
    void TestParsers()
    {
        std::optional<Drill::BoxSpec> box = Drill::ParseInBox("10x42:-19");
        CHECK(box && box->width == 10.f && box->length == 42.f && box->ahead == -19.f);
        box = Drill::ParseInBox("8x20");
        CHECK(box && box->ahead == 0.f);
        for (char const* bad : { "", "10", "10x", "x42", "0x42", "10x0", "10x101", "10x42:", "10x42:60", "10x42:a",
            "-10x42" })
            CHECK(!Drill::ParseInBox(bad));
        // The box's corner and its inside, facing along +x: 42 long, 10 wide, its center 19 yd back.
        Vec3 const boss{ 0.f, 0.f, 0.f };
        Vec3 const c = Drill::Shifted(boss, 0.f, -19.f, 0.f);
        Vec3 const corner = Drill::BoxCorner(c, 0.f, 10.f, 42.f);
        CHECK_NEAR(corner.x, -40.f, kEps);
        CHECK_NEAR(corner.y, -5.f, kEps);
        CHECK(Drill::InBox(c, 0.f, 10.f, 42.f, boss));                      // he stands in it, 2 yd from its end
        CHECK(Drill::InBox(c, 0.f, 10.f, 42.f, { 1.9f, 0.f, 0.f }));
        CHECK(!Drill::InBox(c, 0.f, 10.f, 42.f, { 2.1f, 0.f, 0.f }));
        CHECK(Drill::InBox(c, 0.f, 10.f, 42.f, { -39.9f, 4.9f, 0.f }));
        CHECK(!Drill::InBox(c, 0.f, 10.f, 42.f, { -20.f, 5.1f, 0.f }));
        // Turned: facing +y.
        Vec3 const cy = Drill::Shifted(boss, Geo::kPi / 2.f, -19.f, 0.f);
        CHECK(Drill::InBox(cy, Geo::kPi / 2.f, 10.f, 42.f, { 4.9f, -30.f, 0.f }));
        CHECK(!Drill::InBox(cy, Geo::kPi / 2.f, 10.f, 42.f, { 5.1f, -30.f, 0.f }));

        std::optional<Drill::PolarSpec> from = Drill::ParseFrom("50:150");
        CHECK(from && from->yd == 50.f && from->second == 150.f);
        from = Drill::ParseFrom("30:-45");
        CHECK(from && from->second == -45.f);
        for (char const* bad : { "", "50", "50:", ":150", "0:150", "101:150", "50:400", "a:1" })
            CHECK(!Drill::ParseFrom(bad));
        Vec3 const p = Drill::FromPoint(boss, 0.f, 50.f, 180.f);
        CHECK_NEAR(p.x, -50.f, kEps);
        CHECK_NEAR(p.y, 0.f, 1e-2f);

        std::optional<Drill::PolarSpec> around = Drill::ParseAround("30:11");
        CHECK(around && around->yd == 30.f && around->second == 11.f);
        for (char const* bad : { "30", "30:1", "30:17", "0:4", "30:x", "30:2.5" })
            CHECK(!Drill::ParseAround(bad));

        CHECK(Drill::MarkedAfter(1, 1) == 2 && Drill::MarkedAfter(9, 3) == Drill::kMaxMark
            && Drill::MarkedAfter(0, 0) == 0);
        CHECK(Drill::kMaxMark == Ids::MARKED_DRILL_MAX_STACKS);

        std::optional<Drill::AfflictStep> a = Drill::ParseAfflict("afflict 205014 10000 pick=same lone=6 burst=6 "
            "tag=7");
        CHECK(a && a->spell == 205014u && a->ms == 10000u && a->count == 1u && a->same && a->lone == 6.f
            && a->burst == 6.f && a->ledgerTag == 7u);
        a = Drill::ParseAfflict("afflict 205014 2000 n=3 tag=1");
        CHECK(a && a->count == 3u && !a->same && a->lone == 6.f);
        for (char const* bad : { "afflict 205014 10000", "afflict 0 10000 tag=1", "afflict 205014 500 tag=1",
            "afflict 205014 10000 n=0 tag=1", "afflict 205014 10000 n=6 tag=1", "afflict 205014 10000 pick=fresh tag=1",
            "afflict 205014 10000 lone=0 tag=1", "afflict 205014 10000 burst=21 tag=1", "afflict 205014 10000 tag=0",
            "afflict 205014 10000 pct=1 tag=1", "afflict x 10000 tag=1" })
            CHECK(!Drill::ParseAfflict(bad));
        CHECK(Drill::ShatterHits({ 0.f, 0.f, 0.f }, { 6.f, 0.f, 0.f }, 6.f));
        CHECK(!Drill::ShatterHits({ 0.f, 0.f, 0.f }, { 4.3f, 4.3f, 0.f }, 6.f));

        std::optional<Drill::PartnerStep> pr = Drill::ParsePartner("partner 300405 28000 every=15000 grace=3000 "
            "apart=6 tag=23");
        CHECK(pr && pr->entry == 300405u && pr->ms == 28000u && pr->everyMs == 15000u && pr->graceMs == 3000u
            && pr->tickMs == 1000u && pr->apart == 6.f && pr->ledgerTag == 23u);
        for (char const* bad : { "partner 300405 28000", "partner 0 28000 tag=1", "partner 300405 1000 tag=1",
            "partner 300405 28000 every=1000 tag=1", "partner 300405 28000 every=5000 grace=5000 tag=1",
            "partner 300405 28000 tick=100 tag=1", "partner 300405 28000 apart=1 tag=1",
            "partner 300405 28000 x=1 tag=1" })
            CHECK(!Drill::ParsePartner(bad));
        // The kill order starts on the selected unit and flips every 15 s; nothing judged within 3 s of a flip.
        CHECK(Drill::PartnerSelectedFavored(0, 15000) && Drill::PartnerSelectedFavored(14999, 15000));
        CHECK(!Drill::PartnerSelectedFavored(15000, 15000) && Drill::PartnerSelectedFavored(30000, 15000));
        CHECK(!Drill::PartnerJudged(1000, 15000, 3000) && Drill::PartnerJudged(3000, 15000, 3000));
        CHECK(!Drill::PartnerJudged(17000, 15000, 3000) && Drill::PartnerJudged(18000, 15000, 3000));

        std::optional<Drill::StalkStep> st = Drill::ParseStalk("stalk 5 8000 every=2000 trail=4 trailms=15000 "
            "trailtag=25 home=1");
        CHECK(st && st->speed == 5.f && st->ms == 8000u && st->everyMs == 2000u && st->trail == 4.f
            && st->trailMs == 15000u && st->trailTag == 25u && st->home);
        st = Drill::ParseStalk("stalk 4 1000");
        CHECK(st && st->trail == 0.f && !st->home);
        for (char const* bad : { "stalk 5", "stalk 0 8000", "stalk 15 8000", "stalk 5 100", "stalk 5 8000 every=100",
            "stalk 5 8000 trail=4", "stalk 5 8000 trail=4 trailtag=25 trailms=500", "stalk 5 8000 home=2",
            "stalk 5 8000 trail=11 trailtag=1", "stalk 5 8000 tag=1" })
            CHECK(!Drill::ParseStalk(bad));
        // The trail: one circle every 2 yd walked, the rest carried over (5 yd/s, 0.25 s steps: one every 1.6 steps).
        Drill::TrailMeter meter;
        int drops = 0;
        for (int k = 0; k < 32; ++k)
            drops += meter.Walk(5.f * 0.25f) ? 1 : 0;
        CHECK(drops == 20);         // 40 yd walked in 8 s

        std::optional<Drill::FloesStep> f = Drill::ParseFloes("floes 8 7000 n=6 ring=18 center=1 tag=37");
        CHECK(f && f->r == 8.f && f->ms == 7000u && f->n == 6u && f->ring == 18.f && f->center && f->inset == 0.f
            && f->graceMs == 2000u
            && f->tickMs == 1000u && f->ledgerTag == 37u);
        for (char const* bad : { "floes 8 7000 ring=18 tag=1", "floes 8 7000 n=6 tag=1",
            "floes 0 7000 n=6 ring=18 tag=1",
            "floes 8 500 n=6 ring=18 tag=1", "floes 8 7000 n=13 ring=18 tag=1", "floes 8 7000 n=6 ring=41 tag=1",
            "floes 8 7000 n=6 ring=18 grace=7000 tag=1", "floes 8 7000 n=6 ring=18 center=2 tag=1",
            "floes 8 7000 n=6 ring=18 centre=1 tag=1", "floes 8 7000 n=6 ring=18 inset=8 tag=1",
            "floes 8 7000 n=6 ring=18 inset=-1 tag=1",
            "floes 8 7000 n=6 ring=18" })
            CHECK(!Drill::ParseFloes(bad));
        std::optional<Drill::FloesStep> const in =
            Drill::ParseFloes("floes 8 7000 n=4 ring=18 center=1 inset=1 tag=37");
        CHECK(in && in->inset == 1.f && in->r == 8.f && in->n == 4u && in->center);
        std::vector<Vec3> const floes = Drill::FloeCenters(boss, *f, 0.3f);
        CHECK(floes.size() == 7);
        CHECK(Drill::OnAnyFloe(floes, 8.f, boss));
        CHECK(Drill::OnAnyFloe(floes, 8.f, { 18.f * std::cos(0.3f), 18.f * std::sin(0.3f), 0.f }));
        CHECK(!Drill::OnAnyFloe(floes, 8.f, { 12.f * std::cos(0.3f + 0.5236f), 12.f * std::sin(0.3f + 0.5236f), 0.f }));

        std::optional<Drill::CastStep> cast = Drill::ParseCast("cast 205010 2000 kick=1 twin=1 tag=40");
        CHECK(cast && cast->twin && cast->kick && cast->spell == 205010u);
        CHECK(!Drill::ParseCast("cast 205010 2000 kick=1 twin=2 tag=40"));
        cast = Drill::ParseCast("cast 205010 2000 kick=1 tag=40");
        CHECK(cast && !cast->twin);

        for (char const* step : { "afflict", "partner", "stalk", "floes" })
            CHECK(!Drill::DamageShape(step));
    }

    void TestPicks()
    {
        std::vector<Drill::PickCandidate> c(5);
        c[0].victim = true;
        c[0].healer = false;
        c[3].healer = true;
        c[2].marked = true;
        CHECK((Drill::PickPool(c, Drill::PickRule::Marked, 0.f) == std::vector<std::size_t>{ 2 }));
        CHECK((Drill::PickPool(c, Drill::PickRule::Unmarked, 0.f) == std::vector<std::size_t>{ 0, 1, 3, 4 }));
        CHECK((Drill::PickPool(c, Drill::PickRule::Healer, 0.f) == std::vector<std::size_t>{ 3 }));
        // Nobody fits: anyone (the walker gone, no healer bot).
        std::vector<Drill::PickCandidate> none(3);
        CHECK(Drill::PickPool(none, Drill::PickRule::Marked, 0.f).size() == 3);
        CHECK(Drill::PickPool(none, Drill::PickRule::Healer, 0.f).size() == 3);
        for (Drill::PickCandidate& m : none)
            m.marked = true;
        CHECK(Drill::PickPool(none, Drill::PickRule::Unmarked, 0.f).size() == 3);
    }

    // ---- row 1, Theka ----------------------------------------------------------------------------------------------
    // The pair rises 30 yd out, 60 degrees either side, at 5 yd/s; he walks 6 yd while it travels; the turn after him
    // leaves at its end; TK2 is drawn 2.5 s before the body reaches him. Normal: one line at 4 yd/s.
    void TestTheka()
    {
        std::vector<At> const pair = WithTag(0, Drill::kZfTagSwarm);
        CHECK(pair.size() == 2);
        for (At const& a : pair)
        {
            CHECK(ShapeOf(a.line) == "projectile" && Size(a.line) == Drill::kZfSwarmYd);
            CHECK(Key(a.line, "width", 0.f) == Drill::kZfSwarmWidthYd);
            CHECK(Key(a.line, "speed", 0.f) == Drill::kZfSwarmSpeedYdS);
            CHECK(Is(a.line, "to", "target") && Is(a.line, "fix", "1") && Is(a.line, "weak", "1"));
            CHECK(Drill::WarnMs(a.line) == Drill::kZfRiseMs && a.startMs == pair[0].startMs);
            std::string_view const shift = *Drill::Value(a.line, "shift");
            float const ahead = Drill::Number(shift.substr(0, shift.find(':'))).value_or(0.f);
            float const left = Drill::SignedNumber(shift.substr(shift.find(':') + 1)).value_or(0.f);
            CHECK_NEAR(std::hypot(ahead, left), Drill::kZfSwarmYd, 0.05f);
            CHECK_NEAR(std::fabs(std::atan2(left, ahead)), Geo::kPi / 3.f, 0.01f);
            CHECK(Key(a.line, "pct", 0.f) == 45.f);
        }
        CHECK(Drill::Value(pair[0].line, "shift") != Drill::Value(pair[1].line, "shift"));
        float const travel = Drill::kZfSwarmYd / Drill::kZfSwarmSpeedYdS * 1000.f;
        std::uint32_t const atEnd = pair[0].resolveMs + static_cast<std::uint32_t>(travel);
        // He walks 6 yd while the pair travels, after the rise and before the bodies reach his old spot.
        std::vector<At> const walks = WithShape(0, "walk");
        CHECK(walks.size() == 2);
        CHECK(walks[0].startMs >= pair[0].resolveMs && walks[0].resolveMs <= atEnd);
        CHECK(Size(walks[0].line) == Drill::kZfTheka2MovedYd && Size(walks[1].line) == Drill::kZfTheka2MovedYd);
        CHECK(Key(walks[0].line, "offset", 0.f) + 180.f == Key(walks[1].line, "offset", 0.f));
        // The turn after him: from the recorded end (his old spot), reaching him.
        std::vector<At> const reaim = WithTag(0, Drill::kZfTagReaim);
        CHECK(reaim.size() == 1);
        CHECK(reaim[0].startMs == atEnd && Is(reaim[0].line, "at", "last") && Is(reaim[0].line, "reach", "1"));
        CHECK(Drill::WarnMs(reaim[0].line) == Drill::kZfReaimMs);
        CHECK(Key(reaim[0].line, "speed", 0.f) == Drill::kZfSwarmSpeedYdS);
        std::uint32_t const arrival = reaim[0].resolveMs
            + static_cast<std::uint32_t>(Drill::kZfTheka2MovedYd / Drill::kZfSwarmSpeedYdS * 1000.f);
        std::vector<At> const devour = WithTag(0, Drill::kZfTagDevour);
        CHECK(devour.size() == 1 && Size(devour[0].line) == Drill::kZfDevourYd);
        CHECK(Drill::WarnMs(devour[0].line) == Drill::kZfDevourMs && devour[0].resolveMs == arrival);
        CHECK(Is(devour[0].line, "anchor", "target") && Key(devour[0].line, "pct", 0.f) == 60.f);
        // He is home before the normal line rises; its TK2 lands with it.
        std::vector<At> const single = WithTag(0, Drill::kZfTagSwarmNormal);
        CHECK(single.size() == 1 && single[0].startMs >= walks[1].resolveMs);
        CHECK(Key(single[0].line, "speed", 0.f) == Drill::kZfSwarmSpeedNormalYdS);
        std::vector<At> const devourN = WithTag(0, Drill::kZfTagDevourNormal);
        CHECK(devourN.size() == 1);
        CHECK(devourN[0].resolveMs == single[0].resolveMs
            + static_cast<std::uint32_t>(Drill::kZfSwarmYd / Drill::kZfSwarmSpeedNormalYdS * 1000.f));
        // TK2's warning lets a melee walk out: 6 yd plus a step at 7 yd/s in 2.5 s.
        CHECK((Drill::kZfDevourYd + 1.f) / kRunYdS * 1000.f < static_cast<float>(Drill::kZfDevourMs) - 1000.f);
    }

    // ---- row 2, Antu'sul -------------------------------------------------------------------------------------------
    void TestAntusul()
    {
        std::vector<At> const bolts = WithTag(0, Drill::kZfTagBolt);
        CHECK(bolts.size() == 4);
        for (std::size_t i = 0; i < bolts.size(); ++i)
        {
            CHECK(Is(bolts[i].line, "anchor", "member") && Size(bolts[i].line) == Drill::kZfBoltYd);
            CHECK(Drill::WarnMs(bolts[i].line) == Drill::kZfBoltMs);
            CHECK(Is(bolts[i].line, "inbox", "10x42:-19"));
            if (i)
            {
                std::uint32_t const gap = bolts[i].startMs - bolts[i - 1].startMs;
                CHECK(gap >= 8000 && gap <= 10000);     // heroic: 8 to 10 s
            }
        }
        std::optional<Drill::BoxSpec> const box = Drill::ParseInBox("10x42:-19");
        CHECK(box->width == Drill::kZfTrenchWidthYd && box->length == Drill::kZfTrenchLengthYd
            && box->ahead == Drill::kZfTrenchAheadYd);
        // Anyone in the lane leaves a 6 yd circle on another player along it within the warning.
        CHECK((Drill::kZfBoltYd + 0.75f) / kRunYdS * 1000.f < static_cast<float>(Drill::kZfBoltMs) - 1000.f);
        // Two carriers on the dispel list, each marked first (pick=same), the second at 5 stacks with the Stack on
        // him for its whole affliction.
        std::vector<At> const aff = WithShape(0, "afflict");
        CHECK(aff.size() == 2);
        for (At const& a : aff)
        {
            std::optional<Drill::AfflictStep> const s = Drill::ParseAfflict(Drill::StripPlanKeys(a.line));
            CHECK(s && s->spell == Drill::kZfDispelSpell && s->same && s->lone == Drill::kZfLoneYd);
            CHECK(s->burst == Drill::kZfLoneYd && s->ledgerTag == Drill::kZfTagShatter);
            bool marked = false;
            for (At const& m : WithShape(0, "marker"))
                if (m.startMs == a.startMs && Size(m.line) > 0.f)
                    marked = true;
            CHECK(marked);
        }
        std::vector<At> const stack = WithTag(0, Drill::kZfTagStoneflesh);
        CHECK(stack.size() == 1 && ShapeOf(stack[0].line) == "stack" && Is(stack[0].line, "anchor", "target"));
        CHECK(Is(stack[0].line, "aura", "205009:5") && stack[0].startMs == aff[1].startMs);
        CHECK(stack[0].endMs <= aff[1].startMs + Drill::ParseAfflict(Drill::StripPlanKeys(aff[1].line))->ms);
    }

    // ---- rows 3 and 4, Zum'rah -------------------------------------------------------------------------------------
    struct Soul
    {
        std::uint32_t start = 0;
        float yd = 0.f;
        std::uint32_t travelMs = 0;     // to the cauldron, untaken
    };

    std::vector<Soul> Souls(std::uint32_t tag)
    {
        std::vector<Soul> out;
        for (At const& a : WithTag(1, tag))
        {
            std::optional<Drill::PolarSpec> const from = Drill::ParseFrom(*Drill::Value(a.line, "from"));
            // From the grave to the goal 3 yd off him toward the group (bearing 180), the touch radius short.
            Vec3 const grave = Drill::FromPoint({ 0.f, 0.f, 0.f }, 0.f, from->yd, from->second);
            Vec3 const goal = Drill::FromPoint({ 0.f, 0.f, 0.f }, 0.f, Drill::kZfCauldronYd, 180.f);
            float const d = std::hypot(grave.x - goal.x, grave.y - goal.y) - Drill::kWispReachYd;
            out.push_back({ a.startMs, from->yd, static_cast<std::uint32_t>(d / Drill::kZfSoulSpeedYdS * 1000.f) });
        }
        return out;
    }

    void TestZumrah()
    {
        for (std::uint32_t tag : { Drill::kZfTagSoul, Drill::kZfTagHealerSoul })
        {
            std::vector<At> const crawls = WithTag(1, tag);
            CHECK(crawls.size() == Drill::kZfWalkMs / Drill::kZfSoulEveryMs + 1);
            for (std::size_t i = 0; i < crawls.size(); ++i)
            {
                std::string_view const l = crawls[i].line;
                CHECK(ShapeOf(l) == "crawl" && Size(l) == Drill::kZfSoulYd && Is(l, "aura", "205009:1"));
                CHECK(Key(l, "speed", 0.f) == Drill::kZfSoulSpeedYdS && Key(l, "mark", 0.f) == 1.f);
                CHECK(Key(l, "yd", 0.f) == Drill::kZfCauldronYd && Key(l, "bearing", 0.f) == 180.f);
                CHECK(Drill::WarnMs(l) == Drill::kZfSoulLifeMs);
                CHECK(Is(l, "healreach", "1") == (tag == Drill::kZfTagHealerSoul));
                if (i)
                    CHECK(crawls[i].startMs - crawls[i - 1].startMs == Drill::kZfSoulEveryMs);
            }
            // §7 #29 A: graves within 60 yd; every soul reaches the cauldron inside its life.
            for (Soul const& s : Souls(tag))
            {
                CHECK(s.yd >= 30.f && s.yd <= 60.f);
                CHECK(s.travelMs < Drill::kZfSoulLifeMs);
            }
        }
        // At the walk's peak six souls crawl at once (§3.9 "8 to 10 crawling Soaks" a walk).
        std::vector<Soul> const souls = Souls(Drill::kZfTagSoul);
        std::uint32_t peak = 0;
        for (std::uint32_t t = 0; t <= Drill::kZfWalkMs; t += 250)
        {
            std::uint32_t live = 0;
            for (Soul const& s : souls)
                live += s.start <= t && t < s.start + s.travelMs ? 1 : 0;
            peak = std::max(peak, live);
        }
        CHECK(peak >= Drill::kZfMinLiveSouls);
        // The walker (Marked 1) for 15.5 s; walk 1's last soul is bound before the healer walk marks the healer.
        std::vector<At> const markers = WithShape(1, "marker");
        CHECK(markers.size() == 4);
        CHECK(Size(markers[0].line) == 1.f && markers[0].startMs == 0 && Size(markers[1].line) == 0.f);
        CHECK(markers[1].startMs >= Drill::kZfWalkMs);
        CHECK(Size(markers[2].line) == 1.f && Is(markers[2].line, "pick", "healer"));
        std::uint32_t walkOneEnd = 0;
        for (Soul const& s : souls)
            walkOneEnd = std::max(walkOneEnd, s.start + s.travelMs);
        CHECK(walkOneEnd < markers[2].startMs);
        CHECK(Size(markers[3].line) == 0.f && markers[3].startMs >= markers[2].startMs + Drill::kZfWalkMs);
        // The delivery: 3 stacks (the walker's own and two souls), at the cauldron, before the walk ends; by then the
        // walker could have touched at least two souls.
        std::vector<At> const delivery = WithTag(1, Drill::kZfTagDelivery);
        CHECK(delivery.size() == 1 && ShapeOf(delivery[0].line) == "soak" && Is(delivery[0].line, "need", "1"));
        CHECK(Is(delivery[0].line, "aura", "205009:3") && Drill::kZfDeliveryStacks == 3);
        CHECK(delivery[0].resolveMs <= Drill::kZfWalkMs);
        std::uint32_t risen = 0;
        for (Soul const& s : souls)
            risen += s.start < delivery[0].startMs ? 1 : 0;
        CHECK(risen >= 2);
        // The healer walk: leashed to the tank (36.5 yd) for the whole walk.
        std::vector<At> const leash = WithTag(1, Drill::kZfTagHealerLeash);
        CHECK(leash.size() == 1 && ShapeOf(leash[0].line) == "leash" && Size(leash[0].line) == Drill::kZfHealerReachYd);
        CHECK(Is(leash[0].line, "role", "healer") && Is(leash[0].line, "anchor", "tank"));
        CHECK(leash[0].startMs == markers[2].startMs && Drill::WarnMs(leash[0].line) == Drill::kZfWalkMs);

        // Row 4: walker-only circles (Marked >= 1, on the walker or near it, seen with the walker's detect aura only)
        // next to living-only ones (no Marked, on a living player, seen by all), inside walk 1.
        for (std::uint32_t tag : { Drill::kZfTagWail, Drill::kZfTagGrave })
            for (At const& a : WithTag(1, tag))
            {
                CHECK(Is(a.line, "pick", "marked") && Is(a.line, "aura", "205009:1") && Is(a.line, "seen", "11"));
                CHECK(Size(a.line) == 6.f && Drill::WarnMs(a.line) == 1500);
                CHECK(a.endMs <= markers[1].startMs);
            }
        std::vector<At> const graves = WithTag(1, Drill::kZfTagGrave);
        CHECK(graves.size() == 2 && graves[0].startMs == graves[1].startMs);
        for (At const& g : graves)
            CHECK(Key(g.line, "away", 0.f) > 0.f && Key(g.line, "away", 0.f) <= 20.f);
        for (At const& a : WithTag(1, Drill::kZfTagEcho))
        {
            CHECK(Is(a.line, "pick", "unmarked") && Is(a.line, "aura", "205009:0:0") && !Drill::Value(a.line, "seen"));
            CHECK(a.endMs <= markers[1].startMs);
        }
        // The walker can always step out of a wail (6 yd) in its 1.5 s.
        CHECK((6.f + 0.75f) / kRunYdS * 1000.f < 1500.f);
    }

    // ---- row 5, the Block; row 6, the stair -----------------------------------------------------------------------
    void TestPyramid()
    {
        std::vector<At> const blocks = WithTag(2, Drill::kZfTagBlock);
        CHECK(blocks.size() == 6);
        std::uint32_t lastGap = 0;
        for (std::size_t i = 0; i < blocks.size(); ++i)
        {
            std::string_view const l = blocks[i].line;
            CHECK(ShapeOf(l) == "soak" && Size(l) == Drill::kZfBlockYd && Is(l, "need", "1"));
            CHECK(Drill::WarnMs(l) == Drill::kZfBlockMs && Is(l, "aura", "205009:0:0") && Is(l, "mark", "1"));
            if (!i)
                continue;
            std::uint32_t const gap = blocks[i].startMs - blocks[i - 1].startMs;
            CHECK(gap >= Drill::kZfBlockFloorMs);
            CHECK(i == 1 || gap <= lastGap);
            lastGap = gap;
        }
        CHECK(lastGap == Drill::kZfBlockFloorMs);
        CHECK(blocks[1].startMs - blocks[0].startMs == 12000);
        // The wound clears (marker 0) at most after two Blocks: someone is always eligible in a group of five.
        std::vector<At> const clears = WithShape(2, "marker");
        for (At const& m : clears)
            CHECK(Size(m.line) == 0.f);
        for (std::size_t i = 0; i < blocks.size(); ++i)
        {
            std::uint32_t wounded = 0;
            for (std::size_t j = 0; j < i; ++j)
            {
                bool healed = false;
                for (At const& m : clears)
                    healed = healed || (m.startMs >= blocks[j].resolveMs && m.startMs <= blocks[i].startMs);
                wounded += healed ? 0 : 1;
            }
            CHECK(wounded <= 2);
        }
        // The stair: the barrel lane over its whole width below the landing (the landing's 10 yd its bounds), so a
        // player on the landing is never in it; a ranged player 25 yd down walks up within the 3 s.
        std::vector<At> const barrels = WithTag(2, Drill::kZfTagBarrel);
        CHECK(barrels.size() == 2);
        for (At const& b : barrels)
        {
            CHECK(ShapeOf(b.line) == "line" && Size(b.line) == Drill::kZfStairYd);
            CHECK(Key(b.line, "width", 0.f) == Drill::kZfStairWidthYd && Key(b.line, "offset", 0.f) == 180.f);
            CHECK(Is(b.line, "inside", "10:0") && Key(b.line, "pct", 0.f) == 50.f && Is(b.line, "weak", "1"));
            CHECK(Drill::Number(Drill::Value(b.line, "shift")->substr(0, 2)).value_or(0.f) > Drill::kZfLandingYd);
            CHECK(b.startMs >= blocks.back().resolveMs);
        }
        CHECK((25.f - Drill::kZfLandingYd + 0.75f) / kRunYdS * 1000.f < 3000.f - 500.f);
        std::vector<At> const tramples = WithTag(2, Drill::kZfTagTrample);
        CHECK(tramples.size() == 2);
        for (At const& t : tramples)
            CHECK(Is(t.line, "exempt", "target") && Is(t.line, "reach", "1") && Drill::WarnMs(t.line) == 1500
                && Key(t.line, "width", 0.f) == 4.f);
    }

    // ---- row 7, the crew -------------------------------------------------------------------------------------------
    void TestCrew()
    {
        std::vector<At> const seed = WithTag(1, Drill::kZfTagSeed);
        std::vector<At> const gang = WithTag(1, Drill::kZfTagGangUp);
        std::vector<At> const ambush = WithTag(1, Drill::kZfTagAmbush);
        CHECK(seed.size() == 1 && gang.size() == 1 && ambush.size() == 2);
        CHECK(ShapeOf(seed[0].line) == "runout" && Size(seed[0].line) == 10.f);
        CHECK(ShapeOf(gang[0].line) == "stack" && Size(gang[0].line) == 6.f && Is(gang[0].line, "anchor", "member"));
        CHECK(Drill::WarnMs(gang[0].line) == 4000 && Is(gang[0].line, "pick", "fresh"));
        // The Ambush lands under the running mark (pick=same right after the Stack's pick), all three inside 4 s.
        CHECK(Is(ambush[0].line, "pick", "same") && Size(ambush[0].line) == 3.f);
        std::uint32_t const lo = std::min({ seed[0].resolveMs, gang[0].resolveMs, ambush[0].resolveMs });
        std::uint32_t const hi = std::max({ seed[0].resolveMs, gang[0].resolveMs, ambush[0].resolveMs });
        CHECK(hi - lo <= 4000);
        for (At const& a : Cycle(1))
            if (a.startMs > gang[0].startMs && a.startMs < ambush[0].startMs)
                CHECK(!Drill::Value(a.line, "pick"));      // nothing picks between the mark and its Ambush
        std::vector<At> const rain = WithTag(1, Drill::kZfTagRain);
        CHECK(rain.size() == 1 && Key(rain[0].line, "persist", 0.f) == 9000.f);
        CHECK(Key(rain[0].line, "tick", 0.f) == 3000.f);
    }

    // ---- rows 8 and 12, Ukorz --------------------------------------------------------------------------------------
    void TestUkorz()
    {
        std::vector<At> const partner = WithShape(3, "partner");
        CHECK(partner.size() == 1);
        std::optional<Drill::PartnerStep> const p = Drill::ParsePartner(Drill::StripPlanKeys(partner[0].line));
        CHECK(p && p->entry == Drill::kZfPartnerEntry && p->everyMs == Drill::kZfFlipMs
            && p->graceMs == Drill::kZfFlipGraceMs && p->ledgerTag == Drill::kZfTagKillOrder);
        CHECK(p->ms / p->everyMs >= 1);     // at least one flip
        std::vector<At> const storms = WithTag(3, Drill::kZfTagBladestorm);
        std::vector<At> const stalks = WithShape(3, "stalk");
        CHECK(storms.size() == 2 && stalks.size() == 2);
        for (std::size_t i = 0; i < storms.size(); ++i)
        {
            CHECK(Size(storms[i].line) == Drill::kZfStormYd && Is(storms[i].line, "anchor", "target"));
            CHECK(Key(storms[i].line, "persist", 0.f) == 8000.f && Key(storms[i].line, "pct", 0.f) == 15.f);
            std::optional<Drill::StalkStep> const s = Drill::ParseStalk(Drill::StripPlanKeys(stalks[i].line));
            CHECK(s && s->speed == Drill::kZfStormSpeedYdS && s->everyMs == 2000u && s->trail == Drill::kZfTrailYd);
            CHECK(s->trailTag == Drill::kZfTagTrail && s->home);
            // The walk is the storm: from its resolve to its end.
            CHECK(stalks[i].startMs == storms[i].resolveMs && stalks[i].startMs + s->ms == storms[i].endMs);
            // A player outruns the storm (7 against 5 yd/s).
            CHECK(s->speed < kRunYdS);
            CHECK(storms[i].endMs <= partner[0].startMs + p->ms);
        }
        // The Creeping Sands: 30, 25, 20, 15 gated and 10 reported, each ring live as the last one ends.
        std::vector<At> donuts = WithShape(3, "donut");
        CHECK(donuts.size() == 5);
        float inner = Drill::kZfCreepStartYd;
        for (std::size_t i = 0; i < donuts.size(); ++i)
        {
            CHECK(Key(donuts[i].line, "inner", 0.f) == inner && Size(donuts[i].line) == Drill::kZfCreepOuterYd);
            CHECK(Tag(donuts[i].line) == (inner >= Drill::kZfCreepGatedYd ? Drill::kZfTagCreeping
                : Drill::kZfTagCreepingLast));
            if (i)
                CHECK(donuts[i].resolveMs == donuts[i - 1].endMs);
            inner -= Drill::kZfCreepStepYd;
        }
        // Each Lash's bounds are the room the ring leaves as it lands; five players 8 yd apart fit in it (a pentagon
        // of side 8 has a 6.8 yd circumradius).
        for (At const& lash : WithTag(3, Drill::kZfTagLash))
        {
            float live = Drill::kZfCreepOuterYd;
            for (At const& d : donuts)
                if (d.resolveMs <= lash.resolveMs && lash.resolveMs < d.endMs)
                    live = std::min(live, Key(d.line, "inner", 0.f));
            CHECK(Key(lash.line, "inside", 0.f) >= live && Size(lash.line) == Drill::kZfLashYd);
            CHECK(8.f / (2.f * std::sin(Geo::kPi / 5.f)) + 1.f < live);
        }
        // The pits come in pairs on two fresh members, each with its lasting slow on the same spot.
        std::vector<At> const pits = WithTag(3, Drill::kZfTagPit);
        std::vector<At> const slows = WithTag(3, Drill::kZfTagPitSlow);
        CHECK(pits.size() == 4 && slows.size() == 4);
        for (std::size_t i = 0; i < pits.size(); ++i)
        {
            CHECK(Is(pits[i].line, "pick", "fresh") && Size(pits[i].line) == Drill::kZfPitYd);
            CHECK(Is(slows[i].line, "at", "prev") && slows[i].startMs == pits[i].startMs);
        }
    }

    // ---- row 11, the pool ------------------------------------------------------------------------------------------
    void TestPool()
    {
        std::vector<At> const water = WithTag(4, Drill::kZfTagWater);
        std::vector<At> const hot = WithTag(4, Drill::kZfTagDischarge);
        CHECK(water.size() == 2 && hot.size() == 4);
        for (At const& w : water)
            CHECK(Key(w.line, "pct", -1.f) == 0.f && Key(w.line, "persist", 0.f) > 0.f);
        std::optional<Drill::PolarSpec> const ring = Drill::ParseAround(*Drill::Value(water[0].line, "around"));
        CHECK(ring && ring->yd == Drill::kZfPuddleRingYd && ring->second == static_cast<float>(Drill::kZfPuddles));
        CHECK(Size(water[0].line) == Drill::kZfPuddleYd);
        // Eleven 14 yd puddles on a 30 yd ring touch their neighbors (a band of water from 16 to 44 yd).
        CHECK(Drill::RingSpacing(Drill::kZfPuddleRingYd, Drill::kZfPuddles) < 2.f * Drill::kZfPuddleYd);
        for (At const& h : hot)
        {
            CHECK(Drill::WarnMs(h.line) == Drill::kZfDischargeMs && Key(h.line, "pct", 0.f) == 40.f);
            CHECK(h.resolveMs <= water[0].endMs);
        }
        std::vector<At> const icicles = WithTag(4, Drill::kZfTagIcicle);
        CHECK(icicles.size() == 2);
        for (At const& i : icicles)
        {
            CHECK(Drill::WarnMs(i.line) == Drill::kZfIcicleMs && Is(i.line, "anchor", "member"));
            CHECK(Is(i.line, "drop", "prefer") && Is(i.line, "judge", "prefer"));
            CHECK(i.resolveMs < water[0].endMs);
            for (At const& h : hot)
                CHECK(i.resolveMs <= h.startMs || i.startMs >= h.resolveMs);   // never inside a Discharge
        }
        // A carrier anywhere in the dry middle reaches a puddle's edge inside the cast (§10: 2.5 s).
        float const edge = Drill::kZfPuddleRingYd - Drill::kZfPuddleYd;
        CHECK((edge + 0.75f) / kRunYdS * 1000.f < static_cast<float>(Drill::kZfIcicleMs));
    }

    // ---- rows 9, 10, 13, the hard mode -----------------------------------------------------------------------------
    void TestHardMode()
    {
        std::vector<At> const gather = WithTag(4, Drill::kZfTagGather);
        std::vector<At> const cond = WithTag(4, Drill::kZfTagConductive);
        CHECK(gather.size() == 1 && cond.size() == 1);
        CHECK(Size(gather[0].line) == Drill::kZfGatherYd && Is(gather[0].line, "anchor", "target"));
        CHECK(Size(cond[0].line) == Drill::kZfConductiveYd && ShapeOf(cond[0].line) == "spread");
        CHECK(gather[0].startMs == cond[0].startMs && gather[0].endMs == cond[0].endMs);
        // The anchor walks while the gather lasts, and is back before the floes.
        std::vector<At> const walks = WithShape(4, "walk");
        CHECK(walks.size() == 2 && walks[0].startMs == gather[0].startMs && walks[0].resolveMs <= gather[0].endMs);
        // Five players 6 yd apart fit in the 15 yd gather (a pentagon of side 6: 5.1 yd).
        CHECK(6.f / (2.f * std::sin(Geo::kPi / 5.f)) < Drill::kZfGatherYd);
        std::vector<At> const floes = WithShape(4, "floes");
        CHECK(floes.size() == 2 && floes[0].startMs >= walks[1].resolveMs);
        for (std::size_t i = 0; i < floes.size(); ++i)
        {
            std::optional<Drill::FloesStep> const f = Drill::ParseFloes(Drill::StripPlanKeys(floes[i].line));
            CHECK(f && f->r == Drill::kZfFloeYd && f->center && f->ledgerTag == Drill::kZfTagFloe);
            // D7Hb s3: the bots' bounds are each floe inset 1 yd; 4.5 to 5.5 floes alive on average, so 5 a set.
            CHECK(f->inset == Drill::kZfFloeInsetYd);
            CHECK(f->n + 1 == Drill::kZfFloesAlive);
            // Separate floes (the ring leaves gaps), the center one under him: melee reach him from a floe.
            CHECK(Drill::RingSpacing(f->ring, f->n) > 2.f * f->r);
            CHECK(f->ring - f->r > f->r);
            if (i)
                CHECK(floes[i].startMs == floes[i - 1].startMs + Drill::ParseFloes(
                    Drill::StripPlanKeys(floes[i - 1].line))->ms);
        }
        std::vector<At> const strikes = WithTag(4, Drill::kZfTagStrike);
        CHECK(strikes.size() == 1);
        CHECK(Key(strikes[0].line, "every", 0.f) == static_cast<float>(Drill::kZfStrikeEveryMs));
        CHECK(Key(strikes[0].line, "scatter", 0.f) == Drill::kZfStrikeScatterYd);
        CHECK(Size(strikes[0].line) == Drill::kZfStrikeYd && Key(strikes[0].line, "repeat", 0.f) >= 36.f);
        std::vector<At> const casts = WithShape(4, "cast");
        CHECK(casts.size() == 3);
        for (At const& c : casts)
        {
            std::optional<Drill::CastStep> const s = Drill::ParseCast(Drill::StripPlanKeys(c.line));
            CHECK(s && s->twin && s->kick && s->spell == Drill::kZfTwinCallSpell && s->ms == Drill::kZfTwinCallMs);
            CHECK(s->ledgerTag == Drill::kZfTagTwinCall);
        }
        CHECK(WithTag(4, Drill::kZfTagChosen).size() == 2);
    }
}

int main()
{
    TestTable();
    TestParsers();
    TestPicks();
    TestTheka();
    TestAntusul();
    TestZumrah();
    TestPyramid();
    TestCrew();
    TestUkorz();
    TestPool();
    TestHardMode();
    return Report("zf_drill_test");
}
