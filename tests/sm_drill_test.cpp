// Standalone tests for the Scarlet Monastery shapes of src/MechanicDrill.h (Scarlet Monastery build §3.9, WP-G1): the
// `scarlet-monastery-shapes` drill (wave 1 rows torment, mark-trail, rot-grow, glare, wall-mark, banner-drag,
// spike-carry) against the build spec rows it reproduces (Vishas VI4, Loksey LK3, Ironspine IR1 + IR5 + IR6, Doan DN3 +
// DN3a, Galvar RH1, Thalnos TH3, heroic numbers at the live bosses' timings), the grammar it adds (pick=tank:
// PickRule::Tank; leash near=1: LeashFails; the drill-only `chase` step: ParseChase, ChaseRelocation), and the sweep's
// program. Build: tests/run.sh.

#include "check.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include "MechanicGeometry.h"
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
    // A creature's melee range and stop against a player (combat reaches 1.5 + 1.5 + 4/3; 1.5 + 1.5 + 0.5).
    constexpr float kMeleeRangeYd = 4.33f;
    constexpr float kStopYd = 3.5f;

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
        static Drill::Def const* d = Drill::Find("scarlet-monastery-shapes");
        return *d;
    }

    struct At
    {
        std::size_t index = 0;
        std::string_view line;
        std::uint32_t startMs = 0;
        std::uint32_t resolveMs = 0;
        std::uint32_t endMs = 0;        // resolve plus persist (a leash or chase: its length)
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
                std::uint32_t const end = resolve + static_cast<std::uint32_t>(Key(line, "persist", 0.f));
                out.push_back({ i, line, *offsets[i], resolve, end });
            }
        return out;
    }

    std::vector<At> WithTag(std::uint32_t cycle, std::uint32_t tag)
    {
        std::vector<At> out;
        for (At const& a : Cycle(cycle))
            if (Tag(a.line) == tag && ShapeOf(a.line) != "marker" && ShapeOf(a.line) != "chase")
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

    // A small deterministic generator (the trail's scatter in the models).
    struct Lcg
    {
        std::uint32_t s = 12345;
        float Next()
        {
            s = s * 1664525u + 1013904223u;
            return static_cast<float>(s >> 8) / static_cast<float>(1u << 24);
        }
    };

    // ---- the table ------------------------------------------------------------------------------------------------

    void TestTable()
    {
        Drill::Def const* d = Drill::Find("scarlet-monastery-shapes");
        CHECK(d != nullptr);
        if (!d)
            return;
        CHECK(d->cycleMs == Drill::kSmCycleMs && d->defaultCycles == Drill::kSmCycles);
        CHECK(d->cycleMs * d->defaultCycles == 180000u);
        // The Mark's Bait measures from the test owner: in game it stands at the creature (the sim's owner is it).
        CHECK(d->ownerAtCreature);

        std::set<std::uint32_t> tags;
        for (Drill::Step const& s : d->steps)
        {
            std::string_view const shape = ShapeOf(s.line);
            // every step runs in one of the three cycles
            CHECK(Is(s.line, "cycle", "0/3") || Is(s.line, "cycle", "1/3") || Is(s.line, "cycle", "2/3"));
            if (shape == "marker" || shape == "chase")
            {
                CHECK(!Drill::Value(s.line, "tag"));
                continue;
            }
            tags.insert(Tag(s.line));
            // heroic numbers in pct=, never a dmg= of its own
            if (Drill::DamageStep(s.line))
                CHECK(Drill::Value(s.line, "pct").has_value());
            CHECK(!Drill::Value(s.line, "dmg"));
        }
        CHECK(tags == (std::set<std::uint32_t>{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 }));

        // Gated per member: the back-off, both trail circles, the banner, the rot, the eye pools, the Glare, the burst,
        // the fire pool, the spike's pillar and pool. The Torment's MissedSoak is a group failure; the scent's flat
        // landing and the spike's carried marker (no damage) are reported.
        std::set<std::uint32_t> const bar(std::begin(Drill::kSmBarTags), std::end(Drill::kSmBarTags));
        CHECK(bar == (std::set<std::uint32_t>{ 2, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14 }));
        for (std::uint32_t t = 0; t <= 20; ++t)
            CHECK(Drill::SmBarTag(t) == (bar.count(t) == 1));
        std::set<std::uint32_t> const group(std::begin(Drill::kSmGroupTags), std::end(Drill::kSmGroupTags));
        CHECK(group == (std::set<std::uint32_t>{ Drill::kSmTagTorment }));
        std::set<std::uint32_t> const refs(std::begin(Drill::kSmReferenceTags), std::end(Drill::kSmReferenceTags));
        CHECK(refs == (std::set<std::uint32_t>{ Drill::kSmTagBackOff, Drill::kSmTagTrail, Drill::kSmTagTrailCarrier,
            Drill::kSmTagBanner, Drill::kSmTagSpikePillar }));
        for (std::uint32_t t : refs)
            CHECK(bar.count(t) == 1);   // the member under test is judged by the same tag

        // Cycle 0 holds Vishas and Loksey; cycle 1 Ironspine and Doan; cycle 2 Thalnos's crypt (Galvar's banner and
        // the Flame Spike). Over before the next cycle.
        for (std::uint32_t cycle = 0; cycle < 3; ++cycle)
        {
            std::uint32_t last = 0;
            for (At const& a : Cycle(cycle))
            {
                CHECK(a.startMs >= last);
                last = a.startMs;
                std::uint32_t const len = ShapeOf(a.line) == "chase" || ShapeOf(a.line) == "leash"
                    ? static_cast<std::uint32_t>(Drill::Tokens(a.line).size() >= 3
                        ? Drill::Number(Drill::Tokens(a.line)[2]).value_or(0.f) : 0.f) : 0u;
                CHECK(std::max(a.endMs, a.startMs + len) <= Drill::kSmCycleMs);
                std::uint32_t const tag = Tag(a.line);
                if (cycle == 0)
                    CHECK(tag <= 5);
                else if (cycle == 1)
                    CHECK(tag == 0 || (tag >= 7 && tag <= 11));
                else
                    CHECK(tag == 0 || tag == 6 || tag >= 12);
            }
        }
    }

    // ---- grammar ------------------------------------------------------------------------------------------------

    void TestPickTank()
    {
        std::vector<Drill::PickCandidate> c(5);
        c[2].victim = true;
        std::vector<std::size_t> const pool = Drill::PickPool(c, Drill::PickRule::Tank, 0.f);
        CHECK(pool.size() == 1 && pool[0] == 2);
        // no victim (a dummy nobody tanks): anyone
        std::vector<Drill::PickCandidate> none(4);
        CHECK(Drill::PickPool(none, Drill::PickRule::Tank, 0.f).size() == 4);
        // min= still narrows it (the victim is the only pick, kept even when near: the farthest of one)
        c[2].dist = 2.f;
        std::vector<std::size_t> const far = Drill::PickPool(c, Drill::PickRule::Tank, 8.f);
        CHECK(far.size() == 1 && far[0] == 2);
        // the other rules are unchanged
        CHECK(Drill::PickPool(c, Drill::PickRule::NonTank, 0.f).size() == 4);
        CHECK(Drill::PickPool(c, Drill::PickRule::Any, 0.f).size() == 5);
    }

    void TestLeashNear()
    {
        CHECK(Drill::LeashFails(false, true) && !Drill::LeashFails(false, false));
        CHECK(!Drill::LeashFails(true, true) && Drill::LeashFails(true, false));
        std::optional<Drill::LeashStep> const l = Drill::ParseLeash("leash 8.5 6000 role=tank near=1 tag=2");
        CHECK(l && l->near && l->role == Drill::LeashRole::Tank && l->ledgerTag == 2);
        CHECK_NEAR(l ? l->yd : 0.f, 8.5f, kEps);
        std::optional<Drill::LeashStep> const off = Drill::ParseLeash("leash 8 6000 near=0 tag=2");
        CHECK(off && !off->near);
        CHECK(!Drill::ParseLeash("leash 8 6000 near=2 tag=2"));
        CHECK(!Drill::ParseLeash("leash 8 6000 near=yes tag=2"));
        CHECK(!Drill::ParseLeash("leash 8 6000 near=1 near=1 tag=2"));
        // the old form still reads
        std::optional<Drill::LeashStep> const old = Drill::ParseLeash("leash 40 20000 role=healer anchor=tank tag=3");
        CHECK(old && !old->near && old->anchor == Drill::LeashAnchor::Tank);
    }

    void TestParseChase()
    {
        std::optional<Drill::ChaseSpec> const c = Drill::ParseChase("chase 20 15500 home=1");
        CHECK(c && c->home && c->ms == 15500);
        CHECK_NEAR(c ? c->yd : 0.f, 20.f, kEps);
        std::optional<Drill::ChaseSpec> const plain = Drill::ParseChase("chase 10 1000");
        CHECK(plain && !plain->home);
        CHECK(!Drill::ParseChase("chase 0 1000"));
        CHECK(!Drill::ParseChase("chase 41 1000"));                 // beyond kMaxChaseYd
        CHECK(!Drill::ParseChase("chase 10 100"));                  // shorter than one step
        CHECK(!Drill::ParseChase("chase 10 130000"));               // beyond kMaxWalkMs
        CHECK(!Drill::ParseChase("chase 10 1000 home=2"));
        CHECK(!Drill::ParseChase("chase 10 1000 tag=3"));
        CHECK(!Drill::ParseChase("chase 10"));
        CHECK(!Drill::ParseChase("walk 10 1000"));
        CHECK(!Drill::DamageShape("chase"));
        CHECK(!Drill::DamageStep("chase 20 15500 home=1"));
        CHECK(Drill::ChaseSteps(15500) == 62 && Drill::ChaseSteps(250) == 1 && Drill::ChaseSteps(100) == 1);
    }

    void TestChaseRelocation()
    {
        Vec3 const start{ 0.f, 0.f, 0.f };
        float const step = Drill::kChaseSpeedYdS * static_cast<float>(Drill::kChaseStepMs) / 1000.f;
        // within melee range: stays
        CHECK(!Drill::ChaseRelocation(start, start, { 4.f, 0.f, 0.f }, kMeleeRangeYd, kStopYd, step, 20.f));
        // beyond it: one step toward the victim, at most `step`
        std::optional<Vec3> const p = Drill::ChaseRelocation(start, start, { 10.f, 0.f, 0.f }, kMeleeRangeYd, kStopYd,
            step, 20.f);
        CHECK(p.has_value());
        CHECK_NEAR(p ? p->x : 0.f, step, kEps);
        CHECK_NEAR(p ? p->y : 1.f, 0.f, kEps);
        // never closer than the stop
        std::optional<Vec3> const near = Drill::ChaseRelocation(start, { 5.f, 0.f, 0.f }, { 9.5f, 0.f, 0.f },
            kMeleeRangeYd, kStopYd, step, 20.f);
        CHECK(near.has_value());
        CHECK_NEAR(near ? near->x : 0.f, 6.f, kEps);
        // held on the circle round the start
        std::optional<Vec3> const capped = Drill::ChaseRelocation(start, { 19.5f, 0.f, 0.f }, { 40.f, 0.f, 0.f },
            kMeleeRangeYd, kStopYd, step, 20.f);
        CHECK(capped.has_value());
        CHECK_NEAR(capped ? capped->x : 0.f, 20.f, kEps);
        CHECK(!Drill::ChaseRelocation(start, { 20.f, 0.f, 0.f }, { 40.f, 0.f, 0.f }, kMeleeRangeYd, kStopYd, step,
            20.f));
        // the height stays the unit's (the runtime snaps it to the floor)
        std::optional<Vec3> const z = Drill::ChaseRelocation(start, { 0.f, 0.f, 3.f }, { 0.f, 10.f, 9.f },
            kMeleeRangeYd, kStopYd, step, 20.f);
        CHECK_NEAR(z ? z->z : 0.f, 3.f, kEps);
    }

    // A tank that walks straight out from `fromYd` to `toYd` along +x at run speed after `reactS`, the unit chasing it
    // from the origin every kChaseStepMs: where both stand `atS` seconds in.
    std::pair<Vec3, Vec3> DragModel(float fromYd, float toYd, float reactS, float atS, float maxFromStartYd)
    {
        Vec3 const start{ 0.f, 0.f, 0.f };
        Vec3 unit = start;
        Vec3 tank{ fromYd, 0.f, 0.f };
        float const step = Drill::kChaseSpeedYdS * static_cast<float>(Drill::kChaseStepMs) / 1000.f;
        float const dt = static_cast<float>(Drill::kChaseStepMs) / 1000.f;
        for (float t = dt; t <= atS + 1e-4f; t += dt)
        {
            float const walked = std::max(0.f, t - reactS) * kRunYdS;
            tank.x = std::min(toYd, fromYd + walked);
            if (std::optional<Vec3> const p = Drill::ChaseRelocation(start, unit, tank, kMeleeRangeYd, kStopYd, step,
                maxFromStartYd))
                unit = *p;
        }
        return { unit, tank };
    }

    // ---- the rows ---------------------------------------------------------------------------------------------------

    // Vishas VI4 heroic: a 9.5 s channel (1.5 s to step in, eight judged seconds: VishasLogic.h kTormentWarnMs +
    // kTormentMs), on the tank and then on a non-tank; the targeted tank backs off 10 yd, judged closer than 8.5 yd
    // from the first judged second to the channel's end.
    void TestTorment()
    {
        std::vector<At> const soaks = WithShape(0, "soak");
        CHECK(soaks.size() == 2);
        if (soaks.size() != 2)
            return;
        CHECK(Is(soaks[0].line, "pick", "tank") && Is(soaks[1].line, "pick", "nontank"));
        for (At const& s : soaks)
        {
            CHECK(Tag(s.line) == Drill::kSmTagTorment);
            CHECK(Is(s.line, "shape", "line") && Is(s.line, "anchor", "target") && Is(s.line, "to", "member"));
            CHECK(Is(s.line, "exempt", "target"));
            CHECK(Key(s.line, "need", 0.f) == static_cast<float>(Drill::kTormentSoakers));
            CHECK_NEAR(Size(s.line), Drill::kTormentWidth, kEps);
            CHECK(Drill::WarnMs(s.line) == Drill::kTormentWarnMs);
            CHECK(Key(s.line, "persist", 0.f) == static_cast<float>(Drill::kTormentJudgedMs));
            CHECK(Drill::kTormentWarnMs == 1500u && Drill::kTormentJudgedMs == 8000u);     // D1 live
            CHECK(s.endMs - s.startMs == 9500u);                    // the 9.5 s channel (TormentChannelMs)
            CHECK_NEAR(Key(s.line, "backoff", 0.f), Drill::kTormentBackOffYd, kEps);
            CHECK_NEAR(Key(s.line, "tanknear", 0.f), Drill::kTormentTankNearYd, kEps);
        }
        // the two channels never overlap ("his other tools wait")
        CHECK(soaks[1].startMs >= soaks[0].endMs);

        std::vector<At> const leashes = WithShape(0, "leash");
        CHECK(leashes.size() == 1);
        if (leashes.empty())
            return;
        std::optional<Drill::LeashStep> const l = Drill::ParseLeash(Drill::StripPlanKeys(leashes[0].line));
        CHECK(l && l->near && l->role == Drill::LeashRole::Tank && l->ledgerTag == Drill::kSmTagBackOff);
        if (!l)
            return;
        // judged from the first judged second of the tank's Torment to its end, never during the other one
        CHECK(leashes[0].startMs == soaks[0].resolveMs);
        CHECK(leashes[0].startMs + l->tickMs >= soaks[0].resolveMs + 1000);
        CHECK(leashes[0].startMs + l->ms == soaks[0].endMs);
        CHECK(leashes[0].startMs + l->ms <= soaks[1].startMs);
        // the judge sits inside the back-off spot with a margin, and beyond the melee spot
        CHECK(l->yd < Drill::kTormentBackOffYd - 1.f && l->yd > kSimTankYd + 2.f);
        // the tank walks from its melee spot to the back-off spot before the first judged tick
        float const walkS = (Drill::kTormentBackOffYd - kSimTankYd) / kRunYdS + kReactS;
        CHECK(walkS * 1000.f <= static_cast<float>(Drill::kTormentWarnMs + l->tickMs));
        // tank-near: a tank soaking at his end stays within melee (4 yd from the source)
        CHECK(Drill::kTormentTankNearYd <= kMeleeRangeYd);
        // a non-tank target at the sim's DPS spot: the beam (12 yd) is longer than the tank's region
        CHECK(kSimDpsYd > Drill::kTormentTankNearYd + 2.f);
    }

    // Loksey LK3 heroic: a Marked non-tank carrier (D5's kScentTankExempt), the only one eligible for a 5 s Bait that
    // sends it 22 yd off him (kScentFarYd); then nine markers
    // every 0.5 s, 5 yd, 0-2 yd from the carrier, each landing 1.5 s later; the others' circle and the carrier's share
    // their spot (at=prev right after the at=member one).
    void TestMarkTrail()
    {
        std::vector<At> const all = Cycle(0);
        std::vector<At> const baits = WithShape(0, "bait");
        CHECK(baits.size() == 2);
        std::vector<At> const markers = WithShape(0, "marker");
        CHECK(markers.size() == 4);
        std::vector<At> const others = WithTag(0, Drill::kSmTagTrail);
        std::vector<At> const carrier = WithTag(0, Drill::kSmTagTrailCarrier);
        CHECK(others.size() == 2 * Drill::kTrailMarkers && carrier.size() == 2 * Drill::kTrailMarkers);
        if (baits.size() != 2 || markers.size() != 4 || others.size() != carrier.size())
            return;
        for (std::size_t m = 0; m < 2; ++m)
        {
            At const& bait = baits[m];
            // marked first (the same start, listed before: the schedule keeps the table's order), then cleared
            CHECK(Size(markers[2 * m].line) == 1.f && markers[2 * m].startMs == bait.startMs);
            CHECK(markers[2 * m].index < bait.index);
            CHECK(Size(markers[2 * m + 1].line) == 0.f);
            CHECK(Is(markers[2 * m].line, "pick", "nontank"));      // the tank never carries (R-1-LIB m4 ruling)
            CHECK(Is(bait.line, "aura", "205009:1") && Tag(bait.line) == Drill::kSmTagScent);
            CHECK(Drill::WarnMs(bait.line) == Drill::kScentMs);
            CHECK_NEAR(Size(bait.line), Drill::kScentOthersYd, kEps);
            CHECK_NEAR(Drill::kScentOthersYd, 22.f, kEps);          // LokseyLogic.h kScentFarYd
            CHECK(Key(bait.line, "pct", -1.f) == 0.f);
            for (std::uint32_t k = 0; k < Drill::kTrailMarkers; ++k)
            {
                At const& o = others[m * Drill::kTrailMarkers + k];
                At const& c = carrier[m * Drill::kTrailMarkers + k];
                CHECK(o.startMs == bait.resolveMs + k * Drill::kTrailEveryMs);
                CHECK(c.startMs == o.startMs && c.index == o.index + 1);
                CHECK(Is(o.line, "at", "member") && Is(o.line, "pick", "same") && Is(o.line, "aura", "205009:0:0"));
                CHECK_NEAR(Key(o.line, "scatter", 0.f), Drill::kTrailScatterYd, kEps);
                CHECK(Is(c.line, "at", "prev") && Is(c.line, "aura", "205009:1"));
                for (At const* a : { &o, &c })
                {
                    CHECK_NEAR(Size(a->line), Drill::kTrailMarkerYd, kEps);
                    CHECK(Drill::WarnMs(a->line) == Drill::kTrailWarnMs);
                    CHECK(Key(a->line, "pct", 0.f) == 45.f && Is(a->line, "weak", "1"));
                }
                // no other step registers between the pair (at=prev reads the last point circle)
                for (At const& x : all)
                    if (x.startMs == o.startMs)
                        CHECK(x.index == o.index || x.index == c.index);
            }
            // Marked comes off after the last landing, before the next Mark
            At const& lastMarker = others[m * Drill::kTrailMarkers + Drill::kTrailMarkers - 1];
            CHECK(markers[2 * m + 1].startMs >= lastMarker.resolveMs);
            CHECK(lastMarker.resolveMs - bait.resolveMs == 4000u + Drill::kTrailWarnMs);   // a 4.5 s trail + 1.5 s
        }
        CHECK(baits[1].startMs > markers[1].startMs);

        // Space math: a carrier that keeps moving at run speed (straight, or turning 90 degrees along a wall) is clear
        // of every marker when it lands; one that stops, or turns back over its own trail, is not.
        auto caught = [](int mode)
        {
            Lcg rng;
            Vec3 pos{ 0.f, 0.f, 0.f };
            std::vector<std::pair<float, Vec3>> marks;
            int hits = 0;
            float const dt = 0.05f;
            for (float t = 0.f; t <= 6.f + 1e-4f; t += dt)
            {
                float dir = 0.f;
                if (mode == 1 && t > 2.f)
                    dir = Geo::kPi / 2.f;               // along the wall
                if (mode == 3 && t > 2.f)
                    dir = Geo::kPi;                     // back over the trail
                bool const moving = mode != 2 && t > kReactS;
                if (moving)
                {
                    pos.x += std::cos(dir) * kRunYdS * dt;
                    pos.y += std::sin(dir) * kRunYdS * dt;
                }
                for (std::uint32_t k = 0; k < Drill::kTrailMarkers; ++k)
                {
                    float const drop = static_cast<float>(k * Drill::kTrailEveryMs) / 1000.f;
                    if (std::fabs(t - drop) < dt / 2.f)
                    {
                        float const a = rng.Next() * Geo::kTwoPi;
                        float const r = Drill::kTrailScatterYd * std::sqrt(rng.Next());
                        marks.push_back({ drop + static_cast<float>(Drill::kTrailWarnMs) / 1000.f,
                            { pos.x + r * std::cos(a), pos.y + r * std::sin(a), 0.f } });
                    }
                }
                for (auto const& [land, at] : marks)
                    if (std::fabs(t - land) < dt / 2.f && Geo::Dist2d(pos.x, pos.y, at.x, at.y)
                        <= Drill::kTrailMarkerYd)
                        ++hits;
            }
            return hits;
        };
        CHECK(caught(0) == 0);
        CHECK(caught(1) == 0);
        CHECK(caught(2) > 0);
        CHECK(caught(3) > 0);
        // The Bait sends the carrier othersRange (22 yd) off him: the trail's nearest marker edge then clears the tank
        // (3 yd from him) even when the carrier stands between him and the group.
        CHECK(Drill::kScentOthersYd - Drill::kTrailScatterYd - Drill::kTrailMarkerYd > kSimTankYd);
    }

    // Galvar RH1: a 10 yd zone at his feet only the tank sees for 30 s (3 s warned, 27 s judged: ThalnosHardModeLogic.h
    // kBannerMs), no damage; he chases the tank that drags him off and goes home when it is over.
    void TestBannerDrag()
    {
        std::vector<At> const banner = WithTag(2, Drill::kSmTagBanner);
        CHECK(banner.size() == 1);
        std::vector<At> const chases = WithShape(2, "chase");
        CHECK(chases.size() == 1);
        if (banner.size() != 1 || chases.size() != 1)
            return;
        At const& b = banner[0];
        CHECK(ShapeOf(b.line) == "circle" && Is(b.line, "role", "tank") && Is(b.line, "at", "target"));
        CHECK(Key(b.line, "pct", -1.f) == 0.f && !Drill::Value(b.line, "weak"));
        CHECK_NEAR(Size(b.line), Drill::kBannerYd, kEps);
        CHECK(Drill::WarnMs(b.line) == Drill::kBannerWarnMs);
        CHECK(b.endMs - b.startMs == Drill::kBannerMs);            // the live 30 s
        std::optional<Drill::ChaseSpec> const c = Drill::ParseChase(Drill::StripPlanKeys(chases[0].line));
        CHECK(c && c->home);
        if (!c)
            return;
        // the chase covers the banner and ends (home) after it expires, inside the cycle
        CHECK(chases[0].startMs <= b.startMs && chases[0].startMs + c->ms >= b.endMs);
        CHECK(chases[0].startMs + c->ms <= Drill::kSmCycleMs);
        // the drag: the tank walks out to the zone's edge + its stop + a yard; he follows, out by the first tick
        float const out = Drill::kBannerYd + kStopYd + 1.f;
        auto const [unit, tank] = DragModel(1.5f, out, kReactS, static_cast<float>(Drill::kBannerWarnMs) / 1000.f,
            c->yd);
        CHECK(Geo::Dist2d(0.f, 0.f, tank.x, tank.y) > Drill::kBannerYd);
        CHECK(Geo::Dist2d(0.f, 0.f, unit.x, unit.y) > Drill::kBannerYd - kMarginYd);
        CHECK(c->yd >= out);
        // a still tank never moves him
        auto const [still, stillTank] = DragModel(1.5f, 1.5f, 0.f, 10.f, c->yd);
        CHECK(Geo::Dist2d(0.f, 0.f, still.x, still.y) < kEps && stillTank.x == 1.5f);
    }

    // Ironspine IR1 heroic: the rot under him at 7 yd, then 8, 9, 10 every 10 s from its first tick, each rung warned
    // 1 s ahead and live as the last one ends (the KT-SM GrowingZone pattern); IR5 two eye pools (2 s marker) with each
    // rot; IR6 a Glare every 6 s; the chase covers the rot and goes home before Doan.
    void TestRotGrow()
    {
        std::vector<At> const rot = WithTag(1, Drill::kSmTagRot);
        CHECK(rot.size() == 4);
        if (rot.size() != 4)
            return;
        float const rungs[] = { 7.f, 8.f, 9.f, 10.f };
        for (std::size_t i = 0; i < rot.size(); ++i)
        {
            At const& r = rot[i];
            CHECK_NEAR(Size(r.line), rungs[i], kEps);
            // a fixed point: his spot at the drill's start (no at=, no anchor=, no shift=)
            CHECK(!Drill::Value(r.line, "at") && !Drill::Value(r.line, "anchor") && !Drill::Value(r.line, "shift"));
            CHECK(Key(r.line, "pct", 0.f) == 8.f && Is(r.line, "school", "nature") && !Drill::Value(r.line, "weak"));
            CHECK(Drill::WarnMs(r.line) == (i ? Drill::kRotStepWarnMs : Drill::kRotWarnMs));
            CHECK(Key(r.line, "tick", 0.f) == 1000.f);
            if (i)
            {
                // live exactly as the last rung ends: no gap, and the last rung's final pass comes before it
                CHECK(r.resolveMs == rot[i - 1].endMs);
                CHECK(r.resolveMs - rot[i - 1].resolveMs == Drill::kRotStepMs);
                CHECK(rot[i - 1].endMs - 1000u < r.resolveMs);
            }
        }
        // 7 -> 10 over 30 s from the first tick
        CHECK(rot[3].resolveMs - rot[0].resolveMs == 30000u);
        CHECK_NEAR(Size(rot[3].line) - Size(rot[0].line), Drill::kRotToYd - Drill::kRotFromYd, kEps);
        // a bot that stood just clear of a rung must step a yard out within the next rung's 1 s warning
        CHECK((1.f + kMarginYd) / kRunYdS + kReactS < static_cast<float>(Drill::kRotStepWarnMs) / 1000.f);

        std::vector<At> const eyes = WithTag(1, Drill::kSmTagEyePool);
        CHECK(eyes.size() == 4);
        if (eyes.size() == 4)
        {
            CHECK(eyes[0].startMs == rot[0].startMs && eyes[1].startMs == eyes[0].startMs);
            CHECK(eyes[2].startMs == 14000u && eyes[3].startMs == 14000u);      // the second rot (every 14 s)
            for (std::size_t i = 0; i < eyes.size(); ++i)
            {
                CHECK(Is(eyes[i].line, "at", "member"));
                CHECK(Is(eyes[i].line, "pick", i % 2 ? "other" : "any"));       // tank included, two different
                CHECK(Drill::WarnMs(eyes[i].line) == Drill::kEyePoolWarnMs);
                CHECK_NEAR(Size(eyes[i].line), Drill::kRotFromYd, kEps);
                // they lie to the rot's end (live 60 s), all gone together
                CHECK(eyes[i].endMs == Drill::kEyePoolsEndMs && eyes[i].endMs == rot[3].endMs);
            }
        }

        std::vector<At> const glares = WithShape(1, "gaze");
        CHECK(glares.size() == 5);
        for (std::size_t i = 0; i < glares.size(); ++i)
        {
            CHECK(Tag(glares[i].line) == Drill::kSmTagGlare);
            CHECK(Drill::WarnMs(glares[i].line) == Drill::kGlareMs);
            CHECK_NEAR(Size(glares[i].line), Drill::kGlareRangeYd, kEps);
            CHECK(!Drill::Value(glares[i].line, "arc") && !Drill::Value(glares[i].line, "lead"));   // SM-BM defaults
            if (i)
                CHECK(glares[i].startMs - glares[i - 1].startMs == Drill::kGlareEveryMs);
            // the Eye is loose while the rot grows
            CHECK(glares[i].resolveMs <= rot[3].endMs);
        }

        std::vector<At> const chases = WithShape(1, "chase");
        CHECK(chases.size() == 1);
        if (chases.empty())
            return;
        std::optional<Drill::ChaseSpec> const c = Drill::ParseChase(Drill::StripPlanKeys(chases[0].line));
        CHECK(c && c->home);
        if (!c)
            return;
        CHECK(chases[0].startMs == 0u);
        // home after the last rung and the last eye pool are gone, before Doan's marks
        std::uint32_t const home = chases[0].startMs + c->ms;
        CHECK(home >= rot[3].endMs);
        for (At const& e : eyes)
            CHECK(home >= e.endMs);
        for (At const& g : glares)
            CHECK(home >= g.resolveMs);
        std::vector<At> const marks = WithTag(1, Drill::kSmTagWallMark);
        CHECK(!marks.empty() && marks[0].startMs > home);
        // he can be dragged clear of the grown rot
        CHECK(c->yd >= Drill::kRotToYd + kStopYd + 1.f);
        auto const [unit, tank] = DragModel(1.5f, Drill::kRotFromYd + kStopYd + 1.f, kReactS,
            static_cast<float>(Drill::kRotWarnMs + 1000u) / 1000.f, c->yd);
        CHECK(Geo::Dist2d(0.f, 0.f, unit.x, unit.y) > Drill::kRotFromYd - kMarginYd);
        CHECK(Geo::Dist2d(0.f, 0.f, tank.x, tank.y) > Drill::kRotFromYd);
    }

    // Doan DN3 + DN3a heroic: three non-tanks (distinct) carry an 8 yd burst for 5 s to the edge of a 19 yd room, away
    // from the group; a 3 yd pool forms 1 s after the burst under the last carrier.
    void TestWallMark()
    {
        std::vector<At> const marks = WithTag(1, Drill::kSmTagWallMark);
        CHECK(marks.size() == Drill::kWallMarks);
        if (marks.size() != Drill::kWallMarks)
            return;
        CHECK(Is(marks[0].line, "pick", "nontank"));                // resets the chain: three different players
        for (std::size_t i = 0; i < marks.size(); ++i)
        {
            At const& m = marks[i];
            if (i)
                CHECK(Is(m.line, "pick", "other") && m.startMs == marks[0].startMs);
            CHECK(Is(m.line, "anchor", "member") && Is(m.line, "drop", "edge,away"));
            CHECK_NEAR(Key(m.line, "room", 0.f), Drill::kDoanRoomYd, kEps);
            CHECK_NEAR(Size(m.line), Drill::kWallMarkYd, kEps);
            CHECK(Drill::WarnMs(m.line) == Drill::kWallMarkMs);
            CHECK(Key(m.line, "pct", 0.f) == 45.f && Is(m.line, "weak", "1") && Is(m.line, "school", "fire"));
        }
        std::vector<At> const pools = WithTag(1, Drill::kSmTagFirePool);
        CHECK(pools.size() == 1);
        if (pools.empty())
            return;
        CHECK(pools[0].startMs == marks[0].resolveMs && pools[0].resolveMs == marks[0].resolveMs + 1000u);
        CHECK(Is(pools[0].line, "at", "member") && Is(pools[0].line, "pick", "same"));
        CHECK_NEAR(Size(pools[0].line), Drill::kFirePoolYd, kEps);
        CHECK(Key(pools[0].line, "pct", 0.f) == 10.f);
        // Space math: three carriers 1 yd inside the room's edge, 120 degrees apart, are more than two bursts apart,
        // and a burst there misses the tank and melee at his side.
        float const edge = Drill::kDoanRoomYd - 1.f;
        float const chord = 2.f * edge * std::sin(Geo::kPi / 3.f);
        CHECK(chord > 2.f * Drill::kWallMarkYd);
        CHECK(edge - Drill::kWallMarkYd > kSimTankYd + kMarginYd);
        // the sim's ranged and healer stand inside the room
        CHECK(kSimDpsYd < Drill::kDoanRoomYd && kSimHealerYd < Drill::kDoanRoomYd);
    }

    // Thalnos TH3 heroic (STAGE-R-1-GY MN-1): a random carrier (tank included) walks a 4 yd marker for the 2 s bar to
    // the edge of the crypt, away from the group and off the live pools; at the bar's end it lands where the carrier
    // stands; 1.5 s later the pillar (45%, Weakened, the carrier included); then a 4 yd pool, 8% a second for 30 s.
    // At 6 s and 21 s (kSpikeBeat), beside Galvar's banner as in a hard mode pull.
    void TestSpikeCarry()
    {
        std::vector<At> const carries = WithTag(2, Drill::kSmTagSpikeCarry);
        std::vector<At> const pillars = WithTag(2, Drill::kSmTagSpikePillar);
        std::vector<At> const pools = WithTag(2, Drill::kSmTagSpikePool);
        CHECK(carries.size() == 2 && pillars.size() == 2 && pools.size() == 2);
        if (carries.size() != 2 || pillars.size() != 2 || pools.size() != 2)
            return;
        for (std::size_t i = 0; i < 2; ++i)
        {
            At const& c = carries[i];
            At const& p = pillars[i];
            At const& o = pools[i];
            CHECK(c.startMs == Drill::kSpikeFirstMs + i * Drill::kSpikeEveryMs);
            // the carry: no damage, the tank included, the drop spot rule of SpikeDropRule
            CHECK(ShapeOf(c.line) == "circle" && Is(c.line, "anchor", "member") && Is(c.line, "pick", "any"));
            CHECK(Is(c.line, "drop", "edge,away,avoid"));
            CHECK_NEAR(Key(c.line, "room", 0.f), Drill::kCryptRoomYd, kEps);
            CHECK(Key(c.line, "pct", -1.f) == 0.f && !Drill::Value(c.line, "weak"));
            CHECK(Drill::WarnMs(c.line) == Drill::kSpikeCastMs);
            // the pillar where the carrier stands at the bar's end, 1.5 s later
            CHECK(p.startMs == c.resolveMs && p.index == c.index + 1);
            CHECK(Is(p.line, "at", "member") && Is(p.line, "pick", "same"));
            CHECK(Drill::WarnMs(p.line) == Drill::kSpikeMarkerMs);
            CHECK(Key(p.line, "pct", 0.f) == 45.f && Is(p.line, "weak", "1") && Is(p.line, "school", "fire"));
            CHECK(!Drill::Value(p.line, "persist"));
            // the pool on the same spot (at=prev, registered right after), judged from the tick after the pillar for
            // the rest of its 30 s
            CHECK(o.startMs == p.startMs && o.index == p.index + 1 && Is(o.line, "at", "prev"));
            CHECK(o.resolveMs == p.resolveMs + 1000u);
            CHECK(o.endMs == p.resolveMs + Drill::kSpikePoolMs);
            CHECK(Key(o.line, "pct", 0.f) == 8.f && !Drill::Value(o.line, "weak"));
            CHECK(Key(o.line, "tick", 0.f) == 1000.f);
            for (At const* a : { &c, &p, &o })
                CHECK_NEAR(Size(a->line), Drill::kSpikeYd, kEps);
        }
        // the second carry starts while the first pool burns: its drop rule must avoid it
        CHECK(carries[1].startMs < pools[0].endMs);
        // a carrier at the sim's DPS spot walks clear of the group within the bar: 2 s at run speed is 14 yd
        CHECK((static_cast<float>(Drill::kSpikeCastMs) / 1000.f - kReactS) * kRunYdS > 2.f * Drill::kSpikeYd);
        // the crypt room holds the sim's group (the healer stands 18 yd off: the circle is only the crypt's area)
        CHECK(kSimDpsYd < Drill::kCryptRoomYd);
    }

    void TestSweepProgram()
    {
        std::string err;
        for (char const* name : { "scarlet-monastery-shapes:3", "scarlet-monastery-shapes" })
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
            CHECK(e.cycles == 3 && e.lengthMs == 180000);
            std::vector<Drill::ScheduledStep> const s = Drill::StepSchedule(*e.drill, e.cycles);
            // every step once: cycle 0's rows, then cycle 1's, then cycle 2's
            CHECK(s.size() == e.drill->steps.size());
            for (std::size_t i = 1; i < s.size(); ++i)
                CHECK(s[i].delayMs >= s[i - 1].delayMs);
        }
    }
}

int main()
{
    TestTable();
    TestPickTank();
    TestLeashNear();
    TestParseChase();
    TestChaseRelocation();
    TestTorment();
    TestMarkTrail();
    TestBannerDrag();
    TestRotGrow();
    TestWallMark();
    TestSpikeCarry();
    TestSweepProgram();
    return Report("sm_drill_test");
}
