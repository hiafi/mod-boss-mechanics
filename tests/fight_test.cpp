// The M4 sweep's player-kind gaps (dungeon-mechanics M4-SWEEP G1, G2, G3) and solve cost (G7), reproduced on the
// host with the brain model (tests/brain_model.h) and the view's rules for the player kinds, in the dpssim mechanics
// layout (SimMechanicsScenario: the boss on the origin, the tank 3 yd in front, the others on an arc at 12 yd, the
// healer at 18 yd, towards the anchor at 180 degrees). Each scenario runs with the fix off and on: off reproduces the
// sweep's failure, on clears it. STAGE-M4-Q-REPORT has the numbers.
// Build: tests/run.sh. `fight_test bench` prints the lane-set Fallback cost only.

#include "check.h"
#include "brain_model.h"
#include "MechanicAssign.h"
#include "MechanicJudge.h"
#include "MechanicViewRules.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace BossMechanics;
using BrainModel::Task;
using Geo::Pose;
using Geo::Vec3;
using Ms = std::chrono::milliseconds;
namespace VR = BossMechanics::ViewRules;

namespace
{
    constexpr float kMargin = 0.75f;
    Ms const kStart{ 100000 };

    struct Lcg
    {
        std::uint64_t state;

        float Next01()
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<float>(state >> 40) / 16777216.f;
        }

        float Range(float lo, float hi)
        {
            return lo + (hi - lo) * Next01();
        }
    };

    struct StraightOracle : Solver::PathOracle
    {
        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            out = { from, to };
            return true;
        }
    };

    Vec3 At(float r, float deg)
    {
        float const a = Geo::DegToRad(deg);
        return { r * std::cos(a), r * std::sin(a), 0.f };
    }

    float Dist(Vec3 const& a, Vec3 const& b)
    {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    // The sim's group of 5: tank, healer, the spec under test, two reference DPS. A ranged run's group is tank,
    // healer, ranged actor, Arms (melee), Fire Mage; a melee run's is tank, healer, melee actor, Fire Mage, Combat.
    // Melee chase the boss when out of reach (stock follow is stripped in the sim, the chase is not); ranged and the
    // healer stand. Role bands as BotRole::RoleBandOf: melee reach on the boss, the ranged band on the boss, the
    // healer's on the tank.
    struct Fight
    {
        BrainModel::World w;
        std::array<bool, 5> melee{ { true, false, false, false, false } };
        bool bands = true;
        Vec3 const boss{ 0.f, 0.f, 0.f };

        explicit Fight(bool rangedActor, float rangedYd = 12.f)
        {
            if (rangedActor)
                melee[3] = true;
            else
                melee[2] = melee[4] = true;
            std::array<float, 5> const bearing{ { 180.f, 150.f, 170.f, 190.f, 210.f } };
            for (std::size_t i = 0; i < 5; ++i)
            {
                BrainModel::Bot b;
                b.pos = i == 0 ? At(3.f, 180.f) : (melee[i] ? At(3.5f, bearing[i])
                    : At(i == 1 ? rangedYd + 6.f : rangedYd, bearing[i]));
                w.bots.push_back(b);
            }
            w.follow = [this](std::size_t i, Vec3& to)
            {
                float const d = Dist(w.bots[i].pos, boss);
                if (!melee[i] || d <= 4.3f)
                    return false;
                float const k = 3.5f / d;
                to = { w.bots[i].pos.x * k, w.bots[i].pos.y * k, 0.f };
                return true;
            };
        }

        void Band(std::size_t i, Solver::Input& in) const
        {
            if (!bands)
                return;
            in.role.present = true;
            in.role.anchor = i == 1 ? w.bots[0].pos : boss;
            in.role.minYd = melee[i] ? 0.f : 8.f;
            in.role.maxYd = melee[i] ? 3.8f : 28.f;
        }

        // Another bot's circle as the view places it (ViewRules::PlaceOtherCircle): at its planned spot once it
        // published a reservation for the mechanic, else (a bot about to plan for it) not at all, unless the rule is
        // off (M4 as built: always, at its plan or its current spot).
        bool OtherSpot(std::size_t j, bool published, bool rule, VR::Spot& out) const
        {
            out.p = w.SpotOf(j, out.slackYd);
            out.source = published ? VR::SpotSource::Reservation : VR::SpotSource::BotNow;
            return !rule || VR::PlaceOtherCircle(out);
        }

        void Run(Ms from, Ms to, std::function<void(Ms)> const& before, std::function<void(Ms)> const& after)
        {
            for (Ms now = from; now < to; now += w.cfg.step)
            {
                if (before)
                    before(now);
                w.Step(now);
                if (after)
                    after(now);
            }
        }
    };

    // ---- G1: Fenrus, Bait every 10 s ----------------------------------------------------------------------------

    // `bait 10 2000 rule=fewest aura=205008 r=6`: the picker (fewest stacks, never the tank, ties to the lowest guid)
    // runs out past everyone + 2 yd, everyone else keeps within 10 yd of Fenrus, out of the picker's 6 yd landing.
    // The leap goes to the farthest player; the judge (Registry::StepBait) posts MissedBait on every other player
    // beyond 10 yd and on the farthest one when it is not the pick. Everyone the landing hits gets a stack.
    std::uint32_t RunFenrus(bool rangedActor, bool edgeArcs, bool placeRule, std::uint32_t* picks = nullptr)
    {
        Fight f(rangedActor);
        f.w.cfg.params.regionEdgeArcs = edgeArcs;
        std::array<int, 5> stacks{};
        struct Bait
        {
            Ms resolve;
            std::size_t pick;
        };
        std::vector<Bait> baits;
        f.w.kinds = [&](std::size_t i, Solver::Input& in, Ms now, std::vector<Task>& tasks)
        {
            f.Band(i, in);
            for (std::size_t k = 0; k < baits.size(); ++k)
            {
                Bait const& b = baits[k];
                Ms const from = b.resolve;
                Ms const to = b.resolve + Ms(500);
                if (to + in.params.marginMs < now || b.resolve - Ms(2000) > now)
                    continue;
                Solver::Timing const timing = Solver::MakeInstant(from, to - from);
                std::uint32_t const id = 700 + static_cast<std::uint32_t>(k);
                if (i == b.pick)
                {
                    VR::Spot owner;
                    owner.p = f.boss;
                    in.hazards.push_back(VR::CarrierHazard(id, VR::BaitPickerRadius(10.f, {}), 10.f, owner, kMargin,
                        timing));
                    for (std::size_t j = 0; j < f.w.bots.size(); ++j)
                        if (j != i)
                        {
                            VR::Spot s;
                            s.p = f.w.SpotOf(j, s.slackYd);
                            in.hazards.push_back(VR::CarrierHazard(id, 6.f, 10.f, s, kMargin, timing));
                        }
                    in.far.present = true;
                    in.far.from = f.boss;
                    tasks.push_back({ id, to, true });
                    continue;
                }
                Solver::Region r;
                r.id = id;
                r.shape = Shape::Circle(10.f);
                r.poses = { Pose{} };
                r.marginYd = VR::RegionMarginYd(r.shape, kMargin);
                r.from = from;
                r.to = to;
                in.regions.push_back(r);
                // A picker holding its spot holds a claim (PublishHeldClaims); one in Escape solved for it.
                VR::Spot s;
                if (f.OtherSpot(b.pick, f.w.bots[b.pick].mode != BrainModel::Mode::Watch, placeRule, s))
                    in.hazards.push_back(VR::CarrierHazard(id, 6.f, 10.f, s, kMargin, timing));
                tasks.push_back({ id, to, true });
            }
        };
        std::uint32_t failures = 0;
        auto before = [&](Ms now)
        {
            if (baits.size() < 3 && now >= kStart + Ms(4000) + Ms(10000) * static_cast<int>(baits.size()))
            {
                std::size_t pick = 1;
                for (std::size_t j = 2; j < 5; ++j)
                    if (stacks[j] < stacks[pick])
                        pick = j;
                baits.push_back({ now + Ms(2000), pick });
                ++f.w.revision;
            }
        };
        auto after = [&](Ms now)
        {
            for (Bait const& b : baits)
            {
                if (b.resolve <= now || b.resolve > now + f.w.cfg.step)
                    continue;
                std::size_t far = 0;
                for (std::size_t j = 1; j < 5; ++j)
                    if (Dist(f.w.bots[j].pos, f.boss) > Dist(f.w.bots[far].pos, f.boss))
                        far = j;
                for (std::size_t j = 0; j < 5; ++j)
                    if (j != far && Dist(f.w.bots[j].pos, f.boss) > 10.f)
                        ++failures;
                if (far != b.pick)
                    ++failures;
                if (picks)
                    ++*picks;
                for (std::size_t j = 0; j < 5; ++j)
                    if (Dist(f.w.bots[j].pos, f.w.bots[far].pos) <= 6.f)
                        ++stacks[j];
            }
        };
        f.Run(kStart, kStart + Ms(40000), before, after);
        return failures;
    }

    void TestFenrus()
    {
        // M4 as built: the ranged run's healer (18 yd) has its way in blocked by the ranged picker's landing circle
        // and no candidate round it, so it stands; it is the farthest, the leap goes to it (MissedBait on it, and on
        // the picker beyond 10 yd): the sweep's pair at the second bait. The melee run is clean.
        std::uint32_t picks = 0;
        std::uint32_t const before = RunFenrus(true, false, false, &picks);
        std::printf("fenrus, ranged run, M4 as built: %u MissedBait in %u baits\n", before, picks);
        CHECK(picks == 3);
        CHECK(before >= 2);
        CHECK(RunFenrus(false, false, false) == 0);
        // The entry points round the region's edge alone fix it; the placement rule keeps it fixed.
        CHECK(RunFenrus(true, true, false) == 0);
        std::uint32_t const after = RunFenrus(true, true, true);
        std::printf("fenrus, ranged run, edge arcs + placement rule: %u MissedBait\n", after);
        CHECK(after == 0);
        CHECK(RunFenrus(false, true, true) == 0);
    }

    // The blocked entry on its own (the healer's first solve of the second bait): feasible spots exist round the
    // edge, the M4 candidates miss them and the Fallback stands still.
    void TestRegionEdgeArcs()
    {
        Solver::Input in;
        in.bot = At(18.f, 150.f);
        in.now = kStart;
        Ms const resolve = kStart + Ms(2000);
        Solver::Region r;
        r.id = 1;
        r.shape = Shape::Circle(10.f);
        r.poses = { Pose{} };
        r.marginYd = VR::RegionMarginYd(r.shape, kMargin);
        r.from = resolve;
        r.to = resolve + Ms(500);
        in.regions.push_back(r);
        VR::Spot pick;
        pick.p = At(13.f, 170.f);
        in.hazards.push_back(VR::CarrierHazard(1, 6.f, 10.f, pick, kMargin, Solver::MakeInstant(resolve, Ms(500))));
        StraightOracle oracle;
        Solver::Input off = in;
        off.params.regionEdgeArcs = false;
        Solver::Plan const a = Solver::Solve(off, oracle);
        CHECK(a.outcome == Solver::Outcome::Fallback);
        CHECK(a.violated == 1);
        Solver::Plan const b = Solver::Solve(in, oracle);
        CHECK(b.outcome == Solver::Outcome::Move);
        CHECK(Dist(b.target, {}) <= 10.f - kMargin + 1e-3f);
        CHECK(Dist(b.target, pick.p) > 6.f + kMargin);
        CHECK(Solver::TargetOk(in, b.target, b.arriveAt, kStart + in.params.horizon));
        // The arc points sit on the inset edge round the bot's bearing, and only for a bot outside.
        std::vector<Vec3> c = Solver::Candidates(in);
        std::uint32_t onEdge = 0;
        for (Vec3 const& p : c)
            onEdge += std::fabs(Dist(p, {}) - (10.f - kMargin - 0.5f)) < 0.01f;
        CHECK(onEdge >= 10);
        Solver::Input inside = in;
        inside.bot = At(4.f, 150.f);
        std::uint32_t insideEdge = 0;
        for (Vec3 const& p : Solver::Candidates(inside))
            insideEdge += std::fabs(Dist(p, {}) - (10.f - kMargin - 0.5f)) < 0.01f;
        CHECK(insideEdge < onEdge);
    }

    // ---- G3: Ras, a Stack on the boss while RunOut carriers leave -------------------------------------------------

    // t 0: `weaken 3` on p1, `stack 8 6000 anchor=target aura=205000:0:2` (0-2 stacks stack on the boss),
    // `runout 8 6000 self=205000:3` (3 stacks: nobody else within 8 yd of you); t 2 s: `weaken 3` on p2. Judged at
    // 6 s: MissedStack for a stacker beyond 8 yd of the boss, a RunOut hit for anyone within 8 yd of a carrier.
    std::uint32_t RunRas(bool rangedActor, std::size_t p1, std::size_t p2, bool placeRule)
    {
        Fight f(rangedActor, 9.f);
        std::array<int, 5> stacks{};
        std::array<bool, 5> published{};
        bool active = false;
        Ms const resolve = kStart + Ms(6000);
        Ms const to = resolve + Ms(500);
        Solver::Timing const timing = Solver::MakeInstant(resolve, to - resolve);
        f.w.kinds = [&](std::size_t i, Solver::Input& in, Ms now, std::vector<Task>& tasks)
        {
            f.Band(i, in);
            if (!active || to + in.params.marginMs < now)
                return;
            if (stacks[i] <= 2)
            {
                Solver::Region r;
                r.id = 1;
                r.shape = Shape::Circle(8.f);
                r.poses = { Pose{} };
                r.marginYd = VR::RegionMarginYd(r.shape, kMargin);
                r.from = resolve;
                r.to = to;
                in.regions.push_back(r);
                tasks.push_back({ 1, to, true });
            }
            if (stacks[i] >= 3)
            {
                std::vector<std::uint32_t> allies;
                for (std::size_t j = 0; j < 5; ++j)
                    if (j != i)
                    {
                        float slack = 0.f;
                        Vec3 const p = f.w.SpotOf(j, slack);
                        in.allies.push_back({ j + 1, p, slack });
                        allies.push_back(static_cast<std::uint32_t>(in.allies.size() - 1));
                    }
                in.carried.push_back(VR::CarriedCircle(2, 8.f, 10.f, kMargin, timing, std::move(allies)));
                tasks.push_back({ 2, to, true });
                return;
            }
            for (std::size_t j = 0; j < 5; ++j)
            {
                VR::Spot s;
                if (j != i && stacks[j] >= 3 && f.OtherSpot(j, published[j], placeRule, s))
                    in.hazards.push_back(VR::CarrierHazard(2, 8.f, 10.f, s, kMargin, timing));
            }
            tasks.push_back({ 2, to, false });
        };
        // Every solve of a task bot publishes its reservations (RunSolve).
        f.w.onSolve = [&](BrainModel::Bot const& b, Solver::Input const&, Solver::Plan const&,
            BrainModel::World::SolveAt)
        {
            std::size_t const i = static_cast<std::size_t>(&b - f.w.bots.data());
            if (stacks[i] >= 3)
                published[i] = true;
        };
        std::uint32_t failures = 0;
        auto before = [&](Ms now)
        {
            if (now == kStart)
            {
                stacks[p1] = 3;
                active = true;
                ++f.w.revision;
            }
            if (now == kStart + Ms(2000))
            {
                stacks[p2] = 3;
                ++f.w.revision;
                f.w.bots[p2].lastSolveAt = Ms(-100000);     // a stack change bypasses the rate limit
            }
        };
        auto after = [&](Ms now)
        {
            if (resolve <= now || resolve > now + f.w.cfg.step)
                return;
            for (std::size_t j = 0; j < 5; ++j)
            {
                if (stacks[j] <= 2 && Dist(f.w.bots[j].pos, f.boss) > 8.f)
                    ++failures;
                if (stacks[j] >= 3)
                    for (std::size_t k = 0; k < 5; ++k)
                        if (k != j && Dist(f.w.bots[k].pos, f.w.bots[j].pos) <= 8.f)
                            ++failures;
            }
        };
        f.Run(kStart - Ms(1000), kStart + Ms(8000), before, after);
        return failures;
    }

    void TestRas()
    {
        // Both layouts, every ordered pair of non-tank runners (the sweep's picks are random). As built, a carrier
        // still among the stackers pushes them out of the stack (its circle at its current spot is a Zone for them),
        // and it then sees them clear and stays: MissedStack. With the rule its circle waits for its own plan.
        std::uint32_t before = 0;
        std::uint32_t after = 0;
        std::uint32_t cases = 0;
        for (bool ranged : { true, false })
            for (std::size_t p1 = 1; p1 < 5; ++p1)
                for (std::size_t p2 = 1; p2 < 5; ++p2)
                {
                    if (p1 == p2)
                        continue;
                    ++cases;
                    before += RunRas(ranged, p1, p2, false);
                    after += RunRas(ranged, p1, p2, true);
                }
        std::printf("ras, %u pick pairs: %u failures as built, %u with the placement rule\n", cases, before, after);
        CHECK(before > 0);
        CHECK(after == 0);
    }

    // ---- G2: Alexei, Out of bounds with Assembly ----------------------------------------------------------------

    // `circle 7 4000 at=group persist=6000` (a 7 yd circle on the group's centroid, hot 4 s to 10 s) with
    // `stack 6 10000 cluster=2` (at 10 s everyone within 6 yd of two others): the members gather within 3 yd of a
    // sticky leader (the medoid until it claims). Random starts round the boss, melee in reach.
    std::uint32_t RunAlexei(Lcg* rng, bool rangedActor, bool regionRoute, std::uint32_t* routes = nullptr,
        std::uint32_t* hits = nullptr, std::vector<Vec3> const* starts = nullptr)
    {
        Fight f(rangedActor);
        f.w.cfg.regionRoute = regionRoute;
        for (std::size_t i = 0; i < 5; ++i)
            f.w.bots[i].pos = starts ? (*starts)[i]
                : At(f.melee[i] ? rng->Range(3.f, 4.5f) : rng->Range(6.f, 18.f), rng->Range(0.f, 360.f));
        Vec3 centroid{};
        for (BrainModel::Bot const& b : f.w.bots)
        {
            centroid.x += b.pos.x / 5.f;
            centroid.y += b.pos.y / 5.f;
        }
        Solver::Hazard oob;
        oob.id = 5;
        oob.shape = Shape::Circle(7.f);
        oob.poses = { Pose{ centroid.x, centroid.y, 0.f, 0.f } };
        oob.marginYd = kMargin;
        oob.timing = Solver::MakeInterval(kStart + Ms(4000), kStart + Ms(10000));
        f.w.AddHazard(oob);
        Ms const resolve = kStart + Ms(10000);
        int sticky = -1;
        auto leaderNow = [&f]()
        {
            std::vector<Assign::Point> pts;
            for (std::size_t j = 0; j < 5; ++j)
                pts.push_back({ j + 1, f.w.bots[j].pos.x, f.w.bots[j].pos.y });
            return static_cast<int>(Assign::ClusterLeader(pts, 0)) - 1;
        };
        f.w.kinds = [&](std::size_t i, Solver::Input& in, Ms now, std::vector<Task>& tasks)
        {
            f.Band(i, in);
            Ms const to = resolve + Ms(500);
            if (to + in.params.marginMs < now)
                return;
            int const leader = sticky >= 0 ? sticky : leaderNow();
            if (static_cast<int>(i) == leader)
            {
                tasks.push_back({ 3, to, true });
                return;
            }
            float slack = 0.f;
            Vec3 const spot = f.w.SpotOf(static_cast<std::size_t>(leader), slack);
            Shape const circle = Shape::Circle(VR::ClusterMemberRadius(6.f));
            Solver::Region r;
            r.id = 3;
            r.shape = circle;
            r.poses = { Pose{ spot.x, spot.y, 0.f, 0.f } };
            r.marginYd = VR::RegionMarginYd(circle, kMargin);
            r.from = resolve;
            r.to = to;
            in.regions.push_back(r);
            tasks.push_back({ 3, to, true });
        };
        // The leader's first solve puts its claim on the board (sticky from then on).
        f.w.onSolve = [&](BrainModel::Bot const& b, Solver::Input const&, Solver::Plan const&,
            BrainModel::World::SolveAt)
        {
            int const i = static_cast<int>(&b - f.w.bots.data());
            if (sticky < 0 && leaderNow() == i)
                sticky = i;
        };
        std::uint32_t misses = 0;
        f.Run(kStart, kStart + Ms(11000), nullptr, [&](Ms now)
        {
            if (resolve <= now || resolve > now + f.w.cfg.step)
                return;
            std::vector<Vec3> pts;
            for (BrainModel::Bot const& b : f.w.bots)
                pts.push_back(b.pos);
            misses += static_cast<std::uint32_t>(Judge::ClusterMisses(Shape::Circle(6.f), pts, 2).size());
        });
        BrainModel::Counters const c = f.w.Total();
        if (routes)
            *routes += c.regionRoutesFound;
        if (hits)
            *hits += c.hits;
        return misses;
    }

    void TestAlexei()
    {
        // As built, a member whose way to the cluster crosses the Out of bounds circle late gets a Fallback and
        // stands (the solver validates the oracle's path, not a way round a hazard): the sweep's one ranged
        // MissedStack per spec. The healer 18 yd east of the boss, the rest west of it: the circle lands on the
        // group, the cluster forms past its west edge, the healer's straight way crosses the circle once it is hot.
        // RegionRoute walks it round the circle.
        std::vector<Vec3> const split{ { -3.f, 0.f, 0.f }, { 18.f, 0.f, 0.f }, { -12.f, 2.f, 0.f },
            { -3.5f, 1.f, 0.f }, { -12.f, -2.f, 0.f } };
        std::uint32_t splitRoutes = 0;
        std::uint32_t const splitBefore = RunAlexei(nullptr, true, false, nullptr, nullptr, &split);
        std::uint32_t const splitAfter = RunAlexei(nullptr, true, true, &splitRoutes, nullptr, &split);
        std::printf("alexei, healer across the circle: %u misses as built, %u with region routes (%u routes)\n",
            splitBefore, splitAfter, splitRoutes);
        CHECK(splitBefore > 0);
        CHECK(splitAfter == 0);
        CHECK(splitRoutes > 0);

        // Random starts (about 1 in 100 misses as built, compiler dependent): none with region routes, and the
        // routes never walk anyone into the circle.
        constexpr std::uint32_t kRuns = 240;
        std::uint32_t before = 0;
        std::uint32_t after = 0;
        std::uint32_t routes = 0;
        std::uint32_t hitsBefore = 0;
        std::uint32_t hitsAfter = 0;
        Lcg a{ 7 };
        Lcg b{ 7 };
        for (std::uint32_t k = 0; k < kRuns; ++k)
        {
            before += RunAlexei(&a, k % 2 == 0, false, nullptr, &hitsBefore);
            after += RunAlexei(&b, k % 2 == 0, true, &routes, &hitsAfter);
        }
        std::printf("alexei assembly + out of bounds, %u starts: %u misses as built, %u with region routes "
            "(%u routes found); circle hits %u / %u\n", kRuns, before, after, routes, hitsBefore, hitsAfter);
        CHECK(after == 0);
        CHECK(routes > 0);
        CHECK(hitsAfter <= hitsBefore);
    }

    // RegionRoute on its own: the member 12 yd past a lasting circle that is hot before its straight way is over.
    void TestRegionRoute()
    {
        Solver::Input in;
        in.now = kStart;
        in.bot = { 0.f, -14.f, 0.f };
        Solver::Hazard h;
        h.id = 5;
        h.shape = Shape::Circle(7.f);
        h.poses = { Pose{} };
        h.marginYd = kMargin;
        h.timing = Solver::MakeInterval(kStart + Ms(1000), kStart + Ms(8000));
        in.hazards.push_back(h);
        Solver::Region r;
        r.id = 3;
        r.shape = Shape::Circle(3.f);
        r.poses = { Pose{ 0.f, 12.f, 0.f, 0.f } };
        r.marginYd = VR::RegionMarginYd(r.shape, kMargin);
        r.from = kStart + Ms(8000);
        r.to = kStart + Ms(8500);
        in.regions.push_back(r);
        StraightOracle oracle;
        Solver::Plan const solve = Solver::Solve(in, oracle);
        CHECK(solve.outcome == Solver::Outcome::Fallback);
        CHECK(solve.violated == 1);
        Solver::RouteParams rp;
        Solver::RouteStats rs;
        Solver::Plan const route = Solver::RegionRoute(in, oracle, rp, &rs);
        CHECK(route.outcome == Solver::Outcome::Move);
        CHECK(rs.found);
        CHECK(route.waypoints.size() >= 2);
        CHECK(Solver::TargetOk(in, route.target, route.arriveAt, kStart + in.params.horizon));
        CHECK(Solver::CheckPath(in, route.path, kStart, false).ok
            || Solver::CheckPath(Solver::ReducedMargins(in), route.path, kStart, false).ok);
        CHECK(std::find(route.answers.begin(), route.answers.end(), 3u) != route.answers.end());
        CHECK(route.holdUntil >= r.to);
        // Nothing to do: inside already, or no region at all.
        Solver::Input inside = in;
        inside.bot = { 0.f, 12.f, 0.f };
        CHECK(Solver::RegionRoute(inside, oracle, rp).outcome == Solver::Outcome::Safe);
        Solver::Input none = in;
        none.regions.clear();
        CHECK(Solver::RegionRoute(none, oracle, rp).outcome == Solver::Outcome::Safe);
    }

    // ---- G7: solve cost --------------------------------------------------------------------------------------

    void AddLaneSet(Solver::Input& in, Lcg& rng, Ms reg, std::uint32_t& id)
    {
        Frame const frame{ -20.f, -20.f, 0.f, 0.f, 40.f, 40.f };
        Solver::LaneFrame lf;
        lf.frame = frame;
        std::vector<LaneRect> xs;
        std::vector<LaneRect> ys;
        auto hazard = [&](LanePlacement const& pl, Solver::Timing const& t)
        {
            Solver::Hazard h;
            h.id = id++;
            h.shape = Shape::Line(pl.length, pl.width);
            h.poses = { pl.pose };
            h.marginYd = kMargin;
            h.timing = t;
            return h;
        };
        for (int axis = 0; axis < 2; ++axis)
            for (float o : RandomLaneOffsets(40.f, 3, 6.f, 0.f, [&rng]() { return rng.Next01(); }))
            {
                LaneRect const rect{ axis == 0 ? Axis::X : Axis::Y, o, 6.f };
                (axis == 0 ? xs : ys).push_back(rect);
                lf.rects.push_back(rect);
                if (reg + Ms(4800) >= in.now)
                    in.hazards.push_back(hazard(PlaceLane(frame, rect), Solver::MakeInstant(reg + Ms(4000), Ms(500))));
            }
        for (LaneRect const& x : xs)
            for (LaneRect const& y : ys)
                in.hazards.push_back(hazard(PlaceCrossing(frame, x, y),
                    Solver::MakeInterval(reg + Ms(4000), reg + Ms(10000))));
        in.lanes.push_back(lf);
    }

    // Scene k: k % 3 = 0 two overlapping lane sets (`lanes 6 4000 count=3 cross=1` every 10 s: the last set's
    // crossings still burn while the next set's lanes warn, the sweep's costliest solves); 1 a barrage mix round
    // the bot (circles, donuts, cones, lines, projectiles); 2 the mix plus armed mines. A melee role band.
    Solver::Input Scene(std::uint32_t k)
    {
        Lcg rng{ 0x9e3779b97f4a7c15ull ^ (static_cast<std::uint64_t>(k) * 0x100000001b3ull) };
        Solver::Input in;
        in.now = kStart;
        in.speed = 7.f;
        in.bot = { rng.Range(-18.f, 18.f), rng.Range(-18.f, 18.f), 0.f };
        in.role.present = true;
        in.role.maxYd = 3.8f;
        std::uint32_t id = 1;
        if (k % 3 == 0)
        {
            AddLaneSet(in, rng, kStart - Ms(10000 + static_cast<int>(rng.Range(0.f, 2000.f))), id);
            AddLaneSet(in, rng, kStart - Ms(static_cast<int>(rng.Range(0.f, 3800.f))), id);
            return in;
        }
        auto add = [&](Shape const& s, Pose const& p, Solver::Timing const& t)
        {
            Solver::Hazard h;
            h.id = id++;
            h.shape = s;
            h.poses = { p };
            h.marginYd = kMargin;
            h.timing = t;
            in.hazards.push_back(h);
        };
        std::uint32_t const n = 6 + static_cast<std::uint32_t>(rng.Range(0.f, 10.f));
        for (std::uint32_t i = 0; i < n; ++i)
        {
            Pose const p{ in.bot.x + rng.Range(-8.f, 8.f), in.bot.y + rng.Range(-8.f, 8.f), 0.f,
                rng.Range(0.f, Geo::kTwoPi) };
            Solver::Timing const t = Solver::MakeInstant(kStart + Ms(static_cast<int>(rng.Range(300.f, 3000.f))),
                Ms(500));
            switch (static_cast<int>(rng.Range(0.f, 5.f)))
            {
                case 0:
                    add(Shape::Circle(rng.Range(3.f, 6.f)), p, t);
                    break;
                case 1:
                    add(Shape::Donut(3.f, 8.f), p, t);
                    break;
                case 2:
                    add(Shape::Cone(60.f, 15.f), p, t);
                    break;
                case 3:
                    add(Shape::Line(24.f, 4.f), p, t);
                    break;
                default:
                {
                    ProjectileParams pp;
                    pp.length = 24.f;
                    pp.speed = 12.f;
                    pp.halfLength = 1.5f;
                    add(Shape::Line(24.f, 3.f), p, Solver::MakeProjectile(pp, t.from));
                    break;
                }
            }
        }
        if (k % 3 == 2)
            for (std::uint32_t i = 0; i < 8; ++i)
                add(Shape::Circle(3.f), Pose{ in.bot.x + rng.Range(-12.f, 12.f), in.bot.y + rng.Range(-12.f, 12.f), 0.f,
                    0.f }, Solver::MakeMineArmed(kStart - Ms(5000), Solver::kForever, Ms(500)));
        return in;
    }

    // Solve of scenes 0..59 with the M4 solver as built (module 2df50bf), straight oracle: outcome, target, exposure.
    // The G7 speed-ups (the bounded Fallback ranking, the per-sample hot-hull skip) must not change one of them.
    struct Golden
    {
        int outcome;
        float x, y;
        long long exposure;
    };

    void TestG7Exact()
    {
        static Golden const golden[] = {
            { 1, -3.37f, 2.53f, 0 },
            { 3, 1.34f, -1.34f, 728 },
            { 0, 8.42f, -9.60f, 0 },
            { 1, 1.90f, 0.00f, 0 },
            { 3, 0.00f, -1.90f, 0 },
            { 3, -1.34f, 1.34f, 938 },
            { 1, 17.73f, -11.55f, 0 },
            { 3, -10.61f, 5.00f, 392 },
            { 3, -5.51f, -4.05f, 784 },
            { 3, -10.97f, -17.40f, 0 },
            { 3, 3.33f, 25.12f, 952 },
            { 1, 13.58f, 5.68f, 0 },
            { 1, -7.78f, 3.33f, 0 },
            { 3, 1.76f, 0.73f, 1092 },
            { 3, -0.32f, 10.38f, 1397 },
            { 1, 5.00f, -3.00f, 0 },
            { 3, 26.26f, 1.17f, 112 },
            { 1, 10.91f, -6.32f, 0 },
            { 1, -1.00f, 3.00f, 0 },
            { 1, 0.46f, -6.88f, 0 },
            { 3, -20.18f, 10.41f, 249 },
            { 1, -3.00f, 3.00f, 0 },
            { 3, -1.34f, 1.34f, 1064 },
            { 3, 16.30f, 25.57f, 364 },
            { 3, -8.95f, 13.85f, 0 },
            { 1, -11.05f, 9.46f, 0 },
            { 3, -7.61f, -9.83f, 0 },
            { 3, 16.66f, -1.28f, 0 },
            { 3, -1.76f, -0.73f, 518 },
            { 3, 9.25f, -13.13f, 683 },
            { 1, -0.18f, -4.61f, 0 },
            { 3, 1.92f, -11.87f, 462 },
            { 3, 1.54f, -9.58f, 0 },
            { 1, 3.00f, -3.00f, 0 },
            { 3, -0.73f, -1.76f, 1190 },
            { 3, -22.15f, 24.56f, 490 },
            { 3, -2.64f, 3.48f, 98 },
            { 3, 1.76f, -0.73f, 182 },
            { 3, 12.02f, 21.52f, 0 },
            { 3, 16.53f, -4.09f, 0 },
            { 3, 15.70f, 11.17f, 546 },
            { 3, -11.36f, 9.69f, 557 },
            { 1, 7.68f, -8.71f, 0 },
            { 1, 2.71f, -2.56f, 0 },
            { 1, 4.87f, 14.18f, 0 },
            { 1, 7.61f, 11.37f, 0 },
            { 3, -14.23f, -17.48f, 0 },
            { 1, -18.07f, 12.01f, 0 },
            { 1, -3.15f, 1.91f, 0 },
            { 1, -1.32f, 3.76f, 0 },
            { 3, 7.29f, 18.46f, 2744 },
            { 1, -11.36f, 4.96f, 0 },
            { 1, -4.79f, -2.57f, 0 },
            { 3, 0.00f, -1.90f, 658 },
            { 1, -1.77f, -1.87f, 0 },
            { 1, 9.93f, -10.58f, 0 },
            { 3, -1.76f, 0.73f, 1148 },
            { 1, 18.11f, -7.89f, 0 },
            { 3, 1.76f, -0.73f, 168 },
            { 3, 21.12f, -8.34f, 1061 },
        };
        std::uint32_t fallbacks = 0;
        std::uint32_t k = 0;
        for (Golden const& g : golden)
        {
            Solver::Input const in = Scene(k++);
            StraightOracle oracle;
            Solver::Plan const p = Solver::Solve(in, oracle);
            CHECK(static_cast<int>(p.outcome) == g.outcome);
            CHECK_NEAR(p.target.x, g.x, 0.011);
            CHECK_NEAR(p.target.y, g.y, 0.011);
            CHECK(p.exposure.count() == g.exposure);
            fallbacks += p.outcome == Solver::Outcome::Fallback || p.outcome == Solver::Outcome::NoPath;
        }
        CHECK(fallbacks >= 20);

        // The hot-hull skip relies on this: every window of a non-projectile hazard lies within [from, max(end,
        // strictTo)]. A projectile's does not (its margin band past the far end), so it is never skipped.
        Lcg rng{ 11 };
        bool projectilePast = false;
        for (std::uint32_t i = 0; i < 4000; ++i)
        {
            Solver::Hazard h;
            h.shape = Shape::Line(24.f, 3.f);
            h.poses = { Pose{} };
            h.marginYd = rng.Range(0.f, 2.f);
            Ms const at = kStart + Ms(static_cast<int>(rng.Range(0.f, 3000.f)));
            switch (i % 5)
            {
                case 0:
                    h.timing = Solver::MakeInstant(at, Ms(500));
                    break;
                case 1:
                    h.timing = Solver::MakeInterval(at, at + Ms(4000));
                    break;
                case 2:
                    h.timing = Solver::MakeMineArming(at, at + Ms(6000), Ms(500));
                    h.timing.strictTo = at + Ms(200);
                    break;
                case 3:
                    h.timing = Solver::MakeMineArmed(at, at + Ms(6000), Ms(500));
                    break;
                default:
                {
                    ProjectileParams pp;
                    pp.length = 24.f;
                    pp.speed = 12.f;
                    pp.halfLength = 1.5f;
                    h.timing = Solver::MakeProjectile(pp, at);
                    break;
                }
            }
            Vec3 const p{ rng.Range(-2.f, 28.f), rng.Range(-3.f, 3.f), 0.f };
            Ms const end = std::max(Solver::Detail::HazardEnd(h), h.timing.strictTo);
            for (HotWindow const& w : Solver::WindowsAt(h, p))
            {
                if (h.timing.type == Solver::TimingType::Projectile)
                {
                    projectilePast = projectilePast || w.to > end;
                    continue;
                }
                CHECK(w.from >= h.timing.from);
                CHECK(w.to <= end);
            }
        }
        CHECK(projectilePast);

        // WalkPath's measureCap: the time inside is exact up to the cap and only known to be above it past it.
        Solver::Input in;
        in.now = kStart;
        Solver::Hazard zone;
        zone.id = 1;
        zone.shape = Shape::Circle(4.f);
        zone.poses = { Pose{} };
        zone.timing = Solver::MakeInterval(kStart, kStart + Ms(10000));
        in.hazards.push_back(zone);
        std::vector<Vec3> const path{ { -10.f, 0.f, 0.f }, { 10.f, 0.f, 0.f } };
        Ms const full = Solver::Detail::WalkPath(in, path, kStart, true, true).timeInsideHot;
        CHECK(full > Ms(1000));
        CHECK(Solver::Detail::WalkPath(in, path, kStart, true, true, full).timeInsideHot == full);
        Ms const capped = Solver::Detail::WalkPath(in, path, kStart, true, true, Ms(300)).timeInsideHot;
        CHECK(capped > Ms(300));
        CHECK(capped < full);
    }

    // `fight_test golden`: the table above from the solver as it stands (regenerate only for an intended change).
    void PrintGolden()
    {
        for (std::uint32_t k = 0; k < 60; ++k)
        {
            StraightOracle oracle;
            Solver::Plan const p = Solver::Solve(Scene(k), oracle);
            std::printf("            { %d, %.2ff, %.2ff, %lld },\n", static_cast<int>(p.outcome), p.target.x,
                p.target.y, static_cast<long long>(p.exposure.count()));
        }
    }

    // The lane-set Fallbacks (the sweep's Demonology 4,975 us and Subtlety 2,153 us solves): host cost of scenes
    // 0, 3, 6, ... that end in a Fallback. The navmesh adds its path calls live.
    void Bench(bool check)
    {
        std::vector<double> us;
        for (std::uint32_t k = 0; k < 900; k += 3)
        {
            Solver::Input const in = Scene(k);
            StraightOracle oracle;
            Solver::Plan const first = Solver::Solve(in, oracle);
            if (first.outcome != Solver::Outcome::Fallback && first.outcome != Solver::Outcome::NoPath)
                continue;
            auto const t0 = std::chrono::steady_clock::now();
            for (int r = 0; r < 10; ++r)
                (void)Solver::Solve(in, oracle);
            us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count()
                / 10.0);
        }
        std::sort(us.begin(), us.end());
        double mean = 0.0;
        for (double v : us)
            mean += v / static_cast<double>(us.size());
        std::printf("fight bench, lane-set Fallbacks: %zu solves, mean %.0f us, median %.0f us, p90 %.0f us, max %.0f "
            "us (limit 2000 us)\n", us.size(), mean, us[us.size() / 2], us[us.size() * 9 / 10], us.back());
        if (check)
        {
            CHECK(us.size() >= 20);
            CHECK(us[us.size() / 2] < 2000.0);
        }
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "bench") == 0)
    {
        Bench(false);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "golden") == 0)
    {
        PrintGolden();
        return 0;
    }
    TestRegionEdgeArcs();
    TestFenrus();
    TestRas();
    TestRegionRoute();
    TestAlexei();
    TestG7Exact();
    Bench(true);
    return Report("fight_test");
}
