// Standalone tests for src/MechanicJump.h, the Jump kind's pure rules (Stratholme build X3, handoff §7 #6, ST ruling
// 17), its resolve filter rule (MechanicJudge.h) and the `stratholme-jump` drill (MechanicDrill.h). Build:
// tests/run.sh.

#include "check.h"
#include "MechanicDrill.h"
#include "MechanicJudge.h"
#include "MechanicJump.h"
#include <cmath>
#include <string_view>
#include <vector>

using namespace BossMechanics;
using Jump::Ms;

namespace
{
    constexpr double kEps = 1e-3;

    Judge::Candidate Player()
    {
        return {};
    }

    // The sweep trips players on the ground only; a player in the air is spared; pets are never hit (a pet cannot
    // jump). The airborne flag means nothing to the other kinds.
    void TestFilter()
    {
        CHECK(Judge::PlayersOnly(Kind::Jump));
        CHECK(!Judge::IsShapeKind(Kind::Jump));     // never a hazard to dodge: the floor is safe for a bot that hops
        CHECK(Judge::KeepResolveTarget(Kind::Jump, Player()));
        Judge::Candidate air = Player();
        air.airborne = true;
        CHECK(!Judge::KeepResolveTarget(Kind::Jump, air));
        Judge::Candidate pet = Player();
        pet.player = false;
        CHECK(!Judge::KeepResolveTarget(Kind::Jump, pet));
        Judge::Candidate outside = Player();
        outside.inside = false;
        CHECK(!Judge::KeepResolveTarget(Kind::Jump, outside));
        Judge::Candidate other = Player();
        other.eligible = false;
        CHECK(!Judge::KeepResolveTarget(Kind::Jump, other));
        for (Kind k : { Kind::Zone, Kind::Frontal, Kind::Spread, Kind::RunOut, Kind::Bait })
            CHECK(Judge::KeepResolveTarget(k, air));
        // A decoy rule or a GM still wins.
        Judge::Candidate gm = Player();
        gm.gameMaster = true;
        CHECK(!Judge::KeepResolveTarget(Kind::Jump, gm));
    }

    // The hop's timeline: start HopMs - LandAfterMs before the resolve, land LandAfterMs after it; a late brain step
    // launches a shorter hop that still lands then (or kMinHop from now); at or after the resolve it is too late.
    void TestDecide()
    {
        Jump::HopParams const p;     // 800 / 250
        Ms const r{ 100000 };
        CHECK(Jump::HopAt(r, p) == Ms(99450));
        CHECK(Jump::LandAt(r, p) == Ms(100250));
        CHECK(Jump::Decide(Ms(99449), r, p).step == Jump::Step::Wait);
        Jump::Decision d = Jump::Decide(Ms(99450), r, p);
        CHECK(d.step == Jump::Step::Launch && d.duration == Ms(800) && d.landAt == Ms(100250));
        d = Jump::Decide(Ms(99800), r, p);
        CHECK(d.step == Jump::Step::Launch && d.duration == Ms(450) && d.landAt == Ms(100250));
        d = Jump::Decide(Ms(99950), r, p);
        CHECK(d.step == Jump::Step::Launch && d.duration == Ms(300) && d.landAt == Ms(100250));
        d = Jump::Decide(Ms(99990), r, p);       // later than LandAt - kMinHop: a kMinHop hop over the resolve
        CHECK(d.step == Jump::Step::Launch && d.duration == Jump::kMinHop && d.landAt == Ms(100290));
        CHECK(d.landAt > r);
        CHECK(Jump::Decide(r, r, p).step == Jump::Step::Late);
        CHECK(Jump::Decide(r + Ms(1), r, p).step == Jump::Step::Late);
        // Every launch lands after the resolve and lasts no longer than HopMs.
        for (int64_t t = 99450; t < 100000; t += 7)
        {
            Jump::Decision const e = Jump::Decide(Ms(t), r, p);
            CHECK(e.step == Jump::Step::Launch);
            CHECK(e.landAt > r && e.duration <= p.hop && e.duration >= Jump::kMinHop);
            CHECK(e.landAt - Ms(t) == e.duration);
        }
    }

    // MoveJump(x, y, z, speedXY, speedZ): the spline lasts step / speedXY; the apex is speedZ^2 / (2 g); speedXY must
    // stay above 0.1 (MotionMaster::MoveJump refuses it at or below).
    void TestSpeeds()
    {
        Jump::Speeds const s = Jump::SpeedsFor(Ms(800));
        CHECK_NEAR(s.stepYd / s.xy, 0.8, kEps);
        CHECK_NEAR(s.z * s.z / (2.0 * Jump::kGravity), s.apexYd, kEps);
        CHECK_NEAR(s.apexYd, 1.543, 0.01);      // a player's jump is about 1.6 yd
        CHECK(s.stepYd <= 0.2f);                // a hop in place
        for (int64_t ms = 300; ms <= 1200; ms += 50)
        {
            Jump::Speeds const t = Jump::SpeedsFor(Ms(ms));
            CHECK(t.xy > 0.1f);
            CHECK_NEAR(t.stepYd / t.xy * 1000.0, static_cast<double>(ms), 0.5);
            CHECK(t.stepYd >= Jump::kHopYd && t.stepYd <= 0.25f);
            CHECK(t.apexYd > 0.2f && t.apexYd <= Jump::kMaxApexYd + 1e-3f);
            CHECK_NEAR(t.z * t.z / (2.0 * Jump::kGravity), t.apexYd, kEps);
        }
    }

    // Who counts as in the air at the resolve.
    void TestClear()
    {
        Ms const now{ 5000 };
        Jump::Air a;
        CHECK(Jump::Clear(a, now, false) == Jump::ClearBy::None);
        CHECK(Jump::Clear(a, now, true) == Jump::ClearBy::None);
        a.falling = true;                       // a human's jump, a fall
        CHECK(Jump::Clear(a, now, false) == Jump::ClearBy::Falling);
        a = Jump::Air();
        a.hopNoted = true;
        a.hopFrom = Ms(4500);
        a.hopTo = Ms(5250);
        a.hopSplineLive = true;
        CHECK(Jump::Clear(a, now, false) == Jump::ClearBy::Hop);
        // The hop was cut (a stun, a knockback, another spline) or never launched: only the fallback counts it, and
        // only while its planned air time covers the resolve.
        a.hopSplineLive = false;
        CHECK(Jump::Clear(a, now, false) == Jump::ClearBy::None);
        CHECK(Jump::Clear(a, now, true) == Jump::ClearBy::Planned);
        CHECK(Jump::Clear(a, Ms(5300), true) == Jump::ClearBy::None);
        CHECK(Jump::Clear(a, Ms(4400), true) == Jump::ClearBy::None);
        // No note: no fallback either (a bot that could not hop, rooted or stunned, is tripped like a player).
        Jump::Air none;
        CHECK(Jump::Clear(none, now, true) == Jump::ClearBy::None);
        CHECK(std::string_view(Jump::ClearByName(Jump::ClearBy::Planned)) == "planned");
    }

    // A desk model of the brain and the resolve: the brain steps every `tick` ms from a random phase (the waker's
    // fast tick, 100 ms; a loaded realm or a long update, more), launches at its first step at or after HopAt, and the
    // hop's spline flies until it lands. The registry resolves on the first map update at or after the resolve, up to
    // 100 ms late (full updates). The bot is in the air then for every phase and delay up to a 300 ms brain tick.
    void TestTimingModel()
    {
        Jump::HopParams const p;
        Ms const r{ 50000 };
        for (int64_t tick : { 50, 100, 200, 300 })
            for (int64_t phase = 0; phase < tick; phase += 10)
            {
                // The first brain step at or after HopAt.
                int64_t step = Jump::HopAt(r, p).count() - tick + phase;
                while (step < Jump::HopAt(r, p).count())
                    step += tick;
                Jump::Decision const d = Jump::Decide(Ms(step), r, p);
                CHECK(d.step == Jump::Step::Launch);
                for (int64_t delay = 0; delay <= 100; delay += 10)
                {
                    int64_t const resolveTick = r.count() + delay;
                    CHECK(step <= resolveTick && resolveTick < d.landAt.count());
                }
            }
        // Chained skips 1.5 s apart (Double Dutch, the heroic fallback): the next hop starts after the last one lands,
        // over the whole config range (HopMs 400-1200, LandAfterMs 100 to HopMs - 200).
        for (int64_t hop = 400; hop <= 1200; hop += 100)
            for (int64_t land = 100; land <= hop - 200; land += 100)
            {
                Jump::HopParams q;
                q.hop = Ms(hop);
                q.landAfter = Ms(land);
                Ms const r1{ 10000 };
                Ms const r2 = r1 + Ms(1500);
                CHECK(Jump::HopAt(r2, q) > Jump::LandAt(r1, q));
                CHECK(Jump::HopAt(r1, q) < r1);             // in the air before the resolve
            }
    }

    // The `stratholme-jump` drill against ST "M4. Jump Rope" and Timmy's heroic: three 2 s skips back to back, Freeze
    // Tag's RunOut landing in the third skip's hop, Double Dutch's five skips of 2, 2, 1.5, 1.5 and 1.5 s.
    void TestDrill()
    {
        Drill::Def const* d = Drill::Find("stratholme-jump");
        CHECK(d != nullptr);
        if (!d)
            return;
        std::vector<Drill::Step const*> rope;
        std::vector<Drill::Step const*> dutch;
        Drill::Step const* tag = nullptr;
        for (Drill::Step const& s : d->steps)
        {
            std::string_view const line(s.line);
            std::string_view const shape = Drill::Tokens(line).front();
            std::optional<std::string_view> const t = Drill::Value(line, "tag");
            CHECK(t.has_value());
            if (shape == "jump")
            {
                CHECK(Drill::Tokens(line)[1] == "40");
                CHECK(Drill::Value(line, "pct") == std::optional<std::string_view>("25"));
                CHECK(Drill::Value(line, "weak") == std::optional<std::string_view>("1"));
                if (*t == "1")
                    rope.push_back(&s);
                else
                    dutch.push_back(&s);
            }
            else
            {
                CHECK(shape == "runout");
                tag = &s;
            }
        }
        CHECK(rope.size() == 3);
        CHECK(dutch.size() == 5);
        CHECK(tag != nullptr);
        Jump::HopParams const p;
        auto backToBack = [&p](std::vector<Drill::Step const*> const& skips, std::vector<std::uint32_t> const& warns)
        {
            CHECK(skips.size() == warns.size());
            for (std::size_t i = 0; i < skips.size() && i < warns.size(); ++i)
            {
                CHECK(Drill::WarnMs(skips[i]->line) == warns[i]);
                if (i == 0)
                    continue;
                // Each skip starts as the one before it ends, and its hop starts after the one before it landed.
                std::uint32_t const prevEnd = skips[i - 1]->atMs + Drill::WarnMs(skips[i - 1]->line);
                CHECK(skips[i]->atMs == prevEnd);
                Ms const r1{ prevEnd };
                Ms const r2{ skips[i]->atMs + Drill::WarnMs(skips[i]->line) };
                CHECK(Jump::HopAt(r2, p) > Jump::LandAt(r1, p));
            }
        };
        backToBack(rope, { 2000, 2000, 2000 });
        backToBack(dutch, { 2000, 2000, 1500, 1500, 1500 });
        if (tag && rope.size() == 3)
        {
            // The RunOut resolves inside the third skip's hop: the carrier escapes before the hop, every bot holds
            // (or hops) through its landing.
            Ms const landing{ tag->atMs + Drill::WarnMs(tag->line) };
            Ms const third{ rope[2]->atMs + Drill::WarnMs(rope[2]->line) };
            CHECK(landing >= Jump::HopAt(third, p) && landing <= Jump::LandAt(third, p));
            CHECK(Drill::Value(tag->line, "pick") == std::optional<std::string_view>("nontank"));
        }
        CHECK(d->defaultCycles * d->cycleMs == 120000);
        CHECK(Drill::kJumpTagRope == 1 && Drill::kJumpTagTag == 2 && Drill::kJumpTagDutch == 3
            && Drill::kJumpTagDutchFast == 4);
    }
}

int main()
{
    TestFilter();
    TestDecide();
    TestSpeeds();
    TestClear();
    TestTimingModel();
    TestDrill();
    return Report("jump_test");
}
