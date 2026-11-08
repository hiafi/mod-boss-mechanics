/*
 * mod-boss-mechanics: bot assignment for the player kinds (dungeon-mechanics STAGE-M3-HANDOFF §3.5, §3.6).
 *
 * Pure header: no AzerothCore or playerbots includes (tests/assign_test.cpp compiles it with the host compiler). Every
 * bot computes the same answer from the same registry state (decision §7.1: no messaging, no leader): the inputs are
 * plain values, ties always fall to the lowest guid, and sticky claims keep the answer stable while bots move.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_ASSIGN_H
#define MOD_BOSS_MECHANICS_MECHANIC_ASSIGN_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace BossMechanics::Assign
{
    // One eligible player of a Soak. etaMs and deadlineMs share a time base (absolute GameTime ms: now + straight
    // distance / run speed + StartDelayMs; deadline = from − SafetyMarginMs). claimedSince: when the bot's current
    // claim on this mechanic began (Reservation::since; 0 = no claim).
    struct Claimant { std::uint64_t guid = 0; bool bot = false; bool insideNow = false; std::uint32_t stacks = 0;
                      std::int64_t etaMs = 0; std::int64_t claimedSince = 0; /* 0 = no claim */ };
    // count: bodies needed; ordered: rank by aura stacks (ascending or not); capStacks > 0: players at or above it
    // never claim.
    // keepSticky: a lasting soak whose window is already open; a sticky claim stays whatever its eta (the claimer
    // trails a moving circle and is briefly outside; STAGE-M3-R-REPORT m-5).
    struct SoakRule { std::uint32_t count = 0; bool ordered = false, ascending = true; std::uint32_t capStacks = 0;
                      std::int64_t deadlineMs = 0; bool keepSticky = false; };

    namespace Detail
    {
        // The rule's stack key: smaller is better (0 when the rule is not ordered).
        inline std::int64_t StackKey(SoakRule const& rule, std::uint32_t stacks)
        {
            if (!rule.ordered)
                return 0;
            return rule.ascending ? static_cast<std::int64_t>(stacks) : -static_cast<std::int64_t>(stacks);
        }
    }

    // Who claims: players already inside first (humans and bots, by the rule, up to count); then sticky bot claims
    // that are still eligible and can make it (eta <= deadline); then bots ranked by (stacks per the rule, eta, guid).
    // Players at or above capStacks never claim; count 0: nobody. Late bots fill leftover slots last (best effort).
    // Humans count only once they stand inside. The caller passes eligible players only (Registry::IsEligible), so a
    // claim of a player no longer listed is dropped. Returned in claim order, each guid once.
    inline std::vector<std::uint64_t> SoakClaimers(std::vector<Claimant> const& players, SoakRule const& rule)
    {
        std::vector<std::uint64_t> out;
        if (rule.count == 0)
            return out;

        std::vector<Claimant const*> pool;
        pool.reserve(players.size());
        for (Claimant const& c : players)
        {
            if (rule.capStacks > 0 && c.stacks >= rule.capStacks)
                continue;
            bool dup = false;
            for (Claimant const* q : pool)
                dup = dup || q->guid == c.guid;
            if (!dup)
                pool.push_back(&c);
        }

        std::vector<bool> taken(pool.size(), false);
        auto pick = [&](auto&& want, auto&& less)
        {
            std::vector<std::size_t> idx;
            for (std::size_t i = 0; i < pool.size(); ++i)
                if (!taken[i] && want(*pool[i]))
                    idx.push_back(i);
            std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return less(*pool[a], *pool[b]); });
            for (std::size_t i : idx)
            {
                if (out.size() >= rule.count)
                    return;
                taken[i] = true;
                out.push_back(pool[i]->guid);
            }
        };
        auto byRule = [&](Claimant const& a, Claimant const& b)
        {
            std::int64_t const ka = Detail::StackKey(rule, a.stacks);
            std::int64_t const kb = Detail::StackKey(rule, b.stacks);
            if (ka != kb)
                return ka < kb;
            if (a.etaMs != b.etaMs)
                return a.etaMs < b.etaMs;
            return a.guid < b.guid;
        };
        auto byRuleNoEta = [&](Claimant const& a, Claimant const& b)
        {
            std::int64_t const ka = Detail::StackKey(rule, a.stacks);
            std::int64_t const kb = Detail::StackKey(rule, b.stacks);
            return ka != kb ? ka < kb : a.guid < b.guid;
        };
        auto bySince = [](Claimant const& a, Claimant const& b)
        {
            return a.claimedSince != b.claimedSince ? a.claimedSince < b.claimedSince : a.guid < b.guid;
        };

        // 1. Inside now (humans and bots).
        pick([](Claimant const& c) { return c.insideNow; }, byRuleNoEta);
        // 2. Sticky bot claims that can still make it (oldest claim first).
        pick([&](Claimant const& c)
        {
            return c.bot && c.claimedSince != 0 && (rule.keepSticky || c.etaMs <= rule.deadlineMs);
        }, bySince);
        // 3. Bots that can make it.
        pick([&](Claimant const& c) { return c.bot && c.etaMs <= rule.deadlineMs; }, byRule);
        // 4. Late bots, best effort.
        pick([](Claimant const& c) { return c.bot; }, byRule);
        return out;
    }

    // One live Soak of an exclusive group (MechanicRef::OneClaimPerBot, the same tag): its eligible players and rule.
    struct SoakInput { std::vector<Claimant> players; SoakRule rule; };

    // Claimers of an exclusive group: a player claims at most one Soak of the group, so one low-meter bot does not
    // claim every soul of a drain (Scholomance STAGE-R-B-MALICIA-LATE-REPORT S-4). The caller passes the group in a
    // fixed order (earliest resolve first, then the lowest id); each Soak runs SoakClaimers on the players no earlier
    // Soak took. A bot whose live claim (claimedSince) sits on a later Soak is kept for it: an earlier Soak takes it
    // only while it stands inside (step 1), so claims do not hop between souls. One claimer list per input, in order.
    inline std::vector<std::vector<std::uint64_t>> ExclusiveSoakClaimers(std::vector<SoakInput> const& soaks)
    {
        // home: the Soak holding a bot's oldest live claim (ties to the earlier Soak); soaks.size() = none.
        auto homeOf = [&](std::uint64_t guid)
        {
            std::size_t home = soaks.size();
            std::int64_t since = 0;
            for (std::size_t i = 0; i < soaks.size(); ++i)
                for (Claimant const& c : soaks[i].players)
                    if (c.guid == guid && c.bot && c.claimedSince != 0
                        && (home == soaks.size() || c.claimedSince < since))
                    {
                        home = i;
                        since = c.claimedSince;
                    }
            return home;
        };

        std::vector<std::vector<std::uint64_t>> out(soaks.size());
        std::vector<std::uint64_t> taken;
        for (std::size_t i = 0; i < soaks.size(); ++i)
        {
            std::vector<Claimant> pool;
            for (Claimant const& c : soaks[i].players)
            {
                if (std::find(taken.begin(), taken.end(), c.guid) != taken.end())
                    continue;
                std::size_t const home = homeOf(c.guid);
                if (!c.insideNow && home < soaks.size() && home > i)
                    continue;   // kept for its own, later Soak
                pool.push_back(c);
            }
            out[i] = SoakClaimers(pool, soaks[i].rule);
            taken.insert(taken.end(), out[i].begin(), out[i].end());
        }
        return out;
    }

    struct Point { std::uint64_t guid = 0; float x = 0.f, y = 0.f; };

    // The Stack(Cluster) leader: `sticky` while it is still listed, else the medoid (the player with the smallest
    // sum of distances to the others; ties on the lowest guid). 0 when there is nobody.
    inline std::uint64_t ClusterLeader(std::vector<Point> const& players, std::uint64_t sticky)
    {
        if (sticky != 0)
            for (Point const& p : players)
                if (p.guid == sticky)
                    return sticky;
        std::uint64_t best = 0;
        double bestSum = std::numeric_limits<double>::infinity();
        for (Point const& a : players)
        {
            double sum = 0.0;
            for (Point const& b : players)
                sum += std::hypot(static_cast<double>(a.x) - b.x, static_cast<double>(a.y) - b.y);
            if (sum < bestSum - 1e-6 || (std::fabs(sum - bestSum) <= 1e-6 && a.guid < best))
            {
                bestSum = sum;
                best = a.guid;
            }
        }
        return best;
    }

    // A carrier's slot (rotates the slot ring): its rank by guid among the carriers (the number of distinct carrier
    // guids below it), whether or not `me` is listed.
    inline std::uint32_t SlotOf(std::vector<std::uint64_t> carriers, std::uint64_t me)
    {
        std::sort(carriers.begin(), carriers.end());
        carriers.erase(std::unique(carriers.begin(), carriers.end()), carriers.end());
        return static_cast<std::uint32_t>(std::lower_bound(carriers.begin(), carriers.end(), me) - carriers.begin());
    }
}

#endif
