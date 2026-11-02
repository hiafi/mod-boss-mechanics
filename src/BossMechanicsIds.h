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
 * Display ids (CreatureDisplayInfo id = CreatureModelData id): one block of 100 ids per color,
 *
 *     id = 91000 + 100 * c + offset(shape) + step                       (MarkerDisplay below)
 *
 * c = color index (Color in MechanicRegistry.h: red 0, yellow 1, blue 2, purple 3, orange 4, green 5, teal 6,
 * white 7, black 8). Every shape exists in every color. Every telegraph draws two layers (MechanicTelegraph.h):
 * O = outline (rim + faint interior, static, the full hitbox), F = fill (rim + solid interior, the timer that grows
 * into the outline). Inside one color's block:
 *
 * | +0       | disc (F)                                                   |
 * | +1       | ring (O; also player get-out radii)                        |
 * | +2       | timer ring (rim only): a donut's timer                     |
 * | +3..+9   | donut (F), ratio rho_j in kDonutRatios                     |
 * | +10..+16 | donut (O)                                                  |
 * | +17..+24 | cone (F), angle_j in kConeAngles                           |
 * | +25..+32 | cone (O)                                                   |
 * | +33..+65 | line (F), aspect k_i (kLineSteps, LineAspect in MechanicMath.h)  |
 * | +66..+98 | line (O)                                                   |
 * | +99      | spare                                                      |
 *
 * Line ladder (M1-FIX-LINES): k_i = 2^((i - 8) / 4) for i 0..7 (1/4 up to 0.84, so a fill or a short line keeps its
 * full width while it is shorter than wide) and 2^((i - 8) / 8) for i 8..32 (1 to 8). A line model draws x in [0, k]
 * (model +X = the marker's facing) and is symmetric about its origin, so its direction comes from the texture, the
 * same way a cone's does.
 *
 * 91000-91898 used, 91900-91999 spare (91990-91995: the asset script's opt-in diagnostic displays; 91996: the
 * line-direction probe, an old-style line model for `.morph`). The markers used 90500-90699 before the colors fix
 * (M1-FIX-COLORS-REPORT.md); those ids are free again.
 */

#ifndef MOD_BOSS_MECHANICS_IDS_H
#define MOD_BOSS_MECHANICS_IDS_H

#include <array>
#include <cstddef>
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

    // Displays: a table of shape ladders inside one 100-id block per color (see the table above).
    enum class MarkerShape : std::uint8_t
    {
        Disc,           // F
        Ring,           // O
        TimerRing,      // a donut's timer, rim only
        Donut,          // F, step = kDonutRatios index
        DonutOutline,   // O
        Cone,           // F, step = kConeAngles index
        ConeOutline,    // O
        Line,           // F, step = line ladder index
        LineOutline     // O
    };

    constexpr std::uint8_t COLOR_COUNT = 9;
    constexpr std::uint32_t DISPLAY_FIRST = 91000;
    constexpr std::uint32_t DISPLAY_COLOR_STRIDE = 100;
    constexpr std::uint32_t DISPLAY_SLICE_LAST = 91999;

    // The timer ring's band, as a fraction of its radius (patch_bossmechanics_markers.py TIMER_RIM_FRAC).
    constexpr float kTimerRingBandFrac = 0.04f;

    // Donut inner/outer ratio ladder (j = index).
    constexpr std::array<float, 7> kDonutRatios = { 0.25f, 0.33f, 0.40f, 0.50f, 0.60f, 0.67f, 0.75f };
    // Cone angle ladder in degrees (j = index).
    constexpr std::array<float, 8> kConeAngles = { 30.f, 45.f, 60.f, 90.f, 120.f, 150.f, 180.f, 270.f };
    // Line aspect ladder (LineAspect in MechanicMath.h): quarter-octave steps from 1/4 below aspect 1, eighth-octave
    // steps from 1 to 8 (step kLineUnitStep is aspect 1). Longer lines draw abutting pieces of aspect 8.
    constexpr std::uint32_t kLineSteps = 33;
    constexpr std::uint32_t kLineUnitStep = 8;
    constexpr float kLineMinAspect = 0.25f;
    constexpr float kLineMaxAspect = 8.f;
    // The line-direction probe (patch_bossmechanics_markers.py PROBE_ID): not used by the server.
    constexpr std::uint32_t DISPLAY_PROBE_LINE = 91996;

    // Steps per shape, in id order inside a color block (MarkerShape order).
    constexpr std::array<std::uint32_t, 9> kMarkerShapeSteps = { 1, 1, 1,
        static_cast<std::uint32_t>(kDonutRatios.size()), static_cast<std::uint32_t>(kDonutRatios.size()),
        static_cast<std::uint32_t>(kConeAngles.size()), static_cast<std::uint32_t>(kConeAngles.size()),
        kLineSteps, kLineSteps };

    // First offset of each shape inside a color block: the running sum of the steps before it.
    constexpr std::array<std::uint32_t, 10> kMarkerShapeOffsets = []
    {
        std::array<std::uint32_t, 10> out{};
        for (std::size_t i = 0; i < kMarkerShapeSteps.size(); ++i)
            out[i + 1] = out[i] + kMarkerShapeSteps[i];
        return out;
    }();

    constexpr std::uint32_t DISPLAY_PER_COLOR = kMarkerShapeOffsets.back();    // 99
    constexpr std::uint32_t DISPLAY_LAST =
        DISPLAY_FIRST + DISPLAY_COLOR_STRIDE * (COLOR_COUNT - 1) + DISPLAY_PER_COLOR - 1;
    static_assert(DISPLAY_PER_COLOR <= DISPLAY_COLOR_STRIDE, "a color block overflows its 100 ids");
    static_assert(DISPLAY_LAST == 91898 && DISPLAY_LAST <= DISPLAY_SLICE_LAST);

    // The display of one shape step in one color (patch_bossmechanics_markers.py display_id). color and step are
    // clamped into range, so every call names a shipped display.
    constexpr std::uint32_t MarkerDisplay(std::uint8_t color, MarkerShape shape, std::uint32_t step = 0)
    {
        std::size_t const s = static_cast<std::size_t>(shape);
        std::uint32_t const c = color < COLOR_COUNT ? color : COLOR_COUNT - 1;
        std::uint32_t const k = step < kMarkerShapeSteps[s] ? step : kMarkerShapeSteps[s] - 1;
        return DISPLAY_FIRST + DISPLAY_COLOR_STRIDE * c + kMarkerShapeOffsets[s] + k;
    }

    struct MarkerDisplayParts
    {
        bool valid = false;
        std::uint8_t color = 0;
        MarkerShape shape = MarkerShape::Disc;
        std::uint32_t step = 0;
    };

    // The inverse of MarkerDisplay; valid = false for any id that is not a shipped marker display.
    constexpr MarkerDisplayParts DecodeMarkerDisplay(std::uint32_t display)
    {
        MarkerDisplayParts out;
        if (display < DISPLAY_FIRST || display > DISPLAY_LAST)
            return out;
        std::uint32_t const rel = display - DISPLAY_FIRST;
        std::uint32_t const offset = rel % DISPLAY_COLOR_STRIDE;
        if (offset >= DISPLAY_PER_COLOR)
            return out;
        std::size_t s = 0;
        while (offset >= kMarkerShapeOffsets[s + 1])
            ++s;
        out.valid = true;
        out.color = static_cast<std::uint8_t>(rel / DISPLAY_COLOR_STRIDE);
        out.shape = static_cast<MarkerShape>(s);
        out.step = offset - kMarkerShapeOffsets[s];
        return out;
    }
}

#endif
