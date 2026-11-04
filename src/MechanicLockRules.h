/*
 * mod-boss-mechanics: the pure rules of the lock multiplier (MechanicBotLock) that need no live unit
 * (dungeon-mechanics STAGE-M4-HANDOFF §3.5, WP-X; the audit is STAGE-M4-L-REPORT §6.3, §10 X1-X3, X6, X8), and the
 * forced-leak debug check (BossMechanics.Bots.ForceLeaks, M4-SWEEP G8).
 *
 * No AzerothCore or playerbots includes: the lock resolves units, spells and actions into the plain values below, and
 * tests/lock_rules_test.cpp compiles these with the host compiler.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_LOCK_RULES_H
#define MOD_BOSS_MECHANICS_MECHANIC_LOCK_RULES_H

#include <array>
#include <cstdint>
#include <string_view>

namespace BossMechanics::LockRules
{
    // When a scripted mover moves its caster.
    enum class MoverRule : uint8_t
    {
        Always,                 // every cast moves the caster
        StealthOutOfMelee,      // only from stealth, on a target out of melee range (the script's own CheckCast)
    };

    // A spell that moves its caster from a core C++ script, so its spell effects do not show it (the lock's effect
    // test sees CHARGE, LEAP, JUMP and TELEPORT_UNITS only). Hardcoded, not a conf key: the scripts make them movers,
    // so a new one needs a rebuild anyway, and the ids are dbc-tools reserved ranges that do not drift.
    struct ScriptedMover
    {
        uint32_t spellId;
        MoverRule rule;
    };

    // tests/lock_rules_test.cpp pins this list: removing a row fails the test.
    inline constexpr std::array<ScriptedMover, 6> kScriptedMovers = { {
        // Death from Above: the DUMMY hit launches the leap (spell_rogue_subtlety.cpp, Rogue::LaunchDeathFromAbove).
        { 204148, MoverRule::Always },
        // Killing Spree: 57840 TELEPORT_UNITS on every hit for 2 s (spell_rogue_combat.cpp,
        // spell_rog_killing_spree_rework).
        { 51690, MoverRule::Always },
        // Ambush: from stealth out of melee range, 203726 teleports the rogue behind the target
        // (spell_rogue_shared.cpp, spell_rog_ambush). Only 8676 carries the script; the retired ranks do not, and are
        // not listed.
        { 8676, MoverRule::StealthOutOfMelee },
        // Grappling Hook: a MoveJump to the ground point (spell_rogue_combat.cpp; also a named mover).
        { 203963, MoverRule::Always },
        // Heroic Leap: a MoveJump to the ground point (spell_warrior_shared.cpp; also a named mover).
        { 204400, MoverRule::Always },
        // Demonic Circle: Teleport: NearTeleportTo the circle (spell_warlock.cpp). No rework row casts it today.
        { 48020, MoverRule::Always },
    } };

    // The table row of a spell, or nullptr.
    inline ScriptedMover const* FindScriptedMover(uint32_t spellId)
    {
        for (ScriptedMover const& mover : kScriptedMovers)
            if (mover.spellId == spellId)
                return &mover;
        return nullptr;
    }

    // Whether this cast of the row's spell moves the caster.
    inline bool ScriptedMoverMoves(MoverRule rule, bool stealthed, bool hasTarget, bool inMeleeRange)
    {
        switch (rule)
        {
            case MoverRule::Always:
                return true;
            case MoverRule::StealthOutOfMelee:
                return stealthed && hasTarget && !inMeleeRange;
        }
        return false;
    }

    // The spell `spellId` moves its caster from a script (0 or an unlisted id: false).
    inline bool IsScriptedMoverCast(uint32_t spellId, bool stealthed, bool hasTarget, bool inMeleeRange)
    {
        ScriptedMover const* mover = FindScriptedMover(spellId);
        return mover && ScriptedMoverMoves(mover->rule, stealthed, hasTarget, inMeleeRange);
    }

    // Movers that are not MovementActions, found by the lock audits (STAGE-M2-C-REPORT "Lock audit",
    // STAGE-M4-L-REPORT §3), by the name the action reports (getName(), not always its creator key, so both are
    // listed where they differ). Gap-closers, leaps and blinks in the class rotations; a CastSpellAction is also caught
    // by its spell effect or the scripted-mover table, these are named because a plain Action casts it or the lock
    // cannot learn the action's spell id:
    // - "heroic leap", "heroic leap loose": WarriorHeroicLeapAction (Action; ground-targeted CastSpell 204400);
    // - "cmbt grappling hook": RogueGrapplingHookAction (Action; CastSpell 203963 pulls the rogue to the target);
    // - "cmbt blade rush", "cmbt pack blade rush": RogueCastAction, spell 203980 (a charge);
    // - "shadowstep" (creator "subt shadowstep"): CastSpellAction, spell 36554 (teleport behind the target);
    // - "blink back": CastBlinkBackAction (turns away, then Blink);
    // - "disengage" (also created as "hunter disengage"): CastDisengageAction (leap back);
    // - "arms intervene": WarriorArmsCastAction, spell 3411 (a CHARGE to an ally), and "subt death from above":
    //   RogueCastAction, spell 204148 (a scripted leap). Interim names (M4 X3) for a fork without
    //   CastSpellAction::GetCastSpellId(); harmless once the effect test and the table see the ids.
    // Charge, Intercept and Feral Charge are CastReachTargetSpellAction and are matched by type.
    inline constexpr std::array<std::string_view, 12> kNamedMovers = { "heroic leap", "heroic leap loose",
        "cmbt grappling hook", "cmbt blade rush", "cmbt pack blade rush", "shadowstep", "subt shadowstep", "blink back",
        "disengage", "hunter disengage", "arms intervene", "subt death from above" };

    inline bool IsNamedMoverName(std::string_view name)
    {
        for (std::string_view const mover : kNamedMovers)
            if (mover == name)
                return true;
        return false;
    }

    // Self casts that first turn the bot to its current target (their spell has no facing flag and lands on the bot,
    // so the cast-target test cannot see the turn): MageFrostFrozenOrbAction ("frozen orb", creator "frost frozen
    // orb") and DkFacingCastAction (Breath of Sindragosa and Glacial Advance rows). Judged against the current target.
    inline constexpr std::array<std::string_view, 5> kTurningSelfCasts = { "frozen orb", "frost frozen orb",
        "dk frost breath", "dk frost breath plain", "dk frost glacial advance" };

    inline bool IsTurningSelfCastName(std::string_view name)
    {
        for (std::string_view const cast : kTurningSelfCasts)
            if (cast == name)
                return true;
        return false;
    }

    // One cast under an open look-away window (decision §7.6), as the lock resolved it.
    struct LookAwayCast
    {
        bool groundCast = false;            // a CastAtPositionAction: PlayerbotAI::CastSpell(x, y, z) turns the bot to
                                            // the point, which the lock does not see (it can be the bot's own feet)
        bool turnsToCurrentTarget = false;  // a turning self cast (kTurningSelfCasts)
        bool currentTargetOutside = false;  // ...and the current target lies outside the chosen arc
        bool facingInfront = false;         // the spell carries SPELL_FACING_FLAG_INFRONT (false when the id is 0)
        bool offensiveTarget = false;       // the cast target is an enemy other than the bot
        bool targetOutside = false;         // the cast target lies outside the chosen arc (false with no target)
    };

    // The cast would turn the bot off its chosen facing. Needs no spell id: an offensive cast is judged by its target
    // alone (rework actions turn the bot to it themselves), so an id the lock cannot learn still gets the test.
    inline bool LookAwayBlocksCast(LookAwayCast const& cast)
    {
        if (cast.groundCast)
            return true;
        if (cast.turnsToCurrentTarget && cast.currentTargetOutside)
            return true;
        return (cast.facingInfront || cast.offensiveTarget) && cast.targetOutside;
    }

    // M4 X6 (STAGE-M4-L-REPORT §2.3): what the waker does about the look-away facing of a bot with a cast or channel
    // in progress. A cast in PREPARING stops the bot's engine (PlayerbotAI::UpdateAI yields), so the face action cannot
    // turn it, and a ground cast left it turned to its point (its own feet: world orientation 0). Only an
    // INFRONT-flagged cast on a unit target is turned back to that target every update; the rest keep whatever facing
    // they have. A channel keeps the engine running, but the waker need not wait for the face action.
    enum class CastFacingStep : uint8_t
    {
        None,       // nothing to do
        Break,      // interrupt it: it re-faces a target outside the arc (the M3 rule, facingBreaks)
        Turn,       // turn the bot to the look-away facing now; the cast goes on (facingsApplied)
    };

    struct CastFacing
    {
        bool breaksLookAway = false;    // BotFacing::CastBreaksLookAway: an INFRONT cast on a target outside the arc
                                        // whose window opens before the cast ends
        bool windowOpen = false;        // the look-away window (+- margin) is open now
        bool escaping = false;          // Escape: a move turns the bot along its spline; the facing waits
        bool facingOff = false;         // the bot is off the look-away facing by more than the face tolerance
    };

    // Turning is safe for every cast that is not broken: an INFRONT one that stays has its target inside the arc of
    // the look-away facing (CAST_ANGLE_IN_FRONT, narrower than the cast's own facing check), and the others need no
    // facing (heals, ground casts, self casts).
    inline CastFacingStep CastFacingStepOf(CastFacing const& c)
    {
        if (c.breaksLookAway)
            return CastFacingStep::Break;
        if (c.windowOpen && !c.escaping && c.facingOff)
            return CastFacingStep::Turn;
        return CastFacingStep::None;
    }

    // M4 X8 (STAGE-M4-L-REPORT §2.4): a human's `pull` mid-encounter starts playerbots' pull, whose PullMultiplier
    // then zeroes every other action (the brain's moves included) for up to 15 s. The lock refuses these two chat
    // actions (ChatCommandHandlerStrategy: "pull", "pull back", "pull rti") while an encounter is live on the map.
    inline constexpr std::array<std::string_view, 2> kPullRequests = { "pull my target", "pull rti target" };

    inline bool IsPullRequestName(std::string_view name)
    {
        for (std::string_view const pull : kPullRequests)
            if (pull == name)
                return true;
        return false;
    }

    // BossMechanics.Bots.ForceLeaks (a debug check of the leak detectors, MechanicLeaks.h): the detector a blocked
    // action would feed if it ran. Only blocks the detectors can see are tagged: a mover, a movement spell or a
    // scripted mover in Escape, Hold or Freeze (leakMoves); a cast-time spell or item in Escape or Hold (leakCasts); a
    // look-away block (leakTurns). Never our own actions, a cast-break block, tank facing or a pull.
    enum class LeakKind : uint8_t
    {
        None,
        Move,
        Cast,
        Turn
    };

    // The forced-leak check lets a tagged block of `kind` through while that detector has counted fewer than `budget`
    // leaks since the counters' last reset (0 = off). The first leak it counts closes it again, so it is self-ending
    // and spends nothing on candidates that never ran (the lock judges many per tick).
    inline bool ForceLeakPasses(LeakKind kind, uint32_t leaksCounted, uint32_t budget)
    {
        return budget != 0 && kind != LeakKind::None && leaksCounted < budget;
    }

    // M4 WP-Q2 (m4-s2 Frontal/1): where a gap closer lands, for the lock's landing test in Idle and Watch. Those modes
    // let every mover through (Escape, Hold and Freeze block them all), and Watch stops a stock walk once it sees it;
    // a leap or a charge cannot be stopped once cast, so its landing is judged before it runs.
    enum class MoverLanding : uint8_t
    {
        None,           // not a gap closer: it moves the bot away or elsewhere (Blink, Disengage, a teleport home)
        CastTarget,     // next to the action's own target (Shadowstep, Blade Rush, Death from Above, Intervene)
        CurrentTarget,  // next to the bot's current target (plain Actions: Heroic Leap, Grappling Hook)
        LooseEnemy,     // next to the Prot shell's "prot loose enemy" value ("heroic leap loose")
    };

    // The landing of a named mover (kNamedMovers); None for any other name.
    inline MoverLanding NamedMoverLanding(std::string_view name)
    {
        if (name == "heroic leap" || name == "cmbt grappling hook")
            return MoverLanding::CurrentTarget;
        if (name == "heroic leap loose")
            return MoverLanding::LooseEnemy;
        if (name == "cmbt blade rush" || name == "cmbt pack blade rush" || name == "shadowstep"
            || name == "subt shadowstep" || name == "arms intervene" || name == "subt death from above")
            return MoverLanding::CastTarget;
        return MoverLanding::None;     // "blink back", "disengage", "hunter disengage", and every non-mover
    }

    // A scripted mover (kScriptedMovers) that lands next to its cast target: Death from Above, Killing Spree, Ambush
    // (from stealth), Grappling Hook, Heroic Leap. Demonic Circle: Teleport goes to the circle, not the target.
    inline bool ScriptedMoverLandsAtTarget(uint32_t spellId)
    {
        return spellId == 204148 || spellId == 51690 || spellId == 8676 || spellId == 203963 || spellId == 204400;
    }

    // The near probe of the landing test, as a distance from the destination unit's center toward the bot: its reach
    // plus kLandingReachYd (a charge stops there), never past the bot. The other probe is the center itself (a leap
    // lands on it). `centerToBotYd` is the 2D distance between them.
    inline constexpr float kLandingReachYd = 1.0f;

    inline float LandingNearProbeYd(float centerToBotYd, float destCombatReachYd)
    {
        float const probe = destCombatReachYd + kLandingReachYd;
        return probe < centerToBotYd ? probe : centerToBotYd;
    }

    // M4 WP-Q2: the waker's fast tick (BossMechanics.Bots.FastTickMs). A bot woken in the waker runs its AI on the
    // map's next update, not this one, so the wake is due once the last brain step will be FastTickMs old by then.
    // `gapMs` is the time since the waker's previous sample of the bot (the map's update interval; 0 when unknown),
    // capped at FastTickMs. Without it a map that updates every 100 ms (the sim, a loaded live map) runs a mechanics
    // bot's brain every 200 ms, not every 100: the wake comes on the update after the one that ran the brain.
    inline bool FastTickDue(int64_t sinceBrainMs, int64_t gapMs, int64_t fastTickMs)
    {
        if (gapMs < 0)
            gapMs = 0;
        if (gapMs > fastTickMs)
            gapMs = fastTickMs;
        return sinceBrainMs + gapMs >= fastTickMs;
    }
}

#endif
