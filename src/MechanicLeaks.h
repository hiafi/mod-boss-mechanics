/*
 * mod-boss-mechanics: the lock leak detectors (STAGE-M4-HANDOFF §3.5, WP-C).
 *
 * Pure header (std + the pure solver): tests/bot_stats_test.cpp drives it with the host compiler. The waker
 * (MechanicBotWaker.cpp) fills one Sample per bot per map update from the bot's live state and calls Observe; the
 * verdicts become MechanicBotCounters::leakMoves, leakCasts and leakTurns.
 *
 * The detectors watch outcomes, not the lock's verdicts: whatever path made the bot move, cast or turn (a plain Action,
 * DoSpecificAction, a spell script, an item), it shows up here.
 *
 * - Move: while the lock holds (Escape, Hold, Freeze; Watch with a deferred escape counts as Escape, as in the lock),
 *   a spline with a real path (not a facing-only one) starts that is not ours, or the bot jumps further than it can
 *   run since the last check (a blink or a teleport, which has no spline). One count per spline id.
 * - Cast: while the lock holds, a cast or channel the bot was not casting at the last check, which the mode forbids:
 *   any one in Escape (a cast in PREPARING or a channel has a cast time), in Hold one that ends after leaveBy -
 *   SafetyMarginMs (Solver::CastAllowed against the leaveBy the lock judged with), never in Freeze. One count per cast.
 * - Turn: while a look-away window is open, the bot's orientation moved by more than 5 degrees since the last check,
 *   the bot stands (no moving spline now or at the last check) and it does not face the angle we last turned it to.
 *
 * Our own actions never count:
 * - our splines carry a token: OwnSpline (the move action records the id of the spline its MoveTo launched, with its
 *   end); a spline is also ours when our move action sent a move in this update, or, in Escape, when it ends within
 *   kOurTargetYd of our move target or of our last spline's end (a generator that re-launched our move). Ours is
 *   checked by id first, so the tail of our escape spline after the brain switched to Hold is ours too;
 * - a spline already in flight at the last unlocked check (stock follow or chase the hold action is about to stop)
 *   started before the lock: it is a motion stop, not a leak;
 * - external movers (knockback, fear, a boss pull: a generator in MOTION_SLOT_CONTROLLED) count only when the bot
 *   itself cast a spell since the last check (Charge, Heroic Leap and Disengage use that slot too); a jump with no cast
 *   of the bot's is external;
 * - our facings carry a token: OwnFacing (the face and hold actions record the angle they set); a turn that lands on
 *   it is ours.
 *
 * Spell ids: a leaked cast records its own id; a leaked move records the bot's last non-triggered cast since the last
 * check (SelfCast, from the cast hook), 0 when none (a movement action, not a spell).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_LEAKS_H
#define MOD_BOSS_MECHANICS_MECHANIC_LEAKS_H

#include "MechanicGeometry.h"
#include "MechanicSolver.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace BossMechanics::Leaks
{
    using Ms = std::chrono::milliseconds;

    // A turn is more than this (also the face action's tolerance, BotFacing::kFaceToleranceDeg).
    constexpr float kTurnToleranceDeg = 5.f;
    // A jump is further than 1.5 x the run distance since the last check plus this.
    constexpr float kJumpSlackYd = 2.f;
    constexpr float kJumpRunFactor = 1.5f;
    // A spline that ends this close to our move target or our last spline's end is ours (MovingToTarget's tolerance).
    constexpr float kOurTargetYd = 1.f;
    // A spline whose ends lie closer than this has no path (SetFacingTo launches one in place).
    constexpr float kFacingSplineYd = 0.1f;
    // Hold: a cast ending up to this past leaveBy - margin is not a leak (cast-time rounding between lock and check).
    constexpr Ms kCastSlack{ 50 };

    enum class Lock : std::uint8_t
    {
        None,       // Idle, Watch (no deferred escape), or released: nothing is locked
        Escape,
        Hold,
        Freeze
    };

    // One check of one bot, from its live state (the waker, after the bot's update and before the waker acts).
    struct Sample
    {
        Ms now{ 0 };
        Lock lock = Lock::None;
        bool lookAway = false;              // a look-away window is open (and the bot is not released)
        Ms leaveBy = Solver::kForever;      // Hold: the leaveBy the lock judged casts with
        Ms marginMs{ 0 };                   // SafetyMarginMs
        // The spline in flight (initialized and not finalized).
        bool spline = false;
        std::uint32_t splineId = 0;
        bool splineMoves = false;           // its ends lie kFacingSplineYd or more apart
        float destX = 0.f, destY = 0.f;     // its final destination
        // Escape: where our move action goes (BotBrain::MoveTarget), and whether it sent a move in this update.
        bool targetValid = false;
        float targetX = 0.f, targetY = 0.f;
        bool ourSendNow = false;
        bool controlled = false;            // a generator in MOTION_SLOT_CONTROLLED
        float x = 0.f, y = 0.f, orientation = 0.f;
        float speed = 7.f;                  // run speed, yd/s
        // A cast in PREPARING or a channel.
        bool casting = false;
        std::uintptr_t castKey = 0;         // the Spell object
        std::uint32_t castSpell = 0;
        Ms castRemaining{ 0 };              // Solver::kForever: an endless channel
    };

    // Per bot, between checks. Never reset by Sim::ResetBotStats (it holds references, not counts).
    struct Tracker
    {
        bool valid = false;
        Ms lastAt{ 0 };
        float x = 0.f, y = 0.f, orientation = 0.f;
        bool movingPrev = false;            // a moving spline was in flight at the last check
        std::uint32_t preLockSpline = 0;    // the spline in flight at the last unlocked check (0 = none)
        std::uint32_t countedSpline = 0;    // the last spline counted as a leak
        // Tokens of our own actions.
        std::uint32_t ownSpline = 0;
        bool ownDestValid = false;
        float ownDestX = 0.f, ownDestY = 0.f;
        bool ownFacingValid = false;
        float ownFacing = 0.f;
        // The bot's last non-triggered cast since the last check (0 = none), set by the cast hook.
        std::uint32_t selfCast = 0;
        // The cast seen at the last check.
        std::uintptr_t castKey = 0;
        std::uint32_t castSpell = 0;
    };

    struct Verdict
    {
        bool move = false;
        bool cast = false;
        bool turn = false;
        std::uint32_t moveSpell = 0;        // 0: no spell of the bot's since the last check
        std::uint32_t castSpell = 0;
    };

    // The move action launched spline `id` ending at (x, y).
    inline void OwnSpline(Tracker& t, std::uint32_t id, float x, float y)
    {
        t.ownSpline = id;
        t.ownDestValid = true;
        t.ownDestX = x;
        t.ownDestY = y;
    }

    // The face or hold action turned the bot to `angle`.
    inline void OwnFacing(Tracker& t, float angle)
    {
        t.ownFacingValid = true;
        t.ownFacing = angle;
    }

    // The bot cast `spellId` (not triggered).
    inline void SelfCast(Tracker& t, std::uint32_t spellId)
    {
        t.selfCast = spellId;
    }

    namespace Detail
    {
        inline bool Near(float ax, float ay, float bx, float by, float yd)
        {
            float const dx = ax - bx;
            float const dy = ay - by;
            return dx * dx + dy * dy <= yd * yd;
        }

        inline bool SameAngle(float a, float b)
        {
            return std::fabs(Geo::WrapPi(a - b)) <= Geo::DegToRad(kTurnToleranceDeg);
        }
    }

    inline Verdict Observe(Tracker& t, Sample const& s)
    {
        Verdict v;
        bool const locked = s.lock != Lock::None;

        // Whose spline is it?
        bool ours = false;
        if (s.spline)
        {
            ours = s.splineId == t.ownSpline || s.ourSendNow;
            if (!ours && s.lock == Lock::Escape)
                ours = (s.targetValid && Detail::Near(s.destX, s.destY, s.targetX, s.targetY, kOurTargetYd))
                    || (t.ownDestValid && Detail::Near(s.destX, s.destY, t.ownDestX, t.ownDestY, kOurTargetYd));
            if (ours && s.splineId != t.ownSpline)
                OwnSpline(t, s.splineId, s.destX, s.destY);
        }
        bool const moving = s.spline && s.splineMoves;

        // Moves: a foreign spline started under the lock (once per id), else a jump.
        if (locked && moving && !ours && s.splineId != t.preLockSpline && s.splineId != t.countedSpline
            && (!s.controlled || t.selfCast))
        {
            v.move = true;
            v.moveSpell = t.selfCast;
            t.countedSpline = s.splineId;
        }
        if (locked && !v.move && !moving && t.valid && t.selfCast)
        {
            float const dt = std::max(0.f, static_cast<float>((s.now - t.lastAt).count()) / 1000.f);
            float const run = std::max(0.f, s.speed) * dt * kJumpRunFactor + kJumpSlackYd;
            if (!Detail::Near(t.x, t.y, s.x, s.y, run))
            {
                v.move = true;
                v.moveSpell = t.selfCast;
            }
        }

        // Casts: once per cast, judged by the mode it started in.
        if (s.casting)
        {
            bool const first = s.castKey != t.castKey || s.castSpell != t.castSpell;
            t.castKey = s.castKey;
            t.castSpell = s.castSpell;
            if (first)
            {
                bool forbidden = false;
                if (s.lock == Lock::Escape)
                    forbidden = true;
                else if (s.lock == Lock::Hold)
                    forbidden = !Solver::CastAllowed(s.now, s.castRemaining, s.leaveBy + kCastSlack, s.marginMs);
                if (forbidden)
                {
                    v.cast = true;
                    v.castSpell = s.castSpell;
                }
            }
        }
        else
        {
            t.castKey = 0;
            t.castSpell = 0;
        }

        // Turns under look-away, standing only (a spline turns the bot along its path).
        if (s.lookAway && t.valid && !s.controlled && !moving && !t.movingPrev
            && !Detail::SameAngle(s.orientation, t.orientation)
            && !(t.ownFacingValid && Detail::SameAngle(s.orientation, t.ownFacing)))
            v.turn = true;

        // References for the next check.
        if (!locked)
            t.preLockSpline = s.spline ? s.splineId : 0;
        t.movingPrev = moving;
        t.x = s.x;
        t.y = s.y;
        t.orientation = s.orientation;
        t.lastAt = s.now;
        t.valid = true;
        t.selfCast = 0;
        return v;
    }

    // (spell id, count) pairs in first-seen order.
    inline void AddSpell(std::vector<std::pair<std::uint32_t, std::uint32_t>>& spells, std::uint32_t spellId)
    {
        if (!spellId)
            return;
        for (auto& [id, n] : spells)
            if (id == spellId)
            {
                ++n;
                return;
            }
        spells.emplace_back(spellId, 1);
    }
}

#endif
