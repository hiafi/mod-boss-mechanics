/*
 * mod-boss-mechanics: the Jump kind's pure rules (Stratholme build X3, handoff §3.4 gap (a), §7 #6; ST ruling 17).
 *
 * Mechanic::Jump registers a shape that sweeps at its resolve: every eligible player inside who is on the ground then
 * is hit (Timmy's Jump Rope: the rope trips everyone not in the air when a cast ends). A player in the air is spared:
 * a client's jump or fall (Unit::IsFalling, MOVEMENTFLAG_FALLING), or a bot's hop whose spline is still in flight.
 *
 * Bots cannot jump on their own: the brain (MechanicBotBrain.cpp, StepJump) makes a real hop in place, a parabolic
 * MotionMaster::MoveJump of HopMs timed to land LandAfterMs after the resolve, and notes it on the registry
 * (Registry::NoteHop). The fallback of ST ruling 17 (handoff §7 #6), for when the hop is not reliable in play:
 * BossMechanics.Bots.Jump.CountPlanned = 1 also counts a bot as in the air when its brain launched a hop for this jump
 * whose planned air time covers the resolve, even if the spline was cut or never started (ClearBy::Planned).
 *
 * Pure header: no AzerothCore includes (tests/jump_test.cpp compiles it with the host compiler).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_JUMP_H
#define MOD_BOSS_MECHANICS_MECHANIC_JUMP_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace BossMechanics::Jump
{
    using Ms = std::chrono::milliseconds;

    // Movement::gravity (MovementUtil.cpp): MoveJump's apex is speedZ^2 / (2 g).
    constexpr float kGravity = 19.29110527038574f;
    // The hop's horizontal step along the bot's facing: MoveJump needs one (a zero step launches no spline) and keeps
    // the facing (the spline faces its way). Small enough to read as a jump in place.
    constexpr float kHopYd = 0.15f;
    // MotionMaster::MoveJump refuses a horizontal speed at or below 0.1 yd/s; the step grows so it never gets there.
    constexpr float kMinSpeedXY = 0.2f;
    // A launch after the planned start still hops at least this long (it must be in the air at the resolve tick).
    constexpr Ms kMinHop{ 300 };

    // BossMechanics.Bots.Jump.HopMs / LandAfterMs (clamped in the config: hop 400-1200, landAfter 100 to hop - 200).
    struct HopParams
    {
        Ms hop{ 800 };          // the hop's length, launch to landing (a player's jump is about 0.8 s)
        Ms landAfter{ 250 };    // it lands this long after the resolve: in the air when the resolve tick comes
    };

    constexpr Ms HopAt(Ms resolveAt, HopParams const& p)
    {
        return resolveAt + p.landAfter - p.hop;
    }

    constexpr Ms LandAt(Ms resolveAt, HopParams const& p)
    {
        return resolveAt + p.landAfter;
    }

    enum class Step : std::uint8_t
    {
        Wait,       // before HopAt
        Launch,     // hop now, for `duration`
        Late        // the resolve has come: too late to hop for it
    };

    struct Decision
    {
        Step step = Step::Wait;
        Ms duration{ 0 };       // Launch: launch to landing
        Ms landAt{ 0 };         // Launch: absolute
    };

    // The brain's call at `now` (a brain step) for a jump that resolves at `resolveAt`. A step after HopAt (the bot's
    // brain ticks every FastTickMs, a cast break comes first) launches a shorter hop that still lands at LandAt, or
    // kMinHop from now when that is later.
    inline Decision Decide(Ms now, Ms resolveAt, HopParams const& p)
    {
        Decision d;
        if (now < HopAt(resolveAt, p))
            return d;
        if (now >= resolveAt)
        {
            d.step = Step::Late;
            return d;
        }
        d.step = Step::Launch;
        d.landAt = std::max(LandAt(resolveAt, p), now + kMinHop);
        d.duration = d.landAt - now;
        return d;
    }

    // A hop never rises higher than a player's jump (about 1.6 yd): a longer one floats instead.
    constexpr float kMaxApexYd = 1.6f;

    // MotionMaster::MoveJump's inputs for a hop of `duration`: the spline lasts step / speedXY (the parabola's height
    // is set apart from its length), the apex is speedZ^2 / (2 g) = g t^2 / 8 up to kMaxApexYd (a 0.8 s hop: about
    // 1.5 yd).
    struct Speeds
    {
        float stepYd = kHopYd;
        float xy = 0.f;
        float z = 0.f;
        float apexYd = 0.f;
    };

    inline Speeds SpeedsFor(Ms duration)
    {
        Speeds s;
        float const t = std::max(0.05f, static_cast<float>(duration.count()) / 1000.f);
        s.stepYd = std::max(kHopYd, kMinSpeedXY * t);
        s.xy = s.stepYd / t;
        s.z = std::min(kGravity * t / 2.f, std::sqrt(2.f * kGravity * kMaxApexYd));
        s.apexYd = s.z * s.z / (2.f * kGravity);
        return s;
    }

    // Stratholme F-2b (R-2-FW m-2): a bot walking when its hop launches (a live walk spline: an escape, a follow, a
    // chase) hops along its way instead of in place, so a run of skips costs it no ground: a player runs and jumps at
    // once. The hop goes toward the walk's next corner, `toCornerYd` away, as far as `runSpeed` carries it in
    // `duration`, clipped to the corner (MoveJump is a straight line; past the corner may be a wall). A corner closer
    // than kMinAlongYd hops in place (SpeedsFor). The apex is SpeedsFor's.
    constexpr float kMinAlongYd = 0.5f;

    inline Speeds SpeedsAlong(Ms duration, float runSpeed, float toCornerYd)
    {
        Speeds s = SpeedsFor(duration);
        float const t = std::max(0.05f, static_cast<float>(duration.count()) / 1000.f);
        float const step = std::min(std::max(0.f, runSpeed) * t, toCornerYd);
        if (!(step >= kMinAlongYd) || step <= s.stepYd)
            return s;
        s.stepYd = step;
        s.xy = step / t;
        return s;
    }

    // The ground a hop of `duration` with step `stepYd` gives up against walking it at `runSpeed` (ms): the time the
    // escape's clock and ceiling wait for it.
    inline Ms HopLostMs(Ms duration, float stepYd, float runSpeed)
    {
        if (runSpeed <= 0.f)
            return duration;
        Ms const walked{ static_cast<std::int64_t>(stepYd / runSpeed * 1000.f) };
        return std::max(Ms(0), duration - walked);
    }

    // How a player inside a Jump counts at its resolve.
    enum class ClearBy : std::uint8_t
    {
        None,       // on the ground: hit
        Falling,    // Unit::IsFalling: a client's jump or fall, a falling spline
        Hop,        // a bot's hop (Registry::NoteHop) in flight at the resolve
        Planned     // Jump.CountPlanned: the brain launched a hop for this jump whose air time covers the resolve
    };

    constexpr std::uint8_t kClearByCount = 4;

    // Stratholme F-2b (R-2-FW m-1): what the registry has seen of one noted hop, sampled on every map update until the
    // Jump resolves (Registry::SampleHops). The resolve tick can come late (a loaded world: a 300-400 ms map diff):
    // by then a hop that flew over the resolve has landed and the walk under it launched a new spline, so the bot's
    // spline alone no longer tells. The track keeps the air time the hop was seen (or known) to cover.
    // One map update: a hop replaced this close to its planned landing is taken as landed (SampleHop).
    constexpr Ms kSampleSlack{ 100 };

    struct HopTrack
    {
        bool ended = false;     // the hop's spline is no longer the unit's, or it is finalized
        Ms airTo{ 0 };          // the hop flew from its launch to here (seen, or its planned landing)
    };

    // One sample of a noted hop (`from` the launch, `to` the planned landing). `same`: the unit's spline is still the
    // hop's (same id); `finalized` / `passed`: that spline's state and time passed (MoveSpline::_Finalize sets it to
    // the whole length on landing; an interrupt, a death or a teleport, leaves it where it stopped). Not `same`: a
    // new spline replaced it since the last sample. It either landed (the walk under it resumed) or was cut (a stun,
    // a root, a knockback, death); `cut` is what the registry sees now (the unit dead, stunned, rooted, feared,
    // confused, in another controlled motion, falling). A replacement with no cut seen at or after the planned
    // landing (less `slack`, one map update) is taken as the landing; before it, as a cut where it was last seen.
    inline void SampleHop(HopTrack& t, Ms from, Ms to, bool same, bool finalized, Ms passed, bool cut, Ms now,
        Ms slack)
    {
        if (t.ended)
            return;
        if (same)
        {
            t.airTo = std::max(t.airTo, from + passed);
            t.ended = finalized;
            return;
        }
        t.ended = true;
        if (!cut && now + slack >= to)
            t.airTo = std::max(t.airTo, to);
    }

    struct Air
    {
        bool falling = false;           // Unit::IsFalling() (the flag test, not Player::IsFalling's Z test)
        bool hopNoted = false;          // the bot's brain noted a hop for this mechanic
        bool hopSplineLive = false;     // ...and the unit's spline is still that hop's (same id, not finalized)
        Ms hopFrom{ 0 }, hopTo{ 0 };    // ...its planned air time (launch, landing)
        Ms hopAirTo{ 0 };               // ...the air time the registry saw it cover (HopTrack::airTo; 0: none)
    };

    // `at` is the moment the sweep passes: the Jump's resolveAt (a late map update judges the resolve, not the late
    // tick), or the early detonation's now. A hop counts when its spline is still in flight, or when the air time
    // the registry saw (HopTrack) covers `at`. The fallback counts a planned air time covering `at`.
    inline ClearBy Clear(Air const& a, Ms at, bool countPlanned)
    {
        if (a.falling)
            return ClearBy::Falling;
        if (a.hopNoted && (a.hopSplineLive || (at >= a.hopFrom && at <= a.hopAirTo)))
            return ClearBy::Hop;
        if (countPlanned && a.hopNoted && at >= a.hopFrom && at <= a.hopTo)
            return ClearBy::Planned;
        return ClearBy::None;
    }

    constexpr char const* ClearByName(ClearBy c)
    {
        switch (c)
        {
            case ClearBy::None: return "ground";
            case ClearBy::Falling: return "falling";
            case ClearBy::Hop: return "hop";
            case ClearBy::Planned: return "planned";
        }
        return "?";
    }
}

#endif
