// Standalone tests for src/MechanicDispelRules.h (STAGE-M5-HANDOFF §3.4, §5, WP-P): the class tables (no retired
// spell, every class's pick by dispel type, fallbacks when the best spell is not ready), the deadline floor against
// playerbots' 700 ms dispel skip (a tick-and-GCD timeline), the normal-entry skip, how a cast is counted (urgent,
// deadline, listed, purge) and the stock member rank. Desc::OrderDispels itself is covered by descriptor_test.
// Build: tests/run.sh.

#include "check.h"
#include "MechanicDispelRules.h"
#include <algorithm>
#include <vector>

using namespace BossMechanics;
using namespace BossMechanics::DispelRules;

namespace
{
    std::vector<std::uint32_t> Ids(std::vector<ClassSpell> const& spells)
    {
        std::vector<std::uint32_t> out;
        for (ClassSpell const& s : spells)
            out.push_back(s.spellId);
        return out;
    }

    // The class's table spells as a bot that knows all of them and has them ready.
    std::vector<Owned> AllReady(std::uint8_t classId, Use use)
    {
        std::vector<Owned> out;
        for (ClassSpell const& s : SpellsFor(classId, use))
            out.push_back({ s.spellId, s.types, s.pet, true });
        return out;
    }

    std::uint32_t Picked(std::vector<Owned> const& owned, std::uint32_t type)
    {
        std::optional<std::size_t> const i = PickSpell(owned, type);
        return i ? owned[*i].spellId : 0;
    }

    void TestTables()
    {
        // Retired or not-a-dispel spells stay out of every table.
        for (std::uint32_t retired : { 528u, 552u, 526u, 2782u, 2908u, 32375u })
            CHECK(!InTables(retired));
        for (ClassSpell const& s : kSpells)
        {
            CHECK(s.types != 0);
            CHECK(s.name != nullptr);
            CHECK(InTables(s.spellId));
            // Pets: the Felhunter only.
            CHECK(s.pet == (s.classId == kWarlock));
        }
        CHECK((Ids(SpellsFor(kPriest, Use::Friendly)) == std::vector<std::uint32_t>{ 988, 527 }));
        CHECK((Ids(SpellsFor(kPriest, Use::Hostile)) == std::vector<std::uint32_t>{ 988, 527 }));
        CHECK((Ids(SpellsFor(kShaman, Use::Friendly)) == std::vector<std::uint32_t>{ 51886 }));
        CHECK((Ids(SpellsFor(kShaman, Use::Hostile)) == std::vector<std::uint32_t>{ 8012, 370 }));
        CHECK((Ids(SpellsFor(kDruid, Use::Friendly)) == std::vector<std::uint32_t>{ 200604, 2893, 8946 }));
        CHECK(SpellsFor(kDruid, Use::Hostile).empty());
        CHECK((Ids(SpellsFor(kPaladin, Use::Friendly)) == std::vector<std::uint32_t>{ 4987, 1152 }));
        CHECK(SpellsFor(kPaladin, Use::Hostile).empty());
        CHECK((Ids(SpellsFor(kMage, Use::Friendly)) == std::vector<std::uint32_t>{ 475 }));
        CHECK((Ids(SpellsFor(kMage, Use::Hostile)) == std::vector<std::uint32_t>{ 30449 }));
        CHECK((Ids(SpellsFor(kHunter, Use::Hostile)) == std::vector<std::uint32_t>{ 19801 }));
        CHECK(SpellsFor(kHunter, Use::Friendly).empty());
        CHECK(SpellsFor(kWarlock, Use::Hostile).size() == 7);
        CHECK(SpellsFor(kWarlock, Use::Hostile).front().spellId == 48011);
        CHECK(SpellsFor(kWarlock, Use::Hostile).back().spellId == 19505);
        // Classes with no dispel at all.
        for (std::uint8_t cls : { std::uint8_t(1), std::uint8_t(4), std::uint8_t(6) })
        {
            CHECK(SpellsFor(cls, Use::Friendly).empty());
            CHECK(SpellsFor(cls, Use::Hostile).empty());
        }
        CHECK(Bit(0) == 0);
        CHECK(Bit(kMagic) == 2u);
        CHECK(Bit(kEnrage) == 512u);
        CHECK(Bit(40) == 0);
    }

    void TestClassPicks()
    {
        // Druid: Nature's Cure for Magic and Poison; Abolish Poison when Nature's Cure is not ready; no curse cure.
        std::vector<Owned> druid = AllReady(kDruid, Use::Friendly);
        CHECK(Picked(druid, kMagic) == 200604);
        CHECK(Picked(druid, kPoison) == 200604);
        CHECK(Picked(druid, kCurse) == 0);
        CHECK(Picked(druid, kDisease) == 0);
        druid[0].ready = false;
        CHECK(Picked(druid, kMagic) == 0);
        CHECK(Picked(druid, kPoison) == 2893);
        druid[1].ready = false;
        CHECK(Picked(druid, kPoison) == 8946);
        CHECK(TypesOf(druid) == (Bit(kMagic) | Bit(kPoison)));

        // Shaman: Cleanse Spirit is Magic + Curse, nothing for disease or poison (Cure Toxins retired).
        std::vector<Owned> shaman = AllReady(kShaman, Use::Friendly);
        CHECK(Picked(shaman, kMagic) == 51886);
        CHECK(Picked(shaman, kCurse) == 51886);
        CHECK(Picked(shaman, kPoison) == 0);
        CHECK(Picked(shaman, kDisease) == 0);

        // Paladin: Cleanse first; Purify takes disease when Cleanse is on the GCD or out of range; magic waits.
        std::vector<Owned> paladin = AllReady(kPaladin, Use::Friendly);
        CHECK(Picked(paladin, kDisease) == 4987);
        CHECK(Picked(paladin, kMagic) == 4987);
        CHECK(Picked(paladin, kPoison) == 0);
        paladin[0].ready = false;
        CHECK(Picked(paladin, kDisease) == 1152);
        CHECK(Picked(paladin, kMagic) == 0);

        // Priest: Magic only, the higher rank first when known.
        std::vector<Owned> priest = AllReady(kPriest, Use::Friendly);
        CHECK(Picked(priest, kMagic) == 988);
        priest.erase(priest.begin());
        CHECK(Picked(priest, kMagic) == 527);
        CHECK(Picked(priest, kDisease) == 0);

        // Mage: curses only.
        std::vector<Owned> mage = AllReady(kMage, Use::Friendly);
        CHECK(Picked(mage, kCurse) == 475);
        CHECK(Picked(mage, kMagic) == 0);

        // Purges: Enrage is the hunter's alone; Magic buffs for the rest.
        CHECK(Picked(AllReady(kHunter, Use::Hostile), kEnrage) == 19801);
        CHECK(Picked(AllReady(kHunter, Use::Hostile), kMagic) == 19801);
        CHECK(Picked(AllReady(kShaman, Use::Hostile), kMagic) == 8012);
        CHECK(Picked(AllReady(kShaman, Use::Hostile), kEnrage) == 0);
        CHECK(Picked(AllReady(kMage, Use::Hostile), kMagic) == 30449);
        CHECK(Picked(AllReady(kPriest, Use::Hostile), kEnrage) == 0);
        std::vector<Owned> lock = AllReady(kWarlock, Use::Hostile);
        CHECK(Picked(lock, kMagic) == 48011);
        CHECK(lock.front().pet);
        // A pet that only knows the bootstrap rank.
        std::vector<Owned> bootstrap{ { 19505, Bit(kMagic), true, true } };
        CHECK(Picked(bootstrap, kMagic) == 19505);

        // Nothing owned, or a type with no bit.
        CHECK(Picked({}, kMagic) == 0);
        CHECK(Picked(druid, 0) == 0);
        CHECK(TypesOf({}) == 0);
    }

    void TestDeadlineFloor()
    {
        Ms const skip{ 700 };
        CHECK(EffectiveDeadline(Ms(0), skip) == Ms(0));
        CHECK(EffectiveDeadline(Ms(4000), skip) == Ms(4000));
        CHECK(EffectiveDeadline(Ms(1700), skip) == Ms(1700));
        CHECK(EffectiveDeadline(Ms(300), skip) == Ms(1700));
        CHECK(EffectiveDeadline(Ms(300), Ms(0)) == Ms(1000));
        CHECK(EffectiveDeadline(Ms(300), skip, Ms(0)) == Ms(700));

        Descriptor d;
        d.dispels.push_back(DispelEntry(1, 3, true));
        d.dispels.push_back(DispelEntry(2, 1, false, Ms(4000)));
        d.dispels.push_back(DispelEntry(3, 2, false, Ms(300)));
        d.purges.push_back(PurgeEntry(0, 9));
        Descriptor const v = UrgentView(d, skip);
        CHECK(v.dispels.size() == 3);
        CHECK(v.purges.empty());
        CHECK(v.dispels[0].urgent && v.dispels[0].deadline == Ms(0) && v.dispels[0].priority == 3);
        CHECK(v.dispels[1].deadline == Ms(4000));
        CHECK(v.dispels[2].deadline == Ms(1700) && v.dispels[2].priority == 2 && !v.dispels[2].urgent);

        // Unfloored, a 300 ms deadline would turn urgent only inside the stock skip, where HasAuraToDispel no
        // longer sees the aura; floored, it turns urgent with time for a tick and a GCD.
        CHECK(Desc::IsUrgent(d.dispels[2], Ms(300)));
        CHECK(!Desc::IsUrgent(d.dispels[2], Ms(600)));
        CHECK(Desc::IsUrgent(v.dispels[2], Ms(1700)));
        CHECK(!Desc::IsUrgent(v.dispels[2], Ms(1701)));
    }

    // A bot that ticks every `tick` ms and is on a GCD until `gcdEnd` meets a deadline debuff of `duration` ms
    // applied at 0. Returns the aura's time left when the bot cures it (negative: it expired first).
    long long CureLeft(Ms deadline, Ms duration, Ms tick, Ms gcdEnd, Ms skip)
    {
        DispelEntry const e = UrgentView([&]
        {
            Descriptor d;
            d.dispels.push_back(DispelEntry(7, 1, false, deadline));
            return d;
        }(), skip).dispels.front();
        for (Ms t{ 0 }; t <= duration; t += tick)
        {
            Ms const left = duration - t;
            if (Desc::IsUrgent(e, left) && t >= gcdEnd)
                return left.count();
        }
        return -1;
    }

    void TestDeadlineTimeline()
    {
        Ms const skip{ 700 };
        // Haunting Memory: 8 s, deadline 4 s; the bot cures as soon as it turns urgent, well before the skip.
        CHECK(CureLeft(Ms(4000), Ms(8000), Ms(100), Ms(0), skip) == 4000);
        CHECK(CureLeft(Ms(4000), Ms(8000), Ms(250), Ms(5000), skip) == 3000);
        // A 300 ms deadline is floored to 1.7 s. For every tick length up to 300 ms and wherever a running GCD ends,
        // the cure lands at the floor (within one tick), or as soon as the GCD ends when that is later; the aura then
        // still has more than the stock skip left whenever the GCD ends at least a tick before the skip.
        for (long long tick : { 50LL, 100LL, 200LL, 300LL })
            for (long long gcd = 0; gcd <= 8000; gcd += 50)
            {
                long long const left = CureLeft(Ms(300), Ms(8000), Ms(tick), Ms(gcd), skip);
                if (gcd <= 8000 - 1700)
                    CHECK(left >= 1700 - tick && left <= 1700);
                else if (gcd <= 8000 - 700 - tick)
                    CHECK(left > 700);
            }
        // With the skip configured off (0) the floor is the lead alone.
        CHECK(CureLeft(Ms(300), Ms(8000), Ms(100), Ms(0), Ms(0)) == 1000);
    }

    void TestNormalSkip()
    {
        Ms const skip{ 700 };
        CHECK(NormalSkipped(Ms(500), skip));
        CHECK(NormalSkipped(Ms(0), skip));
        CHECK(!NormalSkipped(Ms(700), skip));
        CHECK(!NormalSkipped(Ms(5000), skip));
        CHECK(!NormalSkipped(Ms(-1), skip));       // permanent
        CHECK(!NormalSkipped(Ms(500), Ms(0)));     // Playerbots.DispelAuraDuration = 0
    }

    void TestClassifyCast()
    {
        Descriptor d;
        d.dispels.push_back(DispelEntry(100, 1, true));                 // stun, Magic
        d.dispels.push_back(DispelEntry(200, 2, false, Ms(4000)));      // Haunting Memory, Magic
        d.dispels.push_back(DispelEntry(300, 5));                       // Slow, Magic
        d.dispels.push_back(DispelEntry(400, 1));                       // a curse
        Descriptor const v = UrgentView(d, Ms(700));

        std::vector<TargetAura> auras{ { 300, kMagic, Ms(9000) }, { 100, kMagic, Ms(3000) } };
        CastClass c = ClassifyCast(v, Bit(kMagic), auras);
        CHECK(c.kind == CastKind::Urgent && c.aura == 1);

        auras = { { 300, kMagic, Ms(9000) }, { 200, kMagic, Ms(3500) } };
        c = ClassifyCast(v, Bit(kMagic), auras);
        CHECK(c.kind == CastKind::Deadline && c.aura == 1);

        auras = { { 300, kMagic, Ms(9000) }, { 200, kMagic, Ms(6000) } };
        c = ClassifyCast(v, Bit(kMagic), auras);
        CHECK(c.kind == CastKind::Normal && c.aura == 0);       // Slow's priority 5 beats Haunting Memory's 2

        // The spell cannot remove the aura's type: not counted; a curse cure on a curse is.
        c = ClassifyCast(v, Bit(kPoison), auras);
        CHECK(c.kind == CastKind::None);
        auras = { { 400, kCurse, Ms(-1) } };
        c = ClassifyCast(v, Bit(kCurse) | Bit(kMagic), auras);
        CHECK(c.kind == CastKind::Normal);
        // An unlisted aura.
        auras = { { 999, kMagic, Ms(1000) } };
        CHECK(ClassifyCast(v, Bit(kMagic), auras).kind == CastKind::None);
        CHECK(ClassifyCast(v, Bit(kMagic), {}).kind == CastKind::None);
    }

    // M5 WP-F (R M-1): a cure is counted by the aura its dispel removed (the core picks it at random).
    void TestClassifyRemoval()
    {
        Descriptor d;
        d.dispels.push_back(DispelEntry(100, 1, true));                 // stun, Magic
        d.dispels.push_back(DispelEntry(200, 2, false, Ms(4000)));      // deadline, Magic
        d.dispels.push_back(DispelEntry(300, 5));                       // Slow, Magic
        Descriptor const v = UrgentView(d, Ms(700));

        // The stun removed: urgent; Slow removed while the stun stayed: listed and the wrong aura.
        RemovalCount r = ClassifyRemoval(v, 100, Ms(5000), true);
        CHECK(r.kind == CastKind::Urgent && !r.wrongAura);
        r = ClassifyRemoval(v, 300, Ms(9000), true);
        CHECK(r.kind == CastKind::Normal && r.wrongAura);
        r = ClassifyRemoval(v, 300, Ms(9000), false);
        CHECK(r.kind == CastKind::Normal && !r.wrongAura);
        // The deadline aura inside its deadline counts as one; outside it as a normal cure.
        CHECK(ClassifyRemoval(v, 200, Ms(3000), true).kind == CastKind::Deadline);
        CHECK(!ClassifyRemoval(v, 200, Ms(3000), true).wrongAura);
        CHECK(ClassifyRemoval(v, 200, Ms(6000), false).kind == CastKind::Normal);
        // An unlisted aura removed next to the stun: nothing listed, the wrong aura.
        r = ClassifyRemoval(v, 999, Ms(1000), true);
        CHECK(r.kind == CastKind::None && r.wrongAura);
        CHECK(!ClassifyRemoval(v, 999, Ms(-1), false).wrongAura);

        // The urgent types a cure sees at the cast: Magic for the stun or an urgent deadline, not for Slow alone.
        std::vector<TargetAura> auras{ { 300, kMagic, Ms(9000) }, { 100, kMagic, Ms(3000) } };
        CHECK(UrgentTypes(v, Bit(kMagic), auras) == Bit(kMagic));
        CHECK(UrgentTypes(v, Bit(kCurse), auras) == 0);         // the spell cannot remove Magic
        auras = { { 300, kMagic, Ms(9000) }, { 200, kMagic, Ms(6000) } };
        CHECK(UrgentTypes(v, Bit(kMagic), auras) == 0);
        auras = { { 200, kMagic, Ms(3000) } };
        CHECK(UrgentTypes(v, Bit(kMagic) | Bit(kDisease), auras) == Bit(kMagic));
    }

    // The pending cures: matched by target and type, oldest first, one aura per type, kept kPendingKeep.
    void TestPendingCures()
    {
        PendingCures book;
        CHECK(book.Empty());
        CHECK(!book.Take(5, kMagic, Ms(1000)));
        // Two bots cure member 5 (a Cleanse: Magic + Disease; a Dispel Magic), one member 6.
        book.Note({ 1, 5, Bit(kMagic) | Bit(kDisease), Bit(kMagic), true, Ms(1000) });
        book.Note({ 2, 5, Bit(kMagic), 0, true, Ms(1100) });
        book.Note({ 3, 6, Bit(kMagic), 0, true, Ms(1100) });
        CHECK(book.Size() == 3);
        // A Poison removal on 5: nobody's.
        CHECK(!book.Take(5, kPoison, Ms(1200)));
        // The first Magic removal on 5 is the oldest cure's; its Magic is spent, its Disease effect stays.
        std::optional<PendingCure> c = book.Take(5, kMagic, Ms(1200));
        CHECK(c && c->caster == 1 && c->urgentTypes == Bit(kMagic));
        CHECK(book.Size() == 3);
        c = book.Take(5, kDisease, Ms(1200));
        CHECK(c && c->caster == 1);
        CHECK(book.Size() == 2);
        // The second Magic removal on 5 is bot 2's.
        c = book.Take(5, kMagic, Ms(1300));
        CHECK(c && c->caster == 2);
        CHECK(!book.Take(5, kMagic, Ms(1300)));
        // Bot 3's cure on 6 is forgotten (its cast failed); nothing left.
        book.Forget(3, 6);
        CHECK(book.Empty());
        // Old cures go: one noted at 2 s is not matched at 2 s + kPendingKeep + 1.
        book.Note({ 4, 7, Bit(kMagic), 0, false, Ms(2000) });
        CHECK(!book.Take(7, kMagic, Ms(2000) + kPendingKeep + Ms(1)));
        CHECK(book.Empty());
        book.Note({ 4, 7, Bit(kMagic), 0, false, Ms(2000) });
        CHECK(book.Take(7, kMagic, Ms(2000) + kPendingKeep));
        // The book is bounded.
        for (std::uint32_t i = 0; i < kPendingMax + 10; ++i)
            book.Note({ i, 8, Bit(kMagic), 0, true, Ms(3000) });
        CHECK(book.Size() == kPendingMax);
        c = book.Take(8, kMagic, Ms(3000));
        CHECK(c && c->caster == 10);        // the 10 oldest were dropped
    }

    void TestPurgeCount()
    {
        Descriptor d;
        d.purges.push_back(PurgeEntry(300406, 4979));       // a ward on one add
        d.purges.push_back(PurgeEntry(0, 8599));            // Enrage on anything
        std::vector<EnemyAura> ward{ { 4979, kMagic } };
        std::vector<EnemyAura> enrage{ { 8599, kEnrage } };
        CHECK(CountsAsPurge(d, 300406, Bit(kMagic), ward));
        CHECK(!CountsAsPurge(d, 300405, Bit(kMagic), ward));        // another unit's ward is not listed
        CHECK(!CountsAsPurge(d, 300406, Bit(kEnrage), ward));       // wrong type
        CHECK(CountsAsPurge(d, 1, Bit(kEnrage) | Bit(kMagic), enrage));
        CHECK(!CountsAsPurge(d, 1, Bit(kMagic), enrage));           // Purge does not soothe
        CHECK(!CountsAsPurge(d, 1, Bit(kMagic), {}));
        CHECK(!CountsAsPurge(Descriptor{}, 300406, Bit(kMagic), ward));
    }

    void TestRankAndOrder()
    {
        CHECK(RankOf(true, true, false) == MemberRank::Master);
        CHECK(RankOf(false, true, true) == MemberRank::Healer);
        CHECK(RankOf(false, false, true) == MemberRank::Tank);
        CHECK(RankOf(false, false, false) == MemberRank::Other);

        // Through the urgent view: a floored short deadline is urgent once inside 1.7 s and, at equal priority,
        // goes before the stun because it has less time left (Desc::OrderDispels: urgent ones by priority, then time
        // left); a normal entry never reaches the urgent list, however high its priority.
        Descriptor d;
        d.dispels.push_back(DispelEntry(100, 1, true));
        d.dispels.push_back(DispelEntry(200, 1, false, Ms(300)));
        d.dispels.push_back(DispelEntry(300, 9));
        Descriptor const v = UrgentView(d, Ms(700));
        std::vector<Desc::Affliction> a{
            { 11, 300, Ms(500), static_cast<std::uint8_t>(MemberRank::Master) },
            { 12, 200, Ms(1600), static_cast<std::uint8_t>(MemberRank::Other) },
            { 13, 100, Ms(5000), static_cast<std::uint8_t>(MemberRank::Tank) } };
        std::vector<Desc::DispelPick> const urgent = Desc::OrderDispels(v, a, true);
        CHECK(urgent.size() == 2);
        CHECK(urgent.size() == 2 && a[urgent[0].affliction].member == 12 && a[urgent[1].affliction].member == 13);
        // A higher priority puts the stun first.
        Descriptor d2 = d;
        d2.dispels[0].priority = 2;
        std::vector<Desc::DispelPick> const stunFirst = Desc::OrderDispels(UrgentView(d2, Ms(700)), a, true);
        CHECK(stunFirst.size() == 2 && a[stunFirst[0].affliction].member == 13);
        // In the raw descriptor the 300 ms deadline is not urgent yet at 1.6 s.
        CHECK(Desc::OrderDispels(d, a, true).size() == 1);
        // The full order (the `party member to dispel` override): urgent ones, then the normal entry.
        std::vector<Desc::DispelPick> const all = Desc::OrderDispels(v, a);
        CHECK(all.size() == 3 && a[all[2].affliction].member == 11);
    }
}

int main()
{
    TestTables();
    TestClassPicks();
    TestDeadlineFloor();
    TestDeadlineTimeline();
    TestNormalSkip();
    TestClassifyCast();
    TestClassifyRemoval();
    TestPendingCures();
    TestPurgeCount();
    TestRankAndOrder();
    return Report("dispel_rules_test");
}
