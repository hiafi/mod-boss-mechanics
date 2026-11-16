// Standalone tests for the Blackrock Spire follow-ups of src/MechanicDrill.h (Blackrock Spire build §3.9, WP-G1): the
// `blackrock-spire-followups` drill (wave 1 rows 0-12) against the build spec rows it reproduces (Omokk OM2 heroic,
// Gizrul and Halycon HG2 + HG3 heroic, the Smolderthorn Champion's WT11 haul, Voone's VO11 chains and VO12 recall),
// the keys it adds (`at=last:<n>`, `to=last[:<n>]`: LastIndex, LineEnds and which steps record a line end), the
// geometry it relies on (pounces at the charges' ends, hauls and recall lanes along the chains) and the sweep's
// program. Build: tests/run.sh.

#include "check.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include "MechanicGeometry.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
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
    // BossMechanics.Bots.SafetyMarginYd default (M2).
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

    // One step of a cycle: its index, line, when it registers and when it resolves (ms after the cycle start).
    struct Timed
    {
        std::size_t index = 0;
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
                out.push_back({ i, d.steps[i].line, *at[i], *at[i] + Drill::WarnMs(d.steps[i].line) });
        return out;
    }

    Drill::Def const* Followups()
    {
        return Drill::Find("blackrock-spire-followups");
    }

    // The runtime's rule (MechanicCommand.cpp RegisterRepeat): a line with no unit anchor left after registration
    // records its far end: a point line, or a `to=` line frozen by fix=1 / reach=1. A line aimed with to=last records
    // none; a unit-anchored line that is not frozen (anchor=target with a live to=) records none either.
    bool RecordsEnd(std::string_view line)
    {
        std::string_view const shape = ShapeOf(line);
        if (shape != "line" && shape != "projectile")
            return false;
        std::optional<std::string_view> const to = Drill::Value(line, "to");
        if (to && Drill::LastIndex(*to))
            return false;
        bool const frozen = Is(line, "fix", "1") || Is(line, "reach", "1");
        if (to)
            return frozen;
        std::optional<std::string_view> const anchor = Drill::Value(line, "anchor");
        return !anchor || *anchor == "point";
    }

    // Which recorded line (a step index) each `at=last[:n]` and `to=last[:n]` step of cycle `cycle` reads, walking the
    // schedule in its order (StepSchedule: time order, list order on ties) with the runtime's LineEnds.
    std::map<std::size_t, std::optional<std::size_t>> LastReads(Drill::Def const& d, std::uint32_t cycle)
    {
        std::map<std::size_t, std::optional<std::size_t>> out;
        Drill::LineEnds<std::size_t> ends;
        for (Drill::ScheduledStep const& s : Drill::StepSchedule(d, cycle + 1))
        {
            if (s.cycle != cycle)
                continue;
            std::string_view const line = d.steps[s.step].line;
            for (char const* key : { "at", "to" })
                if (std::optional<std::string_view> v = Drill::Value(line, key))
                    if (std::optional<std::uint32_t> back = Drill::LastIndex(*v))
                        out[s.step] = ends.Back(*back);
            if (RecordsEnd(line))
                ends.Push(s.step);
        }
        return out;
    }

    // ---- the keys --------------------------------------------------------------------------------------------------
    void TestLastIndex()
    {
        CHECK(Drill::LastIndex("last") == std::optional<std::uint32_t>(0));
        CHECK(Drill::LastIndex("last:0") == std::optional<std::uint32_t>(0));
        CHECK(Drill::LastIndex("last:1") == std::optional<std::uint32_t>(1));
        CHECK(Drill::LastIndex("last:7") == std::optional<std::uint32_t>(Drill::kMaxLineEnds - 1));
        for (char const* bad : { "last:8", "last:10", "last:", "last:-1", "last:1.5", "last:x", "lastt", "last1",
            "member", "target", "me", "", "Last" })
            CHECK(!Drill::LastIndex(bad));
    }

    void TestLineEnds()
    {
        Drill::LineEnds<int> ends;
        CHECK(ends.Size() == 0 && !ends.Back(0));
        ends.Push(1);
        CHECK(ends.Back(0) == std::optional<int>(1) && !ends.Back(1));
        for (int i = 2; i <= 10; ++i)
            ends.Push(i);
        // The oldest ends drop out past kMaxLineEnds.
        CHECK(ends.Size() == Drill::kMaxLineEnds);
        CHECK(ends.Back(0) == std::optional<int>(10));
        CHECK(ends.Back(1) == std::optional<int>(9));
        CHECK(ends.Back(Drill::kMaxLineEnds - 1) == std::optional<int>(3));
        CHECK(!ends.Back(Drill::kMaxLineEnds));
        // Every index the parser accepts is one the history can still hold.
        CHECK(Drill::LastIndex("last:7").value_or(99) < Drill::kMaxLineEnds);
    }

    void TestRecordsEnd()
    {
        CHECK(RecordsEnd("line 30 2000 width=4"));
        CHECK(RecordsEnd("line 30 2000 at=group away=20 to=member fix=1"));
        CHECK(RecordsEnd("line 30 2000 anchor=target to=member reach=1"));
        CHECK(!RecordsEnd("line 30 2000 anchor=target to=member"));
        CHECK(!RecordsEnd("line 30 2000 anchor=target"));
        CHECK(!RecordsEnd("line 5 1500 anchor=target to=last:1 reach=1"));
        CHECK(!RecordsEnd("line 5 1500 anchor=target to=last reach=1"));
        CHECK(!RecordsEnd("circle 8 1500 at=last"));
    }

    // ---- the drill table -------------------------------------------------------------------------------------------
    void TestTable()
    {
        Drill::Def const* found = Followups();
        CHECK(found != nullptr);
        if (!found)
            return;
        Drill::Def const& d = *found;
        // WP-G2 grew the cycle to 40 s (wave 1's rows, then one wave 2 row: brs_upper_followups_test), three a run.
        CHECK(d.cycleMs == 40000 && d.defaultCycles == 3 && !d.ownerAtCreature);
        std::set<std::uint32_t> tags;
        for (Drill::Step const& s : d.steps)
        {
            std::uint32_t const tag = Tag(s.line);
            CHECK(tag >= Drill::kBrsTagHaymaker && tag <= Drill::kBrsTagThunderclap);
            if (tag > Drill::kBrsTagRecall)
            {
                // Wave 2 rows start after wave 1's last row has landed (21.5 s).
                CHECK(s.atMs >= Drill::kBrsWave2Ms);
                continue;
            }
            CHECK(s.atMs < Drill::kBrsWave2Ms);
            tags.insert(tag);
            // Every step deals damage (the runner appends dmg=), names its heroic percent and its timing keys parse.
            CHECK(Drill::DamageStep(s.line));
            CHECK(Key(s.line, "pct", 0.f) >= 10.f);
            CHECK(Drill::PlanKeysOk(s.line));
            CHECK(!Drill::Value(s.line, "dmg"));
        }
        CHECK(tags.size() == 7);
        // The bar's tags are the follow-ups: each a 1.5 s warning but the pounce, drawn with its charge (B7's HG3)
        // and landing 1.5 s after it; the first shapes are reported only, and warn 2 s+.
        for (std::uint32_t tag = Drill::kBrsTagHaymaker; tag <= Drill::kBrsTagRecall; ++tag)
        {
            bool const bar = tag == Drill::kBrsTagOneTwo || tag == Drill::kBrsTagPounce || tag == Drill::kBrsTagHaul
                || tag == Drill::kBrsTagRecall;
            CHECK(Drill::BrsBarTag(tag) == bar);
            for (std::uint32_t cycle : { 0u, 1u })
                for (Timed const& t : StepsWithTag(d, tag, cycle))
                {
                    if (tag == Drill::kBrsTagPounce)
                        CHECK(t.resolveMs - t.startMs == Drill::kPounceWarnMs);
                    else if (bar)
                        CHECK(t.resolveMs - t.startMs == Drill::kBrsFollowupMs);
                    else
                        CHECK(t.resolveMs - t.startMs >= 2000u);
                }
        }
        CHECK(!Drill::BrsBarTag(0) && !Drill::BrsBarTag(8) && !Drill::BrsBarTag(20));
        // Every row is over before the next row starts (6 s apart) and the cycle before the next cycle.
        for (std::uint32_t cycle : { 0u, 1u })
        {
            std::vector<std::optional<std::uint32_t>> const at = Drill::StepOffsets(d, cycle);
            for (std::size_t i = 0; i < d.steps.size(); ++i)
            {
                if (!at[i] || Tag(d.steps[i].line) > Drill::kBrsTagRecall)
                    continue;
                std::uint32_t const end = *at[i] + Drill::WarnMs(d.steps[i].line)
                    + static_cast<std::uint32_t>(Key(d.steps[i].line, "persist", 0.f));
                // Rows 0, 6 and 12 s; row 12's chains run 8 s and their follow-ups after them.
                std::uint32_t const row = std::min<std::uint32_t>(*at[i] / 6000, 2);
                if (row < 2)
                    CHECK(end <= (row + 1) * 6000);
                CHECK(end <= Drill::kBrsWave2Ms);
            }
        }
    }

    // ---- row 0: Omokk OM2 heroic, the one-two ----------------------------------------------------------------------
    void TestOneTwo()
    {
        Drill::Def const& d = *Followups();
        std::vector<Timed> const first = StepsWithTag(d, Drill::kBrsTagHaymaker);
        std::vector<Timed> const second = StepsWithTag(d, Drill::kBrsTagOneTwo);
        CHECK(first.size() == 1 && second.size() == 1);
        if (first.size() != 1 || second.size() != 1)
            return;
        for (Timed const& t : { first[0], second[0] })
        {
            // 5 x 30 yd from Omokk (the creature) toward a spot, frozen at registration (the aim recorded at the cast).
            CHECK(ShapeOf(t.line) == "line" && Size(t.line) == Drill::kHaymakerLength);
            CHECK(Key(t.line, "width", 0.f) == Drill::kHaymakerWidth);
            CHECK(Is(t.line, "anchor", "target") && Is(t.line, "to", "member") && Is(t.line, "fix", "1"));
            CHECK(!Drill::Value(t.line, "reach"));
            // Heroic 50%, no Weakened (OM2: "none").
            CHECK(Key(t.line, "pct", 0.f) == 50.f && !Drill::Value(t.line, "weak"));
        }
        CHECK(first[0].startMs == 0 && first[0].resolveMs == 2500);
        CHECK(Is(first[0].line, "pick", "nontank"));
        // The second line: the same player (pick=same), registered as the first lands, so it aims at the new spot.
        CHECK(Is(second[0].line, "pick", "same") && Is(second[0].line, "after", "resolve"));
        CHECK(second[0].startMs == first[0].resolveMs);
        CHECK(second[0].resolveMs == first[0].resolveMs + Drill::kBrsFollowupMs);
        // Nothing between the two picks could move pick=same off the first one's target.
        for (Drill::Step const& s : d.steps)
            if (s.atMs > 0 && s.atMs < second[0].startMs)
                CHECK(!Drill::Value(s.line, "pick"));

        // Geometry: a player 20 yd out sidesteps the first line (2.5 yd half width plus the margin) and stands in the
        // second, re-aimed at the new spot; 1.5 s covers another sidestep.
        Vec3 const omokk{ 0.f, 0.f, 0.f };
        Vec3 const spot{ 20.f, 0.f, 0.f };
        Drill::FixedLine const l1 = Drill::FixLine(omokk, spot, Drill::kHaymakerLength, false, 0.f);
        Shape const line = Shape::Line(Drill::kHaymakerLength, Drill::kHaymakerWidth);
        Geo::Pose const p1{ l1.source.x, l1.source.y, l1.source.z, l1.facing };
        CHECK(Geo::Contains(line, p1, spot));
        Vec3 const moved{ 20.f, Drill::kHaymakerWidth / 2.f + kMarginYd, 0.f };
        CHECK(!Geo::Contains(line, p1, moved));
        Drill::FixedLine const l2 = Drill::FixLine(omokk, moved, Drill::kHaymakerLength, false, 0.f);
        Geo::Pose const p2{ l2.source.x, l2.source.y, l2.source.z, l2.facing };
        CHECK(Geo::Contains(line, p2, moved));
        CHECK_NEAR(l2.length, Drill::kHaymakerLength, kEps);
        CHECK((Drill::kHaymakerWidth / 2.f + 2.f * kMarginYd) / Drill::kPlayerRunYdS * 1000.f
            < static_cast<float>(Drill::kBrsFollowupMs));
    }

    // ---- row 6: Gizrul and Halycon HG2 + HG3 heroic, two charges and their pounces --------------------------------
    void TestChargesPounces()
    {
        Drill::Def const& d = *Followups();
        std::vector<Timed> const charges = StepsWithTag(d, Drill::kBrsTagCharge);
        std::vector<Timed> const pounces = StepsWithTag(d, Drill::kBrsTagPounce);
        CHECK(charges.size() == 2 && pounces.size() == 2);
        if (charges.size() != 2 || pounces.size() != 2)
            return;
        for (Timed const& t : charges)
        {
            CHECK(ShapeOf(t.line) == "line" && Size(t.line) == Drill::kHaymakerLength);
            CHECK(Key(t.line, "width", 0.f) == Drill::kHaymakerWidth);
            // Through the farthest non-tanks: min= prefers members that far, else the farthest; reach=1 so a member
            // beyond 30 yd is still reached (the line is frozen through the spot, at least 30 yd).
            CHECK(Is(t.line, "anchor", "target") && Is(t.line, "to", "member") && Is(t.line, "reach", "1"));
            CHECK(Key(t.line, "min", 0.f) == Drill::kChargeMinYd);
            CHECK(t.resolveMs - t.startMs == 3000);
            CHECK(Key(t.line, "pct", 0.f) == 40.f && Is(t.line, "weak", "1"));
        }
        CHECK(charges[0].startMs == 6000);
        CHECK(charges[1].startMs == charges[0].startMs + Drill::kChargeGapMs);
        // Two different players: the second is pick=other (a non-tank the chain has not picked).
        CHECK(Is(charges[0].line, "pick", "nontank") && Is(charges[1].line, "pick", "other"));
        for (std::size_t i = 0; i < 2; ++i)
        {
            Timed const& p = pounces[i];
            CHECK(ShapeOf(p.line) == "circle" && Size(p.line) == Drill::kPounceRadius);
            CHECK(Key(p.line, "pct", 0.f) == 30.f && Is(p.line, "weak", "1"));
            // Shown with its own charge line (B7 builds SP's HG3: the circle appears with the line), landing 1.5 s
            // after that charge: a 4.5 s warning.
            CHECK(p.startMs == charges[i].startMs);
            CHECK(p.resolveMs == charges[i].resolveMs + Drill::kBrsFollowupMs);
            CHECK(p.resolveMs - p.startMs == Drill::kPounceWarnMs && Drill::kPounceWarnMs == 4500u);
            CHECK(p.index == charges[i].index + 1);     // right after its charge in list order (ties run in order)
            CHECK(Is(p.line, "at", "last"));
        }
        // The first pounce lands after the second charge is drawn, and still reads the first charge's end.
        CHECK(pounces[0].resolveMs > charges[1].startMs);
        for (std::uint32_t cycle : { 0u, 1u, 2u })
        {
            std::map<std::size_t, std::optional<std::size_t>> const reads = LastReads(d, cycle);
            auto const a = reads.find(pounces[0].index);
            auto const b = reads.find(pounces[1].index);
            CHECK(a != reads.end() && b != reads.end());
            if (a == reads.end() || b == reads.end())
                continue;
            CHECK(a->second == std::optional<std::size_t>(charges[0].index));
            CHECK(b->second == std::optional<std::size_t>(charges[1].index));
        }

        // Geometry: the pounce sits on the line's far end. A target 12 yd out: the end is 18 yd past it and the
        // circle spares its spot; a target 40 yd out: the line reaches it and the circle sits on it.
        Vec3 const gizrul{ 0.f, 0.f, 0.f };
        for (float range : { 12.f, 25.f, 40.f })
        {
            Vec3 const spot{ range * std::cos(0.7f), range * std::sin(0.7f), 0.f };
            Drill::FixedLine const l = Drill::FixLine(gizrul, spot, Drill::kHaymakerLength, true, 0.f);
            CHECK_NEAR(l.length, std::max(Drill::kHaymakerLength, range), kEps);
            Vec3 const end = Drill::LineEnd(l.source, l.facing, l.length);
            Geo::Pose const centre{ end.x, end.y, end.z, 0.f };
            Shape const pounce = Shape::Circle(Drill::kPounceRadius);
            CHECK(Geo::Contains(pounce, centre, end));
            CHECK(Geo::Contains(pounce, centre, spot) == (Drill::kHaymakerLength - range < Drill::kPounceRadius));
        }
        // From the end, 1.5 s is enough to clear the circle (the charge's own 1.5 s gap; the circle is up 4.5 s).
        CHECK((Drill::kPounceRadius + kMarginYd) / Drill::kPlayerRunYdS * 1000.f
            < static_cast<float>(Drill::kBrsFollowupMs));
    }

    // ---- row 12: the axe chains (WT11, VO11), then the haul (WT11) or the recall (VO12 -> VO7) ---------------------
    void TestChainsHaulRecall()
    {
        Drill::Def const& d = *Followups();
        std::vector<Timed> const chains = StepsWithTag(d, Drill::kBrsTagChain);
        CHECK(chains.size() == 2);
        if (chains.size() != 2)
            return;
        for (Timed const& t : chains)
        {
            CHECK(t.startMs == 12000);
            // A 2 yd chain from the creature to a non-tank's spot (the stuck axe), frozen there.
            CHECK(ShapeOf(t.line) == "line" && Key(t.line, "width", 0.f) == Drill::kChainWidth);
            CHECK(Is(t.line, "anchor", "target") && Is(t.line, "to", "member") && Is(t.line, "reach", "1"));
            // 8 s in all, a crossing 10% every 2 s, Weakened.
            CHECK((t.resolveMs - t.startMs) + static_cast<std::uint32_t>(Key(t.line, "persist", 0.f))
                == Drill::kChainLifeMs);
            CHECK(Key(t.line, "tick", 0.f) == static_cast<float>(Drill::kChainTickMs));
            CHECK(Key(t.line, "pct", 0.f) == 10.f && Is(t.line, "weak", "1"));
        }
        CHECK(Is(chains[0].line, "pick", "nontank") && Is(chains[1].line, "pick", "other"));
        // Even cycles haul, odd cycles recall; two lines each, one per chain.
        for (std::uint32_t cycle : { 0u, 1u, 2u, 3u, 4u })
        {
            bool const even = cycle % 2 == 0;
            std::vector<Timed> const hauls = StepsWithTag(d, Drill::kBrsTagHaul, cycle);
            std::vector<Timed> const recalls = StepsWithTag(d, Drill::kBrsTagRecall, cycle);
            CHECK(hauls.size() == (even ? 2u : 0u) && recalls.size() == (even ? 0u : 2u));
            std::vector<Timed> const& along = even ? hauls : recalls;
            if (along.size() != 2)
                continue;
            for (Timed const& t : along)
            {
                // The taut warning starts as the chains end; the line lands 1.5 s later.
                CHECK(t.startMs == chains[0].startMs + Drill::kChainLifeMs);
                CHECK(t.resolveMs == t.startMs + Drill::kBrsFollowupMs);
                CHECK(ShapeOf(t.line) == "line" && Is(t.line, "anchor", "target") && Is(t.line, "reach", "1"));
                CHECK(Is(t.line, "weak", "1"));
                if (even)
                {
                    CHECK(Key(t.line, "width", 0.f) == Drill::kHaulWidth && Key(t.line, "pct", 0.f) == 35.f);
                    CHECK(!Drill::Value(t.line, "exclude"));
                }
                else
                {
                    CHECK(Key(t.line, "width", 0.f) == Drill::kRecallWidth && Key(t.line, "pct", 0.f) == 40.f);
                    CHECK(Key(t.line, "exclude", 0.f) == Drill::kRecallExclude);
                }
            }
            CHECK(Is(along[0].line, "to", "last:1") && Is(along[1].line, "to", "last"));
            // Each reads its own chain: the first chain is the line before the last, and a to=last line records none.
            std::map<std::size_t, std::optional<std::size_t>> const reads = LastReads(d, cycle);
            auto const a = reads.find(along[0].index);
            auto const b = reads.find(along[1].index);
            CHECK(a != reads.end() && b != reads.end());
            if (a == reads.end() || b == reads.end())
                continue;
            CHECK(a->second == std::optional<std::size_t>(chains[0].index));
            CHECK(b->second == std::optional<std::size_t>(chains[1].index));
        }

        // Geometry: the haul and the recall cover the chain they run along (wider, same ends); the recall spares the
        // creature's 5 yd (the tank), the haul does not; a player 3 yd off the chain is clear of both.
        Vec3 const boss{ 5.f, -3.f, 0.f };
        for (float bearing : { 0.f, 2.1f, -2.6f })
        {
            for (float range : { 4.f, 12.f, 25.f })
            {
                Vec3 const axe{ boss.x + range * std::cos(bearing), boss.y + range * std::sin(bearing), 0.f };
                Drill::FixedLine const chainLine = Drill::FixLine(boss, axe, 5.f, true, 0.f);
                Geo::Pose const pose{ boss.x, boss.y, boss.z, chainLine.facing };
                Shape const chain = Shape::Line(chainLine.length, Drill::kChainWidth);
                Shape const haul = Shape::Line(chainLine.length, Drill::kHaulWidth);
                Shape recall = Shape::Line(chainLine.length, Drill::kRecallWidth);
                recall.excludeStart = Drill::kRecallExclude;
                for (float s : { 0.5f, 3.f, 6.f, range - 0.5f })
                {
                    if (s > chainLine.length)
                        continue;
                    Vec3 const on = Drill::Shifted(boss, chainLine.facing, s, 0.4f);
                    CHECK(Geo::Contains(chain, pose, on));
                    CHECK(Geo::Contains(haul, pose, on));
                    CHECK(Geo::Contains(recall, pose, on) == (s >= Drill::kRecallExclude));
                    Vec3 const off = Drill::Shifted(boss, chainLine.facing, s, 3.f);
                    CHECK(!Geo::Contains(haul, pose, off) && !Geo::Contains(recall, pose, off));
                }
                // reach=1: the line ends on the axe (at least 5 yd long).
                CHECK_NEAR(chainLine.length, std::max(5.f, range), kEps);
            }
        }
        // From the middle of the recall lane (2 yd to its edge) a bot clears it in 1.5 s with its margin.
        CHECK((Drill::kRecallWidth / 2.f + kMarginYd) / Drill::kPlayerRunYdS * 1000.f
            < static_cast<float>(Drill::kBrsFollowupMs));
    }

    void TestSweepProgram()
    {
        std::string err;
        for (char const* name : { "blackrock-spire-followups:3", "blackrock-spire-followups" })
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
            // Each cycle runs either the two hauls or the two recall lanes (2 of wave 1's steps skipped) and one wave 2
            // row: every wave 1 row three times, each wave 2 row once.
            std::size_t wave1 = 0;
            std::size_t wave2 = 0;
            for (Drill::Step const& step : e.drill->steps)
                (Tag(step.line) > Drill::kBrsTagRecall ? wave2 : wave1) += 1;
            CHECK(s.size() == 3 * (wave1 - 2) + wave2);
            for (Drill::ScheduledStep const& st : s)
            {
                std::string_view const line = e.drill->steps[st.step].line;
                CHECK(static_cast<float>(st.delayMs + Drill::WarnMs(line)) + Key(line, "persist", 0.f)
                    + Drill::FrontRollMs(line) <= 120000.f);
            }
        }
    }
}

int main()
{
    TestLastIndex();
    TestLineEnds();
    TestRecordsEnd();
    TestTable();
    TestOneTwo();
    TestChargesPounces();
    TestChainsHaulRecall();
    TestSweepProgram();
    return Report("brs_followups_test");
}
