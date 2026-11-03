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
 *    delay playerbots gives bots without a real master while a mechanic matters to the bot;
 * 3. casting or channelling: leaveBy is refreshed here (view only, no solve, no paths) and a cast or channel that
 *    would end after leaveBy - margin is interrupted (PlayerbotAI::RequestSpellInterrupt, consumed on the next AI
 *    tick), the bot woken, and cast-time spells and item uses locked for kNoCastAfterBreak. A released bot (stuck,
 *    rooted; MechanicBotBrain) is not interrupted.
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
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotGate.h"
#include "BossMechanicsConfig.h"
#include "Map.h"
#include "MechanicBotBrain.h"
#include "MechanicRegistry.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include <algorithm>

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
    }

    void BotAfterTick(Map* map, Registry& reg, Milliseconds now)
    {
        if (!map || !BotGate::IsEnabled())
            return;
        BotConfig const& cfg = GetConfig().bots;
        Milliseconds const fastTick(cfg.fastTickMs);
        Milliseconds const margin(cfg.safetyMarginMs);
        uint32 const revision = reg.Revision();

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

            Milliseconds remaining{ 0 };
            if (CurrentCast(bot, remaining))
            {
                Milliseconds const leaveBy = BotBrain::CastLeaveBy(botAI, *st, reg, now);
                // n3: leaveBy already holds SafetyMarginMs once; the second margin is deliberate (MechanicBotLock.cpp).
                if (Solver::CastAllowed(now, remaining, leaveBy, margin) || BotBrain::Released(botAI, *st, now))
                    continue;
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
            else if (st->mode != MechanicBotMode::Idle && now - st->lastBrainAt >= fastTick)
                Wake(botAI, *st);
        }
    }
}

#endif
