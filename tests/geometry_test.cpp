// Standalone tests for src/MechanicGeometry.h (STAGE-M1-HANDOFF §6.1). Build: tests/run.sh.

#include "check.h"
#include "MechanicGeometry.h"
#include <cmath>
#include <initializer_list>

using namespace BossMechanics;
using Geo::Pose;
using Geo::Vec3;

namespace
{
    Pose const kOrigin{ 0.f, 0.f, 0.f, 0.f };

    // A point at distance d along bearing (radians) from (0, 0).
    Vec3 Polar(float d, float bearing, float z = 0.f)
    {
        return { d * std::cos(bearing), d * std::sin(bearing), z };
    }

    void TestCircle()
    {
        Shape const s = Shape::Circle(8.f);
        for (float bearing : { 0.f, 0.7f, 2.5f, -1.9f })
        {
            CHECK(Geo::Contains(s, kOrigin, Polar(7.5f, bearing)));
            CHECK(Geo::Contains(s, kOrigin, Polar(7.99f, bearing)));
            CHECK(Geo::Contains(s, kOrigin, Polar(8.f, bearing)));         // inclusive edge
            CHECK(!Geo::Contains(s, kOrigin, Polar(8.01f, bearing)));
            CHECK(!Geo::Contains(s, kOrigin, Polar(8.5f, bearing)));
        }
        CHECK(Geo::Contains(s, kOrigin, Vec3{ 0.f, 0.f, 0.f }));
        // Off-origin anchor.
        Pose const p{ 100.f, -50.f, 20.f, 1.f };
        CHECK(Geo::Contains(s, p, Vec3{ 107.5f, -50.f, 20.f }));
        CHECK(!Geo::Contains(s, p, Vec3{ 108.5f, -50.f, 20.f }));
        CHECK_NEAR(Geo::Bounds(s), 8.f, 1e-6);
    }

    void TestDonut()
    {
        Shape const s = Shape::Donut(4.f, 12.f);
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ 4.f, 0.f, 0.f }));          // inner edge exclusive
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ 0.f, 0.f, 0.f }));
        CHECK(!Geo::Contains(s, kOrigin, Polar(3.99f, 1.f)));
        CHECK(!Geo::Contains(s, kOrigin, Polar(3.5f, 1.f)));
        CHECK(Geo::Contains(s, kOrigin, Polar(4.01f, 1.f)));
        CHECK(Geo::Contains(s, kOrigin, Polar(4.5f, 1.f)));
        CHECK(Geo::Contains(s, kOrigin, Polar(11.5f, -2.f)));
        CHECK(Geo::Contains(s, kOrigin, Polar(11.99f, -2.f)));
        CHECK(Geo::Contains(s, kOrigin, Vec3{ 12.f, 0.f, 0.f }));          // outer edge inclusive
        CHECK(!Geo::Contains(s, kOrigin, Polar(12.01f, -2.f)));
        CHECK(!Geo::Contains(s, kOrigin, Polar(12.5f, -2.f)));
        CHECK_NEAR(Geo::Bounds(s), 12.f, 1e-6);
    }

    // Point at distance `along` on the cone's side ray, pushed `out` yards perpendicular (positive = outside).
    Vec3 ConeSide(float facing, float halfAngle, float along, float out)
    {
        float const side = facing + halfAngle;
        float const nx = -std::sin(side);   // left normal of the side ray: outside for the +halfAngle side
        float const ny = std::cos(side);
        return { along * std::cos(side) + nx * out, along * std::sin(side) + ny * out, 0.f };
    }

    void TestCone()
    {
        Shape const s = Shape::Cone(90.f, 20.f);
        float const half = Geo::DegToRad(45.f);
        // Tip.
        CHECK(Geo::Contains(s, kOrigin, Vec3{ 19.5f, 0.f, 0.f }));
        CHECK(Geo::Contains(s, kOrigin, Vec3{ 19.99f, 0.f, 0.f }));
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ 20.01f, 0.f, 0.f }));
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ 20.5f, 0.f, 0.f }));
        // Sides (both), 10 yd out along the edge.
        for (float sign : { 1.f, -1.f })
        {
            float const h = half * sign;
            CHECK(Geo::Contains(s, kOrigin, ConeSide(0.f, h, 10.f, -0.5f * sign)));
            CHECK(Geo::Contains(s, kOrigin, ConeSide(0.f, h, 10.f, -0.01f * sign)));
            CHECK(!Geo::Contains(s, kOrigin, ConeSide(0.f, h, 10.f, 0.01f * sign)));
            CHECK(!Geo::Contains(s, kOrigin, ConeSide(0.f, h, 10.f, 0.5f * sign)));
        }
        // Behind is out; apex is in.
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ -5.f, 0.f, 0.f }));
        CHECK(Geo::Contains(s, kOrigin, Vec3{ 0.f, 0.f, 0.f }));
        CHECK_NEAR(Geo::Bounds(s), 20.f, 1e-6);

        // Rear cone (offset 180): behind is in, front is out.
        Shape rear = Shape::Cone(90.f, 8.f);
        rear.facingOffsetDeg = 180.f;
        CHECK(Geo::Contains(rear, kOrigin, Vec3{ -7.5f, 0.f, 0.f }));
        CHECK(Geo::Contains(rear, kOrigin, Polar(7.f, Geo::kPi + half - 0.05f)));
        CHECK(!Geo::Contains(rear, kOrigin, Polar(7.f, Geo::kPi + half + 0.05f)));
        CHECK(!Geo::Contains(rear, kOrigin, Vec3{ 5.f, 0.f, 0.f }));
        CHECK(!Geo::Contains(rear, kOrigin, Vec3{ -8.5f, 0.f, 0.f }));

        // A unit anchor that turned: facing +y.
        Pose const turned{ 0.f, 0.f, 0.f, Geo::kPi / 2.f };
        CHECK(Geo::Contains(s, turned, Vec3{ 0.f, 15.f, 0.f }));
        CHECK(!Geo::Contains(s, turned, Vec3{ 15.f, 0.f, 0.f }));
        CHECK(Geo::Contains(rear, turned, Vec3{ 0.f, -7.f, 0.f }));

        // Wrap-around: facing near +pi, points on both sides of the seam.
        Pose const west{ 0.f, 0.f, 0.f, Geo::kPi - 0.1f };
        CHECK(Geo::Contains(s, west, Polar(10.f, -Geo::kPi + 0.3f)));
        CHECK(Geo::Contains(s, west, Polar(10.f, Geo::kPi - 0.8f)));
        CHECK(!Geo::Contains(s, west, Polar(10.f, 0.f)));

        // Full circle cone.
        Shape all = Shape::Cone(360.f, 5.f);
        CHECK(Geo::Contains(all, kOrigin, Vec3{ -4.f, 0.f, 0.f }));
    }

    void TestLine()
    {
        Shape s = Shape::Line(30.f, 4.f);
        s.excludeStart = 5.f;
        // Sides at s = 15.
        for (float sign : { 1.f, -1.f })
        {
            CHECK(Geo::Contains(s, kOrigin, Vec3{ 15.f, sign * 1.5f, 0.f }));
            CHECK(Geo::Contains(s, kOrigin, Vec3{ 15.f, sign * 1.99f, 0.f }));
            CHECK(!Geo::Contains(s, kOrigin, Vec3{ 15.f, sign * 2.01f, 0.f }));
            CHECK(!Geo::Contains(s, kOrigin, Vec3{ 15.f, sign * 2.5f, 0.f }));
        }
        // Far end.
        CHECK(Geo::Contains(s, kOrigin, Vec3{ 29.5f, 0.f, 0.f }));
        CHECK(Geo::Contains(s, kOrigin, Vec3{ 29.99f, 0.f, 0.f }));
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ 30.01f, 0.f, 0.f }));
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ 30.5f, 0.f, 0.f }));
        // Excluded source end.
        CHECK(Geo::Contains(s, kOrigin, Vec3{ 5.5f, 0.f, 0.f }));
        CHECK(Geo::Contains(s, kOrigin, Vec3{ 5.01f, 0.f, 0.f }));
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ 4.99f, 0.f, 0.f }));
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ 4.5f, 0.f, 0.f }));
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ 0.f, 0.f, 0.f }));
        // Behind the source.
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ -10.f, 0.f, 0.f }));
        // Turned anchor (facing -y) and line coordinates.
        Pose const south{ 10.f, 10.f, 0.f, -Geo::kPi / 2.f };
        CHECK(Geo::Contains(s, south, Vec3{ 10.f, -10.f, 0.f }));
        CHECK(!Geo::Contains(s, south, Vec3{ 10.f, 25.f, 0.f }));
        Geo::LineCoords const c = Geo::ToLineCoords(s, south, Vec3{ 11.f, -10.f, 0.f });
        CHECK_NEAR(c.s, 20.f, 1e-4);
        CHECK_NEAR(c.t, 1.f, 1e-4);
        // Backwards line (offset 180).
        Shape back = Shape::Line(10.f, 2.f);
        back.facingOffsetDeg = 180.f;
        CHECK(Geo::Contains(back, kOrigin, Vec3{ -5.f, 0.f, 0.f }));
        CHECK(!Geo::Contains(back, kOrigin, Vec3{ 5.f, 0.f, 0.f }));
        CHECK_NEAR(Geo::Bounds(s), std::hypot(30.f, 2.f), 1e-5);
    }

    void TestZTolerance()
    {
        Shape s = Shape::Circle(5.f);
        CHECK(Geo::Contains(s, kOrigin, Vec3{ 1.f, 0.f, 5.9f }));
        CHECK(Geo::Contains(s, kOrigin, Vec3{ 1.f, 0.f, -5.9f }));
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ 1.f, 0.f, 6.1f }));
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ 1.f, 0.f, -6.1f }));
        s.zTolerance = 2.f;
        CHECK(Geo::Contains(s, kOrigin, Vec3{ 1.f, 0.f, 1.9f }));
        CHECK(!Geo::Contains(s, kOrigin, Vec3{ 1.f, 0.f, 2.1f }));
        Shape line = Shape::Line(10.f, 2.f);
        CHECK(!Geo::Contains(line, kOrigin, Vec3{ 5.f, 0.f, 10.f }));
    }

    void TestAngles()
    {
        CHECK_NEAR(Geo::WrapPi(3.f * Geo::kPi), Geo::kPi, 1e-5);
        CHECK_NEAR(Geo::WrapPi(-3.f * Geo::kPi / 2.f), Geo::kPi / 2.f, 1e-5);
        CHECK_NEAR(Geo::WrapPi(0.5f), 0.5f, 1e-6);
        CHECK_NEAR(Geo::Wrap2Pi(-Geo::kPi / 2.f), 3.f * Geo::kPi / 2.f, 1e-5);
        Pose const a = Geo::Advance(kOrigin, Geo::kPi / 2.f, 3.f);
        CHECK_NEAR(a.x, 0.f, 1e-5);
        CHECK_NEAR(a.y, 3.f, 1e-5);
    }
}

int main()
{
    TestCircle();
    TestDonut();
    TestCone();
    TestLine();
    TestZTolerance();
    TestAngles();
    return Report("geometry_test");
}
