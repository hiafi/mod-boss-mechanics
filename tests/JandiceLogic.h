/*
 * mod-dungeon-bosses, Scholomance S1:
 * Jandice Barov (10503), the pure, host-testable part (no engine headers): spell ids (kSpellsJandice), tuning
 * constants (normal, heroic), the cast cadence, the Shell Game clock and its find rule, the floor and ring points, the
 * shuffle pairs and the Bloodlines reflection spots. Build spec SC 546-601 (docs/dungeons/scholomance-rework.md);
 * handoff section 3.4 rows JB, section 3.8 (the 21 yd aisle), section 7 #12. Tested by tests/scholo_jandice_test.cpp.
 */

#ifndef MOD_DUNGEON_BOSSES_SCHOLOMANCE_JANDICE_LOGIC_H
#define MOD_DUNGEON_BOSSES_SCHOLOMANCE_JANDICE_LOGIC_H

#include "ScholomanceRework.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ScholoJandice
{
    using DungeonKit::FrameRect;
    using DungeonKit::NH;
    using DungeonKit::Vec2;

    // Every spell of this WP sits in this slice (ScholomanceRework.h; ids.yaml reservation `scholomance`).
    inline constexpr ScholomanceRework::IdSlice kSpellSlice = ScholomanceRework::kSpellsJandice;

    // apps/dbc-tools source/npcs/scholomance/jandice_barov.py declares the same ids (lib/test_scholomance_jandice.py).
    enum JandiceSpells : std::uint32_t
    {
        SPELL_JB_CURSE_OF_BLOOD     = 206110,   // JB9: 2 s cast, 10 yd, physical damage taken +20%, 30 s, Curse
        SPELL_JB_SHELL_GAME         = 206111,   // JB2: the vanish look (self)
        SPELL_JB_SHATTER            = 206112,   // JB3: a copy's hit on its attacker (RefFlatOn, Shadow)
        SPELL_JB_SHATTER_LOOK       = 206113,   // JB3: the copy shatters (self)
        SPELL_JB_UNMASKED           = 206114,   // JB4: damage taken +20%, 8 s, on Jandice
        SPELL_JB_BLOOD_SIGIL        = 206115,   // JB1 / JB5: the registry resolve copy (Shadow)
        SPELL_JB_BLOODLINES         = 206116,   // JB6: the registry resolve copy (Shadow)
        SPELL_JB_BLOOD_PRICE        = 206117,   // JB8 (heroic): 4% Shadow on every player (RefFlat)
        SPELL_JB_REFLECTION_POOF    = 206118,   // a reflection or a copy appears / dissolves (self)
        SPELL_JB_SIGIL_BURST        = 206119,   // a real circle bursts at its caster (self)
        SPELL_JB_FAKE_POP           = 206120,   // the heroic decoy pops harmlessly (self)
        SPELL_JB_BLOOD_THREAD       = 206121,   // JB7 (heroic): the 1 s thread between a swapping pair
        SPELL_JB_BLOOD_BEAM         = 206122,   // JB6: a Bloodlines reflection's beam (self, until it dissolves)
    };

    // creature_equip_template of the Reflection 300530 (the SQL rev, ID 1: the journal 12866). The journal marks real
    // magic; an empty-handed reflection (heroic decoy) and every copy clear their equipment (LoadEquipment(0, true)).
    std::int8_t constexpr kEquipJournal = 1;

    // Registry tags (the ledger and `.mech list`).
    enum JandiceTags : std::uint32_t
    {
        kTagBloodSigil       = 1,   // JB1 real circle
        kTagBloodSigilDecoy  = 2,   // JB1 heroic decoy
        kTagFailedGame       = 3,   // JB5
        kTagBloodlines       = 4,   // JB6 lanes (and the heroic decoy lane)
        kTagBloodlinesAlcove = 5,   // JB6 the alcove circle
    };

    // --- Cadence (SC "Cast order outside the Shell Game", JB1 / JB2 / JB6 / JB9) -----------------------------------
    std::uint32_t constexpr kSigilFirstMs = 8000;
    std::uint32_t constexpr kLinesFirstMs = 18000;      // 10 s after the first sigils (the alternation)
    std::uint32_t constexpr kGameFirstMs = 25000;
    std::uint32_t constexpr kGameEveryMs = 50000;       // start to start
    std::uint32_t constexpr kOutsideEveryMs = 10000;    // one outside cast every 10 s, alternating
    std::uint32_t constexpr kAfterGameMs = 6000;        // the first sigils after a game
    std::uint32_t constexpr kCurseFirstMinMs = 3000;
    std::uint32_t constexpr kCurseFirstMaxMs = 11000;
    std::uint32_t constexpr kCurseEveryMs = 30000;
    std::uint32_t constexpr kCurseCastMs = 2000;
    std::uint32_t constexpr kCurseRetryMs = 1000;
    std::uint32_t constexpr kCurseQuietBeforeGameMs = 3000;     // no curse cast bar this close to a game
    float constexpr kCurseRadius = 10.f;

    // --- JB1 Blood Sigil ---------------------------------------------------------------------------------------------
    float constexpr kSigilRadius = 8.f;
    inline constexpr NH<std::uint32_t> kSigilWarnMs{ 5000, 6000 };
    inline constexpr NH<float> kSigilPct{ 40.f, 60.f };
    std::uint32_t constexpr kNormalSigils = 3;
    float constexpr kSigilMinFromPlayers = 6.f;         // "never on a player" (handoff section 3.4)
    float constexpr kSigilMinApart = 12.f;              // three circles cover about half the aisle
    float constexpr kSigilFloorInset = 2.f;
    std::uint32_t constexpr kSigilTries = 60;

    // --- JB2-JB5, JB7, JB8 The Shell Game ----------------------------------------------------------------------------
    float constexpr kRingRadius = 12.f;
    float constexpr kRingWallInset = 1.f;
    inline constexpr NH<std::uint32_t> kCopies{ 8, 15 };
    inline constexpr NH<std::uint32_t> kGameLimitMs{ 20000, 25000 };
    inline constexpr NH<std::uint32_t> kFindsToEnd{ 1, 2 };
    inline constexpr NH<float> kShatterPct{ 25.f, 40.f };
    inline constexpr NH<std::uint32_t> kFailedWarnMs{ 5000, 6000 };
    inline constexpr NH<float> kFailedPct{ 40.f, 60.f };
    std::uint32_t constexpr kVanishLookMs = 300;        // the vanish look plays, then she is gone
    std::uint32_t constexpr kVanishMs = 1000;           // gone this long, then she stands in the ring
    std::uint32_t constexpr kFindGraceMs = 500;         // hits in flight from before the vanish never count
    std::uint32_t constexpr kMirrorEveryMs = 500;       // copies show her health and mana
    std::uint32_t constexpr kUnmaskedMs = 8000;         // the DBC duration (jandice_barov.py)
    std::uint32_t constexpr kShuffleEveryMs = 5000;     // heroic
    std::uint32_t constexpr kShufflePairs = 3;          // 6 figures
    std::uint32_t constexpr kThreadMs = 1000;
    std::uint32_t constexpr kSwapMs = 750;              // the swap run
    float constexpr kSwapMinSpeed = 7.f;
    // Heroic, the shuffle after the first find: her pair blinks (both gone this long) and trades places, so no player
    // keeps her selected through it; finds wait until it is over.
    std::uint32_t constexpr kBlinkMs = 300;
    std::uint32_t constexpr kFoundGraceMs = kThreadMs + kBlinkMs + 200;
    std::uint32_t constexpr kBloodPriceEveryMs = 3000;  // heroic
    float constexpr kBloodPricePct = 4.f;
    std::uint32_t constexpr kDissolveMs = 400;          // a look plays before the unit goes

    // --- JB6 Bloodlines ----------------------------------------------------------------------------------------------
    float constexpr kLaneWidth = 6.f;
    inline constexpr NH<std::uint32_t> kLinesWarnMs{ 5000, 5000 };
    inline constexpr NH<float> kLinesPct{ 50.f, 70.f };
    float constexpr kLaneEndInset = 1.f;                // a reflection stands this far off its wall

    // ----------------------------------------------------------------------------------------------------------------
    // The cadence: Blood Sigil and Bloodlines alternate every 10 s outside the game (sigils at 8 s, lines at 18 s, the
    // first game at 25 s, then every 50 s start to start; sigils 6 s after a game ends). A due game waits until the
    // outside cast before it has resolved (no sigils during the game, SC "No sigils during the game"); a due outside
    // cast waits behind a due game. Inside the game the outside clock is frozen.
    // ----------------------------------------------------------------------------------------------------------------
    enum class JandiceCast : std::uint8_t
    {
        None,
        BloodSigil,
        Bloodlines,
        ShellGame,
    };

    class JandiceClock
    {
    public:
        void Start()
        {
            _untilGame = kGameFirstMs;
            _untilOutside = kSigilFirstMs;
            _next = JandiceCast::BloodSigil;
            _inGame = false;
        }

        // `castsLive`: an outside cast (or a failed game's circles) still has live zones.
        JandiceCast Update(std::uint32_t diff, bool castsLive)
        {
            _untilGame = Sub(_untilGame, diff);
            if (_inGame)
                return JandiceCast::None;
            _untilOutside = Sub(_untilOutside, diff);
            if (_untilGame == 0)
            {
                if (castsLive)
                    return JandiceCast::None;
                _inGame = true;
                _untilGame = kGameEveryMs;
                return JandiceCast::ShellGame;
            }
            if (_untilOutside == 0)
            {
                JandiceCast const cast = _next;
                _next = _next == JandiceCast::BloodSigil ? JandiceCast::Bloodlines : JandiceCast::BloodSigil;
                _untilOutside = kOutsideEveryMs;
                return cast;
            }
            return JandiceCast::None;
        }

        // The game is over (found, or the failed game's circles burst): sigils in 6 s, then the alternation.
        void GameEnded()
        {
            _inGame = false;
            _untilOutside = kAfterGameMs;
            _next = JandiceCast::BloodSigil;
        }

        // A game started by hand (`.dbtest`): the clock treats it as its own.
        void GameStartedByHand()
        {
            _inGame = true;
            _untilGame = kGameEveryMs;
        }

        bool InGame() const { return _inGame; }
        std::uint32_t UntilGameMs() const { return _untilGame; }
        std::uint32_t UntilOutsideMs() const { return _untilOutside; }
        JandiceCast Next() const { return _next; }

    private:
        static std::uint32_t Sub(std::uint32_t v, std::uint32_t d) { return v > d ? v - d : 0; }

        std::uint32_t _untilGame = kGameFirstMs;
        std::uint32_t _untilOutside = kSigilFirstMs;
        JandiceCast _next = JandiceCast::BloodSigil;
        bool _inGame = false;
    };

    inline char const* CastName(JandiceCast cast)
    {
        switch (cast)
        {
            case JandiceCast::BloodSigil: return "blood-sigil";
            case JandiceCast::Bloodlines: return "bloodlines";
            case JandiceCast::ShellGame:  return "shell-game";
            default:                      return "none";
        }
    }

    // ----------------------------------------------------------------------------------------------------------------
    // The Shell Game clock (from the vanish): heroic Blood Price every 3 s and a shuffle every 5 s, the limit at
    // 20 s / 25 s (the limit tick never shuffles or pays: the game is over).
    // ----------------------------------------------------------------------------------------------------------------
    struct GameTick
    {
        std::uint32_t prices = 0;
        std::uint32_t shuffles = 0;
        bool limit = false;
    };

    class ShellGameClock
    {
    public:
        void Start(bool heroic)
        {
            _heroic = heroic;
            _elapsed = 0;
            _running = true;
        }

        void Stop() { _running = false; }
        bool Running() const { return _running; }
        std::uint32_t ElapsedMs() const { return _elapsed; }
        std::uint32_t LimitMs() const { return kGameLimitMs(_heroic); }

        GameTick Update(std::uint32_t diff)
        {
            GameTick tick;
            if (!_running)
                return tick;
            std::uint32_t const before = _elapsed;
            std::uint32_t const limit = LimitMs();
            _elapsed = std::min(limit, _elapsed + diff);
            if (_heroic)
            {
                tick.prices = Crossed(before, _elapsed, kBloodPriceEveryMs, limit);
                tick.shuffles = Crossed(before, _elapsed, kShuffleEveryMs, limit);
            }
            if (_elapsed >= limit)
            {
                tick.limit = true;
                _running = false;
            }
            return tick;
        }

    private:
        // Multiples of `every` in (before, now], the limit itself excluded.
        static std::uint32_t Crossed(std::uint32_t before, std::uint32_t now, std::uint32_t every,
            std::uint32_t limit)
        {
            std::uint32_t n = 0;
            for (std::uint32_t t = (before / every + 1) * every; t <= now; t += every)
                if (t < limit)
                    ++n;
            return n;
        }

        std::uint32_t _elapsed = 0;
        bool _heroic = false;
        bool _running = false;
    };

    // A find (a direct hit on the real Jandice): normal ends the game and Unmasks her; heroic Unmasks her on the first
    // find and she shuffles once more, the second find ends the game (no second Unmask).
    enum class FindResult : std::uint8_t
    {
        UnmaskAndEnd,
        UnmaskAndShuffle,
        End,
    };

    constexpr FindResult FindOutcome(bool heroic, std::uint32_t findsSoFar)
    {
        if (!heroic)
            return FindResult::UnmaskAndEnd;
        return findsSoFar <= 1 ? FindResult::UnmaskAndShuffle : FindResult::End;
    }

    // What counts as a direct hit on a figure (JB3 / JB2): a melee swing from a player or a pet, or a harmful spell
    // aimed at that unit (it needs an explicit unit target and is not an area spell). Area damage never does.
    constexpr bool CountsAsDirectHit(bool meleeSwing, bool spellHarmful, bool spellArea, bool spellNeedsUnitTarget)
    {
        if (meleeSwing)
            return true;
        return spellHarmful && !spellArea && spellNeedsUnitTarget;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Points
    // ----------------------------------------------------------------------------------------------------------------

    // `p` clamped into `f`, `inset` yd off every edge (the middle of an axis that cannot hold the inset).
    inline Vec2 ClampIntoFrame(FrameRect const& f, Vec2 p, float inset)
    {
        Vec2 q = DungeonKit::ToFrame(f, p);
        auto clampAxis = [inset](float v, float size)
        {
            if (size <= 2.f * inset)
                return size * 0.5f;
            return std::clamp(v, inset, size - inset);
        };
        q.x = clampAxis(q.x, f.sizeX);
        q.y = clampAxis(q.y, f.sizeY);
        return DungeonKit::FramePoint(f, q.x, q.y);
    }

    // The Shell Game ring's centre: where she stood, moved into the aisle far enough for the ring along each axis
    // that can hold it (the 21 yd aisle cannot hold a 24 yd ring across: the centre goes to its middle line).
    inline Vec2 RingCenter(FrameRect const& f, Vec2 at, float ringRadius, float inset)
    {
        return ClampIntoFrame(f, at, ringRadius + inset);
    }

    // `n` points evenly on a circle of `radius` around `center`, the first at `startAngle`, each clamped into `f`
    // (`inset` off the walls: the ring flattens against the aisle's long walls).
    inline std::vector<Vec2> RingPoints(FrameRect const& f, Vec2 center, float radius, std::uint32_t n,
        float startAngle, float inset)
    {
        std::vector<Vec2> out;
        out.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i)
        {
            float const a = startAngle + DungeonKit::kTwoPi * float(i) / float(std::max<std::uint32_t>(1, n));
            out.push_back(ClampIntoFrame(f, DungeonKit::Advance(center, a, radius), inset));
        }
        return out;
    }

    // JB1 normal: up to `count` random floor points in `f` (`inset` off the walls), each at least `minFromPlayers`
    // from every player ("never on a player") and, when it can be had, `minApart` from the others. `rand01()` returns
    // [0, 1). Rules relax in order: first the spacing, never the players. Fewer points when the floor has none left.
    template <class Rand01>
    std::vector<Vec2> PickSigilPoints(FrameRect const& f, std::vector<Vec2> const& players, std::uint32_t count,
        float minFromPlayers, float minApart, float inset, Rand01&& rand01, std::uint32_t tries = kSigilTries)
    {
        std::vector<Vec2> out;
        auto clear = [&](Vec2 p, float apart)
        {
            for (Vec2 const& pl : players)
                if (DungeonKit::Distance(p, pl) < minFromPlayers)
                    return false;
            for (Vec2 const& o : out)
                if (DungeonKit::Distance(p, o) < apart)
                    return false;
            return true;
        };
        float const spanX = std::max(0.f, f.sizeX - 2.f * inset);
        float const spanY = std::max(0.f, f.sizeY - 2.f * inset);
        for (float const apart : { minApart, 0.f })
            for (std::uint32_t t = 0; t < tries && out.size() < count; ++t)
            {
                float const fx = inset + float(rand01()) * spanX;
                float const fy = inset + float(rand01()) * spanY;
                Vec2 const p = DungeonKit::FramePoint(f, fx, fy);
                if (clear(p, apart))
                    out.push_back(p);
            }
        return out;
    }

    // JB7: up to `pairs` disjoint pairs among `figures` units (indexes). With `forced`, that figure is in the first
    // pair (heroic: she shuffles once more after the first find). `rand(n)` returns [0, n).
    template <class Rand>
    std::vector<std::pair<std::size_t, std::size_t>> ShufflePairs(std::size_t figures, std::uint32_t pairs,
        std::optional<std::size_t> forced, Rand&& rand)
    {
        std::vector<std::size_t> pool;
        for (std::size_t i = 0; i < figures; ++i)
            pool.push_back(i);
        auto take = [&pool, &rand]()
        {
            std::size_t const k = static_cast<std::size_t>(rand(static_cast<std::uint32_t>(pool.size()))) % pool.size();
            std::size_t const v = pool[k];
            pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(k));
            return v;
        };
        std::vector<std::pair<std::size_t, std::size_t>> out;
        if (forced && *forced < figures && figures >= 2 && pairs > 0)
        {
            pool.erase(std::find(pool.begin(), pool.end(), *forced));
            std::size_t const other = take();
            out.emplace_back(*forced, other);
        }
        while (out.size() < pairs && pool.size() >= 2)
        {
            std::size_t const a = take();
            std::size_t const b = take();
            out.emplace_back(a, b);
        }
        return out;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Bloodlines: where each lane's reflection stands. A lane is its low edge `offset` across the axis and its width;
    // the free lane (normal: unmarked, no reflection; heroic: the empty-handed decoy) is flagged. The registry's last
    // lane is clamped inside the frame and overlaps the one before it, so each reflection stands at the middle of the
    // part of its lane the others leave (a real lane: past the lane before it; the free lane: the ground no real lane
    // covers, which is the safe lane).
    // ----------------------------------------------------------------------------------------------------------------
    struct LaneSlot
    {
        float offset = 0.f;
        float width = 0.f;
        bool free = false;
    };

    // The across-axis centre of each lane's own strip, in the order given.
    inline std::vector<float> LaneMarkCenters(std::vector<LaneSlot> const& lanes)
    {
        std::vector<float> out(lanes.size(), 0.f);
        for (std::size_t i = 0; i < lanes.size(); ++i)
        {
            float lo = lanes[i].offset;
            float hi = lanes[i].offset + lanes[i].width;
            for (std::size_t j = 0; j < lanes.size(); ++j)
            {
                if (j == i || (lanes[i].free && lanes[j].free))
                    continue;
                float const jlo = lanes[j].offset;
                float const jhi = lanes[j].offset + lanes[j].width;
                bool const before = jlo < lanes[i].offset || (jlo == lanes[i].offset && j < i);
                if (before)
                    lo = std::max(lo, jhi);
                else if (lanes[i].free)
                    hi = std::min(hi, jlo);
            }
            if (hi < lo)
                hi = lo;
            out[i] = (lo + hi) * 0.5f;
        }
        return out;
    }

    // A lane's end on one wall: `alongX` lanes run along the frame's X (offsets across Y, ends at x = 0 or sizeX);
    // otherwise they run along Y (ends at y = 0 or sizeY). `center` is the across-axis centre; `farEnd` picks the wall.
    inline Vec2 LaneEndPoint(FrameRect const& f, bool alongX, float center, bool farEnd, float inset)
    {
        if (alongX)
            return DungeonKit::FramePoint(f, farEnd ? f.sizeX - inset : inset, center);
        return DungeonKit::FramePoint(f, center, farEnd ? f.sizeY - inset : inset);
    }

    // The facing of a reflection at a lane's end: down its lane, toward the other wall.
    inline float LaneFacing(FrameRect const& f, bool alongX, bool farEnd)
    {
        float const along = alongX ? f.angle : f.angle + DungeonKit::kPi * 0.5f;
        float o = farEnd ? along + DungeonKit::kPi : along;
        while (o >= DungeonKit::kTwoPi)
            o -= DungeonKit::kTwoPi;
        while (o < 0.f)
            o += DungeonKit::kTwoPi;
        return o;
    }
}

#endif
