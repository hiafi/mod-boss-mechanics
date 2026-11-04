/*
 * mod-boss-mechanics: bot fallibility (dungeon-mechanics STAGE-M2-HANDOFF §3.8, PLAN D1).
 *
 * Pure header: no AzerothCore or playerbots includes (tests/solver_test.cpp compiles it with the host compiler).
 * Every roll is a hash of (seed, bot, mechanic, salt), so a given seed replays exactly: the same bot misreads the
 * same mechanic the same way. Off by default; with Enable = 0 (scale 0) no roll is taken and every mechanic is seen
 * the moment it registers.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_FALLIBILITY_H
#define MOD_BOSS_MECHANICS_MECHANIC_FALLIBILITY_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace BossMechanics::Fallibility
{
    using Ms = std::chrono::milliseconds;

    // Salts (§3.8). The jitter roll uses mechanic 0, so it is a stable per-bot trait.
    constexpr std::uint64_t kSaltJitter = 1;
    constexpr std::uint64_t kSaltLate = 2;
    constexpr std::uint64_t kSaltWrongSide = 3;

    // BossMechanics.Bots.Fallibility.* (§3.2). `seed` is the resolved seed: the config loader replaces a configured 0
    // with one random 64-bit value per load.
    struct Config
    {
        bool enable = false;
        std::uint32_t reactionMs = 350;
        std::uint32_t jitterMs = 250;
        float lateChancePct = 5.f;
        std::uint32_t lateExtraMs = 700;
        float wrongSideChancePct = 3.f;
        std::uint32_t realiseMs = 700;
        float scaleNormal = 1.f;
        float scaleHeroic = 1.f;
        std::uint64_t seed = 0;
    };

    // How one bot perceives one mechanic. Computed once, when the bot first sees the mechanic in the registry.
    struct Perception
    {
        Ms visibleAt{ 0 };      // the bot plans with the mechanic from here on...
        Ms realiseAt{ 0 };      // ...and, when wrongSide, not before this either
        bool late = false;
        bool wrongSide = false;
    };

    // splitmix64 finaliser.
    inline std::uint64_t SplitMix(std::uint64_t x)
    {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    // splitmix64 chain over the four keys.
    inline std::uint64_t Mix(std::uint64_t seed, std::uint64_t bot, std::uint64_t mechanic, std::uint64_t salt)
    {
        return SplitMix(SplitMix(SplitMix(SplitMix(seed) ^ bot) ^ mechanic) ^ salt);
    }

    // Uniform in [0, 1): the top 24 bits of Mix / 2^24.
    inline float Roll01(std::uint64_t seed, std::uint64_t bot, std::uint64_t mechanic, std::uint64_t salt)
    {
        return static_cast<float>(Mix(seed, bot, mechanic, salt) >> 40) / 16777216.f;
    }

    // The scale for a map: 0 when disabled (no roll is taken), else ScaleHeroic or ScaleNormal.
    inline float ScaleFor(Config const& c, bool heroic)
    {
        if (!c.enable)
            return 0.f;
        return std::max(0.f, heroic ? c.scaleHeroic : c.scaleNormal);
    }

    // The per-bot reaction delay (scale × (ReactionMs + JitterMs × jitter roll)), without the late extra.
    inline Ms ReactionDelay(Config const& c, float scale, std::uint64_t bot)
    {
        if (scale <= 0.f)
            return Ms(0);
        float const jitter = static_cast<float>(c.jitterMs) * Roll01(c.seed, bot, 0, kSaltJitter);
        return Ms(static_cast<std::int64_t>(std::lround(scale * (static_cast<float>(c.reactionMs) + jitter))));
    }

    // Perception of mechanic `mechanic` (registered at createdAt, first hot at firstWindowStart) by `bot`.
    inline Perception Perceive(Config const& c, float scale, std::uint64_t bot, std::uint32_t mechanic, Ms createdAt,
        Ms firstWindowStart)
    {
        Perception p;
        p.visibleAt = createdAt;
        p.realiseAt = createdAt;
        if (scale <= 0.f)
            return p;

        p.visibleAt = createdAt + ReactionDelay(c, scale, bot);
        if (Roll01(c.seed, bot, mechanic, kSaltLate) < scale * c.lateChancePct / 100.f)
        {
            p.late = true;
            p.visibleAt += Ms(c.lateExtraMs);
        }
        if (Roll01(c.seed, bot, mechanic, kSaltWrongSide) < scale * c.wrongSideChancePct / 100.f)
        {
            p.wrongSide = true;
            p.realiseAt = firstWindowStart - Ms(c.realiseMs);
        }
        return p;
    }

    // The moment the bot starts planning with the mechanic.
    inline Ms SeenFrom(Perception const& p)
    {
        return p.wrongSide ? std::max(p.visibleAt, p.realiseAt) : p.visibleAt;
    }

    // Does the bot plan with the mechanic at `now`? When not, `until` (if given) gets the moment it will.
    inline bool Sees(Perception const& p, Ms now, Ms* until = nullptr)
    {
        Ms const from = SeenFrom(p);
        if (now >= from)
            return true;
        if (until)
            *until = from;
        return false;
    }
}

#endif
