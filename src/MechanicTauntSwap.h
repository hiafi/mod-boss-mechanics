/*
 * mod-boss-mechanics: the taunt swap (Blackrock Spire build, WP-TS; designer rulings 62-64, shared doc BS7). The pure
 * rules behind a boss descriptor's TauntSwapEntry (MechanicDescriptor.h): which tank taunts which boss, who holds a
 * boss, who keeps off one, and the taunt table. MechanicBotTauntSwap.cpp is the playerbots glue.
 *
 * The model (one swap boss = one live encounter unit an entry matches, alive and in combat, `when` holding):
 * - the **victim** of a boss holds it (Decision::holds);
 * - when the victim is a tank and has `swapAtStacks` or more stacks of the entry's debuff (from that boss), another
 *   tank whose own stacks of that boss's debuff have fallen off (0) taunts it (Why::Stacks; user ruling 2026-11-15:
 *   "they should taunt when the debuff has fallen off of them"). A tank that holds no swap boss goes first, then the
 *   lower guid; only a tank whose taunt is ready is picked, so two never taunt one boss at once and a tank whose
 *   taunt is on cooldown leaves it to another. No tank at 0: the victim keeps the boss (its stacks grow). The victim
 *   is the caster of the last taunt on the boss when there is one (EffectiveVictim): the boss turns at its next AI
 *   update, after every player's, so a taunt cast earlier in the same map update already counts;
 * - Righteous Defense (at the victim, up to 3 of its attackers) is not cast when the victim holds two swap bosses:
 *   it would hand both to the taker (FriendTauntAllowed);
 * - a tank holding two swap bosses gives one away to a tank holding none (Why::Trade): one whose debuff is gone from
 *   the free tank, the one where the holder has the most stacks (Halycon and Gizrul, Rend and Gyth on heroic: "the
 *   tanks trade"; a second tank carrying Halycon stacks takes the arriving Gizrul);
 * - every other tank keeps off a boss another tank holds (Decision::holdOff): no attack, no taunt, no AoE taunt, no
 *   cast at it until it is picked to taunt. That is the hold-off for the debuffed tank (it would otherwise pull the
 *   boss back with threat once the 3 s taunt ends, or taunt it back on the stock "lose aggro" trigger) and it keeps
 *   the waiting tank from ripping the boss early;
 * - a boss on a non-tank (a healer, a dead tank's replacement) or on nobody is not judged: the stock taunts take it
 *   back as before.
 * Stacks are the tank's own, from that boss: with the designed 15 s debuff and a hit every 6 s the waiting tank is at
 * 0 by the time the other reaches the swap count; with faster hits the victim waits past the count until it is.
 *
 * No AzerothCore includes: tests/taunt_swap_test.cpp compiles this with the host compiler.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_TAUNT_SWAP_H
#define MOD_BOSS_MECHANICS_MECHANIC_TAUNT_SWAP_H

#include "MechanicDescriptor.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace BossMechanics::TauntSwap
{
    using Desc::Key;
    using Desc::Ms;

    // ----------------------------------------------------------------------------------------------------------------
    // The taunt table (by spell id: playerbots' IsTauntAction knows only the stock action classes, not the reworked
    // classes' taunts)
    // ----------------------------------------------------------------------------------------------------------------

    enum class TauntKind : std::uint8_t
    {
        Single,         // at the boss
        RockbiterShock, // Earth Shock: taunts only under Rockbiter (the Enhancement tank build); at the boss
        Friend,         // at the boss's victim: Righteous Defense taunts the attackers of a friend
        Area,           // around the caster: never cast by the swap; blocked while the bot keeps off a boss
        Grip            // Death Grip: moves the boss; never cast by the swap (it shares Dark Command's category)
    };

    struct TauntSpell
    {
        std::uint32_t id = 0;
        char const* name = "";      // one taunt: its ranks share the name
        TauntKind kind = TauntKind::Single;
    };

    // Every tank's taunt, verified against the reworked class data (core apps/dbc-tools/source/classes, 2026-11-15)
    // and the fork's rows. Order: preference (the swap casts the first castable one), ranks high to low within a
    // taunt (the highest rank the bot knows).
    //   Warrior  Taunt 355; Challenging Shout 1161 (area)
    //   Paladin  Hand of Reckoning 62124; Righteous Defense 31789 (at the victim)
    //   Druid    Growl 6795 (Bear Form); Challenging Roar 5209 (area)
    //   Warrior  Mocking Blow 694 (last: a weapon strike in Battle Stance)
    //   DK       Dark Command 56222; Death Grip 49576 (grip)
    //   Rogue    Insult 203722 (rework: Enforcer Stance, 20 yd, category 82 with Taunt and Growl)
    //   Warlock  Demonic Taunt 200839 (rework: Dark Apotheosis); Challenging Howl 59671 (area)
    //   Shaman   Earth Shock 8042..49231 (rework: Rockbiter turns it into a taunt, 202394, 3 s)
    // The Earth Shock rank ids are the stock 3.3.5 ones (not re-checked against the live DBC).
    inline constexpr std::array<TauntSpell, 22> kTable = { {
        { 355, "Taunt", TauntKind::Single },
        { 62124, "Hand of Reckoning", TauntKind::Single },
        { 6795, "Growl", TauntKind::Single },
        { 56222, "Dark Command", TauntKind::Single },
        { 203722, "Insult", TauntKind::Single },
        { 200839, "Demonic Taunt", TauntKind::Single },
        { 49231, "Earth Shock", TauntKind::RockbiterShock },
        { 49230, "Earth Shock", TauntKind::RockbiterShock },
        { 25454, "Earth Shock", TauntKind::RockbiterShock },
        { 10414, "Earth Shock", TauntKind::RockbiterShock },
        { 10413, "Earth Shock", TauntKind::RockbiterShock },
        { 10412, "Earth Shock", TauntKind::RockbiterShock },
        { 8046, "Earth Shock", TauntKind::RockbiterShock },
        { 8045, "Earth Shock", TauntKind::RockbiterShock },
        { 8044, "Earth Shock", TauntKind::RockbiterShock },
        { 8042, "Earth Shock", TauntKind::RockbiterShock },
        { 31789, "Righteous Defense", TauntKind::Friend },
        { 1161, "Challenging Shout", TauntKind::Area },
        { 5209, "Challenging Roar", TauntKind::Area },
        { 59671, "Challenging Howl", TauntKind::Area },
        { 49576, "Death Grip", TauntKind::Grip },
        { 694, "Mocking Blow", TauntKind::Single },
    } };

    inline TauntSpell const* Find(std::uint32_t spellId)
    {
        for (TauntSpell const& t : kTable)
            if (t.id == spellId)
                return &t;
        return nullptr;
    }

    inline bool IsTaunt(std::uint32_t spellId)
    {
        return Find(spellId) != nullptr;
    }

    // Does the swap cast this one? Single and Friend always; Earth Shock only for a tank (the shaman tank build keeps
    // Rockbiter up); Area and Grip never.
    inline bool Castable(TauntSpell const& t, bool tank)
    {
        switch (t.kind)
        {
            case TauntKind::Single:
            case TauntKind::Friend:
                return true;
            case TauntKind::RockbiterShock:
                return tank;
            default:
                return false;
        }
    }

    // The bot's taunt for the swap: the first castable table row it knows and can cast now (`ready(TauntSpell
    // const&)`: known, off cooldown, castable at its target). One rank per taunt: once a rank of a name is known, the
    // lower ranks of that name are skipped (the highest known rank is the one tried). nullptr: none.
    template <class Ready, class Knows>
    TauntSpell const* PickTaunt(bool tank, Knows&& knows, Ready&& ready)
    {
        char const* tried = nullptr;
        for (TauntSpell const& t : kTable)
        {
            if (!Castable(t, tank))
                continue;
            if (tried && std::string_view(tried) == std::string_view(t.name))
                continue;
            if (!knows(t))
                continue;
            tried = t.name;
            if (ready(t))
                return &t;
        }
        return nullptr;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Matching an entry to the encounter's units
    // ----------------------------------------------------------------------------------------------------------------

    // Does the entry name this unit? A guid names that unit only; an entry every unit of it; neither the encounter
    // owner.
    inline bool Matches(TauntSwapEntry const& e, Key unit, std::uint32_t entry, Key owner)
    {
        if (e.guid)
            return e.guid == unit;
        if (e.bossEntry)
            return e.bossEntry == entry;
        return unit == owner;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // The decision
    // ----------------------------------------------------------------------------------------------------------------

    struct BossFacts
    {
        Key unit = 0;
        Key victim = 0;                 // its victim now (0: none)
        std::uint8_t swapAt = 0;        // the entry's swapAtStacks (0: never swaps)
    };

    struct TankFacts
    {
        Key guid = 0;
        bool alive = true;
        bool ready = false;             // a taunt the swap would cast at the boss itself is known and off cooldown
        bool friendReady = false;       // ...Righteous Defense (at the victim) is (FriendTauntAllowed limits it)
        std::vector<std::uint8_t> stacks;   // per boss (index into the bosses): the entry's debuff on this tank
    };

    enum class Why : std::uint8_t
    {
        None,
        Stacks,     // the victim reached the swap count; the taunter's own stacks have fallen off
        Trade       // the victim holds two swap bosses; the taunter holds none
    };

    struct Pick
    {
        Key taunter = 0;
        Why why = Why::None;
    };

    inline std::uint8_t StacksOf(TankFacts const& t, std::size_t boss)
    {
        return boss < t.stacks.size() ? t.stacks[boss] : 0;
    }

    inline TankFacts const* FindTank(std::vector<TankFacts> const& tanks, Key guid)
    {
        if (!guid)
            return nullptr;
        for (TankFacts const& t : tanks)
            if (t.guid == guid)
                return &t;
        return nullptr;
    }

    inline std::size_t HeldCount(std::vector<BossFacts> const& bosses, Key tank)
    {
        return static_cast<std::size_t>(std::count_if(bosses.begin(), bosses.end(), [tank](BossFacts const& b)
        {
            return tank && b.victim == tank;
        }));
    }

    // The boss's victim for the swap: the caster of the last taunt aura on it (the threat manager turns to it at once,
    // ThreatManager::TauntUpdate) over the victim it attacks now, which changes only at its next AI update. Players
    // (bots too) update before creatures in a map update, so without it a third tank stepping after the taunter in
    // the same update would still see the boss on the old victim, the taunter's taunt on cooldown, and taunt too.
    inline Key EffectiveVictim(Key victim, Key lastTaunter)
    {
        return lastTaunter ? lastTaunter : victim;
    }

    // Righteous Defense taunts up to 3 attackers of the victim: never while the victim holds two swap bosses (a trade
    // or a stack swap off a tank holding both), or both would move to the taker.
    inline bool FriendTauntAllowed(std::vector<BossFacts> const& bosses, Key victim)
    {
        return HeldCount(bosses, victim) < 2;
    }

    // Which of the holder's bosses a free tank takes (Why::Trade): among those whose debuff has fallen off the free
    // tank (0 stacks of that boss), the one where the holder has the most stacks; ties to the lower index. nullopt:
    // none qualifies, no trade.
    inline std::optional<std::size_t> GiveAway(std::vector<BossFacts> const& bosses, TankFacts const& holder,
        TankFacts const& taker)
    {
        std::optional<std::size_t> best;
        std::uint8_t bestStacks = 0;
        for (std::size_t i = 0; i < bosses.size(); ++i)
        {
            if (bosses[i].victim != holder.guid || !bosses[i].swapAt || StacksOf(taker, i) != 0)
                continue;
            std::uint8_t const held = StacksOf(holder, i);
            if (!best || held > bestStacks)
            {
                best = i;
                bestStacks = held;
            }
        }
        return best;
    }

    // Who taunts boss `b` now (taunter 0: nobody). Only while its victim is one of the tanks.
    inline Pick PickTaunter(std::vector<BossFacts> const& bosses, std::vector<TankFacts> const& tanks, std::size_t b)
    {
        if (b >= bosses.size())
            return {};
        BossFacts const& x = bosses[b];
        TankFacts const* v = FindTank(tanks, x.victim);
        if (!v || !x.swapAt)
            return {};
        std::uint8_t const vst = StacksOf(*v, b);
        bool const doubled = HeldCount(bosses, v->guid) >= 2;

        Pick best;
        std::size_t bestHeld = 0;
        for (TankFacts const& t : tanks)
        {
            if (t.guid == v->guid || !t.guid || !t.alive || !(t.ready || (t.friendReady && !doubled)))
                continue;
            std::size_t const held = HeldCount(bosses, t.guid);
            Why why = Why::None;
            if (vst >= x.swapAt && StacksOf(t, b) == 0)
                why = Why::Stacks;
            else if (doubled && held == 0 && GiveAway(bosses, *v, t) == b)
                why = Why::Trade;
            if (why == Why::None)
                continue;
            bool better = !best.taunter;
            if (!better && (held == 0) != (bestHeld == 0))
                better = held == 0;
            else if (!better)
                better = t.guid < best.taunter;
            if (better)
            {
                best = { t.guid, why };
                bestHeld = held;
            }
        }
        return best;
    }

    // One tank's decision at one tick.
    struct Decision
    {
        std::vector<Key> holds;         // swap bosses whose victim it is
        std::vector<Key> holdOff;       // swap bosses another tank holds: keep off them
        Key taunt = 0;                  // the boss it taunts now (0: none)
        Key tauntVictim = 0;            // ...that boss's victim now (Righteous Defense's target)
        bool friendTaunt = false;       // ...Righteous Defense may be cast for it (FriendTauntAllowed)
        Why why = Why::None;
        std::uint8_t victimStacks = 0;  // ...the victim's stacks there (the log line)

        bool Active() const { return !holds.empty() || !holdOff.empty() || taunt; }
        bool HeldOff(Key unit) const { return unit && Desc::Contains(holdOff, unit); }
        bool Holds(Key unit) const { return unit && Desc::Contains(holds, unit); }
    };

    // `self`: the deciding tank (not in `tanks`, or not alive: no decision). One taunt at a time: a tank picked for
    // two bosses taunts the first now and neither holds nor keeps off the second (it is next).
    inline Decision Decide(std::vector<BossFacts> const& bosses, std::vector<TankFacts> const& tanks, Key self)
    {
        Decision d;
        TankFacts const* me = FindTank(tanks, self);
        if (!me || !me->alive)
            return d;
        for (std::size_t i = 0; i < bosses.size(); ++i)
        {
            BossFacts const& b = bosses[i];
            if (!b.unit)
                continue;
            if (b.victim == self)
            {
                d.holds.push_back(b.unit);
                continue;
            }
            TankFacts const* v = FindTank(tanks, b.victim);
            if (!v)
                continue;       // on a non-tank or nobody: the stock taunts
            Pick const p = PickTaunter(bosses, tanks, i);
            if (p.taunter == self)
            {
                if (!d.taunt)
                {
                    d.taunt = b.unit;
                    d.tauntVictim = b.victim;
                    d.friendTaunt = FriendTauntAllowed(bosses, b.victim);
                    d.why = p.why;
                    d.victimStacks = StacksOf(*v, i);
                }
                continue;
            }
            d.holdOff.push_back(b.unit);
        }
        return d;
    }

    // The hold-off multiplier's verdict on one action of a tank: blocked when it aims at a boss the tank keeps off
    // (a kick excepted: `isKick`), or is an area taunt while it keeps off any boss. The swap's own action is never
    // judged (the caller lets it through).
    inline bool HoldOffBlocks(Decision const& d, Key actionTarget, bool isAreaTaunt, bool isKick)
    {
        if (d.holdOff.empty())
            return false;
        if (isAreaTaunt)
            return true;
        return !isKick && d.HeldOff(actionTarget);
    }

    // The focus a tank keeps while it holds swap bosses (the targets engine must not walk it to a listed add, say the
    // assist tank's entry, while it holds a wolf): its current target when that is one it holds, else the first.
    // 0: it holds none.
    inline Key HeldFocus(Decision const& d, Key current)
    {
        if (d.holds.empty())
            return 0;
        return d.Holds(current) ? current : d.holds.front();
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Per-bot state (MechanicBotState::tauntSwap)
    // ----------------------------------------------------------------------------------------------------------------

    struct State
    {
        Decision decision;
        Ms decidedAt{ -1 };             // game time of `decision` (once per game-time ms)
        bool entries = false;           // the active descriptor had taunt swap entries at that time
        // counters (`.mech bots`)
        std::uint32_t swaps = 0;        // swap taunts cast
        std::uint32_t trades = 0;       // ...of which trades
        std::uint32_t swapFails = 0;    // the cast was refused
        std::uint32_t approaches = 0;   // out of reach: walked to the boss first
        std::uint32_t holdOffDrops = 0; // attacks on a boss it keeps off stopped
        std::uint32_t blocked = 0;      // actions blocked by the hold-off multiplier
        std::uint8_t maxStacks = 0;     // the most stacks seen on this bot while it held a swap boss
    };
}

#endif
