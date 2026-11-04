/*
 * mod-boss-mechanics: the lock's landing test for gap closers and the melee chase in Idle and Watch (dungeon-mechanics
 * STAGE-M4-Q2-REPORT F1/F2, reworked by WP-F2 for STAGE-M4-R2-REPORT M-1, m-4, m-5).
 *
 * Pure header: no AzerothCore or playerbots includes. MechanicBotLock resolves the destination unit (its center, combat
 * reach and facing) and the kind of landing (MechanicLockRules.h LandingSpot); tests/fight_test.cpp runs the same
 * verdicts with the host compiler.
 *
 * - A gap closer is judged where it really stops (LandingPoint): a charge at the target's reach on the bot's side, a
 *   jump at the unit on its center, Shadowstep behind it. Probing the center for a charge refused every Charge from
 *   behind a boss whose front cone has its apex on the center (R2 M-1 case 1).
 * - The melee chase is a walk, and Watch re-routes one whose end fails round the hazard, so the lock refuses it only
 *   when no point of the melee ring round the target is safe (RingSafe): the ring's radius is the bot's melee band, the
 *   point on the bot's side first, then kRingPoints round it.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_LANDING_H
#define MOD_BOSS_MECHANICS_MECHANIC_LANDING_H

#include "MechanicLockRules.h"
#include "MechanicSolver.h"
#include <cmath>
#include <cstdint>

namespace BossMechanics::Landing
{
    using Ms = std::chrono::milliseconds;

    // The destination unit as the lock reads it.
    struct Dest
    {
        Geo::Vec3 center;
        float combatReachYd = 0.f;
        float facing = 0.f;         // radians, 0 = +x, counter-clockwise (WorldObject::GetOrientation)
    };

    // The melee band's radius round the target: the melee range less this (BotRole::RoleBandOf's melee slack).
    inline constexpr float kMeleeBandSlackYd = 0.5f;
    // Points of the melee ring, the one on the bot's side included, evenly round the target.
    inline constexpr uint32_t kRingPoints = 16;

    // The spot a closer of kind `spot` (Reach or Behind) stops on; Center and CenterOrReach use the center (the
    // caller probes the Reach spot too for CenterOrReach). A bot on the center gives the center.
    inline Geo::Vec3 LandingPoint(LockRules::LandingSpot spot, Dest const& d, Geo::Vec3 const& bot)
    {
        Geo::Vec3 p = d.center;
        float const probe = d.combatReachYd + LockRules::kLandingReachYd;
        if (spot == LockRules::LandingSpot::Behind)
        {
            p.x -= std::cos(d.facing) * probe;
            p.y -= std::sin(d.facing) * probe;
            return p;
        }
        if (spot != LockRules::LandingSpot::Reach)
            return p;
        float const dx = bot.x - d.center.x;
        float const dy = bot.y - d.center.y;
        float const dist = std::sqrt(dx * dx + dy * dy);
        if (dist <= 0.01f)
            return p;
        float const r = LockRules::LandingNearProbeYd(dist, d.combatReachYd);
        p.x += dx / dist * r;
        p.y += dy / dist * r;
        return p;
    }

    // The landing of a closer of kind `spot` fails the bot's hazards over [now, to].
    inline bool LandingUnsafe(Solver::Input const& in, LockRules::LandingSpot spot, Dest const& d, Geo::Vec3 const& bot,
        Ms now, Ms to)
    {
        switch (spot)
        {
            case LockRules::LandingSpot::None:
                return false;
            case LockRules::LandingSpot::Center:
                return !Solver::PointSafe(in, d.center, now, to);
            case LockRules::LandingSpot::Reach:
            case LockRules::LandingSpot::Behind:
                return !Solver::PointSafe(in, LandingPoint(spot, d, bot), now, to);
            case LockRules::LandingSpot::CenterOrReach:
                return !Solver::PointSafe(in, d.center, now, to)
                    || !Solver::PointSafe(in, LandingPoint(LockRules::LandingSpot::Reach, d, bot), now, to);
        }
        return false;
    }

    // Point k (0 .. kRingPoints - 1) of the melee ring of `radiusYd` round `center`, kRingPoints evenly spaced: k = 0
    // on the bot's side, then alternately either way round from it (1, -1, 2, -2, ...; the last is the far side).
    inline Geo::Vec3 RingPoint(Geo::Vec3 const& center, Geo::Vec3 const& bot, float radiusYd, uint32_t k)
    {
        float const dx = bot.x - center.x;
        float const dy = bot.y - center.y;
        float const base = dx * dx + dy * dy > 0.0001f ? std::atan2(dy, dx) : 0.f;
        // 1, -1, 2, -2, ...: nearest to the bot's side first.
        float step = 0.f;
        if (k > 0)
        {
            int const j = static_cast<int>((k + 1) / 2);
            step = static_cast<float>(k % 2 == 1 ? j : -j);
        }
        float const a = base + step * 2.f * Geo::kPi / static_cast<float>(kRingPoints);
        return { center.x + std::cos(a) * radiusYd, center.y + std::sin(a) * radiusYd, center.z };
    }

    // Some point of the melee ring is safe over [now, to]; `safe` gets the first one found (nearest the bot's side).
    inline bool RingSafe(Solver::Input const& in, Geo::Vec3 const& center, Geo::Vec3 const& bot, float radiusYd, Ms now,
        Ms to, Geo::Vec3* safe = nullptr)
    {
        for (uint32_t k = 0; k < kRingPoints; ++k)
        {
            Geo::Vec3 const p = RingPoint(center, bot, radiusYd, k);
            if (Solver::PointSafe(in, p, now, to))
            {
                if (safe)
                    *safe = p;
                return true;
            }
        }
        return false;
    }
}

#endif
