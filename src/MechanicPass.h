/*
 * mod-boss-mechanics: the pass (Blackrock Spire build, WP-PS; shared doc BS6, library L26 Hot Potato, designer ruling
 * 57). The pure rules behind a boss descriptor's PassEntry (MechanicDescriptor.h): who holds it, who may catch it, the
 * pick among the catchers, when the holder throws, where a catcher bot stands to catch, and the per-map bookkeeping
 * of throws and catches. MechanicBotPass.cpp is the playerbots glue.
 *
 * The model:
 * - the **holder** is the player carrying the entry's holder aura (one at a time; a change of holder is a catch when it
 *   goes to the player the last throw aimed at);
 * - a **catcher** is any other live player of the holder's group whose CatchWhen holds (Scorched Hands keeps the
 *   thrower out), within the range of the holder (3D) and in its line of sight (unless NoSight); Prefer ranks some
 *   first (the wounded), then the score: the distance to the nearest FarFrom unit (capped at kFarCapYd) minus
 *   `zoneWeightYd` per counted zone (FewestZones) within `zoneYd`; ties to the lower guid;
 * - the **ready catchers** (ReadyCatchers, default 1) are the best-ranked catchers allowing a walk: out of range or
 *   sight costs, it does not exclude. Each ready catcher bot that stands on a poor spot (a counted zone near, a FarFrom
 *   unit within KeepFrom, the holder out of reach) walks to a reserved spot (SpotCandidates, SpotCost), and holds it;
 * - both picks are sticky: the current ready catcher keeps its place until another of the same preference scores
 *   kStickYd more (or it stops qualifying), so nobody is walked back and forth by a yard;
 * - the **throw** is due when any trigger fires (HeldFor, UnitNear, Casting, Mechanic), unless a minimum hold whose
 *   condition holds on the holder has not run out (an Urgent trigger goes through it) or the holder's last throw is
 *   still in flight. It goes to the first ready catcher when that one is valid now, else the best valid catcher;
 * - a throw whose flight ran out with the thrower still holding failed; two failures in a row at the same catcher
 *   leave that catcher out for kExcludeMs (the Vashj chain's "replan without the catcher" rule).
 *
 * No AzerothCore includes: tests/pass_test.cpp compiles this with the host compiler.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_PASS_H
#define MOD_BOSS_MECHANICS_MECHANIC_PASS_H

#include "MechanicDescriptor.h"
#include "MechanicGeometry.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace BossMechanics::Pass
{
    using Desc::Key;
    using Desc::Ms;

    constexpr float kFarCapYd = 40.f;       // FarFrom distance counts up to here (beyond: all the same)
    constexpr float kNoUnitYd = 1000.f;     // FarDistance with no unit
    constexpr float kStickYd = 5.f;         // a ready catcher keeps its place unless beaten by this much
    constexpr float kSightPenaltyYd = 15.f; // ready ranking: a catcher out of the holder's sight
    constexpr float kRangeMarginYd = 0.5f;  // throw pick: within range - this (the bots step while it flies)
    constexpr float kSpotRangeSlackYd = 4.f;    // a ready spot lies within range - this of the holder
    constexpr float kSpotApartYd = 4.f;     // ready spots of two catchers this far apart
    constexpr float kSpotArriveYd = 1.5f;   // at the spot
    constexpr float kZToleranceYd = 6.f;    // zones and spots on another floor do not count
    constexpr std::uint8_t kFailsToExclude = 2;
    constexpr Ms kExcludeMs{ 5000 };
    constexpr Ms kSpotHoldMs{ 8000 };       // a ready catcher holds its spot at most this long without a throw
    constexpr Ms kSpotKeepMs{ 6000 };       // a reserved spot is re-picked after this long (the room changed)

    // A counted zone (FewestZones), as a disc on the plane.
    struct Disc
    {
        Geo::Vec3 c;
        float r = 0.f;
    };

    // The zones whose disc comes within `yd` of p (edge distance on the plane; another floor does not count).
    inline std::uint32_t ZonesNear(std::vector<Disc> const& zones, Geo::Vec3 const& p, float yd)
    {
        std::uint32_t n = 0;
        for (Disc const& d : zones)
        {
            if (std::fabs(d.c.z - p.z) > kZToleranceYd)
                continue;
            if (Geo::Dist2d(d.c.x, d.c.y, p.x, p.y) - d.r <= yd)
                ++n;
        }
        return n;
    }

    // Yards from p to the nearest unit on the plane; kNoUnitYd when there is none.
    inline float FarDistance(std::vector<Geo::Vec3> const& units, Geo::Vec3 const& p)
    {
        float best = kNoUnitYd;
        for (Geo::Vec3 const& u : units)
            best = std::min(best, Geo::Dist2d(u.x, u.y, p.x, p.y));
        return best;
    }

    // Inside the entry's room (Room box and RoomCircle; neither set: everywhere).
    inline bool InRoom(PassEntry const& e, float x, float y)
    {
        if (e.roomBox && (x < e.roomMinX || x > e.roomMaxX || y < e.roomMinY || y > e.roomMaxY))
            return false;
        if (e.roomR > 0.f && Geo::Dist2d(e.roomX, e.roomY, x, y) > e.roomR)
            return false;
        return true;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // The pick
    // ----------------------------------------------------------------------------------------------------------------

    struct Candidate
    {
        Key guid = 0;
        Geo::Vec3 pos;
        bool eligible = true;       // CatchWhen holds (and alive, not the holder)
        bool preferred = false;     // Prefer holds
        bool excluded = false;      // two failed throws at it, or the throw now in flight is its own
        float holderYd = 0.f;       // distance to the holder (3D, edge to edge as the throw measures it)
        bool inSight = true;        // the holder sees it (always true with NoSight)
        bool bot = true;            // a bot (only bots walk to a ready spot)
    };

    struct Ranked
    {
        Key guid = 0;
        bool preferred = false;
        float score = 0.f;
    };

    // The catch score: FarFrom distance capped at kFarCapYd (0 without units), minus the zones' weight. Higher first.
    inline float Score(PassEntry const& e, Geo::Vec3 const& p, std::vector<Disc> const& zones,
        std::vector<Geo::Vec3> const& units)
    {
        float s = units.empty() ? 0.f : std::min(FarDistance(units, p), kFarCapYd);
        if (e.zoneTag)
            s -= e.zoneWeightYd * static_cast<float>(ZonesNear(zones, p, e.zoneYd));
        return s;
    }

    inline bool RankedLess(Ranked const& a, Ranked const& b)
    {
        if (a.preferred != b.preferred)
            return a.preferred;
        if (a.score != b.score)
            return a.score > b.score;
        return a.guid < b.guid;
    }

    // The catchers a throw may go to now (eligible, not excluded, within range - kRangeMarginYd, in sight), best first.
    inline std::vector<Ranked> RankThrow(PassEntry const& e, std::vector<Candidate> const& cands,
        std::vector<Disc> const& zones, std::vector<Geo::Vec3> const& units)
    {
        std::vector<Ranked> out;
        for (Candidate const& c : cands)
        {
            if (!c.guid || !c.eligible || c.excluded || c.holderYd > e.range - kRangeMarginYd
                || (e.sight && !c.inSight))
                continue;
            out.push_back({ c.guid, c.preferred, Score(e, c.pos, zones, units) });
        }
        std::sort(out.begin(), out.end(), RankedLess);
        return out;
    }

    // The ready ranking: every eligible, not excluded catcher; one out of range loses a yard of score per yard beyond
    // it, one out of sight kSightPenaltyYd (it can walk there). Best first.
    inline std::vector<Ranked> RankReady(PassEntry const& e, std::vector<Candidate> const& cands,
        std::vector<Disc> const& zones, std::vector<Geo::Vec3> const& units)
    {
        std::vector<Ranked> out;
        for (Candidate const& c : cands)
        {
            if (!c.guid || !c.eligible || c.excluded)
                continue;
            float s = Score(e, c.pos, zones, units);
            if (c.holderYd > e.range - kSpotRangeSlackYd)
                s -= c.holderYd - (e.range - kSpotRangeSlackYd);
            if (e.sight && !c.inSight)
                s -= kSightPenaltyYd;
            out.push_back({ c.guid, c.preferred, s });
        }
        std::sort(out.begin(), out.end(), RankedLess);
        return out;
    }

    // The sticky pick from a ranking: `current` keeps first place while it is ranked, not outranked by preference, and
    // within `stickYd` of the best score. 0: the ranking is empty.
    inline Key Pick(std::vector<Ranked> const& ranked, Key current, float stickYd = kStickYd)
    {
        if (ranked.empty())
            return 0;
        Ranked const& best = ranked.front();
        if (current && current != best.guid)
            for (Ranked const& r : ranked)
                if (r.guid == current)
                {
                    if (r.preferred == best.preferred && best.score - r.score <= stickYd)
                        return current;
                    break;
                }
        return best.guid;
    }

    // The first `n` ready catchers, each pick sticky to the one that held that place (`current`, in order).
    inline std::vector<Key> PickReady(std::vector<Ranked> ranked, std::vector<Key> const& current, std::uint8_t n,
        float stickYd = kStickYd)
    {
        std::vector<Key> out;
        for (std::uint8_t i = 0; i < n && !ranked.empty(); ++i)
        {
            Key const held = i < current.size() ? current[i] : 0;
            Key const k = Pick(ranked, held, stickYd);
            out.push_back(k);
            ranked.erase(std::remove_if(ranked.begin(), ranked.end(), [k](Ranked const& r) { return r.guid == k; }),
                ranked.end());
        }
        return out;
    }

    // The throw's target: the first ready catcher when it is valid now (it keeps its place within stickYd), else the
    // best valid catcher. 0: nobody can catch.
    inline Key ThrowTarget(std::vector<Ranked> const& valid, std::vector<Key> const& ready, float stickYd = kStickYd)
    {
        return Pick(valid, ready.empty() ? 0 : ready.front(), stickYd);
    }

    // ----------------------------------------------------------------------------------------------------------------
    // When to throw
    // ----------------------------------------------------------------------------------------------------------------

    enum class Why : std::uint8_t
    {
        None,
        Held,       // HeldFor
        UnitNear,
        Casting,
        Mechanic
    };

    inline Why WhyOf(PassTrigger::Kind k)
    {
        switch (k)
        {
            case PassTrigger::Kind::HeldFor:
                return Why::Held;
            case PassTrigger::Kind::UnitNear:
                return Why::UnitNear;
            case PassTrigger::Kind::Casting:
                return Why::Casting;
            case PassTrigger::Kind::Mechanic:
                return Why::Mechanic;
        }
        return Why::None;
    }

    inline char const* WhyName(Why w)
    {
        switch (w)
        {
            case Why::Held:
                return "held";
            case Why::UnitNear:
                return "unit near";
            case Why::Casting:
                return "casting";
            case Why::Mechanic:
                return "mechanic";
            default:
                return "-";
        }
    }

    struct Due
    {
        bool due = false;
        Why why = Why::None;
        bool urgent = false;
        std::size_t trigger = 0;    // the entry's trigger that made it due
        bool heldBack = false;      // a trigger fired but a minimum hold (or the flight) kept it
    };

    // `fired[i]`: trigger i fired now (HeldFor triggers are judged here from `held`, whatever fired says). `minHold`:
    // the longest minimum hold whose condition holds on the holder now (0: none). `inFlight`: the holder's last throw
    // is still flying.
    inline Due ThrowDue(PassEntry const& e, std::vector<bool> const& fired, Ms held, Ms minHold, bool inFlight)
    {
        Due d;
        for (std::size_t i = 0; i < e.triggers.size(); ++i)
        {
            PassTrigger const& t = e.triggers[i];
            bool const f = t.kind == PassTrigger::Kind::HeldFor ? held >= t.ms : i < fired.size() && fired[i];
            if (!f)
                continue;
            if (inFlight || (!t.urgent && held < minHold))
            {
                d.heldBack = true;
                continue;
            }
            d.due = true;
            d.why = WhyOf(t.kind);
            d.urgent = t.urgent;
            d.trigger = i;
            d.heldBack = false;
            return d;
        }
        return d;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Ready spots
    // ----------------------------------------------------------------------------------------------------------------

    // Is the spot fit to catch on: no counted zone within zoneYd, KeepFrom met, within range - kSpotRangeSlackYd of the
    // holder on the plane (and the same floor), inside the room.
    inline bool SpotFit(PassEntry const& e, Geo::Vec3 const& p, Geo::Vec3 const& holder, std::vector<Disc> const& zones,
        std::vector<Geo::Vec3> const& units)
    {
        if (!InRoom(e, p.x, p.y))
            return false;
        if (std::fabs(p.z - holder.z) > kZToleranceYd
            || Geo::Dist2d(p.x, p.y, holder.x, holder.y) > e.range - kSpotRangeSlackYd)
            return false;
        if (e.zoneTag && ZonesNear(zones, p, e.zoneYd) > 0)
            return false;
        if (e.keepFromYd > 0.f && !units.empty() && FarDistance(units, p) < e.keepFromYd)
            return false;
        return true;
    }

    // Where a ready catcher may stand: rings around the bot (3 to 15 yd, 12 bearings each) and around the holder (0.4,
    // 0.6 and 0.8 of the range, 16 bearings each), at the bot's height (the glue snaps them onto the navmesh).
    inline std::vector<Geo::Vec3> SpotCandidates(Geo::Vec3 const& bot, Geo::Vec3 const& holder, float range)
    {
        std::vector<Geo::Vec3> out;
        out.reserve(4 * 12 + 3 * 16);
        for (float r : { 3.f, 6.f, 10.f, 15.f })
            for (int i = 0; i < 12; ++i)
            {
                float const a = Geo::kTwoPi * static_cast<float>(i) / 12.f;
                out.push_back({ bot.x + r * std::cos(a), bot.y + r * std::sin(a), bot.z });
            }
        for (float f : { 0.4f, 0.6f, 0.8f })
            for (int i = 0; i < 16; ++i)
            {
                float const a = Geo::kTwoPi * static_cast<float>(i) / 16.f;
                out.push_back({ holder.x + f * range * std::cos(a), holder.y + f * range * std::sin(a), bot.z });
            }
        return out;
    }

    // The cost of a ready spot (lower first; infinity: never): the walk, 30 per counted zone near it, 5 per yard short
    // of KeepFrom, 20 per yard short of kSpotApartYd from another ready catcher's spot. Out of the room or out of the
    // holder's reach (range - kSpotRangeSlackYd, the same floor): never.
    inline float SpotCost(PassEntry const& e, Geo::Vec3 const& p, Geo::Vec3 const& bot, Geo::Vec3 const& holder,
        std::vector<Disc> const& zones, std::vector<Geo::Vec3> const& units, std::vector<Geo::Vec3> const& others)
    {
        constexpr float kInf = std::numeric_limits<float>::infinity();
        if (!InRoom(e, p.x, p.y) || std::fabs(p.z - holder.z) > kZToleranceYd
            || Geo::Dist2d(p.x, p.y, holder.x, holder.y) > e.range - kSpotRangeSlackYd)
            return kInf;
        float cost = Geo::Dist2d(p.x, p.y, bot.x, bot.y);
        if (e.zoneTag)
            cost += 30.f * static_cast<float>(ZonesNear(zones, p, e.zoneYd));
        if (e.keepFromYd > 0.f && !units.empty())
            cost += 5.f * std::max(0.f, e.keepFromYd - FarDistance(units, p));
        for (Geo::Vec3 const& o : others)
            cost += 20.f * std::max(0.f, kSpotApartYd - Geo::Dist2d(p.x, p.y, o.x, o.y));
        return cost;
    }

    // The finite-cost candidates, cheapest first (the glue probes them in this order).
    inline std::vector<Geo::Vec3> RankSpots(PassEntry const& e, Geo::Vec3 const& bot, Geo::Vec3 const& holder,
        std::vector<Disc> const& zones, std::vector<Geo::Vec3> const& units, std::vector<Geo::Vec3> const& others)
    {
        std::vector<std::pair<float, Geo::Vec3>> scored;
        for (Geo::Vec3 const& p : SpotCandidates(bot, holder, e.range))
        {
            float const c = SpotCost(e, p, bot, holder, zones, units, others);
            if (std::isfinite(c))
                scored.emplace_back(c, p);
        }
        std::stable_sort(scored.begin(), scored.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        std::vector<Geo::Vec3> out;
        out.reserve(scored.size());
        for (auto const& s : scored)
            out.push_back(s.second);
        return out;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Bookkeeping: one slot per pass entry, per map (MechanicBotPass.cpp keeps them)
    // ----------------------------------------------------------------------------------------------------------------

    struct Reserved
    {
        Key bot = 0;
        Geo::Vec3 spot;
        Ms at{ 0 };                 // reserved here
        Ms arrivedAt{ -1 };         // first seen at the spot (-1: not yet)
    };

    struct Slot
    {
        Key holder = 0;
        Ms heldSince{ -1 };         // the current holder got it here
        Key lastThrower = 0;
        Key lastTarget = 0;
        Ms lastThrowAt{ -1 };
        bool throwJudged = true;    // the last throw's outcome was counted
        std::uint8_t failsAtTarget = 0;
        std::vector<std::pair<Key, Ms>> excluded;   // catcher, until
        std::vector<Key> ready;      // the ready catchers, best first
        Key throwTo = 0;            // the throw due now (0: none)
        Due due;
        std::vector<Reserved> spots;
        // counters (`.mech bots`, `.mech desc`)
        std::uint32_t throws = 0;   // throws sent
        std::uint32_t catches = 0;  // the holder changed to the throw's target
        std::uint32_t failed = 0;   // a flight ran out with the thrower still holding
        std::uint32_t otherChanges = 0;     // the holder changed otherwise (a death, a script, a human's throw)
        std::uint32_t drops = 0;    // a holder lost it with nobody holding
    };

    inline bool Excluded(Slot const& s, Key k, Ms now)
    {
        for (auto const& [g, until] : s.excluded)
            if (g == k && now < until)
                return true;
        return false;
    }

    inline bool InFlight(Slot const& s, Key holder, Ms now, Ms flight)
    {
        return holder && s.lastThrower == holder && s.lastThrowAt >= Ms(0) && now < s.lastThrowAt + flight;
    }

    // The holder now (0: nobody). Counts catches, failed throws and drops, keeps heldSince, prunes the exclusions.
    inline void Observe(Slot& s, Key holderNow, Ms now, Ms flight)
    {
        s.excluded.erase(std::remove_if(s.excluded.begin(), s.excluded.end(), [now](auto const& x)
        {
            return now >= x.second;
        }), s.excluded.end());

        if (holderNow != s.holder)
        {
            if (holderNow && holderNow == s.lastTarget && !s.throwJudged)
            {
                ++s.catches;
                s.failsAtTarget = 0;
                s.throwJudged = true;
            }
            else if (holderNow)
                ++s.otherChanges;
            else
                ++s.drops;
            s.holder = holderNow;
            s.heldSince = holderNow ? now : Ms(-1);
            s.throwTo = 0;
            s.due = Due();
        }
        // A flight that ran out with the thrower still holding: a failed throw (two at one catcher: leave it out).
        if (!s.throwJudged && s.lastThrowAt >= Ms(0) && now >= s.lastThrowAt + flight)
        {
            s.throwJudged = true;
            if (s.holder == s.lastThrower)
            {
                ++s.failed;
                if (++s.failsAtTarget >= kFailsToExclude)
                {
                    s.excluded.emplace_back(s.lastTarget, now + kExcludeMs);
                    s.failsAtTarget = 0;
                }
            }
        }
    }

    // A throw was sent.
    inline void NoteThrow(Slot& s, Key thrower, Key target, Ms now)
    {
        if (target != s.lastTarget)
            s.failsAtTarget = 0;
        s.lastThrower = thrower;
        s.lastTarget = target;
        s.lastThrowAt = now;
        s.throwJudged = false;
        s.throwTo = 0;
        s.due = Due();
        ++s.throws;
    }

    inline Reserved* FindSpot(Slot& s, Key bot)
    {
        for (Reserved& r : s.spots)
            if (r.bot == bot)
                return &r;
        return nullptr;
    }

    // Keeps only the spots of the ready catchers.
    inline void PruneSpots(Slot& s)
    {
        s.spots.erase(std::remove_if(s.spots.begin(), s.spots.end(), [&s](Reserved const& r)
        {
            return std::find(s.ready.begin(), s.ready.end(), r.bot) == s.ready.end();
        }), s.spots.end());
    }

    // The other ready catchers' reserved spots (SpotCost's `others`).
    inline std::vector<Geo::Vec3> OtherSpots(Slot const& s, Key bot)
    {
        std::vector<Geo::Vec3> out;
        for (Reserved const& r : s.spots)
            if (r.bot != bot)
                out.push_back(r.spot);
        return out;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Per-bot decision and counters (MechanicBotState::pass)
    // ----------------------------------------------------------------------------------------------------------------

    enum class Role : std::uint8_t
    {
        None,
        Holder,     // holds it (a throw may be due)
        Ready       // a ready catcher
    };

    struct Decision
    {
        Role role = Role::None;
        std::size_t slot = 0;       // the descriptor's pass entry
        Key throwTo = 0;            // Holder: throw at this player now
        Why why = Why::None;
        bool move = false;          // Ready: walk to `spot`
        bool holdSpot = false;      // Ready: stand where it is (on its spot), the stock moves held off
        Geo::Vec3 spot;

        bool Active() const { return role != Role::None; }
    };

    struct BotState
    {
        Decision decision;
        Ms decidedAt{ -1 };
        bool entries = false;       // the active descriptor had pass entries at that time
        // counters (`.mech bots`)
        std::uint32_t throws = 0;   // throws sent by this bot
        std::uint32_t refused = 0;  // a throw it could not send (no item, the cast refused)
        std::uint32_t walks = 0;    // walks sent to a ready spot
        std::uint32_t spotsPicked = 0;
        std::uint32_t noSpot = 0;   // a ready catcher on a poor spot found no better one
        std::uint32_t blocked = 0;  // stock moves held off while it held its spot
        std::uint32_t castBreaks = 0;   // a cast broken for a due throw
    };
}

#endif
