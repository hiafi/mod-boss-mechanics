/*
 * mod-boss-mechanics: the "boss mechanics" bot strategy, its triggers and actions (STAGE-M2-HANDOFF §3.4, WP-C).
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotStrategy.h"
#include "GameTime.h"
#include "MechanicBotBrain.h"
#include "MechanicBotFacing.h"
#include "MechanicBotLock.h"
#include "MechanicBotState.h"
#include "MechanicLeaks.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

using namespace BossMechanics;

namespace
{
    // The spline under way ends this close to the plan's target: the move is ours and still current.
    constexpr float kSameTargetYd = 1.f;
    // A move to the current target is re-sent at most this often: after a refused send, a partial path (a spline
    // that ends short of the target) or a finished spline short of it. The brain's stuck clock does not depend on
    // sends (STAGE-M2-REVIEW M1); it re-solves or releases the bot after 500 ms without progress.
    constexpr Milliseconds kResendAfter{ 250 };

    Milliseconds Now()
    {
        return GameTime::GetGameTimeMS();
    }

    // Runs the brain for this tick (idempotent) and returns the state, or nullptr when the glue is not registered.
    MechanicBotState* Think(PlayerbotAI* botAI, MechanicBotStateRef& ref)
    {
        MechanicBotState* st = ref.Get(botAI);
        if (st)
            BotBrain::Update(botAI, *st, Now());
        return st;
    }

    // Nothing to send this tick: the last attempt went to the current target and either its spline is under way to
    // it, or the attempt is younger than kResendAfter.
    bool MovingToTarget(Player const* bot, MechanicBotState const& st)
    {
        if (st.escapeSentAt == Milliseconds(0))
            return false;
        Geo::Vec3 const t = BotBrain::MoveTarget(st);
        if (Geo::Dist2d(st.issuedTarget.x, st.issuedTarget.y, t.x, t.y) > 0.1f)
            return false;   // re-planned since the last attempt
        if (st.escapeIssuedAt != Milliseconds(0))
        {
            Movement::MoveSpline const* ms = bot->movespline;
            if (ms && ms->Initialized() && !ms->Finalized())
            {
                G3D::Vector3 const end = ms->FinalDestination();
                if (Geo::Dist2d(end.x, end.y, t.x, t.y) <= kSameTargetYd)
                    return true;
            }
        }
        return Now() - st.escapeSentAt < kResendAfter;
    }
}

MechanicBotState* MechanicBotStateRef::Get(PlayerbotAI* botAI)
{
    if (!_state)
        _state = BotBrain::StateOf(botAI);
    return _state;
}

void MechanicBotStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("boss mechanic reposition",
        { NextAction("boss mechanic move", ACTION_EMERGENCY + 6) }));
    triggers.push_back(new TriggerNode("boss mechanic hold",
        { NextAction("boss mechanic hold", ACTION_EMERGENCY + 5) }));
    triggers.push_back(new TriggerNode("boss mechanic face",
        { NextAction("boss mechanic face", ACTION_EMERGENCY + 4) }));
}

void MechanicBotStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new MechanicBotLockMultiplier(botAI));
}

bool MechanicBotRepositionTrigger::IsActive()
{
    MechanicBotState* st = Think(botAI, _state);
    return st && st->mode == MechanicBotMode::Escape && !MovingToTarget(bot, *st);
}

bool MechanicBotHoldTrigger::IsActive()
{
    MechanicBotState* st = Think(botAI, _state);
    return st && (st->mode == MechanicBotMode::Hold || st->mode == MechanicBotMode::Freeze)
        && BotBrain::HasForeignMotion(bot);
}

bool MechanicBotFaceTrigger::IsActive()
{
    MechanicBotState* st = Think(botAI, _state);
    return st && st->faceOwed && BotFacing::FacingOff(bot, st->faceAngle);
}

bool MechanicBotMoveAction::isUseful()
{
    MechanicBotState* st = _state.Get(botAI);
    return st && st->mode == MechanicBotMode::Escape;
}

bool MechanicBotMoveAction::Execute(Event /*event*/)
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st || st->mode != MechanicBotMode::Escape)
        return false;

    // MoveTo refuses a move while the last one's wait has not elapsed at the same or a higher priority, and a
    // repeat of the same point (§2.2). The escape outranks every pending wait, ours (a re-plan) or a foreign
    // MOVEMENT_FORCED hold, so both timers are cleared first.
    LastMovement& last = AI_VALUE(LastMovement&, "last movement");
    last.msTime = 0;
    last.lastdelayTime = 0;

    // Route (M3): the current waypoint; each leg is the oracle-validated one, so MoveTo paths it the same way.
    Geo::Vec3 const target = BotBrain::MoveTarget(*st);
    Milliseconds const now = Now();
    st->escapeSentAt = now;
    st->issuedTarget = target;
    // The stuck clock (stuckRef, stuckSince) is the brain's and is not touched here: a send is not progress.
    if (!MoveTo(bot->GetMapId(), target.x, target.y, target.z, false, false, false, false,
        MovementPriority::MOVEMENT_FORCED))
        return false;   // rooted, stunned, no path: retried after kResendAfter; the brain re-solves or releases

    st->escapeIssuedAt = now;
    // M4 leak detectors (MechanicLeaks.h): the spline this MoveTo launched is ours.
    Movement::MoveSpline const* ms = bot->movespline;
    if (ms && ms->Initialized() && !ms->Finalized())
    {
        G3D::Vector3 const end = ms->FinalDestination();
        Leaks::OwnSpline(st->leak, ms->GetId(), end.x, end.y);
    }
    return true;
}

bool MechanicBotHoldAction::isUseful()
{
    MechanicBotState* st = _state.Get(botAI);
    return st && (st->mode == MechanicBotMode::Hold || st->mode == MechanicBotMode::Freeze);
}

bool MechanicBotHoldAction::Execute(Event /*event*/)
{
    // Feared, charmed, charging, knocked back: not ours to cancel.
    if (bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE)
        return false;
    bot->StopMoving();
    // Also drops a persistent MoveFollow (stock follow, mod-dungeon-clear's follow-tank).
    bot->GetMotionMaster()->Clear();
    // Look-away (M3): the chosen facing, not the victim (the face action keeps it).
    MechanicBotState* st = _state.Get(botAI);
    if (st && st->faceOwed)
    {
        if (BotFacing::FacingOff(bot, st->faceAngle))
        {
            BotFacing::Apply(bot, st->faceAngle);
            Leaks::OwnFacing(st->leak, st->faceAngle);     // M4: our turn, not a leak
        }
        return true;
    }
    if (Unit* face = BotBrain::FacingTarget(bot))
        if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, face))
        {
            bot->SetFacingToObject(face);
            if (st)
                Leaks::OwnFacing(st->leak, bot->GetAngle(face));
        }
    return true;
}

bool MechanicBotFaceAction::isUseful()
{
    MechanicBotState* st = _state.Get(botAI);
    // A move turns the bot along its spline: the facing is applied once it stands.
    return st && st->faceOwed && !(bot->movespline && bot->movespline->Initialized() && !bot->movespline->Finalized());
}

bool MechanicBotFaceAction::Execute(Event /*event*/)
{
    MechanicBotState* st = _state.Get(botAI);
    if (!st || !st->faceOwed || !BotFacing::FacingOff(bot, st->faceAngle))
        return false;
    BotFacing::Apply(bot, st->faceAngle);
    Leaks::OwnFacing(st->leak, st->faceAngle);     // M4: our turn, not a leak
    ++st->counters.facingsApplied;
    return true;
}

#endif
