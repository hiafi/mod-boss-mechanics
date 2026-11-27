// Standalone tests for the Gnomeregan BM-G additions (HANDOFF §3.11, §7 #12 A, F19, F20, F41): the move cost
// (MoveCostEntry, Desc::MoveCostly; the lock's LockRules::MoveCostRefusesMover and the brain's
// LockRules::MoveCostStepOf; the solver's Input::moveCost, the shortest way out), the preferred band (RangeBandEntry,
// Desc::RangeBandFor; LockRules::BandGapYd, BandWalkDue, BandWalkGains) and the minimum-stacks claim
// (Ordering::minStacks through Assign::SoakRule::minStacks and Assign::SoakClaimers). Build: tests/run.sh.

#include "check.h"
#include "MechanicAssign.h"
#include "MechanicDescriptor.h"
#include "MechanicDescriptorPresets.h"
#include "MechanicLockRules.h"
#include "MechanicSolver.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

using namespace BossMechanics;
using namespace BossMechanics::Desc;
using Geo::Pose;
using Geo::Vec3;
using Ms = std::chrono::milliseconds;

namespace
{
    constexpr std::uint32_t SPELL_STATIC = 214300;          // a stand-in id in the build's spell block
    constexpr std::uint32_t SPELL_IRRADIATED = 214301;      // the dose's stand-in
    constexpr std::uint32_t kStaticAbove = 12;              // §7 #12 A: "above 12"

    BotFacts Bot(bool tank, bool healer, bool melee)
    {
        BotFacts b;
        b.tank = tank;
        b.healer = healer;
        b.melee = melee;
        b.mainTank = tank;
        return b;
    }

    BotFacts const kTank = Bot(true, false, true);
    BotFacts const kHealer = Bot(false, true, false);
    BotFacts const kMeleeDps = Bot(false, false, true);
    BotFacts const kRangedDps = Bot(false, false, false);

    // ---- F19: MoveCostEntry, Desc::MoveCostly ----

    void TestMoveCostly()
    {
        std::map<std::uint32_t, std::uint32_t> stacks;
        auto stacksOf = [&stacks](std::uint32_t aura) { return stacks.count(aura) ? stacks[aura] : 0u; };

        std::vector<MoveCostEntry> none;
        stacks[SPELL_STATIC] = 200;
        CHECK(!MoveCostly(none, stacksOf));

        std::vector<MoveCostEntry> entries;
        entries.emplace_back(SPELL_STATIC, kStaticAbove);
        stacks[SPELL_STATIC] = 0;
        CHECK(!MoveCostly(entries, stacksOf));
        stacks[SPELL_STATIC] = 12;
        CHECK(!MoveCostly(entries, stacksOf));     // "above 12": 12 is not
        stacks[SPELL_STATIC] = 13;
        CHECK(MoveCostly(entries, stacksOf));
        stacks[SPELL_STATIC] = 20;
        CHECK(MoveCostly(entries, stacksOf));

        // An entry with aura 0 does nothing; aboveStacks 0 means any stack; any entry that holds is enough.
        std::vector<MoveCostEntry> mixed;
        mixed.emplace_back(0, 0);
        mixed.emplace_back(SPELL_IRRADIATED, 0);
        stacks.clear();
        CHECK(!MoveCostly(mixed, stacksOf));
        stacks[SPELL_IRRADIATED] = 1;
        CHECK(MoveCostly(mixed, stacksOf));
        mixed.emplace_back(SPELL_STATIC, kStaticAbove);
        stacks.clear();
        stacks[SPELL_STATIC] = 13;
        CHECK(MoveCostly(mixed, stacksOf));

        // Default constructed: nothing.
        MoveCostEntry const e;
        CHECK(e.auraId == 0 && e.aboveStacks == 0);
    }

    // ---- F20: RangeBandEntry, Desc::RangeBandFor ----

    void TestRangeBand()
    {
        std::vector<RangeBandEntry> bands;
        CHECK(!RangeBandFor(bands, kRangedDps, true));
        bands.emplace_back(11.f, 12.f);

        // Only a bot whose role band is a ranged one takes it; Any covers ranged damage dealers and healers.
        CHECK(RangeBandFor(bands, kRangedDps, true) == &bands[0]);
        CHECK(RangeBandFor(bands, kHealer, true) == &bands[0]);
        CHECK(!RangeBandFor(bands, kMeleeDps, false));
        CHECK(!RangeBandFor(bands, kTank, false));
        CHECK(!RangeBandFor(bands, kRangedDps, false));

        // For(): one role only; the first entry that fits wins.
        std::vector<RangeBandEntry> split;
        split.push_back(RangeBandEntry(11.f, 12.f).For(Who::Ranged));
        split.push_back(RangeBandEntry(14.f, 18.f).For(Who::Healer));
        CHECK(RangeBandFor(split, kRangedDps, true) == &split[0]);
        CHECK(RangeBandFor(split, kHealer, true) == &split[1]);

        // Invalid bands are skipped (and the next valid one taken).
        std::vector<RangeBandEntry> bad;
        bad.emplace_back(12.f, 11.f);
        bad.emplace_back(-1.f, 5.f);
        bad.emplace_back(5.f, 5.f);
        bad.emplace_back(std::nanf(""), 5.f);
        CHECK(!RangeBandFor(bad, kRangedDps, true));
        bad.emplace_back(11.f, 12.f);
        CHECK(RangeBandFor(bad, kRangedDps, true) == &bad[4]);
        CHECK(RangeBandValid(RangeBandEntry(0.f, 1.f)));
        CHECK(!RangeBandValid(RangeBandEntry()));

        // F-2 (RF-1 Minor-4): KeepOut clips floor out of the band; a spot inside is out of the band by its depth.
        RangeBandEntry clipped(19.f, 26.f);
        clipped.KeepOut(10.f, 0.f, 4.f).KeepOut(0.f, 10.f, 0.f).KeepOut(0.f, 20.f, std::nanf(""));
        CHECK(clipped.keepOut.size() == 3);
        CHECK(RangeBandValid(clipped));
        CHECK_NEAR(RangeBandKeepOutYd(clipped.keepOut, 10.f, 0.f), 4.f, 1e-5);
        CHECK_NEAR(RangeBandKeepOutYd(clipped.keepOut, 12.f, 0.f), 2.f, 1e-5);
        CHECK_NEAR(RangeBandKeepOutYd(clipped.keepOut, 14.f, 0.f), 0.f, 1e-6);    // on the edge: outside
        CHECK_NEAR(RangeBandKeepOutYd(clipped.keepOut, 0.f, 10.f), 0.f, 1e-6);    // a zero circle does nothing
        CHECK_NEAR(RangeBandKeepOutYd(clipped.keepOut, 0.f, 20.f), 0.f, 1e-6);    // ...nor a NaN one
        CHECK_NEAR(RangeBandKeepOutYd({}, 10.f, 0.f), 0.f, 1e-6);
        CHECK(RangeBandEntry(1.f, 2.f).keepOut.empty());
    }

    void TestDescriptorEmpty()
    {
        Descriptor d;
        CHECK(d.Empty());
        d.moveCosts.emplace_back(SPELL_STATIC, kStaticAbove);
        CHECK(!d.Empty());
        Descriptor b;
        b.rangeBands.emplace_back(11.f, 12.f);
        CHECK(!b.Empty());

        // The drill preset: Static's shape on Marked (drill), the 11-12 yd band.
        Preset const* p = FindPreset("move-cost");
        CHECK(p != nullptr);
        if (p)
        {
            Descriptor const m = p->build();
            CHECK(m.moveCosts.size() == 1 && m.moveCosts[0].auraId == Ids::SPELL_MARKED_DRILL
                && m.moveCosts[0].aboveStacks == DrillPresets::kMoveCostDrillAbove);
            CHECK(DrillPresets::kMoveCostDrillAbove < Ids::MARKED_DRILL_MAX_STACKS);
            CHECK(m.rangeBands.size() == 1 && m.rangeBands[0].minYd == 11.f && m.rangeBands[0].maxYd == 12.f);
        }
    }

    // ---- F19: the lock and the brain's step ----

    void TestMoveCostLock()
    {
        using LockRules::MoveCostRefusesMover;
        CHECK(!MoveCostRefusesMover(false, true, "reach melee"));
        CHECK(MoveCostRefusesMover(true, true, "reach melee"));
        CHECK(MoveCostRefusesMover(true, true, "reach spell"));
        CHECK(MoveCostRefusesMover(true, true, "follow"));
        CHECK(MoveCostRefusesMover(true, true, "tank face"));
        CHECK(MoveCostRefusesMover(true, true, "set behind"));
        CHECK(MoveCostRefusesMover(true, true, "heroic leap"));
        CHECK(!MoveCostRefusesMover(true, true, "avoid aoe"));      // a dodge still goes
        CHECK(!MoveCostRefusesMover(true, false, "attack"));        // not a mover
        CHECK(LockRules::IsMoveCostDodgeName("avoid aoe"));
        CHECK(!LockRules::IsMoveCostDodgeName("flee"));
    }

    void TestMoveCostStep()
    {
        using LockRules::MoveCostStep;
        using LockRules::MoveCostStepOf;
        // No cost, released, or an unsafe spot: the brain goes on as before (the solver answers an unsafe spot).
        CHECK(MoveCostStepOf(false, false, true, true, true) == MoveCostStep::None);
        CHECK(MoveCostStepOf(true, true, true, false, true) == MoveCostStep::None);
        CHECK(MoveCostStepOf(true, false, false, true, true) == MoveCostStep::None);
        CHECK(MoveCostStepOf(true, false, false, false, true) == MoveCostStep::None);
        // Safe: an escape stops on the spot, a foreign motion stops, else the bot stands.
        CHECK(MoveCostStepOf(true, false, true, true, false) == MoveCostStep::StopEscape);
        CHECK(MoveCostStepOf(true, false, true, true, true) == MoveCostStep::StopEscape);
        CHECK(MoveCostStepOf(true, false, true, false, true) == MoveCostStep::StopMotion);
        CHECK(MoveCostStepOf(true, false, true, false, false) == MoveCostStep::Stand);
        CHECK(LockRules::kMoveCostHoldMs > 0 && LockRules::kMoveCostDodgeMs >= 1000);
    }

    // ---- F20: the band's walk rules ----

    void TestBandRules()
    {
        using namespace LockRules;
        CHECK_NEAR(BandGapYd(11.5f, 11.f, 12.f), 0.f, 1e-6);
        CHECK_NEAR(BandGapYd(11.f, 11.f, 12.f), 0.f, 1e-6);
        CHECK_NEAR(BandGapYd(12.f, 11.f, 12.f), 0.f, 1e-6);
        CHECK_NEAR(BandGapYd(25.f, 11.f, 12.f), 13.f, 1e-6);
        CHECK_NEAR(BandGapYd(4.f, 11.f, 12.f), 7.f, 1e-6);

        std::int64_t const t0 = 100000;
        CHECK(!BandWalkDue(t0, 0, 0, 0));                           // in the band
        CHECK(!BandWalkDue(t0 + kBandSlackMs - 1, t0, 0, 0));       // not out long enough
        CHECK(BandWalkDue(t0 + kBandSlackMs, t0, 0, 0));
        CHECK(!BandWalkDue(t0 + kBandSlackMs, t0, t0 + kBandSlackMs - 10, 0));     // checked just now
        CHECK(BandWalkDue(t0 + kBandSlackMs + kBandCheckEveryMs, t0, t0 + kBandSlackMs, 0));
        CHECK(!BandWalkDue(t0 + 3000, t0, 0, t0 + 3000 + kBandBackoffMs));         // backed off

        CHECK(BandWalkGains(13.f, 0.f));
        CHECK(BandWalkGains(13.f, 12.f));
        CHECK(!BandWalkGains(13.f, 12.5f));
        CHECK(!BandWalkGains(0.5f, 0.2f));
        CHECK(BandWalkGains(0.5f, 0.f));
    }

    // ---- F19: the shortest way out (Solver::Input::moveCost) ----

    struct StraightOracle : Solver::PathOracle
    {
        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            out = { from, to };
            return true;
        }
    };

    Ms const kNow{ 100000 };

    Solver::Input InCircle(bool moveCost)
    {
        Solver::Input in;
        in.bot = { 2.f, 0.f, 0.f };
        in.now = kNow;
        in.speed = 7.f;
        Solver::Hazard h;
        h.id = 1;
        h.shape = Shape::Circle(6.f);
        h.poses = { Pose{} };
        h.marginYd = 0.75f;
        h.timing = Solver::MakeInstant(kNow + Ms(3000), Ms(500));
        in.hazards.push_back(h);
        // The band (11-12 yd round an anchor 20 yd west) pulls the bot out by the far, west side.
        in.role.present = true;
        in.role.anchor = { -20.f, 0.f, 0.f };
        in.role.minYd = 11.f;
        in.role.maxYd = 12.f;
        in.moveCost = moveCost;
        return in;
    }

    void TestShortestWayOut()
    {
        StraightOracle oracle;
        Solver::Plan const free = Solver::Solve(InCircle(false), oracle);
        CHECK(free.outcome == Solver::Outcome::Move);
        CHECK(free.target.x < -6.75f);                              // toward its band, through the circle's west
        Solver::Plan const costly = Solver::Solve(InCircle(true), oracle);
        CHECK(costly.outcome == Solver::Outcome::Move);
        CHECK(costly.target.x > 6.75f);                             // the near side
        CHECK(std::hypot(costly.target.x - 2.f, costly.target.y) < 7.f);
        CHECK(std::hypot(costly.target.x - 2.f, costly.target.y)
            < std::hypot(free.target.x - 2.f, free.target.y));

        // Spacing (6 yd from standing members) and reach are dropped too: a member standing just east of the near
        // exit does not push a costly bot round the circle.
        Solver::Input spaced = InCircle(true);
        spaced.role.present = false;
        spaced.standing.push_back({ 9.f, 0.f, 0.f });
        spaced.reach.maxYd = 5.f;
        spaced.reach.weightPerYd = 1.f;
        spaced.reach.points.push_back({ -30.f, 0.f, 0.f });
        Solver::Plan const p = Solver::Solve(spaced, oracle);
        CHECK(p.outcome == Solver::Outcome::Move);
        CHECK(p.target.x > 6.75f);
        CHECK(std::hypot(p.target.x - 2.f, p.target.y) < 7.f);

        // A safe spot stays Safe under the cost (nothing to answer).
        Solver::Input safe = InCircle(true);
        safe.bot = { 15.f, 0.f, 0.f };
        CHECK(Solver::Solve(safe, oracle).outcome == Solver::Outcome::Safe);
    }

    // ---- F-2 (RF-1 Minor-4), RF-2 Nit-3: the band walk's strict keep-out through the solver ----

    // MechanicBotBrain.cpp StepRangeBand's role solve: the band (19-26 yd round the owner at the origin) is the role
    // band, and the keep-out rides as one areas-less strict Bounds (anywhere outside its circles). The circle mirrors
    // Crowd Pummeler's blind corner: 23.8 yd out, 7.75 yd across, so it covers the band's full depth in its sector.
    Vec3 const kKeepOutAt{ 23.8f, 0.f, 0.f };
    float constexpr kKeepOutYd = 7.75f;

    Solver::Input BandWalk(Vec3 const& bot, bool keepOut)
    {
        Solver::Input in;
        in.bot = bot;
        in.now = kNow;
        in.speed = 7.f;
        in.role.present = true;
        in.role.anchor = { 0.f, 0.f, 0.f };
        in.role.minYd = 19.f;
        in.role.maxYd = 26.f;
        in.roleSolve = true;
        if (keepOut)
        {
            Solver::Bounds b;
            b.strict = true;
            b.keepOut.push_back({ kKeepOutAt, kKeepOutYd, false });
            in.bounds.push_back(b);
        }
        return in;
    }

    float BandGap(Vec3 const& p)
    {
        std::vector<RangeBandEntry::KeepOutCircle> const k{ { kKeepOutAt.x, kKeepOutAt.y, kKeepOutYd } };
        return std::max(LockRules::BandGapYd(std::hypot(p.x, p.y), 19.f, 26.f), RangeBandKeepOutYd(k, p.x, p.y));
    }

    void TestBandKeepOutSolve()
    {
        StraightOracle oracle;
        // The bot at the circle's center (the whole band sector round it is clipped): it walks to the clear band.
        for (Vec3 const& bot : std::vector<Vec3>{ kKeepOutAt, { 22.f, 2.f, 0.f }, { 25.f, -3.f, 0.f } })
        {
            CHECK(BandGap(bot) > 0.f);
            Solver::Plan const p = Solver::Solve(BandWalk(bot, true), oracle);
            CHECK(p.outcome == Solver::Outcome::Move);
            CHECK(std::hypot(p.target.x - kKeepOutAt.x, p.target.y - kKeepOutAt.y) >= kKeepOutYd);
            CHECK_NEAR(BandGap(p.target), 0.f, 0.05);
            CHECK(LockRules::BandWalkGains(BandGap(bot), BandGap(p.target)));
        }
        // Outside the circle and outside the band, beside the clipped sector: the nearest band floor is inside the
        // circle, and no answer ends there.
        Vec3 const beside{ 31.f, 5.f, 0.f };
        CHECK(std::hypot(beside.x - kKeepOutAt.x, beside.y - kKeepOutAt.y) > kKeepOutYd);
        Solver::Plan const p = Solver::Solve(BandWalk(beside, true), oracle);
        CHECK(p.outcome == Solver::Outcome::Move);
        CHECK(std::hypot(p.target.x - kKeepOutAt.x, p.target.y - kKeepOutAt.y) >= kKeepOutYd);
        CHECK_NEAR(BandGap(p.target), 0.f, 0.05);
        // Strict: the bot's own spot inside the circle is no answer, even in the band (no Safe, no stay).
        Solver::Plan const inBand = Solver::Solve(BandWalk({ 22.f, 0.f, 0.f }, true), oracle);
        CHECK(inBand.outcome == Solver::Outcome::Move);
        CHECK(std::hypot(inBand.target.x - kKeepOutAt.x, inBand.target.y - kKeepOutAt.y) >= kKeepOutYd);
        // With no keep-out (Electrocutioner's and the Alert System's bands) the same bot in the band stays: the
        // bounds are what move it.
        Solver::Plan const plain = Solver::Solve(BandWalk({ 22.f, 0.f, 0.f }, false), oracle);
        CHECK(plain.outcome != Solver::Outcome::Move
            || std::hypot(plain.target.x - 22.f, plain.target.y) < 1.f);
    }

    // ---- F41: Ordering::minStacks (Assign::SoakRule::minStacks) ----

    Assign::Claimant C(std::uint64_t guid, std::uint32_t stacks, std::int64_t eta, bool inside = false,
        bool bot = true)
    {
        Assign::Claimant c;
        c.guid = guid;
        c.bot = bot;
        c.insideNow = inside;
        c.stacks = stacks;
        c.etaMs = eta;
        return c;
    }

    void TestMinStacks()
    {
        Assign::SoakRule rule;
        rule.count = 1;
        rule.ordered = true;
        rule.ascending = false;     // the highest dose first
        rule.deadlineMs = 5000;

        // No floor: the highest dose claims (the stopgap order), whatever it is.
        std::vector<Assign::Claimant> players = { C(1, 3, 1000), C(2, 5, 1000), C(3, 0, 500) };
        CHECK((Assign::SoakClaimers(players, rule) == std::vector<std::uint64_t>{ 2 }));

        // A floor of 8: nobody below it claims (inside or not, bot or human), so the plate is left alone.
        rule.minStacks = 8;
        CHECK(Assign::SoakClaimers(players, rule).empty());
        std::vector<Assign::Claimant> inside = { C(4, 7, 0, true, false), C(5, 7, 0, true, true) };
        CHECK(Assign::SoakClaimers(inside, rule).empty());

        // At or above it: claims, the highest first, then the eta.
        players = { C(1, 3, 100), C(2, 8, 3000), C(3, 12, 4000), C(6, 12, 2000) };
        CHECK((Assign::SoakClaimers(players, rule) == std::vector<std::uint64_t>{ 6 }));
        rule.count = 3;
        CHECK((Assign::SoakClaimers(players, rule) == std::vector<std::uint64_t>{ 6, 3, 2 }));

        // A late bot at the floor still fills a slot (best effort, as before); a late one below never does.
        rule.count = 1;
        players = { C(1, 2, 100), C(2, 9, 9000) };
        CHECK((Assign::SoakClaimers(players, rule) == std::vector<std::uint64_t>{ 2 }));

        // With a cap too: only [min, cap) claims.
        rule.capStacks = 10;
        players = { C(1, 11, 100), C(2, 9, 200), C(3, 7, 50) };
        CHECK((Assign::SoakClaimers(players, rule) == std::vector<std::uint64_t>{ 2 }));

        // In an exclusive group (one claim per bot, the plates): the floor holds on every soak.
        rule.capStacks = 0;
        Assign::SoakInput a{ { C(1, 9, 100), C(2, 4, 50) }, rule };
        Assign::SoakInput b{ { C(1, 9, 300), C(2, 4, 10), C(3, 8, 400) }, rule };
        std::vector<std::vector<std::uint64_t>> const out = Assign::ExclusiveSoakClaimers({ a, b });
        CHECK(out.size() == 2);
        if (out.size() == 2)
        {
            CHECK((out[0] == std::vector<std::uint64_t>{ 1 }));
            CHECK((out[1] == std::vector<std::uint64_t>{ 3 }));
        }

        // The default Ordering has no floor (every existing soak unchanged).
        Assign::SoakRule const plain;
        CHECK(plain.minStacks == 0);
    }
}

int main()
{
    TestMoveCostly();
    TestRangeBand();
    TestDescriptorEmpty();
    TestMoveCostLock();
    TestMoveCostStep();
    TestBandRules();
    TestShortestWayOut();
    TestBandKeepOutSolve();
    TestMinStacks();
    return Report("gnomer_bmg_test");
}
