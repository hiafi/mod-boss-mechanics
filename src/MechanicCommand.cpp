/*
 * mod-boss-mechanics: the `.mech` GM command (STAGE-M1-HANDOFF §3.10). SEC_GAMEMASTER, in game only.
 *
 *   .mech test <shape> <size> <ms> [key=value ...]     (rider=<spell>: the module's spells 205008-205099 only)
 *   .mech clear [all] | .mech list | .mech ledger [name] | .mech weakened <0-7> | .mech markers [range]
 *   .mech palette <get-out color> <get-in color> [cross-lane color] [cell color]
 *                                                    (red yellow blue purple orange green teal white black)
 *   .mech barrage [secs=30] [every=900] [warn=2000] [shapes=circle,donut,cone,line,projectile] [dmg=1] [self=0]
 *                 [seed=]
 *   .mech minefield [count=10] [radius=3] [spread=15] [arm=3000] [grace=500] [dmg=1] [seed=]
 *   .mech minefield layout=u [gap=8] [depth=12] [radius=3] [arm=3000] [grace=500] [dmg=1]
 *   .mech drill <smite|cookie|fenrus|alexei|soaks|ras|deadmines-followups|sfk-hunt|scholomance-shapes
 *               |stratholme-followups> [cycles=3] [seed=] [dmg=1] [self=0]
 *   .mech dummy <program|stop|report> [dmg=<n>]      (the selected training dummy 300402, MechanicDummyAI.cpp)
 *   .mech bots [all]                                 (the report itself lives in the playerbots glue, M2)
 *
 * `.mech test` registers through the public Mechanic:: API, owned by a hostile Mechanic Test Owner (300401) summoned at
 * the GM (one per map, reused; it despawns after 5 minutes without a live mechanic, or on `.mech clear`). GMs are never
 * valid targets: test with `.gm off`. Only the GMs who ran `.mech test` and their group members can be targeted or hit
 * (Registry::IsInTestScope). `.mech clear` ends the test encounters only; `.mech clear all` every encounter on the
 * map. Shapes, keys and defaults: README.md, section ".mech".
 *
 * `at=member` (common key) lands each repeat on a random alive member of the GM's group, picked when that repeat
 * registers; `seed=` seeds every pick the command makes itself (scatter, member, barrage shapes). `.mech barrage` and
 * `.mech minefield` are scripted series of `.mech test` registrations for the bot acceptance runs (the barrage's shot
 * geometry is MechanicBarrage.h). Delayed registrations run from the test owner's scheduler, so `.mech clear` (which
 * despawns the owner) also ends a barrage.
 *
 * M3 (STAGE-M3-HANDOFF §3.11): the player kinds frontal, bait, holdstill and spot, the eligibility, ordering,
 * self-trigger, lane, cluster, park and landing keys, and `.mech drill`, whose steps are `.mech test` lines run through
 * the same parser (the timelines and landing geometry are MechanicDrill.h, desk-checked by tests/drill_test.cpp).
 *
 * M4 (STAGE-M4-HANDOFF §3.2, §3.7): the drill runner (MechanicDrillRunner.h: RunDrill, StartProgram, StopProgram) is
 * implemented here, on the same parser: `.mech test` lines are parsed against a ParseCtx (the reference unit standing
 * in for the GM) so the training dummy and the dpssim mechanics mode run the drills without a GM session. `.mech
 * minefield layout=u` lays the fixed U of MechanicDrillPlan.h (the M3 playtest's "bot left behind" repro).
 *
 * Deadmines Pilot gap 8 (deadmines-pilot.HANDOFF §3.9, drill `deadmines-followups`): `at=last` lands a shape on the
 * end of the last line this command (or drill run) registered, `at=each` puts one shape under every member,
 * `pick=other` picks a non-tank the chain has not picked yet (Drill::PickPool), `min=<yd>` prefers members that far
 * from a line's source, `fix=1` freezes a `to=` line at registration (from its source toward where the to= unit stands
 * then), `reach=1` also ends it there, and `frontal offset=<deg>` turns a frontal. Drill steps also take `after=` and
 * `cycle=` (MechanicDrill.h, StepOffsets), which the runner strips before this parser.
 *
 * Shadowfang Keep build §3.9 (drill `sfk-hunt`): `hunt` summons a chaser at the selected unit that walks at a member
 * with a catch circle on it, and `soak bell=<n>` is an alarm bell that ends the hunt when filled and lands a circle
 * there (RegisterHunt, RegisterBell; MechanicDrill.h "the chase").
 *
 * Scholomance build (WP-X, WP-G §3.9, drill `scholomance-shapes`): `decoy=1` registers a shape as a Decoy() (lanes
 * safe=1: the free lane marked as one), `grid=<n> apart= decoys=` lays n x n circles with random decoys
 * (RegisterGrid), `away=<yd>` moves a point landing exactly that far at a random bearing, `at=prev` lands on the last
 * point-anchored circle's centre, and `at=predict lead=<ms>` where a member will be (PredictedSpot; MechanicDrill.h
 * "Scholomance shapes").
 *
 * Stratholme build (WP-G §3.9, drill `stratholme-followups`): `cone to=member|target|me` aims a cone at that unit live
 * from its anchor (BZ1's tracking cone), and `exempt=target` (cone, line, projectile with to=) spares the aimed unit:
 * the other scope members are its eligible carriers (MechanicDrill.h "Stratholme follow-ups"). ST's hidden flag is
 * `tele=0`.
 *
 * Stratholme build X2 (drill `stratholme-drops`): `drop=<edge,away,behind,prefer> [room=<yd>] [dwell=<ms>]` on a circle
 * anchor=member or a line to=member gives the member's bot a DropSpotRule (MechanicDropSpot.h; the RegisterTest
 * `dropSpot` lambda): a circle room around the selected unit, the GM's facing as the travel direction, and the test
 * owner's live persistent point circles as enterable preferred spots.
 *
 * Commands run on the world thread while no map updates (handoff §2.1), so they may write the registry directly.
 */

#include "BossMechanics.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MechanicBarrage.h"
#include "MechanicBotReportHook.h"
#include "MechanicDescCommand.h"
#include "MechanicDrill.h"
#include "MechanicDropSpot.h"
#include "MechanicDrillPlan.h"
#include "MechanicDrillRunner.h"
#include "MechanicDummyAI.h"
#include "MechanicMarkerAI.h"
#include "MechanicProviders.h"
#include "MechanicTelegraph.h"
#include "MechanicWeakened.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "TemporarySummon.h"
#include "Tokenize.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;
using namespace BossMechanics;

namespace
{
    constexpr uint32 kMaxRepeat = 50;
    constexpr Milliseconds kDefaultBurn = Milliseconds(6000);
    constexpr uint32 kMaxBarrageShots = 600;
    constexpr uint32 kMaxMines = 100;
    constexpr Milliseconds kMineLife = Milliseconds(120000);    // `.mech minefield`
    constexpr uint32 kFirstMinefieldGroup = 1000;               // cap groups above the hand-typed `group=` values
    constexpr char const* kNoMemberMessage = "no alive member of your group on this map (self=1 includes you)";

    // One `hunt` (MechanicDrill.h, "the chase"): its chaser, the member it fixates, its catch circle. Shared by the
    // circle's resolve callback and a later `soak bell=` of the same command (or drill run), which ends it.
    struct HuntState
    {
        ObjectGuid chaser;
        ObjectGuid hunted;
        uint32 zone = 0;
        Milliseconds endsAt{ 0 };       // game time: the end of the chase
        bool live = false;              // cleared by a catch, a bell, or the hunted member gone
    };
    using HuntPtr = std::shared_ptr<HuntState>;

    // The dice of one command. Delayed registrations share it, so they keep drawing from the same seeded sequence.
    // `lastMember` is the member the last pick landed on (`pick=same`); `chain` the members picked since the last pick
    // that was not `pick=other`; `lastEnd` the far end of the last fixed line registered (`at=last`); `lastCircle` the
    // centre of the last point-anchored circle registered (`at=prev`); `hunt` the last `hunt` registered (a `soak
    // bell=` ends it).
    struct Dice
    {
        explicit Dice(uint32 seed) : rng(seed) { }

        Barrage::Rng rng;
        ObjectGuid lastMember;
        std::vector<ObjectGuid> chain;
        std::optional<Position> lastEnd;
        std::optional<Position> lastCircle;
        HuntPtr hunt;
    };
    using RngPtr = std::shared_ptr<Dice>;

    // ---- parsing ---------------------------------------------------------------------------------------------------
    std::optional<Color> ParseColor(std::string_view s)
    {
        for (uint8 i = 0; i < Ids::COLOR_COUNT; ++i)
            if (s == ColorName(static_cast<Color>(i)))
                return static_cast<Color>(i);
        return std::nullopt;
    }

    std::optional<uint8> ParseSchool(std::string_view s)
    {
        static char const* const names[] = { "physical", "holy", "fire", "nature", "frost", "shadow", "arcane" };
        for (uint8 i = 0; i < 7; ++i)
            if (s == names[i])
                return i;
        return std::nullopt;
    }

    // key=value options with defaults; remembers which keys were read and which failed to parse.
    class Options
    {
    public:
        bool Add(std::string_view token)
        {
            std::size_t const eq = token.find('=');
            if (eq == std::string_view::npos || eq == 0)
                return false;
            _kv[std::string(token.substr(0, eq))] = std::string(token.substr(eq + 1));
            return true;
        }

        bool Has(std::string const& key) const { return _kv.count(key) != 0; }

        std::string Str(std::string const& key, std::string const& def)
        {
            _used.insert(key);
            auto it = _kv.find(key);
            return it != _kv.end() ? it->second : def;
        }

        float Float(std::string const& key, float def)
        {
            _used.insert(key);
            auto it = _kv.find(key);
            if (it == _kv.end())
                return def;
            if (Optional<float> v = Acore::StringTo<float>(it->second))
                return *v;
            _bad.push_back(key);
            return def;
        }

        int32 Int(std::string const& key, int32 def)
        {
            _used.insert(key);
            auto it = _kv.find(key);
            if (it == _kv.end())
                return def;
            if (Optional<int32> v = Acore::StringTo<int32>(it->second))
                return *v;
            _bad.push_back(key);
            return def;
        }

        bool Bool(std::string const& key, bool def)
        {
            return Int(key, def ? 1 : 0) != 0;
        }

        Milliseconds Ms(std::string const& key, Milliseconds def)
        {
            return Milliseconds(std::max<int32>(0, Int(key, static_cast<int32>(def.count()))));
        }

        void MarkBad(std::string const& key) { _bad.push_back(key); }

        std::string Unused() const
        {
            std::string out;
            for (auto const& [k, v] : _kv)
                if (!_used.count(k))
                    out += (out.empty() ? "" : ", ") + k;
            return out;
        }

        std::string Bad() const
        {
            std::string out;
            for (std::string const& k : _bad)
                out += (out.empty() ? "" : ", ") + k;
            return out;
        }

    private:
        std::unordered_map<std::string, std::string> _kv;
        std::set<std::string> _used;
        std::vector<std::string> _bad;
    };

    // Splits `tokens[from..]` into key=value options; false (error sent) on a token that is not one.
    bool CollectOptions(ChatHandler* handler, std::vector<std::string_view> const& tokens, std::size_t from,
        Options& opt)
    {
        for (std::size_t i = from; i < tokens.size(); ++i)
        {
            if (!opt.Add(tokens[i]))
            {
                handler->SendErrorMessage("Not a key=value option: '{}'", tokens[i]);
                return false;
            }
        }
        return true;
    }

    // `seed=<n>` (0 and up), else a fresh random seed; the commands print it so a run can be repeated.
    RngPtr MakeRng(Options& opt)
    {
        uint32 const seed = opt.Has("seed") ? static_cast<uint32>(std::max<int32>(opt.Int("seed", 0), 0))
            : urand(1, 0x7FFFFFFF);
        return std::make_shared<Dice>(seed);
    }

    // ---- M3 key values ---------------------------------------------------------------------------------------------
    // `<a>:<b>[:<c>...]` split on ':'.
    std::vector<std::string_view> Fields(std::string const& s)
    {
        return Acore::Tokenize(s, ':', true);
    }

    std::optional<Role> ParseRole(std::string_view s)
    {
        if (s == "tank")
            return Role::Tank;
        if (s == "healer")
            return Role::Healer;
        if (s == "dps")
            return Role::Damage;
        if (s == "nontank")
            return Role::Healer | Role::Damage;
        if (s == "any")
            return Role::Any;
        return std::nullopt;
    }

    // `aura=<spell>:<min>[:<max>]` (eligibility band).
    bool ParseAuraBand(std::string const& s, Eligibility& e)
    {
        std::vector<std::string_view> const f = Fields(s);
        if (f.size() < 2 || f.size() > 3)
            return false;
        Optional<uint32> spell = Acore::StringTo<uint32>(f[0]);
        Optional<uint32> lo = Acore::StringTo<uint32>(f[1]);
        Optional<uint32> hi = f.size() == 3 ? Acore::StringTo<uint32>(f[2]) : Optional<uint32>(255);
        if (!spell || !*spell || !lo || !hi || *lo > 255 || *hi > 255 || *lo > *hi)
            return false;
        e.auraSpell = *spell;
        e.minStacks = static_cast<uint8>(*lo);
        e.maxStacks = static_cast<uint8>(*hi);
        return true;
    }

    // `self=<spell>:<min>` (self-trigger).
    bool ParseSelfTrigger(std::string const& s, uint32& spell, uint8& minStacks)
    {
        std::vector<std::string_view> const f = Fields(s);
        Optional<uint32> sp = f.size() == 2 ? Acore::StringTo<uint32>(f[0]) : std::nullopt;
        Optional<uint32> n = f.size() == 2 ? Acore::StringTo<uint32>(f[1]) : std::nullopt;
        if (!sp || !*sp || !n || *n > 255)
            return false;
        spell = *sp;
        minStacks = static_cast<uint8>(*n);
        return true;
    }

    // `order=<spell>[:asc|desc][:cap=<n>]`.
    bool ParseOrdering(std::string const& s, Ordering& o)
    {
        std::vector<std::string_view> const f = Fields(s);
        Optional<uint32> spell = f.empty() ? std::nullopt : Acore::StringTo<uint32>(f[0]);
        if (!spell || !*spell || f.size() > 3)
            return false;
        o.auraSpell = *spell;
        for (std::size_t i = 1; i < f.size(); ++i)
        {
            if (f[i] == "asc")
                o.ascending = true;
            else if (f[i] == "desc")
                o.ascending = false;
            else if (f[i].substr(0, 4) == "cap=")
            {
                Optional<uint32> cap = Acore::StringTo<uint32>(f[i].substr(4));
                if (!cap || *cap > 255)
                    return false;
                o.capStacks = static_cast<uint8>(*cap);
            }
            else
                return false;
        }
        return true;
    }

    // `shift=<ahead>:<left>` (yd, relative to the GM's facing).
    bool ParseShift(std::string const& s, float& ahead, float& left)
    {
        std::vector<std::string_view> const f = Fields(s);
        Optional<float> a = f.size() == 2 ? Acore::StringTo<float>(f[0]) : std::nullopt;
        Optional<float> l = f.size() == 2 ? Acore::StringTo<float>(f[1]) : std::nullopt;
        if (!a || !l)
            return false;
        ahead = *a;
        left = *l;
        return true;
    }

    // Everything one `.mech test` registration needs, resolved to plain values and guids so a delayed repeat can run
    // from the test owner's scheduler without holding a unit pointer.
    struct TestSpec
    {
        std::string shape;
        float size = 0.f;
        Milliseconds warn{ 0 };

        // shape keys
        float inner = 0.f;
        float angle = 90.f;
        float offset = 0.f;
        float width = 4.f;
        float exclude = 0.f;
        float speed = 10.f;
        bool back = false;
        Milliseconds pause{ 0 };
        Milliseconds grace{ 500 };
        uint8 cap = 0;
        uint32 capGroup = 0;
        Milliseconds life{ 0 };
        uint32 count = 3;
        uint8 spreadCount = 0;
        float gap = 6.f;
        Axis axis = Axis::X;
        float frameX = 40.f;
        float frameY = 40.f;
        bool safe = false;
        bool cross = false;
        Milliseconds burn = kDefaultBurn;
        uint8 need = 2;
        bool fromOwner = false;

        // landing: at=member picks a group member when the repeat registers; scatter offsets a point anchor
        bool atMember = false;
        bool atLast = false;            // at=last: the end of the last fixed line (Dice::lastEnd)
        bool atEach = false;            // at=each: one shape under every member
        bool includeGm = false;
        float scatter = 0.f;

        // common keys
        int32 dmg = -1;
        float pct = 20.f;
        float tankPct = -1.f;
        uint8 school = 0;
        bool weak = false;
        bool once = false;
        uint32 rider = 0;       // rider=<spell>: cast on every unit hit, the job hit included (module block only)
        Milliseconds persist{ 0 };
        Milliseconds tick{ 1000 };
        std::optional<Color> color;
        bool tele = true;

        // anchor: a point (the selected unit's position, the GM's facing) or a unit
        bool unitAnchor = false;
        Position point;
        ObjectGuid unit;
        ObjectGuid target;      // runout target / stack default anchor: the selected unit
        float facing = 0.f;
        uint32 tag = 1;
        ObjectGuid gm;          // who ran it: the test encounter's scope (the GM and their group)

        // ---- M3 (STAGE-M3-HANDOFF §3.11) ----
        enum class Pick : uint8
        {
            Any,                // a random member
            NonTank,            // a random member other than the selected unit's victim
            Same,               // the member the previous pick of this command landed on (else a random one)
            Other               // a non-tank the chain has not picked (Drill::PickPool)
        };
        enum class To : uint8
        {
            None,
            Member,
            Target,
            Me
        };

        bool rear = false;              // frontal
        bool lineFrontal = false;       // frontal line=1 (width = `width`)
        BaitRule bait = BaitRule::FewestStacks(Ids::SPELL_WEAKENED);
        float baitRadius = 6.f;
        float tolerance = -1.f;         // holdstill tol= (< 0: the registry default)
        float ring = 10.f;              // spot
        std::unordered_map<ObjectGuid, Position> spots;     // spot: filled when the repeat registers
        bool lane = false;              // stack/soak shape=line (width = `width`)
        float laneLength = 30.f;
        uint8 cluster = 0;
        uint8 runoutCount = 0;
        std::optional<Eligibility> eligibility;
        std::optional<Ordering> ordering;
        uint32 selfAura = 0;
        uint8 selfMin = 0;
        bool park = false;
        bool follow = false;
        Pick pick = Pick::Any;
        bool atGm = false;
        bool atGroup = false;
        bool anchorMember = false;
        To to = To::None;
        ObjectGuid toUnit;
        bool shifted = false;
        float shiftAhead = 0.f;
        float shiftLeft = 0.f;
        ObjectGuid selected;            // the selected unit: park, frontal source, pick=nontank's victim
        // gap 8 (line, to=)
        bool fix = false;               // fix=1: frozen at registration toward where the to= unit stands
        bool reach = false;             // reach=1: fixed and ending at that spot (at least `size` long)
        float minDist = 0.f;            // min=: to=member prefers members at least this far from the source
        // the chase (Shadowfang Keep build §3.9; MechanicDrill.h)
        float huntRate = Drill::kHuntRateNormal;    // hunt speed=: x player run speed
        uint32 bells = 0;               // soak bell=<n>: one of n bell points, ends the hunt when filled
        float bellApart = 15.f;         // soak apart=: the bell points' spacing
        float landRadius = 6.f;         // soak land=: the landing circle at the rung bell
        Milliseconds jump{ 2000 };      // soak jump=: the bound to the bell (the landing's warning)
        // Scholomance build (WP-X decoy; WP-G §3.9 shapes; MechanicDrill.h "Scholomance shapes")
        bool decoy = false;             // decoy=1: Mechanic::Decoy (lanes safe=1: the free lane, DecoyFree)
        uint32 grid = 0;                // circle grid=<n>: n x n circles `gridApart` apart (0: one circle)
        float gridApart = 0.f;
        uint32 decoys = 0;              // circle grid= decoys=<k>: k of them are decoys
        float away = 0.f;               // away=<yd>: a point anchor moved exactly this far at a random bearing
        bool atPrev = false;            // at=prev: the centre of the last point-anchored circle (Dice::lastCircle)
        // Stratholme X2: drop=<flags> on a shape a member carries (DropSpot::Flags), room= and dwell= with it.
        std::optional<DropSpot::Flags> drop;
        float dropRoom = 30.f;
        Milliseconds dropDwell{ 1200 };
        bool atPredict = false;         // at=predict: where a member will be `lead` ms from now
        Milliseconds lead{ Drill::kDefaultPredictLeadMs };
        // Stratholme build (WP-G §3.9, drill `stratholme-followups`)
        bool exemptTo = false;          // exempt=target: the to= unit is not eligible (BZ1; RegisterRepeat)
    };

    bool IsShapeName(std::string const& s)
    {
        static char const* const shapes[] = { "circle", "donut", "cone", "rearcone", "line", "projectile", "mine",
            "lanes", "spread", "runout", "stack", "soak", "frontal", "bait", "holdstill", "spot", "hunt" };
        for (char const* name : shapes)
            if (s == name)
                return true;
        return false;
    }

    void ApplyCommon(MechanicRef& ref, TestSpec const& spec, bool damageKind, bool allowPersist)
    {
        if (damageKind)
        {
            if (spec.dmg >= 0)
                ref.FlatDamage(spec.dmg);
            else
            {
                ref.Damage(spec.pct);
                if (spec.tankPct >= 0.f)
                    ref.TankDamage(spec.tankPct);
            }
            ref.School(spec.school);
            if (spec.rider)
                ref.Rider(spec.rider);
        }
        if (spec.once)
            ref.WeakenedOncePerPlayer();
        else if (spec.weak)
            ref.Weakened();
        if (allowPersist && spec.persist > Milliseconds(0))
            ref.Persistent(spec.persist, spec.tick);
        if (spec.eligibility)
            ref.Eligible(*spec.eligibility);
        if (spec.ordering)
            ref.Order(*spec.ordering);
        if (spec.color)
            ref.Color(*spec.color);
        if (!spec.tele)
            ref.NoTelegraph();
        ref.Tag(spec.tag);
    }

    // Registers one test mechanic for `owner`. Returns the ids made (empty: refused, reason in `err`).
    std::vector<uint32> RegisterTest(Creature* owner, TestSpec const& spec, std::string& err)
    {
        std::vector<uint32> ids;
        // (Re)assert the scope on every registration: a delayed repeat may land after the encounter was recreated.
        Registry::GetOrCreate(owner->GetMap()).AddScopePlayer(owner->GetGUID(), spec.gm);
        Unit* anchorUnit = spec.unitAnchor ? ObjectAccessor::GetUnit(*owner, spec.unit) : nullptr;
        if (spec.unitAnchor && !anchorUnit)
        {
            err = "the anchor unit is gone";
            return ids;
        }
        Anchor const anchor = anchorUnit ? Anchor(anchorUnit) : Anchor(spec.point);
        Position const here = anchorUnit ? anchorUnit->GetPosition() : spec.point;
        auto keep = [&ids](MechanicRef const& ref)
        {
            if (ref.IsValid())
                ids.push_back(ref.Id());
        };

        auto park = [&spec, owner](MechanicRef& ref)
        {
            if (spec.park)
                if (Unit* unit = ObjectAccessor::GetUnit(*owner, spec.selected))
                    ref.Park(unit);
        };
        // decoy=1 (WP-X): in the registering statement, before the ref commits.
        auto decoy = [&spec](MechanicRef& ref)
        {
            if (spec.decoy)
                ref.Decoy();
        };
        // drop= (Stratholme X2): the carrier's drop spot rule, built now: a circle room of room= yd around the
        // selected unit, the GM's facing as the travel direction, and with `prefer` every live persistent circle on a
        // point of this test owner as an enterable preferred spot (the drill's old webs).
        auto dropSpot = [&spec, owner](MechanicRef& ref)
        {
            if (!spec.drop)
                return;
            DropSpotRule rule;
            Unit const* sel = ObjectAccessor::GetUnit(*owner, spec.selected);
            if (spec.dropRoom > 0.f)
                rule.Room(sel ? sel->GetPosition() : owner->GetPosition(), spec.dropRoom);
            if (spec.drop->edge)
                rule.Edge();
            if (spec.drop->away)
                rule.Away();
            if (spec.drop->behind)
                rule.Behind(spec.facing);
            rule.MaxDwell(spec.dropDwell);
            if (spec.drop->prefer)
                if (Registry const* reg = Registry::Get(owner->GetMap()))
                    reg->ForEachActive(GameTime::GetGameTimeMS(), [&rule, owner](ActiveMechanic const& m)
                    {
                        if (m.owner == owner->GetGUID() && m.persist && !m.decoy && !m.anchor.IsUnit()
                            && m.IsShapeKind() && m.shape.type == ShapeType::Circle)
                            rule.Prefer(m.anchor.point, m.shape.radius, m.id);
                    });
            ref.DropSpot(rule);
        };
        // stack/soak shape=line: a lane centered on a point anchor along the GM's facing (on a unit: from the unit).
        auto laneSpec = [&spec, &anchor, anchorUnit, &here]()
        {
            if (anchorUnit)
                return Shapes::LineFacing(anchor, spec.facing, spec.laneLength, spec.width);
            Geo::Vec3 const src = Drill::LaneSource({ here.GetPositionX(), here.GetPositionY(), here.GetPositionZ() },
                spec.facing, spec.laneLength);
            return Shapes::LineFacing(Anchor(Position(src.x, src.y, src.z, spec.facing)), spec.facing,
                spec.laneLength, spec.width);
        };

        std::string const& s = spec.shape;
        if (s == "circle")
        {
            MechanicRef ref = Mechanic::Circle(owner, anchor, spec.size, spec.warn);
            ApplyCommon(ref, spec, true, true);
            park(ref);
            decoy(ref);
            dropSpot(ref);
            keep(ref);
        }
        else if (s == "donut")
        {
            MechanicRef ref = Mechanic::Donut(owner, anchor, spec.inner, spec.size, spec.warn);
            ApplyCommon(ref, spec, true, true);
            park(ref);
            decoy(ref);
            keep(ref);
        }
        else if (s == "cone" || s == "rearcone")
        {
            Anchor coneAnchor = anchor;
            if (!anchorUnit)
                coneAnchor.point.SetOrientation(spec.facing);
            ZoneSpec zone = Shapes::Cone(coneAnchor, spec.angle, spec.size);
            if (spec.to != TestSpec::To::None)
            {
                // to= (Stratholme BZ1): the cone turns to face that unit live (Registry::EffectiveShape).
                Unit* toUnit = ObjectAccessor::GetUnit(*owner, spec.toUnit);
                if (!toUnit)
                {
                    err = "the to= unit is gone";
                    return ids;
                }
                zone.to = Anchor(toUnit);
            }
            MechanicRef ref = Mechanic::Zone(owner, zone, spec.warn);
            if (s == "rearcone")
                ref.Rear();
            else if (spec.offset != 0.f)
                ref.FacingOffset(spec.offset);
            if (spec.follow)
                ref.FollowFacing();
            ApplyCommon(ref, spec, true, true);
            park(ref);
            decoy(ref);
            keep(ref);
        }
        else if (s == "frontal")
        {
            // Anchored on the selected creature, turning with it, its victim exempt (B7).
            ZoneSpec const zone = spec.lineFrontal
                ? Shapes::LineFacing(anchor, here.GetOrientation(), spec.size, spec.width)
                : Shapes::Cone(anchor, spec.angle, spec.size);
            MechanicRef ref = Mechanic::Zone(owner, zone, spec.warn);
            ref.FollowFacing();
            if (spec.rear)
                ref.Rear();
            else if (spec.offset != 0.f)
                ref.FacingOffset(spec.offset);
            ref.TankExempt();
            ApplyCommon(ref, spec, true, true);
            decoy(ref);
            keep(ref);
        }
        else if (s == "line" || s == "projectile")
        {
            Unit* toUnit = spec.to != TestSpec::To::None ? ObjectAccessor::GetUnit(*owner, spec.toUnit) : nullptr;
            if (spec.to != TestSpec::To::None && !toUnit)
            {
                err = "the to= unit is gone";
                return ids;
            }
            // to=: aimed at the unit live; the length stays `size` (BossMechanics.h, ZoneSpec Line).
            MechanicRef ref = toUnit
                ? Mechanic::Zone(owner, ZoneSpec{ anchor, Shape::Line(spec.size, spec.width), Anchor(toUnit) },
                    spec.warn)
                : Mechanic::Line(owner, anchor, spec.facing, spec.size, spec.width, spec.warn);
            if (spec.exclude > 0.f)
                ref.ExcludeSourceEnd(spec.exclude);
            if (s == "projectile")
            {
                ref.Projectile(spec.speed);
                if (spec.back)
                    ref.OutAndBack(spec.pause);
            }
            if (spec.follow)
                ref.FollowFacing();
            ApplyCommon(ref, spec, true, s == "line");
            if (s == "line")
                park(ref);
            decoy(ref);
            dropSpot(ref);
            keep(ref);
        }
        else if (s == "mine")
        {
            MechanicRef ref = Mechanic::Mine(owner, here, spec.size, spec.warn, spec.grace);
            if (spec.cap)
                ref.Cap(spec.capGroup, spec.cap);
            if (spec.life > Milliseconds(0))
                ref.Lifetime(spec.life);
            ApplyCommon(ref, spec, true, false);
            keep(ref);
        }
        else if (s == "lanes")
        {
            float const a = spec.facing;
            float const c = std::cos(a);
            float const n = std::sin(a);
            // Frame centred on the anchor point, X along the GM's facing; origin = a corner.
            Position const origin(here.GetPositionX() - c * spec.frameX / 2.f + n * spec.frameY / 2.f,
                here.GetPositionY() - n * spec.frameX / 2.f - c * spec.frameY / 2.f, here.GetPositionZ(), a);
            BossMechanics::RoomFrame const frame{ origin, a, spec.frameX, spec.frameY };
            Axis const other = spec.axis == Axis::X ? Axis::Y : Axis::X;
            std::vector<uint32> cells;
            {
                LaneBuilder lanes = Mechanic::RoomFrame(owner, frame);
                if (spec.safe)
                {
                    lanes.ExceptOne(spec.size, spec.warn, spec.axis);
                    if (spec.decoy)
                        lanes.DecoyFree();
                }
                else
                    lanes.Lanes(spec.axis, { spec.count, spec.size, spec.gap }, spec.warn);
                if (spec.cross)
                    lanes.Lanes(other, { spec.count, spec.size, spec.gap }, spec.warn);
                lanes.Each([&spec](MechanicRef& ref) { ApplyCommon(ref, spec, true, true); });
                if (spec.cross)
                {
                    lanes.BurnCrossings(spec.burn, spec.tick, spec.pct);
                    if (Registry const* reg = Registry::Get(owner->GetMap()))
                        if (LaneSet const* set = reg->FindLaneSet(lanes.Id()))
                            cells = set->cells;
                    for (uint32 id : cells)
                    {
                        MechanicRef cell(owner->GetMap(), id, false);
                        ApplyCommon(cell, spec, true, false);
                    }
                }
                for (MechanicHandle const& h : lanes.Handles())
                    ids.push_back(h.id);
            }   // the builder commits every lane and cell here
        }
        else if (s == "spread")
        {
            MechanicRef ref = Mechanic::Spread(owner, spec.size, spec.warn);
            if (spec.spreadCount)
                ref.Count(spec.spreadCount);
            if (spec.selfAura)
                ref.SelfTrigger(spec.selfAura, spec.selfMin);
            ApplyCommon(ref, spec, true, false);
            keep(ref);
        }
        else if (s == "runout")
        {
            // count= and self= pick their carriers in the registry; otherwise the target is the selected unit.
            Unit* target = nullptr;
            if (!spec.runoutCount && !spec.selfAura)
            {
                target = ObjectAccessor::GetUnit(*owner, spec.target);
                if (!target)
                {
                    err = "the runout target is gone";
                    return ids;
                }
            }
            MechanicRef ref = Mechanic::RunOut(owner, target, spec.size, spec.warn);
            if (spec.fromOwner)
                ref.From(MeasuredFrom::Owner);
            if (spec.runoutCount)
                ref.Count(spec.runoutCount);
            if (spec.selfAura)
                ref.SelfTrigger(spec.selfAura, spec.selfMin);
            ApplyCommon(ref, spec, true, false);
            keep(ref);
        }
        else if (s == "stack")
        {
            MechanicRef ref = spec.lane ? Mechanic::Stack(owner, laneSpec(), spec.warn)
                : Mechanic::Stack(owner, anchor, spec.size, spec.warn);
            if (spec.cluster)
                ref.Cluster(spec.cluster);
            ApplyCommon(ref, spec, false, true);
            keep(ref);
        }
        else if (s == "soak")
        {
            MechanicRef ref = spec.lane ? Mechanic::Soak(owner, laneSpec(), spec.need, spec.warn)
                : Mechanic::Soak(owner, anchor, spec.size, spec.need, spec.warn);
            ApplyCommon(ref, spec, false, true);
            keep(ref);
        }
        else if (s == "bait")
        {
            // Measured from the test owner (HandleTest moves it to the anchor point first).
            MechanicRef ref = Mechanic::Bait(owner, spec.bait, spec.size, spec.warn);
            ref.Radius(spec.baitRadius);
            ApplyCommon(ref, spec, true, false);
            keep(ref);
        }
        else if (s == "holdstill")
        {
            Milliseconds const window(static_cast<int64>(std::lround(spec.size * 1000.f)));
            MechanicRef ref = Mechanic::HoldStill(owner, GuidSet(), window, spec.warn);
            if (spec.tolerance >= 0.f)
                ref.Tolerance(spec.tolerance);
            ApplyCommon(ref, spec, false, false);
            keep(ref);
        }
        else if (s == "spot")
        {
            if (spec.spots.empty())
            {
                err = kNoMemberMessage;
                return ids;
            }
            MechanicRef ref = Mechanic::Spot(owner, spec.spots, spec.warn, spec.size);
            ApplyCommon(ref, spec, false, false);
            keep(ref);
        }
        if (ids.empty() && err.empty())
            err = "the registry refused it (see the server log: size, MaxActivePerMap or BossMechanics.Enable)";
        return ids;
    }

    // ---- landing on group members ----------------------------------------------------------------------------------
    // The alive members of the GM's group on the owner's map, in guid order (a seeded pick stays reproducible). The
    // GM is included only with `self=1`; other GMs never are (they are never valid targets).
    std::vector<Player*> GroupMembers(Creature* owner, ObjectGuid gmGuid, bool includeGm)
    {
        std::vector<Player*> out;
        Player* gm = ObjectAccessor::FindConnectedPlayer(gmGuid);
        if (!gm || !gm->IsInWorld())
            return out;
        Map* map = owner->GetMap();
        auto usable = [map](Player* p)
        {
            return p && p->IsInWorld() && p->GetMap() == map && p->IsAlive();
        };
        if (includeGm && usable(gm))
            out.push_back(gm);
        if (Group* group = gm->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* p = ref->GetSource();
                if (p && p != gm && usable(p) && !p->IsGameMaster())
                    out.push_back(p);
            }
        std::sort(out.begin(), out.end(), [](Player const* a, Player const* b)
        {
            return a->GetGUID() < b->GetGUID();
        });
        return out;
    }

    // A random member (pick=any), one other than the selected unit's victim (pick=nontank), or the member the last pick
    // of this command landed on while it is still usable (pick=same; else one other than the victim, as the steps
    // that follow a pick=nontank one expect: STAGE-M3-R-REPORT n-9), or a non-tank the chain has not picked
    // (pick=other, Drill::PickPool). `source` (with min=): the point the min= distance is measured from. Remembered
    // for pick=same and the chain.
    Player* PickMember(Creature* owner, TestSpec const& spec, Dice& dice, Position const* source = nullptr)
    {
        std::vector<Player*> const members = GroupMembers(owner, spec.gm, spec.includeGm);
        if (spec.pick == TestSpec::Pick::Same && !dice.lastMember.IsEmpty())
            for (Player* p : members)
                if (p->GetGUID() == dice.lastMember)
                    return p;
        Unit const* victim = nullptr;
        if (spec.pick != TestSpec::Pick::Any)
        {
            Unit* selected = spec.selected.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*owner, spec.selected);
            victim = selected ? selected->GetVictim() : nullptr;
        }
        std::vector<Drill::PickCandidate> candidates;
        candidates.reserve(members.size());
        for (Player const* p : members)
        {
            Drill::PickCandidate c;
            c.victim = p == victim;
            c.chained = std::find(dice.chain.begin(), dice.chain.end(), p->GetGUID()) != dice.chain.end();
            c.last = p->GetGUID() == dice.lastMember;
            c.dist = source ? p->GetExactDist2d(source) : 0.f;
            candidates.push_back(c);
        }
        Drill::PickRule const rule = spec.pick == TestSpec::Pick::Any ? Drill::PickRule::Any
            : spec.pick == TestSpec::Pick::Other ? Drill::PickRule::Other : Drill::PickRule::NonTank;
        std::vector<std::size_t> const pool = Drill::PickPool(candidates, rule, source ? spec.minDist : 0.f);
        if (pool.empty())
            return nullptr;
        Player* p = members[pool[dice.rng.Index(static_cast<uint32>(pool.size()))]];
        dice.lastMember = p->GetGUID();
        if (spec.pick != TestSpec::Pick::Other)
            dice.chain.clear();
        dice.chain.push_back(p->GetGUID());
        return p;
    }

    // The floor under (x, y) near z, else z (as `.mech minefield` places its mines).
    float GroundZ(Creature* owner, float x, float y, float z)
    {
        float const ground = owner->GetMap()->GetHeight(owner->GetPhaseMask(), x, y, z + 2.f, true, 10.f);
        return ground > INVALID_HEIGHT && std::fabs(ground - z) <= 5.f ? ground : z;
    }

    // ---- the chase (`hunt`, `soak bell=`; MechanicDrill.h, Shadowfang Keep build §3.9) ------------------------------
    // The chaser lingers this long after its chase or its landing, then despawns.
    constexpr Milliseconds kChaserLinger = Milliseconds(1000);

    bool HuntLive(HuntState const& h)
    {
        return h.live && GameTime::GetGameTimeMS() < h.endsAt;
    }

    // Ends a hunt: its circle gets no more passes, the chaser stops and despawns after `linger`.
    void EndHunt(Map* map, HuntState& h, Milliseconds linger)
    {
        if (!h.live)
            return;
        h.live = false;
        if (Registry* reg = Registry::Get(map))
            reg->Cancel(h.zone);
        if (Creature* chaser = map->GetCreature(h.chaser))
        {
            chaser->GetMotionMaster()->Clear();
            chaser->StopMoving();
            chaser->DespawnOrUnsummon(linger);
        }
    }

    // `hunt`: a chaser (a Mechanic Marker, not selectable or attackable) at the selected unit fixates one member
    // (pick=); a circle of `size` on it, eligible = that member only, persistent for the chase. The circle's first pass
    // (at the end of the warning, the howl) starts the walk and every pass re-aims it at the member, at `huntRate` x
    // player run speed; a pass that holds the member is a catch (its hit is the ledger entry) and ends the hunt, as
    // does the member dying or leaving the map. The chaser despawns on its own after the chase (timed summon).
    std::vector<uint32> RegisterHunt(Creature* owner, TestSpec spec, Dice& dice, std::string& err)
    {
        Unit* start = ObjectAccessor::GetUnit(*owner, spec.selected);
        if (!start || !start->IsInWorld() || start->GetMap() != owner->GetMap())
        {
            err = "the hunt's start unit (the selected creature) is gone";
            return {};
        }
        Player* hunted = PickMember(owner, spec, dice);
        if (!hunted)
        {
            err = kNoMemberMessage;
            return {};
        }
        Map* map = owner->GetMap();
        Milliseconds const life = spec.warn + spec.persist + kChaserLinger;
        TempSummon* chaser = owner->SummonCreature(Ids::NPC_MECHANIC_MARKER, start->GetPositionX(),
            start->GetPositionY(), start->GetPositionZ(), start->GetOrientation(), TEMPSUMMON_TIMED_DESPAWN,
            static_cast<uint32>(life.count()));
        if (!chaser)
        {
            err = Acore::StringFormat("could not summon the chaser ({})", Ids::NPC_MECHANIC_MARKER);
            return {};
        }
        chaser->SetReactState(REACT_PASSIVE);
        chaser->SetLevel(owner->GetLevel(), false);
        if (chaser->GetPhaseMask() != owner->GetPhaseMask())
            chaser->SetPhaseMask(owner->GetPhaseMask(), true);
        // The run speed the bots read for the circle's drift (MechanicBotView AnchorDriftSpeed) and the walk uses.
        chaser->SetSpeed(MOVE_RUN, spec.huntRate, true);

        HuntPtr const h = std::make_shared<HuntState>();
        h->chaser = chaser->GetGUID();
        h->hunted = hunted->GetGUID();
        h->endsAt = GameTime::GetGameTimeMS() + spec.warn + spec.persist;
        dice.hunt = h;

        TestSpec circle = spec;
        circle.shape = "circle";
        circle.unitAnchor = true;
        circle.unit = chaser->GetGUID();
        circle.target = hunted->GetGUID();
        Eligibility elig = spec.eligibility.value_or(Eligibility());
        elig.carriers = { hunted->GetGUID() };
        circle.eligibility = elig;
        std::vector<uint32> ids = RegisterTest(owner, circle, err);
        if (ids.empty())
        {
            chaser->DespawnOrUnsummon();
            return ids;
        }
        h->zone = ids.front();
        h->live = true;
        float const speed = Drill::HuntSpeedYdS(spec.huntRate);
        MechanicRef(map, h->zone, false).OnResolve([map, h, speed](ResolveResult const& r)
        {
            if (r.fizzled || !h->live)
                return;
            Creature* chaser = map->GetCreature(h->chaser);
            Player* member = chaser ? ObjectAccessor::GetPlayer(*chaser, h->hunted) : nullptr;
            if (!r.hits.empty() || !chaser || !member || !member->IsAlive() || member->GetMap() != map)
            {
                EndHunt(map, *h, kChaserLinger);
                return;
            }
            chaser->GetMotionMaster()->MovePoint(0, member->GetPosition(), FORCED_MOVEMENT_NONE, speed);
        });
        return ids;
    }

    // The landing at a rung bell: a circle of `landRadius` with the bell's `jump` as warning and its damage keys, one
    // for everyone but the ringers (BellLandingTag) and one, without a marker of its own, for the ringers
    // (BellRingerTag), so the ledger tells a ringer who did not step clear from anyone else caught in it.
    void LandAtBell(Creature* owner, TestSpec const& bellSpec, Position const& bell,
        std::vector<ObjectGuid> const& ringers)
    {
        TestSpec land = bellSpec;
        land.shape = "circle";
        land.size = bellSpec.landRadius;
        land.warn = bellSpec.jump;
        land.unitAnchor = false;
        land.point = bell;
        land.persist = Milliseconds(0);
        land.lane = false;
        land.ordering.reset();
        land.eligibility.reset();
        GuidSet const ringerSet(ringers.begin(), ringers.end());
        Eligibility others;
        for (Player const* p : GroupMembers(owner, bellSpec.gm, bellSpec.includeGm))
            if (!ringerSet.count(p->GetGUID()))
                others.carriers.insert(p->GetGUID());
        std::string err;
        if (!others.carriers.empty())   // empty carriers would mean everyone
        {
            TestSpec one = land;
            one.eligibility = others;
            one.tag = Drill::BellLandingTag(bellSpec.tag);
            RegisterTest(owner, one, err);
        }
        if (!ringerSet.empty())
        {
            TestSpec one = land;
            Eligibility e;
            e.carriers = ringerSet;
            one.eligibility = e;
            one.tag = Drill::BellRingerTag(bellSpec.tag);
            one.tele = false;           // the same circle as the one above
            RegisterTest(owner, one, err);
        }
    }

    // `soak bell=<n>`: the soak on one of n bell points (`bellApart` yd apart on a ring around the selected unit, the
    // first at the parse facing, so they stay put for a drill run). The member of the command's live hunt is not
    // eligible. Filled at its resolve (`need` bodies), it ends that hunt (the chaser bounds to the bell) and lands
    // there `jump` ms later (LandAtBell). A hunt that ended first (a catch) leaves the bell rung for nothing; with no
    // hunt at all (`.mech test soak bell=3` alone) the landing still follows a fill.
    std::vector<uint32> RegisterBell(Creature* owner, TestSpec spec, Dice& dice, std::string& err)
    {
        Position center = spec.point;
        if (Unit* selected = ObjectAccessor::GetUnit(*owner, spec.selected))
            center = selected->GetPosition();
        std::vector<Geo::Vec3> const bells = Drill::BellPoints({ center.GetPositionX(), center.GetPositionY(),
            center.GetPositionZ() }, spec.facing, spec.bells, spec.bellApart);
        Geo::Vec3 const b = bells[dice.rng.Index(static_cast<uint32>(bells.size()))];
        Position const bell(b.x, b.y, GroundZ(owner, b.x, b.y, b.z), spec.facing);

        HuntPtr const hunt = dice.hunt && HuntLive(*dice.hunt) ? dice.hunt : nullptr;
        GuidSet carriers;
        for (Player const* p : GroupMembers(owner, spec.gm, spec.includeGm))
            if (!hunt || p->GetGUID() != hunt->hunted)
                carriers.insert(p->GetGUID());
        if (carriers.empty())
        {
            err = kNoMemberMessage;
            return {};
        }

        TestSpec soak = spec;
        soak.unitAnchor = false;
        soak.point = bell;
        soak.weak = false;              // the landing carries Weakened, never the soak
        soak.once = false;
        Eligibility elig = spec.eligibility.value_or(Eligibility());
        elig.carriers = carriers;
        soak.eligibility = elig;
        std::vector<uint32> ids = RegisterTest(owner, soak, err);
        if (ids.empty())
            return ids;

        Map* map = owner->GetMap();
        ObjectGuid const ownerGuid = owner->GetGUID();
        MechanicRef(map, ids.front(), false).OnResolve([map, ownerGuid, spec, bell, hunt](ResolveResult const& r)
        {
            if (r.fizzled || r.count < spec.need)
                return;                 // not rung: the MissedSoak is in the ledger
            if (hunt && !HuntLive(*hunt))
                return;                 // caught first (or the chase is over): he went back to the tank
            Creature* testOwner = map->GetCreature(ownerGuid);
            if (!testOwner)
                return;
            if (hunt)
            {
                Creature* chaser = map->GetCreature(hunt->chaser);
                EndHunt(map, *hunt, spec.jump + kChaserLinger);
                if (chaser && chaser->IsInWorld())
                {
                    float const secs = std::max(static_cast<float>(spec.jump.count()) / 1000.f, 0.1f);
                    float const dist = chaser->GetExactDist2d(&bell);
                    chaser->GetMotionMaster()->MoveJump(bell, std::max(dist / secs, 1.f), 10.f);
                }
            }
            LandAtBell(testOwner, spec, bell, r.hits);
        });
        return ids;
    }

    // `at=predict` (Scholomance build §3.9, IB1): where `p` will stand `lead` from now at its heading and speed, read
    // from its movement keys (a bot's spline move sets forward and faces along the path), snapped to the floor; where
    // it stands while it does not move (MechanicDrill.h MoveHeading, PredictSpot).
    Position PredictedSpot(Creature* owner, Player const* p, Milliseconds lead, float facing)
    {
        uint32 const flags = p->GetUnitMovementFlags();
        bool const forward = (flags & MOVEMENTFLAG_FORWARD) != 0;
        bool const back = (flags & MOVEMENTFLAG_BACKWARD) != 0;
        std::optional<float> const heading = Drill::MoveHeading(p->GetOrientation(), forward, back,
            (flags & MOVEMENTFLAG_STRAFE_LEFT) != 0, (flags & MOVEMENTFLAG_STRAFE_RIGHT) != 0);
        UnitMoveType const type = (flags & MOVEMENTFLAG_WALKING) ? MOVE_WALK : back && !forward ? MOVE_RUN_BACK
            : MOVE_RUN;
        Geo::Vec3 const at = Drill::PredictSpot({ p->GetPositionX(), p->GetPositionY(), p->GetPositionZ() },
            heading, p->GetSpeed(type), static_cast<uint32>(std::max<int64>(lead.count(), 0)));
        return Position(at.x, at.y, GroundZ(owner, at.x, at.y, at.z), facing);
    }

    // `grid=<n>` (Scholomance build §3.9, JB1 heroic): n x n circles around the anchor point, `decoys` of them a
    // Decoy() picked by the dice. The grid's centre is the next `at=prev`.
    std::vector<uint32> RegisterGrid(Creature* owner, TestSpec const& spec, Dice& dice, std::string& err)
    {
        std::vector<Geo::Vec3> const points = Drill::GridPoints({ spec.point.GetPositionX(),
            spec.point.GetPositionY(), spec.point.GetPositionZ() }, spec.facing, spec.grid, spec.gridApart);
        std::vector<uint32> const decoys = Drill::PickDistinct(static_cast<uint32>(points.size()), spec.decoys,
            [&dice](uint32 n) { return dice.rng.Index(n); });
        std::vector<uint32> ids;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            TestSpec one = spec;
            one.grid = 0;
            one.point = Position(points[i].x, points[i].y, GroundZ(owner, points[i].x, points[i].y, points[i].z),
                spec.facing);
            one.decoy = std::find(decoys.begin(), decoys.end(), static_cast<uint32>(i)) != decoys.end();
            std::vector<uint32> const made = RegisterTest(owner, one, err);
            ids.insert(ids.end(), made.begin(), made.end());
        }
        dice.lastCircle = spec.point;
        return ids;
    }

    // One repeat of `.mech test`: resolves the landing (at=member|group|gm|last|each|prev|predict, anchor=member, to=,
    // away=, shift=, scatter=, fix=) and the spot ring now, so a delayed repeat follows the bots, then registers.
    // `hunt` and `soak bell=` place themselves (RegisterHunt, RegisterBell); `grid=` lays its circles (RegisterGrid).
    std::vector<uint32> RegisterRepeat(Creature* owner, TestSpec spec, Dice& dice, std::string& err)
    {
        if (spec.shape == "hunt")
            return RegisterHunt(owner, spec, dice, err);
        if (spec.shape == "soak" && spec.bells)
            return RegisterBell(owner, spec, dice, err);
        Barrage::Rng& rng = dice.rng;
        if (spec.atEach)
        {
            // One shape under every member, each where it stands now.
            std::vector<Player*> const members = GroupMembers(owner, spec.gm, spec.includeGm);
            if (members.empty())
            {
                err = kNoMemberMessage;
                return {};
            }
            std::vector<uint32> ids;
            for (Player const* member : members)
            {
                TestSpec one = spec;
                one.atEach = false;
                one.point = member->GetPosition();
                one.point.SetOrientation(spec.facing);
                one.target = member->GetGUID();
                std::vector<uint32> const made = RegisterRepeat(owner, one, dice, err);
                ids.insert(ids.end(), made.begin(), made.end());
            }
            return ids;
        }
        if (spec.atLast)
        {
            // The end of the last fixed line; none yet: where the selected unit stands now.
            if (dice.lastEnd)
                spec.point = *dice.lastEnd;
            else if (Unit* selected = ObjectAccessor::GetUnit(*owner, spec.selected))
                spec.point = selected->GetPosition();
            spec.point.SetOrientation(spec.facing);
        }
        if (spec.atPrev)
        {
            // The centre of the last point-anchored circle; none yet: where the selected unit stands now.
            if (dice.lastCircle)
                spec.point = *dice.lastCircle;
            else if (Unit* selected = ObjectAccessor::GetUnit(*owner, spec.selected))
                spec.point = selected->GetPosition();
            spec.point.SetOrientation(spec.facing);
        }
        if (spec.atMember)
        {
            Player* member = PickMember(owner, spec, dice);
            if (!member)
            {
                err = kNoMemberMessage;
                return {};
            }
            spec.point = member->GetPosition();
            spec.point.SetOrientation(spec.facing);
            spec.target = member->GetGUID();
        }
        else if (spec.atGroup)
        {
            std::vector<Geo::Vec3> points;
            for (Player const* p : GroupMembers(owner, spec.gm, spec.includeGm))
                points.push_back({ p->GetPositionX(), p->GetPositionY(), p->GetPositionZ() });
            if (points.empty())
            {
                err = kNoMemberMessage;
                return {};
            }
            Geo::Vec3 const c = Drill::Centroid(points);
            spec.point = Position(c.x, c.y, GroundZ(owner, c.x, c.y, c.z), spec.facing);
        }
        else if (spec.atGm)
        {
            Player* gm = ObjectAccessor::FindConnectedPlayer(spec.gm);
            if (!gm || !gm->IsInWorld() || gm->GetMap() != owner->GetMap())
            {
                err = "you are not on the test owner's map";
                return {};
            }
            spec.point = gm->GetPosition();
            spec.point.SetOrientation(spec.facing);
        }
        else if (spec.atPredict)
        {
            Player* member = PickMember(owner, spec, dice);
            if (!member)
            {
                err = kNoMemberMessage;
                return {};
            }
            spec.point = PredictedSpot(owner, member, spec.lead, spec.facing);
            spec.target = member->GetGUID();
        }
        if (spec.away > 0.f && !spec.unitAnchor)
        {
            // Exactly `away` yd from the landing at a random bearing (before a line's source is read: GD4's portal).
            Geo::Vec3 const p = Drill::Away({ spec.point.GetPositionX(), spec.point.GetPositionY(),
                spec.point.GetPositionZ() }, rng.Float(0.f, Geo::kTwoPi), spec.away);
            spec.point.Relocate(p.x, p.y, GroundZ(owner, p.x, p.y, p.z));
        }
        if (spec.anchorMember)
        {
            Player* member = PickMember(owner, spec, dice);
            if (!member)
            {
                err = kNoMemberMessage;
                return {};
            }
            spec.unitAnchor = true;
            spec.unit = member->GetGUID();
            spec.target = member->GetGUID();
        }
        // Where a line starts now (min= measures from it, fix= freezes it there).
        Position source = spec.point;
        if (spec.unitAnchor)
            if (Unit* u = ObjectAccessor::GetUnit(*owner, spec.unit))
                source = u->GetPosition();
        switch (spec.to)
        {
            case TestSpec::To::Member:
            {
                Player* member = PickMember(owner, spec, dice, spec.minDist > 0.f ? &source : nullptr);
                if (!member)
                {
                    err = kNoMemberMessage;
                    return {};
                }
                spec.toUnit = member->GetGUID();
                break;
            }
            case TestSpec::To::Target:
                spec.toUnit = spec.selected;
                break;
            case TestSpec::To::Me:
                spec.toUnit = spec.gm;
                break;
            case TestSpec::To::None:
                break;
        }
        if (spec.exemptTo && !spec.toUnit.IsEmpty())
        {
            // exempt=target: every scope member but the aimed one is eligible (the GM too, as for any zone), so the
            // aimed player leads the shape and is never hit by it; nobody left keeps a set no player is in (an empty
            // carrier set would mean everyone).
            std::vector<ObjectGuid> members;
            for (Player const* p : GroupMembers(owner, spec.gm, true))
                members.push_back(p->GetGUID());
            std::vector<ObjectGuid> const rest = Drill::AllBut(members, spec.toUnit);
            Eligibility elig = spec.eligibility.value_or(Eligibility());
            elig.carriers = GuidSet(rest.begin(), rest.end());
            if (elig.carriers.empty())
                elig.carriers.insert(ObjectGuid::Empty);
            spec.eligibility = elig;
        }
        if (spec.fix && spec.to != TestSpec::To::None)
        {
            // Frozen now: a point line from the source toward where the to= unit stands (reach: ending there).
            Unit* toUnit = ObjectAccessor::GetUnit(*owner, spec.toUnit);
            if (!toUnit)
            {
                err = "the to= unit is gone";
                return {};
            }
            Drill::FixedLine const line = Drill::FixLine({ source.GetPositionX(), source.GetPositionY(),
                source.GetPositionZ() }, { toUnit->GetPositionX(), toUnit->GetPositionY(), toUnit->GetPositionZ() },
                spec.size, spec.reach, spec.facing);
            spec.unitAnchor = false;
            spec.to = TestSpec::To::None;
            spec.facing = line.facing;
            spec.size = line.length;
            spec.point = Position(line.source.x, line.source.y, line.source.z, line.facing);
        }
        if (spec.shifted && !spec.unitAnchor)
        {
            Geo::Vec3 const p = Drill::Shifted({ spec.point.GetPositionX(), spec.point.GetPositionY(),
                spec.point.GetPositionZ() }, spec.facing, spec.shiftAhead, spec.shiftLeft);
            spec.point.Relocate(p.x, p.y, GroundZ(owner, p.x, p.y, p.z));
        }
        if (spec.scatter > 0.f && !spec.unitAnchor)
        {
            float const ang = rng.Float(0.f, 2.f * Geo::kPi);
            float const dist = spec.scatter * std::sqrt(rng.Float(0.f, 1.f));
            spec.point.Relocate(spec.point.GetPositionX() + dist * std::cos(ang),
                spec.point.GetPositionY() + dist * std::sin(ang), spec.point.GetPositionZ());
        }
        if (spec.shape == "spot")
        {
            // One spot per member (guid order) on a ring around the anchor point, rotated by the dice.
            Position center = spec.point;
            if (spec.unitAnchor)
                if (Unit* u = ObjectAccessor::GetUnit(*owner, spec.unit))
                    center = u->GetPosition();
            std::vector<Player*> const members = GroupMembers(owner, spec.gm, spec.includeGm);
            float const rotation = rng.Float(0.f, Geo::kTwoPi);
            std::vector<Geo::Vec3> const ring = Drill::RingSpots({ center.GetPositionX(), center.GetPositionY(),
                center.GetPositionZ() }, spec.ring, static_cast<uint32>(members.size()), rotation);
            spec.spots.clear();
            for (std::size_t i = 0; i < members.size(); ++i)
                spec.spots[members[i]->GetGUID()] = Position(ring[i].x, ring[i].y,
                    GroundZ(owner, ring[i].x, ring[i].y, ring[i].z), 0.f);
        }
        if (spec.grid)
            return RegisterGrid(owner, spec, dice, err);
        std::vector<uint32> ids = RegisterTest(owner, spec, err);
        // A point-anchored circle leaves its centre for the next `at=prev`.
        if (!ids.empty() && spec.shape == "circle" && !spec.unitAnchor)
            dice.lastCircle = spec.point;
        // A fixed line (a point source along a fixed facing) leaves its far end for the next `at=last`.
        if (!ids.empty() && (spec.shape == "line" || spec.shape == "projectile") && !spec.unitAnchor
            && spec.to == TestSpec::To::None)
        {
            Geo::Vec3 const end = Drill::LineEnd({ spec.point.GetPositionX(), spec.point.GetPositionY(),
                spec.point.GetPositionZ() }, spec.facing, spec.size);
            dice.lastEnd = Position(end.x, end.y, GroundZ(owner, end.x, end.y, end.z), spec.facing);
        }
        return ids;
    }

    // One `.mech barrage` shot: a random member, a random kind of the list, the shot's geometry around that member.
    void RegisterBarrageShot(Creature* owner, TestSpec spec, std::vector<Barrage::Kind> const& kinds,
        Dice& dice, std::string& err)
    {
        Barrage::Rng& rng = dice.rng;
        Player* member = PickMember(owner, spec, dice);
        if (!member)
        {
            err = kNoMemberMessage;
            return;
        }
        Barrage::Kind const kind = kinds[rng.Index(static_cast<uint32>(kinds.size()))];
        Barrage::Shot const shot = Barrage::MakeShot(kind, { member->GetPositionX(), member->GetPositionY(),
            member->GetPositionZ() }, rng);
        spec.atMember = false;
        spec.shape = Barrage::ShapeName(kind);
        spec.size = shot.size;
        spec.inner = shot.inner;
        spec.angle = shot.angleDeg;
        spec.width = shot.width;
        if (shot.speed > 0.f)
            spec.speed = shot.speed;
        spec.facing = shot.pose.o;
        spec.point = Position(shot.pose.x, shot.pose.y, shot.pose.z, shot.pose.o);
        spec.target = member->GetGUID();
        spec.tag = Barrage::TagOf(kind);
        RegisterTest(owner, spec, err);
    }

    // ---- the test owner --------------------------------------------------------------------------------------------
    std::vector<ObjectGuid> TestOwners(Map* map)
    {
        std::vector<ObjectGuid> out;
        if (Registry const* reg = Registry::Get(map))
            reg->ForEachEncounter([&out](Encounter const& e)
            {
                if (e.owner.GetEntry() == Ids::NPC_MECHANIC_TEST_OWNER)
                    out.push_back(e.owner);
            });
        return out;
    }

    Creature* FindTestOwner(Map* map)
    {
        for (ObjectGuid const& g : TestOwners(map))
            if (Creature* c = map->GetCreature(g))
                if (c->IsInWorld() && c->IsAlive())
                    return c;
        return nullptr;
    }

    Creature* GetOrSummonTestOwner(Player* gm)
    {
        Map* map = gm->GetMap();
        if (Creature* owner = FindTestOwner(map))
        {
            Registry::GetOrCreate(map).AddScopePlayer(owner->GetGUID(), gm->GetGUID());
            return owner;
        }
        TempSummon* owner = map->SummonCreature(Ids::NPC_MECHANIC_TEST_OWNER, gm->GetPosition());
        if (!owner)
            return nullptr;
        owner->SetReactState(REACT_PASSIVE);
        // The content key reads the owner's level unless BossMechanics.ReferenceHealth.MapLevels pins this map.
        owner->SetLevel(gm->GetLevel(), false);
        if (owner->GetPhaseMask() != gm->GetPhaseMask())
            owner->SetPhaseMask(gm->GetPhaseMask(), true);
        owner->SetDisableGravity(true);
        Mechanic::BeginEncounter(owner);
        Registry::GetOrCreate(map).AddScopePlayer(owner->GetGUID(), gm->GetGUID());
        return owner;
    }

    std::string DescribeShape(Shape const& s)
    {
        switch (s.type)
        {
            case ShapeType::Circle:
                return Acore::StringFormat("circle r={:.1f}", s.radius);
            case ShapeType::Donut:
                return Acore::StringFormat("donut {:.1f}-{:.1f}", s.inner, s.radius);
            case ShapeType::Cone:
                return Acore::StringFormat("cone {:.0f}deg len={:.1f} off={:.0f}", s.angleDeg, s.length,
                    s.facingOffsetDeg);
            case ShapeType::Line:
                return Acore::StringFormat("line len={:.1f} w={:.1f} excl={:.1f}", s.length, s.width, s.excludeStart);
        }
        return "?";
    }

    std::string PlayerName(ObjectGuid guid)
    {
        if (Player* p = ObjectAccessor::FindConnectedPlayer(guid))
            return p->GetName();
        return guid.ToString();
    }

    constexpr char const* kTestUsage = "Usage: .mech test <shape> <size> <ms> [key=value ...]; shapes: circle donut "
        "cone rearcone line projectile mine lanes spread runout stack soak frontal bait holdstill spot; "
        "rider=<spell> takes only the module's spells 205008-205099";

    // What a `.mech test` line is parsed against (M4): the reference unit stands in for the GM (`anchor=me`, the
    // "nothing selected" case, the facing of lanes, lines and `shift=`), `scope` is the scope player whose group the
    // member picks land on (`at=gm`, `to=me`). `.mech test` and `.mech drill`: the GM for all three; the training
    // dummy and the sim: their scope player.
    struct ParseCtx
    {
        Unit* reference = nullptr;
        ObjectGuid scope;
        float facing = 0.f;
    };

    ParseCtx CtxOf(Unit* reference, ObjectGuid scope)
    {
        return { reference, scope, reference->GetOrientation() };
    }

    // Parses `.mech test` arguments (`<shape> <size> <ms> [key=value ...]`) into `spec`, `repeat` and `every`;
    // `selected` is the selected unit (ctx.reference when nothing is selected). Returns the error to show (empty:
    // parsed). `opt` keeps which keys were read, for the ignored-keys echo. The drill runner parses its steps here too.
    std::string ParseTest(ParseCtx const& ctx, Unit* selected, std::vector<std::string_view> const& tokens,
        Options& opt, TestSpec& spec, uint32& repeat, Milliseconds& every)
    {
        Unit* const gm = ctx.reference;
        if (tokens.size() < 3)
            return kTestUsage;
        spec.shape = std::string(tokens[0]);
        Optional<float> size = Acore::StringTo<float>(tokens[1]);
        Optional<int32> ms = Acore::StringTo<int32>(tokens[2]);
        if (!IsShapeName(spec.shape) || !size || *size <= 0.f || !ms || *ms < 0)
            return Acore::StringFormat("{} (size > 0, ms >= 0)", kTestUsage);
        spec.size = *size;
        spec.warn = Milliseconds(*ms);
        for (std::size_t i = 3; i < tokens.size(); ++i)
            if (!opt.Add(tokens[i]))
                return Acore::StringFormat("Not a key=value option: '{}'", tokens[i]);

        std::string const& s = spec.shape;
        // shape keys (read only for their shape, so a stray key shows up as unused)
        if (s == "donut")
            spec.inner = opt.Float("inner", spec.size / 2.f);
        if (s == "cone" || s == "rearcone" || s == "frontal")
            spec.angle = opt.Float("angle", 90.f);
        if (s == "cone" || s == "frontal")
            spec.offset = opt.Float("offset", 0.f);
        // to=member|target|me: a line (or, Stratholme BZ1, a cone) aims at that unit live from its anchor.
        auto readTo = [&opt, &spec]()
        {
            std::string const to = opt.Str("to", "");
            if (to == "member")
                spec.to = TestSpec::To::Member;
            else if (to == "target")
                spec.to = TestSpec::To::Target;
            else if (to == "me")
                spec.to = TestSpec::To::Me;
            else if (!to.empty())
                opt.MarkBad("to");
        };
        if (s == "cone")
            readTo();
        if (s == "line" || s == "projectile")
        {
            spec.width = opt.Float("width", s == "projectile" ? 3.f : 4.f);
            spec.exclude = opt.Float("exclude", 0.f);
            readTo();
            spec.reach = opt.Bool("reach", false);
            spec.fix = spec.reach || opt.Bool("fix", false);
            spec.minDist = opt.Float("min", 0.f);
            if (spec.minDist < 0.f)
                opt.MarkBad("min");
        }
        if (s == "projectile")
        {
            spec.speed = opt.Float("speed", 10.f);
            spec.back = opt.Bool("back", false);
            spec.pause = opt.Ms("pause", Milliseconds(0));
        }
        if (s == "mine")
        {
            spec.grace = opt.Ms("grace", Milliseconds(500));
            spec.cap = static_cast<uint8>(std::clamp<int32>(opt.Int("cap", 0), 0, 255));
            spec.capGroup = static_cast<uint32>(std::max<int32>(opt.Int("group", 0), 0));
            spec.life = opt.Ms("life", Milliseconds(0));
        }
        if (s == "lanes")
        {
            spec.count = static_cast<uint32>(std::clamp<int32>(opt.Int("count", 3), 1, 32));
            spec.gap = opt.Float("gap", 6.f);
            std::string const axis = opt.Str("axis", "x");
            if (axis == "y")
                spec.axis = Axis::Y;
            else if (axis != "x")
                opt.MarkBad("axis");
            std::string const frame = opt.Str("frame", "40x40");
            std::vector<std::string_view> const wh = Acore::Tokenize(frame, 'x', false);
            Optional<float> fx = wh.size() == 2 ? Acore::StringTo<float>(wh[0]) : std::nullopt;
            Optional<float> fy = wh.size() == 2 ? Acore::StringTo<float>(wh[1]) : std::nullopt;
            if (fx && fy && *fx > 0.f && *fy > 0.f)
            {
                spec.frameX = *fx;
                spec.frameY = *fy;
            }
            else
                opt.MarkBad("frame");
            spec.safe = opt.Bool("safe", false);
            spec.cross = opt.Bool("cross", false);
            if (spec.cross)
                spec.burn = opt.Ms("burn", kDefaultBurn);
        }
        if (s == "spread")
            spec.spreadCount = static_cast<uint8>(std::clamp<int32>(opt.Int("count", 0), 0, 40));
        if (s == "runout")
        {
            spec.fromOwner = opt.Str("from", "group") == "owner";
            spec.runoutCount = static_cast<uint8>(std::clamp<int32>(opt.Int("count", 0), 0, 40));
        }
        if (s == "soak")
        {
            spec.need = static_cast<uint8>(std::clamp<int32>(opt.Int("need", 2), 0, 40));
            // bell=<n>: the soak is an alarm bell on one of n points; filled, it ends the hunt (MechanicDrill.h).
            int32 const bells = opt.Int("bell", 0);
            if (bells != 0 && (bells < static_cast<int32>(Drill::kMinBells) || bells > static_cast<int32>(
                Drill::kMaxBells)))
                opt.MarkBad("bell");
            else if (bells)
            {
                spec.bells = static_cast<uint32>(bells);
                spec.bellApart = opt.Float("apart", 15.f);
                spec.landRadius = opt.Float("land", 6.f);
                spec.jump = opt.Ms("jump", Milliseconds(2000));
                if (spec.bellApart <= 0.f)
                    opt.MarkBad("apart");
                if (spec.landRadius <= 0.f)
                    opt.MarkBad("land");
            }
        }
        if (s == "hunt")
        {
            spec.huntRate = opt.Float("speed", Drill::kHuntRateNormal);
            if (spec.huntRate <= 0.f || spec.huntRate > Drill::kMaxHuntRate)
                opt.MarkBad("speed");
        }
        if (s == "stack" || s == "soak")
        {
            std::string const shape = opt.Str("shape", "circle");
            if (shape == "line")
            {
                spec.lane = true;
                spec.laneLength = opt.Float("length", 30.f);
                spec.width = opt.Float("width", spec.size);
                if (spec.laneLength <= 0.f)
                    opt.MarkBad("length");
                if (spec.width <= 0.f)
                    opt.MarkBad("width");
            }
            else if (shape != "circle")
                opt.MarkBad("shape");
        }
        if (s == "stack")
            spec.cluster = static_cast<uint8>(std::clamp<int32>(opt.Int("cluster", 0), 0, 40));
        if (s == "frontal")
        {
            spec.rear = opt.Bool("rear", false);
            spec.lineFrontal = opt.Bool("line", false);
            if (spec.lineFrontal)
                spec.width = opt.Float("width", 4.f);
        }
        if (s == "bait")
        {
            // aura=<spell> is the rule's aura; aura=<spell>:<min>[:<max>] is the eligibility band (below).
            uint32 ruleAura = Ids::SPELL_WEAKENED;
            if (opt.Has("aura") && opt.Str("aura", "").find(':') == std::string::npos)
            {
                Optional<uint32> a = Acore::StringTo<uint32>(opt.Str("aura", ""));
                if (a && *a)
                    ruleAura = *a;
                else
                    opt.MarkBad("aura");
            }
            std::string const rule = opt.Str("rule", "fewest");
            if (rule == "fewest")
                spec.bait = BaitRule::FewestStacks(ruleAura);
            else if (rule == "farthest")
                spec.bait = BaitRule::Farthest();
            else
                opt.MarkBad("rule");
            spec.baitRadius = opt.Float("r", 6.f);
            if (spec.baitRadius <= 0.f)
                opt.MarkBad("r");
        }
        if (s == "holdstill" && opt.Has("tol"))
        {
            spec.tolerance = opt.Float("tol", Rules::kHoldStillToleranceYd);
            if (spec.tolerance < 0.f)
                opt.MarkBad("tol");
        }
        if (s == "spot")
        {
            spec.ring = opt.Float("ring", 10.f);
            if (spec.ring < 0.f)
                opt.MarkBad("ring");
        }
        if (s == "circle" || s == "donut" || s == "cone" || s == "rearcone" || s == "line")
            spec.park = opt.Bool("park", false);
        // decoy=1 (Scholomance WP-X): drawn, hits nobody, not in the bot view (lanes: the safe=1 lane drawn as one).
        if (s == "circle" || s == "donut" || s == "cone" || s == "rearcone" || s == "frontal" || s == "line"
            || s == "projectile" || s == "lanes")
            spec.decoy = opt.Bool("decoy", false);
        // drop=<edge,away,behind,prefer> (Stratholme X2): where the member carrying this shape takes it (circle
        // anchor=member, line to=member); room=<yd> around the selected unit (0: no room), dwell=<ms>.
        if (opt.Has("drop"))
        {
            if (s != "circle" && s != "line")
                opt.MarkBad("drop");
            else if (std::optional<DropSpot::Flags> f = DropSpot::ParseFlags(opt.Str("drop", "")))
                spec.drop = f;
            else
                opt.MarkBad("drop");
            spec.dropRoom = opt.Float("room", 30.f);
            if (spec.dropRoom < 0.f)
                opt.MarkBad("room");
            spec.dropDwell = opt.Ms("dwell", Milliseconds(1200));
        }
        if (s == "circle" && opt.Has("grid"))
        {
            int32 const grid = opt.Int("grid", 0);
            if (grid < 2 || grid > static_cast<int32>(Drill::kMaxGrid))
                opt.MarkBad("grid");
            else
            {
                spec.grid = static_cast<uint32>(grid);
                spec.gridApart = opt.Float("apart", spec.size * 1.375f);
                int32 const decoys = opt.Int("decoys", 0);
                if (decoys < 0 || decoys >= grid * grid)
                    opt.MarkBad("decoys");
                else
                    spec.decoys = static_cast<uint32>(decoys);
            }
        }
        if (s == "cone" || s == "rearcone" || s == "line" || s == "projectile")
            spec.follow = opt.Bool("follow", false);
        // exempt=target (Stratholme BZ1): the unit the shape aims at (to=) is spared; everyone else in scope is not.
        if ((s == "cone" || s == "line" || s == "projectile") && opt.Has("exempt"))
        {
            if (opt.Str("exempt", "") == "target")
                spec.exemptTo = true;
            else
                opt.MarkBad("exempt");
        }

        // eligibility, ordering, self-trigger
        Eligibility elig;
        bool hasElig = false;
        if (opt.Has("role"))
        {
            if (std::optional<Role> role = ParseRole(opt.Str("role", "")))
            {
                elig.roles = *role;
                hasElig = true;
            }
            else
                opt.MarkBad("role");
        }
        if (opt.Has("aura") && (s != "bait" || opt.Str("aura", "").find(':') != std::string::npos))
        {
            if (ParseAuraBand(opt.Str("aura", ""), elig))
                hasElig = true;
            else
                opt.MarkBad("aura");
        }
        if (hasElig)
            spec.eligibility = elig;
        if ((s == "soak" || s == "bait") && opt.Has("order"))
        {
            Ordering order;
            if (ParseOrdering(opt.Str("order", ""), order))
                spec.ordering = order;
            else
                opt.MarkBad("order");
        }
        if (opt.Has("self"))
        {
            // self=<spell>:<min>: self-trigger (spread, runout); self=0/1: the GM may be landed on.
            std::string const self = opt.Str("self", "");
            if (self.find(':') != std::string::npos)
            {
                if ((s != "spread" && s != "runout") || !ParseSelfTrigger(self, spec.selfAura, spec.selfMin))
                    opt.MarkBad("self");
            }
            else
                spec.includeGm = opt.Bool("self", false);
        }

        // common keys
        // A bell soak reads the damage keys for its landing (LandAtBell).
        bool const noDamage = s == "stack" || (s == "soak" && !spec.bells) || s == "holdstill" || s == "spot";
        if (!noDamage)
        {
            spec.dmg = opt.Int("dmg", -1);
            spec.pct = opt.Float("pct", 20.f);
            spec.tankPct = opt.Float("tankpct", -1.f);
            std::string const school = opt.Str("school", "physical");
            if (std::optional<uint8> sc = ParseSchool(school))
                spec.school = *sc;
            else
                opt.MarkBad("school");
            // Weakened lands only from resolve hits: the get-in kinds never cast one (key echoed as ignored)
            spec.weak = opt.Bool("weak", false);
            spec.once = opt.Bool("once", false);
            // rider=<spell>: cast on every player hit, the Bait's job hit included (the drill's rotation debuff).
            // Only the module's block past Weakened and the resolve spells (205008-205099): a GM can neither cast
            // arbitrary spells through it nor hand out Weakened on a job hit.
            if (opt.Has("rider"))
            {
                Optional<uint32> rider = Acore::StringTo<uint32>(opt.Str("rider", ""));
                if (!rider || !Ids::RiderAllowed(*rider))
                    return Acore::StringFormat("rider= takes only a harmless drill aura: {} (Baited) or {} (Marked)",
                        Ids::SPELL_BAITED_DRILL, Ids::SPELL_MARKED_DRILL);
                if (!sSpellMgr->GetSpellInfo(*rider))
                    return Acore::StringFormat("rider={}: no such spell (spell_dbc row missing?)", *rider);
                spec.rider = *rider;
            }
        }
        spec.persist = opt.Ms("persist", Milliseconds(0));
        spec.tick = opt.Ms("tick", Milliseconds(1000));
        for (char const* key : { "color", "colour" })     // colour=: a silent alias
        {
            if (!opt.Has(key))
                continue;
            spec.color = ParseColor(opt.Str(key, ""));
            if (!spec.color)
                opt.MarkBad(key);
        }
        spec.tele = opt.Bool("tele", true);
        spec.tag = static_cast<uint32>(std::max<int32>(opt.Int("tag", 1), 0));
        repeat = static_cast<uint32>(std::clamp<int32>(opt.Int("repeat", 1), 1, kMaxRepeat));
        every = opt.Ms("every", Milliseconds(1000));
        spec.scatter = std::max(0.f, opt.Float("scatter", 0.f));
        spec.away = opt.Float("away", 0.f);
        if (spec.away < 0.f)
            opt.MarkBad("away");
        std::string const at = opt.Str("at", "");
        if (at == "member")
            spec.atMember = true;
        else if (at == "group")
            spec.atGroup = true;
        else if (at == "gm")
            spec.atGm = true;
        else if (at == "last")
            spec.atLast = true;
        else if (at == "each")
            spec.atEach = true;
        else if (at == "prev")
            spec.atPrev = true;
        else if (at == "predict")
        {
            spec.atPredict = true;
            spec.lead = opt.Ms("lead", Milliseconds(Drill::kDefaultPredictLeadMs));
        }
        else if (!at.empty())
            opt.MarkBad("at");
        std::string const pick = opt.Str("pick", "any");
        if (pick == "nontank")
            spec.pick = TestSpec::Pick::NonTank;
        else if (pick == "same")
            spec.pick = TestSpec::Pick::Same;
        else if (pick == "other")
            spec.pick = TestSpec::Pick::Other;
        else if (pick != "any")
            opt.MarkBad("pick");
        if (opt.Has("shift"))
        {
            spec.shifted = ParseShift(opt.Str("shift", ""), spec.shiftAhead, spec.shiftLeft);
            if (!spec.shifted)
                opt.MarkBad("shift");
        }

        // anchor
        spec.facing = ctx.facing;
        spec.gm = ctx.scope;
        spec.point = selected->GetPosition();
        spec.point.SetOrientation(spec.facing);
        spec.target = selected->GetGUID();
        spec.selected = selected->GetGUID();
        bool const atPoint = spec.atMember || spec.atGroup || spec.atGm || spec.atLast || spec.atEach || spec.atPrev
            || spec.atPredict;
        if (s == "frontal")
        {
            if (selected == gm || !selected->IsCreature())
                return "frontal needs a selected creature: the cone's source, whose victim is exempt";
            spec.unitAnchor = true;
            spec.unit = selected->GetGUID();
            spec.point = selected->GetPosition();
        }
        else
        {
            std::string const anchorKind = opt.Str("anchor", s == "stack" && selected != gm && !atPoint
                && !spec.cluster && !spec.lane ? "target" : "point");
            if (anchorKind == "me")
            {
                spec.unitAnchor = true;
                spec.unit = gm->GetGUID();
            }
            else if (anchorKind == "target")
            {
                if (selected == gm)
                    return "anchor=target needs a selected unit";
                spec.unitAnchor = true;
                spec.unit = selected->GetGUID();
            }
            else if (anchorKind == "member")
                spec.anchorMember = true;
            else if (anchorKind != "point")
                opt.MarkBad("anchor");
            if (atPoint && anchorKind != "point")
                return "at= lands on a point; leave anchor= out or use anchor=point";
        }
        if (spec.park && selected == gm)
            return "park=1 parks the selected unit: select it (the zone lands at it, or at you with at=gm)";
        if (spec.to == TestSpec::To::Target && selected == gm)
            return "to=target needs a selected unit";
        if (spec.follow && !spec.unitAnchor && !spec.anchorMember)
            return "follow=1 needs a unit anchor (anchor=me, target or member)";
        if (s == "cone" && spec.to != TestSpec::To::None && spec.follow)
            return "to= aims the cone at a unit: no follow=1";
        if (spec.exemptTo && spec.to == TestSpec::To::None)
            return "exempt=target spares the unit the shape aims at: add to=member, target or me";
        if (spec.shifted && (spec.unitAnchor || spec.anchorMember))
            return "shift= moves a point anchor; leave anchor= out";
        if (spec.cluster && spec.lane)
            return "cluster= needs a circle stack (no shape=line)";
        if (spec.fix && spec.to == TestSpec::To::None)
            return "fix=1 and reach=1 aim at the to= unit: add to=member, target or me";
        if (spec.fix && (spec.follow || s == "projectile"))
            return "fix=1 and reach=1 freeze a line: no follow=1, no projectile";
        if (spec.minDist > 0.f && spec.to != TestSpec::To::Member)
            return "min= measures a to=member pick from the line's source: add to=member";
        if (spec.offset != 0.f && s == "frontal" && spec.rear)
            return "a frontal takes rear=1 or offset=, not both";
        if (spec.atEach && (s == "spot" || s == "spread" || s == "runout" || s == "bait" || s == "holdstill"))
            return "at=each puts a zone, stack or soak under every member";
        if (spec.away > 0.f && (spec.unitAnchor || spec.anchorMember))
            return "away= moves a point anchor; leave anchor= out";
        if (spec.decoy && s == "lanes" && !spec.safe)
            return "decoy=1 on lanes draws the safe=1 lane as a decoy: add safe=1";
        if (spec.drop && !spec.anchorMember && spec.to != TestSpec::To::Member)
            return "drop= is for a shape a member carries: a circle anchor=member, or a line to=member";
        if (spec.grid)
        {
            if (spec.decoy)
                return "grid= makes its decoys with decoys=<n>; decoy=1 is for one shape";
            if (spec.unitAnchor || spec.anchorMember || spec.atEach)
                return "grid= lays its circles around a point: leave anchor= and at=each out";
            if (!Drill::GridCovers(spec.size, spec.gridApart))
                return Acore::StringFormat("grid= apart= must be above 0 and at most {:.2f} (radius x 1.41), so the "
                    "circles leave no gap and a decoy is the only safe cell", spec.size * std::sqrt(2.f));
        }
        if (s == "hunt")
        {
            if (selected == gm)
                return "hunt needs a selected unit: the chaser starts at it";
            if (atPoint || spec.unitAnchor || spec.anchorMember || spec.to != TestSpec::To::None || spec.shifted)
                return "hunt starts at the selected unit and fixates a member it picks (pick=): leave at=, anchor= "
                    "and shift= out";
            if (spec.persist <= Milliseconds(0))
                return "hunt needs persist=<ms>: how long the chase lasts";
            if (spec.tick <= Milliseconds(0))
                return "hunt needs tick= > 0: how often the chase is re-aimed and judged";
        }
        if (spec.bells)
        {
            if (spec.lane || spec.ordering || atPoint || spec.unitAnchor || spec.anchorMember || spec.shifted
                || spec.persist > Milliseconds(0))
                return "bell= places the soak on one of its points: no shape=line, order=, at=, anchor=, shift= or "
                    "persist=";
            if (!spec.need)
                return "bell= needs need= >= 1: the bodies that ring it";
        }

        std::string const bad = opt.Bad();
        if (!bad.empty())
            return Acore::StringFormat("Bad value for: {}", bad);
        return {};
    }

    // Moves the test owner (the Bait's and the RunOut from=owner's reference) to `pos`.
    void PlaceTestOwner(Creature* owner, Position const& pos)
    {
        owner->NearTeleportTo(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), owner->GetOrientation());
    }

    // `.mech drill` step `marker <stacks>`: stacks > 0 sets exactly that many Marked (drill) stacks (205009) on one
    // random member below it (not the selected unit's victim with pick=nontank); 0 removes Marked from every member.
    // Never touches Weakened: drill setup hands out no Weakened, and a reset leaves the Weakened members earned from
    // real failures (orchestrator ruling on STAGE-M4-R-REPORT M-2). The encounter records the players, so its end
    // (and `.mech clear`) removes Marked too. The spell row was checked when the drill was parsed.
    void DrillMarker(Creature* owner, TestSpec const& spec, uint8 stacks, Dice& dice)
    {
        std::vector<Player*> members = GroupMembers(owner, spec.gm, spec.includeGm);
        if (!stacks)
        {
            for (Player* p : members)
                p->RemoveAurasDueToSpell(Ids::SPELL_MARKED_DRILL);
            return;
        }
        Unit* selected = ObjectAccessor::GetUnit(*owner, spec.selected);
        Unit const* victim = spec.pick == TestSpec::Pick::NonTank && selected ? selected->GetVictim() : nullptr;
        members.erase(std::remove_if(members.begin(), members.end(), [stacks, victim](Player const* p)
        {
            return p == victim || p->GetAuraCount(Ids::SPELL_MARKED_DRILL) >= stacks;
        }), members.end());
        if (members.empty())
            return;
        Player* p = members[dice.rng.Index(static_cast<uint32>(members.size()))];
        Aura* aura = p->GetAura(Ids::SPELL_MARKED_DRILL, p->GetGUID());
        if (!aura)
            aura = p->AddAura(Ids::SPELL_MARKED_DRILL, p);
        if (!aura)
        {
            LOG_ERROR("module.bossmechanics", "Marked (drill) ({}) could not be applied to {}",
                Ids::SPELL_MARKED_DRILL, p->GetGUID().ToString());
            return;
        }
        aura->SetStackAmount(stacks);
        aura->RefreshDuration();
        Registry::GetOrCreate(owner->GetMap()).BeginEncounter(owner->GetGUID()).markedPlayers.insert(p->GetGUID());
    }

    // ---- the drill runner (M4, MechanicDrillRunner.h) -------------------------------------------------------------
    using StepFn = std::function<void(Creature* owner)>;

    // Where a run's delayed steps go: the owner's MarkerAI (the Mechanic Test Owner, as `.mech drill` always did),
    // else the owner's registry encounter (Registry::Schedule). Lives for one call (it holds the owner pointer).
    class StepScheduler
    {
    public:
        explicit StepScheduler(Creature* owner) : _owner(owner), _ai(dynamic_cast<MarkerAI*>(owner->AI())),
            _now(GameTime::GetGameTimeMS()) { }

        void operator()(Milliseconds delay, StepFn fn) const
        {
            if (_ai)
                _ai->ScheduleTest(delay, std::move(fn));
            else
                Registry::GetOrCreate(_owner->GetMap()).Schedule(_owner->GetGUID(), _now + delay, std::move(fn));
        }

    private:
        Creature* _owner;
        MarkerAI* _ai;
        Milliseconds _now;
    };

    constexpr char const* kScopeGoneMessage = "the scope player is not on the owner's map";

    Player* ScopeOnMap(Creature* owner, ObjectGuid scope)
    {
        Player* p = ObjectAccessor::FindConnectedPlayer(scope);
        return p && p->IsInWorld() && p->GetMap() == owner->GetMap() ? p : nullptr;
    }

    // RunDrill with the run's dice (a program shares one across its entries).
    bool RunDrillWith(Creature* owner, Unit* boss, ObjectGuid scopeGuid, Drill::Def const& def,
        DrillOptions const& opt, RngPtr const& rng, DrillResult& out, std::string& err)
    {
        Player* scope = ScopeOnMap(owner, scopeGuid);
        if (!scope)
        {
            err = kScopeGoneMessage;
            return false;
        }
        if (!boss || !boss->IsInWorld() || boss->GetMap() != owner->GetMap())
        {
            err = "the boss stand-in is not on the owner's map";
            return false;
        }
        uint32 const cycles = Drill::ClampCycles(opt.cycles, def);
        int32 const dmg = std::max<int32>(opt.dmg, 0);
        bool const self = opt.includeScopePlayer;
        if (GroupMembers(owner, scopeGuid, self).empty())
        {
            err = kNoMemberMessage;
            return false;
        }

        // Parse every step now: a bad line fails the whole drill before anything registers.
        struct Planned
        {
            bool marker = false;
            uint8 stacks = 0;
            TestSpec spec;
        };
        ParseCtx const ctx = CtxOf(scope, scopeGuid);
        std::vector<Planned> planned;
        for (Drill::Step const& step : def.steps)
        {
            if (!Drill::PlanKeysOk(step.line))
            {
                err = Acore::StringFormat("Drill {}: step '{}': bad after= (resolve, with) or cycle= (even, odd)",
                    def.name, step.line);
                return false;
            }
            // after= and cycle= are the plan's (StepSchedule); the `.mech test` parser never sees them.
            std::string line = Drill::StripPlanKeys(step.line);
            std::vector<std::string_view> const head = Acore::Tokenize(line, ' ', false);
            Planned p;
            if (!head.empty() && head[0] == "marker")
            {
                Optional<uint32> n = head.size() >= 2 ? Acore::StringTo<uint32>(head[1]) : std::nullopt;
                if (!n || *n > Ids::MARKED_DRILL_MAX_STACKS)
                {
                    err = Acore::StringFormat("Drill {}: bad step '{}'", def.name, step.line);
                    return false;
                }
                if (!sSpellMgr->GetSpellInfo(Ids::SPELL_MARKED_DRILL))
                {
                    err = Acore::StringFormat("Drill {}: step '{}': no spell {} (Marked (drill); spell_dbc row "
                        "missing?)", def.name, step.line, Ids::SPELL_MARKED_DRILL);
                    return false;
                }
                p.marker = true;
                p.stacks = static_cast<uint8>(*n);
                p.spec.gm = scopeGuid;
                p.spec.includeGm = self;
                p.spec.selected = boss->GetGUID();
                std::optional<std::string_view> const pick = Drill::Value(line, "pick");
                if (pick && *pick != "nontank" && *pick != "any")
                {
                    err = Acore::StringFormat("Drill {}: bad step '{}'", def.name, step.line);
                    return false;
                }
                p.spec.pick = pick && *pick == "nontank" ? TestSpec::Pick::NonTank : TestSpec::Pick::Any;
                planned.push_back(std::move(p));
                continue;
            }
            if (Drill::DamageStep(line) && !opt.percent)
                line += Acore::StringFormat(" dmg={}", dmg);
            std::vector<std::string_view> const stepTokens = Acore::Tokenize(line, ' ', false);
            Options stepOpt;
            uint32 repeat = 1;
            Milliseconds every(0);
            std::string stepErr = ParseTest(ctx, boss, stepTokens, stepOpt, p.spec, repeat, every);
            if (stepErr.empty() && !stepOpt.Unused().empty())
                stepErr = "unused keys " + stepOpt.Unused();
            if (!stepErr.empty())
            {
                err = Acore::StringFormat("Drill {}: step '{}': {}", def.name, step.line, stepErr);
                return false;
            }
            p.spec.includeGm = self;
            planned.push_back(std::move(p));
        }

        bool const follow = Drill::OwnerFollowsBoss(def, owner == boss);
        if (follow)
            PlaceTestOwner(owner, boss->GetPosition());
        // In time order, so the test owner's idle clock (ScheduleTest) ends at the last step.
        StepScheduler const schedule(owner);
        for (Drill::ScheduledStep const& at : Drill::StepSchedule(def, cycles))
        {
            Planned const& p = planned[at.step];
            Milliseconds const delay(static_cast<int64>(at.delayMs));
            if (p.marker)
                schedule(delay, [spec = p.spec, stacks = p.stacks, rng](Creature* testOwner)
                {
                    DrillMarker(testOwner, spec, stacks, *rng);
                });
            else
                schedule(delay, [spec = p.spec, rng, follow](Creature* testOwner)
                {
                    // The Bait's owner stands where the creature stands now, not where it stood at the start: the
                    // tank drags it (STAGE-M3-R-REPORT m-15).
                    if (follow)
                        if (Unit* creature = ObjectAccessor::GetUnit(*testOwner, spec.selected))
                            if (creature->IsAlive())
                                PlaceTestOwner(testOwner, creature->GetPosition());
                    std::string ignored;
                    RegisterRepeat(testOwner, spec, *rng, ignored);
                });
        }
        out.seed = rng->rng.Seed();
        out.cycles = cycles;
        out.steps = static_cast<uint32>(planned.size());
        return true;
    }

    // ---- barrage and minefield bodies (the commands and the programs) ----------------------------------------------
    uint32 BarrageShots(uint32 secs, Milliseconds every)
    {
        return std::min<uint32>(kMaxBarrageShots,
            static_cast<uint32>((Milliseconds(secs * 1000) + every - Milliseconds(1)) / every));
    }

    // The first shot registers now (false, `err` set, when it could not); the rest go on the owner's scheduler.
    bool RunBarrage(Creature* owner, TestSpec const& base, std::vector<Barrage::Kind> const& kinds, uint32 shots,
        Milliseconds every, RngPtr const& rng, std::string& err)
    {
        RegisterBarrageShot(owner, base, kinds, *rng, err);
        if (!err.empty())
            return false;
        StepScheduler const schedule(owner);
        for (uint32 k = 1; k < shots; ++k)
            schedule(every * k, [base, kinds, rng](Creature* testOwner)
            {
                std::string ignored;
                RegisterBarrageShot(testOwner, base, kinds, *rng, ignored);
            });
        return true;
    }

    // Atomic (STAGE-M4-R-REPORT m-6): the command runs on the world thread, but programs lay minefields from their
    // owners' map update threads, which run in parallel.
    uint32 NextMinefieldGroup()
    {
        static std::atomic<uint32> nextGroup{ kFirstMinefieldGroup };
        return nextGroup.fetch_add(1, std::memory_order_relaxed);
    }

    // `count` random points within `spread` yd of `centre`, each on the floor (the centre's height where there is
    // none nearby). The dice draw an angle and a distance per mine, in that order.
    std::vector<Position> RandomMinePoints(Creature* owner, Position const& centre, uint32 count, float spread,
        float facing, Dice& dice)
    {
        std::vector<Position> out;
        out.reserve(count);
        for (uint32 k = 0; k < count; ++k)
        {
            float const ang = dice.rng.Float(0.f, 2.f * Geo::kPi);
            float const dist = spread * std::sqrt(dice.rng.Float(0.f, 1.f));
            float const x = centre.GetPositionX() + dist * std::cos(ang);
            float const y = centre.GetPositionY() + dist * std::sin(ang);
            out.emplace_back(x, y, GroundZ(owner, x, y, centre.GetPositionZ()), facing);
        }
        return out;
    }

    // `.mech minefield layout=u`: the U of MechanicDrillPlan.h opening toward `origin`, each mine on the floor.
    std::vector<Position> UMinePoints(Creature* owner, Position const& origin, float facing, float gap, float depth,
        float radius)
    {
        std::vector<Position> out;
        for (Geo::Vec3 const& p : Drill::UMinefield({ origin.GetPositionX(), origin.GetPositionY(),
            origin.GetPositionZ() }, facing, gap, depth, radius))
            out.emplace_back(p.x, p.y, GroundZ(owner, p.x, p.y, p.z), facing);
        return out;
    }

    // One mine per point, tags 1..n, all in spec's cap group.
    std::vector<uint32> LayMines(Creature* owner, TestSpec spec, std::vector<Position> const& points,
        std::string& err)
    {
        std::vector<uint32> made;
        for (std::size_t k = 0; k < points.size(); ++k)
        {
            spec.point = points[k];
            spec.tag = static_cast<uint32>(k + 1);
            std::vector<uint32> const ids = RegisterTest(owner, spec, err);
            made.insert(made.end(), ids.begin(), ids.end());
        }
        return made;
    }

    // A mine spec of `.mech minefield`: one cap group with cap = count, so none of them fizzles each other.
    TestSpec MineSpec(float radius, Milliseconds arm, Milliseconds grace, std::size_t count, Milliseconds life,
        int32 dmg, ObjectGuid scope, float facing)
    {
        TestSpec spec;
        spec.shape = "mine";
        spec.size = radius;
        spec.warn = arm;
        spec.grace = grace;
        spec.cap = static_cast<uint8>(std::min<std::size_t>(count, 255));
        spec.capGroup = NextMinefieldGroup();
        spec.life = life;
        spec.dmg = dmg;
        spec.gm = scope;
        spec.facing = facing;
        return spec;
    }

    // ---- programs (StartProgram) -----------------------------------------------------------------------------------
    // The program defaults for its barrage and minefield entries: `.mech barrage` and `.mech minefield` defaults.
    constexpr Milliseconds kProgramBarrageEvery = Milliseconds(900);
    constexpr Milliseconds kProgramBarrageWarn = Milliseconds(2000);
    constexpr uint32 kProgramMines = 10;
    constexpr float kProgramMineRadius = 3.f;
    constexpr float kProgramMineSpread = 15.f;
    constexpr float kProgramUGap = 8.f;
    constexpr float kProgramUDepth = 12.f;
    constexpr Milliseconds kProgramMineArm = Milliseconds(3000);
    constexpr Milliseconds kProgramMineGrace = Milliseconds(500);

    struct ProgramRun
    {
        Drill::Program program;
        ObjectGuid scope;
        DrillOptions opt;
        RngPtr rng;
    };
    using ProgramPtr = std::shared_ptr<ProgramRun const>;

    char const* EntryName(Drill::ProgramEntry const& e)
    {
        switch (e.kind)
        {
            case Drill::EntryKind::Drill:
                return e.drill ? e.drill->name : "?";
            case Drill::EntryKind::Barrage:
                return "barrage";
            case Drill::EntryKind::Minefield:
                return "minefield";
            case Drill::EntryKind::UMinefield:
                return "uminefield";
            case Drill::EntryKind::Wait:
                return "wait";
            case Drill::EntryKind::Cast:
                return "cast";
            case Drill::EntryKind::Add:
                return "add";
            case Drill::EntryKind::Debuff:
                return "debuff";
            case Drill::EntryKind::Buff:
                return "buff";
            case Drill::EntryKind::Swap:
                return "swap";
            case Drill::EntryKind::Hold:
                return "hold";
            case Drill::EntryKind::Describe:
                return "describe";
        }
        return "?";
    }

    // M5 (STAGE-M5-HANDOFF §3.2): the spells and creatures a program's descriptor entries name must exist, or the
    // program does not start (the dummy and the sim report it like a bad drill step).
    bool CheckProgramIds(Drill::Program const& program, std::string& err)
    {
        for (Drill::ProgramEntry const& e : program.entries)
        {
            std::vector<uint32> spells;
            std::vector<uint32> creatures;
            switch (e.kind)
            {
                case Drill::EntryKind::Cast:
                    spells.push_back(e.spellId);
                    if (e.unitEntry)
                        creatures.push_back(e.unitEntry);
                    break;
                case Drill::EntryKind::Debuff:
                    spells.push_back(e.spellId);
                    break;
                case Drill::EntryKind::Buff:
                    spells.push_back(e.spellId);
                    creatures.push_back(e.unitEntry);
                    break;
                case Drill::EntryKind::Add:
                case Drill::EntryKind::Hold:
                    if (e.auraId)
                        spells.push_back(e.auraId);
                    creatures.push_back(e.unitEntry);
                    break;
                case Drill::EntryKind::Swap:
                    creatures.push_back(e.unitEntry);
                    break;
                case Drill::EntryKind::Describe:
                    if (e.followEntry)
                        creatures.push_back(e.followEntry);
                    break;
                default:
                    break;
            }
            for (uint32 id : spells)
                if (!sSpellMgr->GetSpellInfo(id))
                {
                    err = Acore::StringFormat("program entry {}: no spell {} (spell_dbc row missing?)", EntryName(e),
                        id);
                    return false;
                }
            for (uint32 id : creatures)
                if (!sObjectMgr->GetCreatureTemplate(id))
                {
                    err = Acore::StringFormat("program entry {}: no creature {} (creature_template row missing?)",
                        EntryName(e), id);
                    return false;
                }
        }
        return true;
    }

    // ---- descriptor entries (M5) on the owner ----------------------------------------------------------------------
    constexpr float kAddRingMinYd = 4.f;        // adds land 4-8 yd around the owner
    constexpr float kAddRingMaxYd = 8.f;

    // The live tracked units of `entry` on the owner's encounter, in the order they were added.
    std::vector<Creature*> EncounterAdds(Creature* owner, uint32 entry)
    {
        std::vector<Creature*> out;
        Registry const* reg = Registry::Get(owner->GetMap());
        Encounter const* enc = reg ? reg->FindEncounter(owner->GetGUID()) : nullptr;
        if (!enc)
            return out;
        for (EncounterUnit const& u : enc->units)
            if (u.entry == entry)
                if (Creature* c = owner->GetMap()->GetCreature(u.guid))
                    if (c->IsInWorld() && c->IsAlive())
                        out.push_back(c);
        return out;
    }

    // `n` distinct members, random, `first` (when it is one of them) ahead of the rest.
    std::vector<Player*> PickMembers(std::vector<Player*> members, uint32 n, Unit const* first, Dice& dice)
    {
        std::vector<Player*> out;
        auto it = std::find(members.begin(), members.end(), first);
        if (first && it != members.end())
        {
            out.push_back(*it);
            members.erase(it);
        }
        while (out.size() < n && !members.empty())
        {
            std::size_t const k = dice.rng.Index(static_cast<uint32>(members.size()));
            out.push_back(members[k]);
            members.erase(members.begin() + static_cast<std::ptrdiff_t>(k));
        }
        return out;
    }

    // One `cast`: the owner, or each live add of unit=, casts at its victim (else the owner's, else the scope
    // player), interruptibly. A caster already casting skips this one. `next` (0: none) is noted for the bots.
    void CastOnce(Creature* owner, ProgramRun const& run, Drill::ProgramEntry const& e, Milliseconds next)
    {
        std::vector<Creature*> const casters = e.unitEntry ? EncounterAdds(owner, e.unitEntry)
            : std::vector<Creature*>{ owner };
        Registry& reg = Registry::GetOrCreate(owner->GetMap());
        for (Creature* caster : casters)
        {
            if (next > Milliseconds(0))
                reg.NoteNextCast(caster->GetGUID(), caster->GetEntry(), e.spellId, next);
            if (caster->HasUnitState(UNIT_STATE_CASTING))
                continue;
            Unit* target = caster->GetVictim();
            if (!target)
                target = owner->GetVictim();
            if (!target)
                target = ScopeOnMap(owner, run.scope);
            SpellCastResult const result = caster->CastSpell(target, e.spellId, false);
            if (result == SPELL_CAST_OK)
                reg.NoteCastStarted(caster->GetGUID(), caster->GetEntry(), e.spellId, GameTime::GetGameTimeMS());
            else
                LOG_DEBUG("module.bossmechanics", "Program cast {} by {} failed ({})", e.spellId,
                    caster->GetGUID().ToString(), static_cast<uint32>(result));
        }
    }

    // A `cast every=` chain: this cast at `at`, the next at `at + every` while it starts before `end` (0: until the
    // encounter ends). The chain is scheduled on the encounter, so StopProgram and the encounter's end drop it.
    void CastChain(Creature* owner, ProgramPtr const& run, std::size_t index, Milliseconds at, Milliseconds end)
    {
        Drill::ProgramEntry const& e = run->program.entries[index];
        Milliseconds const next = at + Milliseconds(e.everyMs);
        bool const more = e.everyMs && (end == Milliseconds(0) || next < end);
        CastOnce(owner, *run, e, more ? next : Milliseconds(0));
        if (!more)
            return;
        Registry::GetOrCreate(owner->GetMap()).Schedule(owner->GetGUID(), next, [run, index, next, end](Creature* o)
        {
            CastChain(o, run, index, next, end);
        });
    }

    // WP-K: the module's drill creatures (300404-300407) are level 80 in their template; at the owner's level (the
    // dummy plays its scope player's level, SimTarget its configured one) kicks, hits and debuffs land on them as on
    // the boss. Other entries keep their own level. M5 WP-F (R m-11): Unit::SetLevel writes the level field only, so
    // the base stats of the new level are set as Creature::SelectLevel and UpdateEntry set them (health and mana
    // scaled by the new level's base over the old one, which keeps the rank rate and HealthModifier; weapon damage,
    // attack power and armor from the new level's base), then the totals are recomputed.
    void MatchOwnerLevel(Creature const* owner, Creature* unit)
    {
        uint8 const from = unit->GetLevel();
        uint8 const to = owner->GetLevel();
        if (!Ids::InCreatureBlock(unit->GetEntry()) || from == to)
            return;
        CreatureTemplate const* cInfo = unit->GetCreatureTemplate();
        CreatureBaseStats const* was = sObjectMgr->GetCreatureBaseStats(from, cInfo->unit_class);
        CreatureBaseStats const* now = sObjectMgr->GetCreatureBaseStats(to, cInfo->unit_class);
        unit->SetLevel(to);
        auto rescale = [](uint32 value, uint32 oldBase, uint32 newBase)
        {
            return oldBase ? static_cast<uint32>(uint64(value) * newBase / oldBase) : value;
        };
        uint32 const health = std::max<uint32>(1, rescale(unit->GetCreateHealth(), was->GenerateHealth(cInfo),
            now->GenerateHealth(cInfo)));
        uint32 const mana = rescale(unit->GetCreateMana(), was->GenerateMana(cInfo), now->GenerateMana(cInfo));
        unit->SetCreateHealth(health);
        unit->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(health));
        unit->SetCreateMana(mana);
        unit->SetStatFlatModifier(UNIT_MOD_MANA, BASE_VALUE, float(mana));
        float const damage = now->GenerateBaseDamage(cInfo);
        for (WeaponAttackType att : { BASE_ATTACK, OFF_ATTACK, RANGED_ATTACK })
        {
            unit->SetBaseWeaponDamage(att, MINDAMAGE, damage);
            unit->SetBaseWeaponDamage(att, MAXDAMAGE, damage * 1.5f);
        }
        unit->SetStatFlatModifier(UNIT_MOD_ATTACK_POWER, BASE_VALUE, float(now->AttackPower));
        unit->SetStatFlatModifier(UNIT_MOD_ATTACK_POWER_RANGED, BASE_VALUE, float(now->RangedAttackPower));
        unit->SetStatFlatModifier(UNIT_MOD_ARMOR, BASE_VALUE, now->GenerateArmor(cInfo));
        unit->UpdateAllStats();
        unit->SetFullHealth();
        if (mana)
            unit->SetPower(POWER_MANA, unit->GetMaxPower(POWER_MANA));
    }

    // `add`: n adds 4-8 yd around the owner, tracked on its encounter (despawned at its end), each with the aura and
    // sent at the owner's victim (an add whose AI does not attack stays put).
    void RunAdd(Creature* owner, ProgramRun const& run, Drill::ProgramEntry const& e, std::string& err)
    {
        uint32 const ttl = Drill::EntryTtlMs(run.program, e);
        TempSummonType const type = ttl ? TEMPSUMMON_TIMED_OR_CORPSE_DESPAWN : TEMPSUMMON_CORPSE_DESPAWN;
        Registry& reg = Registry::GetOrCreate(owner->GetMap());
        Unit* victim = owner->GetVictim();
        for (uint32 k = 0; k < e.count; ++k)
        {
            float const ang = run.rng->rng.Float(0.f, Geo::kTwoPi);
            float const dist = run.rng->rng.Float(kAddRingMinYd, kAddRingMaxYd);
            float const x = owner->GetPositionX() + dist * std::cos(ang);
            float const y = owner->GetPositionY() + dist * std::sin(ang);
            float const z = GroundZ(owner, x, y, owner->GetPositionZ());
            TempSummon* add = owner->SummonCreature(e.unitEntry, x, y, z, Position::NormalizeOrientation(ang
                + Geo::kPi), type, ttl);
            if (!add)
            {
                err = Acore::StringFormat("could not summon creature {}", e.unitEntry);
                return;
            }
            reg.TrackUnit(owner->GetGUID(), add->GetGUID(), e.unitEntry, true);
            MatchOwnerLevel(owner, add);
            if (e.auraId)
                add->AddAura(e.auraId, add);
            if (victim && add->IsAIEnabled && add->AI())
                add->AI()->AttackStart(victim);
        }
    }

    // The member carries a harmful aura of dispel type `type` other than `except`.
    bool HasHarmfulOfType(Player const* p, uint32 type, uint32 except)
    {
        for (auto const& [id, app] : p->GetAppliedAuras())
        {
            SpellInfo const* info = app->GetBase()->GetSpellInfo();
            if (id != except && !app->IsPositive() && info && info->Dispel == type)
                return true;
        }
        return false;
    }

    // `debuff`: the aura on n random scope group members (apart= / with= narrow the pool first; M5 WP-F, R M-1).
    void RunDebuff(Creature* owner, ProgramRun const& run, Drill::ProgramEntry const& e, std::string& err)
    {
        std::vector<Player*> pool = GroupMembers(owner, run.scope, run.opt.includeScopePlayer);
        if (e.apart || e.withAura)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(e.spellId);
            uint32 const type = info ? info->Dispel : 0;
            std::vector<Player*> fit;
            for (Player* p : pool)
                if ((!e.withAura || p->HasAura(e.withAura)) && (!e.apart || !type || !HasHarmfulOfType(p, type,
                    e.spellId)))
                    fit.push_back(p);
            if (!fit.empty())
                pool = std::move(fit);
        }
        std::vector<Player*> const members = PickMembers(std::move(pool), e.count, nullptr, *run.rng);
        if (members.empty())
        {
            err = kNoMemberMessage;
            return;
        }
        uint32 applied = 0;
        for (Player* p : members)
        {
            applied += owner->AddAura(e.spellId, p) ? 1 : 0;
            // hp= (WP-K): wound the member down to that percent (never up), the dispel drill's "someone is under 50%".
            if (e.woundPct && p->IsAlive())
                p->SetHealth(std::min<uint32>(p->GetHealth(), std::max<uint32>(1,
                    p->CountPctFromMaxHealth(static_cast<int32>(e.woundPct)))));
        }
        if (!applied)
            err = Acore::StringFormat("spell {} put no aura on anyone (not an aura spell, or every target immune)",
                e.spellId);
    }

    // `buff`: the aura on every live add of the entry (the owner too when it is that entry).
    void RunBuff(Creature* owner, Drill::ProgramEntry const& e)
    {
        std::vector<Creature*> targets = EncounterAdds(owner, e.unitEntry);
        if (owner->GetEntry() == e.unitEntry)
            targets.insert(targets.begin(), owner);
        for (Creature* t : targets)
            t->AddAura(e.spellId, t);
    }

    // `swap`: the owner and a random live add of the entry trade places (bots that follow a guid keep their target).
    void RunSwap(Creature* owner, ProgramRun const& run, Drill::ProgramEntry const& e)
    {
        std::vector<Creature*> const adds = EncounterAdds(owner, e.unitEntry);
        if (adds.empty())
            return;
        Creature* add = adds[run.rng->rng.Index(static_cast<uint32>(adds.size()))];
        Position const a = owner->GetPosition();
        Position const b = add->GetPosition();
        owner->NearTeleportTo(b.GetPositionX(), b.GetPositionY(), b.GetPositionZ(), b.GetOrientation());
        add->NearTeleportTo(a.GetPositionX(), a.GetPositionY(), a.GetPositionZ(), a.GetOrientation());
    }

    // `pick=tank`'s first member: the owner's victim, else (a dummy that attacks nobody) the first member the role
    // provider calls a tank.
    Unit const* TankOf(Creature* owner, std::vector<Player*> const& members)
    {
        if (Unit const* victim = owner->GetVictim())
            return victim;
        for (Player const* p : members)
            if (HasRole(GetRoleProvider().Get(owner, p), Role::Tank))
                return p;
        return nullptr;
    }

    // `hold`: a prison of the entry on each of n members (pick=tank: the tank first, TankOf), the aura on the held
    // player (cast by the prison) until the prison dies or despawns (Registry::NoteHeld).
    void RunHold(Creature* owner, ProgramRun const& run, Drill::ProgramEntry const& e, std::string& err)
    {
        std::vector<Player*> const group = GroupMembers(owner, run.scope, run.opt.includeScopePlayer);
        std::vector<Player*> const members = PickMembers(group, e.count, e.pickTank ? TankOf(owner, group) : nullptr,
            *run.rng);
        if (members.empty())
        {
            err = kNoMemberMessage;
            return;
        }
        uint32 const ttl = Drill::EntryTtlMs(run.program, e);
        TempSummonType const type = ttl ? TEMPSUMMON_TIMED_OR_CORPSE_DESPAWN : TEMPSUMMON_CORPSE_DESPAWN;
        Registry& reg = Registry::GetOrCreate(owner->GetMap());
        for (Player* p : members)
        {
            TempSummon* prison = owner->SummonCreature(e.unitEntry, p->GetPosition(), type, ttl);
            if (!prison)
            {
                err = Acore::StringFormat("could not summon creature {}", e.unitEntry);
                return;
            }
            reg.TrackUnit(owner->GetGUID(), prison->GetGUID(), e.unitEntry, true);
            MatchOwnerLevel(owner, prison);
            // WP-K: a prison stays where it closed on the player and never fights back (the drill creature has no
            // script; its default AI would chase whoever breaks it).
            prison->SetReactState(REACT_PASSIVE);
            prison->SetControlled(true, UNIT_STATE_ROOT);
            if (e.auraId)
                prison->AddAura(e.auraId, p);
            reg.NoteHeld(owner->GetGUID(), prison->GetGUID(), p->GetGUID(), e.auraId);
        }
    }

    // `describe` (WP-K options): phase= sets the preset's phase; follow= turns the preset's first target entry of
    // that creature into the guid of its first live add (the encounter's units in the order they were added).
    void RunDescribe(Creature* owner, Drill::ProgramEntry const& e, std::string& err)
    {
        Descriptor d = e.preset->build();
        if (e.phase)
            d.phase = *e.phase;
        if (e.followEntry)
        {
            std::vector<Creature*> const adds = EncounterAdds(owner, e.followEntry);
            auto it = std::find_if(d.targets.begin(), d.targets.end(), [&e](TargetEntry const& t)
            {
                return !t.guid && t.entry == e.followEntry;
            });
            if (adds.empty() || it == d.targets.end())
                err = Acore::StringFormat("follow={}: {}", e.followEntry, adds.empty() ? "no live add of it"
                    : "the preset lists no target of it");
            else
            {
                it->guid = adds.front()->GetGUID().GetRawValue();
                it->entry = 0;
            }
        }
        Registry::GetOrCreate(owner->GetMap()).Describe(owner->GetGUID(), std::move(d));
    }

    // A failed entry is loud (STAGE-M4-R-REPORT M-1): a LOG_ERROR, a whisper to the training dummy's scope player,
    // and a record on the owner's encounter, which the sim takes into its report (Sim::TakeProgramErrors). A drill
    // step that no longer parses (a spell row missing after a deploy) would otherwise drop that drill from every loop
    // in silence.
    void ReportProgramError(Creature* owner, ProgramRun const& run, Drill::ProgramEntry const& e,
        std::string const& err)
    {
        std::string const text = Acore::StringFormat("Program entry {} on {} failed: {}", EntryName(e),
            owner->GetGUID().ToString(), err);
        LOG_ERROR("module.bossmechanics", "{}", text);
        Registry::GetOrCreate(owner->GetMap()).NoteProgramError(owner->GetGUID(), text);
        if (Ids::IsTrainingDummy(owner->GetEntry()))
            if (Player* scope = ScopeOnMap(owner, run.scope))
                owner->Whisper(Acore::StringFormat("Program entry {} failed: {}", EntryName(e), err), LANG_UNIVERSAL,
                    scope);
    }

    // One entry at its start (`at`); the owner is the boss stand-in. A failure (the scope player gone or dead, nobody
    // to land on, a step that does not parse, a summon refused) skips the entry, reported by ReportProgramError; the
    // program goes on.
    void RunProgramEntry(Creature* owner, ProgramPtr const& runPtr, std::size_t index, Milliseconds at)
    {
        ProgramRun const& run = *runPtr;
        Drill::ProgramEntry const& e = run.program.entries[index];
        std::string err;
        switch (e.kind)
        {
            case Drill::EntryKind::Wait:
                return;
            case Drill::EntryKind::Cast:
            {
                uint32 const window = Drill::EntryWindowMs(run.program, e);
                CastChain(owner, runPtr, index, at, window ? at + Milliseconds(window) : Milliseconds(0));
                return;
            }
            case Drill::EntryKind::Add:
                RunAdd(owner, run, e, err);
                break;
            case Drill::EntryKind::Debuff:
                RunDebuff(owner, run, e, err);
                break;
            case Drill::EntryKind::Buff:
                RunBuff(owner, e);
                break;
            case Drill::EntryKind::Swap:
                RunSwap(owner, run, e);
                break;
            case Drill::EntryKind::Hold:
                RunHold(owner, run, e, err);
                break;
            case Drill::EntryKind::Describe:
                if (e.preset && e.preset->build)
                    RunDescribe(owner, e, err);
                break;
            default:
                break;
        }
        if (Drill::Instant(e.kind))
        {
            if (!err.empty())
                ReportProgramError(owner, run, e, err);
            return;
        }
        // Percent: -1 is the `.mech test` default, a percent of reference health (TestSpec::pct, 20).
        int32 const dmg = run.opt.percent ? -1 : std::max<int32>(run.opt.dmg, 0);
        Player* scope = ScopeOnMap(owner, run.scope);
        if (e.kind == Drill::EntryKind::Drill)
        {
            DrillOptions o = run.opt;
            o.cycles = e.cycles;
            DrillResult res;
            if (e.drill)
                RunDrillWith(owner, owner, run.scope, *e.drill, o, run.rng, res, err);
        }
        else if (!scope)
            err = kScopeGoneMessage;
        else if (e.kind == Drill::EntryKind::Barrage)
        {
            TestSpec base;
            base.gm = run.scope;
            base.facing = scope->GetOrientation();
            base.warn = kProgramBarrageWarn;
            base.dmg = dmg;
            base.atMember = true;
            base.includeGm = run.opt.includeScopePlayer;
            std::vector<Barrage::Kind> kinds;
            for (uint32 i = 0; i < Barrage::kKindCount; ++i)
                kinds.push_back(static_cast<Barrage::Kind>(i));
            RunBarrage(owner, base, kinds, BarrageShots(e.seconds, kProgramBarrageEvery), kProgramBarrageEvery,
                run.rng, err);
        }
        else
        {
            float const facing = scope->GetOrientation();
            std::vector<Position> const points = e.kind == Drill::EntryKind::Minefield
                ? RandomMinePoints(owner, owner->GetPosition(), kProgramMines, kProgramMineSpread, facing, *run.rng)
                : UMinePoints(owner, scope->GetPosition(), facing, kProgramUGap, kProgramUDepth, kProgramMineRadius);
            TestSpec const spec = MineSpec(kProgramMineRadius, kProgramMineArm, kProgramMineGrace, points.size(),
                Milliseconds(e.lengthMs), dmg, run.scope, facing);
            LayMines(owner, spec, points, err);
        }
        if (!err.empty())
            ReportProgramError(owner, run, e, err);
    }

    // Schedules every entry of one loop from `start` on the owner's encounter, and the next loop at its end.
    void ScheduleProgram(Creature* owner, ProgramPtr const& run, Milliseconds start)
    {
        Registry& reg = Registry::GetOrCreate(owner->GetMap());
        ObjectGuid const guid = owner->GetGUID();
        for (std::size_t i = 0; i < run->program.entries.size(); ++i)
        {
            Milliseconds const at = start + Milliseconds(run->program.entries[i].startMs);
            reg.Schedule(guid, at, [run, i, at](Creature* o)
            {
                RunProgramEntry(o, run, i, at);
            });
        }
        if (run->opt.loop && run->program.loopMs)
        {
            Milliseconds const next = start + Milliseconds(run->program.loopMs);
            reg.Schedule(guid, next, [run, next](Creature* o)
            {
                ScheduleProgram(o, run, next);
            });
        }
    }
}

bool BossMechanics::RunDrill(Creature* owner, Unit* boss, ObjectGuid scopePlayer, Drill::Def const& def,
    DrillOptions const& opt, DrillResult& out, std::string& err)
{
    if (!owner || !owner->IsInWorld())
    {
        err = "the owner is not in the world";
        return false;
    }
    uint32 const seed = opt.seed ? *opt.seed : urand(1, 0x7FFFFFFF);
    return RunDrillWith(owner, boss, scopePlayer, def, opt, std::make_shared<Dice>(seed), out, err);
}

bool BossMechanics::StartProgram(Creature* owner, ObjectGuid scopePlayer, std::string_view text,
    DrillOptions const& opt, std::string& err)
{
    if (!owner || !owner->IsInWorld() || !owner->IsAlive())
    {
        err = "the owner is not an alive creature in the world";
        return false;
    }
    std::optional<Drill::Program> program = Drill::ParseProgram(text, err);
    if (!program || !CheckProgramIds(*program, err))
        return false;
    if (!ScopeOnMap(owner, scopePlayer))
    {
        err = kScopeGoneMessage;
        return false;
    }
    Registry& reg = Registry::GetOrCreate(owner->GetMap());
    reg.CancelScheduled(owner->GetGUID());
    reg.AddScopePlayer(owner->GetGUID(), scopePlayer);
    auto run = std::make_shared<ProgramRun>();
    run->program = std::move(*program);
    run->scope = scopePlayer;
    run->opt = opt;
    run->rng = std::make_shared<Dice>(opt.seed ? *opt.seed : urand(1, 0x7FFFFFFF));
    ScheduleProgram(owner, run, GameTime::GetGameTimeMS());
    return true;
}

bool BossMechanics::CheckProgram(std::string_view text, std::string& err)
{
    std::optional<Drill::Program> program = Drill::ParseProgram(text, err);
    return program && CheckProgramIds(*program, err);
}

void BossMechanics::StopProgram(Creature* owner)
{
    if (!owner || !owner->IsInWorld())
        return;
    Registry* reg = Registry::Get(owner->GetMap());
    if (!reg)
        return;
    reg->CancelScheduled(owner->GetGUID());
    if (reg->FindEncounter(owner->GetGUID()))
        reg->EndEncounter(owner->GetGUID());
}

class boss_mechanics_commandscript : public CommandScript
{
public:
    boss_mechanics_commandscript() : CommandScript("boss_mechanics_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable mechTable =
        {
            { "test",     HandleTest,     SEC_GAMEMASTER, Console::No },
            { "clear",    HandleClear,    SEC_GAMEMASTER, Console::No },     // [all]
            { "list",     HandleList,     SEC_GAMEMASTER, Console::No },
            { "ledger",   HandleLedger,   SEC_GAMEMASTER, Console::No },
            { "weakened", HandleWeakened, SEC_GAMEMASTER, Console::No },
            { "palette",  HandlePalette,  SEC_GAMEMASTER, Console::No },
            { "markers",  HandleMarkers,  SEC_GAMEMASTER, Console::No },     // [range]
            { "barrage",  HandleBarrage,  SEC_GAMEMASTER, Console::No },
            { "minefield", HandleMinefield, SEC_GAMEMASTER, Console::No },
            { "drill",    HandleDrill,    SEC_GAMEMASTER, Console::No },
            { "dummy",    HandleMechDummyCommand, SEC_GAMEMASTER, Console::No },   // MechanicDummyAI.cpp
            { "desc",     HandleMechDescCommand, SEC_GAMEMASTER, Console::No },    // MechanicDescCommand.cpp (M5)
            { "bots",     HandleBots,     SEC_GAMEMASTER, Console::No },     // [all]
        };
        static ChatCommandTable commandTable =
        {
            { "mech", mechTable },
        };
        return commandTable;
    }

    static bool HandleTest(ChatHandler* handler, Tail args)
    {
        Player* gm = handler->GetPlayer();
        std::vector<std::string_view> const tokens = Acore::Tokenize(args, ' ', false);
        TestSpec spec;
        Options opt;
        uint32 repeat = 1;
        Milliseconds every(1000);
        Unit* selected = handler->getSelectedUnit();       // the GM when nothing is selected
        std::string const parseErr = ParseTest(CtxOf(gm, gm->GetGUID()), selected, tokens, opt, spec, repeat, every);
        if (!parseErr.empty())
        {
            handler->SendErrorMessage("{}", parseErr);
            return false;
        }
        RngPtr const rng = MakeRng(opt);
        // seed= is read here, after ParseTest's bad-value check (STAGE-M3-R-REPORT n-5).
        if (std::string const bad = opt.Bad(); !bad.empty())
        {
            handler->SendErrorMessage("Bad value for: {}", bad);
            return false;
        }
        std::string const& s = spec.shape;

        Creature* owner = GetOrSummonTestOwner(gm);
        if (!owner)
        {
            handler->SendErrorMessage("Could not summon the Mechanic Test Owner ({})",
                Ids::NPC_MECHANIC_TEST_OWNER);
            return false;
        }

        bool const landsOnMembers = spec.atMember || spec.atGroup || spec.atEach || spec.atPredict
            || spec.anchorMember || spec.to == TestSpec::To::Member || s == "spot" || s == "hunt" || spec.bells;
        if (landsOnMembers && GroupMembers(owner, spec.gm, spec.includeGm).empty())
        {
            handler->SendErrorMessage("{}", kNoMemberMessage);
            return false;
        }
        if (s == "bait")    // the bait lands on whoever is farthest from the owner
            PlaceTestOwner(owner, spec.atGm ? gm->GetPosition() : spec.point);

        MarkerAI* ai = dynamic_cast<MarkerAI*>(owner->AI());
        std::vector<uint32> made;
        std::string err;
        for (uint32 k = 0; k < repeat; ++k)
        {
            TestSpec one = spec;
            one.tag = spec.tag + k;
            Milliseconds const delay = every * k;
            if (k == 0 || delay == Milliseconds(0))
            {
                std::vector<uint32> const ids = RegisterRepeat(owner, one, *rng, err);
                made.insert(made.end(), ids.begin(), ids.end());
            }
            else if (ai)
                ai->ScheduleTest(delay, [one, rng](Creature* testOwner)
                {
                    std::string ignored;
                    RegisterRepeat(testOwner, one, *rng, ignored);
                });
            else
            {
                // No MarkerAI on the test owner (ScriptName missing): register now with the delay in the warning.
                one.warn += delay;
                std::vector<uint32> const ids = RegisterRepeat(owner, one, *rng, err);
                made.insert(made.end(), ids.begin(), ids.end());
            }
        }

        if (made.empty())
        {
            handler->SendErrorMessage("Nothing registered: {}", err);
            return false;
        }
        std::string list;
        for (uint32 id : made)
            list += (list.empty() ? "" : " ") + std::to_string(id);
        handler->PSendSysMessage("Mechanic test {} {:.1f}: registered #{}{}; resolves in {} ms.", s, spec.size, list,
            repeat > 1 ? Acore::StringFormat(" (+{} repeats every {} ms)", repeat - 1, every.count()) : "",
            spec.warn.count());
        ContentKey const key = GetContentKeyProvider().Get(owner->GetMap(), owner);
        ReferenceHealth const ref = GetReferenceHealthProvider().Get(key);
        handler->PSendSysMessage("Reference health ({}): non-tank {}, tank {}.", RefHealth::Describe(key), ref.nonTank,
            ref.tank);
        if (spec.atMember || spec.atPredict || spec.anchorMember || spec.to == TestSpec::To::Member
            || spec.scatter > 0.f || spec.away > 0.f || spec.decoys || s == "spot")
            handler->PSendSysMessage("Landing on {} (seed {}; seed= repeats the picks).",
                (spec.scatter > 0.f || spec.away > 0.f || spec.decoys) && !spec.atMember && !spec.atPredict
                    ? "random points" : "group members, picked as each repeat registers", rng->rng.Seed());
        if (spec.decoy || spec.decoys)
            handler->PSendSysMessage("Decoys are drawn like the rest but hit nobody, and bots do not see them.");
        if (s == "bait")
            handler->PSendSysMessage("The test owner moved to {}: the bait lands on whoever is farthest from it.",
                spec.atGm || selected == gm ? "you" : selected->GetName());
        if (s == "hunt")
            handler->PSendSysMessage("The chaser starts at {} and walks at {:.2f} x run speed ({:.1f} yd/s) for {} ms "
                "after the warning (seed {}).", selected->GetName(), spec.huntRate,
                Drill::HuntSpeedYdS(spec.huntRate), spec.persist.count(), rng->rng.Seed());
        std::string const unused = opt.Unused();
        if (!unused.empty())
            handler->PSendSysMessage("Ignored keys for {}: {}", s, unused);
        if (gm->IsGameMaster())
            handler->PSendSysMessage("Reminder: GMs are never hit; use .gm off to test on yourself.");
        return true;
    }

    // `.mech drill <name> [cycles=3] [seed=] [dmg=1] [self=0]`: a boss doc's mechanics, scripted on the selected
    // creature and the group (MechanicDrill.h). Every step is a `.mech test` line parsed now (with the creature as the
    // selection) and registered from the test owner's scheduler, so `.mech clear` ends the drill. Damage steps get
    // `dmg=<n>`; `self=1` lets member picks land on the GM.
    static bool HandleDrill(ChatHandler* handler, Tail args)
    {
        Player* gm = handler->GetPlayer();
        std::vector<std::string_view> const tokens = Acore::Tokenize(args, ' ', false);
        std::string names;
        for (Drill::Def const& d : Drill::All())
            names += std::string(names.empty() ? "" : "|") + d.name;
        Drill::Def const* def = tokens.empty() ? nullptr : Drill::Find(tokens[0]);
        if (!def)
        {
            handler->SendErrorMessage("Usage: .mech drill <{}> [cycles=3 (alexei 1)] [seed=] [dmg=1] [self=0], with "
                "the creature that plays the boss selected", names);
            return false;
        }
        Options opt;
        if (!CollectOptions(handler, tokens, 1, opt))
            return false;
        uint32 const cycles = static_cast<uint32>(std::clamp<int32>(opt.Int("cycles",
            static_cast<int32>(def->defaultCycles)), 1, Drill::kMaxDrillCycles));
        int32 const dmg = std::max<int32>(opt.Int("dmg", 1), 0);
        bool const self = opt.Bool("self", false);
        RngPtr const rng = MakeRng(opt);
        std::string const bad = opt.Bad();
        if (!bad.empty())
        {
            handler->SendErrorMessage("Bad value for: {}", bad);
            return false;
        }
        Unit* selected = handler->getSelectedUnit();
        if (!selected || selected == gm || !selected->IsCreature())
        {
            handler->SendErrorMessage("Select the creature that plays the boss (a mob a bot tanks where the drill has "
                "a frontal or tank exemption)");
            return false;
        }

        Creature* owner = GetOrSummonTestOwner(gm);
        if (!owner)
        {
            handler->SendErrorMessage("Could not summon the Mechanic Test Owner ({})", Ids::NPC_MECHANIC_TEST_OWNER);
            return false;
        }
        MarkerAI* ai = dynamic_cast<MarkerAI*>(owner->AI());
        if (!ai)
        {
            handler->SendErrorMessage("The Mechanic Test Owner has no MarkerAI (ScriptName npc_boss_mechanic_marker "
                "missing), so it cannot schedule a drill");
            return false;
        }
        if (owner == selected)
        {
            handler->SendErrorMessage("Select the boss creature, not the Mechanic Test Owner");
            return false;
        }
        // The runner (MechanicDrillRunner.h) checks the members, parses every step, then schedules them on the test
        // owner's MarkerAI; its messages are the ones this command always sent.
        DrillOptions dopt;
        dopt.seed = rng->rng.Seed();
        dopt.dmg = dmg;
        dopt.cycles = cycles;
        dopt.includeScopePlayer = self;
        DrillResult run;
        std::string err;
        if (!RunDrillWith(owner, selected, gm->GetGUID(), *def, dopt, rng, run, err))
        {
            handler->SendErrorMessage("{}", err);
            return false;
        }

        handler->PSendSysMessage("Mechanic drill {} on {}: {}.", def->name, selected->GetName(), def->summary);
        handler->PSendSysMessage("{} cycle(s) of {:.1f} s ({} step(s) each), {} damage per hit, seed {}.{} Check "
            ".mech ledger (by tag) and .mech bots; .mech clear stops it.", run.cycles, def->cycleMs / 1000.f,
            run.steps, dmg, run.seed, def->ownerAtCreature ? " The test owner moved to the creature." : "");
        std::string const unused = opt.Unused();
        if (!unused.empty())
            handler->PSendSysMessage("Ignored keys for drill: {}", unused);
        if (!gm->IsGameMaster())
            handler->PSendSysMessage("Reminder: you are not in .gm on, so the drill also targets and hits you.");
        return true;
    }

    // `.mech barrage`: a scheduled series of single Zones, each on a random group member, for the bot acceptance run
    // (every shot threatens its member, see MechanicBarrage.h). The first shot registers now, the rest from the test
    // owner's scheduler, so `.mech clear` ends the series. Hits land in `.mech ledger` under the tag 1 + shape
    // (1 circle, 2 donut, 3 cone, 4 line, 5 projectile).
    static bool HandleBarrage(ChatHandler* handler, Tail args)
    {
        Player* gm = handler->GetPlayer();
        std::vector<std::string_view> const tokens = Acore::Tokenize(args, ' ', false);
        Options opt;
        if (!CollectOptions(handler, tokens, 0, opt))
            return false;

        uint32 const secs = static_cast<uint32>(std::clamp<int32>(opt.Int("secs", 30), 1, 600));
        Milliseconds const every = Milliseconds(std::clamp<int32>(opt.Int("every", 900), 100, 60000));
        Milliseconds const warn = Milliseconds(std::clamp<int32>(opt.Int("warn", 2000), 0, 60000));
        int32 const dmg = std::max<int32>(opt.Int("dmg", 1), 0);
        bool const self = opt.Bool("self", false);
        std::vector<Barrage::Kind> kinds;
        std::string const shapeList = opt.Str("shapes", "circle,donut,cone,line,projectile");
        for (std::string_view const name : Acore::Tokenize(shapeList, ',', false))
        {
            bool known = false;
            for (uint32 i = 0; i < Barrage::kKindCount; ++i)
            {
                Barrage::Kind const kind = static_cast<Barrage::Kind>(i);
                if (name == Barrage::ShapeName(kind))
                {
                    known = true;
                    if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end())
                        kinds.push_back(kind);
                }
            }
            if (!known)
                opt.MarkBad("shapes");
        }
        if (kinds.empty())
            opt.MarkBad("shapes");
        RngPtr const rng = MakeRng(opt);
        std::string const bad = opt.Bad();
        if (!bad.empty())
        {
            handler->SendErrorMessage("Bad value for: {} (shapes: circle donut cone line projectile, comma separated)",
                bad);
            return false;
        }

        TestSpec base;
        base.gm = gm->GetGUID();
        base.facing = gm->GetOrientation();
        base.warn = warn;
        base.dmg = dmg;
        base.atMember = true;       // the shot picks its member itself (RegisterBarrageShot)
        base.includeGm = self;
        uint32 const shots = BarrageShots(secs, every);

        Creature* owner = GetOrSummonTestOwner(gm);
        if (!owner)
        {
            handler->SendErrorMessage("Could not summon the Mechanic Test Owner ({})", Ids::NPC_MECHANIC_TEST_OWNER);
            return false;
        }
        MarkerAI* ai = dynamic_cast<MarkerAI*>(owner->AI());
        if (!ai)
        {
            handler->SendErrorMessage("The Mechanic Test Owner has no MarkerAI (ScriptName npc_boss_mechanic_marker "
                "missing), so it cannot schedule a series");
            return false;
        }
        if (GroupMembers(owner, base.gm, base.includeGm).empty())
        {
            handler->SendErrorMessage("{}", kNoMemberMessage);
            return false;
        }

        std::string err;
        if (!RunBarrage(owner, base, kinds, shots, every, rng, err))      // the rest on the MarkerAI
        {
            handler->SendErrorMessage("Nothing registered: {}", err);
            return false;
        }

        std::string names;
        for (Barrage::Kind kind : kinds)
            names += std::string(names.empty() ? "" : ",") + Barrage::ShapeName(kind);
        handler->PSendSysMessage("Mechanic barrage: {} shot(s) over {} s, one every {} ms on a random group member, "
            "{} ms warning, shapes {}, {} damage each, seed {}.", shots, secs, every.count(), warn.count(), names,
            dmg, rng->rng.Seed());
        handler->PSendSysMessage("Check .mech ledger (tags 1 circle, 2 donut, 3 cone, 4 line, 5 projectile); "
            ".mech clear stops it.");
        std::string const unused = opt.Unused();
        if (!unused.empty())
            handler->PSendSysMessage("Ignored keys for barrage: {}", unused);
        if (gm->IsGameMaster())
            handler->PSendSysMessage("Reminder: GMs are never hit; use .gm off to test on yourself.");
        return true;
    }

    // `.mech minefield`: `count` mines at random points within `spread` yd of the selected unit (else the GM), one cap
    // group with cap = count so none of them fizzles each other, life 120 s. `layout=u` (M4, §3.7): the fixed U of
    // MechanicDrillPlan.h instead, opening toward the GM along their facing (`gap` between the arms' inner edges,
    // `depth` to the closed end), the repro of a bot left behind overlapping mines; no dice.
    static bool HandleMinefield(ChatHandler* handler, Tail args)
    {
        Player* gm = handler->GetPlayer();
        std::vector<std::string_view> const tokens = Acore::Tokenize(args, ' ', false);
        Options opt;
        if (!CollectOptions(handler, tokens, 0, opt))
            return false;

        std::string const layout = opt.Str("layout", "random");
        bool const uLayout = layout == "u";
        if (!uLayout && layout != "random")
            opt.MarkBad("layout");
        uint32 const count = uLayout ? 0 : static_cast<uint32>(std::clamp<int32>(opt.Int("count", 10), 1, kMaxMines));
        float const radius = opt.Float("radius", 3.f);
        float const spread = uLayout ? 0.f : opt.Float("spread", 15.f);
        float const gap = uLayout ? opt.Float("gap", 8.f) : 0.f;
        float const depth = uLayout ? opt.Float("depth", 12.f) : 0.f;
        Milliseconds const arm = opt.Ms("arm", Milliseconds(3000));
        Milliseconds const grace = opt.Ms("grace", Milliseconds(500));
        int32 const dmg = std::max<int32>(opt.Int("dmg", 1), 0);
        RngPtr const rng = uLayout ? nullptr : MakeRng(opt);
        if (radius <= 0.f || radius > 30.f)
            opt.MarkBad("radius");
        if (!uLayout && (spread <= 0.f || spread > 100.f))
            opt.MarkBad("spread");
        if (uLayout && (gap <= 0.f || gap > 40.f))
            opt.MarkBad("gap");
        if (uLayout && (depth <= 0.f || depth > 60.f))
            opt.MarkBad("depth");
        std::string const bad = opt.Bad();
        if (!bad.empty())
        {
            handler->SendErrorMessage(uLayout ? "Bad value for: {} (radius 0-30, gap 0-40, depth 0-60)"
                : "Bad value for: {} (radius 0-30, spread 0-100)", bad);
            return false;
        }

        Creature* owner = GetOrSummonTestOwner(gm);
        if (!owner)
        {
            handler->SendErrorMessage("Could not summon the Mechanic Test Owner ({})", Ids::NPC_MECHANIC_TEST_OWNER);
            return false;
        }

        Unit* centre = handler->getSelectedUnit();      // the GM when nothing is selected
        float const facing = gm->GetOrientation();
        std::vector<Position> const points = uLayout
            ? UMinePoints(owner, gm->GetPosition(), facing, gap, depth, radius)
            : RandomMinePoints(owner, centre->GetPosition(), count, spread, facing, *rng);
        if (points.size() > kMaxMines)
        {
            handler->SendErrorMessage("layout=u needs {} mines (at most {}): lower gap or depth, or raise radius",
                points.size(), kMaxMines);
            return false;
        }
        TestSpec const spec = MineSpec(radius, arm, grace, points.size(), kMineLife, dmg, gm->GetGUID(), facing);
        std::string err;
        std::vector<uint32> const made = LayMines(owner, spec, points, err);
        if (made.empty())
        {
            handler->SendErrorMessage("Nothing registered: {}", err);
            return false;
        }
        if (uLayout)
            handler->PSendSysMessage("Mechanic minefield: a U of {} mine(s) r={:.1f} opening toward you, {:.1f} yd "
                "between the arms, {:.1f} yd deep, arm {} ms, grace {} ms, life {} s, cap group {}, {} damage. Lead "
                "the bots into the pocket, then walk round the closed end: they must leave by the mouth. .mech clear "
                "removes them.", made.size(), radius, gap, depth, arm.count(), grace.count(),
                kMineLife.count() / 1000, spec.capGroup, dmg);
        else
            handler->PSendSysMessage("Mechanic minefield: {} mine(s) r={:.1f} within {:.0f} yd of {}, arm {} ms, grace "
                "{} ms, life {} s, cap group {}, {} damage, seed {}. .mech clear removes them.", made.size(), radius,
                spread, centre == gm ? "you" : centre->GetName(), arm.count(), grace.count(),
                kMineLife.count() / 1000, spec.capGroup, dmg, rng->rng.Seed());
        std::string const unused = opt.Unused();
        if (!unused.empty())
            handler->PSendSysMessage("Ignored keys for minefield: {}", unused);
        if (gm->IsGameMaster())
            handler->PSendSysMessage("Reminder: GMs are never hit; use .gm off to test on yourself.");
        return true;
    }

    // `.mech bots [all]`: per-bot state of the boss-mechanics strategy. The report is the playerbots glue's
    // (MechanicBotReport.cpp registers it through SetBotReport); a build without it says so.
    static bool HandleBots(ChatHandler* handler, Optional<std::string> scope)
    {
        bool const all = scope && *scope == "all";
        if (scope && !all)
        {
            handler->SendErrorMessage("Usage: .mech bots [all]");
            return false;
        }
        BotReportFn const report = GetBotReport();
        if (!report)
        {
            handler->PSendSysMessage("Bot support not compiled into this build (mod-playerbots was not present when "
                "the module was built), so there is nothing to report.");
            return true;
        }
        return report(handler, handler->GetPlayer(), all);
    }

    // `.mech clear`: the `.mech test` encounters only (a real boss fight keeps its mechanics, palette, boss state
    // binding and failure listener). `.mech clear all`: every encounter on the map, real bosses included.
    static bool HandleClear(ChatHandler* handler, Optional<std::string> scope)
    {
        bool const all = scope && *scope == "all";
        if (scope && !all)
        {
            handler->SendErrorMessage("Usage: .mech clear [all]");
            return false;
        }
        Map* map = handler->GetPlayer()->GetMap();
        Registry* reg = Registry::Get(map);
        if (!reg)
        {
            handler->PSendSysMessage("No mechanics on this map.");
            return true;
        }
        std::vector<ObjectGuid> const owners = TestOwners(map);
        std::size_t const before = reg->Size();
        if (all)
            reg->ClearAll();
        else
            for (ObjectGuid const& g : owners)
                reg->EndEncounter(g);       // markers, mechanics, ledger, the Weakened it stacked
        for (ObjectGuid const& g : owners)
        {
            if (Creature* c = map->GetCreature(g))
            {
                c->CombatStop(true);        // a tester already in combat was linked to it by a resolve hit
                c->DespawnOrUnsummon();
            }
        }
        std::size_t const cleared = before - reg->Size();
        if (all)
            handler->PSendSysMessage("Cleared {} mechanics and every encounter on this map.", cleared);
        else
            handler->PSendSysMessage("Cleared {} test mechanics ({} left on this map; .mech clear all ends them too).",
                cleared, reg->Size());
        return true;
    }

    static bool HandleList(ChatHandler* handler)
    {
        Map* map = handler->GetPlayer()->GetMap();
        Registry const* reg = Registry::Get(map);
        Milliseconds const now = GameTime::GetGameTimeMS();
        uint32 shown = 0;
        if (reg)
            reg->ForEachActive(now, [&](ActiveMechanic const& m)
            {
                ++shown;
                handler->PSendSysMessage("#{} {} {} {} tag {} | resolve {:+} ms, expire {:+} ms | {} markers{}{}{}{}",
                    m.id, KindName(m.kind), DescribeShape(m.shape), ColorName(m.color), m.tag,
                    (m.resolveAt - now).count(), (m.expireAt - now).count(), m.markers.size(),
                    m.projectile ? " projectile" : "", m.mine ? (m.mine->armed ? " mine(armed)" : " mine") : "",
                    m.persist ? " persistent" : "", m.decoy ? " decoy" : "");
            });
        handler->PSendSysMessage("{} active mechanic(s) on this map.", shown);
        return true;
    }

    // Every live marker within range (default 100 yd) of the GM, one line each: the server's side of what is drawn.
    static bool HandleMarkers(ChatHandler* handler, Optional<float> range)
    {
        Player* gm = handler->GetPlayer();
        float const r = range && *range > 0.f ? *range : 100.f;
        std::vector<std::string> const lines = Telegraph::DescribeMarkers(gm->GetMap(), gm->GetPosition(), r);
        for (std::string const& line : lines)
            handler->PSendSysMessage("{}", line);
        handler->PSendSysMessage("{} marker(s) within {:.0f} yd. GM at {:.2f} {:.2f} {:.2f} o {:.3f}.", lines.size(),
            r, gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation());
        return true;
    }

    static bool HandleLedger(ChatHandler* handler, Optional<std::string> name)
    {
        Map* map = handler->GetPlayer()->GetMap();
        Registry const* reg = Registry::Get(map);
        ObjectGuid filter;
        if (name)
        {
            Player* p = ObjectAccessor::FindPlayerByName(*name, false);
            if (!p)
            {
                handler->SendErrorMessage("No player named '{}'", *name);
                return false;
            }
            filter = p->GetGUID();
        }
        uint32 encounters = 0;
        if (reg)
            reg->ForEachEncounter([&](Encounter const& e)
            {
                ++encounters;
                handler->PSendSysMessage("Encounter {}: {} player(s) with failures, {} group failure(s), {} event(s)",
                    e.owner.ToString(), e.ledger.size(), e.groupFailures, e.events.size());
                for (auto const& [player, l] : e.ledger)
                {
                    if (!filter.IsEmpty() && player != filter)
                        continue;
                    std::string tags;
                    for (auto const& [tag, n] : l.byTag)
                        tags += Acore::StringFormat(" {}:{}", tag, n);
                    handler->PSendSysMessage("  {}: {} failure(s) | by tag{}", PlayerName(player), l.total, tags);
                }
                if (filter.IsEmpty())
                    return;
                uint32 shownEvents = 0;
                for (auto it = e.events.rbegin(); it != e.events.rend() && shownEvents < 10; ++it)
                {
                    if (it->player != filter)
                        continue;
                    ++shownEvents;
                    handler->PSendSysMessage("    {} mechanic #{} (tag {}, {}) damage {}", FailureTypeName(it->type),
                        it->mechanicId, it->tag, KindName(it->kind), it->damage);
                }
            });
        if (!encounters)
            handler->PSendSysMessage("No encounters on this map.");
        return true;
    }

    static bool HandleWeakened(ChatHandler* handler, uint8 stacks)
    {
        Player* target = handler->getSelectedPlayer();      // the GM when no player is selected
        // IsInWorld first: a player between maps has no map (GetMap asserts).
        if (!target || !target->IsInWorld() || target->GetMap() != handler->GetPlayer()->GetMap())
        {
            handler->SendErrorMessage("Select a player on this map");
            return false;
        }
        if (stacks > Ids::WEAKENED_MAX_STACKS)
        {
            handler->SendErrorMessage("Weakened stacks are 0-{}", Ids::WEAKENED_MAX_STACKS);
            return false;
        }
        Weakened::SetStacks(target, stacks);
        // Recorded on the test encounter, so `.mech clear` takes the stacks off again (STAGE-M3-R-REPORT m-14).
        if (stacks)
            if (Creature* owner = GetOrSummonTestOwner(handler->GetPlayer()))
                Registry::GetOrCreate(owner->GetMap()).BeginEncounter(owner->GetGUID()).weakenedPlayers.insert(
                    target->GetGUID());
        handler->PSendSysMessage("{}: Weakened {} stack(s) (damage done x{:.1f}, threat +{}%).", target->GetName(),
            Weakened::GetStacks(target), WeakenedDamageMultiplier(stacks), WeakenedThreatPct(stacks));
        return true;
    }

    static bool HandlePalette(ChatHandler* handler, std::string getOut, std::string getIn,
        Optional<std::string> crossLanes, Optional<std::string> cells)
    {
        EncounterOptions opt;
        std::optional<Color> out = ParseColor(getOut);
        std::optional<Color> in = ParseColor(getIn);
        std::optional<Color> cross = crossLanes ? ParseColor(*crossLanes) : std::optional<Color>(opt.crossLanes);
        std::optional<Color> cell = cells ? ParseColor(*cells) : std::optional<Color>(opt.cells);
        if (!out || !in || !cross || !cell)
        {
            handler->SendErrorMessage("Usage: .mech palette <get-out color> <get-in color> [cross-lane color] "
                "[cell color]; colors: red yellow blue purple orange green teal white black");
            return false;
        }
        Creature* owner = GetOrSummonTestOwner(handler->GetPlayer());
        if (!owner)
        {
            handler->SendErrorMessage("Could not summon the Mechanic Test Owner");
            return false;
        }
        opt.getOut = *out;
        opt.getIn = *in;
        opt.crossLanes = *cross;
        opt.cells = *cell;
        Mechanic::BeginEncounter(owner, opt);
        handler->PSendSysMessage("Test palette: get-out {}, get-in {}, cross lanes {}, cells {} (applies to new test "
            "mechanics).", ColorName(*out), ColorName(*in), ColorName(*cross), ColorName(*cell));
        return true;
    }
};

void AddSC_boss_mechanics_command()
{
    new boss_mechanics_commandscript();
}
