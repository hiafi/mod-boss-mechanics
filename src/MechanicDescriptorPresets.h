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
