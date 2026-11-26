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
 *               |stratholme-followups|stratholme-drops|stratholme-jump|blackrock-spire-followups
 *               |blackrock-depths-shapes|blackrock-depths-shapes-2|scarlet-monastery-shapes
 *               |scarlet-monastery-shapes-2|ragefire-chasm-shapes|zul-farrak-shapes|gnomeregan-shapes> [cycles=3]
 *               [seed=] [dmg=1] [self=0]
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
 * Stratholme build X2 (drill `stratholme-drops`): `drop=<edge,away,behind,prefer,avoid> [room=<yd>] [dwell=<ms>]` on
 * a circle anchor=member or a line to=member gives the member's bot a DropSpotRule (MechanicDropSpot.h; the
 * RegisterTest `dropSpot` lambda): a circle room around the selected unit, the GM's facing as the travel direction,
 * and the test owner's live persistent point circles as enterable preferred spots (`prefer`) or as avoid points
 * (`avoid`, the Ultimatum's bone piles).
 *
 * Stratholme build X3 (drill `stratholme-jump`): `jump <r> <ms>` is a Mechanic::Jump circle of r on the anchor: at the
 * end of the warning it hits every scope member inside who is on the ground (damage keys as a circle); whoever is in
 * the air is spared, and bots hop for it (MechanicJump.h).
 *
 * Blackrock Depths build WP-HK (the `hide` row of drill `blackrock-depths-shapes`): `hide <range> <ms> [blockers=3]
 * [apart=8] [r=2.5] [display=3711] [go=148503] [behind=3] [share=2] [eye=<yd>] [linger=3000]` summons `blockers` game
 * objects on a ring of `apart` yd round the selected creature (else the test owner), the first along the GM's facing,
 * sets their display, and registers a Mechanic::Hide from that creature over them (MechanicHide.h): at the end of the
 * warning every scope member within `range` it can see is hit (damage keys as a circle); bots hide behind the
 * blockers. The blockers despawn `linger` ms after the resolve.
 *
 * Blackrock Spire build (WP-G1 §3.9, drill `blackrock-spire-followups`): every command (and drill run) keeps the far
 * ends of its last fixed lines (Dice::ends, Drill::LineEnds): `at=last:<n>` lands on the end of the line n before the
 * last (a shape placed after a later line), and `to=last[:<n>]` aims a line at such an end, frozen like fix=1
 * and recording no end of its own (WT11's haul and VO12's recall along an axe chain).
 *
 * Blackrock Spire build (WP-RF, drill `rolling-fronts`): `conefront <end> <ms>` is a rolling cone front from the
 * anchor point (Mechanic::ConeFront; slice=, bearing=, turn=, start=, band=, speed=, park=1 until=) and `linefront
 * <travel> <ms>` a rolling line front across the anchor point (Mechanic::LineFront; width=, band=, speed=, gap=,
 * side=, apart=, facing=); `turn=` and `side=other` read the previous front of the command or drill run (Dice).
 *
 * Blackrock Spire build (WP-G2 §3.9, drill `blackrock-spire-followups` rows 18-30): `next=<deg>` on a conefront is
 * the turn hint to the next ring (ConeFrontSpec::nextTurnDeg), `edge=<yd>` on a linefront keeps the gap that far
 * inside the wall's ends; on the shapes, `inside=<yd>` gives the bots hard bounds (Within) of a circle round the
 * selected unit where it stands at registration, `lethal=1` marks a circle, donut, cone or line Lethal(), and
 * `only=member` makes the shape eligible to one player, drawn with pick= (the challenger of Drakkisath's ring). A
 * cone takes `fix=1` with to= (aimed once, at registration).
 *
 * Blackrock Depths build (WP-G1 §3.9, drill `blackrock-depths-shapes`): `offset=<deg>` on a line turns its fixed
 * facing (and shift='s axes) counterclockwise, and two drill-only steps (RunDrillWith, MechanicDrill.h "Blackrock
 * Depths shapes"): `walk <yd> <ms> [offset=]` relocates the selected creature along the run's facing (ScheduleWalk; a
 * boss with combat movement off) and `leash <yd> <ms> [role=tank|healer|nonhealer] [anchor=tank] [tick=] tag=<n>`
 * posts an OffSpot failure for each scope member left farther than <yd> from it, or with anchor=tank from its nearest
 * alive tank (ScheduleLeash; nothing registers, so the bots never see it).
 *
 * Blackrock Depths build (WP-G2 §3.9, drill `blackrock-depths-shapes-2`): `soak <width> <ms> shape=line
 * to=target|member|me` is a beam from the anchor to that unit (Shapes::Line(from, to, width): it turns with the unit,
 * its length is the distance at registration), and `at=target` lands a point where the selected unit stands as the
 * repeat registers (a drill parses its steps at the start, so a plain point stays where a walking boss started).
 *
 * Scarlet Monastery build (SM-BM; drills `scarlet-monastery-shapes` rows torment and glare build on these):
 * `gaze <range> <ms> [arc=180] [lead=<ms>]` is a Mechanic::Gaze from the selected creature (else the test owner): at
 * the end of the warning every scope member within range who faces it (the source inside the front arc) is hit
 * (damage keys as a circle); bots turn away over the look window (MechanicGaze.h). On a `soak <width> <ms> shape=line
 * to=...` beam, `backoff=<yd>` is EndBacksOff (the to= member's bot walks back along it to that far from the source),
 * `tanknear=<yd>` is TankNearSource (a tank claimer soaks the first yd of it) and `exempt=target` leaves the to= unit
 * out of the soak; `last=tank|healer|dps|nontank` on a soak is Ordering::roleLast (that role claims last; with or
 * without order=); `holdfacing=1` on any shape is HoldTankFacing(the selected unit) (the tank does not re-face it
 * while the shape lives: a spot in its frontal); and spread and runout take persist=<ms> tick=<ms> (Persistent: every
 * carrier's circle judged at every tick).
 *
 * Scarlet Monastery build (WP-G1 §3.9, drill `scarlet-monastery-shapes`): `pick=tank` picks the selected unit's
 * victim (none: anyone; Torment on the tank), `leash ... near=1` fails a member while it stands closer than <yd>
 * (the targeted tank that did not back off), the drill-only step `chase <yd> <ms> [home=1]` makes the selected
 * creature step after its victim like an unrooted boss (ScheduleChase; the sim's stand-in is rooted, so a tank's
 * drag-out would leave it behind), and a drill's `marker <n>` remembers the marked member as the last pick, so a
 * later `pick=same` lands on it (the Mark carrier's trail; MechanicDrill.h "Scarlet Monastery shapes").
 *
 * Scarlet Monastery build (WP-G2 §3.9, drill `scarlet-monastery-shapes-2`): `along=target` turns a point's axes
 * (shift=, a line's facing) to the selected unit's own facing as the repeat registers (Thorne's furrow ahead of him),
 * `past=<yd>` runs a reach=1 line that far past its to= unit (Crell's dash), `shift=` on a fixed to= line moves its
 * source before it is aimed (a blade thrown from Herod's ledge), `leash ... anchor=self` judges each member against
 * where it stood as the leash started, the drill-only step `holdfacing <ms>` is Mechanic::HoldTankFacing(the test
 * owner, the selected unit, ms), and the drill runner now honors a step's `repeat=` / `every=` (it parsed and
 * dropped them: `pass-eggs` laid one egg per cycle), keeping the step's tag on every repeat (MechanicDrill.h
 * "repeat= and every= on a drill step").
 *
 * Ragefire Chasm build (WP-G1 §3.9, drill `ragefire-chasm-shapes`): `mound <r> <arm ms> persist=<ms>` is Oggleflint's
 * Borer (a chaser with a no-damage circle only the hunted member sees, then an eruption circle where it stops:
 * RegisterMound) and `crawl <r> <life ms> yd=<yd>` Jergosh's wisp (a telegraph-only Soak on a unit crawling to a point,
 * taken on touch: RegisterCrawl); `pick=fresh` picks a member, the tank included, the chain has not picked;
 * `inside=<yd>:<ahead>` fixes the bots' bounds on the drill's start; `hide of=<n> scale=<s>` stands the blockers on
 * random points of n and scales them; `marker 0 one=1` takes Marked off one holder; and two drill-only steps: `blink`
 * (the selected creature appears behind a member, on the last circle's centre or back home: ScheduleBlink) and `cast`
 * (it casts a spell, kickable or a must-play bar, judged near its end: ScheduleCast). MechanicDrill.h "Ragefire Chasm
 * shapes".
 *
 * Zul'Farrak build (WP-G1 §3.9, drill `zul-farrak-shapes`): `inbox=<w>x<l>[:<ahead>]` gives the bots hard bounds of a
 * box fixed on the selected unit's place at the parse, `crawl from=<yd>:<bearing>` starts the crawl at a fixed point,
 * `mark=<n>` on a soak or crawl hands whoever fills it n Marked stacks, `pick=marked|unmarked|healer`, `circle
 * around=<yd>:<n>` lays n circles on a ring, `circle drop=prefer judge=prefer` fails a carrier off every lasting point
 * circle, `cast twin=1` adds a second caster, and four drill-only steps: `afflict` (a dispellable debuff held on a
 * crowded carrier, a dispel near others a Hit: ScheduleAfflict), `partner` (a twin and a kill order flipping between
 * the two: SchedulePartner), `stalk` (the selected unit walks at members, a trail behind: ScheduleStalk) and `floes`
 * (strict bounds of safe circles: ScheduleFloes). MechanicDrill.h "Zul'Farrak shapes".
 *
 * Zul'Farrak build (BM-1, for drill `zul-farrak-shapes` rows 3 and 4): `seen=<type>` on any shape that draws is
 * SeenOnlyWith(type) (its markers carry invisibility of that type: only a player with a detect aura of it sees them;
 * the GM needs one to see the shape), and `healreach=1` on a soak or crawl is HealerWithinTankReach (no bot healer
 * claims it while it lies past healer reach of the healer's nearest live tank).
 *
 * Gnomeregan build (WP-G1 §3.9, drill `gnomeregan-shapes`): `pick=marked:<n>` picks the holder of exactly n Marked
 * stacks (`marker <n> pick=unmarked` marks a member without it), `circle exempt=member` is eligible to everyone but the
 * member of its pick=, `cast nokick=1` lists the spell as never kicked, `cast twin=1 kill=<n>` makes the twin a kill
 * target, and three drill-only steps: `lights` (turning cones on the selected unit with a Spotted judge and the swept
 * floor for the bots: ScheduleLights), `movecost` (BM-G's move cost and ranged band on the descriptor for a while:
 * ScheduleMoveCost) and `saturated` (the selected unit in an armed lasting pool fails its tank: ScheduleSaturated).
 * MechanicDrill.h "Gnomeregan shapes".
 *
 * Commands run on the world thread while no map updates (handoff §2.1), so they may write the registry directly.
 */

#include "BossMechanics.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "GameObject.h"
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
#include "Spell.h"
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
    // that was not `pick=other`; `ends` the far ends of the last fixed lines registered (`at=last[:<n>]`, `to=last`,
    // Drill::LineEnds; a line aimed with to=last records none); `lastCircle` the
    // centre of the last point-anchored circle registered (`at=prev`); `hunt` the last `hunt` registered (a `soak
    // bell=` ends it).
    struct Dice
    {
        explicit Dice(uint32 seed) : rng(seed) { }

        Barrage::Rng rng;
        ObjectGuid lastMember;
        std::vector<ObjectGuid> chain;
        Drill::LineEnds<Position> ends;
        std::optional<Position> lastCircle;
        HuntPtr hunt;
        // WP-RF: the slice bearing (radians) of the last `conefront` and the gap centre of the last `linefront`
        // (`turn=`, `side=other`, `apart=`).
        std::optional<float> lastSlice;
        std::optional<float> lastGap;
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

    // `order=<spell>[:asc|desc][:cap=<n>][:min=<n>]` (min: Gnomeregan BM-G, Ordering::minStacks).
    bool ParseOrdering(std::string const& s, Ordering& o)
    {
        std::vector<std::string_view> const f = Fields(s);
        Optional<uint32> spell = f.empty() ? std::nullopt : Acore::StringTo<uint32>(f[0]);
        if (!spell || !*spell || f.size() > 4)
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
            else if (f[i].substr(0, 4) == "min=")
            {
                Optional<uint32> min = Acore::StringTo<uint32>(f[i].substr(4));
                if (!min || *min > 255)
                    return false;
                o.minStacks = static_cast<uint8>(*min);
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
        bool atLast = false;            // at=last[:<n>]: the end of a recent fixed line (Dice::ends)
        uint32 atBack = 0;              // at=last:<n>: n lines before the last (0: the last)
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
            Other,              // a non-tank the chain has not picked (Drill::PickPool)
            Tank,               // the selected unit's victim, else a random member (Scarlet Monastery WP-G1)
            Fresh,              // a member, the tank included, the chain has not picked (Ragefire Chasm WP-G1)
            Marked,             // a member holding Marked (drill), else anyone (Zul'Farrak WP-G1: the walker)
            Unmarked,           // a member without it, else anyone (the living)
            Healer,             // a healer by the role provider, else anyone (the healer walker)
            MarkedAt            // pick=marked:<n> (Gnomeregan WP-G1): a member holding exactly n stacks, else anyone
        };
        enum class To : uint8
        {
            None,
            Member,
            Target,
            Me,
            Last                        // to=last[:<n>] (line): a recorded line end (Dice::ends), frozen
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
        Milliseconds dropDwell{ 800 };
        bool atPredict = false;         // at=predict: where a member will be `lead` ms from now
        Milliseconds lead{ Drill::kDefaultPredictLeadMs };
        // Stratholme build (WP-G §3.9, drill `stratholme-followups`)
        bool exemptTo = false;          // exempt=target: the to= unit is not eligible (BZ1; RegisterRepeat)
        // Blackrock Spire build (WP-G1 §3.9, drill `blackrock-spire-followups`)
        uint32 toBack = 0;              // to=last:<n>: the end n lines before the last (0: the last)
        // Blackrock Spire build WP-RF (rolling fronts; drill `rolling-fronts`)
        float sliceDeg = 60.f;          // conefront slice=: the safe slice's width (0: a full ring)
        std::optional<float> bearing;   // conefront bearing=: the slice's middle, degrees from the GM's facing
        std::optional<float> turn;      // conefront turn=: clockwise from the previous conefront's slice (degrees)
        float frontStart = 0.f;         // conefront start=: the band's inner edge at launch
        float band = 4.f;               // band=: the band's thickness
        float frontSpeed = 8.f;         // speed=
        float untilYd = 0.f;            // conefront park=1 until=: the FrontSafe zone ends once the band passes this
        float sliceAbs = 0.f;           // conefront: the slice's middle (radians), set when the repeat registers
        float frontWidth = 40.f;        // linefront width=: the wall's length across
        float gapWidth = 8.f;           // linefront gap=: the gap's width (0: no gap)
        int gapSide = 0;                // linefront side=left|right (1 / -1), other (2), any (0)
        float gapApart = -1.f;          // linefront apart=: at most this far from the previous gap (< 0: anywhere)
        float frontFacing = 0.f;        // linefront facing=: degrees from the GM's facing
        float gapCentre = 0.f;          // linefront: the gap's centre, set when the repeat registers
        // Blackrock Spire build WP-G2 (drill `blackrock-spire-followups` rows 18-30)
        float nextTurn = 0.f;           // conefront next=: ConeFrontSpec::nextTurnDeg (the turn to the next ring)
        float gapEdge = 0.f;            // linefront edge=: the gap's whole width this far inside the wall's ends
        float insideYd = 0.f;           // inside=: Within a circle of this round the selected unit (0: none)
        Position insideCentre;          // inside=: the selected unit's spot, set when the repeat registers
        bool lethal = false;            // lethal=1: MechanicRef::Lethal()
        bool onlyMember = false;        // only=member: eligible to one member (pick=), players only
        // Blackrock Depths build WP-HK (`hide`, MechanicHide.h): the blockers summoned round the source
        uint32 hideBlockers = 3;        // blockers=: how many (0-8)
        float hideApart = 8.f;          // apart=: their ring round the source (yd)
        float hideRadius = Hide::kDefaultBlockerRadiusYd;   // r=: each footprint (yd)
        uint32 hideGo = Ids::GO_HIDE_TEST_BLOCKER;          // go=: the game object entry summoned
        uint32 hideDisplay = Hide::kDisplayLavaRock;        // display=: its display (0: the template's)
        float hideBehind = Hide::kDefaultBehindYd;          // behind=
        uint8 hideShare = Hide::kDefaultShare;              // share=
        float hideEye = -1.f;           // eye=: the source's eye height (< 0: its collision height)
        Milliseconds hideLinger{ 3000 };    // linger=: the blockers stay this long after the resolve
        // Blackrock Depths build WP-G2 (drill `blackrock-depths-shapes-2`)
        bool atTarget = false;          // at=target: where the selected unit stands as the repeat registers
        // Scarlet Monastery build SM-BM
        float gazeArc = Gaze::kDefaultArcDeg;   // gaze arc=: the front arc that counts as facing (degrees)
        Milliseconds gazeLead{ 0 };             // gaze lead=: the bots' look window before the resolve (0: all)
        float backOff = 0.f;            // soak backoff=: EndBacksOff (a to= beam)
        float tankNear = 0.f;           // soak tanknear=: TankNearSource (a beam or lane)
        bool holdFacing = false;        // holdfacing=1: HoldTankFacing(the selected unit)
        // Scarlet Monastery build WP-G2 (drill `scarlet-monastery-shapes-2`)
        bool alongTarget = false;       // along=target: a point's axes follow the selected unit's facing
        float past = 0.f;               // line past=: a reach=1 line runs this far past the to= unit
        // Ragefire Chasm build WP-G1 (drill `ragefire-chasm-shapes`)
        bool insideFixed = false;       // inside=<yd>:<ahead>: insideCentre set at the parse, never moved
        float moundSpeed = Drill::kMoundSpeedYdS;   // mound speed= (yd/s)
        float moundErupt = Drill::kEruptYd;         // mound erupt=: the eruption's radius
        Milliseconds moundBoom{ Drill::kEruptMs };  // mound boom=: the eruption's warning
        float crawlYd = 0.f;            // crawl yd=: the goal, this far from the selected unit's start
        float crawlBearing = 0.f;       // crawl bearing=: degrees from the run's facing, counterclockwise
        float crawlOff = Drill::kWispOffYd;         // crawl off=: it starts this far from the picked member
        float crawlSpeed = Drill::kWispSpeedYdS;    // crawl speed= (yd/s)
        uint32 hideOf = 0;              // hide of=: the blockers stand on random points of this many (0: all of them)
        std::vector<uint32> hideSlots;  // ... which, drawn when the repeat registers
        float hideScale = 1.f;          // hide scale=: the game objects' scale
        // Zul'Farrak build WP-G1 (drill `zul-farrak-shapes`)
        bool inBox = false;             // inbox=: Within a box fixed at the parse (SpotBounds::Box)
        Position boxCorner;
        float boxFacing = 0.f;
        float boxWidth = 0.f;
        float boxLength = 0.f;
        std::optional<Drill::PolarSpec> crawlFrom;  // crawl from=<yd>:<bearing>: the start, off the selected unit
        uint32 markStacks = 0;          // soak / crawl mark=<n>: Marked stacks for whoever fills it
        uint32 around = 0;              // circle around=<yd>:<n>: n circles on a ring of aroundYd
        float aroundYd = 0.f;
        bool dropJudge = false;         // circle drop= judge=prefer: the carrier off every lasting point circle fails
        // Zul'Farrak BM-1
        int seen = -1;                  // seen=<type>: SeenOnlyWith(type) (markers seen with that detect aura only)
        bool healReach = false;         // soak / crawl healreach=1: HealerWithinTankReach
        // Gnomeregan build WP-G1 (drill `gnomeregan-shapes`)
        uint32 pickStacks = 0;          // pick=marked:<n>: the stacks a MarkedAt pick looks for
        bool exemptMember = false;      // circle exempt=member: everyone but the member of the step's pick=
    };

    bool IsShapeName(std::string const& s)
    {
        static char const* const shapes[] = { "circle", "donut", "cone", "rearcone", "line", "projectile", "mine",
            "lanes", "spread", "runout", "stack", "soak", "frontal", "bait", "holdstill", "spot", "hunt", "jump",
            "conefront", "linefront", "hide", "gaze", "mound", "crawl" };
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
        // WP-G2: lethal=1 in the registering statement (Lethal is refused after the commit), inside= as hard bounds.
        if (spec.lethal)
            ref.Lethal();
        if (spec.insideYd > 0.f)
            ref.Within(SpotBounds().Circle(spec.insideCentre, spec.insideYd));
        // Zul'Farrak WP-G1: inbox= (X along the run's facing for the length, Y to its left for the width).
        if (spec.inBox)
            ref.Within(SpotBounds().Box(spec.boxCorner, spec.boxFacing, spec.boxLength, spec.boxWidth));
        if (spec.ordering)
            ref.Order(*spec.ordering);
        if (spec.color)
            ref.Color(*spec.color);
        if (!spec.tele)
            ref.NoTelegraph();
        // Zul'Farrak F11: seen=<type> (in the registering statement: the markers spawn at the commit).
        if (spec.seen >= 0)
            ref.SeenOnlyWith(static_cast<uint8>(spec.seen));
        ref.Tag(spec.tag);
    }

    // mark=<n> (Zul'Farrak WP-G1): n more Marked (drill) stacks on `p` (a Gathered Soul, the Block's Grievous Wound),
    // never past the spell's cap, recorded on the owner's encounter so its end (and `.mech clear`) takes them off.
    void AddMarked(Creature* owner, Player* p, uint32 n)
    {
        if (!n || !p || !p->IsInWorld() || !p->IsAlive())
            return;
        Aura* aura = p->GetAura(Ids::SPELL_MARKED_DRILL, p->GetGUID());
        uint32 const have = aura ? aura->GetStackAmount() : 0;
        if (!aura)
            aura = p->AddAura(Ids::SPELL_MARKED_DRILL, p);
        if (!aura)
            return;
        aura->SetStackAmount(static_cast<uint8>(Drill::MarkedAfter(have, n)));
        aura->RefreshDuration();
        Registry::GetOrCreate(owner->GetMap()).BeginEncounter(owner->GetGUID()).markedPlayers.insert(p->GetGUID());
    }

    // A drill failure the registry did not judge (leash, the floes, the kill order, a shatter): the ledger's
    // `<Kind>/<tag>` on the owner's encounter.
    void PostDrillFailure(Creature* owner, uint32 tag, ObjectGuid player, FailureEvent::Type type)
    {
        FailureEvent e;
        e.encounter = owner->GetGUID();
        e.tag = tag;
        e.kind = Kind::Zone;
        e.player = player;
        e.type = type;
        e.at = GameTime::GetGameTimeMS();
        Registry::GetOrCreate(owner->GetMap()).PostFailure(e);
    }

    // Is `pos` inside a live lasting point circle of `owner` (the drop rule's `prefer` spots: the pool's puddles)?
    bool InOwnerPuddle(Creature* owner, Position const& pos)
    {
        bool inside = false;
        if (Registry const* reg = Registry::Get(owner->GetMap()))
            reg->ForEachActive(GameTime::GetGameTimeMS(), [&inside, &pos, owner](ActiveMechanic const& m)
            {
                if (inside || m.owner != owner->GetGUID() || !m.persist || m.decoy || m.anchor.IsUnit()
                    || !m.IsShapeKind() || m.shape.type != ShapeType::Circle)
                    return;
                inside = m.anchor.point.GetExactDist2d(&pos) <= m.shape.radius;
            });
        return inside;
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
        // selected unit, the GM's facing as the travel direction, and every live persistent circle on a point of this
        // test owner (the drill's old webs) as an enterable preferred spot with `prefer`, an avoid point with `avoid`.
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
            if (spec.drop->prefer || spec.drop->avoid)
                if (Registry const* reg = Registry::Get(owner->GetMap()))
                    reg->ForEachActive(GameTime::GetGameTimeMS(), [&rule, &spec, owner](ActiveMechanic const& m)
                    {
                        if (m.owner != owner->GetGUID() || !m.persist || m.decoy || m.anchor.IsUnit()
                            || !m.IsShapeKind() || m.shape.type != ShapeType::Circle)
                            return;
                        if (spec.drop->prefer)
                            rule.Prefer(m.anchor.point, m.shape.radius, m.id);
                        if (spec.drop->avoid)
                            rule.Avoid(m.anchor.point, m.shape.radius);
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
            // judge=prefer (Zul'Farrak WP-G1, GZ6): the carrier (the anchor member) off every lasting point circle of
            // the owner as it resolves is an OffSpot failure under DropJudgeTag(tag): the Icicle not in a puddle.
            if (spec.dropJudge && anchorUnit)
            {
                Map* map = owner->GetMap();
                ref.OnResolve([map, ownerGuid = owner->GetGUID(), carrier = anchorUnit->GetGUID(), tag = spec.tag](
                    ResolveResult const&)
                {
                    Creature* o = map->GetCreature(ownerGuid);
                    Unit* unit = o ? ObjectAccessor::GetUnit(*o, carrier) : nullptr;
                    if (!o || !unit || !unit->IsAlive() || InOwnerPuddle(o, unit->GetPosition()))
                        return;
                    PostDrillFailure(o, Drill::DropJudgeTag(tag), carrier, FailureEvent::Type::OffSpot);
                });
            }
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
            ApplyCommon(ref, spec, true, true);     // persist= (Scarlet Monastery F15): a lasting spread
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
            ApplyCommon(ref, spec, true, true);     // persist= (Scarlet Monastery F15): a lasting runout
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
            // shape=line to= (Blackrock Depths WP-G2): a beam from the anchor to that unit, Shapes::Line(from, to):
            // it turns with the unit live, its length is the distance now.
            std::optional<ZoneSpec> beam;
            if (spec.lane && spec.to != TestSpec::To::None)
            {
                Unit* toUnit = ObjectAccessor::GetUnit(*owner, spec.toUnit);
                if (!toUnit)
                {
                    err = "the to= unit is gone";
                    return ids;
                }
                if (here.GetExactDist2d(toUnit) < 1.f)
                {
                    err = "the beam's ends are within 1 yd of each other";
                    return ids;
                }
                beam = Shapes::Line(anchor, Anchor(toUnit), spec.width);
            }
            MechanicRef ref = beam ? Mechanic::Soak(owner, *beam, spec.need, spec.warn)
                : spec.lane ? Mechanic::Soak(owner, laneSpec(), spec.need, spec.warn)
                : Mechanic::Soak(owner, anchor, spec.size, spec.need, spec.warn);
            // Scarlet Monastery F2: backoff= (a beam's to= end walks back), tanknear= (the tank at the source's end).
            if (spec.backOff > 0.f)
                ref.EndBacksOff(spec.backOff);
            if (spec.tankNear > 0.f)
                ref.TankNearSource(spec.tankNear);
            if (spec.healReach)
                ref.HealerWithinTankReach();    // Zul'Farrak F14
            ApplyCommon(ref, spec, false, true);
            // mark=<n> (Zul'Farrak WP-G1): a filled soak hands the players in it n Marked stacks (the Block's wound).
            if (spec.markStacks)
            {
                Map* map = owner->GetMap();
                ref.OnResolve([map, ownerGuid = owner->GetGUID(), n = spec.markStacks, need = spec.need](
                    ResolveResult const& r)
                {
                    Creature* o = map->GetCreature(ownerGuid);
                    if (!o || r.fizzled || r.count < need)
                        return;
                    for (ObjectGuid const& g : r.hits)
                        AddMarked(o, ObjectAccessor::GetPlayer(*o, g), n);
                });
            }
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
        else if (s == "jump")
        {
            // Stratholme X3: a circle of `size` that sweeps at the end of the warning; whoever is in the air is spared.
            MechanicRef ref = Mechanic::Jump(owner, anchor, spec.size, spec.warn);
            ApplyCommon(ref, spec, true, false);
            keep(ref);
        }
        else if (s == "hide")
        {
            // Blackrock Depths WP-HK (MechanicHide.h): the selected creature (else the test owner) is the source;
            // `blockers` game objects on a ring of `apart` yd round it, the first along the GM's facing, each with
            // `display`; a Hide of range `size` from it over them. The blockers despawn `linger` after the resolve.
            Unit* source = ObjectAccessor::GetUnit(*owner, spec.selected);
            if (!source || !source->IsCreature() || !source->IsInWorld() || source->GetMap() != owner->GetMap())
                source = owner;
            HideSpec hide{ Anchor(source) };
            hide.Range(spec.size).Behind(spec.hideBehind).Share(spec.hideShare);
            if (spec.hideEye >= 0.f)
                hide.Eye(spec.hideEye);
            // The Boulders outlive the resolve by at least kHideResolveSlack, even at linger=0: a despawn on the
            // resolve's own map tick could run first (GameObject::Update before the registry), and the resolve would
            // find no blocker and read everyone as Seen (R-FIXES MN-4).
            Milliseconds constexpr kHideResolveSlack(500);
            Milliseconds const life = spec.warn + std::max(spec.hideLinger, kHideResolveSlack);
            uint32 const lifeSecs = static_cast<uint32>((life.count() + 999) / 1000);
            for (uint32 i = 0; i < spec.hideBlockers; ++i)
            {
                // of=<n> (Ragefire Chasm WP-G1): on the drawn points of n, else the blockers' own ring.
                bool const drawn = spec.hideOf && i < spec.hideSlots.size();
                float const a = Geo::WrapPi(spec.facing + Geo::kTwoPi * static_cast<float>(drawn ? spec.hideSlots[i]
                    : i) / static_cast<float>(drawn ? spec.hideOf : spec.hideBlockers));
                float const x = source->GetPositionX() + std::cos(a) * spec.hideApart;
                float const y = source->GetPositionY() + std::sin(a) * spec.hideApart;
                float const z0 = source->GetPositionZ();
                float const ground = owner->GetMap()->GetHeight(owner->GetPhaseMask(), x, y, z0 + 2.f, true, 10.f);
                float const z = ground > INVALID_HEIGHT && std::fabs(ground - z0) <= 5.f ? ground : z0;
                GameObject* go = owner->SummonGameObject(spec.hideGo, x, y, z, a, 0.f, 0.f, 0.f, 0.f, lifeSecs);
                if (!go)
                {
                    err = Acore::StringFormat("game object {} could not be summoned (go=)", spec.hideGo);
                    return ids;
                }
                if (spec.hideScale != 1.f)
                    go->SetObjectScale(spec.hideScale);     // the collision model reads it when it is recreated below
                uint32 const display = spec.hideDisplay ? spec.hideDisplay : go->GetDisplayId();
                if (go->GetDisplayId() != display || spec.hideScale != 1.f)
                    go->SetDisplayId(display);      // recreates its collision model (GameObject::UpdateModel)
                // The respawn timer above runs on GameTime::GetGameTime(), wall-clock seconds even under the sim's
                // clock override, so in an accelerated sim a 7 s Boulder outlived its run by minutes of sim time and
                // stood on the next iteration's Roccor road, breaking the bots' line of sight to the walking boss
                // (Roccor chase report). The despawn delay counts down in GameObject::Update's diff: the map's clock.
                // That delete relies on the default entry's type (148503, SPELL_FOCUS): a `go=` CHEST or GOOBER has a
                // respawn time, so the despawn only resets it to READY (gone for clients, still blocking LOS on the
                // server, and back on the wall clock in a sim). Use a non-lootable, non-usable entry for go=.
                go->DespawnOrUnsummon(life);
                hide.Blocker(go->GetGUID(), go->GetPosition(), spec.hideRadius);
            }
            MechanicRef ref = Mechanic::Hide(owner, hide, spec.warn);
            ApplyCommon(ref, spec, true, false);
            keep(ref);
        }
        else if (s == "gaze")
        {
            // Scarlet Monastery F13 (MechanicGaze.h): the selected creature (else the test owner) is the source; at the
            // end of the warning every scope member within `size` yd who faces it is hit.
            Unit* source = ObjectAccessor::GetUnit(*owner, spec.selected);
            if (!source || !source->IsCreature() || !source->IsInWorld() || source->GetMap() != owner->GetMap())
                source = owner;
            GazeSpec gaze{ Anchor(source) };
            gaze.Range(spec.size).Arc(spec.gazeArc).Lead(spec.gazeLead);
            MechanicRef ref = Mechanic::Gaze(owner, gaze, spec.warn);
            ApplyCommon(ref, spec, true, false);
            keep(ref);
        }
        else if (s == "conefront")
        {
            // WP-RF: a rolling cone front from the anchor point out to `size` yd, its slice at spec.sliceAbs.
            ConeFrontSpec front;
            front.sliceBearing = spec.sliceAbs;
            front.sliceDeg = spec.sliceDeg;
            front.startYd = spec.frontStart;
            front.endYd = spec.size;
            front.bandYd = spec.band;
            front.speed = spec.frontSpeed;
            front.nextTurnDeg = spec.nextTurn;
            MechanicRef ref = Mechanic::ConeFront(owner, here, front, spec.warn);
            ApplyCommon(ref, spec, true, false);
            keep(ref);
            // park=1: the tank leads the selected unit into the slice (Mechanic::FrontSafe).
            if (ref.IsValid() && spec.park)
                if (Unit* unit = ObjectAccessor::GetUnit(*owner, spec.selected))
                    keep(Mechanic::FrontSafe(owner, ref.Handle(), unit, spec.untilYd));
        }
        else if (s == "linefront")
        {
            // WP-RF: a wall `size` yd of travel across the anchor point (the swept area's middle), along the GM's
            // facing turned by facing=, its gap at spec.gapCentre.
            float const f = spec.facing + Geo::DegToRad(spec.frontFacing);
            Position start = here;
            start.Relocate(here.GetPositionX() - std::cos(f) * spec.size * 0.5f,
                here.GetPositionY() - std::sin(f) * spec.size * 0.5f, here.GetPositionZ());
            LineFrontSpec front;
            front.facing = f;
            front.travelYd = spec.size;
            front.widthYd = spec.frontWidth;
            front.bandYd = spec.band;
            front.speed = spec.frontSpeed;
            if (spec.gapWidth > 0.f)
                front.gaps.push_back({ spec.gapCentre, spec.gapWidth });
            MechanicRef ref = Mechanic::LineFront(owner, start, front, spec.warn);
            ApplyCommon(ref, spec, true, false);
            keep(ref);
        }
        // Scarlet Monastery F6: holdfacing=1 (a bots-only option, so it may follow the commit): the tank of the
        // selected unit (else the test owner) does not re-face it while these live.
        if (spec.holdFacing)
        {
            Unit* source = ObjectAccessor::GetUnit(*owner, spec.selected);
            if (!source || !source->IsInWorld() || source->GetMap() != owner->GetMap())
                source = owner;
            for (uint32 id : ids)
                MechanicRef(owner->GetMap(), id, false).HoldTankFacing(source);
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
    // (pick=other, Drill::PickPool), or the selected unit's victim (pick=tank, else anyone). `source` (with min=): the
    // point the min= distance is measured from. Remembered for pick=same and the chain.
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
            // Zul'Farrak WP-G1: pick=marked|unmarked (the walker, the living) and pick=healer.
            c.marked = p->HasAura(Ids::SPELL_MARKED_DRILL);
            c.stacks = p->GetAuraCount(Ids::SPELL_MARKED_DRILL);     // Gnomeregan WP-G1: pick=marked:<n>
            if (spec.pick == TestSpec::Pick::Healer)
                if (Unit* selected = spec.selected.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*owner, spec.selected))
                    c.healer = HasRole(GetRoleProvider().Get(selected, p), Role::Healer);
            candidates.push_back(c);
        }
        Drill::PickRule const rule = spec.pick == TestSpec::Pick::Any ? Drill::PickRule::Any
            : spec.pick == TestSpec::Pick::Other ? Drill::PickRule::Other
            : spec.pick == TestSpec::Pick::Tank ? Drill::PickRule::Tank
            : spec.pick == TestSpec::Pick::Fresh ? Drill::PickRule::Fresh
            : spec.pick == TestSpec::Pick::Marked ? Drill::PickRule::Marked
            : spec.pick == TestSpec::Pick::Unmarked ? Drill::PickRule::Unmarked
            : spec.pick == TestSpec::Pick::Healer ? Drill::PickRule::Healer
            : spec.pick == TestSpec::Pick::MarkedAt ? Drill::PickRule::MarkedAt : Drill::PickRule::NonTank;
        std::vector<std::size_t> const pool = Drill::PickPool(candidates, rule, source ? spec.minDist : 0.f,
            spec.pickStacks);
        if (pool.empty())
            return nullptr;
        Player* p = members[pool[dice.rng.Index(static_cast<uint32>(pool.size()))]];
        dice.lastMember = p->GetGUID();
        bool const keeps = spec.pick == TestSpec::Pick::Other || (spec.pick == TestSpec::Pick::Fresh
            && !Drill::FreshRestarts(candidates));
        if (!keeps)
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
    // does the member dying or leaving the map. The circle is players only: the member's pet in it is not a catch.
    // The chaser despawns on its own after the chase (timed summon).
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
        elig.playersOnly = true;    // the member's pet is not the catch (Drill::HuntCaught)
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
            if (Drill::HuntCaught(r.hits, h->hunted) || !chaser || !member || !member->IsAlive()
                || member->GetMap() != map)
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

    // inside= (Blackrock Spire WP-G2): the bounds' centre is the selected unit where it stands now (else the parsed
    // point); inside=<yd>:<ahead> (Ragefire Chasm WP-G1) fixed it at the parse.
    void PlaceInside(Creature* owner, TestSpec& spec)
    {
        if (spec.insideYd <= 0.f || spec.insideFixed)
            return;
        spec.insideCentre = spec.point;
        if (Unit* selected = ObjectAccessor::GetUnit(*owner, spec.selected))
            spec.insideCentre = selected->GetPosition();
    }

    // ---- the mound and the crawl (Ragefire Chasm WP-G1; MechanicDrill.h "Ragefire Chasm shapes") --------------------
    // A Mechanic Marker that moves: passive, the owner's level and phase, gone `life` later at the latest.
    Creature* SummonMover(Creature* owner, Position const& at, Milliseconds life)
    {
        TempSummon* unit = owner->SummonCreature(Ids::NPC_MECHANIC_MARKER, at.GetPositionX(), at.GetPositionY(),
            at.GetPositionZ(), at.GetOrientation(), TEMPSUMMON_TIMED_DESPAWN, static_cast<uint32>(life.count()));
        if (!unit)
            return nullptr;
        unit->SetReactState(REACT_PASSIVE);
        unit->SetLevel(owner->GetLevel(), false);
        if (unit->GetPhaseMask() != owner->GetPhaseMask())
            unit->SetPhaseMask(owner->GetPhaseMask(), true);
        return unit;
    }

    // `mound` (OG1 + OG2): the Borer at the selected unit runs at the picked member for warn + persist, re-aimed every
    // kMoundAimMs (DungeonKit::Chaser's repath); its circle is the hunted member's alone (players only), deals nothing,
    // draws nothing (the mound is the tell) and stands from `warn` to the chase's end (Persistent, tick 0). At the end
    // it stops and the eruption circle (moundErupt, moundBoom, the step's damage keys, everyone) registers there under
    // MoundEruptTag. The hunted member gone or dead: the mound stands and still erupts (§7 #36 A).
    std::vector<uint32> RegisterMound(Creature* owner, TestSpec spec, Dice& dice, std::string& err)
    {
        Unit* start = ObjectAccessor::GetUnit(*owner, spec.selected);
        if (!start || !start->IsInWorld() || start->GetMap() != owner->GetMap())
        {
            err = "the mound's start unit (the selected creature) is gone";
            return {};
        }
        Player* hunted = PickMember(owner, spec, dice);
        if (!hunted)
        {
            err = kNoMemberMessage;
            return {};
        }
        Milliseconds const life = spec.warn + spec.persist;
        Creature* mound = SummonMover(owner, start->GetPosition(), life + spec.moundBoom + kChaserLinger);
        if (!mound)
        {
            err = Acore::StringFormat("could not summon the mound ({})", Ids::NPC_MECHANIC_MARKER);
            return {};
        }
        // The run speed the bots read for the circle's drift (MechanicBotView AnchorDriftSpeed) and the walk uses.
        mound->SetSpeed(MOVE_RUN, spec.moundSpeed / Drill::kPlayerRunYdS, true);

        PlaceInside(owner, spec);
        TestSpec circle = spec;
        circle.shape = "circle";
        circle.unitAnchor = true;
        circle.unit = mound->GetGUID();
        circle.target = hunted->GetGUID();
        circle.pct = 0.f;
        circle.tankPct = -1.f;
        circle.weak = false;
        circle.once = false;
        circle.rider = 0;
        circle.tele = false;
        circle.tick = Milliseconds(0);
        Eligibility elig = spec.eligibility.value_or(Eligibility());
        elig.carriers = { hunted->GetGUID() };
        elig.playersOnly = true;
        circle.eligibility = elig;
        std::vector<uint32> ids = RegisterTest(owner, circle, err);
        if (ids.empty())
        {
            mound->DespawnOrUnsummon();
            return ids;
        }

        TestSpec boom = spec;
        boom.shape = "circle";
        boom.size = spec.moundErupt;
        boom.warn = spec.moundBoom;
        boom.persist = Milliseconds(0);
        boom.unitAnchor = false;
        boom.tag = Drill::MoundEruptTag(spec.tag);
        Registry& reg = Registry::GetOrCreate(owner->GetMap());
        Milliseconds const now = GameTime::GetGameTimeMS();
        ObjectGuid const moundGuid = mound->GetGUID();
        ObjectGuid const huntedGuid = hunted->GetGUID();
        for (Milliseconds at(0); at < life; at += Milliseconds(Drill::kMoundAimMs))
            reg.Schedule(owner->GetGUID(), now + at, [moundGuid, huntedGuid, speed = spec.moundSpeed](Creature* o)
            {
                Creature* unit = o->GetMap()->GetCreature(moundGuid);
                Player* member = unit ? ObjectAccessor::GetPlayer(*unit, huntedGuid) : nullptr;
                if (!unit)
                    return;
                if (!member || !member->IsAlive())
                {
                    unit->GetMotionMaster()->Clear();
                    unit->StopMoving();
                    return;
                }
                unit->GetMotionMaster()->MovePoint(0, member->GetPosition(), FORCED_MOVEMENT_NONE, speed);
            });
        reg.Schedule(owner->GetGUID(), now + life, [moundGuid, boom](Creature* o)
        {
            Creature* unit = o->GetMap()->GetCreature(moundGuid);
            if (!unit)
                return;
            unit->GetMotionMaster()->Clear();
            unit->StopMoving();
            TestSpec at = boom;
            at.point = unit->GetPosition();
            at.point.SetOrientation(boom.facing);
            std::string ignored;
            RegisterTest(o, at, ignored);
        });
        return ids;
    }

    // `crawl` (JR5): a pickup as DungeonKit::CrawlPickups registers it (a Soak on its own unit, telegraph only:
    // MissedSoak Never, NoFillTimer, OneClaimPerBot, the whole fight's warning), starting crawlOff yd from the picked
    // member toward its goal and crawling there at crawlSpeed (re-pathed every kCrawlRepathMs). An eligible member
    // within `size` takes it (polled every kCrawlPollMs); the goal reached, or `warn` gone, is a group failure
    // (MissedSoak under the step's tag).
    struct CrawlState
    {
        ObjectGuid unit;
        uint32 soak = 0;
        bool done = false;
    };

    std::vector<uint32> RegisterCrawl(Creature* owner, TestSpec spec, Dice& dice, std::string& err)
    {
        Geo::Vec3 const g = Drill::Shifted({ spec.point.GetPositionX(), spec.point.GetPositionY(),
            spec.point.GetPositionZ() }, spec.facing + Geo::DegToRad(spec.crawlBearing), spec.crawlYd, 0.f);
        Position const goal(g.x, g.y, GroundZ(owner, g.x, g.y, g.z), 0.f);
        Geo::Vec3 s;
        if (spec.crawlFrom)
        {
            // from=<yd>:<bearing> (Zul'Farrak WP-G1): a soul rising at a grave, off the selected unit's start.
            s = Drill::FromPoint({ spec.point.GetPositionX(), spec.point.GetPositionY(), spec.point.GetPositionZ() },
                spec.facing, spec.crawlFrom->yd, spec.crawlFrom->second);
        }
        else
        {
            Player* from = PickMember(owner, spec, dice);
            if (!from)
            {
                err = kNoMemberMessage;
                return {};
            }
            s = Drill::CrawlStart({ from->GetPositionX(), from->GetPositionY(), from->GetPositionZ() },
                { goal.GetPositionX(), goal.GetPositionY(), goal.GetPositionZ() }, spec.crawlOff);
        }
        Position const start(s.x, s.y, GroundZ(owner, s.x, s.y, s.z), 0.f);
        Creature* unit = SummonMover(owner, start, spec.warn + kChaserLinger);
        if (!unit)
        {
            err = Acore::StringFormat("could not summon the crawling unit ({})", Ids::NPC_MECHANIC_MARKER);
            return {};
        }
        Registry& reg = Registry::GetOrCreate(owner->GetMap());
        reg.AddScopePlayer(owner->GetGUID(), spec.gm);
        auto const state = std::make_shared<CrawlState>();
        state->unit = unit->GetGUID();
        {
            MechanicRef ref = Mechanic::Soak(owner, unit, spec.size, std::max<uint8>(1, spec.need),
                Milliseconds(4 * 3600 * 1000));     // CrawlPickups::kWholeFight
            if (!ref.IsValid())
            {
                unit->DespawnOrUnsummon();
                err = "the registry refused the crawl's soak";
                return {};
            }
            ref.MissedSoak(MissedSoakMode::Never).NoFillTimer().OneClaimPerBot();
            if (spec.healReach)
                ref.HealerWithinTankReach();    // Zul'Farrak F14
            if (spec.seen >= 0)
                ref.SeenOnlyWith(static_cast<uint8>(spec.seen));    // Zul'Farrak F11
            ref.Eligible(spec.eligibility.value_or(Eligibility()));
            if (spec.ordering)
                ref.Order(*spec.ordering);
            ref.Tag(spec.tag);
            state->soak = ref.Id();
        }
        Milliseconds const now = GameTime::GetGameTimeMS();
        Milliseconds const step(Drill::kCrawlPollMs);
        for (Milliseconds at(0); at <= spec.warn; at += step)
        {
            bool const repath = at.count() % Drill::kCrawlRepathMs == 0;
            bool const last = at + step > spec.warn;
            reg.Schedule(owner->GetGUID(), now + at, [state, spec, goal, repath, last](Creature* o)
            {
                if (state->done)
                    return;
                Registry& reg = Registry::GetOrCreate(o->GetMap());
                Creature* unit = o->GetMap()->GetCreature(state->unit);
                auto const end = [&state, &reg, unit]()
                {
                    state->done = true;
                    reg.Cancel(state->soak);
                    if (unit)
                        unit->DespawnOrUnsummon();
                };
                if (!unit || !unit->IsAlive())
                {
                    end();
                    return;
                }
                ActiveMechanic const* soak = reg.Find(state->soak);
                for (Player* p : GroupMembers(o, spec.gm, spec.includeGm))
                    if (p->GetExactDist2d(unit) <= spec.size
                        && std::fabs(p->GetPositionZ() - unit->GetPositionZ()) <= Drill::kCrawlZBandYd
                        && (!soak || reg.IsEligible(*soak, p)))
                    {
                        end();
                        AddMarked(o, p, spec.markStacks);   // mark=<n> (Zul'Farrak WP-G1): a Gathered Soul
                        return;
                    }
                if (last || unit->GetExactDist2d(&goal) <= Drill::kWispReachYd)
                {
                    FailureEvent e;
                    e.encounter = o->GetGUID();
                    e.mechanicId = state->soak;
                    e.tag = spec.tag;
                    e.kind = Kind::Soak;
                    e.type = FailureEvent::Type::MissedSoak;
                    e.at = GameTime::GetGameTimeMS();
                    reg.PostFailure(e);
                    end();
                    return;
                }
                if (repath)
                    unit->GetMotionMaster()->MovePoint(0, goal, FORCED_MOVEMENT_NONE, spec.crawlSpeed);
            });
        }
        return { state->soak };
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

    // `around=<yd>:<n>` (Zul'Farrak WP-G1, the pool's puddles): n circles on a ring of `aroundYd` round the landing,
    // the first along the run's facing (Drill::RingSpots). The landing is the next `at=prev`.
    std::vector<uint32> RegisterAround(Creature* owner, TestSpec const& spec, Dice& dice, std::string& err)
    {
        std::vector<Geo::Vec3> const points = Drill::RingSpots({ spec.point.GetPositionX(), spec.point.GetPositionY(),
            spec.point.GetPositionZ() }, spec.aroundYd, spec.around, spec.facing);
        std::vector<uint32> ids;
        for (Geo::Vec3 const& p : points)
        {
            TestSpec one = spec;
            one.around = 0;
            one.point = Position(p.x, p.y, GroundZ(owner, p.x, p.y, p.z), spec.facing);
            std::vector<uint32> const made = RegisterTest(owner, one, err);
            ids.insert(ids.end(), made.begin(), made.end());
        }
        dice.lastCircle = spec.point;
        return ids;
    }

    // One repeat of `.mech test`: resolves the landing (at=member|group|gm|last|each|prev|predict, anchor=member, to=,
    // away=, shift=, scatter=, fix=) and the spot ring now, so a delayed repeat follows the bots, then registers.
    // `hunt`, `soak bell=`, `mound` and `crawl` place themselves (RegisterHunt, RegisterBell, RegisterMound,
    // RegisterCrawl); `grid=` lays its circles (RegisterGrid).
    std::vector<uint32> RegisterRepeat(Creature* owner, TestSpec spec, Dice& dice, std::string& err)
    {
        if (spec.shape == "hunt")
            return RegisterHunt(owner, spec, dice, err);
        if (spec.shape == "soak" && spec.bells)
            return RegisterBell(owner, spec, dice, err);
        if (spec.shape == "mound")
            return RegisterMound(owner, spec, dice, err);
        if (spec.shape == "crawl")
            return RegisterCrawl(owner, spec, dice, err);
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
        if (spec.alongTarget)
        {
            // along=target (Scarlet Monastery WP-G2): the axes of shift= (and a line's facing, turned by offset=) are
            // the selected unit's facing now, not the run's: Thorne's furrow 5 yd ahead of him.
            if (Unit* selected = ObjectAccessor::GetUnit(*owner, spec.selected))
            {
                spec.facing = Geo::WrapPi(selected->GetOrientation() + Geo::DegToRad(spec.shape == "line"
                    ? spec.offset : 0.f));
                spec.point.SetOrientation(spec.facing);
            }
        }
        if (spec.atLast)
        {
            // The end of the fixed line `atBack` before the last; none: where the selected unit stands now.
            if (std::optional<Position> const end = dice.ends.Back(spec.atBack))
                spec.point = *end;
            else if (Unit* selected = ObjectAccessor::GetUnit(*owner, spec.selected))
                spec.point = selected->GetPosition();
            spec.point.SetOrientation(spec.facing);
        }
        if (spec.atTarget)
        {
            // at=target (Blackrock Depths WP-G2): where the selected unit stands now. A drill parses its steps at its
            // start, so a plain point would stay where a walking boss stood then.
            if (Unit* selected = ObjectAccessor::GetUnit(*owner, spec.selected))
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
        // A fixed to= line (Scarlet Monastery WP-G2): shift= moves its source first, along the run's (or along=)
        // facing, and the line is aimed from there; otherwise shift= would move the frozen source along the aim.
        if (spec.fix && spec.to != TestSpec::To::None && spec.shifted && !spec.unitAnchor)
        {
            Geo::Vec3 const p = Drill::Shifted({ spec.point.GetPositionX(), spec.point.GetPositionY(),
                spec.point.GetPositionZ() }, spec.facing, spec.shiftAhead, spec.shiftLeft);
            spec.point.Relocate(p.x, p.y, GroundZ(owner, p.x, p.y, p.z));
            spec.shifted = false;
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
            case TestSpec::To::Last:
            case TestSpec::To::None:
                break;
        }
        if (spec.onlyMember)
        {
            // only=member (WP-G2): one member alone is eligible (players only), drawn with pick= now (pick=same: the
            // member of the last pick, after this step's own at= or to= pick).
            Player* member = PickMember(owner, spec, dice);
            if (!member)
            {
                err = kNoMemberMessage;
                return {};
            }
            Eligibility elig = spec.eligibility.value_or(Eligibility());
            elig.carriers = { member->GetGUID() };
            elig.playersOnly = true;
            spec.eligibility = elig;
        }
        if (spec.exemptMember)
        {
            // exempt=member (Gnomeregan WP-G1, TB6): every scope member (the GM too, as for any zone) but the member of
            // a pick= drawn now (pick=same, pick=marked:<n>: the owner the step landed on) is eligible; nobody left
            // keeps a set no player is in.
            Player* member = PickMember(owner, spec, dice);
            if (!member)
            {
                err = kNoMemberMessage;
                return {};
            }
            std::vector<ObjectGuid> members;
            for (Player const* p : GroupMembers(owner, spec.gm, true))
                members.push_back(p->GetGUID());
            std::vector<ObjectGuid> const rest = Drill::AllBut(members, member->GetGUID());
            Eligibility elig = spec.eligibility.value_or(Eligibility());
            elig.carriers = GuidSet(rest.begin(), rest.end());
            if (elig.carriers.empty())
                elig.carriers.insert(ObjectGuid::Empty);
            spec.eligibility = elig;
        }
        PlaceInside(owner, spec);
        if (spec.hideOf)
            spec.hideSlots = Drill::PickDistinct(spec.hideOf, spec.hideBlockers,
                [&rng](uint32 n) { return rng.Index(n); });
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
        // to=last: the line is aimed at a recorded end, so it records none of its own (the next to=last:<n> keeps
        // counting the same lines).
        bool const aimedAtEnd = spec.to == TestSpec::To::Last;
        if (spec.fix && spec.to != TestSpec::To::None)
        {
            // Frozen now: a point line from the source toward where the to= unit stands (reach: ending there), or
            // (to=last) toward a recorded line end; none recorded: where the selected unit stands.
            Position toward;
            if (aimedAtEnd)
            {
                if (std::optional<Position> const end = dice.ends.Back(spec.toBack))
                    toward = *end;
                else if (Unit* selected = ObjectAccessor::GetUnit(*owner, spec.selected))
                    toward = selected->GetPosition();
                else
                {
                    err = "to=last: no line end recorded and the selected unit is gone";
                    return {};
                }
            }
            else if (Unit* toUnit = ObjectAccessor::GetUnit(*owner, spec.toUnit))
                toward = toUnit->GetPosition();
            else
            {
                err = "the to= unit is gone";
                return {};
            }
            Drill::FixedLine const line = Drill::FixLine({ source.GetPositionX(), source.GetPositionY(),
                source.GetPositionZ() }, { toward.GetPositionX(), toward.GetPositionY(), toward.GetPositionZ() },
                spec.size, spec.reach, spec.facing, spec.past);
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
        if (spec.around)
            return RegisterAround(owner, spec, dice, err);
        if (spec.shape == "conefront")
        {
            // WP-RF: turn= from the previous conefront's slice (clockwise), else bearing= from the GM's facing, else
            // a random slice.
            if (spec.turn && dice.lastSlice)
                spec.sliceAbs = Front::SliceBearing(*dice.lastSlice, 1, *spec.turn);
            else if (spec.bearing)
                spec.sliceAbs = Geo::WrapPi(spec.facing + Geo::DegToRad(*spec.bearing));
            else
                spec.sliceAbs = Geo::WrapPi(rng.Float(0.f, Geo::kTwoPi));
        }
        if (spec.shape == "linefront" && spec.gapWidth > 0.f)
        {
            // WP-RF: side=other puts the gap across the centreline from the previous linefront's (apart=: at most that
            // far from it); with no previous one, anywhere.
            int side = spec.gapSide == 2 ? (dice.lastGap ? -Front::SideOf(*dice.lastGap) : 0) : spec.gapSide;
            bool const near = spec.gapApart >= 0.f && dice.lastGap;
            std::optional<float> const c = Front::PlaceGap(spec.frontWidth, spec.gapWidth, {}, rng.Float(0.f, 1.f),
                side, spec.gapEdge, near ? *dice.lastGap : 0.f, near ? spec.gapApart : -1.f);
            if (!c)
            {
                err = "linefront: no place for the gap (side=, apart= and gap= against width=)";
                return {};
            }
            spec.gapCentre = *c;
        }
        std::vector<uint32> ids = RegisterTest(owner, spec, err);
        if (!ids.empty() && spec.shape == "conefront")
            dice.lastSlice = spec.sliceAbs;
        if (!ids.empty() && spec.shape == "linefront" && spec.gapWidth > 0.f)
            dice.lastGap = spec.gapCentre;
        // A point-anchored circle leaves its centre for the next `at=prev`.
        if (!ids.empty() && spec.shape == "circle" && !spec.unitAnchor)
            dice.lastCircle = spec.point;
        // A fixed line (a point source along a fixed facing) leaves its far end for the next `at=last` (and
        // `to=last`); a line aimed at a recorded end leaves none.
        if (!ids.empty() && (spec.shape == "line" || spec.shape == "projectile") && !spec.unitAnchor
            && spec.to == TestSpec::To::None && !aimedAtEnd)
        {
            Geo::Vec3 const end = Drill::LineEnd({ spec.point.GetPositionX(), spec.point.GetPositionY(),
                spec.point.GetPositionZ() }, spec.facing, spec.size);
            dice.ends.Push(Position(end.x, end.y, GroundZ(owner, end.x, end.y, end.z), spec.facing));
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
        "cone rearcone line projectile mine lanes spread runout stack soak frontal bait holdstill spot jump conefront "
        "linefront hide; "
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
        // to=last[:<n>] (lines only, Blackrock Spire WP-G1): aimed at a recorded line end, frozen like fix=1.
        auto readTo = [&opt, &spec](bool allowLast)
        {
            std::string const to = opt.Str("to", "");
            std::optional<uint32> const back = allowLast ? Drill::LastIndex(to) : std::nullopt;
            if (back)
            {
                spec.to = TestSpec::To::Last;
                spec.toBack = *back;
            }
            else if (to == "member")
                spec.to = TestSpec::To::Member;
            else if (to == "target")
                spec.to = TestSpec::To::Target;
            else if (to == "me")
                spec.to = TestSpec::To::Me;
            else if (!to.empty())
                opt.MarkBad("to");
        };
        if (s == "cone")
        {
            readTo(false);
            // fix=1 (WP-G2): aimed at the to= unit once, at registration (Drakkisath's blows), not turning live.
            spec.fix = opt.Bool("fix", false);
        }
        if (s == "line" || s == "projectile")
        {
            spec.width = opt.Float("width", s == "projectile" ? 3.f : 4.f);
            spec.exclude = opt.Float("exclude", 0.f);
            readTo(s == "line");
            spec.reach = opt.Bool("reach", false);
            spec.fix = spec.reach || opt.Bool("fix", false) || spec.to == TestSpec::To::Last;
            // past=<yd> (line, Scarlet Monastery WP-G2): a reach=1 line runs that far past the to= unit.
            if (s == "line" && opt.Has("past"))
            {
                spec.past = opt.Float("past", 0.f);
                if (!(spec.past >= 0.f))
                    opt.MarkBad("past");
            }
            spec.minDist = opt.Float("min", 0.f);
            if (spec.minDist < 0.f)
                opt.MarkBad("min");
            // offset=<deg> (line, Blackrock Depths WP-G1): turns the fixed facing counterclockwise (applied below).
            if (s == "line")
                spec.offset = opt.Float("offset", 0.f);
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
            // to=target|member|me with shape=line (Blackrock Depths WP-G2): a beam from the anchor to that unit.
            readTo(false);
            // Scarlet Monastery F2: backoff=<yd> (EndBacksOff, a to= beam) and tanknear=<yd> (TankNearSource).
            if (opt.Has("backoff"))
            {
                spec.backOff = opt.Float("backoff", 0.f);
                if (!(spec.backOff > 0.f))
                    opt.MarkBad("backoff");
            }
            if (opt.Has("tanknear"))
            {
                spec.tankNear = opt.Float("tanknear", 0.f);
                if (!(spec.tankNear > 0.f))
                    opt.MarkBad("tanknear");
            }
        }
        // Scarlet Monastery F13: `gaze <range> <ms> [arc=180] [lead=<ms>]`.
        if (s == "gaze")
        {
            spec.gazeArc = opt.Float("arc", Gaze::kDefaultArcDeg);
            if (!(spec.gazeArc >= Gaze::kMinArcDeg && spec.gazeArc <= Gaze::kMaxArcDeg))
                opt.MarkBad("arc");
            spec.gazeLead = opt.Ms("lead", Milliseconds(0));
            if (spec.gazeLead < Milliseconds(0))
                opt.MarkBad("lead");
        }
        // Scarlet Monastery F6: holdfacing=1 on any shape.
        if (opt.Has("holdfacing"))
            spec.holdFacing = opt.Bool("holdfacing", false);
        // Zul'Farrak F11: seen=<type> on any shape that draws (markers only for players detecting that type).
        if (opt.Has("seen"))
        {
            spec.seen = opt.Int("seen", -1);
            if (spec.seen < 0 || !Judge::SeenOnlyWithFits(true, false, static_cast<unsigned>(spec.seen)))
            {
                opt.MarkBad("seen");
                spec.seen = -1;
            }
        }
        // Zul'Farrak F14: healreach=1 on a soak or crawl (no bot healer claims it past its tank's reach).
        if (opt.Has("healreach"))
        {
            spec.healReach = opt.Bool("healreach", false);
            if (s != "soak" && s != "crawl")
                opt.MarkBad("healreach");
        }
        if (s == "hunt")
        {
            spec.huntRate = opt.Float("speed", Drill::kHuntRateNormal);
            if (spec.huntRate <= 0.f || spec.huntRate > Drill::kMaxHuntRate)
                opt.MarkBad("speed");
        }
        // Ragefire Chasm WP-G1: `mound <r> <arm ms>` and `crawl <r> <life ms>`.
        if (s == "mound")
        {
            spec.moundSpeed = opt.Float("speed", Drill::kMoundSpeedYdS);
            spec.moundErupt = opt.Float("erupt", Drill::kEruptYd);
            spec.moundBoom = opt.Ms("boom", Milliseconds(Drill::kEruptMs));
            if (spec.moundSpeed <= 0.f || spec.moundSpeed > Drill::kMaxHuntRate * Drill::kPlayerRunYdS)
                opt.MarkBad("speed");
            if (spec.moundErupt <= 0.f)
                opt.MarkBad("erupt");
        }
        if (s == "crawl")
        {
            spec.need = static_cast<uint8>(std::clamp<int32>(opt.Int("need", 1), 1, 40));
            spec.crawlYd = opt.Float("yd", 0.f);
            spec.crawlBearing = opt.Float("bearing", 0.f);
            spec.crawlOff = opt.Float("off", Drill::kWispOffYd);
            spec.crawlSpeed = opt.Float("speed", Drill::kWispSpeedYdS);
            if (spec.crawlYd <= 0.f || spec.crawlYd > 100.f)
                opt.MarkBad("yd");
            if (spec.crawlOff < 0.f)
                opt.MarkBad("off");
            if (spec.crawlSpeed <= 0.f)
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
        // Blackrock Depths WP-HK: `hide <range> <ms>`, blockers summoned on a ring round the source.
        if (s == "hide")
        {
            int32 const blockers = opt.Int("blockers", 3);
            if (blockers < 0 || blockers > static_cast<int32>(Hide::kTestMaxBlockers))
                opt.MarkBad("blockers");
            else
                spec.hideBlockers = static_cast<uint32>(blockers);
            spec.hideApart = opt.Float("apart", 8.f);
            spec.hideRadius = opt.Float("r", Hide::kDefaultBlockerRadiusYd);
            spec.hideBehind = opt.Float("behind", Hide::kDefaultBehindYd);
            spec.hideEye = opt.Float("eye", -1.f);
            spec.hideLinger = opt.Ms("linger", Milliseconds(3000));
            int32 const share = opt.Int("share", Hide::kDefaultShare);
            int32 const go = opt.Int("go", static_cast<int32>(Ids::GO_HIDE_TEST_BLOCKER));
            int32 const display = opt.Int("display", static_cast<int32>(Hide::kDisplayLavaRock));
            if (spec.hideApart <= spec.hideRadius)
                opt.MarkBad("apart");
            if (spec.hideRadius <= 0.f)
                opt.MarkBad("r");
            if (spec.hideBehind < 0.f)
                opt.MarkBad("behind");
            if (share < 1 || share > 40)
                opt.MarkBad("share");
            else
                spec.hideShare = static_cast<uint8>(share);
            if (go <= 0)
                opt.MarkBad("go");
            else
                spec.hideGo = static_cast<uint32>(go);
            if (display < 0)
                opt.MarkBad("display");
            else
                spec.hideDisplay = static_cast<uint32>(display);
            // of=<n> and scale=<s> (Ragefire Chasm WP-G1)
            int32 const of = opt.Int("of", 0);
            if (of != 0 && (of < static_cast<int32>(spec.hideBlockers) || of > static_cast<int32>(
                Hide::kTestMaxBlockers)))
                opt.MarkBad("of");
            else
                spec.hideOf = static_cast<uint32>(of);
            spec.hideScale = opt.Float("scale", 1.f);
            if (!(spec.hideScale > 0.f && spec.hideScale <= 10.f))
                opt.MarkBad("scale");
        }
        if (s == "circle" || s == "donut" || s == "cone" || s == "rearcone" || s == "line" || s == "conefront")
            spec.park = opt.Bool("park", false);
        // WP-RF: the rolling fronts' keys.
        if (s == "conefront" || s == "linefront")
        {
            spec.band = opt.Float("band", 4.f);
            spec.frontSpeed = opt.Float("speed", 8.f);
            if (spec.band < Front::kMinBandYd)
                opt.MarkBad("band");
            if (spec.frontSpeed <= 0.f)
                opt.MarkBad("speed");
        }
        if (s == "conefront")
        {
            spec.sliceDeg = opt.Float("slice", 60.f);
            if (spec.sliceDeg < 0.f || spec.sliceDeg > Front::kMaxSliceDeg)
                opt.MarkBad("slice");
            if (opt.Has("bearing") && opt.Str("bearing", "") != "random")
                spec.bearing = opt.Float("bearing", 0.f);
            if (opt.Has("turn"))
                spec.turn = opt.Float("turn", 0.f);
            spec.frontStart = opt.Float("start", 0.f);
            if (spec.frontStart < 0.f || spec.frontStart >= spec.size)
                opt.MarkBad("start");
            spec.untilYd = opt.Float("until", 0.f);
            spec.nextTurn = opt.Float("next", 0.f);
            if (!std::isfinite(spec.nextTurn))
                opt.MarkBad("next");
        }
        if (s == "linefront")
        {
            spec.frontWidth = opt.Float("width", 40.f);
            spec.gapWidth = opt.Float("gap", 8.f);
            spec.gapApart = opt.Float("apart", -1.f);
            spec.frontFacing = opt.Float("facing", 0.f);
            spec.gapEdge = opt.Float("edge", 0.f);
            if (spec.frontWidth <= 0.f)
                opt.MarkBad("width");
            if (spec.gapEdge < 0.f || spec.gapEdge * 2.f + spec.gapWidth > spec.frontWidth)
                opt.MarkBad("edge");
            if (spec.gapWidth < 0.f || spec.gapWidth >= spec.frontWidth)
                opt.MarkBad("gap");
            std::string const side = opt.Str("side", "any");
            if (side == "left")
                spec.gapSide = 1;
            else if (side == "right")
                spec.gapSide = -1;
            else if (side == "other")
                spec.gapSide = 2;
            else if (side != "any")
                opt.MarkBad("side");
        }
        // decoy=1 (Scholomance WP-X): drawn, hits nobody, not in the bot view (lanes: the safe=1 lane drawn as one).
        if (s == "circle" || s == "donut" || s == "cone" || s == "rearcone" || s == "frontal" || s == "line"
            || s == "projectile" || s == "lanes")
            spec.decoy = opt.Bool("decoy", false);
        // drop=<edge,away,behind,prefer,avoid> (Stratholme X2): where the member carrying this shape takes it
        // (circle anchor=member, line to=member); room=<yd> around the selected unit (0: no room), dwell=<ms>.
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
            spec.dropDwell = opt.Ms("dwell", Milliseconds(800));
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
        if ((s == "cone" || s == "line" || s == "projectile" || s == "soak") && opt.Has("exempt"))
        {
            if (opt.Str("exempt", "") == "target")
                spec.exemptTo = true;
            else
                opt.MarkBad("exempt");
        }
        // exempt=member (Gnomeregan WP-G1, TB6's save point): a circle eligible to all but the member of its pick=.
        if (s == "circle" && opt.Has("exempt"))
        {
            if (opt.Str("exempt", "") == "member")
                spec.exemptMember = true;
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
        // WP-G2: only=member (one member eligible), inside=<yd> (the bots' hard bounds), lethal=1.
        if (opt.Has("only"))
        {
            if (opt.Str("only", "") == "member")
                spec.onlyMember = true;
            else
                opt.MarkBad("only");
        }
        if (opt.Has("inside"))
        {
            // inside=<yd>[:<ahead>] (the fixed form: Ragefire Chasm WP-G1)
            if (std::optional<Drill::InsideSpec> const in = Drill::ParseInside(opt.Str("inside", "")))
            {
                spec.insideYd = in->yd;
                spec.insideFixed = in->ahead.has_value();
                if (in->ahead)
                {
                    Geo::Vec3 const c = Drill::Shifted({ selected->GetPositionX(), selected->GetPositionY(),
                        selected->GetPositionZ() }, ctx.facing, *in->ahead, 0.f);
                    spec.insideCentre = Position(c.x, c.y, c.z, ctx.facing);
                }
            }
            else
                opt.MarkBad("inside");
        }
        // Zul'Farrak WP-G1: inbox=<w>x<l>[:<ahead>], a box of hard bounds fixed on the selected unit's place now.
        if (opt.Has("inbox"))
        {
            if (std::optional<Drill::BoxSpec> const box = Drill::ParseInBox(opt.Str("inbox", "")))
            {
                Geo::Vec3 const c = Drill::Shifted({ selected->GetPositionX(), selected->GetPositionY(),
                    selected->GetPositionZ() }, ctx.facing, box->ahead, 0.f);
                Geo::Vec3 const corner = Drill::BoxCorner(c, ctx.facing, box->width, box->length);
                spec.inBox = true;
                spec.boxCorner = Position(corner.x, corner.y, corner.z, ctx.facing);
                spec.boxFacing = ctx.facing;
                spec.boxWidth = box->width;
                spec.boxLength = box->length;
            }
            else
                opt.MarkBad("inbox");
        }
        // crawl from=<yd>:<bearing>; soak / crawl mark=<n>; circle around=<yd>:<n>; circle drop= judge=prefer.
        if (s == "crawl" && opt.Has("from"))
        {
            spec.crawlFrom = Drill::ParseFrom(opt.Str("from", ""));
            if (!spec.crawlFrom)
                opt.MarkBad("from");
        }
        if ((s == "soak" || s == "crawl") && opt.Has("mark"))
        {
            int32 const n = opt.Int("mark", 0);
            if (n < 1 || n > static_cast<int32>(Drill::kMaxMark))
                opt.MarkBad("mark");
            else
                spec.markStacks = static_cast<uint32>(n);
        }
        if (s == "circle" && opt.Has("around"))
        {
            if (std::optional<Drill::PolarSpec> const a = Drill::ParseAround(opt.Str("around", "")))
            {
                spec.aroundYd = a->yd;
                spec.around = static_cast<uint32>(a->second);
            }
            else
                opt.MarkBad("around");
        }
        if (s == "circle" && opt.Has("judge"))
        {
            spec.dropJudge = opt.Str("judge", "") == "prefer";
            if (!spec.dropJudge || !spec.drop)
                opt.MarkBad("judge");
        }
        if (opt.Has("lethal"))
        {
            spec.lethal = opt.Bool("lethal", false);
            if (spec.lethal && s != "circle" && s != "donut" && s != "cone" && s != "rearcone" && s != "line")
                opt.MarkBad("lethal");
        }
        if ((s == "soak" || s == "bait") && opt.Has("order"))
        {
            Ordering order;
            if (ParseOrdering(opt.Str("order", ""), order))
                spec.ordering = order;
            else
                opt.MarkBad("order");
        }
        // Scarlet Monastery F7: last=<role> (a soak's Ordering::roleLast; with or without order=).
        if ((s == "soak" || s == "crawl") && opt.Has("last"))
        {
            if (std::optional<Role> role = ParseRole(opt.Str("last", "")); role && *role != Role::Any)
            {
                Ordering order = spec.ordering.value_or(Ordering());
                order.roleLast = *role;
                spec.ordering = order;
            }
            else
                opt.MarkBad("last");
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
        bool const noDamage = s == "crawl" || s == "stack" || (s == "soak" && !spec.bells) || s == "holdstill"
            || s == "spot";
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
        else if (std::optional<uint32> back = Drill::LastIndex(at))
        {
            spec.atLast = true;
            spec.atBack = *back;
        }
        else if (at == "each")
            spec.atEach = true;
        else if (at == "prev")
            spec.atPrev = true;
        else if (at == "predict")
        {
            spec.atPredict = true;
            spec.lead = opt.Ms("lead", Milliseconds(Drill::kDefaultPredictLeadMs));
        }
        else if (at == "target")
            spec.atTarget = true;
        else if (!at.empty())
            opt.MarkBad("at");
        std::string const pick = opt.Str("pick", "any");
        if (pick == "nontank")
            spec.pick = TestSpec::Pick::NonTank;
        else if (pick == "same")
            spec.pick = TestSpec::Pick::Same;
        else if (pick == "other")
            spec.pick = TestSpec::Pick::Other;
        else if (pick == "tank")
            spec.pick = TestSpec::Pick::Tank;
        else if (pick == "fresh")
            spec.pick = TestSpec::Pick::Fresh;
        else if (pick == "marked")
            spec.pick = TestSpec::Pick::Marked;
        else if (pick == "unmarked")
            spec.pick = TestSpec::Pick::Unmarked;
        else if (pick == "healer")
            spec.pick = TestSpec::Pick::Healer;
        else if (std::optional<uint32> const stacks = Drill::ParseMarkedPick(pick))
        {
            // Gnomeregan WP-G1: pick=marked:<n>, the holder of exactly n Marked stacks (a Rubber Band owner).
            spec.pick = TestSpec::Pick::MarkedAt;
            spec.pickStacks = *stacks;
        }
        else if (pick != "any")
            opt.MarkBad("pick");
        if (opt.Has("shift"))
        {
            spec.shifted = ParseShift(opt.Str("shift", ""), spec.shiftAhead, spec.shiftLeft);
            if (!spec.shifted)
                opt.MarkBad("shift");
        }
        // along=target (Scarlet Monastery WP-G2): the point's axes turn with the selected unit as the repeat registers.
        if (opt.Has("along"))
        {
            if (opt.Str("along", "") == "target")
                spec.alongTarget = true;
            else
                opt.MarkBad("along");
        }

        // anchor
        spec.facing = ctx.facing;
        // A line's offset= turns its fixed facing, and with it the axes of shift= (Blackrock Depths WP-G1).
        if (s == "line" && spec.offset != 0.f)
            spec.facing = Geo::WrapPi(spec.facing + Geo::DegToRad(spec.offset));
        spec.gm = ctx.scope;
        spec.point = selected->GetPosition();
        spec.point.SetOrientation(spec.facing);
        spec.target = selected->GetGUID();
        spec.selected = selected->GetGUID();
        bool const atPoint = spec.atMember || spec.atGroup || spec.atGm || spec.atLast || spec.atEach || spec.atPrev
            || spec.atPredict || spec.atTarget;
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
        if ((s == "conefront" || s == "linefront") && (spec.unitAnchor || spec.anchorMember))
            return "a rolling front rolls from a point: leave anchor= out (at= and shift= move the point)";
        if (spec.to == TestSpec::To::Target && selected == gm)
            return "to=target needs a selected unit";
        if (spec.follow && !spec.unitAnchor && !spec.anchorMember)
            return "follow=1 needs a unit anchor (anchor=me, target or member)";
        if (s == "cone" && spec.to != TestSpec::To::None && spec.follow)
            return "to= aims the cone at a unit: no follow=1";
        if (spec.exemptTo && spec.to == TestSpec::To::None)
            return "exempt=target spares the unit the shape aims at: add to=member, target or me";
        if (spec.exemptTo && spec.to == TestSpec::To::Last)
            return "exempt=target spares a unit: to=last aims at a point";
        if (spec.to == TestSpec::To::Last && spec.follow)
            return "to=last freezes the line at a recorded end: no follow=1";
        if (s == "line" && spec.offset != 0.f && (spec.to != TestSpec::To::None || spec.follow))
            return "offset= turns a line's fixed facing: not with to= (it aims the line) or follow=1";
        if (spec.shifted && (spec.unitAnchor || spec.anchorMember))
            return "shift= moves a point anchor; leave anchor= out";
        if (spec.cluster && spec.lane)
            return "cluster= needs a circle stack (no shape=line)";
        if (spec.atTarget && selected == gm)
            return "at=target lands where the selected unit stands: select one";
        if (s == "soak" && spec.to != TestSpec::To::None)
        {
            // WP-G2: a beam between two units (Shapes::Line(from, to)); its length is the distance at registration.
            if (!spec.lane)
                return "to= on a soak makes a beam to that unit: add shape=line";
            if (opt.Has("length"))
                return "a beam runs to the to= unit: leave length= out";
            if (spec.unitAnchor && ((spec.to == TestSpec::To::Target && spec.unit == spec.selected)
                || (spec.to == TestSpec::To::Me && spec.unit == gm->GetGUID())))
                return "a beam needs two ends: anchor= and to= name the same unit";
        }
        if (spec.backOff > 0.f && (spec.to == TestSpec::To::None || spec.to == TestSpec::To::Last || !spec.lane))
            return "backoff= is for a beam's to= end: a soak shape=line to=member, target or me";
        if (spec.tankNear > 0.f && !spec.lane)
            return "tanknear= is for a line soak: add shape=line";
        if (spec.bells && (spec.backOff > 0.f || spec.tankNear > 0.f))
            return "bell= places the soak on one of its points: no backoff= or tanknear=";
        if (spec.fix && spec.to == TestSpec::To::None)
            return "fix=1 and reach=1 aim at the to= unit: add to=member, target or me";
        // A frozen projectile is fine (Zul'Farrak's swarms, Gnomeregan F-1f): RegisterRepeat makes either a point
        // source along a fixed facing, the shape a projectile without to= already has.
        if (spec.fix && spec.follow)
            return "fix=1 and reach=1 freeze a line: no follow=1";
        if (spec.minDist > 0.f && spec.to != TestSpec::To::Member)
            return "min= measures a to=member pick from the line's source: add to=member";
        if (spec.past > 0.f && (!spec.reach || spec.to == TestSpec::To::Last))
            return "past= runs a reach=1 line beyond its to= unit: add reach=1 and to=member, target or me";
        if (spec.alongTarget && (selected == gm || spec.unitAnchor || spec.anchorMember))
            return "along=target turns a point's axes with the selected unit: select one, leave anchor= out";
        if (spec.offset != 0.f && s == "frontal" && spec.rear)
            return "a frontal takes rear=1 or offset=, not both";
        if (spec.atEach && (s == "spot" || s == "spread" || s == "runout" || s == "bait" || s == "holdstill"
            || s == "jump"))
            return "at=each puts a zone, stack or soak under every member";
        if (spec.away > 0.f && (spec.unitAnchor || spec.anchorMember))
            return "away= moves a point anchor; leave anchor= out";
        if (spec.decoy && s == "lanes" && !spec.safe)
            return "decoy=1 on lanes draws the safe=1 lane as a decoy: add safe=1";
        if (spec.exemptMember && (spec.onlyMember || spec.atEach || spec.decoy))
            return "exempt=member spares the member of the step's pick=: not with only=member, at=each or decoy=1";
        if (spec.decoy && (spec.lethal || spec.insideYd > 0.f || spec.inBox))
            return "a decoy is never seen by the bots: no lethal=1, inside= or inbox=";
        // Zul'Farrak WP-G1
        if (spec.around && (spec.grid || spec.unitAnchor || spec.anchorMember || spec.atEach))
            return "around= lays its circles round a point: not with grid=, anchor= or at=each";
        if (spec.onlyMember && spec.atEach)
            return "only=member makes one member eligible: not with at=each";
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
        if (s == "mound" || s == "crawl")
        {
            if (selected == gm)
                return "mound and crawl need a selected unit: the mound starts at it, the crawl's goal is measured "
                    "from it";
            if (atPoint || spec.unitAnchor || spec.anchorMember || spec.to != TestSpec::To::None || spec.shifted
                || spec.onlyMember)
                return "mound and crawl place themselves and pick a member (pick=): leave at=, anchor=, only= and "
                    "shift= out";
            if (s == "mound" && spec.persist <= Milliseconds(0))
                return "mound needs persist=<ms>: how long it chases after its arm time";
            if (s == "crawl" && spec.warn < Milliseconds(Drill::kCrawlPollMs))
                return "crawl needs a life (its second number, ms): how long it crawls at most";
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
    // random member below it (not the selected unit's victim with pick=nontank), remembered as the run's last pick (a
    // later pick=same lands on it: Scarlet Monastery WP-G1's Mark trail); 0 removes Marked from every member.
    // Never touches Weakened: drill setup hands out no Weakened, and a reset leaves the Weakened members earned from
    // real failures (orchestrator ruling on STAGE-M4-R-REPORT M-2). The encounter records the players, so its end
    // (and `.mech clear`) removes Marked too. The spell row was checked when the drill was parsed.
    // `marker 0 one=1` (`one`; Ragefire Chasm WP-G1) removes it from one random holder only (a dispel).
    void DrillMarker(Creature* owner, TestSpec const& spec, uint8 stacks, bool one, Dice& dice)
    {
        std::vector<Player*> members = GroupMembers(owner, spec.gm, spec.includeGm);
        if (!stacks)
        {
            if (one)
            {
                members.erase(std::remove_if(members.begin(), members.end(), [](Player const* p)
                {
                    return !p->HasAura(Ids::SPELL_MARKED_DRILL);
                }), members.end());
                if (!members.empty())
                    members[dice.rng.Index(static_cast<uint32>(members.size()))]->RemoveAurasDueToSpell(
                        Ids::SPELL_MARKED_DRILL);
                return;
            }
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
        // pick=unmarked (Gnomeregan WP-G1, the second Rubber Band owner): a member without Marked when there is one.
        if (spec.pick == TestSpec::Pick::Unmarked)
        {
            std::vector<Player*> clean;
            for (Player* p : members)
                if (!p->HasAura(Ids::SPELL_MARKED_DRILL))
                    clean.push_back(p);
            if (!clean.empty())
                members = std::move(clean);
        }
        // pick=healer (Zul'Farrak WP-G1, the healer walker): a healer by the role provider when there is one.
        if (spec.pick == TestSpec::Pick::Healer && selected)
        {
            std::vector<Player*> healers;
            for (Player* p : members)
                if (HasRole(GetRoleProvider().Get(selected, p), Role::Healer))
                    healers.push_back(p);
            if (!healers.empty())
                members = std::move(healers);
        }
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
        dice.lastMember = p->GetGUID();
        dice.chain.clear();
        dice.chain.push_back(p->GetGUID());
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

    // Drill step `walk` (Blackrock Depths WP-G1, MechanicDrill.h): the selected creature walks `w.yd` along the run's
    // facing turned by w.offsetDeg over `w.ms`, relocated every kWalkStepMs from where it stands when the walk starts
    // (NearTeleportTo: a rooted unit moves too, the sim's boss stand-in is one), keeping its orientation and snapped
    // to the floor from its last height. A creature only: never a player.
    void ScheduleWalk(StepScheduler const& schedule, Milliseconds delay, TestSpec const& spec, Drill::WalkStep const& w)
    {
        auto const start = std::make_shared<std::optional<Position>>();
        float const heading = Geo::WrapPi(spec.facing + Geo::DegToRad(w.offsetDeg));
        uint32 const steps = Drill::WalkSteps(w.ms);
        for (uint32 k = 0; k <= steps; ++k)
        {
            uint32 const elapsed = k ? Drill::WalkStepAt(k, w.ms) : 0;
            schedule(delay + Milliseconds(elapsed), [selected = spec.selected, start, heading, w, elapsed](
                Creature* testOwner)
            {
                Creature* unit = ObjectAccessor::GetCreature(*testOwner, selected);
                if (!unit || !unit->IsInWorld() || !unit->IsAlive())
                    return;
                if (!*start)
                    *start = unit->GetPosition();
                if (!elapsed)
                    return;
                Position const& s = **start;
                Geo::Vec3 const p = Drill::WalkPoint({ s.GetPositionX(), s.GetPositionY(), s.GetPositionZ() }, heading,
                    w.yd, w.ms, elapsed);
                unit->NearTeleportTo(p.x, p.y, GroundZ(testOwner, p.x, p.y, unit->GetPositionZ()),
                    unit->GetOrientation());
            });
        }
    }

    // Drill step `leash` (Blackrock Depths WP-G1): every tick for `l.ms`, each alive scope member (role=: the ones the
    // role provider calls tanks / healers / not healers for the selected unit) farther than `l.yd` from the selected
    // unit (anchor=tank: from every other alive scope tank; none: skipped) takes an OffSpot failure on the owner's
    // encounter (ledger key Zone/<tag>); near=1 (Scarlet Monastery WP-G1): each one closer than that instead;
    // anchor=self (Scarlet Monastery WP-G2): each one farther than that from where it stood as the leash started
    // (a member with no start spot, joined later, is skipped). No mechanic registers: the bots never see it.
    void ScheduleLeash(StepScheduler const& schedule, Milliseconds delay, TestSpec const& spec,
        Drill::LeashStep const& l)
    {
        auto const starts = std::make_shared<std::unordered_map<ObjectGuid, Geo::Vec3>>();
        if (l.anchor == Drill::LeashAnchor::Self)
            schedule(delay, [spec, starts](Creature* testOwner)
            {
                for (Player const* p : GroupMembers(testOwner, spec.gm, spec.includeGm))
                    (*starts)[p->GetGUID()] = { p->GetPositionX(), p->GetPositionY(), p->GetPositionZ() };
            });
        uint32 const ticks = Drill::LeashTicks(l.ms, l.tickMs);
        for (uint32 k = 1; k <= ticks; ++k)
            schedule(delay + Milliseconds(static_cast<int64>(k) * l.tickMs), [spec, l, starts](Creature* testOwner)
            {
                Unit* unit = ObjectAccessor::GetUnit(*testOwner, spec.selected);
                if (!unit || !unit->IsInWorld() || !unit->IsAlive())
                    return;
                Registry& reg = Registry::GetOrCreate(testOwner->GetMap());
                reg.AddScopePlayer(testOwner->GetGUID(), spec.gm);
                Geo::Vec3 const at{ unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ() };
                std::vector<Player*> const members = GroupMembers(testOwner, spec.gm, spec.includeGm);
                std::vector<Role> roles;
                roles.reserve(members.size());
                for (Player const* p : members)
                    roles.push_back(GetRoleProvider().Get(unit, p));
                for (std::size_t i = 0; i < members.size(); ++i)
                {
                    Player const* p = members[i];
                    if (!Drill::LeashJudges(l.role, HasRole(roles[i], Role::Tank), HasRole(roles[i], Role::Healer)))
                        continue;
                    Geo::Vec3 const pos{ p->GetPositionX(), p->GetPositionY(), p->GetPositionZ() };
                    if (l.anchor == Drill::LeashAnchor::Self)
                    {
                        auto const it = starts->find(p->GetGUID());
                        if (it == starts->end() || !Drill::LeashFails(l.near, Drill::OutOfLeash(it->second, pos, l.yd)))
                            continue;
                    }
                    else if (l.anchor == Drill::LeashAnchor::Tank)
                    {
                        std::vector<Geo::Vec3> tanks;
                        for (std::size_t j = 0; j < members.size(); ++j)
                            if (j != i && HasRole(roles[j], Role::Tank))
                                tanks.push_back({ members[j]->GetPositionX(), members[j]->GetPositionY(),
                                    members[j]->GetPositionZ() });
                        std::optional<bool> const out = Drill::OutOfTankLeash(tanks, pos, l.yd);
                        if (!out || !Drill::LeashFails(l.near, *out))
                            continue;
                    }
                    else if (!Drill::LeashFails(l.near, Drill::OutOfLeash(at, pos, l.yd)))
                        continue;
                    FailureEvent e;
                    e.encounter = testOwner->GetGUID();
                    e.tag = l.ledgerTag;
                    e.kind = Kind::Zone;
                    e.player = p->GetGUID();
                    e.type = FailureEvent::Type::OffSpot;
                    e.at = GameTime::GetGameTimeMS();
                    reg.PostFailure(e);
                }
            });
    }

    // Drill step `chase` (Scarlet Monastery WP-G1, MechanicDrill.h): for `c.ms` the selected creature steps after its
    // victim every kChaseStepMs like an unrooted boss: from its melee range in to its combat reaches + 0.5 (the stop
    // the tank's drag-out plans for, MechanicBotRole.cpp), at most kChaseSpeedYdS, never more than `c.yd` from where
    // the chase started (NearTeleportTo: the sim's rooted stand-in moves too), facing its victim. home=1: back to the
    // start at the end. A creature only: never a player.
    void ScheduleChase(StepScheduler const& schedule, Milliseconds delay, TestSpec const& spec,
        Drill::ChaseSpec const& c)
    {
        auto const start = std::make_shared<std::optional<Position>>();
        uint32 const steps = Drill::ChaseSteps(c.ms);
        float const maxStep = Drill::kChaseSpeedYdS * static_cast<float>(Drill::kChaseStepMs) / 1000.f;
        for (uint32 k = 0; k <= steps; ++k)
        {
            uint32 const elapsed = k ? std::min(k * Drill::kChaseStepMs, c.ms) : 0;
            bool const last = k == steps;
            schedule(delay + Milliseconds(elapsed), [selected = spec.selected, start, c, maxStep, elapsed, last](
                Creature* testOwner)
            {
                Creature* unit = ObjectAccessor::GetCreature(*testOwner, selected);
                if (!unit || !unit->IsInWorld() || !unit->IsAlive())
                    return;
                if (!*start)
                    *start = unit->GetPosition();
                if (!elapsed)
                    return;
                Position const& s = **start;
                if (last && c.home)
                {
                    unit->NearTeleportTo(s.GetPositionX(), s.GetPositionY(), s.GetPositionZ(), s.GetOrientation());
                    return;
                }
                Unit* victim = unit->GetVictim();
                if (!victim || !victim->IsInWorld() || victim->GetMap() != unit->GetMap())
                    return;
                std::optional<Geo::Vec3> const p = Drill::ChaseRelocation(
                    { s.GetPositionX(), s.GetPositionY(), s.GetPositionZ() },
                    { unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ() },
                    { victim->GetPositionX(), victim->GetPositionY(), victim->GetPositionZ() },
                    unit->GetMeleeRange(victim), unit->GetCombatReach() + victim->GetCombatReach() + 0.5f, maxStep,
                    c.yd);
                if (!p)
                    return;
                unit->NearTeleportTo(p->x, p->y, GroundZ(testOwner, p->x, p->y, unit->GetPositionZ()),
                    unit->GetAngle(victim));
            });
        }
    }

    // Drill step `blink` (Ragefire Chasm WP-G1, MechanicDrill.h): the selected creature appears `b.yd` behind a member
    // picked now, facing it (a Shadowstep); at=prev: on the last point circle's centre (a leap's landing); home=1: back
    // where it stood before the run's first blink (`home`, shared by the run; a later blink remembers again).
    // NearTeleportTo: the sim's rooted stand-in moves too. A creature only: never a player.
    void ScheduleBlink(StepScheduler const& schedule, Milliseconds delay, TestSpec const& spec,
        Drill::BlinkStep const& b, RngPtr const& rng, std::shared_ptr<std::optional<Position>> const& home)
    {
        schedule(delay, [spec, b, rng, home](Creature* testOwner)
        {
            Creature* unit = ObjectAccessor::GetCreature(*testOwner, spec.selected);
            if (!unit || !unit->IsInWorld() || !unit->IsAlive())
                return;
            if (b.home)
            {
                if (*home)
                    unit->NearTeleportTo((*home)->GetPositionX(), (*home)->GetPositionY(), (*home)->GetPositionZ(),
                        (*home)->GetOrientation());
                home->reset();
                return;
            }
            Position to;
            if (b.atPrev)
            {
                if (!rng->lastCircle)
                    return;
                to = *rng->lastCircle;
                to.SetOrientation(unit->GetOrientation());
            }
            else
            {
                Player const* member = PickMember(testOwner, spec, *rng);
                if (!member)
                    return;
                Geo::Vec3 const p = Drill::BehindSpot({ member->GetPositionX(), member->GetPositionY(),
                    member->GetPositionZ() }, member->GetOrientation(), b.yd);
                to = Position(p.x, p.y, GroundZ(testOwner, p.x, p.y, p.z), member->GetOrientation());
            }
            if (!*home)
                *home = unit->GetPosition();
            unit->NearTeleportTo(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ(), to.GetOrientation());
        });
    }

    void MatchOwnerLevel(Creature const* owner, Creature* unit);

    // Drill step `cast` (Ragefire Chasm WP-G1, MechanicDrill.h): the selected creature casts `c.spell` at its victim
    // (itself with none), not triggered, so a kick, the school lockout after one and the cast rules apply. kick=1 puts
    // it on the encounter's interrupt list first (an entry of the descriptor; NoteNextCast with every=). Judged
    // kCastJudgeLeadMs before the end (Drill::CastFails): a failure is the group's (MissedSoak under the step's tag).
    // twin=1 (Zul'Farrak WP-G1, Twin Call): a Drill Caster (300404) appears kTwinApartYd to the selected unit's left,
    // the owner's level, sent at the same victim, gone 1.5 s after the cast; it casts the same spell at the same time
    // and its cast is judged on its own (both must be kicked: two failures when neither is).
    void ScheduleCast(StepScheduler const& schedule, Milliseconds delay, TestSpec const& spec, Drill::CastStep const& c)
    {
        struct Caster
        {
            ObjectGuid guid;
            bool started = false;
        };
        auto const casters = std::make_shared<std::vector<Caster>>();
        schedule(delay, [selected = spec.selected, c, casters](Creature* testOwner)
        {
            Creature* unit = ObjectAccessor::GetCreature(*testOwner, selected);
            if (!unit || !unit->IsInWorld() || !unit->IsAlive())
                return;
            Registry& reg = Registry::GetOrCreate(testOwner->GetMap());
            Milliseconds const now = GameTime::GetGameTimeMS();
            std::vector<Creature*> heads{ unit };
            if (c.twin)
            {
                float const o = unit->GetOrientation() + Geo::kPi / 2.f;
                float const x = unit->GetPositionX() + Drill::kTwinApartYd * std::cos(o);
                float const y = unit->GetPositionY() + Drill::kTwinApartYd * std::sin(o);
                if (TempSummon* twin = testOwner->SummonCreature(Ids::NPC_DRILL_CASTER, x, y,
                    GroundZ(testOwner, x, y, unit->GetPositionZ()), unit->GetOrientation(), TEMPSUMMON_TIMED_DESPAWN,
                    c.ms + 1500))
                {
                    reg.TrackUnit(testOwner->GetGUID(), twin->GetGUID(), Ids::NPC_DRILL_CASTER, true);
                    MatchOwnerLevel(testOwner, twin);
                    if (Unit* victim = unit->GetVictim())
                        if (twin->IsAIEnabled && twin->AI())
                            twin->AI()->AttackStart(victim);
                    heads.push_back(twin);
                    // kill=<n> (Gnomeregan WP-G1, TB3): the casting twin is a kill target of priority n (a duplicating
                    // bug first); the entry outlives it harmlessly (no live unit of the entry, no pick).
                    if (c.kill)
                        reg.EditDescriptor(testOwner->GetGUID(), [&c](Descriptor& d)
                        {
                            for (TargetEntry const& e : d.targets)
                                if (e.entry == Ids::NPC_DRILL_CASTER && !e.guid)
                                    return;
                            d.targets.emplace_back(Ids::NPC_DRILL_CASTER, static_cast<int8>(c.kill));
                        });
                }
                else
                    LOG_DEBUG("module.bossmechanics", "Drill cast twin: could not summon {}", Ids::NPC_DRILL_CASTER);
            }
            if (c.kick)
            {
                if (unit != testOwner)
                    Mechanic::TrackUnit(testOwner, unit);
                reg.EditDescriptor(testOwner->GetGUID(), [&c](Descriptor& d)
                {
                    for (InterruptEntry const& e : d.interrupts)
                        if (e.spellId == c.spell)
                            return;
                    d.interrupts.emplace_back(0, c.spell, 1, std::chrono::milliseconds(c.everyMs));
                });
                if (c.everyMs)
                    for (Creature* head : heads)
                        reg.NoteNextCast(head->GetGUID(), head->GetEntry(), c.spell, now + Milliseconds(c.everyMs));
            }
            if (c.noKick)
            {
                // nokick=1 (Gnomeregan WP-G1, TB4 with no bug alive): a do-not-kick entry (priority 0), so the bots
                // hold their kicks; the cast must run to its end (kick=0's must-play judge below).
                if (unit != testOwner)
                    Mechanic::TrackUnit(testOwner, unit);
                reg.EditDescriptor(testOwner->GetGUID(), [&c](Descriptor& d)
                {
                    for (InterruptEntry const& e : d.interrupts)
                        if (e.spellId == c.spell)
                            return;
                    d.interrupts.emplace_back(0, c.spell, 0);
                });
            }
            for (Creature* head : heads)
            {
                Unit* target = unit->GetVictim() ? unit->GetVictim() : unit;
                SpellCastResult const result = head->CastSpell(target, c.spell, false);
                casters->push_back({ head->GetGUID(), result == SPELL_CAST_OK });
                if (result == SPELL_CAST_OK)
                    reg.NoteCastStarted(head->GetGUID(), head->GetEntry(), c.spell, now);
                else
                    LOG_DEBUG("module.bossmechanics", "Drill cast {} by {} failed ({})", c.spell,
                        head->GetGUID().ToString(), static_cast<uint32>(result));
            }
            // The twin that could not be summoned is a cast nobody had to kick: judged as never started.
            if (c.twin && heads.size() < 2)
                casters->push_back({ ObjectGuid::Empty, false });
        });
        schedule(delay + Milliseconds(c.ms - Drill::kCastJudgeLeadMs), [selected = spec.selected, c, casters](
            Creature* testOwner)
        {
            Creature* unit = ObjectAccessor::GetCreature(*testOwner, selected);
            if (!unit || !unit->IsInWorld() || !unit->IsAlive())
                return;
            for (Caster const& caster : *casters)
            {
                Creature* head = caster.guid.IsEmpty() ? nullptr : ObjectAccessor::GetCreature(*testOwner, caster.guid);
                bool running = false;
                if (head && head->IsInWorld() && head->IsAlive())
                    for (CurrentSpellTypes slot : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
                        if (Spell const* spell = head->GetCurrentSpell(slot))
                            running = running || spell->GetSpellInfo()->Id == c.spell;
                if (!Drill::CastFails(c.kick, caster.started, running))
                    continue;
                FailureEvent e;
                e.encounter = testOwner->GetGUID();
                e.tag = c.ledgerTag;
                e.kind = Kind::Soak;
                e.type = FailureEvent::Type::MissedSoak;
                e.at = GameTime::GetGameTimeMS();
                Registry::GetOrCreate(testOwner->GetMap()).PostFailure(e);
            }
        });
    }

    // Drill step `afflict` (Zul'Farrak WP-G1, MechanicDrill.h; Antu'sul AS3 / AS7): the spell on `a.count` members
    // (pick=same: the last pick first, a `marker` before it), cast by the owner as the descriptor drills' `debuff`,
    // and on the encounter's dispel list held while another player stands within `a.lone` of the carrier (Calcify's
    // spacing rule, §7 #9 A). Every kAfflictPollMs until `a.ms` a carrier that lost it (a dispel: it outlasts the step)
    // makes every other alive member within `a.burst` a Hit failure (Shatter). At the end it comes off the rest.
    void ScheduleAfflict(StepScheduler const& schedule, Milliseconds delay, TestSpec const& spec,
        Drill::AfflictStep const& a, RngPtr const& rng)
    {
        auto const carriers = std::make_shared<std::vector<ObjectGuid>>();
        schedule(delay, [spec, a, rng, carriers](Creature* testOwner)
        {
            Registry& reg = Registry::GetOrCreate(testOwner->GetMap());
            reg.AddScopePlayer(testOwner->GetGUID(), spec.gm);
            reg.EditDescriptor(testOwner->GetGUID(), [&a](Descriptor& d)
            {
                for (DispelEntry const& e : d.dispels)
                    if (e.auraId == a.spell)
                        return;
                d.dispels.emplace_back(a.spell, 1, false, std::chrono::milliseconds(0),
                    BossMechanics::Condition::Not(BossMechanics::Condition::NearPlayer(a.lone)));
            });
            TestSpec pick = spec;
            pick.pick = a.same ? TestSpec::Pick::Same : TestSpec::Pick::Any;
            for (uint32 k = 0; k < a.count; ++k)
            {
                Player* p = PickMember(testOwner, pick, *rng);
                pick.pick = TestSpec::Pick::Fresh;
                if (!p || std::find(carriers->begin(), carriers->end(), p->GetGUID()) != carriers->end())
                    continue;
                if (testOwner->AddAura(a.spell, p))
                    carriers->push_back(p->GetGUID());
            }
        });
        uint32 const polls = a.ms / Drill::kAfflictPollMs;
        for (uint32 k = 1; k <= polls; ++k)
        {
            bool const last = k == polls;
            schedule(delay + Milliseconds(static_cast<int64>(k) * Drill::kAfflictPollMs), [spec, a, carriers, last](
                Creature* testOwner)
            {
                std::vector<Player*> const members = GroupMembers(testOwner, spec.gm, spec.includeGm);
                for (auto it = carriers->begin(); it != carriers->end();)
                {
                    Player* carrier = ObjectAccessor::GetPlayer(*testOwner, *it);
                    if (!carrier || !carrier->IsAlive())
                    {
                        it = carriers->erase(it);
                        continue;
                    }
                    if (carrier->HasAura(a.spell))
                    {
                        if (last)
                            carrier->RemoveAurasDueToSpell(a.spell);
                        ++it;
                        continue;
                    }
                    Geo::Vec3 const at{ carrier->GetPositionX(), carrier->GetPositionY(), carrier->GetPositionZ() };
                    for (Player const* other : members)
                        if (other != carrier && other->IsAlive() && Drill::ShatterHits(at, { other->GetPositionX(),
                            other->GetPositionY(), other->GetPositionZ() }, a.burst))
                            PostDrillFailure(testOwner, a.ledgerTag, other->GetGUID(), FailureEvent::Type::Hit);
                    it = carriers->erase(it);
                }
            });
        }
    }

    // Drill step `partner` (Zul'Farrak WP-G1; Ukorz UK1, Fury of the Sands' meter): a twin of `p.entry` (tracked, the
    // owner's level, the selected unit's health) `p.apart` yd to the selected unit's left, sent at its victim, for
    // `p.ms`. The kill order starts on the selected unit and flips every `p.everyMs`: the favored one is a
    // TargetEntry by guid at priority 2, the other at 1 and ignored by the DPS bots (IgnoreEntry.For(Who::Dps), BM-1:
    // out of their attacker counts, so no AoE on it). Every `p.tickMs` outside `p.graceMs` of a flip, each DPS member
    // (by the role provider) whose victim (else selection) is the disfavored unit is an OffSpot failure. At the end the
    // entries come off and the twin goes.
    void SchedulePartner(StepScheduler const& schedule, Milliseconds delay, TestSpec const& spec,
        Drill::PartnerStep const& p)
    {
        auto const partner = std::make_shared<ObjectGuid>();
        auto const order = [](Creature* testOwner, ObjectGuid selected, ObjectGuid twin, bool selectedFavored,
            bool clear)
        {
            uint64 const a = selected.GetRawValue();
            uint64 const b = twin.GetRawValue();
            Registry::GetOrCreate(testOwner->GetMap()).EditDescriptor(testOwner->GetGUID(),
                [a, b, selectedFavored, clear](Descriptor& d)
            {
                auto const ours = [a, b](uint64 g) { return g == a || g == b; };
                d.targets.erase(std::remove_if(d.targets.begin(), d.targets.end(), [&ours](TargetEntry const& e)
                {
                    return e.guid && ours(e.guid);
                }), d.targets.end());
                d.ignores.erase(std::remove_if(d.ignores.begin(), d.ignores.end(), [&ours](IgnoreEntry const& e)
                {
                    return e.guid && ours(e.guid);
                }), d.ignores.end());
                if (clear)
                    return;
                uint64 const favored = selectedFavored ? a : b;
                uint64 const other = selectedFavored ? b : a;
                d.targets.push_back(TargetEntry::ForUnit(favored, 2));
                d.targets.push_back(TargetEntry::ForUnit(other, 1));
                d.ignores.push_back(IgnoreEntry::ForUnit(other).For(Who::Dps));
            });
        };
        schedule(delay, [spec, p, partner, order](Creature* testOwner)
        {
            Creature* unit = ObjectAccessor::GetCreature(*testOwner, spec.selected);
            if (!unit || !unit->IsInWorld() || !unit->IsAlive())
                return;
            float const o = unit->GetOrientation() + Geo::kPi / 2.f;
            float const x = unit->GetPositionX() + p.apart * std::cos(o);
            float const y = unit->GetPositionY() + p.apart * std::sin(o);
            TempSummon* twin = testOwner->SummonCreature(p.entry, x, y, GroundZ(testOwner, x, y, unit->GetPositionZ()),
                unit->GetOrientation(), TEMPSUMMON_TIMED_DESPAWN, p.ms + 500);
            if (!twin)
            {
                LOG_ERROR("module.bossmechanics", "Drill partner: could not summon creature {}", p.entry);
                return;
            }
            Registry& reg = Registry::GetOrCreate(testOwner->GetMap());
            reg.AddScopePlayer(testOwner->GetGUID(), spec.gm);
            reg.TrackUnit(testOwner->GetGUID(), twin->GetGUID(), p.entry, true);
            if (unit != testOwner)
                Mechanic::TrackUnit(testOwner, unit);
            MatchOwnerLevel(testOwner, twin);
            // A twin boss: the selected unit's health, so it outlives the flips.
            uint32 const health = std::max<uint32>(1, unit->GetMaxHealth());
            twin->SetCreateHealth(health);
            twin->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(health));
            twin->UpdateMaxHealth();
            twin->SetHealth(std::max<uint32>(1, unit->GetHealth()));
            if (Unit* victim = unit->GetVictim())
                if (twin->IsAIEnabled && twin->AI())
                    twin->AI()->AttackStart(victim);
            *partner = twin->GetGUID();
            order(testOwner, spec.selected, *partner, true, false);
        });
        for (uint32 at = p.everyMs; at < p.ms; at += p.everyMs)
            schedule(delay + Milliseconds(at), [spec, partner, order, favored = Drill::PartnerSelectedFavored(at,
                p.everyMs)](Creature* testOwner)
            {
                if (!partner->IsEmpty())
                    order(testOwner, spec.selected, *partner, favored, false);
            });
        for (uint32 at = p.tickMs; at < p.ms; at += p.tickMs)
        {
            if (!Drill::PartnerJudged(at, p.everyMs, p.graceMs))
                continue;
            schedule(delay + Milliseconds(at), [spec, p, partner, at](Creature* testOwner)
            {
                Creature* twin = partner->IsEmpty() ? nullptr : ObjectAccessor::GetCreature(*testOwner, *partner);
                Unit* unit = ObjectAccessor::GetUnit(*testOwner, spec.selected);
                if (!twin || !twin->IsAlive() || !unit || !unit->IsAlive())
                    return;
                ObjectGuid const disfavored = Drill::PartnerSelectedFavored(at, p.everyMs) ? *partner : spec.selected;
                for (Player const* m : GroupMembers(testOwner, spec.gm, spec.includeGm))
                {
                    Role const role = GetRoleProvider().Get(unit, m);
                    if (!m->IsAlive() || HasRole(role, Role::Tank) || HasRole(role, Role::Healer))
                        continue;
                    ObjectGuid const on = m->GetVictim() ? m->GetVictim()->GetGUID() : m->GetTarget();
                    if (on == disfavored)
                        PostDrillFailure(testOwner, p.ledgerTag, m->GetGUID(), FailureEvent::Type::OffSpot);
                }
            });
        }
        schedule(delay + Milliseconds(p.ms), [spec, partner, order](Creature* testOwner)
        {
            if (partner->IsEmpty())
                return;
            order(testOwner, spec.selected, *partner, true, true);
            if (Creature* twin = ObjectAccessor::GetCreature(*testOwner, *partner))
                twin->DespawnOrUnsummon();
        });
    }

    // Drill step `stalk` (Zul'Farrak WP-G1; Ukorz UK3 + UK4, Bladestorm): for `s.ms` the selected creature walks at
    // `s.speed` yd/s toward a member picked anew every `s.everyMs` (relocated every kZfStalkStepMs, facing it; the
    // sim's rooted stand-in moves too), never past it. With trail=<r>: a lasting circle of r (no damage, its 0.5 s
    // warning, `s.trailMs`, tag `s.trailTag`) where it stands each kZfTrailEveryYd walked (Bladestorm's sand). home=1:
    // back where it started at the end. A creature only: never a player.
    void ScheduleStalk(StepScheduler const& schedule, Milliseconds delay, TestSpec const& spec,
        Drill::StalkStep const& st, RngPtr const& rng)
    {
        struct State
        {
            std::optional<Position> home;
            ObjectGuid target;
            Drill::TrailMeter meter;
        };
        auto const state = std::make_shared<State>();
        uint32 const steps = st.ms / Drill::kZfStalkStepMs;
        float const step = st.speed * static_cast<float>(Drill::kZfStalkStepMs) / 1000.f;
        for (uint32 k = 0; k <= steps; ++k)
        {
            uint32 const elapsed = k * Drill::kZfStalkStepMs;
            schedule(delay + Milliseconds(elapsed), [spec, st, rng, state, k, steps, elapsed, step](
                Creature* testOwner)
            {
                Creature* unit = ObjectAccessor::GetCreature(*testOwner, spec.selected);
                if (!unit || !unit->IsInWorld() || !unit->IsAlive())
                    return;
                if (!state->home)
                    state->home = unit->GetPosition();
                if (k == steps)
                {
                    if (st.home)
                        unit->NearTeleportTo(state->home->GetPositionX(), state->home->GetPositionY(),
                            state->home->GetPositionZ(), state->home->GetOrientation());
                    return;
                }
                if (elapsed % st.everyMs == 0)
                {
                    TestSpec pick = spec;
                    pick.pick = TestSpec::Pick::Any;
                    if (Player* p = PickMember(testOwner, pick, *rng))
                        state->target = p->GetGUID();
                }
                Player* target = state->target.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*unit, state->target);
                if (!target || !target->IsAlive() || target->GetMap() != unit->GetMap())
                    return;
                Geo::Vec3 const from{ unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ() };
                Geo::Vec3 const next = Drill::ChaseStep(from, { target->GetPositionX(), target->GetPositionY(),
                    target->GetPositionZ() }, step / 0.25f, 0.25f);
                float const walked = std::hypot(next.x - from.x, next.y - from.y);
                float const z = GroundZ(testOwner, next.x, next.y, unit->GetPositionZ());
                unit->NearTeleportTo(next.x, next.y, z, unit->GetAngle(target));
                if (st.trail <= 0.f || !state->meter.Walk(walked))
                    return;
                TestSpec trail;
                trail.shape = "circle";
                trail.size = st.trail;
                trail.warn = Milliseconds(500);
                trail.persist = Milliseconds(st.trailMs);
                trail.tick = Milliseconds(1000);
                trail.dmg = -1;
                trail.pct = 0.f;
                trail.tag = st.trailTag;
                trail.gm = spec.gm;
                trail.includeGm = spec.includeGm;
                trail.selected = spec.selected;
                trail.facing = spec.facing;
                trail.point = Position(next.x, next.y, z, spec.facing);
                std::string ignored;
                RegisterTest(testOwner, trail, ignored);
            });
        }
    }

    // Drill step `floes` (Zul'Farrak WP-G1; the Deluge GZ22 / GZ23): the floes' circles round the selected unit's spot
    // now (FloeCenters, a random rotation) are the strict bounds of every member for `f.ms` (a Spot with no spots
    // carries them, as Incendius's causeway; Within ... Strict: a bot off them walks on), drawn as lasting decoys.
    // From `f.graceMs` every `f.tickMs`, each alive member on none of them is an OffSpot failure (in the water). With
    // `f.inset` the bounds are each floe shrunk by it (the live bots' bounds, D7Hb); the look and the judge keep r.
    void ScheduleFloes(StepScheduler const& schedule, Milliseconds delay, TestSpec const& spec,
        Drill::FloesStep const& f, RngPtr const& rng)
    {
        auto const centers = std::make_shared<std::vector<Geo::Vec3>>();
        schedule(delay, [spec, f, rng, centers](Creature* testOwner)
        {
            Unit* unit = ObjectAccessor::GetUnit(*testOwner, spec.selected);
            if (!unit || !unit->IsInWorld())
                return;
            *centers = Drill::FloeCenters({ unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ() }, f,
                rng->rng.Float(0.f, Geo::kTwoPi));
            SpotBounds bounds;
            for (Geo::Vec3& c : *centers)
            {
                c.z = GroundZ(testOwner, c.x, c.y, c.z);
                bounds.Circle(Position(c.x, c.y, c.z, 0.f), f.r - f.inset);     // inset=: the bots' bounds only
            }
            bounds.Strict();
            Registry::GetOrCreate(testOwner->GetMap()).AddScopePlayer(testOwner->GetGUID(), spec.gm);
            MechanicRef ref = Mechanic::Spot(testOwner, {}, Milliseconds(f.ms));
            ref.NoTelegraph().Within(bounds).Tag(f.ledgerTag);
            for (Geo::Vec3 const& c : *centers)
            {
                TestSpec look;
                look.shape = "circle";
                look.size = f.r;
                look.warn = Milliseconds(500);
                look.persist = Milliseconds(f.ms > 500 ? f.ms - 500 : 0);
                look.decoy = true;
                look.color = Color::Teal;
                look.tag = f.ledgerTag;
                look.gm = spec.gm;
                look.includeGm = spec.includeGm;
                look.selected = spec.selected;
                look.facing = spec.facing;
                look.point = Position(c.x, c.y, c.z, spec.facing);
                std::string ignored;
                RegisterTest(testOwner, look, ignored);
            }
        });
        for (uint32 at = f.graceMs; at < f.ms; at += f.tickMs)
            schedule(delay + Milliseconds(at), [spec, f, centers](Creature* testOwner)
            {
                if (centers->empty())
                    return;
                for (Player const* m : GroupMembers(testOwner, spec.gm, spec.includeGm))
                    if (m->IsAlive() && !Drill::OnAnyFloe(*centers, f.r, { m->GetPositionX(), m->GetPositionY(),
                        m->GetPositionZ() }))
                        PostDrillFailure(testOwner, f.ledgerTag, m->GetGUID(), FailureEvent::Type::OffSpot);
            });
    }

    // Drill step `lights` (Gnomeregan WP-G1, MechanicDrill.h LightsStep; the Alert System's MA1-MA4 and the hard mode's
    // MT23, as KT-G's TurningLights runs them live): every kLightsTickMs the clock turns (the dark start and a
    // reversal's stop hold it), and once lit each alive member inside a light (2D, InLightCone round the unit where it
    // stands now) is Spotted once per latch, shared by every light: a Hit failure `Zone/<tag>`, and `faster` percent
    // more speed. Every `step` ms each light's floor over the next `ahead` ms (LightsClock::Swept) registers as a
    // lasting no-damage cone on the unit's spot, hot `step` + kLightsOverlapMs past its warning, so the bots read the
    // swept floor with no gap (its contacts are reported under LightsFloorTag). Nothing is re-registered on a ring's
    // resolve instant: the cones carry no damage (RB-29).
    void ScheduleLights(StepScheduler const& schedule, Milliseconds delay, TestSpec const& spec,
        Drill::LightsStep const& l, RngPtr const& rng)
    {
        struct State
        {
            Drill::LightsClock clock;
            std::unordered_map<ObjectGuid, uint32> spottedAt;   // each player's last Spotted (ms into the step)
            bool started = false;
        };
        auto const state = std::make_shared<State>();
        uint32 const ticks = l.ms / Drill::kLightsTickMs;
        for (uint32 k = 0; k < ticks; ++k)
        {
            uint32 const elapsed = k * Drill::kLightsTickMs;
            schedule(delay + Milliseconds(elapsed), [spec, l, rng, state, elapsed](Creature* testOwner)
            {
                Unit* unit = ObjectAccessor::GetUnit(*testOwner, spec.selected);
                if (!unit || !unit->IsInWorld() || !unit->IsAlive())
                    return;
                if (!state->started)
                {
                    // light 0's start: start= from the run's facing, pick=any at a random member, else anywhere
                    float start = rng->rng.Float(0.f, Geo::kTwoPi);
                    if (l.startDeg)
                        start = spec.facing + Geo::DegToRad(*l.startDeg);
                    else if (l.pickMember)
                    {
                        TestSpec pick = spec;
                        pick.pick = TestSpec::Pick::Any;
                        if (Player* p = PickMember(testOwner, pick, *rng))
                            start = unit->GetAngle(p);
                    }
                    state->clock = Drill::LightsClock(start, l.degPerSec, l.direction, l.litMs);
                    state->started = true;
                }
                else
                {
                    state->clock.Advance(Drill::kLightsTickMs);
                    if (Drill::LightsReverseDue(elapsed, l.litMs, l.reverseMs))
                        state->clock.Reverse(l.stopMs);
                }
                Geo::Vec3 const apex{ unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ() };
                if (elapsed >= l.litMs)
                    for (Player const* m : GroupMembers(testOwner, spec.gm, spec.includeGm))
                    {
                        if (!m->IsAlive())
                            continue;
                        Geo::Vec3 const p{ m->GetPositionX(), m->GetPositionY(), m->GetPositionZ() };
                        bool lit = false;
                        for (uint32 i = 0; i < l.count && !lit; ++i)
                            lit = Drill::InLightCone(apex, state->clock.BearingOf(i, l.count), l.angleDeg, l.lengthYd,
                                p);
                        if (!lit)
                            continue;
                        auto const last = state->spottedAt.find(m->GetGUID());
                        if (last != state->spottedAt.end() && elapsed < last->second + l.latchMs)
                            continue;
                        state->spottedAt[m->GetGUID()] = elapsed;
                        PostDrillFailure(testOwner, l.ledgerTag, m->GetGUID(), FailureEvent::Type::Hit);
                        if (l.fasterPct > 0.f)
                            state->clock.SetSpeed(Drill::LightsFaster(state->clock.DegPerSec(), l.fasterPct));
                    }
                if (elapsed % l.stepMs != 0)
                    return;
                uint32 const left = l.ms - elapsed;
                uint32 const persist = std::min(Drill::LightsPersistMs(l.stepMs),
                    left > Drill::kLightsWarnMs ? left - Drill::kLightsWarnMs : 0u);
                if (!persist)
                    return;
                for (uint32 i = 0; i < l.count; ++i)
                {
                    Drill::LightSector const sector = state->clock.Swept(i, l.count, l.angleDeg, l.aheadMs);
                    TestSpec cone;
                    cone.shape = "cone";
                    cone.size = l.lengthYd;
                    cone.angle = sector.angleDeg;
                    cone.warn = Milliseconds(Drill::kLightsWarnMs);
                    cone.persist = Milliseconds(persist);
                    cone.tick = Milliseconds(1000);
                    cone.dmg = -1;
                    cone.pct = 0.f;
                    cone.color = Color::Yellow;
                    cone.tag = Drill::LightsFloorTag(l.ledgerTag);
                    cone.gm = spec.gm;
                    cone.includeGm = spec.includeGm;
                    cone.selected = spec.selected;
                    cone.facing = sector.bearing;
                    cone.point = Position(apex.x, apex.y, apex.z, sector.bearing);
                    std::string ignored;
                    RegisterTest(testOwner, cone, ignored);
                }
            });
        }
    }

    // Drill step `movecost` (Gnomeregan WP-G1, MechanicDrill.h MoveCostStep; Static's bot rule, BM-G §7 #12 A): for
    // `m.ms` the owner's descriptor carries "more than `above` Marked (drill) stacks: no optional move" and, unless
    // band=0, the ranged band round the owner; the step takes exactly its own entries off at the end (a `describe` or
    // another step's entries stay).
    void ScheduleMoveCost(StepScheduler const& schedule, Milliseconds delay, TestSpec const& spec,
        Drill::MoveCostStep const& m)
    {
        schedule(delay, [gm = spec.gm, m](Creature* testOwner)
        {
            Registry::GetOrCreate(testOwner->GetMap()).AddScopePlayer(testOwner->GetGUID(), gm);
            Registry::GetOrCreate(testOwner->GetMap()).EditDescriptor(testOwner->GetGUID(), [&m](Descriptor& d)
            {
                d.moveCosts.emplace_back(Ids::SPELL_MARKED_DRILL, m.above);
                if (m.band)
                    d.rangeBands.emplace_back(m.bandMinYd, m.bandMaxYd);
            });
        });
        schedule(delay + Milliseconds(m.ms), [m](Creature* testOwner)
        {
            Registry::GetOrCreate(testOwner->GetMap()).EditDescriptor(testOwner->GetGUID(), [&m](Descriptor& d)
            {
                auto const cost = std::find_if(d.moveCosts.begin(), d.moveCosts.end(), [&m](MoveCostEntry const& e)
                {
                    return e.auraId == Ids::SPELL_MARKED_DRILL && e.aboveStacks == m.above;
                });
                if (cost != d.moveCosts.end())
                    d.moveCosts.erase(cost);
                if (!m.band)
                    return;
                auto const band = std::find_if(d.rangeBands.begin(), d.rangeBands.end(), [&m](RangeBandEntry const& e)
                {
                    return e.minYd == m.bandMinYd && e.maxYd == m.bandMaxYd && e.who == Who::Any;
                });
                if (band != d.rangeBands.end())
                    d.rangeBands.erase(band);
            });
        });
    }

    // Drill step `saturated` (Gnomeregan WP-G1, MechanicDrill.h SaturatedStep; Viscous Fallout VF5, §7 #38 b A): every
    // `tick` ms the selected unit inside an armed lasting point circle of the owner (InArmedPool: past its warning) is
    // an OffSpot failure on its victim, the tank who should have dragged it out (L47, BM drag-out).
    void ScheduleSaturated(StepScheduler const& schedule, Milliseconds delay, TestSpec const& spec,
        Drill::SaturatedStep const& st)
    {
        for (uint32 at = st.tickMs; at <= st.ms; at += st.tickMs)
            schedule(delay + Milliseconds(at), [selected = spec.selected, st](Creature* testOwner)
            {
                Unit* unit = ObjectAccessor::GetUnit(*testOwner, selected);
                if (!unit || !unit->IsInWorld() || !unit->IsAlive())
                    return;
                Player* tank = unit->GetVictim() ? unit->GetVictim()->ToPlayer() : nullptr;
                Registry const* reg = Registry::Get(testOwner->GetMap());
                if (!tank || !reg)
                    return;
                Milliseconds const now = GameTime::GetGameTimeMS();
                bool inside = false;
                Position const pos = unit->GetPosition();
                reg->ForEachActive(now, [&inside, &pos, testOwner, now](ActiveMechanic const& m)
                {
                    if (inside || m.owner != testOwner->GetGUID() || !m.persist || m.decoy || m.anchor.IsUnit()
                        || !m.IsShapeKind() || m.shape.type != ShapeType::Circle)
                        return;
                    inside = Drill::InArmedPool(m.anchor.point.GetExactDist2d(&pos), m.shape.radius,
                        static_cast<uint64>(now.count()), static_cast<uint64>(m.resolveAt.count()));
                });
                if (inside)
                    PostDrillFailure(testOwner, st.ledgerTag, tank->GetGUID(), FailureEvent::Type::OffSpot);
            });
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
            bool one = false;                           // `marker 0 one=1` (Ragefire Chasm WP-G1)
            std::optional<Drill::BlinkStep> blink;      // `blink`
            std::optional<Drill::CastStep> cast;        // `cast`
            std::optional<Drill::WalkStep> walk;        // `walk` (Blackrock Depths WP-G1)
            std::optional<Drill::LeashStep> leash;      // `leash`
            std::optional<Drill::ChaseSpec> chase;      // `chase` (Scarlet Monastery WP-G1)
            std::optional<uint32> holdFacingMs;         // `holdfacing <ms>` (Scarlet Monastery WP-G2)
            std::optional<Drill::AfflictStep> afflict;  // `afflict` (Zul'Farrak WP-G1)
            std::optional<Drill::PartnerStep> partner;  // `partner`
            std::optional<Drill::StalkStep> stalk;      // `stalk`
            std::optional<Drill::FloesStep> floes;      // `floes`
            std::optional<Drill::LightsStep> lights;    // `lights` (Gnomeregan WP-G1)
            std::optional<Drill::MoveCostStep> moveCost;    // `movecost`
            std::optional<Drill::SaturatedStep> saturated;  // `saturated`
            std::vector<uint32> repeats{ 0 };           // repeat= / every=: each repeat's offset (Drill::RepeatOffsets)
            TestSpec spec;
        };
        ParseCtx const ctx = CtxOf(scope, scopeGuid);
        std::vector<Planned> planned;
        for (Drill::Step const& step : def.steps)
        {
            if (!Drill::PlanKeysOk(step.line))
            {
                err = Acore::StringFormat("Drill {}: step '{}': bad after= (resolve, with) or cycle= (even, odd, "
                    "<k>/<m>)", def.name, step.line);
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
                if (pick && *pick != "nontank" && *pick != "any" && *pick != "healer" && *pick != "unmarked")
                {
                    err = Acore::StringFormat("Drill {}: bad step '{}'", def.name, step.line);
                    return false;
                }
                p.spec.pick = pick && *pick == "nontank" ? TestSpec::Pick::NonTank
                    : pick && *pick == "healer" ? TestSpec::Pick::Healer
                    : pick && *pick == "unmarked" ? TestSpec::Pick::Unmarked : TestSpec::Pick::Any;
                std::optional<std::string_view> const one = Drill::Value(line, "one");
                if (one && (*one != "1" || *n))
                {
                    err = Acore::StringFormat("Drill {}: bad step '{}' (one=1 is for marker 0)", def.name, step.line);
                    return false;
                }
                p.one = one.has_value();
                planned.push_back(std::move(p));
                continue;
            }
            if (!head.empty() && (head[0] == "blink" || head[0] == "cast"))
            {
                // Drill-only (MechanicDrill.h "Ragefire Chasm shapes"): the selected unit appears elsewhere, or casts.
                if (head[0] == "blink")
                    p.blink = Drill::ParseBlink(line);
                else
                    p.cast = Drill::ParseCast(line);
                if (!p.blink && !p.cast)
                {
                    err = Acore::StringFormat("Drill {}: bad step '{}' (blink <yd> 0 [pick=any|fresh|nontank|same], "
                        "blink 0 0 at=prev|home=1, cast <spell> <ms> [kick=1] [every=<ms>] tag=<n>)", def.name,
                        step.line);
                    return false;
                }
                if (p.cast && !sSpellMgr->GetSpellInfo(p.cast->spell))
                {
                    err = Acore::StringFormat("Drill {}: step '{}': no spell {}", def.name, step.line, p.cast->spell);
                    return false;
                }
                p.spec.gm = scopeGuid;
                p.spec.includeGm = self;
                p.spec.selected = boss->GetGUID();
                if (p.blink)
                    p.spec.pick = p.blink->same ? TestSpec::Pick::Same
                        : p.blink->pick == Drill::PickRule::Fresh ? TestSpec::Pick::Fresh
                        : p.blink->pick == Drill::PickRule::NonTank ? TestSpec::Pick::NonTank : TestSpec::Pick::Any;
                planned.push_back(std::move(p));
                continue;
            }
            if (!head.empty() && (head[0] == "afflict" || head[0] == "partner" || head[0] == "stalk"
                || head[0] == "floes"))
            {
                // Drill-only (MechanicDrill.h "Zul'Farrak shapes"): a dispellable debuff with a spacing rule, a twin
                // and a flipping kill order, the selected unit walking at members, strict floes; their judges post
                // to the ledger themselves.
                if (head[0] == "afflict")
                    p.afflict = Drill::ParseAfflict(line);
                else if (head[0] == "partner")
                    p.partner = Drill::ParsePartner(line);
                else if (head[0] == "stalk")
                    p.stalk = Drill::ParseStalk(line);
                else
                    p.floes = Drill::ParseFloes(line);
                if (!p.afflict && !p.partner && !p.stalk && !p.floes)
                {
                    err = Acore::StringFormat("Drill {}: bad step '{}' (afflict <spell> <ms> [n=] [pick=any|same] "
                        "[lone=] [burst=] tag=<n>, partner <entry> <ms> [every=] [grace=] [tick=] [apart=] tag=<n>, "
                        "stalk <speed> <ms> [every=] [trail=] [trailms=] [trailtag=] [home=1], floes <r> <ms> n=<n> "
                        "ring=<yd> [center=1] [inset=] [grace=] [tick=] tag=<n>)", def.name, step.line);
                    return false;
                }
                if (p.afflict && !sSpellMgr->GetSpellInfo(p.afflict->spell))
                {
                    err = Acore::StringFormat("Drill {}: step '{}': no spell {}", def.name, step.line,
                        p.afflict->spell);
                    return false;
                }
                if (p.partner && !sObjectMgr->GetCreatureTemplate(p.partner->entry))
                {
                    err = Acore::StringFormat("Drill {}: step '{}': no creature {}", def.name, step.line,
                        p.partner->entry);
                    return false;
                }
                p.spec.gm = scopeGuid;
                p.spec.includeGm = self;
                p.spec.selected = boss->GetGUID();
                p.spec.facing = ctx.facing;
                planned.push_back(std::move(p));
                continue;
            }
            if (!head.empty() && (head[0] == "lights" || head[0] == "movecost" || head[0] == "saturated"))
            {
                // Drill-only (MechanicDrill.h "Gnomeregan shapes"): turning lights with their own Spotted judge, BM-G's
                // move cost and ranged band for a while, the boss in an armed pool; nothing deals damage.
                if (head[0] == "lights")
                    p.lights = Drill::ParseLights(line);
                else if (head[0] == "movecost")
                    p.moveCost = Drill::ParseMoveCost(line);
                else
                    p.saturated = Drill::ParseSaturated(line);
                if (!p.lights && !p.moveCost && !p.saturated)
                {
                    err = Acore::StringFormat("Drill {}: bad step '{}' (lights <angle> <ms> [n=] [len=] [speed=] "
                        "[dir=1|-1] [start=<deg>|pick=any] [lit=] [reverse=] [stop=] [faster=] [latch=] [ahead=] "
                        "[step=] tag=<n>, movecost <above> <ms> [band=<min>:<max>|0], saturated <tick> <ms> tag=<n>)",
                        def.name, step.line);
                    return false;
                }
                p.spec.gm = scopeGuid;
                p.spec.includeGm = self;
                p.spec.selected = boss->GetGUID();
                p.spec.facing = ctx.facing;
                planned.push_back(std::move(p));
                continue;
            }
            if (!head.empty() && head[0] == "holdfacing")
            {
                // Drill-only (MechanicDrill.h "Scarlet Monastery shapes, wave 2"): the selected unit's bot tank holds
                // its facing for <ms> (Mechanic::HoldTankFacing, SM-BM F6); nothing registers.
                p.holdFacingMs = Drill::ParseHoldFacing(line);
                if (!p.holdFacingMs)
                {
                    err = Acore::StringFormat("Drill {}: bad step '{}' (holdfacing <ms>)", def.name, step.line);
                    return false;
                }
                p.spec.gm = scopeGuid;
                p.spec.includeGm = self;
                p.spec.selected = boss->GetGUID();
                planned.push_back(std::move(p));
                continue;
            }
            if (!head.empty() && (head[0] == "walk" || head[0] == "leash" || head[0] == "chase"))
            {
                // Drill-only (MechanicDrill.h "Blackrock Depths shapes", "Scarlet Monastery shapes"): the selected
                // unit walks or chases its victim, or the scope members it leaves behind take ledger failures;
                // nothing registers.
                if (head[0] == "walk")
                    p.walk = Drill::ParseWalk(line);
                else if (head[0] == "leash")
                    p.leash = Drill::ParseLeash(line);
                else
                    p.chase = Drill::ParseChase(line);
                if (!p.walk && !p.leash && !p.chase)
                {
                    err = Acore::StringFormat("Drill {}: bad step '{}' (walk <yd> <ms> [offset=<deg>], leash <yd> "
                        "<ms> [role=tank|healer|nonhealer|any] [anchor=tank|self] [tick=<ms>] [near=1] tag=<n>, "
                        "chase <yd> <ms> [home=1])", def.name, step.line);
                    return false;
                }
                p.spec.gm = scopeGuid;
                p.spec.includeGm = self;
                p.spec.selected = boss->GetGUID();
                p.spec.facing = ctx.facing;
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
            // repeat= / every= (Scarlet Monastery WP-G2): the runner used to drop them; each repeat keeps the step's
            // tag (a drill's tag names its mechanic: `pass-eggs`' 40 eggs are all Zone/60).
            p.repeats = Drill::RepeatOffsets(repeat, static_cast<uint32>(every.count()));
            planned.push_back(std::move(p));
        }

        bool const follow = Drill::OwnerFollowsBoss(def, owner == boss);
        if (follow)
            PlaceTestOwner(owner, boss->GetPosition());
        // In time order, so the test owner's idle clock (ScheduleTest) ends at the last step.
        StepScheduler const schedule(owner);
        auto const blinkHome = std::make_shared<std::optional<Position>>();     // `blink ... home=1`
        for (Drill::ScheduledStep const& at : Drill::StepSchedule(def, cycles))
        {
            Planned const& p = planned[at.step];
            Milliseconds const delay(static_cast<int64>(at.delayMs));
            if (p.marker)
                schedule(delay, [spec = p.spec, stacks = p.stacks, one = p.one, rng](Creature* testOwner)
                {
                    DrillMarker(testOwner, spec, stacks, one, *rng);
                });
            else if (p.blink)
                ScheduleBlink(schedule, delay, p.spec, *p.blink, rng, blinkHome);
            else if (p.cast)
                ScheduleCast(schedule, delay, p.spec, *p.cast);
            else if (p.walk)
                ScheduleWalk(schedule, delay, p.spec, *p.walk);
            else if (p.leash)
                ScheduleLeash(schedule, delay, p.spec, *p.leash);
            else if (p.chase)
                ScheduleChase(schedule, delay, p.spec, *p.chase);
            else if (p.afflict)
                ScheduleAfflict(schedule, delay, p.spec, *p.afflict, rng);
            else if (p.partner)
                SchedulePartner(schedule, delay, p.spec, *p.partner);
            else if (p.stalk)
                ScheduleStalk(schedule, delay, p.spec, *p.stalk, rng);
            else if (p.floes)
                ScheduleFloes(schedule, delay, p.spec, *p.floes, rng);
            else if (p.lights)
                ScheduleLights(schedule, delay, p.spec, *p.lights, rng);
            else if (p.moveCost)
                ScheduleMoveCost(schedule, delay, p.spec, *p.moveCost);
            else if (p.saturated)
                ScheduleSaturated(schedule, delay, p.spec, *p.saturated);
            else if (p.holdFacingMs)
                schedule(delay, [selected = p.spec.selected, ms = *p.holdFacingMs](Creature* testOwner)
                {
                    if (Unit* unit = ObjectAccessor::GetUnit(*testOwner, selected))
                        if (unit->IsInWorld() && unit->IsAlive())
                            Mechanic::HoldTankFacing(testOwner, unit, Milliseconds(ms));
                });
            else
                for (uint32 offset : p.repeats)
                    schedule(delay + Milliseconds(offset), [spec = p.spec, rng, follow](Creature* testOwner)
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
                handler->PSendSysMessage("#{} {} {} {} tag {} | resolve {:+} ms, expire {:+} ms | {} markers{}{}{}{}{}",
                    m.id, KindName(m.kind), DescribeShape(m.shape), ColorName(m.color), m.tag,
                    (m.resolveAt - now).count(), (m.expireAt - now).count(), m.markers.size(),
                    m.front ? " front" : (m.projectile ? " projectile" : ""), m.mine ? (m.mine->armed ? " mine(armed)"
                    : " mine") : "",
                    m.persist ? " persistent" : "", m.decoy ? " decoy" : "", m.lethal ? " lethal" : "");
                // Hide (Blackrock Depths WP-HK): the blockers alive and the spots of the last placement.
                if (m.hide)
                    handler->PSendSysMessage("    hide: {} of {} blocker(s) live, {} spot(s) ({} not good), placed {} "
                        "time(s)", m.hide->live.size(), m.hide->spec.blockers.size(), m.hide->spots.size(),
                        m.hide->badSpots, m.hide->placements);
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
