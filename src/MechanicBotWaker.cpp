/*
 * mod-boss-mechanics: the bot waker and the cast break (STAGE-M2-HANDOFF §3.6, WP-C).
 *
 * Installed with SetBotAfterTick; runs right after Registry::Tick on every full update of a map that has a registry,
 * on that map's update thread. Players (bots included) update on every world tick before it, so a bot woken here runs
 * its AI on the next world tick. Per bot with "boss mechanics" installed:
 *
 * 1. registry revision changed since the bot's view, or a mechanic hidden by fallibility becomes visible
 *    (nextPerceptionAt) -> SetNextCheckDelay(0);
 * 2. mode not Idle and FastTickMs since its last brain step -> SetNextCheckDelay(0). This replaces the 0.5-3 s react
 *    delay playerbots gives bots without a real master while a mechanic matters to the bot. The woken bot thinks on
 *    the next update, so the rule counts that update's gap in (LockRules::FastTickDue, WP-Q2): a 100 ms map update
 *    otherwise gives a brain step every 200 ms;
 * 3. casting or channelling: leaveBy is refreshed here (view only, no solve, no paths) and a cast or channel that
 *    would end after leaveBy - margin is interrupted (PlayerbotAI::RequestSpellInterrupt, consumed on the next AI
 *    tick), the bot woken, and cast-time spells and item uses locked for kNoCastAfterBreak. A released bot (stuck,
 *    rooted; MechanicBotBrain) is not interrupted; a frozen one (HoldStill, M3) has leaveBy = forever. M3 also
 *    breaks a SPELL_FACING_FLAG_INFRONT cast on a target outside the look-away arc when a look-away window opens
 *    before it ends (MechanicBotFacing).
 *
 * Only a cast in SPELL_STATE_PREPARING stops the bot's engine (PlayerbotAI::UpdateAI yields then). A channel is
 * SPELL_STATE_CASTING: the engine and the brain keep running through it, so the check here is redundant for channels
 * but harmless.
 *
 * Channels: Spell::m_timer (GetCastTimeRemaining) holds the remaining channel time; Spell::SendChannelStart sets it to
 * the duration and Spell::update counts it down in SPELL_STATE_CASTING (checked in Spell.cpp). A negative timer is an
 * endless channel.
 *
 * The playerbots reaction engine is not used: it runs on the same dynamic delay and needs its own install.
 *
 * M4 (STAGE-M4-HANDOFF §3.5, §3.6, WP-C), per bot and map update, before the waker acts:
 * - the mode clocks (MechanicBotCounters::msIdle .. msReleased): the time since the last sample goes to the mode the
 *   bot is in now (released first);
 * - the leak detectors (MechanicLeaks.h): a Sample of the bot's live state (spline, motion slots, position,
 *   orientation, the cast in progress) against the mode, leaveBy and look-away window the lock judged with in the
 *   bot's update just before; leaks go to leakMoves / leakCasts / leakTurns and leakSpells. BotOnSpellCast (the
 *   player cast hook) remembers the bot's own non-triggered casts, so a leaked move names its spell;
 * - after the loop, the brain time of every bot on the map since the last take (BotBrain::TakeMapBrainUs: this
 *   update's, plus any step outside a map update) goes to each bot's CostStats.
 *
 * Order (M4-SWEEP G8): the sample is taken after the bot's own AI step of this update. Live, playerbots runs it in
 * OnPlayerAfterUpdate inside Map::Update's player loop, before this hook, so the sample sees what that step did (our
 * own move included: hence the spline and facing tokens). The mod-dpssim mechanics mode ticks like live since M4
 * WP-F (STAGE-M4-R-REPORT M-4: it no longer ticks every bot's AI once more after Map::Update); the other sim modes
 * still do, and what that extra step does is sampled by the next update, after Unit::Update.
 *
 * M4 X6 (STAGE-M4-L-REPORT §2.3, WP-X2): a casting bot keeps the look-away facing. The engine of a bot in a
 * PREPARING cast yields, so the face action cannot turn it, and a ground cast left it turned to its point: under an
 * open window (not in Escape) the waker turns it to the look-away facing itself, unless the cast is one the M3 rule
 * breaks (an INFRONT cast on a target outside the arc, which UpdateAI would turn back). Channels too.
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotGate.h"
#include "BossMechanicsConfig.h"
#include "Log.h"
#include "Map.h"
#include "MechanicBotBrain.h"
#include "MechanicBotFacing.h"
#include "MechanicBotLock.h"
#include "MechanicLeaks.h"
#include "MechanicLockRules.h"
#include "MechanicRegistry.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace BossMechanics
{
    namespace
    {
        // Cast breaks closer together than this count once (the request stays set until the next AI tick).
        constexpr Milliseconds kCastBreakDedup{ 500 };
        // After a cast break the lock holds back cast-time spells and item uses this long (STAGE-M2-REVIEW m5).
        constexpr Milliseconds kNoCastAfterBreak{ 500 };

        // The cast or channel in progress and its remaining time; nullptr when the bot is not casting.
        Spell* CurrentCast(Player* bot, Milliseconds& remaining)
        {
            if (Spell* spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            {
                if (spell->getState() == SPELL_STATE_PREPARING)
                {
                    remaining = Milliseconds(std::max(0, spell->GetCastTimeRemaining()));
                    return spell;
                }
            }
            if (Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            {
                int32 const timer = channel->GetCastTimeRemaining();
                remaining = timer < 0 ? Solver::kForever : Milliseconds(timer);
                return channel;
            }
            return nullptr;
        }

        void Wake(PlayerbotAI* botAI, MechanicBotState& st)
        {
            botAI->SetNextCheckDelay(0);
            ++st.counters.wakes;
        }

        constexpr char const* kLog = "module.bossmechanics.bots";
        // Mode clocks: a gap longer than this between two samples (the bot was dead, out of the map, the strategy
        // off) is not counted.
        constexpr Milliseconds kMaxClockStep{ 2000 };

        // Saturating (STAGE-M4-R-REPORT nit 5): the clocks never reset live, and a uint32 of ms wraps after about 49
        // days of one bot's uptime; stuck at the top, `.mech bots` shows a full clock instead of a small one.
        void AddMs(uint32& clock, uint32 ms)
        {
            clock = ms > std::numeric_limits<uint32>::max() - clock ? std::numeric_limits<uint32>::max() : clock + ms;
        }

        void TickModeClock(MechanicBotState& st, Milliseconds now, bool released)
        {
            Milliseconds const last = st.clockAt;
            st.clockAt = now;
            if (last <= Milliseconds(0) || now <= last || now - last > kMaxClockStep)
                return;
            uint32 const ms = static_cast<uint32>((now - last).count());
            MechanicBotCounters& c = st.counters;
            if (released)
            {
                AddMs(c.msReleased, ms);
                return;
            }
            switch (st.mode)
            {
                case MechanicBotMode::Idle:
                    AddMs(c.msIdle, ms);
                    break;
                case MechanicBotMode::Watch:
                    AddMs(c.msWatch, ms);
                    break;
                case MechanicBotMode::Escape:
                    AddMs(c.msEscape, ms);
                    break;
                case MechanicBotMode::Hold:
                    AddMs(c.msHold, ms);
                    break;
                case MechanicBotMode::Freeze:
                    AddMs(c.msFreeze, ms);
                    break;
            }
        }

        // The mode the lock judges with (MechanicBotLock.cpp): a deferred escape locks as Escape, a released bot not
        // at all.
        Leaks::Lock LockOf(MechanicBotState const& st, bool released)
        {
            if (released)
                return Leaks::Lock::None;
            switch (st.mode)
            {
                case MechanicBotMode::Escape:
                    return Leaks::Lock::Escape;
                case MechanicBotMode::Hold:
                    return Leaks::Lock::Hold;
                case MechanicBotMode::Freeze:
                    return Leaks::Lock::Freeze;
                case MechanicBotMode::Watch:
                    return st.pendingEscape ? Leaks::Lock::Escape : Leaks::Lock::None;
                case MechanicBotMode::Idle:
                    break;
            }
            return Leaks::Lock::None;
        }

        // §3.5 leak detectors: the bot's live state as left by its update, judged against the state the lock used.
        void ObserveLeaks(Player* bot, MechanicBotState& st, Milliseconds now, bool released, Spell const* spell,
            Milliseconds remaining, Milliseconds margin, bool debug)
        {
            Leaks::Sample s;
            s.now = now;
            s.lock = LockOf(st, released);
            s.lookAway = !released && BotFacing::LookAwayActive(st, now);
            s.leaveBy = st.leaveBy;
            s.marginMs = margin;
            Movement::MoveSpline const* ms = bot->movespline;
            if (ms && ms->Initialized() && !ms->Finalized())
            {
                auto const& spline = ms->_Spline();
                G3D::Vector3 const& from = spline.getPoint(spline.first());
                G3D::Vector3 const& to = spline.getPoint(spline.last());
                s.spline = true;
                s.splineId = ms->GetId();
                s.splineMoves = Geo::Dist2d(from.x, from.y, to.x, to.y) >= Leaks::kFacingSplineYd;
                s.destX = to.x;
                s.destY = to.y;
            }
            if (st.mode == MechanicBotMode::Escape)
            {
                Geo::Vec3 const target = BotBrain::MoveTarget(st);
                s.targetValid = true;
                s.targetX = target.x;
                s.targetY = target.y;
            }
            s.ourSendNow = st.escapeSentAt == now;
            MovementGeneratorType const controlled = bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED);
            s.controlled = controlled != NULL_MOTION_TYPE;
            s.effectMotion = controlled == EFFECT_MOTION_TYPE;
            s.x = bot->GetPositionX();
            s.y = bot->GetPositionY();
            s.orientation = bot->GetOrientation();
            s.speed = bot->GetSpeed(MOVE_RUN);
            if (spell && spell->GetSpellInfo())
            {
                s.casting = true;
                s.castKey = reinterpret_cast<std::uintptr_t>(spell);
                s.castSpell = spell->GetSpellInfo()->Id;
                s.castRemaining = remaining;
            }

            Leaks::Verdict const v = Leaks::Observe(st.leak, s);
            if (!v.move && !v.cast && !v.turn)
                return;
            MechanicBotCounters& c = st.counters;
            if (v.move)
            {
                ++c.leakMoves;
                Leaks::AddSpell(c.leakSpells, v.moveSpell);
            }
            if (v.cast)
            {
                ++c.leakCasts;
                Leaks::AddSpell(c.leakSpells, v.castSpell);
            }
            if (v.turn)
                ++c.leakTurns;
            // Also under BossMechanics.Bots.ForceLeaks, whose check reads these lines.
            if (debug || GetConfig().bots.forceLeaks)
                LOG_INFO(kLog, "Bot {}: lock leak in {}{}: {}{}{}", bot->GetName(), ModeName(st.mode),
                    s.lookAway ? " (look-away)" : "",
                    v.move ? Acore::StringFormat("move (spell {}) ", v.moveSpell) : std::string(),
                    v.cast ? Acore::StringFormat("cast {} ", v.castSpell) : std::string(), v.turn ? "turn" : "");
        }
    }

    void BotOnSpellCast(Player* player, Spell const* spell)
    {
        if (!player || !spell || spell->IsTriggered() || !spell->GetSpellInfo() || !BotGate::IsEnabled())
            return;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (!botAI)
            return;
        if (MechanicBotState* st = BotBrain::StateOf(botAI))
            Leaks::SelfCast(st->leak, spell->GetSpellInfo()->Id, BotLock::MovesCaster(spell->GetSpellInfo()));
    }

    void BotAfterTick(Map* map, Registry& reg, Milliseconds now)
    {
        if (!map || !BotGate::IsEnabled())
            return;
        BotConfig const& cfg = GetConfig().bots;
        Milliseconds const fastTick(cfg.fastTickMs);
        Milliseconds const margin(cfg.safetyMarginMs);
        uint32 const revision = reg.Revision();
        // The bots seen in this update, for the map's brain time (M4 §3.6); reused across updates on this thread.
        thread_local std::vector<MechanicBotState*> seen;
        seen.clear();

        for (MapReference const& ref : map->GetPlayers())
        {
            Player* bot = ref.GetSource();
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                continue;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                continue;
            if (!botAI->HasStrategy("boss mechanics", BOT_STATE_COMBAT)
                && !botAI->HasStrategy("boss mechanics", BOT_STATE_NON_COMBAT))
                continue;
            MechanicBotState* st = BotBrain::StateOf(botAI);
            if (!st)
                continue;
            seen.push_back(st);

            // M4: mode clocks and leak detectors, from the state the bot's update left (before the waker acts).
            bool const released = BotBrain::Released(botAI, *st, now);
            // WP-Q2: the time since the previous sample, the map's update interval, for the fast tick below.
            Milliseconds const gap = st->clockAt > Milliseconds(0) && now > st->clockAt ? now - st->clockAt
                : Milliseconds(0);
            TickModeClock(*st, now, released);
            Milliseconds remaining{ 0 };
            Spell const* spell = CurrentCast(bot, remaining);
            ObserveLeaks(bot, *st, now, released, spell, remaining, margin, cfg.debug);

            if (spell)
            {
                Milliseconds const leaveBy = BotBrain::CastLeaveBy(botAI, *st, reg, now);
                if (BotBrain::Released(botAI, *st, now))
                    continue;
                // n3: leaveBy already holds SafetyMarginMs once; the second margin is deliberate (MechanicBotLock.cpp).
                // M3 look-away: a cast on a target outside the chosen arc whose re-facing (PlayerbotAI::UpdateAI turns
                // a casting bot to its target every update) would run into a window.
                bool const turnsAway = BotFacing::CastBreaksLookAway(bot, *st, spell, now, remaining);
                if (Solver::CastAllowed(now, remaining, leaveBy, margin) && !turnsAway)
                {
                    // X6: the cast goes on; the bot turns to the look-away facing (CastLeaveBy refreshed it).
                    LockRules::CastFacing f;
                    f.windowOpen = BotFacing::LookAwayActive(*st, now);
                    f.escaping = st->mode == MechanicBotMode::Escape;
                    f.facingOff = f.windowOpen && BotFacing::FacingOff(bot, st->lookFacing);
                    if (LockRules::CastFacingStepOf(f) == LockRules::CastFacingStep::Turn)
                    {
                        BotFacing::Apply(bot, st->lookFacing);
                        Leaks::OwnFacing(st->leak, st->lookFacing);     // ours, not a leak
                        ++st->counters.facingsApplied;
                    }
                    continue;
                }
                if (turnsAway)
                    ++st->counters.facingBreaks;
                botAI->RequestSpellInterrupt();
                Wake(botAI, *st);
                if (now - st->lastCastBreakAt >= kCastBreakDedup)
                    ++st->counters.castBreaks;
                st->lastCastBreakAt = now;
                st->noCastUntil = now + kNoCastAfterBreak;
                continue;
            }

            if (revision != st->seenRevision || now >= st->nextPerceptionAt)
                Wake(botAI, *st);
            else if (st->mode != MechanicBotMode::Idle
                && LockRules::FastTickDue((now - st->lastBrainAt).count(), gap.count(), fastTick.count()))
                Wake(botAI, *st);
        }

        if (!seen.empty())
        {
            uint32 const us = BotBrain::TakeMapBrainUs(map->GetId(), map->GetInstanceId());
            for (MechanicBotState* st : seen)
                st->cost.AddMapUpdate(us);
        }
        seen.clear();
    }
}

#endif
