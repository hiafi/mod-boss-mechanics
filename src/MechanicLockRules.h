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
    // M4 WP-F2 (STAGE-M4-R2-REPORT m-5): the spot a gap closer really stops on, for the lock's landing test in Idle
    // and Watch (MechanicLanding.h LandingPoint). None: it does not land next to its target (a teleport home).
    enum class LandingSpot : uint8_t
    {
        None,
        Center,         // on the target's center: a jump at the unit (SPELL_EFFECT_JUMP, MoveJump), Heroic Leap
        Reach,          // at the target's reach on the bot's side: a charge (Spell::EffectCharge's collision point)
        Behind,         // behind the target, by its facing: Shadowstep, Ambush from stealth
        CenterOrReach,  // a scripted landing near the target the lock does not model exactly: both probes
    };

    struct ScriptedMover
    {
        uint32_t spellId;
        MoverRule rule;
        LandingSpot landing;    // where a cast at a unit lands (STAGE-M4-R2-REPORT nit 4: kept with the row)
    };

    // tests/lock_rules_test.cpp pins this list: removing a row fails the test.
    inline constexpr std::array<ScriptedMover, 6> kScriptedMovers = { {
        // Death from Above: the DUMMY hit launches the leap (spell_rogue_subtlety.cpp, Rogue::LaunchDeathFromAbove;
        // its landing point is RogueMechanics' own pick near the target).
        { 204148, MoverRule::Always, LandingSpot::CenterOrReach },
        // Killing Spree: 57840 TELEPORT_UNITS on every hit for 2 s (spell_rogue_combat.cpp,
        // spell_rog_killing_spree_rework).
        { 51690, MoverRule::Always, LandingSpot::CenterOrReach },
        // Ambush: from stealth out of melee range, 203726 teleports the rogue behind the target
        // (spell_rogue_shared.cpp, spell_rog_ambush). Only 8676 carries the script; the retired ranks do not, and are
        // not listed.
        { 8676, MoverRule::StealthOutOfMelee, LandingSpot::Behind },
        // Grappling Hook: a MoveJump to the ground point, the target's position (spell_rogue_combat.cpp; also a named
        // mover).
        { 203963, MoverRule::Always, LandingSpot::Center },
        // Heroic Leap: a MoveJump to the ground point, the target's position (spell_warrior_shared.cpp; also a named
        // mover).
        { 204400, MoverRule::Always, LandingSpot::Center },
        // Demonic Circle: Teleport: NearTeleportTo the circle (spell_warlock.cpp). No rework row casts it today.
        { 48020, MoverRule::Always, LandingSpot::None },
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

    // WP-Q3 (m4-q3/q4 Hit Spread/3): the scripted movers whose aura keeps moving the bot after the cast. Killing Spree
    // teleports the rogue next to a target on every hit, for up to 2.5 s (500 ms per combo point;
    // spell_rogue_combat.cpp, spell_rog_killing_spree_rework_aura). The lock judges the cast, not what comes after it:
    // a Spread warned while the aura ran had the rogue teleported back next to the boss, into the other melee's
    // circle, on every hit of its escape. tests/lock_rules_test.cpp pins this list.
    inline constexpr std::array<uint32_t, 1> kMoverAuras = { { 51690 } };

    inline bool IsMoverAura(uint32_t spellId)
    {
        for (uint32_t id : kMoverAuras)
            if (id == spellId)
                return true;
        return false;
    }

    // A mover aura is cancelled while the brain owns where the bot stands: an escape walk, a hold, a freeze (Hold
    // still). In Idle and Watch the spell the stock AI chose runs on.
    inline bool MoverAuraBreaks(bool escape, bool hold, bool freeze)
    {
        return escape || hold || freeze;
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

    // M5 CD (C6): a facing-only action (`set facing`) is refused while the brain walks the bot (Escape, a route's
    // legs included, or a deferred escape) and under an open look-away window, as before. It does not stop the escape
    // spline (ServerFacade::SetFacingTo only sets the orientation and sends a heartbeat), but every such tick
    // broadcasts a heartbeat that snaps the walking bot on observers' clients and spends an action slot and a react
    // delay. Standing (Watch, Hold, Freeze) it stays allowed.
    inline bool FacingOnlyRefused(bool escape, bool lookAway)
    {
        return escape || lookAway;
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

    // WP-F2 (m-5): where a named mover that closes in stops. Heroic Leap and Grappling Hook jump onto the target's
    // position; Blade Rush (203980) and Intervene (3411) are charges; Shadowstep (36554) teleports behind the target;
    // Death from Above picks its own spot near it.
    inline LandingSpot NamedMoverSpot(std::string_view name)
    {
        switch (NamedMoverLanding(name))
        {
            case MoverLanding::None:
                return LandingSpot::None;
            case MoverLanding::CurrentTarget:
            case MoverLanding::LooseEnemy:
                return LandingSpot::Center;
            case MoverLanding::CastTarget:
                break;
        }
        if (name == "shadowstep" || name == "subt shadowstep")
            return LandingSpot::Behind;
        if (name == "subt death from above")
            return LandingSpot::CenterOrReach;
        return LandingSpot::Reach;     // Blade Rush, Intervene
    }

    // The landing of a scripted mover cast at a unit (its table row); None for an unlisted id or one that lands
    // elsewhere (Demonic Circle: Teleport goes to the circle, not the target).
    inline LandingSpot ScriptedMoverSpot(uint32_t spellId)
    {
        ScriptedMover const* mover = FindScriptedMover(spellId);
        return mover ? mover->landing : LandingSpot::None;
    }

    inline bool ScriptedMoverLandsAtTarget(uint32_t spellId)
    {
        return ScriptedMoverSpot(spellId) != LandingSpot::None;
    }

    // A spell's effects that carry its caster to the cast unit, as the lock reads them (Spell::EffectCharge,
    // EffectChargeDest, EffectJump, EffectJumpDest): a charge stops at the target's reach on the caster's side, a jump
    // lands on the target's position. A jump wins over a charge (the center probe is the stricter one only for it).
    inline LandingSpot SpellEffectSpot(bool charge, bool jump)
    {
        if (jump)
            return LandingSpot::Center;
        return charge ? LandingSpot::Reach : LandingSpot::None;
    }

    // The probe of a charge or a Behind landing, as a distance from the destination unit's center: its reach plus
    // kLandingReachYd (a charge stops at the reach's collision point); a Reach probe never lies past the bot.
    // `centerToBotYd` is the 2D distance between them.
    inline constexpr float kLandingReachYd = 1.0f;

    inline float LandingNearProbeYd(float centerToBotYd, float destCombatReachYd)
    {
        float const probe = destCombatReachYd + kLandingReachYd;
        return probe < centerToBotYd ? probe : centerToBotYd;
    }

    // ---- WP-F2 (STAGE-M4-R2-REPORT M-1): the melee chase's safety valve -------------------------------------------

    // The lock refuses the stock melee chase only while no point of the melee ring round its target is safe
    // (MechanicLanding.h RingSafe); the brain counts a chase whose end fails with no band re-route as refused too.
    // A melee bot or tank refused for its release time (BossMechanics.Bots.ChaseReleaseMs; a tank half of it), out of
    // reach all along, walks in for that long (the brain's StepChaseValve; M5 CD: a walk-in, see WalkInHorizonMs): back
    // into melee and stays. A stranded bot loses all its damage, a tank its threat (R2 Q-1 ruling: tanks are not
    // exempt, they get the valve at half the time). The refusal looks a whole horizon ahead (HorizonMs, 10 s), so the
    // valve only needs the ring to be clear for the walk in, the stay and the walk out (ChaseValveWindowMs): a
    // valve that walked the bot back into a zone about to resolve put the melee into alexei's Out of bounds at its
    // Hold still (fight_test TestChaseValve), the m4-s2 Zone/5 the lock was made for.
    struct ChaseClock
    {
        int64_t refusedSince = 0;   // the first refusal of the current run (0 = none)
        int64_t lastRefusedAt = 0;  // the run's last refusal
    };

    // A refusal "stands" this long after it was judged (msChaseRefused, the brain's recovery solve): a few AI ticks.
    inline constexpr int64_t kChaseRefusedNowMs = 500;
    // A run of refusals ends when none came for HoldCapMs + this (a bot held for an unsafe chase end is not judged
    // again before its hold ends).
    inline constexpr int64_t kChaseRunGapMs = 1000;

    inline int64_t ChaseRunStaleMs(uint32_t holdCapMs)
    {
        return static_cast<int64_t>(holdCapMs) + kChaseRunGapMs;
    }

    inline int64_t ChaseReleaseMsFor(uint32_t releaseMs, bool tank)
    {
        return tank ? releaseMs / 2 : releaseMs;
    }

    // One refused chase at `nowMs`.
    inline void NoteChaseRefusal(ChaseClock& c, int64_t nowMs, int64_t staleMs)
    {
        if (c.lastRefusedAt != 0 && nowMs - c.lastRefusedAt > staleMs)
            c.refusedSince = 0;
        if (c.refusedSince == 0)
            c.refusedSince = nowMs;
        c.lastRefusedAt = nowMs;
    }

    // The brain, every step: a bot in reach of its target, with no melee band, or just released ends the run.
    inline void ChaseInReach(ChaseClock& c)
    {
        c.refusedSince = 0;
        c.lastRefusedAt = 0;
    }

    // The last chase judgement refused it, a moment ago.
    inline bool ChaseRefusedNow(ChaseClock const& c, int64_t nowMs)
    {
        return c.lastRefusedAt != 0 && nowMs - c.lastRefusedAt <= kChaseRefusedNowMs;
    }

    // The walk in and out a valve's walk-in must find clear on some point of the ring, beyond its stay itself.
    inline constexpr int64_t kChaseValveSlackMs = 2000;

    inline int64_t ChaseValveWindowMs(int64_t releaseMs)
    {
        return releaseMs + kChaseValveSlackMs;
    }

    // M5 CD (C3 A, C5): the valve no longer releases the bot blind. It starts a walk-in until `untilMs` (now + the
    // release time): the bot is judged as in Watch (its spot, its stock chase, the lock's ring test), but looks only
    // this far ahead, the window the valve found clear. A hazard registered during the walk-in that threatens the bot
    // inside that window is answered at once (the blind release walked melee into the barrage's donuts and onto mines
    // on the chase line); the lasting hazard that refused the chase is seen again once the walk-in ends, in time to
    // leave. Outside a walk-in: the full horizon.
    inline int64_t WalkInHorizonMs(int64_t untilMs, int64_t nowMs, int64_t horizonMs)
    {
        if (untilMs <= nowMs)
            return horizonMs;
        int64_t const walkIn = untilMs - nowMs + kChaseValveSlackMs;
        return walkIn < horizonMs ? walkIn : horizonMs;
    }

    // M5 CD (C5): a melee bot out of melee range (its band + Landing::kMeleeBandSlackYd) for its release time while
    // some point of the melee ring is clear over the valve's window (ChaseValveWindowMs) is stranded: the stock chase
    // line ran into a lasting hazard on its side (solve, escape, hold, again), which the refusal clock never saw. The
    // brain counts it (msChaseStranded) and routes to the first ring point that is safe over the horizon, this far
    // inside the band (Landing::RingPoint). A whole-ring block (alexei's Out of bounds, a mine on the boss) is not
    // stranding: no ring point is clear.
    inline constexpr float kStrandRingInsetYd = 0.3f;

    inline bool StrandedFor(int64_t lastInRangeMs, int64_t nowMs, int64_t releaseMs)
    {
        return lastInRangeMs != 0 && nowMs - lastInRangeMs >= releaseMs;
    }

    // The valve is due: an unbroken run of refusals (none staleMs apart) at least releaseMs long, still going.
    inline bool ChaseValveDue(ChaseClock const& c, int64_t nowMs, int64_t releaseMs, int64_t staleMs)
    {
        return c.refusedSince != 0 && nowMs - c.lastRefusedAt <= staleMs && nowMs - c.refusedSince >= releaseMs;
    }

    // M4 WP-Q2: the waker's fast tick (BossMechanics.Bots.FastTickMs). A bot woken in the waker runs its AI some time
    // after the wake, not in this update, so the wake is due once the last brain step will be FastTickMs old by then.
    // `leadMs` is that delay, capped at FastTickMs (0 when unknown: never early). Without it a map that updates every
    // 100 ms (the sim, a loaded live map) runs a mechanics bot's brain every 200 ms, not every 100.
    inline bool FastTickDue(int64_t sinceBrainMs, int64_t leadMs, int64_t fastTickMs)
    {
        if (leadMs < 0)
            leadMs = 0;
        if (leadMs > fastTickMs)
            leadMs = fastTickMs;
        return sinceBrainMs + leadMs >= fastTickMs;
    }

    // WP-F2 (STAGE-M4-R2-REPORT m-2): the wake's lead is measured, not assumed. The map's full-update interval (the
    // waker's own gap) is the lead in the sim, where a bot updates on full updates only; live, Map::Update ticks
    // players on the partial updates too, so a woken bot thinks one world tick after the wake, often well before the
    // next full update, and the full-update gap would wake it up to 1.7x too often under load. The brain stamps the
    // time from a wake to the brain step that follows it; the waker keeps the smallest one seen (a lead that comes
    // later than the estimate only delays a step, one that comes earlier would shorten it) and never more than its
    // own gap. -1 = none measured yet (FastTickDue then uses 0).
    inline int64_t NextWakeLead(int64_t estimateMs, int64_t measuredMs)
    {
        if (measuredMs < 0)
            return estimateMs;
        return estimateMs < 0 || measuredMs < estimateMs ? measuredMs : estimateMs;
    }

    // The lead FastTickDue gets: the estimate, never past the waker's own gap (0 when there is none yet).
    inline int64_t WakeLeadFor(int64_t estimateMs, int64_t gapMs)
    {
        if (estimateMs < 0 || gapMs <= 0)
            return 0;
        return estimateMs < gapMs ? estimateMs : gapMs;
    }

    // M5 CD (E, STAGE-M5-I-REPORT question 3): the waker also wakes a bot for its boss descriptor, so a kick or an
    // urgent dispel does not wait out playerbots' react delay (0.5-3 s for a bot with no real master, 100 ms and up
    // otherwise). One signature per kind: what the bot could act on now (the listed casts in progress it can kick,
    // the listed urgent or deadline auras on its group it can cure), mixed order-free from (unit, id, instance); 0 =
    // nothing. A wake is due when the signature is new and not empty (a cast or aura that ended only empties it).
    inline uint64_t MixWakeSignature(uint64_t sig, uint64_t unit, uint64_t id, uint64_t instance)
    {
        uint64_t h = unit * 0x9E3779B97F4A7C15ull;
        h ^= (id + 0x632BE59BD9B4E019ull) * 0xBF58476D1CE4E5B9ull;
        h ^= instance * 0x94D049BB133111EBull;
        h ^= h >> 31;
        return sig + (h | 1ull);
    }

    inline bool SignatureWakeDue(uint64_t seen, uint64_t now)
    {
        return now != 0 && now != seen;
    }

    // ...and when the descriptor's revision moved since the bot's view was last brought up to date (a new or edited
    // descriptor, a phase, a NoteNextCast, a listed cast another bot saw start), while a descriptor applies to it.
    // `seenRevision` 0: the view was never built (the first update after the bot joined the map does not wake).
    inline bool DescriptorWakeDue(uint32_t seenRevision, uint32_t revision, bool active)
    {
        return active && seenRevision != 0 && seenRevision != revision;
    }
}

#endif
