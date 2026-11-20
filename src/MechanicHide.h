/*
 * mod-boss-mechanics: the Hide kind's pure rules (Blackrock Depths build WP-HK; hub BD1, library L31).
 *
 * Mechanic::Hide registers "stand where the source cannot see you": at the end of the warning everyone eligible within
 * range of the source whom the source can see is hit. The judge is real line of sight (WorldObject::IsWithinLOSInMap
 * from the source, which checks the map's dynamic tree, so a game object with a collision model blocks it: Bael'Gar's
 * Boulders, display 3711 Lavarock01). The blockers the script lists are for the bots only: the registry places each
 * eligible player on a spot behind one of them (Assign below; the engine checks every spot with a real ray and skips
 * spots on a lasting zone), and the bot view makes that spot a region and each blocker a small hazard to walk round.
 *
 * The bot rule generalizes mod-playerbots' hand-made hides (NaxxActions_Sapphiron.cpp: Sapphiron's ice blocks;
 * ICCActions_SG.cpp: Sindragosa's Frost Beacon tombs): stand behind the blocker on the far side from the source. No
 * code is copied from them: they pick a fixed spot per encounter, this places every player per cast.
 *
 * Placement (Assign):
 *   - every blocker has a capacity of max(share, ceil(players / blockers)) (BD1: a Boulder shelters 2 or 3, so
 *     players spread; too few blockers and they crowd behind the ones there are);
 *   - players take blockers greedily, the nearest (player, spot) pair first, ties to the lower guid then the lower
 *     blocker; a blocker whose own spot is bad (seen, on a lasting zone, off the floor) costs kBadSpotCostYd more per
 *     level of badness, so the players go to the good ones first; then moves and swaps lower the longest walk (the
 *     last player to reach cover sets the pace), then the total (AssignBlockers);
 *   - behind one blocker the players stand side by side across the shadow (SlotOffset), sorted by where they come
 *     from (their lateral side), so two never cross; each spot is behindYd past the blocker's far edge, on the line
 *     from the source through the blocker's centre, then tried closer and farther (SpotCandidates) until a spot is
 *     good.
 *
 * Pure header: no AzerothCore includes (tests/hide_test.cpp compiles it with the host compiler).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_HIDE_H
#define MOD_BOSS_MECHANICS_MECHANIC_HIDE_H

#include "MechanicGeometry.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace BossMechanics::Hide
{
    // ---- defaults (HideSpec, MechanicRegistry.h) ---------------------------------------------------------------
    // Only players within this of the source on the plane (and within the shape's zTolerance of its height) are
    // judged: the Hide's shape is a circle of the range on the source (Mechanic::Hide), so `.ZTolerance()` sets the
    // height band.
    constexpr float kDefaultRangeYd = 60.f;
    constexpr float kMaxRangeYd = 95.f;                 // the resolve spell's area search (100 yd, less the slack)
    constexpr float kDefaultBehindYd = 3.f;             // BD1: about 3 yd behind the blocker
    constexpr float kDefaultSpotRadiusYd = 1.5f;        // the bots' region round a spot
    constexpr float kDefaultBlockerRadiusYd = 2.5f;     // Lavarock01 (display 3711) is about 5 by 5 yd
    constexpr std::uint8_t kDefaultShare = 2;           // BD1: a Boulder shelters 2 or 3 players
    constexpr float kDefaultZToleranceYd = 10.f;        // a cavern's floor; line of sight keeps other floors out
    // A point check's target height above the floor (a player's chest, about a human's collision height). The judge
    // at the resolve uses each player's own height (IsWithinLOSInMap); this only serves the placement's rays and
    // ContainsPoint for a bare point.
    constexpr float kTargetHeightYd = 2.f;
    // The resolve and the placement judge from the source's centre: 0 yd of reach, so a giant standing next to a
    // blocker does not start its ray past it (the core's default for spells is the caster's combat reach).
    constexpr float kDefaultReachYd = 0.f;
    // Side by side behind one blocker: at most this far apart (closer for a narrow blocker, SlotOffset).
    constexpr float kLateralStepYd = 1.5f;
    // The fraction of the blocker's width the slots may span (the shadow is wider than the blocker behind it).
    constexpr float kSlotSpanFraction = 0.8f;
    // The source moved this far since the spots were placed: place them again (the tank drags the boss a little).
    constexpr float kReplaceDriftYd = 2.f;
    // The bots keep this far off a blocker's footprint when they walk (a small hazard, MechanicBotView.cpp).
    constexpr float kBlockerMarginYd = 0.5f;
    // A placement cost per level of badness (SpotBadness): above any walk in a room.
    constexpr float kBadSpotCostYd = 100.f;
    // Displays (HANDOFF §10): the Boulder's display, and the fallback when a summoned 3711 does not block sight.
    constexpr std::uint32_t kDisplayLavaRock = 3711;           // Lavarock01.m2
    constexpr std::uint32_t kDisplayLavaRockFallback = 5736;   // Lavarock02.m2

    constexpr std::uint32_t kNoBlocker = std::numeric_limits<std::uint32_t>::max();
    // `.mech test hide blockers=`: at most this many blockers summoned.
    constexpr std::uint32_t kTestMaxBlockers = 8;

    // How bad a candidate spot is (the engine's verdict on it; lower is better). Assign takes the best candidate.
    enum class SpotBadness : std::uint8_t
    {
        Good = 0,           // out of the source's sight, on open floor
        OnLastingZone = 1,  // hidden, but on a lasting zone (a lava crack): the bot would stand in it
        Seen = 2,           // the source sees it (a giant over a low rock, a bad angle)
        OffFloor = 3        // no floor there (a wall, a lava channel)
    };

    struct Blocker
    {
        Geo::Vec3 c;
        float r = kDefaultBlockerRadiusYd;
    };

    struct Seeker
    {
        std::uint64_t guid = 0;
        Geo::Vec3 p;
    };

    struct Params
    {
        float behindYd = kDefaultBehindYd;
        std::uint8_t share = kDefaultShare;
        float lateralStepYd = kLateralStepYd;
    };

    struct Placement
    {
        std::uint64_t guid = 0;
        std::uint32_t blocker = kNoBlocker;     // index into the blockers given
        std::uint8_t slot = 0;                  // 0 .. of - 1, across the shadow from right to left
        std::uint8_t of = 0;                    // players behind that blocker
        Geo::Vec3 spot;
        SpotBadness badness = SpotBadness::Good;
    };

    // The players one blocker takes: share, or more when there are too few blockers for everyone.
    inline std::uint32_t Capacity(std::size_t seekers, std::size_t blockers, std::uint8_t share)
    {
        if (blockers == 0)
            return 0;
        std::size_t const even = (seekers + blockers - 1) / blockers;
        return static_cast<std::uint32_t>(std::max<std::size_t>({ even, share, 1 }));
    }

    // The unit direction from the source to the blocker's centre on the plane, and its left normal. A blocker on top
    // of the source (closer than 0.01 yd) faces +x.
    inline void Axes(Geo::Vec3 const& source, Blocker const& b, float& dx, float& dy, float& lx, float& ly)
    {
        float const vx = b.c.x - source.x;
        float const vy = b.c.y - source.y;
        float const len = std::hypot(vx, vy);
        dx = len > 0.01f ? vx / len : 1.f;
        dy = len > 0.01f ? vy / len : 0.f;
        lx = -dy;
        ly = dx;
    }

    // The spot `behind` yd past the blocker's far edge on the line from the source through its centre, moved
    // `lateral` yd to the left of that line (z = the blocker's; the engine puts it on the floor).
    inline Geo::Vec3 SpotBehind(Geo::Vec3 const& source, Blocker const& b, float behind, float lateral)
    {
        float dx, dy, lx, ly;
        Axes(source, b, dx, dy, lx, ly);
        float const along = b.r + std::max(0.f, behind);
        return { b.c.x + dx * along + lx * lateral, b.c.y + dy * along + ly * lateral, b.c.z };
    }

    // The lateral offset of slot `slot` of `of` behind a blocker of radius r: evenly spaced and centred, at most
    // `step` apart and spanning at most kSlotSpanFraction of the blocker's width.
    inline float SlotOffset(std::uint8_t slot, std::uint8_t of, float r, float step = kLateralStepYd)
    {
        if (of < 2)
            return 0.f;
        float const span = 2.f * r * kSlotSpanFraction;
        float const s = std::min(step, span / static_cast<float>(of - 1));
        return (static_cast<float>(slot) - static_cast<float>(of - 1) * 0.5f) * s;
    }

    // The candidate spots for one slot, best first: the slot's lateral offset at behindYd, then closer (half), then
    // farther (+ 1.5 yd), then the same three straight behind the centre (a slot whose side the source sees).
    inline std::vector<Geo::Vec3> SpotCandidates(Geo::Vec3 const& source, Blocker const& b, float behindYd,
        float lateral)
    {
        std::vector<Geo::Vec3> out;
        float const depths[] = { behindYd, behindYd * 0.5f, behindYd + 1.5f };
        for (float const d : depths)
            out.push_back(SpotBehind(source, b, d, lateral));
        if (std::fabs(lateral) > 0.01f)
            for (float const d : depths)
                out.push_back(SpotBehind(source, b, d, 0.f));
        return out;
    }

    // Pure plane shadow: the source's ray to `p` passes within the blocker's radius of its centre, and `p` lies
    // beyond the centre along that ray. A source inside the footprint casts no shadow. The engine judges with real
    // line of sight; this is the placement's default when it has none (tests) and the drill's desk check.
    inline bool InShadow2d(Geo::Vec3 const& source, Blocker const& b, Geo::Vec3 const& p)
    {
        float const sx = b.c.x - source.x;
        float const sy = b.c.y - source.y;
        if (std::hypot(sx, sy) <= b.r)
            return false;
        float const px = p.x - source.x;
        float const py = p.y - source.y;
        float const len = std::hypot(px, py);
        if (len <= 0.f)
            return false;
        float const along = (sx * px + sy * py) / len;     // the centre's projection on the ray
        if (along <= 0.f || len < along)
            return false;
        float const across = std::fabs(px * sy - py * sx) / len;
        return across <= b.r;
    }

    // In the shadow of any of the blockers.
    inline bool InAnyShadow2d(Geo::Vec3 const& source, std::vector<Blocker> const& blockers, Geo::Vec3 const& p)
    {
        for (Blocker const& b : blockers)
            if (InShadow2d(source, b, p))
                return true;
        return false;
    }

    // Who goes behind which blocker, from cost[s * blockers + b] (the walk plus the badness), at most `cap` a blocker
    // (cap * blockers >= seekers: Capacity). Greedy first (the cheapest pair, ties to the lower guid then the lower
    // blocker), then single moves and pairwise swaps while they lower the worst cost (the last one to cover sets the
    // pace), then the total; at most kImproveSweeps sweeps. Deterministic.
    constexpr std::uint32_t kImproveSweeps = 32;

    inline std::vector<std::uint32_t> AssignBlockers(std::vector<float> const& cost, std::size_t seekers,
        std::size_t blockers, std::uint32_t cap, std::vector<Seeker> const& who)
    {
        std::vector<std::uint32_t> of(seekers, 0);
        std::vector<std::uint32_t> count(blockers, 0);
        struct Pair
        {
            float cost;
            std::uint64_t guid;
            std::uint32_t blocker;
            std::uint32_t seeker;
        };
        std::vector<Pair> pairs;
        pairs.reserve(seekers * blockers);
        for (std::uint32_t s = 0; s < seekers; ++s)
            for (std::uint32_t b = 0; b < blockers; ++b)
                pairs.push_back({ cost[s * blockers + b], who[s].guid, b, s });
        std::sort(pairs.begin(), pairs.end(), [](Pair const& a, Pair const& b)
        {
            return std::tie(a.cost, a.guid, a.blocker) < std::tie(b.cost, b.guid, b.blocker);
        });
        std::vector<bool> done(seekers, false);
        for (Pair const& p : pairs)
        {
            if (done[p.seeker] || count[p.blocker] >= cap)
                continue;
            done[p.seeker] = true;
            of[p.seeker] = p.blocker;
            ++count[p.blocker];
        }

        // (worst, total) of an assignment, compared lexicographically with a small tolerance.
        auto score = [&](std::vector<std::uint32_t> const& a)
        {
            float worst = 0.f;
            float total = 0.f;
            for (std::size_t s = 0; s < seekers; ++s)
            {
                float const c = cost[s * blockers + a[s]];
                worst = std::max(worst, c);
                total += c;
            }
            return std::pair<float, float>(worst, total);
        };
        auto better = [](std::pair<float, float> const& a, std::pair<float, float> const& b)
        {
            constexpr float kTol = 1e-3f;
            if (a.first < b.first - kTol)
                return true;
            return a.first <= b.first + kTol && a.second < b.second - kTol;
        };
        std::pair<float, float> best = score(of);
        for (std::uint32_t sweep = 0; sweep < kImproveSweeps; ++sweep)
        {
            bool improved = false;
            for (std::uint32_t s = 0; s < seekers; ++s)
            {
                for (std::uint32_t b = 0; b < blockers; ++b)
                {
                    if (b == of[s] || count[b] >= cap)
                        continue;
                    std::uint32_t const was = of[s];
                    of[s] = b;
                    std::pair<float, float> const tried = score(of);
                    if (better(tried, best))
                    {
                        --count[was];
                        ++count[b];
                        best = tried;
                        improved = true;
                    }
                    else
                        of[s] = was;
                }
                for (std::uint32_t t = s + 1; t < seekers; ++t)
                {
                    if (of[s] == of[t])
                        continue;
                    std::swap(of[s], of[t]);
                    std::pair<float, float> const tried = score(of);
                    if (better(tried, best))
                    {
                        best = tried;
                        improved = true;
                    }
                    else
                        std::swap(of[s], of[t]);
                }
            }
            if (!improved)
                break;
        }
        return of;
    }

    // Places every seeker behind a blocker. `badness(spot)` is the engine's verdict on a candidate spot (SpotBadness;
    // the tests pass the plane shadow). One Placement per seeker, in the seekers' order; blocker kNoBlocker for all
    // when there is no blocker.
    template <class Badness>
    std::vector<Placement> Assign(Geo::Vec3 const& source, std::vector<Blocker> const& blockers,
        std::vector<Seeker> const& seekers, Params const& params, Badness&& badness)
    {
        std::vector<Placement> out(seekers.size());
        for (std::size_t i = 0; i < seekers.size(); ++i)
            out[i].guid = seekers[i].guid;
        if (blockers.empty() || seekers.empty())
            return out;

        // How bad each blocker's own spot is (straight behind it): the greedy pass prefers the good ones.
        std::vector<std::uint8_t> blockerBad(blockers.size(), 0);
        for (std::size_t b = 0; b < blockers.size(); ++b)
        {
            std::uint8_t best = static_cast<std::uint8_t>(SpotBadness::OffFloor);
            for (Geo::Vec3 const& c : SpotCandidates(source, blockers[b], params.behindYd, 0.f))
                best = std::min(best, static_cast<std::uint8_t>(badness(c)));
            blockerBad[b] = best;
        }

        // The cost of each (seeker, blocker): the walk to the spot straight behind it, plus the blocker's badness.
        std::size_t const n = seekers.size();
        std::size_t const nb = blockers.size();
        std::vector<float> cost(n * nb);
        for (std::size_t s = 0; s < n; ++s)
            for (std::size_t b = 0; b < nb; ++b)
            {
                Geo::Vec3 const spot = SpotBehind(source, blockers[b], params.behindYd, 0.f);
                cost[s * nb + b] = Geo::Dist2d(seekers[s].p.x, seekers[s].p.y, spot.x, spot.y)
                    + kBadSpotCostYd * blockerBad[b];
            }
        std::uint32_t const cap = Capacity(n, nb, params.share);
        std::vector<std::uint32_t> const pick = AssignBlockers(cost, n, nb, cap, seekers);

        std::vector<std::vector<std::uint32_t>> taken(nb);
        for (std::uint32_t s = 0; s < n; ++s)
            taken[pick[s]].push_back(s);

        for (std::uint32_t b = 0; b < blockers.size(); ++b)
        {
            std::vector<std::uint32_t>& group = taken[b];
            if (group.empty())
                continue;
            float dx, dy, lx, ly;
            Axes(source, blockers[b], dx, dy, lx, ly);
            // Right to left across the shadow (slot 0 is the rightmost), by where each player comes from.
            std::sort(group.begin(), group.end(), [&](std::uint32_t a, std::uint32_t c)
            {
                float const la = (seekers[a].p.x - blockers[b].c.x) * lx + (seekers[a].p.y - blockers[b].c.y) * ly;
                float const lc = (seekers[c].p.x - blockers[b].c.x) * lx + (seekers[c].p.y - blockers[b].c.y) * ly;
                return std::tie(la, seekers[a].guid) < std::tie(lc, seekers[c].guid);
            });
            std::uint8_t const of = static_cast<std::uint8_t>(std::min<std::size_t>(group.size(), 255));
            for (std::uint8_t slot = 0; slot < of; ++slot)
            {
                Placement& pl = out[group[slot]];
                pl.blocker = b;
                pl.slot = slot;
                pl.of = of;
                float const lateral = SlotOffset(slot, of, blockers[b].r, params.lateralStepYd);
                std::vector<Geo::Vec3> const cands = SpotCandidates(source, blockers[b], params.behindYd, lateral);
                pl.spot = cands.front();
                pl.badness = SpotBadness::OffFloor;
                for (Geo::Vec3 const& c : cands)
                {
                    SpotBadness const v = badness(c);
                    if (static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(pl.badness))
                    {
                        pl.badness = v;
                        pl.spot = c;
                    }
                    if (v == SpotBadness::Good)
                        break;
                }
            }
        }
        return out;
    }

    // The placement's default verdict without an engine: Good in a blocker's plane shadow, else Seen.
    inline auto PlaneBadness(Geo::Vec3 const& source, std::vector<Blocker> const& blockers)
    {
        return [&source, &blockers](Geo::Vec3 const& p)
        {
            return InAnyShadow2d(source, blockers, p) ? SpotBadness::Good : SpotBadness::Seen;
        };
    }

    // Should the registry place the spots again before the resolve? A blocker gone (or a new one), a change in the
    // players judged, or the source moved kReplaceDriftYd since the last placement.
    inline bool NeedsReplace(std::size_t blockersBefore, std::size_t blockersNow, bool seekersChanged,
        float sourceDriftYd)
    {
        return blockersBefore != blockersNow || seekersChanged || sourceDriftYd >= kReplaceDriftYd;
    }
}

#endif
