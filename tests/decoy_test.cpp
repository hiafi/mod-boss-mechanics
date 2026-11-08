// Standalone tests for Decoy() (Scholomance build WP-X, Jandice's heroic fake sigil and fake lane): which kinds can be
// one (Judge::DecoyFits), that its resolve keeps nobody (Judge::KeepResolveTarget), and the strip ExceptOne's free
// lane draws as a decoy (DecoyLaneStrip: it tiles with the real lanes and lies outside every real lane's hitbox).
// Build: tests/run.sh.

#include "check.h"
#include "MechanicJudge.h"
#include "MechanicMath.h"
#include <algorithm>
#include <cstdint>
#include <vector>

using namespace BossMechanics;

namespace
{
    constexpr float kEps = 1e-3f;

    Kind const kAllKinds[] = { Kind::Zone, Kind::Frontal, Kind::Spread, Kind::RunOut, Kind::Stack, Kind::Soak,
        Kind::Bait, Kind::HoldStill, Kind::Spot };

    void TestDecoyFits()
    {
        for (Kind k : kAllKinds)
        {
            bool const shape = k == Kind::Zone || k == Kind::Frontal;
            CHECK(Judge::DecoyFits(k, false) == shape);
            // A mine is never a decoy (its armed ring is a tell of its own; a body sets it off).
            CHECK(!Judge::DecoyFits(k, true));
        }
    }

    void TestDecoyKeepsNobody()
    {
        for (Kind k : kAllKinds)
        {
            Judge::Candidate c;     // alive, a player, inside, eligible, not exempt, not hit yet
            CHECK(Judge::KeepResolveTarget(k, c));
            c.decoy = true;
            CHECK(!Judge::KeepResolveTarget(k, c));
            // Whatever else is true of the unit: a pet, a GM, outside, ineligible.
            c.player = false;
            CHECK(!Judge::KeepResolveTarget(k, c));
            c.player = true;
            c.gameMaster = true;
            CHECK(!Judge::KeepResolveTarget(k, c));
        }
    }

    // ExceptOne's free lane as a decoy, for every layout CoverLaneOffsets makes: the real lanes' drawn strips
    // (CoverLaneDrawnStrip) and the decoy's tile [0, span] with no overlap (so the decoy looks like any other lane),
    // and the decoy lies inside the free lane and outside every real lane's hitbox (its ground is really safe).
    void TestDecoyLaneStrip()
    {
        std::uint32_t layouts = 0;
        for (float span : { 12.f, 18.f, 21.f, 25.f, 30.5f, 40.f, 41.f })
        {
            for (float width : { 4.f, 6.f, 7.f })
            {
                std::uint32_t free = 0;
                std::uint32_t const n = static_cast<std::uint32_t>(
                    CoverLaneOffsets(span, width, []() { return 0.f; }, free).size());
                CHECK(n >= 2);
                for (std::uint32_t f = 0; f < n; ++f)
                {
                    float const r = (static_cast<float>(f) + 0.5f) / static_cast<float>(n);
                    std::vector<float> const offsets = CoverLaneOffsets(span, width, [r]() { return r; }, free);
                    CHECK(free == f);
                    ++layouts;

                    LaneStrip const decoy = DecoyLaneStrip(offsets, width, free);
                    CHECK(decoy.width > 0.f);
                    CHECK(decoy.offset >= offsets[free] - kEps);
                    CHECK(decoy.offset + decoy.width <= offsets[free] + width + kEps);
                    for (std::uint32_t i = 0; i < n; ++i)
                    {
                        if (i == free)
                            continue;
                        // No real hitbox reaches into the decoy strip.
                        bool const apart = offsets[i] + width <= decoy.offset + kEps
                            || offsets[i] >= decoy.offset + decoy.width - kEps;
                        CHECK(apart);
                    }

                    // Drawn strips, sorted: contiguous from 0 to span.
                    std::vector<LaneStrip> drawn{ decoy };
                    for (std::uint32_t i = 0; i < n; ++i)
                        if (i != free)
                            drawn.push_back(CoverLaneDrawnStrip(offsets, width, i, free));
                    std::sort(drawn.begin(), drawn.end(), [](LaneStrip const& a, LaneStrip const& b)
                    {
                        return a.offset < b.offset;
                    });
                    float at = 0.f;
                    float total = 0.f;
                    for (LaneStrip const& s : drawn)
                    {
                        CHECK_NEAR(s.offset, at, kEps);
                        at = s.offset + s.width;
                        total += s.width;
                    }
                    CHECK_NEAR(at, span, kEps);
                    CHECK_NEAR(total, span, kEps);
                }
            }
        }
        CHECK(layouts > 30);

        // Jandice's aisle, 21 yd across in 6 yd lanes (HANDOFF §3.8): offsets 0, 6, 12, 15. A free third or last lane
        // leaves a 3 yd strip; a free first or second the whole lane.
        std::vector<float> const aisle = { 0.f, 6.f, 12.f, 15.f };
        CHECK_NEAR(DecoyLaneStrip(aisle, 6.f, 0).width, 6.f, kEps);
        CHECK_NEAR(DecoyLaneStrip(aisle, 6.f, 1).offset, 6.f, kEps);
        CHECK_NEAR(DecoyLaneStrip(aisle, 6.f, 2).width, 3.f, kEps);
        CHECK_NEAR(DecoyLaneStrip(aisle, 6.f, 3).offset, 18.f, kEps);
        CHECK_NEAR(DecoyLaneStrip(aisle, 6.f, 3).width, 3.f, kEps);
        // One lane: the decoy is the whole lane; an index past the end: nothing.
        CHECK_NEAR(DecoyLaneStrip({ 0.f }, 6.f, 0).width, 6.f, kEps);
        CHECK(DecoyLaneStrip(aisle, 6.f, 4).width == 0.f);
    }
}

int main()
{
    TestDecoyFits();
    TestDecoyKeepsNobody();
    TestDecoyLaneStrip();
    return Report("decoy_test");
}
