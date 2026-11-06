/*
 * mod-boss-mechanics: pure timing and number helpers (STAGE-M1-HANDOFF §3.4, §3.7, §3.8, §2.6).
 *
 * No AzerothCore includes (tests/math_test.cpp compiles this with the host compiler). Covers: travelling projectile
 * timing and swept bands, mine arming and grace, the mine cap, room-frame lanes and crossings, the Weakened threat
 * compensator, reference health interpolation, per-target flat damage, and the marker visual pick (ladder rounding).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_MATH_H
#define MOD_BOSS_MECHANICS_MECHANIC_MATH_H

#include "BossMechanicsIds.h"
#include "MechanicGeometry.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace BossMechanics
{
    using Ms = std::chrono::milliseconds;

    // ------------------------------------------------------------------------------------------------------------
    // Tunables (handoff §7: each recommendation is a constant here, easy to flip)
    // ------------------------------------------------------------------------------------------------------------
    namespace Rules
    {
        // §7.1: a drawn line is never shorter than its hitbox (its last piece up to 9% longer, 19% below aspect 1).
        // false = nearest ladder step.
        constexpr bool kLineNeverShorter = true;
        // §3.8: a donut draws the nearest ladder ratio <= inner/outer (false, harness ruling 2026-11-02): the drawn
        // safe hole is never bigger than the real one. true = the nearest ratio >= inner/outer (the handoff's text).
        constexpr bool kDonutRatioRoundUp = false;
        // Weakened from failures that are not resolve hits (MissedStack, MissedBait, Moved, OffSpot). false (harness
        // ruling 2026-11-02): only hits the resolve spell lands apply Weakened; the others go to the ledger only.
        constexpr bool kWeakenedOnNonHitFailures = false;
        // HoldStill: a listed player who moved more than this during the window posts Moved.
        constexpr float kHoldStillToleranceYd = 0.3f;
        // Spot: a listed player farther than this from its spot posts OffSpot (default; the API takes a radius).
        constexpr float kDefaultSpotRadius = 2.f;
        // Resolve search radius slack on top of Bounds(shape).
        constexpr float kSearchSlackYd = 2.f;
        // Encounter event log cap (the ledger counts are never capped).
        constexpr std::size_t kMaxEventsPerEncounter = 1024;
        // A tracked projectile that moves back by more than this along the line has turned around.
        constexpr float kTrackedReversalYd = 0.05f;
        // RunOut From(Owner): the hitbox ring sits on the owner; this small ring (the mechanic's color) follows the
        // targeted player so the target knows it is the one who must run (review M3, harness ruling 2026-11-02).
        constexpr float kRunOutTargetMarkerRadiusYd = 1.5f;
        // Telegraph fill: a fill piece smaller than this (yd) is not drawn yet (no near-zero creature scale).
        constexpr float kFillMinScaleYd = 0.1f;
        // Telegraph fill pacing (round 4b): never step the fill more often than this (ms), whatever FillStepYd asks.
        constexpr std::uint32_t kFillMinStepMs = 50;
        // ShrinkProjectileLines: the drawn start moves in steps of the line width, but never smaller than this (yd).
        constexpr float kShrinkMinStepYd = 1.f;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Resolve search radius
    // ------------------------------------------------------------------------------------------------------------

    // The resolve spell's area search keeps a unit only when its 3D distance from the caster is within the radius
    // (Spell.cpp, SRC area search). Every point Geo::Contains accepts lies within Bounds horizontally and zTolerance
    // vertically of the pose, so this radius (slack added horizontally) covers the whole hitbox from a caster at
    // (cx, cy, cz), including one that trails the anchor or stands on another level.
    inline float ResolveSearchRadius(Shape const& shape, Geo::Pose const& pose, float cx, float cy, float cz)
    {
        float const horizontal = Geo::Bounds(shape) + Rules::kSearchSlackYd + Geo::Dist2d(pose.x, pose.y, cx, cy);
        float const vertical = shape.zTolerance + std::fabs(cz - pose.z);
        return std::hypot(horizontal, vertical);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Travelling projectile
    // ------------------------------------------------------------------------------------------------------------

    // The projectile is a body of half-length h centred on c(e), travelling along the line's s axis from 0.
    // Single pass: c = v*e, it keeps going until its tail clears `length`.
    // Out and back: c = v*e up to `length`, waits `pause` there, comes back to 0 and keeps going until its tail clears
    // 0. Pass 0 is everything before the return starts (including the pause), pass 1 the return.
    struct ProjectileParams
    {
        float length = 0.f;             // yd
        float speed = 0.f;              // yd/s; <= 0: the whole line at once
        float halfLength = 0.f;         // h, yd
        bool outAndBack = false;
        Ms pause{ 0 };
    };

    inline float SecondsOf(Ms t)
    {
        return static_cast<float>(t.count()) / 1000.f;
    }

    inline Ms MsOf(float seconds)
    {
        return Ms(static_cast<std::int64_t>(std::ceil(seconds * 1000.f - 1e-3f)));
    }

    // Time of the turnaround (start of pass 1), relative to resolveAt.
    inline float ProjectileReturnStart(ProjectileParams const& p)
    {
        return p.speed > 0.f ? p.length / p.speed + SecondsOf(p.pause) : 0.f;
    }

    // Time until the last pass has cleared the line, relative to resolveAt.
    inline Ms ProjectileDuration(ProjectileParams const& p)
    {
        if (p.speed <= 0.f)
            return Ms(0);
        float t = (p.length + p.halfLength) / p.speed;
        if (p.outAndBack)
            t += ProjectileReturnStart(p);
        return MsOf(t);
    }

    // Centre of the body at elapsed seconds e, and the pass it belongs to.
    inline float ProjectileCentre(ProjectileParams const& p, float e, std::uint32_t* pass = nullptr)
    {
        if (pass)
            *pass = 0;
        if (p.speed <= 0.f)
            return 0.f;
        if (!p.outAndBack)
            return p.speed * e;
        float const tOut = p.length / p.speed;
        float const tBack = tOut + SecondsOf(p.pause);
        if (e < tBack)
            return std::min(p.speed * e, p.length);
        if (pass)
            *pass = 1;
        return p.length - p.speed * (e - tBack);
    }

    struct SweepBand
    {
        std::uint32_t pass = 0;
        float lo = 0.f;     // s range the body covered during the interval (inclusive)
        float hi = 0.f;
    };

    // The s ranges the body covered between two elapsed times (ms since resolveAt, prev <= now). Every point the body
    // passed over in the interval is inside a band, however long the interval: a slow map tick never skips a player.
    inline std::vector<SweepBand> SweptBands(ProjectileParams const& p, Ms prev, Ms now)
    {
        std::vector<SweepBand> out;
        if (p.speed <= 0.f)
        {
            out.push_back({ 0, -p.halfLength, p.length + p.halfLength });
            return out;
        }
        float const a = std::max(0.f, SecondsOf(prev));
        float const b = std::min(SecondsOf(now), SecondsOf(ProjectileDuration(p)) + 1e-3f);
        if (b < a)
            return out;

        auto addSegment = [&](std::uint32_t pass, float segStart, float segEnd)
        {
            float const s0 = std::max(a, segStart);
            float const s1 = std::min(b, segEnd);
            if (s1 < s0)
                return;
            float const c0 = ProjectileCentre(p, s0);
            float const c1 = ProjectileCentre(p, s1);
            out.push_back({ pass, std::min(c0, c1) - p.halfLength, std::max(c0, c1) + p.halfLength });
        };

        if (!p.outAndBack)
        {
            addSegment(0, 0.f, 1e9f);
            return out;
        }
        float const tBack = ProjectileReturnStart(p);
        addSegment(0, 0.f, std::nextafter(tBack, 0.f));
        addSegment(1, tBack, 1e9f);
        return out;
    }

    // Tracked mode: the script moves a real unit; c is its projected s each tick. Pass 0 until it turns around.
    inline std::vector<SweepBand> TrackedSweep(float prevS, float nowS, float halfLength, bool outAndBack,
        std::uint32_t& pass)
    {
        std::vector<SweepBand> out;
        if (outAndBack && pass == 0 && nowS < prevS - Rules::kTrackedReversalYd)
        {
            out.push_back({ 0, prevS - halfLength, prevS + halfLength });
            pass = 1;
        }
        out.push_back({ pass, std::min(prevS, nowS) - halfLength, std::max(prevS, nowS) + halfLength });
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Hot windows (bot-facing: "when is this point dangerous")
    // ------------------------------------------------------------------------------------------------------------
    struct HotWindow
    {
        Ms from{ 0 };
        Ms to{ 0 };
        std::uint32_t pass = 0;
        bool graceAllowed = false;      // armed mine: crossing is allowed while under the grace time
    };

    // Projectile windows for the point at s, relative to resolveAt.
    inline std::vector<HotWindow> ProjectileHotWindows(ProjectileParams const& p, float s)
    {
        std::vector<HotWindow> out;
        float const h = p.halfLength;
        if (p.speed <= 0.f)
        {
            out.push_back({ Ms(0), Ms(0), 0, false });
            return out;
        }
        float const v = p.speed;
        if (!p.outAndBack)
        {
            out.push_back({ MsOf(std::max(0.f, (s - h) / v)), MsOf((s + h) / v), 0, false });
            return out;
        }
        float const tOut = p.length / v;
        float const tBack = tOut + SecondsOf(p.pause);
        float const outFrom = std::max(0.f, (s - h) / v);
        float const outTo = (s + h >= p.length) ? tBack : std::min((s + h) / v, tOut);
        out.push_back({ MsOf(outFrom), MsOf(outTo), 0, false });
        float const backFrom = tBack + std::max(0.f, (p.length - s - h) / v);
        float const backTo = tBack + (p.length - s + h) / v;
        out.push_back({ MsOf(backFrom), MsOf(backTo), 1, false });
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Mine: arms at armAt; armed, a unit inside for >= grace (or anyone inside at the arming moment) detonates it
    // ------------------------------------------------------------------------------------------------------------
    template <class Key, class Hash = std::hash<Key>>
    struct MineTimer
    {
        enum class Step : std::uint8_t
        {
            None,
            Armed,          // this step armed it (swap the marker); nobody was inside
            Detonate        // detonate now (resolve everything inside), then the mine is spent
        };

        Ms armAt{ 0 };
        Ms grace{ 0 };
        bool armed = false;
        bool spent = false;
        std::unordered_map<Key, Ms, Hash> insideSince;     // per unit: first tick seen inside; erased on exit

        // `inside`: the eligible units inside the shape now.
        Step Update(Ms now, std::vector<Key> const& inside)
        {
            if (spent)
                return Step::None;
            if (!armed)
            {
                if (now < armAt)
                    return Step::None;
                armed = true;
                insideSince.clear();
                if (!inside.empty())
                {
                    spent = true;
                    return Step::Detonate;
                }
                return Step::Armed;
            }

            for (auto it = insideSince.begin(); it != insideSince.end();)
            {
                if (std::find(inside.begin(), inside.end(), it->first) == inside.end())
                    it = insideSince.erase(it);
                else
                    ++it;
            }
            for (Key const& k : inside)
            {
                auto const res = insideSince.emplace(k, now);
                if (now - res.first->second >= grace)
                {
                    spent = true;
                    return Step::Detonate;
                }
            }
            return Step::None;
        }
    };

    // Cap: given the live mines of one cap group as (createdAt, id), the ids that must fizzle (oldest first) so at
    // most `cap` remain. Ties on createdAt break by id.
    template <class Id>
    std::vector<Id> CapOverflow(std::vector<std::pair<Ms, Id>> live, std::size_t cap)
    {
        std::vector<Id> out;
        if (cap == 0 || live.size() <= cap)
            return out;
        std::sort(live.begin(), live.end());
        for (std::size_t i = 0; i < live.size() - cap; ++i)
            out.push_back(live[i].second);
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Room frame and lanes
    // ------------------------------------------------------------------------------------------------------------
    enum class Axis : std::uint8_t
    {
        X,      // lanes run along the frame's X axis (offsets across Y)
        Y       // lanes run along the frame's Y axis (offsets across X)
    };

    // origin = one corner; X runs along `angle` for sizeX, Y runs along angle + 90 degrees for sizeY.
    struct Frame
    {
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        float angle = 0.f;
        float sizeX = 0.f;
        float sizeY = 0.f;
    };

    // Frame coordinates (fx along X, fy along Y) to world.
    inline Geo::Pose FramePoint(Frame const& f, float fx, float fy, float facing)
    {
        float const c = std::cos(f.angle);
        float const s = std::sin(f.angle);
        return { f.x + c * fx - s * fy, f.y + s * fx + c * fy, f.z, facing };
    }

    struct LaneRect
    {
        Axis axis = Axis::X;
        float offset = 0.f;     // low edge across the axis, frame coordinates
        float width = 0.f;
    };

    // A lane as a Line shape: pose at the lane's start on its centreline, facing along it, and its length.
    struct LanePlacement
    {
        Geo::Pose pose;
        float length = 0.f;
        float width = 0.f;
    };

    inline LanePlacement PlaceLane(Frame const& f, LaneRect const& lane)
    {
        float const mid = lane.offset + lane.width * 0.5f;
        if (lane.axis == Axis::X)
            return { FramePoint(f, 0.f, mid, f.angle), f.sizeX, lane.width };
        return { FramePoint(f, mid, 0.f, f.angle + Geo::kPi * 0.5f), f.sizeY, lane.width };
    }

    inline float FrameSpanAcross(Frame const& f, Axis axis)
    {
        return axis == Axis::X ? f.sizeY : f.sizeX;
    }

    // `count` lanes of `width` across `span`, random offsets, gaps between neighbours >= minGap. Uniform over the
    // valid placements (no rejection loop). Infeasible (they do not fit): even spacing and *fellBack = true.
    // rand01() returns a float in [0, 1).
    template <class Rand01>
    std::vector<float> RandomLaneOffsets(float span, std::uint32_t count, float width, float minGap, Rand01&& rand01,
        bool* fellBack = nullptr)
    {
        std::vector<float> out;
        if (fellBack)
            *fellBack = false;
        if (count == 0)
            return out;
        float const slack = span - count * width - (count - 1) * minGap;
        if (slack < 0.f)
        {
            if (fellBack)
                *fellBack = true;
            float const step = count > 1 ? (span - width) / (count - 1) : 0.f;
            for (std::uint32_t i = 0; i < count; ++i)
                out.push_back(count > 1 ? std::max(0.f, step * i) : std::max(0.f, (span - width) * 0.5f));
            return out;
        }
        std::vector<float> u(count);
        for (float& x : u)
            x = std::clamp(static_cast<float>(rand01()), 0.f, 1.f) * slack;
        std::sort(u.begin(), u.end());
        for (std::uint32_t i = 0; i < count; ++i)
            out.push_back(u[i] + i * (width + minGap));
        return out;
    }

    // Lanes of `width` covering `span` edge to edge (the last one clamped inside), and one random free index.
    template <class Rand01>
    std::vector<float> CoverLaneOffsets(float span, float width, Rand01&& rand01, std::uint32_t& freeIndex)
    {
        std::vector<float> out;
        freeIndex = 0;
        if (width <= 0.f || span <= 0.f)
            return out;
        std::uint32_t const n = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(span / width - 1e-4f)));
        for (std::uint32_t i = 0; i < n; ++i)
            out.push_back(std::min(i * width, std::max(0.f, span - width)));
        float const r = std::clamp(static_cast<float>(rand01()), 0.f, 1.f);
        freeIndex = std::min(n - 1, static_cast<std::uint32_t>(r * n));
        return out;
    }

    // The part of a lane its marker draws, across the axis (frame coordinates).
    struct LaneStrip
    {
        float offset = 0.f;     // low edge
        float width = 0.f;
    };

    // ExceptOne (CoverLaneOffsets): the strip of lane `index` that the drawn lane before it leaves uncovered. The last
    // lane is clamped inside the span and so overlaps the one before it; drawing it whole doubles the translucent
    // layers over the overlap (M1-FIX-COLORS 10.2), so its marker draws only the uncovered strip. Its hitbox stays
    // the whole lane. When the lane before it is the free lane, nothing else draws the overlap (it is real danger
    // inside the free lane), so the lane draws whole. Every other lane draws whole.
    inline LaneStrip CoverLaneDrawnStrip(std::vector<float> const& offsets, float width, std::uint32_t index,
        std::uint32_t freeIndex)
    {
        if (index >= offsets.size())
            return {};
        LaneStrip strip{ offsets[index], width };
        if (index == 0 || index - 1 == freeIndex)
            return strip;
        float const prevEnd = offsets[index - 1] + width;
        if (prevEnd <= strip.offset + 1e-4f || prevEnd >= strip.offset + width - 1e-4f)
            return strip;
        return { prevEnd, strip.offset + width - prevEnd };
    }

    // The overlap of an X lane and a Y lane, as a Line along the frame's X axis.
    inline LanePlacement PlaceCrossing(Frame const& f, LaneRect const& xLane, LaneRect const& yLane)
    {
        float const midY = xLane.offset + xLane.width * 0.5f;
        return { FramePoint(f, yLane.offset, midY, f.angle), yLane.width, xLane.width };
    }

    // ------------------------------------------------------------------------------------------------------------
    // Weakened
    // ------------------------------------------------------------------------------------------------------------

    // Threat compensator in percent for s stacks: round(100 * (1 / (1 - 0.1 s) - 1)). 1..7 = 11 25 43 67 100 150 233.
    inline std::int32_t WeakenedThreatPct(std::uint8_t stacks)
    {
        if (stacks == 0)
            return 0;
        stacks = std::min<std::uint8_t>(stacks, Ids::WEAKENED_MAX_STACKS);
        double const keep = 1.0 - 0.1 * stacks;
        return static_cast<std::int32_t>(std::lround(100.0 * (1.0 / keep - 1.0)));
    }

    // Damage done multiplier for s stacks: 1 - 0.1 s.
    inline float WeakenedDamageMultiplier(std::uint8_t stacks)
    {
        stacks = std::min<std::uint8_t>(stacks, Ids::WEAKENED_MAX_STACKS);
        return 1.f - 0.1f * stacks;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Reference health and per-target damage
    // ------------------------------------------------------------------------------------------------------------
    struct ReferenceHealth
    {
        std::uint32_t nonTank = 0;
        std::uint32_t tank = 0;
    };

    struct ReferenceHealthRow
    {
        std::uint32_t level = 0;
        ReferenceHealth health;
    };

    // Parses "level:nonTank:tank" entries separated by spaces, commas or semicolons. Returns false (and leaves `out`
    // empty) on any malformed entry or when nothing parses. Sorted by level on success.
    inline bool ParseReferenceHealthRows(std::string const& text, std::vector<ReferenceHealthRow>& out)
    {
        out.clear();
        std::string norm = text;
        for (char& ch : norm)
            if (ch == ',' || ch == ';' || ch == '\t')
                ch = ' ';
        std::istringstream in(norm);
        std::string tok;
        while (in >> tok)
        {
            unsigned long level = 0;
            unsigned long nonTank = 0;
            unsigned long tank = 0;
            char c1 = 0;
            char c2 = 0;
            std::istringstream t(tok);
            bool const ok = static_cast<bool>(t >> level >> c1 >> nonTank >> c2 >> tank);
            if (!ok || c1 != ':' || c2 != ':' || t.peek() != std::char_traits<char>::eof())
            {
                out.clear();
                return false;
            }
            out.push_back({ static_cast<std::uint32_t>(level),
                { static_cast<std::uint32_t>(nonTank), static_cast<std::uint32_t>(tank) } });
        }
        if (out.empty())
            return false;
        std::sort(out.begin(), out.end(), [](ReferenceHealthRow const& a, ReferenceHealthRow const& b)
        {
            return a.level < b.level;
        });
        return true;
    }

    // Linear interpolation by level between the rows, clamped at both ends.
    inline ReferenceHealth InterpolateReferenceHealth(std::vector<ReferenceHealthRow> const& rows, std::uint32_t level)
    {
        if (rows.empty())
            return {};
        if (level <= rows.front().level)
            return rows.front().health;
        if (level >= rows.back().level)
            return rows.back().health;
        for (std::size_t i = 1; i < rows.size(); ++i)
        {
            ReferenceHealthRow const& hi = rows[i];
            if (level > hi.level)
                continue;
            ReferenceHealthRow const& lo = rows[i - 1];
            double const f = hi.level == lo.level ? 0.0
                : static_cast<double>(level - lo.level) / static_cast<double>(hi.level - lo.level);
            auto lerp = [f](std::uint32_t a, std::uint32_t b)
            {
                return static_cast<std::uint32_t>(std::lround(a + (static_cast<double>(b) - a) * f));
            };
            return { lerp(lo.health.nonTank, hi.health.nonTank), lerp(lo.health.tank, hi.health.tank) };
        }
        return rows.back().health;
    }

    // A mechanic's damage, as the script gave it.
    struct DamageSpec
    {
        float pctNonTank = 0.f;             // percent of reference non-tank health
        float pctTank = -1.f;               // -1: the tank takes the non-tank number
        std::int32_t flat = -1;             // >= 0 overrides the provider (tests, `.mech test dmg=`)
        std::uint8_t school = 0;            // SpellSchools: 0 physical .. 6 arcane
        std::uint32_t spellId = 0;          // 0 = Ids::ResolveSpellForSchool(school)
        std::uint32_t riderSpell = 0;       // cast on every unit hit (triggered, original caster = owner)

        bool HasDamage() const
        {
            return flat > 0 || pctNonTank > 0.f || pctTank > 0.f;
        }

        std::uint32_t ResolveSpell() const
        {
            return spellId ? spellId : Ids::ResolveSpellForSchool(school);
        }
    };

    // The flat number one target takes: flat >= 0 wins; else the tank's percent of tank health when pctTank >= 0 and
    // the target is the tank; else the non-tank percent of non-tank health.
    inline std::int32_t FlatDamageFor(DamageSpec const& d, ReferenceHealth const& ref, bool isTank)
    {
        if (d.flat >= 0)
            return d.flat;
        double v = 0.0;
        if (isTank && d.pctTank >= 0.f)
            v = d.pctTank / 100.0 * ref.tank;
        else
            v = d.pctNonTank / 100.0 * ref.nonTank;
        return static_cast<std::int32_t>(std::max<long>(0, std::lround(v)));
    }

    // The DamageSpec a registry mechanic holds for `.Damage(pct)` (tank false) or `.Damage(pct).TankDamage(pct)`
    // (tank true): the same clamp of a negative percent to 0 as MechanicRef::Damage / TankDamage.
    inline DamageSpec RefFlatSpec(float pct, bool tank)
    {
        DamageSpec d;
        d.pctNonTank = std::max(0.f, pct);
        if (tank)
            d.pctTank = std::max(0.f, pct);
        return d;
    }

    // Mechanic::RefFlat's pure part: `pct` percent of the reference row's tank (tank) or non-tank health, rounded and
    // clamped by FlatDamageFor, so a script's own spell deals exactly what a registry mechanic with the same percent
    // deals on the same row.
    inline std::uint32_t RefFlatFor(ReferenceHealth const& ref, float pct, bool tank)
    {
        return static_cast<std::uint32_t>(FlatDamageFor(RefFlatSpec(pct, tank), ref, tank));
    }

    // ------------------------------------------------------------------------------------------------------------
    // Marker visual pick (handoff §3.8; used by C2's Telegraph). Two layers per telegraph (M1 telegraph-fill fix):
    // the outline is the hitbox at full size from the first frame; the fill is the timer that grows into it.
    // ------------------------------------------------------------------------------------------------------------
    enum class MarkerLayer : std::uint8_t
    {
        Outline,    // rim + faint interior: static, the full hitbox (also Spread/RunOut player rings)
        Fill        // rim + solid interior: grows to the outline by the resolve, full while the danger is live
    };

    // One marker creature: display id, object scale, facing relative to the anchor pose's facing (radians), and its
    // spawn point `forward` yards from the anchor along (anchor facing + facingOffset). display 0: nothing in this
    // slot (PickFill: a fill piece that has not started yet). hasZ: the marker stands at world height z (the ground
    // snap, MechanicGround.h; the lift is included) instead of the anchor's z + BossMechanics.Telegraph.Lift.
    struct VisualPiece
    {
        std::uint32_t display = 0;
        float scale = 1.f;
        float facingOffset = 0.f;
        float forward = 0.f;
        bool hasZ = false;
        float z = 0.f;
    };

    struct VisualPick
    {
        std::vector<VisualPiece> pieces;
        bool clamped = false;           // the shape is past the end of its ladder (drawn at the last step or filled)
        bool filled = false;            // drawn as a full disc: donut hole below the ladder, cone wider than the ladder
    };

    // Smallest ladder index with value >= x (within eps); size() when none.
    template <std::size_t N>
    std::size_t LadderAtLeast(std::array<float, N> const& ladder, float x)
    {
        for (std::size_t i = 0; i < N; ++i)
            if (ladder[i] >= x - 1e-4f)
                return i;
        return N;
    }

    // Largest ladder index with value <= x (within eps); 0 when none.
    template <std::size_t N>
    std::size_t LadderAtMost(std::array<float, N> const& ladder, float x)
    {
        std::size_t best = 0;
        for (std::size_t i = 0; i < N; ++i)
            if (ladder[i] <= x + 1e-4f)
                best = i;
        return best;
    }

    // Line ladder aspect k_i (length / width) of step i: 2^((i - 8) / 4) below step 8 (1/4 up to 0.84), 2^((i - 8) / 8)
    // from step 8 (1 to 64). patch_bossmechanics_markers.py line_aspect computes the same.
    inline float LineAspect(std::uint32_t i)
    {
        float const d = static_cast<float>(i) - static_cast<float>(Ids::kLineUnitStep);
        return std::exp2(d < 0.f ? d / 4.f : d / 8.f);
    }

    // Line ladder index for aspect r (length / width): the smallest step at least r (Rules::kLineNeverShorter) or the
    // nearest one in log scale; clamped to [0, kLineSteps-1] (below 1/4 the first step, above 64 the last).
    inline std::uint32_t LineStepFor(float r)
    {
        if (Rules::kLineNeverShorter)
        {
            for (std::uint32_t i = 0; i < Ids::kLineSteps; ++i)
                if (LineAspect(i) >= r * (1.f - 1e-5f))
                    return i;
            return Ids::kLineSteps - 1;
        }
        std::uint32_t best = 0;
        float bestErr = 1e9f;
        float const lr = std::log2(std::max(r, 1e-6f));
        for (std::uint32_t i = 0; i < Ids::kLineSteps; ++i)
        {
            float const err = std::fabs(std::log2(LineAspect(i)) - lr);
            if (err < bestErr)
            {
                bestErr = err;
                best = i;
            }
        }
        return best;
    }

    // A line's pieces, source end first, as ladder steps (LineAspect(step) * width long each). Every piece but the last
    // is an exact ladder length, so the pieces abut with no overlap; the last one covers the rest, rounded up (the only
    // overshoot is at the far end).
    using LineSteps = std::vector<std::uint32_t>;

    // The default layout (M1-FIX-LINES): full pieces at the top step (exactly kLineMaxAspect = 64 widths), then one
    // last piece rounded up to cover the rest. Most lines are one piece. Empty for a line with nothing to draw.
    inline LineSteps DefaultLineSteps(Shape const& shape)
    {
        LineSteps steps;
        float const drawn = shape.length - shape.excludeStart;
        float const w = shape.width;
        if (shape.type != ShapeType::Line || drawn <= 0.f || w <= 0.f)
            return steps;
        float const full = Ids::kLineMaxAspect * w;
        std::uint32_t const n = std::max<std::uint32_t>(1,
            static_cast<std::uint32_t>(std::ceil(drawn / full - 1e-4f)));
        steps.assign(n - 1, Ids::kLineSteps - 1);
        steps.push_back(LineStepFor((drawn - (n - 1) * full) / w));
        return steps;
    }

    // Do these steps lay the line out the LineSteps way: every piece but the last ends short of the line's end, and
    // the last one reaches it? A layout made for another shape (or none) fails, and PickVisual draws the default.
    inline bool LineStepsCover(Shape const& shape, LineSteps const& steps)
    {
        float const w = shape.width;
        if (steps.empty() || shape.type != ShapeType::Line || w <= 0.f)
            return false;
        float at = shape.excludeStart;
        for (std::size_t k = 0; k < steps.size(); ++k)
        {
            if (steps[k] >= Ids::kLineSteps)
                return false;
            bool const last = k + 1 == steps.size();
            float const end = at + LineAspect(steps[k]) * w;
            if (!last && end >= shape.length - 1e-4f)
                return false;
            if (last && end < shape.length - 1e-3f)
                return false;
            at = end;
        }
        return true;
    }

    // The full-size pieces of one layer. Ladder rounding is the same for both layers (the outline and the full fill
    // draw the same geometry): cones round the angle up, donuts the ratio down, lines the length up. Every shape exists
    // in every color (Ids::MarkerDisplay); a color index past the palette draws the last color. `lineSteps` (lines
    // only): a layout of the line's pieces (the ground snap's terrain split); null, or one that does not cover the
    // shape, draws DefaultLineSteps.
    inline VisualPick PickVisual(Shape const& shape, std::uint8_t color, MarkerLayer layer,
        LineSteps const* lineSteps = nullptr)
    {
        using Ids::MarkerShape;
        VisualPick pick;
        bool const outline = layer == MarkerLayer::Outline;
        auto display = [color](MarkerShape s, std::size_t step = 0)
        {
            return Ids::MarkerDisplay(color, s, static_cast<std::uint32_t>(step));
        };
        MarkerShape const circle = outline ? MarkerShape::Ring : MarkerShape::Disc;

        switch (shape.type)
        {
            case ShapeType::Circle:
                pick.pieces.push_back({ display(circle), shape.radius, 0.f, 0.f });
                break;
            case ShapeType::Donut:
            {
                float const ratio = shape.radius > 0.f ? shape.inner / shape.radius : 0.f;
                std::size_t j = 0;
                if (Rules::kDonutRatioRoundUp)
                {
                    j = LadderAtLeast(Ids::kDonutRatios, ratio);
                    if (j == Ids::kDonutRatios.size())
                    {
                        j = Ids::kDonutRatios.size() - 1;
                        pick.clamped = true;
                    }
                }
                else
                {
                    if (ratio < Ids::kDonutRatios.front() - 1e-4f)
                    {
                        // The smallest drawn hole would be bigger than the real one: draw the whole circle as
                        // danger (the hole is not shown) rather than a safe-looking area that hits.
                        pick.clamped = true;
                        pick.filled = true;
                        pick.pieces.push_back({ display(circle), shape.radius, 0.f, 0.f });
                        break;
                    }
                    j = LadderAtMost(Ids::kDonutRatios, ratio);
                }
                pick.pieces.push_back({ display(outline ? MarkerShape::DonutOutline : MarkerShape::Donut, j),
                    shape.radius, 0.f, 0.f });
                break;
            }
            case ShapeType::Cone:
            {
                std::size_t const j = LadderAtLeast(Ids::kConeAngles, shape.angleDeg);
                if (j == Ids::kConeAngles.size())
                {
                    // Wider than the widest cone: the largest drawn cone would be smaller than the hitbox, so draw
                    // a full disc of the cone's length on the apex (never smaller than the hitbox).
                    pick.clamped = true;
                    pick.filled = true;
                    pick.pieces.push_back({ display(circle), shape.length, 0.f, 0.f });
                    break;
                }
                pick.pieces.push_back({ display(outline ? MarkerShape::ConeOutline : MarkerShape::Cone, j),
                    shape.length, Geo::DegToRad(shape.facingOffsetDeg), 0.f });
                break;
            }
            case ShapeType::Line:
            {
                // End to end (M1-FIX-LINES): see LineSteps. Each piece starts where the one before it ends.
                float const drawn = shape.length - shape.excludeStart;
                float const w = shape.width;
                if (drawn <= 0.f || w <= 0.f)
                    break;
                LineSteps const steps = lineSteps && LineStepsCover(shape, *lineSteps) ? *lineSteps
                    : DefaultLineSteps(shape);
                MarkerShape const line = outline ? MarkerShape::LineOutline : MarkerShape::Line;
                float const facing = Geo::DegToRad(shape.facingOffsetDeg);
                float at = shape.excludeStart;
                for (std::uint32_t step : steps)
                {
                    pick.pieces.push_back({ display(line, step), w, facing, at });
                    at += LineAspect(step) * w;
                }
                if (steps.size() == 1 && drawn < Ids::kLineMinAspect * w * (1.f - 1e-5f))
                    pick.clamped = true;                    // shorter than the ladder's first step: drawn longer
                break;
            }
        }
        return pick;
    }

    // An armed mine's outline (M1-FIX-LINE-FILL): the outline pieces with every ring swapped for the dashed ring, so an
    // armed mine (no fill: the danger waits for a player) reads differently from an incoming hit (a solid fill). Mines
    // are circles; any other piece is kept as it is.
    inline VisualPick PickArmedMine(Shape const& shape, std::uint8_t color)
    {
        VisualPick pick = PickVisual(shape, color, MarkerLayer::Outline);
        for (VisualPiece& piece : pick.pieces)
        {
            Ids::MarkerDisplayParts const parts = Ids::DecodeMarkerDisplay(piece.display);
            if (parts.valid && parts.shape == Ids::MarkerShape::Ring)
                piece.display = Ids::MarkerDisplay(parts.color, Ids::MarkerShape::DashedRing);
        }
        return pick;
    }

    // Where a marker piece stands (before the telegraph's lift): `forward` yards from the anchor along the anchor's
    // facing + the piece's facing offset, facing that way. The line hitbox (Geo::ToLineCoords) measures s along the
    // same direction from the same anchor, so a line piece's model origin sits on the line's centreline at s =
    // forward - and the piece draws s in [forward, forward + extent].
    inline Geo::Pose PieceWorldPose(Geo::Pose const& anchor, VisualPiece const& piece)
    {
        return Geo::Advance(anchor, anchor.o + piece.facingOffset, piece.forward);
    }

    // Where the client draws model point (mx, my) of a marker standing at `pose` with object scale `scale`: model +X
    // along the facing, +Y to its left (the M2 convention; in-game check S5 confirmed it for the cone markers, whose
    // direction comes from the same texture-to-+X mapping the line markers use since M1-FIX-LINES).
    inline Geo::Vec3 MarkerModelToWorld(Geo::Pose const& pose, float scale, float mx, float my)
    {
        float const c = std::cos(pose.o);
        float const s = std::sin(pose.o);
        return { pose.x + scale * (c * mx - s * my), pose.y + scale * (s * mx + c * my), pose.z };
    }

    // ExceptOne's last lane draws a narrower strip than its hitbox: the drawn shape is `width` wide and its centreline
    // `lateral` yards to the left of the hitbox's (Geo::ToLineCoords t). width <= 0: draw the hitbox.
    inline void ApplyDrawStrip(Shape& shape, Geo::Pose& pose, float lateral, float width)
    {
        if (shape.type != ShapeType::Line || width <= 0.f)
            return;
        float const f = Geo::ShapeFacing(shape, pose);
        pose.x -= std::sin(f) * lateral;
        pose.y += std::cos(f) * lateral;
        shape.width = width;
    }

    // The drawn extent of a fill or outline piece along its own axis (yd): radius for discs, rings, donuts, the timer
    // ring and cones; length for line pieces. 0 for display 0. Tests and the debug log use it.
    inline float PieceExtent(VisualPiece const& piece)
    {
        Ids::MarkerDisplayParts const parts = Ids::DecodeMarkerDisplay(piece.display);
        if (parts.valid && Ids::IsLineShape(parts.shape))
            return LineAspect(parts.step) * piece.scale;
        return piece.display ? piece.scale : 0.f;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Fill timer
    // ------------------------------------------------------------------------------------------------------------

    // How far the warning has run at `now`, 0..1: linear from createdAt to resolveAt, `lead` early (the client blends
    // a scale change in over a short time, so the server target runs ahead by about that much to land the drawn fill
    // on the outline at the resolve). A mechanic with no warning (resolveAt <= createdAt) is 1.
    inline float FillFraction(Ms createdAt, Ms resolveAt, Ms now, Ms lead)
    {
        if (resolveAt <= createdAt)
            return 1.f;
        float const f = SecondsOf(now + lead - createdAt) / SecondsOf(resolveAt - createdAt);
        return std::clamp(f, 0.f, 1.f);
    }

    // The donut timer ring's last radius (its band then ends on the drawn hole's edge, on the danger side).
    inline float DonutTimerEnd(float outer, std::uint32_t ratioStep)
    {
        return std::min(outer, Ids::kDonutRatios[ratioStep] * outer / (1.f - Ids::kTimerRingBandFrac));
    }

    // The fill layer at timer fraction f. One entry per full-size fill piece (same order, so slot k always belongs
    // to piece k); display 0 = nothing drawn in that slot yet. `active` (the danger is live: resolved, an armed mine,
    // a running persistent zone) returns the full fill. Never bigger than the outline at any f, and never a display
    // other than the full fill's (a live marker only ever changes scale, which the client blends smoothly):
    // - circle, cone, a donut or cone drawn as a disc: one piece, scale f * full (grows from the centre / the apex;
    //   uniform scale keeps the cone's angle);
    // - donut: the timer ring (rim only) shrinks from the outer rim to the drawn hole's rim, its band always on the
    //   danger side (it ends with its inner edge on the hole's edge); active: the filled donut;
    // - line (M1-FIX-LINE-FILL): a copy of the whole drawn line scaled by f about its source end (the first piece's
    //   start, after ExcludeSourceEnd), on its axis, the way a cone grows from its apex: every piece at scale f * w,
    //   piece k standing f * (start_k - source) from the source. It starts as a sliver at the source and grows longer
    //   and wider together; the line is convex, so the copy stays inside the outline.
    // Pieces smaller than Rules::kFillMinScaleYd are left out (display 0).
    inline VisualPick PickFill(Shape const& shape, std::uint8_t color, float f, bool active,
        LineSteps const* lineSteps = nullptr)
    {
        VisualPick pick = PickVisual(shape, color, MarkerLayer::Fill, lineSteps);
        if (active || pick.pieces.empty())
            return pick;
        f = std::clamp(f, 0.f, 1.f);
        auto tooSmall = [](VisualPiece& piece)
        {
            if (piece.scale < Rules::kFillMinScaleYd)
                piece.display = 0;
        };

        if (shape.type == ShapeType::Donut && !pick.filled)
        {
            VisualPiece& piece = pick.pieces.front();
            Ids::MarkerDisplayParts const parts = Ids::DecodeMarkerDisplay(piece.display);
            float const outer = shape.radius;
            float const end = DonutTimerEnd(outer, parts.step);
            piece.display = Ids::MarkerDisplay(parts.color, Ids::MarkerShape::TimerRing);
            piece.scale = outer + f * (end - outer);
            tooSmall(piece);
            return pick;
        }

        // Uniform scale about the source end: for circles and cones forward is 0 (the centre / the apex).
        float const source = pick.pieces.front().forward;
        for (VisualPiece& piece : pick.pieces)
        {
            piece.scale *= f;
            piece.forward = source + f * (piece.forward - source);
            tooSmall(piece);
        }
        return pick;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Fill pacing (round 4b) and the projectile shrink
    // ------------------------------------------------------------------------------------------------------------

    // How far the fill's leading edge travels from f = 0 to the full fill (yd): a line's far end from its source end
    // (every piece included), a disc's radius, a cone's length, a donut timer ring's way from the outer rim to its end.
    inline float FillTravel(Shape const& shape)
    {
        VisualPick const full = PickVisual(shape, 0, MarkerLayer::Fill);
        if (full.pieces.empty())
            return 0.f;
        if (shape.type == ShapeType::Donut && !full.filled)
        {
            Ids::MarkerDisplayParts const parts = Ids::DecodeMarkerDisplay(full.pieces.front().display);
            return shape.radius - DonutTimerEnd(shape.radius, parts.step);
        }
        VisualPiece const& last = full.pieces.back();
        return last.forward - full.pieces.front().forward + PieceExtent(last);
    }

    // The fill step interval (ms) of one mechanic: short enough that the fill's leading edge moves at most `stepYd`
    // per step over the warning (`travelYd` in `warn`), never shorter than Rules::kFillMinStepMs (nor than maxStepMs)
    // and never longer than `maxStepMs` (BossMechanics.Telegraph.FillStepMs). The client blends each scale change in
    // over less than a long step, so a big fill stepped slowly visibly jumps (round 4b, Q8); a small one needs no more
    // than maxStepMs. stepYd <= 0 (pacing off), no travel or no warning: maxStepMs.
    inline std::uint32_t FillStepInterval(Ms warn, float travelYd, float stepYd, std::uint32_t maxStepMs)
    {
        if (stepYd <= 0.f || travelYd <= 0.f || warn <= Ms(0))
            return maxStepMs;
        double const lo = std::min(Rules::kFillMinStepMs, maxStepMs);
        double const ms = std::floor(static_cast<double>(warn.count()) * stepYd / travelYd);
        return static_cast<std::uint32_t>(std::clamp(ms, lo, static_cast<double>(maxStepMs)));
    }

    // ShrinkProjectileLines: the drawn start of a projectile line whose projectile tail is at `tail` (yd along the
    // line). Rounded down to whole steps of the line width (at least Rules::kShrinkMinStepYd) from the line's own
    // start, so the outline is re-picked (a display swap: the client re-instances the model, which flickers) once per
    // width of travel rather than on every follow step. Rounded down, the drawn line never starts past the tail.
    inline float ShrunkExcludeStart(float tail, float excludeStart, float length, float width)
    {
        if (tail <= excludeStart)
            return excludeStart;
        float const step = std::max(width, Rules::kShrinkMinStepYd);
        float const start = excludeStart + std::floor((tail - excludeStart) / step) * step;
        return std::min(start, length);
    }
}

#endif
