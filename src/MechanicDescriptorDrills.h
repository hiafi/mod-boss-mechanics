/*
 * mod-boss-mechanics: the M5 descriptor drills (STAGE-M5-HANDOFF §3.6, §1 acceptance item 2, WP-K).
 *
 * No AzerothCore includes: tests/drill_plan_test.cpp and tests/drill_test.cpp read it with the host compiler.
 *
 * An M4 drill (Drill::Def, MechanicDrill.h) is a list of `.mech test` lines. A descriptor drill is a program text
 * instead (the descriptor entries of MechanicDrillPlan.h: describe, add, cast, debuff, buff, swap, hold, wait), run
 * on the owner, so the same drill works on the Descriptor Training Dummy (300403) and on the dpssim SimTarget: nothing
 * in it depends on the dummy's AI. Drill::ParseProgram expands a drill's name in place:
 * - its entries start where the name stands, and it takes the time of its waits, then kProgramGapMs;
 * - everything it leaves running (a `cast every=` chain, an add or prison without ttl) ends with the drill, not with
 *   the loop (ProgramEntry::endMs), so drills in a row never overlap;
 * - `<name>:<n>` runs it n times in a row;
 * - a drill may name other drills (its sub-drills), at most kMaxProgramDrillDepth deep.
 *
 * The stand-in ids are BossMechanicsIds.h's M5 block (205010-205023, 300404-300407); the presets they describe are in
 * MechanicDescriptorPresets.h. The training dummy's gossip lists the drills marked `listed`.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DESCRIPTOR_DRILLS_H
#define MOD_BOSS_MECHANICS_MECHANIC_DESCRIPTOR_DRILLS_H

#include <cstdint>
#include <string_view>
#include <vector>

namespace BossMechanics::Drill
{
    struct ProgramDef
    {
        char const* name = "";
        char const* summary = "";       // one line for the gossip and the command's reply
        char const* program = "";       // ParseProgram text; ends with a wait or a drill (nothing at its very end)
        bool listed = true;             // offered by the Descriptor Training Dummy's gossip (sub-drills are not)
    };

    constexpr std::uint32_t kMaxProgramDrillDepth = 3;    // descriptor-drills > targets > targets-order

    // The Descriptor Training Dummy's default program and the dpssim `descriptor` slice: every feature at once, 60 s.
    constexpr char const* kDescriptorProgram = "descriptor";
    // Every descriptor drill in turn (about 7 minutes a loop).
    constexpr char const* kDescriptorDrillsProgram = "descriptor-drills";

    inline std::vector<ProgramDef> const& ProgramDrills()
    {
        static std::vector<ProgramDef> const drills = {
            // ---- interrupts ----
            // Ordered across two casters: the owner casts mid (0 s), do-not-kick (3 s) and high (6 s) every 12 s; two
            // Drill Casters cast low every 3 s. Under that load a kicker holds its kick off the low casts for the
            // high one (the overload hold), so some low casts go through; the do-not-kick cast is never kicked.
            { "interrupts-order", "owner mid/no-kick/high every 12 s, two casters low every 3 s",
                "describe interrupts,add 300404 n=2,cast 205010 unit=300404 every=3000,cast 205011 every=12000,"
                "wait:3,cast 205013 every=12000,wait:3,cast 205012 every=12000,wait:30", false },
            // A fast caster: one Drill Caster casts the high cast (3 s) every 4 s, faster than any one kick comes
            // back, so two kickers alternate.
            { "interrupts-fast", "one caster casts high every 4 s: two kickers alternate",
                "describe interrupts,add 300404 n=1,cast 205012 unit=300404 every=4000,wait:30", false },
            { "interrupts", "kick order across the dummy and two casters, overload, do-not-kick, a fast caster",
                "interrupts-order,interrupts-fast" },

            // ---- dispels ----
            // Normal debuffs of every type, the urgent stun among them (first, past the gate), the deadline debuff
            // (dispelled before it expires), then normal Magic on two members wounded to 40% (the healers' 50% gate
            // holds it until they are healed) with an urgent stun and a deadline debuff that still go at once.
            { "dispels",
                "urgent stun first, deadline debuff before expiry, normal ones held while someone is under 50%",
                "describe dispels,debuff 205014,debuff 205015,debuff 205016,debuff 205017,wait:2,debuff 205018,"
                "wait:8,debuff 205019,wait:10,debuff 205014 n=2 hp=40,debuff 205018,wait:10,debuff 205019 hp=40,"
                "wait:15" },

            // ---- purges ----
            // Two Drill Adds with the purgeable Magic buff and the Enrage buff, both put back every 10 s; the adds
            // rank below the dummy, so they live through the drill.
            { "purges", "two adds with a purgeable Magic buff and an Enrage, rebuffed every 10 s",
                "describe purges,add 300405 n=2 aura=205021,buff 300405 205022,wait:10,buff 300405 205021,"
                "buff 300405 205022,wait:10,buff 300405 205021,buff 300405 205022,wait:10" },

            // ---- targets: one sub-drill per §1 target row ----
            // Fixed order, adds over the boss: the caster (2), then the adds (1), then the dummy (0).
            { "targets-order", "fixed order: caster, then adds, then the dummy",
                "describe targets,add 300404 n=1,add 300405 n=2,wait:20", false },
            // Come and go: an add that leaves after 8 s (if still alive) and a new one at 12 s.
            { "targets-comeback", "an add leaves after 8 s and another comes at 12 s: retarget each time",
                "describe targets,add 300405 n=1 ttl=8000,wait:12,add 300405 n=1 ttl=8000,wait:12", false },
            // Aura-keyed (Willey's aiming riflemen): the add with Aiming (205023) before the two without.
            { "targets-aim", "the aiming add before the two others",
                "describe targets-aim,add 300405 n=2,add 300405 n=1 aura=205023,wait:20", false },
            // Zone-conditional (the Unforgiven's firelight): the adds are ignored until they are "in the zone", which
            // a HasAura stand-in on Marked (drill) 205009 plays (InZone needs a runtime mechanic id).
            { "targets-zone", "adds ignored for 8 s, then in the zone (Marked) and first",
                "describe targets-zone,add 300405 n=2,wait:8,buff 300405 205009,wait:16", false },
            // Phase: the caster ranks below the dummy in phase 1 and above it in phase 2.
            { "targets-phase", "the caster below the dummy in phase 1, first in phase 2 (at 8 s)",
                "describe targets-phase phase=1,add 300404 n=1,wait:8,describe targets-phase phase=2,wait:12", false },
            // GUID follow through a swap (Jandice): the first Drill Add is followed by guid, a second one of the same
            // entry ranks below the dummy; the dummy trades places with an add twice.
            { "targets-follow", "follow one add by guid while the dummy swaps places with the adds",
                "add 300405 n=1,describe targets-follow follow=300405,add 300405 n=1,wait:6,swap 300405,wait:6,"
                "swap 300405,wait:12", false },
            // Kill together within 10% (Ravenian and Corvane): a caster and an add.
            { "targets-together", "a caster and an add die together (within 10%)",
                "describe targets-together,add 300404 n=1,add 300405 n=1,wait:25", false },
            // Role split on a shared pool (Marduk and Vectus): ranged on the caster, melee on the add.
            { "targets-split", "ranged on the caster, melee on the add",
                "describe targets-split,add 300404 n=1,add 300405 n=1,wait:20", false },
            // Break-out: two prisons, the tank's first (Marked stands in for the prison aura).
            { "targets-breakout", "two prisons hold members: the tank's broken first",
                "describe targets-breakout,hold 300407 n=2 pick=tank aura=205009,wait:20", false },
            { "targets", "fixed order, come and go, aiming, zone, phase, guid follow, kill together, role split, "
                "break-out", "targets-order,targets-comeback,targets-aim,targets-zone,targets-phase,targets-follow,"
                "targets-together,targets-split,targets-breakout" },

            // ---- ignore ----
            // Three Drill Copies beside the dummy: no bot or pet damage on them, AoE counts as if they were not there.
            { "ignore", "three copies beside the dummy: never hit, not counted for AoE",
                "describe ignore,add 300406 n=3,wait:30" },

            // ---- everything ----
            // The dummy's default loop and the sim slice: every list at once in 60 s, on the combined preset.
            { "descriptor", "every list at once (60 s): kicks, dispels, purges, targets, prisons, copies",
                "describe descriptor,add 300404 n=2,cast 205010 unit=300404 every=3000,add 300406 n=2,"
                "cast 205011 every=12000,debuff 205014,debuff 205018,wait:3,cast 205013 every=12000,wait:3,"
                "cast 205012 every=12000,debuff 205019,wait:9,add 300405 n=1 aura=205021,buff 300405 205022,"
                "debuff 205015 hp=40,debuff 205016,wait:10,add 300405 n=1 aura=205023,debuff 205017,debuff 205018,"
                "wait:10,hold 300407 n=2 ttl=20000 pick=tank aura=205009,debuff 205019,wait:25" },
            { "descriptor-drills", "every descriptor drill in turn (about 7 minutes)",
                "interrupts,dispels,purges,targets,ignore" },
        };
        return drills;
    }

    inline ProgramDef const* FindProgramDrill(std::string_view name)
    {
        for (ProgramDef const& d : ProgramDrills())
            if (name == d.name)
                return &d;
        return nullptr;
    }
}

#endif
