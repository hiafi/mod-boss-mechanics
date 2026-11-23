/*
 * mod-boss-mechanics: the Gaze kind's pure rules (Scarlet Monastery build SM-BM, F13; Ironspine's heroic Glare, GY
 * IR6; shared doc "Look away", Deadmines addition 2).
 *
 * Mechanic::Gaze registers "do not look at the source": at the end of the warning everyone eligible within range of
 * the source who faces it is hit (the script's cost: damage, a rider such as a stun). A player faces the source when
 * the source lies inside the player's front arc (`arcDeg` wide, centered on the player's facing; the default 180 is
 * the front half, the client's own "in front" test). The judge reads the server's orientation of each player at the
 * resolve: a human turns (or walks) away, a bot turns to its look-away facing.
 *
 * Bots (MechanicBotView.cpp GazeKind, MechanicBotFacing.cpp): an eligible bot in range (plus kReachSlackYd) owes a
 * facing outside every live gaze's arc, widened by LookAway.MarginDeg, over the look window ([resolve - lead, resolve],
 * the whole warning by default, +- SafetyMarginMs). The facing is the one nearest the bot's victim that clears the
 * arc (ChooseFacing), so a melee bot whose boss stands away from the source keeps hitting it. The face action turns
 * the bot, the lock refuses what would turn it back (a facing cast or a swing at a target in the arc) and the waker
 * breaks a cast that would; the bot holds its spot over the window (a spline turns a walking unit along its path).
 * This is the carried-cone look-away (M3 decision §7.6) with the source's bearing as the thing to keep out of the arc.
 *
 * Pure header: no AzerothCore includes (tests/sm_shapes_test.cpp compiles it with the host compiler).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_GAZE_H
#define MOD_BOSS_MECHANICS_MECHANIC_GAZE_H

#include "MechanicGeometry.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace BossMechanics::Gaze
{
    using Ms = std::chrono::milliseconds;

    // ---- defaults (GazeSpec, MechanicRegistry.h) ---------------------------------------------------------------
    constexpr float kDefaultRangeYd = 40.f;
    constexpr float kMaxRangeYd = 95.f;                 // the resolve spell's area search (100 yd, less the slack)
    constexpr float kDefaultArcDeg = 180.f;             // the front half (the client's in-front test)
    constexpr float kMinArcDeg = 1.f;
    constexpr float kMaxArcDeg = 360.f;                 // 360: facing does not matter (everyone in range is hit)
    constexpr float kDefaultZToleranceYd = 10.f;        // as Hide: a room's floor, other floors kept out
    // A player this close to the source (on the plane) faces it whatever its facing: there is no away.
    constexpr float kOnSourceYd = 0.5f;
    // Bots: a bot this much beyond the range still owes the look (it may walk in before the resolve).
    constexpr float kReachSlackYd = 5.f;

    inline float ClampArcDeg(float arcDeg)
    {
        if (!std::isfinite(arcDeg))
            return kDefaultArcDeg;
        return std::clamp(arcDeg, kMinArcDeg, kMaxArcDeg);
    }

    // Half the front arc, radians.
    inline float HalfArcRad(float arcDeg)
    {
        return Geo::DegToRad(ClampArcDeg(arcDeg)) * 0.5f;
    }

    // The bearing from `from` to `to` on the plane (radians).
    inline float Bearing(Geo::Vec3 const& from, Geo::Vec3 const& to)
    {
        return std::atan2(to.y - from.y, to.x - from.x);
    }

    // The judge: a player at `player` facing `facing` (radians) faces the source at `source` when the source lies
    // inside the front arc (its edge counts as inside). On the source itself: faces it.
    inline bool Faces(Geo::Vec3 const& player, float facing, Geo::Vec3 const& source, float arcDeg)
    {
        if (Geo::Dist2d(player.x, player.y, source.x, source.y) < kOnSourceYd)
            return true;
        float const off = std::fabs(Geo::WrapPi(Bearing(player, source) - facing));
        return off <= HalfArcRad(arcDeg) + 1e-4f;
    }

    // How far (radians, 0 = clear) `facing` would have to turn for the source at `bearing` to leave its arc widened by
    // `marginDeg` on each side.
    inline float Overlap(float facing, float bearing, float arcDeg, float marginDeg)
    {
        float const need = HalfArcRad(arcDeg) + Geo::DegToRad(std::max(0.f, marginDeg));
        float const off = std::fabs(Geo::WrapPi(bearing - facing));
        return std::max(0.f, need - off);
    }

    // One gaze a bot must not look at: the bearing from the bot to the source and the gaze's arc.
    struct Duty
    {
        float bearing = 0.f;
        float arcDeg = kDefaultArcDeg;
    };

    // The facing nearest `preferred` (the bot's victim, or its own facing) that clears every duty's arc widened by
    // `marginDeg`. Candidates: `preferred`, each duty's two arc edges and its straight-away facing; the one clearing
    // the most duties wins, then the least total overlap (conflicting gazes: the least bad), then the nearest
    // `preferred`. No duty: `preferred`.
    inline float ChooseFacing(float preferred, std::vector<Duty> const& duties, float marginDeg)
    {
        if (duties.empty())
            return Geo::WrapPi(preferred);
        std::vector<float> candidates{ Geo::WrapPi(preferred) };
        for (Duty const& d : duties)
        {
            float const need = HalfArcRad(d.arcDeg) + Geo::DegToRad(std::max(0.f, marginDeg)) + 1e-3f;
            candidates.push_back(Geo::WrapPi(d.bearing + need));
            candidates.push_back(Geo::WrapPi(d.bearing - need));
            candidates.push_back(Geo::WrapPi(d.bearing + Geo::kPi));
        }
        float best = candidates.front();
        int bestClear = -1;
        float bestOverlap = 0.f;
        float bestTurn = 0.f;
        for (float c : candidates)
        {
            int clear = 0;
            float overlap = 0.f;
            for (Duty const& d : duties)
            {
                float const o = Overlap(c, d.bearing, d.arcDeg, marginDeg);
                if (o <= 0.f)
                    ++clear;
                overlap += o;
            }
            float const turn = std::fabs(Geo::WrapPi(c - preferred));
            bool const better = clear > bestClear
                || (clear == bestClear && overlap < bestOverlap - 1e-4f)
                || (clear == bestClear && std::fabs(overlap - bestOverlap) <= 1e-4f && turn < bestTurn - 1e-4f);
            if (better)
            {
                best = c;
                bestClear = clear;
                bestOverlap = overlap;
                bestTurn = turn;
            }
        }
        return best;
    }

    // The bots' look window of one gaze: [resolve - lead, resolve], never before its registration; lead 0 = the whole
    // warning (from the registration).
    struct Window
    {
        Ms from{ 0 };
        Ms to{ 0 };
    };

    inline Window LookWindow(Ms createdAt, Ms resolveAt, Ms lead)
    {
        Window w;
        w.to = resolveAt;
        w.from = lead <= Ms(0) ? createdAt : std::max(createdAt, resolveAt - lead);
        w.from = std::min(w.from, w.to);
        return w;
    }

    // The look is owed now: the window widened by `margin` on both sides (SafetyMarginMs).
    inline bool LookOwedAt(Window const& w, Ms now, Ms margin)
    {
        return now >= w.from - margin && now <= w.to + margin;
    }

    // A bot owes the look for a gaze whose source is `distYd` away on the plane (range plus kReachSlackYd).
    inline bool InLookRange(float distYd, float rangeYd)
    {
        return distYd <= rangeYd + kReachSlackYd;
    }

    // ---- Scarlet Monastery F-4a: a walk of the bot's own against its look windows ------------------------------
    // A walk no hazard asks for (the strand route to the melee ring, the route in past a refused chase) turns the bot
    // along its path, so it must not run while a look is owed: it does not start while a window is owed (LookOwedAt)
    // or opens within kOwnWalkLeadMs, and one already walking stops (holds) once a window is owed. A hazard's escape
    // is not one of these: it still runs, with the look-away facing applied when it stands.
    constexpr Ms kOwnWalkLeadMs{ 1000 };

    enum class OwnWalk : std::uint8_t
    {
        Go,         // no window owed or near: start it, or walk on
        Defer,      // not started: a window is owed now or opens within the lead; asked again after it
        Stop,       // walking: a window is owed now; hold for it
    };

    inline OwnWalk OwnWalkAt(std::vector<Window> const& windows, Ms now, Ms margin, bool walking,
        Ms lead = kOwnWalkLeadMs)
    {
        for (Window const& w : windows)
        {
            if (LookOwedAt(w, now, margin))
                return walking ? OwnWalk::Stop : OwnWalk::Defer;
            if (!walking && now < w.from - margin && now >= w.from - margin - lead)
                return OwnWalk::Defer;
        }
        return OwnWalk::Go;
    }
}

#endif
