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

    // Stratholme F-2b (R-2-FW m-1): the resolve's map update comes 400 ms late (a loaded world). The bot hopped on
    // time (launch 4,450, landing 5,250 for a resolve at 5,000); the registry sampled the hop every 100 ms up to
    // 4,950; at the late tick (5,400) the hop has landed and the walk under it launched a new spline. The sweep is
    // judged at the resolve: the bot is in the air; with the fallback on, the Planned window reads the resolve too.
    Jump::HopTrack Sampled(Ms from, Ms to, Ms lastLive, Ms lateTick, bool replaced, bool cut)
    {
        Jump::HopTrack t;
        for (Ms at = from + Ms(50); at <= lastLive; at += Ms(100))
            Jump::SampleHop(t, from, to, true, false, at - from, false, at, Jump::kSampleSlack);
        if (replaced)
            Jump::SampleHop(t, from, to, false, false, Ms(0), cut, lateTick, Jump::kSampleSlack);
        else       // the bot stood still: the hop's spline stays, finalized at its whole length
            Jump::SampleHop(t, from, to, true, true, to - from, false, lateTick, Jump::kSampleSlack);
        return t;
    }

    void TestLateTick()
    {
        Jump::HopParams const p;
        Ms const resolve{ 5000 };
        Ms const from = Jump::HopAt(resolve, p);
        Ms const to = Jump::LandAt(resolve, p);
        Ms const late = resolve + Ms(400);
        CHECK(late > to);                                   // the case: the hop has landed by the judging tick
        for (bool replaced : { true, false })
        {
            Jump::HopTrack const t = Sampled(from, to, Ms(4950), late, replaced, false);
            CHECK(t.ended);
            CHECK(t.airTo == to);
            Jump::Air a;
            a.hopNoted = true;
            a.hopFrom = from;
            a.hopTo = to;
            a.hopAirTo = t.airTo;
            a.hopSplineLive = false;
            CHECK(Jump::Clear(a, resolve, false) == Jump::ClearBy::Hop);
            CHECK(Jump::Clear(a, late, false) == Jump::ClearBy::None);     // the as-built judgment at the late tick
        }
        // The fallback alone (no track: the launch was refused): the Planned window covers the resolve, not the
        // late tick.
        Jump::Air planned;
        planned.hopNoted = true;
        planned.hopFrom = from;
        planned.hopTo = to;
        CHECK(Jump::Clear(planned, resolve, true) == Jump::ClearBy::Planned);
        CHECK(Jump::Clear(planned, late, true) == Jump::ClearBy::None);
        // A cut is not credited: stunned at the late tick, the hop counts only as far as it was seen (4,950).
        Jump::HopTrack const cut = Sampled(from, to, Ms(4950), late, true, true);
        CHECK(cut.ended && cut.airTo < resolve);
        // Replaced well before its landing with nothing seen (a stop spline, a knockback the flags missed): a cut.
        Jump::HopTrack const early = Sampled(from, to, Ms(4650), Ms(4750), true, false);
        CHECK(early.ended && early.airTo < Ms(4750));
        Jump::Air e;
        e.hopNoted = true;
        e.hopFrom = from;
        e.hopTo = to;
        e.hopAirTo = early.airTo;
        CHECK(Jump::Clear(e, resolve, false) == Jump::ClearBy::None);
        // Interrupted on the spot (DisableSpline: same id, done, time passed where it stopped).
        Jump::HopTrack stop;
        Jump::SampleHop(stop, from, to, true, true, Ms(200), false, from + Ms(250), Jump::kSampleSlack);
        CHECK(stop.ended && stop.airTo == from + Ms(200));
        // Once ended, later samples change nothing (the next spline is not the hop's).
        Jump::SampleHop(stop, from, to, false, false, Ms(0), false, to + Ms(500), Jump::kSampleSlack);
        CHECK(stop.airTo == from + Ms(200));
        // Still in flight at the late tick: the live spline counts, as before.
        Jump::Air live;
        live.hopNoted = true;
        live.hopFrom = from;
        live.hopTo = to;
        live.hopSplineLive = true;
        CHECK(Jump::Clear(live, resolve, false) == Jump::ClearBy::Hop);
    }

    // Stratholme F-2b (R-2-FW m-2): a walking bot hops along its walk. Three back-to-back 2 s skips (hops 1,450-2,250,
    // 3,450-4,250, 5,450-6,250) and a RunOut registered at 1,000 that resolves at 5,500: the bot must walk 25 yd by
    // 5,200 (marginMs 300 before it); the solver timed it at full run speed (no hop in its travel time). Hopping in
    // place loses 2.4 s of the walk and misses; hopping along keeps the walk's pace.
    float Walked(Ms start, Ms until, float speed, std::vector<Ms> const& hopsAt, Ms hopLen, float toCornerYd,
        bool along)
    {
        // 10 ms steps: walking adds speed * dt; a hop adds its step spread over its length.
        float yd = 0.f;
        for (Ms t = start; t < until; t += Ms(10))
        {
            bool inHop = false;
            for (Ms h : hopsAt)
                if (t >= h && t < h + hopLen)
                {
                    inHop = true;
                    Jump::Speeds const s = along ? Jump::SpeedsAlong(hopLen, speed, toCornerYd)
                        : Jump::SpeedsFor(hopLen);
                    yd += s.stepYd * 10.f / static_cast<float>(hopLen.count());
                }
            if (!inHop)
                yd += speed * 0.01f;
        }
        return yd;
    }

    void TestHopAlong()
    {
        Jump::HopParams const p;
        float const run = 7.f;
        // The step: run speed x the hop's length, clipped to the next corner; a corner too close hops in place.
        Jump::Speeds const s = Jump::SpeedsAlong(p.hop, run, 30.f);
        CHECK_NEAR(s.stepYd, 5.6, 1e-3);
        CHECK_NEAR(s.xy, run, 1e-3);
        CHECK_NEAR(s.apexYd, Jump::SpeedsFor(p.hop).apexYd, 1e-4);     // the same jump, moving
        CHECK_NEAR(Jump::SpeedsAlong(p.hop, run, 3.f).stepYd, 3.0, 1e-4);
        CHECK(Jump::SpeedsAlong(p.hop, run, 0.3f).stepYd == Jump::SpeedsFor(p.hop).stepYd);
        CHECK(Jump::SpeedsAlong(p.hop, 0.f, 30.f).stepYd == Jump::SpeedsFor(p.hop).stepYd);
        CHECK(Jump::HopLostMs(p.hop, 5.6f, run) == Ms(0));
        CHECK(Jump::HopLostMs(p.hop, 3.5f, run) == Ms(300));
        CHECK(Jump::HopLostMs(p.hop, 0.15f, 0.f) == p.hop);

        std::vector<Ms> const hops{ Jump::HopAt(Ms(2000), p), Jump::HopAt(Ms(4000), p), Jump::HopAt(Ms(6000), p) };
        CHECK(hops[0] == Ms(1450) && hops[1] == Ms(3450) && hops[2] == Ms(5450));
        Ms const start = Ms(1000) + Ms(150);                // the RunOut's registration + startDelay
        Ms const deadline = Ms(5500) - Ms(300);
        float const need = 25.f;
        CHECK(start + Ms(static_cast<std::int64_t>(need / run * 1000.f)) <= deadline);  // the solver's plan fits
        float const inPlace = Walked(start, deadline, run, hops, p.hop, 100.f, false);
        float const along = Walked(start, deadline, run, hops, p.hop, 100.f, true);
        CHECK(inPlace < need);                              // the as-built in-place hop misses the RunOut
        CHECK(along >= need);
        // A path of short legs (corners every 4 yd): each hop stops at its corner, the loss stays under 1 s per hop.
        float const cornered = Walked(start, deadline, run, hops, p.hop, 4.f, true);
        CHECK(cornered > inPlace);
        CHECK(Jump::HopLostMs(p.hop, 4.f, run) < Ms(300));
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
    TestLateTick();
    TestHopAlong();
    TestTimingModel();
    TestDrill();
    return Report("jump_test");
}
