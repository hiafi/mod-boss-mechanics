// Host test: every step of every drill program (Drill::All(): Zul'Farrak, Gnomeregan and all the others) goes through
// the live parser, so a step the engine refuses fails here instead of at `.mech drill` or the sweep (Gnomeregan build
// F-1f; R-1-FW m2: `zul-farrak-shapes` shipped refused live because no host test ran ParseTest).
//
// The parser is NOT copied: tests/run.sh extracts ParseTest and the blocks it needs (Options, TestSpec, the key
// parsers, IsShapeName, ParseCtx) from src/MechanicCommand.cpp into parse_extract.inc (tests/extract_parse.awk), and
// this file compiles them against the small stand-ins below for the server types they touch (Unit, Position,
// ObjectGuid, the registry's Color / Role / Eligibility / Ordering / BaitRule, sSpellMgr, Acore's StringTo,
// StringFormat and Tokenize). A new server call in those blocks fails this build: add its stand-in here.
//
// The step dispatch mirrors RunDrillWith (src/MechanicCommand.cpp, "Parse every step now"): plan keys, `marker`, the
// drill-only steps through their Drill::Parse* functions, everything else through ParseTest plus its unused-key check,
// in both damage modes (percent, and flat with ` dmg=<n>` appended to a damage step). Spell and creature rows are the
// database's: every id passes here. Build: tests/run.sh.

#include "BossMechanicsIds.h"
#include "MechanicDrill.h"
#include "MechanicDropSpot.h"
#include "MechanicFront.h"
#include "MechanicGaze.h"
#include "MechanicGeometry.h"
#include "MechanicHide.h"
#include "MechanicJudge.h"
#include "MechanicMath.h"
#include "check.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ---- stand-ins for the server types the extracted blocks use -------------------------------------------------------
using uint8 = std::uint8_t;
using uint32 = std::uint32_t;
using int32 = std::int32_t;
using Milliseconds = std::chrono::milliseconds;
template <typename T>
using Optional = std::optional<T>;

struct ObjectGuid
{
    std::uint64_t raw = 0;
    static ObjectGuid const Empty;
    bool operator==(ObjectGuid const& o) const { return raw == o.raw; }
    bool IsEmpty() const { return raw == 0; }
};
ObjectGuid const ObjectGuid::Empty{};

template <>
struct std::hash<ObjectGuid>
{
    std::size_t operator()(ObjectGuid const& g) const noexcept { return std::hash<std::uint64_t>()(g.raw); }
};

struct Position
{
    Position() = default;
    Position(float x, float y, float z, float o = 0.f) : _x(x), _y(y), _z(z), _o(o) { }
    float GetPositionX() const { return _x; }
    float GetPositionY() const { return _y; }
    float GetPositionZ() const { return _z; }
    float GetOrientation() const { return _o; }
    void SetOrientation(float o) { _o = o; }
    void Relocate(float x, float y, float z) { _x = x; _y = y; _z = z; }

private:
    float _x = 0.f;
    float _y = 0.f;
    float _z = 0.f;
    float _o = 0.f;
};

struct Unit
{
    ObjectGuid guid;
    Position pos;
    bool creature = false;
    ObjectGuid GetGUID() const { return guid; }
    Position GetPosition() const { return pos; }
    float GetPositionX() const { return pos.GetPositionX(); }
    float GetPositionY() const { return pos.GetPositionY(); }
    float GetPositionZ() const { return pos.GetPositionZ(); }
    float GetOrientation() const { return pos.GetOrientation(); }
    bool IsCreature() const { return creature; }
};

struct SpellInfo
{
};

struct SpellMgrStandIn
{
    // Spell rows are the database's: the host test accepts every id (the live runner checks the row).
    SpellInfo const* GetSpellInfo(uint32 /*id*/) const
    {
        static SpellInfo const info;
        return &info;
    }
};
SpellMgrStandIn* const sSpellMgr = new SpellMgrStandIn();

namespace Acore
{
    template <typename T>
    Optional<T> StringTo(std::string_view s)
    {
        if constexpr (std::is_floating_point_v<T>)
        {
            // StringConvert.h (non-MSVC): std::stold over the whole string.
            if (s.empty() || (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')))
                return std::nullopt;
            try
            {
                std::string const tmp(s);
                std::size_t n = 0;
                T const v = static_cast<T>(std::stold(tmp, &n));
                if (n != tmp.size())
                    return std::nullopt;
                return v;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }
        else
        {
            T v{};
            std::from_chars_result const r = std::from_chars(s.data(), s.data() + s.size(), v, 10);
            if (r.ec != std::errc() || r.ptr != s.data() + s.size())
                return std::nullopt;
            return v;
        }
    }

    template <typename... Args>
    std::string StringFormat(std::format_string<Args...> fmt, Args&&... args)
    {
        return std::format(fmt, std::forward<Args>(args)...);
    }

    // Tokenize.cpp, as is.
    inline std::vector<std::string_view> Tokenize(std::string_view str, char sep, bool keepEmpty)
    {
        std::vector<std::string_view> tokens;
        std::size_t start = 0;
        for (std::size_t end = str.find(sep); end != std::string_view::npos; end = str.find(sep, start))
        {
            if (keepEmpty || start < end)
                tokens.push_back(str.substr(start, end - start));
            start = end + 1;
        }
        if (keepEmpty || start < str.length())
            tokens.push_back(str.substr(start));
        return tokens;
    }
}

namespace BossMechanics
{
    // MechanicRegistry.h, the fields and helpers the parser reads (keep in step with it).
    enum class Color : uint8
    {
        Red,
        Yellow,
        Blue,
        Purple,
        Orange,
        Green,
        Teal,
        White,
        Black
    };
    static_assert(static_cast<uint8>(Color::Black) + 1 == Ids::COLOR_COUNT, "Color must match the marker palette");

    // MechanicRegistry.cpp ColorName.
    inline char const* ColorName(Color color)
    {
        static char const* const names[] = { "red", "yellow", "blue", "purple", "orange", "green", "teal", "white",
            "black" };
        return names[static_cast<uint8>(color)];
    }

    enum class Role : uint8
    {
        None = 0,
        Tank = 1,
        Healer = 2,
        Damage = 4,
        Any = 7
    };

    constexpr Role operator|(Role a, Role b)
    {
        return static_cast<Role>(static_cast<uint8>(a) | static_cast<uint8>(b));
    }

    using GuidSet = std::unordered_set<ObjectGuid>;

    struct Eligibility
    {
        Role roles = Role::Any;
        GuidSet carriers;
        uint32 auraSpell = 0;
        uint8 minStacks = 0;
        uint8 maxStacks = 255;
        bool playersOnly = false;
    };

    struct Ordering
    {
        uint32 auraSpell = 0;
        bool ascending = true;
        uint8 capStacks = 0;
        Role roleLast = Role::None;
        uint8 minStacks = 0;
    };

    struct BaitRule
    {
        enum class Type : uint8
        {
            FewestStacks,
            Farthest
        };

        Type type = Type::Farthest;
        uint32 auraSpell = 0;

        static BaitRule FewestStacks(uint32 spell) { return { Type::FewestStacks, spell }; }
        static BaitRule Farthest() { return { Type::Farthest, 0 }; }
    };
}

using namespace BossMechanics;

namespace
{
    // MechanicCommand.cpp's file constants the extracted blocks read.
    constexpr uint32 kMaxRepeat = 50;
    constexpr Milliseconds kDefaultBurn = Milliseconds(6000);
    constexpr char const* kTestUsage = "Usage: .mech test <shape> <size> <ms> [key=value ...]";

#include "parse_extract.inc"

    // RunDrillWith's step dispatch, up to ParseTest (see the file comment). Empty: the runner would take the step.
    std::string ParseStep(Drill::Def const& def, Drill::Step const& step, bool percent, Unit* gm, Unit* boss)
    {
        if (!Drill::PlanKeysOk(step.line))
            return "bad after= or cycle=";
        std::string line = Drill::StripPlanKeys(step.line);
        std::vector<std::string_view> const head = Acore::Tokenize(line, ' ', false);
        if (head.empty())
            return "empty step";
        std::string_view const kind = head[0];
        if (kind == "marker")
        {
            Optional<uint32> n = head.size() >= 2 ? Acore::StringTo<uint32>(head[1]) : std::nullopt;
            if (!n || *n > Ids::MARKED_DRILL_MAX_STACKS)
                return "bad marker count";
            std::optional<std::string_view> const pick = Drill::Value(line, "pick");
            if (pick && *pick != "nontank" && *pick != "any" && *pick != "healer" && *pick != "unmarked")
                return "bad marker pick=";
            std::optional<std::string_view> const one = Drill::Value(line, "one");
            if (one && (*one != "1" || *n))
                return "one=1 is for marker 0";
            return {};
        }
        if (kind == "blink")
            return Drill::ParseBlink(line) ? "" : "bad blink";
        if (kind == "cast")
            return Drill::ParseCast(line) ? "" : "bad cast";
        if (kind == "afflict")
            return Drill::ParseAfflict(line) ? "" : "bad afflict";
        if (kind == "partner")
            return Drill::ParsePartner(line) ? "" : "bad partner";
        if (kind == "stalk")
            return Drill::ParseStalk(line) ? "" : "bad stalk";
        if (kind == "floes")
            return Drill::ParseFloes(line) ? "" : "bad floes";
        if (kind == "lights")
            return Drill::ParseLights(line) ? "" : "bad lights";
        if (kind == "movecost")
            return Drill::ParseMoveCost(line) ? "" : "bad movecost";
        if (kind == "saturated")
            return Drill::ParseSaturated(line) ? "" : "bad saturated";
        if (kind == "holdfacing")
            return Drill::ParseHoldFacing(line) ? "" : "bad holdfacing";
        if (kind == "walk")
            return Drill::ParseWalk(line) ? "" : "bad walk";
        if (kind == "leash")
            return Drill::ParseLeash(line) ? "" : "bad leash";
        if (kind == "chase")
            return Drill::ParseChase(line) ? "" : "bad chase";
        if (Drill::DamageStep(line) && !percent)
            line += " dmg=500";
        std::vector<std::string_view> const tokens = Acore::Tokenize(line, ' ', false);
        Options opt;
        TestSpec spec;
        uint32 repeat = 1;
        Milliseconds every(0);
        ParseCtx const ctx{ gm, gm->GetGUID(), gm->GetOrientation() };
        std::string err = ParseTest(ctx, boss, tokens, opt, spec, repeat, every);
        if (err.empty() && !opt.Unused().empty())
            err = "unused keys " + opt.Unused();
        (void)def;
        return err;
    }

    std::size_t ParseAll(Drill::Def const& def, bool percent, Unit* gm, Unit* boss, bool print)
    {
        std::size_t bad = 0;
        for (Drill::Step const& step : def.steps)
        {
            std::string const err = ParseStep(def, step, percent, gm, boss);
            if (err.empty())
                continue;
            ++bad;
            if (print)
                std::printf("  drill %s (%s): step '%s': %s\n", def.name, percent ? "percent" : "flat",
                    step.line, err.c_str());
        }
        return bad;
    }
}

int main()
{
    Unit gm{ ObjectGuid{ 1 }, Position(0.f, 0.f, 0.f, 0.f), false };
    Unit boss{ ObjectGuid{ 2 }, Position(10.f, 0.f, 0.f, 3.14159f), true };

    // Every program, every step, both damage modes.
    std::size_t programs = 0;
    std::size_t steps = 0;
    for (Drill::Def const& def : Drill::All())
    {
        ++programs;
        steps += def.steps.size();
        CHECK(ParseAll(def, true, &gm, &boss, true) == 0);
        CHECK(ParseAll(def, false, &gm, &boss, true) == 0);
    }
    CHECK(programs >= 20);
    CHECK(Drill::Find("zul-farrak-shapes") && Drill::Find("gnomeregan-shapes"));

    // The refusals still bite: the harness is not a rubber stamp.
    Drill::Step const bad[] = {
        { 0, "line 30 1500 width=4 fix=1" },                                // fix=1 needs to=
        { 0, "line 30 1500 width=4 anchor=target to=member fix=1 follow=1" },   // a frozen line cannot follow
        { 0, "circle 5 1500 nosuchkey=1" },                                 // unused key
        { 0, "projectile 30 2000 width=4 to=target fix=1 anchor=target follow=1" },
        { 0, "marker 11" },                                                 // above Marked's cap
    };
    Drill::Def const probe{ "probe", "", 0, false, {}, 1 };
    for (Drill::Step const& step : bad)
        CHECK(!ParseStep(probe, step, true, &gm, &boss).empty());
    // F-1f: a frozen projectile (Zul'Farrak's swarm, row 1) is a point source along a fixed facing, like a fixed line.
    CHECK(ParseStep(probe, { 0, "projectile 30 2000 width=4 speed=5 shift=15:26 to=target fix=1 pct=45 tag=1" }, true,
        &gm, &boss).empty());
    CHECK(ParseStep(probe, { 0, "projectile 30 2000 width=4 speed=5 anchor=target to=member reach=1 pct=45 tag=1" },
        true, &gm, &boss).empty());

    std::printf("drill_parse_test: %zu programs, %zu steps\n", programs, steps);
    return Report("drill_parse_test");
}
