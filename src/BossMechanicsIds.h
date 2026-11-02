/*
 * mod-boss-mechanics: pinned ids (dungeon-mechanics STAGE-M1-HANDOFF §3.1).
 *
 * Pure header: no AzerothCore includes, so the standalone tests can use it. The asset script
 * (apps/dbc-tools/patch_bossmechanics_markers.py) quotes the same table; keep both in sync.
 *
 * | Id            | What                                                              |
 * | 205000        | Weakened (debuff, 7 stacks, 30 s)                                 |
 * | 205001-205007 | Resolve damage, one per school: 205001 + SpellSchools value       |
 * | 300400        | Mechanic Marker: telegraph stalker and resolve caster             |
 * | 300401        | Mechanic Test Owner: hostile owner for `.mech test`               |
 *
 * Display ids (CreatureDisplayInfo id = CreatureModelData id; colour index c: red 0, yellow 1,
 * blue 2, purple 3; get-out shapes ship red and yellow only):
 *
 * | 90500-90503 | disc (filled, rim)                       | 90500 + c          |
 * | 90504-90507 | ring (rim, faint fill)                   | 90504 + c          |
 * | 90508-90521 | donut, ratio rho in kDonutRatios         | 90508 + 2j + c     |
 * | 90522-90537 | cone, angle in kConeAngles               | 90522 + 2j + c     |
 * | 90538-90619 | line, aspect k_i = 2^(i/8), i = 0..40    | 90538 + 2i + c     |
 * | 90620-90649 | spare                                    |                    |
 */

#ifndef MOD_BOSS_MECHANICS_IDS_H
#define MOD_BOSS_MECHANICS_IDS_H

#include <array>
#include <cstdint>

namespace BossMechanics::Ids
{
    // Spells
    constexpr std::uint32_t SPELL_WEAKENED = 205000;
    constexpr std::uint32_t SPELL_RESOLVE_BASE = 205001;        // + school (0 physical .. 6 arcane)
    constexpr std::uint32_t SPELL_RESOLVE_LAST = 205007;
    constexpr std::uint8_t WEAKENED_MAX_STACKS = 7;

    constexpr std::uint32_t ResolveSpellForSchool(std::uint8_t school)
    {
        return school <= 6 ? SPELL_RESOLVE_BASE + school : SPELL_RESOLVE_BASE;
    }

    // Creatures
    constexpr std::uint32_t NPC_MECHANIC_MARKER = 300400;
    constexpr std::uint32_t NPC_MECHANIC_TEST_OWNER = 300401;

    // Displays
    constexpr std::uint32_t DISPLAY_DISC_BASE = 90500;          // + c (4 colours)
    constexpr std::uint32_t DISPLAY_RING_BASE = 90504;          // + c (4 colours)
    constexpr std::uint32_t DISPLAY_DONUT_BASE = 90508;         // + 2j + c (red, yellow)
    constexpr std::uint32_t DISPLAY_CONE_BASE = 90522;          // + 2j + c (red, yellow)
    constexpr std::uint32_t DISPLAY_LINE_BASE = 90538;          // + 2i + c (red, yellow)
    constexpr std::uint32_t DISPLAY_LAST = 90619;
    constexpr std::uint32_t DISPLAY_SPARE_FIRST = 90620;
    constexpr std::uint32_t DISPLAY_SPARE_LAST = 90649;

    constexpr std::uint8_t COLOUR_COUNT_ALL = 4;                // disc, ring
    constexpr std::uint8_t COLOUR_COUNT_GET_OUT = 2;            // donut, cone, line

    // Donut inner/outer ratio ladder (j = index).
    constexpr std::array<float, 7> kDonutRatios = { 0.25f, 0.33f, 0.40f, 0.50f, 0.60f, 0.67f, 0.75f };
    // Cone angle ladder in degrees (j = index).
    constexpr std::array<float, 8> kConeAngles = { 30.f, 45.f, 60.f, 90.f, 120.f, 150.f, 180.f, 270.f };
    // Line aspect ladder: k_i = 2^(i/8), i = 0..kLineSteps-1 (1 to 32).
    constexpr std::uint32_t kLineSteps = 41;
    constexpr float kLineMaxAspect = 32.f;
}

#endif
