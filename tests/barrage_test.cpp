// Standalone tests for src/MechanicBarrage.h (STAGE-M2-HANDOFF §3.11, WP-D): every `.mech barrage` shot threatens the
// member it was aimed at (the donut is the one deliberate exception: the member stands in its safe hole), and one seed
// gives one sequence. Build: tests/run.sh.

#include "check.h"
#include "MechanicBarrage.h"
#include <cmath>
#include <initializer_list>

using namespace BossMechanics;
using Geo::Vec3;

namespace
{
    // Members at assorted places, heights and a negative corner of the map.
    Vec3 const kMembers[] = { { 0.f, 0.f, 0.f }, { -1830.f, 5200.f, 91.5f }, { 412.25f, -3377.75f, -12.f },
        { 31.f, 17.f, 240.f } };

    void TestEveryShotThreatensItsMember()
    {
        for (Vec3 const& member : kMembers)
        {
            for (std::uint32_t seed = 1; seed <= 400; ++seed)
            {
                Barrage::Rng rng(seed);
                for (std::uint32_t k = 0; k < Barrage::kKindCount; ++k)
                {
                    Barrage::Kind const kind = static_cast<Barrage::Kind>(k);
                    Barrage::Shot const shot = Barrage::MakeShot(kind, member, rng);
                    Shape const shape = Barrage::ShapeOf(shot);
                    Geo::Pose const pose = shot.pose;
                    if (kind == Barrage::Kind::Donut)
                    {
                        // Centred 0-2 yd off the member, hole radius 3: the member is inside the hole, not the ring.
                        float const off = Geo::Dist2d(pose.x, pose.y, member.x, member.y);
                        CHECK(off >= Barrage::kDonutOffsetMin - 1e-3f && off <= Barrage::kDonutOffsetMax + 1e-3f);
                        CHECK(!Geo::Contains(shape, pose, member));
                        CHECK(off < shot.inner);
                        // ... and a point 5 yd from the centre is in the ring.
                        CHECK(Geo::Contains(shape, pose, { pose.x + 5.f, pose.y, pose.z }));
                    }
                    else
                    {
                        CHECK(Geo::Contains(shape, pose, member));
                    }
                }
            }
        }
    }

    void TestShotDimensions()
    {
        Barrage::Rng rng(7);
        for (int i = 0; i < 500; ++i)
        {
            Barrage::Shot const circle = Barrage::MakeShot(Barrage::Kind::Circle, kMembers[1], rng);
            CHECK(circle.size >= 3.f - 1e-4f && circle.size <= 6.f + 1e-4f);
            CHECK_NEAR(circle.pose.x, kMembers[1].x, 1e-6);
            CHECK_NEAR(circle.pose.y, kMembers[1].y, 1e-6);

            Barrage::Shot const donut = Barrage::MakeShot(Barrage::Kind::Donut, kMembers[1], rng);
            CHECK_NEAR(donut.inner, 3.f, 1e-6);
            CHECK_NEAR(donut.size, 8.f, 1e-6);

            Barrage::Shot const cone = Barrage::MakeShot(Barrage::Kind::Cone, kMembers[1], rng);
            CHECK(cone.angleDeg == 60.f || cone.angleDeg == 90.f);
            CHECK_NEAR(cone.size, 15.f, 1e-6);
            CHECK_NEAR(Geo::Dist2d(cone.pose.x, cone.pose.y, kMembers[1].x, kMembers[1].y), 8.f, 1e-3);
            // The apex faces the member: the member sits on the axis.
            Geo::LineCoords const onAxis = Geo::ToLineCoords(Shape::Line(15.f, 1.f), cone.pose, kMembers[1]);
            CHECK_NEAR(onAxis.s, 8.f, 1e-2);
            CHECK_NEAR(onAxis.t, 0.f, 1e-2);

            Barrage::Shot const line = Barrage::MakeShot(Barrage::Kind::Line, kMembers[1], rng);
            CHECK_NEAR(line.size, 24.f, 1e-6);
            CHECK_NEAR(line.width, 4.f, 1e-6);
            Geo::LineCoords const lc = Geo::ToLineCoords(Barrage::ShapeOf(line), line.pose, kMembers[1]);
            CHECK_NEAR(lc.s, 12.f, 1e-2);
            CHECK_NEAR(lc.t, 0.f, 1e-2);
            CHECK(line.pose.o >= 0.f && line.pose.o < Geo::kTwoPi);

            Barrage::Shot const projectile = Barrage::MakeShot(Barrage::Kind::Projectile, kMembers[1], rng);
            CHECK_NEAR(projectile.width, 3.f, 1e-6);
            CHECK_NEAR(projectile.speed, 12.f, 1e-6);
            CHECK_NEAR(projectile.size, 24.f, 1e-6);
        }
    }

    void TestSeedRepeats()
    {
        Barrage::Rng a(1234);
        Barrage::Rng b(1234);
        Barrage::Rng c(1235);
        bool differs = false;
        for (int i = 0; i < 50; ++i)
        {
            Barrage::Kind const kind = static_cast<Barrage::Kind>(i % Barrage::kKindCount);
            Barrage::Shot const sa = Barrage::MakeShot(kind, kMembers[2], a);
            Barrage::Shot const sb = Barrage::MakeShot(kind, kMembers[2], b);
            Barrage::Shot const sc = Barrage::MakeShot(kind, kMembers[2], c);
            CHECK(sa.pose.x == sb.pose.x && sa.pose.y == sb.pose.y && sa.pose.o == sb.pose.o && sa.size == sb.size);
            if (sa.pose.x != sc.pose.x || sa.size != sc.size)
                differs = true;
        }
        CHECK(differs);
        CHECK(Barrage::Rng(5).Index(0) == 0);
    }

    void TestTags()
    {
        CHECK(Barrage::TagOf(Barrage::Kind::Circle) == 1);
        CHECK(Barrage::TagOf(Barrage::Kind::Projectile) == 5);
    }
}

int main()
{
    TestEveryShotThreatensItsMember();
    TestShotDimensions();
    TestSeedRepeats();
    TestTags();
    return Report("barrage_test");
}
