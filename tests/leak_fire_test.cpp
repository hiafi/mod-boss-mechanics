// Standalone tests that the M4 leak detectors (src/MechanicLeaks.h) fire on the inputs real leaks give them
// (dungeon-mechanics M4-SWEEP G8, WP-X2): a foreign move, a forbidden cast and a turn under look-away, each fed the way
// the waker (src/MechanicBotWaker.cpp ObserveLeaks) builds its Sample, in both update orders the detectors meet:
// - live: the bot's AI step runs in Map::Update's player loop (playerbots' OnPlayerAfterUpdate) and the waker samples
//   right after it, in the same update;
// - sim (mod-dpssim SimDaemon): Map::Update (Unit::Update, maybe an AI step, then the waker), then one more AI step
//   per bot; what that step does is sampled by the next update, after Unit::Update moved splines and cast timers on.
// Also the BossMechanics.Bots.ForceLeaks loop (src/MechanicLockRules.h ForceLeakPasses) against the detectors.
// Build: tests/run.sh.

#include "check.h"
#include "MechanicLeaks.h"
#include "MechanicLockRules.h"
#include <chrono>
#include <cmath>
#include <cstdint>

using namespace BossMechanics;
using Leaks::Lock;
using Leaks::Sample;
using Leaks::Tracker;
using Leaks::Verdict;
using Ms = std::chrono::milliseconds;

namespace
{
    constexpr Ms kStep{ 100 };      // the sim's step (and a 10 Hz live update)
    constexpr float kRun = 7.f;     // run speed, yd/s

    // The bot as the waker sees it: position, facing, the spline in flight, the cast in progress.
    struct Bot
    {
        float x = 0.f, y = 0.f, orientation = 0.f;
        bool spline = false;
        std::uint32_t splineId = 0;
        float fromX = 0.f, fromY = 0.f, toX = 0.f, toY = 0.f;
        bool controlled = false;
        bool casting = false;
        std::uintptr_t castKey = 0;
        std::uint32_t castSpell = 0;
        Ms castLeft{ 0 };

        // MotionMaster::MovePoint (playerbots MovementAction::MoveTo): the spline launches at once.
        void LaunchSpline(std::uint32_t id, float tx, float ty, bool isControlled = false)
        {
            spline = true;
            splineId = id;
            fromX = x;
            fromY = y;
            toX = tx;
            toY = ty;
            controlled = isControlled;
        }

        // Unit::Update: the spline moves the bot `speed * dt` along (finalized at its end), cast timers run down.
        void UnitUpdate(Ms dt, float speed = kRun)
        {
            if (spline)
            {
                float const dx = toX - x;
                float const dy = toY - y;
                float const left = std::sqrt(dx * dx + dy * dy);
                float const step = speed * static_cast<float>(dt.count()) / 1000.f;
                if (step >= left)
                {
                    x = toX;
                    y = toY;
                    spline = false;
                    controlled = false;
                }
                else
                {
                    x += dx / left * step;
                    y += dy / left * step;
                    orientation = std::atan2(dy, dx);   // a spline turns the bot along its path
                }
            }
            if (casting)
            {
                castLeft -= dt;
                if (castLeft <= Ms(0))
                    casting = false;
            }
        }

        // The hold action: StopMoving + MotionMaster::Clear (the stop spline is finalized at once).
        void Stop()
        {
            spline = false;
            controlled = false;
        }

        void StartCast(std::uintptr_t key, std::uint32_t spellId, Ms length)
        {
            casting = true;
            castKey = key;
            castSpell = spellId;
            castLeft = length;
        }
    };

    // The lock state the waker samples with (the mode, leaveBy, the look-away window).
    struct LockState
    {
        Lock lock = Lock::None;
        bool lookAway = false;
        Ms leaveBy = Solver::kForever;
        Ms margin{ 300 };
    };

    // ObserveLeaks' Sample (MechanicBotWaker.cpp), with no escape target and no send in this update.
    Sample SampleOf(Bot const& b, Ms now, LockState const& l)
    {
        Sample s;
        s.now = now;
        s.lock = l.lock;
        s.lookAway = l.lookAway;
        s.leaveBy = l.leaveBy;
        s.marginMs = l.margin;
        if (b.spline)
        {
            s.spline = true;
            s.splineId = b.splineId;
            float const dx = b.toX - b.fromX;
            float const dy = b.toY - b.fromY;
            s.splineMoves = std::sqrt(dx * dx + dy * dy) >= Leaks::kFacingSplineYd;
            s.destX = b.toX;
            s.destY = b.toY;
        }
        s.controlled = b.controlled;
        s.x = b.x;
        s.y = b.y;
        s.orientation = b.orientation;
        s.speed = kRun;
        if (b.casting)
        {
            s.casting = true;
            s.castKey = b.castKey;
            s.castSpell = b.castSpell;
            s.castRemaining = b.castLeft;
        }
        return s;
    }

    // The waker's counting (ObserveLeaks after Observe).
    struct Counts
    {
        std::uint32_t moves = 0, casts = 0, turns = 0;
        std::uint32_t lastMoveSpell = 0, lastCastSpell = 0;

        Verdict Add(Verdict v)
        {
            if (v.move)
            {
                ++moves;
                lastMoveSpell = v.moveSpell;
            }
            if (v.cast)
            {
                ++casts;
                lastCastSpell = v.castSpell;
            }
            if (v.turn)
                ++turns;
            return v;
        }
    };

    Ms const kT0{ 500000 };

    // ------------------------------------------------------------------------------------------------------------
    // A foreign move
    // ------------------------------------------------------------------------------------------------------------

    void TestForeignMove()
    {
        LockState const hold{ Lock::Hold };
        // Live: in Hold a foreign movement action (say stock "reach melee" let through) launches a MovePoint spline in
        // the bot's AI step; the waker samples right after it. The next update the hold action stops it. One leak,
        // no spell (a movement action).
        {
            Bot b;
            Tracker t;
            Counts c;
            Ms now = kT0;
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            now += kStep;
            b.UnitUpdate(kStep);
            b.LaunchSpline(501, 10.f, 0.f);                 // AI step
            CHECK(c.Add(Leaks::Observe(t, SampleOf(b, now, hold))).move);
            CHECK(c.lastMoveSpell == 0);
            now += kStep;
            b.UnitUpdate(kStep);
            b.Stop();                                       // the hold action's AI step
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            now += kStep;
            b.UnitUpdate(kStep);
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            CHECK(c.moves == 1);
        }
        // Sim: the spline is launched by the extra AI step after Map::Update; the next update's Unit::Update moves the
        // bot along it, no in-map AI step runs (its react delay), and the waker sees the spline in flight.
        {
            Bot b;
            Tracker t;
            Counts c;
            Ms now = kT0;
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));   // waker of update k
            b.LaunchSpline(502, 10.f, 0.f);                     // the sim's extra AI step of update k
            now += kStep;
            b.UnitUpdate(kStep);                                // update k + 1
            CHECK(b.spline && b.x > 0.5f);
            CHECK(c.Add(Leaks::Observe(t, SampleOf(b, now, hold))).move);
            CHECK(c.moves == 1);
        }
        // Sim, the blind case: an in-map AI step in update k + 1 runs the hold action before the waker, so the foreign
        // spline is stopped before any sample sees it. The bot moved one step (0.7 yd) and nothing counts. Rare (the
        // in-map step runs only when the react delay ran out in the same update), and the forced-leak check retries.
        {
            Bot b;
            Tracker t;
            Counts c;
            Ms now = kT0;
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            b.LaunchSpline(503, 10.f, 0.f);
            now += kStep;
            b.UnitUpdate(kStep);
            b.Stop();                                           // in-map AI step: the hold action
            CHECK(!c.Add(Leaks::Observe(t, SampleOf(b, now, hold))).move);
            CHECK(c.moves == 0 && b.x > 0.5f);
        }
        // Charge (a movement spell let through): the cast hook records the spell before its effect launches the
        // charge spline in MOTION_SLOT_CONTROLLED; the leak names the spell.
        {
            Bot b;
            Tracker t;
            Counts c;
            Ms now = kT0;
            LockState const escape{ Lock::Escape };
            c.Add(Leaks::Observe(t, SampleOf(b, now, escape)));
            now += kStep;
            b.UnitUpdate(kStep);
            Leaks::SelfCast(t, 11578);                          // BotOnSpellCast (Spell::cast, before the effects)
            b.LaunchSpline(504, 20.f, 5.f, true);               // MoveCharge
            CHECK(c.Add(Leaks::Observe(t, SampleOf(b, now, escape))).move);
            CHECK(c.lastMoveSpell == 11578);
        }
        // STAGE-M4-R-REPORT nit 4: a boss's knockback (an EffectMovementGenerator in MOTION_SLOT_CONTROLLED) in the
        // update the bot cast an instant heal is not the bot's move; the bot's own Disengage (a mover) still is.
        for (bool ownMover : { false, true })
        {
            Bot b;
            Tracker t;
            Counts c;
            Ms now = kT0;
            LockState const escape{ Lock::Escape };
            c.Add(Leaks::Observe(t, SampleOf(b, now, escape)));
            now += kStep;
            b.UnitUpdate(kStep);
            Leaks::SelfCast(t, ownMover ? 781u : 2061u, ownMover);     // Disengage, or Flash Heal
            b.LaunchSpline(505, -8.f, 0.f, true);                       // MoveKnockbackFrom / MoveJump
            Sample s = SampleOf(b, now, escape);
            s.effectMotion = true;
            CHECK(c.Add(Leaks::Observe(t, s)).move == ownMover);
            CHECK(c.moves == (ownMover ? 1u : 0u));
        }
        // Blink in the sim order: cast in the extra AI step, sampled the next update. No spline, a jump far beyond a
        // run step, a cast of the bot's since the last check.
        {
            Bot b;
            Tracker t;
            Counts c;
            Ms now = kT0;
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            Leaks::SelfCast(t, 1953);
            b.x = 20.f;                                         // NearTeleportTo
            now += kStep;
            b.UnitUpdate(kStep);
            CHECK(c.Add(Leaks::Observe(t, SampleOf(b, now, hold))).move);
            CHECK(c.lastMoveSpell == 1953);
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // A forbidden cast
    // ------------------------------------------------------------------------------------------------------------

    void TestForbiddenCast()
    {
        // Live, Escape: a 3 s cast started in the AI step is in PREPARING at the sample: one leak with its spell, and
        // only one while it runs (the waker breaks it in the same update; the interrupt lands on the next AI step).
        {
            Bot b;
            Tracker t;
            Counts c;
            Ms now = kT0;
            LockState const escape{ Lock::Escape };
            c.Add(Leaks::Observe(t, SampleOf(b, now, escape)));
            now += kStep;
            b.UnitUpdate(kStep);
            b.StartCast(0xA000, 25306, Ms(3000));               // Fireball
            CHECK(c.Add(Leaks::Observe(t, SampleOf(b, now, escape))).cast);
            CHECK(c.lastCastSpell == 25306);
            now += kStep;
            b.UnitUpdate(kStep);
            c.Add(Leaks::Observe(t, SampleOf(b, now, escape)));
            CHECK(c.casts == 1);
        }
        // Sim, Hold: Army of the Dead (a 4 s channel) started by the extra AI step with leaveBy 1 s away. The next
        // update's sample sees it 100 ms in; it ends far after leaveBy - margin: a leak.
        {
            Bot b;
            Tracker t;
            Counts c;
            Ms now = kT0;
            LockState hold{ Lock::Hold };
            hold.leaveBy = kT0 + Ms(1000);
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            b.StartCast(0xB000, 42650, Ms(4000));
            now += kStep;
            b.UnitUpdate(kStep);
            CHECK(c.Add(Leaks::Observe(t, SampleOf(b, now, hold))).cast);
            CHECK(c.lastCastSpell == 42650);
        }
        // Control: the same Hold with a 500 ms cast that ends in time is no leak.
        {
            Bot b;
            Tracker t;
            Counts c;
            Ms now = kT0;
            LockState hold{ Lock::Hold };
            hold.leaveBy = kT0 + Ms(1000);
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            b.StartCast(0xB100, 2061, Ms(500));
            now += kStep;
            b.UnitUpdate(kStep);
            CHECK(!c.Add(Leaks::Observe(t, SampleOf(b, now, hold))).cast);
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // A turn under look-away
    // ------------------------------------------------------------------------------------------------------------

    void TestTurnUnderLookAway()
    {
        float const look = 1.f;
        LockState hold{ Lock::Hold };
        hold.lookAway = true;
        // Live: the face action holds the look-away facing (its token). A cast let through on a target behind the bot
        // turns it in the AI step (PlayerbotAI::CastSpell: ServerFacade::SetFacingTo, an immediate SetOrientation):
        // a leak. The face action turning it back on the next step is ours.
        {
            Bot b;
            b.orientation = look;
            Tracker t;
            Counts c;
            Ms now = kT0;
            Leaks::OwnFacing(t, look);
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            now += kStep;
            b.UnitUpdate(kStep);
            b.orientation = look + 3.f;                         // the foreign turn
            CHECK(c.Add(Leaks::Observe(t, SampleOf(b, now, hold))).turn);
            now += kStep;
            b.UnitUpdate(kStep);
            b.orientation = look;                               // the face action
            Leaks::OwnFacing(t, look);
            CHECK(!c.Add(Leaks::Observe(t, SampleOf(b, now, hold))).turn);
            CHECK(c.turns == 1);
        }
        // Sim: the turn by the extra AI step; the next update has no in-map AI step, the waker sees it.
        {
            Bot b;
            b.orientation = look;
            Tracker t;
            Counts c;
            Ms now = kT0;
            Leaks::OwnFacing(t, look);
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            b.orientation = look - 2.f;
            now += kStep;
            b.UnitUpdate(kStep);
            CHECK(c.Add(Leaks::Observe(t, SampleOf(b, now, hold))).turn);
        }
        // Sim, the blind case: an in-map AI step runs the face action before the waker, so the sample never sees the
        // foreign facing (it lasted one step). The forced-leak check retries until a turn survives to a sample.
        {
            Bot b;
            b.orientation = look;
            Tracker t;
            Counts c;
            Ms now = kT0;
            Leaks::OwnFacing(t, look);
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            b.orientation = look - 2.f;
            now += kStep;
            b.UnitUpdate(kStep);
            b.orientation = look;                               // in-map AI step: the face action
            CHECK(!c.Add(Leaks::Observe(t, SampleOf(b, now, hold))).turn);
        }
        // X6: the waker turns a bot in a PREPARING ground cast to the look-away facing (Apply + OwnFacing) after its
        // sample; the next sample sees our facing: no leak. Without the token it would be one.
        {
            Bot b;
            b.orientation = 0.f;                                // CastSpell(x, y, z) at its feet: world orientation 0
            b.StartCast(0xC000, 2120, Ms(2000));                // Flamestrike, started before the window
            Tracker t;
            Counts c;
            Ms now = kT0;
            LockState watch;
            c.Add(Leaks::Observe(t, SampleOf(b, now, watch)));
            now += kStep;
            b.UnitUpdate(kStep);
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));   // the window opens: Hold under look-away
            b.orientation = look;                               // the waker's turn
            Leaks::OwnFacing(t, look);
            now += kStep;
            b.UnitUpdate(kStep);
            CHECK(!c.Add(Leaks::Observe(t, SampleOf(b, now, hold))).turn);
            CHECK(c.turns == 0 && c.casts == 0);                // a cast begun before the lock is not a leak either
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // BossMechanics.Bots.ForceLeaks against the detectors
    // ------------------------------------------------------------------------------------------------------------

    // The lock's side of the check: a tagged block passes while the detector of its kind is short of the budget.
    bool LockLetsThrough(LockRules::LeakKind kind, Counts const& c, std::uint32_t budget)
    {
        std::uint32_t const counted = kind == LockRules::LeakKind::Move ? c.moves
            : kind == LockRules::LeakKind::Cast ? c.casts : c.turns;
        return LockRules::ForceLeakPasses(kind, counted, budget);
    }

    void TestForcedLeakLoop()
    {
        // Budget 1, Hold: every update the rotation offers a foreign move the lock would block. The first passes, the
        // detector counts it, and the lock closes again: exactly one leak however many updates follow.
        {
            Bot b;
            Tracker t;
            Counts c;
            LockState const hold{ Lock::Hold };
            Ms now = kT0;
            std::uint32_t passes = 0;
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            for (int i = 0; i < 20; ++i)
            {
                now += kStep;
                b.UnitUpdate(kStep);
                if (b.spline)
                    b.Stop();                                   // the hold action
                else if (LockLetsThrough(LockRules::LeakKind::Move, c, 1))
                {
                    ++passes;
                    b.LaunchSpline(600 + i, b.x + 10.f, 0.f);
                }
                c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            }
            CHECK(passes == 1);
            CHECK(c.moves == 1);
        }
        // Budget 2 for casts in Escape: two casts let through, two leaks, then none.
        {
            Bot b;
            Tracker t;
            Counts c;
            LockState const escape{ Lock::Escape };
            Ms now = kT0;
            std::uint32_t passes = 0;
            c.Add(Leaks::Observe(t, SampleOf(b, now, escape)));
            for (int i = 0; i < 20; ++i)
            {
                now += kStep;
                b.UnitUpdate(kStep);
                if (b.casting)
                    b.casting = false;                          // the waker's break lands on the next AI step
                else if (LockLetsThrough(LockRules::LeakKind::Cast, c, 2))
                {
                    ++passes;
                    b.StartCast(0xD000 + static_cast<std::uintptr_t>(i), 25306, Ms(3000));
                }
                c.Add(Leaks::Observe(t, SampleOf(b, now, escape)));
            }
            CHECK(passes == 2);
            CHECK(c.casts == 2);
        }
        // A blind detector (the in-map face action hides every turn) never closes the check: the lock keeps letting
        // turns through. This is what the sim check reads as "detector broken": leaks 0 with forced passes rising.
        {
            Bot b;
            float const look = 1.f;
            b.orientation = look;
            Tracker t;
            Counts c;
            LockState hold{ Lock::Hold };
            hold.lookAway = true;
            Ms now = kT0;
            Leaks::OwnFacing(t, look);
            std::uint32_t passes = 0;
            c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            for (int i = 0; i < 10; ++i)
            {
                if (LockLetsThrough(LockRules::LeakKind::Turn, c, 1))
                {
                    ++passes;
                    b.orientation = look + 2.f;
                }
                now += kStep;
                b.UnitUpdate(kStep);
                b.orientation = look;                           // corrected before every sample
                c.Add(Leaks::Observe(t, SampleOf(b, now, hold)));
            }
            CHECK(passes == 10);
            CHECK(c.turns == 0);
        }
        // Off (0) and untagged blocks never pass.
        Counts none;
        CHECK(!LockLetsThrough(LockRules::LeakKind::Move, none, 0));
        CHECK(!LockRules::ForceLeakPasses(LockRules::LeakKind::None, 0, 5));
    }
}

int main()
{
    TestForeignMove();
    TestForbiddenCast();
    TestTurnUnderLookAway();
    TestForcedLeakLoop();
    return Report("leak_fire_test");
}
