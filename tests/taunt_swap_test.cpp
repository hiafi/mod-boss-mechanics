// Standalone tests for the taunt swap (Blackrock Spire build, WP-TS): src/MechanicTauntSwap.h (the taunt table and
// the pick of a bot's taunt, entry matching, PickTaunter / Decide with the stack and trade rules, the hold-off verdict,
// the held focus), the descriptor's TauntSwapEntry and the main / assist tank split (Who::MainTank, Who::AssistTank in
// MechanicDescriptor.h), the swap focus (TargetRules::Focus::swapHold), the two-tank drill and its preset, and the
// zero-stack eligibility rule (Judge::StacksEligible, Vosh'gajin's Needle). Timelines: Smolderweb's Widow's Bite (one
// boss, 15 s debuff, a bite every 6 s), Halycon and Gizrul's Torn Hide (two wolves, the trade), Gizrul's arrival on
// the main tank, the drill's Taunt Swap (drill) 205024, and three tanks on one boss (also stepped one tank after
// another, as in a map update: R-1-FW m-3); Righteous Defense never in a trade (m-2). Build: tests/run.sh.

#include "check.h"
#include "MechanicDescriptor.h"
#include "MechanicDescriptorDrills.h"
#include "MechanicDescriptorPresets.h"
#include "MechanicDrillPlan.h"
#include "MechanicJudge.h"
#include "MechanicTargetRules.h"
#include "MechanicTauntSwap.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace BossMechanics;
using namespace BossMechanics::TauntSwap;
using Desc::Ms;

namespace
{
    constexpr Key kHalycon = 1001;
    constexpr Key kGizrul = 1002;
    constexpr Key kTankA = 11;
    constexpr Key kTankB = 12;
    constexpr Key kTankC = 13;
    constexpr Key kHealer = 21;

    TankFacts Tank(Key guid, std::vector<std::uint8_t> stacks, bool ready = true)
    {
        TankFacts t;
        t.guid = guid;
        t.ready = ready;
        t.stacks = std::move(stacks);
        return t;
    }

    // ---- the taunt table -----------------------------------------------------------------------------------------

    void TestTable()
    {
        // The reworked classes' taunts are taunts by id (playerbots' IsTauntAction misses them).
        for (std::uint32_t id : { 355u, 62124u, 6795u, 56222u, 203722u, 200839u, 8042u, 49231u, 31789u })
            CHECK(IsTaunt(id));
        CHECK(Find(203722)->kind == TauntKind::Single && std::string(Find(203722)->name) == "Insult");
        CHECK(Find(200839)->kind == TauntKind::Single);
        CHECK(Find(8042)->kind == TauntKind::RockbiterShock);
        CHECK(Find(31789)->kind == TauntKind::Friend);
        CHECK(Find(1161)->kind == TauntKind::Area && Find(5209)->kind == TauntKind::Area);
        CHECK(Find(49576)->kind == TauntKind::Grip);
        CHECK(!IsTaunt(6552) && !IsTaunt(0));       // Pummel is a kick, not a taunt
        // No id twice.
        for (std::size_t i = 0; i < kTable.size(); ++i)
            for (std::size_t k = i + 1; k < kTable.size(); ++k)
                CHECK(kTable[i].id != kTable[k].id);
        // Area and grip taunts are never cast by the swap; Earth Shock only by a tank.
        CHECK(!Castable(*Find(1161), true) && !Castable(*Find(49576), true));
        CHECK(Castable(*Find(8042), true) && !Castable(*Find(8042), false));
        CHECK(Castable(*Find(355), false) && Castable(*Find(31789), true));
    }

    TauntSpell const* PickFor(bool tank, std::vector<std::uint32_t> known, std::vector<std::uint32_t> notReady = {},
        std::vector<std::uint32_t>* tried = nullptr)
    {
        return PickTaunt(tank,
            [&](TauntSpell const& t) { return std::find(known.begin(), known.end(), t.id) != known.end(); },
            [&](TauntSpell const& t)
            {
                if (tried)
                    tried->push_back(t.id);
                return std::find(notReady.begin(), notReady.end(), t.id) == notReady.end();
            });
    }

    void TestPickTaunt()
    {
        // One per class, the reworked ones included.
        CHECK(PickFor(true, { 355, 1161 })->id == 355);             // Warrior: Taunt, never the shout
        CHECK(PickFor(true, { 203722 })->id == 203722);             // Rogue tank: Insult
        CHECK(PickFor(true, { 200839, 59671 })->id == 200839);      // Warlock (Dark Apotheosis)
        CHECK(PickFor(true, { 6795, 5209 })->id == 6795);           // Druid: Growl
        CHECK(PickFor(true, { 56222, 49576 })->id == 56222);        // DK: Dark Command
        CHECK(!PickFor(true, { 56222, 49576 }, { 56222 }));         // ...never Death Grip
        CHECK(PickFor(true, { 62124, 31789 })->id == 62124);        // Paladin: Hand of Reckoning first
        CHECK(PickFor(true, { 62124, 31789 }, { 62124 })->id == 31789);     // ...Righteous Defense when it is not ready
        // Shaman: the highest Earth Shock rank it knows, a tank only; a lower rank is not tried after the highest.
        std::vector<std::uint32_t> tried;
        CHECK(PickFor(true, { 8042, 10414, 49231 }, {}, &tried)->id == 49231);
        tried.clear();
        CHECK(!PickFor(true, { 8042, 49231 }, { 49231 }, &tried));
        CHECK(tried.size() == 1 && tried[0] == 49231);
        CHECK(!PickFor(false, { 8042, 49231 }));
        CHECK(!PickFor(true, {}));
        CHECK(!PickFor(true, { 1161, 5209, 59671 }));               // area taunts only
    }

    // ---- entries ---------------------------------------------------------------------------------------------------

    void TestMatches()
    {
        TauntSwapEntry const owner(0, 7000, 3);
        CHECK(Matches(owner, 500, 9, 500) && !Matches(owner, 501, 9, 500));
        TauntSwapEntry const byEntry(10268, 7000, 3);
        CHECK(Matches(byEntry, 501, 10268, 500) && !Matches(byEntry, 500, 10220, 500));
        TauntSwapEntry const byGuid = TauntSwapEntry::ForUnit(777, 7000, 3);
        CHECK(Matches(byGuid, 777, 1, 500) && !Matches(byGuid, 778, 10268, 500));
        CHECK(!byGuid.anyCaster && TauntSwapEntry(1, 2).AnyCaster().anyCaster);
        CHECK(TauntSwapEntry().swapAtStacks == 3);

        // The descriptor: not empty with only a taunt swap; its condition is referenced (the view gathers the aura).
        Descriptor d;
        CHECK(d.Empty());
        d.tauntSwaps.emplace_back(0, 7000, 3, Condition::HasAura(7100));
        CHECK(!d.Empty());
        Desc::References const r = Desc::Referenced(d);
        CHECK(r.auras.size() == 1 && r.auras[0] == 7100);
    }

    // ---- the pick and the decision ---------------------------------------------------------------------------------

    void TestPickTaunter()
    {
        std::vector<BossFacts> bosses{ { kHalycon, kTankA, 3 } };
        // Below the swap count: nobody.
        std::vector<TankFacts> tanks{ Tank(kTankA, { 2 }), Tank(kTankB, { 0 }) };
        CHECK(PickTaunter(bosses, tanks, 0).taunter == 0);
        // At the swap count: the other tank.
        tanks[0].stacks = { 3 };
        Pick p = PickTaunter(bosses, tanks, 0);
        CHECK(p.taunter == kTankB && p.why == Why::Stacks);
        // Its taunt on cooldown: nobody (the stacks grow: the tank's own failure).
        tanks[1].ready = false;
        CHECK(PickTaunter(bosses, tanks, 0).taunter == 0);
        tanks[1].ready = true;
        // Dead: nobody.
        tanks[1].alive = false;
        CHECK(PickTaunter(bosses, tanks, 0).taunter == 0);
        tanks[1].alive = true;
        // User ruling 2026-11-15: only once the taunter's own stacks have fallen off (0), whatever the victim has.
        tanks[1].stacks = { 3 };
        CHECK(PickTaunter(bosses, tanks, 0).taunter == 0);
        tanks[1].stacks = { 1 };
        CHECK(PickTaunter(bosses, tanks, 0).taunter == 0);
        tanks[1].stacks = { 0 };
        CHECK(PickTaunter(bosses, tanks, 0).taunter == kTankB);
        // On a non-tank: not judged (stock taunts).
        bosses[0].victim = kHealer;
        CHECK(PickTaunter(bosses, tanks, 0).taunter == 0);
        bosses[0].victim = 0;
        CHECK(PickTaunter(bosses, tanks, 0).taunter == 0);
        // swapAt 0: never.
        bosses[0] = { kHalycon, kTankA, 0 };
        tanks[0].stacks = { 9 };
        CHECK(PickTaunter(bosses, tanks, 0).taunter == 0);

        // Three tanks: only those at 0 on that boss; the one holding nothing before the one holding the other boss,
        // then the lower guid.
        bosses = { { kHalycon, kTankA, 3 }, { kGizrul, kTankB, 3 } };
        tanks = { Tank(kTankA, { 3, 0 }), Tank(kTankB, { 0, 1 }), Tank(kTankC, { 1, 0 }) };
        CHECK(PickTaunter(bosses, tanks, 0).taunter == kTankB);     // C still carries a Halycon stack
        tanks[2] = Tank(kTankC, { 0, 0 });
        CHECK(PickTaunter(bosses, tanks, 0).taunter == kTankC);     // C holds nothing
        tanks[2].ready = false;
        CHECK(PickTaunter(bosses, tanks, 0).taunter == kTankB);
        tanks[1].stacks = { 1, 1 };
        CHECK(PickTaunter(bosses, tanks, 0).taunter == 0);          // nobody at 0 and ready: A keeps Halycon
    }

    void TestDecide()
    {
        std::vector<BossFacts> const bosses{ { kHalycon, kTankA, 3 } };
        std::vector<TankFacts> tanks{ Tank(kTankA, { 3 }), Tank(kTankB, { 0 }) };
        Decision a = Decide(bosses, tanks, kTankA);
        CHECK(a.Holds(kHalycon) && a.holdOff.empty() && !a.taunt);
        Decision b = Decide(bosses, tanks, kTankB);
        CHECK(b.taunt == kHalycon && b.tauntVictim == kTankA && b.why == Why::Stacks && b.victimStacks == 3);
        CHECK(b.holds.empty() && b.holdOff.empty());
        // Below the count the waiting tank keeps off: the hold-off.
        tanks[0].stacks = { 2 };
        b = Decide(bosses, tanks, kTankB);
        CHECK(!b.taunt && b.HeldOff(kHalycon));
        // A non-tank (not in the tanks) decides nothing.
        CHECK(!Decide(bosses, tanks, kHealer).Active());
        // A dead tank decides nothing.
        tanks[1].alive = false;
        CHECK(!Decide(bosses, tanks, kTankB).Active());
        // On a healer: no hold-off (the stock taunts take it back).
        std::vector<BossFacts> const loose{ { kHalycon, kHealer, 3 } };
        tanks[1].alive = true;
        CHECK(!Decide(loose, tanks, kTankB).Active());

        // Picked for two bosses at once: one taunt now, the other neither held nor kept off.
        std::vector<BossFacts> const two{ { kHalycon, kTankA, 3 }, { kGizrul, kTankA, 3 } };
        std::vector<TankFacts> const t2{ Tank(kTankA, { 4, 5 }), Tank(kTankB, { 0, 0 }) };
        Decision const d = Decide(two, t2, kTankB);
        CHECK(d.taunt == kHalycon && d.holdOff.empty() && d.holds.empty());
    }

    // A tank with 1 or more stacks of the boss's debuff never taunts it, however far past the count the victim is.
    void TestNeverWithStacks()
    {
        std::vector<BossFacts> const bosses{ { kHalycon, kTankA, 3 } };
        for (std::uint8_t mine = 1; mine <= 10; ++mine)
            for (std::uint8_t victim = 3; victim <= 60; victim = static_cast<std::uint8_t>(victim + 3))
            {
                std::vector<TankFacts> const tanks{ Tank(kTankA, { victim }), Tank(kTankB, { mine }) };
                CHECK(PickTaunter(bosses, tanks, 0).taunter == 0);
                Decision const d = Decide(bosses, tanks, kTankB);
                CHECK(!d.taunt && d.HeldOff(kHalycon));
            }
    }

    void TestTrade()
    {
        // Gizrul arrives on the main tank, who also holds Halycon (2 Torn Hide stacks from Halycon): the free tank
        // takes the one with the most stacks on the holder (Halycon) at once, without waiting for 3.
        std::vector<BossFacts> const bosses{ { kHalycon, kTankA, 3 }, { kGizrul, kTankA, 3 } };
        std::vector<TankFacts> tanks{ Tank(kTankA, { 2, 0 }), Tank(kTankB, { 0, 0 }) };
        Decision const b = Decide(bosses, tanks, kTankB);
        CHECK(b.taunt == kHalycon && b.why == Why::Trade);
        // ...not Gizrul (nor is it kept off: the trade picks one).
        CHECK(!b.HeldOff(kHalycon));
        CHECK(b.HeldOff(kGizrul));
        Decision const a = Decide(bosses, tanks, kTankA);
        CHECK(a.holds.size() == 2 && !a.taunt);
        // A free tank still carrying Halycon stacks (any) takes Gizrul: the spec's "second tank picks up Gizrul".
        tanks[1].stacks = { 3, 0 };
        CHECK(Decide(bosses, tanks, kTankB).taunt == kGizrul);
        tanks[1].stacks = { 1, 0 };
        CHECK(Decide(bosses, tanks, kTankB).taunt == kGizrul);
        // Ties (the holder at 0 on both) go to the lower index.
        tanks = { Tank(kTankA, { 0, 0 }), Tank(kTankB, { 0, 0 }) };
        CHECK(Decide(bosses, tanks, kTankB).taunt == kHalycon);
        // The holder's most stacked boss among those the free tank is clear of.
        tanks = { Tank(kTankA, { 1, 2 }), Tank(kTankB, { 0, 0 }) };
        CHECK(Decide(bosses, tanks, kTankB).taunt == kGizrul);
        // Debuffed from both: no trade.
        tanks = { Tank(kTankA, { 2, 2 }), Tank(kTankB, { 1, 1 }) };
        CHECK(!Decide(bosses, tanks, kTankB).taunt);
        // A tank holding one boss is not free: no trade.
        std::vector<BossFacts> const three{ { kHalycon, kTankA, 3 }, { kGizrul, kTankA, 3 }, { 1003, kTankB, 3 } };
        tanks = { Tank(kTankA, { 1, 1, 0 }), Tank(kTankB, { 0, 0, 0 }) };
        CHECK(!Decide(three, tanks, kTankB).taunt);
    }

    void TestHoldOffAndFocus()
    {
        Decision d;
        d.holdOff = { kHalycon };
        d.holds = { kGizrul };
        CHECK(HoldOffBlocks(d, kHalycon, false, false));     // an attack or a taunt at Halycon
        CHECK(!HoldOffBlocks(d, kHalycon, false, true));     // a kick stays
        CHECK(!HoldOffBlocks(d, kGizrul, false, false));
        CHECK(!HoldOffBlocks(d, 0, false, false));
        CHECK(HoldOffBlocks(d, 0, true, false));             // an area taunt while it keeps off a boss
        Decision free;
        CHECK(!HoldOffBlocks(free, kHalycon, true, false));
        // The held focus: the current target when held, else the first held.
        CHECK(HeldFocus(d, kGizrul) == kGizrul && HeldFocus(d, kHalycon) == kGizrul && HeldFocus(free, 5) == 0);
        d.holds = { kGizrul, kHalycon };
        d.holdOff.clear();
        CHECK(HeldFocus(d, kHalycon) == kHalycon);
        // A swap hold engages the focus (the stock pickers stand down).
        TargetRules::Focus f;
        f.target = kGizrul;
        CHECK(!f.Engaged());
        f.swapHold = true;
        CHECK(f.Engaged() && TargetRules::SuppressPicker(TargetRules::Picker::TankAssist, f));
    }

    // ---- main and assist tank (Who) --------------------------------------------------------------------------------

    void TestWho()
    {
        Desc::BotFacts mt;
        mt.tank = true;
        mt.mainTank = true;
        Desc::BotFacts at;
        at.tank = true;
        Desc::BotFacts dps;
        Desc::BotFacts healer;
        healer.healer = true;
        CHECK(Desc::Fits(Who::MainTank, mt) && !Desc::Fits(Who::MainTank, at) && !Desc::Fits(Who::MainTank, dps));
        CHECK(Desc::Fits(Who::AssistTank, at) && !Desc::Fits(Who::AssistTank, mt) && !Desc::Fits(Who::AssistTank, dps));
        CHECK(!Desc::Fits(Who::AssistTank, healer));
        CHECK(Desc::Fits(Who::Tank, mt) && Desc::Fits(Who::Tank, at));

        // "The second tank picks up Gizrul": the assist tank's focus is Gizrul, the main tank and the damage dealers
        // keep Halycon (the owner).
        Descriptor d;
        d.targets.emplace_back(10268, 1, Who::AssistTank);
        Desc::UnitFacts h;
        h.guid = kHalycon;
        h.entry = 10220;
        Desc::UnitFacts g;
        g.guid = kGizrul;
        g.entry = 10268;
        std::vector<Desc::TargetCandidate> const c{ { &h, Desc::HeldRank::None }, { &g, Desc::HeldRank::None } };
        Desc::Context const ctx;
        TargetRules::Focus const fa = TargetRules::DecideFocus(d, c, kHalycon, at, 0, ctx);
        CHECK(fa.target == kGizrul && fa.Engaged());
        TargetRules::Focus const fm = TargetRules::DecideFocus(d, c, kHalycon, mt, 0, ctx);
        CHECK(fm.target == kHalycon && !fm.Engaged());
        TargetRules::Focus const fd = TargetRules::DecideFocus(d, c, kHalycon, dps, 0, ctx);
        CHECK(fd.target == kHalycon && !fd.Engaged());
    }

    // ---- eligibility: zero stacks (Vosh'gajin's Needle) ------------------------------------------------------------

    void TestStacksEligible()
    {
        // Eligible({BOUND, maxStacks = 0}): the Bound player (1 stack) is never the needle's target.
        CHECK(Judge::StacksEligible(0, 0, 0));
        CHECK(!Judge::StacksEligible(1, 0, 0));
        CHECK(!Judge::StacksEligible(5, 0, 0));
        // The defaults (0 .. 255) let everyone through; Urok's Unstable `{UNSTABLE, maxStacks = 1}`.
        CHECK(Judge::StacksEligible(0, 0, 255) && Judge::StacksEligible(255, 0, 255));
        CHECK(Judge::StacksEligible(1, 0, 1) && !Judge::StacksEligible(2, 0, 1));
        CHECK(!Judge::StacksEligible(0, 1, 255) && Judge::StacksEligible(1, 1, 255));
    }

    // ---- timelines -------------------------------------------------------------------------------------------------

    struct SimStack
    {
        std::uint8_t n = 0;
        Ms expires{ 0 };
    };

    struct SimTank
    {
        Key guid = 0;
        Ms tauntReadyAt{ 0 };
        std::vector<SimStack> stacks;   // per boss
    };

    struct SimBoss
    {
        Key unit = 0;
        Key victim = 0;
        Ms nextHit{ 0 };
        Ms period{ 6000 };
    };

    struct SimResult
    {
        std::uint8_t maxStacks = 0;
        int swaps = 0;
        int trades = 0;
        int doubleTaunts = 0;       // two tanks taunted one boss in one tick
        int earlyTaunts = 0;        // a taunt while the taunter's taunt was on cooldown
        int offTank = 0;            // ticks a boss was on no tank
        int holdOffViolations = 0;  // a tank not holding a boss also neither kept off nor taunted it
        int taunterWithStacks = 0;  // a taunt by a tank still carrying that boss's debuff (ruling 2026-11-15)
        int ticksPastCount = 0;     // ticks a boss's victim sat at or past the swap count
    };

    // How the tanks decide within one tick.
    enum class Step : std::uint8_t
    {
        Snapshot,       // every tank on the same snapshot, then the taunts land
        Sequential,     // one after another (players update before creatures, Map::Update): a taunt puts its aura on
                        // the boss at once (EffectiveVictim reads it), the boss turns after every tank has stepped
        SequentialRaw   // ...the facts read the boss's current victim only (the R-1-FW m-3 double taunt)
    };

    // 100 ms ticks: hits stack the victim (refreshing), stacks fall off after `duration` (capped at `cap`), the tanks
    // decide (`step`), then the bosses turn to their taunters (taunt cooldown 8 s, taunt aura 3 s).
    SimResult Run(std::vector<SimBoss> bosses, std::vector<SimTank> tanks, Ms duration, std::uint8_t cap, Ms length,
        std::uint8_t swapAt = 3, Step step = Step::Snapshot)
    {
        SimResult r;
        for (SimTank& t : tanks)
            t.stacks.assign(bosses.size(), SimStack());
        std::vector<Key> taunter(bosses.size(), 0);     // the last taunt aura's caster on each boss
        std::vector<Ms> tauntUntil(bosses.size(), Ms(0));
        for (Ms now{ 0 }; now <= length; now += Ms(100))
        {
            for (SimTank& t : tanks)
                for (SimStack& s : t.stacks)
                    if (s.n && now >= s.expires)
                        s = SimStack();
            for (std::size_t b = 0; b < bosses.size(); ++b)
            {
                if (taunter[b] && now >= tauntUntil[b])
                    taunter[b] = 0;
                SimBoss& x = bosses[b];
                if (now < x.nextHit)
                    continue;
                x.nextHit += x.period;
                for (SimTank& t : tanks)
                    if (t.guid == x.victim)
                    {
                        SimStack& s = t.stacks[b];
                        s.n = std::min<std::uint8_t>(static_cast<std::uint8_t>(s.n + 1), cap);
                        s.expires = now + duration;
                        r.maxStacks = std::max(r.maxStacks, s.n);
                    }
            }

            auto factsNow = [&]()
            {
                std::vector<BossFacts> facts;
                for (std::size_t b = 0; b < bosses.size(); ++b)
                    facts.push_back({ bosses[b].unit, step == Step::Sequential
                        ? EffectiveVictim(bosses[b].victim, taunter[b]) : bosses[b].victim, swapAt });
                std::vector<TankFacts> tf;
                for (SimTank const& t : tanks)
                {
                    TankFacts f;
                    f.guid = t.guid;
                    f.ready = now >= t.tauntReadyAt;
                    for (SimStack const& s : t.stacks)
                        f.stacks.push_back(s.n);
                    tf.push_back(f);
                }
                return std::make_pair(facts, tf);
            };
            auto snapshot = factsNow();
            std::vector<std::pair<Key, Key>> taunts;    // boss, tank
            for (SimTank& t : tanks)
            {
                if (step != Step::Snapshot)
                    snapshot = factsNow();
                std::vector<BossFacts> const& facts = snapshot.first;
                Decision const d = Decide(facts, snapshot.second, t.guid);
                for (BossFacts const& x : facts)
                    if (x.victim != t.guid && !d.HeldOff(x.unit) && d.taunt != x.unit)
                        ++r.holdOffViolations;
                if (d.taunt)
                {
                    for (std::size_t b = 0; b < bosses.size(); ++b)
                        if (bosses[b].unit == d.taunt)
                        {
                            if (t.stacks[b].n)
                                ++r.taunterWithStacks;
                            taunter[b] = t.guid;
                            tauntUntil[b] = now + Ms(3000);
                        }
                    if (now < t.tauntReadyAt)
                        ++r.earlyTaunts;
                    if (d.why == Why::Trade)
                        ++r.trades;
                    taunts.emplace_back(d.taunt, t.guid);
                    if (step != Step::Snapshot)
                        t.tauntReadyAt = now + Ms(8000);
                }
            }
            for (std::size_t i = 0; i < taunts.size(); ++i)
                for (std::size_t k = i + 1; k < taunts.size(); ++k)
                    if (taunts[i].first == taunts[k].first)
                        ++r.doubleTaunts;
            for (auto const& [boss, tank] : taunts)
            {
                for (SimBoss& x : bosses)
                    if (x.unit == boss)
                        x.victim = tank;
                for (SimTank& t : tanks)
                    if (t.guid == tank)
                        t.tauntReadyAt = now + Ms(8000);
                ++r.swaps;
            }
            for (std::size_t b = 0; b < bosses.size(); ++b)
                for (SimTank const& t : tanks)
                    if (t.guid == bosses[b].victim && t.stacks[b].n >= swapAt)
                        ++r.ticksPastCount;
            for (SimBoss const& x : bosses)
                if (std::none_of(tanks.begin(), tanks.end(), [&](SimTank const& t) { return t.guid == x.victim; }))
                    ++r.offTank;
        }
        return r;
    }

    SimTank T(Key guid)
    {
        SimTank t;
        t.guid = guid;
        return t;
    }

    void TestSmolderwebTimeline()
    {
        // Widow's Bite: 6 s after the pull, every 6 s; 15 s, refreshed per stack, no cap. Two tanks over 3 minutes.
        SimBoss sw;
        sw.unit = kHalycon;
        sw.victim = kTankA;
        sw.nextHit = Ms(6000);
        SimResult const r = Run({ sw }, { T(kTankA), T(kTankB) }, Ms(15000), 255, Ms(180000));
        CHECK(r.maxStacks == 3);                // traded at 3, never more
        CHECK(r.swaps >= 9);                    // every 3 bites
        CHECK(r.doubleTaunts == 0 && r.earlyTaunts == 0 && r.offTank == 0 && r.holdOffViolations == 0);
        CHECK(r.taunterWithStacks == 0 && r.ticksPastCount == 0);     // the waiting tank is clear by every third bite
        // One tank alone: uncapped (the tank's own failure), no swap.
        SimResult const solo = Run({ sw }, { T(kTankA) }, Ms(15000), 255, Ms(180000));
        CHECK(solo.swaps == 0 && solo.maxStacks == 30);
    }

    void TestWolvesTimeline()
    {
        // Torn Hide: each wolf bites its own target every 6 s (Halycon from 6 s, Gizrul from 9 s), 15 s per stack.
        SimBoss h;
        h.unit = kHalycon;
        h.victim = kTankA;
        h.nextHit = Ms(6000);
        SimBoss g;
        g.unit = kGizrul;
        g.victim = kTankB;
        g.nextHit = Ms(9000);
        SimResult const r = Run({ h, g }, { T(kTankA), T(kTankB) }, Ms(15000), 255, Ms(180000));
        CHECK(r.maxStacks <= 3);
        CHECK(r.swaps >= 10 && r.trades >= 1);
        CHECK(r.doubleTaunts == 0 && r.earlyTaunts == 0 && r.offTank == 0 && r.holdOffViolations == 0);
        CHECK(r.taunterWithStacks == 0);
        // Under Call of the Pack the bites come every 4 s: a holder at 3 may wait for the other tank's debuff to fall
        // off (ruling 2026-11-15), but the timeline still peaks at 3.
        h.period = Ms(4000);
        g.period = Ms(4000);
        SimResult const fast = Run({ h, g }, { T(kTankA), T(kTankB) }, Ms(15000), 255, Ms(120000));
        CHECK(fast.maxStacks <= 4 && fast.doubleTaunts == 0 && fast.earlyTaunts == 0 && fast.offTank == 0);
        CHECK(fast.taunterWithStacks == 0 && fast.ticksPastCount > 0);
        // Gizrul arrives on the main tank: the second tank takes a wolf at once.
        g.victim = kTankA;
        h.period = Ms(6000);
        g.period = Ms(6000);
        SimResult const arrive = Run({ h, g }, { T(kTankA), T(kTankB) }, Ms(15000), 255, Ms(60000));
        CHECK(arrive.trades >= 1 && arrive.maxStacks <= 3 && arrive.doubleTaunts == 0 && arrive.taunterWithStacks == 0);
    }

    void TestDrillTimeline()
    {
        // The two-tank drill: Taunt Swap (drill) 205024, 15 s from the last stack, cap 10, a stack every 6 s, 60 s.
        SimBoss dummy;
        dummy.unit = kHalycon;
        dummy.victim = kTankA;
        dummy.nextHit = Ms(0);
        dummy.period = Ms(6000);
        for (Step step : { Step::Snapshot, Step::Sequential })
        {
            SimResult const r = Run({ dummy }, { T(kTankA), T(kTankB) }, Ms(15000), Ids::TAUNT_SWAP_DRILL_MAX_STACKS,
                Ms(60000), 3, step);
            // A swap at every third stack (12, 30, 48 s): the giver's stacks are gone by the taker's third, so the
            // tanks trade again and again and nobody waits past 3 (ruling 2026-11-15).
            CHECK(r.swaps == 3 && r.maxStacks == 3 && r.taunterWithStacks == 0 && r.ticksPastCount == 0);
            CHECK(r.doubleTaunts == 0 && r.earlyTaunts == 0 && r.offTank == 0 && r.holdOffViolations == 0);
        }
        // The old stand-in, Baited (drill) 205008 (2 min), allowed one swap a run: the giver never cleared.
        SimBoss fast = dummy;
        fast.period = Ms(3000);
        CHECK(Run({ fast }, { T(kTankA), T(kTankB) }, Ms(120000), 10, Ms(60000)).swaps == 1);
    }

    void TestThreeTanksTimeline()
    {
        SimBoss sw;
        sw.unit = kHalycon;
        sw.victim = kTankB;
        sw.nextHit = Ms(6000);
        SimResult const r = Run({ sw }, { T(kTankA), T(kTankB), T(kTankC) }, Ms(15000), 255, Ms(120000));
        CHECK(r.maxStacks == 3 && r.doubleTaunts == 0 && r.earlyTaunts == 0 && r.offTank == 0);
        CHECK(r.holdOffViolations == 0 && r.taunterWithStacks == 0);
        // R-1-FW m-3: in the engine the tanks step one after another and the boss turns after them. Reading only its
        // current victim, a tank stepping after the taunter in the same update sees no answer and taunts too...
        SimResult const raw = Run({ sw }, { T(kTankA), T(kTankB), T(kTankC) }, Ms(15000), 255, Ms(120000), 3,
            Step::SequentialRaw);
        CHECK(raw.doubleTaunts > 0);
        // ...reading the last taunt on the boss (EffectiveVictim) it does not.
        SimResult const seq = Run({ sw }, { T(kTankA), T(kTankB), T(kTankC) }, Ms(15000), 255, Ms(120000), 3,
            Step::Sequential);
        CHECK(seq.maxStacks == 3 && seq.swaps >= 6 && seq.doubleTaunts == 0 && seq.earlyTaunts == 0);
        CHECK(seq.offTank == 0 && seq.holdOffViolations == 0 && seq.taunterWithStacks == 0);
        // The two-wolf timeline steps the same way.
        SimBoss h;
        h.unit = kHalycon;
        h.victim = kTankA;
        h.nextHit = Ms(6000);
        SimBoss g;
        g.unit = kGizrul;
        g.victim = kTankB;
        g.nextHit = Ms(9000);
        SimResult const wolves = Run({ h, g }, { T(kTankA), T(kTankB), T(kTankC) }, Ms(15000), 255, Ms(180000), 3,
            Step::Sequential);
        CHECK(wolves.maxStacks <= 3 && wolves.doubleTaunts == 0 && wolves.earlyTaunts == 0 && wolves.offTank == 0);
        CHECK(wolves.holdOffViolations == 0 && wolves.taunterWithStacks == 0);
    }

    // R-1-FW m-3, the pick itself: B sits at 3, A has just taunted (its taunt on cooldown, the boss not yet turned),
    // C is clear and ready.
    void TestTauntInFlight()
    {
        CHECK(EffectiveVictim(kTankB, kTankA) == kTankA && EffectiveVictim(kTankB, 0) == kTankB);
        CHECK(EffectiveVictim(0, kTankA) == kTankA);
        std::vector<TankFacts> const tanks{ Tank(kTankA, { 0 }, false), Tank(kTankB, { 3 }), Tank(kTankC, { 0 }) };
        // The boss's current victim alone: C would taunt as well.
        std::vector<BossFacts> const raw{ { kHalycon, kTankB, 3 } };
        CHECK(Decide(raw, tanks, kTankC).taunt == kHalycon);
        // The taunt counted: A holds, B and C keep off, nobody taunts.
        std::vector<BossFacts> const seen{ { kHalycon, EffectiveVictim(kTankB, kTankA), 3 } };
        Decision const a = Decide(seen, tanks, kTankA);
        Decision const b = Decide(seen, tanks, kTankB);
        Decision const c = Decide(seen, tanks, kTankC);
        CHECK(a.Holds(kHalycon) && !a.taunt);
        CHECK(!b.taunt && b.HeldOff(kHalycon) && !c.taunt && c.HeldOff(kHalycon));
    }

    // R-1-FW m-2: Righteous Defense taunts up to 3 attackers of the victim, so it is never the swap's taunt while the
    // victim holds two swap bosses (both wolves would move to the taker).
    void TestRighteousDefense()
    {
        std::vector<BossFacts> const one{ { kHalycon, kTankA, 3 } };
        std::vector<BossFacts> const two{ { kHalycon, kTankA, 3 }, { kGizrul, kTankA, 3 } };
        CHECK(FriendTauntAllowed(one, kTankA) && !FriendTauntAllowed(two, kTankA) && FriendTauntAllowed(two, kTankB));

        // A paladin whose Hand of Reckoning is on cooldown, Righteous Defense ready.
        auto paladin = [](Key guid, std::vector<std::uint8_t> stacks)
        {
            TankFacts t = Tank(guid, std::move(stacks), false);
            t.friendReady = true;
            return t;
        };
        // One boss on the victim: Righteous Defense may take it.
        std::vector<TankFacts> tanks{ Tank(kTankA, { 3 }), paladin(kTankB, { 0 }) };
        Decision d = Decide(one, tanks, kTankB);
        CHECK(d.taunt == kHalycon && d.friendTaunt);
        // The trade (A holds both): not with Righteous Defense; nobody is picked and the paladin keeps off.
        tanks = { Tank(kTankA, { 2, 0 }), paladin(kTankB, { 0, 0 }) };
        CHECK(PickTaunter(two, tanks, 0).taunter == 0 && PickTaunter(two, tanks, 1).taunter == 0);
        d = Decide(two, tanks, kTankB);
        CHECK(!d.taunt && d.HeldOff(kHalycon) && d.HeldOff(kGizrul));
        // A stack swap off a tank holding both: the same.
        tanks = { Tank(kTankA, { 3, 1 }), paladin(kTankB, { 0, 1 }) };
        CHECK(!Decide(two, tanks, kTankB).taunt);
        // Hand of Reckoning ready: the trade goes ahead, Righteous Defense still refused for it.
        tanks = { Tank(kTankA, { 2, 0 }), Tank(kTankB, { 0, 0 }) };
        tanks[1].friendReady = true;
        d = Decide(two, tanks, kTankB);
        CHECK(d.taunt == kHalycon && d.why == Why::Trade && !d.friendTaunt);
        // The action's pick with the refusal (MechanicBotTauntSwap.cpp): Hand of Reckoning on cooldown, Righteous
        // Defense refused -> no taunt; allowed -> Righteous Defense.
        auto pick = [](bool friendTaunt, bool horReady)
        {
            return PickTaunt(true, [](TauntSpell const& t) { return t.id == 62124 || t.id == 31789; },
                [&](TauntSpell const& t)
                {
                    if (t.kind == TauntKind::Friend)
                        return friendTaunt;
                    return horReady;
                });
        };
        CHECK(!pick(false, false));
        CHECK(pick(true, false) && pick(true, false)->id == 31789);
        CHECK(pick(false, true) && pick(false, true)->id == 62124);
    }

    // ---- the drill and its preset ----------------------------------------------------------------------------------

    void TestDrill()
    {
        Desc::Preset const* p = Desc::FindPreset("taunt-swap");
        CHECK(p && p->build);
        if (p && p->build)
        {
            Descriptor const d = p->build();
            CHECK(d.tauntSwaps.size() == 1);
            if (!d.tauntSwaps.empty())
            {
                TauntSwapEntry const& e = d.tauntSwaps.front();
                CHECK(!e.bossEntry && !e.guid && e.debuffSpell == Ids::SPELL_DRILL_TAUNT_SWAP && e.swapAtStacks == 3);
            }
            CHECK(d.targets.size() == 1 && d.targets[0].who == Who::AssistTank
                && d.targets[0].entry == Ids::NPC_DRILL_ADD);
        }
        Drill::ProgramDef const* drill = Drill::FindProgramDrill("taunt-swap");
        CHECK(drill && drill->listed);
        std::string err;
        std::optional<Drill::Program> const prog = Drill::ParseProgram("taunt-swap", err);
        CHECK(prog.has_value());
        if (!prog)
            std::printf("  taunt-swap: %s\n", err.c_str());
        bool cast = false;
        if (prog)
            for (Drill::ProgramEntry const& e : prog->entries)
                if (e.kind == Drill::EntryKind::Cast)
                    cast = e.spellId == Ids::SPELL_DRILL_TAUNT_SWAP && e.everyMs == 6000 && !e.unitEntry;
        CHECK(cast);
        // 205024 is the module's (BossMechanicsIds.h), not a `.mech test rider=` aura.
        CHECK(Ids::SPELL_DRILL_TAUNT_SWAP == 205024 && Ids::InSpellBlock(Ids::SPELL_DRILL_TAUNT_SWAP));
        CHECK(Ids::SPELL_DRILL_TAUNT_SWAP > Ids::SPELL_M5_LAST && !Ids::RiderAllowed(Ids::SPELL_DRILL_TAUNT_SWAP));
        // Not in the descriptor loops (a group without two tanks would only watch the stacks grow).
        Drill::ProgramDef const* loop = Drill::FindProgramDrill(Drill::kDescriptorDrillsProgram);
        CHECK(loop && std::string(loop->program).find("taunt-swap") == std::string::npos);
    }
}

int main()
{
    TestTable();
    TestPickTaunt();
    TestMatches();
    TestPickTaunter();
    TestDecide();
    TestNeverWithStacks();
    TestTrade();
    TestHoldOffAndFocus();
    TestWho();
    TestStacksEligible();
    TestSmolderwebTimeline();
    TestWolvesTimeline();
    TestDrillTimeline();
    TestThreeTanksTimeline();
    TestTauntInFlight();
    TestRighteousDefense();
    TestDrill();
    return Report("taunt_swap_test");
}
