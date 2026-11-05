// The barrage crowd (STAGE-M4-HANDOFF §3.7, WP-P): `.mech barrage seed=1` on a group of bots standing round the GM,
// run through the brain model (tests/brain_model.h): the share of Fallback solves, clear (no time inside an exact
// hitbox on the path taken) against hot, at 4 and 9 bots, and the hypotheses of §2.4. The fix it pins: the Escape
// re-check keeps a clear Fallback (Solver::FallbackHolds) instead of solving it again on every view rebuild.
// Build: tests/run.sh. `crowd_test sweep` and `crowd_test policies [seeds]` print the experiment tables of
// STAGE-M4-P-REPORT §3 (no checks).

#include "check.h"
#include "brain_model.h"
#include "MechanicBarrage.h"
#include <array>
#include <cmath>
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

    // M5 CD (C3 A, C5): `barrage:60` in the dpssim mechanics layout (SimMechanicsScenario: the boss on the origin,
    // the tank 3 yd in front, the others at 12 yd, the healer at 18 yd). `meleeActor`: bots 0 (the tank), 2 and 3
    // are melee (the sweep's melee-actor runs); else 0, 2 and 4. The melee chase runs under the lock's ring test
    // (Config::chaseLanding) with the chase valve; the barrage's dice as `.mech barrage` (PickMember over the five).
    struct MeleeBarrage
    {
        Counters all;
        std::uint32_t meleeHits = 0;
        std::uint32_t donutHits = 0;            // hits by a donut (the sweep's Zone/2)
        std::uint32_t msChaseStranded = 0;      // melee only
        std::uint32_t msChaseRefused = 0;
        std::uint32_t meleeIterations = 0;
        std::uint32_t strandedOver4s = 0;       // melee-iterations stranded over 4,000 ms
        std::uint32_t longestStrandedRun = 0;   // the longest continuous msChaseStranded run, ms
    };

    void RunMeleeBarrage(Config const& cfg, std::uint32_t seed, bool meleeActor, MeleeBarrage& out)
    {
        BrainModel::World w;
        w.cfg = cfg;
        w.cfg.chaseLanding = true;
        w.cfg.roleSlack = Ms(1000);
        std::array<bool, 5> melee{ { true, false, true, meleeActor, !meleeActor } };
        std::array<float, 5> const bearing{ { 180.f, 150.f, 170.f, 190.f, 210.f } };
        auto at = [](float r, float deg)
        {
            float const a = Geo::DegToRad(deg);
            return Vec3{ r * std::cos(a), r * std::sin(a), 0.f };
        };
        for (std::size_t i = 0; i < 5; ++i)
        {
            BrainModel::Bot b;
            b.pos = i == 0 ? at(3.f, 180.f) : (melee[i] ? at(3.5f, bearing[i]) : at(i == 1 ? 18.f : 12.f, bearing[i]));
            w.bots.push_back(b);
        }
        float const meleeYd = 3.8f;
        float const chaseYd = 3.5f;
        w.isTank = [](std::size_t i) { return i == 0; };
        w.isHealer = [](std::size_t i) { return i == 1; };
        w.follow = [&w, &melee, meleeYd, chaseYd](std::size_t i, Vec3& to)
        {
            Vec3 const p = w.bots[i].pos;
            float const d = std::hypot(p.x, p.y);
            if (!melee[i] || d <= meleeYd + 0.5f)
                return false;
            to = { p.x * chaseYd / d, p.y * chaseYd / d, 0.f };
            return true;
        };
        w.kinds = [&w, &melee, meleeYd](std::size_t i, Solver::Input& in, Ms, std::vector<BrainModel::Task>&)
        {
            in.role.present = true;
            in.role.anchor = i == 1 ? w.bots[0].pos : Vec3{};
            in.role.minYd = melee[i] ? 0.f : 8.f;
            in.role.maxYd = melee[i] ? meleeYd : 28.f;
        };
        Bar::Rng rng(seed);
        Ms const every{ 900 };
        std::uint32_t const shots = (60000 + 899) / 900;
        Ms const start{ 100000 };
        std::uint32_t fired = 0;
        std::vector<Bar::Kind> kinds;
        std::array<std::uint32_t, 5> run{};
        std::array<std::uint32_t, 5> prev{};
        for (Ms now = start; now < start + every * shots + Ms(6000); now += w.cfg.step)
        {
            while (fired < shots && start + every * fired <= now)
            {
                std::uint32_t const member = rng.Index(5);
                auto const kind = static_cast<Bar::Kind>(rng.Index(Bar::kKindCount));
                Bar::Shot const shot = Bar::MakeShot(kind, w.bots[member].pos, rng);
                kinds.push_back(kind);
                Solver::Hazard h;
                h.id = 1000 + fired;
                h.shape = Bar::ShapeOf(shot);
                h.poses = { shot.pose };
                h.marginYd = 0.75f;
                Ms const resolveAt = now + Ms(2000);
                if (kind == Bar::Kind::Projectile)
                {
                    ProjectileParams p;
                    p.length = shot.size;
                    p.speed = shot.speed;
                    p.halfLength = shot.width * 0.5f;
                    h.timing = Solver::MakeProjectile(p, resolveAt);
                }
                else
                    h.timing = Solver::MakeInstant(resolveAt, Ms(500));
                w.AddHazard(std::move(h));
                ++fired;
            }
            for (std::size_t i = 0; i < 5; ++i)
                prev[i] = w.bots[i].c.msChaseStranded;
            w.Step(now);
            for (std::size_t i = 0; i < 5; ++i)
            {
                run[i] = w.bots[i].c.msChaseStranded > prev[i] ? run[i] + 100 : 0;
                out.longestStrandedRun = std::max(out.longestStrandedRun, run[i]);
            }
        }
        out.all.Add(w.Total());
        for (BrainModel::Bot const& b : w.bots)
            for (std::uint32_t id : b.hitBy)
                out.donutHits += id >= 1000 && id - 1000 < kinds.size() && kinds[id - 1000] == Bar::Kind::Donut;
        for (std::size_t i = 0; i < 5; ++i)
        {
            if (!melee[i])
                continue;
            BrainModel::Counters const& c = w.bots[i].c;
            out.meleeHits += c.hits;
            out.msChaseStranded += c.msChaseStranded;
            out.msChaseRefused += c.msChaseRefused;
            ++out.meleeIterations;
            out.strandedOver4s += c.msChaseStranded > 4000;
        }
    }

    MeleeBarrage MeleeBarrageSeeds(Config const& cfg, bool meleeActor, std::uint32_t seeds)
    {
        MeleeBarrage m;
        for (std::uint32_t s = 1; s <= seeds; ++s)
            RunMeleeBarrage(cfg, s, meleeActor, m);
        return m;
    }

    void PrintMelee(char const* name, MeleeBarrage const& m)
    {
        std::printf("melee barrage %-30s hits %3u (melee %3u, donut %3u), fallbacks %3u, valve %3u, stranded %6u ms "
            "(%u over 4 s, longest run %u ms), refused %6u ms, strand routes %u\n", name, m.all.hits, m.meleeHits,
            m.donutHits, m.all.Fallbacks(), m.all.chaseReleases, m.msChaseStranded, m.strandedOver4s,
            m.longestStrandedRun, m.msChaseRefused, m.all.strandRoutes);
    }

    // The WP-F2 build: the valve releases blind, no stranding route, no spacing.
    Config AsBuiltF2()
    {
        Config c;
        c.walkIn = false;
        c.strandRoute = false;
        c.standing = false;
        return c;
    }

    // `crowd_test melee [seeds]`: the melee barrage arms one fix at a time (STAGE-M5-CD-FIX-REPORT).
    void MeleeArms(std::uint32_t seeds)
    {
        for (bool meleeActor : { true, false })
        {
            std::printf("melee barrage, %s, barrage:60 seeds 1-%u:\n", meleeActor ? "melee actor (0, 2, 3)"
                : "ranged actor (0, 2, 4)", seeds);
            Config c = AsBuiltF2();
            PrintMelee("as built (WP-F2)", MeleeBarrageSeeds(c, meleeActor, seeds));
            c.walkIn = true;
            PrintMelee("walk-in", MeleeBarrageSeeds(c, meleeActor, seeds));
            c.strandRoute = true;
            PrintMelee("walk-in + strand route", MeleeBarrageSeeds(c, meleeActor, seeds));
            c.strandRoute = false;
            c.standing = true;
            PrintMelee("walk-in + spacing", MeleeBarrageSeeds(c, meleeActor, seeds));
            PrintMelee("all (default config)", MeleeBarrageSeeds(Config(), meleeActor, seeds));
        }
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
    if (argc > 1 && std::strcmp(argv[1], "melee") == 0)
    {
        MeleeArms(argc > 2 ? static_cast<std::uint32_t>(std::atoi(argv[2])) : 20);
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
    // Both without the M5 spacing term (Config::standing), which this comparison predates.
    Config m3;
    m3.fallbackKeep = 0;
    m3.standing = false;
    Config m4;
    m4.standing = false;
    Counters const before = Seeds(m3, nine, 20);
    Counters const after = Seeds(m4, nine, 20);
    Counters const spaced = Seeds(cfg, nine, 20);
    Print("9 bots, M3 re-check", before, 20);
    Print("9 bots, M4 re-check", after, 20);
    Print("9 bots, M4 re-check + M5 spacing", spaced, 20);
    CHECK(after.fallbackKept > 0);
    CHECK(after.solves < before.solves);
    CHECK(after.Fallbacks() * 10 < before.Fallbacks() * 9);
    CHECK(after.hits * 10 < before.hits * 7);
    CHECK(after.solves && after.solveUsTotal / after.solves < 2000);
    // M5 CD (C3 B): the spacing term does not cost the crowd round the GM hits.
    CHECK(spaced.hits <= after.hits + 2);

    // M5 CD (C3 A): the melee barrage, 20 seeds, in both sim layouts. The blind valve release walked melee into the
    // donuts registered on them (and the mines and lanes on the chase line); the walk-in answers them. The walk-in
    // arm has only the walk-in on. The strand route and spacing on top keep the hits level (summed over both layouts:
    // 20 seeds of one layout swing by about 15% either way; `crowd_test melee 60` prints the 60-seed arms).
    std::uint32_t walkedHits = 0;
    std::uint32_t fixedHits = 0;
    for (bool meleeActor : { true, false })
    {
        Config walkIn = AsBuiltF2();
        walkIn.walkIn = true;
        MeleeBarrage const asBuilt = MeleeBarrageSeeds(AsBuiltF2(), meleeActor, 20);
        MeleeBarrage const walked = MeleeBarrageSeeds(walkIn, meleeActor, 20);
        MeleeBarrage const fixed = MeleeBarrageSeeds(cfg, meleeActor, 20);

        std::printf("melee barrage, %s, barrage:60 seeds 1-20:\n", meleeActor ? "melee actor (0, 2, 3)"
            : "ranged actor (0, 2, 4)");
        PrintMelee("as built (WP-F2)", asBuilt);
        PrintMelee("walk-in", walked);
        PrintMelee("walk-in + strand route + spacing", fixed);
        CHECK(asBuilt.all.hits > 0);
        CHECK(walked.all.hits * 4 <= asBuilt.all.hits * 3);
        walkedHits += walked.all.hits;
        fixedHits += fixed.all.hits;
        // C5: the strand route ends the runs the walk-in leaves; no stranded run over ChaseReleaseMs + 500.
        CHECK(fixed.msChaseStranded * 2 < walked.msChaseStranded);
        CHECK(fixed.longestStrandedRun <= 3500);
    }
    CHECK(fixedHits * 10 <= walkedHits * 11);
    return Report("crowd_test");
}
