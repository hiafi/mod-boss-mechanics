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
 * blue 2, purple 3; get-out shapes ship red and yellow only). Every telegraph draws two layers
 * (MechanicTelegraph.h): O = outline (rim + faint interior, static, the full hitbox), F = fill (rim +
 * solid interior, the timer that grows into the outline):
 *
 * | 90500-90503 | disc (F)                                 | 90500 + c          |
 * | 90504-90507 | ring (O; also player get-out radii)      | 90504 + c          |
 * | 90508-90521 | donut (F), ratio rho in kDonutRatios     | 90508 + 2j + c     |
 * | 90522-90537 | cone (F), angle in kConeAngles           | 90522 + 2j + c     |
 * | 90538-90587 | line (F), aspect k_i = 2^(i/8), i 0..24  | 90538 + 2i + c     |
 * | 90588-90637 | line (O)                                 | 90588 + 2i + c     |
 * | 90638-90653 | cone (O)                                 | 90638 + 2j + c     |
 * | 90654-90667 | donut (O)                                | 90654 + 2j + c     |
 * | 90668-90669 | timer ring (rim only): a donut's timer   | 90668 + c          |
 * | 90670-90699 | spare (90690-90695: the asset script's opt-in diagnostic displays) |
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

    // Displays (F = fill layer, O = outline layer)
    constexpr std::uint32_t DISPLAY_DISC_BASE = 90500;          // F, + c (4 colours)
    constexpr std::uint32_t DISPLAY_RING_BASE = 90504;          // O, + c (4 colours)
    constexpr std::uint32_t DISPLAY_DONUT_BASE = 90508;         // F, + 2j + c (red, yellow)
    constexpr std::uint32_t DISPLAY_CONE_BASE = 90522;          // F, + 2j + c (red, yellow)
    constexpr std::uint32_t DISPLAY_LINE_BASE = 90538;          // F, + 2i + c (red, yellow)
    constexpr std::uint32_t DISPLAY_LINE_OUTLINE_BASE = 90588;  // O, + 2i + c (red, yellow)
    constexpr std::uint32_t DISPLAY_CONE_OUTLINE_BASE = 90638;  // O, + 2j + c (red, yellow)
    constexpr std::uint32_t DISPLAY_DONUT_OUTLINE_BASE = 90654; // O, + 2j + c (red, yellow)
    constexpr std::uint32_t DISPLAY_TIMER_RING_BASE = 90668;    // + c (red, yellow): rim only
    constexpr std::uint32_t DISPLAY_LAST = 90669;
    constexpr std::uint32_t DISPLAY_SPARE_FIRST = 90670;
    constexpr std::uint32_t DISPLAY_SPARE_LAST = 90699;

    constexpr std::uint8_t COLOUR_COUNT_ALL = 4;                // disc, ring
    constexpr std::uint8_t COLOUR_COUNT_GET_OUT = 2;            // donut, cone, line, timer ring

    // The timer ring's band, as a fraction of its radius (patch_bossmechanics_markers.py TIMER_RIM_FRAC).
    constexpr float kTimerRingBandFrac = 0.04f;

    // Donut inner/outer ratio ladder (j = index).
    constexpr std::array<float, 7> kDonutRatios = { 0.25f, 0.33f, 0.40f, 0.50f, 0.60f, 0.67f, 0.75f };
    // Cone angle ladder in degrees (j = index).
    constexpr std::array<float, 8> kConeAngles = { 30.f, 45.f, 60.f, 90.f, 120.f, 150.f, 180.f, 270.f };
    // Line aspect ladder: k_i = 2^(i/8), i = 0..kLineSteps-1 (1 to 8; longer lines draw abutting pieces).
    constexpr std::uint32_t kLineSteps = 25;
    constexpr float kLineMaxAspect = 8.f;
}

#endif
