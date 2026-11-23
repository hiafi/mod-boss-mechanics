// Standalone tests for the Scarlet Monastery build's BM additions (SM-BM): the Gaze kind's pure rules
// (src/MechanicGaze.h) and its resolve filter rule (MechanicJudge.h); Ordering::roleLast in the Soak claim
// (MechanicAssign.h); EndBacksOff / TankNearSource's back-off spot and claim region (MechanicViewRules.h) run through
// the solver; the facing hold rule; and the pass ids of a lasting Spread (Judge::CarrierPass). F-4a: the bot's own
// walk against its look windows (Gaze::OwnWalkAt), the route in past a refused chase (Landing::ReachSafe, Halden's
// Close Ranks band) and Feral Charge - Cat's Behind landing beside Ironspine's rot. Build: tests/run.sh.

#include "check.h"
#include "MechanicAssign.h"
#include "MechanicGaze.h"
#include "MechanicJudge.h"
#include "MechanicLanding.h"
#include "MechanicSolver.h"
#include "MechanicViewRules.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

using namespace BossMechanics;
using Geo::Pose;
using Geo::Vec3;
using Ms = std::chrono::milliseconds;

namespace
{
    constexpr double kEps = 1e-3;
    constexpr float kMargin = 0.75f;            // BossMechanics.Bots.SafetyMarginYd
    constexpr float kLookMarginDeg = 10.f;      // BossMechanics.Bots.LookAway.MarginDeg
    Ms const kNow{ 100000 };

    struct Lcg
    {
        std::uint64_t state = 0x5c4e7ull;

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

    float Deg(float rad)
    {
        return Geo::RadToDeg(rad);
    }

    // ---- Gaze (F13) ----------------------------------------------------------------------------------------------

    void TestGazeFaces()
    {
        Vec3 const me{ 0.f, 0.f, 0.f };
        Vec3 const east{ 10.f, 0.f, 0.f };
        CHECK(Gaze::Faces(me, 0.f, east, 180.f));                       // looking straight at it
        CHECK(!Gaze::Faces(me, Geo::kPi, east, 180.f));                 // back turned
        CHECK(Gaze::Faces(me, Geo::DegToRad(89.f), east, 180.f));       // inside the front half
        CHECK(Gaze::Faces(me, Geo::DegToRad(90.f), east, 180.f));       // the edge counts as facing
        CHECK(!Gaze::Faces(me, Geo::DegToRad(91.f), east, 180.f));
        CHECK(!Gaze::Faces(me, Geo::DegToRad(-91.f), east, 180.f));
        CHECK(Gaze::Faces(me, Geo::DegToRad(29.f), east, 60.f));        // a narrow arc
        CHECK(!Gaze::Faces(me, Geo::DegToRad(31.f), east, 60.f));
        for (float f = -180.f; f < 180.f; f += 15.f)
            CHECK(Gaze::Faces(me, Geo::DegToRad(f), east, 360.f));      // 360: facing does not matter
        CHECK(Gaze::Faces(me, Geo::kPi, { 0.3f, 0.f, 0.f }, 180.f));    // on the source: no away
        // Clamped arcs: nonsense becomes the default, the range is [1, 360].
        CHECK_NEAR(Gaze::ClampArcDeg(NAN), Gaze::kDefaultArcDeg, kEps);
        CHECK_NEAR(Gaze::ClampArcDeg(0.f), Gaze::kMinArcDeg, kEps);
        CHECK_NEAR(Gaze::ClampArcDeg(720.f), Gaze::kMaxArcDeg, kEps);
        CHECK_NEAR(Gaze::Bearing(me, { 0.f, 5.f, 0.f }), Geo::kPi / 2.f, kEps);
    }

    void TestGazeFilter()
    {
        CHECK(Judge::PlayersOnly(Kind::Gaze));
        CHECK(!Judge::IsShapeKind(Kind::Gaze));     // never a hazard to dodge: the bots turn, the floor is not hot
        Judge::Candidate facing;
        CHECK(Judge::KeepResolveTarget(Kind::Gaze, facing));
        Judge::Candidate away;
        away.lookedAway = true;
        CHECK(!Judge::KeepResolveTarget(Kind::Gaze, away));
        Judge::Candidate pet;
        pet.player = false;
        CHECK(!Judge::KeepResolveTarget(Kind::Gaze, pet));
        Judge::Candidate far;
        far.inside = false;                         // out of range
        CHECK(!Judge::KeepResolveTarget(Kind::Gaze, far));
        Judge::Candidate other;
        other.eligible = false;
        CHECK(!Judge::KeepResolveTarget(Kind::Gaze, other));
        // The flag means nothing to the other kinds.
        for (Kind k : { Kind::Zone, Kind::Frontal, Kind::Spread, Kind::RunOut, Kind::Bait, Kind::Jump, Kind::Hide })
            CHECK(Judge::KeepResolveTarget(k, away));
        Judge::Candidate hidden;
        hidden.hidden = true;
        CHECK(Judge::KeepResolveTarget(Kind::Gaze, hidden));   // a blocker does not save you from a gaze
    }

    void TestGazeChooseFacing()
    {
        // No duty: the preferred facing.
        CHECK_NEAR(Gaze::ChooseFacing(1.f, {}, kLookMarginDeg), 1.f, kEps);
        // The victim stands clear of the gaze: keep facing it (a melee bot keeps hitting).
        std::vector<Gaze::Duty> const eastGaze = { { 0.f, 180.f } };
        CHECK_NEAR(Gaze::ChooseFacing(Geo::kPi, eastGaze, kLookMarginDeg), Geo::kPi, kEps);
        CHECK_NEAR(Gaze::ChooseFacing(Geo::DegToRad(120.f), eastGaze, kLookMarginDeg), Geo::DegToRad(120.f), kEps);
        // The victim stands in the arc (the Eye beside the boss): the nearest edge of the widened arc.
        float const f = Gaze::ChooseFacing(Geo::DegToRad(30.f), eastGaze, kLookMarginDeg);
        CHECK(std::fabs(Deg(f) - 100.f) < 0.2f);
        CHECK(Gaze::Overlap(f, 0.f, 180.f, kLookMarginDeg) <= 0.f);
        float const g = Gaze::ChooseFacing(Geo::DegToRad(-30.f), eastGaze, kLookMarginDeg);
        CHECK(std::fabs(Deg(g) + 100.f) < 0.2f);
        // Two gazes from opposite sides with 120 degree arcs: north or south clears both.
        std::vector<Gaze::Duty> const two = { { 0.f, 120.f }, { Geo::kPi, 120.f } };
        float const h = Gaze::ChooseFacing(0.f, two, kLookMarginDeg);
        CHECK(Gaze::Overlap(h, 0.f, 120.f, kLookMarginDeg) <= 0.f);
        CHECK(Gaze::Overlap(h, Geo::kPi, 120.f, kLookMarginDeg) <= 0.f);
        // Two front halves from opposite sides cannot both be cleared: the least overlap, never a crash.
        std::vector<Gaze::Duty> const clash = { { 0.f, 180.f }, { Geo::kPi, 180.f } };
        float const c = Gaze::ChooseFacing(0.3f, clash, kLookMarginDeg);
        CHECK(std::isfinite(c));
        CHECK(!Gaze::Faces({ 0.f, 0.f, 0.f }, c, { 10.f, 0.f, 0.f }, 180.f)
            || !Gaze::Faces({ 0.f, 0.f, 0.f }, c, { -10.f, 0.f, 0.f }, 180.f));
    }

    // 2,000 random bots, sources and victims: the chosen facing never faces the source (the judge's own test, the
    // margin on top), and it is the victim's bearing whenever that bearing is clear.
    void TestGazeRandom()
    {
        Lcg rng;
        std::uint32_t kept = 0;
        std::uint32_t turned = 0;
        for (int i = 0; i < 2000; ++i)
        {
            Vec3 const bot{ rng.Range(-20.f, 20.f), rng.Range(-20.f, 20.f), 0.f };
            Vec3 const src{ rng.Range(-20.f, 20.f), rng.Range(-20.f, 20.f), 0.f };
            if (Geo::Dist2d(bot.x, bot.y, src.x, src.y) < 1.f)
                continue;
            float const arc = rng.Next01() < 0.5f ? 180.f : rng.Range(30.f, 300.f);
            float const victim = rng.Range(-Geo::kPi, Geo::kPi);
            float const bearing = Gaze::Bearing(bot, src);
            float const f = Gaze::ChooseFacing(victim, { { bearing, arc } }, kLookMarginDeg);
            CHECK(!Gaze::Faces(bot, f, src, arc));
            CHECK(Gaze::Overlap(f, bearing, arc, kLookMarginDeg) <= 0.f);
            if (Gaze::Overlap(victim, bearing, arc, kLookMarginDeg) <= 0.f)
            {
                CHECK_NEAR(Geo::WrapPi(f - victim), 0.f, kEps);
                ++kept;
            }
            else
                ++turned;
        }
        std::printf("  gaze: random facings: %u kept the victim, %u turned to the arc's edge\n", kept, turned);
        CHECK(kept > 100 && turned > 100);
    }

    void TestGazeWindow()
    {
        Ms const created{ 1000 };
        Ms const resolve{ 3000 };
        Gaze::Window const all = Gaze::LookWindow(created, resolve, Ms(0));
        CHECK(all.from == created && all.to == resolve);
        Gaze::Window const late = Gaze::LookWindow(created, resolve, Ms(800));
        CHECK(late.from == Ms(2200) && late.to == resolve);
        Gaze::Window const longLead = Gaze::LookWindow(created, resolve, Ms(5000));
        CHECK(longLead.from == created);            // never before the registration
        Ms const m{ 300 };
        CHECK(!Gaze::LookOwedAt(late, Ms(1800), m));
        CHECK(Gaze::LookOwedAt(late, Ms(1900), m)); // the margin opens it early
        CHECK(Gaze::LookOwedAt(late, Ms(3300), m));
        CHECK(!Gaze::LookOwedAt(late, Ms(3301), m));
        CHECK(Gaze::InLookRange(44.f, 40.f));
        CHECK(Gaze::InLookRange(45.f, 40.f));
        CHECK(!Gaze::InLookRange(45.1f, 40.f));
    }

    // ---- Ordering::roleLast (F7) ---------------------------------------------------------------------------------

    Assign::Claimant Bot(std::uint64_t guid, std::int64_t eta, bool last = false, bool inside = false)
    {
        Assign::Claimant c;
        c.guid = guid;
        c.bot = true;
        c.etaMs = eta;
        c.last = last;
        c.insideNow = inside;
        return c;
    }

    void TestRoleLast()
    {
        Assign::SoakRule rule;
        rule.count = 1;
        rule.deadlineMs = 2000;
        // The tank is nearest, yet a damage dealer that can make it takes the Vigil Flame.
        std::vector<std::uint64_t> a = Assign::SoakClaimers({ Bot(1, 100, true), Bot(2, 900), Bot(3, 1500) }, rule);
        CHECK(a.size() == 1 && a[0] == 2);
        // Without the role key the nearest (the tank) would.
        std::vector<std::uint64_t> b = Assign::SoakClaimers({ Bot(1, 100), Bot(2, 900), Bot(3, 1500) }, rule);
        CHECK(b.size() == 1 && b[0] == 1);
        // The tank already inside still does not take a slot another can.
        std::vector<std::uint64_t> c = Assign::SoakClaimers({ Bot(1, 0, true, true), Bot(2, 900) }, rule);
        CHECK(c.size() == 1 && c[0] == 2);
        // A late damage dealer does not take the slot from a tank who can make it (STAGE-R-1-FW M-1: unshared Vigil
        // Flame is lethal).
        std::vector<std::uint64_t> d = Assign::SoakClaimers({ Bot(1, 100, true), Bot(2, 9000) }, rule);
        CHECK(d.size() == 1 && d[0] == 1);
        // The same with the tank already inside.
        std::vector<std::uint64_t> d2 = Assign::SoakClaimers({ Bot(1, 0, true, true), Bot(2, 9000) }, rule);
        CHECK(d2.size() == 1 && d2[0] == 1);
        // Both late: the damage dealer first (best effort, the tank last).
        std::vector<std::uint64_t> d3 = Assign::SoakClaimers({ Bot(1, 5000, true), Bot(2, 9000) }, rule);
        CHECK(d3.size() == 1 && d3[0] == 2);
        // Count 2, one damage dealer late: the on-time tank first, then the late damage dealer.
        rule.count = 2;
        std::vector<std::uint64_t> d4 = Assign::SoakClaimers({ Bot(1, 100, true), Bot(2, 9000) }, rule);
        CHECK(d4.size() == 2 && d4[0] == 1 && d4[1] == 2);
        rule.count = 1;
        // Nobody else eligible (everyone Seared): the tank takes it.
        std::vector<std::uint64_t> e = Assign::SoakClaimers({ Bot(1, 100, true) }, rule);
        CHECK(e.size() == 1 && e[0] == 1);
        // A count the others cannot fill: the tank fills the rest.
        rule.count = 3;
        std::vector<std::uint64_t> f = Assign::SoakClaimers({ Bot(1, 100, true), Bot(2, 900), Bot(3, 1500) }, rule);
        CHECK(f.size() == 3 && f[0] == 2 && f[1] == 3 && f[2] == 1);
        // A human outside never claims, in either tier.
        Assign::Claimant human = Bot(4, 50);
        human.bot = false;
        rule.count = 1;
        std::vector<std::uint64_t> g = Assign::SoakClaimers({ Bot(1, 100, true), human }, rule);
        CHECK(g.size() == 1 && g[0] == 1);
        // The aura order still ranks within a tier.
        Assign::SoakRule ordered = rule;
        ordered.ordered = true;
        Assign::Claimant lo = Bot(5, 1500);
        lo.stacks = 0;
        Assign::Claimant hi = Bot(6, 100);
        hi.stacks = 3;
        std::vector<std::uint64_t> h = Assign::SoakClaimers({ Bot(1, 50, true), hi, lo }, ordered);
        CHECK(h.size() == 1 && h[0] == 5);
        // Exclusive groups pass the key through.
        std::vector<std::vector<std::uint64_t>> const x = Assign::ExclusiveSoakClaimers({
            { { Bot(1, 100, true), Bot(2, 900) }, rule }, { { Bot(1, 100, true), Bot(2, 900) }, rule } });
        CHECK(x.size() == 2 && x[0].size() == 1 && x[0][0] == 2 && x[1].size() == 1 && x[1][0] == 1);
    }

    // ---- EndBacksOff / TankNearSource (F2) -----------------------------------------------------------------------

    void TestBackOffSpot()
    {
        Vec3 const vishas{ 0.f, 0.f, 0.f };
        // The tank 3 yd east: its spot is 10 yd east, on the line.
        Vec3 const s = ViewRules::BackOffSpot(vishas, { 3.f, 0.f, 1.f }, 0.f, 10.f);
        CHECK_NEAR(s.x, 10.f, kEps);
        CHECK_NEAR(s.y, 0.f, kEps);
        CHECK_NEAR(s.z, 1.f, kEps);
        // Diagonal.
        Vec3 const d = ViewRules::BackOffSpot(vishas, { 2.f, 2.f, 0.f }, 0.f, 10.f);
        CHECK_NEAR(d.x, 10.f / std::sqrt(2.f), kEps);
        CHECK_NEAR(d.y, 10.f / std::sqrt(2.f), kEps);
        // Already farther: stays where it stands.
        Vec3 const far = ViewRules::BackOffSpot(vishas, { 25.f, 3.f, 0.f }, 0.f, 10.f);
        CHECK_NEAR(far.x, 25.f, kEps);
        CHECK_NEAR(far.y, 3.f, kEps);
        // On the source: along the line's facing.
        Vec3 const on = ViewRules::BackOffSpot(vishas, { 0.f, 0.f, 0.f }, Geo::kPi / 2.f, 10.f);
        CHECK_NEAR(on.x, 0.f, kEps);
        CHECK_NEAR(on.y, 10.f, kEps);
    }

    void TestBeamRegion()
    {
        Shape const line = Shape::Line(10.f, 3.f);
        Vec3 const src{ 0.f, 0.f, 0.f };
        // The claimers' region: to the end's spot less kBeamEndClearYd.
        ViewRules::BeamRegion const r = ViewRules::BeamRegionOf(line, src, { 10.f, 0.f, 0.f }, 0.f, false, 0.f);
        CHECK_NEAR(r.shape.length, 9.f, kEps);
        CHECK_NEAR(r.shape.width, 3.f, kEps);
        CHECK_NEAR(r.pose.o, 0.f, kEps);
        // A short way: never under kMinBeamRegionYd.
        ViewRules::BeamRegion const s = ViewRules::BeamRegionOf(line, src, { 1.f, 0.f, 0.f }, 0.f, false, 0.f);
        CHECK_NEAR(s.shape.length, ViewRules::kMinBeamRegionYd, kEps);
        // An end past the hitbox: the hitbox's length.
        ViewRules::BeamRegion const l = ViewRules::BeamRegionOf(line, src, { 0.f, 30.f, 0.f }, 0.f, false, 0.f);
        CHECK_NEAR(l.shape.length, 10.f, kEps);
        CHECK_NEAR(l.pose.o, Geo::kPi / 2.f, kEps);
        // The tank's share: the first tankNearYd.
        ViewRules::BeamRegion const t = ViewRules::BeamRegionOf(line, src, { 10.f, 0.f, 0.f }, 0.f, true, 4.f);
        CHECK_NEAR(t.shape.length, 4.f, kEps);
        ViewRules::BeamRegion const n = ViewRules::BeamRegionOf(line, src, { 10.f, 0.f, 0.f }, 0.f, false, 4.f);
        CHECK_NEAR(n.shape.length, 9.f, kEps);      // tankNearYd binds the tank only
    }

    Solver::Region RegionOf(std::uint32_t id, Shape const& shape, Pose const& pose, Ms from, Ms to)
    {
        Solver::Region r;
        r.id = id;
        r.shape = shape;
        r.poses = { pose };
        r.marginYd = ViewRules::RegionMarginYd(shape, kMargin);
        r.from = from;
        r.to = to;
        return r;
    }

    // Torment with the tank as the target: the tank (the `to` end, 3 yd from Vishas) walks back to its spot 10 yd
    // out on the line; three soakers on all sides walk into the line between them; with TankNearSource the tank
    // claimer (when it is not the target) soaks at Vishas's end. Every plan arrives before the channel (a 3 s warning:
    // the farthest soaker walks about 12 yd).
    void TestTormentSolve()
    {
        Vec3 const vishas{ 0.f, 0.f, 0.f };
        Ms const resolveAt = kNow + Ms(3000);     // a 3 s warning (D1 picks its own)
        Ms const until = resolveAt + Ms(8000);
        StraightOracle oracle;
        Shape line = Shape::Line(10.f, 3.f);        // EndBacksOff(10) made it 10 yd long

        // The tank: its spot 10 yd east.
        {
            Vec3 const tank{ 3.f, 0.5f, 0.f };
            Vec3 const spot = ViewRules::BackOffSpot(vishas, tank, 0.f, 10.f);
            Solver::Input in;
            in.now = kNow;
            in.bot = tank;
            Shape circle = Shape::Circle(ViewRules::kBackOffSpotRadiusYd);
            in.regions.push_back(RegionOf(1, circle, { spot.x, spot.y, spot.z, 0.f }, resolveAt, until));
            CHECK(!Solver::TargetOk(in, tank, kNow, kNow + in.params.horizon));
            Solver::Plan const plan = Solver::Solve(in, oracle);
            CHECK(plan.outcome == Solver::Outcome::Move);
            CHECK(Geo::Dist2d(plan.target.x, plan.target.y, vishas.x, vishas.y) > 8.5f);
            CHECK(plan.arriveAt <= resolveAt);
            CHECK(Solver::TargetOk(in, plan.target, plan.arriveAt, until));
        }
        // The soakers: from the west, north and far south-east, into the line before the tank's planned spot.
        Vec3 const end{ 10.f, 0.f, 0.f };
        for (Vec3 const bot : { Vec3{ -6.f, 4.f, 0.f }, Vec3{ 4.f, 12.f, 0.f }, Vec3{ 18.f, -9.f, 0.f } })
        {
            ViewRules::BeamRegion const r = ViewRules::BeamRegionOf(line, vishas, end, 0.f, false, 0.f);
            Solver::Input in;
            in.now = kNow;
            in.bot = bot;
            in.regions.push_back(RegionOf(2, r.shape, r.pose, resolveAt, until));
            Solver::Plan const plan = Solver::Solve(in, oracle);
            CHECK(plan.outcome == Solver::Outcome::Move);
            CHECK(plan.arriveAt <= resolveAt);
            Geo::LineCoords const c = Geo::ToLineCoords(line, { 0.f, 0.f, 0.f, 0.f }, plan.target);
            CHECK(c.s >= 0.f && c.s <= 9.f + kEps);     // between Vishas and the tank's place
            CHECK(std::fabs(c.t) <= 1.5f);
        }
        // TankNearSource(4): a tank claimer beside Vishas soaks within 4 yd of him.
        {
            Vec3 const tank{ 1.f, -3.f, 0.f };
            ViewRules::BeamRegion const r = ViewRules::BeamRegionOf(line, vishas, end, 0.f, true, 4.f);
            Solver::Input in;
            in.now = kNow;
            in.bot = tank;
            in.regions.push_back(RegionOf(3, r.shape, r.pose, resolveAt, until));
            Solver::Plan const plan = Solver::Solve(in, oracle);
            CHECK(plan.outcome == Solver::Outcome::Move);
            CHECK(plan.target.x >= -kEps && plan.target.x <= 4.f + kEps);
            CHECK(std::fabs(plan.target.y) <= 1.5f);
        }
    }

    // ---- facing hold (F6) and the lasting Spread (F15) ------------------------------------------------------------

    void TestFacingHold()
    {
        CHECK(ViewRules::RefaceAllowed(false, false));
        CHECK(!ViewRules::RefaceAllowed(true, false));      // a warning: hold (decision §7.5)
        CHECK(!ViewRules::RefaceAllowed(false, true));      // a facing hold names the source
        CHECK(!ViewRules::RefaceAllowed(true, true));
        // Thorne: the Sow spot 5 yd ahead of him is inside his remembered Prune cone (the hold's reason).
        std::vector<Shape> prune;
        Shape cone = Shape::Cone(120.f, 8.f);
        CHECK(ViewRules::RememberFrontShape(prune, cone));
        Vec3 const thorne{ 0.f, 0.f, 0.f };
        Vec3 const tank{ -3.f, 0.f, 0.f };                  // the tank west, so he faces west
        Vec3 const furrow{ -5.f, 0.f, 0.f };
        CHECK(ViewRules::FrontalHits(prune, thorne, tank, { furrow }, kMargin, 15.f));
    }

    void TestCarrierPasses()
    {
        // A one-shot keeps its old ids (the carrier's index); a lasting one never repeats an id across ticks.
        CHECK(Judge::CarrierPass(0, 0) == 0);
        CHECK(Judge::CarrierPass(0, 4) == 4);
        std::set<std::uint32_t> seen;
        for (std::uint32_t tick = 0; tick < 12; ++tick)
            for (std::uint32_t c = 0; c < 40; ++c)
                CHECK(seen.insert(Judge::CarrierPass(tick, c)).second);
        // The bots' view of a lasting spread: never "no warning, no job" (it lasts), and a carrier circle held for
        // its whole window keeps an ally out at every point of it.
        CHECK(!ViewRules::Undodgeable(kNow, kNow, true));
        CHECK(ViewRules::Undodgeable(kNow, kNow, false));
        Solver::Timing const window = Solver::MakeInterval(kNow + Ms(1000), kNow + Ms(11000));
        ViewRules::Spot carrier;
        carrier.p = { 0.f, 0.f, 0.f };
        carrier.source = ViewRules::SpotSource::Reservation;
        Solver::Input in;
        in.now = kNow;
        in.bot = { 2.f, 0.f, 0.f };
        in.hazards.push_back(ViewRules::CarrierHazard(7, 6.f, 10.f, carrier, kMargin, window));
        CHECK(!Solver::TargetOk(in, in.bot, kNow, kNow + Ms(10000)));
        CHECK(!Solver::TargetOk(in, in.bot, kNow + Ms(9000), kNow + Ms(9500)));     // late in the window too
        CHECK(Solver::TargetOk(in, in.bot, kNow + Ms(11500), kNow + Ms(12000)));    // over
        CHECK(Solver::TargetOk(in, { 7.5f, 0.f, 0.f }, kNow, kNow + Ms(10000)));    // 6 yd + margin out
    }

    // ---- F-4a ----------------------------------------------------------------------------------------------------

    // Ironspine's Glare (D2 IR6, Gaze/9 on melee): a strand route asked at 63.5 s with the window opening at 64 s
    // walked through it. The bot's own walk waits while a window is owed or opens within the lead, and one walking
    // stops once a window is owed; outside the windows it goes.
    void TestOwnWalk()
    {
        Ms const m{ 300 };
        std::vector<Gaze::Window> const glares{ Gaze::LookWindow(Ms(64000), Ms(66000), Ms(0)),
            Gaze::LookWindow(Ms(70000), Ms(72000), Ms(0)) };
        using Gaze::OwnWalk;
        CHECK(Gaze::OwnWalkAt({}, Ms(65000), m, false) == OwnWalk::Go);
        CHECK(Gaze::OwnWalkAt({}, Ms(65000), m, true) == OwnWalk::Go);
        // Well before: go. Within the lead (window - margin - 1000 ms): wait, but a walk already going walks on.
        CHECK(Gaze::OwnWalkAt(glares, Ms(62000), m, false) == OwnWalk::Go);
        CHECK(Gaze::OwnWalkAt(glares, Ms(62699), m, false) == OwnWalk::Go);
        CHECK(Gaze::OwnWalkAt(glares, Ms(62700), m, false) == OwnWalk::Defer);
        CHECK(Gaze::OwnWalkAt(glares, Ms(63500), m, false) == OwnWalk::Defer);   // the sweep's strand at 63.5 s
        CHECK(Gaze::OwnWalkAt(glares, Ms(63500), m, true) == OwnWalk::Go);
        // Owed (the margin opens it early, closes it late): a start waits, a walk stops.
        CHECK(Gaze::OwnWalkAt(glares, Ms(63700), m, true) == OwnWalk::Stop);
        CHECK(Gaze::OwnWalkAt(glares, Ms(65000), m, false) == OwnWalk::Defer);
        CHECK(Gaze::OwnWalkAt(glares, Ms(65000), m, true) == OwnWalk::Stop);
        CHECK(Gaze::OwnWalkAt(glares, Ms(66300), m, true) == OwnWalk::Stop);
        // Between the glares: free again until the next one's lead.
        CHECK(Gaze::OwnWalkAt(glares, Ms(66301), m, true) == OwnWalk::Go);
        CHECK(Gaze::OwnWalkAt(glares, Ms(67000), m, false) == OwnWalk::Go);
        CHECK(Gaze::OwnWalkAt(glares, Ms(68700), m, false) == OwnWalk::Defer);
        CHECK(Gaze::OwnWalkAt(glares, Ms(71000), m, true) == OwnWalk::Stop);
        CHECK(Gaze::OwnWalkAt(glares, Ms(73000), m, false) == OwnWalk::Go);
        // Against the Looker hold (TaskHoldUntil): every step the hold counts, the own walk is stopped or waits.
        for (std::int64_t t = 60000; t <= 75000; t += 50)
        {
            bool owed = false;
            for (Gaze::Window const& w : glares)
                owed = owed || Gaze::LookOwedAt(w, Ms(t), m);
            if (owed)
            {
                CHECK(Gaze::OwnWalkAt(glares, Ms(t), m, true) == OwnWalk::Stop);
                CHECK(Gaze::OwnWalkAt(glares, Ms(t), m, false) == OwnWalk::Defer);
            }
        }
    }

    // Halden's Close Ranks (D10 MA4, Zone/27 on tanks): `donut 11 inner=5 anchor=target`. The tank's melee ring
    // (melee range - 0.5: 4.5 yd for a warrior, more for a bear) lies in the band plus its margin, so the chase was
    // refused with no way in. ReachSafe finds a point inside the inner edge, on the bot's side, within melee reach.
    void TestReachSafe()
    {
        Solver::Input in;
        in.now = kNow;
        Solver::Hazard band;
        band.id = 27;
        band.shape = Shape::Donut(5.f, 11.f);
        band.poses = { Pose{ 0.f, 0.f, 0.f, 0.f } };
        band.marginYd = kMargin;
        band.timing = Solver::MakeInterval(kNow, kNow + Ms(8000));
        in.hazards.push_back(band);
        Ms const to = kNow + in.params.horizon;
        Vec3 const boss{};
        for (float ring : { 4.5f, 5.5f, 6.8f })    // Prot Warrior, a bigger reach, Bear form
        {
            Vec3 const tank{ ring + 3.f, 1.f, 0.f };     // left behind as the boss stepped 4 yd away
            CHECK(!Landing::RingSafe(in, boss, tank, ring, kNow, to));      // the chase as refused before
            Vec3 safe{ 99.f, 99.f, 0.f };
            CHECK(Landing::ReachSafe(in, boss, tank, ring, kNow, to, &safe));
            float const r = std::sqrt(safe.x * safe.x + safe.y * safe.y);
            CHECK(r <= ring && r >= Landing::kReachMinYd - 0.001f);         // within melee reach
            CHECK(r <= 5.f - kMargin);                                      // clear of the band and its margin
            CHECK(Solver::PointSafe(in, safe, kNow, to));
            CHECK(safe.x > 0.f);                                            // on the bot's side
            // A step deeper than the outermost safe ring (the boss keeps walking, the band with it).
            CHECK(r <= 5.f - kMargin - Landing::kReachInsetYd + Landing::kReachStepYd + 0.001f);
        }
        // Nothing within reach: a circle on the boss covering the whole disk.
        Solver::Input full = in;
        full.hazards[0].shape = Shape::Circle(8.f);
        Vec3 safe{};
        CHECK(!Landing::ReachSafe(full, boss, Vec3{ 9.f, 0.f, 0.f }, 4.f, kNow, to, &safe));
        // A clear ring: its own point, nothing deeper (the strand route's job, unchanged).
        Solver::Input clear;
        clear.now = kNow;
        Vec3 const bot{ 9.f, 0.f, 0.f };
        CHECK(Landing::ReachSafe(clear, boss, bot, 4.f, kNow, to, &safe));
        Vec3 const first = Landing::RingPoint(boss, bot, 4.f, 0);
        CHECK(Geo::Dist2d(safe.x, safe.y, first.x, first.y) < 0.001f);
        // A bad radius finds nothing (no endless loop).
        CHECK(!Landing::ReachSafe(in, boss, bot, std::nanf(""), kNow, to, &safe));
        CHECK(!Landing::ReachSafe(full, boss, bot, 0.2f, kNow, to, &safe));
        // After the band: the ring is clear again.
        CHECK(Landing::RingSafe(in, boss, bot, 4.5f, kNow + Ms(9000), kNow + Ms(12000)));
    }

    // Feral Charge - Cat (49376: JUMP_DEST at TARGET_DEST_TARGET_BACK) beside Ironspine's rot: the boss dragged off
    // the rot faces his tank, so his back points into it. The landing test now probes the boss's back (where the cat
    // lands) and refuses the leap; the old center probe allowed it.
    void TestFeralChargeLanding()
    {
        Solver::Input in;
        in.now = kNow;
        Solver::Hazard rot;
        rot.id = 7;
        rot.shape = Shape::Circle(7.f);
        rot.poses = { Pose{ -9.f, 0.f, 0.f, 0.f } };        // the rot left behind; the boss stands at 0, 2 yd off it
        rot.marginYd = kMargin;
        rot.timing = Solver::MakeInterval(kNow, kNow + Ms(20000));
        in.hazards.push_back(rot);
        Ms const to = kNow + Ms(1000);
        Landing::Dest boss;
        boss.combatReachYd = 1.5f;
        boss.facing = 0.f;                                  // toward the tank at +x; his back toward the rot
        Vec3 const cat{ 4.f, 12.f, 0.f };                   // the Feral, out of melee on the side
        using LockRules::LandingSpot;
        LandingSpot const spot = LockRules::SpellEffectSpot(false, false, LockRules::JumpLandsBehind(65, 0));
        CHECK(spot == LandingSpot::Behind);
        CHECK(!Landing::LandingUnsafe(in, LandingSpot::Center, boss, cat, kNow, to));   // as built: allowed
        CHECK(Landing::LandingUnsafe(in, spot, boss, cat, kNow, to));                   // now: refused
        Vec3 const lands = Landing::LandingPoint(spot, boss, cat);
        CHECK(lands.x < -2.f && std::fabs(lands.y) < 0.001f);
        // Turned the other way (his back away from the rot) the leap is allowed.
        boss.facing = Geo::kPi;
        CHECK(!Landing::LandingUnsafe(in, spot, boss, cat, kNow, to));
    }
}

int main()
{
    TestGazeFaces();
    TestGazeFilter();
    TestGazeChooseFacing();
    TestGazeRandom();
    TestGazeWindow();
    TestRoleLast();
    TestBackOffSpot();
    TestBeamRegion();
    TestTormentSolve();
    TestFacingHold();
    TestCarrierPasses();
    TestOwnWalk();
    TestReachSafe();
    TestFeralChargeLanding();
    return Report("sm_shapes_test");
}
