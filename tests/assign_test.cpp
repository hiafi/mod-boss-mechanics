// Standalone tests for src/MechanicAssign.h (STAGE-M3-HANDOFF §4 WP-A).
// Build: tests/run.sh.

#include "check.h"
#include "MechanicAssign.h"
#include <cstdint>
#include <vector>

using namespace BossMechanics;
using Assign::Claimant;
using Assign::SoakRule;

namespace
{
    constexpr std::int64_t kDeadline = 10000;

    Claimant Bot(std::uint64_t guid, std::uint32_t stacks, std::int64_t eta, std::int64_t since = 0)
    {
        Claimant c;
        c.guid = guid;
        c.bot = true;
        c.stacks = stacks;
        c.etaMs = eta;
        c.claimedSince = since;
        return c;
    }

    Claimant Human(std::uint64_t guid, bool inside, std::uint32_t stacks = 0)
    {
        Claimant c;
        c.guid = guid;
        c.insideNow = inside;
        c.stacks = stacks;
        c.etaMs = 0;
        return c;
    }

    SoakRule Rule(std::uint32_t count, bool ordered = false, bool ascending = true, std::uint32_t cap = 0)
    {
        SoakRule r;
        r.count = count;
        r.ordered = ordered;
        r.ascending = ascending;
        r.capStacks = cap;
        r.deadlineMs = kDeadline;
        return r;
    }

    using Guids = std::vector<std::uint64_t>;

    void TestCountZero()
    {
        std::vector<Claimant> const all{ Bot(1, 0, 100), Bot(2, 0, 200), Human(3, true) };
        CHECK(Assign::SoakClaimers(all, Rule(0)).empty());
    }

    void TestUnorderedByEta()
    {
        std::vector<Claimant> const all{ Bot(5, 3, 900), Bot(2, 0, 500), Bot(9, 1, 300) };
        CHECK(Assign::SoakClaimers(all, Rule(1)) == Guids{ 9 });
        CHECK(Assign::SoakClaimers(all, Rule(2)) == (Guids{ 9, 2 }));
        // Ties on eta: lowest guid.
        std::vector<Claimant> const tie{ Bot(7, 0, 400), Bot(4, 0, 400), Bot(6, 0, 400) };
        CHECK(Assign::SoakClaimers(tie, Rule(2)) == (Guids{ 4, 6 }));
    }

    void TestOrdering()
    {
        std::vector<Claimant> const all{ Bot(1, 2, 100), Bot(2, 0, 900), Bot(3, 1, 500), Bot(4, 3, 50) };
        // Ascending: the fewest stacks first, whatever the eta.
        CHECK(Assign::SoakClaimers(all, Rule(1, true, true)) == Guids{ 2 });
        CHECK(Assign::SoakClaimers(all, Rule(2, true, true)) == (Guids{ 2, 3 }));
        // Descending: the most stacks first.
        CHECK(Assign::SoakClaimers(all, Rule(1, true, false)) == Guids{ 4 });
        CHECK(Assign::SoakClaimers(all, Rule(4, true, false)) == (Guids{ 4, 1, 3, 2 }));
        // Ties on stacks: eta, then guid.
        std::vector<Claimant> const tie{ Bot(8, 1, 300), Bot(6, 1, 300), Bot(7, 1, 200) };
        CHECK(Assign::SoakClaimers(tie, Rule(3, true, true)) == (Guids{ 7, 6, 8 }));
    }

    void TestCap()
    {
        std::vector<Claimant> const all{ Bot(1, 0, 900), Bot(2, 2, 100), Bot(3, 3, 50), Bot(4, 1, 400) };
        // cap 2: bots at 2 or more never claim, even when nobody else is left.
        CHECK(Assign::SoakClaimers(all, Rule(1, true, false, 2)) == Guids{ 4 });
        CHECK(Assign::SoakClaimers(all, Rule(4, true, true, 2)) == (Guids{ 1, 4 }));
        // A human inside at the cap does not claim either.
        std::vector<Claimant> const human{ Human(9, true, 2), Bot(1, 0, 900) };
        CHECK(Assign::SoakClaimers(human, Rule(1, true, true, 2)) == Guids{ 1 });
    }

    void TestHumansInsideFirst()
    {
        std::vector<Claimant> all{ Bot(1, 0, 100), Human(50, true, 3), Human(51, false), Bot(2, 0, 200) };
        // A human inside takes the slot first (even against the rule).
        CHECK(Assign::SoakClaimers(all, Rule(1, true, true)) == Guids{ 50 });
        CHECK(Assign::SoakClaimers(all, Rule(2, true, true)) == (Guids{ 50, 1 }));
        // A human outside never counts.
        all[1].insideNow = false;
        CHECK(Assign::SoakClaimers(all, Rule(3)) == (Guids{ 1, 2 }));
        // A bot already inside counts with the humans inside.
        std::vector<Claimant> bots{ Bot(1, 0, 100), Bot(2, 0, 5000) };
        bots[1].insideNow = true;
        CHECK(Assign::SoakClaimers(bots, Rule(1)) == Guids{ 2 });
    }

    void TestStickyClaims()
    {
        // Bot 3 claimed at t=1000; then bot 1 got closer than it. The claim holds while bot 3 can make it.
        std::vector<Claimant> all{ Bot(1, 0, 200), Bot(2, 0, 400), Bot(3, 0, 900, 1000) };
        CHECK(Assign::SoakClaimers(all, Rule(1)) == Guids{ 3 });
        // It moves (eta grows) but still makes the deadline: still the claimer.
        all[2].etaMs = 9000;
        CHECK(Assign::SoakClaimers(all, Rule(1)) == Guids{ 3 });
        // Two sticky claims: the older one first.
        all[1].claimedSince = 800;
        CHECK(Assign::SoakClaimers(all, Rule(1)) == Guids{ 2 });
        CHECK(Assign::SoakClaimers(all, Rule(2)) == (Guids{ 2, 3 }));
        // A sticky claim that can no longer make it drops behind the bots that can.
        all[2].etaMs = kDeadline + 1;
        all[1].claimedSince = 0;
        CHECK(Assign::SoakClaimers(all, Rule(1)) == Guids{ 1 });
        // A claim of a player no longer eligible (not listed) is gone.
        all.erase(all.begin() + 2);
        CHECK(Assign::SoakClaimers(all, Rule(2)) == (Guids{ 1, 2 }));
        // Stable across calls (every bot computes the same answer).
        CHECK(Assign::SoakClaimers(all, Rule(2)) == Assign::SoakClaimers(all, Rule(2)));
        // Input order does not matter.
        std::vector<Claimant> rev(all.rbegin(), all.rend());
        CHECK(Assign::SoakClaimers(rev, Rule(2)) == Assign::SoakClaimers(all, Rule(2)));
    }

    // STAGE-M3-R-REPORT m-5: a lasting soak already open (RG2, a walking anchor). The claimer trails the circle and
    // is outside for a moment: every eta is past the deadline, and the lowest-stack bot would take the slot, unless
    // the sticky claim is kept.
    void TestLastingSoakKeepsClaim()
    {
        std::vector<Claimant> const all{ Bot(1, 0, kDeadline + 300), Bot(2, 2, kDeadline + 900, 1500) };
        SoakRule rule = Rule(1, true);
        CHECK(Assign::SoakClaimers(all, rule) == Guids{ 1 });
        rule.keepSticky = true;
        CHECK(Assign::SoakClaimers(all, rule) == Guids{ 2 });
        // Inside players still come first.
        std::vector<Claimant> withHuman = all;
        withHuman.push_back(Human(9, true));
        CHECK(Assign::SoakClaimers(withHuman, rule) == Guids{ 9 });
    }

    void TestLateBotsLast()
    {
        // Bot 1 is the best by the rule but late; bot 2 can make it.
        std::vector<Claimant> const all{ Bot(1, 0, kDeadline + 500), Bot(2, 3, 300), Bot(3, 1, kDeadline + 100) };
        CHECK(Assign::SoakClaimers(all, Rule(1, true, true)) == Guids{ 2 });
        // Leftover slots go to the late ones, by the rule.
        CHECK(Assign::SoakClaimers(all, Rule(3, true, true)) == (Guids{ 2, 1, 3 }));
        // Only late bots: best effort, still filled.
        std::vector<Claimant> const late{ Bot(4, 0, kDeadline + 900), Bot(5, 0, kDeadline + 100) };
        CHECK(Assign::SoakClaimers(late, Rule(1)) == Guids{ 5 });
        // Duplicates count once.
        std::vector<Claimant> const dup{ Bot(4, 0, 100), Bot(4, 0, 100) };
        CHECK(Assign::SoakClaimers(dup, Rule(2)) == Guids{ 4 });
    }

    void TestClusterLeader()
    {
        using P = Assign::Point;
        // The medoid: the middle of a line of three.
        std::vector<P> const line{ { 30, 0.f, 0.f }, { 10, 5.f, 0.f }, { 20, 10.f, 0.f } };
        CHECK(Assign::ClusterLeader(line, 0) == 10);
        // Sticky while listed, even when it is not the medoid.
        CHECK(Assign::ClusterLeader(line, 30) == 30);
        // A sticky leader that left is replaced by the medoid.
        CHECK(Assign::ClusterLeader(line, 99) == 10);
        // Ties: lowest guid (a symmetric square).
        std::vector<P> const square{ { 7, 0.f, 0.f }, { 4, 4.f, 0.f }, { 9, 4.f, 4.f }, { 5, 0.f, 4.f } };
        CHECK(Assign::ClusterLeader(square, 0) == 4);
        CHECK(Assign::ClusterLeader({}, 3) == 0);
        CHECK(Assign::ClusterLeader({ { 8, 1.f, 1.f } }, 0) == 8);
    }

    void TestSlotOf()
    {
        CHECK(Assign::SlotOf({ 40, 10, 30, 20 }, 10) == 0);
        CHECK(Assign::SlotOf({ 40, 10, 30, 20 }, 30) == 2);
        CHECK(Assign::SlotOf({ 40, 10, 30, 20 }, 40) == 3);
        CHECK(Assign::SlotOf({ 40, 10, 10, 20 }, 40) == 2);    // duplicates count once
        CHECK(Assign::SlotOf({ 40, 10 }, 25) == 1);             // not listed: its rank among them
        CHECK(Assign::SlotOf({}, 25) == 0);
    }
}

int main()
{
    TestCountZero();
    TestUnorderedByEta();
    TestOrdering();
    TestCap();
    TestHumansInsideFirst();
    TestStickyClaims();
    TestLastingSoakKeepsClaim();
    TestLateBotsLast();
    TestClusterLeader();
    TestSlotOf();
    return Report("assign_test");
}
