/*
 * mod-boss-mechanics: named descriptor presets for the program entry `describe <name>` (STAGE-M5-HANDOFF §3.2,
 * §3.6): the training dummy's and the sim's stand-in for a boss script's Mechanic::Describe.
 *
 * No AzerothCore includes: tests/drill_plan_test.cpp and tests/descriptor_test.cpp read it with the host compiler.
 * Each preset builds a fresh Descriptor (a Custom condition cannot be a constant). WP-K's drill presets use the M5
 * stand-in ids of BossMechanicsIds.h and serve the descriptor drills (MechanicDescriptorDrills.h): interrupts,
 * dispels, purges, targets (with one `targets-*` preset per target row), ignore, and `descriptor` (every list at once,
 * the dummy's default loop and the sim slice). Programs never use Custom or InZone conditions: a preset cannot know a
 * runtime mechanic id, so the zone row plays InZone with HasAura (STAGE-M5-D-REPORT open item 3).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DESCRIPTOR_PRESETS_H
#define MOD_BOSS_MECHANICS_MECHANIC_DESCRIPTOR_PRESETS_H

#include "BossMechanicsIds.h"
#include "MechanicDescriptor.h"
#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

namespace BossMechanics::Desc
{
    struct Preset
    {
        char const* name = "";
        char const* summary = "";       // one line for `.mech` replies
        Descriptor (*build)() = nullptr;
    };

    // ---- WP-K drill presets (STAGE-M5-HANDOFF §3.6) ----
    namespace DrillPresets
    {
        using std::chrono::milliseconds;
        namespace I = BossMechanics::Ids;

        // Interrupts: low (casters, 1) < mid (2) < high (3), the do-not-kick cast (0) never; the owner's casts are
        // listed for any caster (casterEntry 0: the dummy live, SimTarget in the sim). Cadences match the drills'
        // `every`; the runner's NoteNextCast overrides them.
        inline void AddInterrupts(Descriptor& d)
        {
            d.interrupts.emplace_back(I::NPC_DRILL_CASTER, I::SPELL_DRILL_CAST_LOW, 1, milliseconds(3000));
            d.interrupts.emplace_back(0, I::SPELL_DRILL_CAST_MID, 2, milliseconds(12000));
            d.interrupts.emplace_back(0, I::SPELL_DRILL_CAST_HIGH, 3, milliseconds(12000));
            d.interrupts.emplace_back(0, I::SPELL_DRILL_CAST_NO_KICK, 0);
        }

        // Dispels: the stun urgent, the deadline debuff urgent with 4 s left (of 8), the four types normal.
        inline void AddDispels(Descriptor& d)
        {
            d.dispels.emplace_back(I::SPELL_DRILL_STUN, 3, true);
            d.dispels.emplace_back(I::SPELL_DRILL_DEADLINE, 2, false, milliseconds(4000));
            for (std::uint32_t aura : { I::SPELL_DRILL_DEBUFF_MAGIC, I::SPELL_DRILL_DEBUFF_CURSE,
                I::SPELL_DRILL_DEBUFF_POISON, I::SPELL_DRILL_DEBUFF_DISEASE })
                d.dispels.emplace_back(aura, 1);
        }

        // Purges: the Magic buff before the Enrage, on the Drill Adds.
        inline void AddPurges(Descriptor& d)
        {
            d.purges.emplace_back(I::NPC_DRILL_ADD, I::SPELL_DRILL_PURGEABLE, 2);
            d.purges.emplace_back(I::NPC_DRILL_ADD, I::SPELL_DRILL_ENRAGE, 1);
        }

        inline Descriptor Interrupts()
        {
            Descriptor d;
            AddInterrupts(d);
            return d;
        }

        inline Descriptor Dispels()
        {
            Descriptor d;
            AddDispels(d);
            return d;
        }

        // The buffed adds rank below the dummy, so they live through the drill and keep being rebuffed.
        inline Descriptor Purges()
        {
            Descriptor d;
            AddPurges(d);
            d.targets.emplace_back(I::NPC_DRILL_ADD, -1);
            return d;
        }

        // Fixed order, adds over the boss, come and go: the caster (2), the adds (1), then the dummy (0).
        inline Descriptor Targets()
        {
            Descriptor d;
            d.targets.emplace_back(I::NPC_DRILL_CASTER, 2);
            d.targets.emplace_back(I::NPC_DRILL_ADD, 1);
            return d;
        }

        // Aura-keyed: an add with Aiming (2) before the others (1).
        inline Descriptor TargetsAim()
        {
            Descriptor d;
            d.targets.emplace_back(I::NPC_DRILL_ADD, 2, Who::Dps, Condition::HasAura(I::SPELL_DRILL_AIMING));
            d.targets.emplace_back(I::NPC_DRILL_ADD, 1);
            return d;
        }

        // Zone-conditional: an add "in the zone" (Marked (drill) on it, the InZone stand-in) is a target; outside
        // it is on the ignore list (the Unforgiven's phantoms outside the firelight).
        inline Descriptor TargetsZone()
        {
            Descriptor d;
            d.targets.emplace_back(I::NPC_DRILL_ADD, 1, Who::Dps, Condition::HasAura(I::SPELL_MARKED_DRILL));
            d.ignores.emplace_back(I::NPC_DRILL_ADD, Condition::LacksAura(I::SPELL_MARKED_DRILL));
            return d;
        }

        // Phase: the caster below the dummy, above it in phase 2 (`describe targets-phase phase=<n>`).
        inline Descriptor TargetsPhase()
        {
            Descriptor d;
            d.targets.emplace_back(I::NPC_DRILL_CASTER, -1);
            d.targets.emplace_back(I::NPC_DRILL_CASTER, 1, Who::Dps, Condition::Phase(2));
            d.phase = 1;
            return d;
        }

        // GUID follow: `describe targets-follow follow=<entry>` turns the first entry of that creature into its
        // first live add's guid (priority 1); the other adds of the entry rank below the dummy.
        inline Descriptor TargetsFollow()
        {
            Descriptor d;
            d.targets.emplace_back(I::NPC_DRILL_ADD, 1);
            d.targets.emplace_back(I::NPC_DRILL_ADD, -1);
            return d;
        }

        // Kill together within 10%: the caster and the add, each the other's partner.
        inline Descriptor TargetsTogether()
        {
            Descriptor d;
            d.targets.push_back(TargetEntry(I::NPC_DRILL_CASTER, 1).KillTogether(I::NPC_DRILL_ADD, 10.f));
            d.targets.push_back(TargetEntry(I::NPC_DRILL_ADD, 1).KillTogether(I::NPC_DRILL_CASTER, 10.f));
            return d;
        }

        // Role split on a shared pool: ranged on the caster, melee on the add.
        inline Descriptor TargetsSplit()
        {
            Descriptor d;
            d.targets.emplace_back(I::NPC_DRILL_CASTER, 1, Who::Ranged);
            d.targets.emplace_back(I::NPC_DRILL_ADD, 1, Who::Melee);
            return d;
        }

        // Break-out: prisons first, ordered by the held player's role (tank, healer, others).
        inline Descriptor TargetsBreakOut()
        {
            Descriptor d;
            d.targets.push_back(TargetEntry(I::NPC_DRILL_PRISON, 2).BreakOut());
            return d;
        }

        inline Descriptor Ignore()
        {
            Descriptor d;
            d.ignores.emplace_back(I::NPC_DRILL_COPY);
            return d;
        }

        // Blackrock Spire WP-TS, the two-tank drill: the dummy (the owner) stacks Taunt Swap (drill) 205024 on its
        // victim with each `cast`; at 3 the other tank taunts it. Stacks count from the dummy (the cast's caster). The
        // assist tank picks up the Drill Add (Who::AssistTank), as the second tank picks up Gizrul.
        inline Descriptor TauntSwapDrill()
        {
            Descriptor d;
            d.tauntSwaps.emplace_back(0, I::SPELL_DRILL_TAUNT_SWAP, 3);
            d.targets.emplace_back(I::NPC_DRILL_ADD, 1, Who::AssistTank);
            return d;
        }

        // Blackrock Spire WP-PS, the `pass` drill: the hot potato on the drill auras. The holder is whoever carries
        // Marked (drill) 205009 (the drill's `debuff 205009` hands it to one member); the module moves it to the
        // catcher itself (no item in the drill) and puts Baited (drill) 205008 on the thrower for 8 s, the drill's
        // Scorched Hands: a Baited player does not catch. Catchers far from the dummy (at least 10 yd for a ready
        // spot) and off the drill's egg circles (`pass-eggs`, tag kPassEggTag) within 4 yd; the holder throws once
        // the dummy is within 8 yd of it, or after 5 s, never before 1.5 s. Two ready catchers.
        constexpr std::uint32_t kPassEggTag = 60;

        inline Descriptor PassDrill()
        {
            Descriptor d;
            d.passes.push_back(PassEntry::MovingAura(I::SPELL_MARKED_DRILL, 30.f, I::SPELL_BAITED_DRILL,
                milliseconds(8000))
                .CatchWhen(Condition::LacksAura(I::SPELL_BAITED_DRILL))
                .FarFrom(0).KeepFrom(10.f).FewestZones(kPassEggTag, 4.f)
                .When(PassTrigger::UnitNear(0, 8.f)).When(PassTrigger::HeldFor(milliseconds(5000)))
                .MinHold(milliseconds(1500)).Flight(milliseconds(500)).ReadyCatchers(2));
            return d;
        }

        // Blackrock Depths WP-DS (BD3), Spell Eater's shape: the Drill Adds rank above the dummy; while one carries the
        // purgeable Magic buff (the shell) no bot casts magic damage on it (physical spells and white swings go on),
        // and the buff is on the purge list (the shell's reward).
        inline Descriptor HoldMagic()
        {
            Descriptor d;
            d.targets.emplace_back(I::NPC_DRILL_ADD, 1);
            d.holds.push_back(HoldEntry::Damage(I::NPC_DRILL_ADD, I::SPELL_DRILL_PURGEABLE, kSchoolMaskMagic));
            d.purges.emplace_back(I::NPC_DRILL_ADD, I::SPELL_DRILL_PURGEABLE, 2);
            return d;
        }

        // WP-DS, Commanding Aura's shape: the casters' high cast is on the kick list, but no kick lands on a caster
        // with Aiming (the aura that holds interrupts); the other caster is kicked as usual.
        inline Descriptor HoldKicks()
        {
            Descriptor d;
            d.interrupts.emplace_back(I::NPC_DRILL_CASTER, I::SPELL_DRILL_CAST_HIGH, 3);
            d.holds.push_back(HoldEntry::Interrupts(I::NPC_DRILL_CASTER, I::SPELL_DRILL_AIMING));
            return d;
        }

        // WP-DS, Gerstahn's Doubt: Magic Affliction is cured on the member with the most Taunt Swap (drill) stacks
        // first (the stacking stand-in for Doubt).
        inline Descriptor DispelStacks()
        {
            Descriptor d;
            d.dispels.push_back(DispelEntry(I::SPELL_DRILL_DEBUFF_MAGIC, 1).MostStacksFirst(I::SPELL_DRILL_TAUNT_SWAP));
            return d;
        }

        // Gnomeregan BM-G, Static's shape (§7 #12 A): a bot carrying more than kMoveCostDrillAbove Marked (drill)
        // stacks (the stand-in for Static, 10 at most) makes no optional move and dodges by the shortest way out;
        // ranged bots and healers prefer 11 to 12 yd from the dummy (the Electrocutioner's band).
        inline constexpr std::uint32_t kMoveCostDrillAbove = 4;
        inline constexpr float kMoveCostDrillBandMinYd = 11.f;
        inline constexpr float kMoveCostDrillBandMaxYd = 12.f;

        inline Descriptor MoveCostDrill()
        {
            Descriptor d;
            d.moveCosts.emplace_back(I::SPELL_MARKED_DRILL, kMoveCostDrillAbove);
            d.rangeBands.emplace_back(kMoveCostDrillBandMinYd, kMoveCostDrillBandMaxYd);
            return d;
        }

        // Every list at once (the `descriptor` drill): kicks, dispels, purges; prisons (3) before the aiming add
        // (2) before the casters (1) before the dummy; the purge adds below the dummy; the copies ignored.
        inline Descriptor All()
        {
            Descriptor d;
            AddInterrupts(d);
            AddDispels(d);
            AddPurges(d);
            d.targets.push_back(TargetEntry(I::NPC_DRILL_PRISON, 3).BreakOut());
            d.targets.emplace_back(I::NPC_DRILL_ADD, 2, Who::Dps, Condition::HasAura(I::SPELL_DRILL_AIMING));
            d.targets.emplace_back(I::NPC_DRILL_CASTER, 1);
            d.targets.emplace_back(I::NPC_DRILL_ADD, -1);
            d.ignores.emplace_back(I::NPC_DRILL_COPY);
            return d;
        }
    }

    inline std::vector<Preset> const& Presets()
    {
        static std::vector<Preset> const presets = {
            { "none", "an empty descriptor (clears the lists; phase 0)", [] { return Descriptor{}; } },
            { "interrupts", "kick high > mid > low (casters), never the do-not-kick cast", &DrillPresets::Interrupts },
            { "dispels", "urgent stun, deadline debuff (4 s left), then Magic, Curse, Poison, Disease",
                &DrillPresets::Dispels },
            { "purges", "purge the adds' Magic buff, then their Enrage; the adds rank below the dummy",
                &DrillPresets::Purges },
            { "targets", "caster, then adds, then the dummy", &DrillPresets::Targets },
            { "targets-aim", "the aiming add, then the other adds", &DrillPresets::TargetsAim },
            { "targets-zone", "adds with Marked (the zone stand-in) first, adds without it ignored",
                &DrillPresets::TargetsZone },
            { "targets-phase", "the caster below the dummy, first in phase 2 (phase 1 as built)",
                &DrillPresets::TargetsPhase },
            { "targets-follow", "one add followed by guid (follow=), the other adds below the dummy",
                &DrillPresets::TargetsFollow },
            { "targets-together", "the caster and the add die together (within 10%)", &DrillPresets::TargetsTogether },
            { "targets-split", "ranged on the caster, melee on the add", &DrillPresets::TargetsSplit },
            { "targets-breakout", "prisons first, the tank's, then the healer's", &DrillPresets::TargetsBreakOut },
            { "ignore", "the copies are ignored", &DrillPresets::Ignore },
            { "taunt-swap", "the tanks trade the dummy at 3 Taunt Swap stacks; the assist tank picks up the add",
                &DrillPresets::TauntSwapDrill },
            { "pass", "Marked is the hot potato: thrown on near the dummy or after 5 s, away from the dummy and the "
                "egg circles; the thrower is Baited for 8 s", &DrillPresets::PassDrill },
            { "hold-magic", "no magic damage on an add with the purgeable buff (the shell); purge it",
                &DrillPresets::HoldMagic },
            { "hold-kicks", "kick the casters' high cast, never on a caster with Aiming", &DrillPresets::HoldKicks },
            { "dispel-stacks", "Magic Affliction cured on the member with the most Taunt Swap stacks first",
                &DrillPresets::DispelStacks },
            { "move-cost", "Static's shape: above 4 Marked (drill) stacks no optional move, dodges by the shortest "
                "way out; ranged bots and healers prefer 11-12 yd from the owner", &DrillPresets::MoveCostDrill },
            { "descriptor", "every list at once: kicks, dispels, purges, prisons > aiming add > casters, copies "
                "ignored", &DrillPresets::All },
        };
        return presets;
    }

    inline Preset const* FindPreset(std::string_view name)
    {
        for (Preset const& p : Presets())
            if (name == p.name)
                return &p;
        return nullptr;
    }
}

#endif
