// Standalone tests for src/MechanicLockRules.h, the pure rules of the bots' lock multiplier (STAGE-M4-HANDOFF §3.5,
// WP-X; STAGE-M4-L-REPORT §6.3, §10 X1-X3): the scripted-mover table and its conditions, the named movers and the
// turning self casts, and the look-away cast rule; WP-X2: the waker's cast facing step (X6), the pull requests (X8) and
// the forced-leak passes (BossMechanics.Bots.ForceLeaks); WP-Q2: the gap closers' landings and the waker's fast tick;
// WP-F2: where each closer stops, the chase clock and its valve, and the measured wake lead.
// Build: tests/run.sh.

#include "check.h"
#include "MechanicLockRules.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

using namespace BossMechanics::LockRules;

namespace
{
    // The six scripted movers are pinned: an accidental removal (or a stray addition) fails here.
    void TestScriptedMoverTable()
    {
        CHECK(kScriptedMovers.size() == 6);
        std::set<uint32_t> ids;
        for (ScriptedMover const& mover : kScriptedMovers)
            ids.insert(mover.spellId);
        CHECK(ids.size() == kScriptedMovers.size());    // no duplicate row
        for (uint32_t id : { 204148u, 51690u, 8676u, 203963u, 204400u, 48020u })
            CHECK(FindScriptedMover(id) != nullptr);

        CHECK(FindScriptedMover(204148)->rule == MoverRule::Always);    // Death from Above
        CHECK(FindScriptedMover(51690)->rule == MoverRule::Always);     // Killing Spree
        CHECK(FindScriptedMover(8676)->rule == MoverRule::StealthOutOfMelee);   // Ambush
        CHECK(FindScriptedMover(203963)->rule == MoverRule::Always);    // Grappling Hook
        CHECK(FindScriptedMover(204400)->rule == MoverRule::Always);    // Heroic Leap
        CHECK(FindScriptedMover(48020)->rule == MoverRule::Always);     // Demonic Circle: Teleport

        // Not movers: 0, the Ambush ranks without the teleport script, the teleport helpers themselves (they are cast
        // by the scripts, never by an action), Shadowstep (its effect is TELEPORT_UNITS, the effect test sees it).
        for (uint32_t id : { 0u, 8724u, 8725u, 11267u, 11268u, 11269u, 27441u, 48689u, 48690u, 48691u, 203726u,
                 57840u, 36554u })
            CHECK(FindScriptedMover(id) == nullptr);
    }

    void TestScriptedMoverConditions()
    {
        // Always: whatever the state.
        for (bool stealthed : { false, true })
            for (bool hasTarget : { false, true })
                for (bool inMelee : { false, true })
                    CHECK(ScriptedMoverMoves(MoverRule::Always, stealthed, hasTarget, inMelee));

        // Ambush: only from stealth on a target out of melee range (spell_rog_ambush's CheckCast / BeforeCast).
        CHECK(ScriptedMoverMoves(MoverRule::StealthOutOfMelee, true, true, false));
        CHECK(!ScriptedMoverMoves(MoverRule::StealthOutOfMelee, true, true, true));     // in melee: a plain strike
        CHECK(!ScriptedMoverMoves(MoverRule::StealthOutOfMelee, false, true, false));   // out of stealth: no teleport
        CHECK(!ScriptedMoverMoves(MoverRule::StealthOutOfMelee, false, true, true));
        CHECK(!ScriptedMoverMoves(MoverRule::StealthOutOfMelee, true, false, false));   // no target: the cast fails

        // Through the id.
        CHECK(IsScriptedMoverCast(8676, true, true, false));
        CHECK(!IsScriptedMoverCast(8676, true, true, true));
        CHECK(IsScriptedMoverCast(51690, false, true, true));
        CHECK(IsScriptedMoverCast(204148, false, true, true));
        CHECK(!IsScriptedMoverCast(0, true, true, false));
        CHECK(!IsScriptedMoverCast(8724, true, true, false));
    }

    void TestNamedMovers()
    {
        // M2/M3 names keep their place.
        for (char const* name : { "heroic leap", "heroic leap loose", "cmbt grappling hook", "cmbt blade rush",
                 "cmbt pack blade rush", "shadowstep", "subt shadowstep", "blink back", "disengage",
                 "hunter disengage" })
            CHECK(IsNamedMoverName(name));
        // M4 X3 interim names.
        CHECK(IsNamedMoverName("arms intervene"));
        CHECK(IsNamedMoverName("subt death from above"));
        CHECK(kNamedMovers.size() == 12);
        // Exact names only: not a prefix, not the bare spell names of name casts the effect test covers.
        for (char const* name : { "", "arms", "intervene", "death from above", "arms intervene::x", "charge",
                 "killing spree", "ambush", "frozen orb" })
            CHECK(!IsNamedMoverName(name));
    }

    void TestTurningSelfCasts()
    {
        for (char const* name : { "frozen orb", "frost frozen orb", "dk frost breath", "dk frost breath plain",
                 "dk frost glacial advance" })
            CHECK(IsTurningSelfCastName(name));
        CHECK(kTurningSelfCasts.size() == 5);
        for (char const* name : { "", "dk frost obliterate", "dk pillar of frost", "frostbolt", "blizzard",
                 "dk death and decay" })
            CHECK(!IsTurningSelfCastName(name));
    }

    LookAwayCast Cast(bool ground, bool turnsSelf, bool currentOut, bool infront, bool offensive, bool targetOut)
    {
        LookAwayCast c;
        c.groundCast = ground;
        c.turnsToCurrentTarget = turnsSelf;
        c.currentTargetOutside = currentOut;
        c.facingInfront = infront;
        c.offensiveTarget = offensive;
        c.targetOutside = targetOut;
        return c;
    }

    void TestLookAwayRule()
    {
        // Ground casts: always blocked (DnD at the bot's own feet turns it to world orientation 0; a heal point has no
        // target to judge).
        CHECK(LookAwayBlocksCast(Cast(true, false, false, false, false, false)));
        CHECK(LookAwayBlocksCast(Cast(true, false, false, true, true, false)));

        // Turning self casts: judged by the current target.
        CHECK(LookAwayBlocksCast(Cast(false, true, true, false, false, false)));
        CHECK(!LookAwayBlocksCast(Cast(false, true, false, false, false, false)));

        // An offensive cast with no spell id (H1: facingInfront false): its target alone decides.
        CHECK(LookAwayBlocksCast(Cast(false, false, false, false, true, true)));
        CHECK(!LookAwayBlocksCast(Cast(false, false, false, false, true, false)));

        // A facing-flagged spell on anything outside the arc (a friendly target too).
        CHECK(LookAwayBlocksCast(Cast(false, false, false, true, false, true)));
        CHECK(!LookAwayBlocksCast(Cast(false, false, false, true, false, false)));

        // A heal (no facing flag, friendly target) behind the bot: allowed.
        CHECK(!LookAwayBlocksCast(Cast(false, false, false, false, false, true)));
        // A self buff: no target outside, nothing turns.
        CHECK(!LookAwayBlocksCast(Cast(false, false, false, false, false, false)));

        // Exhaustive: blocked exactly when one of the three rules holds.
        for (int bits = 0; bits < 64; ++bits)
        {
            LookAwayCast const c = Cast(bits & 1, bits & 2, bits & 4, bits & 8, bits & 16, bits & 32);
            bool const expected = c.groundCast || (c.turnsToCurrentTarget && c.currentTargetOutside)
                || ((c.facingInfront || c.offensiveTarget) && c.targetOutside);
            CHECK(LookAwayBlocksCast(c) == expected);
        }
    }

    // X6: the waker's step for a casting bot under look-away.
    void TestCastFacingStep()
    {
        auto step = [](bool breaks, bool open, bool escaping, bool off)
        {
            CastFacing c;
            c.breaksLookAway = breaks;
            c.windowOpen = open;
            c.escaping = escaping;
            c.facingOff = off;
            return CastFacingStepOf(c);
        };
        // A Flamestrike that left the bot facing its point, the window open, Hold: turned, the cast goes on.
        CHECK(step(false, true, false, true) == CastFacingStep::Turn);
        // Already on the facing, or the window not open yet: nothing.
        CHECK(step(false, true, false, false) == CastFacingStep::None);
        CHECK(step(false, false, false, true) == CastFacingStep::None);
        // Escape: the move turns the bot; the facing waits for it to stand (as the face action does).
        CHECK(step(false, true, true, true) == CastFacingStep::None);
        // The M3 rule wins: an INFRONT cast on a target outside the arc is broken, never turned (UpdateAI would turn
        // the bot back to its target every update).
        for (int bits = 0; bits < 8; ++bits)
            CHECK(step(true, bits & 1, bits & 2, bits & 4) == CastFacingStep::Break);
        // Exhaustive.
        for (int bits = 0; bits < 16; ++bits)
        {
            bool const breaks = bits & 1, open = bits & 2, escaping = bits & 4, off = bits & 8;
            CastFacingStep const expected = breaks ? CastFacingStep::Break
                : (open && !escaping && off) ? CastFacingStep::Turn : CastFacingStep::None;
            CHECK(step(breaks, open, escaping, off) == expected);
        }
    }

    // X8: the two pull requests, exact names only.
    void TestPullRequests()
    {
        CHECK(kPullRequests.size() == 2);
        CHECK(IsPullRequestName("pull my target"));
        CHECK(IsPullRequestName("pull rti target"));
        // The pull's own steps run only once a request started one; the chat trigger names are not action names.
        for (char const* name : { "pull", "pull back", "pull rti", "pull start", "pull action", "pull end",
                 "reach pull", "return to pull position", "dc pull", "Pull my target", "pull my target " })
            CHECK(!IsPullRequestName(name));
    }

    // BossMechanics.Bots.ForceLeaks: the lock's half of the forced-leak check (leak_fire_test.cpp runs it against
    // the detectors).
    void TestForceLeakPasses()
    {
        for (LeakKind kind : { LeakKind::Move, LeakKind::Cast, LeakKind::Turn })
        {
            CHECK(!ForceLeakPasses(kind, 0, 0));        // off
            CHECK(ForceLeakPasses(kind, 0, 1));
            CHECK(!ForceLeakPasses(kind, 1, 1));        // the detector counted it: closed
            CHECK(ForceLeakPasses(kind, 2, 3));
            CHECK(!ForceLeakPasses(kind, 7, 3));
        }
        CHECK(!ForceLeakPasses(LeakKind::None, 0, 1));  // a block no detector sees never passes
        CHECK(!ForceLeakPasses(LeakKind::None, 0, 100));
    }

    // WP-Q2: where each named mover lands (every name in the list has a decision), and which scripted movers land
    // next to their target.
    void TestMoverLanding()
    {
        CHECK(NamedMoverLanding("heroic leap") == MoverLanding::CurrentTarget);
        CHECK(NamedMoverLanding("cmbt grappling hook") == MoverLanding::CurrentTarget);
        CHECK(NamedMoverLanding("heroic leap loose") == MoverLanding::LooseEnemy);
        for (char const* name : { "cmbt blade rush", "cmbt pack blade rush", "shadowstep", "subt shadowstep",
                 "arms intervene", "subt death from above" })
            CHECK(NamedMoverLanding(name) == MoverLanding::CastTarget);
        // Away from the target: never judged by a landing.
        for (char const* name : { "blink back", "disengage", "hunter disengage" })
            CHECK(NamedMoverLanding(name) == MoverLanding::None);
        // Not movers.
        for (char const* name : { "", "charge", "reach melee", "heroic leap::x", "frozen orb" })
            CHECK(NamedMoverLanding(name) == MoverLanding::None);
        // Every named mover is either a gap closer or one of the three that land away.
        std::uint32_t away = 0;
        for (std::string_view const name : kNamedMovers)
            away += NamedMoverLanding(name) == MoverLanding::None;
        CHECK(away == 3);

        for (uint32_t id : { 204148u, 51690u, 8676u, 203963u, 204400u })
            CHECK(ScriptedMoverLandsAtTarget(id));
        CHECK(!ScriptedMoverLandsAtTarget(48020));     // Demonic Circle: Teleport goes to the circle
        CHECK(!ScriptedMoverLandsAtTarget(0));
        // Every scripted mover has a decision: all but the teleport home land at the target.
        std::uint32_t atTarget = 0;
        for (ScriptedMover const& mover : kScriptedMovers)
            atTarget += ScriptedMoverLandsAtTarget(mover.spellId);
        CHECK(atTarget == kScriptedMovers.size() - 1);

        // The near probe: the destination's reach + kLandingReachYd, never past the bot.
        CHECK(LandingNearProbeYd(12.f, 1.5f) == 1.5f + kLandingReachYd);
        CHECK(LandingNearProbeYd(2.f, 1.5f) == 2.f);
        CHECK(LandingNearProbeYd(0.f, 1.5f) == 0.f);

        // WP-F2 (STAGE-M4-R2-REPORT m-5, nit 4): where each one stops, kept with its row.
        CHECK(NamedMoverSpot("heroic leap") == LandingSpot::Center);
        CHECK(NamedMoverSpot("heroic leap loose") == LandingSpot::Center);
        CHECK(NamedMoverSpot("cmbt grappling hook") == LandingSpot::Center);
        CHECK(NamedMoverSpot("shadowstep") == LandingSpot::Behind);
        CHECK(NamedMoverSpot("subt shadowstep") == LandingSpot::Behind);
        CHECK(NamedMoverSpot("cmbt blade rush") == LandingSpot::Reach);
        CHECK(NamedMoverSpot("cmbt pack blade rush") == LandingSpot::Reach);
        CHECK(NamedMoverSpot("arms intervene") == LandingSpot::Reach);
        CHECK(NamedMoverSpot("subt death from above") == LandingSpot::CenterOrReach);
        for (std::string_view const name : kNamedMovers)
            CHECK((NamedMoverSpot(name) == LandingSpot::None) == (NamedMoverLanding(name) == MoverLanding::None));
        CHECK(NamedMoverSpot("reach melee") == LandingSpot::None);
        CHECK(ScriptedMoverSpot(204400) == LandingSpot::Center);    // Heroic Leap
        CHECK(ScriptedMoverSpot(203963) == LandingSpot::Center);    // Grappling Hook
        CHECK(ScriptedMoverSpot(8676) == LandingSpot::Behind);      // Ambush
        CHECK(ScriptedMoverSpot(204148) == LandingSpot::CenterOrReach);
        CHECK(ScriptedMoverSpot(51690) == LandingSpot::CenterOrReach);
        CHECK(ScriptedMoverSpot(48020) == LandingSpot::None);
        CHECK(ScriptedMoverSpot(0) == LandingSpot::None);
        // Spell effects: a charge stops at reach, a jump on the center, a spell with both is judged as a jump.
        CHECK(SpellEffectSpot(true, false) == LandingSpot::Reach);
        CHECK(SpellEffectSpot(false, true) == LandingSpot::Center);
        CHECK(SpellEffectSpot(true, true) == LandingSpot::Center);
        CHECK(SpellEffectSpot(false, false) == LandingSpot::None);
    }

    // WP-F2 (STAGE-M4-R2-REPORT M-1 fix 4): the chase clock and its valve.
    void TestChaseClock()
    {
        int64_t const stale = ChaseRunStaleMs(3000);
        CHECK(stale == 4000);
        CHECK(ChaseReleaseMsFor(3000, false) == 3000);
        CHECK(ChaseReleaseMsFor(3000, true) == 1500);        // R2 Q-1 ruling: a tank's valve is half the time
        CHECK(ChaseValveWindowMs(3000) == 3000 + kChaseValveSlackMs);
        ChaseClock c;
        CHECK(!ChaseRefusedNow(c, 1000));
        CHECK(!ChaseValveDue(c, 1000, 3000, stale));
        // Refused every AI tick (100 ms) from 1 s: due at 4 s, not before.
        for (int64_t t = 1000; t < 4000; t += 100)
        {
            NoteChaseRefusal(c, t, stale);
            CHECK(ChaseRefusedNow(c, t));
            CHECK(!ChaseValveDue(c, t, 3000, stale));
        }
        NoteChaseRefusal(c, 4000, stale);
        CHECK(ChaseValveDue(c, 4000, 3000, stale));
        CHECK(ChaseValveDue(c, 2500, 1500, stale));           // the tank's at 2.5 s
        // A refusal stands kChaseRefusedNowMs; the run survives a hold (HoldCapMs) between two refusals.
        CHECK(ChaseRefusedNow(c, 4000 + kChaseRefusedNowMs));
        CHECK(!ChaseRefusedNow(c, 4001 + kChaseRefusedNowMs));
        CHECK(ChaseValveDue(c, 4000 + stale, 3000, stale));
        CHECK(!ChaseValveDue(c, 4001 + stale, 3000, stale));  // gone stale: no valve on an old run
        // In reach ends the run.
        ChaseInReach(c);
        CHECK(c.refusedSince == 0 && c.lastRefusedAt == 0);
        CHECK(!ChaseValveDue(c, 5000, 3000, stale));
        // A stale run starts again at the next refusal.
        NoteChaseRefusal(c, 10000, stale);
        NoteChaseRefusal(c, 20000, stale);
        CHECK(c.refusedSince == 20000);
        CHECK(!ChaseValveDue(c, 20000, 3000, stale));
        // A refusal after a hold (3.5 s apart, under the stale gap) keeps the run.
        NoteChaseRefusal(c, 23500, stale);
        CHECK(c.refusedSince == 20000);
        CHECK(ChaseValveDue(c, 23500, 3000, stale));
    }

    // WP-Q2: the waker's fast tick counts the wake's lead in, so the brain steps every FastTickMs on any map update
    // interval, not every FastTickMs + one update. WP-F2 (STAGE-M4-R2-REPORT m-2): the lead is measured.
    void TestFastTickDue()
    {
        // The sim (100 ms updates, FastTickMs 100): due right after the step that ran the brain, so the AI runs on
        // the next update, 100 ms after it. The M4 rule (lead 0) waited one update more: a step every 200 ms.
        CHECK(FastTickDue(0, 100, 100));
        CHECK(!FastTickDue(0, 0, 100));
        CHECK(FastTickDue(100, 0, 100));
        CHECK(!FastTickDue(80, 10, 100));
        CHECK(FastTickDue(90, 10, 100));
        // A lead longer than FastTickMs counts as FastTickMs; a negative one is 0.
        CHECK(FastTickDue(0, 2000, 100));
        CHECK(!FastTickDue(0, -50, 100));
        // FastTickMs 0: every update.
        CHECK(FastTickDue(0, 0, 0));
        // The lead estimate: the smallest measured, never past the waker's gap, 0 while none is known.
        CHECK(NextWakeLead(-1, 100) == 100);
        CHECK(NextWakeLead(100, 15) == 15);
        CHECK(NextWakeLead(15, 40) == 15);
        CHECK(NextWakeLead(15, -1) == 15);
        CHECK(WakeLeadFor(-1, 100) == 0);
        CHECK(WakeLeadFor(15, 60) == 15);
        CHECK(WakeLeadFor(100, 60) == 60);
        CHECK(WakeLeadFor(15, 0) == 0);
    }

    // The cadence of an engaged bot's brain, simulated: world ticks of `tick(i)` ms; the map's full update (and the
    // waker after it) on every `fullEvery`-th tick; a woken bot thinks on the next world tick (live: Map::Update ticks
    // players on the partial updates too) or, `simLike`, on the next full update only (the sim). `measured`: WP-F2,
    // the lead from the wake to the brain step; else WP-Q2, the full-update gap as the lead. Returns the shortest and
    // the longest time between two brain steps after the first few.
    struct Cadence
    {
        int64_t shortest = 1000000;
        int64_t longest = 0;
    };

    template <class TickFn>
    Cadence RunCadence(TickFn tick, int fullEvery, bool simLike, bool measured, int64_t fastTick = 100)
    {
        Cadence out;
        int64_t now = 0;
        int64_t lastBrain = 0;
        int64_t lastFull = -1;
        int64_t wakeAt = 0;
        int64_t lead = -1;
        bool woken = true;
        int brains = 0;
        for (int i = 1; i <= 4000; ++i)
        {
            now += tick(i);
            bool const full = i % fullEvery == 0;
            // The bot's update (before the waker in the same tick).
            if (woken && (!simLike || full))
            {
                if (brains > 3)
                {
                    out.shortest = std::min(out.shortest, now - lastBrain);
                    out.longest = std::max(out.longest, now - lastBrain);
                }
                ++brains;
                if (wakeAt != 0)
                    lead = NextWakeLead(lead, now - wakeAt);
                wakeAt = 0;
                lastBrain = now;
                woken = false;
            }
            if (!full)
                continue;
            int64_t const gap = lastFull >= 0 ? now - lastFull : 0;
            lastFull = now;
            int64_t const useLead = measured ? WakeLeadFor(lead, gap) : gap;
            if (!woken && FastTickDue(now - lastBrain, useLead, fastTick))
            {
                woken = true;
                if (wakeAt == 0)
                    wakeAt = now;
            }
        }
        return out;
    }

    void TestFastTickCadence()
    {
        // The sim: 100 ms full updates only. Every 100 ms, as WP-Q2 made it.
        Cadence const sim = RunCadence([](int) { return int64_t(100); }, 1, true, true);
        CHECK(sim.shortest == 100 && sim.longest == 100);
        // Live, a loaded realm (R2 m-2's table): 15 ms world ticks, a full update every 4 (60 ms), and 25 ms ticks
        // (100 ms). WP-Q2's rule thought every 60 ms on the first; the measured lead never under FastTickMs.
        Cadence const old15 = RunCadence([](int) { return int64_t(15); }, 4, false, false);
        Cadence const new15 = RunCadence([](int) { return int64_t(15); }, 4, false, true);
        Cadence const new25 = RunCadence([](int) { return int64_t(25); }, 4, false, true);
        std::printf("fast tick, 15 ms world ticks / 60 ms full updates: WP-Q2 %lld..%lld ms, WP-F2 %lld..%lld ms; "
            "25 ms ticks: WP-F2 %lld..%lld ms\n", static_cast<long long>(old15.shortest),
            static_cast<long long>(old15.longest), static_cast<long long>(new15.shortest),
            static_cast<long long>(new15.longest), static_cast<long long>(new25.shortest),
            static_cast<long long>(new25.longest));
        CHECK(old15.shortest < 100);
        CHECK(new15.shortest >= 100);
        CHECK(new15.longest <= 100 + 60 + 15);
        CHECK(new25.shortest >= 100);
        CHECK(new25.longest <= 100 + 100 + 25);
        // An unloaded live realm: 10 ms ticks, a full update every tick.
        Cadence const idle = RunCadence([](int) { return int64_t(10); }, 1, false, true);
        CHECK(idle.shortest >= 100 && idle.longest <= 110);
        // Mixed intervals: world ticks of 5 to 45 ms (a fixed pseudo-random mix), full updates every 1 to 4 ticks.
        // Never faster than FastTickMs, less a tick shorter than any seen before (the lead is a measurement: one
        // within 5 ms here), never slower than FastTickMs + the longest full-update gap + a tick.
        for (int fullEvery : { 1, 2, 3, 4 })
        {
            uint32_t state = 12345u + static_cast<uint32_t>(fullEvery);
            auto mixed = [&state](int)
            {
                state = state * 1664525u + 1013904223u;
                return static_cast<int64_t>(5 + (state >> 16) % 41);
            };
            Cadence const m = RunCadence(mixed, fullEvery, false, true);
            CHECK(m.shortest >= 95);
            CHECK(m.longest <= 100 + 45 * fullEvery + 45);
        }
        // The sim at other steps: every max(FastTickMs, step) (a step is one full update).
        for (int64_t step : { 25, 50, 75, 100, 150 })
        {
            Cadence const c = RunCadence([step](int) { return step; }, 1, true, true);
            int64_t const want = step >= 100 ? step : (100 + step - 1) / step * step;
            CHECK(c.shortest == want && c.longest == want);
        }
    }
}

int main()
{
    TestScriptedMoverTable();
    TestScriptedMoverConditions();
    TestNamedMovers();
    TestTurningSelfCasts();
    TestLookAwayRule();
    TestCastFacingStep();
    TestPullRequests();
    TestForceLeakPasses();
    TestMoverLanding();
    TestChaseClock();
    TestFastTickDue();
    TestFastTickCadence();
    return Report("lock_rules_test");
}
