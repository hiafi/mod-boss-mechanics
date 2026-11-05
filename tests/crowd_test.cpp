// The barrage crowd (STAGE-M4-HANDOFF §3.7, WP-P): `.mech barrage seed=1` on a group of bots standing round the GM,
// run through the brain model (tests/brain_model.h): the share of Fallback solves, clear (no time inside an exact
// hitbox on the path taken) against hot, at 4 and 9 bots, and the hypotheses of §2.4. The fix it pins: the Escape
// re-check keeps a clear Fallback (Solver::FallbackHolds) instead of solving it again on every view rebuild.
// Build: tests/run.sh. `crowd_test sweep` and `crowd_test policies [seeds]` print the experiment tables of
// STAGE-M4-P-REPORT §3 (no checks).

#include "check.h"
#include "brain_model.h"
#include "MechanicBarrage.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace BossMechanics;
using BrainModel::Config;
using BrainModel::Counters;
using Geo::Vec3;
using Ms = std::chrono::milliseconds;

namespace
{
    namespace Bar = ::BossMechanics::Barrage;

    struct BarrageSpec
    {
        std::uint32_t bots = 9;
        std::uint32_t seed = 1;
        Ms every{ 900 };                // `.mech barrage` defaults: every=900, warn=2000, secs=30
        Ms warn{ 2000 };
        std::uint32_t secs = 30;
        float marginYd = 0.75f;         // BossMechanics.Bots.SafetyMarginYd
        Ms hold{ 500 };                 // BossMechanics.Bots.PostResolveHoldMs
    };

    // The follow slot of bot k round the GM at (0, 0): FollowDistance 1.5 yd and two looser rings, spread round the
    // GM like a stock formation.
    Vec3 Slot(std::uint32_t k, std::uint32_t n)
    {
        float const a = Geo::kTwoPi * static_cast<float>(k) / static_cast<float>(n) + 0.3f;
        float const r = 1.5f + static_cast<float>(k % 3);
        return { r * std::cos(a), r * std::sin(a), 0.f };
    }

    // `.mech barrage`: one shot every `every` for `secs` (BarrageShots), each at a member drawn by the command's dice
    // (PickMember: members in order, not the GM), its kind drawn from the five, its shot from MakeShot, all from one
    // mt19937 seeded with `seed`. The member is where it stands when the shot is registered.
    Counters RunBarrage(Config const& cfg, BarrageSpec const& spec, BrainModel::World* out = nullptr)
    {
        BrainModel::World world;
        world.cfg = cfg;
        for (std::uint32_t k = 0; k < spec.bots; ++k)
        {
            BrainModel::Bot b;
            b.pos = Slot(k, spec.bots);
            world.bots.push_back(b);
        }
        std::uint32_t const n = spec.bots;
        world.follow = [n](std::size_t i, Vec3& to)
        {
            to = Slot(static_cast<std::uint32_t>(i), n);
            return true;
        };
        Bar::Rng rng(spec.seed);
        std::uint32_t const shots = static_cast<std::uint32_t>((spec.secs * 1000 + spec.every.count() - 1)
            / spec.every.count());
        Ms const start{ 100000 };
        Ms const end = start + spec.every * shots + spec.warn + Ms(4000);
        std::uint32_t fired = 0;
        for (Ms now = start; now < end; now += cfg.step)
        {
            while (fired < shots && start + spec.every * fired <= now)
            {
                std::uint32_t const member = rng.Index(n);
                auto const kind = static_cast<Bar::Kind>(rng.Index(
                    Bar::kKindCount));
                Bar::Shot const shot = Bar::MakeShot(kind,
                    world.bots[member].pos, rng);
                Solver::Hazard h;
                h.id = 1000 + fired;
                h.shape = Bar::ShapeOf(shot);
                h.poses = { shot.pose };
                h.marginYd = spec.marginYd;
                Ms const resolveAt = now + spec.warn;
                if (kind == Bar::Kind::Projectile)
                {
                    ProjectileParams p;
                    p.length = shot.size;
                    p.speed = shot.speed;
                    p.halfLength = shot.width * 0.5f;
                    h.timing = Solver::MakeProjectile(p, resolveAt);
                }
                else
                    h.timing = Solver::MakeInstant(resolveAt, spec.hold);
                world.AddHazard(std::move(h));
                ++fired;
            }
            world.Step(now);
        }
        Counters const total = world.Total();
        if (out)
            *out = std::move(world);
        return total;
    }

    double Pct(std::uint32_t part, std::uint32_t whole)
    {
        return whole ? 100.0 * part / whole : 0.0;
    }

    void Print(char const* name, Counters const& c, std::uint32_t runs = 1)
    {
        std::printf("crowd %-34s solves %5u, move %5u, fallback %4u (%4.1f%%: clear %4u, hot %3u; pass-1 budget "
            "stop %3u), no path %u, budget stops %5u, hits %3u, solve us max %5lld mean %4lld, denied %u, kept %u%s\n",
            name,
            c.solves, c.outcomes[1] + c.outcomes[2], c.Fallbacks(), Pct(c.Fallbacks(), c.solves), c.fallbackClear,
            c.fallbackHot, c.fallbackBudget, c.outcomes[4], c.budgetStops, c.hits,
            static_cast<long long>(c.solveUsMax),
            static_cast<long long>(c.solves ? c.solveUsTotal / c.solves : 0), c.deniedSolves, c.fallbackKept,
            runs > 1 ? (" (" + std::to_string(runs) + " seeds)").c_str() : "");
    }

    Counters Seeds(Config const& cfg, BarrageSpec spec, std::uint32_t seeds)
    {
        Counters sum;
        for (std::uint32_t s = 1; s <= seeds; ++s)
        {
            spec.seed = s;
            sum.Add(RunBarrage(cfg, spec));
        }
        return sum;
    }

    void Sweep()
    {
        constexpr std::uint32_t kSeeds = 20;
        for (std::uint32_t bots : { 4u, 9u })
        {
            BarrageSpec spec;
            spec.bots = bots;
            for (std::uint32_t checks : { 8u, 12u, 16u })
            {
                Config cfg;
                cfg.params.maxPathChecks = checks;
                char name[64];
                std::snprintf(name, sizeof(name), "%u bots, MaxPathChecks %u, seed 1", bots, checks);
                Print(name, RunBarrage(cfg, spec));
                std::snprintf(name, sizeof(name), "%u bots, MaxPathChecks %u", bots, checks);
                Print(name, Seeds(cfg, spec, kSeeds), kSeeds);
            }
            Config noCrowd;
            noCrowd.crowd = false;
            char name[64];
            std::snprintf(name, sizeof(name), "%u bots, no crowd term", bots);
            Print(name, Seeds(noCrowd, spec, kSeeds), kSeeds);
            for (int keep : { 1, 2 })
            {
                Config k;
                k.fallbackKeep = keep;
                std::snprintf(name, sizeof(name), "%u bots, keep %s fallbacks", bots, keep == 1 ? "clear" : "all");
                Print(name, Seeds(k, spec, kSeeds), kSeeds);
            }
        }
    }

    void Policies(std::uint32_t seeds)
    {
        for (std::uint32_t bots : { 4u, 5u, 9u, 10u })
            for (int keep : { 0, 1, 2 })
            {
                BarrageSpec spec;
                spec.bots = bots;
                Config k;
                k.fallbackKeep = keep;
                char name[64];
                std::snprintf(name, sizeof(name), "%u bots, keep %s", bots, keep == 0 ? "none (M3)"
                    : (keep == 1 ? "clear" : "all"));
                Print(name, Seeds(k, spec, seeds), seeds);
            }
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "sweep") == 0)
    {
        Sweep();
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "policies") == 0)
    {
        Policies(argc > 2 ? static_cast<std::uint32_t>(std::atoi(argv[2])) : 100);
        return 0;
    }
    // seed 1, as in the M3 playtest (1.1: 9 bots, a third of the solves Fallback, 1 hit).
    Config const cfg;
    BarrageSpec nine;
    Counters const c9 = RunBarrage(cfg, nine);
    BarrageSpec four;
    four.bots = 4;
    Counters const c4 = RunBarrage(cfg, four);
    Print("9 bots, seed 1", c9);
    Print("4 bots, seed 1", c4);
    CHECK(c9.solves > 0 && c4.solves > 0);
    CHECK(c9.fallbackClear + c9.fallbackHot == c9.Fallbacks());

    // 20 seeds at 9 bots: the M3 re-check (every rebuild re-solves a Fallback escape) against the M4 one (a clear
    // Fallback walks on while its rest stays clear). Fewer solves, fewer Fallbacks, far fewer hits.
    Config m3;
    m3.fallbackKeep = 0;
    Counters const before = Seeds(m3, nine, 20);
    Counters const after = Seeds(cfg, nine, 20);
    Print("9 bots, M3 re-check", before, 20);
    Print("9 bots, M4 re-check", after, 20);
    CHECK(after.fallbackKept > 0);
    CHECK(after.solves < before.solves);
    CHECK(after.Fallbacks() * 10 < before.Fallbacks() * 9);
    CHECK(after.hits * 10 < before.hits * 7);
    CHECK(after.solves && after.solveUsTotal / after.solves < 2000);
    return Report("crowd_test");
}
