/*
 * mod-boss-mechanics: look-away facing (STAGE-M3-HANDOFF §3.7, decision §7.6, WP-C).
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotFacing.h"
#include "Player.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include <algorithm>
#include <cmath>

namespace BossMechanics
{
    void BotFacing::UpdateLookAway(Player const* bot, MechanicBotState& st, Milliseconds now)
    {
        st.lookOwed = false;
        st.lookWindow = false;
        st.faceOwed = false;
        if (!st.viewValid || !bot)
            return;
        Solver::Input const& in = st.input;
        Milliseconds const margin = in.params.marginMs;
        Milliseconds from = Solver::kForever;
        Milliseconds to{ 0 };
        for (Solver::Carried const& c : in.carried)
        {
            if (!Solver::Detail::CarriedLive(in, c) || !Solver::Detail::ChoosesFacing(c))
                continue;
            st.lookOwed = true;
            from = std::min(from, c.timing.from - margin);
            to = std::max(to, Solver::Detail::TimingEnd(c.timing) + margin);
        }
        if (!st.lookOwed)
            return;
        st.lookFrom = from;
        st.lookTo = to;
        st.lookWindow = now >= from && now <= to;

        // On the way to a target: the facing the solver chose there. Standing: the free facing here, else the plan's.
        float f = 0.f;
        if (st.mode == MechanicBotMode::Escape && st.plan.hasFacing)
            st.lookFacing = st.plan.facing;
        else if (Solver::CarriedOk(in, in.bot, &f))
            st.lookFacing = f;
        else if (st.plan.hasFacing)
            st.lookFacing = st.plan.facing;
        else
            st.lookFacing = bot->GetOrientation();

        // A move turns the bot along its spline: the facing is applied once it stands.
        st.faceOwed = st.lookWindow && st.mode != MechanicBotMode::Escape;
        st.faceAngle = st.lookFacing;
    }

    bool BotFacing::LookAwayActive(MechanicBotState const& st, Milliseconds now)
    {
        return st.lookOwed && now >= st.lookFrom && now <= st.lookTo;
    }

    bool BotFacing::InArc(Player const* bot, float facing, WorldObject const* target)
    {
        if (!bot || !target || target == bot)
            return true;
        float const dx = target->GetPositionX() - bot->GetPositionX();
        float const dy = target->GetPositionY() - bot->GetPositionY();
        if (dx * dx + dy * dy < 1e-4f)
            return true;
        float const off = std::fabs(Geo::WrapPi(std::atan2(dy, dx) - facing));
        return off <= CAST_ANGLE_IN_FRONT * 0.5f;
    }

    bool BotFacing::WouldTurnAway(Player const* bot, MechanicBotState const& st, Milliseconds now, Unit const* target)
    {
        if (!LookAwayActive(st, now) || !target)
            return false;
        return !InArc(bot, st.lookFacing, target);
    }

    bool BotFacing::TurnsCaster(SpellInfo const* info)
    {
        return info && (info->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT);
    }

    bool BotFacing::CastBreaksLookAway(Player const* bot, MechanicBotState const& st, Spell const* spell,
        Milliseconds now, Milliseconds remaining)
    {
        if (!spell || !st.lookOwed || now > st.lookTo)
            return false;
        if (remaining < Solver::kForever && now + remaining < st.lookFrom)
            return false;   // ends before the window opens
        if (!TurnsCaster(spell->GetSpellInfo()))
            return false;
        Unit const* target = spell->m_targets.GetUnitTarget();
        return target && target != bot && !InArc(bot, st.lookFacing, target);
    }

    bool BotFacing::FacingOff(Player const* bot, float angle)
    {
        return std::fabs(Geo::WrapPi(bot->GetOrientation() - angle)) > Geo::DegToRad(kFaceToleranceDeg);
    }

    void BotFacing::Apply(Player* bot, float angle)
    {
        bot->SetOrientation(Position::NormalizeOrientation(angle));
        if (!bot->IsRooted())
            bot->SendMovementFlagUpdate(true);
    }
}

#endif
