/*
 * mod-boss-mechanics: the per-bot cost counters (STAGE-M4-HANDOFF §3.6, WP-C).
 *
 * Pure header (std only): tests/bot_stats_test.cpp compiles it with the host compiler. The bot glue keeps one
 * CostStats per bot (MechanicBotState::cost) and fills it from the brain, a timing decorator around the navmesh oracle
 * and the waker; Sim::ReadBotStats copies it, `.mech bots` prints a summary. Times are wall-clock microseconds
 * (std::chrono::steady_clock): CPU cost on the map thread, valid as such in the sim (MapUpdate.Threads 0).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_COST_H
#define MOD_BOSS_MECHANICS_MECHANIC_COST_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace BossMechanics
{
    // A log2 histogram of microsecond samples: bucket 0 holds [0, 2) us, bucket b in 1..14 holds [2^b, 2^(b+1)) us,
    // bucket 15 everything from 32,768 us up. Count, sum and max are exact; quantiles are bucket upper bounds.
    struct UsHistogram
    {
        static constexpr std::size_t kBuckets = 16;

        std::array<std::uint32_t, kBuckets> buckets{};
        std::uint32_t count = 0;
        std::uint32_t max = 0;
        std::uint64_t total = 0;

        static std::size_t BucketOf(std::uint32_t us)
        {
            std::size_t b = 0;
            while (b + 1 < kBuckets && us >= (std::uint32_t(2) << b))
                ++b;
            return b;
        }

        // The exclusive upper bound of bucket b in us (the last bucket: UINT32_MAX).
        static std::uint32_t BucketUpper(std::size_t b)
        {
            return b + 1 >= kBuckets ? UINT32_MAX : (std::uint32_t(2) << b);
        }

        void Add(std::uint32_t us)
        {
            ++buckets[BucketOf(us)];
            ++count;
            max = std::max(max, us);
            total += us;
        }

        std::uint32_t Mean() const
        {
            return count ? static_cast<std::uint32_t>(total / count) : 0;
        }

        // The upper bound of the bucket holding the q-quantile (0 < q <= 1), capped at the exact max; 0 when empty.
        std::uint32_t Quantile(double q) const
        {
            if (!count)
                return 0;
            double const want = std::clamp(q, 0.0, 1.0) * count;
            std::uint64_t seen = 0;
            for (std::size_t b = 0; b < kBuckets; ++b)
            {
                seen += buckets[b];
                if (seen > 0 && static_cast<double>(seen) >= want)
                    return std::min(BucketUpper(b), max);
            }
            return max;
        }
    };

    // M4 cost measurement (STAGE-M4-HANDOFF §3.6, PLAN §7, D9). Per bot, summed since the last ResetBotStats.
    struct CostStats
    {
        // Navmesh oracle calls of every solve, route and tank re-facing search, each timed (count = calls).
        UsHistogram pathUs;             // PathOracle::Find (PathGenerator)
        UsHistogram probeUs;            // PathOracle::Walkable (findNearestPoly)
        UsHistogram sightUs;            // PathOracle::InSight (healer line of sight)
        // Lane frames (D9): candidates the lane frames put into the solver's candidate list (after dedup and the cap)
        // and the path calls the validation spent on them, over the solves that had a lane frame in view.
        std::uint32_t laneSolves = 0;
        std::uint32_t laneCandidates = 0;
        std::uint32_t lanePathCalls = 0;
        // Solve (allies' refresh included, as `.mech bots`) and route search wall time.
        UsHistogram solveUs;
        UsHistogram routeUs;
        // Path calls per mechanic (PLAN §7 "about 50 per bot per mechanic"): every solve and route adds its path
        // calls to each mechanic its plan answers; plans that answer none go to pathCallsUnattributed. A mechanic is
        // folded into the summary once it left the registry (FoldGone); PerMechanic() includes the live ones.
        std::unordered_map<std::uint32_t, std::uint32_t> livePathCalls;
        std::uint32_t mechanics = 0;            // folded mechanics
        std::uint32_t mechanicPathCallsMax = 0;
        std::uint64_t mechanicPathCallsTotal = 0;
        std::uint32_t pathCallsUnattributed = 0;
        // Brain: every brain step that did work (BotBrain::Update past its once-per-ms check, and the waker's
        // CastLeaveBy for a casting bot), this bot alone.
        UsHistogram brainUs;
        // The bot's map, per map update that ran the waker: the brain time of every bot on that map in that update,
        // summed (the same numbers on every bot of the map; read one).
        std::uint32_t mapUpdates = 0;
        std::uint32_t mapBrainUsMax = 0;
        std::uint64_t mapBrainUsTotal = 0;

        void AddMechanicPathCalls(std::uint32_t mechanicId, std::uint32_t calls)
        {
            livePathCalls[mechanicId] += calls;
        }

        // Folds the live mechanics `gone(id)` says are over into the summary.
        template <class Gone>
        void FoldGone(Gone&& gone)
        {
            for (auto it = livePathCalls.begin(); it != livePathCalls.end();)
            {
                if (!gone(it->first))
                {
                    ++it;
                    continue;
                }
                ++mechanics;
                mechanicPathCallsMax = std::max(mechanicPathCallsMax, it->second);
                mechanicPathCallsTotal += it->second;
                it = livePathCalls.erase(it);
            }
        }

        // Mechanics seen (folded + live), the most path calls one of them cost, and the mean.
        void PerMechanic(std::uint32_t& count, std::uint32_t& maxCalls, double& mean) const
        {
            count = mechanics;
            maxCalls = mechanicPathCallsMax;
            std::uint64_t total = mechanicPathCallsTotal;
            for (auto const& [id, calls] : livePathCalls)
            {
                ++count;
                maxCalls = std::max(maxCalls, calls);
                total += calls;
            }
            mean = count ? static_cast<double>(total) / count : 0.0;
        }

        void AddMapUpdate(std::uint32_t us)
        {
            ++mapUpdates;
            mapBrainUsMax = std::max(mapBrainUsMax, us);
            mapBrainUsTotal += us;
        }
    };
}

#endif
