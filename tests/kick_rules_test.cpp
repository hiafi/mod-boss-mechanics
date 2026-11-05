// Standalone tests for src/MechanicKickRules.h (STAGE-M5-HANDOFF §3.3, decision §7.1 #2, WP-I): the kick table and
// the choice of a bot's kick, the interruptible test, the canonical cast start, who kicks which cast (order, hold,
// alternation, deferral), the stock-kick verdict, the classification of a kick that went out and the miss test.
// Build: tests/run.sh.

#include "check.h"
#include "MechanicKickRules.h"
#include <set>
#include <string_view>
#include <vector>

using namespace BossMechanics;
using namespace BossMechanics::Kick;
using Desc::DueCast;

namespace
{
    Ms ms(long long v)
    {
        return Ms(v);
    }

    std::size_t IndexOf(std::uint32_t id)
    {
        for (std::size_t i = 0; i < kTable.size(); ++i)
            if (kTable[i].id == id)
                return i;
        return kTable.size();
    }

    // ---- the table ---------------------------------------------------------------------------------------------

    void TestTable()
    {
        // Every id once; the reworked ids are there; the ids the rework gave to talents are not.
        std::set<std::uint32_t> ids;
        for (KickSpell const& k : kTable)
            CHECK(ids.insert(k.id).second);
        for (std::uint32_t id : { 6552u, 72u, 201473u, 200605u, 200335u, 1766u, 57994u, 2139u, 47528u, 34490u,
            15487u, 19244u, 19647u })
            CHECK(Find(id) != nullptr);
        for (std::uint32_t id : { 1671u, 1672u, 1767u, 1768u, 1769u, 0u, 133u })
            CHECK(Find(id) == nullptr);
        CHECK(Find(19647)->caster == Caster::Pet);
        CHECK(Find(19244)->caster == Caster::Pet);
        CHECK(Find(201473)->caster == Caster::Bot);
        CHECK(std::string_view(Find(6554)->name) == "Pummel");
        // Ranks of one kick sit together, highest first.
        CHECK(IndexOf(6554) + 1 == IndexOf(6552));
        CHECK(IndexOf(38768) + 1 == IndexOf(1766));
        CHECK(IndexOf(19647) + 1 == IndexOf(19244));
    }

    void TestKnownKicks()
    {
        // A warrior with Pummel rank 1 only and Shield Bash: one entry each, table order.
        std::set<std::uint32_t> warrior = { 6552, 72 };
        std::vector<std::size_t> k = KnownKicks([&](KickSpell const& s) { return warrior.count(s.id) > 0; });
        CHECK(k.size() == 2);
        CHECK(k.size() == 2 && kTable[k[0]].id == 6552 && kTable[k[1]].id == 72);

        // Both ranks known: only the highest.
        std::set<std::uint32_t> both = { 6552, 6554 };
        k = KnownKicks([&](KickSpell const& s) { return both.count(s.id) > 0; });
        CHECK(k.size() == 1 && kTable[k[0]].id == 6554);

        // A warlock whose Felhunter knows rank 1 only.
        k = KnownKicks([](KickSpell const& s) { return s.id == 19244; });
        CHECK(k.size() == 1 && kTable[k[0]].caster == Caster::Pet);

        // A druid: Skull Bash before Solar Beam. Nobody: nothing.
        k = KnownKicks([](KickSpell const& s) { return s.id == 200335 || s.id == 200605; });
        CHECK(k.size() == 2 && kTable[k[0]].id == 200605 && kTable[k[1]].id == 200335);
        CHECK(KnownKicks([](KickSpell const&) { return false; }).empty());
    }

    void TestChooseKick()
    {
        Ms const now = ms(10000);
        // Pummel ready: Pummel.
        std::vector<KickOption> o = { { IndexOf(6552), true, now, ms(10000) }, { IndexOf(72), true, now, ms(12000) } };
        CHECK(ChooseKick(o, now) == std::optional<std::size_t>(0));
        // Pummel on cooldown, Shield Bash ready: Shield Bash.
        o[0].readyAt = now + ms(4000);
        CHECK(ChooseKick(o, now) == std::optional<std::size_t>(1));
        // Both on cooldown: the one back first.
        o[1].readyAt = now + ms(6000);
        CHECK(ChooseKick(o, now) == std::optional<std::size_t>(0));
        o[0].readyAt = now + ms(7000);
        CHECK(ChooseKick(o, now) == std::optional<std::size_t>(1));
        // A druid in caster form: Skull Bash does not fit, Solar Beam neither: none.
        std::vector<KickOption> d = { { IndexOf(200605), false, now, ms(16000) },
            { IndexOf(200335), false, now, ms(60000) } };
        CHECK(!ChooseKick(d, now));
        // In Cat Form: Skull Bash.
        d[0].formOk = true;
        CHECK(ChooseKick(d, now) == std::optional<std::size_t>(0));
        CHECK(!ChooseKick({}, now));
    }

    // ---- the cast ----------------------------------------------------------------------------------------------

    void TestInterruptible()
    {
        CastShape c;
        c.preparing = true;
        c.castTime = 2000;
        c.silencePrevention = true;
        c.interruptFlag = true;
        CHECK(Interruptible(c));
        CastShape instant = c;
        instant.castTime = 0;           // an instant still preparing (GCD queue): nothing to stop
        CHECK(!Interruptible(instant));
        CastShape noFlag = c;
        noFlag.interruptFlag = false;
        CHECK(!Interruptible(noFlag));
        CastShape pacify = c;
        pacify.silencePrevention = false;   // PreventionType pacify: a kick does not stop it
        CHECK(!Interruptible(pacify));
        CastShape channel;
        channel.channel = true;
        channel.casting = true;
        channel.silencePrevention = true;
        channel.interruptFlag = true;       // the generic flag says nothing about a channel
        CHECK(!Interruptible(channel));
        channel.channelInterruptFlag = true;
        CHECK(Interruptible(channel));
        CastShape idle;
        CHECK(!Interruptible(idle));
    }

    void TestCanonicalStart()
    {
        // No note: the estimate, new.
        Start s = CanonicalStart(std::nullopt, ms(5000), ms(300));
        CHECK(s.at == ms(5000) && s.fresh);
        // A note within tolerance either side: the note.
        s = CanonicalStart(ms(4900), ms(5000), ms(300));
        CHECK(s.at == ms(4900) && !s.fresh);
        s = CanonicalStart(ms(5250), ms(5000), ms(300));
        CHECK(s.at == ms(5250) && !s.fresh);
        // The previous cast's note: too far, a new cast.
        s = CanonicalStart(ms(1000), ms(5000), ms(300));
        CHECK(s.at == ms(5000) && s.fresh);
        // Bots estimating a few ms apart all land on the first one's start.
        Start const first = CanonicalStart(std::nullopt, ms(7003), ms(300));
        for (long long jitter : { -80, -10, 0, 25, 99 })
            CHECK(CanonicalStart(first.at, ms(7003 + jitter), ms(300)).at == ms(7003));
    }

    // ---- who kicks ---------------------------------------------------------------------------------------------

    CandidateFacts Cand(std::uint8_t priority, Ms start, std::vector<bool> inRange)
    {
        CandidateFacts c;
        c.priority = priority;
        c.start = start;
        c.inRange = std::move(inRange);
        return c;
    }

    void TestAssignBasic()
    {
        Ms const now = ms(20000);
        Ms const defer = ms(600);
        std::vector<KickerFacts> k = { { 1, now, ms(10000) }, { 2, now, ms(10000) }, { 3, now, ms(10000) } };
        std::vector<CandidateFacts> one = { Cand(2, now, { true, true, true }) };
        // The lowest key kicks; the others do not.
        CHECK(Assign(one, k, 0, {}, now, defer).cast == std::optional<std::size_t>(0));
        CHECK(!Assign(one, k, 1, {}, now, defer).cast);
        CHECK(!Assign(one, k, 2, {}, now, defer).cast);
        // Out of range: the next one.
        one[0].inRange = { false, true, true };
        CHECK(!Assign(one, k, 0, {}, now, defer).cast);
        CHECK(Assign(one, k, 1, {}, now, defer).cast == std::optional<std::size_t>(0));
        // Two casts at once, ordered: bot 1 takes the first, bot 2 the second, bot 3 nothing.
        std::vector<CandidateFacts> two = { Cand(3, now, { true, true, true }), Cand(1, now, { true, true, true }) };
        CHECK(Assign(two, k, 0, {}, now, defer).cast == std::optional<std::size_t>(0));
        CHECK(Assign(two, k, 1, {}, now, defer).cast == std::optional<std::size_t>(1));
        CHECK(!Assign(two, k, 2, {}, now, defer).cast);
        // A self index past the kickers (a bot with no kick): nothing.
        CHECK(!Assign(two, k, 7, {}, now, defer).cast);
    }

    void TestAssignDefer()
    {
        Ms const defer = ms(600);
        std::vector<KickerFacts> k = { { 1, ms(0), ms(10000) }, { 2, ms(0), ms(10000) } };
        // Bot 1 is picked; bot 2 waits until the cast has run `defer` unclaimed, then may take it.
        std::vector<CandidateFacts> c = { Cand(2, ms(1000), { true, true }) };
        CHECK(!Assign(c, k, 1, {}, ms(1000), defer).cast);
        CHECK(!Assign(c, k, 1, {}, ms(1599), defer).cast);
        CHECK(Assign(c, k, 1, {}, ms(1600), defer).cast == std::optional<std::size_t>(0));
        // A bot out of range never defers in.
        c[0].inRange = { true, false };
        CHECK(!Assign(c, k, 1, {}, ms(3000), defer).cast);
    }

    // Two kickers on one fast caster (Arugal HM): casts every 6 s, kicks back after 10 s. They alternate, no cast is
    // kicked twice (the first to act claims it), none is missed.
    void TestAlternation()
    {
        Ms const defer = ms(600);
        Ms const cd = ms(10000);
        std::vector<Ms> ready = { ms(0), ms(0) };
        std::vector<Key> who;
        Desc::ClaimBook claims;
        for (int i = 0; i < 10; ++i)
        {
            Ms const start = ms(6000 * i + 1000);
            std::vector<CandidateFacts> c = { Cand(2, start, { true, true }) };
            Desc::CastKey const key{ 900, 205010, start };
            // Each bot thinks 100 ms after the start; the first to be assigned claims and kicks.
            Ms const now = start + ms(100);
            std::vector<KickerFacts> k = { { 1, ready[0], cd }, { 2, ready[1], cd } };
            for (std::size_t self : { 1u, 0u })
            {
                Assignment const a = Assign(c, k, self, {}, now, defer);
                if (a.cast && claims.Take(key, k[self].bot, start + ms(2000), now))
                {
                    who.push_back(k[self].bot);
                    ready[self] = now + cd;
                }
            }
            CHECK(claims.Holder(key, now) != 0);
        }
        CHECK(who.size() == 10);
        for (std::size_t i = 1; i < who.size(); ++i)
            CHECK(who[i] != who[i - 1]);
    }

    // The overload hold (Sneed: Repairs high every 12 s, Tazer low every 4 s; one kicker, kick back after 10 s).
    void TestAssignHold()
    {
        Ms const defer = ms(600);
        Ms const now = ms(30000);
        std::vector<KickerFacts> one = { { 1, now, ms(10000) } };
        std::vector<CandidateFacts> low = { Cand(1, now, { true }) };
        // Repairs due in 5 s: the kicker holds the Tazer.
        std::vector<DueCast> due = { { 3, now + ms(5000) } };
        Assignment a = Assign(low, one, 0, due, now, defer);
        CHECK(!a.cast);
        CHECK(a.held.size() == 1 && a.held[0] == 0);
        // Repairs due in 11 s (after the kick is back): kick the Tazer.
        due = { { 3, now + ms(11000) } };
        a = Assign(low, one, 0, due, now, defer);
        CHECK(a.cast == std::optional<std::size_t>(0));
        CHECK(a.held.empty());
        // Repairs due in 5 s but a second kicker is ready by then: the first kicks the Tazer, the second keeps its
        // kick.
        due = { { 3, now + ms(5000) } };
        std::vector<KickerFacts> two = { { 1, now, ms(10000) }, { 2, now + ms(3000), ms(10000) } };
        std::vector<CandidateFacts> lowTwo = { Cand(1, now, { true, true }) };
        a = Assign(lowTwo, two, 0, due, now, defer);
        CHECK(a.cast == std::optional<std::size_t>(0));
        // A higher cast in progress and unclaimed counts as due now: with Repairs and Tazer both up, one kicker kicks
        // Repairs (ordered first) and does not hold it.
        std::vector<CandidateFacts> both = { Cand(3, now, { true }), Cand(1, now, { true }) };
        due = { { 3, now }, { 1, now } };
        a = Assign(both, one, 0, due, now, defer);
        CHECK(a.cast == std::optional<std::size_t>(0));
        // Do-not-kick casts never reach Assign (OrderInterrupts leaves them out); a held bot does not defer in.
        due = { { 3, now + ms(5000) } };
        std::vector<CandidateFacts> lowLate = { Cand(1, now - ms(1500), { true }) };
        CHECK(!Assign(lowLate, one, 0, due, now, defer).cast);
    }

    // M5 WP-F (R m-9): another kicker covers a due cast only when it reaches that cast's caster.
    void TestAssignHoldReach()
    {
        Ms const defer = ms(600);
        Ms const now = ms(30000);
        std::vector<KickerFacts> two = { { 1, now, ms(10000) }, { 2, now + ms(3000), ms(10000) } };
        std::vector<CandidateFacts> lowTwo = { Cand(1, now, { true, true }) };
        // The high cast due in 5 s comes from caster 77.
        std::vector<DueCast> const due = { { 3, now + ms(5000), 77 } };
        // Kicker 2 reaches caster 77: it covers it, kicker 1 kicks the low cast now (the plain rule's answer).
        std::vector<Desc::DueReach> reach = { { 77, { 1, 2 } } };
        CHECK(Assign(lowTwo, two, 0, due, now, defer, reach).cast == std::optional<std::size_t>(0));
        CHECK(Assign(lowTwo, two, 0, due, now, defer).cast == std::optional<std::size_t>(0));
        // Kicker 2 is out of range of caster 77: no cover, kicker 1 holds its kick for the high cast.
        reach = { { 77, { 1 } } };
        Assignment const a = Assign(lowTwo, two, 0, due, now, defer, reach);
        CHECK(!a.cast);
        CHECK(a.held.size() == 1 && a.held[0] == 0);
        // A due cast whose caster the list does not name (only noted, no unit) keeps the plain rule.
        reach = { { 88, { 1 } } };
        CHECK(Assign(lowTwo, two, 0, due, now, defer, reach).cast == std::optional<std::size_t>(0));
    }

    // M5 WP-F (R M-2): the spells that stop a cast besides the kick table.
    void TestStopsCasts()
    {
        CHECK(StopsCasts(kEffectInterruptCast, 0));
        CHECK(StopsCasts(kEffectApplyAura, kAuraModStun));          // Hammer of Justice, Bash, Kidney Shot
        CHECK(StopsCasts(kEffectApplyAura, kAuraModSilence));       // Silence, Arcane Torrent
        CHECK(StopsCasts(kEffectApplyAura, kAuraModPacifySilence));
        CHECK(StopsCasts(kEffectApplyAura, kAuraModConfuse));       // Polymorph, Sap, Repentance
        CHECK(StopsCasts(kEffectApplyAura, kAuraModFear));          // Psychic Scream, Fear
        CHECK(!StopsCasts(kEffectApplyAura, 3));                    // a periodic damage aura
        CHECK(!StopsCasts(kEffectApplyAura, 26));                   // root (MOD_ROOT): the cast goes on
        CHECK(!StopsCasts(2, kAuraModStun));                        // school damage, the aura field unused
        CHECK(!StopsCasts(0, 0));
    }

    // ---- stock kicks and counters ------------------------------------------------------------------------------

    void TestStockVerdict()
    {
        TargetCast t;
        CHECK(JudgeStockKick(t) == StockVerdict::Allow);        // nothing to stop: stock decides
        t.casting = true;
        CHECK(JudgeStockKick(t) == StockVerdict::Allow);        // unlisted, nothing due
        t.listedDue = true;
        CHECK(JudgeStockKick(t) == StockVerdict::Hold);         // unlisted, a listed cast due before the kick is back
        t.listed = true;
        CHECK(JudgeStockKick(t) == StockVerdict::Listed);       // listed: boss kick owns it
        t.claimedByOther = true;
        CHECK(JudgeStockKick(t) == StockVerdict::Claimed);
        t.doNotKick = true;
        CHECK(JudgeStockKick(t) == StockVerdict::DoNotKick);    // do-not-kick wins over everything
        TargetCast dnk;
        dnk.casting = true;
        dnk.doNotKick = true;
        dnk.listed = true;                                      // a kick stops the do-not-kick cast too
        CHECK(JudgeStockKick(dnk) == StockVerdict::DoNotKick);
    }

    void TestClassify()
    {
        TargetCast t;
        CHECK(Classify(t, false) == Outcome::None);
        t.casting = true;
        CHECK(Classify(t, false) == Outcome::Unlisted);
        t.listedDue = true;
        CHECK(Classify(t, false) == Outcome::Wasted);
        TargetCast l;
        l.casting = true;
        l.listed = true;
        CHECK(Classify(l, true) == Outcome::Landed);
        CHECK(Classify(l, false) == Outcome::Duplicate);
        l.claimedByOther = true;
        CHECK(Classify(l, true) == Outcome::Duplicate);
        l.doNotKick = true;
        CHECK(Classify(l, true) == Outcome::DoNotKick);
    }

    void TestMissed()
    {
        Ms const slack = ms(150);
        // Gone at or after its end, unclaimed: completed.
        CHECK(Missed(ms(5000), ms(5000), false, slack));
        CHECK(Missed(ms(5000), ms(5400), false, slack));
        CHECK(Missed(ms(5000), ms(4900), false, slack));    // within the slack of a map update
        // Gone well before its end: stopped (a kick, a stun, a death), not a miss.
        CHECK(!Missed(ms(5000), ms(3000), false, slack));
        // Claimed: a kick went out on it.
        CHECK(!Missed(ms(5000), ms(5200), true, slack));
    }

    void TestMarkOnce()
    {
        std::vector<Mark> marks;
        Desc::CastKey const a{ 10, 205010, ms(1000) };
        Desc::CastKey const b{ 10, 205010, ms(5000) };
        CHECK(MarkOnce(marks, a, 1, ms(1100), ms(60000)));
        CHECK(!MarkOnce(marks, a, 1, ms(1200), ms(60000)));
        CHECK(MarkOnce(marks, a, 2, ms(1200), ms(60000)));      // another kind
        CHECK(MarkOnce(marks, b, 1, ms(5100), ms(60000)));      // another cast
        // Old marks go.
        CHECK(MarkOnce(marks, a, 1, ms(70000), ms(60000)));
        CHECK(marks.size() <= 3);
    }
}

int main()
{
    TestTable();
    TestKnownKicks();
    TestChooseKick();
    TestInterruptible();
    TestCanonicalStart();
    TestAssignBasic();
    TestAssignDefer();
    TestAlternation();
    TestAssignHold();
    TestAssignHoldReach();
    TestStopsCasts();
    TestStockVerdict();
    TestClassify();
    TestMissed();
    TestMarkOnce();
    return Report("kick_rules_test");
}
