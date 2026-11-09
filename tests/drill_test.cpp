// Standalone tests for src/MechanicDrill.h (STAGE-M3-HANDOFF §3.11, WP-D): the `.mech` landing geometry (group
// centroid, shift, spot ring, centered lane) and a desk check of every `.mech drill` against the boss doc rows it
// reproduces (shapes, sizes, timings, the separations Alexei's pairs need). Build: tests/run.sh.

#include "BossMechanicsIds.h"
#include "check.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace BossMechanics;
using Geo::Vec3;

namespace
{
    constexpr float kEps = 1e-3f;
    // BossMechanics.Bots.SafetyMarginYd default (M2): the room a bot keeps inside or outside an edge.
    constexpr float kMarginYd = 0.5f;

    std::set<std::string_view> const kTestShapes = { "circle", "donut", "cone", "rearcone", "line", "projectile",
        "mine", "lanes", "spread", "runout", "stack", "soak", "frontal", "bait", "holdstill", "spot", "hunt" };

    struct Parsed
    {
        std::string_view shape;
        float size = 0.f;
        float ms = 0.f;
        std::string_view line;
    };

    Parsed Parse(std::string_view line)
    {
        std::vector<std::string_view> const t = Drill::Tokens(line);
        Parsed p;
        p.line = line;
        if (!t.empty())
            p.shape = t[0];
        if (t.size() >= 2)
            p.size = Drill::Number(t[1]).value_or(-1.f);
        if (t.size() >= 3)
            p.ms = Drill::Number(t[2]).value_or(-1.f);
        return p;
    }

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

    // When the step's mechanic is over, relative to the cycle start: resolve, plus a persistent zone's or a hold
    // window's length. `startMs`: when it starts (StepOffsets), default its atMs.
    float EndMs(Drill::Step const& step, std::optional<std::uint32_t> startMs = std::nullopt)
    {
        Parsed const p = Parse(step.line);
        float end = static_cast<float>(startMs.value_or(step.atMs)) + p.ms;
        if (p.shape == "holdstill")
            end += p.size * 1000.f;
        end += Key(step.line, "persist", 0.f);
        return end;
    }

    // Resolve passes of a persistent mechanic (StepPersistent: at resolveAt, then every tick while before `until`).
    int Passes(float persistMs, float tickMs)
    {
        int n = 0;
        for (float t = 0.f; n == 0 || t < persistMs; t += tickMs)
            ++n;
        return n;
    }

    std::vector<Drill::Step> StepsAt(Drill::Def const& d, std::uint32_t atMs)
    {
        std::vector<Drill::Step> out;
        for (Drill::Step const& s : d.steps)
            if (s.atMs == atMs)
                out.push_back(s);
        return out;
    }

    // ---- geometry --------------------------------------------------------------------------------------------------
    void TestCentroidAndShift()
    {
        Vec3 const c = Drill::Centroid({ { 0.f, 0.f, 10.f }, { 4.f, 0.f, 12.f }, { 2.f, 6.f, 14.f } });
        CHECK_NEAR(c.x, 2.f, kEps);
        CHECK_NEAR(c.y, 2.f, kEps);
        CHECK_NEAR(c.z, 12.f, kEps);
        Vec3 const none = Drill::Centroid({});
        CHECK_NEAR(none.x, 0.f, kEps);

        // Facing +y: ahead is +y, left is -x.
        Vec3 const s = Drill::Shifted({ 10.f, 20.f, 5.f }, Geo::kPi / 2.f, 6.f, 10.f);
        CHECK_NEAR(s.x, 0.f, kEps);
        CHECK_NEAR(s.y, 26.f, kEps);
        CHECK_NEAR(s.z, 5.f, kEps);
        // Facing +x: left is +y.
        Vec3 const t = Drill::Shifted({ 0.f, 0.f, 0.f }, 0.f, -3.f, 4.f);
        CHECK_NEAR(t.x, -3.f, kEps);
        CHECK_NEAR(t.y, 4.f, kEps);
    }

    void TestRingSpots()
    {
        Vec3 const center{ -1830.f, 5200.f, 91.5f };
        for (std::uint32_t n = 1; n <= 6; ++n)
        {
            for (float rot : { 0.f, 1.3f, 5.9f })
            {
                std::vector<Vec3> const spots = Drill::RingSpots(center, 12.f, n, rot);
                CHECK(spots.size() == n);
                float minGap = 1e9f;
                for (std::size_t i = 0; i < spots.size(); ++i)
                {
                    CHECK_NEAR(Geo::Dist2d(spots[i].x, spots[i].y, center.x, center.y), 12.f, kEps);
                    CHECK_NEAR(spots[i].z, center.z, kEps);
                    for (std::size_t j = i + 1; j < spots.size(); ++j)
                        minGap = std::min(minGap, Geo::Dist2d(spots[i].x, spots[i].y, spots[j].x, spots[j].y));
                }
                if (n >= 2)
                    CHECK_NEAR(minGap, Drill::RingSpacing(12.f, n), 1e-2);
                // The first spot sits at the rotation bearing.
                CHECK_NEAR(spots[0].x, center.x + 12.f * std::cos(rot), kEps);
                CHECK_NEAR(spots[0].y, center.y + 12.f * std::sin(rot), kEps);
            }
        }
        // AB7: desks at least 8 yd apart for a 5-man group plus the GM (n <= 6), on the default ring and the drill's.
        CHECK(Drill::RingSpacing(10.f, 6) >= 8.f);
        CHECK(Drill::RingSpacing(12.f, 6) >= 8.f);
        CHECK(Drill::RingSpacing(10.f, 1) == 0.f);
    }

    void TestLaneSource()
    {
        Vec3 const center{ 100.f, 50.f, 3.f };
        for (float facing : { 0.f, 0.7f, 3.1f, 4.4f })
        {
            Vec3 const src = Drill::LaneSource(center, facing, 40.f);
            // The far end mirrors the source through the center: the lane is centered on the anchor point.
            Vec3 const far{ src.x + 40.f * std::cos(facing), src.y + 40.f * std::sin(facing), src.z };
            CHECK_NEAR((src.x + far.x) / 2.f, center.x, kEps);
            CHECK_NEAR((src.y + far.y) / 2.f, center.y, kEps);
            Shape const lane = Shape::Line(40.f, 4.f);
            Geo::Pose const pose{ src.x, src.y, src.z, facing };
            CHECK(Geo::Contains(lane, pose, center));
            CHECK(!Geo::Contains(lane, pose, Drill::Shifted(center, facing, 0.f, 2.5f)));
            CHECK(!Geo::Contains(lane, pose, Drill::Shifted(center, facing, 20.5f, 0.f)));
        }
    }

    // ---- every drill -----------------------------------------------------------------------------------------------
    void TestEveryDrillIsWellFormed()
    {
        std::set<std::string_view> names;
        for (Drill::Def const& d : Drill::All())
        {
            CHECK(names.insert(d.name).second);
            CHECK(Drill::Find(d.name) == &d);
            CHECK(d.cycleMs > 0);
            CHECK(!d.steps.empty());
            // In start order in every cycle (the after= steps follow their previous step), all below the cycle.
            for (std::uint32_t cycle = 0; cycle < 2; ++cycle)
            {
                std::vector<std::optional<std::uint32_t>> const at = Drill::StepOffsets(d, cycle);
                std::uint32_t last = 0;
                for (std::size_t i = 0; i < d.steps.size(); ++i)
                {
                    if (!at[i])
                        continue;
                    CHECK(*at[i] >= last);
                    last = *at[i];
                    CHECK(*at[i] < d.cycleMs);
                    // Over before the next cycle starts, so cycles never stack.
                    CHECK(EndMs(d.steps[i], at[i]) <= static_cast<float>(d.cycleMs));
                }
            }
            for (Drill::Step const& step : d.steps)
            {
                CHECK(Drill::PlanKeysOk(step.line));
                Parsed const p = Parse(step.line);
                if (p.shape == "marker")
                {
                    CHECK(p.size >= 0.f && p.size <= static_cast<float>(Ids::MARKED_DRILL_MAX_STACKS));
                    continue;
                }
                // Drill setup never hands out Weakened (STAGE-M4-R-REPORT M-2): no step names it as a selector.
                CHECK(std::string_view(step.line).find("205000") == std::string_view::npos);
                CHECK(p.shape != "weaken");
                // The selectors read Marked (drill), never the fenrus rider's Baited: its stacks would leak into the
                // eligibility within one program slice.
                for (char const* key : { "aura", "self", "order" })
                    if (std::optional<std::string_view> v = Drill::Value(step.line, key))
                        if (p.shape != "bait")
                            CHECK(v->substr(0, 6) == "205009");
                CHECK(kTestShapes.count(p.shape) == 1);
                CHECK(p.size > 0.f);
                CHECK(p.ms >= 0.f);
                // Ledger tags name the mechanic.
                CHECK(Drill::Value(step.line, "tag").has_value());
                // dmg= comes from the drill; get-in kinds take none.
                CHECK(!Drill::Value(step.line, "dmg"));
                if (!Drill::DamageStep(step.line))
                    CHECK(!Drill::Value(step.line, "weak"));
            }
        }
        for (char const* name : { "smite", "cookie", "fenrus", "alexei", "soaks", "ras", "deadmines-followups",
            "sfk-hunt", "scholomance-shapes", "stratholme-followups" })
            CHECK(Drill::Find(name) != nullptr);
        CHECK(Drill::Find("nope") == nullptr);
        CHECK(Drill::DamageShape("frontal") && Drill::DamageShape("bait") && !Drill::DamageShape("spot"));
    }

    // Deadmines SM2 (Sweeping Slash: Frontal cone 90 deg, 8 yd, 2 s cast), SM3 (Backswing: rear cone, 1.5 s after
    // SM2), SM12 (Harpoon Volley: Spread on every non-tank, 6 yd, 2.5 s cast).
    void TestSmite()
    {
        Drill::Def const& d = *Drill::Find("smite");
        CHECK(d.steps.size() == 3);
        Parsed const front = Parse(d.steps[0].line);
        Parsed const rear = Parse(d.steps[1].line);
        Parsed const volley = Parse(d.steps[2].line);
        CHECK(front.shape == "frontal" && front.size == 8.f && front.ms == 2000.f);
        CHECK(Key(front.line, "angle", 0.f) == 90.f && !Is(front.line, "rear", "1"));
        CHECK(rear.shape == "frontal" && rear.size == 8.f && Key(rear.line, "angle", 0.f) == 90.f);
        CHECK(Is(rear.line, "rear", "1"));
        // The backswing resolves 1.5 s after the slash.
        CHECK_NEAR(d.steps[1].atMs + rear.ms - (d.steps[0].atMs + front.ms), 1500.f, kEps);
        CHECK(volley.shape == "spread" && volley.size == 6.f && volley.ms == 2500.f);
        CHECK(Is(volley.line, "role", "nontank") && !Drill::Value(volley.line, "count"));
        // The volley never overlaps the cones: each is judged on its own.
        CHECK(d.steps[2].atMs + volley.ms > EndMs(d.steps[1]) + 1000.f);
    }

    // Deadmines CK4 (Chowder: cone 60 deg / 8 yd on the eater, 3 breaths 2 s apart, first 2 s after the feed), CK6
    // (Gut Bomb: RunOut 8 yd, 5 s after the feed), CK5 (Fish Stew: 3 puddles of 4 yd under the eater, 2 s apart, first
    // 2 s after the feed; here a RunOut 4 at each drop and the puddle where the eater stands then).
    void TestCookie()
    {
        Drill::Def const& d = *Drill::Find("cookie");
        Parsed const chowder = Parse(d.steps[0].line);
        CHECK(chowder.shape == "cone" && chowder.size == 8.f && Key(chowder.line, "angle", 0.f) == 60.f);
        CHECK(chowder.ms == 2000.f);
        CHECK(Is(chowder.line, "anchor", "member") && Is(chowder.line, "follow", "1"));
        CHECK(Is(chowder.line, "pick", "nontank"));
        float const tick = Key(chowder.line, "tick", 0.f);
        CHECK(tick == 2000.f);
        CHECK(Passes(Key(chowder.line, "persist", 0.f), tick) == 3);
        // Look-away cones stay at 180 degrees or less (§3.12).
        CHECK(Key(chowder.line, "angle", 0.f) <= 180.f);

        int bombs = 0;
        std::vector<float> drops;
        std::vector<float> puddles;
        for (Drill::Step const& s : d.steps)
        {
            Parsed const p = Parse(s.line);
            if (p.shape == "runout" && p.size == 8.f)
            {
                ++bombs;
                CHECK(p.ms == 5000.f && Is(p.line, "at", "member"));
            }
            if (p.shape == "runout" && p.size == 4.f)
            {
                drops.push_back(s.atMs + p.ms);
                CHECK(p.ms == 2000.f);
            }
            if (p.shape == "circle")
            {
                puddles.push_back(static_cast<float>(s.atMs));
                CHECK(p.size == 4.f && Is(p.line, "pick", "same") && Is(p.line, "at", "member"));
                CHECK(Key(p.line, "persist", 0.f) > 0.f);
            }
        }
        CHECK(bombs == 1);
        CHECK(drops.size() == 3 && puddles.size() == 3);
        for (std::size_t i = 0; i < drops.size() && i < puddles.size(); ++i)
        {
            // Each puddle lands where the eater stands as its RunOut resolves; drops 2 s apart.
            CHECK_NEAR(puddles[i], drops[i], kEps);
            if (i)
                CHECK_NEAR(drops[i] - drops[i - 1], 2000.f, kEps);
        }
        // The stew's first runout picks the eater; the rest follow it.
        for (Drill::Step const& s : d.steps)
            if (Parse(s.line).shape == "runout" && Parse(s.line).size == 4.f && s.atMs != 15000)
                CHECK(Is(s.line, "pick", "same"));
    }

    // Shadowfang FN6 (Shadow Leap: Bait, fewest rotation-debuff stacks, circle 6 yd on the landing, 2 s emote);
    // othersRange 10. The rotation reads Baited 205008, which the rider lands on everyone hit (baiter included), so
    // the pick rotates; weak=1 only reaches the others caught in the circle (the baiter's hit is the job).
    // Never Weakened as the rule's aura: the baiter never gains it, so the pick would never move.
    void TestFenrus()
    {
        Drill::Def const& d = *Drill::Find("fenrus");
        CHECK(d.ownerAtCreature);
        CHECK(d.steps.size() == 1);
        Parsed const p = Parse(d.steps[0].line);
        CHECK(p.shape == "bait" && p.size == 10.f && p.ms == 2000.f);
        CHECK(Is(p.line, "rule", "fewest") && Is(p.line, "aura", "205008"));
        CHECK(Key(p.line, "r", 0.f) == 6.f && Is(p.line, "weak", "1"));
        CHECK(Is(p.line, "rider", "205008"));
        CHECK(Key(p.line, "aura", 0.f) == static_cast<float>(Ids::SPELL_BAITED_DRILL));
        CHECK(Key(p.line, "rider", 0.f) == static_cast<float>(Ids::SPELL_BAITED_DRILL));
        CHECK(Key(p.line, "aura", 0.f) != static_cast<float>(Ids::SPELL_WEAKENED));
        CHECK(Ids::InSpellBlock(Ids::SPELL_BAITED_DRILL));
        CHECK(Ids::InSpellBlock(205000) && Ids::InSpellBlock(205099));
        CHECK(!Ids::InSpellBlock(204999) && !Ids::InSpellBlock(205100) && !Ids::InSpellBlock(0));
        // STAGE-M4-R-REPORT nit 2: rider= refuses Weakened and the resolve spells; M5 WP-K: and every other spell but
        // the two harmless drill auras (the M5 stand-ins deal damage, stun or buff enemies).
        CHECK(Ids::RiderAllowed(Ids::SPELL_BAITED_DRILL) && Ids::RiderAllowed(Ids::SPELL_MARKED_DRILL));
        for (std::uint32_t id = 204990; id <= 205110; ++id)
            CHECK(Ids::RiderAllowed(id) == (id == 205008 || id == 205009));
        for (std::uint32_t id = Ids::SPELL_M5_FIRST; id <= Ids::SPELL_M5_LAST; ++id)
            CHECK(Ids::InSpellBlock(id) && !Ids::RiderAllowed(id));
        CHECK(Ids::SPELL_M5_FIRST == 205010 && Ids::SPELL_M5_LAST == 205023);
        CHECK(Ids::NPC_DESCRIPTOR_DUMMY == 300403 && Ids::NPC_DRILL_PRISON == 300407);
        CHECK(Ids::IsTrainingDummy(300402) && Ids::IsTrainingDummy(300403) && !Ids::IsTrainingDummy(300404));
        CHECK(Ids::InCreatureBlock(300407) && !Ids::InCreatureBlock(300420) && !Ids::InCreatureBlock(300399));
        CHECK(Ids::SPELL_MARKED_DRILL != Ids::SPELL_BAITED_DRILL && Ids::InSpellBlock(Ids::SPELL_MARKED_DRILL));
    }

    // Scholomance AB2-AB8: each rule alone, then each heroic pair; never Assembly with Keep your distance or desks;
    // No running only with Out of bounds.
    void TestAlexei()
    {
        Drill::Def const& d = *Drill::Find("alexei");
        auto rule = [](std::string_view line) -> std::string
        {
            Parsed const p = Parse(line);
            if (p.shape == "holdstill")
                return "still";
            if (p.shape == "stack" && Drill::Value(line, "cluster"))
                return "assembly";
            if (p.shape == "stack" && Is(line, "shape", "line"))
                return "lineup";
            if (p.shape == "spread")
                return "distance";
            if (p.shape == "circle" && Drill::Value(line, "persist"))
                return "bounds";
            if (p.shape == "spot")
                return "desks";
            return "?";
        };
        std::set<std::string> singles;
        std::set<std::pair<std::string, std::string>> pairs;
        CHECK(d.cycleMs == 13 * Drill::kAlexeiSlotMs);
        for (std::uint32_t slot = 0; slot < 13; ++slot)
        {
            std::uint32_t const at = slot * Drill::kAlexeiSlotMs;
            std::vector<Drill::Step> const steps = StepsAt(d, at);
            CHECK(steps.size() == 1 || steps.size() == 2);
            // AB1: 3 s warning (normal, singles), 4 s (heroic pairs), then a 6 s window; at-close rules resolve at the
            // close; everything is over before the next slot.
            float const warn = steps.size() == 1 ? 3000.f : 4000.f;
            for (Drill::Step const& s : steps)
            {
                Parsed const p = Parse(s.line);
                std::string const r = rule(s.line);
                CHECK(r != "?");
                if (r == "still")
                {
                    CHECK(p.ms == warn && p.size == 6.f && Key(s.line, "tol", 0.f) == 1.f);      // AB2: 1 yd
                }
                else if (r == "bounds")
                {
                    CHECK(p.ms == warn && Key(s.line, "persist", 0.f) == 6000.f && Is(s.line, "at", "group"));
                }
                else
                {
                    CHECK(p.ms == warn + 6000.f);
                }
                CHECK(EndMs(s) <= static_cast<float>(at + Drill::kAlexeiSlotMs));
            }
            if (steps.size() == 1)
                CHECK(singles.insert(rule(steps[0].line)).second);
            else
            {
                std::string a = rule(steps[0].line);
                std::string b = rule(steps[1].line);
                if (b < a)
                    std::swap(a, b);
                CHECK(pairs.insert({ a, b }).second);
            }
        }
        CHECK(singles == (std::set<std::string>{ "still", "assembly", "distance", "bounds", "lineup", "desks" }));
        std::set<std::pair<std::string, std::string>> const allowed = { { "bounds", "still" },
            { "assembly", "bounds" }, { "bounds", "distance" }, { "bounds", "lineup" }, { "bounds", "desks" },
            { "assembly", "lineup" }, { "distance", "lineup" } };
        CHECK(pairs == allowed);
        CHECK(!pairs.count({ "assembly", "distance" }) && !pairs.count({ "assembly", "desks" }));

        // Sizes: Assembly 6 yd with two others (AB3), Keep your distance 8 (AB4), lane 4 yd (AB6), desks 3 yd (AB7).
        float boundsR = 0.f;
        float laneW = 0.f;
        float laneLen = 0.f;
        float deskR = 0.f;
        float deskRing = 0.f;
        float spread = 0.f;
        float laneShift = 0.f;
        for (Drill::Step const& s : d.steps)
        {
            Parsed const p = Parse(s.line);
            std::string const r = rule(s.line);
            if (r == "assembly")
                CHECK(p.size == 6.f && Key(s.line, "cluster", 0.f) == 2.f);
            if (r == "distance")
                spread = p.size;
            if (r == "bounds")
                boundsR = p.size;
            if (r == "lineup")
            {
                laneW = Key(s.line, "width", p.size);
                laneLen = Key(s.line, "length", 30.f);
                if (Drill::Value(s.line, "shift"))
                {
                    std::string_view const v = *Drill::Value(s.line, "shift");
                    laneShift = Drill::Number(v.substr(v.find(':') + 1)).value_or(0.f);
                    // Shifted only when paired with Out of bounds.
                    CHECK(StepsAt(d, s.atMs).size() == 2);
                }
            }
            if (r == "desks")
            {
                deskR = p.size;
                deskRing = Key(s.line, "ring", 10.f);
            }
        }
        CHECK(spread == 8.f && laneW == 4.f && deskR == 3.f);
        // §3.12: a get-in region never overlaps a damage Zone hot at the same time. Desks and the shifted lane stay a
        // margin clear of the Out of bounds circle (all centered on the group).
        CHECK(deskRing - deskR >= boundsR + 2.f * kMarginYd);
        CHECK(laneShift - laneW / 2.f >= boundsR + 2.f * kMarginYd);
        // Line up + Keep your distance: 5 bots in a row need 4 gaps of the spread plus margins.
        CHECK(laneLen - 2.f * kMarginYd >= 4.f * (spread + 2.f * kMarginYd));
        // Desks: one per member of a GM + 5 group, at least 8 yd apart (AB7).
        CHECK(Drill::RingSpacing(deskRing, 6) >= 8.f);
    }

    // Payday (Deadmines MJ1: 1 body, non-tank, circle 2 yd, 2.5 s flight); an ordered soak with a cap; the Barov
    // fortune (AB10: nobody takes it, 8-12 yd from the group); a lasting soak on the GM (Rattlegore RG2: 3 yd, 5 s).
    void TestSoaks()
    {
        Drill::Def const& d = *Drill::Find("soaks");
        std::vector<Drill::Step> soaks;
        std::vector<Drill::Step> markers;
        for (Drill::Step const& s : d.steps)
            (Parse(s.line).shape == "marker" ? markers : soaks).push_back(s);
        CHECK(soaks.size() == 4);
        Parsed const payday = Parse(soaks[0].line);
        CHECK(payday.shape == "soak" && payday.size == 2.f && payday.ms == 2500.f);
        CHECK(Key(payday.line, "need", 0.f) == 1.f && Is(payday.line, "role", "nontank"));
        // STAGE-M3-R-REPORT m-13: it lands near a non-tank (the eligible ones), never next to the tank alone.
        CHECK(Is(payday.line, "at", "member") && Is(payday.line, "pick", "nontank"));
        // §3.12: a 1-body soak lands within (warning - 0.5 s) x 7 yd of an eligible player.
        CHECK(Key(payday.line, "scatter", 0.f) <= (payday.ms / 1000.f - 0.5f) * 7.f);
        Parsed const ordered = Parse(soaks[1].line);
        CHECK(ordered.shape == "soak" && Key(ordered.line, "need", 0.f) == 1.f);
        CHECK(Is(ordered.line, "order", "205009:asc:cap=2"));
        // STAGE-M3-R-REPORT n-7: before it, two non-tanks get 2 and 1 stacks (the rule and the cap decide, not only
        // the guid), after Payday resolved; everyone is back to 0 once it resolved, before the next soak.
        CHECK(markers.size() == 3);
        CHECK(Parse(markers[0].line).size == 2.f && Is(markers[0].line, "pick", "nontank"));
        CHECK(Parse(markers[1].line).size == 1.f && Is(markers[1].line, "pick", "nontank"));
        CHECK(markers[0].atMs >= soaks[0].atMs + 2500 && markers[1].atMs <= soaks[1].atMs);
        CHECK(Parse(markers[2].line).size == 0.f);
        CHECK(markers[2].atMs >= soaks[1].atMs + 4000 && markers[2].atMs <= soaks[2].atMs);
        Parsed const fortune = Parse(soaks[2].line);
        CHECK(fortune.shape == "soak" && Key(fortune.line, "need", -1.f) == 0.f);
        std::string_view const shift = *Drill::Value(fortune.line, "shift");
        float const off = Drill::Number(shift.substr(shift.find(':') + 1)).value_or(0.f);
        CHECK(off >= 8.f && off <= 12.f);
        Parsed const lasting = Parse(soaks[3].line);
        CHECK(lasting.shape == "soak" && lasting.size == 3.f && Is(lasting.line, "anchor", "me"));
        CHECK(Key(lasting.line, "persist", 0.f) == 5000.f && Key(lasting.line, "need", 0.f) == 1.f);
    }

    // STAGE-M3-R-REPORT n-11: alexei runs one cycle by default (2.6 minutes), deadmines-followups two (its alternating
    // follow-up once each), stratholme-drops four (30 s cycles, 120 s), the others three.
    void TestDefaultCycles()
    {
        for (Drill::Def const& d : Drill::All())
        {
            std::string_view const name = d.name;
            CHECK(d.defaultCycles == (name == "alexei" ? 1u
                : name == "deadmines-followups" || name == "sfk-hunt" ? 2u
                : name == "stratholme-drops" ? 4u : 3u));
            CHECK(d.defaultCycles * d.cycleMs <= 180000u);
        }
    }

    // Scholomance RF1/RF2/RF5: the runners (Marked (drill) >= 3, self-keyed RunOut 8) and the stackers (0-2 stacks,
    // Stack 8 on Ras) split every player; a second runner appears during the warning; everyone thaws after the resolve.
    void TestRas()
    {
        Drill::Def const& d = *Drill::Find("ras");
        float resolve = 0.f;
        float stackMax = -1.f;
        float runMin = -1.f;
        std::vector<std::uint32_t> chills;
        std::uint32_t thaw = 0;
        for (Drill::Step const& s : d.steps)
        {
            Parsed const p = Parse(s.line);
            if (p.shape == "stack")
            {
                CHECK(p.size == 8.f && Is(s.line, "anchor", "target"));
                std::string_view const band = *Drill::Value(s.line, "aura");
                CHECK(band.substr(0, 7) == "205009:");
                stackMax = Drill::Number(band.substr(band.rfind(':') + 1)).value_or(-1.f);
                resolve = s.atMs + p.ms;
            }
            if (p.shape == "runout")
            {
                CHECK(p.size == 8.f);
                std::string_view const self = *Drill::Value(s.line, "self");
                CHECK(self.substr(0, 7) == "205009:");
                runMin = Drill::Number(self.substr(7)).value_or(-1.f);
                CHECK_NEAR(s.atMs + p.ms, resolve, kEps);
            }
            if (p.shape == "marker" && p.size > 0.f)
            {
                chills.push_back(s.atMs);
                CHECK(p.size == runMin || runMin < 0.f);
                CHECK(Is(s.line, "pick", "nontank"));
            }
            if (p.shape == "marker" && p.size == 0.f)
                thaw = s.atMs;
        }
        CHECK(stackMax + 1.f == runMin);        // every player is a stacker or a runner, never both
        CHECK(chills.size() == 2);
        CHECK(chills.size() == 2 && chills[0] == 0 && chills[1] > 0 && chills[1] < resolve);
        CHECK(thaw > resolve && thaw < d.cycleMs);
    }

    // ---- Deadmines Pilot gap 8 (deadmines-pilot.HANDOFF §3.9) ------------------------------------------------------

    // A signed decimal (offset=-30); Drill::Number reads no sign.
    float Signed(std::string_view v)
    {
        bool const neg = !v.empty() && v.front() == '-';
        float const n = Drill::Number(neg ? v.substr(1) : v).value_or(-999.f);
        return neg ? -n : n;
    }

    // after=resolve / after=with / cycle=: the step starts measured from the previous step of its cycle.
    void TestStepOffsetsGrammar()
    {
        Drill::Def const d = { "t", "", 30000, false, {
            { 1000, "circle 5 2000 tag=1" },                                // 1000
            { 0, "circle 5 1500 after=resolve tag=2" },                     // 1000 + 2000 = 3000
            { 500, "circle 5 1500 after=resolve tag=3" },                   // 3000 + 1500 + 500 = 5000
            { 0, "circle 5 1500 after=with tag=4" },                        // 5000 (with the previous)
            { 250, "circle 5 1500 after=with tag=5" },                      // 5250
            { 10000, "circle 5 2000 tag=6" },                               // 10000
            { 0, "circle 5 1500 after=resolve cycle=even tag=7" },          // 12000, even cycles
            { 0, "circle 5 1500 after=resolve cycle=odd tag=8" },           // 12000, odd cycles: step 6 is previous
            { 0, "circle 5 1500 after=resolve tag=9" },                     // 13500 either way
            { 20000, "marker 2 pick=nontank" },                             // 20000
            { 300, "circle 5 1500 after=resolve tag=10" },                  // a marker resolves at once: 20300
        } };
        for (std::uint32_t cycle = 0; cycle < 4; ++cycle)
        {
            std::vector<std::optional<std::uint32_t>> const at = Drill::StepOffsets(d, cycle);
            CHECK(at.size() == d.steps.size());
            if (at.size() != d.steps.size())
                return;
            CHECK(at[0] == 1000u && at[1] == 3000u && at[2] == 5000u && at[3] == 5000u && at[4] == 5250u);
            CHECK(at[5] == 10000u);
            bool const even = cycle % 2 == 0;
            CHECK(at[6].has_value() == even && at[7].has_value() == !even);
            CHECK((even ? at[6] : at[7]) == 12000u);
            CHECK(at[8] == 13500u);
            CHECK(at[9] == 20000u && at[10] == 20300u);
        }
        // An after= step with nothing before it in the cycle counts from the cycle start.
        Drill::Def const lone = { "u", "", 10000, false, {
            { 0, "circle 5 2000 cycle=odd tag=1" },
            { 700, "circle 5 1500 after=resolve tag=2" },
        } };
        CHECK(Drill::StepOffsets(lone, 0)[1] == 700u);
        CHECK(Drill::StepOffsets(lone, 1)[1] == 2700u);
        CHECK(!Drill::StepOffsets(lone, 0)[0].has_value() && Drill::StepOffsets(lone, 1)[0] == 0u);

        // The keys: readable values only; stripped before the `.mech test` parser, nothing else touched.
        CHECK(Drill::PlanKeysOk("circle 5 1500 after=resolve cycle=odd"));
        CHECK(Drill::PlanKeysOk("circle 5 1500 after=with cycle=even"));
        CHECK(Drill::PlanKeysOk("circle 5 1500"));
        CHECK(!Drill::PlanKeysOk("circle 5 1500 after=start"));
        CHECK(!Drill::PlanKeysOk("circle 5 1500 cycle=2"));
        CHECK(Drill::StripPlanKeys("circle 5 1500 after=resolve at=each cycle=odd weak=1 tag=12")
            == "circle 5 1500 at=each weak=1 tag=12");
        CHECK(Drill::StripPlanKeys("line 8 1500  at=last  pick=other") == "line 8 1500 at=last pick=other");
        CHECK(Drill::WarnMs("circle 5 1500 tag=1") == 1500u && Drill::WarnMs("marker 3") == 0u);
        CHECK(Drill::WarnMs("circle 5") == 0u);
        CHECK(Drill::RunsInCycle("circle 5 1 cycle=even", 2) && !Drill::RunsInCycle("circle 5 1 cycle=even", 3));
        CHECK(Drill::RunsInCycle("circle 5 1", 3) && Drill::RunsInCycle("circle 5 1 cycle=odd", 1));
    }

    // pick=other, pick=nontank, pick=any and min= (Drill::PickPool).
    void TestPickPool()
    {
        using C = Drill::PickCandidate;
        // 0 the tank (victim), 1 picked first, 2 picked second (the last), 3 and 4 fresh; distances from the source.
        std::vector<C> const c = { { true, false, false, 20.f }, { false, true, false, 12.f },
            { false, true, true, 3.f }, { false, false, false, 5.f }, { false, false, false, 9.f } };
        CHECK(Drill::PickPool(c, Drill::PickRule::Any, 0.f) == (std::vector<std::size_t>{ 0, 1, 2, 3, 4 }));
        CHECK(Drill::PickPool(c, Drill::PickRule::NonTank, 0.f) == (std::vector<std::size_t>{ 1, 2, 3, 4 }));
        CHECK(Drill::PickPool(c, Drill::PickRule::Other, 0.f) == (std::vector<std::size_t>{ 3, 4 }));
        // min=: the fresh ones at least 8 yd away; none that far: the farthest fresh one.
        CHECK(Drill::PickPool(c, Drill::PickRule::Other, 8.f) == (std::vector<std::size_t>{ 4 }));
        CHECK(Drill::PickPool(c, Drill::PickRule::Other, 15.f) == (std::vector<std::size_t>{ 4 }));
        CHECK(Drill::PickPool(c, Drill::PickRule::NonTank, 8.f) == (std::vector<std::size_t>{ 1, 4 }));
        // Every non-tank in the chain: any non-tank but the previous pick; only the previous pick left: it.
        std::vector<C> const used = { { true, false, false, 1.f }, { false, true, false, 1.f },
            { false, true, true, 1.f } };
        CHECK(Drill::PickPool(used, Drill::PickRule::Other, 0.f) == (std::vector<std::size_t>{ 1 }));
        std::vector<C> const one = { { true, false, false, 1.f }, { false, true, true, 1.f } };
        CHECK(Drill::PickPool(one, Drill::PickRule::Other, 0.f) == (std::vector<std::size_t>{ 1 }));
        // Only the tank: nobody for a non-tank pick (the runner reports no member).
        std::vector<C> const tankOnly = { { true, false, false, 9.f } };
        CHECK(Drill::PickPool(tankOnly, Drill::PickRule::Other, 0.f).empty());
        CHECK(Drill::PickPool(tankOnly, Drill::PickRule::NonTank, 8.f).empty());
        CHECK(Drill::PickPool({}, Drill::PickRule::Any, 0.f).empty());
    }

    // fix=1 / reach=1 and at=last: a frozen line toward a spot, and its far end.
    void TestFixedLines()
    {
        Vec3 const src{ 10.f, 10.f, 2.f };
        Drill::FixedLine const reach = Drill::FixLine(src, { 10.f, 22.f, 2.f }, 8.f, true, 0.f);
        CHECK_NEAR(reach.facing, Geo::kPi / 2.f, kEps);
        CHECK_NEAR(reach.length, 12.f, kEps);
        Vec3 const end = Drill::LineEnd(reach.source, reach.facing, reach.length);
        CHECK_NEAR(end.x, 10.f, kEps);
        CHECK_NEAR(end.y, 22.f, kEps);
        // A target closer than `size`: the line still runs `size` (through it).
        CHECK_NEAR(Drill::FixLine(src, { 13.f, 10.f, 2.f }, 8.f, true, 0.f).length, 8.f, kEps);
        // fix=1 without reach: `size` long, aimed through the spot (SN3: 30 yd through a non-tank).
        Drill::FixedLine const through = Drill::FixLine(src, { 0.f, 10.f, 2.f }, 30.f, false, 0.f);
        CHECK_NEAR(std::fabs(through.facing), Geo::kPi, kEps);
        CHECK_NEAR(through.length, 30.f, kEps);
        // A target on the source keeps the given facing.
        CHECK_NEAR(Drill::FixLine(src, src, 8.f, true, 1.25f).facing, 1.25f, kEps);
        // The target stands inside the 3 yd line at its end, the excluded 4 yd at the source stay clear.
        Shape line = Shape::Line(reach.length, 3.f);
        line.excludeStart = 4.f;
        Geo::Pose const pose{ src.x, src.y, src.z, reach.facing };
        CHECK(Geo::Contains(line, pose, { 10.f, 21.5f, 2.f }));
        CHECK(!Geo::Contains(line, pose, { 10.f, 12.f, 2.f }));
    }

    // The dash chain on a 5-member group as the runner draws it: three dashes, each from the last one's end, to three
    // different non-tanks at least 8 yd from the dash's start (when someone is that far).
    void TestDashChain()
    {
        Drill::Def const& d = *Drill::Find("deadmines-followups");
        std::vector<Vec3> const members = { { 0.f, 3.f, 0.f }, { 15.f, 0.f, 0.f }, { -12.f, 4.f, 0.f },
            { 3.f, -14.f, 0.f }, { 6.f, 6.f, 0.f } };
        std::size_t const tank = 0;
        for (std::uint32_t seed = 1; seed <= 40; ++seed)
        {
            std::uint32_t rng = seed;
            auto draw = [&rng](std::size_t n)
            {
                rng = rng * 1103515245u + 12345u;
                return static_cast<std::size_t>((rng >> 16) % n);
            };
            std::vector<std::size_t> chain;
            std::optional<std::size_t> last;
            Vec3 source{ 0.f, 0.f, 0.f };   // the creature
            for (std::size_t k = 0; k < 3; ++k)
            {
                std::string_view const line = d.steps[k].line;
                Drill::PickRule const rule = Is(line, "pick", "other") ? Drill::PickRule::Other
                    : Drill::PickRule::NonTank;
                float const min = Key(line, "min", 0.f);
                std::vector<Drill::PickCandidate> c;
                for (std::size_t m = 0; m < members.size(); ++m)
                    c.push_back({ m == tank, std::find(chain.begin(), chain.end(), m) != chain.end(), last == m,
                        Geo::Dist2d(members[m].x, members[m].y, source.x, source.y) });
                std::vector<std::size_t> const pool = Drill::PickPool(c, rule, min);
                CHECK(!pool.empty());
                if (pool.empty())
                    return;
                std::size_t const pick = pool[draw(pool.size())];
                CHECK(pick != tank);
                CHECK(std::find(chain.begin(), chain.end(), pick) == chain.end());
                // Someone is 8 yd away from every start in this layout, so the pick is.
                CHECK(c[pick].dist >= 8.f);
                if (rule != Drill::PickRule::Other)
                    chain.clear();
                chain.push_back(pick);
                last = pick;
                Drill::FixedLine const dash = Drill::FixLine(source, members[pick], Parse(line).size,
                    Is(line, "reach", "1"), 0.f);
                Vec3 const end = Drill::LineEnd(dash.source, dash.facing, dash.length);
                CHECK_NEAR(end.x, members[pick].x, 1e-2);
                CHECK_NEAR(end.y, members[pick].y, 1e-2);
                source = end;               // at=last: the next dash starts here
            }
            CHECK(chain.size() == 3);
        }
    }

    // The drill table of §3.9: rows, shapes, timings (first shapes 2 s, follow-ups 1.5 s after the first resolves),
    // anchors, tags and the alternating SN2 / SN3 follow-up.
    void TestDeadminesFollowups()
    {
        Drill::Def const* found = Drill::Find("deadmines-followups");
        CHECK(found != nullptr);
        if (!found)
            return;
        Drill::Def const& d = *found;
        CHECK(d.cycleMs == 30000 && d.defaultCycles == 2 && !d.ownerAtCreature);
        CHECK(d.steps.size() == 13);
        if (d.steps.size() != 13)
            return;
        // Every damage step carries Weakened; the drill adds dmg= (flat 1 by default).
        for (Drill::Step const& s : d.steps)
            CHECK(Is(s.line, "weak", "1") && Drill::DamageShape(Parse(s.line).shape));

        struct Row
        {
            std::uint32_t at;
            std::vector<std::size_t> first;
            std::vector<std::size_t> followups;
        };
        std::vector<Row> const rows = { { 0, { 0 }, { 1, 2 } }, { 8000, { 3 }, { 4, 5 } }, { 14000, { 6 }, { 7 } },
            { 20000, { 8 }, { 9, 10 } }, { 25000, { 11 }, { 12 } } };
        std::set<std::uint32_t> firstTags;
        std::set<std::uint32_t> followTags;
        for (std::uint32_t cycle = 0; cycle < 2; ++cycle)
        {
            std::vector<std::optional<std::uint32_t>> const at = Drill::StepOffsets(d, cycle);
            for (std::size_t r = 0; r < rows.size(); ++r)
            {
                Row const& row = rows[r];
                std::size_t const f = row.first[0];
                CHECK(at[f] == row.at);
                CHECK(Parse(d.steps[f].line).ms == 2000.f);
                CHECK(!Drill::Value(d.steps[f].line, "after"));
                firstTags.insert(static_cast<std::uint32_t>(Key(d.steps[f].line, "tag", 0.f)));
                float rowEnd = static_cast<float>(*at[f]) + 2000.f;
                for (std::size_t i : row.followups)
                {
                    Parsed const p = Parse(d.steps[i].line);
                    CHECK(p.ms == 1500.f);
                    CHECK(Drill::Value(d.steps[i].line, "after").has_value());
                    std::uint32_t const tag = static_cast<std::uint32_t>(Key(d.steps[i].line, "tag", 0.f));
                    CHECK(Drill::FollowupTag(tag));
                    followTags.insert(tag);
                    if (at[i])
                        rowEnd = std::max(rowEnd, static_cast<float>(*at[i]) + p.ms);
                }
                // The next row starts after this one's last resolve, at least 1 s later.
                std::uint32_t const next = r + 1 < rows.size() ? rows[r + 1].at : d.cycleMs;
                CHECK(rowEnd + 1000.f <= static_cast<float>(next));
            }
            // Each follow-up resolves 1.5 s after the shape before it (the first, or the previous dash).
            auto resolve = [&](std::size_t i) { return static_cast<float>(*at[i]) + Parse(d.steps[i].line).ms; };
            CHECK_NEAR(resolve(1) - resolve(0), 1500.f, kEps);
            CHECK_NEAR(resolve(2) - resolve(1), 1500.f, kEps);
            CHECK_NEAR(resolve(4) - resolve(3), 1500.f, kEps);
            CHECK_NEAR(resolve(5) - resolve(3), 1500.f, kEps);       // both aftershocks together
            CHECK_NEAR(resolve(7) - resolve(6), 1500.f, kEps);
            std::size_t const sn = cycle % 2 == 0 ? 9 : 10;
            CHECK(at[9].has_value() == (cycle % 2 == 0) && at[10].has_value() == (cycle % 2 == 1));
            CHECK_NEAR(resolve(sn) - resolve(8), 1500.f, kEps);
            CHECK_NEAR(resolve(12) - resolve(11), 1500.f, kEps);
            // The second Cave In wave registers as the first lands: positions are taken then.
            CHECK(at[12] == 27000u);
        }
        // First-shape and follow-up tags never share a value (the sweep tells them apart).
        CHECK(firstTags == (std::set<std::uint32_t>{ 1, 4, 6, 8, 11 }));
        CHECK(followTags == (std::set<std::uint32_t>{ 2, 3, 5, 7, 9, 10, 12 }));
        for (std::uint32_t t : firstTags)
            CHECK(!Drill::FollowupTag(t));

        // SM4: 3 yd lines, 4 yd excluded at the start; first from the creature to a non-tank >= 8 yd, the follow-ups
        // from the last end to a non-tank the chain has not hit, ending at the target.
        for (std::size_t i = 0; i < 3; ++i)
        {
            std::string_view const l = d.steps[i].line;
            CHECK(Parse(l).shape == "line" && Key(l, "width", 0.f) == 3.f && Key(l, "exclude", 0.f) == 4.f);
            CHECK(Is(l, "to", "member") && Is(l, "reach", "1") && Key(l, "min", 0.f) == 8.f);
            CHECK(Is(l, "pick", i == 0 ? "nontank" : "other"));
            CHECK(i == 0 ? Is(l, "anchor", "target") : Is(l, "at", "last") && Is(l, "after", "resolve"));
        }
        // SM7 + SM8: a 4 x 25 frontal line, then two at +-30 degrees, both from the first's resolve.
        for (std::size_t i = 3; i < 6; ++i)
        {
            std::string_view const l = d.steps[i].line;
            CHECK(Parse(l).shape == "frontal" && Parse(l).size == 25.f && Is(l, "line", "1"));
            CHECK(Key(l, "width", 0.f) == 4.f);
        }
        CHECK(!Drill::Value(d.steps[3].line, "offset"));
        CHECK(Signed(*Drill::Value(d.steps[4].line, "offset")) == 30.f && Is(d.steps[4].line, "after", "resolve"));
        CHECK(Signed(*Drill::Value(d.steps[5].line, "offset")) == -30.f && Is(d.steps[5].line, "after", "with"));
        // SM10 + SM11: donut 5 / 14 on the creature, then circle 6 on it.
        CHECK(Parse(d.steps[6].line).shape == "donut" && Parse(d.steps[6].line).size == 14.f);
        CHECK(Key(d.steps[6].line, "inner", 0.f) == 5.f && Is(d.steps[6].line, "anchor", "target"));
        CHECK(Parse(d.steps[7].line).shape == "circle" && Parse(d.steps[7].line).size == 6.f);
        CHECK(Is(d.steps[7].line, "anchor", "target"));
        // The Haft Slam circle covers the donut's safe hole: in (within 5), then out (beyond 6).
        CHECK(Parse(d.steps[7].line).size > Key(d.steps[6].line, "inner", 0.f));
        // SN1, then SN2 (even cycles) or SN3 (odd cycles).
        CHECK(Parse(d.steps[8].line).shape == "circle" && Parse(d.steps[8].line).size == 8.f);
        CHECK(Is(d.steps[8].line, "anchor", "target"));
        CHECK(Parse(d.steps[9].line).shape == "frontal" && Parse(d.steps[9].line).size == 10.f);
        CHECK(Key(d.steps[9].line, "angle", 0.f) == 90.f && Is(d.steps[9].line, "cycle", "even"));
        std::string_view const runaway = d.steps[10].line;
        CHECK(Parse(runaway).shape == "line" && Parse(runaway).size == 30.f && Key(runaway, "width", 0.f) == 4.f);
        CHECK(Is(runaway, "to", "member") && Is(runaway, "pick", "nontank") && Is(runaway, "fix", "1"));
        CHECK(Is(runaway, "anchor", "target") && Key(runaway, "exclude", 0.f) == 4.f && Is(runaway, "cycle", "odd"));
        CHECK(!Drill::Value(runaway, "reach"));
        // RZ1 + RZ10: circle 5 under every member, twice.
        for (std::size_t i : { std::size_t(11), std::size_t(12) })
            CHECK(Parse(d.steps[i].line).shape == "circle" && Parse(d.steps[i].line).size == 5.f
                && Is(d.steps[i].line, "at", "each"));
        // The `.mech test` line the runner parses keeps every key but after= and cycle=.
        CHECK(Drill::StripPlanKeys(d.steps[10].line).find("cycle=") == std::string::npos);
        CHECK(Drill::StripPlanKeys(d.steps[10].line).find("fix=1") != std::string::npos);
    }

    void TestNumberParsing()
    {
        CHECK(Drill::Number("12").value_or(-1.f) == 12.f);
        CHECK_NEAR(Drill::Number("0.3").value_or(-1.f), 0.3f, 1e-6);
        CHECK(!Drill::Number("-1"));
        CHECK(!Drill::Number("1a"));
        CHECK(!Drill::Number(""));
        CHECK(Drill::Tokens("  a  b c ").size() == 3);
        CHECK(Drill::Value("soak 2 2500 need=1", "need").value_or("") == "1");
        CHECK(!Drill::Value("soak 2 2500 need=1", "ne"));
    }
}

namespace
{
    // M5: a program entry's first token picks a descriptor entry before a drill name, so no drill (and not the lane
    // set) may be named like one.
    void TestNoDrillShadowsADescriptorEntry()
    {
        for (Drill::Def const& d : Drill::All())
            CHECK(!Drill::DescriptorEntryKind(d.name));
        CHECK(!Drill::DescriptorEntryKind(Drill::Lanes().name));
        for (std::string_view name : { "barrage", "minefield", "uminefield", "none", "all" })
            CHECK(!Drill::DescriptorEntryKind(name));
        for (std::string_view name : { "wait", "cast", "add", "debuff", "buff", "swap", "hold", "describe" })
            CHECK(Drill::DescriptorEntryKind(name) && !Drill::Find(name));
    }
}

int main()
{
    TestCentroidAndShift();
    TestRingSpots();
    TestLaneSource();
    TestEveryDrillIsWellFormed();
    TestSmite();
    TestCookie();
    TestFenrus();
    TestAlexei();
    TestSoaks();
    TestDefaultCycles();
    TestRas();
    TestNumberParsing();
    TestNoDrillShadowsADescriptorEntry();
    TestStepOffsetsGrammar();
    TestPickPool();
    TestFixedLines();
    TestDashChain();
    TestDeadminesFollowups();
    return Report("drill_test");
}
