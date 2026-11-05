// Standalone tests for src/MechanicDescriptor.h and src/MechanicDescriptorPresets.h (STAGE-M5-HANDOFF §3.2-§3.5,
// §5, WP-D): conditions, interrupt ordering and the do-not-kick rule, cast notes and due times, the overload hold
// timeline, kicker alternation, kick claims and their expiry, the dispel pick with urgent and deadline entries,
// purges, the target pick (priority, who, conditions, kill together, GUID follow, break-out order) and the ignore
// filter. Build: tests/run.sh.

#include "check.h"
#include "MechanicDescriptor.h"
#include "MechanicDescriptorPresets.h"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace BossMechanics;
using namespace BossMechanics::Desc;

namespace
{
    Ms ms(long long v)
    {
        return Ms(v);
    }

    UnitFacts Facts(Key guid, std::uint32_t entry, float pct = 100.f)
    {
        UnitFacts u;
        u.guid = guid;
        u.entry = entry;
        u.healthPct = pct;
        return u;
    }

    // ---- conditions --------------------------------------------------------------------------------------------

    void TestConditions()
    {
        Context ctx;
        ctx.phase = 2;
        ctx.ownerHealthPct = 55.f;
        UnitFacts u = Facts(10, 500);
        u.auras = { 7001 };
        u.near = { { 600, 6.f } };
        u.zones = { 42 };

        CHECK(Holds(Condition::Always(), u, ctx));
        CHECK(Holds(Condition::HasAura(7001), u, ctx));
        CHECK(!Holds(Condition::HasAura(7002), u, ctx));
        CHECK(!Holds(Condition::LacksAura(7001), u, ctx));
        CHECK(Holds(Condition::LacksAura(7002), u, ctx));
        CHECK(Holds(Condition::NearUnit(600, 8.f), u, ctx));
        CHECK(Holds(Condition::NearUnit(600, 6.f), u, ctx));
        CHECK(!Holds(Condition::NearUnit(600, 5.f), u, ctx));
        CHECK(!Holds(Condition::NearUnit(601, 50.f), u, ctx));
        CHECK(Holds(Condition::InZone(42), u, ctx));
        CHECK(!Holds(Condition::InZone(43), u, ctx));
        CHECK(Holds(Condition::Phase(2), u, ctx));
        CHECK(!Holds(Condition::Phase(1), u, ctx));
        CHECK(Holds(Condition::OwnerBelowPct(60.f), u, ctx));     // Springvale's Holy Light below 60%
        CHECK(!Holds(Condition::OwnerBelowPct(50.f), u, ctx));

        // Custom: the view's cached result for that very condition; never evaluated here, absent = false.
        int calls = 0;
        Condition custom = Condition::Custom([&calls](::Unit const*)
        {
            ++calls;
            return true;
        });
        CHECK(!Holds(custom, u, ctx));
        u.custom.emplace_back(&custom, true);
        CHECK(Holds(custom, u, ctx));
        Condition other = Condition::Custom([](::Unit const*) { return true; });
        CHECK(!Holds(other, u, ctx));
        CHECK(calls == 0);

        // References: what the view gathers.
        Descriptor d;
        d.interrupts.emplace_back(0, 1, 1, ms(0), Condition::HasAura(7001));
        d.targets.emplace_back(500, 1, Who::Dps, Condition::NearUnit(600, 5.f));
        d.targets.emplace_back(501, 1, Who::Dps, Condition::NearUnit(600, 12.f));
        d.ignores.emplace_back(502, Condition::InZone(42));
        d.ignores.emplace_back(503, Condition::LacksAura(7001));
        d.ignores.emplace_back(504, std::move(custom));
        References const r = Referenced(d);
        CHECK(r.auras.size() == 1 && r.auras[0] == 7001);
        CHECK(r.near.size() == 1 && r.near[0].first == 600 && r.near[0].second == 12.f);
        CHECK(r.zones.size() == 1 && r.zones[0] == 42);
        CHECK(r.custom.size() == 1 && r.custom[0] == &d.ignores[2].when);
    }

    // ---- interrupts --------------------------------------------------------------------------------------------

    enum : std::uint32_t
    {
        SNEED = 100,
        EVOKER = 101,
        REPAIRS = 9001,
        TAZER = 9002,
        VOLLEY = 9003,
        INCISION = 9004,
        HOLY_LIGHT = 9005
    };

    Descriptor KickList()
    {
        Descriptor d;
        d.interrupts.emplace_back(SNEED, REPAIRS, 3);  // Sneed Repairs > Tazer
        d.interrupts.emplace_back(0, TAZER, 2, ms(8000));
        d.interrupts.emplace_back(EVOKER, VOLLEY, 1, ms(6000));
        d.interrupts.emplace_back(0, INCISION, 0);  // do-not-kick
        d.interrupts.emplace_back(SNEED, HOLY_LIGHT, 4, ms(0), Condition::OwnerBelowPct(60.f));
        return d;
    }

    void TestMatchAndOrder()
    {
        Descriptor d = KickList();
        Context ctx;
        UnitFacts const none;
        CHECK(MatchInterrupt(d, SNEED, REPAIRS, none, ctx) == std::optional<std::size_t>(0));
        CHECK(MatchInterrupt(d, EVOKER, TAZER, none, ctx) == std::optional<std::size_t>(1));     // any caster
        CHECK(!MatchInterrupt(d, SNEED, VOLLEY, none, ctx));      // Volley is the Evoker's
        CHECK(!MatchInterrupt(d, SNEED, 1234, none, ctx));
        // A condition that fails leaves the cast unlisted (Holy Light above 60%).
        CHECK(!MatchInterrupt(d, SNEED, HOLY_LIGHT, none, ctx));
        ctx.ownerHealthPct = 40.f;
        CHECK(MatchInterrupt(d, SNEED, HOLY_LIGHT, none, ctx) == std::optional<std::size_t>(4));
        ctx.ownerHealthPct = 100.f;

        // A caster-specific entry wins over a generic one for the same spell, whatever the list order.
        Descriptor g;
        g.interrupts.emplace_back(0, TAZER, 1);
        g.interrupts.emplace_back(SNEED, TAZER, 5);
        CHECK(MatchInterrupt(g, SNEED, TAZER, none, ctx) == std::optional<std::size_t>(1));
        CHECK(MatchInterrupt(g, EVOKER, TAZER, none, ctx) == std::optional<std::size_t>(0));

        // Do-not-kick: listed, never picked.
        CHECK(IsDoNotKick(d, 777, INCISION, none, ctx));
        CHECK(!IsDoNotKick(d, SNEED, REPAIRS, none, ctx));
        CHECK(!IsDoNotKick(d, SNEED, 1234, none, ctx));

        std::vector<CastFacts> casts = {
            { 1, EVOKER, VOLLEY, ms(0), ms(3000) },
            { 2, SNEED, TAZER, ms(0), ms(2500) },
            { 3, 999, INCISION, ms(0), ms(1500) },
            { 4, SNEED, REPAIRS, ms(500), ms(3500) },
            { 5, 999, 4242, ms(0), ms(2000) },        // unlisted
            { 6, EVOKER, VOLLEY, ms(0), ms(2000) },   // same priority, ends sooner
        };
        std::vector<KickOption> o = OrderInterrupts(d, casts, ctx, ms(1000));
        CHECK(o.size() == 4);
        if (o.size() == 4)
        {
            CHECK(o[0].cast == 3 && o[0].priority == 3);      // Repairs
            CHECK(o[1].cast == 1 && o[1].priority == 2);      // Tazer
            CHECK(o[2].cast == 5);                            // Volley ending at 2000 before the one at 3000
            CHECK(o[3].cast == 0);
        }
        for (KickOption const& k : o)
            CHECK(k.cast != 2 && k.cast != 4);                // never the do-not-kick, never unlisted

        // Claimed by another bot, or already complete: left out.
        casts[3].claimedByOther = true;
        o = OrderInterrupts(d, casts, ctx, ms(2200));
        CHECK(o.size() == 2 && o[0].cast == 1 && o[1].cast == 0);
        o = OrderInterrupts(d, casts, ctx, ms(5000));
        CHECK(o.empty());

        // An aura-keyed entry reads the caster's facts.
        Descriptor a;
        a.interrupts.emplace_back(0, VOLLEY, 2, ms(0), Condition::LacksAura(555));
        UnitFacts shielded = Facts(7, EVOKER);
        shielded.auras = { 555 };
        std::vector<CastFacts> two = { { 7, EVOKER, VOLLEY, ms(0), ms(3000), false, &shielded },
            { 8, EVOKER, VOLLEY, ms(0), ms(3000) } };
        o = OrderInterrupts(a, two, ctx, ms(0));
        CHECK(o.size() == 1 && o[0].cast == 1);
    }

    void TestNotesAndDue()
    {
        CastNoteBook book;
        book.NoteNext(1, SNEED, REPAIRS, ms(10000));
        CHECK(book.NoteStart(2, EVOKER, VOLLEY, ms(1000)));
        CHECK(!book.NoteStart(2, EVOKER, VOLLEY, ms(1000)));      // same start: nothing changed
        CHECK(book.NoteStart(2, EVOKER, VOLLEY, ms(7000)));
        CHECK(book.All().size() == 2);
        CastNote const* n = book.Find(1, REPAIRS);
        CHECK(n && n->nextAt == ms(10000) && !n->lastStart && n->casterEntry == SNEED);
        CHECK(!book.Find(1, VOLLEY));

        InterruptEntry const repairs(SNEED, REPAIRS, 3);
        InterruptEntry const volley(EVOKER, VOLLEY, 1, ms(6000));
        CHECK(NextDue(repairs, n, ms(4000)) == std::optional<Ms>(ms(10000)));
        CHECK(!NextDue(repairs, n, ms(11000)));       // stale note, no cadence
        CHECK(!NextDue(repairs, nullptr, ms(0)));
        CastNote const* v = book.Find(2, VOLLEY);
        CHECK(NextDue(volley, v, ms(8000)) == std::optional<Ms>(ms(13000)));
        CHECK(NextDue(volley, v, ms(13000)) == std::optional<Ms>(ms(13000)));
        CHECK(NextDue(volley, v, ms(13001)) == std::optional<Ms>(ms(19000)));     // rolled forward
        CHECK(NextDue(volley, v, ms(31000)) == std::optional<Ms>(ms(31000)));
        // A note beats the cadence while it is ahead.
        book.NoteNext(2, EVOKER, VOLLEY, ms(9000));
        CHECK(NextDue(volley, book.Find(2, VOLLEY), ms(8000)) == std::optional<Ms>(ms(9000)));
        CHECK(NextDue(volley, book.Find(2, VOLLEY), ms(9500)) == std::optional<Ms>(ms(13000)));

        // Prune by age; DropCaster.
        CHECK(!book.Prune(ms(9000) + CastNoteBook::kKeep));
        CHECK(book.Prune(ms(10001) + CastNoteBook::kKeep));      // both older than kKeep now
        CHECK(book.All().empty());
        book.NoteNext(5, 1, 1, ms(0));
        book.NoteNext(6, 1, 1, ms(0));
        CHECK(book.DropCaster(5) && !book.DropCaster(5) && book.All().size() == 1);

        // DueCasts: priorities from the descriptor, do-not-kick and unlisted left out, conditions on the caster.
        Descriptor d = KickList();
        std::vector<CastNote> notes = {
            { 1, SNEED, REPAIRS, ms(9000), std::nullopt },
            { 2, EVOKER, VOLLEY, std::nullopt, ms(1000) },
            { 3, 999, INCISION, ms(5000), std::nullopt },
            { 4, 999, 4242, ms(5000), std::nullopt },
            { 1, SNEED, HOLY_LIGHT, ms(6000), std::nullopt },
        };
        Context ctx;
        auto noFacts = [](Key) -> UnitFacts const* { return nullptr; };
        std::vector<DueCast> due = DueCasts(d, notes, ctx, ms(2000), noFacts);
        CHECK(due.size() == 2);
        if (due.size() == 2)
        {
            CHECK(due[0].priority == 3 && due[0].at == ms(9000));
            CHECK(due[1].priority == 1 && due[1].at == ms(7000));
        }
        ctx.ownerHealthPct = 30.f;
        CHECK(DueCasts(d, notes, ctx, ms(2000), noFacts).size() == 3);
    }

    // The overload hold on a timeline: Sneed's Repairs (priority 3, noted every 12 s) and Volley (priority 1) from an
    // Evoker every 4 s; one kicker, 10 s kick. Decision §7.1 #2 for an unlisted cast.
    void TestOverloadHold()
    {
        Ms const cd = ms(10000);
        // High cast due in 5 s: the kick for a lower cast now would be back at 10 s, too late: hold.
        std::vector<DueCast> due = { { 3, ms(5000) } };
        CHECK(HoldKick(1, ms(0), cd, due, {}));
        // Due after the kick is back: kick the low cast now.
        due = { { 3, ms(10000) } };
        CHECK(!HoldKick(1, ms(0), cd, due, {}));
        due = { { 3, ms(12000) } };
        CHECK(!HoldKick(1, ms(0), cd, due, {}));
        // Same or lower priority due: no hold (only a higher cast justifies it).
        due = { { 1, ms(2000) }, { 0, ms(1000) } };
        CHECK(!HoldKick(1, ms(0), cd, due, {}));
        // Unlisted (priority 0): kicked only when no listed cast is due before the kick is back.
        due = { { 1, ms(9000) } };
        CHECK(HoldKick(0, ms(0), cd, due, {}));
        CHECK(!HoldKick(0, ms(0), cd, {}, {}));
        CHECK(!HoldKick(0, ms(0), ms(8000), due, {}));
        // Another kicker ready by then covers the high cast: no hold.
        due = { { 3, ms(5000) } };
        CHECK(!HoldKick(1, ms(0), cd, due, { ms(4000) }));
        CHECK(!HoldKick(1, ms(0), cd, due, { ms(5000) }));
        CHECK(HoldKick(1, ms(0), cd, due, { ms(6000) }));
        // The top priority never holds.
        CHECK(!HoldKick(3, ms(0), cd, due, {}));

        // Timeline: Repairs every 24 s from 6 s (noted), Volley every 4 s from 0; the kicker follows the rules.
        // Expect every Repairs kicked, Volleys let through when a Repairs comes before the kick is back.
        Ms ready = ms(0);
        int repairsKicked = 0;
        int repairsMissed = 0;
        int volleysKicked = 0;
        int volleysHeld = 0;
        for (long long t = 0; t < 120000; t += 1000)
        {
            Ms const now = ms(t);
            bool const repairs = t >= 6000 && (t - 6000) % 24000 == 0;
            bool const volley = t % 4000 == 0;
            Ms const nextRepairs = ms(t < 6000 ? 6000 : 6000 + ((t - 6000) / 24000 + 1) * 24000);
            if (repairs)
            {
                if (ready <= now)
                {
                    ++repairsKicked;
                    ready = now + cd;
                }
                else
                    ++repairsMissed;
            }
            else if (volley && ready <= now)
            {
                if (HoldKick(1, now, cd, { { 3, nextRepairs } }, {}))
                    ++volleysHeld;
                else
                {
                    ++volleysKicked;
                    ready = now + cd;
                }
            }
        }
        CHECK(repairsMissed == 0);
        CHECK(repairsKicked == 5);
        CHECK(volleysHeld > 0);
        CHECK(volleysKicked > 0);
    }

    void TestKickers()
    {
        // Lowest key among the ready and in-range kickers.
        std::vector<Kicker> k = { { 30, ms(0) }, { 20, ms(5000) }, { 10, ms(0), false } };
        CHECK(PickKicker(k, ms(0)) == std::optional<Key>(30));
        CHECK(PickKicker(k, ms(5000)) == std::optional<Key>(20));
        CHECK(!PickKicker({ { 1, ms(100) } }, ms(0)));
        CHECK(!PickKicker({}, ms(0)));

        // Arugal HM: a fast caster every 6 s, two kickers with a 10 s kick: they alternate, every cast kicked once.
        std::vector<Kicker> two = { { 1, ms(0) }, { 2, ms(0) } };
        ClaimBook claims;
        std::vector<Key> who;
        int duplicates = 0;
        for (long long t = 0; t < 60000; t += 6000)
        {
            CastKey const cast{ 900, VOLLEY, ms(t) };
            Ms const end = ms(t + 2500);
            std::optional<Key> const kicker = PickKicker(two, ms(t));
            CHECK(kicker.has_value());
            if (!kicker)
                continue;
            CHECK(claims.Take(cast, *kicker, end, ms(t)));
            // The other bot sees the claim and does not kick.
            for (Kicker const& other : two)
                if (other.bot != *kicker && claims.Take(cast, other.bot, end, ms(t)))
                    ++duplicates;
            who.push_back(*kicker);
            for (Kicker& kk : two)
                if (kk.bot == *kicker)
                    kk.readyAt = ms(t + 10000);
        }
        CHECK(duplicates == 0);
        CHECK(who.size() == 10);
        for (std::size_t i = 1; i < who.size(); ++i)
            CHECK(who[i] != who[i - 1]);
    }

    void TestClaims()
    {
        ClaimBook b;
        CastKey const a{ 1, TAZER, ms(1000) };
        CastKey const next{ 1, TAZER, ms(9000) };       // the same caster's next cast is another claim
        CHECK(b.Take(a, 10, ms(3500), ms(1000)));
        CHECK(!b.Take(a, 11, ms(3500), ms(1200)));
        CHECK(b.Take(a, 10, ms(3600), ms(1300)));       // its own: refreshed
        CHECK(b.Holder(a, ms(2000)) == 10);
        CHECK(b.Take(next, 11, ms(11500), ms(9000)));
        // Expiry: the claim ends with the cast; another bot may then take that key.
        CHECK(b.Holder(a, ms(3600)) == 0);
        CHECK(b.Take(a, 11, ms(4000), ms(3600)));
        CHECK(b.Holder(a, ms(3700)) == 11);
        // Release and prune.
        CHECK(!b.Release(a, 10));
        CHECK(b.Release(a, 11));
        CHECK(b.Holder(a, ms(3700)) == 0);
        CHECK(b.All().size() == 1);
        CHECK(!b.Prune(ms(11000)));
        CHECK(b.Prune(ms(11500)));
        CHECK(b.All().empty());
        b.Take(a, 10, ms(5000), ms(0));
        b.Take({ 2, TAZER, ms(0) }, 10, ms(5000), ms(0));
        b.DropCaster(1);
        CHECK(b.All().size() == 1 && b.All()[0].cast.caster == 2);
    }

    // ---- dispels and purges ------------------------------------------------------------------------------------

    void TestDispels()
    {
        enum : std::uint32_t
        {
            STUN = 1,
            HAUNT = 2,
            SLOW = 3,
            CORRUPTION = 4,
            BURN = 5
        };
        Descriptor d;
        d.dispels.emplace_back(STUN, 1, true);  // loss of control
        d.dispels.emplace_back(HAUNT, 1, false, ms(4000));  // deadline: 8 s aura, before 4 s left
        d.dispels.emplace_back(SLOW, 3);  // Malicia: Slow above Corruption
        d.dispels.emplace_back(CORRUPTION, 2);

        CHECK(IsUrgent(d.dispels[0], ms(99999)));
        CHECK(!IsUrgent(d.dispels[1], ms(6000)));
        CHECK(IsUrgent(d.dispels[1], ms(4000)));
        CHECK(!IsUrgent(d.dispels[2], ms(10)));
        CHECK(FindDispel(d, SLOW) == std::optional<std::size_t>(2));
        CHECK(!FindDispel(d, BURN));

        std::vector<Affliction> a = {
            { 1, CORRUPTION, ms(9000), 3 },
            { 2, SLOW, ms(9000), 3 },
            { 3, BURN, ms(1000), 0 },           // unlisted: the stock order's
            { 4, HAUNT, ms(6000), 1 },
            { 5, STUN, ms(5000), 3 },
        };
        std::vector<DispelPick> o = OrderDispels(d, a);
        CHECK(o.size() == 4);
        if (o.size() == 4)
        {
            CHECK(o[0].affliction == 4 && o[0].urgent);       // the stun first
            CHECK(o[1].affliction == 1);                      // Slow (3) > Corruption (2) > Haunt (1)
            CHECK(o[2].affliction == 0);
            CHECK(o[3].affliction == 3 && !o[3].urgent);
        }
        CHECK(OrderDispels(d, a, true).size() == 1);
        // The deadline debuff turns urgent at 4 s left and jumps the queue (it never expires undispelled); with less
        // time left than the stun it even goes first.
        a[3].remaining = ms(3900);
        o = OrderDispels(d, a);
        CHECK(o.size() == 4 && o[0].affliction == 3 && o[0].urgent && o[1].affliction == 4);
        CHECK(OrderDispels(d, a, true).size() == 2);
        // Among urgent ones: priority, then less time left, then the stock rank, then the key.
        Descriptor u;
        u.dispels.emplace_back(STUN, 1, true);
        std::vector<Affliction> s = { { 9, STUN, ms(3000), 2 }, { 8, STUN, ms(2000), 2 }, { 7, STUN, ms(2000), 1 },
            { 6, STUN, ms(2000), 1 } };
        o = OrderDispels(u, s);
        CHECK(o.size() == 4 && o[0].affliction == 3 && o[1].affliction == 2 && o[2].affliction == 1
            && o[3].affliction == 0);
        CHECK(OrderDispels(Descriptor(), a).empty());
    }

    void TestPurges()
    {
        Descriptor d;
        d.purges.emplace_back(EVOKER, 4979, 1);  // the Evoker's Quick Flame Ward
        d.purges.emplace_back(0, 8599, 2);  // Enrage on anyone
        std::vector<EnemyBuff> b = { { 1, SNEED, 4979 }, { 2, EVOKER, 4979 }, { 3, 300, 8599 }, { 4, EVOKER, 1 } };
        std::optional<PurgePick> p = PickPurge(d, b);
        CHECK(p && p->buff == 2 && p->entry == 1);
        b.erase(b.begin() + 2);
        p = PickPurge(d, b);
        CHECK(p && p->buff == 1 && p->entry == 0);      // not Sneed's ward: the entry names the Evoker
        CHECK(!PickPurge(d, { { 1, SNEED, 4979 } }));
        CHECK(!PickPurge(Descriptor(), b));
    }

    // ---- targets and the ignore list ---------------------------------------------------------------------------

    BotFacts const kTank{ true, false, false };
    BotFacts const kHealer{ false, true, false };
    BotFacts const kMelee{ false, false, true };
    BotFacts const kRanged{ false, false, false };

    void TestFits()
    {
        CHECK(Fits(Who::Any, kTank) && Fits(Who::Any, kHealer) && Fits(Who::Any, kMelee) && Fits(Who::Any, kRanged));
        CHECK(!Fits(Who::Dps, kTank) && !Fits(Who::Dps, kHealer) && Fits(Who::Dps, kMelee) && Fits(Who::Dps, kRanged));
        CHECK(Fits(Who::Melee, kMelee) && !Fits(Who::Melee, kRanged) && !Fits(Who::Melee, kTank));
        CHECK(Fits(Who::Ranged, kRanged) && !Fits(Who::Ranged, kMelee) && !Fits(Who::Ranged, kHealer));
        CHECK(Fits(Who::Tank, kTank) && !Fits(Who::Tank, kMelee));
        CHECK(Fits(Who::Healer, kHealer) && !Fits(Who::Healer, kRanged));
    }

    std::vector<TargetCandidate> Cands(std::vector<UnitFacts> const& units)
    {
        std::vector<TargetCandidate> out;
        for (UnitFacts const& u : units)
            out.push_back({ &u });
        return out;
    }

    Key PickedGuid(Descriptor const& d, std::vector<TargetCandidate> const& c, Key owner, BotFacts const& bot,
        Key current = 0, Context const& ctx = {})
    {
        std::optional<TargetPick> const p = PickTarget(d, c, owner, bot, current, ctx);
        return p ? c[p->candidate].facts->guid : 0;
    }

    void TestTargets()
    {
        constexpr Key BOSS = 1;
        enum : std::uint32_t
        {
            E_BOSS = 200,
            E_ADD = 201,
            E_RAT = 202,
            E_RIFLE = 203,
            E_PHANTOM = 204,
            E_LOW = 205
        };
        std::vector<UnitFacts> units = { Facts(BOSS, E_BOSS), Facts(2, E_ADD, 80.f), Facts(3, E_ADD, 40.f),
            Facts(4, E_LOW) };
        auto c = Cands(units);

        // No list: the boss (implicit priority 0) for everyone; unlisted adds are the stock pickers'.
        Descriptor none;
        CHECK(PickedGuid(none, c, BOSS, kMelee) == BOSS);
        CHECK(PickedGuid(none, c, BOSS, kTank) == BOSS);

        // Add over boss (Rhahk'Zor, Sneed's Shredder): DPS take the add with less health left; tanks keep the boss.
        Descriptor d;
        d.targets.emplace_back(E_ADD, 1);
        d.targets.emplace_back(E_LOW, -1);
        CHECK(PickedGuid(d, c, BOSS, kMelee) == 3);
        CHECK(PickedGuid(d, c, BOSS, kRanged) == 3);
        CHECK(PickedGuid(d, c, BOSS, kTank) == BOSS);
        std::optional<TargetPick> p = PickTarget(d, c, BOSS, kMelee, 0, {});
        CHECK(p && p->outranksBoss && p->priority == 1 && !p->aoeOk && p->entry == std::optional<std::size_t>(0));
        p = PickTarget(d, c, BOSS, kTank, 0, {});
        CHECK(p && !p->outranksBoss && !p->entry);
        // The current target wins a tie (no flip-flop between two adds of one priority).
        CHECK(PickedGuid(d, c, BOSS, kMelee, 2) == 2);
        // Come and go: the adds die, the boss; then only the below-boss unit is left.
        units[1].alive = false;
        units[2].alive = false;
        CHECK(PickedGuid(d, c, BOSS, kMelee) == BOSS);
        units[0].alive = false;
        CHECK(PickedGuid(d, c, BOSS, kMelee) == 4);
        units[0].alive = units[1].alive = units[2].alive = true;

        // aoeOk (Forresten's rats).
        Descriptor rats;
        rats.targets.push_back(TargetEntry(E_ADD, 2, Who::Any).AoeOk());
        p = PickTarget(rats, c, BOSS, kTank, 0, {});
        CHECK(p && p->aoeOk && p->outranksBoss);

        // Aura-keyed (Willey's aiming riflemen), zone-conditional (phantoms in firelight), phase.
        std::vector<UnitFacts> u2 = { Facts(BOSS, E_BOSS), Facts(10, E_RIFLE), Facts(11, E_RIFLE),
            Facts(12, E_PHANTOM) };
        u2[2].auras = { 777 };
        auto c2 = Cands(u2);
        Descriptor cond;
        cond.targets.emplace_back(E_RIFLE, 2, Who::Dps, Condition::HasAura(777));
        cond.targets.emplace_back(E_PHANTOM, 1, Who::Dps, Condition::InZone(55));
        cond.targets.emplace_back(E_RIFLE, 3, Who::Dps, Condition::Phase(2));
        CHECK(PickedGuid(cond, c2, BOSS, kRanged) == 11);
        u2[2].auras.clear();
        CHECK(PickedGuid(cond, c2, BOSS, kRanged) == BOSS);
        u2[3].zones = { 55 };
        CHECK(PickedGuid(cond, c2, BOSS, kRanged) == 12);
        Context phase2;
        phase2.phase = 2;
        CHECK(PickedGuid(cond, c2, BOSS, kRanged, 0, phase2) == 10);

        // Role split on a shared pool (Marduk / Vectus): melee on one, ranged on the other.
        std::vector<UnitFacts> u3 = { Facts(20, 300), Facts(21, 301) };
        auto c3 = Cands(u3);
        Descriptor split;
        split.targets.emplace_back(300, 1, Who::Melee);
        split.targets.emplace_back(301, 1, Who::Ranged);
        CHECK(PickedGuid(split, c3, 0, kMelee) == 20);
        CHECK(PickedGuid(split, c3, 0, kRanged) == 21);
        CHECK(PickedGuid(split, c3, 0, kTank) == 0);      // nothing for the tank: stock

        // GUID follow (Jandice): one of three same-entry units, followed through swaps and a changed entry.
        std::vector<UnitFacts> u4 = { Facts(BOSS, E_BOSS), Facts(30, 400), Facts(31, 400), Facts(32, 400) };
        auto c4 = Cands(u4);
        Descriptor follow;
        follow.targets.emplace_back(400, 1);
        follow.targets.push_back(TargetEntry::ForUnit(31, 2));
        CHECK(PickedGuid(follow, c4, BOSS, kMelee) == 31);
        std::swap(u4[1], u4[2]);                // positions and list order trade places
        CHECK(PickedGuid(follow, c4, BOSS, kMelee) == 31);
        u4[1].entry = 401;                      // a guid entry ignores the entry
        CHECK(PickedGuid(follow, c4, BOSS, kMelee) == 31);
        // A guid entry wins over an entry entry on the same unit even at a lower priority.
        Descriptor both;
        both.targets.emplace_back(400, 5);
        both.targets.push_back(TargetEntry::ForUnit(30, 1));
        std::optional<std::size_t> const m = MatchTarget(both, u4[2], kMelee, {});
        CHECK(u4[2].guid == 30 && m == std::optional<std::size_t>(1));

        // Break-out (Ras HM tombs, Nerub'enkan's cocoon): the tank's prison, then the healer's, then the others'.
        std::vector<UnitFacts> u5 = { Facts(BOSS, E_BOSS), Facts(40, 500), Facts(41, 500), Facts(42, 500) };
        std::vector<TargetCandidate> c5 = { { &u5[0] }, { &u5[1], HeldRank::Other }, { &u5[2], HeldRank::Healer },
            { &u5[3], HeldRank::Tank } };
        Descriptor tomb;
        tomb.targets.push_back(TargetEntry(500, 2, Who::Any).BreakOut());
        CHECK(PickedGuid(tomb, c5, BOSS, kMelee) == 42);
        CHECK(PickedGuid(tomb, c5, BOSS, kMelee, 40) == 42);  // the held rank beats the current target
        u5[3].alive = false;
        CHECK(PickedGuid(tomb, c5, BOSS, kHealer) == 41);
        u5[2].alive = false;
        CHECK(PickedGuid(tomb, c5, BOSS, kTank) == 40);

        // Ignore: never picked, whatever its priority.
        Descriptor ign;
        ign.targets.emplace_back(E_ADD, 5);
        ign.ignores.emplace_back(E_ADD, Condition::HasAura(888));
        units[2].auras = { 888 };
        CHECK(PickedGuid(ign, c, BOSS, kMelee) == 2);
        units[1].auras = { 888 };
        CHECK(PickedGuid(ign, c, BOSS, kMelee) == BOSS);
        units[1].auras.clear();
        units[2].auras.clear();
    }

    // Kill together (Ravenian / Corvane): two units at 100%, every bot hits the pick, 0.5% per hit. The two stay
    // within togetherPct of each other all the way down and die within it.
    void TestKillTogether()
    {
        std::vector<UnitFacts> u = { Facts(50, 600), Facts(51, 601) };
        auto c = Cands(u);
        Descriptor d;
        d.targets.push_back(TargetEntry(600, 1, Who::Any).KillTogether(601, 10.f));
        d.targets.push_back(TargetEntry(601, 1, Who::Any).KillTogether(600, 10.f));
        Key current = 0;
        float maxGap = 0.f;
        int switches = 0;
        for (int hit = 0; hit < 400 && (u[0].alive || u[1].alive); ++hit)
        {
            std::optional<TargetPick> const p = PickTarget(d, c, 0, kMelee, current, {});
            CHECK(p.has_value());
            if (!p)
                break;
            UnitFacts& target = u[p->candidate];
            if (current && target.guid != current)
                ++switches;
            current = target.guid;
            target.healthPct -= 0.5f;
            if (target.healthPct <= 0.f)
            {
                target.healthPct = 0.f;
                target.alive = false;
            }
            if (u[0].alive && u[1].alive)
                maxGap = std::max(maxGap, std::fabs(u[0].healthPct - u[1].healthPct));
        }
        CHECK(!u[0].alive && !u[1].alive);
        CHECK(maxGap <= 10.f + 0.5f + 1e-3f);
        CHECK(switches >= 2);
        // Once the partner is dead the survivor is fair game.
        u[0] = Facts(50, 600, 5.f);
        u[1] = Facts(51, 601, 90.f);
        u[1].alive = false;
        CHECK(PickedGuid(d, c, 0, kMelee) == 50);
        // Far below its partner: dropped, the partner is the pick.
        u[1].alive = true;
        CHECK(PickedGuid(d, c, 0, kMelee, 50) == 51);
    }

    void TestIgnore()
    {
        Descriptor d;
        d.ignores.emplace_back(700, Condition::Always());  // Jandice's copies
        d.ignores.emplace_back(701, Condition::LacksAura(9));  // phantoms outside firelight
        d.ignores.push_back(IgnoreEntry::ForUnit(77));  // one unit
        d.ignores.emplace_back();  // matches nothing
        Context ctx;
        CHECK(IsIgnored(d, Facts(1, 700), ctx));
        UnitFacts ph = Facts(2, 701);
        CHECK(IsIgnored(d, ph, ctx));
        ph.auras = { 9 };
        CHECK(!IsIgnored(d, ph, ctx));
        CHECK(IsIgnored(d, Facts(77, 999), ctx));
        CHECK(!IsIgnored(d, Facts(78, 999), ctx));
        CHECK(!IsIgnored(Descriptor(), Facts(1, 700), ctx));
        // The filter over an attackers list (bots, pets and AoE counts read the same list).
        std::vector<UnitFacts> attackers = { Facts(1, 700), Facts(3, 702), ph, Facts(77, 5), Facts(4, 703) };
        attackers.erase(std::remove_if(attackers.begin(), attackers.end(), [&](UnitFacts const& u)
        {
            return IsIgnored(d, u, ctx);
        }), attackers.end());
        CHECK(attackers.size() == 3 && attackers[0].guid == 3 && attackers[1].guid == 2 && attackers[2].guid == 4);
    }

    void TestPresets()
    {
        Preset const* none = FindPreset("none");
        CHECK(none && none->build);
        if (none && none->build)
        {
            Descriptor const d = none->build();
            CHECK(d.Empty() && d.phase == 0);
        }
        CHECK(!FindPreset("bogus"));
        for (Preset const& p : Presets())
        {
            CHECK(p.build != nullptr);
            CHECK(FindPreset(p.name) == &p);
        }
        CHECK(Descriptor().Empty());
        Descriptor d;
        d.ignores.emplace_back(1);
        CHECK(!d.Empty());
    }
}

int main()
{
    TestConditions();
    TestMatchAndOrder();
    TestNotesAndDue();
    TestOverloadHold();
    TestKickers();
    TestClaims();
    TestDispels();
    TestPurges();
    TestFits();
    TestTargets();
    TestKillTogether();
    TestIgnore();
    TestPresets();
    return Report("descriptor_test");
}
