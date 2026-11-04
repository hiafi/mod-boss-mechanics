// Standalone tests for src/MechanicJudge.h, the registry's judging rules (STAGE-M3-HANDOFF §3.3, decisions §7.7 and
// §7.8). Build: tests/run.sh.

#include "check.h"
#include "MechanicAssign.h"
#include "MechanicJudge.h"
#include "MechanicMath.h"
#include <vector>

using namespace BossMechanics;
using Judge::Ms;

namespace
{
    Kind const kAllKinds[] = { Kind::Zone, Kind::Frontal, Kind::Spread, Kind::RunOut, Kind::Stack, Kind::Soak,
        Kind::Bait, Kind::HoldStill, Kind::Spot };

    Judge::Candidate Player()
    {
        return {};
    }

    Judge::Candidate Pet()
    {
        Judge::Candidate c;
        c.player = false;
        return c;
    }

    // B9 / decision §7.8: Spread, RunOut and Bait circles hit players only; Zones and Frontals still hit pets.
    void TestPlayersOnly()
    {
        for (Kind k : { Kind::Spread, Kind::RunOut, Kind::Bait })
        {
            CHECK(Judge::PlayersOnly(k));
            CHECK(Judge::KeepResolveTarget(k, Player()));
            CHECK(!Judge::KeepResolveTarget(k, Pet()));
        }
        for (Kind k : { Kind::Zone, Kind::Frontal })
        {
            CHECK(!Judge::PlayersOnly(k));
            CHECK(Judge::KeepResolveTarget(k, Player()));
            CHECK(Judge::KeepResolveTarget(k, Pet()));
        }
    }

    // The filter's other rules hold for every kind.
    void TestFilterRules()
    {
        for (Kind k : kAllKinds)
        {
            Judge::Candidate c = Player();
            c.alive = false;
            CHECK(!Judge::KeepResolveTarget(k, c));
            c = Player();
            c.playerControlled = false;         // a creature (an add, the marker)
            c.player = false;
            CHECK(!Judge::KeepResolveTarget(k, c));
            c = Player();
            c.gameMaster = true;
            CHECK(!Judge::KeepResolveTarget(k, c));
            c = Player();
            c.inside = false;
            CHECK(!Judge::KeepResolveTarget(k, c));
            c = Player();
            c.eligible = false;
            CHECK(!Judge::KeepResolveTarget(k, c));
            c = Player();
            c.tankExempted = true;
            CHECK(!Judge::KeepResolveTarget(k, c));
            c = Player();
            c.hitThisPass = true;               // a second cast of the same pass
            CHECK(!Judge::KeepResolveTarget(k, c));
        }
    }

    // B8 / decision §7.7: a shape kind anchored on a player spares that player; nothing else changes.
    void TestSelfAnchorSparing()
    {
        CHECK(Judge::SparesAnchor(Kind::Zone, true));
        CHECK(Judge::SparesAnchor(Kind::Frontal, true));
        CHECK(!Judge::SparesAnchor(Kind::Zone, false));         // a cone on the boss hits whoever stands in it
        CHECK(!Judge::SparesAnchor(Kind::Frontal, false));
        for (Kind k : { Kind::Spread, Kind::RunOut, Kind::Stack, Kind::Soak, Kind::Bait, Kind::HoldStill, Kind::Spot })
            CHECK(!Judge::SparesAnchor(k, true));
        CHECK(Judge::IsShapeKind(Kind::Zone) && Judge::IsShapeKind(Kind::Frontal));
        CHECK(!Judge::IsShapeKind(Kind::Stack) && !Judge::IsShapeKind(Kind::Soak));
    }

    // B7: the victim of the source creature, else the owner's.
    void TestTankExemptSource()
    {
        CHECK(Judge::TankExemptSource(true, true) == Judge::ExemptFrom::Anchor);
        CHECK(Judge::TankExemptSource(true, false) == Judge::ExemptFrom::Owner);    // the source despawned
        CHECK(Judge::TankExemptSource(false, true) == Judge::ExemptFrom::Owner);    // a point or a player anchor
        CHECK(Judge::TankExemptSource(false, false) == Judge::ExemptFrom::Owner);
    }

    // Soak: fewer than N fails; N or more never does (bots claim exactly N, the registry tolerates extras).
    void TestSoakCount()
    {
        CHECK(Judge::SoakShort(0, 1));
        CHECK(!Judge::SoakShort(1, 1));
        CHECK(!Judge::SoakShort(3, 1));             // extra bodies never fail
        CHECK(Judge::SoakShort(1, 2));
        CHECK(!Judge::SoakShort(2, 2));
        CHECK(!Judge::SoakShort(0, 0));             // count 0 (the fortune): nobody needed, never fails
        CHECK(!Judge::SoakShort(4, 0));
    }

    // B5: HoldStill tolerance per mechanic (default 0.3 yd).
    void TestHoldStillTolerance()
    {
        CHECK(!Judge::HoldStillMoved(0.3f, Rules::kHoldStillToleranceYd));
        CHECK(Judge::HoldStillMoved(0.31f, Rules::kHoldStillToleranceYd));
        CHECK(!Judge::HoldStillMoved(0.9f, 1.f));    // Alexei: "more than 1 yd"
        CHECK(!Judge::HoldStillMoved(1.f, 1.f));
        CHECK(Judge::HoldStillMoved(1.01f, 1.f));
        CHECK(Judge::HoldStillMoved(0.01f, 0.f));
    }

    // B4: Bait landing circle.
    void TestBaitRadius()
    {
        CHECK(Judge::BaitHitsPickOnly(0.f));
        CHECK_NEAR(Judge::BaitHitRadius(0.f), 0.5f, 1e-6);
        CHECK(!Judge::BaitHitsPickOnly(6.f));
        CHECK(!Judge::HitIsFailure(Kind::Bait, true));
        CHECK(Judge::HitIsFailure(Kind::Bait, false));
        CHECK(Judge::HitIsFailure(Kind::Zone, true));
        CHECK(Judge::HitIsFailure(Kind::Spread, false));
        CHECK_NEAR(Judge::BaitHitRadius(6.f), 6.f, 1e-6);
        CHECK_NEAR(Judge::BaitHitRadius(0.2f), 0.5f, 1e-6);
    }

    // B3: Stack Cluster(n).
    void TestCluster()
    {
        Shape const circle = Shape::Circle(6.f);
        // Three together, one far away: with n = 2 the three pass and the far one misses.
        std::vector<Geo::Vec3> const group{ { 0.f, 0.f, 0.f }, { 3.f, 0.f, 0.f }, { 0.f, 3.f, 0.f },
            { 30.f, 0.f, 0.f } };
        std::vector<std::size_t> miss = Judge::ClusterMisses(circle, group, 2);
        CHECK(miss.size() == 1 && miss[0] == 3);
        // n = 3: nobody has 3 others within 6 yd.
        CHECK(Judge::ClusterMisses(circle, group, 3).size() == 4);
        // A chain 5 yd apart: the ends see one neighbor, the middle two.
        std::vector<Geo::Vec3> const chain{ { 0.f, 0.f, 0.f }, { 5.f, 0.f, 0.f }, { 10.f, 0.f, 0.f } };
        miss = Judge::ClusterMisses(circle, chain, 2);
        CHECK(miss.size() == 2 && miss[0] == 0 && miss[1] == 2);
        CHECK(Judge::ClusterMisses(circle, chain, 1).empty());
        // The edge is inclusive; another floor is not counted.
        std::vector<Geo::Vec3> const edge{ { 0.f, 0.f, 0.f }, { 6.f, 0.f, 0.f }, { 0.f, 1.f, 20.f } };
        CHECK(Judge::ClusterCount(circle, edge[0], { edge[1], edge[2] }) == 1);
        // A lone player can never satisfy n >= 1.
        CHECK(Judge::ClusterMisses(circle, { { 0.f, 0.f, 0.f } }, 1).size() == 1);
    }

    // B2 (and M1 persistent zones): the pass clock.
    void TestPassClock()
    {
        Ms const forever{ 1'000'000'000 };
        bool detonate = false;
        std::uint32_t pass = 99;

        // Tick 1000 over 3000 from resolveAt 1000: passes at 1000, 2000, 3000; none at 4000 (= until).
        Judge::PassClock c{ Ms(1000), Ms(4000), Ms(1000), 0 };
        CHECK(!Judge::TakePass(c, Ms(999), detonate, false, forever, pass));
        CHECK(Judge::TakePass(c, Ms(1000), detonate, false, forever, pass) && pass == 0);
        CHECK(!Judge::TakePass(c, Ms(1500), detonate, true, forever, pass));
        CHECK(Judge::TakePass(c, Ms(2000), detonate, true, forever, pass) && pass == 1);
        CHECK(Judge::TakePass(c, Ms(3050), detonate, true, forever, pass) && pass == 2);
        CHECK(!Judge::PassesDone(c, Ms(3050), true));
        CHECK(!Judge::TakePass(c, Ms(4000), detonate, true, forever, pass));
        CHECK(Judge::PassesDone(c, Ms(4000), true));

        // Tick 0: one pass at resolveAt, then it lingers until `until`.
        Judge::PassClock once{ Ms(500), Ms(5500), Ms(0), 0 };
        CHECK(Judge::TakePass(once, Ms(500), detonate, false, forever, pass) && pass == 0);
        CHECK(once.nextTick == forever);
        CHECK(!Judge::TakePass(once, Ms(3000), detonate, true, forever, pass));
        CHECK(!Judge::PassesDone(once, Ms(5499), true));
        CHECK(Judge::PassesDone(once, Ms(5500), true));

        // Duration 0: still one pass.
        Judge::PassClock zero{ Ms(500), Ms(500), Ms(0), 0 };
        CHECK(Judge::TakePass(zero, Ms(500), detonate, false, forever, pass) && pass == 0);
        CHECK(Judge::PassesDone(zero, Ms(500), true));

        // Detonate before the first pass pulls it to now; after it, it is ignored.
        Judge::PassClock d{ Ms(2000), Ms(6000), Ms(1000), 0 };
        detonate = true;
        CHECK(Judge::TakePass(d, Ms(1200), detonate, false, forever, pass) && pass == 0);
        CHECK(!detonate);
        CHECK(d.nextTick == Ms(2200));
        detonate = true;
        CHECK(!Judge::TakePass(d, Ms(1300), detonate, true, forever, pass));
        CHECK(!detonate && d.nextTick == Ms(2200));
    }

    struct FakeReservation
    {
        int bot = 0;
        float x = 0.f;
        Ms until{ 0 };
        Ms since{ 0 };
        bool claim = false;
        int slot = 0;
    };

    FakeReservation Res(int bot, float x, Ms until, Ms since, bool claim = false, int slot = 0)
    {
        FakeReservation r;
        r.bot = bot;
        r.x = x;
        r.until = until;
        r.since = since;
        r.claim = claim;
        r.slot = slot;
        return r;
    }

    // B10: reservations keep `since`, report only visible changes, drop on unreserve and expiry.
    void TestReservations()
    {
        auto same = [](FakeReservation const& a, FakeReservation const& b)
        {
            return a.x == b.x && a.claim == b.claim && a.slot == b.slot;
        };
        std::vector<FakeReservation> list;
        CHECK(Judge::UpsertReservation(list, Res(1, 5.f, Ms(3000), Ms(0)), Ms(1000), same));
        CHECK(list.size() == 1 && list[0].since == Ms(1000));
        // Same spot, later expiry: no change for the others, since kept.
        CHECK(!Judge::UpsertReservation(list, Res(1, 5.f, Ms(4000), Ms(0)), Ms(1500), same));
        CHECK(list.size() == 1 && list[0].until == Ms(4000) && list[0].since == Ms(1000));
        // A new spot: a change; since still the first one.
        CHECK(Judge::UpsertReservation(list, Res(1, 7.f, Ms(4000), Ms(1400)), Ms(1600), same));
        CHECK(list.size() == 1 && list[0].x == 7.f && list[0].since == Ms(1000));
        // A second bot with its own since.
        CHECK(Judge::UpsertReservation(list, Res(2, 1.f, Ms(2000), Ms(1200)), Ms(1700), same));
        CHECK(list.size() == 2 && list[1].since == Ms(1200));
        CHECK(!Judge::PruneReservations(list, Ms(1999)));
        CHECK(Judge::PruneReservations(list, Ms(2000)) && list.size() == 1 && list[0].bot == 1);
        CHECK(Judge::EraseReservation(list, 1) && list.empty());
        CHECK(!Judge::EraseReservation(list, 1));
    }

    // STAGE-M3-R-REPORT m-4 (the review's since_claim repro): B avoids a Soak from 1,000, C claims it at 1,100, B
    // turns claimer at 3,000. B's claim dates from 3,000, so with one slot left (a human stepped inside) C, the older
    // claim, keeps it.
    void TestClaimSince()
    {
        auto same = [](FakeReservation const& a, FakeReservation const& b)
        {
            return a.x == b.x && a.claim == b.claim && a.slot == b.slot;
        };
        std::vector<FakeReservation> list;
        Judge::UpsertReservation(list, Res(20, 0.f, Ms(9000), Ms(0), false), Ms(1000), same);
        Judge::UpsertReservation(list, Res(30, 5.f, Ms(9000), Ms(0), true), Ms(1100), same);
        CHECK(Judge::UpsertReservation(list, Res(20, 5.f, Ms(9000), Ms(0), true), Ms(3000), same));
        CHECK(list[0].since == Ms(3000));
        CHECK(list[1].since == Ms(1100));
        // Re-reserving the same claim keeps its start.
        CHECK(!Judge::UpsertReservation(list, Res(20, 5.f, Ms(9500), Ms(0), true), Ms(3500), same));
        CHECK(list[0].since == Ms(3000));
        // A Cluster member (slot 1) that becomes the leader (slot 0) leads from now.
        CHECK(Judge::UpsertReservation(list, Res(20, 5.f, Ms(9500), Ms(0), true, 1), Ms(4000), same));
        CHECK(list[0].since == Ms(4000));

        std::vector<Assign::Claimant> players(3);
        players[0].guid = 10;
        players[0].insideNow = true;
        players[0].etaMs = 3500;
        players[1].guid = 20;
        players[1].bot = true;
        players[1].etaMs = 3600;
        players[1].claimedSince = Ms(3000).count();
        players[2].guid = 30;
        players[2].bot = true;
        players[2].etaMs = 3600;
        players[2].claimedSince = list[1].since.count();
        Assign::SoakRule rule;
        rule.count = 2;
        rule.deadlineMs = 8000;
        std::vector<std::uint64_t> const out = Assign::SoakClaimers(players, rule);
        CHECK(out.size() == 2 && out[0] == 10 && out[1] == 30);
    }
}

int main()
{
    TestPlayersOnly();
    TestFilterRules();
    TestSelfAnchorSparing();
    TestTankExemptSource();
    TestSoakCount();
    TestHoldStillTolerance();
    TestBaitRadius();
    TestCluster();
    TestPassClock();
    TestReservations();
    TestClaimSince();
    return Report("judge_test");
}
