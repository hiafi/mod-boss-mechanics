/*
 * mod-boss-mechanics: the bots' kicks against a boss's interrupt list (STAGE-M5-HANDOFF §3.3, decision §7.1 #2, WP-I).
 *
 * The pure half of MechanicBotInterrupt.cpp: the kick table, the choice of a bot's kick, who kicks which listed cast
 * (with the overload hold), the verdict on stock kick actions, the classification of a kick that went out (for the
 * counters) and the per-bot kick state. No AzerothCore includes: tests/kick_rules_test.cpp compiles it with the host
 * compiler. It builds on the descriptor rules (MechanicDescriptor.h: MatchInterrupt, OrderInterrupts, DueCasts,
 * HoldKick, PickKicker, ClaimBook).
 *
 * Spell Lock (§2.12 #1, settled by reading the cast path): playerbots' stock `spell lock` row is a plain
 * CastSpellAction. PlayerbotAI::CastSpell exempts 19244 / 19647 from its pet-autocast toggle and then casts them with
 * the warlock as the caster (`new Spell(bot, ...)`); nothing in Spell::prepare or CheckCast asks whether a player
 * knows the spell, so the interrupt does land, but from the warlock: range and line of sight from the warlock, the
 * cooldown on the warlock (the pet's stays free), and no Felhunter needed in range. The kick table casts Spell Lock
 * the way the core does for a player's pet cast (Caster::Pet: a Spell with the pet as caster, CheckPetCast, the pet's
 * cooldown, prepare), as playerbots' CastPetSpellAction does.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_KICK_RULES_H
#define MOD_BOSS_MECHANICS_MECHANIC_KICK_RULES_H

#include "MechanicDescriptor.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace BossMechanics::Kick
{
    using Ms = Desc::Ms;
    using Key = Desc::Key;

    // ----------------------------------------------------------------------------------------------------------------
    // The kick table
    // ----------------------------------------------------------------------------------------------------------------

    enum class Caster : std::uint8_t
    {
        Bot,    // PlayerbotAI::CastSpell
        Pet     // the bot's pet casts it (Spell Lock)
    };

    struct KickSpell
    {
        std::uint32_t id = 0;
        char const* name = "";      // one kick: its ranks share the name
        Caster caster = Caster::Bot;
    };

    // Every class's interrupt, verified against the reworked class data (core apps/dbc-tools/source/classes,
    // 2026-11-04) and the fork's rows. Order: preference (a bot that knows two kicks uses the first ready one), ranks
    // high to low within a kick (a bot uses the highest rank it knows). Ids the rework gave to something else are left
    // out: Shield Bash 1671 / 1672 are Warlock talent ids and Kick 1767-1769 Priest talent ids; 6554, 29704 and 38768
    // are the stock higher ranks, kept for a bot that still knows one.
    //   Warrior  Pummel 6552 (any stance, 10 s), Shield Bash 72 (shield, 12 s)
    //   Paladin  Rebuke 201473 (every spec, 16 s)
    //   Druid    Skull Bash 200605 (Cat, Bear, Bestial Fury: SS_FERAL, 16 s, off the GCD), Solar Beam 200335 (Moonkin,
    //            60 s, GCD)
    //   Rogue    Kick 1766 (10 s)
    //   Shaman   Wind Shear 57994 (25 yd, 10 s)
    //   Mage     Counterspell 2139 (30 yd, 24 s)
    //   DK       Mind Freeze 47528 (10 s)
    //   Hunter   Silencing Shot 34490 (35 yd, 20 s; a trainer baseline at 14 in the rework)
    //   Priest   Silence 15487 (30 yd, 45 s; its ShapeshiftMask asks for Shadowform)
    //   Warlock  Spell Lock 19647 / 19244 (the Felhunter, 30 yd, 24 s)
    inline constexpr std::array<KickSpell, 16> kTable = { {
        { 6554, "Pummel", Caster::Bot },
        { 6552, "Pummel", Caster::Bot },
        { 29704, "Shield Bash", Caster::Bot },
        { 72, "Shield Bash", Caster::Bot },
        { 201473, "Rebuke", Caster::Bot },
        { 200605, "Skull Bash", Caster::Bot },
        { 200335, "Solar Beam", Caster::Bot },
        { 38768, "Kick", Caster::Bot },
        { 1766, "Kick", Caster::Bot },
        { 57994, "Wind Shear", Caster::Bot },
        { 2139, "Counterspell", Caster::Bot },
        { 47528, "Mind Freeze", Caster::Bot },
        { 34490, "Silencing Shot", Caster::Bot },
        { 15487, "Silence", Caster::Bot },
        { 19647, "Spell Lock", Caster::Pet },
        { 19244, "Spell Lock", Caster::Pet },
    } };

    // The table row of a spell id (any rank), or nullptr: the multiplier's and the cast hook's "is this a kick?".
    inline KickSpell const* Find(std::uint32_t spellId)
    {
        for (KickSpell const& k : kTable)
            if (k.id == spellId)
                return &k;
        return nullptr;
    }

    // ---- other cast stoppers (M5 WP-F, R M-2) ----------------------------------------------------------------------
    // A spell outside the table that still stops its target's cast, judged by the stock-kick multiplier and the cast
    // hook like a kick (Hammer of Justice and Bash on the stock `... interrupt` triggers, rotation stuns, Polymorph,
    // Psychic Scream): an interrupt effect, or an aura that silences, stuns, confuses (incapacitate, polymorph, sap)
    // or fears. Unit::SetControlled stops every cast on a stun, confuse or fear; a silence stops a silenceable cast,
    // which every cast a kick stops is. The glue reads the effects from SpellInfo and checks the target's immunity
    // per effect; these are the SharedDefines.h / SpellAuraDefines.h values.
    constexpr std::uint32_t kEffectApplyAura = 6;
    constexpr std::uint32_t kEffectInterruptCast = 68;
    constexpr std::uint32_t kAuraModConfuse = 5;
    constexpr std::uint32_t kAuraModFear = 7;
    constexpr std::uint32_t kAuraModStun = 12;
    constexpr std::uint32_t kAuraModSilence = 27;
    constexpr std::uint32_t kAuraModPacifySilence = 60;

    inline bool StopsCasts(std::uint32_t effect, std::uint32_t auraType)
    {
        if (effect == kEffectInterruptCast)
            return true;
        if (effect != kEffectApplyAura)
            return false;
        switch (auraType)
        {
            case kAuraModConfuse:
            case kAuraModFear:
            case kAuraModStun:
            case kAuraModSilence:
            case kAuraModPacifySilence:
                return true;
            default:
                return false;
        }
    }

    inline bool SameKick(KickSpell const& a, KickSpell const& b)
    {
        return a.caster == b.caster && std::string_view(a.name) == std::string_view(b.name);
    }

    // The kicks a bot has: the table indices of the highest rank it knows of each kick, in table order.
    // `knows(KickSpell const&)`: the bot (or its pet, for Caster::Pet) knows that id.
    template <class Knows>
    std::vector<std::size_t> KnownKicks(Knows&& knows)
    {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < kTable.size(); ++i)
        {
            bool const haveThisKick = std::any_of(out.begin(), out.end(), [i](std::size_t j)
            {
                return SameKick(kTable[i], kTable[j]);
            });
            if (!haveThisKick && knows(kTable[i]))
                out.push_back(i);
        }
        return out;
    }

    // One of the bot's kicks as the runtime sees it now.
    struct KickOption
    {
        std::size_t index = 0;      // into kTable
        bool formOk = true;         // the bot's form or stance allows it (SpellInfo::CheckShapeshift)
        Ms readyAt{ 0 };            // its cooldown (and GCD, cast in progress) ends here
        Ms cooldown{ 0 };           // its recovery once used (the hold's horizon)
    };

    // The kick the bot would use: the first (table order) whose form fits and that is ready now; else the one ready
    // soonest (for the hold and the other bots' cover). nullopt: no usable kick at all.
    inline std::optional<std::size_t> ChooseKick(std::vector<KickOption> const& options, Ms now)
    {
        std::optional<std::size_t> soonest;
        for (std::size_t i = 0; i < options.size(); ++i)
        {
            KickOption const& o = options[i];
            if (!o.formOk)
                continue;
            if (o.readyAt <= now)
                return i;
            if (!soonest || o.readyAt < options[*soonest].readyAt)
                soonest = i;
        }
        return soonest;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // The cast being kicked
    // ----------------------------------------------------------------------------------------------------------------

    // One current spell of a unit, as Spell::EffectInterruptCast tests it.
    struct CastShape
    {
        bool channel = false;           // CURRENT_CHANNELED_SPELL (else CURRENT_GENERIC_SPELL)
        bool preparing = false;         // SPELL_STATE_PREPARING
        bool casting = false;           // SPELL_STATE_CASTING (a channel under way)
        std::int32_t castTime = 0;      // Spell::GetCastTime
        bool silencePrevention = false; // PreventionType == SPELL_PREVENTION_TYPE_SILENCE
        bool interruptFlag = false;     // InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT (generic)
        bool channelInterruptFlag = false;  // ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT (channel)
    };

    // A kick stops it (Spell::EffectInterruptCast, the same test).
    inline bool Interruptible(CastShape const& c)
    {
        if (!(c.casting || (c.preparing && c.castTime > 0)) || !c.silencePrevention)
            return false;
        return c.channel ? c.channelInterruptFlag : c.interruptFlag;
    }

    // The cast's start, the same for every bot: a cast note's start within `tolerance` of the estimate (end - cast
    // time, which drifts by a map update between bots and ticks) is the cast's start; else the estimate is, and it is
    // new (the caller notes it with Registry::NoteCastStarted).
    struct Start
    {
        Ms at{ 0 };
        bool fresh = false;
    };

    inline Start CanonicalStart(std::optional<Ms> noted, Ms estimate, Ms tolerance)
    {
        if (noted)
        {
            Ms const d = *noted > estimate ? *noted - estimate : estimate - *noted;
            if (d <= tolerance)
                return { *noted, false };
        }
        return { estimate, true };
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Who kicks which cast (§3.3)
    // ----------------------------------------------------------------------------------------------------------------

    // One bot that could kick (a bot of the group with the strategy and a kick).
    struct KickerFacts
    {
        Key bot = 0;
        Ms readyAt{ 0 };    // its chosen kick is ready here
        Ms cooldown{ 0 };   // ...and back this long after use
    };

    // One listed cast worth a kick, in OrderInterrupts order.
    struct CandidateFacts
    {
        std::uint8_t priority = 0;
        Ms start{ 0 };
        std::vector<bool> inRange;  // per kicker (same index): its kick reaches the caster
    };

    struct Assignment
    {
        std::optional<std::size_t> cast;    // the candidate this bot kicks now
        std::vector<std::size_t> held;      // candidates this bot could kick now but holds for (overload hold)
    };

    // Each bot runs this on the same facts and gets the same answer. Casts go in order; each takes the lowest-key
    // kicker that is ready now, in range and does not hold it (Desc::HoldKick over `due`, the other kickers' readyAt
    // as cover); a kicker takes one cast. Two kickers on one fast caster alternate: the one whose kick is on cooldown
    // is not ready. Deferral: once a cast has run `deferAfter` without a claim, any eligible bot may take it (the
    // claim book keeps it to one kick), so a picked bot that cannot act (it was casting, it lost control) costs at
    // most that much. `due` holds the casts the notes predict plus every unclaimed listed cast in progress (at now).
    // `reach` (M5 WP-F, R m-9): which kickers reach each due cast's caster; another kicker covers a due cast only when
    // it reaches it (Desc::HoldKickCovered). Empty: any kicker covers any due cast.
    inline Assignment Assign(std::vector<CandidateFacts> const& casts, std::vector<KickerFacts> const& kickers,
        std::size_t self, std::vector<Desc::DueCast> const& due, Ms now, Ms deferAfter,
        std::vector<Desc::DueReach> const& reach = {})
    {
        Assignment out;
        if (self >= kickers.size())
            return out;
        std::vector<bool> used(kickers.size(), false);
        std::vector<Desc::CoverKick> others;
        auto holds = [&](std::size_t k, std::uint8_t priority)
        {
            others.clear();
            for (std::size_t j = 0; j < kickers.size(); ++j)
                if (j != k)
                    others.push_back({ kickers[j].bot, kickers[j].readyAt });
            return Desc::HoldKickCovered(priority, now, kickers[k].cooldown, due, others, reach);
        };
        for (std::size_t c = 0; c < casts.size(); ++c)
        {
            CandidateFacts const& cast = casts[c];
            std::vector<Desc::Kicker> eligible;
            bool selfEligible = false;
            for (std::size_t k = 0; k < kickers.size(); ++k)
            {
                bool const reach = k < cast.inRange.size() && cast.inRange[k];
                if (used[k] || !reach || kickers[k].readyAt > now)
                    continue;
                if (holds(k, cast.priority))
                {
                    if (k == self)
                        out.held.push_back(c);
                    continue;
                }
                eligible.push_back({ kickers[k].bot, kickers[k].readyAt, true });
                if (k == self)
                    selfEligible = true;
            }
            std::optional<Key> const picked = Desc::PickKicker(eligible, now);
            if (!picked)
                continue;
            if (*picked == kickers[self].bot || (selfEligible && now - cast.start >= deferAfter))
            {
                out.cast = c;
                return out;
            }
            for (std::size_t k = 0; k < kickers.size(); ++k)
                if (kickers[k].bot == *picked)
                    used[k] = true;
        }
        return out;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Stock kicks and the counters (decision §7.1 #2)
    // ----------------------------------------------------------------------------------------------------------------

    // What a kick on a unit would stop: every interruptible current spell of it (a kick stops them all).
    struct TargetCast
    {
        bool casting = false;           // some current spell is interruptible
        bool doNotKick = false;         // one of them is on the do-not-kick list now
        bool listed = false;            // one is listed (priority > 0)
        bool claimedByOther = false;    // a listed one is claimed by another bot
        bool listedDue = false;         // unlisted only: a listed cast is due before this bot's kick is back and no
                                        // other kicker covers it (Desc::HoldKickCovered(0, ...))
        bool uninterruptibleOnly = false;   // nothing interruptible, but the unit is an encounter unit that is casting
                                            // or channeling (Corvane's leash): a kick on it stops nothing
    };

    enum class StockVerdict : std::uint8_t
    {
        Allow,
        DoNotKick,  // never kicked
        Claimed,    // another bot kicks it
        Listed,     // `boss kick` owns listed casts (the order, the hold and the claim live there)
        Hold,       // unlisted, and a listed cast is due before the kick is back
        Nothing     // a real kick on an encounter unit whose current cast is not interruptible: it stops nothing and
                    // only puts the kick on cooldown before the next listed cast (STAGE-R-B-RAVENIAN-REPORT RV-1)
    };

    // The stock-kick multiplier's verdict (only while a descriptor with an interrupt list applies to the bot). `kick`:
    // the action is a real kick (Kick::Find); a stun or silence used as an interrupt still stops a channel (CastStop)
    // or has its own use, so it is never dropped for Nothing.
    inline StockVerdict JudgeStockKick(TargetCast const& t, bool kick = true)
    {
        if (!t.casting)
            return t.uninterruptibleOnly && kick ? StockVerdict::Nothing : StockVerdict::Allow;
        if (t.doNotKick)
            return StockVerdict::DoNotKick;
        if (t.claimedByOther)
            return StockVerdict::Claimed;
        if (t.listed)
            return StockVerdict::Listed;
        if (t.listedDue)
            return StockVerdict::Hold;
        return StockVerdict::Allow;
    }

    enum class Outcome : std::uint8_t
    {
        None,       // nothing to count (no interruptible cast, no descriptor)
        Landed,     // on a listed cast this bot holds the claim of
        Duplicate,  // on a listed cast another bot claimed
        DoNotKick,  // on a do-not-kick cast (a violation)
        Wasted,     // on an unlisted cast while a listed cast was due (what the hold forbids)
        Unlisted    // on an unlisted cast, nothing due: allowed
    };

    // A kick that went out (the player cast hook, before its effects), judged on what it stops. `claimed`: the cast
    // hook's claim attempt on the listed cast (Registry::ClaimCast) succeeded.
    inline Outcome Classify(TargetCast const& t, bool claimed)
    {
        if (!t.casting)
            return Outcome::None;
        if (t.doNotKick)
            return Outcome::DoNotKick;
        if (t.listed)
            return claimed && !t.claimedByOther ? Outcome::Landed : Outcome::Duplicate;
        return t.listedDue ? Outcome::Wasted : Outcome::Unlisted;
    }

    // A listed cast seen in progress that is gone now: it completed (a miss) when it went at or past its end and nobody
    // claimed a kick on it. One that went earlier was stopped by something (the caster died, a stun, a kick).
    inline bool Missed(Ms end, Ms now, bool claimed, Ms slack)
    {
        return !claimed && now + slack >= end;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Per-bot state (MechanicBotState::kick)
    // ----------------------------------------------------------------------------------------------------------------

    struct SeenCast
    {
        Desc::CastKey key;
        Ms end{ 0 };
    };

    // A count taken once per cast (a hold, a dropped stock kick) is remembered here until the cast is old.
    struct Mark
    {
        Desc::CastKey key;
        std::uint8_t kind = 0;
    };

    // true the first time (key, kind) is marked; marks of casts that started over `keep` ago are dropped.
    inline bool MarkOnce(std::vector<Mark>& marks, Desc::CastKey const& key, std::uint8_t kind, Ms now, Ms keep)
    {
        marks.erase(std::remove_if(marks.begin(), marks.end(), [&](Mark const& m)
        {
            return m.key.start + keep < now;
        }), marks.end());
        for (Mark const& m : marks)
            if (m.kind == kind && m.key == key)
                return false;
        marks.push_back({ key, kind });
        return true;
    }

    struct KickState
    {
        Ms planAt{ -1 };                    // the tick the plan below is for (-1: never)
        bool active = false;                // a descriptor with an interrupt list applies to the bot
        // The cast this bot kicks now (target.caster 0: none) and with what.
        Desc::CastKey target;
        Ms targetEnd{ 0 };
        std::uint8_t priority = 0;
        std::uint32_t kickSpell = 0;
        bool petKick = false;
        // The hold's inputs, for the multiplier and the cast hook (unlisted casts).
        std::vector<Desc::DueCast> due;
        Ms cooldown{ 0 };                   // this bot's kick (0: it has none)
        Ms readyAt{ 0 };
        std::vector<Desc::CoverKick> others;    // the other kickers of the group
        std::vector<Desc::DueReach> dueReach;   // which kickers reach each due cast's caster (R m-9)
        // Miss detection: the group's observer (lowest key among its bots with the strategy) counts.
        bool observer = false;
        std::vector<SeenCast> seen;
        std::vector<Mark> marks;
    };
}

#endif
