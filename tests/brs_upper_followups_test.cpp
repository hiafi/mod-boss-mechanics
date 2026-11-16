// Standalone tests for the Upper Blackrock Spire rows of `blackrock-spire-followups` (Blackrock Spire build §3.9 rows
// 18-30, WP-G2; src/MechanicDrill.h "Blackrock Spire follow-ups, wave 2"): the rotation of the wave 2 rows over the
// 40 s cycle (`cycle=<k>/3`), each row against the boss it reproduces as built (B10 Pyroguard Emberseer EM2 + EM3
// heroic, B12 Rend RG6 heroic, B15 Drakkisath DR17 + DR18), the keys the rows use (next=, edge=, inside=, lethal=,
// only=member, a cone's fix=1), the geometry they rely on (the rings' slices and turn hints, the gaps in Rend's
// strip, a way out of every blow inside the challenger's ring) and a brain-model run of the Challenge (one bot kept
// inside a lethal ring by its Within bounds while six blows land on it; Blackrock Spire F-3: also a pet owner, and on
// the live timing) and of row 24 (Rend's two walls, F-3, on both timings). Build: tests/run.sh.

#include "check.h"
#include "brain_model.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include "MechanicFront.h"
#include "MechanicSolver.h"
#include "MechanicViewRules.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace BossMechanics;
using Geo::Pose;
using Geo::Vec3;
using Solver::Hazard;
using Ms = std::chrono::milliseconds;

namespace
{
    constexpr float kEps = 1e-3f;
    constexpr float kMargin = 0.75f;            // BossMechanics.Bots.SafetyMarginYd
    constexpr float kDeg = Geo::kPi / 180.f;

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

    struct Timed
    {
        std::size_t index = 0;
        std::string_view line;
        std::uint32_t startMs = 0;
        std::uint32_t resolveMs = 0;
    };

    Drill::Def const& Followups()
    {
        static Drill::Def const empty;
        Drill::Def const* d = Drill::Find("blackrock-spire-followups");
        return d ? *d : empty;
    }

    // The steps of cycle `cycle` with a tag in [lo, hi], in list order.
    std::vector<Timed> Steps(std::uint32_t cycle, std::uint32_t lo, std::uint32_t hi)
    {
        Drill::Def const& d = Followups();
        std::vector<std::optional<std::uint32_t>> const at = Drill::StepOffsets(d, cycle);
        std::vector<Timed> out;
        for (std::size_t i = 0; i < d.steps.size(); ++i)
        {
            std::uint32_t const tag = Tag(d.steps[i].line);
            if (at[i] && tag >= lo && tag <= hi)
                out.push_back({ i, d.steps[i].line, *at[i], *at[i] + Drill::WarnMs(d.steps[i].line) });
        }
        return out;
    }

    // When a step's mechanic is over (ms after the cycle start): the resolve, a persistent zone's life, a front's roll.
    float EndMs(Timed const& t)
    {
        return static_cast<float>(t.resolveMs) + Key(t.line, "persist", 0.f) + Drill::FrontRollMs(t.line);
    }

    // ---- the rotation ----------------------------------------------------------------------------------------------
    void TestRotation()
    {
        Drill::Def const& d = Followups();
        CHECK(d.cycleMs == 40000 && d.defaultCycles == 3);
        CHECK(Drill::kBrsWave2Rows == 3);
        // Tags 8-19 are wave 2's; each row runs in its own third of the cycles, never beside another wave 2 row.
        std::uint32_t const rowOf[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 2, 2, 2, 2, 2 };
        std::set<std::uint32_t> seen;
        for (Drill::Step const& s : d.steps)
        {
            std::uint32_t const tag = Tag(s.line);
            if (tag <= Drill::kBrsTagRecall)
            {
                CHECK(!Drill::Value(s.line, "cycle") || Is(s.line, "cycle", "even") || Is(s.line, "cycle", "odd"));
                continue;
            }
            CHECK(tag <= Drill::kBrsTagThunderclap);
            if (tag > Drill::kBrsTagThunderclap)
                continue;
            seen.insert(tag);
            std::optional<Drill::CycleRule> const rule = Drill::CycleOf(s.line);
            CHECK(rule && rule->every == Drill::kBrsWave2Rows && rule->at == rowOf[tag]);
            CHECK(s.atMs >= Drill::kBrsWave2Ms);
        }
        CHECK(seen.size() == 12);
        for (std::uint32_t cycle = 0; cycle < 6; ++cycle)
        {
            std::vector<Timed> const w1 = Steps(cycle, Drill::kBrsTagHaymaker, Drill::kBrsTagRecall);
            std::vector<Timed> const w2 = Steps(cycle, Drill::kBrsTagNova, Drill::kBrsTagThunderclap);
            CHECK(!w1.empty() && !w2.empty());
            float w1End = 0.f;
            for (Timed const& t : w1)
                w1End = std::max(w1End, EndMs(t));
            // Wave 1's last row (the haul or the recall) has landed before the wave 2 row starts.
            CHECK(w1End <= static_cast<float>(Drill::kBrsWave2Ms));
            for (Timed const& t : w2)
            {
                CHECK(rowOf[Tag(t.line)] == cycle % 3);
                CHECK(t.startMs >= Drill::kBrsWave2Ms);
                // Over (the fronts rolled, the ring gone) before the next cycle's one-two.
                CHECK(EndMs(t) <= static_cast<float>(d.cycleMs));
            }
        }
        // The bar: the follow-ups of each row (rings 2-4, the second wall) and the Challenge (the ring and every
        // blow) are gated; the nova, ring 1, the first wall and the Call are reported.
        for (std::uint32_t tag = Drill::kBrsTagNova; tag <= Drill::kBrsTagThunderclap; ++tag)
        {
            bool const gated = tag == Drill::kBrsTagRingNext || tag == Drill::kBrsTagWallSecond
                || tag >= Drill::kBrsTagRing;
            CHECK(Drill::BrsBarTag(tag) == gated);
        }
        std::size_t bar = 0;
        for (std::uint32_t t : Drill::kBrsBarTags)
        {
            CHECK(t >= 1 && t <= Drill::kBrsTagThunderclap);
            ++bar;
        }
        CHECK(bar == 12);
        // Every wave 2 step parses its plan keys and names its percent; only the Call deals no damage.
        for (Timed const& t : Steps(0, 8, 19))
            CHECK(Drill::PlanKeysOk(t.line));
        for (std::uint32_t cycle : { 0u, 1u, 2u })
            for (Timed const& t : Steps(cycle, Drill::kBrsTagNova, Drill::kBrsTagThunderclap))
            {
                CHECK(Drill::PlanKeysOk(t.line) && !Drill::Value(t.line, "dmg"));
                bool const call = Tag(t.line) == Drill::kBrsTagCall;
                CHECK(Drill::DamageStep(t.line) == !call);
                if (!call)
                    CHECK(Key(t.line, "pct", 0.f) >= 25.f);
            }
    }

    // ---- row 18: Emberseer EM2 + EM3 heroic at 16 stacks -----------------------------------------------------------
    void TestEmberseer()
    {
        std::vector<Timed> const nova = Steps(0, Drill::kBrsTagNova, Drill::kBrsTagNova);
        std::vector<Timed> const first = Steps(0, Drill::kBrsTagRingFirst, Drill::kBrsTagRingFirst);
        std::vector<Timed> const next = Steps(0, Drill::kBrsTagRingNext, Drill::kBrsTagRingNext);
        CHECK(nova.size() == 1 && first.size() == 1 && next.size() == Drill::kEmberRings - 1);
        if (nova.size() != 1 || first.size() != 1 || next.size() != Drill::kEmberRings - 1)
            return;
        // B10's numbers: the nova 7 + 0.3 a stack, every hit 30% x (1 + 4% a stack).
        CHECK_NEAR(Drill::kEmberNovaYd, 7.f + 0.3f * static_cast<float>(Drill::kEmberStacks), kEps);
        CHECK_NEAR(Drill::kEmberPct, 30.f * (1.f + 0.04f * static_cast<float>(Drill::kEmberStacks)), 1e-2f);
        std::vector<Timed> rings = first;
        rings.insert(rings.end(), next.begin(), next.end());
        // The kit's PlanRings (firstWarnMs 5000, spacingMs 1500, revealAtPreviousLaunch): ring k launches at 5 s +
        // 1.5 s x k from the pulse start and is shown as ring k - 1 launches.
        std::uint32_t const t0 = first[0].startMs;
        CHECK(t0 == Drill::kBrsWave2Ms);
        for (std::size_t k = 0; k < rings.size(); ++k)
        {
            Timed const& r = rings[k];
            std::string_view const l = r.line;
            std::uint32_t const launch = t0 + Drill::kEmberRingWarnMs + static_cast<std::uint32_t>(k)
                * Drill::kEmberSpacingMs;
            CHECK(r.resolveMs == launch);
            CHECK(r.startMs == (k == 0 ? t0 : launch - Drill::kEmberSpacingMs));
            CHECK(ShapeOf(l) == "conefront" && Size(l) == Drill::kEmberEndYd);
            CHECK(Key(l, "slice", 0.f) == Drill::kEmberSliceDeg);
            // Ring 1 from the nova's edge, the later rings from the apex (ruling 2).
            CHECK_NEAR(Key(l, "start", -1.f), k == 0 ? Drill::kEmberNovaYd : 0.f, kEps);
            // Ring 1 comes before the nova, at the creature; the later rings pin the apex at the nova's centre
            // (at=prev), as the kit fixes it for the pulse: the parked tank leading him must not move it (R-2-FW m4).
            CHECK(k == 0 ? !Drill::Value(l, "at") : Is(l, "at", "prev"));
            // Ring 1 random, each later ring turned 60 degrees clockwise from the one before (turn=).
            CHECK(k == 0 ? !Drill::Value(l, "turn") && !Drill::Value(l, "bearing")
                : Key(l, "turn", 0.f) == Drill::kEmberTurnDeg);
            // The hint the kit sets: the turn to the next ring on every ring that has one (WP-SF).
            CHECK(Key(l, "next", 0.f) == (k + 1 < rings.size() ? Drill::kEmberTurnDeg : 0.f));
            // The tank parks him in the slice until the band passes nova + 12 yd; the hall as the bounds.
            CHECK(Is(l, "park", "1"));
            CHECK_NEAR(Key(l, "until", 0.f), Drill::kEmberNovaYd + Drill::kEmberSafeExtraYd, kEps);
            CHECK(Key(l, "inside", 0.f) == 29.f);
            CHECK(Key(l, "band", 4.f) == 4.f && Key(l, "speed", 8.f) == 8.f);
            CHECK(Is(l, "school", "fire") && Is(l, "weak", "1"));
            CHECK_NEAR(Key(l, "pct", 0.f), Drill::kEmberPct, kEps);
        }
        // The nova: registered 2.5 s into the pulse with its own 2.5 s warning, erupting as ring 1 launches.
        CHECK(ShapeOf(nova[0].line) == "circle");
        CHECK_NEAR(Size(nova[0].line), Drill::kEmberNovaYd, kEps);
        CHECK(nova[0].resolveMs - nova[0].startMs == Drill::kEmberNovaWarnMs);
        CHECK(nova[0].resolveMs == first[0].resolveMs);
        CHECK(Is(nova[0].line, "school", "fire") && Is(nova[0].line, "weak", "1"));
        CHECK(nova[0].index > first[0].index && nova[0].index < next[0].index);
        // Every ring after the first reads the slice of the ring before it (Dice::lastSlice): no other conefront in
        // between, in this cycle.
        for (Timed const& t : Steps(0, 1, 19))
            if (ShapeOf(t.line) == "conefront")
                CHECK(Tag(t.line) == Drill::kBrsTagRingFirst || Tag(t.line) == Drill::kBrsTagRingNext);
        // The slices: ring k at k x 60 degrees clockwise from ring 1's; the hint on ring k is ring k + 1's turn.
        float const firstSlice = 0.7f;
        for (std::uint32_t k = 0; k + 1 < Drill::kEmberRings; ++k)
        {
            float const a = Front::SliceBearing(firstSlice, k, Drill::kEmberTurnDeg);
            float const b = Front::SliceBearing(firstSlice, k + 1, Drill::kEmberTurnDeg);
            CHECK_NEAR(Geo::WrapPi(a - b), Drill::kEmberTurnDeg * kDeg, 1e-3f);
            CHECK_NEAR(Front::SliceBearing(a, 1, Key(rings[k + 1].line, "turn", 0.f)), b, 1e-3f);
        }
        // Consecutive 45 degree slices turned 60 do not overlap (the 15 degree gap of B10's question 1).
        CHECK(Drill::kEmberTurnDeg > Drill::kEmberSliceDeg);
        // The last ring rolls out of the hall (45 yd) by 37.2 s.
        CHECK(EndMs(rings.back()) <= 37200.f);
    }

    // ---- row 24: Rend RG6 heroic -----------------------------------------------------------------------------------
    void TestRend()
    {
        std::vector<Timed> const wall = Steps(1, Drill::kBrsTagWall, Drill::kBrsTagWall);
        std::vector<Timed> const second = Steps(1, Drill::kBrsTagWallSecond, Drill::kBrsTagWallSecond);
        CHECK(wall.size() == 1 && second.size() == 1);
        if (wall.size() != 1 || second.size() != 1)
            return;
        for (Timed const& t : { wall[0], second[0] })
        {
            std::string_view const l = t.line;
            CHECK(ShapeOf(l) == "linefront" && Size(l) == Drill::kRendTravelYd);
            CHECK(Key(l, "width", 0.f) == Drill::kRendWidthYd && Key(l, "gap", 0.f) == Drill::kRendGapYd);
            CHECK(Key(l, "edge", 0.f) == Drill::kRendEdgeYd);
            CHECK(Key(l, "inside", 0.f) == Drill::kRendRoomYd);
            CHECK(Key(l, "band", 4.f) == 4.f && Key(l, "speed", 8.f) == 8.f);
            CHECK(Is(l, "school", "frost") && Key(l, "pct", 0.f) == 25.f && Is(l, "weak", "1"));
            // Both registered at the call (the kit's default: the second gap is seen from the start).
            CHECK(t.startMs == Drill::kBrsWave2Ms);
        }
        CHECK(wall[0].resolveMs - wall[0].startMs == Drill::kRendWarnMs);
        CHECK(second[0].resolveMs - wall[0].resolveMs == Drill::kRendSpacingMs);
        CHECK(Is(second[0].line, "side", "other") && Key(second[0].line, "apart", 0.f) == Drill::kRendApartYd);
        CHECK(!Drill::Value(wall[0].line, "side") && !Drill::Value(wall[0].line, "apart"));
        CHECK(second[0].index == wall[0].index + 1);
        // B12's numbers: edge 15 on the 68 yd wall is the kit's 40 yd span with its 1 yd edge.
        CHECK_NEAR(Drill::kRendEdgeYd, (Drill::kRendWidthYd - Drill::kRendSpanYd) / 2.f + 1.f, kEps);
        CHECK_NEAR(Drill::kRendRoomYd * 2.f, Drill::kRendSpanYd, kEps);
        // The gap placement the runtime does (Front::PlaceGap, as MechanicCommand.cpp calls it): the first gap
        // anywhere in the strip, the second across the centreline within 20 yd of it, both whole inside the strip.
        float const halfSpan = Drill::kRendSpanYd / 2.f - 1.f;
        for (int i = 0; i < 200; ++i)
        {
            float const u = static_cast<float>(i) / 200.f;
            std::optional<float> const g1 = Front::PlaceGap(Drill::kRendWidthYd, Drill::kRendGapYd, {}, u, 0,
                Drill::kRendEdgeYd);
            CHECK(g1.has_value());
            if (!g1)
                continue;
            CHECK(std::fabs(*g1) + Drill::kRendGapYd / 2.f <= halfSpan + kEps);
            for (float v : { 0.f, 0.37f, 0.99f })
            {
                int const side = -Front::SideOf(*g1);
                std::optional<float> const g2 = Front::PlaceGap(Drill::kRendWidthYd, Drill::kRendGapYd, {}, v, side,
                    Drill::kRendEdgeYd, *g1, Drill::kRendApartYd);
                CHECK(g2.has_value());
                if (!g2)
                    continue;
                CHECK(std::fabs(*g2) + Drill::kRendGapYd / 2.f <= halfSpan + kEps);
                CHECK(std::fabs(*g2 - *g1) <= Drill::kRendApartYd + kEps);
                if (side != 0)
                    CHECK(Front::SideOf(*g2) != Front::SideOf(*g1) || std::fabs(*g2) < kEps);
            }
        }
        // The edge key's own rule: a gap still fits (2 x edge + gap within the width).
        CHECK(2.f * Drill::kRendEdgeYd + Drill::kRendGapYd <= Drill::kRendWidthYd);
        // Each wall rolls 86 yd at 8 yd/s (10.75 s): the second is over at 38.75 s.
        CHECK_NEAR(Drill::FrontRollMs(wall[0].line), 10750.f, 1.f);
        CHECK_NEAR(EndMs(second[0]), 38750.f, 1.f);
    }

    // ---- row 30: Drakkisath DR17 + DR18, the Challenge -------------------------------------------------------------
    enum class Blow : std::uint8_t { Sweep, Cleave, FireBlast, RingOfFlame, Thunderclap };

    Blow BlowOf(std::uint32_t tag)
    {
        switch (tag)
        {
            case Drill::kBrsTagSweep:       return Blow::Sweep;
            case Drill::kBrsTagCleave:      return Blow::Cleave;
            case Drill::kBrsTagFireBlast:   return Blow::FireBlast;
            case Drill::kBrsTagRingOfFlame: return Blow::RingOfFlame;
            default:                        return Blow::Thunderclap;
        }
    }

    char const* BlowName(Blow b)
    {
        static char const* const names[] = { "sweep", "cleave", "fire-blast", "ring-of-flame", "thunderclap" };
        return names[static_cast<std::size_t>(b)];
    }

    void TestChallengeTable()
    {
        std::vector<Timed> const call = Steps(2, Drill::kBrsTagCall, Drill::kBrsTagCall);
        std::vector<Timed> const ring = Steps(2, Drill::kBrsTagRing, Drill::kBrsTagRing);
        std::vector<Timed> const blows = Steps(2, Drill::kBrsTagSweep, Drill::kBrsTagThunderclap);
        CHECK(call.size() == 1 && ring.size() == 1);
        if (call.size() != 1 || ring.size() != 1)
            return;
        // The Call: everyone to a 2 yd spot on a 7 yd ring round him, 4 s; the ring closes as it ends.
        CHECK(ShapeOf(call[0].line) == "spot" && Key(call[0].line, "ring", 0.f) == Drill::kCallRingYd);
        CHECK(call[0].startMs == Drill::kBrsWave2Ms && call[0].resolveMs - call[0].startMs == Drill::kCallMs);
        // Inside B15's 9.5 yd (RingCentre keeps the ring on him) and the ring's inner edge, spot radius included.
        CHECK(Drill::kCallRingYd + Size(call[0].line) <= 9.5f);
        // The ring: a donut 11-13 yd on him, armed 1.5 s after the close, 10.5 s, lethal, the challenger only (a
        // non-tank: the first pick of the row), 50% for the kill.
        std::string_view const r = ring[0].line;
        CHECK(ShapeOf(r) == "donut");
        CHECK_NEAR(Size(r), Drill::kRingRadiusYd + Drill::kRingWidthYd / 2.f, kEps);
        CHECK_NEAR(Key(r, "inner", 0.f), Drill::kRingRadiusYd - Drill::kRingWidthYd / 2.f, kEps);
        CHECK(Is(r, "anchor", "target") && Is(r, "lethal", "1") && Is(r, "only", "member") && Is(r, "pick", "nontank"));
        CHECK(ring[0].startMs == call[0].resolveMs);
        CHECK(ring[0].resolveMs - ring[0].startMs == Drill::kRingArmMs);
        CHECK(Key(r, "persist", 0.f) == static_cast<float>(Drill::kRingLifeMs));
        CHECK(Drill::kRingArmMs + Drill::kRingLifeMs == 12000u);   // SP DR17: 12 s
        CHECK(Key(r, "tick", 0.f) == 250.f && !Drill::Value(r, "inside"));
        // RingInside: the band's inner edge less 1 yd.
        CHECK_NEAR(Drill::kRingInsideYd, Drill::kRingRadiusYd - Drill::kRingWidthYd / 2.f - 1.f, kEps);
        // The six blows: 2 s apart from the close, 1.5 s each, every one landing while the ring is armed.
        std::vector<std::uint32_t> starts;
        std::vector<Blow> order;
        for (Timed const& t : blows)
        {
            std::string_view const l = t.line;
            CHECK(Is(l, "only", "member") && Is(l, "pick", "same") && Key(l, "inside", 0.f) == Drill::kRingInsideYd);
            CHECK(!Drill::Value(l, "lethal"));
            CHECK(t.resolveMs - t.startMs == Drill::kBlowWarnMs);
            CHECK(t.resolveMs >= ring[0].resolveMs);
            CHECK(static_cast<float>(t.resolveMs) < EndMs(ring[0]));
            CHECK(t.index > ring[0].index);
            if (starts.empty() || starts.back() != t.startMs)
            {
                starts.push_back(t.startMs);
                order.push_back(BlowOf(Tag(l)));
            }
            Blow const b = BlowOf(Tag(l));
            switch (b)
            {
                case Blow::Sweep:
                    CHECK(ShapeOf(l) == "cone" && Size(l) == Drill::kSweepYd
                        && Key(l, "angle", 0.f) == Drill::kSweepDeg);
                    CHECK(Key(l, "pct", 0.f) == 40.f && Is(l, "weak", "1"));
                    break;
                case Blow::Cleave:
                    CHECK(ShapeOf(l) == "cone" && Size(l) == Drill::kCleaveYd
                        && Key(l, "angle", 0.f) == Drill::kCleaveDeg);
                    // 50%, its own penalty: no Weakened.
                    CHECK(Key(l, "pct", 0.f) == 50.f && !Drill::Value(l, "weak"));
                    break;
                case Blow::FireBlast:
                    CHECK(ShapeOf(l) == "circle" && Size(l) == Drill::kFireBlastYd && Is(l, "at", "member"));
                    CHECK(Key(l, "pct", 0.f) == 30.f && Is(l, "weak", "1") && Is(l, "school", "fire"));
                    break;
                case Blow::RingOfFlame:
                    CHECK(ShapeOf(l) == "donut" && Size(l) == Drill::kRingOfFlameOuter
                        && Key(l, "inner", 0.f) == Drill::kRingOfFlameInner);
                    CHECK(Key(l, "pct", 0.f) == 40.f && Is(l, "weak", "1") && Is(l, "school", "fire"));
                    break;
                case Blow::Thunderclap:
                    CHECK(ShapeOf(l) == "circle" && Size(l) == Drill::kThunderclapYd);
                    CHECK(Key(l, "pct", 0.f) == 40.f && Is(l, "weak", "1"));
                    break;
            }
            // The cones are aimed once, at the challenger where they stand as the blow is shown (B15: frozen at the
            // cast), from him; the circles on him are his (anchor=target); Fire Blast lands on the challenger.
            if (ShapeOf(l) == "cone")
                CHECK(Is(l, "anchor", "target") && Is(l, "to", "member") && Is(l, "fix", "1"));
            else if (b != Blow::FireBlast)
                CHECK(Is(l, "anchor", "target"));
        }
        CHECK(starts.size() == Drill::kBlows);
        for (std::size_t k = 0; k < starts.size(); ++k)
            CHECK(starts[k] == ring[0].startMs + static_cast<std::uint32_t>(k) * Drill::kBlowEveryMs);
        // B15's plan: the five kinds once, then one of the first four.
        std::set<Blow> const kinds(order.begin(), order.begin() + std::min<std::size_t>(order.size(), 5));
        CHECK(kinds.size() == 5);
        if (order.size() == Drill::kBlows)
            CHECK(std::find(order.begin(), order.begin() + 4, order.back()) != order.begin() + 4);
        // Fire Blast: three circles at the challenger, two of them 5 yd to either side.
        std::vector<Timed> const fb = Steps(2, Drill::kBrsTagFireBlast, Drill::kBrsTagFireBlast);
        CHECK(fb.size() == 3);
        if (fb.size() == 3)
        {
            CHECK(!Drill::Value(fb[0].line, "shift"));
            CHECK(Is(fb[1].line, "shift", "0:5") && Is(fb[2].line, "shift", "0:-5"));
        }
        // Nothing between the ring's pick and the last blow picks anyone else (pick=same keeps the challenger).
        for (Timed const& t : Steps(2, 1, 19))
            if (t.index > ring[0].index)
                CHECK(Is(t.line, "pick", "same"));
    }

    // A blow as the bot view builds it, aimed at `at` (the challenger) from the boss at the origin.
    std::vector<Hazard> BlowHazards(Blow b, std::uint32_t& id, Vec3 const& at, Ms land, float gmFacing)
    {
        std::vector<Hazard> out;
        float const facing = std::atan2(at.y, at.x);
        auto make = [&](Shape const& s, Pose const& p)
        {
            Hazard h;
            h.id = id++;
            h.shape = s;
            h.poses = { p };
            h.marginYd = kMargin;
            h.timing = Solver::MakeInstant(land, Ms(500));
            out.push_back(h);
        };
        switch (b)
        {
            case Blow::Sweep:
                make(Shape::Cone(Drill::kSweepDeg, Drill::kSweepYd), Pose{ 0.f, 0.f, 0.f, facing });
                break;
            case Blow::Cleave:
                make(Shape::Cone(Drill::kCleaveDeg, Drill::kCleaveYd), Pose{ 0.f, 0.f, 0.f, facing });
                break;
            case Blow::FireBlast:
                for (float left : { 0.f, Drill::kFireBlastAroundYd, -Drill::kFireBlastAroundYd })
                {
                    Vec3 const c = Drill::Shifted(at, gmFacing, 0.f, left);
                    make(Shape::Circle(Drill::kFireBlastYd), Pose{ c.x, c.y, 0.f, 0.f });
                }
                break;
            case Blow::RingOfFlame:
                make(Shape::Donut(Drill::kRingOfFlameInner, Drill::kRingOfFlameOuter), Pose{ 0.f, 0.f, 0.f, 0.f });
                break;
            case Blow::Thunderclap:
                make(Shape::Circle(Drill::kThunderclapYd), Pose{ 0.f, 0.f, 0.f, 0.f });
                break;
        }
        return out;
    }

    // Is p inside the exact shape of any of the hazards?
    bool InAny(std::vector<Hazard> const& hs, Vec3 const& p, float grow = 0.f)
    {
        for (Hazard const& h : hs)
            for (Pose const& pose : h.poses)
                if (Solver::SignedDistance(h.shape, pose, p) <= grow)
                    return true;
        return false;
    }

    // From where a challenger may stand (inside the bounds), the nearest spot inside the bounds clear of the blow by
    // the safety margin, against the run in the blow's 1.5 s (7 yd/s, less a 0.3 s reaction). Cleave, Fire Blast and
    // Ring of Flame always leave one in reach. The Thunderclap (8 yd on him) and the Sweep (a 10 yd half disc toward
    // the challenger) do not from everywhere: from his feet the Thunderclap's edge is 8.75 yd away, and the Sweep's
    // only way out inside 10 yd bounds is behind him (10.75 yd and more from the bounds' edge in front: a player can
    // also step back into the 10-11 yd strip, a bot kept to RingInside cannot). Reported with the share of spots.
    void TestChallengeGeometry()
    {
        float const runYd = Drill::kPlayerRunYdS * (static_cast<float>(Drill::kBlowWarnMs) - 300.f) / 1000.f;
        float const bound = Drill::kRingInsideYd;
        // The bounds sit wholly inside the ring's inner edge: a bot that keeps them never touches the band.
        CHECK(bound + kMargin <= Drill::kRingRadiusYd - Drill::kRingWidthYd / 2.f);
        for (int b = 0; b < 5; ++b)
        {
            float worst = 0.f;
            std::uint32_t spots = 0;
            std::uint32_t outOfReach = 0;
            for (float r = 0.5f; r <= bound; r += 0.5f)
                for (float a = 0.f; a < Geo::kTwoPi; a += 0.2f)
                {
                    Vec3 const at{ r * std::cos(a), r * std::sin(a), 0.f };
                    std::uint32_t id = 1;
                    std::vector<Hazard> const hs = BlowHazards(static_cast<Blow>(b), id, at, Ms(0), 0.4f);
                    float best = 1e9f;
                    for (float sr = 0.f; sr <= bound - 0.25f; sr += 0.25f)
                        for (float sa = 0.f; sa < Geo::kTwoPi; sa += 0.05f)
                        {
                            Vec3 const s{ sr * std::cos(sa), sr * std::sin(sa), 0.f };
                            if (!InAny(hs, s, kMargin))
                                best = std::min(best, std::hypot(s.x - at.x, s.y - at.y));
                        }
                    worst = std::max(worst, best);
                    ++spots;
                    outOfReach += best > runYd ? 1u : 0u;
                }
            Blow const blow = static_cast<Blow>(b);
            std::printf("  challenge geometry: %-13s worst way out %.1f yd (run %.1f yd), out of reach from %u of %u "
                "spots\n", BlowName(blow), worst, runYd, outOfReach, spots);
            if (blow == Blow::Cleave || blow == Blow::FireBlast || blow == Blow::RingOfFlame)
                CHECK(outOfReach == 0);
            else if (blow == Blow::Thunderclap)
                CHECK(worst <= Drill::kThunderclapYd + kMargin + kEps);     // only from his feet
            else
                CHECK(worst <= bound + kMargin + 0.25f);                    // from the bounds' edge in front of him
        }
    }

    // ---- the Challenge in the brain model --------------------------------------------------------------------------
    struct ChallengeResult
    {
        std::array<std::uint32_t, 5> blows{};
        std::array<std::uint32_t, 5> hits{};
        std::uint32_t ringHits = 0;         // steps that ended in the band or crossed it
        std::uint32_t leftBounds = 0;       // challenges that ended with the bot outside the bounds
        std::uint32_t runs = 0;
        // hits by (the blow before, the blow): which transition costs the bot
        std::array<std::array<std::uint32_t, 6>, 5> after{};
        BrainModel::Counters c;
    };

    // One bot (the challenger) on the Call's 7 yd ring at a random bearing; its stock follow pulls it to its role
    // spot (`roleYd` from him: a melee 3 yd, a ranged 25 yd, outside the ring). The lethal ring from the close on,
    // the six blows (the drill's order, or a B15 shuffle) aimed at where it stands as each is shown, the Within
    // circle of 10 yd in its view throughout.
    // F-3 (STAGE-SWEEP-DIAG Finding 2): `pet`: the challenger owns a pet or guardian, and its view adds the pet margin
    // to the lethal ring as MechanicBotView does (ViewRules::LethalPetExtraYd, as drift); `strikesPets`: the ring as
    // the view saw it before F-3 (a players-only ring that spares pets still got the margin). `live`: the live timing
    // (BrainModel::Config::Live).
    enum class Order : std::uint8_t { Drill, Shuffle, ThunderclapSweep };

    ChallengeResult RunChallenges(float roleYd, Order how, std::uint32_t seeds, bool pet = false,
        bool strikesPets = false, bool live = false)
    {
        ChallengeResult out;
        Drill::Def const& d = Followups();
        std::vector<Blow> drillOrder;
        {
            std::uint32_t last = 0;
            for (Timed const& t : Steps(2, Drill::kBrsTagSweep, Drill::kBrsTagThunderclap))
                if (drillOrder.empty() || t.startMs != last)
                {
                    drillOrder.push_back(BlowOf(Tag(t.line)));
                    last = t.startMs;
                }
        }
        (void)d;
        std::uint32_t rng = 0x2545f491u;
        auto next = [&rng](std::uint32_t n)
        {
            rng = rng * 1664525u + 1013904223u;
            return n ? (rng >> 8) % n : 0u;
        };
        for (std::uint32_t seed = 0; seed < seeds; ++seed)
        {
            BrainModel::Config cfg;
            cfg.params.marginMs = Ms(300);
            if (live)
                cfg.Live();
            BrainModel::World world;
            world.cfg = cfg;
            float const bearing = static_cast<float>(next(6283)) / 1000.f;
            float const callYd = Drill::kCallRingYd + (static_cast<float>(next(400)) / 100.f - 2.f);
            BrainModel::Bot bot;
            bot.pos = { callYd * std::cos(bearing), callYd * std::sin(bearing), 0.f };
            world.bots.push_back(bot);
            Vec3 const role{ roleYd * std::cos(bearing + 0.3f), roleYd * std::sin(bearing + 0.3f), 0.f };
            world.follow = [role](std::size_t, Vec3& to)
            {
                to = role;
                return true;
            };
            Solver::Bounds inside;
            Solver::BoundsArea area;
            area.center = { 0.f, 0.f, 0.f };
            area.radius = Drill::kRingInsideYd;
            inside.areas.push_back(area);
            world.kinds = [&inside](std::size_t, Solver::Input& in, Ms, std::vector<BrainModel::Task>&)
            {
                in.bounds = { inside };
            };
            std::vector<Blow> order = drillOrder;
            if (how == Order::ThunderclapSweep)
                order = { Blow::Thunderclap, Blow::Sweep, Blow::FireBlast, Blow::RingOfFlame, Blow::Cleave,
                    Blow::Thunderclap };
            else if (how == Order::Shuffle)
            {
                // B15 PlanBlows: the five kinds shuffled, then one of the first four.
                std::array<Blow, 5> kinds{ Blow::Sweep, Blow::Cleave, Blow::FireBlast, Blow::RingOfFlame,
                    Blow::Thunderclap };
                for (std::size_t i = kinds.size() - 1; i > 0; --i)
                    std::swap(kinds[i], kinds[next(static_cast<std::uint32_t>(i + 1))]);
                order.assign(kinds.begin(), kinds.end());
                order.push_back(kinds[next(4)]);
            }
            Ms const close{ 100000 };
            std::uint32_t id = 1;
            Hazard ring;
            ring.id = id++;
            ring.shape = Shape::Donut(Drill::kRingRadiusYd - Drill::kRingWidthYd / 2.f,
                Drill::kRingRadiusYd + Drill::kRingWidthYd / 2.f);
            ring.poses = { Pose{ 0.f, 0.f, 0.f, 0.f } };
            // The drill's ring is eligible to the challenger alone (only=member: players only), the real one spares
            // pets (the kit's SparesPets): no pet margin since F-3.
            ring.driftYd = ViewRules::LethalPetExtraYd(kMargin, pet, strikesPets);
            ring.marginYd = kMargin + ring.driftYd;
            ring.lethal = true;
            // ViewRules::LethalTiming: hot from its registration (the close) to its end.
            ring.timing = Solver::MakeInterval(close, close + Ms(Drill::kRingArmMs + Drill::kRingLifeMs));
            world.AddHazard(ring);
            std::vector<std::vector<std::uint32_t>> idsOf(order.size());
            Ms const end = close + Ms(Drill::kRingArmMs + Drill::kRingLifeMs);
            float const inner = Drill::kRingRadiusYd - Drill::kRingWidthYd / 2.f;
            float const outer = Drill::kRingRadiusYd + Drill::kRingWidthYd / 2.f;
            for (Ms now = close; now < end; now += cfg.step)
            {
                for (std::size_t k = 0; k < order.size(); ++k)
                    if (now == close + Ms(static_cast<std::int64_t>(k) * Drill::kBlowEveryMs) + cfg.sightLag)
                    {
                        Vec3 const at = world.bots[0].pos;
                        Ms const land = close
                            + Ms(static_cast<std::int64_t>(k) * Drill::kBlowEveryMs + Drill::kBlowWarnMs);
                        for (Hazard const& h : BlowHazards(order[k], id, at, land, 0.4f))
                        {
                            idsOf[k].push_back(h.id);
                            world.AddHazard(h);
                        }
                    }
                float const before = std::hypot(world.bots[0].pos.x, world.bots[0].pos.y);
                world.Step(now);
                float const after = std::hypot(world.bots[0].pos.x, world.bots[0].pos.y);
                // The ring kills from its arming on: in the band, or across it in one step.
                if (now >= close + Ms(Drill::kRingArmMs)
                    && ((after >= inner && after <= outer) || (before < inner) != (after < inner)))
                    ++out.ringHits;
            }
            BrainModel::Bot const& b = world.bots[0];
            for (std::size_t k = 0; k < order.size(); ++k)
            {
                std::size_t const kind = static_cast<std::size_t>(order[k]);
                ++out.blows[kind];
                bool hit = false;
                for (std::uint32_t hid : idsOf[k])
                    hit = hit || std::find(b.hitBy.begin(), b.hitBy.end(), hid) != b.hitBy.end();
                out.hits[kind] += hit ? 1u : 0u;
                if (hit)
                    ++out.after[kind][k == 0 ? 5 : static_cast<std::size_t>(order[k - 1])];
            }
            if (std::hypot(b.pos.x, b.pos.y) > Drill::kRingInsideYd + 0.25f)
                ++out.leftBounds;
            out.c.Add(world.Total());
            ++out.runs;
        }
        return out;
    }

    void TestChallengeBots()
    {
        struct Case
        {
            char const* name;
            float roleYd;
            Order how;
            bool pet = false;
            bool strikesPets = false;   // the pet margin as the view gave it before F-3 (reported only)
            bool live = false;
        };
        // Gated: the drill's order (no blow hit) and, in every case, the ring and the bounds. Reported: B15's
        // shuffles and the Thunderclap -> Sweep step on its own (the report's B15 question). F-3 (STAGE-SWEEP-DIAG
        // Finding 2): a pet owner as the challenger, gated like the pet-less one, also on the live timing; reported:
        // the pet margin on the ring as built before F-3 (no spot fits: the Thunderclap lands on it).
        Case const cases[] = {
            { "melee, drill order", 3.f, Order::Drill },
            { "ranged 25 yd, drill order", 25.f, Order::Drill },
            { "melee, B15 shuffles", 3.f, Order::Shuffle },
            { "ranged 25 yd, B15 shuffles", 25.f, Order::Shuffle },
            { "melee, thunderclap -> sweep", 3.f, Order::ThunderclapSweep },
            { "pet, melee, drill order", 3.f, Order::Drill, true },
            { "pet, ranged 25 yd, drill order", 25.f, Order::Drill, true },
            { "live, melee, drill order", 3.f, Order::Drill, false, false, true },
            { "live, ranged 25 yd, drill order", 25.f, Order::Drill, false, false, true },
            { "live, pet, melee, drill order", 3.f, Order::Drill, true, false, true },
            { "live, pet, ranged, drill order", 25.f, Order::Drill, true, false, true },
            { "pet margin as built, ranged", 25.f, Order::Drill, true, true },
        };
        for (Case const& c : cases)
        {
            ChallengeResult const r = RunChallenges(c.roleYd, c.how, 40, c.pet, c.strikesPets, c.live);
            std::uint32_t blows = 0;
            std::uint32_t hits = 0;
            std::printf("  challenge bots: %-28s %u runs, ring %u, left bounds %u, fallbacks %u, solve max %lld us;",
                c.name, r.runs, r.ringHits, r.leftBounds, r.c.Fallbacks(), static_cast<long long>(r.c.solveUsMax));
            for (std::size_t k = 0; k < 5; ++k)
            {
                blows += r.blows[k];
                hits += r.hits[k];
                std::printf(" %s %u/%u", BlowName(static_cast<Blow>(k)), r.hits[k], r.blows[k]);
            }
            std::printf("\n");
            for (std::size_t k = 0; k < 5; ++k)
                for (std::size_t p = 0; p < 6; ++p)
                    if (r.after[k][p])
                        std::printf("    %s hits after %s: %u\n", BlowName(static_cast<Blow>(k)),
                            p == 5 ? "the close" : BlowName(static_cast<Blow>(p)), r.after[k][p]);
            // Gated: the challenger never steps into or across the lethal ring and ends inside its bounds.
            CHECK(r.ringHits == 0);
            CHECK(r.leftBounds == 0);
            CHECK(blows == r.runs * Drill::kBlows);
            if (c.strikesPets)
            {
                // The as-built margin: the ring forbids 8.0 yd and out, the Thunderclap 8.75 yd and in.
                CHECK(r.hits[static_cast<std::size_t>(Blow::Thunderclap)] > 0);
                continue;
            }
            if (c.how == Order::Drill)
                CHECK(hits == 0);
            else
            {
                // Every hit off the drill's order is a Sweep (the out-of-reach step), none of the other kinds.
                CHECK(hits == r.hits[static_cast<std::size_t>(Blow::Sweep)]);
            }
        }
    }

    // ---- row 24 in the brain model (F-3) ----------------------------------------------------------------------------
    // Rend's two walls as the row registers them: both at the call, 68 yd wide over 86 yd of travel across the boss
    // (MechanicCommand.cpp linefront: the swept area's middle on him), the first gap anywhere in the 40 yd strip (edge
    // 15), the second across the centerline within 20 yd of it, 3 s apart; the Within circle of 20 yd round him. The
    // sweep's group (mod-dpssim SimMechanicsScenario.cpp): the tank 3 yd off him toward the anchor, the healer 18 yd
    // and the member under test 12 yd on the arc round that side, two melee 3 yd off him; stock follow back to those
    // spots. The walls travel along `travelDeg` (0: in from the arc's side, through the group and then him).
    constexpr float kRendNearApartYd = 16.f;    // gaps this far apart or nearer: 10 yd between their near edges

    struct RendWallsResult
    {
        std::uint32_t first = 0;        // hits by the first wall (the sweep's Zone/11)
        std::uint32_t secondNear = 0;   // by the second (Zone/12), gaps at most kRendNearApartYd apart
        std::uint32_t secondFar = 0;    // by the second, gaps farther apart
        std::uint32_t passes = 0;       // member passes of each wall
        std::uint32_t passesNear = 0;   // of them with the gaps at most kRendNearApartYd apart
        BrainModel::Counters c;
    };

    RendWallsResult RunRendWalls(float travelDeg, bool live, std::uint32_t seeds)
    {
        RendWallsResult out;
        std::uint32_t rng = 0x6a09e667u;
        auto roll = [&rng]()
        {
            rng = rng * 1664525u + 1013904223u;
            return static_cast<float>(rng >> 8) / 16777216.f;
        };
        float const halfTravel = Drill::kRendTravelYd * 0.5f;
        for (std::uint32_t seed = 0; seed < seeds; ++seed)
        {
            BrainModel::Config cfg;
            if (live)
                cfg.Live();
            BrainModel::World world;
            world.cfg = cfg;
            // The anchor side at bearing pi; the walls travel along `facing`, from behind him on that line.
            float const facing = travelDeg * kDeg;
            std::vector<Vec3> slots;
            auto at = [](float bearingDeg, float r)
            {
                return Vec3{ r * std::cos(bearingDeg * kDeg), r * std::sin(bearingDeg * kDeg), 0.f };
            };
            slots.push_back(at(180.f, 3.f));                // tank
            slots.push_back(at(150.f, 18.f));               // healer
            slots.push_back(at(170.f, 12.f));               // member under test
            slots.push_back(at(10.f, 3.f));                 // melee
            slots.push_back(at(-10.f, 3.f));                // melee
            for (Vec3 const& v : slots)
            {
                BrainModel::Bot b;
                b.pos = v;
                world.bots.push_back(b);
            }
            world.follow = [&slots](std::size_t i, Vec3& to)
            {
                to = slots[i];
                return true;
            };
            Solver::Bounds room;
            Solver::BoundsArea area;
            area.center = { 0.f, 0.f, 0.f };
            area.radius = Drill::kRendRoomYd;
            room.areas.push_back(area);
            world.kinds = [&room](std::size_t, Solver::Input& in, Ms, std::vector<BrainModel::Task>&)
            {
                in.bounds = { room };
            };
            LineFrontSpec first;
            first.facing = facing;
            first.travelYd = Drill::kRendTravelYd;
            first.widthYd = Drill::kRendWidthYd;
            float const u1 = roll();
            float const u2 = roll();
            std::optional<float> const placed = Front::PlaceGap(Drill::kRendWidthYd, Drill::kRendGapYd, {}, u1, 0,
                Drill::kRendEdgeYd);
            CHECK(placed.has_value());
            float const g1 = placed.value_or(0.f);
            std::optional<float> const across = Front::PlaceGap(Drill::kRendWidthYd, Drill::kRendGapYd, {}, u2,
                -Front::SideOf(g1), Drill::kRendEdgeYd, g1, Drill::kRendApartYd);
            CHECK(across.has_value());
            float const g2 = across.value_or(-g1);
            first.gaps = { { g1, Drill::kRendGapYd } };
            LineFrontSpec second = first;
            second.gaps = { { g2, Drill::kRendGapYd } };
            Vec3 const start{ -std::cos(facing) * halfTravel, -std::sin(facing) * halfTravel, 0.f };
            Ms const t0{ 100000 };
            Ms const call = t0 + Ms(2000);
            Ms const launch = call + Ms(Drill::kRendWarnMs);
            std::vector<Hazard> walls;
            Pose const pose{ start.x, start.y, 0.f, facing };
            for (LineFrontSpec const* spec : { &first, &second })
                for (Front::Segment const& seg : Front::Segments(Front::LineShape(*spec), pose, spec->gaps))
                {
                    Hazard h;
                    h.id = spec == &first ? 1u : 2u;
                    h.shape = seg.shape;
                    h.poses = { seg.pose };
                    h.marginYd = kMargin;
                    h.timing = Solver::MakeProjectile(Front::LineParams(*spec),
                        spec == &first ? launch : launch + Ms(Drill::kRendSpacingMs));
                    h.front = true;
                    walls.push_back(h);
                }
            bool added = false;
            Ms const end = launch + Ms(Drill::kRendSpacingMs) + Front::Duration(Front::LineParams(second)) + Ms(1000);
            for (Ms now = t0; now < end; now += cfg.step)
            {
                if (!added && now >= call + cfg.sightLag)
                {
                    for (Hazard const& h : walls)
                        world.AddHazard(h);
                    added = true;
                }
                world.Step(now);
            }
            bool const near = std::fabs(g1 - g2) <= kRendNearApartYd;
            for (BrainModel::Bot const& b : world.bots)
                for (std::uint32_t hid : b.hitBy)
                    ++(hid == 1 ? out.first : (near ? out.secondNear : out.secondFar));
            out.passes += static_cast<std::uint32_t>(world.bots.size());
            out.passesNear += near ? static_cast<std::uint32_t>(world.bots.size()) : 0u;
            out.c.Add(world.Total());
        }
        return out;
    }

    void TestRendBots()
    {
        // F-3: the live sweep (mechsweep-brs-followups-1116) had the first wall on nearly every member in 4 of 5
        // iterations: the Fallback left the first gap at once on a way through the first wall (the second gap is
        // hazard-free from an arrival behind it, the first never is), which the model's 150 ms start delay hid
        // (live timing before F-3: 72% first-wall hits). Gated on both timings: the first wall at most 2% of the member
        // passes, the second at most 2% of those with the gaps 16 yd apart or nearer. Reported: the second wall with
        // the gaps farther apart (up to the ruled 20 yd), where the bots wait at the 20 yd circle's far edge and the
        // circle leaves too little room to cross behind the first wall in time (Rend's own 40 x 86 yd strip does:
        // front_test's live sweeps).
        for (bool live : { false, true })
            for (float travel : { 0.f, 90.f, 180.f })
            {
                RendWallsResult const r = RunRendWalls(travel, live, 24);
                std::printf("  rend walls (row 24): %-4s travel %3.0f deg: first wall %2u of %3u passes, second %2u of "
                    "%3u (gaps <= 16 yd apart) and %2u of %3u (farther), solves %4u, fallbacks %4u, solve max %lld "
                    "us\n", live ? "live" : "host", travel, r.first, r.passes, r.secondNear, r.passesNear, r.secondFar,
                    r.passes - r.passesNear, r.c.solves, r.c.Fallbacks(), static_cast<long long>(r.c.solveUsMax));
                CHECK(r.passes == 24u * 5u && r.passesNear > 0);
                CHECK(r.first * 50 <= r.passes);
                CHECK(r.secondNear * 50 <= r.passesNear);
            }
    }

    // The program: `blackrock-spire-followups:3` is one 120 s entry; each wave 2 row runs once.
    void TestSweepProgram()
    {
        std::string err;
        std::optional<Drill::Program> const p = Drill::ParseProgram("blackrock-spire-followups:3", err);
        CHECK(p && err.empty() && p->entries.size() == 1);
        if (!p || p->entries.size() != 1)
            return;
        Drill::ProgramEntry const& e = p->entries[0];
        CHECK(e.cycles == 3 && e.lengthMs == 120000);
        std::array<std::uint32_t, 20> perTag{};
        for (Drill::ScheduledStep const& st : Drill::StepSchedule(*e.drill, e.cycles))
            ++perTag[std::min<std::uint32_t>(Tag(e.drill->steps[st.step].line), 19)];
        CHECK(perTag[Drill::kBrsTagRingFirst] == 1 && perTag[Drill::kBrsTagRingNext] == 3);
        CHECK(perTag[Drill::kBrsTagWall] == 1 && perTag[Drill::kBrsTagWallSecond] == 1);
        CHECK(perTag[Drill::kBrsTagRing] == 1 && perTag[Drill::kBrsTagFireBlast] == 3);
        CHECK(perTag[Drill::kBrsTagRingOfFlame] == 2 && perTag[Drill::kBrsTagThunderclap] == 1);
        CHECK(perTag[Drill::kBrsTagHaymaker] == 3 && perTag[Drill::kBrsTagHaul] == 4
            && perTag[Drill::kBrsTagRecall] == 2);
    }
}

int main()
{
    TestRotation();
    TestEmberseer();
    TestRend();
    TestChallengeTable();
    TestChallengeGeometry();
    TestChallengeBots();
    TestRendBots();
    TestSweepProgram();
    return Report("brs_upper_followups_test");
}
