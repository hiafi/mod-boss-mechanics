/*
 * mod-boss-mechanics: the per-bot brain of the boss-mechanics strategy (STAGE-M2-HANDOFF §3.4, WP-C;
 * STAGE-M3-HANDOFF §3.7, §3.8, WP-C).
 *
 * M4 (STAGE-M4-HANDOFF §3.6, §3.7, WP-C): the cost counters (MechanicCost.h: every oracle call timed through
 * TimedOracle, solve, route and brain time, lane-frame candidates, path calls per mechanic, brain time per map
 * update), the Fallback split by exposure and the route-loop counter.
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotBrain.h"
#include "AiObjectContext.h"
#include "BossMechanicsConfig.h"
#include "Creature.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MechanicBotFacing.h"
#include "MechanicBotRole.h"
#include "MechanicBotView.h"
#include "MechanicRegistry.h"
#include "MechanicRoute.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SpellAuras.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

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
        // Escape: the target (and, on a route, each waypoint) counts as reached within this (§3.4 step 4).
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
        // their solveBypass, and the lock treats them as escaping meanwhile. Role solves, region tracking, tank
        // re-facing searches and routes count too (M3 §3.8).
        constexpr uint32 kMaxSolvesPerMapUpdate = 8;
        // Tank facing: how often a tank between warnings checks whether a remembered source's cone holds the group.
        constexpr Milliseconds kTankCheckEvery{ 500 };
        // Healer line of sight to the tank: one ray this often (Watch, in combat); out of sight -> a role solve.
        constexpr Milliseconds kSightCheckEvery{ 1000 };
        // ...and after a role solve that stayed (nothing in sight worth the walk), no new one for this long.
        constexpr Milliseconds kSightBackoff{ 5000 };
        // Parking / drag-out hold: renewed every step while the park input holds.
        constexpr Milliseconds kParkHold{ 500 };
        // Look-away: a new facing goes onto the reservation board when it moved more than this.
        constexpr float kRepublishFacingDeg = 5.f;

        using CostClock = std::chrono::steady_clock;

        uint32 UsSince(CostClock::time_point t0)
        {
            return static_cast<uint32>(std::chrono::duration_cast<std::chrono::microseconds>(CostClock::now() - t0)
                .count());
        }

        // M4 cost (§3.6): times every oracle call into the bot's CostStats and counts the path calls (for the
        // per-mechanic attribution). Wraps the NavOracle of one solve, route or re-facing search.
        class TimedOracle : public Solver::PathOracle
        {
        public:
            TimedOracle(Solver::PathOracle& inner, CostStats& cost) : _inner(inner), _cost(cost) { }

            bool Find(Geo::Vec3 const& from, Geo::Vec3 const& to, std::vector<Geo::Vec3>& out) override
            {
                CostClock::time_point const t0 = CostClock::now();
                bool const found = _inner.Find(from, to, out);
                _cost.pathUs.Add(UsSince(t0));
                ++_calls;
                return found;
            }

            bool Walkable(Geo::Vec3& p) override
            {
                CostClock::time_point const t0 = CostClock::now();
                bool const walkable = _inner.Walkable(p);
                _cost.probeUs.Add(UsSince(t0));
                return walkable;
            }

            bool InSight(Geo::Vec3 const& from, Geo::Vec3 const& to) override
            {
                CostClock::time_point const t0 = CostClock::now();
                bool const clear = _inner.InSight(from, to);
                _cost.sightUs.Add(UsSince(t0));
                return clear;
            }

            uint32 Calls() const { return _calls; }

        private:
            Solver::PathOracle& _inner;
            CostStats& _cost;
            uint32 _calls = 0;
        };

        // The path calls of one solve or route go to every mechanic its plan answers (none: unattributed).
        void AttributePathCalls(CostStats& cost, std::vector<uint32> const& answers, uint32 calls)
        {
            if (!calls)
                return;
            if (answers.empty())
                cost.pathCallsUnattributed += calls;
            for (uint32 id : answers)
                cost.AddMechanicPathCalls(id, calls);
        }

        // Brain time of every bot on the map in one map update (§3.6): bots run their brain on their map's update
        // thread, so (map id, instance id, now) identifies the update, as for the solve budget. The waker takes the
        // sum after the bots' updates (BotBrain::TakeMapBrainUs).
        struct MapBrainTime
        {
            uint32 mapId = 0;
            uint32 instanceId = 0;
            Milliseconds at{ -1 };
            uint64 us = 0;
        };
        thread_local MapBrainTime tMapBrainTime;

        MapBrainTime& MapBrainTimeOf(uint32 mapId, uint32 instanceId, Milliseconds now)
        {
            MapBrainTime& t = tMapBrainTime;
            if (t.mapId != mapId || t.instanceId != instanceId || t.at != now)
            {
                t.mapId = mapId;
                t.instanceId = instanceId;
                t.at = now;
                t.us = 0;
            }
            return t;
        }

        // Times one brain step (Update past its once-per-ms check, CastLeaveBy) into the bot's and its map's brain
        // time.
        class BrainTimer
        {
        public:
            BrainTimer(MechanicBotState& st, Player const* bot, Milliseconds now)
                : _st(st), _bot(bot), _now(now), _t0(CostClock::now()) { }

            ~BrainTimer()
            {
                uint32 const us = UsSince(_t0);
                _st.cost.brainUs.Add(us);
                if (_bot)
                    MapBrainTimeOf(_bot->GetMapId(), _bot->GetInstanceId(), _now).us += us;
            }

            BrainTimer(BrainTimer const&) = delete;
            BrainTimer& operator=(BrainTimer const&) = delete;

        private:
            MechanicBotState& _st;
            Player const* _bot;
            Milliseconds _now;
            CostClock::time_point _t0;
        };

        Geo::Vec3 PosOf(WorldObject const* o)
        {
            return { o->GetPositionX(), o->GetPositionY(), o->GetPositionZ() };
        }

        float Dist2d(Geo::Vec3 const& a, Geo::Vec3 const& b)
        {
            return Geo::Dist2d(a.x, a.y, b.x, b.y);
        }

        bool SameMap(Player const* a, Player const* b)
        {
            return a && b && a->IsInWorld() && b->IsInWorld() && a->GetMap() == b->GetMap();
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
                if (other && (other->mode == MechanicBotMode::Escape || other->mode == MechanicBotMode::Hold
                    || other->mode == MechanicBotMode::Freeze))
                    out.push_back(other->plan.target);
            }
        }

        // Stacks of the self-trigger auras the view listed: a change is a view change (§3.4 RunOut/Spread SelfTrigger;
        // a stack change does not bump the registry revision).
        uint64 AuraSignature(Player const* bot, std::vector<uint32> const& auras)
        {
            uint64 sig = 0;
            for (uint32 spell : auras)
            {
                uint32 stacks = 0;
                if (Aura const* aura = bot->GetAura(spell))
                    stacks = aura->GetStackAmount();
                sig = sig * 1000003ull + (static_cast<uint64>(spell) << 8) + stacks + 1;
            }
            return sig;
        }

        // What the view needs from the glue for one brain step: the cached options, the fallibility filter and the
        // planned-spot hooks. Built lazily (only a rebuild or a solve needs it); lives for one call.
        class ViewCtx
        {
        public:
            ViewCtx(Player* bot, MechanicBotState& st, Registry const& reg, Milliseconds now, BotConfig const& cfg)
                : _bot(bot), _st(st), _reg(reg), _now(now), _cfg(cfg) { }

            BotViewOptions const& Options()
            {
                Init();
                return *_opt;
            }

            // nullptr when fallibility is off (no roll is taken).
            PerceptionFilter const* Filter()
            {
                Init();
                return _useFilter ? &_filter : nullptr;
            }

            BotViewHooks const* Hooks()
            {
                Init();
                return &_hooks;
            }

        private:
            void Init()
            {
                if (_opt)
                    return;
                _opt = BotViewOptions::FromConfig(_cfg);
                Fallibility::Config const fc = FallibilityConfigOf(_cfg.fallibility);
                float const scale = Fallibility::ScaleFor(fc, _bot->GetMap()->IsHeroic());
                _useFilter = scale > 0.f;
                if (_useFilter)
                {
                    MechanicBotState& st = _st;
                    Registry const& reg = _reg;
                    Milliseconds const now = _now;
                    uint64 const botKey = _bot->GetGUID().GetRawValue();
                    _filter = [&st, &reg, now, fc, scale, botKey](uint32 id, Milliseconds& until)
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
                }
                else
                    _st.perception.clear();
                Player* bot = _bot;
                _hooks.plannedSpot = [bot](ObjectGuid guid, Geo::Vec3& spot, float& facing)
                {
                    Player const* member = ObjectAccessor::GetPlayer(*bot, guid);
                    return member && member != bot && BotRole::PlannedSpotOf(member, spot, facing);
                };
                // A bot the others may plan for: one that runs this brain now (the strategy in its current engine) and
                // is not released. Anything else stands where it stands, like a human (STAGE-M3-R-REPORT m-7).
                Milliseconds const now = _now;
                _hooks.isBot = [bot, now](ObjectGuid guid)
                {
                    if (guid == bot->GetGUID())
                        return true;
                    Player* member = ObjectAccessor::GetPlayer(*bot, guid);
                    if (!member || IsSelfBot(member))
                        return false;
                    PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
                    if (!ai || !ai->HasStrategy("boss mechanics", ai->GetState()))
                        return false;
                    MechanicBotState const* other = BotBrain::StateOf(ai);
                    return other && now >= other->releaseUntil;
                };
            }

            Player* _bot;
            MechanicBotState& _st;
            Registry const& _reg;
            Milliseconds _now;
            BotConfig const& _cfg;
            std::optional<BotViewOptions> _opt;
            bool _useFilter = false;
            PerceptionFilter _filter;
            BotViewHooks _hooks;
        };

        // Rebuilds the view (§3.5) with the fallibility filter (§3.8) when it is enabled.
        void RebuildView(Player* bot, MechanicBotState& st, Registry const& reg, Milliseconds now, ViewCtx& ctx)
        {
            // M4 cost: mechanic ids are per registry, so another map's live mechanics are over for this bot.
            uint64 const registryKey = (static_cast<uint64>(bot->GetMapId()) << 32) | bot->GetInstanceId();
            if (registryKey != st.costRegistryKey)
            {
                st.cost.FoldGone([](uint32) { return true; });
                st.costRegistryKey = registryKey;
            }
            uint32 const revision = reg.Revision();
            if (revision != st.seenRevision || !st.viewValid)
            {
                // A new revision: drop what refers to mechanics that are gone, and let the next solve skip the rate
                // limit (§3.4 step 5).
                std::erase_if(st.perception, [&reg](auto const& kv) { return reg.Find(kv.first) == nullptr; });
                std::erase_if(st.loggedFallback, [&reg](uint32 id) { return reg.Find(id) == nullptr; });
                st.cost.FoldGone([&reg](uint32 id) { return reg.Find(id) == nullptr; });
                st.solveBypass = true;
            }
            if (now >= st.nextPerceptionAt)
                st.solveBypass = true;      // a hidden mechanic became visible: react at once

            BotViewInfo info = BuildInput(reg, bot, now, ctx.Options(), st.input, ctx.Filter(), ctx.Hooks());
            FillCrowd(bot, st.input.crowd);

            uint64 const sig = AuraSignature(bot, info.selfTriggerAuras);
            if (sig != st.auraSignature)
                st.solveBypass = true;      // a self-trigger stack changed: a carrier now (or no longer)
            st.auraSignature = sig;
            st.seenRevision = info.revision;
            st.seenReservationRevision = info.reservationRevision;
            st.movingAnchors = info.movingAnchors;
            st.nextPerceptionAt = info.nextPerceptionAt;
            st.viewInfo = std::move(info);
            st.lastViewAt = now;
            st.viewValid = true;
            st.viewRebuilt = true;
        }

        // Keeps the view current: a rebuild when it is stale (§3.4 step 2), the player kinds alone when only the
        // reservation board changed (§3.6), else only the bot's own pose and the time.
        void RefreshView(Player* bot, MechanicBotState& st, Registry const& reg, Milliseconds now, BotConfig const& cfg,
            ViewCtx& ctx)
        {
            st.viewRebuilt = false;
            bool const idle = st.mode == MechanicBotMode::Idle;
            bool const auraChanged = !st.viewInfo.selfTriggerAuras.empty()
                && AuraSignature(bot, st.viewInfo.selfTriggerAuras) != st.auraSignature;
            bool const stale = !st.viewValid
                || reg.Revision() != st.seenRevision
                || now >= st.nextPerceptionAt
                || auraChanged
                || (!idle && now - st.lastViewAt >= kViewRefresh)
                || (st.movingAnchors && now - st.lastViewAt >= Milliseconds(cfg.movingAnchorRecheckMs))
                || (idle && reg.Size() && now - st.lastViewAt >= kIdleViewRefresh);
            if (stale)
            {
                RebuildView(bot, st, reg, now, ctx);
                return;
            }
            st.input.bot = PosOf(bot);
            st.input.speed = bot->GetSpeed(MOVE_RUN);
            st.input.now = now;
            if (!idle && reg.ReservationRevision() != st.seenReservationRevision)
            {
                // Another bot published or dropped a spot: allies, synthetic hazards and claims only.
                RefreshAllies(reg, bot, now, ctx.Options(), ctx.Hooks(), st.input, &st.viewInfo, ctx.Filter());
                st.seenReservationRevision = st.viewInfo.reservationRevision;
                st.viewRebuilt = true;
            }
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

        // A task of the view whose window has not ended (M3: player kinds make a bot relevant without a hazard).
        bool AnyTask(MechanicBotState const& st, Milliseconds now)
        {
            Milliseconds const margin = st.input.params.marginMs;
            for (BotTask const& t : st.viewInfo.tasks)
                if (t.to + margin >= now)
                    return true;
            return false;
        }

        // The bot holds its spot for its tasks until this (0 = no task hold): every live task but Avoider (it only
        // keeps out of other players' circles; Watch polices its motion) and Frozen (the Freeze mode); and the
        // parking hold.
        Milliseconds TaskHoldUntil(MechanicBotState const& st, Milliseconds now, std::vector<uint32>* ids = nullptr)
        {
            Milliseconds const margin = st.input.params.marginMs;
            Milliseconds until{ 0 };
            for (BotTask const& t : st.viewInfo.tasks)
            {
                if (t.role == BotTaskRole::Avoider || t.role == BotTaskRole::Frozen || t.to + margin < now)
                    continue;
                until = std::max(until, t.to + margin);
                if (ids)
                    ids->push_back(t.mechanicId);
            }
            if (st.parking)
                until = std::max(until, now + kParkHold);
            return until;
        }

        bool HasRole(MechanicBotState const& st, BotTaskRole role, Milliseconds now)
        {
            Milliseconds const margin = st.input.params.marginMs;
            for (BotTask const& t : st.viewInfo.tasks)
                if (t.role == role && t.to + margin >= now)
                    return true;
            return false;
        }

        // §3.8: leaveBy stays hazard-based; an unmet hard region adds its latest start (from - marginMs - startDelay -
        // the straight travel in), so a heal in Hold may finish only when it ends in time.
        Milliseconds LeaveByAll(Solver::Input const& in)
        {
            Milliseconds best = Solver::LeaveBy(in, in.bot);
            float const speed = Solver::Detail::Speed(in);
            for (Solver::Region const& r : in.regions)
            {
                if (!r.hard || !Solver::Detail::RegionLive(in, r) || Solver::Detail::InsideRegion(r, in.bot))
                    continue;
                float const way = std::max(0.f, Solver::Detail::RegionSd(r.shape, r.poses, in.bot) + r.marginYd);
                best = std::min(best, r.from - in.params.marginMs - in.params.startDelay
                    - Solver::Detail::TravelMs(way + Solver::Detail::kExitPushYd, speed));
            }
            return best;
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

        // The bot stands in a hazard's exact hitbox (no margin) while one of its windows there is open.
        bool InsideHot(Solver::Input const& in, Geo::Vec3 const& p, Milliseconds now)
        {
            for (Solver::Hazard const& h : in.hazards)
            {
                bool inside = false;
                for (Geo::Pose const& pose : h.poses)
                    inside = inside || Solver::SignedDistance(h.shape, pose, p) <= 0.f;
                if (!inside)
                    continue;
                for (HotWindow const& w : Solver::WindowsAt(h, p))
                    if (w.from <= now && now <= w.to)
                        return true;
            }
            return false;
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

        // One unit of the per-map budget (a solve, a role solve, a re-facing search or a route). false = spent.
        bool TakeBudget(Player const* bot, Milliseconds now)
        {
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

        // May the bot solve now? Rate limit per bot (MinSolveIntervalMs, unless solveBypass), then the per-map budget,
        // which a true answer consumes (every true answer is followed by a solve).
        bool CanSolve(Player const* bot, MechanicBotState const& st, Milliseconds now, BotConfig const& cfg)
        {
            bool const rateOk = st.solveBypass || st.lastSolveAt == Milliseconds(0)
                || now - st.lastSolveAt >= Milliseconds(cfg.minSolveIntervalMs);
            return rateOk && TakeBudget(bot, now);
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

        // The bot leaves every player kind it took part in (released, idle, dead): its reservations go.
        void DropBoard(Registry* reg, Player const* bot, MechanicBotState& st)
        {
            if (!st.reserved)
                return;
            st.reserved = false;
            st.lookPublished = false;
            if (reg && bot)
                DropReservations(*reg, bot->GetGUID());
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
            st.routePending = false;
            st.waypointIdx = 0;
            st.freezeUntil = Milliseconds(0);
            st.freezeTo = Milliseconds(0);
            st.lookOwed = false;
            st.lookWindow = false;
            st.faceOwed = false;
            st.parking = false;
            st.tankWarning = false;
            st.refacing = false;
            st.regionRoutePending = false;
            st.regionRoute = false;
        }

        // Gives the bot back to the stock AI for `duration` (STAGE-M2-REVIEW M1): mode Watch, no plan, nothing
        // locked, no cast break; an unsafe spot is not solved again until releaseUntil (or, forRoot, until the bot can
        // move again). Its reservations go (M3): the others plan against where it really stands.
        void Release(Registry* reg, Player const* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg,
            bool forRoot, char const* why)
        {
            st.mode = MechanicBotMode::Watch;
            st.plan = Solver::Plan();
            st.planFor.clear();
            st.outOfRoleSince = Milliseconds(0);
            st.escapeIssuedAt = Milliseconds(0);
            st.pendingEscape = false;
            st.routePending = false;
            st.waypointIdx = 0;
            st.refacing = false;
            st.regionRoutePending = false;
            st.regionRoute = false;
            st.releaseUntil = now + kReleaseFor;
            st.releaseForRoot = forRoot;
            ++st.counters.releases;
            DropBoard(reg, bot, st);
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

        // Hold the current (safe) spot for the bot's tasks (§3.4: a Stack leader, a claimer in place, a carrier whose
        // ring is clear, the tank during a Frontal warning, parking): no stock wander until `until`.
        void HoldForTasks(MechanicBotState& st, Milliseconds now, Milliseconds until, std::vector<uint32>&& ids)
        {
            Solver::Input const& in = st.input;
            st.plan = Solver::Plan();
            st.plan.target = in.bot;
            st.plan.path = { in.bot };
            st.plan.arriveAt = now;
            st.plan.holdUntil = until;
            st.plan.hasFacing = st.lookOwed;
            st.plan.facing = st.lookFacing;
            st.plan.answers = std::move(ids);
            st.planFor = st.plan.answers;
            st.mode = MechanicBotMode::Hold;
            st.outOfRoleSince = Milliseconds(0);
        }

        // Solve (§3.9) with the telemetry and the once-per-mechanic Fallback log. M3: allies, synthetic hazards and
        // claims are refreshed right before the solve, and the plan goes onto the reservation board right after it,
        // so the next bot solving on this map thread sees it (§3.6). A re-route plan is never published.
        Solver::Plan RunSolve(Registry& reg, Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg,
            ViewCtx& ctx, bool roleSolve = false)
        {
            Solver::Input& in = st.input;
            // Timed with the allies' refresh: `.mech bots` shows what one step costs (STAGE-M3-R-REPORT n-1).
            auto const t0 = std::chrono::steady_clock::now();
            if (!in.reroute)
                RefreshAllies(reg, bot, now, ctx.Options(), ctx.Hooks(), in, &st.viewInfo, ctx.Filter());
            st.seenReservationRevision = st.viewInfo.reservationRevision;
            in.roleSolve = roleSolve;

            // Debug = 1 only: the oracle counts why its probes and path calls failed (nothing is counted otherwise).
            NavOracleStats nav;
            NavOracle navOracle(bot, cfg.debug ? &nav : nullptr, cfg.healerMaxSightChecks);
            TimedOracle oracle(navOracle, st.cost);
            Solver::Plan plan = Solver::Solve(in, oracle);
            auto const us = static_cast<uint32>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
            in.roleSolve = false;

            // M4 cost (§3.6) and the Fallback split (§3.7).
            CostStats& cost = st.cost;
            cost.solveUs.Add(us);
            if (!in.lanes.empty())
            {
                ++cost.laneSolves;
                cost.laneCandidates += plan.stats.laneCandidates;
                cost.lanePathCalls += plan.stats.lanePathChecks;
            }
            AttributePathCalls(cost, plan.answers, oracle.Calls());

            MechanicBotCounters& c = st.counters;
            if (plan.outcome == Solver::Outcome::Fallback)
                ++(plan.exposure > Milliseconds(0) ? c.fallbackHot : c.fallbackClear);
            ++c.solves;
            c.pathChecks += plan.pathChecks;
            ++c.outcomes[static_cast<std::size_t>(plan.outcome)];
            if (plan.outcome == Solver::Outcome::Move && plan.stats.pass == 2)
                ++c.tightMoves;
            c.solveUsLast = us;
            c.solveUsMax = std::max(c.solveUsMax, us);
            c.solveUsTotal += us;
            c.sightRays += plan.sightChecks;
            if (roleSolve)
                ++c.roleSolves;
            if (!in.reroute)
            {
                if (HasRole(st, BotTaskRole::Carrier, now))
                    ++c.carrierSolves;
                if (HasRole(st, BotTaskRole::Claimer, now))
                    ++c.claimSolves;
                if (HasRole(st, BotTaskRole::Picker, now))
                    ++c.pickSolves;
            }
            st.lastSolveAt = now;
            st.solveBypass = false;
            // M4 WP-Q (G2): a Fallback that breaks a hard region asks for a route round the hazards into it
            // (StepRegionRoute, on a later brain step).
            if (!in.reroute && !roleSolve && cfg.routeEnable && plan.violated > 0
                && (plan.outcome == Solver::Outcome::Fallback || plan.outcome == Solver::Outcome::NoPath))
            {
                st.regionRoutePending = true;
                st.regionRouteAskedAt = now;
            }

            Geo::Vec3 const from = in.bot;
            if (cfg.debug)
            {
                c.debugSolve.Add(plan.stats);
                c.debugNav.Add(nav);
                LOG_INFO(kLog, "Bot {}: {} -> {} at ({:.1f}, {:.1f}, {:.1f}), {:.1f} yd, answers {}, {} candidates, "
                    "{} path checks, {} us; pass {}, {}; nav {}", bot->GetName(), in.reroute ? "reroute"
                    : (roleSolve ? "role solve" : "solve"), OutcomeName(plan.outcome), plan.target.x, plan.target.y,
                    plan.target.z, Dist2d(from, plan.target), IdList(plan.answers), plan.candidates, plan.pathChecks,
                    us, PassName(plan.stats.pass), FormatSolveStats(plan.stats), FormatNavStats(nav));
            }
            if (plan.violated > 0)
            {
                // Decision §7.2: a Zone outranks the player kinds; the plan knowingly breaks this many of them.
                ++c.violatedPlans;
                if (cfg.debug)
                    LOG_INFO(kLog, "Bot {}: {} breaks {} player kind(s) for a Zone (tasks {})", bot->GetName(),
                        OutcomeName(plan.outcome), plan.violated, st.viewInfo.tasks.size());
                else
                    LOG_DEBUG(kLog, "Bot {}: {} breaks {} player kind(s) for a Zone (tasks {})", bot->GetName(),
                        OutcomeName(plan.outcome), plan.violated, st.viewInfo.tasks.size());
            }
            if (plan.outcome == Solver::Outcome::Fallback || plan.outcome == Solver::Outcome::NoPath)
            {
                // Once per bot and mechanic (PLAN §7 navmesh gaps). Debug level: a recurring infeasible mechanic
                // (a boss-anchored zone on the chased tank) would otherwise warn per bot per cast; `.mech bots` counts
                // them, and Debug = 1 logs them at INFO.
                uint32 first = 0;
                Solver::PointSafe(in, from, now, now + in.params.horizon, &first);
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

            // The reservation board (§3.6): the target (Safe: the bot's spot) of every task; also clears the
            // mechanics the bot left.
            if (!in.reroute && (st.reserved || !st.viewInfo.tasks.empty()))
            {
                PublishReservations(reg, bot, st.viewInfo, plan, now);
                st.reserved = !st.viewInfo.tasks.empty();
                st.lookPublished = plan.hasFacing;
                st.lookPublishedFacing = plan.facing;
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
            st.lateCounted = false;
            st.planReduced = plan.stats.pass == 2;
            st.plan = std::move(plan);
            st.planFor = st.plan.answers;
            st.mode = MechanicBotMode::Escape;
            st.escapeIssuedAt = Milliseconds(0);
            st.escapeSentAt = Milliseconds(0);
            st.waypointIdx = 0;
            st.stuckRef = from;
            st.stuckSince = now;
            st.outOfRoleSince = Milliseconds(0);
            st.pendingEscape = false;
            st.routePending = false;
            st.refacing = false;
            st.regionRoute = false;
        }

        // Solve and take the plan: Move/GraceMove/Fallback/NoPath -> Escape; Safe -> Watch.
        void SolveAndApply(Registry& reg, Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg,
            ViewCtx& ctx)
        {
            Solver::Plan plan = RunSolve(reg, bot, st, now, cfg, ctx);
            if (plan.outcome == Solver::Outcome::Safe)
            {
                st.mode = MechanicBotMode::Watch;
                st.pendingEscape = false;
                return;
            }
            if (st.parking && (plan.outcome == Solver::Outcome::Move || plan.outcome == Solver::Outcome::GraceMove))
                ++st.counters.parks;
            BeginEscape(st, std::move(plan), now);
        }

        // The bot's foreign motion crosses a hazard (STAGE-M2-REVIEW m2): walk around it towards the motion's end,
        // by a validated path, when that end is safe and the solver finds a clean target closer to it. The bot then
        // holds nothing (holdUntil = arrival): Watch takes the stock motion back, and re-routes again if it still
        // crosses. Returns false when the caller should hold instead (deferred is set when only the solve rate limit
        // or budget said no).
        bool TryReroute(Registry& reg, Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg,
            ViewCtx& ctx, std::vector<Geo::Vec3> const& motion, bool& deferred)
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
            Solver::Plan plan = RunSolve(reg, bot, st, now, cfg, ctx);
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

        // A route ends where stock follow can take over (§3.8): RouteHandoff's waypoint (the first one whose straight
        // line to the goal is clean, STAGE-M3-R-REPORT m-9). Arrival and holdUntil are re-timed to it; the answers
        // keep hazards only (a route is for following, not for a region or a carried shape, WP-A note).
        void TrimRoute(Solver::Input const& in, Solver::Plan& plan, Geo::Vec3 const& goal)
        {
            Solver::TrimToHandoff(in, plan, goal);
            std::erase_if(plan.answers, [&in](uint32 id) { return !FindHazard(in, id); });
        }

        // §3.8 route search: on a brain step after a failed, non-deferred re-route, at most once per Route.RetryMs, one
        // unit of the per-map budget. A found route becomes an Escape that walks its waypoints. false = no route
        // (the bot keeps holding, as before).
        bool RunRoute(Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg)
        {
            Solver::Input& in = st.input;
            Geo::Vec3 const goal = st.routeGoal;
            st.lastRouteAt = now;
            // §3.7: routed again for the goal a route just handed off to stock follow (the follow re-crossed).
            if (Solver::IsRouteLoop(st.routeHandoffAt, st.routeHandoffGoal, now, goal))
            {
                ++st.counters.routeLoops;
                if (cfg.debug)
                    LOG_INFO(kLog, "Bot {}: route loop, routed again {} ms after a handoff for ({:.1f}, {:.1f})",
                        bot->GetName(), (now - st.routeHandoffAt).count(), goal.x, goal.y);
            }
            Solver::RouteParams rp;
            rp.padYd = cfg.routePadYd;
            rp.cellYd = cfg.routeCellYd;
            rp.maxCells = cfg.routeMaxCells;
            rp.maxExpansions = cfg.routeMaxExpansions;
            rp.maxLegs = cfg.routeMaxLegs;
            NavOracleStats nav;
            NavOracle navOracle(bot, cfg.debug ? &nav : nullptr, 0);
            TimedOracle oracle(navOracle, st.cost);
            Solver::RouteStats rs;
            auto const t0 = std::chrono::steady_clock::now();
            Solver::Plan plan = Solver::Route(in, goal, oracle, rp, &rs);
            auto const us = static_cast<uint32>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
            st.cost.routeUs.Add(us);
            AttributePathCalls(st.cost, plan.answers, oracle.Calls());
            MechanicBotCounters& c = st.counters;
            c.routeUsLast = us;
            c.routeUsMax = std::max(c.routeUsMax, us);
            if (cfg.debug)
            {
                c.debugNav.Add(nav);
                LOG_INFO(kLog, "Bot {}: route to ({:.1f}, {:.1f}) {}: {} cells ({:.2f} yd{}), {} blocked, {} "
                    "expansions, {} legs, {} oracle calls, {} retries, pass {}, {} us", bot->GetName(), goal.x, goal.y,
                    rs.found ? "found" : "none", rs.cells, rs.cellYd, rs.grown ? ", box grown" : "", rs.blocked,
                    rs.expansions, rs.legs, rs.oracleCalls, rs.retries, PassName(plan.stats.pass), us);
            }
            if (!rs.found || plan.outcome != Solver::Outcome::Move || plan.waypoints.empty())
            {
                ++c.routesFailed;
                return false;
            }
            ++c.routesFound;
            TrimRoute(in, plan, goal);
            BeginEscape(st, std::move(plan), now);
            return true;
        }

        // M4 WP-Q (G2): Solver::RegionRoute after a Fallback that broke a hard region (the Alexei Assembly member whose
        // way to the cluster crosses the Out of bounds circle). A found route becomes an Escape that walks its
        // waypoints to a spot passing TargetOk (st.regionRoute: the re-check holds its end to TargetOk) and goes onto
        // the reservation board like a solve's plan. false = none (the Fallback stands).
        bool RunRegionRoute(Registry& reg, Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg)
        {
            Solver::Input& in = st.input;
            st.lastRegionRouteAt = now;
            ++st.counters.regionRoutes;
            Solver::RouteParams rp;
            rp.padYd = cfg.routePadYd;
            rp.cellYd = cfg.routeCellYd;
            rp.maxCells = cfg.routeMaxCells;
            rp.maxExpansions = cfg.routeMaxExpansions;
            rp.maxLegs = cfg.routeMaxLegs;
            NavOracleStats nav;
            NavOracle navOracle(bot, cfg.debug ? &nav : nullptr, 0);
            TimedOracle oracle(navOracle, st.cost);
            Solver::RouteStats rs;
            auto const t0 = std::chrono::steady_clock::now();
            Solver::Plan plan = Solver::RegionRoute(in, oracle, rp, &rs);
            auto const us = static_cast<uint32>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
            st.cost.routeUs.Add(us);
            AttributePathCalls(st.cost, plan.answers, oracle.Calls());
            MechanicBotCounters& c = st.counters;
            c.routeUsLast = us;
            c.routeUsMax = std::max(c.routeUsMax, us);
            if (cfg.debug)
            {
                c.debugNav.Add(nav);
                LOG_INFO(kLog, "Bot {}: region route {}: {} cells ({:.2f} yd{}), {} expansions, {} legs, {} oracle "
                    "calls, {} us", bot->GetName(), rs.found ? "found" : "none", rs.cells, rs.cellYd,
                    rs.grown ? ", box grown" : "", rs.expansions, rs.legs, rs.oracleCalls, us);
            }
            if (plan.outcome != Solver::Outcome::Move)
                return false;
            ++c.regionRoutesFound;
            if (st.reserved || !st.viewInfo.tasks.empty())
            {
                PublishReservations(reg, bot, st.viewInfo, plan, now);
                st.reserved = !st.viewInfo.tasks.empty();
            }
            BeginEscape(st, std::move(plan), now);
            st.regionRoute = true;
            return true;
        }

        // Tank facing between warnings (§3.7, decision §7.5): once a source has cast a tank-exempt Frontal this fight,
        // a cone that would now hold a group member makes the tank walk round the source to a clear ring point.
        bool StepTankFacing(Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg)
        {
            if (!cfg.tankFacingEnable || st.frontalSources.empty() || st.tankWarning)
                return false;
            if (now - st.lastTankCheckAt < kTankCheckEvery)
                return false;
            st.lastTankCheckAt = now;
            for (MechanicBotState::FrontalSource const& f : st.frontalSources)
            {
                Creature const* source = ObjectAccessor::GetCreature(*bot, f.source);
                if (!source || !BotRole::FrontalHitsGroup(bot, source, f, st.input.bot, cfg.facingSlackDeg))
                    continue;
                if (!TakeBudget(bot, now))
                    return false;
                NavOracle navOracle(bot, nullptr, 0);
                TimedOracle oracle(navOracle, st.cost);
                Solver::Plan plan;
                bool const found = BotRole::TankRefacePlan(bot, source, f, st.input, cfg.facingSlackDeg, oracle, plan);
                st.cost.pathCallsUnattributed += oracle.Calls();
                if (!found)
                {
                    if (cfg.debug)
                        LOG_INFO(kLog, "Bot {}: no clear ring point to turn {} away from the group", bot->GetName(),
                            source->GetName());
                    return false;
                }
                ++st.counters.tankRefacings;
                if (cfg.debug)
                    LOG_INFO(kLog, "Bot {}: turns {} away from the group, to ({:.1f}, {:.1f})", bot->GetName(),
                        source->GetName(), plan.target.x, plan.target.y);
                BeginEscape(st, std::move(plan), now);
                st.refacing = true;
                return true;
            }
            return false;
        }

        // Healer line of sight (§3.8 role band v2): a ray to the tank now and then; out of sight -> a role solve
        // (Input::roleSolve) that lets the Sight score move it. Soft (decision §7.11): a Safe answer means stay.
        bool StepHealerSight(Registry& reg, Player* bot, MechanicBotState& st, Milliseconds now, BotConfig const& cfg,
            ViewCtx& ctx)
        {
            Solver::Input& in = st.input;
            if (in.sight.points.empty() || now - st.lastSightAt < kSightCheckEvery || now < st.nextSightAt)
                return false;
            st.lastSightAt = now;
            Player* tank = BotRole::NearestGroupTank(bot);
            if (!tank || bot->IsWithinLOSInMap(tank))
                return false;
            if (!CanSolve(bot, st, now, cfg))
                return false;
            Solver::Plan plan = RunSolve(reg, bot, st, now, cfg, ctx, true);
            if (plan.outcome != Solver::Outcome::Move && plan.outcome != Solver::Outcome::GraceMove)
            {
                // Nothing in sight worth the walk: do not pay a solve every second for the same answer.
                st.nextSightAt = now + kSightBackoff;
                return false;
            }
            BeginEscape(st, std::move(plan), now);
            return true;
        }

        // A claim role (Cluster leader, Soak claimer, Bait picker) whose spot already passes holds without a solve, so
        // nothing would put its claim on the board: publish the spot it holds (STAGE-M3-R-REPORT m-3), or every other
        // bot recomputes the Cluster leader (and the claims) from moving positions. Only when the claim is missing.
        void PublishHeldClaims(Registry& reg, Player* bot, MechanicBotState& st, Milliseconds now)
        {
            ObjectGuid const self = bot->GetGUID();
            Milliseconds const margin = st.input.params.marginMs;
            bool missing = false;
            for (BotTask const& t : st.viewInfo.tasks)
            {
                if (t.to + margin < now || (t.role != BotTaskRole::Leader && t.role != BotTaskRole::Claimer
                    && t.role != BotTaskRole::Picker))
                    continue;
                ActiveMechanic const* m = reg.Find(t.mechanicId);
                if (!m)
                    continue;
                bool held = false;
                for (Reservation const& r : m->reservations)
                    held = held || (r.bot == self && r.claim && r.until > now);
                missing = missing || !held;
            }
            if (!missing)
                return;
            Solver::Plan p;
            p.outcome = Solver::Outcome::Safe;
            p.target = st.input.bot;
            p.hasFacing = st.lookOwed;
            p.facing = st.lookFacing;
            PublishReservations(reg, bot, st.viewInfo, p, now);
            st.reserved = true;
        }

        // Idle/Watch (§3.4 step 4): solve when the spot fails TargetOk; hold for live tasks; re-route or hold when
        // only the current motion is unsafe; then the tank and healer tasks.
        void StepWatch(Registry& reg, PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Milliseconds now,
            BotConfig const& cfg, ViewCtx& ctx)
        {
            Solver::Input& in = st.input;
            Milliseconds const horizonEnd = now + in.params.horizon;
            if (!Solver::TargetOk(in, in.bot, now, horizonEnd))
            {
                st.mode = MechanicBotMode::Watch;
                st.pendingEscape = false;
                if (BotBrain::Released(botAI, st, now))
                    return;
                if (!botAI->CanMove())
                {
                    // Rooted, stunned, polymorphed: no escape can start. Let it cast (heal) until it can move.
                    Release(&reg, bot, st, now, cfg, true, "cannot move");
                    return;
                }
                if (CanSolve(bot, st, now, cfg))
                    SolveAndApply(reg, bot, st, now, cfg, ctx);
                else
                    st.pendingEscape = true;
                return;
            }
            st.pendingEscape = false;
            if (!BotBrain::Released(botAI, st, now))
            {
                std::vector<uint32> ids;
                Milliseconds const until = TaskHoldUntil(st, now, &ids);
                if (until > now)
                {
                    PublishHeldClaims(reg, bot, st, now);
                    HoldForTasks(st, now, until, std::move(ids));
                    return;
                }
            }
            std::vector<Geo::Vec3> motion;
            if (RemainingSpline(bot, motion))
            {
                Solver::PathCheck const pc = CheckMotion(in, motion);
                if (!pc.ok)
                {
                    bool deferred = false;
                    if (TryReroute(reg, bot, st, now, cfg, ctx, motion, deferred))
                        return;
                    HoldHere(st, now, cfg, pc.firstHit, deferred);
                    // M3 §3.8: the greedy hop found nothing; a route search runs on a later brain step.
                    if (!deferred && cfg.routeEnable
                        && Solver::PointSafe(in, motion.back(), now, now + in.params.horizon))
                    {
                        st.routePending = true;
                        st.routeGoal = motion.back();
                        st.routeAskedAt = now;
                    }
                    return;
                }
            }
            st.mode = MechanicBotMode::Watch;
            if (StepTankFacing(bot, st, now, cfg))
                return;
            StepHealerSight(reg, bot, st, now, cfg, ctx);
        }

        bool SplineInFlight(Player const* bot)
        {
            Movement::MoveSpline const* ms = bot->movespline;
            return ms && ms->Initialized() && !ms->Finalized();
        }

        void StepEscape(Registry& reg, PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Milliseconds now,
            BotConfig const& cfg, ViewCtx& ctx)
        {
            Solver::Input& in = st.input;
            Geo::Vec3 const pos = in.bot;
            Milliseconds const horizonEnd = now + in.params.horizon;
            bool const spline = SplineInFlight(bot);
            // Route (waypoint mode, §3.8): the next leg once the bot is within kArriveYd of this waypoint, or our move
            // to it ended (short, a partial path); in both cases only when the straight line from where the bot
            // stands to the next waypoint passes the route's own margin (Solver::RouteAdvanceOk): the move action
            // paths from here, not from the waypoint the leg was validated from (STAGE-M3-R-REPORT M-4). Otherwise
            // it walks on to the waypoint (a resend), or the stuck clock re-solves.
            bool const lastLeg = st.plan.waypoints.empty() || st.waypointIdx + 1 >= st.plan.waypoints.size();
            if (!lastLeg)
            {
                Geo::Vec3 const& wp = st.plan.waypoints[st.waypointIdx];
                bool const sentHere = st.escapeIssuedAt != Milliseconds(0) && Dist2d(st.issuedTarget, wp) <= 0.1f;
                if (Dist2d(pos, wp) <= kArriveYd || (sentHere && !spline))
                {
                    Geo::Vec3 const& next = st.plan.waypoints[st.waypointIdx + 1];
                    bool ok = false;
                    if (st.planReduced)
                        ok = Solver::RouteAdvanceOk(Solver::ReducedMargins(in), pos, next, now);
                    else
                        ok = Solver::RouteAdvanceOk(in, pos, next, now);
                    if (ok)
                    {
                        ++st.waypointIdx;
                        st.escapeIssuedAt = Milliseconds(0);
                    }
                }
            }
            // Arrived: at the target, or the move we sent has ended short (SearchForBestPath may move the end, a
            // partial path stops early) at a spot that is safe anyway. Within kArriveYd of the target while our
            // spline still runs, the spot must be safe too: an exit point lies only 0.5 yd beyond the margin, so a
            // bot stopped 0.6 yd short stands inside it, re-solves to the same point and is stopped again by the
            // hold, a creep of 0.1 yd per solve that ends in a Fallback once the window is near (M2 playtest).
            bool const onLastLeg = st.plan.waypoints.empty() || st.waypointIdx + 1 >= st.plan.waypoints.size();
            bool const spotSafe = Solver::TargetOk(in, pos, now, horizonEnd);
            bool const arrived = (Dist2d(pos, st.plan.target) <= kArriveYd && (spotSafe || !spline))
                || (onLastLeg && st.escapeIssuedAt != Milliseconds(0) && !spline && spotSafe);
            // Late: still inside a hazard's exact hitbox once one of its windows there has started. Reaching the
            // target after the window opened is not late on its own: the target often lies well past the edge.
            if (!st.lateCounted && now >= st.escapeDeadline && InsideHot(in, pos, now))
            {
                st.lateCounted = true;
                ++st.counters.lateArrivals;
                if (st.plan.outcome == Solver::Outcome::Fallback || st.plan.outcome == Solver::Outcome::NoPath)
                    ++st.counters.lateFallbacks;
            }
            if (arrived)
            {
                // A route's escape ends where stock follow takes over (§3.8): the route-loop counter's handoff (§3.7).
                if (!st.plan.waypoints.empty() && !st.regionRoute)
                {
                    st.routeHandoffAt = now;
                    st.routeHandoffGoal = st.routeGoal;
                }
                st.mode = MechanicBotMode::Hold;
                st.outOfRoleSince = Milliseconds(0);
                return;
            }

            // Hard ceilings: never block heals and following for good.
            if (now >= st.escapeCeilingAt || now - st.escapeChainSince >= kEscapeChainCap)
            {
                Release(&reg, bot, st, now, cfg, false, "ceiling");
                return;
            }

            bool resolve = false;
            // The view changed (new revision, moving anchors, perception, the board): is the rest of the way still
            // clean? A plan from the reduced-margin pass (or a route leg that needed it) is checked at those margins.
            if (st.viewRebuilt)
            {
                Solver::Input reduced;
                if (st.planReduced)
                    reduced = Solver::ReducedMargins(in);
                std::vector<Geo::Vec3> const rest = RemainingPath(st.plan.path, pos);
                Solver::PathCheck const pc = CheckMotion(st.planReduced ? reduced : in, rest);
                // A route's end only has to be hazard-safe (stock follow takes over there); a solve's target, and a
                // region route's end, must pass TargetOk.
                bool const endOk = st.plan.waypoints.empty() || st.regionRoute
                    ? Solver::TargetOk(in, st.plan.target, pc.arriveAt, std::max(pc.arriveAt, horizonEnd))
                    : Solver::PointSafe(in, st.plan.target, pc.arriveAt, std::max(pc.arriveAt, horizonEnd));
                resolve = !pc.ok || !endOk;
                // M4 §3.7: a clear Fallback never passed the margins, so it failed this check on every rebuild and
                // was solved again, nearly always to another Fallback, and the turn-arounds cost hits (the barrage
                // crowd, STAGE-M4-P-REPORT §3). It walks on while the rest of its way stays out of the exact
                // hitboxes and its end stays clear; a hot one is still solved again.
                if (resolve && Solver::FallbackHolds(in, st.plan, rest, horizonEnd, true))
                {
                    resolve = false;
                    ++st.counters.fallbackKept;
                }
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
                Release(&reg, bot, st, now, cfg, true, "cannot move");
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
            Solver::Plan plan = RunSolve(reg, bot, st, now, cfg, ctx);
            if (plan.outcome == Solver::Outcome::Safe)
            {
                // The spot the bot reached is safe: hold it for what the old plan answered.
                st.plan.target = pos;
                st.plan.path = { pos };
                st.plan.waypoints.clear();
                st.plan.hasFacing = plan.hasFacing;
                st.plan.facing = plan.facing;
                st.plan.holdUntil = std::max(now, holdUntil);
                st.mode = MechanicBotMode::Hold;
                st.outOfRoleSince = Milliseconds(0);
                st.pendingEscape = false;
                return;
            }
            if (stuck && Dist2d(plan.target, oldTarget) <= kSameTargetYd)
            {
                // The same target again (off the navmesh, a partial path): nothing new to try. Give the bot back.
                Release(&reg, bot, st, now, cfg, false, "stuck on the same target");
                return;
            }
            BeginEscape(st, std::move(plan), now);
        }

        // Region tracking (§3.8): a hard region on a moving anchor, the bot within TrackSlackYd of its inner edge ->
        // a role solve (the spot competes; the region comfort score pulls the bot back in), at most once per
        // MovingAnchorRecheckMs.
        bool NeedsTracking(MechanicBotState const& st, Milliseconds now, BotConfig const& cfg)
        {
            Solver::Input const& in = st.input;
            if (!st.movingAnchors || now - st.lastTrackAt < Milliseconds(cfg.movingAnchorRecheckMs))
                return false;
            for (Solver::Region const& r : in.regions)
            {
                if (!r.hard || !Solver::Detail::RegionLive(in, r))
                    continue;
                if (Solver::Detail::RegionSd(r.shape, r.poses, in.bot) > -(r.marginYd + cfg.trackSlackYd))
                    return true;
            }
            return false;
        }

        // M4 WP-Q (G5): the out-of-band clock, sampled once per brain step with the mode the bot is in now (the
        // waker's mode clocks' rule; a gap over 2 s is not counted).
        void TickBandClock(MechanicBotState& st, Milliseconds now)
        {
            Milliseconds const last = st.bandClockAt;
            st.bandClockAt = now;
            if (last == Milliseconds(0) || now <= last || now - last > Milliseconds(2000))
                return;
            bool const locked = st.mode == MechanicBotMode::Escape || st.mode == MechanicBotMode::Hold
                || st.mode == MechanicBotMode::Freeze;
            Solver::RoleBand const& role = st.input.role;
            if (!locked || !role.present)
                return;
            float const d = Dist2d(st.input.bot, role.anchor);
            if (d >= role.minYd && d <= role.maxYd)
                return;
            uint32 const ms = static_cast<uint32>((now - last).count());
            st.counters.msOutOfBand += ms;
            if (TaskHoldUntil(st, now) > now)
                st.counters.msOutOfBandTask += ms;
        }

        void StepHold(Registry& reg, PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Milliseconds now,
            BotConfig const& cfg, ViewCtx& ctx)
        {
            Solver::Input& in = st.input;
            if (now >= st.plan.holdUntil)
            {
                st.plan = Solver::Plan();
                st.planFor.clear();
                st.outOfRoleSince = Milliseconds(0);
                StepWatch(reg, botAI, bot, st, now, cfg, ctx);
                return;
            }
            if (!Solver::TargetOk(in, in.bot, now, now + in.params.horizon))
            {
                if (CanSolve(bot, st, now, cfg))
                    SolveAndApply(reg, bot, st, now, cfg, ctx);
                return;
            }
            if (NeedsTracking(st, now, cfg) && TakeBudget(bot, now))
            {
                st.lastTrackAt = now;
                Solver::Plan plan = RunSolve(reg, bot, st, now, cfg, ctx, true);
                if (plan.outcome == Solver::Outcome::Move || plan.outcome == Solver::Outcome::GraceMove)
                {
                    plan.holdUntil = std::max(plan.holdUntil, st.plan.holdUntil);
                    BeginEscape(st, std::move(plan), now);
                    return;
                }
            }
            // A task hold never gives the spot back for the role band: a region the bot must stay in, a carrier's
            // clear spot, the tank during a warning (§3.8 tracking: "Hold never clears a region the bot must stay in").
            if (TaskHoldUntil(st, now) > now)
            {
                st.outOfRoleSince = Milliseconds(0);
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

        // HoldStill (§3.8, decision §7.4): from freeze.from - settle to freeze.to + SafetyMarginMs the bot does not
        // move at all; Solve is not even asked. Released bots are never frozen. true = the bot is (still) frozen.
        // The latch (freezeUntil) covers only the natural margin after the window: before the window's own end the
        // HoldStill must still be there (`.mech clear`, the boss died, a map change: STAGE-M3-R-REPORT m-1).
        bool StepFreeze(Registry const& reg, PlayerbotAI* botAI, Player* bot, MechanicBotState& st, Milliseconds now,
            BotConfig const& cfg)
        {
            Solver::Input const& in = st.input;
            bool const window = in.freeze.present && now >= in.freeze.from - in.freeze.settle && now <= in.freeze.to;
            bool const released = BotBrain::Released(botAI, st, now);
            if (window && !released)
            {
                st.freezeUntil = std::max(st.freezeUntil, in.freeze.to + in.params.marginMs);
                st.freezeTo = std::max(st.freezeTo, in.freeze.to);
            }
            if (st.mode == MechanicBotMode::Freeze)
            {
                bool listed = false;
                for (uint32 id : st.planFor)
                    listed = listed || reg.Find(id) != nullptr;
                bool const cleared = now < st.freezeTo && (!in.freeze.present || !listed);
                if (now <= st.freezeUntil && !released && !cleared)
                {
                    st.plan.holdUntil = st.freezeUntil;
                    return true;
                }
                if (cleared && cfg.debug)
                    LOG_INFO(kLog, "Bot {}: unfrozen, its HoldStill is gone", bot->GetName());
                st.mode = MechanicBotMode::Watch;
                st.plan = Solver::Plan();
                st.planFor.clear();
                st.freezeUntil = Milliseconds(0);
                st.freezeTo = Milliseconds(0);
                return false;
            }
            if (!window || released)
                return false;
            st.mode = MechanicBotMode::Freeze;
            st.plan = Solver::Plan();
            st.plan.target = in.bot;
            st.plan.path = { in.bot };
            st.plan.arriveAt = now;
            st.plan.holdUntil = st.freezeUntil;
            st.planFor.clear();
            for (BotTask const& t : st.viewInfo.tasks)
                if (t.role == BotTaskRole::Frozen)
                    st.planFor.push_back(t.mechanicId);
            st.plan.answers = st.planFor;
            st.pendingEscape = false;
            st.routePending = false;
            st.waypointIdx = 0;
            st.outOfRoleSince = Milliseconds(0);
            ++st.counters.freezes;
            if (cfg.debug)
                LOG_INFO(kLog, "Bot {}: frozen until {} ms from now (HoldStill {})", bot->GetName(),
                    (st.freezeUntil - now).count(), IdList(st.planFor));
            return true;
        }

        // Look-away: a facing that moved goes onto the board (allies plan against the cone at the reserved facing).
        void PublishLookAway(Registry& reg, Player* bot, MechanicBotState& st, Milliseconds now)
        {
            if (!st.lookOwed)
            {
                st.lookPublished = false;
                return;
            }
            if (st.viewInfo.tasks.empty())
                return;
            if (st.lookPublished && std::fabs(Geo::WrapPi(st.lookFacing - st.lookPublishedFacing))
                <= Geo::DegToRad(kRepublishFacingDeg))
                return;
            bool const planned = st.mode == MechanicBotMode::Escape || st.mode == MechanicBotMode::Hold
                || st.mode == MechanicBotMode::Freeze;
            Solver::Plan p;
            p.outcome = Solver::Outcome::Safe;
            p.target = planned ? st.plan.target : st.input.bot;
            p.hasFacing = true;
            p.facing = st.lookFacing;
            PublishReservations(reg, bot, st.viewInfo, p, now);
            st.reserved = true;
            st.lookPublished = true;
            st.lookPublishedFacing = st.lookFacing;
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
            case MechanicBotMode::Freeze:
                return "freeze";
        }
        return "?";
    }

    void MechanicBotState::Reset()
    {
        MechanicBotCounters keptCounters = std::move(counters);
        CostStats keptCost = std::move(cost);
        bool const keptAvoidAoe = avoidAoeByGate;
        *this = MechanicBotState();
        counters = std::move(keptCounters);
        cost = std::move(keptCost);
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
        if (!bot || !bot->IsInWorld() || !bot->GetMap())
        {
            ToIdle(st);
            return;
        }
        BrainTimer const timer(st, bot, now);
        // No negative cache (STAGE-M2-REVIEW m4): Get is one DataMap look-up, and a cached "no registry" delayed
        // the first mechanic of every map by up to its lifetime.
        Registry* reg = Registry::Get(bot->GetMap());
        if (!bot->IsAlive())
        {
            DropBoard(reg, bot, st);
            ToIdle(st);
            return;
        }
        if (!reg)
        {
            st.viewValid = false;
            st.reserved = false;
            ToIdle(st);
            return;
        }

        BotConfig const& cfg = GetConfig().bots;
        ViewCtx ctx(bot, st, *reg, now, cfg);
        RefreshView(bot, st, *reg, now, cfg, ctx);
        Solver::Input& in = st.input;
        if (st.reserved && st.viewInfo.tasks.empty())
            DropBoard(reg, bot, st);
        BotRole::UpdateFrontalSources(bot, *reg, st, cfg);
        bool const relevant = AnyRelevant(in) || AnyTask(st, now) || now <= st.freezeUntil
            || (cfg.tankFacingEnable && !st.frontalSources.empty());
        if (!relevant)
        {
            DropBoard(reg, bot, st);
            ToIdle(st);
            return;
        }
        in.role = BotRole::RoleBandOf(botAI, bot, cfg);
        TickBandClock(st, now);
        BotRole::FillSightReach(bot, cfg, in);
        BotRole::FillPark(bot, *reg, st, cfg, now, in);
        st.tankWarning = HasRole(st, BotTaskRole::Tank, now);
        if (StepFreeze(*reg, botAI, bot, st, now, cfg))
        {
            st.leaveBy = Solver::kForever;     // frozen: nothing to leave for (decision §7.4)
            BotFacing::UpdateLookAway(bot, st, now);
            PublishLookAway(*reg, bot, st, now);
            return;
        }
        st.leaveBy = LeaveByAll(in);

        // Decision §7.5 (STAGE-M3-R-REPORT M-2): a Frontal warning started while the tank walks round its source to
        // turn it: stop on the spot and hold, so the boss does not turn during the warning. The hold action stops the
        // spline. A Zone the tank is not exempt from still makes it move (the hold re-checks its spot).
        if (st.tankWarning && st.refacing && st.mode == MechanicBotMode::Escape)
        {
            std::vector<uint32> ids;
            Milliseconds const until = std::max(TaskHoldUntil(st, now, &ids), now + kMinMotionHold);
            HoldForTasks(st, now, until, std::move(ids));
            st.refacing = false;
            ++st.counters.refacingStops;
            if (cfg.debug)
                LOG_INFO(kLog, "Bot {}: a Frontal warning started, the re-facing walk stops here", bot->GetName());
            BotFacing::UpdateLookAway(bot, st, now);
            PublishLookAway(*reg, bot, st, now);
            return;
        }

        // §3.8 route search: a brain step after the failed re-route, never the same one. A route that was due and
        // took its budget unit ends the step whatever it found (never a route and a solve in one step,
        // STAGE-M3-R-REPORT m-2).
        if (st.routePending && now > st.routeAskedAt)
        {
            st.routePending = false;
            bool const due = st.mode == MechanicBotMode::Hold && cfg.routeEnable
                && now - st.lastRouteAt >= Milliseconds(cfg.routeRetryMs)
                && !BotBrain::Released(botAI, st, now)
                && Solver::PointSafe(in, st.routeGoal, now, now + in.params.horizon);
            if (due && TakeBudget(bot, now))
            {
                RunRoute(bot, st, now, cfg);
                BotFacing::UpdateLookAway(bot, st, now);
                PublishLookAway(*reg, bot, st, now);
                return;
            }
        }

        // M4 WP-Q (G2): the region route a Fallback asked for, on a later step, Route.RetryMs apart, one budget unit,
        // while the bot still walks or holds that Fallback; like the route search, never with a solve in one step.
        if (st.regionRoutePending && now > st.regionRouteAskedAt)
        {
            st.regionRoutePending = false;
            bool const fallback = st.plan.outcome == Solver::Outcome::Fallback
                || st.plan.outcome == Solver::Outcome::NoPath;
            bool const due = cfg.routeEnable && fallback
                && (st.mode == MechanicBotMode::Escape || st.mode == MechanicBotMode::Hold)
                && now - st.lastRegionRouteAt >= Milliseconds(cfg.routeRetryMs)
                && !BotBrain::Released(botAI, st, now);
            if (due && TakeBudget(bot, now))
            {
                RunRegionRoute(*reg, bot, st, now, cfg);
                BotFacing::UpdateLookAway(bot, st, now);
                PublishLookAway(*reg, bot, st, now);
                return;
            }
        }

        switch (st.mode)
        {
            case MechanicBotMode::Idle:
            case MechanicBotMode::Watch:
            case MechanicBotMode::Freeze:
                StepWatch(*reg, botAI, bot, st, now, cfg, ctx);
                break;
            case MechanicBotMode::Escape:
                StepEscape(*reg, botAI, bot, st, now, cfg, ctx);
                break;
            case MechanicBotMode::Hold:
                StepHold(*reg, botAI, bot, st, now, cfg, ctx);
                break;
        }
        BotFacing::UpdateLookAway(bot, st, now);
        PublishLookAway(*reg, bot, st, now);
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

        BrainTimer const timer(st, bot, now);
        BotConfig const& cfg = GetConfig().bots;
        ViewCtx ctx(bot, st, reg, now, cfg);
        RefreshView(bot, st, reg, now, cfg, ctx);
        BotFacing::UpdateLookAway(bot, st, now);
        if (!AnyRelevant(st.input) && !AnyTask(st, now))
            return Solver::kForever;
        // Frozen (or about to be): the bot cannot leave anyway, and casting is what it may do (decision §7.4).
        Solver::Input const& in = st.input;
        bool const frozen = st.mode == MechanicBotMode::Freeze
            || (in.freeze.present && now >= in.freeze.from - in.freeze.settle && now <= in.freeze.to);
        st.leaveBy = frozen ? Solver::kForever : LeaveByAll(st.input);
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

    uint32 BotBrain::TakeMapBrainUs(uint32 mapId, uint32 instanceId, Milliseconds now)
    {
        MapBrainTime& t = tMapBrainTime;
        if (t.mapId != mapId || t.instanceId != instanceId || t.at != now)
            return 0;
        uint64 const us = t.us;
        t.us = 0;
        return static_cast<uint32>(std::min<uint64>(us, std::numeric_limits<uint32>::max()));
    }

    bool BotBrain::TankOwned(MechanicBotState const& st)
    {
        return GetConfig().bots.tankFacingEnable && !st.frontalSources.empty();
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

    Geo::Vec3 BotBrain::MoveTarget(MechanicBotState const& st)
    {
        if (st.plan.waypoints.empty())
            return st.plan.target;
        return st.plan.waypoints[std::min(st.waypointIdx, st.plan.waypoints.size() - 1)];
    }
}

#endif
