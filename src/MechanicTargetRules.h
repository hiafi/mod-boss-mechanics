/*
 * mod-boss-mechanics: the pure rules of the bots' target priority and ignore list (STAGE-M5-HANDOFF §3.5, WP-T), on
 * top of Desc::PickTarget and Desc::IsIgnored (MechanicDescriptor.h). MechanicBotTargets.cpp is the playerbots glue.
 *
 * - DecideFocus: the bot's pick and whether it is engaged. An engaged focus attacks the pick (`boss focus`, ACTION_RAID
 *   + 2) and stands the stock pickers down (dps assist, dps aoe unless the pick is aoeOk, tank assist), as MgT's focus
 *   order does. It engages while the pick outranks the boss (a listed unit above priority 0) or while a live listed
 *   unit ranks below the pick for this bot (an add below the boss: the bot keeps the boss). A bot no entry names (a
 *   tank or a healer under Dps entries) has the owner as its pick and nothing below it: never engaged, stock.
 * - MayBeIgnored: the cheap name test the target filter runs before it builds a unit's facts.
 * - DecideMinion: what to do with a pet, guardian or totem whose victim is ignored.
 * - BreakOutClock: how long each prison held its player, as one bot saw it (the break-out time counter).
 *
 * No AzerothCore includes: tests/target_rules_test.cpp compiles this with the host compiler.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_TARGET_RULES_H
#define MOD_BOSS_MECHANICS_MECHANIC_TARGET_RULES_H

#include "MechanicDescriptor.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace BossMechanics::TargetRules
{
    using Desc::Key;
    using Desc::Ms;

    // The bot's focus at one tick.
    struct Focus
    {
        Key target = 0;                 // the pick (0: none: no listed targets, or no live candidate)
        std::int8_t priority = 0;
        bool aoeOk = false;             // the pick's entry allows AoE (the dps aoe picker stays)
        bool outranksBoss = false;      // a listed unit above priority 0 that is not the owner
        bool lowerListed = false;       // a live, not ignored candidate listed for this bot ranks below the pick

        bool Engaged() const { return target && (outranksBoss || lowerListed); }
    };

    inline Focus DecideFocus(Descriptor const& d, std::vector<Desc::TargetCandidate> const& candidates, Key owner,
        Desc::BotFacts const& bot, Key current, Desc::Context const& ctx)
    {
        Focus f;
        if (d.targets.empty())
            return f;
        std::optional<Desc::TargetPick> const pick = Desc::PickTarget(d, candidates, owner, bot, current, ctx);
        if (!pick)
            return f;
        f.target = candidates[pick->candidate].facts->guid;
        f.priority = pick->priority;
        f.aoeOk = pick->aoeOk;
        f.outranksBoss = pick->outranksBoss;
        for (Desc::TargetCandidate const& c : candidates)
        {
            Desc::UnitFacts const* u = c.facts;
            if (!u || !u->alive || u->guid == f.target || Desc::IsIgnored(d, *u, ctx))
                continue;
            std::int8_t rank = 0;
            if (std::optional<std::size_t> const e = Desc::MatchTarget(d, *u, bot, ctx))
                rank = d.targets[*e].priority;
            else if (u->guid != owner)
                continue;       // unlisted for this bot: the stock pickers' business
            if (rank < f.priority)
            {
                f.lowerListed = true;
                break;
            }
        }
        return f;
    }

    // The stock target pickers the focus stands down.
    enum class Picker : std::uint8_t
    {
        None,
        Assist,         // DpsAssistAction
        Aoe,            // DpsAoeAction
        TankAssist      // TankAssistAction
    };

    inline bool SuppressPicker(Picker p, Focus const& f)
    {
        if (p == Picker::None || !f.Engaged())
            return false;
        return p != Picker::Aoe || !f.aoeOk;
    }

    // The bot attacks something else while its engaged pick lives (the lower-priority time counter).
    inline bool OnLowerTarget(Focus const& f, Key victim)
    {
        return f.Engaged() && victim && victim != f.target;
    }

    // Can any ignore entry name this unit at all? false: it is not ignored whatever the conditions say, and the
    // caller skips its facts.
    inline bool MayBeIgnored(Descriptor const& d, Key guid, std::uint32_t entry)
    {
        for (IgnoreEntry const& e : d.ignores)
            if (e.guid ? e.guid == guid : e.entry && e.entry == entry)
                return true;
        return false;
    }

    enum class MinionStep : std::uint8_t
    {
        Keep,           // its victim is not ignored
        Redirect,       // onto the owner's target (alive, hostile, not ignored)
        Recall          // back to the owner (follow)
    };

    inline MinionStep DecideMinion(bool victimIgnored, bool ownerTargetUsable)
    {
        if (!victimIgnored)
            return MinionStep::Keep;
        return ownerTargetUsable ? MinionStep::Redirect : MinionStep::Recall;
    }

    // A sample further than this from the previous one is not counted (as the waker's mode clocks).
    constexpr Ms kMaxSampleGap{ 2000 };

    inline Ms SampleGap(Ms last, Ms now)
    {
        if (last <= Ms(0) || now <= last || now - last > kMaxSampleGap)
            return Ms(0);
        return now - last;
    }

    // How long each prison held its player, as one bot saw it: a prison is timed from the first sample that lists it
    // to the first that does not. MaxMs covers the open ones too, so a prison never broken shows.
    class BreakOutClock
    {
    public:
        // `prisons`: the prisons that hold a player now.
        void Sample(std::vector<Key> const& prisons, Ms now)
        {
            for (std::size_t i = 0; i < _open.size();)
            {
                if (std::find(prisons.begin(), prisons.end(), _open[i].first) != prisons.end())
                {
                    ++i;
                    continue;
                }
                _closedMax = std::max(_closedMax, Span(_open[i].second, now));
                _open.erase(_open.begin() + static_cast<std::ptrdiff_t>(i));
            }
            for (Key p : prisons)
                if (p && std::none_of(_open.begin(), _open.end(), [p](auto const& o) { return o.first == p; }))
                    _open.emplace_back(p, now);
            _lastAt = now;
        }

        std::uint32_t MaxMs() const
        {
            std::uint32_t m = _closedMax;
            for (auto const& o : _open)
                m = std::max(m, Span(o.second, _lastAt));
            return m;
        }

        std::size_t Open() const { return _open.size(); }

    private:
        static std::uint32_t Span(Ms from, Ms to)
        {
            return to > from ? static_cast<std::uint32_t>((to - from).count()) : 0;
        }

        std::vector<std::pair<Key, Ms>> _open;
        std::uint32_t _closedMax = 0;
        Ms _lastAt{ 0 };
    };

    // A focus older than this is not sampled (the bot's engine has not run the value since).
    constexpr Ms kFocusStale{ 1000 };

    // The per-bot part of WP-T that is not a counter (MechanicBotState::targets).
    struct State
    {
        Focus focus;                    // the last `boss kill target` result...
        Ms focusAt{ 0 };                // ...and when it was taken
        Ms sampledAt{ 0 };              // the waker's last sample (lower-priority clock)
        BreakOutClock breakOut;
        bool attackersLogged = false;   // the `attackers` class check was logged for this bot
    };
}

#endif
