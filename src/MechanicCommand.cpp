/*
 * mod-boss-mechanics: the `.mech` GM command (STAGE-M1-HANDOFF §3.10). SEC_GAMEMASTER, in game only.
 *
 *   .mech test <shape> <size> <ms> [key=value ...]
 *   .mech clear [all] | .mech list | .mech ledger [name] | .mech weakened <0-7> | .mech markers [range]
 *   .mech palette <get-out color> <get-in color> [cross-lane color] [cell color]
 *                                                    (red yellow blue purple orange green teal white black)
 *   .mech barrage [secs=30] [every=900] [warn=2000] [shapes=circle,donut,cone,line,projectile] [dmg=1] [self=0]
 *                 [seed=]
 *   .mech minefield [count=10] [radius=3] [spread=15] [arm=3000] [grace=500] [dmg=1] [seed=]
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
 * Commands run on the world thread while no map updates (handoff §2.1), so they may write the registry directly.
 */

#include "BossMechanics.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "GameTime.h"
#include "Group.h"
#include "Map.h"
#include "MechanicBarrage.h"
#include "MechanicBotReportHook.h"
#include "MechanicMarkerAI.h"
#include "MechanicTelegraph.h"
#include "MechanicWeakened.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "StringConvert.h"
#include "TemporarySummon.h"
#include "Tokenize.h"
#include <algorithm>
#include <cmath>
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

    // The dice of one command. Delayed registrations share it, so they keep drawing from the same seeded sequence.
    using RngPtr = std::shared_ptr<Barrage::Rng>;

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
        return std::make_shared<Barrage::Rng>(seed);
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
        bool includeGm = false;
        float scatter = 0.f;

        // common keys
        int32 dmg = -1;
        float pct = 20.f;
        float tankPct = -1.f;
        uint8 school = 0;
        bool weak = false;
        bool once = false;
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
    };

    bool IsShapeName(std::string const& s)
    {
        static char const* const shapes[] = { "circle", "donut", "cone", "rearcone", "line", "projectile", "mine",
            "lanes", "spread", "runout", "stack", "soak" };
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
        }
        if (spec.once)
            ref.WeakenedOncePerPlayer();
        else if (spec.weak)
            ref.Weakened();
        if (allowPersist && spec.persist > Milliseconds(0))
            ref.Persistent(spec.persist, spec.tick);
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
        Registry::GetOrCreate(owner->GetMap()).BeginEncounter(owner->GetGUID()).testGms.insert(spec.gm);
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

        std::string const& s = spec.shape;
        if (s == "circle")
        {
            MechanicRef ref = Mechanic::Circle(owner, anchor, spec.size, spec.warn);
            ApplyCommon(ref, spec, true, true);
            keep(ref);
        }
        else if (s == "donut")
        {
            MechanicRef ref = Mechanic::Donut(owner, anchor, spec.inner, spec.size, spec.warn);
            ApplyCommon(ref, spec, true, true);
            keep(ref);
        }
        else if (s == "cone" || s == "rearcone")
        {
            Anchor coneAnchor = anchor;
            if (!anchorUnit)
                coneAnchor.point.SetOrientation(spec.facing);
            MechanicRef ref = Mechanic::Zone(owner, Shapes::Cone(coneAnchor, spec.angle, spec.size), spec.warn);
            if (s == "rearcone")
                ref.Rear();
            else if (spec.offset != 0.f)
                ref.FacingOffset(spec.offset);
            ApplyCommon(ref, spec, true, true);
            keep(ref);
        }
        else if (s == "line" || s == "projectile")
        {
            MechanicRef ref = Mechanic::Line(owner, anchor, spec.facing, spec.size, spec.width, spec.warn);
            if (spec.exclude > 0.f)
                ref.ExcludeSourceEnd(spec.exclude);
            if (s == "projectile")
            {
                ref.Projectile(spec.speed);
                if (spec.back)
                    ref.OutAndBack(spec.pause);
            }
            ApplyCommon(ref, spec, true, s == "line");
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
                    lanes.ExceptOne(spec.size, spec.warn, spec.axis);
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
            ApplyCommon(ref, spec, true, false);
            keep(ref);
        }
        else if (s == "runout")
        {
            Unit* target = ObjectAccessor::GetUnit(*owner, spec.target);
            if (!target)
            {
                err = "the runout target is gone";
                return ids;
            }
            MechanicRef ref = Mechanic::RunOut(owner, target, spec.size, spec.warn);
            if (spec.fromOwner)
                ref.From(MeasuredFrom::Owner);
            ApplyCommon(ref, spec, true, false);
            keep(ref);
        }
        else if (s == "stack")
        {
            MechanicRef ref = Mechanic::Stack(owner, anchor, spec.size, spec.warn);
            ApplyCommon(ref, spec, false, false);
            keep(ref);
        }
        else if (s == "soak")
        {
            MechanicRef ref = Mechanic::Soak(owner, anchor, spec.size, spec.need, spec.warn);
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

    Player* PickMember(Creature* owner, TestSpec const& spec, Barrage::Rng& rng)
    {
        std::vector<Player*> const members = GroupMembers(owner, spec.gm, spec.includeGm);
        return members.empty() ? nullptr : members[rng.Index(static_cast<uint32>(members.size()))];
    }

    constexpr char const* kNoMemberMessage = "no alive member of your group on this map (self=1 includes you)";

    // One repeat of `.mech test`: resolves `at=member` and `scatter=` now (so a delayed repeat follows the bots), then
    // registers.
    std::vector<uint32> RegisterRepeat(Creature* owner, TestSpec spec, Barrage::Rng& rng, std::string& err)
    {
        if (spec.atMember)
        {
            Player* member = PickMember(owner, spec, rng);
            if (!member)
            {
                err = kNoMemberMessage;
                return {};
            }
            spec.point = member->GetPosition();
            spec.point.SetOrientation(spec.facing);
            spec.target = member->GetGUID();
        }
        if (spec.scatter > 0.f && !spec.unitAnchor)
        {
            float const ang = rng.Float(0.f, 2.f * Geo::kPi);
            float const dist = spec.scatter * std::sqrt(rng.Float(0.f, 1.f));
            spec.point.Relocate(spec.point.GetPositionX() + dist * std::cos(ang),
                spec.point.GetPositionY() + dist * std::sin(ang), spec.point.GetPositionZ());
        }
        return RegisterTest(owner, spec, err);
    }

    // One `.mech barrage` shot: a random member, a random kind of the list, the shot's geometry around that member.
    void RegisterBarrageShot(Creature* owner, TestSpec spec, std::vector<Barrage::Kind> const& kinds,
        Barrage::Rng& rng, std::string& err)
    {
        Player* member = PickMember(owner, spec, rng);
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
            Registry::GetOrCreate(map).BeginEncounter(owner->GetGUID()).testGms.insert(gm->GetGUID());
            return owner;
        }
        TempSummon* owner = map->SummonCreature(Ids::NPC_MECHANIC_TEST_OWNER, gm->GetPosition());
        if (!owner)
            return nullptr;
        owner->SetReactState(REACT_PASSIVE);
        owner->SetLevel(gm->GetLevel(), false);     // the stub content key reads the owner's level
        if (owner->GetPhaseMask() != gm->GetPhaseMask())
            owner->SetPhaseMask(gm->GetPhaseMask(), true);
        owner->SetDisableGravity(true);
        Mechanic::BeginEncounter(owner);
        Registry::GetOrCreate(map).BeginEncounter(owner->GetGUID()).testGms.insert(gm->GetGUID());
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
        if (tokens.size() < 3)
        {
            handler->SendErrorMessage("Usage: .mech test <shape> <size> <ms> [key=value ...]; shapes: circle donut "
                "cone rearcone line projectile mine lanes spread runout stack soak");
            return false;
        }

        TestSpec spec;
        spec.shape = std::string(tokens[0]);
        Optional<float> size = Acore::StringTo<float>(tokens[1]);
        Optional<int32> ms = Acore::StringTo<int32>(tokens[2]);
        if (!IsShapeName(spec.shape) || !size || *size <= 0.f || !ms || *ms < 0)
        {
            handler->SendErrorMessage("Usage: .mech test <shape> <size> <ms> [key=value ...] (size > 0, ms >= 0)");
            return false;
        }
        spec.size = *size;
        spec.warn = Milliseconds(*ms);

        Options opt;
        if (!CollectOptions(handler, tokens, 3, opt))
            return false;

        std::string const& s = spec.shape;
        // shape keys (read only for their shape, so a stray key shows up as unused)
        if (s == "donut")
            spec.inner = opt.Float("inner", spec.size / 2.f);
        if (s == "cone" || s == "rearcone")
            spec.angle = opt.Float("angle", 90.f);
        if (s == "cone")
            spec.offset = opt.Float("offset", 0.f);
        if (s == "line" || s == "projectile")
        {
            spec.width = opt.Float("width", s == "projectile" ? 3.f : 4.f);
            spec.exclude = opt.Float("exclude", 0.f);
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
            spec.fromOwner = opt.Str("from", "group") == "owner";
        if (s == "soak")
            spec.need = static_cast<uint8>(std::clamp<int32>(opt.Int("need", 2), 1, 40));

        // common keys
        bool const getIn = s == "stack" || s == "soak";
        if (!getIn)
        {
            spec.dmg = opt.Int("dmg", -1);
            spec.pct = opt.Float("pct", 20.f);
            spec.tankPct = opt.Float("tankpct", -1.f);
            std::string const school = opt.Str("school", "physical");
            if (std::optional<uint8> sc = ParseSchool(school))
                spec.school = *sc;
            else
                opt.MarkBad("school");
        }
        if (!getIn)     // Weakened lands only from resolve hits: stack/soak never cast one (key echoed as ignored)
        {
            spec.weak = opt.Bool("weak", false);
            spec.once = opt.Bool("once", false);
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
        uint32 const repeat = static_cast<uint32>(std::clamp<int32>(opt.Int("repeat", 1), 1, kMaxRepeat));
        Milliseconds const every = opt.Ms("every", Milliseconds(1000));
        spec.scatter = std::max(0.f, opt.Float("scatter", 0.f));
        std::string const at = opt.Str("at", "");
        if (at == "member")
        {
            spec.atMember = true;
            spec.includeGm = opt.Bool("self", false);
        }
        else if (!at.empty())
            opt.MarkBad("at");
        RngPtr const rng = MakeRng(opt);

        // anchor
        Unit* selected = handler->getSelectedUnit();       // the GM when nothing is selected
        spec.facing = gm->GetOrientation();
        spec.gm = gm->GetGUID();
        spec.point = selected->GetPosition();
        spec.point.SetOrientation(spec.facing);
        spec.target = selected->GetGUID();
        std::string const anchorKind = opt.Str("anchor",
            s == "stack" && selected != gm && !spec.atMember ? "target" : "point");
        if (anchorKind == "me")
        {
            spec.unitAnchor = true;
            spec.unit = gm->GetGUID();
        }
        else if (anchorKind == "target")
        {
            if (selected == gm)
            {
                handler->SendErrorMessage("anchor=target needs a selected unit");
                return false;
            }
            spec.unitAnchor = true;
            spec.unit = selected->GetGUID();
        }
        else if (anchorKind != "point")
            opt.MarkBad("anchor");
        if (spec.atMember && anchorKind != "point")
        {
            handler->SendErrorMessage("at=member lands on a point; leave anchor= out or use anchor=point");
            return false;
        }

        std::string const bad = opt.Bad();
        if (!bad.empty())
        {
            handler->SendErrorMessage("Bad value for: {}", bad);
            return false;
        }

        Creature* owner = GetOrSummonTestOwner(gm);
        if (!owner)
        {
            handler->SendErrorMessage("Could not summon the Mechanic Test Owner ({})",
                Ids::NPC_MECHANIC_TEST_OWNER);
            return false;
        }

        if (spec.atMember && GroupMembers(owner, spec.gm, spec.includeGm).empty())
        {
            handler->SendErrorMessage("at=member: {}", kNoMemberMessage);
            return false;
        }

        MarkerAI* ai = dynamic_cast<MarkerAI*>(owner->AI());
        std::vector<uint32> made;
        std::string err;
        for (uint32 k = 0; k < repeat; ++k)
        {
            TestSpec one = spec;
            one.tag = k + 1;
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
        if (spec.atMember || spec.scatter > 0.f)
            handler->PSendSysMessage("Landing on {} (seed {}; seed= repeats the picks).",
                spec.atMember ? "random members of your group, picked as each repeat registers" : "scattered points",
                rng->Seed());
        std::string const unused = opt.Unused();
        if (!unused.empty())
            handler->PSendSysMessage("Ignored keys for {}: {}", s, unused);
        if (gm->IsGameMaster())
            handler->PSendSysMessage("Reminder: GMs are never hit; use .gm off to test on yourself.");
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
        uint32 const shots = std::min<uint32>(kMaxBarrageShots,
            static_cast<uint32>((Milliseconds(secs * 1000) + every - Milliseconds(1)) / every));

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
        RegisterBarrageShot(owner, base, kinds, *rng, err);
        if (!err.empty())
        {
            handler->SendErrorMessage("Nothing registered: {}", err);
            return false;
        }
        for (uint32 k = 1; k < shots; ++k)
            ai->ScheduleTest(every * k, [base, kinds, rng](Creature* testOwner)
            {
                std::string ignored;
                RegisterBarrageShot(testOwner, base, kinds, *rng, ignored);
            });

        std::string names;
        for (Barrage::Kind kind : kinds)
            names += std::string(names.empty() ? "" : ",") + Barrage::ShapeName(kind);
        handler->PSendSysMessage("Mechanic barrage: {} shot(s) over {} s, one every {} ms on a random group member, "
            "{} ms warning, shapes {}, {} damage each, seed {}.", shots, secs, every.count(), warn.count(), names,
            dmg, rng->Seed());
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
    // group with cap = count so none of them fizzles each other, life 120 s.
    static bool HandleMinefield(ChatHandler* handler, Tail args)
    {
        static uint32 nextGroup = kFirstMinefieldGroup;

        Player* gm = handler->GetPlayer();
        std::vector<std::string_view> const tokens = Acore::Tokenize(args, ' ', false);
        Options opt;
        if (!CollectOptions(handler, tokens, 0, opt))
            return false;

        uint32 const count = static_cast<uint32>(std::clamp<int32>(opt.Int("count", 10), 1, kMaxMines));
        float const radius = opt.Float("radius", 3.f);
        float const spread = opt.Float("spread", 15.f);
        Milliseconds const arm = opt.Ms("arm", Milliseconds(3000));
        Milliseconds const grace = opt.Ms("grace", Milliseconds(500));
        int32 const dmg = std::max<int32>(opt.Int("dmg", 1), 0);
        RngPtr const rng = MakeRng(opt);
        if (radius <= 0.f || radius > 30.f)
            opt.MarkBad("radius");
        if (spread <= 0.f || spread > 100.f)
            opt.MarkBad("spread");
        std::string const bad = opt.Bad();
        if (!bad.empty())
        {
            handler->SendErrorMessage("Bad value for: {} (radius 0-30, spread 0-100)", bad);
            return false;
        }

        Creature* owner = GetOrSummonTestOwner(gm);
        if (!owner)
        {
            handler->SendErrorMessage("Could not summon the Mechanic Test Owner ({})", Ids::NPC_MECHANIC_TEST_OWNER);
            return false;
        }

        Unit* centre = handler->getSelectedUnit();      // the GM when nothing is selected
        TestSpec spec;
        spec.shape = "mine";
        spec.size = radius;
        spec.warn = arm;
        spec.grace = grace;
        spec.cap = static_cast<uint8>(count);
        spec.capGroup = nextGroup++;
        spec.life = kMineLife;
        spec.dmg = dmg;
        spec.gm = gm->GetGUID();
        spec.facing = gm->GetOrientation();

        std::vector<uint32> made;
        std::string err;
        for (uint32 k = 0; k < count; ++k)
        {
            float const ang = rng->Float(0.f, 2.f * Geo::kPi);
            float const dist = spread * std::sqrt(rng->Float(0.f, 1.f));
            float const x = centre->GetPositionX() + dist * std::cos(ang);
            float const y = centre->GetPositionY() + dist * std::sin(ang);
            float z = centre->GetPositionZ();
            // Put the mine on the floor under that point; keep the centre's height where there is none nearby.
            float const ground = owner->GetMap()->GetHeight(owner->GetPhaseMask(), x, y, z + 2.f, true, 10.f);
            if (ground > INVALID_HEIGHT && std::fabs(ground - z) <= 5.f)
                z = ground;
            spec.point = Position(x, y, z, spec.facing);
            spec.tag = k + 1;
            std::vector<uint32> const ids = RegisterTest(owner, spec, err);
            made.insert(made.end(), ids.begin(), ids.end());
        }
        if (made.empty())
        {
            handler->SendErrorMessage("Nothing registered: {}", err);
            return false;
        }
        handler->PSendSysMessage("Mechanic minefield: {} mine(s) r={:.1f} within {:.0f} yd of {}, arm {} ms, grace {} "
            "ms, life {} s, cap group {}, {} damage, seed {}. .mech clear removes them.", made.size(), radius, spread,
            centre == gm ? "you" : centre->GetName(), arm.count(), grace.count(), kMineLife.count() / 1000,
            spec.capGroup, dmg, rng->Seed());
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
                handler->PSendSysMessage("#{} {} {} {} tag {} | resolve {:+} ms, expire {:+} ms | {} markers{}{}{}",
                    m.id, KindName(m.kind), DescribeShape(m.shape), ColorName(m.color), m.tag,
                    (m.resolveAt - now).count(), (m.expireAt - now).count(), m.markers.size(),
                    m.projectile ? " projectile" : "", m.mine ? (m.mine->armed ? " mine(armed)" : " mine") : "",
                    m.persist ? " persistent" : "");
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
