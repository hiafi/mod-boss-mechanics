/*
 * mod-boss-mechanics: role constraints v2 and the tank tasks' inputs (STAGE-M3-HANDOFF §3.7, §3.8, WP-C).
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotRole.h"
#include "BossMechanicsConfig.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "MechanicBotBrain.h"
#include "MechanicRegistry.h"
#include "MechanicViewRules.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include <algorithm>
#include <cmath>

namespace BossMechanics
{
    namespace
    {
        // Role band (M2's numbers, §3.4 of the M2 handoff).
        constexpr float kMeleeSlackYd = 0.5f;
        constexpr float kRangedMinYd = 8.f;
        constexpr float kRangedExtraYd = 6.f;
        constexpr float kFollowExtraYd = 2.f;
        // Healer Reach: members within HealDistance - kReachSlackYd; each yard beyond costs kReachWeightPerYd.
        constexpr float kReachSlackYd = 2.f;
        constexpr float kReachWeightPerYd = 1.f;
        // Tank facing ring: bearings every kRingStepDeg, radius kept kRingInsetYd inside [stop, melee range].
        constexpr float kRingStepDeg = 15.f;
        constexpr float kRingInsetYd = 0.5f;
        // Tank facing: oracle path calls per re-facing search.
        constexpr uint32 kRefacePathChecks = 3;
        // Drag-out: the victim stands inside, or within this of, a lasting hazard.
        constexpr float kDragNearYd = 3.f;
        // Drag-out: a hazard counts as lasting when it stays hot at least this much longer.
        constexpr Milliseconds kDragLasting{ 2000 };

        Geo::Vec3 PosOf(WorldObject const* o)
        {
            return { o->GetPositionX(), o->GetPositionY(), o->GetPositionZ() };
        }

        bool SameMap(Player const* a, Player const* b)
        {
            return a && b && a->IsInWorld() && b->IsInWorld() && a->GetMap() == b->GetMap();
        }

        // f(Player* member) for every other alive group member on the bot's map.
        template <class F>
        void ForEachMember(Player* bot, F&& f)
        {
            Group* group = bot->GetGroup();
            if (!group)
                return;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !SameMap(bot, member) || !member->IsAlive() || member->IsGameMaster())
                    continue;
                f(member);
            }
        }
    }

    Player* BotRole::NearestGroupTank(Player* bot)
    {
        Player* best = nullptr;
        float bestDist = 0.f;
        ForEachMember(bot, [&](Player* member)
        {
            if (!PlayerbotAI::IsTank(member))
                return;
            float const d = bot->GetExactDist2d(member);
            if (!best || d < bestDist)
            {
                best = member;
                bestDist = d;
            }
        });
        return best;
    }

    Solver::RoleBand BotRole::RoleBandOf(PlayerbotAI* botAI, Player* bot, BotConfig const& cfg)
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

    void BotRole::FillSightReach(Player* bot, BotConfig const& cfg, Solver::Input& in)
    {
        in.sight = Solver::Sight();
        in.reach = Solver::Reach();
        if (!bot->IsInCombat() || !PlayerbotAI::IsHeal(bot))
            return;
        Player* tank = NearestGroupTank(bot);
        in.sight.maxChecks = cfg.healerMaxSightChecks;
        if (cfg.healerMaxSightChecks && cfg.healerSightWeight > 0.f)
        {
            if (tank)
            {
                in.sight.points.push_back(PosOf(tank));
                in.sight.weights.push_back(cfg.healerSightWeight);
            }
            ForEachMember(bot, [&](Player* member)
            {
                if (member == tank)
                    return;
                in.sight.points.push_back(PosOf(member));
                in.sight.weights.push_back(cfg.healerSightWeight * 0.5f);
            });
        }
        in.reach.maxYd = std::max(0.f, sPlayerbotAIConfig.HealDistance - kReachSlackYd);
        in.reach.weightPerYd = kReachWeightPerYd;
        ForEachMember(bot, [&](Player* member) { in.reach.points.push_back(PosOf(member)); });
    }

    bool BotRole::PlannedSpotOf(Player const* member, Geo::Vec3& spot, float& facing)
    {
        if (!member)
            return false;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(const_cast<Player*>(member));
        if (!ai || IsSelfBot(const_cast<Player*>(member)))
            return false;
        MechanicBotState const* st = BotBrain::StateOf(ai);
        if (!st)
            return false;
        bool const planned = st->mode == MechanicBotMode::Escape || st->mode == MechanicBotMode::Hold
            || st->mode == MechanicBotMode::Freeze;
        // No plan and no facing to hold: the view places it at its current spot with drift slack.
        if (!planned && !st->lookOwed)
            return false;
        spot = planned ? st->plan.target : PosOf(member);
        facing = st->lookOwed ? st->lookFacing : (st->plan.hasFacing ? st->plan.facing : member->GetOrientation());
        return true;
    }

    float BotRole::ChaseStopYd(Unit const* chaser, Unit const* target)
    {
        // TargetedMovementGenerator's GetChaseRange (hover ignored: bosses that chase on foot) + CONTACT_DISTANCE.
        return chaser->GetCombatReach() + target->GetCombatReach() + CONTACT_DISTANCE;
    }

    void BotRole::UpdateFrontalSources(Player* bot, Registry const& reg, MechanicBotState& st, BotConfig const& cfg)
    {
        if (!cfg.tankFacingEnable || !bot->IsInCombat())
        {
            st.frontalSources.clear();
            return;
        }
        // Learn: every tank-exempt shape whose exempt victim is the bot, sourced on a creature, that faces the victim
        // (a rear or side shape needs no facing: STAGE-M3-R-REPORT M-1). Every distinct front shape is kept.
        for (uint32 id : st.viewInfo.tankFrontals)
        {
            ActiveMechanic const* m = reg.Find(id);
            if (!m || !m->anchor.IsUnit() || !m->anchor.unit.IsCreatureOrVehicle() || !ViewRules::FacesVictim(m->shape))
                continue;
            auto it = std::find_if(st.frontalSources.begin(), st.frontalSources.end(),
                [&](MechanicBotState::FrontalSource const& f) { return f.source == m->anchor.unit; });
            if (it == st.frontalSources.end())
                it = st.frontalSources.insert(st.frontalSources.end(), MechanicBotState::FrontalSource());
            it->source = m->anchor.unit;
            ViewRules::RememberFrontShape(it->shapes, m->shape);
            it->marginYd = cfg.safetyMarginYd;
        }
        // Forget: the source is gone, dead, or no longer on the bot.
        std::erase_if(st.frontalSources, [&](MechanicBotState::FrontalSource const& f)
        {
            Creature const* c = ObjectAccessor::GetCreature(*bot, f.source);
            return f.shapes.empty() || !c || !c->IsAlive() || c->GetVictim() != bot;
        });
    }

    bool BotRole::FrontalHitsGroup(Player* bot, Unit const* source, MechanicBotState::FrontalSource const& f,
        Geo::Vec3 const& tankAt, float facingSlackDeg)
    {
        std::vector<Geo::Vec3> spots;
        ForEachMember(bot, [&](Player* member)
        {
            Geo::Vec3 spot = PosOf(member);
            float facing = 0.f;
            PlannedSpotOf(member, spot, facing);
            spots.push_back(spot);
        });
        return ViewRules::FrontalHits(f.shapes, PosOf(source), tankAt, spots, f.marginYd, facingSlackDeg);
    }

    bool BotRole::TankRefacePlan(Player* bot, Unit const* source, MechanicBotState::FrontalSource const& f,
        Solver::Input const& in, float facingSlackDeg, Solver::PathOracle& oracle, Solver::Plan& out)
    {
        Geo::Vec3 const s = PosOf(source);
        float const lo = ChaseStopYd(source, bot) + kRingInsetYd;
        float const hi = std::max(lo, source->GetMeleeRange(bot) - kRingInsetYd);
        float const r = std::clamp(Geo::Dist2d(s.x, s.y, in.bot.x, in.bot.y), lo, hi);
        float const here = std::atan2(in.bot.y - s.y, in.bot.x - s.x);
        int const steps = static_cast<int>(std::lround(360.f / kRingStepDeg));
        Milliseconds const horizonEnd = in.now + in.params.horizon;
        uint32 checks = 0;
        // Nearest bearings first: +step, -step, +2 step, ...
        for (int k = 1; k <= steps / 2 && checks < kRefacePathChecks; ++k)
        {
            for (int sign : { 1, -1 })
            {
                float const b = here + sign * k * Geo::DegToRad(kRingStepDeg);
                Geo::Vec3 p{ s.x + std::cos(b) * r, s.y + std::sin(b) * r, in.bot.z };
                if (!oracle.Walkable(p))
                    continue;
                if (FrontalHitsGroup(bot, source, f, p, facingSlackDeg))
                    continue;
                float const travelYd = Geo::Dist2d(in.bot.x, in.bot.y, p.x, p.y);
                Milliseconds const arrival = in.now + in.params.startDelay
                    + Milliseconds(static_cast<int64>(travelYd / std::max(0.1f, in.speed) * 1000.f));
                if (!Solver::TargetOk(in, p, arrival, std::max(arrival, horizonEnd)))
                    continue;
                if (checks >= kRefacePathChecks)
                    return false;
                ++checks;
                std::vector<Geo::Vec3> path;
                if (!oracle.Find(in.bot, p, path) || path.empty())
                    continue;
                Solver::PathCheck const pc = Solver::CheckPath(in, path, in.now, false);
                if (!pc.ok)
                    continue;
                out = Solver::Plan();
                out.outcome = Solver::Outcome::Move;
                out.target = path.back();
                out.path = std::move(path);
                out.arriveAt = pc.arriveAt;
                out.holdUntil = pc.arriveAt;
                out.pathChecks = checks;
                return true;
            }
        }
        return false;
    }

    bool BotRole::FillPark(Player* bot, Registry const& reg, MechanicBotState& st, BotConfig const& cfg,
        Milliseconds now, Solver::Input& in)
    {
        in.park = Solver::Park();
        st.parking = false;
        if (!cfg.parkEnable || !bot->IsInCombat())
        {
            st.parkSince.clear();
            return false;
        }
        std::erase_if(st.parkSince, [&reg, now](auto const& kv)
        {
            ActiveMechanic const* m = reg.Find(kv.first);
            return !m || !m->IsActive(now);
        });
        Milliseconds const cap(cfg.parkHoldCapMs);
        auto capped = [&](uint32 key)
        {
            auto it = st.parkSince.try_emplace(key, now).first;
            return now - it->second > cap;
        };

        // A Park zone whose parked unit is the bot's victim.
        for (uint32 id : st.viewInfo.parks)
        {
            ActiveMechanic const* m = reg.Find(id);
            if (!m || !m->park || !m->IsActive(now))
                continue;
            Unit const* parked = ObjectAccessor::GetUnit(*bot, m->ParkedUnit());
            if (!parked || !parked->IsAlive() || parked->GetVictim() != bot || capped(id))
                continue;
            Geo::Pose pose;
            Shape const shape = reg.EffectiveShape(*m, pose);
            Solver::Park& pk = in.park;
            pk.present = true;
            pk.inside = true;
            pk.boss = PosOf(parked);
            pk.stopYd = ChaseStopYd(parked, bot);
            pk.triggerYd = parked->GetMeleeRange(bot);
            pk.depthYd = cfg.parkDepthYd;
            pk.zone.id = id;
            pk.zone.shape = shape;
            pk.zone.poses = { pose };
            pk.zone.marginYd = ViewRules::RegionMarginYd(shape, cfg.safetyMarginYd);
            pk.zone.from = m->resolveAt;
            pk.zone.to = m->expireAt;
            st.parking = true;
            return true;
        }

        // Drag-out: the bot's victim stands in (or next to) a lasting hazard the bot is eligible for.
        Unit* victim = bot->GetVictim();
        if (!victim || !victim->IsAlive() || victim->GetVictim() != bot || victim->IsPlayer())
            return false;
        Geo::Vec3 const v = PosOf(victim);
        Solver::Park& pk = in.park;
        uint32 key = 0;
        auto lastingNear = [&](Solver::Hazard const& h)
        {
            return !h.synthetic && h.timing.type == Solver::TimingType::Interval && h.timing.from <= now
                && h.timing.to >= now + kDragLasting && Solver::Detail::MinSd(h.shape, h.poses, v) <= kDragNearYd;
        };
        for (Solver::Hazard const& h : in.hazards)
        {
            if (!lastingNear(h))
                continue;
            pk.avoid.push_back(h);
            key = key ? key : h.id;
        }
        // Lasting zones the tank is not eligible for (a role=nontank puddle) are not in its view, but the melee at
        // the victim take them: drag out of those too (STAGE-M3-R-REPORT n-2).
        reg.ForEachActive(now, [&](ActiveMechanic const& m)
        {
            if (!m.IsShapeKind() || !m.persist || m.mine || reg.IsEligible(m, bot) || reg.EligiblePlayers(m).empty())
                return;
            Solver::Hazard h;
            h.id = m.id;
            Geo::Pose pose;
            h.shape = reg.EffectiveShape(m, pose);
            h.poses = { pose };
            h.marginYd = cfg.safetyMarginYd;
            h.timing = Solver::MakeInterval(m.resolveAt, m.persist->until);
            if (!lastingNear(h))
                return;
            pk.avoid.push_back(std::move(h));
            key = key ? key : m.id;
        });
        if (pk.avoid.empty() || capped(key))
        {
            pk = Solver::Park();
            return false;
        }
        pk.present = true;
        pk.inside = false;
        pk.boss = v;
        pk.stopYd = ChaseStopYd(victim, bot);
        pk.triggerYd = victim->GetMeleeRange(bot);
        pk.depthYd = cfg.parkDepthYd;
        st.parking = true;
        return true;
    }
}

#endif
