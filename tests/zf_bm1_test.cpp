// Standalone tests for the Zul'Farrak BM-1 additions (HANDOFF §3.11): the ignore entry for one role (F33, gap 14:
// IgnoreEntry::who, Desc::IgnoreConcerns, Desc::IsIgnored with the bot, Desc::PickTarget, TargetRules::DecideFocus),
// the heal-until rule (F17: HealUntilEntry, Desc::HealReadPct), the marker visibility option's rules (F11:
// Judge::SeenOnlyWithFits, Judge::ResolveFromMarker) and the far healer's soak rule (F14:
// Assign::HealerSoakOutOfReach, composed with Assign::SoakClaimers as the bot view does). Build: tests/run.sh.

#include "check.h"
#include "MechanicAssign.h"
#include "MechanicDescriptor.h"
#include "MechanicJudge.h"
#include "MechanicMath.h"
#include "MechanicTargetRules.h"
#include <cstdint>
#include <vector>

using namespace BossMechanics;
using namespace BossMechanics::Desc;

namespace
{
    constexpr std::uint32_t NPC_UKORZ = 7267;
    constexpr std::uint32_t NPC_RUUZLU = 7797;
    constexpr std::uint32_t SPELL_FURY_LOW = 213900;    // a stand-in: "the partner's Fury is lower"
    constexpr std::uint32_t SPELL_GRIEVOUS_WOUND = 213901;
    constexpr std::uint32_t SPELL_OTHER_BLEED = 213902;

    UnitFacts Unit(std::uint64_t guid, std::uint32_t entry, std::vector<std::uint32_t> auras = {})
    {
        UnitFacts u;
        u.guid = guid;
        u.entry = entry;
        u.auras = std::move(auras);
        return u;
    }

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

    // ---- F33: IgnoreEntry::who ----

    void TestIgnoreForRole()
    {
        Context const ctx;
        UnitFacts const ruuzlu = Unit(2, NPC_RUUZLU);

        // The default stays every bot (and the bot-less call, as before).
        Descriptor any;
        any.ignores.emplace_back(NPC_RUUZLU);
        CHECK(any.ignores.front().who == Who::Any);
        CHECK(IsIgnored(any, ruuzlu, ctx));
        for (BotFacts const& b : { kTank, kHealer, kMeleeDps, kRangedDps })
            CHECK(IsIgnored(any, ruuzlu, ctx, &b));

        // DPS only: the damage dealers ignore it, the tank and the healer keep it; no bot facts: not applied.
        Descriptor dps;
        dps.ignores.push_back(IgnoreEntry(NPC_RUUZLU).For(Who::Dps));
        CHECK(IsIgnored(dps, ruuzlu, ctx, &kMeleeDps) && IsIgnored(dps, ruuzlu, ctx, &kRangedDps));
        CHECK(!IsIgnored(dps, ruuzlu, ctx, &kTank) && !IsIgnored(dps, ruuzlu, ctx, &kHealer));
        CHECK(!IsIgnored(dps, ruuzlu, ctx));
        CHECK(!IsIgnored(dps, Unit(1, NPC_UKORZ), ctx, &kMeleeDps));
        CHECK(IgnoreConcerns(dps.ignores.front(), &kRangedDps) && !IgnoreConcerns(dps.ignores.front(), nullptr));

        // A narrower set (ranged only) and a guid entry.
        Descriptor ranged;
        ranged.ignores.push_back(IgnoreEntry::ForUnit(2).For(Who::Ranged));
        CHECK(IsIgnored(ranged, ruuzlu, ctx, &kRangedDps) && !IsIgnored(ranged, ruuzlu, ctx, &kMeleeDps));

        // The condition still gates it (the partner's Fury is lower).
        Descriptor cond;
        cond.ignores.push_back(IgnoreEntry(NPC_RUUZLU, Condition::HasAura(SPELL_FURY_LOW)).For(Who::Dps));
        CHECK(!IsIgnored(cond, ruuzlu, ctx, &kMeleeDps));
        CHECK(IsIgnored(cond, Unit(2, NPC_RUUZLU, { SPELL_FURY_LOW }), ctx, &kMeleeDps));
        References const r = Referenced(cond);
        CHECK(r.auras.size() == 1u && r.auras.front() == SPELL_FURY_LOW);

        // The cheap pre-check is role-blind (a superset): the view still builds the facts for the unit.
        CHECK(TargetRules::MayBeIgnored(dps, 2, NPC_RUUZLU) && !TargetRules::MayBeIgnored(dps, 1, NPC_UKORZ));

        // An Attacks hold still ignores for every bot, whatever the role entries say.
        Descriptor hold = dps;
        hold.holds.push_back(HoldEntry::Attacks(NPC_UKORZ, 0));
        CHECK(IsIgnored(hold, Unit(1, NPC_UKORZ), ctx, &kTank));
    }

    void TestIgnoreForRoleInPicks()
    {
        Context const ctx;
        // Ukorz (owner) and Ruuzlu, both listed for the DPS at priority 1 as the meter kill order does; Ruuzlu is
        // ignored by the DPS bots only.
        Descriptor d;
        d.targets.push_back(TargetEntry(NPC_UKORZ, 1));
        d.targets.push_back(TargetEntry(NPC_RUUZLU, 1));
        d.ignores.push_back(IgnoreEntry(NPC_RUUZLU).For(Who::Dps));
        std::vector<UnitFacts> const facts{ Unit(1, NPC_UKORZ), Unit(2, NPC_RUUZLU) };
        std::vector<TargetCandidate> candidates;
        for (UnitFacts const& f : facts)
            candidates.push_back({ &f, HeldRank::None });

        // A DPS bot on Ruuzlu (its current target) drops to Ukorz.
        std::optional<TargetPick> const dpsPick = PickTarget(d, candidates, 1, kMeleeDps, 2, ctx);
        CHECK(dpsPick && candidates[dpsPick->candidate].facts->guid == 1u);
        // The tank keeps the owner (tanks keep the boss unless an entry names them).
        std::optional<TargetPick> const tankPick = PickTarget(d, candidates, 1, kTank, 2, ctx);
        CHECK(tankPick && candidates[tankPick->candidate].facts->guid == 1u);
        // Without the ignore entry, the DPS keep its current target on a tie.
        Descriptor tie = d;
        tie.ignores.clear();
        std::optional<TargetPick> const tiePick = PickTarget(tie, candidates, 1, kMeleeDps, 2, ctx);
        CHECK(tiePick && candidates[tiePick->candidate].facts->guid == 2u);

        // DecideFocus: Ruuzlu is no lower listed target for the DPS (ignored), but is for nobody else's view.
        TargetRules::Focus const f = TargetRules::DecideFocus(d, candidates, 1, kRangedDps, 2, ctx);
        CHECK(f.target == 1u);
    }

    // ---- F17: healUntil ----

    void TestHealUntil()
    {
        auto has = [](std::vector<std::uint32_t> auras)
        {
            return [auras](std::uint32_t a)
            {
                for (std::uint32_t x : auras)
                    if (x == a)
                        return true;
                return false;
            };
        };
        std::vector<HealUntilEntry> wound;
        wound.emplace_back(SPELL_GRIEVOUS_WOUND, 95.f);
        CHECK(wound.front().readAsPct == kHealUntilReadAsPct && kHealUntilReadAsPct == 80.f);

        // No aura: unchanged.
        CHECK_NEAR(HealReadPct(wound, 88.f, has({})), 88.f, 1e-4);
        CHECK_NEAR(HealReadPct(wound, 88.f, has({ SPELL_OTHER_BLEED })), 88.f, 1e-4);
        // The carrier between 80 and 95 reads 80 (under the 85 heal line); below 80 its real value; at or above 95
        // its real value (the script removes the wound at 95).
        CHECK_NEAR(HealReadPct(wound, 88.f, has({ SPELL_GRIEVOUS_WOUND })), 80.f, 1e-4);
        CHECK_NEAR(HealReadPct(wound, 94.9f, has({ SPELL_GRIEVOUS_WOUND })), 80.f, 1e-4);
        CHECK_NEAR(HealReadPct(wound, 62.f, has({ SPELL_GRIEVOUS_WOUND })), 62.f, 1e-4);
        CHECK_NEAR(HealReadPct(wound, 95.f, has({ SPELL_GRIEVOUS_WOUND })), 95.f, 1e-4);
        CHECK_NEAR(HealReadPct(wound, 100.f, has({ SPELL_GRIEVOUS_WOUND })), 100.f, 1e-4);

        // Several entries: the lowest applying one; never raised; aura 0 does nothing; empty: unchanged.
        std::vector<HealUntilEntry> two = wound;
        two.emplace_back(SPELL_OTHER_BLEED, 90.f, 70.f);
        CHECK_NEAR(HealReadPct(two, 88.f, has({ SPELL_GRIEVOUS_WOUND, SPELL_OTHER_BLEED })), 70.f, 1e-4);
        CHECK_NEAR(HealReadPct(two, 92.f, has({ SPELL_GRIEVOUS_WOUND, SPELL_OTHER_BLEED })), 80.f, 1e-4);
        std::vector<HealUntilEntry> const high{ HealUntilEntry(SPELL_GRIEVOUS_WOUND, 95.f, 90.f) };
        CHECK_NEAR(HealReadPct(high, 50.f, has({ SPELL_GRIEVOUS_WOUND })), 50.f, 1e-4);
        std::vector<HealUntilEntry> const zero{ HealUntilEntry(0, 95.f) };
        CHECK_NEAR(HealReadPct(zero, 88.f, [](std::uint32_t) { return true; }), 88.f, 1e-4);
        CHECK_NEAR(HealReadPct({}, 88.f, [](std::uint32_t) { return true; }), 88.f, 1e-4);

        // A descriptor with only a heal-until entry is not empty (bots copy it).
        Descriptor d;
        CHECK(d.Empty());
        d.healUntil.emplace_back(SPELL_GRIEVOUS_WOUND, 95.f);
        CHECK(!d.Empty());
    }

    // ---- F11: SeenOnlyWith ----

    void TestSeenOnlyWith()
    {
        CHECK(Judge::SeenOnlyWithFits(true, false, 11));
        CHECK(Judge::SeenOnlyWithFits(true, false, 0));
        CHECK(!Judge::SeenOnlyWithFits(true, false, 12));       // past TOTAL_INVISIBILITY_TYPES
        CHECK(!Judge::SeenOnlyWithFits(false, false, 11));      // NoTelegraph: nothing to hide
        CHECK(!Judge::SeenOnlyWithFits(true, true, 11));        // after the commit the markers are out
        CHECK(Judge::kMaxMarkerInvisibilityType == 11u);
        CHECK(Rules::kMarkerInvisibilityValue == 1);
        // The resolve casts from a marker only while it is visible to all.
        CHECK(Judge::ResolveFromMarker(-1));
        CHECK(!Judge::ResolveFromMarker(11) && !Judge::ResolveFromMarker(0));
    }

    // ---- F14: HealerWithinTankReach ----

    void TestHealerSoakReach()
    {
        float const reach = 36.5f;
        // No tank (or no reach): never held back.
        CHECK(!Assign::HealerSoakOutOfReach(-1.f, reach, false));
        CHECK(!Assign::HealerSoakOutOfReach(60.f, 0.f, false));
        // Inside or within reach: offered; past it: not.
        CHECK(!Assign::HealerSoakOutOfReach(0.f, reach, false));
        CHECK(!Assign::HealerSoakOutOfReach(36.5f, reach, false));
        CHECK(Assign::HealerSoakOutOfReach(36.6f, reach, false));
        CHECK(Assign::HealerSoakOutOfReach(60.f, reach, false));
        // A claim held keeps 3 yd more (the tank stepping at the edge does not flip it).
        CHECK(Assign::kHealerSoakReachKeepYd == 3.f);
        CHECK(!Assign::HealerSoakOutOfReach(39.f, reach, true));
        CHECK(Assign::HealerSoakOutOfReach(39.6f, reach, true));

        // Composed as the view does: the far healer is left out of the claimants, so the next bot claims it.
        std::int64_t const deadline = 10000;
        Assign::Claimant healer;
        healer.guid = 5;
        healer.bot = true;
        healer.etaMs = 1000;
        Assign::Claimant dps;
        dps.guid = 9;
        dps.bot = true;
        dps.etaMs = 4000;
        Assign::SoakRule rule;
        rule.count = 1;
        rule.deadlineMs = deadline;
        float const soulToTankYd = 52.f;    // a soul risen 60 yd out, the tank 52 yd from it
        std::vector<Assign::Claimant> all{ healer, dps };
        std::vector<Assign::Claimant> offered;
        for (Assign::Claimant const& c : all)
            if (!(c.guid == healer.guid && Assign::HealerSoakOutOfReach(soulToTankYd, reach, c.claimedSince != 0)))
                offered.push_back(c);
        CHECK(Assign::SoakClaimers(all, rule) == std::vector<std::uint64_t>{ 5 });
        CHECK(Assign::SoakClaimers(offered, rule) == std::vector<std::uint64_t>{ 9 });
    }
}

int main()
{
    TestIgnoreForRole();
    TestIgnoreForRoleInPicks();
    TestHealUntil();
    TestSeenOnlyWith();
    TestHealerSoakReach();
    return Report("zf_bm1_test");
}
