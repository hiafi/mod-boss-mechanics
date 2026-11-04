/*
 * mod-boss-mechanics: pure geometry and dice of `.mech barrage` (STAGE-M2-HANDOFF §3.11).
 *
 * No AzerothCore includes: the command (MechanicCommand.cpp) turns a Shot into a `.mech test` registration, and
 * tests/barrage_test.cpp compiles this header with the host compiler to check that every shot threatens the member
 * it was aimed at. Positions are the member's pose at registration (x, y, z; o unused), angles in radians.
 *
 * Shapes, per shot (all constants below):
 *   circle      r in [3, 6], centred on the member.
 *   donut       3 / 8, centred 0-2 yd off the member: the member stands in the safe hole (the "stay in the hole"
 *               case; the solver must not run out of it). This holds while BossMechanics.Bots.SafetyMarginYd < 1
 *               (the hole edge is at least 1 yd away); with a margin of 1 or more a member 2 yd off-centre counts as
 *               inside and runs out through the ring, which is correct for that margin but no longer the hole case
 *               (STAGE-M2-REVIEW Q1). To make the donut hit the member instead, move the offset band to
 *               kDonutOffsetMin/Max = 3.5 / 7.5 (inside the ring).
 *   cone        60 or 90 degrees, length 15, apex 8 yd from the member, pointing at it.
 *   line        length 24, width 4, source 12 yd from the member, pointing through it (the member is at s = 12).
 *   projectile  the same line, width 3, speed 12.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BARRAGE_H
#define MOD_BOSS_MECHANICS_MECHANIC_BARRAGE_H

#include "MechanicGeometry.h"
#include <cmath>
#include <cstdint>
#include <random>

namespace BossMechanics::Barrage
{
    constexpr float kCircleRadiusMin = 3.f;
    constexpr float kCircleRadiusMax = 6.f;
    constexpr float kDonutInner = 3.f;
    constexpr float kDonutOuter = 8.f;
    constexpr float kDonutOffsetMin = 0.f;
    constexpr float kDonutOffsetMax = 2.f;
    constexpr float kConeAngleA = 60.f;
    constexpr float kConeAngleB = 90.f;
    constexpr float kConeLength = 15.f;
    constexpr float kConeApexDistance = 8.f;
    constexpr float kLineLength = 24.f;
    constexpr float kLineWidth = 4.f;
    constexpr float kLineSourceDistance = 12.f;
    constexpr float kProjectileWidth = 3.f;
    constexpr float kProjectileSpeed = 12.f;

    enum class Kind : std::uint8_t
    {
        Circle,
        Donut,
        Cone,
        Line,
        Projectile
    };
    constexpr std::uint32_t kKindCount = 5;

    // The `.mech test` shape name of a kind, and the ledger tag its hits carry (1 + kind).
    inline char const* ShapeName(Kind kind)
    {
        switch (kind)
        {
            case Kind::Circle:
                return "circle";
            case Kind::Donut:
                return "donut";
            case Kind::Cone:
                return "cone";
            case Kind::Line:
                return "line";
            case Kind::Projectile:
                return "projectile";
        }
        return "circle";
    }

    inline std::uint32_t TagOf(Kind kind)
    {
        return static_cast<std::uint32_t>(kind) + 1;
    }

    // The command's own dice: mt19937, so one seed gives one sequence on every platform. <random> is used on purpose
    // despite cpp-guidelines' "Random.h helpers" rule (STAGE-M2-REVIEW n2): Random.h has no seeded generator, and
    // `seed=` must replay the same barrage, also in the host test.
    class Rng
    {
    public:
        explicit Rng(std::uint32_t seed) : _seed(seed), _gen(seed) { }

        std::uint32_t Seed() const { return _seed; }

        float Float(float lo, float hi)
        {
            double const u = static_cast<double>(_gen()) / 4294967296.0;
            return static_cast<float>(lo + (hi - lo) * u);
        }

        std::uint32_t Index(std::uint32_t n) { return n ? _gen() % n : 0; }

    private:
        std::uint32_t _seed;
        std::mt19937 _gen;
    };

    // One planned shot. `pose` is the shape's anchor: the centre (circle, donut), the apex (cone) or the source
    // (line, projectile); `pose.o` is the facing of a cone or line. Sizes follow `.mech test`: `size` is the radius,
    // the outer radius, or the length.
    struct Shot
    {
        Kind kind = Kind::Circle;
        float size = 0.f;
        float inner = 0.f;
        float angleDeg = 0.f;
        float width = 0.f;
        float speed = 0.f;
        Geo::Pose pose;
    };

    inline Shot MakeShot(Kind kind, Geo::Vec3 const& member, Rng& rng)
    {
        Shot shot;
        shot.kind = kind;
        shot.pose = { member.x, member.y, member.z, 0.f };
        float const bearing = rng.Float(0.f, Geo::kTwoPi);
        float const cx = std::cos(bearing);
        float const cy = std::sin(bearing);
        switch (kind)
        {
            case Kind::Circle:
                shot.size = rng.Float(kCircleRadiusMin, kCircleRadiusMax);
                break;
            case Kind::Donut:
            {
                float const off = rng.Float(kDonutOffsetMin, kDonutOffsetMax);
                shot.inner = kDonutInner;
                shot.size = kDonutOuter;
                shot.pose.x += cx * off;
                shot.pose.y += cy * off;
                break;
            }
            case Kind::Cone:
                shot.angleDeg = rng.Float(0.f, 1.f) < 0.5f ? kConeAngleA : kConeAngleB;
                shot.size = kConeLength;
                shot.pose.x += cx * kConeApexDistance;
                shot.pose.y += cy * kConeApexDistance;
                shot.pose.o = Geo::Wrap2Pi(bearing + Geo::kPi);     // from the apex back at the member
                break;
            case Kind::Line:
            case Kind::Projectile:
                shot.size = kLineLength;
                shot.width = kind == Kind::Line ? kLineWidth : kProjectileWidth;
                shot.speed = kind == Kind::Projectile ? kProjectileSpeed : 0.f;
                shot.pose.x += cx * kLineSourceDistance;
                shot.pose.y += cy * kLineSourceDistance;
                shot.pose.o = Geo::Wrap2Pi(bearing + Geo::kPi);     // from the source through the member
                break;
        }
        return shot;
    }

    // The registered hitbox of a shot (the same fields RegisterTest passes to the Mechanic:: API).
    inline Shape ShapeOf(Shot const& shot)
    {
        switch (shot.kind)
        {
            case Kind::Circle:
                return Shape::Circle(shot.size);
            case Kind::Donut:
                return Shape::Donut(shot.inner, shot.size);
            case Kind::Cone:
                return Shape::Cone(shot.angleDeg, shot.size);
            case Kind::Line:
            case Kind::Projectile:
                return Shape::Line(shot.size, shot.width);
        }
        return Shape::Circle(0.f);
    }
}

#endif
