// Standalone tests for the Stratholme follow-ups of src/MechanicDrill.h (Stratholme build §3.9, WP-G): the
// `stratholme-followups` drill against the build spec rows it reproduces (The Unforgiven UF1 heroic, Malor MZ7 + MZ8,
// Baroness BA4 + BA5, Balnazzar BZ1 heroic, Willey WY9 heroic), the geometry it relies on (the crossing ranks through
// one center, the drawn holes of the two donuts, the tracking cone and its exempt target, the hall halves), the
// exemption helper and the sweep's program. Build: tests/run.sh.

#include "BossMechanicsIds.h"
#include "check.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include "MechanicMath.h"
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
    // BossMechanics.Bots.SafetyMarginYd default (M2) and a player's base run speed.
    constexpr float kMarginYd = 0.5f;

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

    // `shift=<ahead>:<left>` with signs (Drill::Number reads no sign).
    bool Shift(std::string_view line, float& ahead, float& left)
    {
        std::optional<std::string_view> v = Drill::Value(line, "shift");
        if (!v)
            return false;
        std::size_t const colon = v->find(':');
        if (colon == std::string_view::npos)
            return false;
        auto signedNumber = [](std::string_view s) -> std::optional<float>
        {
            bool const neg = !s.empty() && s[0] == '-';
            std::optional<float> n = Drill::Number(neg ? s.substr(1) : s);
            if (!n)
                return std::nullopt;
            return neg ? -*n : *n;
        };
        std::optional<float> a = signedNumber(v->substr(0, colon));
        std::optional<float> l = signedNumber(v->substr(colon + 1));
        if (!a || !l)
            return false;
        ahead = *a;
        left = *l;
        return true;
    }

    // One step of a cycle: its line, when it registers and when it resolves (ms after the cycle start).
    struct Timed
    {
        std::string_view line;
        std::uint32_t startMs = 0;
        std::uint32_t resolveMs = 0;
    };

    std::vector<Timed> StepsWithTag(Drill::Def const& d, std::uint32_t tag, std::uint32_t cycle = 0)
    {
        std::vector<std::optional<std::uint32_t>> const at = Drill::StepOffsets(d, cycle);
        std::vector<Timed> out;
        for (std::size_t i = 0; i < d.steps.size(); ++i)
            if (at[i] && Tag(d.steps[i].line) == tag)
                out.push_back({ d.steps[i].line, *at[i], *at[i] + Drill::WarnMs(d.steps[i].line) });
        return out;
    }

    Drill::Def const* Followups()
    {
        return Drill::Find("stratholme-followups");
    }

    // The drawn hole of a donut (PickVisual's ratio step x the outer radius); nullopt when it draws a full disc.
    std::optional<float> DrawnHole(float inner, float outer)
    {
        VisualPick const pick = PickVisual(BossMechanics::Shape::Donut(inner, outer), 0, MarkerLayer::Outline);
        if (pick.filled || pick.pieces.empty())
            return std::nullopt;
        Ids::MarkerDisplayParts const parts = Ids::DecodeMarkerDisplay(pick.pieces[0].display);
        if (!parts.valid || parts.step >= Ids::kDonutRatios.size())
            return std::nullopt;
        return Ids::kDonutRatios[parts.step] * outer;
    }

    // ---- the drill table -------------------------------------------------------------------------------------------
    void TestTable()
    {
        Drill::Def const* found = Followups();
        CHECK(found != nullptr);
        if (!found)
            return;
        Drill::Def const& d = *found;
        CHECK(d.cycleMs == 40000 && d.defaultCycles == 3 && !d.ownerAtCreature);
        std::set<std::uint32_t> tags;
        for (Drill::Step const& s : d.steps)
        {
            std::uint32_t const tag = Tag(s.line);
            CHECK(tag >= Drill::kStratTagRank && tag <= Drill::kStratTagBarrage);
            tags.insert(tag);
            // Every step deals damage (the runner appends dmg=) and names its heroic percent.
            CHECK(Drill::DamageStep(s.line));
            CHECK(Key(s.line, "pct", 0.f) >= 30.f);
            CHECK(Drill::PlanKeysOk(s.line));
        }
        CHECK(tags.size() == 9);
        // The bar's tags are the follow-ups and the hidden Barrage; the first shapes are reported only.
        for (std::uint32_t t : Drill::kStratBarTags)
            CHECK(tags.count(t) == 1 && Drill::StratBarTag(t));
        for (std::uint32_t t : { Drill::kStratTagRank, Drill::kStratTagSmash, Drill::kStratTagWail,
            Drill::kStratTagSwarm })
            CHECK(!Drill::StratBarTag(t));
        CHECK(!Drill::StratBarTag(0) && !Drill::StratBarTag(10));
        // Every mechanic is over before the next row starts (rows at 0, 7, 13, 20 and 25 s; §3.9).
        std::uint32_t const rows[] = { 0, 7000, 13000, 20000, 25000, 40000 };
        std::uint32_t const firstTag[] = { 1, 3, 5, 7, 9 };
        std::uint32_t const lastTag[] = { 2, 4, 6, 8, 9 };
        for (std::uint32_t cycle = 0; cycle < 2; ++cycle)
            for (std::size_t r = 0; r < 5; ++r)
            {
                std::vector<Timed> const first = StepsWithTag(d, firstTag[r], cycle);
                std::vector<Timed> const last = StepsWithTag(d, lastTag[r], cycle);
                CHECK(!first.empty() && !last.empty());
                if (first.empty() || last.empty())
                    continue;
                CHECK(first[0].startMs == rows[r]);
                for (Timed const& t : last)
                    CHECK(t.resolveMs < rows[r + 1]);
            }
    }

    // UF1 heroic: a 30 x 10 rank (3 s), then the crossing rank on the other axis through the same center, 1.5 s after
    // the first passes. 45% Shadow, Weakened.
    void TestCrossingRanks()
    {
        Drill::Def const& d = *Followups();
        std::vector<Timed> const rank = StepsWithTag(d, Drill::kStratTagRank);
        std::vector<Timed> const crossing = StepsWithTag(d, Drill::kStratTagCrossing);
        CHECK(rank.size() == 1 && crossing.size() == 1);
        if (rank.size() != 1 || crossing.size() != 1)
            return;
        for (Timed const& t : { rank[0], crossing[0] })
        {
            CHECK(ShapeOf(t.line) == "lanes" && Size(t.line) == Drill::kRankWidth);
            CHECK(Key(t.line, "count", 0.f) == 1.f);
            CHECK(Is(t.line, "school", "shadow") && Key(t.line, "pct", 0.f) == 45.f && Is(t.line, "weak", "1"));
        }
        CHECK(rank[0].resolveMs - rank[0].startMs == 3000);
        CHECK(crossing[0].startMs == rank[0].resolveMs);
        CHECK(crossing[0].resolveMs - crossing[0].startMs == 1500);
        CHECK(Is(rank[0].line, "axis", "x") && Is(rank[0].line, "frame", "30x10"));
        CHECK(Is(crossing[0].line, "axis", "y") && Is(crossing[0].line, "frame", "10x30"));

        // The runtime's frame: centered on the anchor point, X along the facing (MechanicCommand.cpp, lanes). One lane
        // as wide as the frame's span has no slack: it is centered whatever the dice say, and never falls back.
        for (float facing : { 0.f, 1.1f, 3.761f })
        {
            Vec3 const center{ 100.f, -50.f, 0.f };
            auto frameOf = [&center, facing](float sx, float sy)
            {
                float const c = std::cos(facing);
                float const n = std::sin(facing);
                return Frame{ center.x - c * sx / 2.f + n * sy / 2.f, center.y - n * sx / 2.f - c * sy / 2.f, 0.f,
                    facing, sx, sy };
            };
            Frame const fx = frameOf(Drill::kRankLength, Drill::kRankWidth);
            Frame const fy = frameOf(Drill::kRankWidth, Drill::kRankLength);
            for (float dice : { 0.f, 0.5f, 0.999f })
            {
                bool fellBack = true;
                std::vector<float> const ox = RandomLaneOffsets(FrameSpanAcross(fx, Axis::X), 1, Drill::kRankWidth,
                    6.f, [dice]() { return dice; }, &fellBack);
                CHECK(!fellBack && ox.size() == 1 && std::fabs(ox[0]) < kEps);
                std::vector<float> const oy = RandomLaneOffsets(FrameSpanAcross(fy, Axis::Y), 1, Drill::kRankWidth,
                    6.f, [dice]() { return dice; }, &fellBack);
                CHECK(!fellBack && oy.size() == 1 && std::fabs(oy[0]) < kEps);
            }
            LanePlacement const a = PlaceLane(fx, { Axis::X, 0.f, Drill::kRankWidth });
            LanePlacement const b = PlaceLane(fy, { Axis::Y, 0.f, Drill::kRankWidth });
            CHECK_NEAR(a.length, Drill::kRankLength, kEps);
            CHECK_NEAR(b.length, Drill::kRankLength, kEps);
            // Both lanes' midpoints are the center, and they cross at a right angle.
            Geo::Pose const ma = Geo::Advance(a.pose, a.pose.o, a.length / 2.f);
            Geo::Pose const mb = Geo::Advance(b.pose, b.pose.o, b.length / 2.f);
            CHECK_NEAR(ma.x, center.x, kEps);
            CHECK_NEAR(ma.y, center.y, kEps);
            CHECK_NEAR(mb.x, center.x, kEps);
            CHECK_NEAR(mb.y, center.y, kEps);
            CHECK_NEAR(std::cos(a.pose.o - b.pose.o), 0.f, kEps);
            // A player who left the first rank sideways (6 yd off its axis, at the center) stands in the crossing.
            BossMechanics::Shape const lineA = BossMechanics::Shape::Line(a.length, a.width);
            BossMechanics::Shape const lineB = BossMechanics::Shape::Line(b.length, b.width);
            Vec3 const dodged{ center.x - std::sin(facing) * 6.f, center.y + std::cos(facing) * 6.f, 0.f };
            CHECK(!Geo::Contains(lineA, a.pose, dodged) && Geo::Contains(lineB, b.pose, dodged));
            // The safe ground is a quadrant corner: 5 yd off both axes.
            Vec3 const corner{ center.x + std::cos(facing) * 6.f - std::sin(facing) * 6.f,
                center.y + std::sin(facing) * 6.f + std::cos(facing) * 6.f, 0.f };
            CHECK(!Geo::Contains(lineA, a.pose, corner) && !Geo::Contains(lineB, b.pose, corner));
        }
    }

    // MZ7 + MZ8 heroic: circle 8 on the owner, tank included (2 s), then the donut 8 / 14 (1.5 s): out, then back in.
    // 30% Physical each, Weakened.
    void TestSmashAftershock()
    {
        Drill::Def const& d = *Followups();
        std::vector<Timed> const smash = StepsWithTag(d, Drill::kStratTagSmash);
        std::vector<Timed> const shock = StepsWithTag(d, Drill::kStratTagAftershock);
        CHECK(smash.size() == 1 && shock.size() == 1);
        if (smash.size() != 1 || shock.size() != 1)
            return;
        CHECK(ShapeOf(smash[0].line) == "circle" && Size(smash[0].line) == 8.f);
        CHECK(Is(smash[0].line, "anchor", "target"));
        CHECK(smash[0].resolveMs - smash[0].startMs == 2000);
        CHECK(ShapeOf(shock[0].line) == "donut" && Size(shock[0].line) == 14.f);
        CHECK(Key(shock[0].line, "inner", 0.f) == 8.f);
        CHECK(Is(shock[0].line, "anchor", "target"));
        CHECK(shock[0].startMs == smash[0].resolveMs && shock[0].resolveMs - shock[0].startMs == 1500);
        for (Timed const& t : { smash[0], shock[0] })
        {
            CHECK(Key(t.line, "pct", 0.f) == 30.f && Is(t.line, "weak", "1"));
            // Physical (the default school) and no tank exemption: the tank goes too.
            CHECK(!Drill::Value(t.line, "school") && !Drill::Value(t.line, "role"));
        }
        // The donut's inner edge is the circle's: who left the circle stands in the ring. The ring draws at the 0.50
        // step (8 / 14 = 0.57 rounds down): a 7 yd drawn hole, never bigger than the real one.
        std::optional<float> const hole = DrawnHole(8.f, 14.f);
        CHECK(hole.has_value());
        if (hole)
        {
            CHECK(*hole <= 8.f + kEps);
            CHECK_NEAR(*hole, 7.f, kEps);
        }
        // Back in from just outside the circle (8 + margin) to inside the hole (8 - margin): 1 yd, well inside 1.5 s.
        CHECK((2.f * kMarginYd) / Drill::kPlayerRunYdS * 1000.f < 1500.f);
    }

    // BA4 + BA5 heroic: circle 12 on her (3 s), then the echo beyond 8 yd (2 s after the wail lands). 50% then 40%
    // Shadow, no Weakened (ST: over 30%, Terrified instead).
    void TestWailEcho()
    {
        Drill::Def const& d = *Followups();
        std::vector<Timed> const wail = StepsWithTag(d, Drill::kStratTagWail);
        std::vector<Timed> const echo = StepsWithTag(d, Drill::kStratTagEcho);
        CHECK(wail.size() == 1 && echo.size() == 1);
        if (wail.size() != 1 || echo.size() != 1)
            return;
        CHECK(ShapeOf(wail[0].line) == "circle" && Size(wail[0].line) == 12.f && Is(wail[0].line, "anchor", "target"));
        CHECK(wail[0].resolveMs - wail[0].startMs == 3000);
        CHECK(Key(wail[0].line, "pct", 0.f) == 50.f && Is(wail[0].line, "school", "shadow"));
        CHECK(ShapeOf(echo[0].line) == "donut" && Is(echo[0].line, "anchor", "target"));
        CHECK(Size(echo[0].line) == Drill::kEchoOuter && Key(echo[0].line, "inner", 0.f) == Drill::kEchoInner);
        CHECK(echo[0].startMs == wail[0].resolveMs && echo[0].resolveMs - echo[0].startMs == 2000);
        CHECK(Key(echo[0].line, "pct", 0.f) == 40.f && Is(echo[0].line, "school", "shadow"));
        for (Timed const& t : { wail[0], echo[0] })
            CHECK(!Drill::Value(t.line, "weak"));
        // Ratio exactly 0.25, the ladder's first step: the drawn hole is the real 8 yd one. The arena's leash (30 yd)
        // lies inside the echo, so nobody escapes it by running out.
        CHECK_NEAR(Drill::kEchoInner / Drill::kEchoOuter, Ids::kDonutRatios[0], 1e-6f);
        std::optional<float> const hole = DrawnHole(Drill::kEchoInner, Drill::kEchoOuter);
        CHECK(hole.has_value());
        if (hole)
            CHECK_NEAR(*hole, Drill::kEchoInner, kEps);
        CHECK(Drill::kEchoOuter >= 30.f);
        // The §3.9 row's 8 / 60 would draw a full disc: no safe hole shown (the finding the constants answer).
        CHECK(!DrawnHole(8.f, 60.f).has_value());
        // Out past 12 (+ margin), then back inside 8 (- margin): 5 yd in 2 s at run speed.
        CHECK((12.f - 8.f + 2.f * kMarginYd) / Drill::kPlayerRunYdS * 1000.f < 2000.f);
    }

    // BZ1 heroic: a 60 degree / 30 yd cone from him toward a non-tank, tracking them (2.5 s), the target exempt; a
    // second at another non-tank 1.5 s after the first starts. 40% Shadow to the others.
    void TestTrackingCones()
    {
        Drill::Def const& d = *Followups();
        std::vector<Timed> const first = StepsWithTag(d, Drill::kStratTagSwarm);
        std::vector<Timed> const second = StepsWithTag(d, Drill::kStratTagSecondSwarm);
        CHECK(first.size() == 1 && second.size() == 1);
        if (first.size() != 1 || second.size() != 1)
            return;
        for (Timed const& t : { first[0], second[0] })
        {
            CHECK(ShapeOf(t.line) == "cone" && Size(t.line) == 30.f && Key(t.line, "angle", 0.f) == 60.f);
            CHECK(t.resolveMs - t.startMs == 2500);
            CHECK(Is(t.line, "anchor", "target") && Is(t.line, "to", "member") && Is(t.line, "exempt", "target"));
            // Tracking comes from to= (live aim), never follow=1 (the parser refuses both together).
            CHECK(!Drill::Value(t.line, "follow"));
            CHECK(Key(t.line, "pct", 0.f) == 40.f && Is(t.line, "school", "shadow") && !Drill::Value(t.line, "weak"));
        }
        CHECK(Is(first[0].line, "pick", "nontank") && Is(second[0].line, "pick", "other"));
        CHECK(Is(second[0].line, "after", "with"));
        CHECK(second[0].startMs - first[0].startMs == 1500);

        // The target is inside its own cone wherever it walks (so the exemption is what spares it); a member 9 yd to
        // its side at 15 yd from the boss is outside (31 degrees off the aim), so spreading answers it.
        BossMechanics::Shape const cone = BossMechanics::Shape::Cone(60.f, 30.f);
        for (float bearing : { 0.f, 0.7f, 2.5f, -1.9f })
            for (float dist : { 3.f, 15.f, 29.f })
            {
                Vec3 const target{ std::cos(bearing) * dist, std::sin(bearing) * dist, 0.f };
                Geo::Pose const aim{ 0.f, 0.f, 0.f, std::atan2(target.y, target.x) };
                CHECK(Geo::Contains(cone, aim, target));
            }
        Geo::Pose const aim{ 0.f, 0.f, 0.f, 0.f };
        CHECK(!Geo::Contains(cone, aim, Vec3{ 15.f, 9.f, 0.f }));
        CHECK(Geo::Contains(cone, aim, Vec3{ 15.f, 5.f, 0.f }));
    }

    // exempt=target's carriers: every member but the aimed one, in order.
    void TestAllBut()
    {
        std::vector<int> const members = { 3, 5, 8, 13 };
        CHECK((Drill::AllBut(members, 8) == std::vector<int>{ 3, 5, 13 }));
        CHECK((Drill::AllBut(members, 4) == members));
        CHECK(Drill::AllBut(std::vector<int>{ 7 }, 7).empty());
        CHECK(Drill::AllBut(std::vector<int>{}, 7).empty());
    }

    // WY9 heroic: five blasts on the hall's halves, no marker, all registered at the call: the first resolves 3 s
    // later and the others 1.5 s apart, 3 on one half and 2 on the other. 30% Fire, Weakened.
    void TestHiddenBarrage()
    {
        Drill::Def const& d = *Followups();
        std::vector<float> sides[2];
        for (std::uint32_t cycle = 0; cycle < 2; ++cycle)
        {
            std::vector<Timed> const blasts = StepsWithTag(d, Drill::kStratTagBarrage, cycle);
            CHECK(blasts.size() == Drill::kBarrageBlasts);
            int left = 0;
            for (std::size_t i = 0; i < blasts.size(); ++i)
            {
                Timed const& t = blasts[i];
                CHECK(ShapeOf(t.line) == "line" && Size(t.line) == Drill::kHallLength);
                CHECK(Key(t.line, "width", 0.f) == Drill::kHalfWidth);
                CHECK(Is(t.line, "tele", "0"));
                CHECK(Key(t.line, "pct", 0.f) == 30.f && Is(t.line, "school", "fire") && Is(t.line, "weak", "1"));
                CHECK(t.startMs == 25000);
                CHECK(t.resolveMs == 25000 + Drill::kBarrageCallMs + static_cast<std::uint32_t>(i)
                    * Drill::kBarrageGapMs);
                CHECK(Is(t.line, "cycle", cycle == 0 ? "even" : "odd"));
                float ahead = 0.f;
                float side = 0.f;
                CHECK(Shift(t.line, ahead, side));
                CHECK_NEAR(ahead, -Drill::kHallLength / 2.f, kEps);
                CHECK_NEAR(std::fabs(side), Drill::kHalfWidth / 2.f, kEps);
                if (side > 0.f)
                    ++left;
                sides[cycle].push_back(side);
            }
            CHECK(left == 2 || left == 3);
            // The order changes half at least once (a call to dance, not one half held all along).
            bool changes = false;
            for (std::size_t i = 1; i < sides[cycle].size(); ++i)
                changes = changes || sides[cycle][i] != sides[cycle][i - 1];
            CHECK(changes);
        }
        // The two parities mirror each other, so neither half is the safe one across cycles.
        CHECK(sides[0].size() == sides[1].size());
        for (std::size_t i = 0; i < sides[0].size() && i < sides[1].size(); ++i)
            CHECK_NEAR(sides[0][i], -sides[1][i], kEps);

        // The halves tile the 58 x 20 hall centered on the owner: a point is in exactly one of them (off the axis).
        for (float facing : { 0.f, 3.761f })
        {
            Vec3 const owner{ 10.f, 20.f, 0.f };
            BossMechanics::Shape const half = BossMechanics::Shape::Line(Drill::kHallLength, Drill::kHalfWidth);
            auto source = [&owner, facing](float side)
            {
                Vec3 const s = Drill::Shifted(owner, facing, -Drill::kHallLength / 2.f, side);
                return Geo::Pose{ s.x, s.y, s.z, facing };
            };
            Geo::Pose const left = source(Drill::kHalfWidth / 2.f);
            Geo::Pose const right = source(-Drill::kHalfWidth / 2.f);
            for (float ahead : { -28.f, -10.f, 0.f, 15.f, 28.f })
                for (float lateral : { -9.5f, -4.f, -0.5f, 0.5f, 4.f, 9.5f })
                {
                    Vec3 const p = Drill::Shifted(owner, facing, ahead, lateral);
                    bool const inLeft = Geo::Contains(half, left, p);
                    bool const inRight = Geo::Contains(half, right, p);
                    CHECK(inLeft == (lateral > 0.f) && inRight == (lateral < 0.f));
                }
            // Outside the hall's width (the walls in the room) is in neither.
            CHECK(!Geo::Contains(half, left, Drill::Shifted(owner, facing, 0.f, 10.5f)));
            CHECK(!Geo::Contains(half, right, Drill::Shifted(owner, facing, 0.f, -10.5f)));
        }
        // 1.5 s is enough to cross from one half to the other (from 1 yd in, to 1 yd in): 2 yd plus margins.
        CHECK((2.f + 2.f * kMarginYd) / Drill::kPlayerRunYdS * 1000.f < static_cast<float>(Drill::kBarrageGapMs));
    }

    void TestSweepProgram()
    {
        std::string err;
        for (char const* name : { "stratholme-followups:3", "stratholme-followups" })
        {
            std::optional<Drill::Program> const p = Drill::ParseProgram(name, err);
            CHECK(p && err.empty());
            if (!p || p->entries.size() != 1)
            {
                CHECK(false);
                continue;
            }
            Drill::ProgramEntry const& e = p->entries[0];
            CHECK(e.kind == Drill::EntryKind::Drill && e.drill == Followups());
            CHECK(e.cycles == 3 && e.lengthMs == 120000);
            std::vector<Drill::ScheduledStep> const s = Drill::StepSchedule(*e.drill, e.cycles);
            // The Barrage runs one of its two orders per cycle: 5 of its 10 steps.
            CHECK(s.size() == 3 * (e.drill->steps.size() - Drill::kBarrageBlasts));
            for (Drill::ScheduledStep const& st : s)
                CHECK(st.delayMs + Drill::WarnMs(e.drill->steps[st.step].line) <= 120000u);
        }
    }
}

int main()
{
    TestTable();
    TestCrossingRanks();
    TestSmashAftershock();
    TestWailEcho();
    TestTrackingCones();
    TestAllBut();
    TestHiddenBarrage();
    TestSweepProgram();
    return Report("strat_followups_test");
}
