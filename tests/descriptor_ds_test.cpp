// Standalone tests for the Blackrock Depths descriptor additions (WP-DS, shared doc BD3): the hold entries
// (src/MechanicDescriptor.h: HoldEntry, HoldOn, HeldDamageMask, KicksHeld, IsIgnored; src/MechanicTargetRules.h:
// MayBeIgnored), the dispel rank key (DispelEntry::MostStacksFirst, OrderDispels), NearPlayer as the Livewire rule
// reads it, strict spot bounds (src/MechanicDropSpot.h, src/MechanicSolver.h; the role scope is the bot view's), the
// "keep one player in the band" composition (src/MechanicAssign.h) and the WP-DS presets and drills. Build:
// tests/run.sh.

#include "check.h"
#include "BossMechanicsIds.h"
#include "MechanicAssign.h"
#include "MechanicDescriptor.h"
#include "MechanicDescriptorDrills.h"
#include "MechanicDescriptorPresets.h"
#include "MechanicDrillPlan.h"
#include "MechanicDropSpot.h"
#include "MechanicSolver.h"
#include "MechanicTargetRules.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace BossMechanics;
using namespace BossMechanics::Desc;
using Geo::Vec3;
using Solver::Input;
using Solver::Outcome;
using Solver::Plan;

namespace
{
    constexpr std::uint32_t NPC_EVISCERATOR = 9029;
    constexpr std::uint32_t NPC_OFFICER = 8895;
    constexpr std::uint32_t NPC_SHADE = 9038;
    constexpr std::uint32_t SPELL_SHELL = 210700;
    constexpr std::uint32_t SPELL_COMMAND = 210701;
    constexpr std::uint32_t SPELL_INCORPOREAL = 210702;
    constexpr std::uint32_t SPELL_SWP = 14032;
    constexpr std::uint32_t SPELL_DOUBT = 210703;
    constexpr std::uint32_t SPELL_OTHER = 210704;
    constexpr std::uint32_t kFire = 0x04;
    constexpr std::uint32_t kShadow = 0x20;

    UnitFacts Unit(std::uint64_t guid, std::uint32_t entry, std::vector<std::uint32_t> auras = {})
    {
        UnitFacts u;
        u.guid = guid;
        u.entry = entry;
        u.auras = std::move(auras);
        return u;
    }

    // ---- Holds (BD3) ----

    void TestHoldMatch()
    {
        Context const ctx;
        Descriptor d;
        d.holds.push_back(HoldEntry::Damage(NPC_EVISCERATOR, SPELL_SHELL));
        UnitFacts const shelled = Unit(1, NPC_EVISCERATOR, { SPELL_SHELL });
        UnitFacts const bare = Unit(1, NPC_EVISCERATOR);
        UnitFacts const other = Unit(2, NPC_OFFICER, { SPELL_SHELL });

        // Spell Eater: magic held while the shell is up, physical never; another entry with the aura is not held.
        CHECK(HeldDamageMask(d, shelled, ctx) == kSchoolMaskMagic);
        CHECK(DamageHeld(d, shelled, ctx, kFire) && DamageHeld(d, shelled, ctx, kShadow | kFire));
        CHECK(!DamageHeld(d, shelled, ctx, kSchoolMaskPhysical));
        CHECK(HeldDamageMask(d, bare, ctx) == 0);
        CHECK(HeldDamageMask(d, other, ctx) == 0);
        // A Damage hold neither ignores the unit nor holds kicks.
        CHECK(!IsIgnored(d, shelled, ctx) && !KicksHeld(d, shelled, ctx));
        // A dead unit is never held.
        UnitFacts dead = shelled;
        dead.alive = false;
        CHECK(HeldDamageMask(d, dead, ctx) == 0);

        // A school mask of the script's choice (fire only), and the `when` condition on top of the aura.
        Descriptor f;
        f.holds.push_back(HoldEntry::Damage(NPC_EVISCERATOR, SPELL_SHELL, kFire, Condition::Phase(2)));
        CHECK(HeldDamageMask(f, shelled, ctx) == 0);       // phase 0
        Context p2;
        p2.phase = 2;
        CHECK(HeldDamageMask(f, shelled, p2) == kFire);
        CHECK(!DamageHeld(f, shelled, p2, kShadow));

        // By guid (whatever the entry), and entry 0: any non-player unit, never a player.
        Descriptor g;
        g.holds.push_back(HoldEntry::Damage(0, SPELL_SHELL).OnUnit(7));
        CHECK(HeldDamageMask(g, Unit(7, NPC_OFFICER, { SPELL_SHELL }), ctx) == kSchoolMaskMagic);
        CHECK(HeldDamageMask(g, Unit(8, NPC_OFFICER, { SPELL_SHELL }), ctx) == 0);
        Descriptor any;
        any.holds.push_back(HoldEntry::Damage(0, SPELL_SHELL));
        CHECK(HeldDamageMask(any, other, ctx) == kSchoolMaskMagic);
        UnitFacts player = Unit(9, 0, { SPELL_SHELL });
        player.player = true;
        CHECK(HeldDamageMask(any, player, ctx) == 0);
        // auraId 0: held whenever the unit matches.
        Descriptor always;
        always.holds.push_back(HoldEntry::Damage(NPC_EVISCERATOR, 0, kSchoolMaskAll));
        CHECK(HeldDamageMask(always, bare, ctx) == kSchoolMaskAll);
    }

    void TestHoldActions()
    {
        Context const ctx;
        // Angerforge's Commanding Aura: no kicks on an officer with it; damage goes on.
        Descriptor k;
        k.holds.push_back(HoldEntry::Interrupts(NPC_OFFICER, SPELL_COMMAND));
        UnitFacts const commanding = Unit(3, NPC_OFFICER, { SPELL_COMMAND });
        CHECK(KicksHeld(k, commanding, ctx));
        CHECK(!KicksHeld(k, Unit(4, NPC_OFFICER), ctx));
        CHECK(HeldDamageMask(k, commanding, ctx) == 0 && !IsIgnored(k, commanding, ctx));
        CHECK(HasHolds(k, HoldEntry::Action::Interrupts) && !HasHolds(k, HoldEntry::Action::Damage));

        // The Seven's shades: an Attacks hold is the ignore list while the aura is up (every school held too).
        Descriptor a;
        a.holds.push_back(HoldEntry::Attacks(NPC_SHADE, SPELL_INCORPOREAL));
        UnitFacts const shade = Unit(5, NPC_SHADE, { SPELL_INCORPOREAL });
        UnitFacts const solid = Unit(5, NPC_SHADE);
        CHECK(IsIgnored(a, shade, ctx) && !IsIgnored(a, solid, ctx));
        CHECK(HeldDamageMask(a, shade, ctx) == kSchoolMaskAll);
        CHECK(!KicksHeld(a, shade, ctx));       // kicks go through the ignore list's own drops
        CHECK(TargetRules::MayBeIgnored(a, 5, NPC_SHADE) && !TargetRules::MayBeIgnored(a, 6, NPC_OFFICER));
        CHECK(!TargetRules::MayBeIgnored(k, 3, NPC_OFFICER));
        Descriptor anyShade;
        anyShade.holds.push_back(HoldEntry::Attacks(0, SPELL_INCORPOREAL));
        CHECK(TargetRules::MayBeIgnored(anyShade, 6, NPC_OFFICER));

        // An ignored Attacks unit leaves the target pick (the Seven's kill order skips a shade).
        Descriptor t = a;
        t.targets.emplace_back(NPC_SHADE, 1);
        BotFacts const dps;
        std::vector<TargetCandidate> c{ { &shade, HeldRank::None } };
        CHECK(!PickTarget(t, c, 99, dps, 0, ctx));
        c = { { &solid, HeldRank::None } };
        CHECK(PickTarget(t, c, 99, dps, 0, ctx).has_value());

        // References: the hold's aura and its condition's auras are gathered (the view fills them).
        Descriptor r;
        r.holds.push_back(HoldEntry::Damage(NPC_EVISCERATOR, SPELL_SHELL, kSchoolMaskMagic,
            Condition::LacksAura(SPELL_OTHER)));
        References const refs = Referenced(r);
        CHECK(Contains(refs.auras, SPELL_SHELL) && Contains(refs.auras, SPELL_OTHER));
        CHECK(!r.Empty() && Descriptor().Empty());
    }

    // ---- The dispel rank key (Gerstahn) ----

    void TestMostStacksFirst()
    {
        Descriptor d;
        d.dispels.push_back(DispelEntry(SPELL_SWP, 2).MostStacksFirst(SPELL_DOUBT));
        CHECK(d.dispels.front().rankAura == SPELL_DOUBT);
        // Three members with Shadow Word: Pain: 1, 4 and 2 Doubt; the one with less time left would go first
        // without the key.
        std::vector<Affliction> a = {
            { 10, SPELL_SWP, Ms(2000), 0, false, 1 },
            { 11, SPELL_SWP, Ms(15000), 3, false, 4 },
            { 12, SPELL_SWP, Ms(9000), 1, false, 2 },
        };
        std::vector<DispelPick> const o = OrderDispels(d, a);
        CHECK(o.size() == 3 && o[0].affliction == 1 && o[1].affliction == 2 && o[2].affliction == 0);
        // Ties on stacks fall back to the time left.
        a[2].rankStacks = 4;
        std::vector<DispelPick> const t = OrderDispels(d, a);
        CHECK(t.size() == 3 && t[0].affliction == 2 && t[1].affliction == 1);
        // A held one stays out; urgency and priority still come first.
        a[1].held = true;
        CHECK(OrderDispels(d, a).front().affliction == 2);
        a[1].held = false;
        d.dispels.emplace_back(SPELL_OTHER, 3);
        a.push_back({ 13, SPELL_OTHER, Ms(9000), 0, false, 0 });
        CHECK(OrderDispels(d, a).front().affliction == 3);
        // Without the key the stacks are not read (the old order: time left).
        Descriptor plain;
        plain.dispels.emplace_back(SPELL_SWP, 2);
        std::vector<DispelPick> const p = OrderDispels(plain, std::vector<Affliction>(a.begin(), a.begin() + 3));
        CHECK(p.size() == 3 && p[0].affliction == 0);
    }

    // ---- NearPlayer (Livewire, wave 2) ----

    // The pure half of NearPlayer: FactsOf (MechanicDescriptorView.cpp) fills nearestPlayerYd with the nearest other
    // live player on the unit's map (any player: in a dungeon, the candidate's allies) within the largest range
    // asked, on the plane and within 6 yd of height (NearPlayerDistance); pets and creatures never count. Livewire's
    // dispel: `when = Not(NearPlayer(8))` on the afflicted member.
    void TestNearPlayer()
    {
        Context const ctx;
        Condition const clear = Condition::Not(Condition::NearPlayer(8.f));
        UnitFacts carrier = Unit(20, 0, {});
        carrier.player = true;
        carrier.nearestPlayerYd = -1.f;     // nobody within the range asked
        CHECK(Holds(clear, carrier, ctx));
        carrier.nearestPlayerYd = 8.5f;
        CHECK(Holds(clear, carrier, ctx));
        carrier.nearestPlayerYd = 8.f;      // at 8 yd: still near (the burst's 8 yd is inclusive)
        CHECK(!Holds(clear, carrier, ctx));
        carrier.nearestPlayerYd = 3.f;
        CHECK(!Holds(clear, carrier, ctx));
        CHECK(NearPlayerDistance(6.f, 0.f, 5.9f) == 6.f);
        CHECK(NearPlayerDistance(6.f, 0.f, -6.5f) < 0.f);       // another floor: not near at all
        // The view asks out to the largest NearPlayer range of the descriptor.
        Descriptor d;
        d.dispels.emplace_back(SPELL_OTHER, 2, false, Ms(0), clear);
        d.dispels.emplace_back(SPELL_SWP, 1, false, Ms(0), Condition::NearPlayer(12.f));
        CHECK(Referenced(d).playerRange == 12.f);
        // Held while an ally is near: OrderDispels leaves it out (the caller sets held from DispelHeld).
        CHECK(DispelHeld(d.dispels.front(), carrier, ctx));
    }

    // ---- Strict and role-scoped spot bounds (Lord of the Lake) ----

    // The causeway in miniature: x in [0, 30], y in [-4, 4], floor z 0 (band -3 .. 5). The boss stands off its end at
    // (34, 0); "behind" him (x > 34) is the lava.
    Solver::Bounds Causeway(bool strict)
    {
        Solver::Bounds b;
        b.areas.push_back({ { { 0.f, -4.f, 0.f }, { 30.f, -4.f, 0.f }, { 30.f, 4.f, 0.f }, { 0.f, 4.f, 0.f } }, {}, 0.f,
            -3.f, 5.f });
        b.strict = strict;
        return b;
    }

    struct StraightOracle : Solver::PathOracle
    {
        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            out = { from, to };
            return true;
        }
    };

    Input BotAt(Vec3 const& p)
    {
        Input in;
        in.bot = p;
        in.now = Ms(100000);
        return in;
    }

    void TestStrictBounds()
    {
        Vec3 const behind{ 38.f, 2.f, 0.f };
        Vec3 const front{ 28.f, 1.f, 0.f };
        Solver::Bounds const loose = Causeway(false);
        Solver::Bounds const strict = Causeway(true);
        CHECK(!DropSpot::SameBounds(loose, strict) && DropSpot::SameBounds(strict, Causeway(true)));
        CHECK(DropSpot::StrictBreaks({ loose, strict }, behind) == 1u);
        CHECK(DropSpot::StrictBreaks({ loose, strict }, front) == 0u);
        CHECK(DropSpot::AnyStrict({ loose, strict }) && !DropSpot::AnyStrict({ loose }));

        // F-1b bounds: a bot outside may stay (its own spot is safe). Strict: it is not, and inside is.
        Input in = BotAt(behind);
        in.bounds.push_back(loose);
        Ms const end = in.now + Ms(3000);
        CHECK(Solver::TargetOk(in, behind, in.now, end));
        in.bounds.front().strict = true;
        CHECK(!Solver::TargetOk(in, behind, in.now, end));
        CHECK(Solver::TargetOk(in, front, in.now, end));

        // The solve walks it in: the target keeps the bounds (a candidate on the edge nearest the bot is offered).
        std::vector<Vec3> cands;
        Solver::Detail::StrictCandidates(in, cands);
        CHECK(cands.size() == 2u);
        for (Vec3 const& c : cands)
            CHECK(DropSpot::InBounds(strict, c));
        StraightOracle oracle;
        Plan const p = Solver::Solve(in, oracle);
        CHECK(p.outcome == Outcome::Move);
        CHECK(DropSpot::InBounds(strict, p.target));
        CHECK(Geo::Dist2d(p.target.x, p.target.y, 30.f, 2.f) < 6.f);     // the near end, not the far one

        // Inside already: nothing to do. The same set without strict: Safe where it stands, outside.
        Input inside = BotAt(front);
        inside.bounds.push_back(strict);
        CHECK(Solver::Solve(inside, oracle).outcome == Outcome::Safe);
        Input stay = BotAt(behind);
        stay.bounds.push_back(loose);
        CHECK(Solver::Solve(stay, oracle).outcome == Outcome::Safe);

        // A circle area (the reach band round a point): the edge point toward the bot and the center.
        Input ring = BotAt({ 20.f, 0.f, 0.f });
        Solver::Bounds band;
        band.areas.push_back({ {}, { 0.f, 0.f, 0.f }, 6.f, -100.f, 100.f });
        band.strict = true;
        ring.bounds.push_back(band);
        cands.clear();
        Solver::Detail::StrictCandidates(ring, cands);
        CHECK(cands.size() == 2u);
        if (cands.size() == 2u)
        {
            CHECK_NEAR(cands[0].x, 4.5f, 1e-4);
            CHECK_NEAR(cands[1].x, 0.f, 1e-4);
        }
        Plan const q = Solver::Solve(ring, oracle);
        CHECK(q.outcome == Outcome::Move && DropSpot::InBounds(band, q.target));
    }

    // "Keep one player in band B": a Soak of count 1 on the band, eligible tanks only. Its claimer is the tank (the
    // non-eligible never see it, so the melee are not pushed out of the reach band); a second tank, if any, is an
    // eligible non-claimer and keeps out. A human tank already inside fills it.
    void TestBandComposition()
    {
        Assign::SoakRule rule;
        rule.count = 1;
        rule.deadlineMs = 101000;
        rule.keepSticky = true;
        std::vector<Assign::Claimant> tanksOnly = { { 7, true, false, 0, 100500, 0 } };
        std::vector<std::uint64_t> const c = Assign::SoakClaimers(tanksOnly, rule);
        CHECK(c.size() == 1u && c.front() == 7u);
        tanksOnly.push_back({ 8, false, true, 0, 0, 0 });       // a human tank inside
        std::vector<std::uint64_t> const h = Assign::SoakClaimers(tanksOnly, rule);
        CHECK(h.size() == 1u && h.front() == 8u);
    }

    // ---- Presets and drills ----

    void TestPresetsAndDrills()
    {
        Context const ctx;
        Preset const* magic = FindPreset("hold-magic");
        Preset const* kicks = FindPreset("hold-kicks");
        Preset const* stacks = FindPreset("dispel-stacks");
        CHECK(magic && kicks && stacks);
        if (magic && kicks && stacks)
        {
            Descriptor const m = magic->build();
            UnitFacts const shelled = Unit(3, Ids::NPC_DRILL_ADD, { Ids::SPELL_DRILL_PURGEABLE });
            CHECK(DamageHeld(m, shelled, ctx, kFire) && !DamageHeld(m, Unit(3, Ids::NPC_DRILL_ADD), ctx, kFire));
            CHECK(!m.purges.empty() && m.purges.front().auraId == Ids::SPELL_DRILL_PURGEABLE);
            Descriptor const k = kicks->build();
            CHECK(KicksHeld(k, Unit(4, Ids::NPC_DRILL_CASTER, { Ids::SPELL_DRILL_AIMING }), ctx));
            CHECK(!KicksHeld(k, Unit(5, Ids::NPC_DRILL_CASTER), ctx));
            Descriptor const s = stacks->build();
            CHECK(s.dispels.size() == 1u && s.dispels.front().rankAura == Ids::SPELL_DRILL_TAUNT_SWAP);
        }
        for (char const* name : { "hold-magic", "hold-kicks", "dispel-stacks" })
        {
            Drill::ProgramDef const* d = Drill::FindProgramDrill(name);
            CHECK(d != nullptr);
            std::string err;
            std::optional<Drill::Program> const p = d ? Drill::ParseProgram(d->name, err) : std::nullopt;
            CHECK(p && p->loopMs > 0);
            if (!p)
                std::printf("  drill %s: %s\n", name, err.c_str());
        }
    }
}

int main()
{
    TestHoldMatch();
    TestHoldActions();
    TestMostStacksFirst();
    TestNearPlayer();
    TestStrictBounds();
    TestBandComposition();
    TestPresetsAndDrills();
    return Report("descriptor_ds_test");
}
