// Standalone tests for src/MechanicDrill.h (STAGE-M3-HANDOFF §3.11, WP-D): the `.mech` landing geometry (group
// centroid, shift, spot ring, centered lane) and a desk check of every `.mech drill` against the boss doc rows it
// reproduces (shapes, sizes, timings, the separations Alexei's pairs need). Build: tests/run.sh.

#include "BossMechanicsIds.h"
#include "check.h"
#include "MechanicDrill.h"
#include <algorithm>
#include <cmath>
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
        "mine", "lanes", "spread", "runout", "stack", "soak", "frontal", "bait", "holdstill", "spot" };

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
    // window's length.
    float EndMs(Drill::Step const& step)
    {
        Parsed const p = Parse(step.line);
        float end = static_cast<float>(step.atMs) + p.ms;
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
            std::uint32_t last = 0;
            for (Drill::Step const& step : d.steps)
            {
                CHECK(step.atMs >= last);           // sorted: the command schedules in time order
                last = step.atMs;
                CHECK(step.atMs < d.cycleMs);
                Parsed const p = Parse(step.line);
                if (p.shape == "weaken")
                {
                    CHECK(p.size >= 0.f && p.size <= 7.f);
                    continue;
                }
                CHECK(kTestShapes.count(p.shape) == 1);
                CHECK(p.size > 0.f);
                CHECK(p.ms >= 0.f);
                // Over before the next cycle starts, so cycles never stack.
                CHECK(EndMs(step) <= static_cast<float>(d.cycleMs));
                // Ledger tags name the mechanic.
                CHECK(Drill::Value(step.line, "tag").has_value());
                // dmg= comes from the drill; get-in kinds take none.
                CHECK(!Drill::Value(step.line, "dmg"));
                if (!Drill::DamageShape(p.shape))
                    CHECK(!Drill::Value(step.line, "weak"));
            }
        }
        for (char const* name : { "smite", "cookie", "fenrus", "alexei", "soaks", "ras" })
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
        std::vector<Drill::Step> weakens;
        for (Drill::Step const& s : d.steps)
            (Parse(s.line).shape == "weaken" ? weakens : soaks).push_back(s);
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
        CHECK(Is(ordered.line, "order", "205000:asc:cap=2"));
        // STAGE-M3-R-REPORT n-7: before it, two non-tanks get 2 and 1 stacks (the rule and the cap decide, not only
        // the guid), after Payday resolved; everyone is back to 0 once it resolved, before the next soak.
        CHECK(weakens.size() == 3);
        CHECK(Parse(weakens[0].line).size == 2.f && Is(weakens[0].line, "pick", "nontank"));
        CHECK(Parse(weakens[1].line).size == 1.f && Is(weakens[1].line, "pick", "nontank"));
        CHECK(weakens[0].atMs >= soaks[0].atMs + 2500 && weakens[1].atMs <= soaks[1].atMs);
        CHECK(Parse(weakens[2].line).size == 0.f);
        CHECK(weakens[2].atMs >= soaks[1].atMs + 4000 && weakens[2].atMs <= soaks[2].atMs);
        Parsed const fortune = Parse(soaks[2].line);
        CHECK(fortune.shape == "soak" && Key(fortune.line, "need", -1.f) == 0.f);
        std::string_view const shift = *Drill::Value(fortune.line, "shift");
        float const off = Drill::Number(shift.substr(shift.find(':') + 1)).value_or(0.f);
        CHECK(off >= 8.f && off <= 12.f);
        Parsed const lasting = Parse(soaks[3].line);
        CHECK(lasting.shape == "soak" && lasting.size == 3.f && Is(lasting.line, "anchor", "me"));
        CHECK(Key(lasting.line, "persist", 0.f) == 5000.f && Key(lasting.line, "need", 0.f) == 1.f);
    }

    // STAGE-M3-R-REPORT n-11: alexei runs one cycle by default (2.6 minutes), the others three.
    void TestDefaultCycles()
    {
        for (Drill::Def const& d : Drill::All())
        {
            std::string_view const name = d.name;
            CHECK(d.defaultCycles == (name == "alexei" ? 1u : 3u));
            CHECK(d.defaultCycles * d.cycleMs <= 180000u);
        }
    }

    // Scholomance RF1/RF2/RF5: the runners (Weakened >= 3, self-keyed RunOut 8) and the stackers (0-2 stacks, Stack 8
    // on Ras) split every player; a second runner appears during the warning; everyone thaws after the resolve.
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
                CHECK(band.substr(0, 7) == "205000:");
                stackMax = Drill::Number(band.substr(band.rfind(':') + 1)).value_or(-1.f);
                resolve = s.atMs + p.ms;
            }
            if (p.shape == "runout")
            {
                CHECK(p.size == 8.f);
                std::string_view const self = *Drill::Value(s.line, "self");
                CHECK(self.substr(0, 7) == "205000:");
                runMin = Drill::Number(self.substr(7)).value_or(-1.f);
                CHECK_NEAR(s.atMs + p.ms, resolve, kEps);
            }
            if (p.shape == "weaken" && p.size > 0.f)
            {
                chills.push_back(s.atMs);
                CHECK(p.size == runMin || runMin < 0.f);
                CHECK(Is(s.line, "pick", "nontank"));
            }
            if (p.shape == "weaken" && p.size == 0.f)
                thaw = s.atMs;
        }
        CHECK(stackMax + 1.f == runMin);        // every player is a stacker or a runner, never both
        CHECK(chills.size() == 2);
        CHECK(chills.size() == 2 && chills[0] == 0 && chills[1] > 0 && chills[1] < resolve);
        CHECK(thaw > resolve && thaw < d.cycleMs);
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
    return Report("drill_test");
}
