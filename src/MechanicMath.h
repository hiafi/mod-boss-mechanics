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
        // §7.1: a drawn line is never shorter than its hitbox (up to 9% longer). false = nearest ladder step.
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
        // RunOut From(Owner): the hitbox ring sits on the owner; this small ring (the mechanic's colour) follows the
        // targeted player so the target knows it is the one who must run (review M3, harness ruling 2026-11-02).
        constexpr float kRunOutTargetMarkerRadiusYd = 1.5f;
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

    // ------------------------------------------------------------------------------------------------------------
    // Marker visual pick (handoff §3.8; used by C2's Telegraph)
    // ------------------------------------------------------------------------------------------------------------
    enum class MarkerStyle : std::uint8_t
    {
        Disc,   // filled + rim (zones, get-in, armed mine)
        Ring    // rim + faint fill (player get-out radii, arming mine)
    };

    // One marker creature: display id, object scale, facing relative to the anchor pose's facing (radians), and its
    // spawn point `forward` yards from the anchor along (anchor facing + facingOffset).
    struct VisualPiece
    {
        std::uint32_t display = 0;
        float scale = 1.f;
        float facingOffset = 0.f;
        float forward = 0.f;
    };

    struct VisualPick
    {
        std::vector<VisualPiece> pieces;
        bool clamped = false;           // the shape is past the end of its ladder (drawn at the last step or filled)
        bool filled = false;            // drawn as a full disc: donut hole below the ladder, cone wider than the ladder
        bool colourFallback = false;    // blue/purple asked for a red/yellow-only shape
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

    inline float LineAspect(std::uint32_t i)
    {
        return std::exp2(static_cast<float>(i) / 8.f);
    }

    // Line ladder index for aspect r (length / width), per Rules::kLineNeverShorter; clamped to [0, kLineSteps-1].
    inline std::uint32_t LineStepFor(float r)
    {
        if (r <= 1.f)
            return 0;
        float const exact = 8.f * std::log2(r);
        float const step = Rules::kLineNeverShorter ? std::ceil(exact - 1e-4f) : std::round(exact);
        return static_cast<std::uint32_t>(std::clamp(step, 0.f, static_cast<float>(Ids::kLineSteps - 1)));
    }

    inline VisualPick PickVisual(Shape const& shape, std::uint8_t colour, MarkerStyle style)
    {
        VisualPick pick;
        std::uint8_t c = std::min<std::uint8_t>(colour, Ids::COLOUR_COUNT_ALL - 1);
        auto getOutColour = [&pick](std::uint8_t col) -> std::uint8_t
        {
            if (col < Ids::COLOUR_COUNT_GET_OUT)
                return col;
            pick.colourFallback = true;
            return col % Ids::COLOUR_COUNT_GET_OUT;
        };

        switch (shape.type)
        {
            case ShapeType::Circle:
            {
                std::uint32_t const base = style == MarkerStyle::Ring ? Ids::DISPLAY_RING_BASE : Ids::DISPLAY_DISC_BASE;
                pick.pieces.push_back({ base + c, shape.radius, 0.f, 0.f });
                break;
            }
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
                        pick.pieces.push_back({ Ids::DISPLAY_DISC_BASE + getOutColour(c), shape.radius, 0.f, 0.f });
                        break;
                    }
                    j = LadderAtMost(Ids::kDonutRatios, ratio);
                }
                std::uint32_t const display = Ids::DISPLAY_DONUT_BASE + 2 * static_cast<std::uint32_t>(j)
                    + getOutColour(c);
                pick.pieces.push_back({ display, shape.radius, 0.f, 0.f });
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
                    pick.pieces.push_back({ Ids::DISPLAY_DISC_BASE + getOutColour(c), shape.length, 0.f, 0.f });
                    break;
                }
                std::uint32_t const display = Ids::DISPLAY_CONE_BASE + 2 * static_cast<std::uint32_t>(j)
                    + getOutColour(c);
                pick.pieces.push_back({ display, shape.length, Geo::DegToRad(shape.facingOffsetDeg), 0.f });
                break;
            }
            case ShapeType::Line:
            {
                float const drawn = shape.length - shape.excludeStart;
                if (drawn <= 0.f || shape.width <= 0.f)
                    break;
                float const ratio = drawn / shape.width;
                std::uint32_t const n = std::max<std::uint32_t>(1,
                    static_cast<std::uint32_t>(std::ceil(ratio / Ids::kLineMaxAspect - 1e-4f)));
                float const pieceLen = drawn / n;
                std::uint32_t const i = LineStepFor(pieceLen / shape.width);
                std::uint32_t const display = Ids::DISPLAY_LINE_BASE + 2 * i + getOutColour(c);
                for (std::uint32_t k = 0; k < n; ++k)
                    pick.pieces.push_back({ display, shape.width, Geo::DegToRad(shape.facingOffsetDeg),
                        shape.excludeStart + k * pieceLen });
                break;
            }
        }
        return pick;
    }
}

#endif
