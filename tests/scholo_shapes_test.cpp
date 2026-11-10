// Standalone tests for the Scholomance shapes of src/MechanicDrill.h (Scholomance build §3.9, WP-G): the
// `scholomance-shapes` drill against the build spec rows it reproduces (Polkelt PK2, Malicia MA6, Illucia IB1,
// Rattlegore RG4, Jandice JB1 heroic, Gandling GD4), the landing helpers it adds (grid, decoy picks, away=, the
// Foresight prediction), the decoy grid's geometry (the decoy's circle is the only safe ground), the solver finding
// that ground (the pocket pass, Stratholme SWEEP-2 D6) and the sweep's program. Build: tests/run.sh.

#include "BossMechanicsIds.h"
#include "check.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include "MechanicSolver.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace BossMechanics;
using Geo::Vec3;

namespace
{
    constexpr float kEps = 1e-3f;

    float Key(std::string_view line, std::string_view key, float def)
    {
        std::optional<std::string_view> v = Drill::Value(line, key);
        return v ? Drill::Number(*v).value_or(-1.f) : def;
    }

    bool Is(std::string_view line, std::string_view key, std::string_view value)
    {
        std::optional<std::string_view> v = Drill::Value(line, key);
        return v && *v == value;
    }

    std::string_view Shape(std::string_view line)
    {
        std::vector<std::string_view> const t = Drill::Tokens(line);
        return t.empty() ? std::string_view() : t[0];
    }

    float Size(std::string_view line)
    {
        std::vector<std::string_view> const t = Drill::Tokens(line);
        return t.size() >= 2 ? Drill::Number(t[1]).value_or(-1.f) : -1.f;
    }

    std::uint32_t Tag(std::string_view line)
    {
        return static_cast<std::uint32_t>(Key(line, "tag", 0.f));
    }

    // One step of cycle 0: its line, when it registers and when it resolves (ms after the cycle start).
    struct Timed
    {
        std::string_view line;
        std::uint32_t startMs = 0;
        std::uint32_t resolveMs = 0;
    };

    std::vector<Timed> StepsWithTag(Drill::Def const& d, std::uint32_t tag)
    {
        std::vector<std::optional<std::uint32_t>> const at = Drill::StepOffsets(d, 0);
        std::vector<Timed> out;
        for (std::size_t i = 0; i < d.steps.size(); ++i)
            if (at[i] && Tag(d.steps[i].line) == tag)
                out.push_back({ d.steps[i].line, *at[i], *at[i] + Drill::WarnMs(d.steps[i].line) });
        return out;
    }

    Drill::Def const* Shapes()
    {
        return Drill::Find("scholomance-shapes");
    }

    // ---- the drill table -------------------------------------------------------------------------------------------
    void TestTable()
    {
        Drill::Def const* found = Shapes();
        CHECK(found != nullptr);
        if (!found)
            return;
        Drill::Def const& d = *found;
        CHECK(d.cycleMs == 40000 && d.defaultCycles == 3 && !d.ownerAtCreature);
        std::set<std::uint32_t> tags;
        for (Drill::Step const& s : d.steps)
        {
            std::uint32_t const tag = Tag(s.line);
            CHECK(tag >= Drill::kScholoTagFuse && tag <= Drill::kScholoTagPortal);
            tags.insert(tag);
            // Every step deals damage (the runner appends dmg=) and names its heroic percent.
            CHECK(Drill::DamageStep(s.line));
            CHECK(Key(s.line, "pct", 0.f) >= 30.f);
        }
        CHECK(tags.size() == 10);
        // The bar's tags are the §3.9 short warnings; the two 2 s lead-ins are reported only.
        for (std::uint32_t t : Drill::kScholoBarTags)
            CHECK(tags.count(t) == 1);
        CHECK(!Drill::ScholoBarTag(Drill::kScholoTagFuse) && !Drill::ScholoBarTag(Drill::kScholoTagOssuary));
        for (std::uint32_t t : { 2u, 3u, 4u, 5u, 7u, 8u, 9u, 10u })
            CHECK(Drill::ScholoBarTag(t));
        CHECK(!Drill::ScholoBarTag(0) && !Drill::ScholoBarTag(11));
    }

    // PK2 (ruling 18): a 6 yd fuse ring (2 s) near a non-tank, then chained rings 1 s each, each on a specimen inside
    // the last ring (4 yd off its centre). Weakened (ruling 21).
    void TestPolkeltChain()
    {
        Drill::Def const& d = *Shapes();
        std::vector<Timed> const first = StepsWithTag(d, Drill::kScholoTagFuse);
        std::vector<Timed> const chain = StepsWithTag(d, Drill::kScholoTagChain);
        CHECK(first.size() == 1 && chain.size() == 2);
        if (first.size() != 1 || chain.size() != 2)
            return;
        CHECK(Shape(first[0].line) == "circle" && Size(first[0].line) == 6.f);
        CHECK(first[0].resolveMs - first[0].startMs == 2000);
        CHECK(Is(first[0].line, "at", "member") && Is(first[0].line, "pick", "nontank"));
        // The specimen stands inside the ring with its non-tank: the ring holds the member.
        CHECK(Key(first[0].line, "away", 0.f) < Size(first[0].line));
        std::uint32_t prevResolve = first[0].resolveMs;
        for (Timed const& t : chain)
        {
            CHECK(Shape(t.line) == "circle" && Size(t.line) == 6.f);
            CHECK(t.resolveMs - t.startMs == 1000);
            // Lit as the last ring bursts (after=resolve), from inside it.
            CHECK(t.startMs == prevResolve);
            CHECK(Is(t.line, "at", "prev"));
            float const hop = Key(t.line, "away", 0.f);
            CHECK(hop > 0.f && hop < 6.f);
            prevResolve = t.resolveMs;
        }
        for (Timed const& t : first)
            CHECK(Is(t.line, "weak", "1"));
        for (Timed const& t : chain)
            CHECK(Is(t.line, "weak", "1"));
    }

    // MA6 heroic: three waves 1 s apart, a 4 yd circle 2 yd off every player, 1.5 s each; no Weakened (a Soul stack).
    void TestMaliciaBarrage()
    {
        std::vector<Timed> const waves = StepsWithTag(*Shapes(), Drill::kScholoTagBarrage);
        CHECK(waves.size() == 3);
        for (std::size_t i = 0; i < waves.size(); ++i)
        {
            Timed const& w = waves[i];
            CHECK(Shape(w.line) == "circle" && Size(w.line) == 4.f);
            CHECK(w.resolveMs - w.startMs == 1500);
            CHECK(Is(w.line, "at", "each") && Key(w.line, "away", 0.f) == 2.f);
            CHECK(!Drill::Value(w.line, "weak"));
            if (i)
                CHECK(w.startMs - waves[i - 1].startMs == 1000);
        }
        // The player stands 2 yd inside its own 4 yd circle: 2 yd of the run are needed at least.
        Vec3 const member{ 10.f, 10.f, 0.f };
        for (float bearing : { 0.f, 1.f, 2.5f, 4.f })
            CHECK_NEAR(Geo::Dist2d(Drill::Away(member, bearing, 2.f).x, Drill::Away(member, bearing, 2.f).y,
                member.x, member.y), 2.f, kEps);
    }

    // IB1 heroic: a 4 yd marker on the read player (any, the tank included), then four more, each registered as the
    // last lands, where that player will be 1.5 s later. Weakened (ruling 20).
    void TestIlluciaForesight()
    {
        Drill::Def const& d = *Shapes();
        std::vector<Timed> const read = StepsWithTag(d, Drill::kScholoTagRead);
        std::vector<Timed> const chain = StepsWithTag(d, Drill::kScholoTagForesight);
        CHECK(read.size() == 1 && chain.size() == 4);
        if (read.size() != 1 || chain.size() != 4)
            return;
        CHECK(Shape(read[0].line) == "circle" && Size(read[0].line) == 4.f);
        CHECK(Is(read[0].line, "at", "member") && Is(read[0].line, "pick", "any"));
        CHECK(read[0].resolveMs - read[0].startMs == 1500);
        std::uint32_t prev = read[0].resolveMs;
        for (Timed const& t : chain)
        {
            CHECK(Shape(t.line) == "circle" && Size(t.line) == 4.f);
            CHECK(Is(t.line, "at", "predict") && Is(t.line, "pick", "same"));
            CHECK(Key(t.line, "lead", 0.f) == 1500.f);
            // The lead matches the warning: the marker sits where the player will be as it lands.
            CHECK(t.resolveMs - t.startMs == static_cast<std::uint32_t>(Key(t.line, "lead", 0.f)));
            CHECK(t.startMs == prev);
            CHECK(Is(t.line, "weak", "1"));
            prev = t.resolveMs;
        }
    }

    // RG4: rings 0-6, 6-12, 12-18 yd on him, 1.5 s apart after a 2 s cast; heroic then back inward, 1 s apart. The
    // rings tile 0-18 yd with no gap and no overlap.
    void TestRattlegoreOssuary()
    {
        Drill::Def const& d = *Shapes();
        std::vector<Timed> rings = StepsWithTag(d, Drill::kScholoTagOssuary);
        std::vector<Timed> const out = StepsWithTag(d, Drill::kScholoTagOut);
        std::vector<Timed> const in = StepsWithTag(d, Drill::kScholoTagIn);
        CHECK(rings.size() == 1 && out.size() == 2 && in.size() == 3);
        if (rings.size() != 1 || out.size() != 2 || in.size() != 3)
            return;
        CHECK(rings[0].resolveMs - rings[0].startMs == 2000);
        rings.insert(rings.end(), out.begin(), out.end());
        rings.insert(rings.end(), in.begin(), in.end());
        float const inner[] = { 0.f, 6.f, 12.06f, 12.06f, 6.f, 0.f };
        float const outer[] = { 6.f, 12.f, 18.f, 18.f, 12.f, 6.f };
        std::uint32_t const gap[] = { 0, 1500, 1500, 1000, 1000, 1000 };
        for (std::size_t i = 0; i < rings.size(); ++i)
        {
            std::string_view const line = rings[i].line;
            CHECK(Is(line, "anchor", "target"));
            CHECK(Size(line) == outer[i]);
            CHECK(Key(line, "inner", 0.f) == inner[i]);
            CHECK((inner[i] == 0.f) == (Shape(line) == "circle"));
            // The 12-18 yd ring sits exactly on the 0.67 donut ratio step, so the drawn hole is the real one.
            if (outer[i] == 18.f)
                CHECK(std::fabs(inner[i] / outer[i] - BossMechanics::Ids::kDonutRatios[5]) < 1e-5f);
            if (i)
            {
                CHECK(rings[i].resolveMs - rings[i - 1].resolveMs == gap[i]);
                // Registered as the last one lands (after=resolve): its warning is the gap.
                CHECK(rings[i].startMs == rings[i - 1].resolveMs);
            }
            // No Weakened (RG4 penalty none).
            CHECK(!Drill::Value(line, "weak"));
        }
    }

    // ---- JB1 heroic: the decoy grid --------------------------------------------------------------------------------

    // The grid step and the room's edge, and the constants the geometry check below uses.
    void TestJandiceSteps()
    {
        std::vector<Timed> const s = StepsWithTag(*Shapes(), Drill::kScholoTagSigil);
        CHECK(s.size() == 2);
        if (s.size() != 2)
            return;
        Timed const& grid = s[0];
        Timed const& edge = s[1];
        CHECK(Shape(grid.line) == "circle" && Size(grid.line) == Drill::kSigilRadius);
        CHECK(Key(grid.line, "grid", 0.f) == static_cast<float>(Drill::kSigilGrid));
        CHECK(Key(grid.line, "apart", 0.f) == Drill::kSigilApart);
        CHECK(Key(grid.line, "decoys", 0.f) == 1.f);
        CHECK(Is(grid.line, "at", "group"));
        CHECK(grid.resolveMs - grid.startMs == 6000);
        CHECK(Shape(edge.line) == "donut" && Size(edge.line) == Drill::kSigilEdgeOuter);
        CHECK(Key(edge.line, "inner", 0.f) == Drill::kSigilEdgeInner);
        CHECK(Is(edge.line, "at", "group") && Is(edge.line, "after", "with"));
        CHECK(edge.startMs == grid.startMs && edge.resolveMs == grid.resolveMs);
        // The edge's ratio is a ladder step (0.25), so its drawn hole is the real one.
        CHECK_NEAR(Drill::kSigilEdgeInner / Drill::kSigilEdgeOuter, Ids::kDonutRatios[0], kEps);
        // 6 s at a player's 7 yd/s never crosses the edge from inside the grid.
        CHECK(Drill::kSigilEdgeOuter - Drill::kSigilEdgeInner > 6.f * Drill::kPlayerRunYdS);
        CHECK(Drill::GridCovers(Drill::kSigilRadius, Drill::kSigilApart));
        CHECK(!Drill::GridCovers(8.f, 11.4f) && Drill::GridCovers(8.f, 11.3f) && !Drill::GridCovers(8.f, 0.f));
    }

    // For every decoy cell: the safe ground (inside the edge's hole, outside every real sigil) is all inside the
    // decoy's circle, and it is big enough for the group (at least kMinSafeArea sq yd and a 2 yd circle of clearance).
    void TestDecoyGridGeometry()
    {
        constexpr float kStep = 0.25f;
        constexpr float kMinSafeArea = 20.f;
        Vec3 const center{ 0.f, 0.f, 0.f };
        for (float facing : { 0.f, 0.6f })
        {
            std::vector<Vec3> const cells = Drill::GridPoints(center, facing, Drill::kSigilGrid, Drill::kSigilApart);
            CHECK(cells.size() == 9);
            for (std::size_t decoy = 0; decoy < cells.size(); ++decoy)
            {
                float area = 0.f;
                float bestClearance = 0.f;
                bool allInDecoy = true;
                for (float x = -Drill::kSigilEdgeInner; x <= Drill::kSigilEdgeInner; x += kStep)
                    for (float y = -Drill::kSigilEdgeInner; y <= Drill::kSigilEdgeInner; y += kStep)
                    {
                        float const fromCenter = std::sqrt(x * x + y * y);
                        if (fromCenter >= Drill::kSigilEdgeInner)
                            continue;           // the edge donut
                        float clearance = Drill::kSigilEdgeInner - fromCenter;
                        bool safe = true;
                        for (std::size_t i = 0; i < cells.size() && safe; ++i)
                        {
                            if (i == decoy)
                                continue;
                            float const d = Geo::Dist2d(x, y, cells[i].x, cells[i].y);
                            safe = d > Drill::kSigilRadius;
                            clearance = std::min(clearance, d - Drill::kSigilRadius);
                        }
                        if (!safe)
                            continue;
                        area += kStep * kStep;
                        bestClearance = std::max(bestClearance, clearance);
                        if (Geo::Dist2d(x, y, cells[decoy].x, cells[decoy].y) > Drill::kSigilRadius + kStep)
                            allInDecoy = false;
                    }
                CHECK(allInDecoy);
                CHECK(area >= kMinSafeArea);
                CHECK(bestClearance >= 2.f);
            }
        }
        // With no decoy the grid and the edge leave no safe ground at all (the grid covers out past the edge).
        std::vector<Vec3> const cells = Drill::GridPoints(center, 0.f, Drill::kSigilGrid, Drill::kSigilApart);
        bool anySafe = false;
        for (float x = -Drill::kSigilEdgeInner; x <= Drill::kSigilEdgeInner && !anySafe; x += kStep)
            for (float y = -Drill::kSigilEdgeInner; y <= Drill::kSigilEdgeInner && !anySafe; y += kStep)
            {
                if (std::sqrt(x * x + y * y) >= Drill::kSigilEdgeInner)
                    continue;
                bool covered = false;
                for (Vec3 const& c : cells)
                    covered = covered || Geo::Dist2d(x, y, c.x, c.y) <= Drill::kSigilRadius;
                anySafe = !covered;
            }
        CHECK(!anySafe);
    }

    // ---- GD4 heroic: portal lanes ----------------------------------------------------------------------------------

    // Three 4 yd lanes at once, 2 s, each from a point 20 yd off the group through a different non-tank, 40 yd long:
    // a member up to 15 yd from the group's centre is inside its lane with at least 5 yd of lane beyond it.
    // ---- the solver on the decoy grid (Stratholme SWEEP-2 D6) ----------------------------------------------------
    // The bots never see the decoy (the view skips it): they must find its ground as uncovered floor. The ground is
    // a pocket between the real sigils and the edge, which neither the exits (only from the hazards that hold the
    // bot, radial) nor the rings round the bot aim at; the pocket pass (Params::hazardEdgeArcs) tries the hazards'
    // free edges when nothing else is clean.

    struct StraightOracle : Solver::PathOracle
    {
        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            out = { from, to };
            return true;
        }
    };

    constexpr float kBotMarginYd = 0.75f;          // BossMechanics.Bots.SafetyMarginYd
    constexpr float kBotSpeed = 7.f;
    constexpr Solver::Ms kSolveAt{ 100000 };
    constexpr Solver::Ms kLeft{ 5700 };             // a 6 s warning, the bot's first solve 0.3 s in

    Solver::Hazard Sigil(std::uint32_t id, Vec3 const& c, float r)
    {
        Solver::Hazard h;
        h.id = id;
        h.shape = BossMechanics::Shape::Circle(r);
        h.poses = { Geo::Pose{ c.x, c.y, c.z, 0.f } };
        h.marginYd = kBotMarginYd;
        h.timing = Solver::MakeInstant(kSolveAt + kLeft, Solver::Ms(500));
        return h;
    }

    struct GridTally
    {
        std::uint32_t runs = 0, misses = 0, pocketMoves = 0, maxPocketPoints = 0, maxPocketChecks = 0;
        double worstUs = 0.0, totalUs = 0.0;
    };

    // Every start on a 2 yd lattice within 14 yd of the group's center, each decoy cell, facings 0 and 0.6: the plan's
    // target is outside every real sigil (no margin) and inside the edge's hole.
    GridTally DrillGrid(bool pocket, bool melee)
    {
        GridTally t;
        StraightOracle oracle;
        Vec3 const center{ 0.f, 0.f, 0.f };
        for (float facing : { 0.f, 0.6f })
        {
            std::vector<Vec3> const cells = Drill::GridPoints(center, facing, Drill::kSigilGrid, Drill::kSigilApart);
            for (std::size_t decoy = 0; decoy < cells.size(); ++decoy)
                for (float x = -14.f; x <= 14.f; x += 2.f)
                    for (float y = -14.f; y <= 14.f; y += 2.f)
                    {
                        if (std::sqrt(x * x + y * y) > 14.f)
                            continue;
                        Solver::Input in;
                        in.bot = { x, y, 0.f };
                        in.now = kSolveAt;
                        in.speed = kBotSpeed;
                        in.params.hazardEdgeArcs = pocket;
                        if (melee)
                        {
                            in.role.present = true;
                            in.role.anchor = { 0.f, 6.f, 0.f };
                            in.role.maxYd = 5.f;
                        }
                        std::uint32_t id = 1;
                        for (std::size_t i = 0; i < cells.size(); ++i)
                            if (i != decoy)
                                in.hazards.push_back(Sigil(id++, cells[i], Drill::kSigilRadius));
                        Solver::Hazard edge = Sigil(100, center, 0.f);
                        edge.shape = BossMechanics::Shape::Donut(Drill::kSigilEdgeInner, Drill::kSigilEdgeOuter);
                        in.hazards.push_back(edge);

                        auto const t0 = std::chrono::steady_clock::now();
                        Solver::Plan const plan = Solver::Solve(in, oracle);
                        double const us = std::chrono::duration<double, std::micro>(
                            std::chrono::steady_clock::now() - t0).count();
                        t.totalUs += us;
                        t.worstUs = std::max(t.worstUs, us);
                        ++t.runs;
                        bool safe = Geo::Dist2d(plan.target.x, plan.target.y, 0.f, 0.f) < Drill::kSigilEdgeInner;
                        for (std::size_t i = 0; i < cells.size(); ++i)
                            if (i != decoy && Geo::Dist2d(plan.target.x, plan.target.y, cells[i].x, cells[i].y)
                                <= Drill::kSigilRadius)
                                safe = false;
                        t.misses += safe ? 0 : 1;
                        t.pocketMoves += plan.stats.pass == 4 ? 1 : 0;
                        t.maxPocketPoints = std::max(t.maxPocketPoints, plan.stats.pocketCandidates);
                        t.maxPocketChecks = std::max(t.maxPocketChecks, plan.stats.pocketPathChecks);
                    }
        }
        return t;
    }

    // Jandice's real heroic grid (mod-dungeon-bosses ScholomanceRework.h kJandiceSigilGrid, JandiceLogic.h
    // kGridSigilRadius 9, the aisle kJandiceAisle x 228-283.5, y 84-105): an oracle that refuses paths and probes off
    // the aisle stands in for the navmesh.
    constexpr float kJandiceGrid[8][2] = { { 234.9f, 99.8f }, { 248.8f, 99.8f }, { 262.7f, 99.8f },
        { 276.6f, 99.8f }, { 234.9f, 89.2f }, { 248.8f, 89.2f }, { 262.7f, 89.2f }, { 276.6f, 89.2f } };
    constexpr float kJandiceSigilRadius = 9.f;
    constexpr float kJandiceZ = 95.9f;

    bool InAisle(Vec3 const& p)
    {
        return p.x >= 228.5f && p.x <= 283.f && p.y >= 84.5f && p.y <= 104.5f;
    }

    struct AisleOracle : Solver::PathOracle
    {
        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            if (!InAisle(to))
                return false;
            out = { from, to };
            return true;
        }

        bool Walkable(Vec3& p) override
        {
            return InAisle(p);
        }
    };

    bool JandiceSafe(Vec3 const& p, std::size_t decoy, float margin)
    {
        if (!InAisle(p))
            return false;
        for (std::size_t k = 0; k < 8; ++k)
            if (k != decoy && Geo::Dist2d(p.x, p.y, kJandiceGrid[k][0], kJandiceGrid[k][1])
                <= kJandiceSigilRadius + margin)
                return false;
        return true;
    }

    // Starts with the decoy's ground well within the warning (its nearest point at the solver's margins at most
    // kWithinYd away: 7 yd/s over 5.7 s less the start delay and the time margin, less 2 yd of slack): `misses`
    // counts the plans that do not end on safe ground; the starts beyond are left out (`runs` counts only the rest).
    GridTally JandiceGrid(bool pocket)
    {
        constexpr float kWithinYd = 34.f;
        GridTally t;
        AisleOracle oracle;
        for (std::size_t decoy = 0; decoy < 8; ++decoy)
        {
            std::vector<Vec3> ground;
            for (float x = 228.5f; x <= 283.f; x += 0.5f)
                for (float y = 84.5f; y <= 104.5f; y += 0.5f)
                    if (JandiceSafe({ x, y, kJandiceZ }, decoy, kBotMarginYd + 0.5f))
                        ground.push_back({ x, y, kJandiceZ });
            CHECK(!ground.empty());
            for (float x = 230.f; x <= 282.f; x += 2.f)
                for (float y = 86.f; y <= 103.f; y += 2.f)
                {
                    Vec3 const bot{ x, y, kJandiceZ };
                    float nearest = 1e9f;
                    for (Vec3 const& g : ground)
                        nearest = std::min(nearest, Geo::Dist2d(bot.x, bot.y, g.x, g.y));
                    if (nearest > kWithinYd)
                        continue;
                    Solver::Input in;
                    in.bot = bot;
                    in.now = kSolveAt;
                    in.speed = kBotSpeed;
                    in.params.hazardEdgeArcs = pocket;
                    std::uint32_t id = 1;
                    for (std::size_t k = 0; k < 8; ++k)
                        if (k != decoy)
                            in.hazards.push_back(Sigil(id++, { kJandiceGrid[k][0], kJandiceGrid[k][1], kJandiceZ },
                                kJandiceSigilRadius));
                    auto const t0 = std::chrono::steady_clock::now();
                    Solver::Plan const plan = Solver::Solve(in, oracle);
                    double const us = std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - t0).count();
                    t.totalUs += us;
                    t.worstUs = std::max(t.worstUs, us);
                    ++t.runs;
                    t.misses += JandiceSafe(plan.target, decoy, 0.f) ? 0 : 1;
                    t.pocketMoves += plan.stats.pass == 4 ? 1 : 0;
                    t.maxPocketPoints = std::max(t.maxPocketPoints, plan.stats.pocketCandidates);
                    t.maxPocketChecks = std::max(t.maxPocketChecks, plan.stats.pocketPathChecks);
                }
        }
        return t;
    }

    void PrintTally(char const* name, GridTally const& t)
    {
        std::printf("decoy grid solver, %s: %u of %u starts miss the safe ground, %u pocket moves, pocket points max "
            "%u, pocket path checks max %u, solve mean %.0f us max %.0f us (soft limit 2000 us)%s\n", name, t.misses,
            t.runs, t.pocketMoves, t.maxPocketPoints, t.maxPocketChecks, t.runs ? t.totalUs / t.runs : 0.0, t.worstUs,
            t.worstUs > 2000.0 ? "  ** OVER **" : "");
    }

    void TestDecoyGridSolver()
    {
        // The drill's grid: every start finds the decoy's ground, melee or not. Without the pocket pass a quarter of
        // them miss it (the SWEEP-2 sweep: Zone/9 on 26 of 31 specs).
        for (bool melee : { false, true })
        {
            GridTally const on = DrillGrid(true, melee);
            GridTally const off = DrillGrid(false, melee);
            PrintTally(melee ? "drill, melee band, pocket pass" : "drill, pocket pass", on);
            PrintTally(melee ? "drill, melee band, as before" : "drill, as before", off);
            CHECK(on.runs == 2682);
            CHECK(on.misses == 0);
            CHECK(off.misses * 5 >= off.runs);         // 744 of 2682 as before
            CHECK(on.pocketMoves >= off.misses);
            CHECK(on.maxPocketPoints <= Solver::Detail::kMaxPocketCandidates);
            CHECK(on.maxPocketChecks <= Solver::Params{}.maxPathChecks);
        }
        // Jandice's real grid: every start within reach of the decoy's ground finds it.
        GridTally const on = JandiceGrid(true);
        GridTally const off = JandiceGrid(false);
        PrintTally("Jandice, pocket pass", on);
        PrintTally("Jandice, as before", off);
        CHECK(on.runs > 1000);
        CHECK(on.misses == 0);
        CHECK(off.misses * 5 >= off.runs);
        CHECK(on.maxPocketPoints <= Solver::Detail::kMaxPocketCandidates);
        CHECK(on.maxPocketChecks <= Solver::Params{}.maxPathChecks);
    }

    // The pocket pass's worst case: nothing safe anywhere (a field of overlapping circles round the bot and past its
    // reach), so it builds, pre-filters and drops every edge point and the Fallback runs as before. The plan is the
    // same as without the pass; the cost is printed against the same soft limit.
    void TestPocketPassWorstCase()
    {
        Solver::Input in;
        in.bot = { 0.f, 0.f, 0.f };
        in.now = kSolveAt;
        in.speed = kBotSpeed;
        std::uint32_t id = 1;
        for (float x = -45.f; x <= 45.f; x += 9.f)
            for (float y = -45.f; y <= 45.f; y += 9.f)
                in.hazards.push_back(Sigil(id++, { x, y, 0.f }, 7.f));
        StraightOracle oracle;
        Solver::Input off = in;
        off.params.hazardEdgeArcs = false;
        Solver::Plan const a = Solver::Solve(in, oracle);
        Solver::Plan const b = Solver::Solve(off, oracle);
        CHECK(a.outcome == Solver::Outcome::Fallback);
        CHECK(a.outcome == b.outcome);
        CHECK(Geo::Dist2d(a.target.x, a.target.y, b.target.x, b.target.y) < kEps);
        CHECK(a.pathChecks == b.pathChecks);
        CHECK(a.stats.pocketCandidates > 0 && a.stats.pocketCandidates <= Solver::Detail::kMaxPocketCandidates);
        CHECK(b.stats.pocketCandidates == 0);
        double worst[2] = { 0.0, 0.0 };
        double total[2] = { 0.0, 0.0 };
        constexpr int kRuns = 50;
        for (int r = 0; r < kRuns; ++r)
            for (int k = 0; k < 2; ++k)
            {
                auto const t0 = std::chrono::steady_clock::now();
                Solver::Plan const p = Solver::Solve(k ? off : in, oracle);
                double const us = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - t0).count();
                CHECK(p.outcome == a.outcome);
                worst[k] = std::max(worst[k], us);
                total[k] += us;
            }
        std::printf("pocket pass worst case: %zu hazards, %u pocket points, all dropped: mean %.0f us max %.0f us with "
            "the pass, mean %.0f us max %.0f us without (soft limit 2000 us)%s\n", in.hazards.size(),
            a.stats.pocketCandidates, total[0] / kRuns, worst[0], total[1] / kRuns, worst[1],
            total[0] / kRuns > 2000.0 ? "  ** OVER **" : "");
    }

    void TestGandlingPortals()
    {
        std::vector<Timed> const lanes = StepsWithTag(*Shapes(), Drill::kScholoTagPortal);
        CHECK(lanes.size() == 3);
        if (lanes.size() != 3)
            return;
        for (std::size_t i = 0; i < lanes.size(); ++i)
        {
            std::string_view const line = lanes[i].line;
            CHECK(Shape(line) == "line" && Size(line) == 40.f && Key(line, "width", 0.f) == 4.f);
            CHECK(lanes[i].resolveMs - lanes[i].startMs == 2000);
            CHECK(lanes[i].startMs == lanes[0].startMs);
            CHECK(Is(line, "at", "group") && Key(line, "away", 0.f) == 20.f);
            CHECK(Is(line, "to", "member") && Is(line, "fix", "1"));
            CHECK(Is(line, "pick", i == 0 ? "nontank" : "other"));
            CHECK(Is(line, "weak", "1"));
        }
        Vec3 const group{ 50.f, -20.f, 0.f };
        float const length = Size(lanes[0].line);
        float const away = Key(lanes[0].line, "away", 0.f);
        for (float bearing = 0.f; bearing < Geo::kTwoPi; bearing += 0.5f)
        {
            Vec3 const source = Drill::Away(group, bearing, away);
            for (float mb = 0.f; mb < Geo::kTwoPi; mb += 0.7f)
                for (float md : { 0.f, 5.f, 15.f })
                {
                    Vec3 const member = Drill::Away(group, mb, md);
                    Drill::FixedLine const l = Drill::FixLine(source, member, length, false, 0.f);
                    CHECK(Geo::Contains(Shape::Line(l.length, 4.f), { l.source.x, l.source.y, 0.f, l.facing },
                        member));
                    CHECK(length - Geo::Dist2d(source.x, source.y, member.x, member.y) >= 5.f - kEps);
                }
        }
    }

    // ---- helpers ---------------------------------------------------------------------------------------------------
    void TestGridPoints()
    {
        Vec3 const c{ 100.f, 200.f, 7.f };
        for (float facing : { 0.f, 1.2f, 3.9f })
        {
            for (std::uint32_t n = 2; n <= Drill::kMaxGrid; ++n)
            {
                std::vector<Vec3> const g = Drill::GridPoints(c, facing, n, 11.f);
                CHECK(g.size() == n * n);
                // Centred on c, on its z, neighbors exactly `apart` apart.
                CHECK_NEAR(Drill::Centroid(g).x, c.x, 1e-2);
                CHECK_NEAR(Drill::Centroid(g).y, c.y, 1e-2);
                float nearest = 1e9f;
                for (std::size_t i = 0; i < g.size(); ++i)
                {
                    CHECK_NEAR(g[i].z, c.z, kEps);
                    for (std::size_t j = i + 1; j < g.size(); ++j)
                        nearest = std::min(nearest, Geo::Dist2d(g[i].x, g[i].y, g[j].x, g[j].y));
                }
                CHECK_NEAR(nearest, 11.f, 1e-2);
            }
            // 3 x 3: the middle point is the centre; rows run along the facing.
            std::vector<Vec3> const g = Drill::GridPoints(c, facing, 3, 10.f);
            CHECK_NEAR(g[4].x, c.x, kEps);
            CHECK_NEAR(g[4].y, c.y, kEps);
            CHECK_NEAR(g[7].x - g[4].x, 10.f * std::cos(facing), kEps);
            CHECK_NEAR(g[7].y - g[4].y, 10.f * std::sin(facing), kEps);
        }
    }

    void TestPickDistinct()
    {
        std::uint32_t seed = 12345;
        auto lcg = [&seed](std::uint32_t n)
        {
            seed = seed * 1664525u + 1013904223u;
            return n ? (seed >> 8) % n : 0u;
        };
        for (std::uint32_t n : { 1u, 4u, 9u, 25u })
            for (std::uint32_t k : { 0u, 1u, 3u, 30u })
            {
                std::vector<std::uint32_t> const p = Drill::PickDistinct(n, k, lcg);
                CHECK(p.size() == std::min(n, k));
                std::set<std::uint32_t> const unique(p.begin(), p.end());
                CHECK(unique.size() == p.size());
                for (std::uint32_t i : p)
                    CHECK(i < n);
            }
        // Every cell can be the decoy.
        std::set<std::uint32_t> seen;
        for (int i = 0; i < 400; ++i)
            seen.insert(Drill::PickDistinct(9, 1, lcg).front());
        CHECK(seen.size() == 9);
        // An index function that overshoots is clamped.
        std::vector<std::uint32_t> const clamped = Drill::PickDistinct(3, 2, [](std::uint32_t) { return 99u; });
        CHECK(clamped.size() == 2 && clamped[0] == 2 && clamped[1] == 1);
    }

    // at=predict: the heading from the movement keys and the spot `lead` ahead.
    void TestPredict()
    {
        float const o = 0.8f;
        CHECK(!Drill::MoveHeading(o, false, false, false, false));
        CHECK(!Drill::MoveHeading(o, true, true, false, false));
        CHECK(!Drill::MoveHeading(o, false, false, true, true));
        CHECK_NEAR(*Drill::MoveHeading(o, true, false, false, false), o, kEps);
        CHECK_NEAR(*Drill::MoveHeading(o, false, true, false, false), o + Geo::kPi, kEps);
        CHECK_NEAR(*Drill::MoveHeading(o, false, false, true, false), o + Geo::kPi / 2.f, kEps);
        CHECK_NEAR(*Drill::MoveHeading(o, false, false, false, true), o - Geo::kPi / 2.f, kEps);
        CHECK_NEAR(*Drill::MoveHeading(o, true, false, true, false), o + Geo::kPi / 4.f, kEps);

        Vec3 const at{ 10.f, -5.f, 3.f };
        // Standing (no heading, or no speed): on the player.
        Vec3 const still = Drill::PredictSpot(at, std::nullopt, 7.f, 1500);
        CHECK_NEAR(still.x, at.x, kEps);
        CHECK_NEAR(still.y, at.y, kEps);
        Vec3 const stopped = Drill::PredictSpot(at, 1.f, 0.f, 1500);
        CHECK_NEAR(stopped.x, at.x, kEps);
        // Running at 7 yd/s: 10.5 yd ahead after 1.5 s, along the heading.
        Vec3 const ahead = Drill::PredictSpot(at, 0.f, Drill::kPlayerRunYdS, 1500);
        CHECK_NEAR(ahead.x, at.x + 10.5f, kEps);
        CHECK_NEAR(ahead.y, at.y, kEps);
        CHECK_NEAR(ahead.z, at.z, kEps);
        // A player who keeps running is in the 4 yd marker as it lands; one who stops dead as it appears is 10.5 yd
        // short of it (the dodge: change course or stop).
        CHECK(Geo::Dist2d(ahead.x, ahead.y, at.x + 10.5f, at.y) <= 4.f);
        CHECK(Geo::Dist2d(ahead.x, ahead.y, at.x, at.y) > 4.f);
    }

    // ---- the sweep's program ---------------------------------------------------------------------------------------

    // `scholomance-shapes:3` (and the drill's default) is one drill entry of 120 s, everything over inside it.
    void TestSweepProgram()
    {
        std::string err;
        for (char const* name : { "scholomance-shapes:3", "scholomance-shapes" })
        {
            std::optional<Drill::Program> const p = Drill::ParseProgram(name, err);
            CHECK(p && err.empty());
            if (!p || p->entries.size() != 1)
            {
                CHECK(false);
                continue;
            }
            Drill::ProgramEntry const& e = p->entries[0];
            CHECK(e.kind == Drill::EntryKind::Drill && e.drill == Shapes());
            CHECK(e.cycles == 3 && e.lengthMs == 120000);
            std::vector<Drill::ScheduledStep> const s = Drill::StepSchedule(*e.drill, e.cycles);
            CHECK(s.size() == 3 * e.drill->steps.size());
            for (Drill::ScheduledStep const& st : s)
                CHECK(st.delayMs + Drill::WarnMs(e.drill->steps[st.step].line) <= 120000u);
        }
    }
}

int main()
{
    TestTable();
    TestPolkeltChain();
    TestMaliciaBarrage();
    TestIlluciaForesight();
    TestRattlegoreOssuary();
    TestJandiceSteps();
    TestDecoyGridGeometry();
    TestDecoyGridSolver();
    TestPocketPassWorstCase();
    TestGandlingPortals();
    TestGridPoints();
    TestPickDistinct();
    TestPredict();
    TestSweepProgram();
    return Report("scholo_shapes_test");
}
