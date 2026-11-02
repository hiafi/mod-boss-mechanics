/*
 * mod-boss-mechanics: pure shape geometry (STAGE-M1-HANDOFF §3.4).
 *
 * No AzerothCore includes: the resolve filter, the registry, the visual pick and the bot solver (M2) all call these
 * functions, and tests/geometry_test.cpp compiles them with the host compiler. Point-in-shape works in 2D on (x, y)
 * with |dz| <= zTolerance; units are points; edges are inclusive except the donut's inner edge.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_GEOMETRY_H
#define MOD_BOSS_MECHANICS_MECHANIC_GEOMETRY_H

#include <cmath>
#include <cstdint>

namespace BossMechanics
{
    enum class ShapeType : std::uint8_t
    {
        Circle,
        Donut,
        Cone,
        Line
    };

    // The hitbox. Sizes in yards, angles in degrees. Facing comes from the anchor pose (radians) plus facingOffsetDeg.
    struct Shape
    {
        ShapeType type = ShapeType::Circle;
        float radius = 0.f;                 // Circle: radius; Donut: outer radius
        float inner = 0.f;                  // Donut: inner radius (exclusive)
        float length = 0.f;                 // Cone: length; Line: length
        float angleDeg = 0.f;               // Cone: full opening angle
        float width = 0.f;                  // Line: full width
        float excludeStart = 0.f;           // Line: the first excludeStart yards from the source are safe
        float facingOffsetDeg = 0.f;        // Cone/Line: 180 = rear cone / backwards line
        float zTolerance = 6.f;             // |dz| allowed; keeps other floors out

        static Shape Circle(float r)
        {
            Shape s;
            s.type = ShapeType::Circle;
            s.radius = r;
            return s;
        }

        static Shape Donut(float innerRadius, float outerRadius)
        {
            Shape s;
            s.type = ShapeType::Donut;
            s.inner = innerRadius;
            s.radius = outerRadius;
            return s;
        }

        static Shape Cone(float angle, float len)
        {
            Shape s;
            s.type = ShapeType::Cone;
            s.angleDeg = angle;
            s.length = len;
            return s;
        }

        static Shape Line(float len, float w)
        {
            Shape s;
            s.type = ShapeType::Line;
            s.length = len;
            s.width = w;
            return s;
        }
    };
}

namespace BossMechanics::Geo
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 2.f * kPi;
    // Inclusive edges get this much slack against float rounding (yards / radians).
    constexpr float kEdgeEpsilon = 1e-3f;
    constexpr float kAngleEpsilon = 1e-4f;

    struct Vec3
    {
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
    };

    // An anchor's live pose: position and facing (radians, 0 = +x, counter-clockwise).
    struct Pose
    {
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        float o = 0.f;
    };

    inline float DegToRad(float deg)
    {
        return deg * kPi / 180.f;
    }

    inline float RadToDeg(float rad)
    {
        return rad * 180.f / kPi;
    }

    // Wraps an angle into (-pi, pi].
    inline float WrapPi(float a)
    {
        a = std::fmod(a, kTwoPi);
        if (a <= -kPi)
            a += kTwoPi;
        else if (a > kPi)
            a -= kTwoPi;
        return a;
    }

    // Wraps an angle into [0, 2pi).
    inline float Wrap2Pi(float a)
    {
        a = std::fmod(a, kTwoPi);
        if (a < 0.f)
            a += kTwoPi;
        return a;
    }

    // The direction a cone or line points: anchor facing + the shape's offset.
    inline float ShapeFacing(Shape const& shape, Pose const& pose)
    {
        return pose.o + DegToRad(shape.facingOffsetDeg);
    }

    inline float Dist2d(float ax, float ay, float bx, float by)
    {
        return std::hypot(bx - ax, by - ay);
    }

    // Line coordinates of p: s = distance along the line from the source, t = lateral offset (left positive).
    struct LineCoords
    {
        float s = 0.f;
        float t = 0.f;
    };

    inline LineCoords ToLineCoords(Shape const& shape, Pose const& pose, Vec3 const& p)
    {
        float const f = ShapeFacing(shape, pose);
        float const ux = std::cos(f);
        float const uy = std::sin(f);
        float const dx = p.x - pose.x;
        float const dy = p.y - pose.y;
        return { dx * ux + dy * uy, -dx * uy + dy * ux };
    }

    // Is p inside the shape posed at `pose`? Edges inclusive; the donut's inner edge is exclusive.
    inline bool Contains(Shape const& shape, Pose const& pose, Vec3 const& p)
    {
        if (std::fabs(p.z - pose.z) > shape.zTolerance + kEdgeEpsilon)
            return false;

        float const dx = p.x - pose.x;
        float const dy = p.y - pose.y;
        float const d2 = dx * dx + dy * dy;

        switch (shape.type)
        {
            case ShapeType::Circle:
            {
                float const r = shape.radius + kEdgeEpsilon;
                return d2 <= r * r;
            }
            case ShapeType::Donut:
            {
                float const r = shape.radius + kEdgeEpsilon;
                return d2 > shape.inner * shape.inner && d2 <= r * r;
            }
            case ShapeType::Cone:
            {
                float const r = shape.length + kEdgeEpsilon;
                if (d2 > r * r)
                    return false;
                if (shape.angleDeg >= 360.f || d2 < 1e-8f)
                    return true;    // full circle, or the apex itself
                float const bearing = std::atan2(dy, dx);
                float const delta = std::fabs(WrapPi(bearing - ShapeFacing(shape, pose)));
                return delta <= DegToRad(shape.angleDeg) * 0.5f + kAngleEpsilon;
            }
            case ShapeType::Line:
            {
                LineCoords const c = ToLineCoords(shape, pose, p);
                return c.s >= shape.excludeStart - kEdgeEpsilon && c.s <= shape.length + kEdgeEpsilon
                    && std::fabs(c.t) <= shape.width * 0.5f + kEdgeEpsilon;
            }
        }
        return false;
    }

    // Radius around the anchor that contains the whole shape (the resolve spell's search radius before slack).
    inline float Bounds(Shape const& shape)
    {
        switch (shape.type)
        {
            case ShapeType::Circle:
            case ShapeType::Donut:
                return shape.radius;
            case ShapeType::Cone:
                return shape.length;
            case ShapeType::Line:
                return std::hypot(shape.length, shape.width * 0.5f);
        }
        return 0.f;
    }

    // A point at `forward` yards along `facing` from `pose` (z unchanged).
    inline Pose Advance(Pose const& pose, float facing, float forward)
    {
        return { pose.x + std::cos(facing) * forward, pose.y + std::sin(facing) * forward, pose.z, facing };
    }
}

#endif
