/*
 * mod-boss-mechanics: the boss descriptor (STAGE-M5-HANDOFF §3.2-§3.5, WP-D): what a boss tells bots besides its
 * mechanics, namely which casts to kick and in what order, which debuffs to dispel first, which enemy buffs to purge,
 * which units to kill first and which to leave alone.
 *
 * No AzerothCore includes: tests/descriptor_test.cpp compiles this header with the host compiler. Units are keyed by
 * their raw guid (Desc::Key = ObjectGuid::GetRawValue()); times are absolute GameTime milliseconds, like the
 * registry's. Unit appears only as the forward-declared argument of a Custom condition, which scripts write and the
 * pure rules never call: the bot view (MechanicDescriptorView.h) runs every Custom condition once per unit and tick
 * and hands the results in through UnitFacts::custom.
 *
 * The descriptor lives on the registry encounter (Encounter::descriptor, cleared at its end), set through
 * Mechanic::Describe / EditDescriptor / SetPhase (BossMechanics.h) or the program entry `describe <preset>`
 * (MechanicDescriptorPresets.h). Bots read a copy through their DescriptorView and decide with the rules below:
 * - interrupts (WP-I): MatchInterrupt, OrderInterrupts, NextDue, DueCasts, HoldKick(Covered), PickKicker,
 *   ClaimBook;
 * - dispels and purges (WP-P): IsUrgent, OrderDispels, PickPurge;
 * - targets and the ignore list (WP-T): Fits, PickTarget, IsIgnored;
 * - turned teammates (SFK F1): IsHostilePlayer, read by TargetEntry::HostilePlayers and the playerbots fork's
 *   hostile-teammate hook (MechanicBotTargets.cpp).
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DESCRIPTOR_H
#define MOD_BOSS_MECHANICS_MECHANIC_DESCRIPTOR_H

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

class Unit;

namespace BossMechanics
{
    // ----------------------------------------------------------------------------------------------------------------
    // Data model (§3.2)
    // ----------------------------------------------------------------------------------------------------------------

    enum class CondKind : std::uint8_t
    {
        Always,
        HasAura,        // the unit has aura `id`
        LacksAura,      // the unit lacks aura `id`
        NearUnit,       // another live unit of entry `id` is within `value` yd of the unit
        InZone,         // the unit stands in registry mechanic `id` (a script passes its MechanicHandle's id)
        Phase,          // the descriptor's phase is `id`
        OwnerBelowPct,  // the encounter owner's health is below `value` percent
        Custom          // fn(unit), scripts only (programs use the kinds above); evaluated once per unit and tick
                        // while the encounter owner is in the world. The lambda may still run on a bot's stale copy
                        // for one map update after the boss despawned: capture guids only (never `this`, a
                        // Creature* or an InstanceScript*) and resolve them through the passed unit's map.
    };

    // A condition on one unit: the caster of an interrupt entry, the candidate of a target or ignore entry.
    struct Condition
    {
        CondKind kind = CondKind::Always;
        std::uint32_t id = 0;       // aura id, unit entry, registry mechanic id or phase
        float value = 0.f;          // yards or percent
        std::function<bool(Unit const* unit)> fn;   // Custom (scripts only; programs use the kinds)

        static Condition Always() { return {}; }
        static Condition HasAura(std::uint32_t auraId) { return { CondKind::HasAura, auraId, 0.f, nullptr }; }
        static Condition LacksAura(std::uint32_t auraId) { return { CondKind::LacksAura, auraId, 0.f, nullptr }; }
        static Condition NearUnit(std::uint32_t entry, float yd) { return { CondKind::NearUnit, entry, yd, nullptr }; }
        static Condition InZone(std::uint32_t mechanicId) { return { CondKind::InZone, mechanicId, 0.f, nullptr }; }
        static Condition Phase(std::uint32_t phase) { return { CondKind::Phase, phase, 0.f, nullptr }; }
        static Condition OwnerBelowPct(float pct) { return { CondKind::OwnerBelowPct, 0, pct, nullptr }; }
        static Condition Custom(std::function<bool(Unit const*)> f)
        {
            return { CondKind::Custom, 0, 0.f, std::move(f) };
        }
    };

    // Which bots an entry concerns. Melee and Ranged are damage dealers only; Dps is every damage dealer; tanks and
    // healers take an entry only when it names them (Tank, Healer) or Any.
    enum class Who : std::uint8_t
    {
        Any,
        Melee,
        Ranged,
        Tank,
        Healer,
        Dps
    };

    // The structs below take their main fields through a constructor (a brace list naming only some fields would warn
    // under -Wextra); the rest are set by name or with the chaining helpers:
    //     d.interrupts.emplace_back(NPC_SNEED, SPELL_REPAIRS, 2);
    //     d.targets.push_back(TargetEntry(NPC_RAVENIAN, 1).KillTogether(NPC_CORVANE, 10.f));
    struct InterruptEntry
    {
        InterruptEntry(std::uint32_t casterEntry_ = 0, std::uint32_t spellId_ = 0, std::uint8_t priority_ = 1,
            std::chrono::milliseconds every_ = std::chrono::milliseconds(0), Condition when_ = {})
            : casterEntry(casterEntry_), spellId(spellId_), priority(priority_), every(every_), when(std::move(when_))
        {
        }

        std::uint32_t casterEntry = 0;      // 0 = any unit of the encounter
        std::uint32_t spellId = 0;
        std::uint8_t priority = 1;          // higher kicks first; 0 = never kick (do-not-kick)
        std::chrono::milliseconds every{ 0 };   // cadence for the overload hold; NoteNextCast overrides it
        Condition when;
    };

    struct DispelEntry
    {
        DispelEntry(std::uint32_t auraId_ = 0, std::uint8_t priority_ = 1, bool urgent_ = false,
            std::chrono::milliseconds deadline_ = std::chrono::milliseconds(0))
            : auraId(auraId_), priority(priority_), urgent(urgent_), deadline(deadline_)
        {
        }

        std::uint32_t auraId = 0;
        std::uint8_t priority = 1;          // higher first
        bool urgent = false;                // loss of control: before everything, past the healers' 50% gate
        std::chrono::milliseconds deadline{ 0 };    // urgent once the aura has this little left (0 = never)
    };

    struct PurgeEntry
    {
        PurgeEntry(std::uint32_t unitEntry_ = 0, std::uint32_t auraId_ = 0, std::uint8_t priority_ = 1)
            : unitEntry(unitEntry_), auraId(auraId_), priority(priority_)
        {
        }

        std::uint32_t unitEntry = 0;        // 0 = any enemy unit
        std::uint32_t auraId = 0;
        std::uint8_t priority = 1;
    };

    struct TargetEntry
    {
        TargetEntry(std::uint32_t entry_ = 0, std::int8_t priority_ = 0, Who who_ = Who::Dps, Condition when_ = {})
            : entry(entry_), priority(priority_), who(who_), when(std::move(when_))
        {
        }

        // One unit, followed by its guid through swaps (raw ObjectGuid).
        static TargetEntry ForUnit(std::uint64_t guid, std::int8_t priority, Who who = Who::Dps, Condition when = {})
        {
            TargetEntry e(0, priority, who, std::move(when));
            e.guid = guid;
            return e;
        }

        // Any player the descriptor marks hostile now (Desc::IsHostilePlayer: charmed and carrying one of
        // Descriptor::hostilePlayerAuras), such as Arugal's turned players. `when` narrows it further. Players have no
        // entry, so this is the only way a target entry names one.
        static TargetEntry HostilePlayers(std::int8_t priority, Who who = Who::Dps, Condition when = {})
        {
            TargetEntry e(0, priority, who, std::move(when));
            e.hostilePlayers = true;
            return e;
        }

        TargetEntry& AoeOk()
        {
            aoeOk = true;
            return *this;
        }

        TargetEntry& KillTogether(std::uint32_t partner, float pct)
        {
            partnerEntry = partner;
            togetherPct = pct;
            return *this;
        }

        TargetEntry& BreakOut()
        {
            breakOut = true;
            return *this;
        }

        std::uint32_t entry = 0;            // entry, or one unit (follow the guid through swaps)
        std::uint64_t guid = 0;             // raw ObjectGuid; set: matches that unit only, whatever its entry
        std::int8_t priority = 0;           // the boss is 0; negative = below the boss
        Who who = Who::Dps;                 // tanks keep the boss unless an entry names them
        Condition when;
        bool aoeOk = false;                 // AoE allowed while it is up (Forresten's rats, Timmy's playmates)
        std::uint32_t partnerEntry = 0;     // kill together: hit the one with more % left
        float togetherPct = 0.f;            // ...keeping the two within this many percent
        bool breakOut = false;              // holds a player: order by the held player's role (tank, healer)
        bool hostilePlayers = false;        // HostilePlayers: matches a hostile player, never an entry or a guid
    };

    // Ignored while `when` holds: no damage from bots or pets, no AoE count.
    struct IgnoreEntry
    {
        IgnoreEntry(std::uint32_t entry_ = 0, Condition when_ = {}) : entry(entry_), when(std::move(when_)) { }

        static IgnoreEntry ForUnit(std::uint64_t guid, Condition when = {})
        {
            IgnoreEntry e(0, std::move(when));
            e.guid = guid;
            return e;
        }

        std::uint32_t entry = 0;
        std::uint64_t guid = 0;             // raw ObjectGuid; set: that unit only
        Condition when;
    };

    struct Descriptor
    {
        std::vector<InterruptEntry> interrupts;
        std::vector<DispelEntry> dispels;
        std::vector<PurgeEntry> purges;
        std::vector<TargetEntry> targets;
        std::vector<IgnoreEntry> ignores;
        // Auras that mark a charmed player as turned against the group (SFK F1: Arugal's Turned). A charmed player
        // carrying one is a valid attack target for bots, and bots stop healing, buffing and curing it, until the
        // aura or the charm ends. Empty: a charmed teammate stays off limits (the playerbots default).
        std::vector<std::uint32_t> hostilePlayerAuras;
        std::uint32_t phase = 0;

        bool Empty() const
        {
            return interrupts.empty() && dispels.empty() && purges.empty() && targets.empty() && ignores.empty()
                && hostilePlayerAuras.empty();
        }
    };
}

namespace BossMechanics::Desc
{
    using Ms = std::chrono::milliseconds;
    using Key = std::uint64_t;      // ObjectGuid::GetRawValue(); 0 = none

    // ----------------------------------------------------------------------------------------------------------------
    // Conditions
    // ----------------------------------------------------------------------------------------------------------------

    // What a condition may read about one unit. The view fills it once per unit and tick, and only with what the
    // descriptor references (References below): the auras, the entries of NearUnit conditions, the zones, and the
    // result of each Custom condition.
    struct UnitFacts
    {
        Key guid = 0;
        std::uint32_t entry = 0;
        bool alive = true;
        float healthPct = 100.f;
        bool player = false;                                    // a player (players have no entry)
        bool charmed = false;                                   // charmed by another unit (mind control)
        std::vector<std::uint32_t> auras;                       // the referenced auras the unit has
        std::vector<std::pair<std::uint32_t, float>> near;      // entry -> yd to the nearest other live unit of it
                                                                // (left out when there is none in range)
        std::vector<std::uint32_t> zones;                       // the referenced mechanics the unit stands in
        std::vector<std::pair<Condition const*, bool>> custom;  // Custom results (keyed by the view's own copy)
    };

    // What every condition shares: the descriptor's phase and the owner's health.
    struct Context
    {
        std::uint32_t phase = 0;
        float ownerHealthPct = 100.f;
    };

    template <class T>
    bool Contains(std::vector<T> const& v, T const& x)
    {
        return std::find(v.begin(), v.end(), x) != v.end();
    }

    // Does `c` hold for the unit? A Custom condition the view did not evaluate does not hold.
    inline bool Holds(Condition const& c, UnitFacts const& u, Context const& ctx)
    {
        switch (c.kind)
        {
            case CondKind::Always:
                return true;
            case CondKind::HasAura:
                return Contains(u.auras, c.id);
            case CondKind::LacksAura:
                return !Contains(u.auras, c.id);
            case CondKind::NearUnit:
                for (auto const& [entry, yd] : u.near)
                    if (entry == c.id && yd <= c.value)
                        return true;
                return false;
            case CondKind::InZone:
                return Contains(u.zones, c.id);
            case CondKind::Phase:
                return ctx.phase == c.id;
            case CondKind::OwnerBelowPct:
                return ctx.ownerHealthPct < c.value;
            case CondKind::Custom:
                for (auto const& [cond, result] : u.custom)
                    if (cond == &c)
                        return result;
                return false;
        }
        return false;
    }

    // Everything the descriptor's conditions read, so the view gathers only that: aura ids, NearUnit entries with the
    // largest range asked of each, zone ids, and the Custom conditions (pointers into `d`: valid while `d` is).
    struct References
    {
        std::vector<std::uint32_t> auras;
        std::vector<std::pair<std::uint32_t, float>> near;
        std::vector<std::uint32_t> zones;
        std::vector<Condition const*> custom;
    };

    inline void AddReference(References& r, Condition const& c)
    {
        switch (c.kind)
        {
            case CondKind::HasAura:
            case CondKind::LacksAura:
                if (!Contains(r.auras, c.id))
                    r.auras.push_back(c.id);
                break;
            case CondKind::NearUnit:
            {
                auto it = std::find_if(r.near.begin(), r.near.end(), [&c](auto const& p) { return p.first == c.id; });
                if (it == r.near.end())
                    r.near.emplace_back(c.id, c.value);
                else
                    it->second = std::max(it->second, c.value);
                break;
            }
            case CondKind::InZone:
                if (!Contains(r.zones, c.id))
                    r.zones.push_back(c.id);
                break;
            case CondKind::Custom:
                if (c.fn)
                    r.custom.push_back(&c);
                break;
            default:
                break;
        }
    }

    inline References Referenced(Descriptor const& d)
    {
        References r;
        for (std::uint32_t aura : d.hostilePlayerAuras)
            if (aura && !Contains(r.auras, aura))
                r.auras.push_back(aura);
        for (InterruptEntry const& e : d.interrupts)
            AddReference(r, e.when);
        for (TargetEntry const& e : d.targets)
            AddReference(r, e.when);
        for (IgnoreEntry const& e : d.ignores)
            AddReference(r, e.when);
        return r;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Interrupts (§3.3, decision §7.1 #2)
    // ----------------------------------------------------------------------------------------------------------------

    // The entry that lists this cast now: a match on the caster's entry before a casterEntry 0 one, the first in list
    // order among equals, and only an entry whose `when` holds for the caster. nullopt: the cast is unlisted now (an
    // entry whose condition fails, do-not-kick ones included, does not list it).
    inline std::optional<std::size_t> MatchInterrupt(Descriptor const& d, std::uint32_t casterEntry,
        std::uint32_t spellId, UnitFacts const& caster, Context const& ctx)
    {
        std::optional<std::size_t> generic;
        for (std::size_t i = 0; i < d.interrupts.size(); ++i)
        {
            InterruptEntry const& e = d.interrupts[i];
            if (e.spellId != spellId || (e.casterEntry && e.casterEntry != casterEntry))
                continue;
            if (!Holds(e.when, caster, ctx))
                continue;
            if (e.casterEntry)
                return i;
            if (!generic)
                generic = i;
        }
        return generic;
    }

    // A cast in progress on a unit of the encounter.
    struct CastFacts
    {
        Key caster = 0;
        std::uint32_t casterEntry = 0;
        std::uint32_t spellId = 0;
        Ms start{ 0 };
        Ms end{ 0 };                    // when the cast completes
        bool claimedByOther = false;    // ClaimBook: another bot kicks it
        UnitFacts const* facts = nullptr;   // the caster (nullptr: no aura, zone, near or custom facts)
    };

    struct KickOption
    {
        std::size_t cast = 0;           // index into the casts
        std::size_t entry = 0;          // index into Descriptor::interrupts
        std::uint8_t priority = 0;
    };

    // The listed casts in progress worth a kick, best first: priority, then the one that completes first, then the
    // caster key and the spell. Left out: unlisted, do-not-kick (priority 0, never picked), claimed by another bot,
    // already complete at `now`.
    inline std::vector<KickOption> OrderInterrupts(Descriptor const& d, std::vector<CastFacts> const& casts,
        Context const& ctx, Ms now)
    {
        static UnitFacts const kNoFacts;
        std::vector<KickOption> out;
        for (std::size_t i = 0; i < casts.size(); ++i)
        {
            CastFacts const& c = casts[i];
            if (c.claimedByOther || c.end <= now)
                continue;
            std::optional<std::size_t> const entry = MatchInterrupt(d, c.casterEntry, c.spellId,
                c.facts ? *c.facts : kNoFacts, ctx);
            if (!entry || d.interrupts[*entry].priority == 0)
                continue;
            out.push_back({ i, *entry, d.interrupts[*entry].priority });
        }
        std::sort(out.begin(), out.end(), [&casts](KickOption const& a, KickOption const& b)
        {
            if (a.priority != b.priority)
                return a.priority > b.priority;
            CastFacts const& ca = casts[a.cast];
            CastFacts const& cb = casts[b.cast];
            if (ca.end != cb.end)
                return ca.end < cb.end;
            if (ca.caster != cb.caster)
                return ca.caster < cb.caster;
            return ca.spellId < cb.spellId;
        });
        return out;
    }

    // Is this cast on the do-not-kick list right now (a listed entry of priority 0 whose `when` holds)?
    inline bool IsDoNotKick(Descriptor const& d, std::uint32_t casterEntry, std::uint32_t spellId,
        UnitFacts const& caster, Context const& ctx)
    {
        std::optional<std::size_t> const entry = MatchInterrupt(d, casterEntry, spellId, caster, ctx);
        return entry && d.interrupts[*entry].priority == 0;
    }

    // What the map knows about one caster's listed spell (Registry::NoteNextCast, NoteCastStarted).
    struct CastNote
    {
        Key caster = 0;
        std::uint32_t casterEntry = 0;
        std::uint32_t spellId = 0;
        std::optional<Ms> nextAt;       // the script's scheduler: the next cast starts here
        std::optional<Ms> lastStart;    // the last cast seen starting
    };

    // The notes of one map: a few per caster, pruned by age.
    class CastNoteBook
    {
    public:
        // A note older than this (its nextAt and lastStart both further back) is dropped by Prune.
        static constexpr Ms kKeep{ 120000 };

        void NoteNext(Key caster, std::uint32_t casterEntry, std::uint32_t spellId, Ms at)
        {
            Upsert(caster, casterEntry, spellId).nextAt = at;
        }

        // false when the same start was already noted (nothing changed).
        bool NoteStart(Key caster, std::uint32_t casterEntry, std::uint32_t spellId, Ms start)
        {
            CastNote& n = Upsert(caster, casterEntry, spellId);
            if (n.lastStart == start)
                return false;
            n.lastStart = start;
            return true;
        }

        CastNote const* Find(Key caster, std::uint32_t spellId) const
        {
            for (CastNote const& n : _notes)
                if (n.caster == caster && n.spellId == spellId)
                    return &n;
            return nullptr;
        }

        // Drops the notes with nothing in the last kKeep; true when one went.
        bool Prune(Ms now)
        {
            auto const stale = [now](CastNote const& n)
            {
                bool const nextOld = !n.nextAt || *n.nextAt + kKeep < now;
                bool const lastOld = !n.lastStart || *n.lastStart + kKeep < now;
                return nextOld && lastOld;
            };
            std::size_t const before = _notes.size();
            _notes.erase(std::remove_if(_notes.begin(), _notes.end(), stale), _notes.end());
            return _notes.size() != before;
        }

        // Every note of a unit (its encounter ended); true when one went.
        bool DropCaster(Key caster)
        {
            std::size_t const before = _notes.size();
            _notes.erase(std::remove_if(_notes.begin(), _notes.end(), [caster](CastNote const& n)
            {
                return n.caster == caster;
            }), _notes.end());
            return _notes.size() != before;
        }

        std::vector<CastNote> const& All() const { return _notes; }
        void Clear() { _notes.clear(); }

    private:
        CastNote& Upsert(Key caster, std::uint32_t casterEntry, std::uint32_t spellId)
        {
            for (CastNote& n : _notes)
                if (n.caster == caster && n.spellId == spellId)
                {
                    n.casterEntry = casterEntry;
                    return n;
                }
            _notes.push_back({ caster, casterEntry, spellId, std::nullopt, std::nullopt });
            return _notes.back();
        }

        std::vector<CastNote> _notes;
    };

    // When the listed cast is next due: the noted time while it is not in the past; else, with a cadence, the first
    // lastStart + k * every at or after `now`; nullopt when neither is known (the hold then cannot predict it, §6).
    inline std::optional<Ms> NextDue(InterruptEntry const& e, CastNote const* note, Ms now)
    {
        if (note && note->nextAt && *note->nextAt >= now)
            return note->nextAt;
        if (e.every > Ms(0) && note && note->lastStart)
        {
            Ms next = *note->lastStart + e.every;
            if (next < now)
                next += e.every * ((now - next + e.every - Ms(1)) / e.every);
            return next;
        }
        return std::nullopt;
    }

    // A listed cast expected to start at `at` (a cast already in progress and unclaimed: at = now).
    struct DueCast
    {
        std::uint8_t priority = 0;
        Ms at{ 0 };
        Key caster = 0;     // its caster (M5 WP-F, m-9: who can cover it; 0: unknown, any other kicker covers it)
    };

    // The listed casts the notes predict, with their priorities (do-not-kick left out). `factsOf(Key)` gives the
    // caster's UnitFacts const* for the entry's condition (nullptr: no unit facts).
    template <class FactsOf>
    std::vector<DueCast> DueCasts(Descriptor const& d, std::vector<CastNote> const& notes, Context const& ctx, Ms now,
        FactsOf&& factsOf)
    {
        static UnitFacts const kNoFacts;
        std::vector<DueCast> out;
        for (CastNote const& n : notes)
        {
            UnitFacts const* f = factsOf(n.caster);
            std::optional<std::size_t> const entry = MatchInterrupt(d, n.casterEntry, n.spellId, f ? *f : kNoFacts,
                ctx);
            if (!entry || d.interrupts[*entry].priority == 0)
                continue;
            if (std::optional<Ms> const at = NextDue(d.interrupts[*entry], &n, now))
                out.push_back({ d.interrupts[*entry].priority, *at, n.caster });
        }
        return out;
    }

    // The overload hold (§3.3, decision §7.1 #2). A bot whose kick is ready now and back at now + cooldown holds it
    // for a cast of `candidatePriority` (0 = an unlisted cast) when a listed cast of higher priority (any listed cast
    // for an unlisted one) is due before the kick is back, unless another kicker's kick is ready by that time.
    // Another kicker of the group, as cover for a due cast.
    struct CoverKick
    {
        Key bot = 0;
        Ms readyAt{ 0 };    // its kick is ready here
    };

    // M5 WP-F (R m-9): the kickers whose kick reaches one due cast's caster (range; the kick lands on it).
    struct DueReach
    {
        Key caster = 0;
        std::vector<Key> kickers;
    };

    // Does `kicker` reach `caster`? A caster the list does not name (0, or only noted, with no unit to measure)
    // keeps the plain rule: any kicker covers it.
    inline bool DueReaches(std::vector<DueReach> const& reach, Key caster, Key kicker)
    {
        if (!caster)
            return true;
        for (DueReach const& r : reach)
            if (r.caster == caster)
                return std::find(r.kickers.begin(), r.kickers.end(), kicker) != r.kickers.end();
        return true;
    }

    // HoldKick with the cover judged per due cast: another kicker covers a due cast only when its kick is ready by
    // then and reaches that cast's caster (DueReaches).
    inline bool HoldKickCovered(std::uint8_t candidatePriority, Ms now, Ms cooldown, std::vector<DueCast> const& due,
        std::vector<CoverKick> const& others, std::vector<DueReach> const& reach)
    {
        Ms const back = now + cooldown;
        for (DueCast const& c : due)
        {
            if (c.priority <= candidatePriority || c.at >= back)
                continue;
            bool const covered = std::any_of(others.begin(), others.end(), [&](CoverKick const& o)
            {
                return o.readyAt <= c.at && DueReaches(reach, c.caster, o.bot);
            });
            if (!covered)
                return true;
        }
        return false;
    }

    inline bool HoldKick(std::uint8_t candidatePriority, Ms now, Ms cooldown, std::vector<DueCast> const& due,
        std::vector<Ms> const& otherKicksReadyAt)
    {
        std::vector<CoverKick> others;
        others.reserve(otherKicksReadyAt.size());
        for (Ms ready : otherKicksReadyAt)
            others.push_back({ 0, ready });
        return HoldKickCovered(candidatePriority, now, cooldown, due, others, {});
    }

    // One bot with a kick for the cast.
    struct Kicker
    {
        Key bot = 0;
        Ms readyAt{ 0 };        // the kick's cooldown ends here
        bool inRange = true;    // in range and line of sight of the caster
    };

    // Who kicks (§3.3): the kicker with the lowest key among those ready now and in range. A kick on cooldown drops
    // its bot from the next pick, so two kickers on one fast caster alternate. nullopt: nobody can.
    inline std::optional<Key> PickKicker(std::vector<Kicker> const& kickers, Ms now)
    {
        std::optional<Key> best;
        for (Kicker const& k : kickers)
            if (k.inRange && k.readyAt <= now && (!best || k.bot < *best))
                best = k.bot;
        return best;
    }

    // One cast: the caster, the spell and its start.
    struct CastKey
    {
        Key caster = 0;
        std::uint32_t spellId = 0;
        Ms start{ 0 };

        bool operator==(CastKey const& o) const
        {
            return caster == o.caster && spellId == o.spellId && start == o.start;
        }
    };

    struct Claim
    {
        CastKey cast;
        Key bot = 0;
        Ms expires{ 0 };
    };

    // Per-cast kick claims (§3.3): the first bot to claim a cast kicks it; the claim ends with the cast (its expiry,
    // the cast end the claimer passes) or a Release. One map's book; the registry holds it.
    class ClaimBook
    {
    public:
        // true when `bot` holds the cast now (a new claim, or its own, whose expiry moves to `expires`); false while
        // another bot's claim on it is live.
        bool Take(CastKey const& cast, Key bot, Ms expires, Ms now)
        {
            for (Claim& c : _claims)
            {
                if (!(c.cast == cast))
                    continue;
                if (c.bot != bot && c.expires > now)
                    return false;
                c.bot = bot;
                c.expires = expires;
                return true;
            }
            _claims.push_back({ cast, bot, expires });
            return true;
        }

        // The bot holding the cast (0 = nobody, or the claim has expired).
        Key Holder(CastKey const& cast, Ms now) const
        {
            for (Claim const& c : _claims)
                if (c.cast == cast && c.expires > now)
                    return c.bot;
            return 0;
        }

        // Drops the bot's claim on the cast; true when it had one.
        bool Release(CastKey const& cast, Key bot)
        {
            std::size_t const before = _claims.size();
            _claims.erase(std::remove_if(_claims.begin(), _claims.end(), [&](Claim const& c)
            {
                return c.cast == cast && c.bot == bot;
            }), _claims.end());
            return _claims.size() != before;
        }

        // Drops expired claims; true when one went.
        bool Prune(Ms now)
        {
            std::size_t const before = _claims.size();
            _claims.erase(std::remove_if(_claims.begin(), _claims.end(), [now](Claim const& c)
            {
                return c.expires <= now;
            }), _claims.end());
            return _claims.size() != before;
        }

        // Every claim on a caster's casts (its encounter ended).
        void DropCaster(Key caster)
        {
            _claims.erase(std::remove_if(_claims.begin(), _claims.end(), [caster](Claim const& c)
            {
                return c.cast.caster == caster;
            }), _claims.end());
        }

        std::vector<Claim> const& All() const { return _claims; }
        void Clear() { _claims.clear(); }

    private:
        std::vector<Claim> _claims;
    };

    // ----------------------------------------------------------------------------------------------------------------
    // Dispels and purges (§3.4, decision §7.1 #3)
    // ----------------------------------------------------------------------------------------------------------------

    // Urgent: loss of control, or a deadline entry whose aura has `deadline` or less left (Haunting Memory: 8 s aura,
    // deadline 4 s). Urgent entries go before the others and past the healers' 50% gate.
    inline bool IsUrgent(DispelEntry const& e, Ms remaining)
    {
        return e.urgent || (e.deadline > Ms(0) && remaining <= e.deadline);
    }

    inline std::optional<std::size_t> FindDispel(Descriptor const& d, std::uint32_t auraId)
    {
        for (std::size_t i = 0; i < d.dispels.size(); ++i)
            if (d.dispels[i].auraId == auraId)
                return i;
        return std::nullopt;
    }

    // One harmful aura on a group member the bot can dispel.
    struct Affliction
    {
        Key member = 0;
        std::uint32_t auraId = 0;
        Ms remaining{ 0 };
        std::uint8_t rank = 0;      // the stock order of the member (master, healers, tanks, others: 0 first)
    };

    struct DispelPick
    {
        std::size_t affliction = 0;
        std::size_t entry = 0;
        bool urgent = false;
    };

    // The listed afflictions, best first: urgent before the rest, then the entry's priority, then the aura with less
    // time left, then the member's stock rank and key. Unlisted auras are left out (the stock order handles them,
    // after every listed one). `urgentOnly`: the urgent ones only (the `boss dispel urgent` action).
    inline std::vector<DispelPick> OrderDispels(Descriptor const& d, std::vector<Affliction> const& afflictions,
        bool urgentOnly = false)
    {
        std::vector<DispelPick> out;
        for (std::size_t i = 0; i < afflictions.size(); ++i)
        {
            std::optional<std::size_t> const entry = FindDispel(d, afflictions[i].auraId);
            if (!entry)
                continue;
            bool const urgent = IsUrgent(d.dispels[*entry], afflictions[i].remaining);
            if (urgentOnly && !urgent)
                continue;
            out.push_back({ i, *entry, urgent });
        }
        std::sort(out.begin(), out.end(), [&](DispelPick const& a, DispelPick const& b)
        {
            if (a.urgent != b.urgent)
                return a.urgent;
            std::uint8_t const pa = d.dispels[a.entry].priority;
            std::uint8_t const pb = d.dispels[b.entry].priority;
            if (pa != pb)
                return pa > pb;
            Affliction const& x = afflictions[a.affliction];
            Affliction const& y = afflictions[b.affliction];
            if (x.remaining != y.remaining)
                return x.remaining < y.remaining;
            if (x.rank != y.rank)
                return x.rank < y.rank;
            return x.member < y.member;
        });
        return out;
    }

    // One buff on an enemy unit.
    struct EnemyBuff
    {
        Key unit = 0;
        std::uint32_t unitEntry = 0;
        std::uint32_t auraId = 0;
    };

    struct PurgePick
    {
        std::size_t buff = 0;
        std::size_t entry = 0;
    };

    // The listed buff to purge first: the highest priority, then list order of the buffs. A purge entry with unitEntry
    // 0 covers every unit. nullopt: nothing listed.
    inline std::optional<PurgePick> PickPurge(Descriptor const& d, std::vector<EnemyBuff> const& buffs)
    {
        std::optional<PurgePick> best;
        for (std::size_t i = 0; i < buffs.size(); ++i)
            for (std::size_t k = 0; k < d.purges.size(); ++k)
            {
                PurgeEntry const& e = d.purges[k];
                if (e.auraId != buffs[i].auraId || (e.unitEntry && e.unitEntry != buffs[i].unitEntry))
                    continue;
                if (!best || e.priority > d.purges[best->entry].priority)
                    best = PurgePick{ i, k };
            }
        return best;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Targets and the ignore list (§3.5)
    // ----------------------------------------------------------------------------------------------------------------

    // The bot's role, as the target rules read it.
    struct BotFacts
    {
        bool tank = false;
        bool healer = false;
        bool melee = false;     // a melee damage dealer (ignored for tanks and healers)
    };

    inline bool Fits(Who who, BotFacts const& bot)
    {
        bool const dps = !bot.tank && !bot.healer;
        switch (who)
        {
            case Who::Any:
                return true;
            case Who::Melee:
                return dps && bot.melee;
            case Who::Ranged:
                return dps && !bot.melee;
            case Who::Tank:
                return bot.tank;
            case Who::Healer:
                return bot.healer;
            case Who::Dps:
                return dps;
        }
        return false;
    }

    // Is the unit on the ignore list now? An entry with neither guid nor entry matches nothing.
    inline bool IsIgnored(Descriptor const& d, UnitFacts const& u, Context const& ctx)
    {
        for (IgnoreEntry const& e : d.ignores)
        {
            bool const match = e.guid ? e.guid == u.guid : e.entry && e.entry == u.entry;
            if (match && Holds(e.when, u, ctx))
                return true;
        }
        return false;
    }

    // Is the unit a player turned against the group now: a charmed player carrying one of the descriptor's
    // hostilePlayerAuras (SFK F1)? Needs the auras in the facts, which References adds.
    inline bool IsHostilePlayer(Descriptor const& d, UnitFacts const& u)
    {
        if (!u.player || !u.charmed || !u.alive)
            return false;
        for (std::uint32_t aura : d.hostilePlayerAuras)
            if (aura && Contains(u.auras, aura))
                return true;
        return false;
    }

    // The held player's role, for break-out targets (tank's prison first, then the healer's, then the others').
    enum class HeldRank : std::uint8_t
    {
        Tank,
        Healer,
        Other,
        None        // holds nobody (not a prison, or its player is gone)
    };

    struct TargetCandidate
    {
        UnitFacts const* facts = nullptr;
        HeldRank held = HeldRank::None;
    };

    struct TargetPick
    {
        std::size_t candidate = 0;
        std::optional<std::size_t> entry;   // nullopt: the owner by the implicit boss rule (priority 0)
        std::int8_t priority = 0;
        bool aoeOk = false;
        bool outranksBoss = false;          // priority > 0 and not the owner: the stock assist and AoE stand down
    };

    // The entry that ranks this unit for this bot: a guid entry before entry ones, then the highest priority among
    // those whose `who` fits the bot and whose `when` holds. nullopt: not listed for this bot now.
    inline std::optional<std::size_t> MatchTarget(Descriptor const& d, UnitFacts const& u, BotFacts const& bot,
        Context const& ctx)
    {
        std::optional<std::size_t> best;
        bool bestByGuid = false;
        for (std::size_t i = 0; i < d.targets.size(); ++i)
        {
            TargetEntry const& e = d.targets[i];
            bool const byGuid = e.guid != 0;
            if (e.hostilePlayers ? !IsHostilePlayer(d, u)
                : byGuid ? e.guid != u.guid : (u.player || !e.entry || e.entry != u.entry))
                continue;
            if (!Fits(e.who, bot) || !Holds(e.when, u, ctx))
                continue;
            if (!best || (byGuid && !bestByGuid)
                || (byGuid == bestByGuid && e.priority > d.targets[*best].priority))
            {
                best = i;
                bestByGuid = byGuid;
            }
        }
        return best;
    }

    // The unit the bot should attack (§3.5): the highest priority among the listed candidates (the owner is a
    // candidate at priority 0 for every bot unless an entry ranks it), alive and not ignored. Ties, in order: a
    // break-out entry by the held player's rank (tank, healer, others) before entries that free nobody; the current
    // target; kill-together entries the one with more % left, the others the one with less; the lower key. A
    // kill-together candidate (partnerEntry, togetherPct) is dropped while it is more than togetherPct below its
    // healthiest live partner, so the bot switches to the partner and the two die together. nullopt: no candidate.
    inline std::optional<TargetPick> PickTarget(Descriptor const& d, std::vector<TargetCandidate> const& candidates,
        Key owner, BotFacts const& bot, Key current, Context const& ctx)
    {
        struct Ranked
        {
            std::size_t candidate;
            std::optional<std::size_t> entry;
            std::int8_t priority;
            std::uint8_t heldRank;
            bool together;
        };
        auto partnerPct = [&candidates](std::uint32_t partnerEntry) -> std::optional<float>
        {
            std::optional<float> best;
            for (TargetCandidate const& c : candidates)
                if (c.facts && c.facts->alive && c.facts->entry == partnerEntry)
                    best = std::max(best.value_or(0.f), c.facts->healthPct);
            return best;
        };
        std::vector<Ranked> ranked;
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            UnitFacts const* u = candidates[i].facts;
            if (!u || !u->alive || IsIgnored(d, *u, ctx))
                continue;
            std::optional<std::size_t> const entry = MatchTarget(d, *u, bot, ctx);
            if (!entry && u->guid != owner)
                continue;
            Ranked r{ i, entry, 0, static_cast<std::uint8_t>(HeldRank::None), false };
            if (entry)
            {
                TargetEntry const& e = d.targets[*entry];
                r.priority = e.priority;
                if (e.breakOut)
                    r.heldRank = static_cast<std::uint8_t>(candidates[i].held == HeldRank::None ? HeldRank::Other
                        : candidates[i].held);
                if (e.partnerEntry)
                {
                    r.together = true;
                    if (std::optional<float> const p = partnerPct(e.partnerEntry))
                        if (u->healthPct < *p - e.togetherPct)
                            continue;
                }
            }
            ranked.push_back(r);
        }
        if (ranked.empty())
            return std::nullopt;
        auto const better = [&](Ranked const& a, Ranked const& b)
        {
            if (a.priority != b.priority)
                return a.priority > b.priority;
            if (a.heldRank != b.heldRank)
                return a.heldRank < b.heldRank;
            UnitFacts const& ua = *candidates[a.candidate].facts;
            UnitFacts const& ub = *candidates[b.candidate].facts;
            bool const ca = current && ua.guid == current;
            bool const cb = current && ub.guid == current;
            if (ca != cb)
                return ca;
            if (ua.healthPct != ub.healthPct)
                return (a.together && b.together) ? ua.healthPct > ub.healthPct : ua.healthPct < ub.healthPct;
            return ua.guid < ub.guid;
        };
        Ranked const& top = *std::min_element(ranked.begin(), ranked.end(), better);
        TargetPick pick;
        pick.candidate = top.candidate;
        pick.entry = top.entry;
        pick.priority = top.priority;
        pick.aoeOk = top.entry && d.targets[*top.entry].aoeOk;
        pick.outranksBoss = top.priority > 0 && candidates[top.candidate].facts->guid != owner;
        return pick;
    }
}

#endif
