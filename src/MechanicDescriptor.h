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
 * - dispels and purges (WP-P): IsUrgent, DispelHeld, OrderDispels, PickPurge;
 * - targets and the ignore list (WP-T): Fits, PickTarget, IsIgnored;
 * - turned teammates (SFK F1): IsHostilePlayer, read by TargetEntry::HostilePlayers and the playerbots fork's
 *   hostile-teammate hook (MechanicBotTargets.cpp);
 * - taunt swaps (Blackrock Spire WP-TS): TauntSwapEntry here, the rules in MechanicTauntSwap.h;
 * - passes (Blackrock Spire WP-PS): PassEntry here, the rules in MechanicPass.h;
 * - holds (Blackrock Depths WP-DS, shared doc BD3): HoldEntry, HoldOn, HeldDamageMask, DamageHeld, KicksHeld (an
 *   Attacks hold is read by IsIgnored); the dispel rank key DispelEntry::MostStacksFirst (OrderDispels);
 * - Zul'Farrak BM-1: IgnoreEntry::who (an ignore entry for one role, IgnoreConcerns), HealUntilEntry and HealReadPct
 *   (the Grievous Wound healer rule, read by the playerbots fork's heal read hook);
 * - Gnomeregan BM-G: MoveCostEntry and MoveCostly (Static: a carrier makes no optional move), RangeBandEntry and
 *   RangeBandFor (a preferred band round the encounter owner for ranged bots), read by the bot brain.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_DESCRIPTOR_H
#define MOD_BOSS_MECHANICS_MECHANIC_DESCRIPTOR_H

#include <algorithm>
#include <chrono>
#include <cmath>
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
        NearPlayer,     // another live player is within `value` yd of the unit (Stratholme X1: Maleki's spacing rule)
        All,            // every condition in `all` holds (none: holds)
        Not,            // the first condition in `all` does not hold (none: holds)
        Custom          // fn(unit), scripts only (programs use the kinds above); evaluated once per unit and tick
                        // while the encounter owner is in the world. The lambda may still run on a bot's stale copy
                        // for one map update after the boss despawned: capture guids only (never `this`, a
                        // Creature* or an InstanceScript*) and resolve them through the passed unit's map.
    };

    // A condition on one unit: the caster of an interrupt entry, the candidate of a target or ignore entry, the
    // afflicted member of a dispel entry. All and Not combine others (Maleki's dispel: no player within 8 yd and not
    // during the Eye, `AllOf({ Not(NearPlayer(8)), Not(Phase(EYE)) })`).
    struct Condition
    {
        CondKind kind = CondKind::Always;
        std::uint32_t id = 0;       // aura id, unit entry, registry mechanic id or phase
        float value = 0.f;          // yards or percent
        std::function<bool(Unit const* unit)> fn;   // Custom (scripts only; programs use the kinds)
        std::vector<Condition> all; // All: the conditions; Not: the one negated

        static Condition Make(CondKind kind, std::uint32_t id, float value)
        {
            Condition c;
            c.kind = kind;
            c.id = id;
            c.value = value;
            return c;
        }
        static Condition Always() { return {}; }
        static Condition HasAura(std::uint32_t auraId) { return Make(CondKind::HasAura, auraId, 0.f); }
        static Condition LacksAura(std::uint32_t auraId) { return Make(CondKind::LacksAura, auraId, 0.f); }
        static Condition NearUnit(std::uint32_t entry, float yd) { return Make(CondKind::NearUnit, entry, yd); }
        static Condition InZone(std::uint32_t mechanicId) { return Make(CondKind::InZone, mechanicId, 0.f); }
        static Condition Phase(std::uint32_t phase) { return Make(CondKind::Phase, phase, 0.f); }
        static Condition OwnerBelowPct(float pct) { return Make(CondKind::OwnerBelowPct, 0, pct); }
        // Another live player (not the unit itself; pets and creatures do not count) within `yd` of the unit.
        static Condition NearPlayer(float yd) { return Make(CondKind::NearPlayer, 0, yd); }
        static Condition AllOf(std::vector<Condition> conditions)
        {
            Condition c = Make(CondKind::All, 0, 0.f);
            c.all = std::move(conditions);
            return c;
        }
        static Condition Not(Condition condition)
        {
            Condition c = Make(CondKind::Not, 0, 0.f);
            c.all.push_back(std::move(condition));
            return c;
        }
        static Condition Custom(std::function<bool(Unit const*)> f)
        {
            Condition c = Make(CondKind::Custom, 0, 0.f);
            c.fn = std::move(f);
            return c;
        }
        // Blackrock Spire WP-PS: any of the conditions holds (none: never), written with the kinds above as
        // Not(AllOf(Not a, Not b, ...)). Solakar's catcher: `AnyOf({ LacksAura(SCORCHED), HasAura(SPEAR_WOUND) })`.
        static Condition AnyOf(std::vector<Condition> conditions)
        {
            std::vector<Condition> negated;
            negated.reserve(conditions.size());
            for (Condition& c : conditions)
                negated.push_back(Not(std::move(c)));
            return Not(AllOf(std::move(negated)));
        }
    };

    // Which bots an entry concerns. Melee and Ranged are damage dealers only; Dps is every damage dealer; tanks and
    // healers take an entry only when it names them (Tank, Healer, MainTank, AssistTank) or Any. MainTank is the
    // group's main tank (the main tank flag, else its first live tank: playerbots' IsMainTank); AssistTank every
    // other tank (Blackrock Spire WP-TS: "the second tank picks up Gizrul / the champions / the Guards").
    enum class Who : std::uint8_t
    {
        Any,
        Melee,
        Ranged,
        Tank,
        Healer,
        Dps,
        MainTank,
        AssistTank
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
            std::chrono::milliseconds deadline_ = std::chrono::milliseconds(0), Condition when_ = {})
            : auraId(auraId_), priority(priority_), urgent(urgent_), deadline(deadline_), when(std::move(when_))
        {
        }

        // Blackrock Depths WP-DS (Gerstahn's Doubt): among afflictions of equal urgency and priority, the member with
        // the most stacks of aura `aura` (any caster) goes first, before the time-left order. Shadow Word: Pain on the
        // player with the most Doubt first: `DispelEntry(SPELL_SWP, 2).MostStacksFirst(SPELL_DOUBT)`.
        DispelEntry& MostStacksFirst(std::uint32_t aura)
        {
            rankAura = aura;
            return *this;
        }

        std::uint32_t auraId = 0;
        std::uint8_t priority = 1;          // higher first
        bool urgent = false;                // loss of control: before everything, past the healers' 50% gate
        std::chrono::milliseconds deadline{ 0 };    // urgent once the aura has this little left (0 = never)
        // Judged on the afflicted member (Stratholme X1). While it does not hold the aura is held: no bot cures it,
        // neither the listed walk nor the stock cure rows (they pass over a member whose only aura of the type is
        // held). Urgency does not override it.
        Condition when;
        std::uint32_t rankAura = 0;         // MostStacksFirst: the stacked aura ranked on (0: none)
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

        // For one bot only (its raw ObjectGuid): a per-bot pick the script makes, such as Jandice's Shell Game guess.
        TargetEntry& OnlyFor(std::uint64_t botGuid)
        {
            bot = botGuid;
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
        std::uint64_t bot = 0;              // OnlyFor: set, the entry concerns that bot alone (raw ObjectGuid)
    };

    // A taunt swap (Blackrock Spire WP-TS, designer rulings 62-64, shared doc BS7): the boss's hits stack
    // `debuffSpell` on its victim; once the victim is a tank with `swapAtStacks` or more, another tank whose own stacks
    // have fallen off taunts it, and the debuffed tank keeps off it until its turn comes again (MechanicTauntSwap.h;
    // user ruling 2026-11-15). A boss scripts the entry only where the swap applies: in a flex dungeon only when the
    // pull's held player count is exactly 10 (ruling 63; the kit's TankSwapOn(n)). `when` is judged on the boss.
    //     d.tauntSwaps.emplace_back(NPC_SMOLDERWEB, SPELL_WIDOWS_BITE, 3);
    //     d.tauntSwaps.push_back(TauntSwapEntry::ForUnit(gizrul.GetRawValue(), SPELL_TORN_HIDE, 3));
    struct TauntSwapEntry
    {
        TauntSwapEntry(std::uint32_t bossEntry_ = 0, std::uint32_t debuffSpell_ = 0, std::uint8_t swapAtStacks_ = 3,
            Condition when_ = {})
            : bossEntry(bossEntry_), debuffSpell(debuffSpell_), swapAtStacks(swapAtStacks_), when(std::move(when_))
        {
        }

        // One unit (raw ObjectGuid), whatever its entry.
        static TauntSwapEntry ForUnit(std::uint64_t guid, std::uint32_t debuffSpell, std::uint8_t swapAtStacks,
            Condition when = {})
        {
            TauntSwapEntry e(0, debuffSpell, swapAtStacks, std::move(when));
            e.guid = guid;
            return e;
        }

        // Count the debuff's stacks from every caster (default: only the stacks this boss put on the tank, so two
        // wolves with one debuff id count apart).
        TauntSwapEntry& AnyCaster()
        {
            anyCaster = true;
            return *this;
        }

        std::uint32_t bossEntry = 0;        // every encounter unit of this entry; 0 (and no guid): the owner
        std::uint64_t guid = 0;             // raw ObjectGuid; set: that unit only
        std::uint32_t debuffSpell = 0;
        std::uint8_t swapAtStacks = 3;      // 0: never swaps (the entry still keeps the other tank off the boss)
        Condition when;                     // judged on the boss; while it fails the boss is not a swap boss
        bool anyCaster = false;
    };

    // Ignored while `when` holds: no damage from bots or pets, no AoE count. Zul'Farrak F33 (gap 14): `who` names
    // the bots that ignore it (default Any: every bot); Ukorz's DPS bots leave the disfavored boss out of their
    // attacker counts (so most of their area spells stop) while the tank still holds it.
    //     d.ignores.push_back(IgnoreEntry(NPC_RUUZLU, Condition::Custom(...)).For(Who::Dps));
    struct IgnoreEntry
    {
        IgnoreEntry(std::uint32_t entry_ = 0, Condition when_ = {}) : entry(entry_), when(std::move(when_)) { }

        static IgnoreEntry ForUnit(std::uint64_t guid, Condition when = {})
        {
            IgnoreEntry e(0, std::move(when));
            e.guid = guid;
            return e;
        }

        IgnoreEntry& For(Who who_)
        {
            who = who_;
            return *this;
        }

        std::uint32_t entry = 0;
        std::uint64_t guid = 0;             // raw ObjectGuid; set: that unit only
        Condition when;
        Who who = Who::Any;                 // the bots it concerns (Desc::Fits); Any: every bot
    };

    // Zul'Farrak F17 (the Grievous Wound healer rule, §7 #7): heal a carrier of `auraId` on to `pct`. While a unit
    // carries the aura (any caster) and its health is below `pct`, bots read it as at most `readAsPct` for their heal
    // decisions (Desc::HealReadPct, through the playerbots fork's heal read hook), so their healers keep healing it
    // past the shared 85% trigger line. 80 stays above the emergency lines (Lay on Hands, Sacrifice).
    //     d.healUntil.emplace_back(SPELL_GRIEVOUS_WOUND, 95.f);
    constexpr float kHealUntilReadAsPct = 80.f;

    struct HealUntilEntry
    {
        HealUntilEntry(std::uint32_t auraId_ = 0, float pct_ = 95.f, float readAsPct_ = kHealUntilReadAsPct)
            : auraId(auraId_), pct(pct_), readAsPct(readAsPct_)
        {
        }

        std::uint32_t auraId = 0;           // the carrier has this aura (0: the entry does nothing)
        float pct = 95.f;                   // bots heal the carrier on to this percent
        float readAsPct = kHealUntilReadAsPct;  // ...reading it as at most this while it is below pct
    };

    // Gnomeregan BM-G (HANDOFF §7 #12 A, F19: heroic Electrocutioner's Static). Moving costs a carrier of `auraId`:
    // while a bot carries more than `aboveStacks` of it (Unit::GetAuraCount, any caster), it makes no optional move.
    // No repositioning (role solves, the healer's sight and reach walks, the preferred band, a tank's re-facing walk,
    // a drop rule's walk), no chasing (stock chase and follow, the chase valve and its routes) and no shuffling (the
    // stock movers are refused in Idle, Watch and Hold). It still answers every mechanic (a danger zone, a Spread, a
    // region it must stand in), by the shortest way out: its solves drop the role band, spacing, sight and reach
    // terms, and its escape stops at the first spot that is safe. Several entries: any one that holds.
    //     d.moveCosts.emplace_back(SPELL_STATIC, 12);     // above 12 Static (13 and more)
    struct MoveCostEntry
    {
        MoveCostEntry(std::uint32_t auraId_ = 0, std::uint32_t aboveStacks_ = 0)
            : auraId(auraId_), aboveStacks(aboveStacks_)
        {
        }

        std::uint32_t auraId = 0;           // the carrier has this aura (0: the entry does nothing)
        std::uint32_t aboveStacks = 0;      // more stacks than this (0: any stack)
    };

    // Gnomeregan BM-G (§7 #12 A, F20): ranged bots prefer to stand `minYd` to `maxYd` from the encounter owner (the
    // boss), 2D. Applies in combat to the bots whose role band is a ranged one (ranged damage dealers and healers) and
    // that fit `who` (Desc::Fits; Any: both): their role band becomes this band round the owner, and a bot outside it
    // walks in by a role solve (MechanicBotBrain.cpp StepRangeBand), unless a MoveCostEntry holds for it. The first
    // entry that fits wins.
    //     d.rangeBands.emplace_back(11.f, 12.f);      // Electrocutioner: a short step from the donut or the circle
    // Gnomeregan F-2 (RF-1 Minor-4): KeepOut clips floor out of the band (2D circles, e.g. a corner with no line of
    // sight to the owner): a bot inside one is out of its band, and the band walk never ends inside one.
    //     d.rangeBands.emplace_back(19.f, 26.f).KeepOut(-896.f, 383.5f, 7.75f);   // Crowd Pummeler's blind corner
    struct RangeBandEntry
    {
        struct KeepOutCircle
        {
            float x = 0.f;
            float y = 0.f;
            float radiusYd = 0.f;           // <= 0 or not finite: the circle does nothing
        };

        RangeBandEntry(float minYd_ = 0.f, float maxYd_ = 0.f) : minYd(minYd_), maxYd(maxYd_) { }

        RangeBandEntry& For(Who who_)
        {
            who = who_;
            return *this;
        }

        RangeBandEntry& KeepOut(float x, float y, float radiusYd)
        {
            keepOut.push_back({ x, y, radiusYd });
            return *this;
        }

        float minYd = 0.f;
        float maxYd = 0.f;                  // <= minYd or not finite: the entry does nothing
        Who who = Who::Any;                 // which ranged bots (Any: ranged damage dealers and healers)
        std::vector<KeepOutCircle> keepOut; // floor clipped out of the band (none: the whole band)
    };

    // Blackrock Depths WP-DS (shared doc BD3, aura-conditioned boss data): "hold action A on unit U while U has aura
    // X", one rule for Eviscerator's Spell Eater (hold magic damage on him while the shell is up), Angerforge's
    // Commanding Aura (hold interrupts on an add that has it) and any "leave it alone while" case. The bots, not the
    // server: a human may still hit or kick it.
    //     d.holds.push_back(HoldEntry::Damage(NPC_EVISCERATOR, SPELL_SPELL_EATER, kSchoolMaskMagic));
    //     d.holds.push_back(HoldEntry::Interrupts(NPC_ANVILRAGE_OFFICER, SPELL_COMMANDING_AURA));
    //     d.holds.push_back(HoldEntry::Attacks(NPC_SHADE, SPELL_INCORPOREAL));
    constexpr std::uint32_t kSchoolMaskPhysical = 0x01;
    constexpr std::uint32_t kSchoolMaskMagic = 0x7E;     // holy, fire, nature, frost, shadow, arcane
    constexpr std::uint32_t kSchoolMaskAll = 0x7F;

    struct HoldEntry
    {
        enum class Action : std::uint8_t
        {
            Damage,         // no damaging spell (direct, periodic, leech, weapon) whose school meets `schoolMask`;
                            // a wand's shot counts by the wand's damage school. White swings are not held (Attacks).
            Interrupts,     // no kick or cast-stopper used as one: neither `boss kick` nor the stock rows
            Attacks         // no attack at all: the unit is treated as ignored (IgnoreEntry) while the hold is on
        };

        HoldEntry(std::uint32_t entry_ = 0, std::uint32_t auraId_ = 0, Action action_ = Action::Damage,
            std::uint32_t schoolMask_ = kSchoolMaskMagic, Condition when_ = {})
            : entry(entry_), auraId(auraId_), action(action_), schoolMask(schoolMask_), when(std::move(when_))
        {
        }

        static HoldEntry Damage(std::uint32_t entry, std::uint32_t auraId, std::uint32_t schoolMask = kSchoolMaskMagic,
            Condition when = {})
        {
            return HoldEntry(entry, auraId, Action::Damage, schoolMask, std::move(when));
        }
        static HoldEntry Interrupts(std::uint32_t entry, std::uint32_t auraId, Condition when = {})
        {
            return HoldEntry(entry, auraId, Action::Interrupts, 0, std::move(when));
        }
        static HoldEntry Attacks(std::uint32_t entry, std::uint32_t auraId, Condition when = {})
        {
            return HoldEntry(entry, auraId, Action::Attacks, kSchoolMaskAll, std::move(when));
        }

        // One unit (raw ObjectGuid), whatever its entry.
        HoldEntry& OnUnit(std::uint64_t guid_)
        {
            guid = guid_;
            return *this;
        }

        std::uint32_t entry = 0;            // 0 (and no guid): any non-player unit
        std::uint64_t guid = 0;             // raw ObjectGuid; set: that unit only
        std::uint32_t auraId = 0;           // the unit has this aura (0: always)
        Action action = Action::Damage;
        std::uint32_t schoolMask = kSchoolMaskMagic;    // Damage: the held schools (SpellSchoolMask bits)
        Condition when;                     // judged on the unit too (both must hold)
    };

    // When the holder of a pass throws (Blackrock Spire WP-PS, PassEntry below). Any one trigger that fires makes the
    // throw due; a minimum hold (PassEntry::MinHold) holds it back unless the trigger is Urgent().
    struct PassTrigger
    {
        enum class Kind : std::uint8_t
        {
            HeldFor,        // the holder has held it `ms` or longer
            UnitNear,       // a live unit of `entry` (0: the encounter owner) is within `yd` of the holder, `when`
                            // holding on that unit (Solakar hunting, the Hatcher chasing)
            Casting,        // a unit of `entry` (0: the owner) casts or channels spell `id`, within `yd` of the holder
                            // (0: any distance)
            Mechanic        // a live registry mechanic tagged `id` resolves within `ms` (0: while it is live at all)
        };

        static PassTrigger HeldFor(std::chrono::milliseconds ms)
        {
            PassTrigger t;
            t.kind = Kind::HeldFor;
            t.ms = ms;
            return t;
        }
        static PassTrigger UnitNear(std::uint32_t entry, float yd, Condition when = {})
        {
            PassTrigger t;
            t.kind = Kind::UnitNear;
            t.entry = entry;
            t.yd = yd;
            t.when = std::move(when);
            return t;
        }
        static PassTrigger Casting(std::uint32_t entry, std::uint32_t spellId, float yd = 0.f)
        {
            PassTrigger t;
            t.kind = Kind::Casting;
            t.entry = entry;
            t.id = spellId;
            t.yd = yd;
            return t;
        }
        static PassTrigger Mechanic(std::uint32_t tag, std::chrono::milliseconds within = std::chrono::milliseconds(0))
        {
            PassTrigger t;
            t.kind = Kind::Mechanic;
            t.id = tag;
            t.ms = within;
            return t;
        }

        // Fires through the minimum holds (a Hunt 4 yd away beats searing a wound).
        PassTrigger& Urgent()
        {
            urgent = true;
            return *this;
        }

        Kind kind = Kind::HeldFor;
        std::uint32_t entry = 0;
        std::uint32_t id = 0;
        float yd = 0.f;
        std::chrono::milliseconds ms{ 0 };
        Condition when;                     // UnitNear: judged on the unit
        bool urgent = false;
    };

    // A minimum hold: a new holder keeps it at least `ms` from its catch while `when` holds on it (a wounded catcher
    // holds 2 s to sear the wound; the script clears the wound, the condition fails, the hold is over).
    struct PassHold
    {
        PassHold(std::chrono::milliseconds ms_ = std::chrono::milliseconds(0), Condition when_ = {})
            : ms(ms_), when(std::move(when_))
        {
        }

        std::chrono::milliseconds ms{ 0 };
        Condition when;
    };

    // A pass (Blackrock Spire WP-PS; shared doc BS6, library L26 Hot Potato; designer ruling 57): the player with
    // `holderAura` holds something and throws it on to another player. Boss data names who may catch it, the range,
    // the pick among catchers, when to throw and how long a catcher holds first; bots throw by it, humans by hand. The
    // pure rules are MechanicPass.h, the bot glue MechanicBotPass.h; README "Pass". Solakar's Father Flame:
    //     d.passes.push_back(PassEntry::WithItem(SPELL_FATHER_FLAME, ITEM_FATHER_FLAME, 30.f)
    //         .CatchWhen(Condition::AnyOf({ Condition::LacksAura(SPELL_SCORCHED_HANDS),
    //             Condition::HasAura(SPELL_SPEAR_WOUND) }))
    //         .Prefer(Condition::HasAura(SPELL_SPEAR_WOUND))
    //         .FarFrom(NPC_SOLAKAR).FarFrom(NPC_ROOKERY_HATCHER).KeepFrom(15.f).FewestZones(TAG_EGGS, 4.f)
    //         .When(PassTrigger::UnitNear(NPC_SOLAKAR, 15.f, Condition::HasAura(SPELL_WARDENS_HUNT)))
    //         .When(PassTrigger::UnitNear(NPC_ROOKERY_HATCHER, 15.f))
    //         .When(PassTrigger::HeldFor(6000ms))
    //         .MinHold(2000ms, Condition::HasAura(SPELL_SPEAR_WOUND)));
    struct PassEntry
    {
        enum class Throw : std::uint8_t
        {
            Item,       // the holder uses item `item` on the catcher (playerbots' ImbueItem; a human by hand)
            Spell,      // the holder casts spell `spell` at the catcher
            MoveAura    // the module moves `holderAura` to the catcher itself and puts `afterAura` on the thrower
                        // for `afterMs` (the `pass` drill; a script with no item or spell)
        };

        explicit PassEntry(std::uint32_t holderAura_ = 0, float range_ = 30.f) : holderAura(holderAura_),
            range(range_)
        {
        }

        static PassEntry WithItem(std::uint32_t holderAura, std::uint32_t itemEntry, float range = 30.f)
        {
            PassEntry e(holderAura, range);
            e.how = Throw::Item;
            e.item = itemEntry;
            return e;
        }
        static PassEntry WithSpell(std::uint32_t holderAura, std::uint32_t spellId, float range = 30.f)
        {
            PassEntry e(holderAura, range);
            e.how = Throw::Spell;
            e.spell = spellId;
            return e;
        }
        static PassEntry MovingAura(std::uint32_t holderAura, float range = 30.f, std::uint32_t afterAura_ = 0,
            std::chrono::milliseconds afterMs_ = std::chrono::milliseconds(0))
        {
            PassEntry e(holderAura, range);
            e.how = Throw::MoveAura;
            e.afterAura = afterAura_;
            e.afterMs = afterMs_;
            return e;
        }

        // Who may catch it (judged on the candidate; default: anyone). Scorched Hands: LacksAura(SCORCHED).
        PassEntry& CatchWhen(Condition c)
        {
            catchWhen = std::move(c);
            return *this;
        }
        // Ranked before everyone else (judged on the candidate): the wounded first.
        PassEntry& Prefer(Condition c)
        {
            prefer = std::move(c);
            preferSet = true;
            return *this;
        }
        // Prefer catchers far from the nearest live unit of `entry` (0: the encounter owner); call once per unit.
        PassEntry& FarFrom(std::uint32_t entry)
        {
            farFrom.push_back(entry);
            return *this;
        }
        // A catcher bot readying for the catch stands at least this far from the FarFrom units (0: no limit).
        PassEntry& KeepFrom(float yd)
        {
            keepFromYd = yd;
            return *this;
        }
        // Prefer catchers with the fewest live registry mechanics tagged `tag` within `yd` (edge distance); each one
        // costs `weightYd` yards of FarFrom distance in the pick. Bots ready for the catch on a spot with none.
        PassEntry& FewestZones(std::uint32_t tag, float yd, float weightYd = 10.f)
        {
            zoneTag = tag;
            zoneYd = yd;
            zoneWeightYd = weightYd;
            return *this;
        }
        PassEntry& When(PassTrigger t)
        {
            triggers.push_back(std::move(t));
            return *this;
        }
        PassEntry& MinHold(std::chrono::milliseconds ms, Condition when = {})
        {
            holds.emplace_back(ms, std::move(when));
            return *this;
        }
        // The throw's flight: the holder does not throw again (and the target is not judged) for this long.
        PassEntry& Flight(std::chrono::milliseconds ms)
        {
            flight = ms;
            return *this;
        }
        // How many of the best-ranked catcher bots walk to a ready spot (0: none moves; the pick still runs).
        PassEntry& ReadyCatchers(std::uint8_t n)
        {
            readyCatchers = n;
            return *this;
        }
        // No line of sight needed between the holder and the catcher.
        PassEntry& NoSight()
        {
            sight = false;
            return *this;
        }
        // Ready spots stay inside this box (map x, y), or this circle (0 radius: no room).
        PassEntry& Room(float minX, float minY, float maxX, float maxY)
        {
            roomMinX = minX;
            roomMinY = minY;
            roomMaxX = maxX;
            roomMaxY = maxY;
            roomBox = true;
            return *this;
        }
        PassEntry& RoomCircle(float x, float y, float r)
        {
            roomX = x;
            roomY = y;
            roomR = r;
            return *this;
        }
        // Judged on the encounter owner: while it fails the pass is idle (no throw, no ready spot).
        PassEntry& ActiveWhen(Condition c)
        {
            active = std::move(c);
            return *this;
        }

        std::uint32_t holderAura = 0;       // the holder is the player with this aura
        Throw how = Throw::Item;
        std::uint32_t item = 0;
        std::uint32_t spell = 0;
        std::uint32_t afterAura = 0;        // MoveAura: put on the thrower
        std::chrono::milliseconds afterMs{ 0 };     // ...for this long (0: the aura's own duration)
        float range = 30.f;
        bool sight = true;
        Condition catchWhen;
        Condition prefer;
        bool preferSet = false;
        std::vector<std::uint32_t> farFrom;
        float keepFromYd = 0.f;
        std::uint32_t zoneTag = 0;          // 0: no zone count
        float zoneYd = 4.f;
        float zoneWeightYd = 10.f;
        std::vector<PassTrigger> triggers;
        std::vector<PassHold> holds;
        std::chrono::milliseconds flight{ 1500 };
        std::uint8_t readyCatchers = 1;
        bool roomBox = false;
        float roomMinX = 0.f, roomMinY = 0.f, roomMaxX = 0.f, roomMaxY = 0.f;
        float roomX = 0.f, roomY = 0.f, roomR = 0.f;
        Condition active;
    };

    struct Descriptor
    {
        std::vector<InterruptEntry> interrupts;
        std::vector<DispelEntry> dispels;
        std::vector<PurgeEntry> purges;
        std::vector<TargetEntry> targets;
        std::vector<IgnoreEntry> ignores;
        std::vector<TauntSwapEntry> tauntSwaps;     // Blackrock Spire WP-TS
        std::vector<PassEntry> passes;              // Blackrock Spire WP-PS
        std::vector<HoldEntry> holds;               // Blackrock Depths WP-DS (BD3)
        std::vector<HealUntilEntry> healUntil;      // Zul'Farrak F17 (Desc::HealReadPct)
        std::vector<MoveCostEntry> moveCosts;       // Gnomeregan BM-G (Desc::MoveCostly)
        std::vector<RangeBandEntry> rangeBands;     // Gnomeregan BM-G (Desc::RangeBandFor)
        // Auras that mark a charmed player as turned against the group (SFK F1: Arugal's Turned). A charmed player
        // carrying one is a valid attack target for bots, and bots stop healing, buffing and curing it, until the
        // aura or the charm ends. Empty: a charmed teammate stays off limits (the playerbots default).
        std::vector<std::uint32_t> hostilePlayerAuras;
        std::uint32_t phase = 0;

        bool Empty() const
        {
            return interrupts.empty() && dispels.empty() && purges.empty() && targets.empty() && ignores.empty()
                && tauntSwaps.empty() && passes.empty() && holds.empty() && hostilePlayerAuras.empty()
                && healUntil.empty() && moveCosts.empty() && rangeBands.empty();
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

    // NearPlayer's distance (Stratholme F-1b, R-1-BM m-1): on the plane, like the registry's circles it guards
    // against (Maleki's Shatter), and only within their default height tolerance (Shape::zTolerance, 6 yd): a player
    // on a walkway 4.5 yd below and 7 yd away is 7 yd away, one on another floor is not near at all. -1: not near.
    constexpr float kNearPlayerZToleranceYd = 6.f;

    inline float NearPlayerDistance(float dx, float dy, float dz)
    {
        if (dz > kNearPlayerZToleranceYd || dz < -kNearPlayerZToleranceYd)
            return -1.f;
        return std::sqrt(dx * dx + dy * dy);
    }

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
        float nearestPlayerYd = -1.f;                           // yd to the nearest other live player within
                                                                // References::playerRange (-1: none, or unasked)
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
            case CondKind::NearPlayer:
                return u.nearestPlayerYd >= 0.f && u.nearestPlayerYd <= c.value;
            case CondKind::All:
                for (Condition const& sub : c.all)
                    if (!Holds(sub, u, ctx))
                        return false;
                return true;
            case CondKind::Not:
                return c.all.empty() || !Holds(c.all.front(), u, ctx);
            case CondKind::Custom:
                for (auto const& [cond, result] : u.custom)
                    if (cond == &c)
                        return result;
                return false;
        }
        return false;
    }

    // Everything the descriptor's conditions read, so the view gathers only that: aura ids, NearUnit entries with the
    // largest range asked of each, zone ids, the largest NearPlayer range, and the Custom conditions (pointers into
    // `d`, nested ones included: valid while `d` is).
    struct References
    {
        std::vector<std::uint32_t> auras;
        std::vector<std::pair<std::uint32_t, float>> near;
        std::vector<std::uint32_t> zones;
        std::vector<Condition const*> custom;
        float playerRange = 0.f;
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
            case CondKind::NearPlayer:
                r.playerRange = std::max(r.playerRange, c.value);
                break;
            case CondKind::All:
            case CondKind::Not:
                for (Condition const& sub : c.all)
                    AddReference(r, sub);
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
        for (DispelEntry const& e : d.dispels)
            AddReference(r, e.when);
        for (TauntSwapEntry const& e : d.tauntSwaps)
            AddReference(r, e.when);
        // Blackrock Depths WP-DS: the hold's aura and condition, both judged on the held unit.
        for (HoldEntry const& e : d.holds)
        {
            if (e.auraId && !Contains(r.auras, e.auraId))
                r.auras.push_back(e.auraId);
            AddReference(r, e.when);
        }
        // Blackrock Spire WP-PS: the holder aura (who holds it), the catch, preference, hold, gate and trigger
        // conditions.
        for (PassEntry const& e : d.passes)
        {
            if (e.holderAura && !Contains(r.auras, e.holderAura))
                r.auras.push_back(e.holderAura);
            AddReference(r, e.catchWhen);
            AddReference(r, e.prefer);
            AddReference(r, e.active);
            for (PassHold const& h : e.holds)
                AddReference(r, h.when);
            for (PassTrigger const& t : e.triggers)
                AddReference(r, t.when);
        }
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

    // Is the entry's aura held on this member now (its `when` does not hold; Stratholme X1)? `u`: the member's facts.
    inline bool DispelHeld(DispelEntry const& e, UnitFacts const& u, Context const& ctx)
    {
        return !Holds(e.when, u, ctx);
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
        bool held = false;          // DispelHeld for this member now (the caller judged it): left out
        std::uint32_t rankStacks = 0;   // the member's stacks of the entry's rankAura (MostStacksFirst; 0: none)
    };

    struct DispelPick
    {
        std::size_t affliction = 0;
        std::size_t entry = 0;
        bool urgent = false;
    };

    // The listed afflictions, best first: urgent before the rest, then the entry's priority, then (WP-DS) the more
    // stacks of the entry's rankAura (MostStacksFirst; entries without one count 0), then the aura with less time
    // left, then the member's stock rank and key. Unlisted auras are left out (the stock order handles them,
    // after every listed one), and so are held ones (Affliction::held). `urgentOnly`: the urgent ones only (the
    // `boss dispel urgent` action).
    inline std::vector<DispelPick> OrderDispels(Descriptor const& d, std::vector<Affliction> const& afflictions,
        bool urgentOnly = false)
    {
        std::vector<DispelPick> out;
        for (std::size_t i = 0; i < afflictions.size(); ++i)
        {
            std::optional<std::size_t> const entry = FindDispel(d, afflictions[i].auraId);
            if (!entry || afflictions[i].held)
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
            std::uint32_t const sx = d.dispels[a.entry].rankAura ? x.rankStacks : 0;
            std::uint32_t const sy = d.dispels[b.entry].rankAura ? y.rankStacks : 0;
            if (sx != sy)
                return sx > sy;
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
        bool mainTank = false;  // the group's main tank (Who::MainTank; Who::AssistTank is every other tank)
        Key guid = 0;           // the bot itself (TargetEntry::OnlyFor); 0 matches no OnlyFor entry
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
            case Who::MainTank:
                return bot.tank && bot.mainTank;
            case Who::AssistTank:
                return bot.tank && !bot.mainTank;
        }
        return false;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Holds (Blackrock Depths WP-DS, shared doc BD3)
    // ----------------------------------------------------------------------------------------------------------------

    // Does the hold apply to the unit now: the unit matches (guid; else entry; entry 0: any non-player unit), it has
    // the aura (auraId 0: no aura needed) and `when` holds on it. A dead unit is never held.
    inline bool HoldOn(HoldEntry const& e, UnitFacts const& u, Context const& ctx)
    {
        if (!u.alive)
            return false;
        bool const match = e.guid ? e.guid == u.guid : e.entry ? !u.player && e.entry == u.entry : !u.player;
        if (!match || (e.auraId && !Contains(u.auras, e.auraId)))
            return false;
        return Holds(e.when, u, ctx);
    }

    // The schools no damage may come in on the unit now (SpellSchoolMask bits): every Damage hold's mask, and all of
    // them for an Attacks hold. 0: nothing held.
    inline std::uint32_t HeldDamageMask(Descriptor const& d, UnitFacts const& u, Context const& ctx)
    {
        std::uint32_t mask = 0;
        for (HoldEntry const& e : d.holds)
            if ((e.action == HoldEntry::Action::Damage || e.action == HoldEntry::Action::Attacks) && HoldOn(e, u, ctx))
                mask |= e.action == HoldEntry::Action::Attacks ? kSchoolMaskAll : e.schoolMask;
        return mask;
    }

    // A damaging spell of `spellSchoolMask` on the unit is held now (any school in common with a held mask).
    inline bool DamageHeld(Descriptor const& d, UnitFacts const& u, Context const& ctx, std::uint32_t spellSchoolMask)
    {
        return (HeldDamageMask(d, u, ctx) & spellSchoolMask) != 0;
    }

    // No kick on the unit now: an Interrupts hold (an Attacks hold covers kicks through the ignore list).
    inline bool KicksHeld(Descriptor const& d, UnitFacts const& u, Context const& ctx)
    {
        for (HoldEntry const& e : d.holds)
            if (e.action == HoldEntry::Action::Interrupts && HoldOn(e, u, ctx))
                return true;
        return false;
    }

    inline bool HasHolds(Descriptor const& d, HoldEntry::Action action)
    {
        return std::any_of(d.holds.begin(), d.holds.end(), [action](HoldEntry const& e) { return e.action == action; });
    }

    // Does the ignore entry concern this bot? Zul'Farrak F33: an entry for a role (who not Any) concerns only a bot
    // that Fits it; with no bot facts (nullptr) only the Any entries do.
    inline bool IgnoreConcerns(IgnoreEntry const& e, BotFacts const* bot)
    {
        return e.who == Who::Any || (bot && Fits(e.who, *bot));
    }

    // Is the unit on this bot's ignore list now? An entry with neither guid nor entry matches nothing; one for a role
    // only for a bot of it (IgnoreConcerns). WP-DS: an Attacks hold that applies counts as an ignore entry (no damage
    // from bots or pets, no AoE count, no kick), for every bot.
    inline bool IsIgnored(Descriptor const& d, UnitFacts const& u, Context const& ctx,
        BotFacts const* bot = nullptr)
    {
        for (IgnoreEntry const& e : d.ignores)
        {
            bool const match = e.guid ? e.guid == u.guid : e.entry && e.entry == u.entry;
            if (match && IgnoreConcerns(e, bot) && Holds(e.when, u, ctx))
                return true;
        }
        for (HoldEntry const& e : d.holds)
            if (e.action == HoldEntry::Action::Attacks && HoldOn(e, u, ctx))
                return true;
        return false;
    }

    // Zul'Farrak F17: the health percent a bot reads for a unit with `pct` health (HealUntilEntry). Below an entry's
    // pct while the unit carries its aura (`hasAura(auraId)`): at most the entry's readAsPct; several such entries: the
    // lowest. Otherwise (no aura, at or above pct, an entry with aura 0) `pct` unchanged. Never raises the value.
    template <class HasAura>
    float HealReadPct(std::vector<HealUntilEntry> const& entries, float pct, HasAura&& hasAura)
    {
        float out = pct;
        for (HealUntilEntry const& e : entries)
            if (e.auraId && pct < e.pct && hasAura(e.auraId))
                out = std::min(out, e.readAsPct);
        return out;
    }

    // Gnomeregan BM-G: does a MoveCostEntry hold for a bot? `stacksOf(auraId)` gives the bot's stacks of the aura (0
    // without it). True when any entry with an aura has more than its aboveStacks.
    template <class StacksOf>
    bool MoveCostly(std::vector<MoveCostEntry> const& entries, StacksOf&& stacksOf)
    {
        for (MoveCostEntry const& e : entries)
            if (e.auraId && static_cast<std::uint32_t>(stacksOf(e.auraId)) > e.aboveStacks)
                return true;
        return false;
    }

    // Gnomeregan BM-G: is a RangeBandEntry usable (a finite band with maxYd > minYd >= 0)?
    inline bool RangeBandValid(RangeBandEntry const& e)
    {
        return std::isfinite(e.minYd) && std::isfinite(e.maxYd) && e.minYd >= 0.f && e.maxYd > e.minYd;
    }

    // Gnomeregan BM-G: the preferred band for a bot (the first valid entry whose `who` fits it), or nullptr. Only a
    // bot whose role band is a ranged one (`rangedRole`: a healer, or a ranged bot that is not a tank) takes one.
    inline RangeBandEntry const* RangeBandFor(std::vector<RangeBandEntry> const& entries, BotFacts const& bot,
        bool rangedRole)
    {
        if (!rangedRole)
            return nullptr;
        for (RangeBandEntry const& e : entries)
            if (RangeBandValid(e) && Fits(e.who, bot))
                return &e;
        return nullptr;
    }

    // Gnomeregan F-2 (RF-1 Minor-4): how deep (x, y) lies inside a band's keep-out circles (the deepest; 0 outside
    // every one): a bot there is that far out of its band (added to LockRules::BandGapYd's gap).
    inline float RangeBandKeepOutYd(std::vector<RangeBandEntry::KeepOutCircle> const& keepOut, float x, float y)
    {
        float deepest = 0.f;
        for (RangeBandEntry::KeepOutCircle const& k : keepOut)
        {
            if (!std::isfinite(k.radiusYd) || k.radiusYd <= 0.f)
                continue;
            float const d = std::hypot(x - k.x, y - k.y);
            if (d < k.radiusYd)
                deepest = std::max(deepest, k.radiusYd - d);
        }
        return deepest;
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
            if (!Fits(e.who, bot) || (e.bot && e.bot != bot.guid) || !Holds(e.when, u, ctx))
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
            if (!u || !u->alive || IsIgnored(d, *u, ctx, &bot))
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
