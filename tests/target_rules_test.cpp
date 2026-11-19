// Standalone tests for src/MechanicTargetRules.h (STAGE-M5-HANDOFF §3.5, WP-T): when the focus engages and which
// stock pickers it stands down, the lower-priority clock's test, the ignore list's name pre-check, the minion step
// and the break-out clock, the line-of-sight re-pick (an unseen pick kept) and the sight walk's samples.
// Desc::PickTarget and Desc::IsIgnored themselves are covered by descriptor_test.
// Build: tests/run.sh.

#include "check.h"
#include "MechanicTargetRules.h"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace BossMechanics;
using namespace BossMechanics::Desc;
using namespace BossMechanics::TargetRules;

namespace
{
    constexpr Key kOwner = 1;
    constexpr std::uint32_t kBoss = 300403;
    constexpr std::uint32_t kAdd = 300405;
    constexpr std::uint32_t kCopy = 300406;
    constexpr std::uint32_t kPrison = 300407;
    constexpr std::uint32_t kMarduk = 900001;
    constexpr std::uint32_t kVectus = 900002;
    constexpr std::uint32_t kAiming = 205023;

    UnitFacts Facts(Key guid, std::uint32_t entry, float pct = 100.f, bool alive = true)
    {
        UnitFacts u;
        u.guid = guid;
        u.entry = entry;
        u.healthPct = pct;
        u.alive = alive;
        return u;
    }

    std::vector<TargetCandidate> Candidates(std::vector<UnitFacts> const& facts,
        std::vector<HeldRank> const& held = {})
    {
        std::vector<TargetCandidate> c;
        for (std::size_t i = 0; i < facts.size(); ++i)
            c.push_back({ &facts[i], i < held.size() ? held[i] : HeldRank::None });
        return c;
    }

    BotFacts Dps(bool melee = false)
    {
        BotFacts b;
        b.melee = melee;
        return b;
    }

    BotFacts Tank()
    {
        BotFacts b;
        b.tank = true;
        b.melee = true;
        return b;
    }

    BotFacts Healer()
    {
        BotFacts b;
        b.healer = true;
        return b;
    }

    void TestNoList()
    {
        Descriptor d;
        d.ignores.push_back(IgnoreEntry(kCopy));
        std::vector<UnitFacts> f{ Facts(kOwner, kBoss), Facts(10, kAdd) };
        Focus const r = DecideFocus(d, Candidates(f), kOwner, Dps(), 0, {});
        CHECK(!r.target);
        CHECK(!r.Engaged());
        CHECK(!SuppressPicker(Picker::Assist, r));
    }

    void TestAddOverBoss()
    {
        Descriptor d;
        d.targets.push_back(TargetEntry(kAdd, 2));
        std::vector<UnitFacts> f{ Facts(kOwner, kBoss), Facts(10, kAdd) };
        Focus r = DecideFocus(d, Candidates(f), kOwner, Dps(), kOwner, {});
        CHECK(r.target == 10);
        CHECK(r.priority == 2);
        CHECK(r.outranksBoss);
        CHECK(r.Engaged());
        CHECK(SuppressPicker(Picker::Assist, r));
        CHECK(SuppressPicker(Picker::Aoe, r));
        CHECK(SuppressPicker(Picker::TankAssist, r));
        CHECK(!SuppressPicker(Picker::None, r));
        CHECK(OnLowerTarget(r, kOwner));
        CHECK(!OnLowerTarget(r, 10));
        CHECK(!OnLowerTarget(r, 0));

        // The add dies: back to the boss, nothing engaged, the stock pickers run again.
        f[1].alive = false;
        r = DecideFocus(d, Candidates(f), kOwner, Dps(), 10, {});
        CHECK(r.target == kOwner);
        CHECK(!r.outranksBoss);
        CHECK(!r.Engaged());
        CHECK(!SuppressPicker(Picker::Assist, r));
        CHECK(!OnLowerTarget(r, 10));

        // AoE allowed on the add: the dps aoe picker stays, the others stand down.
        Descriptor a;
        a.targets.push_back(TargetEntry(kAdd, 2).AoeOk());
        f[1].alive = true;
        r = DecideFocus(a, Candidates(f), kOwner, Dps(), 0, {});
        CHECK(r.Engaged());
        CHECK(r.aoeOk);
        CHECK(!SuppressPicker(Picker::Aoe, r));
        CHECK(SuppressPicker(Picker::Assist, r));
    }

    void TestBelowBoss()
    {
        Descriptor d;
        d.targets.push_back(TargetEntry(kAdd, -1));
        std::vector<UnitFacts> f{ Facts(kOwner, kBoss), Facts(10, kAdd) };
        // The bot is on the add: the boss is the pick and the focus engages to move it there.
        Focus r = DecideFocus(d, Candidates(f), kOwner, Dps(), 10, {});
        CHECK(r.target == kOwner);
        CHECK(!r.outranksBoss);
        CHECK(r.lowerListed);
        CHECK(r.Engaged());
        CHECK(OnLowerTarget(r, 10));
        // On the boss it stays engaged (the pickers would pull it back to the add) but the trigger has nothing to do.
        r = DecideFocus(d, Candidates(f), kOwner, Dps(), kOwner, {});
        CHECK(r.Engaged());
        CHECK(!OnLowerTarget(r, kOwner));
        // An ignored add below the boss does not engage the focus.
        d.ignores.push_back(IgnoreEntry(kAdd));
        r = DecideFocus(d, Candidates(f), kOwner, Dps(), kOwner, {});
        CHECK(r.target == kOwner);
        CHECK(!r.Engaged());
        // An unlisted add is the stock pickers' business.
        Descriptor u;
        u.targets.push_back(TargetEntry(kVectus, 3));
        r = DecideFocus(u, Candidates(f), kOwner, Dps(), 10, {});
        CHECK(r.target == kOwner);
        CHECK(!r.Engaged());
    }

    void TestRoles()
    {
        Descriptor d;
        d.targets.push_back(TargetEntry(kAdd, 2));                      // Who::Dps
        std::vector<UnitFacts> f{ Facts(kOwner, kBoss), Facts(10, kAdd) };
        // Tanks and healers keep the stock behavior under damage-dealer entries.
        Focus r = DecideFocus(d, Candidates(f), kOwner, Tank(), kOwner, {});
        CHECK(r.target == kOwner);
        CHECK(!r.Engaged());
        CHECK(!SuppressPicker(Picker::TankAssist, r));
        r = DecideFocus(d, Candidates(f), kOwner, Healer(), 0, {});
        CHECK(!r.Engaged());
        // An entry that names the tank moves it.
        d.targets.push_back(TargetEntry(kAdd, 3, Who::Tank));
        r = DecideFocus(d, Candidates(f), kOwner, Tank(), kOwner, {});
        CHECK(r.target == 10);
        CHECK(r.Engaged());
        CHECK(SuppressPicker(Picker::TankAssist, r));
        // Who::Any names everyone, healers included.
        Descriptor any;
        any.targets.push_back(TargetEntry(kAdd, 1, Who::Any));
        CHECK(DecideFocus(any, Candidates(f), kOwner, Healer(), 0, {}).Engaged());

        // Melee on Marduk, ranged on Vectus (a shared pool split by role).
        Descriptor s;
        s.targets.push_back(TargetEntry(kMarduk, 1, Who::Melee));
        s.targets.push_back(TargetEntry(kVectus, 1, Who::Ranged));
        std::vector<UnitFacts> mv{ Facts(kOwner, kBoss), Facts(20, kMarduk), Facts(21, kVectus) };
        CHECK(DecideFocus(s, Candidates(mv), kOwner, Dps(true), 0, {}).target == 20);
        CHECK(DecideFocus(s, Candidates(mv), kOwner, Dps(false), 0, {}).target == 21);
        CHECK(DecideFocus(s, Candidates(mv), kOwner, Dps(false), 0, {}).Engaged());
    }

    void TestConditionalAndGuid()
    {
        // The aiming add first, while it aims.
        Descriptor d;
        d.targets.push_back(TargetEntry(kAdd, 3, Who::Dps, Condition::HasAura(kAiming)));
        std::vector<UnitFacts> f{ Facts(kOwner, kBoss), Facts(10, kAdd), Facts(11, kAdd) };
        f[2].auras = { kAiming };
        Focus r = DecideFocus(d, Candidates(f), kOwner, Dps(), 10, {});
        CHECK(r.target == 11);
        CHECK(r.Engaged());
        f[2].auras.clear();
        r = DecideFocus(d, Candidates(f), kOwner, Dps(), 11, {});
        CHECK(r.target == kOwner);
        CHECK(!r.Engaged());

        // A phase-2 entry.
        Descriptor p;
        p.targets.push_back(TargetEntry(kAdd, 2, Who::Dps, Condition::Phase(2)));
        Context ctx;
        ctx.phase = 1;
        CHECK(!DecideFocus(p, Candidates(f), kOwner, Dps(), 0, ctx).Engaged());
        ctx.phase = 2;
        CHECK(DecideFocus(p, Candidates(f), kOwner, Dps(), 0, ctx).Engaged());

        // One unit by guid, followed after a swap changed its entry.
        Descriptor g;
        g.targets.push_back(TargetEntry::ForUnit(11, 2));
        f[2].entry = kBoss;
        r = DecideFocus(g, Candidates(f), kOwner, Dps(), kOwner, {});
        CHECK(r.target == 11);
        CHECK(r.Engaged());
    }

    void TestTogetherAndBreakOut()
    {
        // Kill together: the one with more % left, both above the boss.
        Descriptor d;
        d.targets.push_back(TargetEntry(kMarduk, 1).KillTogether(kVectus, 10.f));
        d.targets.push_back(TargetEntry(kVectus, 1).KillTogether(kMarduk, 10.f));
        std::vector<UnitFacts> f{ Facts(kOwner, kBoss), Facts(20, kMarduk, 60.f), Facts(21, kVectus, 75.f) };
        Focus r = DecideFocus(d, Candidates(f), kOwner, Dps(), 20, {});
        CHECK(r.target == 21);
        CHECK(r.Engaged());
        CHECK(OnLowerTarget(r, 20));

        // Break-out: the tank's prison first, then the healer's.
        Descriptor b;
        b.targets.push_back(TargetEntry(kPrison, 4, Who::Any).BreakOut());
        std::vector<UnitFacts> pf{ Facts(kOwner, kBoss), Facts(30, kPrison), Facts(31, kPrison), Facts(32, kPrison) };
        auto cands = Candidates(pf, { HeldRank::None, HeldRank::Other, HeldRank::Healer, HeldRank::Tank });
        r = DecideFocus(b, cands, kOwner, Dps(), 0, {});
        CHECK(r.target == 32);
        CHECK(r.Engaged());
        pf[3].alive = false;
        r = DecideFocus(b, cands, kOwner, Healer(), 0, {});
        CHECK(r.target == 31);
        pf[2].alive = false;
        r = DecideFocus(b, cands, kOwner, Tank(), 0, {});
        CHECK(r.target == 30);
        CHECK(SuppressPicker(Picker::TankAssist, r));
    }

    void TestComeAndGo()
    {
        // 30 s at 100 ms steps: an add over the boss lives from 5 s to 12 s and again from 20 s to 24 s. The focus
        // engages exactly while it lives, and a bot that follows the focus never sits on a lower target.
        Descriptor d;
        d.targets.push_back(TargetEntry(kAdd, 1));
        std::uint32_t engagedSteps = 0;
        std::uint32_t lowerSteps = 0;
        std::uint32_t switches = 0;
        Key current = kOwner;
        for (int t = 0; t < 300; ++t)
        {
            bool const up = (t >= 50 && t < 120) || (t >= 200 && t < 240);
            std::vector<UnitFacts> f{ Facts(kOwner, kBoss) };
            if (up)
                f.push_back(Facts(t < 150 ? 10 : 11, kAdd));
            Focus const r = DecideFocus(d, Candidates(f), kOwner, Dps(), current, {});
            CHECK(r.Engaged() == up);
            if (r.Engaged())
                ++engagedSteps;
            // The bot: the focus action when engaged, else the stock assist (the boss).
            Key const next = r.Engaged() ? r.target : kOwner;
            if (next != current)
            {
                ++switches;
                current = next;
            }
            if (OnLowerTarget(r, current))
                ++lowerSteps;
        }
        CHECK(engagedSteps == 110);
        CHECK(lowerSteps == 0);
        CHECK(switches == 4);
    }

    void TestIgnoreNames()
    {
        Descriptor d;
        CHECK(!MayBeIgnored(d, 10, kCopy));
        d.ignores.push_back(IgnoreEntry(kCopy, Condition::HasAura(kAiming)));
        d.ignores.push_back(IgnoreEntry::ForUnit(77));
        d.ignores.push_back(IgnoreEntry());                 // names nothing
        CHECK(MayBeIgnored(d, 10, kCopy));                  // by entry (the condition is checked later)
        CHECK(MayBeIgnored(d, 77, kBoss));                  // by guid
        CHECK(!MayBeIgnored(d, 10, kAdd));
        CHECK(!MayBeIgnored(d, 0, 0));
        // The pre-check never contradicts IsIgnored: a unit it rejects is never ignored.
        UnitFacts u = Facts(10, kAdd);
        u.auras = { kAiming };
        CHECK(!IsIgnored(d, u, {}));
        u.entry = kCopy;
        CHECK(IsIgnored(d, u, {}));
    }

    void TestMinion()
    {
        CHECK(DecideMinion(false, true) == MinionStep::Keep);
        CHECK(DecideMinion(false, false) == MinionStep::Keep);
        CHECK(DecideMinion(true, true) == MinionStep::Redirect);
        CHECK(DecideMinion(true, false) == MinionStep::Recall);
    }

    void TestClocks()
    {
        CHECK(SampleGap(Ms(0), Ms(500)) == Ms(0));          // no previous sample
        CHECK(SampleGap(Ms(1000), Ms(1100)) == Ms(100));
        CHECK(SampleGap(Ms(1000), Ms(3000)) == Ms(2000));
        CHECK(SampleGap(Ms(1000), Ms(3001)) == Ms(0));      // a gap over 2 s is not counted
        CHECK(SampleGap(Ms(1000), Ms(900)) == Ms(0));

        BreakOutClock b;
        CHECK(b.MaxMs() == 0);
        b.Sample({}, Ms(100));
        CHECK(b.Open() == 0);
        b.Sample({ 30 }, Ms(1000));
        b.Sample({ 30, 31 }, Ms(1500));
        CHECK(b.Open() == 2);
        CHECK(b.MaxMs() == 500);
        b.Sample({ 31 }, Ms(4000));                         // 30 broken after 3 s
        CHECK(b.Open() == 1);
        CHECK(b.MaxMs() == 3000);
        b.Sample({ 31 }, Ms(5000));
        CHECK(b.MaxMs() == 3500);                           // 31 still held: counted while open
        b.Sample({}, Ms(6000));
        CHECK(b.MaxMs() == 4500);
        CHECK(b.Open() == 0);
        b.Sample({ 30 }, Ms(10000));                        // the same prison holds again: a new timer
        b.Sample({}, Ms(11000));
        CHECK(b.MaxMs() == 4500);
        b.Sample({ 0 }, Ms(12000));                         // an empty guid is not a prison
        CHECK(b.Open() == 0);
    }
}

    // SFK F1: turned players over Arugal engage DPS (the stock pickers stand down) and leave tanks and healers stock;
    // during Moonrise they rank below him, so the focus keeps DPS on Arugal.
    void TestTurnedPlayers()
    {
        constexpr std::uint32_t kTurned = 300590;
        constexpr std::uint32_t kArugal = 4275;
        auto Turned = [](Key guid, bool on)
        {
            UnitFacts u = Facts(guid, 0, 70.f);
            u.player = true;
            u.charmed = on;
            if (on)
                u.auras = { kTurned };
            return u;
        };
        Descriptor d;
        d.hostilePlayerAuras = { kTurned };
        d.targets.push_back(TargetEntry::HostilePlayers(1, Who::Dps, Condition::Phase(0)));
        d.targets.push_back(TargetEntry::HostilePlayers(-1, Who::Dps, Condition::Phase(1)));
        std::vector<UnitFacts> f{ Facts(kOwner, kArugal, 60.f), Turned(30, true) };
        Context ctx;
        Focus focus = DecideFocus(d, Candidates(f), kOwner, Dps(true), 0, ctx);
        CHECK(focus.target == 30 && focus.Engaged() && focus.outranksBoss);
        CHECK(SuppressPicker(Picker::Assist, focus) && SuppressPicker(Picker::Aoe, focus));
        focus = DecideFocus(d, Candidates(f), kOwner, Tank(), 0, ctx);
        CHECK(focus.target == kOwner && !focus.Engaged());
        focus = DecideFocus(d, Candidates(f), kOwner, Healer(), 0, ctx);
        CHECK(!focus.Engaged());

        ctx.phase = 1;
        focus = DecideFocus(d, Candidates(f), kOwner, Dps(), 30, ctx);
        CHECK(focus.target == kOwner && focus.Engaged() && !focus.outranksBoss);

        // Released (charm and aura gone): the boss, nothing engaged.
        ctx.phase = 0;
        f[1] = Turned(30, false);
        focus = DecideFocus(d, Candidates(f), kOwner, Dps(), 30, ctx);
        CHECK(focus.target == kOwner && !focus.Engaged());
    }

namespace
{
    // Blackrock Depths 2026-11-19: Hurley's kill order, the marked keg (3), the Cronies (2), him (0).
    void TestUnseenFocus()
    {
        constexpr std::uint32_t kKeg = 300839;
        constexpr std::uint32_t kCrony = 9541;
        Descriptor d;
        d.targets.push_back(TargetEntry(kKeg, 3));
        d.targets.push_back(TargetEntry(kCrony, 2));
        std::vector<UnitFacts> f{ Facts(kOwner, kBoss), Facts(10, kKeg), Facts(20, kCrony) };
        std::vector<Key> hidden;
        float far = 30.f;
        auto sees = [&hidden](Key k) { return std::find(hidden.begin(), hidden.end(), k) == hidden.end(); };
        auto yd = [&far](Key k) { return k == 10 ? far : 10.f; };
        auto pick = [&](Key current)
        {
            return DecideFocusInSight(d, Candidates(f), kOwner, Dps(), current, {}, 3, sees, yd);
        };

        // In sight: the plain pick.
        Focus r = pick(kOwner);
        CHECK(r.target == 10 && r.Engaged() && !r.unseen);

        // The keg out of sight, a Crony in sight: the keg stays the focus (it ranks above), unseen.
        hidden = { 10 };
        r = pick(kOwner);
        CHECK(r.target == 10 && r.priority == 3 && r.Engaged() && r.outranksBoss && r.unseen);
        CHECK(SuppressPicker(Picker::Assist, r));

        // The keg out of sight and too far: the best pick in sight (the Crony).
        far = kUnseenFocusYd + 1.f;
        r = pick(kOwner);
        CHECK(r.target == 20 && r.Engaged() && !r.unseen);
        far = kUnseenFocusYd;
        CHECK(pick(kOwner).unseen);

        // No Crony left: the keg unseen over the boss in sight.
        f[2].alive = false;
        r = pick(kOwner);
        CHECK(r.target == 10 && r.unseen && r.Engaged());

        // Nothing in sight at all: still the keg.
        hidden = { 10, kOwner };
        r = pick(kOwner);
        CHECK(r.target == 10 && r.unseen);

        // An unseen pick that does not outrank the boss is never kept: the boss out of sight, an add below it in
        // sight -> the add (the old re-pick).
        Descriptor below;
        below.targets.push_back(TargetEntry(kAdd, -1));
        std::vector<UnitFacts> g{ Facts(kOwner, kBoss), Facts(30, kAdd) };
        hidden = { kOwner };
        Focus b = DecideFocusInSight(below, Candidates(g), kOwner, Dps(), 0, {}, 3, sees, yd);
        CHECK(b.target == 30 && !b.unseen);

        // Two listed units of the keg's rank, one out of sight: the one in sight, not a walk.
        f[2] = Facts(11, kKeg);
        hidden = { 10 };
        r = pick(kOwner);
        CHECK(r.target == 11 && !r.unseen && r.Engaged());

        // A tank or a healer under Dps entries: the owner, never engaged, never a walk.
        hidden = { 10, 11 };
        r = DecideFocusInSight(d, Candidates(f), kOwner, Tank(), kOwner, {}, 3, sees, yd);
        CHECK(r.target == kOwner && !r.Engaged() && !r.unseen);

        // The repicks run out with nothing in sight: the first unseen pick.
        Descriptor many;
        for (std::uint32_t e = 1; e <= 6; ++e)
            many.targets.push_back(TargetEntry(1000 + e, static_cast<std::int8_t>(e)));
        std::vector<UnitFacts> m{ Facts(kOwner, kBoss) };
        for (std::uint32_t e = 1; e <= 6; ++e)
            m.push_back(Facts(100 + e, 1000 + e));
        hidden = { 101, 102, 103, 104, 105, 106, kOwner };
        r = DecideFocusInSight(many, Candidates(m), kOwner, Dps(), 0, {}, 3, sees, yd);
        CHECK(r.target == 106 && r.priority == 6 && r.unseen);
    }

    void TestSightSamples()
    {
        // Too short a path: nothing.
        CHECK(SightSamples({}).empty());
        CHECK(SightSamples({ { 0.f, 0.f, 0.f } }).empty());
        // A 5 yd leg: 2, 4 and the end.
        std::vector<PathPoint> s = SightSamples({ { 0.f, 0.f, 0.f }, { 5.f, 0.f, 0.f } });
        CHECK(s.size() == 3);
        CHECK(s[0].x == 2.f && s[1].x == 4.f && s[2].x == 5.f);
        // A corner: the step carries over it (3 yd east, then 3 yd north: 2, then 1 yd past the corner, the end).
        s = SightSamples({ { 0.f, 0.f, 0.f }, { 3.f, 0.f, 0.f }, { 3.f, 3.f, 0.f } });
        CHECK(s.size() == 3);
        CHECK(s[0].x == 2.f && s[0].y == 0.f);
        CHECK(s[1].x == 3.f && std::fabs(s[1].y - 1.f) < 1e-4f);
        CHECK(s[2].x == 3.f && s[2].y == 3.f);
        // The end exactly on a step is not doubled.
        s = SightSamples({ { 0.f, 0.f, 0.f }, { 4.f, 0.f, 0.f } });
        CHECK(s.size() == 2 && s[1].x == 4.f);
        // A long path: at most kSightSamples, the end last.
        s = SightSamples({ { 0.f, 0.f, 0.f }, { 200.f, 0.f, 0.f } });
        CHECK(s.size() == kSightSamples);
        CHECK(s.back().x == 200.f);
        CHECK(s[kSightSamples - 2].x == 2.f * static_cast<float>(kSightSamples - 1));
        // A duplicate point (a zero-length leg) is harmless.
        s = SightSamples({ { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, { 2.f, 0.f, 0.f } });
        CHECK(s.size() == 1 && s[0].x == 2.f);
    }
}

int main()
{
    TestNoList();
    TestAddOverBoss();
    TestBelowBoss();
    TestRoles();
    TestConditionalAndGuid();
    TestTogetherAndBreakOut();
    TestComeAndGo();
    TestIgnoreNames();
    TestMinion();
    TestClocks();
    TestTurnedPlayers();
    TestUnseenFocus();
    TestSightSamples();
    return Report("target_rules_test");
}
