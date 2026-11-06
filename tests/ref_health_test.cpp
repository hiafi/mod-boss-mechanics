// Standalone tests for src/MechanicRefHealth.h (dungeon difficulties STAGE-E0-E3-HANDOFF §3.7, decision §7 #5,
// WP-3P): parsing the keyed reference health rows (the level-only stub format included), lookup and fallback per
// tier, the canonical text, map level pins and the default content key. Build: tests/run.sh.

#include "check.h"
#include "MechanicRefHealth.h"
#include <string>

using namespace BossMechanics;
using namespace BossMechanics::RefHealth;

namespace
{
    ContentKey Key(std::uint8_t level, bool heroic = false, std::uint8_t empowered = 0)
    {
        ContentKey k;
        k.level = level;
        k.heroic = heroic;
        k.empoweredLevel = empowered;
        return k;
    }

    bool Parses(std::string const& text)
    {
        Table t;
        std::string err;
        return ParseTable(text, t, err);
    }

    void TestStubFormat()
    {
        // The M1 stub string is a table of normal rows, and looks up exactly as InterpolateReferenceHealth did.
        Table t;
        std::string err;
        CHECK(ParseTable("20:1000:1500 60:5000:8000", t, err));
        CHECK(err.empty());
        CHECK(t.normal.size() == 2);
        CHECK(t.heroic.empty());
        CHECK(t.empowered.empty());
        std::vector<ReferenceHealthRow> old;
        CHECK(ParseReferenceHealthRows("20:1000:1500 60:5000:8000", old));
        for (std::uint8_t level : { 1, 10, 20, 21, 40, 59, 60, 70, 80, 255 })
        {
            ReferenceHealth const a = Lookup(t, Key(level)).health;
            ReferenceHealth const b = InterpolateReferenceHealth(old, level);
            CHECK(a.nonTank == b.nonTank);
            CHECK(a.tank == b.tank);
        }
        // Heroic and empowered keys fall back to the normal rows when the config has only those.
        Resolved const r = Lookup(t, Key(60, true, 2));
        CHECK(r.tier == Tier::Normal);
        CHECK(r.health.nonTank == 5000);
        CHECK(r.health.tank == 8000);
    }

    void TestKeyedParse()
    {
        Table t;
        std::string err;
        CHECK(ParseTable("60e2:9000:14000, 60h:6000:9600;20:1000:1500\t60:5000:8000 60E1:7000:11000 70H:6500:10000",
            t, err));
        CHECK(t.normal.size() == 2);
        CHECK(t.heroic.size() == 2);
        CHECK(t.empowered.size() == 2);
        CHECK(t.empowered.count(1) == 1 && t.empowered.count(2) == 1);
        CHECK(t.normal.front().level == 20);       // sorted by level
        CHECK(t.heroic.front().level == 60);
        CHECK(t.heroic.front().health.nonTank == 6000);
        CHECK(t.empowered[1].front().health.tank == 11000);
        // Canonical text: normal, heroic, empowered by level; it parses back to the same table.
        std::string const text = Format(t);
        CHECK(text == "20:1000:1500 60:5000:8000 60h:6000:9600 70h:6500:10000 60e1:7000:11000 60e2:9000:14000");
        Table back;
        CHECK(ParseTable(text, back, err));
        CHECK(Format(back) == text);
        // Blank text, separators only, and the placeholder default.
        CHECK(!Parses(""));
        CHECK(!Parses(" ,; "));
        CHECK(Parses("20:1000:1500 60:5000:8000 60h:6000:9600"));

        // The string mod-dpssim tools/refhealth.py prints (its fixture run, STAGE-E3-3G-REPORT) parses as is.
        std::string const printed = "20:1050:1540 60:3055:3550 60h:4055:4550 60e1:5055:5550 60e2:6055:6550 "
            "60e3:7055:7550";
        CHECK(ParseTable(printed, t, err));
        CHECK(Format(t) == printed);
        CHECK(Lookup(t, Key(60, true, 3)).health.nonTank == 7055);
        CHECK(Lookup(t, Key(60, true)).health.tank == 4550);
        CHECK(Lookup(t, Key(20)).health.nonTank == 1050);
    }

    void TestBadEntries()
    {
        Table t;
        std::string err;
        CHECK(!ParseTable("20:1000:1500 60x:5000:8000", t, err));
        CHECK(err.find("60x:5000:8000") != std::string::npos);
        CHECK(t.normal.empty());                    // left empty on failure
        CHECK(!Parses("60h:6000:9600"));            // no normal row
        CHECK(!Parses("60e1:7000:11000"));
        CHECK(!Parses("20:1000"));                  // missing tank
        CHECK(!Parses("20:1000:1500:7"));           // extra field
        CHECK(!Parses("20::1500"));
        CHECK(!Parses(":1000:1500"));
        CHECK(!Parses("h:1000:1500"));              // no level
        CHECK(!Parses("20:-1000:1500"));            // no signs
        CHECK(!Parses("20:+1000:1500"));
        CHECK(!Parses("20:0:1500"));                // health > 0
        CHECK(!Parses("20:1000:0"));
        CHECK(!Parses("20:1000:3000000000"));       // above int32
        CHECK(!Parses("256:1000:1500"));            // a content level is a byte
        CHECK(!Parses("60e0:1000:1500"));           // empowered 1-255
        CHECK(!Parses("60e256:1000:1500"));
        CHECK(!Parses("60e:1000:1500"));
        CHECK(!Parses("60he1:1000:1500"));
        CHECK(!Parses("20:1000:1500 20:1100:1600"));    // duplicate within a tier
        CHECK(!ParseTable("20:1000:1500 60h:1:1 60H:2:2", t, err));
        CHECK(err.find("duplicate") != std::string::npos);
        CHECK(Parses("60:1000:1500 60h:1100:1600 60e1:1200:1700"));     // one level in three tiers is fine
        CHECK(Parses("0:1:1"));
    }

    void TestLookup()
    {
        Table t;
        std::string err;
        CHECK(ParseTable("20:1000:1500 60:5000:8000 60h:6000:9600 60e1:7000:11000 60e3:12000:18000", t, err));

        // Normal: interpolated by level, clamped.
        Resolved r = Lookup(t, Key(20));
        CHECK(r.tier == Tier::Normal && r.health.nonTank == 1000 && r.health.tank == 1500);
        r = Lookup(t, Key(40));
        CHECK(r.health.nonTank == 3000 && r.health.tank == 4750);
        r = Lookup(t, Key(60));
        CHECK(r.health.nonTank == 5000 && r.health.tank == 8000);
        r = Lookup(t, Key(80));
        CHECK(r.health.nonTank == 5000);

        // Heroic: the heroic rows only, clamped (a heroic at any level reads 60h until more heroic rows exist).
        r = Lookup(t, Key(60, true));
        CHECK(r.tier == Tier::Heroic && r.health.nonTank == 6000 && r.health.tank == 9600);
        r = Lookup(t, Key(20, true));             // E1: heroic Deadmines still on its normal (level 20) templates
        CHECK(r.tier == Tier::Heroic && r.health.nonTank == 6000);

        // Empowered: its own rows; a missing level uses the heroic rows; heroic is implied.
        r = Lookup(t, Key(60, true, 1));
        CHECK(r.tier == Tier::Empowered && r.empoweredLevel == 1 && r.health.nonTank == 7000);
        r = Lookup(t, Key(60, false, 3));
        CHECK(r.tier == Tier::Empowered && r.empoweredLevel == 3 && r.health.tank == 18000);
        r = Lookup(t, Key(60, true, 2));
        CHECK(r.tier == Tier::Heroic && r.empoweredLevel == 0 && r.health.nonTank == 6000);
        r = Lookup(t, Key(60, false, 2));
        CHECK(r.tier == Tier::Heroic);

        // Without heroic rows an empowered level with no rows goes to normal.
        Table n;
        CHECK(ParseTable("20:1000:1500 60:5000:8000 60e1:7000:11000", n, err));
        r = Lookup(n, Key(60, true, 2));
        CHECK(r.tier == Tier::Normal && r.health.nonTank == 5000);
        r = Lookup(n, Key(60, true, 1));
        CHECK(r.tier == Tier::Empowered && r.health.nonTank == 7000);

        // Several heroic rows interpolate among themselves only.
        Table h;
        CHECK(ParseTable("20:1000:1500 60:5000:8000 60h:6000:9600 70h:8000:12000", h, err));
        r = Lookup(h, Key(65, true));
        CHECK(r.health.nonTank == 7000 && r.health.tank == 10800);
        r = Lookup(h, Key(65));
        CHECK(r.health.nonTank == 5000);

        // The flat damage of a 20% non-tank mechanic per row (the E3 acceptance check's arithmetic).
        DamageSpec d;
        d.pctNonTank = 20.f;
        CHECK(FlatDamageFor(d, Lookup(t, Key(20)).health, false) == 200);
        CHECK(FlatDamageFor(d, Lookup(t, Key(60, true)).health, false) == 1200);
        CHECK(FlatDamageFor(d, Lookup(t, Key(60)).health, false) == 1000);
    }

    void TestDescribe()
    {
        CHECK(Describe(Key(20)) == "level 20 normal");
        CHECK(Describe(Key(60, true)) == "level 60 heroic");
        CHECK(Describe(Key(60, true, 2)) == "level 60 empowered 2");
        CHECK(std::string(TierName(Tier::Normal)) == "normal");
        CHECK(std::string(TierName(Tier::Heroic)) == "heroic");
        CHECK(std::string(TierName(Tier::Empowered)) == "empowered");
    }

    void TestMapLevels()
    {
        MapLevels pins;
        std::string err;
        CHECK(ParseMapLevels("", pins, err));
        CHECK(pins.empty());
        CHECK(ParseMapLevels("36:20 36h:60, 289:60", pins, err));
        CHECK(pins.size() == 3);
        CHECK(pins.at({ 36, false }) == 20);
        CHECK(pins.at({ 36, true }) == 60);
        CHECK(pins.at({ 289, false }) == 60);
        CHECK(ParseMapLevels("36H:60", pins, err));
        CHECK(pins.at({ 36, true }) == 60);

        CHECK(!ParseMapLevels("36:20 36:21", pins, err));
        CHECK(err.find("duplicate") != std::string::npos);
        CHECK(pins.empty());
        CHECK(!ParseMapLevels("36", pins, err));
        CHECK(!ParseMapLevels("36:0", pins, err));
        CHECK(!ParseMapLevels("36:256", pins, err));
        CHECK(!ParseMapLevels("h:20", pins, err));
        CHECK(!ParseMapLevels("36x:20", pins, err));
        CHECK(!ParseMapLevels("36:20:1", pins, err));
        CHECK(!ParseMapLevels("-36:20", pins, err));
    }

    void TestContentKey()
    {
        MapLevels pins;
        std::string err;
        CHECK(ParseMapLevels("36:20", pins, err));

        // Normal Deadmines: pinned to 20 whatever the owner (a level 60 GM's test owner, a level 21 boss).
        ContentInputs in;
        in.hasMap = true;
        in.mapId = 36;
        in.ownerLevel = 60;
        ContentKey k = ResolveContentKey(in, pins);
        CHECK(k.level == 20 && !k.heroic && k.empoweredLevel == 0);
        in.ownerLevel = 21;
        CHECK(ResolveContentKey(in, pins).level == 20);

        // Heroic Deadmines: not pinned (no "36h"), so the owner's level (60 on the heroic templates).
        in.heroic = true;
        in.ownerLevel = 60;
        k = ResolveContentKey(in, pins);
        CHECK(k.level == 60 && k.heroic);

        // A level 60 normal map without a pin (Scholomance): the owner's level.
        in.mapId = 289;
        in.heroic = false;
        k = ResolveContentKey(in, pins);
        CHECK(k.level == 60 && !k.heroic);

        // No owner: level 0 (the lowest row). Empowered passes through.
        in.ownerLevel = 0;
        in.empoweredLevel = 2;
        k = ResolveContentKey(in, pins);
        CHECK(k.level == 0 && k.empoweredLevel == 2);

        // No map: never heroic, never pinned.
        ContentInputs none;
        none.heroic = true;
        none.mapId = 36;
        none.ownerLevel = 45;
        k = ResolveContentKey(none, pins);
        CHECK(!k.heroic && k.level == 45);

        // The acceptance chain end to end with the placeholder rows.
        Table t;
        CHECK(ParseTable("20:1000:1500 60:5000:8000 60h:6000:9600", t, err));
        ContentInputs dm;
        dm.hasMap = true;
        dm.mapId = 36;
        dm.ownerLevel = 60;
        CHECK(Lookup(t, ResolveContentKey(dm, pins)).health.nonTank == 1000);
        dm.heroic = true;
        CHECK(Lookup(t, ResolveContentKey(dm, pins)).health.nonTank == 6000);
        dm.mapId = 289;
        dm.heroic = false;
        CHECK(Lookup(t, ResolveContentKey(dm, pins)).health.nonTank == 5000);
    }
    // The MapLevels pin is per difficulty (WP-H, dungeon difficulties E2): "36:20" pins normal Deadmines only and heroic
    // reads the owner's level; the default "36:20 36h:60" pins heroic at 60 too.
    void TestMapLevelsPerDifficulty()
    {
        std::string err;
        Table t;
        CHECK(ParseTable("20:1300:1290 60:3670:3710 60h:4120:4110", t, err));   // kDefaultReferenceHealthRows
        MapLevels pins;
        CHECK(ParseMapLevels("36:20 36h:60", pins, err));                        // kDefaultReferenceHealthMapLevels
        CHECK(pins.size() == 2 && pins.at({ 36, false }) == 20 && pins.at({ 36, true }) == 60);

        ContentInputs dm;
        dm.hasMap = true;
        dm.mapId = 36;
        // Heroic: a stock level 20 boss (before the E2 templates), a level 60 heroic template, an 80 GM's test owner.
        dm.heroic = true;
        for (std::uint8_t owner : { std::uint8_t(20), std::uint8_t(60), std::uint8_t(80) })
        {
            dm.ownerLevel = owner;
            ContentKey const k = ResolveContentKey(dm, pins);
            CHECK(k.level == 60 && k.heroic);
            CHECK(Lookup(t, k).health.nonTank == 4120);
            CHECK(Describe(k) == "level 60 heroic");
        }
        // Normal: 20 whatever the owner.
        dm.heroic = false;
        for (std::uint8_t owner : { std::uint8_t(19), std::uint8_t(60), std::uint8_t(80) })
        {
            dm.ownerLevel = owner;
            ContentKey const k = ResolveContentKey(dm, pins);
            CHECK(k.level == 20 && !k.heroic);
            CHECK(Lookup(t, k).health.nonTank == 1300);
        }

        // The old default "36:20" never pinned heroic: heroic Deadmines reads the heroic rows at the owner's level,
        // which with the single 60h row clamps to 60h (4120), not the normal 20 row.
        MapLevels normalOnly;
        CHECK(ParseMapLevels("36:20", normalOnly, err));
        dm.heroic = true;
        dm.ownerLevel = 20;
        ContentKey k = ResolveContentKey(dm, normalOnly);
        CHECK(k.level == 20 && k.heroic);
        CHECK(Lookup(t, k).health.nonTank == 4120);
        // Where the explicit heroic pin matters: with a heroic row below 60 the unpinned owner level would pick it.
        Table more;
        CHECK(ParseTable("20:1300:1290 60:3670:3710 20h:1500:1500 60h:4120:4110", more, err));
        CHECK(Lookup(more, ResolveContentKey(dm, normalOnly)).health.nonTank == 1500);
        CHECK(Lookup(more, ResolveContentKey(dm, pins)).health.nonTank == 4120);
        dm.ownerLevel = 80;
        CHECK(Lookup(more, ResolveContentKey(dm, pins)).health.nonTank == 4120);
    }
}

int main()
{
    TestStubFormat();
    TestKeyedParse();
    TestBadEntries();
    TestLookup();
    TestDescribe();
    TestMapLevels();
    TestContentKey();
    TestMapLevelsPerDifficulty();
    return Report("ref_health_test");
}
