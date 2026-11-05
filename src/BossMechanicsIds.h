/*
 * mod-boss-mechanics: pinned ids (dungeon-mechanics STAGE-M1-HANDOFF §3.1).
 *
 * Pure header: no AzerothCore includes, so the standalone tests can use it. The asset script
 * (apps/dbc-tools/patch_bossmechanics_markers.py) quotes the same table; keep both in sync.
 *
 * | Id            | What                                                              |
 * | 205000        | Weakened (debuff, 7 stacks, 30 s)                                 |
 * | 205001-205007 | Resolve damage, one per school: 205001 + SpellSchools value       |
 * | 205008        | Baited (drill): dummy debuff, 10 stacks, 2 min (bait rotation)    |
 * | 205009        | Marked (drill): dummy debuff, 10 stacks, 2 min (drill selector)   |
 * | 205010-205012 | M5 interruptible casts: low / mid / high (2 / 2.5 / 3 s)          |
 * | 205013        | M5 do-not-kick cast                                               |
 * | 205014-205017 | M5 dispellable debuffs: Magic, Curse, Poison, Disease             |
 * | 205018        | M5 urgent loss of control (Magic stun)                            |
 * | 205019-205020 | M5 deadline debuff (Magic, 8 s) and its expiry burst              |
 * | 205021-205022 | M5 purgeable Magic buff, Enrage buff (soothe)                     |
 * | 205023        | M5 "Aiming" aura (raises an add's priority)                       |
 * | 205000-205099 | the reserved spell block (`rider=` takes 205008 and 205009 only)  |
 * | 300400        | Mechanic Marker: telegraph stalker and resolve caster             |
 * | 300401        | Mechanic Test Owner: hostile owner for `.mech test`               |
 * | 300402        | Mechanics Training Dummy: runs the drill program on itself (M4)   |
 * | 300403        | Descriptor Training Dummy: 300402's AI, kickable (M5)             |
 * | 300404-300407 | M5 Drill Caster, Drill Add, Drill Copy, Drill Prison              |
 *
 * Display ids (CreatureDisplayInfo id = CreatureModelData id), one block of 100 ids per color in each range:
 *
 *     id = 91000 + 100 * c + offset(shape) + step           discs, rings, the timer ring, donuts, cones
 *     id = 92000 + 100 * c + i                               line (F), i = 0..56
 *     id = 93000 + 100 * c + i                               line (O)                (MarkerDisplay below)
 *
 * c = color index (Color in MechanicRegistry.h: red 0, yellow 1, blue 2, purple 3, orange 4, green 5, teal 6,
 * white 7, black 8). Every shape exists in every color. Every telegraph draws two layers (MechanicTelegraph.h):
 * O = outline (rim + faint interior, static, the full hitbox), F = fill (rim + solid interior, the timer that grows
 * into the outline). Inside one color's 91000 block:
 *
 * | +0       | disc (F)                                                   |
 * | +1       | ring (O; also player get-out radii)                        |
 * | +2       | timer ring (rim only): a donut's timer                     |
 * | +3..+9   | donut (F), ratio rho_j in kDonutRatios                     |
 * | +10..+16 | donut (O)                                                  |
 * | +17..+24 | cone (F), angle_j in kConeAngles                           |
 * | +25..+32 | cone (O)                                                   |
 * | +33      | dashed ring (rim dashes only): an armed mine               |
 * | +34..+99 | spare (+33..+98 were the lines before M1-FIX-LINE-FILL)    |
 *
 * Line ladder (M1-FIX-LINE-FILL): k_i = 2^((i - 8) / 4) for i 0..7 (1/4 up to 0.84) and 2^((i - 8) / 8) for i 8..56
 * (1 to 64), the same for both layers (LineAspect in MechanicMath.h); +57..+99 of each line block are spare. A line
 * model is one quad, symmetric about its origin (x in [-k, k]; the client draws a model centred on its bounds), with
 * the source quad's UVs: v grows with +X and the line texture is transparent on rows v < 1/2, so the line runs
 * x in [0, k] from the marker forward (model +X = the marker's facing). Its far end is the quad's edge; its start is
 * the texture's edge at v = 1/2, soft by half a texel of the active mip. Every aspect shares one texture. (The
 * two-quad model of M1-FIX-LINE-FILL drew only part of the line in game and was reverted in round 4b.)
 *
 * 91000-91833 used, 91834-91999 spare (91990-91995: the asset script's opt-in diagnostic displays), 92000-92856 and
 * 93000-93856 used, the rest of 92000-93999 spare. The markers used 90500-90699 before the colors fix
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
    // Baited (drill): a harmless stacking dummy debuff, the drills' stand-in for a boss's bait rotation debuff
    // (`.mech drill fenrus`: the Bait rider and the fewest-stacks rule's aura). Not Weakened: the baiter doing the job
    // gets no Weakened (user ruling 2026-11-04).
    constexpr std::uint32_t SPELL_BAITED_DRILL = 205008;
    // Marked (drill): a second harmless stacking dummy debuff, the drills' stand-in for an eligibility or order aura
    // (Ras's Chill, the ordered soak's stacks). The drill step `marker <stacks>` sets it; drill setup never hands out
    // Weakened (orchestrator ruling on STAGE-M4-R-REPORT M-2). Separate from 205008, so the fenrus Baited stacks never
    // leak into ras or soaks eligibility within one program slice.
    constexpr std::uint32_t SPELL_MARKED_DRILL = 205009;
    constexpr std::uint8_t MARKED_DRILL_MAX_STACKS = 10;
    // The module's reserved spell block (apps/dbc-tools source/ids.yaml `bossmechanics`).
    constexpr std::uint32_t SPELL_BLOCK_FIRST = 205000;
    constexpr std::uint32_t SPELL_BLOCK_LAST = 205099;

    constexpr bool InSpellBlock(std::uint32_t spellId)
    {
        return spellId >= SPELL_BLOCK_FIRST && spellId <= SPELL_BLOCK_LAST;
    }

    // M5 descriptor stand-ins (STAGE-M5-HANDOFF §3.6; declared in core apps/dbc-tools source/npcs/boss_mechanics.py).
    // The drills (MechanicDescriptorDrills.h) and their presets (MechanicDescriptorPresets.h) use them.
    constexpr std::uint32_t SPELL_DRILL_CAST_LOW = 205010;      // interruptible 2.0 s, priority low
    constexpr std::uint32_t SPELL_DRILL_CAST_MID = 205011;      // interruptible 2.5 s, priority mid
    constexpr std::uint32_t SPELL_DRILL_CAST_HIGH = 205012;     // interruptible 3.0 s, priority high
    constexpr std::uint32_t SPELL_DRILL_CAST_NO_KICK = 205013;  // do-not-kick (a kick is a violation)
    constexpr std::uint32_t SPELL_DRILL_DEBUFF_MAGIC = 205014;
    constexpr std::uint32_t SPELL_DRILL_DEBUFF_CURSE = 205015;
    constexpr std::uint32_t SPELL_DRILL_DEBUFF_POISON = 205016;
    constexpr std::uint32_t SPELL_DRILL_DEBUFF_DISEASE = 205017;
    constexpr std::uint32_t SPELL_DRILL_STUN = 205018;          // urgent loss of control (Magic)
    constexpr std::uint32_t SPELL_DRILL_DEADLINE = 205019;      // Magic, 8 s; expiring casts SPELL_DRILL_DEADLINE_BURST
    constexpr std::uint32_t SPELL_DRILL_DEADLINE_BURST = 205020;
    constexpr std::uint32_t SPELL_DRILL_PURGEABLE = 205021;     // Magic buff on an add
    constexpr std::uint32_t SPELL_DRILL_ENRAGE = 205022;        // Enrage buff (soothe)
    constexpr std::uint32_t SPELL_DRILL_AIMING = 205023;        // raises an add's target priority
    constexpr std::uint32_t SPELL_M5_FIRST = SPELL_DRILL_CAST_LOW;
    constexpr std::uint32_t SPELL_M5_LAST = SPELL_DRILL_AIMING;     // 205024-205029 reserved, undeclared

    // `.mech test rider=`: the harmless drill auras only (Baited and Marked). Weakened would be handed out on a job hit
    // (around the user ruling) and outlive the encounter; a resolve spell would deal a second hit; the M5 stand-ins
    // deal damage, stun or buff enemies (STAGE-M5-HANDOFF §3.6).
    constexpr bool RiderAllowed(std::uint32_t spellId)
    {
        return spellId == SPELL_BAITED_DRILL || spellId == SPELL_MARKED_DRILL;
    }

    constexpr std::uint32_t ResolveSpellForSchool(std::uint8_t school)
    {
        return school <= 6 ? SPELL_RESOLVE_BASE + school : SPELL_RESOLVE_BASE;
    }

    // Creatures
    constexpr std::uint32_t NPC_MECHANIC_MARKER = 300400;
    constexpr std::uint32_t NPC_MECHANIC_TEST_OWNER = 300401;
    constexpr std::uint32_t NPC_MECHANIC_DUMMY = 300402;          // npc_boss_mechanics_dummy (MechanicDummyAI.cpp)
    // M5 (STAGE-M5-HANDOFF §3.6): the second dummy (same script, immunities -273: kickable) and the drill creatures.
    constexpr std::uint32_t NPC_DESCRIPTOR_DUMMY = 300403;
    constexpr std::uint32_t NPC_DRILL_CASTER = 300404;
    constexpr std::uint32_t NPC_DRILL_ADD = 300405;
    constexpr std::uint32_t NPC_DRILL_COPY = 300406;            // the ignore list's copy
    constexpr std::uint32_t NPC_DRILL_PRISON = 300407;          // holds a player (break-out)
    constexpr std::uint32_t NPC_BLOCK_FIRST = 300400;
    constexpr std::uint32_t NPC_BLOCK_LAST = 300419;
    // The reworked bosses' CreatureImmunitiesId (stun, fear, root, silence ...; not interrupt), as 300403 carries it.
    // M5 WP-F (R M-2): the sim's descriptor slice gives it to its owner, so stuns and silences stop no program cast.
    constexpr std::int32_t REWORKED_BOSS_IMMUNITIES = -273;

    constexpr bool IsTrainingDummy(std::uint32_t entry)
    {
        return entry == NPC_MECHANIC_DUMMY || entry == NPC_DESCRIPTOR_DUMMY;
    }

    constexpr bool InCreatureBlock(std::uint32_t entry)
    {
        return entry >= NPC_BLOCK_FIRST && entry <= NPC_BLOCK_LAST;
    }

    // Displays: a table of shape ladders inside one 100-id block per color (see the table above); the two line
    // ladders have their own ranges.
    enum class MarkerShape : std::uint8_t
    {
        Disc,           // F
        Ring,           // O
        TimerRing,      // a donut's timer, rim only
        Donut,          // F, step = kDonutRatios index
        DonutOutline,   // O
        Cone,           // F, step = kConeAngles index
        ConeOutline,    // O
        DashedRing,     // an armed mine: rim dashes, no interior
        Line,           // F, step = line ladder index
        LineOutline     // O
    };

    constexpr std::uint8_t COLOR_COUNT = 9;
    constexpr std::uint32_t DISPLAY_FIRST = 91000;
    constexpr std::uint32_t DISPLAY_COLOR_STRIDE = 100;
    constexpr std::uint32_t DISPLAY_LINE_FIRST = 92000;             // line (F); the outline ladder follows it
    constexpr std::uint32_t DISPLAY_LINE_OUTLINE_FIRST = 93000;
    constexpr std::uint32_t DISPLAY_SLICE_LAST = 93999;

    // The timer ring's band, as a fraction of its radius (patch_bossmechanics_markers.py TIMER_RIM_FRAC).
    constexpr float kTimerRingBandFrac = 0.04f;

    // Donut inner/outer ratio ladder (j = index).
    constexpr std::array<float, 7> kDonutRatios = { 0.25f, 0.33f, 0.40f, 0.50f, 0.60f, 0.67f, 0.75f };
    // Cone angle ladder in degrees (j = index).
    constexpr std::array<float, 8> kConeAngles = { 30.f, 45.f, 60.f, 90.f, 120.f, 150.f, 180.f, 270.f };
    // Line aspect ladder (LineAspect in MechanicMath.h): quarter-octave steps from 1/4 below aspect 1, eighth-octave
    // steps from 1 to 64 (step kLineUnitStep is aspect 1). Longer lines draw abutting pieces of aspect 64.
    constexpr std::uint32_t kLineSteps = 57;
    constexpr std::uint32_t kLineUnitStep = 8;
    constexpr float kLineMinAspect = 0.25f;
    constexpr float kLineMaxAspect = 64.f;

    // Steps per shape (MarkerShape order).
    constexpr std::array<std::uint32_t, 10> kMarkerShapeSteps = { 1, 1, 1,
        static_cast<std::uint32_t>(kDonutRatios.size()), static_cast<std::uint32_t>(kDonutRatios.size()),
        static_cast<std::uint32_t>(kConeAngles.size()), static_cast<std::uint32_t>(kConeAngles.size()), 1,
        kLineSteps, kLineSteps };

    constexpr bool IsLineShape(MarkerShape shape)
    {
        return shape == MarkerShape::Line || shape == MarkerShape::LineOutline;
    }

    // First offset of each non-line shape inside a 91000 color block: the running sum of the steps before it (the
    // line entries hold the block's end).
    constexpr std::array<std::uint32_t, 9> kMarkerShapeOffsets = []
    {
        std::array<std::uint32_t, 9> out{};
        for (std::size_t i = 0; i + 1 < out.size(); ++i)
            out[i + 1] = out[i] + kMarkerShapeSteps[i];
        return out;
    }();

    constexpr std::uint32_t DISPLAY_PER_COLOR = kMarkerShapeOffsets.back();    // 34
    constexpr std::uint32_t DISPLAY_SHAPES_LAST =
        DISPLAY_FIRST + DISPLAY_COLOR_STRIDE * (COLOR_COUNT - 1) + DISPLAY_PER_COLOR - 1;
    constexpr std::uint32_t DISPLAY_LAST =
        DISPLAY_LINE_OUTLINE_FIRST + DISPLAY_COLOR_STRIDE * (COLOR_COUNT - 1) + kLineSteps - 1;
    static_assert(DISPLAY_PER_COLOR <= DISPLAY_COLOR_STRIDE && kLineSteps <= DISPLAY_COLOR_STRIDE,
        "a color block overflows its 100 ids");
    static_assert(DISPLAY_SHAPES_LAST == 91833 && DISPLAY_SHAPES_LAST < DISPLAY_LINE_FIRST);
    static_assert(DISPLAY_LINE_FIRST + DISPLAY_COLOR_STRIDE * (COLOR_COUNT - 1) + kLineSteps - 1 == 92856
        && 92856 < DISPLAY_LINE_OUTLINE_FIRST);
    static_assert(DISPLAY_LAST == 93856 && DISPLAY_LAST <= DISPLAY_SLICE_LAST);

    // The armed-mine dashed ring of the red block (+33), pinned for the asset script's tests.
    static_assert(DISPLAY_FIRST + 33 == 91033 && kMarkerShapeSteps.size() == 10);

    // The display of one shape step in one color (patch_bossmechanics_markers.py display_id). color and step are
    // clamped into range, so every call names a shipped display.
    constexpr std::uint32_t MarkerDisplay(std::uint8_t color, MarkerShape shape, std::uint32_t step = 0)
    {
        std::size_t const s = static_cast<std::size_t>(shape);
        std::uint32_t const c = color < COLOR_COUNT ? color : COLOR_COUNT - 1;
        std::uint32_t const k = step < kMarkerShapeSteps[s] ? step : kMarkerShapeSteps[s] - 1;
        if (IsLineShape(shape))
            return (shape == MarkerShape::Line ? DISPLAY_LINE_FIRST : DISPLAY_LINE_OUTLINE_FIRST)
                + DISPLAY_COLOR_STRIDE * c + k;
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
        std::uint32_t first = DISPLAY_FIRST;
        std::uint32_t perColor = DISPLAY_PER_COLOR;
        if (display >= DISPLAY_LINE_OUTLINE_FIRST)
        {
            first = DISPLAY_LINE_OUTLINE_FIRST;
            perColor = kLineSteps;
            out.shape = MarkerShape::LineOutline;
        }
        else if (display >= DISPLAY_LINE_FIRST)
        {
            first = DISPLAY_LINE_FIRST;
            perColor = kLineSteps;
            out.shape = MarkerShape::Line;
        }
        if (display < first || display >= first + DISPLAY_COLOR_STRIDE * COLOR_COUNT)
            return out;
        std::uint32_t const rel = display - first;
        std::uint32_t const offset = rel % DISPLAY_COLOR_STRIDE;
        if (offset >= perColor)
            return out;
        out.valid = true;
        out.color = static_cast<std::uint8_t>(rel / DISPLAY_COLOR_STRIDE);
        if (first != DISPLAY_FIRST)
        {
            out.step = offset;
            return out;
        }
        std::size_t s = 0;
        while (offset >= kMarkerShapeOffsets[s + 1])
            ++s;
        out.shape = static_cast<MarkerShape>(s);
        out.step = offset - kMarkerShapeOffsets[s];
        return out;
    }
}

#endif
