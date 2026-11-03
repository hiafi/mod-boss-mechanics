/*
 * mod-boss-mechanics: the per-bot brain of the boss-mechanics strategy (STAGE-M2-HANDOFF §3.4, WP-C).
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotBrain.h"
#include "AiObjectContext.h"
#include "BossMechanicsConfig.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MechanicBotView.h"
#include "MechanicRegistry.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>

namespace BossMechanics
{
    namespace
    {
        constexpr char const* kLog = "module.bossmechanics.bots";

        // Mode not Idle: the view is rebuilt at least this often (§3.4 step 2).
        constexpr Milliseconds kViewRefresh{ 200 };
        // Idle on a map whose registry holds mechanics: rebuild this often, so a hazard the view left out for distance
        // comes back as the bot walks towards it (the view culls at run speed x horizon).
        constexpr Milliseconds kIdleViewRefresh{ 1000 };
        // Escape: the target counts as reached within this (§3.4 step 4).
        constexpr float kArriveYd = 0.6f;
        // Escape: a stuck re-solve that lands within this of the old target found nothing new (the move action's
        // "same target" tolerance too).
        constexpr float kSameTargetYd = 1.f;
        // Escape: the bot counts as stuck when it moved less than kStuckYd in kStuckAfter. The clock starts when the
        // escape begins and restarts on progress only, so re-sends and refused sends cannot hold it back.
        constexpr Milliseconds kStuckAfter{ 500 };
        constexpr float kStuckYd = 0.5f;
        // Escape ceilings: one escape is given up at 2 x its planned travel + kEscapeSlack; an unbroken run of escapes
        // (re-solve after re-solve, never arriving) at kEscapeChainCap. Then the bot is released for kReleaseFor.
        constexpr Milliseconds kEscapeSlack{ 1500 };
        constexpr Milliseconds kEscapeChainCap{ 10000 };
        constexpr Milliseconds kReleaseFor{ 1500 };
        // A hold that stops foreign motion lasts at least this (then stock movement may try again).
        constexpr Milliseconds kMinMotionHold{ 250 };
        // Re-route of an unsafe foreign motion: role band [0, this] around the motion's end; the target must bring
        // the bot at least Solver::Params::rerouteGainYd (1 yd) closer to that end (the solver only offers such
        // targets), and the motion must still have kRerouteMinYd to go.
        constexpr float kRerouteBandYd = 3.f;
        constexpr float kRerouteMinYd = 2.f;
        // Relevance (STAGE-M2-REVIEW m6): a hazard matters to a bot that could reach its region (bounds + margin)
        // within the time to its first window plus kRelevanceLead, plus kRelevanceSlackYd. Open and endless hazards
        // thus keep only the bots near them in Watch, not every bot within the view's 75 yd cull.
        constexpr Milliseconds kRelevanceLead{ 2000 };
        constexpr float kRelevanceSlackYd = 3.f;
        // Solves per map update across all bots of a map (STAGE-M2-REVIEW m6); the rest wait one update, keeping
        // their solveBypass, and the lock treats them as escaping meanwhile.
        constexpr uint32 kMaxSolvesPerMapUpdate = 8;
        // Role band (M2's minimal version, §3.4): melee reach slack, ranged band [8, RangedDistanceYd + 6], follow
        // band [0, FollowDistance + 2].
        constexpr float kMeleeSlackYd = 0.5f;
        constexpr float kRangedMinYd = 8.f;
        constexpr float kRangedExtraYd = 6.f;
        constexpr float kFollowExtraYd = 2.f;

        Geo::Vec3 PosOf(WorldObject const* o)
        {
            return { o->GetPositionX(), o->GetPositionY(), o->GetPositionZ() };
        }

        float Dist2d(Geo::Vec3 const& a, Geo::Vec3 const& b)
        {
            return Geo::Dist2d(a.x, a.y, b.x, b.y);
        }

        Fallibility::Config FallibilityConfigOf(BotFallibilityConfig const& f)
        {
            Fallibility::Config c;
            c.enable = f.enable;
            c.reactionMs = f.reactionMs;
            c.jitterMs = f.jitterMs;
            c.lateChancePct = f.lateChancePct;
            c.lateExtraMs = f.lateExtraMs;
            c.wrongSideChancePct = f.wrongSideChancePct;
            c.realiseMs = f.realiseMs;
            c.scaleNormal = f.scaleNormal;
            c.scaleHeroic = f.scaleHeroic;
            c.seed = f.effectiveSeed;
            return c;
        }

        bool SameMap(Player const* a, Player const* b)
        {
            return a && b && a->IsInWorld() && b->IsInWorld() && a->GetMap() == b->GetMap();
        }

        // Healers anchor on the nearest group tank (§3.4 role band).
        Player* NearestGroupTank(Player* bot)
        {
            Group* group = bot->GetGroup();
            if (!group)
                return nullptr;
            Player* best = nullptr;
            float bestDist = 0.f;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !SameMap(bot, member) || !member->IsAlive())
                    continue;
                if (!PlayerbotAI::IsTank(member))
                    continue;
                float const d = bot->GetExactDist2d(member);
                if (!best || d < bestDist)
                {
                    best = member;
                    bestDist = d;
                }
            }
            return best;
        }

        // M2's minimal role band (§3.4); M3 replaces it.
        Solver::RoleBand RoleBandOf(PlayerbotAI* botAI, Player* bot, BotConfig const& cfg)
        {
            Solver::RoleBand band;
            if (bot->IsInCombat())
            {
                Unit* anchor = bot->GetVictim();
                bool const healer = PlayerbotAI::IsHeal(bot);
                if (healer)
                    if (Player* tank = NearestGroupTank(bot))
                        anchor = tank;
                if (!anchor || !anchor->IsInWorld() || anchor->GetMap() != bot->GetMap())
                    return band;
                band.present = true;
                band.anchor = PosOf(anchor);
                if (healer || (PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsTank(bot)))
                {
                    band.minYd = kRangedMinYd;
                    band.maxYd = cfg.rangedDistanceYd + kRangedExtraYd;
                }
                else
                {
                    band.minYd = 0.f;
                    band.maxYd = std::max(0.f, bot->GetMeleeRange(anchor) - kMeleeSlackYd);
                }
                return band;
            }

            Player* anchor = botAI->GetMaster();
            if (anchor == bot || !SameMap(bot, anchor))
                anchor = botAI->GetGroupLeader();
            if (!anchor || anchor == bot || !SameMap(bot, anchor))
                return band;
            band.present = true;
            band.anchor = PosOf(anchor);
            band.minYd = 0.f;
            band.maxYd = sPlayerbotAIConfig.FollowDistance + kFollowExtraYd;
            return band;
        }

        // Other group bots' targets while they escape or hold (§3.5 crowd). Same map, so same thread.
        void FillCrowd(Player* bot, std::vector<Geo::Vec3>& out)
        {
            out.clear();
            Group* group = bot->GetGroup();
            if (!group)
                return;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !SameMap(bot, member))
                    continue;
                PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
                if (!ai)
                    continue;
                MechanicBotState const* other = BotBrain::StateOf(ai);
                if (other && (other->mode == MechanicBotMode::Escape || other->mode == MechanicBotMode::Hold))
                    out.push_back(other->plan.target);
            }
        }

        // Rebuilds the view (§3.5) with the fallibility filter (§3.8) when it is enabled.
        void RebuildView(Player* bot, MechanicBotState& st, Registry const& reg, Milliseconds now,
            BotConfig const& cfg)
        {
            uint32 const revision = reg.Revision();
            if (revision != st.seenRevision || !st.viewValid)
            {
                // A new revision: drop what refers to mechanics that are gone, and let the next solve skip the rate
                // limit (§3.4 step 5).
                std::erase_if(st.perception, [&reg](auto const& kv) { return reg.Find(kv.first) == nullptr; });
                std::erase_if(st.loggedFallback, [&reg](uint32 id) { return reg.Find(id) == nullptr; });
                st.solveBypass = true;
            }
            if (now >= st.nextPerceptionAt)
                st.solveBypass = true;      // a hidden mechanic became visible: react at once

            BotViewOptions const opt = BotViewOptions::FromConfig(cfg);
            Fallibility::Config const fc = FallibilityConfigOf(cfg.fallibility);
            float const scale = Fallibility::ScaleFor(fc, bot->GetMap()->IsHeroic());
            BotViewInfo info;
            if (scale > 0.f)
            {
                uint64 const botKey = bot->GetGUID().GetRawValue();
                PerceptionFilter const filter = [&](uint32 id, Milliseconds& until)
                {
                    auto it = st.perception.find(id);
                    if (it == st.perception.end())
                    {
                        ActiveMechanic const* m = reg.Find(id);
                        if (!m)
                            return true;
                        it = st.perception.emplace(id, Fallibility::Perceive(fc, scale, botKey, id, m->createdAt,
                            m->resolveAt)).first;
                    }
                    Milliseconds seenFrom{ 0 };
                    if (Fallibility::Sees(it->second, now, &seenFrom))
                        return true;
                    until = seenFrom;
                    return false;
                };
                info = BuildInput(reg, bot, now, opt, st.input, &filter);
            }
            else
            {
                st.perception.clear();
                info = BuildInput(reg, bot, now, opt, st.input);
            }
            FillCrowd(bot, st.input.crowd);

            st.seenRevision = info.revision;
            st.movingAnchors = info.movingAnchors;
            st.nextPerceptionAt = info.nextPerceptionAt;
            st.lastViewAt = now;
            st.viewValid = true;
            st.viewRebuilt = true;
        }

        // Keeps the view current: a rebuild when it is stale (§3.4 step 2), else only the bot's own pose and the time.
        void RefreshView(Player* bot, MechanicBotState& st, Registry const& reg, Milliseconds now, BotConfig const& cfg)
        {
            st.viewRebuilt = false;
            bool const idle = st.mode == MechanicBotMode::Idle;
            bool const stale = !st.viewValid
                || reg.Revision() != st.seenRevision
                || now >= st.nextPerceptionAt
                || (!idle && now - st.lastViewAt >= kViewRefresh)
                || (st.movingAnchors && now - st.lastViewAt >= Milliseconds(cfg.movingAnchorRecheckMs))
                || (idle && reg.Size() && now - st.lastViewAt >= kIdleViewRefresh);
            if (stale)
            {
                RebuildView(bot, st, reg, now, cfg);
                return;
            }
            st.input.bot = PosOf(bot);
            st.input.speed = bot->GetSpeed(MOVE_RUN);
            st.input.now = now;
        }

        // A hazard whose windows meet [now, now + horizon] and whose region the bot could reach before its first
        // window (+ kRelevanceLead, + kRelevanceSlackYd).
        bool AnyRelevant(Solver::Input const& in)
        {
            Milliseconds const end = in.now + in.params.horizon;
            float const speed = std::max(0.1f, in.speed);
            for (Solver::Hazard const& h : in.hazards)
            {
                if (h.timing.from > end || h.timing.to + in.params.marginMs < in.now)
                    continue;
                Milliseconds const lead = std::max(Milliseconds(0), h.timing.from - in.now) + kRelevanceLead;
                float const reach = Geo::Bounds(h.shape) + h.marginYd + kRelevanceSlackYd
                    + speed * static_cast<float>(lead.count()) / 1000.f;
                for (Geo::Pose const& pose : h.poses)
                    if (Geo::Dist2d(in.bot.x, in.bot.y, pose.x, pose.y) <= reach)
                        return true;
            }
            return false;
        }

        Solver::Hazard const* FindHazard(Solver::Input const& in, uint32 id)
        {
            for (Solver::Hazard const& h : in.hazards)
                if (h.id == id)
                    return &h;
            return nullptr;
        }

        // The first window start (not yet over) of the hazards holding p: an escape that arrives later is late.
        Milliseconds FirstThreatAt(Solver::Input const& in, Geo::Vec3 const& p)
        {
            Milliseconds first = Solver::kForever;
            for (Solver::Hazard const& h : in.hazards)
            {
                bool inside = false;
                for (Geo::Pose const& pose : h.poses)
                    inside = inside || Solver::SignedDistance(h.shape, pose, p) <= h.marginYd;
                if (!inside)
                    continue;
                for (HotWindow const& w : Solver::WindowsAt(h, p))
                    if (w.to >= in.now)
                        first = std::min(first, w.from);
            }
            return first;
        }

        // The bot's spline still to walk: its position, then the spline points after the current segment.
        bool RemainingSpline(Player const* bot, std::vector<Geo::Vec3>& out)
        {
            out.clear();
            Movement::MoveSpline const* ms = bot->movespline;
            if (!ms || !ms->Initialized() || ms->Finalized() || ms->isCyclic())
                return false;
            auto const& spline = ms->_Spline();
            out.push_back(PosOf(bot));
            for (int32 i = ms->_currentSplineIdx() + 1; i <= spline.last(); ++i)
            {
                G3D::Vector3 const& p = spline.getPoint(i);
                out.push_back({ p.x, p.y, p.z });
            }
            return out.size() > 1;
        }

        // The rest of an escape path from where the bot is now: its position, then the corners after the segment
        // nearest to it.
        std::vector<Geo::Vec3> RemainingPath(std::vector<Geo::Vec3> const& path, Geo::Vec3 const& pos)
        {
            std::vector<Geo::Vec3> out{ pos };
            if (path.size() < 2)
            {
                if (!path.empty())
                    out.push_back(path.back());
                return out;
            }
            std::size_t bestSeg = 0;
            float bestDist = std::numeric_limits<float>::max();
            for (std::size_t i = 0; i + 1 < path.size(); ++i)
            {
                Geo::Vec3 const& a = path[i];
                Geo::Vec3 const& b = path[i + 1];
                float const dx = b.x - a.x;
                float const dy = b.y - a.y;
                float const len2 = dx * dx + dy * dy;
                float t = len2 > 0.f ? ((pos.x - a.x) * dx + (pos.y - a.y) * dy) / len2 : 0.f;
                t = std::clamp(t, 0.f, 1.f);
                float const d = Geo::Dist2d(a.x + dx * t, a.y + dy * t, pos.x, pos.y);
                if (d < bestDist)
                {
                    bestDist = d;
                    bestSeg = i;
                }
            }
            for (std::size_t i = bestSeg + 1; i < path.size(); ++i)
                out.push_back(path[i]);
            return out;
        }

        // CheckPath for a motion already under way: no start delay.
        Solver::PathCheck CheckMotion(Solver::Input& in, std::vector<Geo::Vec3> const& path)
        {
            Milliseconds const startDelay = in.params.startDelay;
            in.params.startDelay = Milliseconds(0);
            Solver::PathCheck const pc = Solver::CheckPath(in, path, in.now, false);
            in.params.startDelay = startDelay;
            return pc;
        }

        // Per map update: how many solves the bots of this thread's map took. Bots solve on their map's update
        // thread and GameTime does not move during a map update, so (map id, instance id, now) identifies one
        // update. Ids, not a Map pointer, so nothing dangles across ticks.
        struct SolveBudget
        {
            uint32 mapId = 0;
            uint32 instanceId = 0;
            Milliseconds at{ -1 };
            uint32 used = 0;
        };
        thread_local SolveBudget tSolveBudget;

        // May the bot solve now? Rate limit per bot (MinSolveIntervalMs, unless solveBypass), then the per-map budget,
        // which a true answer consumes (every true answer is followed by a solve).
        bool CanSolve(Player const* bot, MechanicBotState const& st, Milliseconds now, BotConfig const& cfg)
        {
            bool const rateOk = st.solveBypass || st.lastSolveAt == Milliseconds(0)
                || now - st.lastSolveAt >= Milliseconds(cfg.minSolveIntervalMs);
            if (!rateOk)
                return false;
            SolveBudget& b = tSolveBudget;
            uint32 const mapId = bot->GetMapId();
            uint32 const instanceId = bot->GetInstanceId();
            if (b.mapId != mapId || b.instanceId != instanceId || b.at != now)
            {
                b.mapId = mapId;
                b.instanceId = instanceId;
                b.at = now;
                b.used = 0;
            }
            if (b.used >= kMaxSolvesPerMapUpdate)
                return false;
            ++b.used;
            return true;
        }

        char const* OutcomeName(Solver::Outcome o)
        {
            switch (o)
            {
                case Solver::Outcome::Safe:
                    return "safe";
                case Solver::Outcome::Move:
                    return "move";
                case Solver::Outcome::GraceMove:
                    return "grace-move";
                case Solver::Outcome::Fallback:
                    return "fallback";
                case Solver::Outcome::NoPath:
                    return "no-path";
            }
            return "?";
        }

        char const* PassName(uint8 pass)
        {
            switch (pass)
            {
                case 1:
                    return "clean";
                case 2:
                    return "reduced margin";
                case 3:
                    return "grace";
                default:
                    return "-";
            }
        }

        std::string IdList(std::vector<uint32> const& ids)
        {
            std::string s;
            for (uint32 id : ids)
                s += (s.empty() ? "" : ",") + std::to_string(id);
            return s.empty() ? "-" : s;
        }

        void ToIdle(MechanicBotState& st)
        {
            st.mode = MechanicBotMode::Idle;
            st.plan = Solver::Plan();
            st.planFor.clear();
            st.leaveBy = Solver::kForever;
            st.outOfRoleSince = Milliseconds(0);
            st.escapeIssuedAt = Milliseconds(0);
            st.pendingEscape = false;
        }

        // Gives the bot back to the stock AI for `duration` (STAGE-M2-REVIEW M1): mode Watch, no plan, nothing
        // locked, no cast break; an unsafe spot is not solved again until releaseUntil (or, forRoot, until the bot can
        // move again).
        void Release(Player const* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg, bool forRoot,
            char const* why)
        {
            st.mode = MechanicBotMode::Watch;
            st.plan = Solver::Plan();
            st.planFor.clear();
            st.outOfRoleSince = Milliseconds(0);
            st.escapeIssuedAt = Milliseconds(0);
            st.pendingEscape = false;
            st.releaseUntil = now + kReleaseFor;
            st.releaseForRoot = forRoot;
            ++st.counters.releases;
            if (cfg.debug)
                LOG_INFO(kLog, "Bot {}: escape released ({}) for {} ms", bot->GetName(), why, kReleaseFor.count());
        }

        // Hold where the bot stands (its foreign motion was unsafe): until the hazard it would have crossed is over,
        // at least kMinMotionHold, at most HoldCapMs.
        // shortOnly: only kMinMotionHold (a re-route was merely deferred by the solve rate limit or budget).
        void HoldHere(MechanicBotState& st, Milliseconds now, BotConfig const& cfg, uint32 hazardId,
            bool shortOnly = false)
        {
            Solver::Input const& in = st.input;
            Milliseconds until = now + Milliseconds(cfg.holdCapMs);
            if (Solver::Hazard const* h = FindHazard(in, hazardId))
                if (h->timing.to < Solver::kForever)
                    until = std::min(until, std::max(now + kMinMotionHold, h->timing.to + in.params.marginMs));
            if (shortOnly)
                until = std::min(until, now + kMinMotionHold);
            st.plan = Solver::Plan();
            st.plan.target = in.bot;
            st.plan.path = { in.bot };
            st.plan.arriveAt = now;
            st.plan.holdUntil = until;
            if (hazardId)
                st.plan.answers = { hazardId };
            st.planFor = st.plan.answers;
            st.mode = MechanicBotMode::Hold;
            st.outOfRoleSince = Milliseconds(0);
            ++st.counters.motionStops;
        }

        // Solve (§3.9) with the telemetry and the once-per-mechanic Fallback log.
        Solver::Plan RunSolve(Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg)
        {
            // Debug = 1 only: the oracle counts why its probes and path calls failed (nothing is counted otherwise).
            NavOracleStats nav;
            NavOracle oracle(bot, cfg.debug ? &nav : nullptr);
            auto const t0 = std::chrono::steady_clock::now();
            Solver::Plan plan = Solver::Solve(st.input, oracle);
            auto const us = static_cast<uint32>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());

            MechanicBotCounters& c = st.counters;
            ++c.solves;
            c.pathChecks += plan.pathChecks;
            ++c.outcomes[static_cast<std::size_t>(plan.outcome)];
            if (plan.outcome == Solver::Outcome::Move && plan.stats.pass == 2)
                ++c.tightMoves;
            c.solveUsLast = us;
            c.solveUsMax = std::max(c.solveUsMax, us);
            c.solveUsTotal += us;
            st.lastSolveAt = now;
            st.solveBypass = false;

            Geo::Vec3 const from = st.input.bot;
            if (cfg.debug)
            {
                c.debugSolve.Add(plan.stats);
                c.debugNav.Add(nav);
                LOG_INFO(kLog, "Bot {}: {} -> {} at ({:.1f}, {:.1f}, {:.1f}), {:.1f} yd, answers {}, {} candidates, "
                    "{} path checks, {} us; pass {}, {}; nav {}", bot->GetName(), st.input.reroute ? "reroute"
                    : "solve", OutcomeName(plan.outcome), plan.target.x, plan.target.y, plan.target.z,
                    Dist2d(from, plan.target), IdList(plan.answers), plan.candidates, plan.pathChecks, us,
                    PassName(plan.stats.pass), FormatSolveStats(plan.stats), FormatNavStats(nav));
            }
            if (plan.outcome == Solver::Outcome::Fallback || plan.outcome == Solver::Outcome::NoPath)
            {
                // Once per bot and mechanic (PLAN §7 navmesh gaps). Debug level: a recurring infeasible mechanic
                // (a boss-anchored zone on the chased tank) would otherwise warn per bot per cast; `.mech bots` counts
                // them, and Debug = 1 logs them at INFO.
                uint32 first = 0;
                Solver::PointSafe(st.input, from, now, now + st.input.params.horizon, &first);
                if (first && st.loggedFallback.insert(first).second)
                {
                    if (cfg.debug)
                        LOG_INFO(kLog, "Bot {} on map {}: no clean escape from mechanic {} at ({:.1f}, {:.1f}, "
                            "{:.1f}): {} to ({:.1f}, {:.1f}, {:.1f})", bot->GetName(), bot->GetMapId(), first, from.x,
                            from.y, from.z, OutcomeName(plan.outcome), plan.target.x, plan.target.y, plan.target.z);
                    else
                        LOG_DEBUG(kLog, "Bot {} on map {}: no clean escape from mechanic {} at ({:.1f}, {:.1f}, "
                            "{:.1f}): {} to ({:.1f}, {:.1f}, {:.1f})", bot->GetName(), bot->GetMapId(), first, from.x,
                            from.y, from.z, OutcomeName(plan.outcome), plan.target.x, plan.target.y, plan.target.z);
                }
            }
            return plan;
        }

        // Takes a moving plan: mode Escape, a fresh stuck clock and the two ceilings.
        void BeginEscape(MechanicBotState& st, Solver::Plan&& plan, Milliseconds now)
        {
            Geo::Vec3 const from = st.input.bot;
            if (st.mode != MechanicBotMode::Escape)
                st.escapeChainSince = now;
            Milliseconds const travel = std::max(Milliseconds(0), plan.arriveAt - now);
            st.escapeCeilingAt = now + 2 * travel + kEscapeSlack;
            st.escapeDeadline = FirstThreatAt(st.input, from);
            st.planReduced = plan.stats.pass == 2;
            st.plan = std::move(plan);
            st.planFor = st.plan.answers;
            st.mode = MechanicBotMode::Escape;
            st.escapeIssuedAt = Milliseconds(0);
            st.escapeSentAt = Milliseconds(0);
            st.stuckRef = from;
            st.stuckSince = now;
            st.outOfRoleSince = Milliseconds(0);
            st.pendingEscape = false;
        }

        // Solve and take the plan: Move/GraceMove/Fallback/NoPath -> Escape; Safe -> Watch.
        void SolveAndApply(Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg)
        {
            Solver::Plan plan = RunSolve(bot, st, now, cfg);
            if (plan.outcome == Solver::Outcome::Safe)
            {
                st.mode = MechanicBotMode::Watch;
                st.pendingEscape = false;
                return;
            }
            BeginEscape(st, std::move(plan), now);
        }

        // The bot's foreign motion crosses a hazard (STAGE-M2-REVIEW m2): walk around it towards the motion's end,
        // by a validated path, when that end is safe and the solver finds a clean target closer to it. The bot then
        // holds nothing (holdUntil = arrival): Watch takes the stock motion back, and re-routes again if it still
        // crosses. Returns false when the caller should hold instead (deferred is set when only the solve rate limit
        // or budget said no).
        bool TryReroute(Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg,
            std::vector<Geo::Vec3> const& motion, bool& deferred)
        {
            deferred = false;
            Solver::Input& in = st.input;
            Geo::Vec3 const end = motion.back();
            float const toGo = Dist2d(in.bot, end);
            if (toGo < kRerouteMinYd || !Solver::PointSafe(in, end, now, now + in.params.horizon))
                return false;
            if (!CanSolve(bot, st, now, cfg))
            {
                deferred = true;
                return false;
            }
            Solver::RoleBand const role = in.role;
            in.role.present = true;
            in.role.anchor = end;
            in.role.minYd = 0.f;
            in.role.maxYd = kRerouteBandYd;
            in.reroute = true;
            Solver::Plan plan = RunSolve(bot, st, now, cfg);
            in.role = role;
            in.reroute = false;
            bool const moving = plan.outcome == Solver::Outcome::Move || plan.outcome == Solver::Outcome::GraceMove;
            // The solver offered only targets that close in on the end; its real path end (a navmesh snap) may not.
            if (!moving || Dist2d(plan.target, end) > toGo - in.params.rerouteGainYd)
                return false;
            plan.holdUntil = plan.arriveAt;
            BeginEscape(st, std::move(plan), now);
            ++st.counters.motionStops;
            ++st.counters.reroutes;
            return true;
        }

        // Idle/Watch (§3.4 step 4): solve when the spot is unsafe; re-route or hold when only the current motion is.
        void StepWatch(PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg)
        {
            Solver::Input& in = st.input;
            Milliseconds const horizonEnd = now + in.params.horizon;
            if (!Solver::PointSafe(in, in.bot, now, horizonEnd))
            {
                st.mode = MechanicBotMode::Watch;
                st.pendingEscape = false;
                if (BotBrain::Released(botAI, st, now))
                    return;
                if (!botAI->CanMove())
                {
                    // Rooted, stunned, polymorphed: no escape can start. Let it cast (heal) until it can move.
                    Release(bot, st, now, cfg, true, "cannot move");
                    return;
                }
                if (CanSolve(bot, st, now, cfg))
                    SolveAndApply(bot, st, now, cfg);
                else
                    st.pendingEscape = true;
                return;
            }
            st.pendingEscape = false;
            std::vector<Geo::Vec3> motion;
            if (RemainingSpline(bot, motion))
            {
                Solver::PathCheck const pc = CheckMotion(in, motion);
                if (!pc.ok)
                {
                    bool deferred = false;
                    if (TryReroute(bot, st, now, cfg, motion, deferred))
                        return;
                    HoldHere(st, now, cfg, pc.firstHit, deferred);
                    return;
                }
            }
            st.mode = MechanicBotMode::Watch;
        }

        bool SplineInFlight(Player const* bot)
        {
            Movement::MoveSpline const* ms = bot->movespline;
            return ms && ms->Initialized() && !ms->Finalized();
        }

        void StepEscape(PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg)
        {
            Solver::Input& in = st.input;
            Geo::Vec3 const pos = in.bot;
            Milliseconds const horizonEnd = now + in.params.horizon;
            // Arrived: at the target, or the move we sent has ended short (SearchForBestPath may move the end, a
            // partial path stops early) at a spot that is safe anyway. Within kArriveYd of the target while our
            // spline still runs, the spot must be safe too: an exit point lies only 0.5 yd beyond the margin, so a
            // bot stopped 0.6 yd short stands inside it, re-solves to the same point and is stopped again by the
            // hold, a creep of 0.1 yd per solve that ends in a Fallback once the window is near (M2 playtest).
            bool const spline = SplineInFlight(bot);
            bool const spotSafe = Solver::PointSafe(in, pos, now, horizonEnd);
            bool const arrived = (Dist2d(pos, st.plan.target) <= kArriveYd && (spotSafe || !spline))
                || (st.escapeIssuedAt != Milliseconds(0) && !spline && spotSafe);
            if (arrived)
            {
                if (now > st.escapeDeadline)
                {
                    ++st.counters.lateArrivals;
                    if (st.plan.outcome == Solver::Outcome::Fallback || st.plan.outcome == Solver::Outcome::NoPath)
                        ++st.counters.lateFallbacks;
                }
                st.mode = MechanicBotMode::Hold;
                st.outOfRoleSince = Milliseconds(0);
                return;
            }

            // Hard ceilings: never block heals and following for good.
            if (now >= st.escapeCeilingAt || now - st.escapeChainSince >= kEscapeChainCap)
            {
                Release(bot, st, now, cfg, false, "ceiling");
                return;
            }

            bool resolve = false;
            // The view changed (new revision, moving anchors, perception): is the rest of the way still clean? A plan
            // from the reduced-margin pass is checked at those margins (its path squeezes through a gap).
            if (st.viewRebuilt)
            {
                Solver::Input reduced;
                if (st.planReduced)
                    reduced = Solver::ReducedMargins(in);
                Solver::PathCheck const pc = CheckMotion(st.planReduced ? reduced : in,
                    RemainingPath(st.plan.path, pos));
                resolve = !pc.ok || !Solver::PointSafe(in, st.plan.target, pc.arriveAt,
                    std::max(pc.arriveAt, horizonEnd));
                if (!resolve)
                    st.solveBypass = false;
            }
            // Stuck, rooted or every send refused: no progress for kStuckAfter since the escape began or the bot last
            // moved kStuckYd. Sends do not restart this clock.
            bool stuck = false;
            if (Dist2d(pos, st.stuckRef) >= kStuckYd)
            {
                st.stuckRef = pos;
                st.stuckSince = now;
            }
            else if (now - st.stuckSince >= kStuckAfter)
                stuck = true;
            if (stuck && !botAI->CanMove())
            {
                Release(bot, st, now, cfg, true, "cannot move");
                return;
            }
            if (stuck)
            {
                resolve = true;
                st.solveBypass = true;
            }
            if (!resolve || !CanSolve(bot, st, now, cfg))
                return;     // a stuck re-solve stays due (stuckSince unchanged) until the budget allows it

            if (stuck)
                ++st.counters.stuckResolves;
            Geo::Vec3 const oldTarget = st.plan.target;
            Milliseconds const holdUntil = st.plan.holdUntil;
            Solver::Plan plan = RunSolve(bot, st, now, cfg);
            if (plan.outcome == Solver::Outcome::Safe)
            {
                // The spot the bot reached is safe: hold it for what the old plan answered.
                st.plan.target = pos;
                st.plan.path = { pos };
                st.plan.holdUntil = std::max(now, holdUntil);
                st.mode = MechanicBotMode::Hold;
                st.outOfRoleSince = Milliseconds(0);
                st.pendingEscape = false;
                return;
            }
            if (stuck && Dist2d(plan.target, oldTarget) <= kSameTargetYd)
            {
                // The same target again (off the navmesh, a partial path): nothing new to try. Give the bot back.
                Release(bot, st, now, cfg, false, "stuck on the same target");
                return;
            }
            BeginEscape(st, std::move(plan), now);
        }

        void StepHold(PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg)
        {
            Solver::Input& in = st.input;
            if (now >= st.plan.holdUntil)
            {
                st.plan = Solver::Plan();
                st.planFor.clear();
                st.outOfRoleSince = Milliseconds(0);
                StepWatch(botAI, bot, st, now, cfg);
                return;
            }
            if (!Solver::PointSafe(in, in.bot, now, now + in.params.horizon))
            {
                if (CanSolve(bot, st, now, cfg))
                    SolveAndApply(bot, st, now, cfg);
                return;
            }
            // Out of the role band for RoleSlackMs: give the spot back to stock movement (Watch polices its path).
            // Solve would only answer Safe here (the spot is safe), so the hold is released instead of re-solved.
            Solver::RoleBand const& role = in.role;
            if (!role.present)
            {
                st.outOfRoleSince = Milliseconds(0);
                return;
            }
            float const d = Dist2d(in.bot, role.anchor);
            if (d >= role.minYd && d <= role.maxYd)
            {
                st.outOfRoleSince = Milliseconds(0);
                return;
            }
            if (st.outOfRoleSince == Milliseconds(0))
                st.outOfRoleSince = now;
            else if (now - st.outOfRoleSince >= Milliseconds(cfg.roleSlackMs))
            {
                st.plan = Solver::Plan();
                st.planFor.clear();
                st.outOfRoleSince = Milliseconds(0);
                st.mode = MechanicBotMode::Watch;
            }
        }
    }

    char const* ModeName(MechanicBotMode mode)
    {
        switch (mode)
        {
            case MechanicBotMode::Idle:
                return "idle";
            case MechanicBotMode::Watch:
                return "watch";
            case MechanicBotMode::Escape:
                return "escape";
            case MechanicBotMode::Hold:
                return "hold";
        }
        return "?";
    }

    void MechanicBotState::Reset()
    {
        MechanicBotCounters const keptCounters = counters;
        bool const keptAvoidAoe = avoidAoeByGate;
        *this = MechanicBotState();
        counters = keptCounters;
        avoidAoeByGate = keptAvoidAoe;
    }

    MechanicBotState* BotBrain::StateOf(PlayerbotAI* botAI)
    {
        if (!botAI)
            return nullptr;
        AiObjectContext* context = botAI->GetAiObjectContext();
        if (!context)
            return nullptr;
        Value<MechanicBotState&>* value = context->GetValue<MechanicBotState&>("boss mechanic state");
        return value ? &value->RefGet() : nullptr;
    }

    void BotBrain::Update(PlayerbotAI* botAI, MechanicBotState& st, Milliseconds now)
    {
        if (st.brainRan && st.lastBrainAt == now)
            return;
        st.brainRan = true;
        st.lastBrainAt = now;

        Player* bot = botAI ? botAI->GetBot() : nullptr;
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || !bot->GetMap())
        {
            ToIdle(st);
            return;
        }
        // No negative cache (STAGE-M2-REVIEW m4): Get is one DataMap look-up, and a cached "no registry" delayed
        // the first mechanic of every map by up to its lifetime.
        Registry const* reg = Registry::Get(bot->GetMap());
        if (!reg)
        {
            st.viewValid = false;
            ToIdle(st);
            return;
        }

        BotConfig const& cfg = GetConfig().bots;
        RefreshView(bot, st, *reg, now, cfg);
        Solver::Input& in = st.input;
        if (!AnyRelevant(in))
        {
            ToIdle(st);
            return;
        }
        in.role = RoleBandOf(botAI, bot, cfg);
        st.leaveBy = Solver::LeaveBy(in, in.bot);

        switch (st.mode)
        {
            case MechanicBotMode::Idle:
            case MechanicBotMode::Watch:
                StepWatch(botAI, bot, st, now, cfg);
                break;
            case MechanicBotMode::Escape:
                StepEscape(botAI, bot, st, now, cfg);
                break;
            case MechanicBotMode::Hold:
                StepHold(botAI, bot, st, now, cfg);
                break;
        }
    }

    Milliseconds BotBrain::CastLeaveBy(PlayerbotAI* botAI, MechanicBotState& st, Registry const& reg,
        Milliseconds now)
    {
        Player* bot = botAI ? botAI->GetBot() : nullptr;
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
            return Solver::kForever;
        // Idle and nothing changed: the bot was not threatened before the cast and still is not.
        bool const idleUnchanged = st.mode == MechanicBotMode::Idle && st.viewValid
            && reg.Revision() == st.seenRevision && now < st.nextPerceptionAt
            && now - st.lastViewAt < kIdleViewRefresh;
        if (idleUnchanged)
            return Solver::kForever;

        BotConfig const& cfg = GetConfig().bots;
        RefreshView(bot, st, reg, now, cfg);
        if (!AnyRelevant(st.input))
            return Solver::kForever;
        st.leaveBy = Solver::LeaveBy(st.input, st.input.bot);
        // Something matters now: keep the waker's fast tick going once the cast ends.
        if (st.mode == MechanicBotMode::Idle)
            st.mode = MechanicBotMode::Watch;
        return st.leaveBy;
    }

    bool BotBrain::Released(PlayerbotAI* botAI, MechanicBotState& st, Milliseconds now)
    {
        if (now >= st.releaseUntil)
            return false;
        if (st.releaseForRoot && botAI && botAI->CanMove())
        {
            st.releaseUntil = Milliseconds(0);
            st.releaseForRoot = false;
            return false;
        }
        return true;
    }

    bool BotBrain::HasForeignMotion(Player const* bot)
    {
        if (!bot)
            return false;
        if (bot->movespline && bot->movespline->Initialized() && !bot->movespline->Finalized())
            return true;
        return bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE) != NULL_MOTION_TYPE;
    }

    Unit* BotBrain::FacingTarget(Player* bot)
    {
        if (!bot || !bot->IsInCombat())
            return nullptr;
        Unit* victim = bot->GetVictim();
        return victim && victim->IsInWorld() && victim->GetMap() == bot->GetMap() ? victim : nullptr;
    }
}

#endif
