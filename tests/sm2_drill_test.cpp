// Standalone tests for the Scarlet Monastery wave 2 shapes of src/MechanicDrill.h (Scarlet Monastery build §3.9,
// WP-G2): the `scarlet-monastery-shapes-2` drill (rows sow-prune, blessed-ground, close-ranks, vigil-flame,
// blade-window, herod-lap, turning-beams) against the build spec rows it reproduces (Thorne GD1, GD1a, GD1c, GD2,
// GD2a; Halden MA4; Whitemane WM5, WM5a; Crell AR7; Herod HB1a-HB1d, HM3; heroic numbers, the beams' cycle half hard
// mode) on the NAVMESH numbers of HANDOFF §2.3 and §7 #28-#33, the grammar it adds (FixLine past, leash anchor=self,
// ParseHoldFacing) and the runner's repeat= / every= fix (RepeatOffsets, StepRepeatOffsets). Build: tests/run.sh.

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
    constexpr float kShiftEps = 0.02f;          // the table's shift= values are written to 0.01 yd
    constexpr float kMarginYd = 0.5f;           // BossMechanics.Bots.SafetyMarginYd default (M2)
    constexpr float kRunYdS = 7.f;              // a player's run speed
    constexpr float kReactS = 0.3f;             // the bots' reaction allowance the other drill tests use
    // The sim's layout (SimMechanicsScenario.cpp, as sm_drill_test): the tank 3 yd and the DPS 12 yd from the boss,
    // the healer 18 yd, on the reference's side: the run's facing points from the group to the boss.
    constexpr float kSimTankYd = 3.f;
    constexpr float kSimDpsYd = 12.f;
    constexpr float kSimHealerYd = 18.f;
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

    // A signed number (Drill::Number reads none; shift= values may be negative, as Acore::StringTo reads them).
    std::optional<float> Signed(std::string_view s)
    {
        if (!s.empty() && s[0] == '-')
        {
            std::optional<float> const v = Drill::Number(s.substr(1));
            return v ? std::optional<float>(-*v) : std::nullopt;
        }
        return Drill::Number(s);
    }

    // shift=<ahead>:<left> (0:0 when absent)
    Vec3 ShiftOf(std::string_view line)
    {
        std::optional<std::string_view> const v = Drill::Value(line, "shift");
        if (!v)
            return { 0.f, 0.f, 0.f };
        std::size_t const colon = v->find(':');
        CHECK(colon != std::string_view::npos);
        if (colon == std::string_view::npos)
            return { 0.f, 0.f, 0.f };
        return { Signed(v->substr(0, colon)).value_or(1e9f), Signed(v->substr(colon + 1)).value_or(1e9f), 0.f };
    }

    Drill::Def const& Shapes()
    {
        static Drill::Def const* d = Drill::Find("scarlet-monastery-shapes-2");
        return *d;
    }

    struct At
    {
        std::string_view line;
        std::uint32_t startMs = 0;
        std::uint32_t resolveMs = 0;
        std::uint32_t endMs = 0;        // resolve plus persist
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
                out.push_back({ line, *offsets[i], resolve, end });
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

    // The length of a drill-only step: chase / leash / walk `<yd> <ms>`, holdfacing `<ms>`.
    std::uint32_t OnlyStepMs(std::string_view line)
    {
        std::vector<std::string_view> const t = Drill::Tokens(line);
        if (!t.empty() && t[0] == "holdfacing")
            return t.size() >= 2 ? static_cast<std::uint32_t>(Drill::Number(t[1]).value_or(0.f)) : 0u;
        return t.size() >= 3 ? static_cast<std::uint32_t>(Drill::Number(t[2]).value_or(0.f)) : 0u;
    }

    float Dist(Vec3 const& a, Vec3 const& b)
    {
        return Geo::Dist2d(a.x, a.y, b.x, b.y);
    }

    Vec3 Dir(float rad, float yd, Vec3 const& from = { 0.f, 0.f, 0.f })
    {
        return { from.x + yd * std::cos(rad), from.y + yd * std::sin(rad), from.z };
    }

    // A line step's source in the run frame (boss start at the origin, the run's facing along +x): shift= read along
    // the line's facing (the run's turned by offset=), as RegisterRepeat does for a plain line; a fixed to= line's
    // shift= is read along the run's facing before it is aimed (WP-G2), and it has no offset=.
    Vec3 LineSource(std::string_view line)
    {
        float const facing = Geo::DegToRad(Key(line, "offset", 0.f));
        Vec3 const s = ShiftOf(line);
        return Drill::Shifted({ 0.f, 0.f, 0.f }, facing, s.x, s.y);
    }

    // The pit's center (the planted axe): kPitAheadYd past his start along the run's facing.
    Vec3 const kPit{ Drill::kPitAheadYd, 0.f, 0.f };

    // ---- the table ------------------------------------------------------------------------------------------------

    void TestTable()
    {
        Drill::Def const* d = Drill::Find("scarlet-monastery-shapes-2");
        CHECK(d != nullptr);
        if (!d)
            return;
        CHECK(d->cycleMs == Drill::kSm2CycleMs && d->defaultCycles == Drill::kSm2Cycles);
        CHECK(d->cycleMs * d->defaultCycles == 180000u);
        CHECK(!d->ownerAtCreature);

        std::set<std::uint32_t> tags;
        for (Drill::Step const& s : d->steps)
        {
            std::string_view const shape = ShapeOf(s.line);
            CHECK(Is(s.line, "cycle", "0/3") || Is(s.line, "cycle", "1/3") || Is(s.line, "cycle", "2/3"));
            if (shape == "marker" || shape == "chase" || shape == "walk" || shape == "holdfacing")
            {
                CHECK(!Drill::Value(s.line, "tag"));
                continue;
            }
            tags.insert(Tag(s.line));
            if (Drill::DamageStep(s.line))
                CHECK(Drill::Value(s.line, "pct").has_value());
            CHECK(!Drill::Value(s.line, "dmg"));
            // no step of this drill repeats itself (every repeat is its own row)
            CHECK(Drill::StepRepeatOffsets(s.line).size() == 1);
        }
        CHECK(tags == (std::set<std::uint32_t>{ 21, 22, 23, 24, 25, 26, 27, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
            39, 40 }));
        // never meets wave 1's tags
        for (std::uint32_t t : tags)
            CHECK(!Drill::SmBarTag(t) && t > 11);

        std::set<std::uint32_t> const bar(std::begin(Drill::kSm2BarTags), std::end(Drill::kSm2BarTags));
        CHECK(bar == (std::set<std::uint32_t>{ 21, 22, 23, 24, 25, 26, 27, 30, 31, 33, 34, 35, 36, 37, 38, 39, 40 }));
        for (std::uint32_t t = 0; t <= 45; ++t)
            CHECK(Drill::Sm2BarTag(t) == (bar.count(t) == 1));
        std::set<std::uint32_t> const group(std::begin(Drill::kSm2GroupTags), std::end(Drill::kSm2GroupTags));
        CHECK(group == (std::set<std::uint32_t>{ Drill::kSm2TagFlame }));
        std::set<std::uint32_t> const refs(std::begin(Drill::kSm2ReferenceTags), std::end(Drill::kSm2ReferenceTags));
        CHECK(refs == (std::set<std::uint32_t>{ Drill::kSm2TagFurrow, Drill::kSm2TagTankHold,
            Drill::kSm2TagBlessedGround, Drill::kSm2TagBloom, Drill::kSm2TagSeared, Drill::kSm2TagFlameTank }));
        for (std::uint32_t t : refs)
            CHECK(bar.count(t) == 1);
        // every tag is gated, a group failure or the reported spread (32)
        for (std::uint32_t t : tags)
            CHECK(bar.count(t) + group.count(t) + (t == Drill::kSm2TagBladeSpread ? 1u : 0u) == 1u);

        // Cycle 0: Thorne then Halden (21-27); cycle 1: Whitemane then Crell (29-34); cycle 2: Herod (35-40). In time
        // order, over before the next cycle.
        for (std::uint32_t cycle = 0; cycle < 3; ++cycle)
        {
            std::uint32_t last = 0;
            for (At const& a : Cycle(cycle))
            {
                CHECK(a.startMs >= last);
                last = a.startMs;
                CHECK(std::max(a.endMs, a.startMs + OnlyStepMs(a.line)) <= Drill::kSm2CycleMs);
                std::uint32_t const tag = Tag(a.line);
                if (!tag)
                    continue;
                CHECK(cycle == 0 ? tag >= 21 && tag <= 27 : cycle == 1 ? tag >= 29 && tag <= 34 : tag >= 35);
            }
        }
    }

    // ---- grammar --------------------------------------------------------------------------------------------------

    void TestRepeatOffsets()
    {
        // The runner's fix: repeat= / every= expand into offsets (it used to drop them: one egg a cycle).
        std::vector<std::uint32_t> const one = Drill::RepeatOffsets(1, 50);
        CHECK(one.size() == 1 && one[0] == 0);
        std::vector<std::uint32_t> const three = Drill::RepeatOffsets(3, 400);
        CHECK(three == (std::vector<std::uint32_t>{ 0, 400, 800 }));
        CHECK(Drill::RepeatOffsets(0, 100).size() == 1);                                // at least one
        CHECK(Drill::RepeatOffsets(99, 100).size() == Drill::kMaxStepRepeat);           // .mech test's cap
        CHECK(Drill::StepRepeatOffsets("circle 2 500 tag=1").size() == 1);
        std::vector<std::uint32_t> const dflt = Drill::StepRepeatOffsets("circle 2 500 repeat=3 tag=1");
        CHECK(dflt == (std::vector<std::uint32_t>{ 0, 1000, 2000 }));                  // every= 1000 by default
        CHECK(Drill::StepRepeatOffsets("circle 2 500 repeat=80 every=10 tag=1").size() == 50);

        // `pass-eggs`: forty eggs 50 ms apart, laid inside the 20 s cycle and gone before the next one lays them.
        Drill::Def const* eggs = Drill::Find("pass-eggs");
        CHECK(eggs != nullptr && eggs->steps.size() == 1);
        if (!eggs || eggs->steps.empty())
            return;
        std::string_view const line = eggs->steps.front().line;
        std::vector<std::uint32_t> const laid = Drill::StepRepeatOffsets(line);
        CHECK(laid.size() == 40);
        CHECK(laid.front() == 0 && laid.back() == 39u * 50u);
        for (std::size_t i = 1; i < laid.size(); ++i)
            CHECK(laid[i] - laid[i - 1] == 50u);
        CHECK(laid.back() + Drill::WarnMs(line) + static_cast<std::uint32_t>(Key(line, "persist", 0.f))
            <= eggs->cycleMs);
        // the repeats keep the step's tag: the `pass` descriptor counts Zone/60 (DrillPresets::kPassEggTag)
        CHECK(Tag(line) == 60);
        // every other drill step registers once (none relied on the dropped keys); Ragefire Chasm's fel spots came
        // later and repeat on purpose (rfc_drill_test)
        for (Drill::Def const& d : Drill::All())
            for (Drill::Step const& s : d.steps)
                if (&d != eggs && std::string_view(d.name) != "ragefire-chasm-shapes")
                    CHECK(Drill::StepRepeatOffsets(s.line).size() == 1);
    }

    void TestFixLinePast()
    {
        Vec3 const src{ 0.f, 0.f, 0.f };
        // reach=1: ends at the unit (at least size); past= runs on beyond it
        Drill::FixedLine const plain = Drill::FixLine(src, { 10.f, 0.f, 0.f }, 5.f, true, 0.f);
        CHECK_NEAR(plain.length, 10.f, kEps);
        Drill::FixedLine const past = Drill::FixLine(src, { 10.f, 0.f, 0.f }, 5.f, true, 0.f, 5.f);
        CHECK_NEAR(past.length, 15.f, kEps);
        // a near unit: past still applies (d + past against size)
        Drill::FixedLine const near = Drill::FixLine(src, { 0.f, 2.f, 0.f }, 5.f, true, 0.f, 5.f);
        CHECK_NEAR(near.length, 7.f, kEps);
        CHECK_NEAR(near.facing, Geo::kPi / 2.f, kEps);
        // without reach past is ignored (fix=1 keeps size)
        CHECK_NEAR(Drill::FixLine(src, { 10.f, 0.f, 0.f }, 5.f, false, 0.f, 5.f).length, 5.f, kEps);
        CHECK_NEAR(Drill::FixLine(src, { 10.f, 0.f, 0.f }, 5.f, true, 0.f, -3.f).length, 10.f, kEps);
        // the next dash starts at the end: 5 yd past the target's spot
        Vec3 const end = Drill::LineEnd(src, past.facing, past.length);
        CHECK_NEAR(Dist(end, { 10.f, 0.f, 0.f }), Drill::kStrikePastYd, kEps);
    }

    void TestLeashSelfAndHoldFacing()
    {
        std::optional<Drill::LeashStep> const l = Drill::ParseLeash("leash 4 9000 role=tank anchor=self tag=23");
        CHECK(l && l->anchor == Drill::LeashAnchor::Self && l->role == Drill::LeashRole::Tank && !l->near);
        CHECK(l && l->ledgerTag == 23 && l->ms == 9000);
        CHECK(!Drill::ParseLeash("leash 4 9000 anchor=me tag=23"));
        CHECK(Drill::ParseLeash("leash 8 6000 anchor=tank tag=2").has_value());     // the old forms still read
        // anchor=self: judged against the member's own start (OutOfLeash from it)
        Vec3 const start{ 3.f, 0.f, 0.f };
        CHECK(!Drill::OutOfLeash(start, { 5.f, 2.f, 0.f }, Drill::kTankHoldDriftYd));
        // a re-facing walk a quarter round him at 3 yd moves the tank 4.24 yd: caught
        Vec3 const quarter = Dir(Geo::kPi / 2.f, kSimTankYd);
        CHECK(Drill::OutOfLeash(start, quarter, Drill::kTankHoldDriftYd));

        std::optional<std::uint32_t> const h = Drill::ParseHoldFacing("holdfacing 9000");
        CHECK(h && *h == 9000);
        CHECK(!Drill::ParseHoldFacing("holdfacing"));
        CHECK(!Drill::ParseHoldFacing("holdfacing 100"));          // shorter than one step
        CHECK(!Drill::ParseHoldFacing("holdfacing 130000"));       // beyond kMaxWalkMs
        CHECK(!Drill::ParseHoldFacing("holdfacing 9000 tag=3"));
        CHECK(!Drill::ParseHoldFacing("holdfacing 9000.5"));
        CHECK(!Drill::ParseHoldFacing("chase 10 1000"));
        CHECK(!Drill::DamageShape("holdfacing"));
        CHECK(!Drill::DamageStep("holdfacing 9000"));
        CHECK(Drill::WarnMs("holdfacing 9000") == 0);
    }

    // ---- cycle 0: Thorne (sow-prune, blessed-ground) and Halden (close-ranks) ----------------------------------

    void TestSowPrune()
    {
        // GD1a: a Prune every 10 s, 120 deg / 8 yd, 2 s wind-up, heroic 45% + Weakened, from 0 s
        std::vector<At> const prunes = WithTag(0, Drill::kSm2TagPrune);
        CHECK(prunes.size() == 4);
        for (std::size_t i = 0; i < prunes.size(); ++i)
        {
            At const& p = prunes[i];
            CHECK(ShapeOf(p.line) == "frontal");
            CHECK(p.startMs == i * Drill::kPruneEveryMs);
            CHECK_NEAR(Size(p.line), Drill::kPruneYd, kEps);
            CHECK(Key(p.line, "angle", 0.f) == Drill::kPruneAngleDeg);
            CHECK(Drill::WarnMs(p.line) == Drill::kPruneWarnMs);
            CHECK(Key(p.line, "pct", 0.f) == 45.f && Is(p.line, "weak", "1"));
        }

        // GD1 heroic: three carriers, a 5 s debuff, the furrow a 3 yd circle 5 yd ahead of him along HIS facing; the
        // first Sow 1 s after a Prune lands (prune 0 resolves at 2 s; the Sow at 3 s), the second likewise (22 -> 23)
        std::vector<At> const furrows = WithTag(0, Drill::kSm2TagFurrow);
        CHECK(furrows.size() == 2);
        std::vector<At> const markers = WithShape(0, "marker");
        for (std::size_t i = 0; i < furrows.size() && i < 2; ++i)
        {
            At const& f = furrows[i];
            CHECK(f.startMs == prunes[2 * i].resolveMs + 1000u);
            CHECK(ShapeOf(f.line) == "spot" && Key(f.line, "ring", -1.f) == 0.f);
            CHECK_NEAR(Size(f.line), Drill::kFurrowYd, kEps);
            CHECK(Drill::WarnMs(f.line) == Drill::kSowMs);
            CHECK(Is(f.line, "at", "target") && Is(f.line, "along", "target"));
            Vec3 const s = ShiftOf(f.line);
            CHECK_NEAR(s.x, Drill::kFurrowAheadYd, kEps);
            CHECK_NEAR(s.y, 0.f, kEps);
            CHECK(Is(f.line, "aura", "205009:1"));
            // three Marked (drill) carriers set at the Sow, cleared after the cutting Prune
            int sown = 0;
            for (At const& m : markers)
                if (m.startMs == f.startMs && Size(m.line) == 1.f)
                {
                    ++sown;
                    // the first Sow: non-tanks, so the hold's judge reads the tank alone
                    CHECK(Is(m.line, "pick", "nontank") == (i == 0));
                }
            CHECK(sown == static_cast<int>(Drill::kSowCarriers));
        }
        int clears = 0;
        for (At const& m : markers)
            if (Size(m.line) == 0.f)
            {
                ++clears;
                CHECK(m.startMs == prunes[1].resolveMs || m.startMs == prunes[3].resolveMs);
            }
        CHECK(clears == 2);

        // The furrow lies inside the Prune: its far point 8 yd out (the cone's length), its edges 31 deg off the
        // facing (inside the 60 deg half angle), so a seed planted there is cut.
        float const far = Drill::kFurrowAheadYd + Drill::kFurrowYd;
        CHECK(far <= Drill::kPruneYd + kEps);
        float const halfAngle = std::atan2(Drill::kFurrowYd, Drill::kFurrowAheadYd) * 180.f / Geo::kPi;
        CHECK(halfAngle < Drill::kPruneAngleDeg / 2.f);
        // and the carriers clear it in the wind-up: from the furrow's center out of the 8 yd cone, sideways
        float const sideways = std::tan(Geo::DegToRad(Drill::kPruneAngleDeg / 2.f)) * Drill::kFurrowAheadYd;
        float const clearS = (sideways + kMarginYd) / kRunYdS + kReactS;
        CHECK(clearS <= static_cast<float>(Drill::kPruneWarnMs) / 1000.f);
        // the Sow's debuff ends before the cutting Prune's wind-up starts (carriers plant, then step out)
        CHECK(furrows[0].resolveMs <= prunes[1].startMs);

        // F6: the hold runs from the first Sow to the Prune that cuts its seeds, the anchor=self leash over it
        std::vector<At> const holds = WithShape(0, "holdfacing");
        CHECK(holds.size() == 2);
        for (std::size_t i = 0; i < holds.size() && i < 2; ++i)
        {
            CHECK(holds[i].startMs == furrows[i].startMs);
            CHECK(OnlyStepMs(holds[i].line) == Drill::kFacingHoldMs);
            CHECK(holds[i].startMs + Drill::kFacingHoldMs == prunes[2 * i + 1].resolveMs);
        }
        std::vector<At> const drift = WithTag(0, Drill::kSm2TagTankHold);
        CHECK(drift.size() == 1);
        if (!drift.empty())
        {
            std::optional<Drill::LeashStep> const l = Drill::ParseLeash(Drill::StripPlanKeys(drift[0].line));
            CHECK(l && l->anchor == Drill::LeashAnchor::Self && l->role == Drill::LeashRole::Tank);
            CHECK(l && l->ms == Drill::kFacingHoldMs && drift[0].startMs == holds[0].startMs);
            CHECK_NEAR(l ? l->yd : 0.f, Drill::kTankHoldDriftYd, kEps);
        }
        // a tank carrier would walk to the furrow: in front of him at 3 yd, the furrow's center is 2 yd away (well
        // inside the 4 yd drift), but the first Sow keeps the tank out of the carriers anyway
        CHECK(Drill::kFurrowAheadYd - kSimTankYd < Drill::kTankHoldDriftYd);

        // the second Sow's seeds bloom at once (Bless the Harvest missed): the 6 yd ring at the furrow as the debuff
        // ends, 2 s, inside the hold (the tank, 2 yd from the furrow, has to step out: the hold does not stop an
        // escape) and before the next Prune lands
        std::vector<At> const blooms = WithTag(0, Drill::kSm2TagBloom);
        CHECK(blooms.size() == 1);
        if (!blooms.empty())
        {
            At const& b = blooms[0];
            CHECK(b.startMs == furrows[1].resolveMs);
            CHECK(Drill::WarnMs(b.line) == Drill::kBloomWarnMs);
            CHECK_NEAR(Size(b.line), Drill::kBloomYd, kEps);
            CHECK(Is(b.line, "at", "target") && Is(b.line, "along", "target") && Is(b.line, "school", "holy"));
            Vec3 const s = ShiftOf(b.line);
            CHECK_NEAR(s.x, Drill::kFurrowAheadYd, kEps);
            CHECK(Key(b.line, "pct", 0.f) == 30.f && Is(b.line, "weak", "1"));
            CHECK(b.startMs >= holds[1].startMs && b.resolveMs <= holds[1].startMs + Drill::kFacingHoldMs);
            CHECK(Drill::kFurrowAheadYd - kSimTankYd < Drill::kBloomYd);
            // carriers on the furrow's edge get out in time
            CHECK((Drill::kBloomYd + Drill::kFurrowYd + kMarginYd) / kRunYdS + kReactS
                <= static_cast<float>(Drill::kBloomWarnMs) / 1000.f);
        }
    }

    void TestBlessedGround()
    {
        // GD2 heroic Holy Water: 12 yd at him, 2.5 s, 45% Holy + Weakened, 12 s after the first Prune lands
        std::vector<At> const water = WithTag(0, Drill::kSm2TagHolyWater);
        CHECK(water.size() == 1);
        std::vector<At> const ground = WithTag(0, Drill::kSm2TagBlessedGround);
        CHECK(ground.size() == 1);
        std::vector<At> const chases = WithShape(0, "chase");
        CHECK(chases.size() == 1);
        if (water.empty() || ground.empty() || chases.empty())
            return;
        At const& w = water[0];
        CHECK(ShapeOf(w.line) == "circle" && Is(w.line, "at", "target") && Is(w.line, "school", "holy"));
        CHECK_NEAR(Size(w.line), Drill::kHolyWaterYd, kEps);
        CHECK(Drill::WarnMs(w.line) == Drill::kHolyWaterMs);
        CHECK(Key(w.line, "pct", 0.f) == 45.f && Is(w.line, "weak", "1"));
        CHECK(w.startMs == 2000u + 12000u);
        // the tank steps out of it from 3 yd in the cast
        CHECK((Drill::kHolyWaterYd - kSimTankYd + kMarginYd) / kRunYdS + kReactS
            <= static_cast<float>(Drill::kHolyWaterMs) / 1000.f);

        // GD2a (the F14 shape on him): 8 yd at his spot as the water lands, 8 s, tank-only, no damage
        At const& g = ground[0];
        CHECK(g.startMs == w.resolveMs);
        CHECK(ShapeOf(g.line) == "circle" && Is(g.line, "at", "target") && Is(g.line, "role", "tank"));
        CHECK_NEAR(Size(g.line), Drill::kBlessedGroundYd, kEps);
        CHECK(Key(g.line, "pct", -1.f) == 0.f && !Drill::Value(g.line, "weak"));
        CHECK(Key(g.line, "persist", 0.f) == static_cast<float>(Drill::kBlessedGroundMs));
        CHECK(Drill::WarnMs(g.line) == Drill::kBlessedGroundWarnMs);
        // he chases the tank from the splash, through the ground and the second Sow, home after its Prune
        At const& c = chases[0];
        std::optional<Drill::ChaseSpec> const spec = Drill::ParseChase(Drill::StripPlanKeys(c.line));
        CHECK(spec && spec->home);
        if (!spec)
            return;
        CHECK(c.startMs == g.startMs && c.startMs + spec->ms >= g.endMs);
        std::vector<At> const prunes = WithTag(0, Drill::kSm2TagPrune);
        CHECK(!prunes.empty() && c.startMs + spec->ms >= prunes.back().resolveMs);
        CHECK(c.startMs + spec->ms < WithTag(0, Drill::kSm2TagCloseRanks).front().startMs);

        // Drag model: the tank, out of the water at 12.5 yd, holds there (the ground is its zone); he follows at
        // 7 yd/s and leaves the ground before its first judged second ends.
        Vec3 const start{ 0.f, 0.f, 0.f };
        Vec3 const tank{ -(Drill::kHolyWaterYd + kMarginYd), 0.f, 0.f };
        Vec3 boss = start;
        float const step = Drill::kChaseSpeedYdS * static_cast<float>(Drill::kChaseStepMs) / 1000.f;
        std::uint32_t offMs = 0;
        for (std::uint32_t k = 1; k <= Drill::ChaseSteps(spec->ms); ++k)
        {
            if (std::optional<Vec3> const p = Drill::ChaseRelocation(start, boss, tank, kMeleeRangeYd, kStopYd, step,
                spec->yd))
                boss = *p;
            if (!offMs && Dist(boss, start) > Drill::kBlessedGroundYd)
                offMs = k * Drill::kChaseStepMs;
        }
        CHECK(offMs > 0 && offMs <= Drill::kBlessedGroundWarnMs + 1000u);
        CHECK(Dist(tank, start) > Drill::kBlessedGroundYd + kMarginYd);
    }

    void TestCloseRanks()
    {
        // MA4 heroic on NAVMESH numbers (§7 #30 A): the band 5-11 yd round him (not 6-14), 8 s, 12% a second, on him
        std::vector<At> const ranks = WithTag(0, Drill::kSm2TagCloseRanks);
        CHECK(ranks.size() == 1);
        std::vector<At> const walks = WithShape(0, "walk");
        CHECK(walks.size() == 2);
        if (ranks.empty() || walks.size() != 2)
            return;
        At const& r = ranks[0];
        CHECK(ShapeOf(r.line) == "donut" && Is(r.line, "anchor", "target"));
        CHECK_NEAR(Size(r.line), Drill::kRanksOuterYd, kEps);
        CHECK_NEAR(Key(r.line, "inner", 0.f), Drill::kRanksInnerYd, kEps);
        CHECK(Drill::WarnMs(r.line) == Drill::kRanksWarnMs);
        CHECK(Key(r.line, "persist", 0.f) == static_cast<float>(Drill::kRanksMs));
        CHECK(Key(r.line, "pct", 0.f) == 12.f && !Drill::Value(r.line, "weak"));    // lingering: no Weakened
        // "a boss that steps": 4 yd out along the run's facing and 4 yd back, both inside the band's life
        std::optional<Drill::WalkStep> const out = Drill::ParseWalk(Drill::StripPlanKeys(walks[0].line));
        std::optional<Drill::WalkStep> const back = Drill::ParseWalk(Drill::StripPlanKeys(walks[1].line));
        CHECK(out && back);
        if (!out || !back)
            return;
        CHECK_NEAR(out->yd, Drill::kRanksStepYd, kEps);
        CHECK_NEAR(back->yd, Drill::kRanksStepYd, kEps);
        CHECK(out->offsetDeg == 0.f && back->offsetDeg == 180.f);
        CHECK(out->ms == Drill::kRanksStepMs && back->ms == Drill::kRanksStepMs);
        CHECK(walks[0].startMs >= r.resolveMs && walks[0].startMs + out->ms <= walks[1].startMs);
        CHECK(walks[1].startMs + back->ms <= r.endMs);
        // the sim's layout before he steps: the tank inside 5, the DPS and healer beyond 11 (margins kept)
        CHECK(kSimTankYd + kMarginYd <= Drill::kRanksInnerYd);
        CHECK(kSimDpsYd >= Drill::kRanksOuterYd + kMarginYd && kSimHealerYd >= Drill::kRanksOuterYd + kMarginYd);
        // after 4 yd away from the group the tank (and melee) are 7 yd off him, inside the band: they must follow
        float const tankAfter = kSimTankYd + Drill::kRanksStepYd;
        CHECK(tankAfter > Drill::kRanksInnerYd && tankAfter < Drill::kRanksOuterYd);
        // the step is slower than a run: 2 yd/s
        CHECK(Drill::kRanksStepYd / (static_cast<float>(Drill::kRanksStepMs) / 1000.f) < kRunYdS);
    }

    // ---- cycle 1: Whitemane (vigil-flame), Crell (blade-window) -------------------------------------------------

    void TestVigilFlame()
    {
        std::vector<At> const flares = WithTag(1, Drill::kSm2TagFlame);
        std::vector<At> const seared = WithTag(1, Drill::kSm2TagSeared);
        std::vector<At> const markers = WithShape(1, "marker");
        CHECK(flares.size() == Drill::kFlares && seared.size() == Drill::kFlares);
        CHECK(markers.size() == Drill::kFlares + 1);
        if (flares.size() != Drill::kFlares || seared.size() != Drill::kFlares || markers.size() != Drill::kFlares + 1)
            return;
        for (std::uint32_t k = 0; k < Drill::kFlares; ++k)
        {
            At const& f = flares[k];
            // a flare every 10 s: the next carrier marked just after the last flare resolved
            CHECK(f.resolveMs == (k + 1) * Drill::kFlameEveryMs);
            CHECK(f.startMs == k * Drill::kFlameEveryMs + Drill::kFlameRegisterLagMs);
            CHECK(markers[k].startMs == f.startMs && Size(markers[k].line) == 1.f);
            CHECK(Is(markers[k].line, "pick", "nontank"));
            // the 1-body soak follows the carrier, the unmarked eligible (no Seared, not the carrier), the tank last
            CHECK(ShapeOf(f.line) == "soak" && Is(f.line, "anchor", "member") && Is(f.line, "pick", "same"));
            CHECK_NEAR(Size(f.line), Drill::kFlameYd, kEps);
            CHECK(Key(f.line, "need", 0.f) == 1.f && Is(f.line, "last", "tank"));
            CHECK(Is(f.line, "aura", "205009:0:0"));
            // the Seared (Marked) players judged within 6 yd of the carrier at the flare (the carrier spared)
            At const& s = seared[k];
            CHECK(s.startMs == f.startMs && s.resolveMs == f.resolveMs);
            CHECK(ShapeOf(s.line) == "circle" && Is(s.line, "anchor", "member") && Is(s.line, "pick", "same"));
            CHECK_NEAR(Size(s.line), Drill::kFlameYd, kEps);
            CHECK(Is(s.line, "aura", "205009:1") && Key(s.line, "pct", -1.f) == 0.f);
        }
        CHECK(Size(markers.back().line) == 0.f && markers.back().startMs > flares.back().resolveMs);

        // The loop on a 5-man group: four non-tanks, each carrier stays Marked (Seared) for the cycle; the eligible
        // non-tanks at flare k are the unmarked ones, so the fourth flare leaves only the tank (F7: roleLast, the
        // tank shares only when no other can).
        for (std::uint32_t k = 0; k < Drill::kFlares; ++k)
        {
            std::uint32_t const eligibleNonTanks = 4u - (k + 1u);
            CHECK((eligibleNonTanks == 0) == (k + 1 == Drill::kFlares));
        }
        // the tank stays on Mograine (8 yd) over the first three flares' windows, never into the fourth
        std::vector<At> const leash = WithTag(1, Drill::kSm2TagFlameTank);
        CHECK(leash.size() == 1);
        if (leash.empty())
            return;
        std::optional<Drill::LeashStep> const l = Drill::ParseLeash(Drill::StripPlanKeys(leash[0].line));
        CHECK(l && l->role == Drill::LeashRole::Tank && l->anchor == Drill::LeashAnchor::Unit && !l->near);
        if (!l)
            return;
        CHECK_NEAR(l->yd, Drill::kFlameTankLeashYd, kEps);
        CHECK(leash[0].startMs == flares[0].startMs);
        CHECK(leash[0].startMs + l->ms <= flares[2].resolveMs && leash[0].startMs + l->ms < flares[3].startMs);
        CHECK(kSimTankYd + kMarginYd < l->yd);
    }

    void TestBladeWindow()
    {
        // AR7 heroic: the Issue at 41 s; the Blade's 6 yd spread (F15) from the Issue for the Blade's 15 s, judged
        // every second (pct 0: a ledger hit, reported); chains of five dashes at Issue + 2 and Issue + 12.
        std::vector<At> const spread = WithTag(1, Drill::kSm2TagBladeSpread);
        CHECK(spread.size() == 1);
        if (spread.empty())
            return;
        At const& s = spread[0];
        CHECK(ShapeOf(s.line) == "spread" && Key(s.line, "pct", -1.f) == 0.f);
        CHECK_NEAR(Size(s.line), Drill::kBladeSpreadYd, kEps);
        CHECK(s.endMs == s.startMs + Drill::kBladeMs);
        CHECK(Key(s.line, "tick", 0.f) == 1000.f);

        std::vector<At> const firsts = WithTag(1, Drill::kSm2TagStrike);
        std::vector<At> const nexts = WithTag(1, Drill::kSm2TagStrikeNext);
        CHECK(firsts.size() == 2 && nexts.size() == 8);
        if (firsts.size() != 2 || nexts.size() != 8)
            return;
        for (std::size_t c = 0; c < 2; ++c)
        {
            At const& f = firsts[c];
            CHECK(f.startMs
                == s.startMs + Drill::kStrikeFirstMs + static_cast<std::uint32_t>(c) * Drill::kStrikeEveryMs);
            CHECK(Drill::WarnMs(f.line) == Drill::kStrikeFirstMs);
            CHECK(Is(f.line, "anchor", "target") && Is(f.line, "to", "member") && Is(f.line, "pick", "nontank"));
            CHECK(Key(f.line, "pct", 0.f) == 45.f && Is(f.line, "weak", "1"));
            std::uint32_t at = f.resolveMs;
            int tanks = 0;
            for (std::size_t i = 0; i < 4; ++i)
            {
                At const& n = nexts[c * 4 + i];
                // each from where the last ended, 1 s, the next not-yet-tested player; the tank last
                CHECK(n.startMs == at && Drill::WarnMs(n.line) == Drill::kStrikeNextMs);
                at = n.resolveMs;
                CHECK(Is(n.line, "at", "last") && Is(n.line, "to", "member"));
                CHECK(Is(n.line, "pick", i < 3 ? "other" : "tank"));
                tanks += Is(n.line, "pick", "tank") ? 1 : 0;
                CHECK(Key(n.line, "pct", 0.f) == 30.f && Is(n.line, "weak", "1"));
            }
            CHECK(tanks == 1);
            // five dashes: four non-tanks and the tank, every player once
            CHECK(1u + 4u == Drill::kStrikeDashes);
            CHECK(at - f.startMs == Drill::kStrikeFirstMs + 4u * Drill::kStrikeNextMs);
        }
        // the second chain still starts inside the Blade's 15 s
        CHECK(firsts[1].startMs < s.startMs + Drill::kBladeMs);
        for (At const& d : firsts)
        {
            CHECK_NEAR(Key(d.line, "width", 0.f), Drill::kStrikeWidth, kEps);
            CHECK(Is(d.line, "reach", "1") && Key(d.line, "past", 0.f) == Drill::kStrikePastYd);
        }
        for (At const& d : nexts)
        {
            CHECK_NEAR(Key(d.line, "width", 0.f), Drill::kStrikeWidth, kEps);
            CHECK(Is(d.line, "reach", "1") && Key(d.line, "past", 0.f) == Drill::kStrikePastYd);
        }
        // A target steps sideways out of a 3 yd dash in 1 s; spread 6 yd apart, a dash at one player misses a
        // neighbor 6 yd to its side.
        CHECK((Drill::kStrikeWidth / 2.f + kMarginYd) / kRunYdS + kReactS <= Drill::kStrikeNextMs / 1000.f);
        CHECK(Drill::kBladeSpreadYd > Drill::kStrikeWidth / 2.f + kMarginYd);
    }

    // ---- cycle 2: Herod (herod-lap, turning-beams) --------------------------------------------------------------

    // Checks a lap: the band, the out lane, `blades` throws (pairs when `pair`), the home lane, all on kPit.
    void CheckLap(std::uint32_t yellMs, float ydPerS, float startDeg, int dir, std::uint32_t bladeTag, float pct,
        bool pair, std::uint32_t throws)
    {
        float const lapMs = Drill::LapMs(Drill::kLapRadiusYd, ydPerS);
        // the band: from the yell (bots go down at T: the out lane's 2.5 s), closed for the lap
        std::optional<At> band;
        for (At const& a : WithTag(2, Drill::kSm2TagLapBand))
            if (a.startMs == yellMs)
                band = a;
        CHECK(band.has_value());
        if (!band)
            return;
        CHECK(ShapeOf(band->line) == "donut");
        CHECK_NEAR(Key(band->line, "inner", 0.f), Drill::kPitYd, kEps);
        CHECK_NEAR(Size(band->line), Drill::kBandOuterYd, kEps);
        Vec3 const bandAt = ShiftOf(band->line);
        CHECK_NEAR(bandAt.x, kPit.x, kEps);
        CHECK_NEAR(bandAt.y, kPit.y, kEps);
        CHECK(Drill::WarnMs(band->line) == Drill::kOutWarnMs);
        CHECK(std::fabs(Key(band->line, "persist", 0.f) - lapMs) <= 50.f);
        CHECK(Is(band->line, "once", "1"));
        // the sim's DPS (15 yd off the pit's center) and healer (21) go down inside the warning; the tank (6) is in
        float const healerOut = kSimHealerYd + Drill::kPitAheadYd;
        CHECK((healerOut - (Drill::kPitYd - kMarginYd)) / kRunYdS + kReactS <= Drill::kOutWarnMs / 1000.f);
        CHECK(kSimDpsYd + Drill::kPitAheadYd > Drill::kPitYd);
        CHECK(kSimTankYd + Drill::kPitAheadYd < Drill::kPitYd);

        // out: a 10 yd lane from the pit's center to the lap's start on the ledge, with the band
        std::optional<At> out;
        std::optional<At> home;
        for (At const& a : WithTag(2, Drill::kSm2TagLapOut))
            if (a.startMs == yellMs)
                out = a;
        for (At const& a : WithTag(2, Drill::kSm2TagLapHome))
            if (a.startMs == band->endMs)
                home = a;
        CHECK(out.has_value() && home.has_value());
        if (!out || !home)
            return;
        CHECK(Drill::WarnMs(out->line) == Drill::kOutWarnMs && Drill::WarnMs(home->line) == Drill::kHomeWarnMs);
        for (At const* lane : { &*out, &*home })
        {
            CHECK_NEAR(Key(lane->line, "width", 0.f), Drill::kLaneWidth, kEps);
            CHECK_NEAR(Size(lane->line), Drill::kLapRadiusYd, kEps);
            CHECK(Key(lane->line, "pct", 0.f) == pct && Is(lane->line, "weak", "1"));
        }
        float const startRad = Geo::DegToRad(startDeg);
        Vec3 const ledge = Dir(startRad, Drill::kLapRadiusYd, kPit);
        Vec3 const outSrc = LineSource(out->line);
        CHECK_NEAR(Dist(outSrc, kPit), 0.f, kShiftEps);
        Vec3 const outEnd = Drill::LineEnd(outSrc, Geo::DegToRad(Key(out->line, "offset", 0.f)),
            Size(out->line));
        CHECK_NEAR(Dist(outEnd, ledge), 0.f, kShiftEps);
        // home: from where the lap ends (one full turn: its start) to the pit's center, as the band opens
        Vec3 const homeSrc = LineSource(home->line);
        CHECK_NEAR(Dist(homeSrc, ledge), 0.f, kShiftEps);
        Vec3 const homeEnd = Drill::LineEnd(homeSrc, Geo::DegToRad(Key(home->line, "offset", 0.f)),
            Size(home->line));
        CHECK_NEAR(Dist(homeEnd, kPit), 0.f, kShiftEps);

        // blades: every 1.5 s of the lap from the cyclone's spot at the throw, 4 x 32 yd across the pit toward a
        // random player (tank included), 1.5 s; each lands before the lap ends
        std::vector<At> blades;
        for (At const& a : WithTag(2, bladeTag))
            if (a.startMs >= band->resolveMs && a.startMs < band->endMs)
                blades.push_back(a);
        CHECK(blades.size() == throws * (pair ? 2u : 1u));
        for (std::size_t i = 0; i < blades.size(); ++i)
        {
            At const& b = blades[i];
            std::uint32_t const k = static_cast<std::uint32_t>(pair ? i / 2 : i);
            CHECK(b.startMs == band->resolveMs + k * Drill::kBladeEveryMs);
            CHECK(b.resolveMs <= band->endMs);
            CHECK(Drill::WarnMs(b.line) == Drill::kBladeWarnMs);
            CHECK(Is(b.line, "to", "member") && Is(b.line, "fix", "1") && !Drill::Value(b.line, "offset"));
            CHECK(Is(b.line, "pick", pair && i % 2 ? "other" : "any"));
            CHECK_NEAR(Key(b.line, "width", 0.f), Drill::kBladeLaneWidth, kEps);
            CHECK_NEAR(Size(b.line), Drill::kBladeLaneYd, kEps);
            CHECK(Is(b.line, "school", "holy") && Key(b.line, "pct", 0.f) == pct && Is(b.line, "weak", "1"));
            float const bearing = Drill::LapBearing(startRad, dir, Drill::kLapRadiusYd, ydPerS,
                static_cast<float>(k * Drill::kBladeEveryMs));
            // the source on the lap circle where the cyclone is (shift= read along the run's facing, then aimed)
            Vec3 const src = Drill::Shifted({ 0.f, 0.f, 0.f }, 0.f, ShiftOf(b.line).x, ShiftOf(b.line).y);
            CHECK_NEAR(Dist(src, Dir(bearing, Drill::kLapRadiusYd, kPit)), 0.f, kShiftEps);
            // long enough to cross the pit's center to its far edge
            CHECK(Size(b.line) >= Drill::kLapRadiusYd + Drill::kPitYd - 0.5f);
        }
        // the lap: about 10 s at 12 yd/s, 8.6 s at 14 (HB1b)
        CHECK(band->endMs - band->resolveMs == static_cast<std::uint32_t>(Key(band->line, "persist", 0.f)));
        // a blade is a 4 yd lane: a player in the pit steps out sideways in its 1.5 s
        CHECK((Drill::kBladeLaneWidth / 2.f + kMarginYd) / kRunYdS + kReactS <= Drill::kBladeWarnMs / 1000.f);
    }

    void TestHerodLap()
    {
        // the 121 yd lap (NAVMESH): r 19.3 yd; 12 yd/s heroic about 10.1 s, 14 yd/s hard mode about 8.7 s
        CHECK_NEAR(Geo::kTwoPi * Drill::kLapRadiusYd, 121.f, 0.5f);
        CHECK_NEAR(Drill::LapMs(Drill::kLapRadiusYd, Drill::kLapHeroicYdS), 10105.f, 5.f);
        CHECK_NEAR(Drill::LapMs(Drill::kLapRadiusYd, Drill::kLapHardYdS), 8662.f, 5.f);
        // the pit r 13.1 inside the lap's ledge band
        CHECK(Drill::kPitYd <= 13.1f && Drill::kLapRadiusYd > Drill::kPitYd + Drill::kLaneWidth / 2.f);
        CHECK_NEAR(Drill::LapBearing(0.f, 1, 10.f, 10.f, 1000.f), 1.f, kEps);
        CHECK_NEAR(Drill::LapBearing(0.f, -1, 10.f, 10.f, 1000.f), Geo::kTwoPi - 1.f, kEps);
        // heroic: at the cycle's start, counterclockwise from the run's left, one blade every 1.5 s, 45%
        CheckLap(0, Drill::kLapHeroicYdS, 90.f, 1, Drill::kSm2TagBlade, 45.f, false, Drill::kLapBlades);
        // hard mode, at the beams' flip: clockwise from the run's right, two blades at once, 60%
        CheckLap(34000, Drill::kLapHardYdS, 270.f, -1, Drill::kSm2TagBladeHard, 60.f, true, Drill::kLapBladesHard);
        // the two laps share the out, band and home tags
        CHECK(WithTag(2, Drill::kSm2TagLapBand).size() == 2 && WithTag(2, Drill::kSm2TagLapOut).size() == 2);
        CHECK(WithTag(2, Drill::kSm2TagLapHome).size() == 2);
    }

    void TestTurningBeams()
    {
        std::vector<At> const beams = WithTag(2, Drill::kSm2TagBeam);
        CHECK(beams.size() == 2u * Drill::kBeamSteps);
        if (beams.size() != 2u * Drill::kBeamSteps)
            return;
        std::uint32_t const first = beams.front().startMs;
        CHECK(first > WithTag(2, Drill::kSm2TagLapHome).front().resolveMs);      // after the heroic lap
        std::vector<float> bearings;
        for (std::uint32_t k = 0; k < Drill::kBeamSteps; ++k)
        {
            At const& a = beams[2 * k];
            At const& b = beams[2 * k + 1];
            // one step every 1.5 s, each marked 1.5 s ahead: it lands as the next is marked
            CHECK(a.startMs == first + k * Drill::kBeamStepMs && b.startMs == a.startMs);
            CHECK(Drill::WarnMs(a.line) == Drill::kBeamStepMs && Drill::WarnMs(b.line) == Drill::kBeamStepMs);
            for (At const* beam : { &a, &b })
            {
                std::string_view const line = beam->line;
                CHECK(ShapeOf(line) == "line" && !Drill::Value(line, "to") && !Drill::Value(line, "at"));
                CHECK_NEAR(Size(line), Drill::kBeamYd, kEps);
                CHECK_NEAR(Key(line, "width", 0.f), Drill::kBeaconWidth, kEps);
                CHECK(Is(line, "school", "holy") && Key(line, "pct", 0.f) == 45.f && Is(line, "weak", "1"));
                // from 3 yd off the planted axe along its bearing, to the pit's edge
                float const rad = Geo::DegToRad(Key(line, "offset", 0.f));
                Vec3 const src = LineSource(line);
                CHECK_NEAR(Dist(src, Dir(rad, Drill::kBeamInnerYd, kPit)), 0.f, kShiftEps);
                Vec3 const end = Drill::LineEnd(src, rad, Size(line));
                CHECK_NEAR(Dist(end, kPit), Drill::kBeamInnerYd + Drill::kBeamYd, kShiftEps);
                CHECK(Drill::kBeamInnerYd + Drill::kBeamYd <= 13.1f);
                // the matching shift helper agrees
                Vec3 const off{ src.x, src.y, 0.f };
                Vec3 const turned = Drill::ShiftInTurnedFrame(off, rad);
                Vec3 const sh = ShiftOf(line);
                CHECK_NEAR(turned.x, sh.x, kShiftEps);
                CHECK_NEAR(turned.y, sh.y, kShiftEps);
            }
            // opposite each other
            float const da = std::fmod(Key(b.line, "offset", 0.f) - Key(a.line, "offset", 0.f) + 720.f, 360.f);
            CHECK_NEAR(da, 180.f, kEps);
            bearings.push_back(Key(a.line, "offset", 0.f));
        }
        // 30 degrees a step: 12 counterclockwise (a full turn of the pair is 6), then the flip at the hard mode lap's
        // yell, 12 clockwise
        std::uint32_t const flipStep = 12;
        for (std::uint32_t k = 1; k < Drill::kBeamSteps; ++k)
        {
            float const d = std::fmod(bearings[k] - bearings[k - 1] + 720.f, 360.f);
            CHECK_NEAR(d, k < flipStep ? Drill::kBeamStepDeg : 360.f - Drill::kBeamStepDeg, kEps);
        }
        CHECK(beams[2 * flipStep].startMs == 34000u);
        bool flipAtYell = false;
        for (At const& band : WithTag(2, Drill::kSm2TagLapBand))
            flipAtYell = flipAtYell || band.startMs == beams[2 * flipStep].startMs;
        CHECK(flipAtYell);
        // the tank walks him round the pit ahead of the beams: he chases for the beams' whole life, then goes home
        std::vector<At> const chases = WithShape(2, "chase");
        CHECK(chases.size() == 1);
        if (chases.empty())
            return;
        std::optional<Drill::ChaseSpec> const c = Drill::ParseChase(Drill::StripPlanKeys(chases[0].line));
        CHECK(c && c->home && chases[0].startMs == first);
        CHECK(c && chases[0].startMs + c->ms == beams.back().resolveMs);
        CHECK_NEAR(c ? c->yd : 0.f, Drill::kBeamChaseYd, kEps);
        // he kneels 3 yd from the axe: the beams sweep his spot (and the tank's in front of him)
        CHECK(Drill::kPitAheadYd >= Drill::kBeamInnerYd);
        CHECK(Drill::kPitAheadYd + kSimTankYd <= Drill::kBeamInnerYd + Drill::kBeamYd);
    }

    void TestSweepProgram()
    {
        std::string err;
        for (char const* name : { "scarlet-monastery-shapes-2:3", "scarlet-monastery-shapes-2" })
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
    TestRepeatOffsets();
    TestFixLinePast();
    TestLeashSelfAndHoldFacing();
    TestSowPrune();
    TestBlessedGround();
    TestCloseRanks();
    TestVigilFlame();
    TestBladeWindow();
    TestHerodLap();
    TestTurningBeams();
    TestSweepProgram();
    return Report("sm2_drill_test");
}
