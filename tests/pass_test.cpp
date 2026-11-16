// Standalone tests for src/MechanicPass.h, the descriptor's PassEntry and the `pass` drill (Blackrock Spire build,
// WP-PS; shared doc BS6). Build: tests/run.sh.
//
// Covers the pick (eligibility, range, sight, the preference, FarFrom against the zones, stickiness), the throw
// triggers and minimum holds, the per-map slot (catches, failed throws, exclusion), the ready spots on the Rookery's
// real egg clutches, Solakar's heat table as a catcher-supply model at 5 and 10 players, the drill's rows, and the
// solver cost with the Rookery's 33 clutches, 40 zones and one zone per egg (138) against M5's 2,000 us.

#include "check.h"
#include "MechanicDescriptorPresets.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include "MechanicPass.h"
#include "MechanicRoute.h"
#include "MechanicSolver.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace BossMechanics;
using Geo::Vec3;
using Ms = std::chrono::milliseconds;

namespace
{
    // The Rookery's egg clutches (live gameobject 175124 on map 229 inside the room box x 48.5..107.5, y -338..-252.5;
    // eggs within 3 yd chained, 2026-11-15): centre, radius (the farthest egg + 1.5 yd) and egg count. 138 eggs, 33
    // clutches (the handoff's §3.8 count).
    struct Clutch
    {
        float x, y, z, r;
        int eggs;
    };

    constexpr Clutch kRookery[] = {
        { 48.8f, -337.1f, 91.5f, 1.5f, 1 }, { 51.2f, -316.1f, 91.5f, 2.7f, 3 }, { 54.6f, -336.7f, 91.5f, 1.5f, 1 },
        { 57.0f, -254.9f, 96.9f, 4.2f, 7 }, { 58.4f, -334.4f, 91.5f, 4.6f, 5 }, { 60.3f, -270.4f, 93.6f, 3.8f, 4 },
        { 61.6f, -315.6f, 91.5f, 5.1f, 6 }, { 63.0f, -335.5f, 91.5f, 2.8f, 2 }, { 64.8f, -268.2f, 93.2f, 2.1f, 2 },
        { 68.3f, -314.9f, 91.4f, 1.5f, 1 }, { 68.7f, -307.1f, 91.5f, 1.5f, 1 }, { 69.6f, -304.7f, 91.4f, 7.0f, 9 },
        { 71.2f, -329.0f, 91.5f, 6.8f, 8 }, { 73.0f, -286.6f, 91.7f, 2.7f, 2 }, { 74.3f, -276.2f, 91.9f, 4.7f, 6 },
        { 76.8f, -292.7f, 91.4f, 4.5f, 5 }, { 77.7f, -262.7f, 91.5f, 3.0f, 3 }, { 79.8f, -265.8f, 91.5f, 2.9f, 2 },
        { 80.8f, -253.6f, 91.5f, 1.5f, 1 }, { 81.4f, -329.1f, 91.5f, 5.8f, 11 }, { 82.0f, -336.0f, 91.5f, 1.5f, 1 },
        { 83.4f, -268.2f, 91.5f, 2.4f, 2 }, { 85.7f, -294.4f, 91.4f, 2.5f, 2 }, { 85.7f, -316.4f, 91.4f, 7.5f, 9 },
        { 87.2f, -253.4f, 91.5f, 2.5f, 2 }, { 87.7f, -279.1f, 91.5f, 2.5f, 2 }, { 89.5f, -302.8f, 91.4f, 6.6f, 8 },
        { 90.2f, -333.6f, 91.5f, 3.8f, 3 }, { 93.2f, -289.2f, 91.4f, 5.7f, 7 }, { 95.0f, -280.9f, 91.4f, 4.6f, 4 },
        { 98.5f, -312.5f, 91.4f, 4.7f, 6 }, { 104.5f, -301.5f, 91.5f, 5.1f, 5 }, { 105.9f, -285.6f, 91.5f, 5.7f, 7 },
    };
    constexpr Vec3 kSolakarSpawn{ 80.f, -280.f, 93.f };
    constexpr Vec3 kFatherFlame{ 76.f, -334.7f, 91.5f };

    std::vector<Pass::Disc> RookeryDiscs()
    {
        std::vector<Pass::Disc> out;
        for (Clutch const& c : kRookery)
            out.push_back({ { c.x, c.y, c.z }, c.r });
        return out;
    }

    // 40 zones: the 33 clutches, the seven largest also split off one egg 1.5 yd out (a script that caps its zones at
    // about 40, SP's count).
    std::vector<Pass::Disc> Rookery40()
    {
        std::vector<Pass::Disc> out = RookeryDiscs();
        std::vector<Clutch> big(std::begin(kRookery), std::end(kRookery));
        std::sort(big.begin(), big.end(), [](Clutch const& a, Clutch const& b) { return a.eggs > b.eggs; });
        for (std::size_t i = 0; i < 7; ++i)
            out.push_back({ { big[i].x + big[i].r, big[i].y, big[i].z }, 1.5f });
        return out;
    }

    // One zone per egg (138): the clutch's eggs spread on a ring inside it (the live spots are not in the test).
    std::vector<Pass::Disc> RookeryEggs()
    {
        std::vector<Pass::Disc> out;
        for (Clutch const& c : kRookery)
            for (int i = 0; i < c.eggs; ++i)
            {
                float const a = Geo::kTwoPi * static_cast<float>(i) / static_cast<float>(c.eggs);
                float const r = c.eggs == 1 ? 0.f : c.r - 1.5f;
                out.push_back({ { c.x + r * std::cos(a), c.y + r * std::sin(a), c.z }, 1.5f });
            }
        return out;
    }

    // Deterministic LCG (no <random>, so runs match across hosts).
    struct Lcg
    {
        std::uint64_t state = 0x5eedull;

        float Next01()
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<float>(state >> 40) / 16777216.f;
        }

        float Range(float lo, float hi)
        {
            return lo + (hi - lo) * Next01();
        }
    };

    struct StraightOracle : Solver::PathOracle
    {
        bool Find(Vec3 const& from, Vec3 const& to, std::vector<Vec3>& out) override
        {
            out = { from, to };
            return true;
        }
    };

    Pass::Candidate Cand(Desc::Key guid, Vec3 pos, float holderYd, bool eligible = true, bool preferred = false)
    {
        Pass::Candidate c;
        c.guid = guid;
        c.pos = pos;
        c.holderYd = holderYd;
        c.eligible = eligible;
        c.preferred = preferred;
        return c;
    }

    // ---- conditions and references ---------------------------------------------------------------------------------

    void TestAnyOfAndReferences()
    {
        constexpr std::uint32_t kScorched = 1, kWound = 2;
        Condition const c = Condition::AnyOf({ Condition::LacksAura(kScorched), Condition::HasAura(kWound) });
        Desc::Context ctx;
        Desc::UnitFacts u;
        CHECK(Desc::Holds(c, u, ctx));                  // no aura: not scorched
        u.auras = { kScorched };
        CHECK(!Desc::Holds(c, u, ctx));                 // scorched, not wounded
        u.auras = { kScorched, kWound };
        CHECK(Desc::Holds(c, u, ctx));                  // scorched but wounded: catches
        CHECK(!Desc::Holds(Condition::AnyOf({}), u, ctx));

        Descriptor d;
        d.passes.push_back(PassEntry::WithItem(10, 13371, 30.f).CatchWhen(c).Prefer(Condition::HasAura(kWound))
            .When(PassTrigger::UnitNear(5, 15.f, Condition::HasAura(77))).MinHold(Ms(2000), Condition::HasAura(88)));
        CHECK(!d.Empty());
        Desc::References const r = Desc::Referenced(d);
        for (std::uint32_t aura : { 10u, kScorched, kWound, 77u, 88u })
            CHECK(Desc::Contains(r.auras, aura));
        // Every builder fills its field.
        PassEntry const e = PassEntry::MovingAura(1, 25.f, 2, Ms(8000)).FarFrom(0).FarFrom(9).KeepFrom(12.f)
            .FewestZones(60, 4.f, 8.f).Flight(Ms(700)).ReadyCatchers(2).NoSight().Room(0.f, 0.f, 10.f, 10.f)
            .RoomCircle(5.f, 5.f, 3.f).ActiveWhen(Condition::Phase(2));
        CHECK(e.how == PassEntry::Throw::MoveAura && e.afterAura == 2 && e.afterMs == Ms(8000) && e.range == 25.f);
        CHECK(e.farFrom.size() == 2 && e.farFrom[0] == 0 && e.keepFromYd == 12.f && e.zoneTag == 60
            && e.zoneWeightYd == 8.f && e.flight == Ms(700) && e.readyCatchers == 2 && !e.sight && e.roomBox
            && e.roomR == 3.f && e.active.kind == CondKind::Phase);
        CHECK(PassEntry::WithSpell(1, 99).how == PassEntry::Throw::Spell);
    }

    // ---- geometry helpers ------------------------------------------------------------------------------------------

    void TestZonesAndDistances()
    {
        std::vector<Pass::Disc> const zones = { { { 0.f, 0.f, 0.f }, 2.f }, { { 10.f, 0.f, 0.f }, 1.f },
            { { 0.f, 0.f, 20.f }, 5.f } };     // the last one is another floor
        CHECK(Pass::ZonesNear(zones, { 5.f, 0.f, 0.f }, 4.f) == 2);     // edges at 3 and 4 yd
        CHECK(Pass::ZonesNear(zones, { 5.f, 0.f, 0.f }, 2.9f) == 0);
        CHECK(Pass::ZonesNear(zones, { 0.f, 0.f, 0.f }, 0.f) == 1);
        CHECK_NEAR(Pass::FarDistance({ { 3.f, 4.f, 0.f }, { 30.f, 0.f, 0.f } }, { 0.f, 0.f, 0.f }), 5.f, 1e-4);
        CHECK(Pass::FarDistance({}, { 0.f, 0.f, 0.f }) == Pass::kNoUnitYd);
        PassEntry e;
        CHECK(Pass::InRoom(e, 1000.f, -1000.f));
        e.Room(48.5f, -338.f, 107.5f, -252.5f);
        CHECK(Pass::InRoom(e, 80.f, -280.f) && !Pass::InRoom(e, 40.f, -280.f));
        e.RoomCircle(80.f, -280.f, 10.f);
        CHECK(Pass::InRoom(e, 85.f, -280.f) && !Pass::InRoom(e, 95.f, -280.f));
    }

    // ---- the pick --------------------------------------------------------------------------------------------------

    void TestRankThrow()
    {
        PassEntry e = PassEntry::WithItem(1, 13371, 30.f).FarFrom(0).FewestZones(60, 4.f);
        std::vector<Vec3> const boss = { { 0.f, 0.f, 0.f } };
        std::vector<Pass::Disc> const eggs = { { { 25.f, 0.f, 0.f }, 2.f } };
        std::vector<Pass::Candidate> c = {
            Cand(1, { 10.f, 0.f, 0.f }, 10.f),          // 10 yd from the boss
            Cand(2, { 25.f, 1.f, 0.f }, 20.f),          // 25 yd, on an egg: 25 - 10 = 15
            Cand(3, { 0.f, 20.f, 0.f }, 20.f),          // 20 yd, clear: 20
            Cand(4, { 0.f, -35.f, 0.f }, 35.f),         // out of range
            Cand(5, { -20.f, 0.f, 0.f }, 20.f, false),  // scorched
            Cand(6, { 0.f, -28.f, 0.f }, 29.8f),        // past range - margin
        };
        c.push_back(Cand(7, { -22.f, 0.f, 0.f }, 22.f));
        c.back().inSight = false;                       // behind a pillar
        c.push_back(Cand(8, { -24.f, 0.f, 0.f }, 22.f));
        c.back().excluded = true;                       // two failed throws
        std::vector<Pass::Ranked> const r = Pass::RankThrow(e, c, eggs, boss);
        CHECK(r.size() == 3);
        CHECK(r[0].guid == 3 && r[1].guid == 2 && r[2].guid == 1);
        CHECK_NEAR(r[1].score, 15.f, 0.05);
        // NoSight: the one behind the pillar counts.
        e.NoSight();
        CHECK(Pass::RankThrow(e, c, eggs, boss).front().guid == 7);
        // The wounded first, whatever the score.
        c[0].preferred = true;
        CHECK(Pass::RankThrow(e, c, eggs, boss).front().guid == 1);
        // Ties to the lower guid; no FarFrom units: all score 0 (zones only).
        std::vector<Pass::Candidate> const tie = { Cand(9, { 1.f, 0.f, 0.f }, 5.f), Cand(4, { 2.f, 0.f, 0.f }, 5.f) };
        CHECK(Pass::RankThrow(PassEntry(1), tie, {}, {}).front().guid == 4);
        // Far beyond kFarCapYd counts as kFarCapYd: the clear one wins over the one 60 yd off on an egg.
        PassEntry far = PassEntry(1, 100.f).FarFrom(0).FewestZones(60, 4.f);
        std::vector<Pass::Candidate> const caps = { Cand(1, { 60.f, 0.f, 0.f }, 60.f),
            Cand(2, { 45.f, 0.f, 0.f }, 45.f) };
        std::vector<Pass::Disc> const egg60 = { { { 60.f, 0.f, 0.f }, 1.f } };
        CHECK(Pass::RankThrow(far, caps, egg60, boss).front().guid == 2);
    }

    void TestStickyPicks()
    {
        std::vector<Pass::Ranked> const r = { { 1, false, 30.f }, { 2, false, 27.f }, { 3, false, 20.f } };
        CHECK(Pass::Pick(r, 0) == 1);
        CHECK(Pass::Pick(r, 2) == 2);           // 3 yd behind: keeps its place
        CHECK(Pass::Pick(r, 3) == 1);           // 10 yd behind: loses it
        CHECK(Pass::Pick(r, 9) == 1);           // gone from the ranking
        CHECK(Pass::Pick({}, 2) == 0);
        std::vector<Pass::Ranked> const pref = { { 4, true, 0.f }, { 2, false, 27.f } };
        CHECK(Pass::Pick(pref, 2) == 4);        // the wounded outranks a sticky catcher
        std::vector<Desc::Key> const ready = Pass::PickReady(r, { 2, 3 }, 2);
        CHECK(ready.size() == 2 && ready[0] == 2 && ready[1] == 1);  // 3 is 10 behind 1 for the second place
        CHECK(Pass::PickReady(r, {}, 5).size() == 3);
        CHECK(Pass::ThrowTarget(r, { 2 }) == 2 && Pass::ThrowTarget(r, {}) == 1);
        CHECK(Pass::ThrowTarget(r, { 9 }) == 1);    // the ready catcher cannot catch now: the best who can
    }

    void TestRankReady()
    {
        PassEntry const e = PassEntry(1, 30.f).FarFrom(0);
        std::vector<Vec3> const boss = { { 0.f, 0.f, 0.f } };
        std::vector<Pass::Candidate> c = { Cand(1, { 0.f, 30.f, 0.f }, 34.f), Cand(2, { 0.f, 20.f, 0.f }, 20.f) };
        c.push_back(Cand(3, { 0.f, -25.f, 0.f }, 20.f));
        c.back().inSight = false;
        std::vector<Pass::Ranked> const r = Pass::RankReady(e, c, {}, boss);
        // 1: 30 - (34 - 26) = 22; 2: 20; 3: 25 - 15 = 10. Out of range costs, it does not exclude.
        CHECK(r.size() == 3 && r[0].guid == 1 && r[1].guid == 2 && r[2].guid == 3);
        CHECK_NEAR(r[0].score, 22.f, 1e-4);
    }

    // ---- when to throw ---------------------------------------------------------------------------------------------

    void TestThrowDue()
    {
        PassEntry const e = PassEntry::WithItem(1, 13371).When(PassTrigger::UnitNear(10264, 15.f))
            .When(PassTrigger::UnitNear(10264, 4.f).Urgent()).When(PassTrigger::HeldFor(Ms(6000)))
            .When(PassTrigger::Mechanic(9, Ms(1000))).MinHold(Ms(2000), Condition::HasAura(5));
        std::vector<bool> none(4, false);
        CHECK(!Pass::ThrowDue(e, none, Ms(5999), Ms(0), false).due);
        Pass::Due d = Pass::ThrowDue(e, none, Ms(6000), Ms(0), false);
        CHECK(d.due && d.why == Pass::Why::Held && d.trigger == 2);
        // The hunter within 15 yd, a second after the catch.
        std::vector<bool> near = { true, false, false, false };
        d = Pass::ThrowDue(e, near, Ms(1000), Ms(0), false);
        CHECK(d.due && d.why == Pass::Why::UnitNear && !d.urgent);
        // A wounded holder holds 2 s first...
        d = Pass::ThrowDue(e, near, Ms(1000), Ms(2000), false);
        CHECK(!d.due && d.heldBack);
        CHECK(Pass::ThrowDue(e, near, Ms(2000), Ms(2000), false).due);
        // ...unless he is 4 yd away (urgent).
        std::vector<bool> close = { true, true, false, false };
        d = Pass::ThrowDue(e, close, Ms(500), Ms(2000), false);
        CHECK(d.due && d.urgent && d.trigger == 1);
        // A throw in flight holds everything, urgent too.
        CHECK(!Pass::ThrowDue(e, close, Ms(500), Ms(0), true).due);
        // Fired flags for HeldFor are ignored (judged from the time held).
        std::vector<bool> heldFlag = { false, false, true, false };
        CHECK(!Pass::ThrowDue(e, heldFlag, Ms(100), Ms(0), false).due);
        std::vector<bool> mech = { false, false, false, true };
        CHECK(Pass::ThrowDue(e, mech, Ms(100), Ms(0), false).why == Pass::Why::Mechanic);
        // No triggers: never.
        CHECK(!Pass::ThrowDue(PassEntry(1), {}, Ms(600000), Ms(0), false).due);
        CHECK(std::string(Pass::WhyName(Pass::Why::UnitNear)) == "unit near");
    }

    // ---- the slot --------------------------------------------------------------------------------------------------

    void TestSlot()
    {
        Ms const flight(1500);
        Pass::Slot s;
        Pass::Observe(s, 0, Ms(0), flight);
        CHECK(s.holder == 0 && s.drops == 0);
        Pass::Observe(s, 11, Ms(1000), flight);     // the looter holds it
        CHECK(s.holder == 11 && s.heldSince == Ms(1000) && s.otherChanges == 1);
        Pass::NoteThrow(s, 11, 22, Ms(7000));
        CHECK(s.throws == 1 && Pass::InFlight(s, 11, Ms(8000), flight) && !Pass::InFlight(s, 11, Ms(8500), flight));
        Pass::Observe(s, 11, Ms(7750), flight);     // still flying
        CHECK(s.failed == 0 && s.catches == 0);
        Pass::Observe(s, 22, Ms(7800), flight);     // caught
        CHECK(s.catches == 1 && s.holder == 22 && s.heldSince == Ms(7800) && s.failed == 0);
        // Two throws at 33 that never land: 33 is left out for kExcludeMs.
        Pass::NoteThrow(s, 22, 33, Ms(10000));
        Pass::Observe(s, 22, Ms(11600), flight);
        CHECK(s.failed == 1 && !Pass::Excluded(s, 33, Ms(11600)));
        Pass::NoteThrow(s, 22, 33, Ms(12000));
        Pass::Observe(s, 22, Ms(13600), flight);
        CHECK(s.failed == 2 && Pass::Excluded(s, 33, Ms(13600)) && Pass::Excluded(s, 33, Ms(18599)));
        Pass::Observe(s, 22, Ms(18600), flight);
        CHECK(!Pass::Excluded(s, 33, Ms(18600)) && s.excluded.empty());
        // A failure at 33, then one at 44: the count starts again.
        Pass::NoteThrow(s, 22, 33, Ms(20000));
        Pass::Observe(s, 22, Ms(21600), flight);
        Pass::NoteThrow(s, 22, 44, Ms(22000));
        Pass::Observe(s, 22, Ms(23600), flight);
        CHECK(s.failed == 4 && s.excluded.empty());
        // The holder dies: a drop; a script hands it to 55: another change, not a catch.
        Pass::Observe(s, 0, Ms(24000), flight);
        CHECK(s.drops == 1 && s.heldSince == Ms(-1));
        Pass::Observe(s, 55, Ms(25000), flight);
        CHECK(s.otherChanges == 2 && s.catches == 1);
        // Ready spots kept for the ready catchers only.
        s.ready = { 1, 2 };
        s.spots = { { 1, { 1.f, 0.f, 0.f }, Ms(0), Ms(-1) }, { 3, { 3.f, 0.f, 0.f }, Ms(0), Ms(-1) } };
        Pass::PruneSpots(s);
        CHECK(s.spots.size() == 1 && Pass::FindSpot(s, 1) && !Pass::FindSpot(s, 3));
        CHECK(Pass::OtherSpots(s, 2).size() == 1 && Pass::OtherSpots(s, 1).empty());
    }

    // ---- ready spots -----------------------------------------------------------------------------------------------

    void TestSpotsInTheRookery()
    {
        PassEntry const e = PassEntry::WithItem(1, 13371, 30.f).FarFrom(10264).KeepFrom(15.f)
            .FewestZones(60, 4.f).Room(48.5f, -338.f, 107.5f, -252.5f);
        std::vector<Pass::Disc> const eggs = RookeryDiscs();
        std::vector<Vec3> const solakar = { kSolakarSpawn };
        // A bot on the big clutch at (85.7, -316.4) with the holder at Father Flame: its spot is poor.
        Vec3 const bot{ 86.f, -316.f, 91.4f };
        CHECK(!Pass::SpotFit(e, bot, kFatherFlame, eggs, solakar));
        std::vector<Vec3> const spots = Pass::RankSpots(e, bot, kFatherFlame, eggs, solakar, {});
        CHECK(!spots.empty());
        Vec3 const best = spots.front();
        CHECK(Pass::SpotFit(e, best, kFatherFlame, eggs, solakar));
        CHECK(Pass::ZonesNear(eggs, best, 4.f) == 0);
        CHECK(Pass::FarDistance(solakar, best) >= 15.f);
        CHECK(Geo::Dist2d(best.x, best.y, kFatherFlame.x, kFatherFlame.y) <= 30.f - Pass::kSpotRangeSlackYd);
        // A second catcher keeps kSpotApartYd from the first one's spot.
        std::vector<Vec3> const second = Pass::RankSpots(e, bot, kFatherFlame, eggs, solakar, { best });
        CHECK(!second.empty());
        CHECK(Geo::Dist2d(second.front().x, second.front().y, best.x, best.y) >= Pass::kSpotApartYd - 1e-3f);
        // Out of the room and out of reach never offered; another floor neither.
        for (Vec3 const& p : spots)
        {
            CHECK(Pass::InRoom(e, p.x, p.y));
            CHECK(std::isfinite(Pass::SpotCost(e, p, bot, kFatherFlame, eggs, solakar, {})));
        }
        CHECK(!std::isfinite(Pass::SpotCost(e, { 40.f, -300.f, 91.5f }, bot, kFatherFlame, eggs, solakar, {})));
        CHECK(!std::isfinite(Pass::SpotCost(e, { 80.f, -320.f, 120.f }, bot, kFatherFlame, eggs, solakar, {})));
        // The bare patch at (49.2, -290.2), no egg within 21 yd (handoff §3.8): fit, with the holder 20 yd off.
        CHECK(Pass::SpotFit(e, { 50.f, -290.f, 91.5f }, { 60.f, -300.f, 91.5f }, eggs, solakar));
        // How many of the room's 1 yd grid points are fit at all with the holder in the middle (printed for B11).
        int fit = 0, total = 0;
        for (float x = 49.f; x <= 107.f; x += 1.f)
            for (float y = -337.f; y <= -253.f; y += 1.f)
            {
                ++total;
                fit += Pass::SpotFit(e, { x, y, 91.5f }, { 78.f, -295.f, 91.5f }, eggs, solakar) ? 1 : 0;
            }
        std::printf("pass: Rookery floor points fit to catch on (no clutch within 4 yd, Solakar 15 yd off, holder in "
            "reach): %d of %d (%.0f%%)\n", fit, total, 100.0 * fit / total);
        CHECK(fit > total / 20);
    }

    // ---- Solakar's heat table as a catcher supply model (SP SK4/SK5) ---------------------------------------------

    struct HeatRow
    {
        char const* name;
        int players;
        int holdMs;         // the bot throws after holding this long (SK5: 6 / 4 s normal, 4 / 3 s heroic)
        int scorchedMs;     // Scorched Hands (15 / 25 s normal, 10 / 25 s heroic)
    };

    // Throws on HeldFor alone (the worst case for the supply: no Hunt gives an early throw), 0.75 s flights, for 120 s.
    // The fewest eligible catchers seen at a throw must stay at 1 or more; prints the fewest and the mean.
    void TestCatcherSupply()
    {
        HeatRow const rows[] = { { "normal 5", 5, 6000, 15000 }, { "normal 10", 10, 4000, 25000 },
            { "heroic 5", 5, 4000, 10000 }, { "heroic 10", 10, 3000, 25000 } };
        for (HeatRow const& row : rows)
        {
            PassEntry const e = PassEntry::WithItem(1, 13371, 30.f).When(PassTrigger::HeldFor(Ms(row.holdMs)))
                .Flight(Ms(750));
            Lcg rng;
            std::vector<Vec3> pos;
            for (int i = 0; i < row.players; ++i)
                pos.push_back({ rng.Range(60.f, 100.f), rng.Range(-330.f, -260.f), 91.5f });
            std::vector<int> scorchedUntil(row.players, -1);
            Pass::Slot s;
            int holder = 0;
            Pass::Observe(s, 1, Ms(0), e.flight);
            int fewest = 1000, throws = 0;
            double sum = 0.0;
            int flightTo = -1, landAt = 0;
            for (int t = 0; t <= 120000; t += 50)
            {
                if (flightTo >= 0 && t >= landAt)
                {
                    holder = flightTo;
                    flightTo = -1;
                }
                Pass::Observe(s, static_cast<Desc::Key>(holder + 1), Ms(t), e.flight);
                Ms const held = Ms(t) - s.heldSince;
                Pass::Due const due = Pass::ThrowDue(e, { false }, held, Ms(0),
                    Pass::InFlight(s, static_cast<Desc::Key>(holder + 1), Ms(t), e.flight));
                if (!due.due)
                    continue;
                std::vector<Pass::Candidate> cands;
                for (int i = 0; i < row.players; ++i)
                    if (i != holder)
                        cands.push_back(Cand(static_cast<Desc::Key>(i + 1), pos[i], 10.f, scorchedUntil[i] <= t));
                std::vector<Pass::Ranked> const valid = Pass::RankThrow(e, cands, {}, {});
                int const n = static_cast<int>(valid.size());
                fewest = std::min(fewest, n);
                sum += n;
                ++throws;
                if (valid.empty())
                    continue;
                Desc::Key const to = valid[(throws * 7) % n].guid;   // spread the catches over the eligible
                Pass::NoteThrow(s, static_cast<Desc::Key>(holder + 1), to, Ms(t));
                scorchedUntil[holder] = t + row.scorchedMs;
                flightTo = static_cast<int>(to) - 1;
                landAt = t + 750;
            }
            std::printf("pass: %s: %d throws in 120 s, eligible catchers at a throw: fewest %d, mean %.1f\n",
                row.name, throws, fewest, throws ? sum / throws : 0.0);
            CHECK(throws > 10);
            CHECK(fewest >= 1);
            std::uint32_t const sent = static_cast<std::uint32_t>(throws);
            CHECK(s.catches == sent || s.catches + 1 == sent);      // the last one may still fly
        }
    }

    // ---- the drill -------------------------------------------------------------------------------------------------

    void TestDrill()
    {
        Desc::Preset const* p = Desc::FindPreset("pass");
        CHECK(p != nullptr);
        Descriptor const d = p ? p->build() : Descriptor();
        CHECK(d.passes.size() == 1);
        if (d.passes.size() == 1)
        {
            PassEntry const& e = d.passes.front();
            CHECK(e.how == PassEntry::Throw::MoveAura && e.holderAura == Ids::SPELL_MARKED_DRILL
                && e.afterAura == Ids::SPELL_BAITED_DRILL && e.afterMs == Ms(8000));
            CHECK(e.zoneTag == Desc::DrillPresets::kPassEggTag && e.readyCatchers == 2);
            // Baited (the thrower) does not catch; everyone else does.
            Desc::Context ctx;
            Desc::UnitFacts u;
            CHECK(Desc::Holds(e.catchWhen, u, ctx));
            u.auras = { Ids::SPELL_BAITED_DRILL };
            CHECK(!Desc::Holds(e.catchWhen, u, ctx));
        }
        Drill::Def const* eggs = Drill::Find("pass-eggs");
        CHECK(eggs != nullptr);
        if (eggs)
        {
            CHECK(eggs->steps.size() == 1);
            std::string_view const line = eggs->steps.front().line;
            CHECK(Drill::Value(line, "tag") == std::to_string(Desc::DrillPresets::kPassEggTag));
            CHECK(Drill::Value(line, "repeat") == std::string_view("40"));
            CHECK(Drill::Value(line, "pct") == std::string_view("0"));
            // Laid (40 x 50 ms) and gone (500 + 16,000) before the next cycle lays them again.
            CHECK(500u + 16000u + 40u * 50u <= eggs->cycleMs);
        }
        Drill::ProgramDef const* pd = Drill::FindProgramDrill("pass");
        CHECK(pd != nullptr && pd->listed);
        std::string err;
        std::optional<Drill::Program> const prog = Drill::ParseProgram("pass", err);
        CHECK(prog.has_value() && err.empty());
        if (prog)
        {
            bool describe = false, debuff = false, eggRun = false;
            for (Drill::ProgramEntry const& x : prog->entries)
            {
                describe = describe || (x.kind == Drill::EntryKind::Describe && x.preset == p);
                debuff = debuff || (x.kind == Drill::EntryKind::Debuff && x.spellId == Ids::SPELL_MARKED_DRILL
                    && x.count == 1);
                eggRun = eggRun || (x.kind == Drill::EntryKind::Drill && x.drill == eggs && x.cycles == 3);
            }
            CHECK(describe && debuff && eggRun);
            CHECK(prog->loopMs == 3u * 20000u + Drill::kProgramGapMs);
        }
    }

    // ---- solver cost with the egg zones (M5's 2,000 us) ------------------------------------------------------------

    Solver::Input EggInput(std::vector<Pass::Disc> const& zones, Vec3 bot)
    {
        Solver::Input in;
        in.bot = bot;
        in.now = Ms(100000);
        in.speed = 7.f;
        std::uint32_t id = 1;
        for (Pass::Disc const& z : zones)
        {
            Solver::Hazard h;
            h.id = id++;
            h.shape = Shape::Circle(z.r);
            h.poses = { { z.c.x, z.c.y, z.c.z, 0.f } };
            h.marginYd = 0.75f;
            h.timing = Solver::MakeInterval(in.now - Ms(5000), Solver::kForever);
            in.hazards.push_back(h);
        }
        return in;
    }

    struct Cost
    {
        double mean = 0.0;
        double worst = 0.0;
        int outcome = 0;
    };

    Cost TimeSolve(Solver::Input const& in, int runs)
    {
        StraightOracle oracle;
        Cost c;
        c.outcome = static_cast<int>(Solver::Solve(in, oracle).outcome);
        double total = 0.0;
        for (int i = 0; i < runs; ++i)
        {
            auto const s0 = std::chrono::steady_clock::now();
            Solver::Plan const p = Solver::Solve(in, oracle);
            auto const s1 = std::chrono::steady_clock::now();
            double const us = std::chrono::duration<double, std::micro>(s1 - s0).count();
            total += us;
            c.worst = std::max(c.worst, us);
            CHECK(p.candidates > 0);
        }
        c.mean = total / runs;
        return c;
    }

    void BenchmarkEggs()
    {
        // The bot stands on the zone nearest the big clutch at (85.7, -316.4): it must leave it (a fresh escape).
        auto onZone = [](std::vector<Pass::Disc> const& zones)
        {
            Pass::Disc best = zones.front();
            for (Pass::Disc const& z : zones)
                if (Geo::Dist2d(z.c.x, z.c.y, 86.f, -316.f) < Geo::Dist2d(best.c.x, best.c.y, 86.f, -316.f))
                    best = z;
            return Vec3{ best.c.x + 0.3f, best.c.y, best.c.z };
        };
        struct Case
        {
            char const* name;
            std::vector<Pass::Disc> zones;
        };
        Case const cases[] = { { "33 clutches", RookeryDiscs() }, { "40 zones", Rookery40() },
            { "138 eggs, one zone each", RookeryEggs() } };
        double forty = 0.0;
        for (Case const& c : cases)
        {
            Solver::Input const in = EggInput(c.zones, onZone(c.zones));
            Cost const cost = TimeSolve(in, 100);
            std::printf("pass: solve with %s (%zu hazards, bot on a clutch): outcome %d, mean %.0f us, max %.0f us "
                "(M5 limit 2000 us)%s\n", c.name, in.hazards.size(), cost.outcome, cost.mean, cost.worst,
                cost.mean > 2000.0 ? "  ** OVER **" : "");
            CHECK(cost.outcome == static_cast<int>(Solver::Outcome::Move));
            if (c.zones.size() == 40)
                forty = cost.mean;

            // Route around the eggs across the room, bare patch to bare patch (a follow that crosses the clutches).
            Solver::Input standing = in;
            standing.bot = { 50.f, -290.f, 91.5f };
            StraightOracle oracle;
            Solver::RouteStats st;
            auto const r0 = std::chrono::steady_clock::now();
            Solver::Plan const route = Solver::Route(standing, { 106.f, -261.f, 91.5f }, oracle,
                Solver::RouteParams(), &st);
            auto const r1 = std::chrono::steady_clock::now();
            std::printf("pass: route across the Rookery with %s: outcome %d, %u cells, %u expansions, %u legs: %.0f us"
                "\n", c.name, static_cast<int>(route.outcome), st.cells, st.expansions, st.legs,
                std::chrono::duration<double, std::micro>(r1 - r0).count());
            CHECK(route.outcome == Solver::Outcome::Move && st.found);
        }
        // The handoff's question (§3.4 SK eggs, §6 solver cost): about 40 persistent zones stay under M5's budget.
        CHECK(forty < 2000.0);

        // The pass's own share per update: 10 catchers ranked twice and a ready spot picked, with the 40 zones.
        PassEntry const e = PassEntry::WithItem(1, 13371, 30.f).FarFrom(10264).KeepFrom(15.f).FewestZones(60, 4.f)
            .Room(48.5f, -338.f, 107.5f, -252.5f).ReadyCatchers(2);
        std::vector<Pass::Disc> const zones = Rookery40();
        std::vector<Vec3> const units = { kSolakarSpawn, { 70.f, -300.f, 91.5f } };
        Lcg rng;
        std::vector<Pass::Candidate> cands;
        for (int i = 0; i < 9; ++i)
            cands.push_back(Cand(static_cast<Desc::Key>(i + 1), { rng.Range(55.f, 105.f), rng.Range(-335.f, -255.f),
                91.5f }, rng.Range(5.f, 35.f)));
        int const runs = 200;
        std::size_t sink = 0;
        auto const t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < runs; ++i)
        {
            std::vector<Pass::Ranked> const ready = Pass::RankReady(e, cands, zones, units);
            sink += Pass::PickReady(ready, {}, 2).size();
            sink += Pass::RankThrow(e, cands, zones, units).size();
            sink += Pass::RankSpots(e, cands[0].pos, kFatherFlame, zones, units, {}).size();
        }
        auto const t1 = std::chrono::steady_clock::now();
        double const us = std::chrono::duration<double, std::micro>(t1 - t0).count() / runs;
        std::printf("pass: the pass's rules per update (9 catchers ranked twice, 96 spot candidates, 40 zones): "
            "%.1f us\n", us);
        CHECK(sink > 0);
        CHECK(us < 500.0);
    }
}

int main()
{
    TestAnyOfAndReferences();
    TestZonesAndDistances();
    TestRankThrow();
    TestStickyPicks();
    TestRankReady();
    TestThrowDue();
    TestSlot();
    TestSpotsInTheRookery();
    TestCatcherSupply();
    TestDrill();
    BenchmarkEggs();
    return Report("pass_test");
}
