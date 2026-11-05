// Standalone tests for src/MechanicRoleRules.h, the stub role provider's pure rules (dungeon mechanics STAGE-M5 C4):
// the spec tab from a player's learned talent ranks (Player::GetSpec's result without its talent store walk), the
// tank / healer / damage decision (HasTankSpec / HasHealSpec and the owner victim) and the spec cache's freshness.
// Build: tests/run.sh.

#include "check.h"
#include "MechanicRoleRules.h"
#include <array>
#include <cstdint>
#include <vector>

using namespace BossMechanics::RoleRules;

namespace
{
    // Core TalentTree tabs used below beyond the ones the header names.
    constexpr uint32_t kTabWarriorArms = 161;
    constexpr uint32_t kTabWarriorFury = 164;
    constexpr uint32_t kTabPriestShadow = 203;
    constexpr uint32_t kTabDruidBalance = 283;
    constexpr uint32_t kTabMageFire = 41;
    constexpr uint8_t kFormCat = 0x01;
    constexpr uint8_t kFormNone = 0x00;

    std::array<uint32_t, 3> const kWarrior{ kTabWarriorArms, kTabWarriorFury, kTabWarriorProtection };
    std::array<uint32_t, 3> const kPriest{ kTabPriestDiscipline, kTabPriestHoly, kTabPriestShadow };
    std::array<uint32_t, 3> const kDruid{ kTabDruidBalance, kTabDruidFeral, kTabDruidRestoration };

    // `points` talents of `rank` ranks each in one tab, talent ids from `firstId`.
    void Add(std::vector<TalentRank>& v, uint32_t tab, uint32_t firstId, uint32_t talents, uint8_t rank)
    {
        for (uint32_t i = 0; i < talents; ++i)
            v.push_back({ firstId + i, tab, rank });
    }

    void TestSpecTabMostPoints()
    {
        std::vector<TalentRank> v;
        Add(v, kTabWarriorArms, 100, 3, 3);         // 9
        Add(v, kTabWarriorProtection, 200, 10, 5);  // 50
        Add(v, kTabWarriorFury, 300, 4, 2);         // 8
        CHECK(SpecTab(kWarrior, v) == kTabWarriorProtection);

        std::vector<TalentRank> druid;
        Add(druid, kTabDruidBalance, 100, 2, 1);
        Add(druid, kTabDruidFeral, 200, 3, 1);
        CHECK(SpecTab(kDruid, druid) == kTabDruidFeral);
    }

    void TestSpecTabNoTalents()
    {
        std::vector<TalentRank> v;
        CHECK(SpecTab(kWarrior, v) == 0);   // GetSpec: no point anywhere is no spec, not the first tab
        CHECK(SpecTab(kPriest, v) == 0);    // so a talentless priest is not Discipline (a healer)
    }

    // GetSpec walks the tabs in order with a strict "<": the first of tied tabs wins.
    void TestSpecTabTieFirstTab()
    {
        std::vector<TalentRank> v;
        Add(v, kTabPriestShadow, 100, 2, 5);
        Add(v, kTabPriestHoly, 200, 2, 5);
        CHECK(SpecTab(kPriest, v) == kTabPriestHoly);
        Add(v, kTabPriestDiscipline, 300, 5, 2);
        CHECK(SpecTab(kPriest, v) == kTabPriestDiscipline);
    }

    // Per talent only its highest learned rank counts (a lower rank still in the map is not added on top).
    void TestSpecTabHighestRankOnly()
    {
        std::vector<TalentRank> v;
        Add(v, kTabWarriorArms, 100, 3, 3);                     // Arms 9
        for (uint8_t r = 1; r <= 5; ++r)
            v.push_back({ 500, kTabWarriorFury, r });           // Fury 5 (not 15)
        v.push_back({ 501, kTabWarriorFury, 3 });               // Fury 8
        CHECK(SpecTab(kWarrior, v) == kTabWarriorArms);
        v.push_back({ 502, kTabWarriorFury, 2 });               // Fury 10
        CHECK(SpecTab(kWarrior, v) == kTabWarriorFury);
        // Order of the input does not matter.
        std::vector<TalentRank> rev(v.rbegin(), v.rend());
        CHECK(SpecTab(kWarrior, rev) == kTabWarriorFury);
    }

    // Talents of tabs that are not the class's (another class, a custom tab) are not counted.
    void TestSpecTabForeignTabsIgnored()
    {
        std::vector<TalentRank> v;
        Add(v, kTabWarriorArms, 100, 1, 1);
        Add(v, kTabMageFire, 200, 10, 5);
        Add(v, 9999, 300, 10, 5);
        CHECK(SpecTab(kWarrior, v) == kTabWarriorArms);
        std::vector<TalentRank> onlyForeign;
        Add(onlyForeign, kTabMageFire, 200, 10, 5);
        CHECK(SpecTab(kWarrior, onlyForeign) == 0);
    }

    // One spell listed by two talent rows (the index keeps both): each row counts in its own tab.
    void TestSpecTabSpellInTwoTalents()
    {
        std::vector<TalentRank> v;
        v.push_back({ 700, kTabWarriorArms, 2 });
        v.push_back({ 701, kTabWarriorProtection, 2 });
        v.push_back({ 702, kTabWarriorProtection, 1 });
        CHECK(SpecTab(kWarrior, v) == kTabWarriorProtection);
    }

    void TestTankAndHealTabs()
    {
        for (uint32_t tab : { kTabWarriorProtection, kTabPaladinProtection, kTabDeathKnightBlood })
        {
            CHECK(IsTankTab(tab, kFormNone));
            CHECK(!IsHealTab(tab));
        }
        CHECK(IsTankTab(kTabDruidFeral, kFormBear));
        CHECK(IsTankTab(kTabDruidFeral, kFormDireBear));
        CHECK(!IsTankTab(kTabDruidFeral, kFormCat));
        CHECK(!IsTankTab(kTabDruidFeral, kFormNone));
        for (uint32_t tab : { kTabPaladinHoly, kTabPriestDiscipline, kTabPriestHoly, kTabShamanRestoration,
            kTabDruidRestoration })
        {
            CHECK(IsHealTab(tab));
            CHECK(!IsTankTab(tab, kFormBear));
        }
        for (uint32_t tab : { 0u, kTabWarriorArms, kTabWarriorFury, kTabPriestShadow, kTabDruidBalance, kTabMageFire })
        {
            CHECK(!IsHealTab(tab));
            CHECK(!IsTankTab(tab, kFormBear));
        }
    }

    // StubRoleProvider::Get's order, unchanged since M1.
    void TestDecide()
    {
        // The owner's victim is the tank, whatever its spec.
        CHECK(Decide(true, true, kTabPriestHoly, kFormNone) == Kind::Tank);
        CHECK(Decide(true, true, 0, kFormNone) == Kind::Tank);
        // With a victim, nobody else is a tank: a Protection warrior off the boss deals damage.
        CHECK(Decide(true, false, kTabWarriorProtection, kFormNone) == Kind::Damage);
        CHECK(Decide(true, false, kTabDruidFeral, kFormDireBear) == Kind::Damage);
        // ...and a healer is still a healer.
        CHECK(Decide(true, false, kTabPriestDiscipline, kFormNone) == Kind::Healer);
        // Without a victim, the tank spec decides (Feral by form).
        CHECK(Decide(false, false, kTabWarriorProtection, kFormNone) == Kind::Tank);
        CHECK(Decide(false, false, kTabDruidFeral, kFormDireBear) == Kind::Tank);
        CHECK(Decide(false, false, kTabDruidFeral, kFormCat) == Kind::Damage);
        CHECK(Decide(false, false, kTabDruidRestoration, kFormNone) == Kind::Healer);
        CHECK(Decide(false, false, kTabMageFire, kFormNone) == Kind::Damage);
        CHECK(Decide(false, false, 0, kFormNone) == Kind::Damage);
    }

    void TestSpecCacheFresh()
    {
        TalentFingerprint a;
        a.talents = 51;
        a.freePoints = 0;
        a.activeSpec = 0;
        a.specsCount = 2;
        a.level = 60;
        CHECK(SpecCacheFresh(a, 1000, a, 1000));
        CHECK(SpecCacheFresh(a, 1000, a, 1000 + kSpecCacheTtlMs - 1));
        CHECK(!SpecCacheFresh(a, 1000, a, 1000 + kSpecCacheTtlMs));     // the TTL backstop
        CHECK(!SpecCacheFresh(a, 1000, a, 999));                        // a clock going back recomputes

        // Every fingerprint field invalidates on its own.
        TalentFingerprint b = a;
        b.talents = 52;                 // a talent spell taught directly
        CHECK(!SpecCacheFresh(a, 1000, b, 1001));
        b = a;
        b.freePoints = 1;               // a point spent or refunded (learn, reset)
        CHECK(!SpecCacheFresh(a, 1000, b, 1001));
        b = a;
        b.activeSpec = 1;               // dual spec switch
        CHECK(!SpecCacheFresh(a, 1000, b, 1001));
        b = a;
        b.specsCount = 1;
        CHECK(!SpecCacheFresh(a, 1000, b, 1001));
        b = a;
        b.level = 61;
        CHECK(!SpecCacheFresh(a, 1000, b, 1001));
    }
}

int main()
{
    TestSpecTabMostPoints();
    TestSpecTabNoTalents();
    TestSpecTabTieFirstTab();
    TestSpecTabHighestRankOnly();
    TestSpecTabForeignTabsIgnored();
    TestSpecTabSpellInTwoTalents();
    TestTankAndHealTabs();
    TestDecide();
    TestSpecCacheFresh();
    return Report("role_rules_test");
}
