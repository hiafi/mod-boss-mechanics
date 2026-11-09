/*
 * mod-boss-mechanics: the lock multiplier of the "boss mechanics" strategy (STAGE-M2-HANDOFF §3.7, WP-C) and the
 * by-name gate that shares its verdict (STAGE-M4-HANDOFF §3.5, WP-X). WP-X2: item cast length in Escape and Hold
 * (X7), `pull` refused during an encounter (X8), and the BossMechanics.Bots.ForceLeaks debug passes (M4-SWEEP G8).
 */

#include "MechanicBotGuard.h"

#if BOSS_MECHANICS_WITH_BOTS

#include "MechanicBotLock.h"
#include "AiObjectContext.h"
#include "AttackAction.h"
#include "BossMechanicsConfig.h"
#include "CastAtPositionAction.h"
#include "CastFacingUnitAction.h"
#include "ChooseTargetActions.h"
#include "Engine.h"
#include "GameTime.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "MechanicBotBrain.h"
#include "MechanicBotFacing.h"
#include "MechanicBotState.h"
#include "MechanicBotTargets.h"
#include "MechanicLanding.h"
#include "MechanicLockRules.h"
#include "MechanicRegistry.h"
#include "MovementActions.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ReachTargetActions.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "UseItemAction.h"
#include <algorithm>
#include <string>
#include <typeinfo>
#include <vector>

using namespace BossMechanics;

namespace
{
    // Stock target picks and auto-attack. They derive from MovementAction (through AttackAction) but never move the
    // bot: they pick a target, start the swing and turn to it. Exact types only: the encounter actions that derive
    // from AttackAction (Karazhan, Zul'Aman, Black Temple, ... "position boss") do move, and stay blocked.
    template <class... Types>
    bool IsExactly(Action const* action)
    {
        std::type_info const& type = typeid(*action);
        return ((type == typeid(Types)) || ...);
    }

    bool IsAttackOnly(Action const* action)
    {
        return IsExactly<AttackAction, MeleeAction, AttackMyTargetAction, AttackDuelOpponentAction, DpsAoeAction,
            DpsAssistAction, TankAssistAction, AggressiveTargetAction, AttackAnythingAction, AttackLeastHpTargetAction,
            AttackEnemyPlayerAction, AttackRtiTargetAction, AttackEnemyFlagCarrierAction>(action);
    }

    // Movers that are not MovementActions, by name (the list and its sources: MechanicLockRules.h kNamedMovers).
    bool IsNamedMover(Action* action)
    {
        return dynamic_cast<CastReachTargetSpellAction*>(action) || LockRules::IsNamedMoverName(action->getName());
    }

    // Spells that move the caster: Charge, Intercept, Feral Charge (charge); Blink (leap); Disengage (leap back);
    // Heroic Leap and other jumps; teleports. A mover whose script moves it is in the scripted-mover table instead.
    bool IsMovementSpell(SpellInfo const* info)
    {
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            switch (effect.Effect)
            {
                case SPELL_EFFECT_CHARGE:
                case SPELL_EFFECT_CHARGE_DEST:
                case SPELL_EFFECT_LEAP:
                case SPELL_EFFECT_LEAP_BACK:
                case SPELL_EFFECT_JUMP:
                case SPELL_EFFECT_JUMP_DEST:
                case SPELL_EFFECT_TELEPORT_UNITS:
                    return true;
                default:
                    break;
            }
        }
        return false;
    }

    // How long the bot is busy casting it: cast time, plus the channel (an endless channel counts as forever).
    Milliseconds BusyTime(SpellInfo const* info, Player* bot)
    {
        Milliseconds busy(info->CalcCastTime(bot));
        if (info->IsChanneled())
        {
            int32 const duration = info->GetDuration();
            if (duration < 0)
                return Solver::kForever;
            busy += Milliseconds(duration);
        }
        return busy;
    }

    // Freeze: an item whose spell moves the caster (a rocket, a jump): the item the action would use, by its name.
    bool ItemMoves(PlayerbotAI* botAI, Action* action)
    {
        std::vector<Item*> items = botAI->GetAiObjectContext()->GetValue<std::vector<Item*>>("inventory items",
            action->getName())->Get();
        if (items.empty() || !items.front())
            return false;
        ItemTemplate const* proto = items.front()->GetTemplate();
        if (!proto)
            return false;
        for (auto const& spell : proto->Spells)
            if (spell.SpellId > 0)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId))
                    if (IsMovementSpell(info))
                        return true;
        return false;
    }

    // X7 (STAGE-M4-L-REPORT H3): how long using the item keeps the bot casting: the longest of its on-use spells
    // (BusyTime), for the item the action would use, by its name, as ItemMoves. The warlock stones are 3 s casts.
    Milliseconds ItemBusyTime(PlayerbotAI* botAI, Action* action, Player* bot)
    {
        std::vector<Item*> items = botAI->GetAiObjectContext()->GetValue<std::vector<Item*>>("inventory items",
            action->getName())->Get();
        if (items.empty() || !items.front())
            return Milliseconds(0);
        ItemTemplate const* proto = items.front()->GetTemplate();
        if (!proto)
            return Milliseconds(0);
        Milliseconds busy(0);
        for (auto const& spell : proto->Spells)
            if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId))
                    busy = std::max(busy, BusyTime(info, bot));
        return busy;
    }

    // X8: the bot's own encounter is live (begun, not ending). In an instance that is any encounter on the map (a boss
    // fight, a drill there). In the open world (STAGE-M4-R-REPORT m-7) only an encounter whose scope holds the bot or
    // a member of its group (raid): a drill, the training dummy, the sim; another player's dummy or a GM's `.mech
    // test` far away does not block this bot's pull.
    bool EncounterLive(Player* bot)
    {
        Registry const* reg = Registry::Get(bot->GetMap());
        if (!reg)
            return false;
        bool const instance = bot->GetMap()->IsDungeon();
        Group const* group = bot->GetGroup();
        ObjectGuid const self = bot->GetGUID();
        bool live = false;
        reg->ForEachEncounter([&](Encounter const& e)
        {
            if (live || e.ending)
                return;
            if (instance)
            {
                live = true;
                return;
            }
            for (ObjectGuid const& g : e.testGms)
                live = live || g == self || (group && group->IsMember(g));
        });
        return live;
    }

    // A spell that moves its caster from a script (MechanicLockRules.h kScriptedMovers); Ambush only from stealth on a
    // target out of melee range, the script's own test. `castTarget` reads the cast's target, only for such a rule.
    template <class TargetFn>
    bool IsScriptedMover(SpellInfo const* info, Player* bot, TargetFn&& castTarget)
    {
        LockRules::ScriptedMover const* mover = LockRules::FindScriptedMover(info->Id);
        if (!mover)
            return false;
        if (mover->rule == LockRules::MoverRule::Always)
            return true;
        Unit* const target = castTarget();
        return LockRules::ScriptedMoverMoves(mover->rule, bot->HasStealthAura(), target != nullptr,
            target && bot->IsWithinMeleeRange(target));
    }

    // The spell the cast action casts. With the fork's CastSpellAction::GetCastSpellId() the rework actions that cast
    // a stored id under a row name report it; without it the name lookup gives 0 for them (STAGE-M4-L-REPORT H1).
    uint32 CastSpellIdOf(PlayerbotAI* botAI, CastSpellAction* cast)
    {
#if defined(PLAYERBOTS_HAS_CAST_SPELL_ID)
        (void)botAI;
        return cast->GetCastSpellId();
#else
        AiObjectContext* context = botAI->GetAiObjectContext();
        return AI_VALUE2(uint32, "spell id", cast->getSpell());
#endif
    }

    // WP-Q2, WP-F2 (STAGE-M4-R2-REPORT m-5): how a spell's effects carry its caster to the cast unit: a charge
    // (CHARGE, CHARGE_DEST: Spell::EffectCharge stops at the target's reach on the caster's side) or a jump (JUMP,
    // JUMP_DEST: MoveJump onto the unit's position). LEAP, LEAP_BACK and TELEPORT_UNITS land elsewhere (Blink,
    // Disengage; Shadowstep's own test is by name).
    LockRules::LandingSpot SpellLanding(SpellInfo const* info)
    {
        bool charge = false;
        bool jump = false;
        for (SpellEffectInfo const& effect : info->GetEffects())
            switch (effect.Effect)
            {
                case SPELL_EFFECT_CHARGE:
                case SPELL_EFFECT_CHARGE_DEST:
                    charge = true;
                    break;
                case SPELL_EFFECT_JUMP:
                case SPELL_EFFECT_JUMP_DEST:
                    jump = true;
                    break;
                default:
                    break;
            }
        return LockRules::SpellEffectSpot(charge, jump);
    }

    // The unit a gap closer lands next to and where it stops, for the Idle/Watch landing test.
    struct GapCloser
    {
        Unit* dest = nullptr;           // nullptr: not a gap closer (or not judged)
        LockRules::LandingSpot spot = LockRules::LandingSpot::None;
        bool chase = false;             // the stock melee chase: judged on the whole melee ring (Landing::RingSafe)
    };

    // WP-Q2 (m4-s2 Frontal/1): the gap closer an action is (LockRules::MoverLanding, LandingSpot). Charges by type, the
    // plain-Action movers by name, a cast by its spell's effects or the scripted-mover table (only when this cast
    // moves). A cast at a position (CastAtPositionAction) lands at its own point, which the action keeps to itself
    // (FindPosition is protected): it is not judged, never at its unit (R2 m-5; none in the class rotations today).
    // STAGE-M4-R2-REPORT nit 3: the spell-id path (an id look-up, the spell's effects) runs only for a bot in Watch
    // or in combat; an Idle bot out of combat has no gap closer worth the look-up per candidate.
    GapCloser GapCloserOf(PlayerbotAI* botAI, Player* bot, Action* action, bool castLookup)
    {
        GapCloser g;
        if (dynamic_cast<CastReachTargetSpellAction*>(action))
        {
            g.dest = action->GetTarget();
            g.spot = LockRules::LandingSpot::Reach;
            return g;
        }
        AiObjectContext* context = botAI->GetAiObjectContext();
        // The stock melee chase too (m4-s2 Zone/5): refused only while the whole melee ring is unsafe.
        if (dynamic_cast<ReachMeleeAction*>(action))
        {
            g.dest = AI_VALUE(Unit*, "current target");
            g.chase = true;
            return g;
        }
        g.spot = LockRules::NamedMoverSpot(action->getName());
        switch (LockRules::NamedMoverLanding(action->getName()))
        {
            case LockRules::MoverLanding::CastTarget:
                g.dest = action->GetTarget();
                return g;
            case LockRules::MoverLanding::CurrentTarget:
                g.dest = AI_VALUE(Unit*, "current target");
                return g;
            case LockRules::MoverLanding::LooseEnemy:
            {
                Value<Unit*>* loose = context->GetValue<Unit*>("prot loose enemy");
                g.dest = loose ? loose->Get() : nullptr;
                return g;
            }
            case LockRules::MoverLanding::None:
                break;
        }
        g.spot = LockRules::LandingSpot::None;
        if (!castLookup || LockRules::IsNamedMoverName(action->getName()))
            return g;     // a named mover that lands away (Blink back, Disengage)
        CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action);
        if (!cast || dynamic_cast<CastAtPositionAction*>(cast))
            return g;
        uint32 const spellId = CastSpellIdOf(botAI, cast);
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info)
            return g;
        g.spot = SpellLanding(info);
        if (g.spot == LockRules::LandingSpot::None)
        {
            g.spot = LockRules::ScriptedMoverSpot(info->Id);
            if (g.spot != LockRules::LandingSpot::None
                && !IsScriptedMover(info, bot, [cast]() -> Unit* { return cast->GetTarget(); }))
                g.spot = LockRules::LandingSpot::None;
        }
        if (g.spot != LockRules::LandingSpot::None)
            g.dest = cast->GetTarget();
        return g;
    }

    bool Usable(Player* bot, Unit* dest)
    {
        return dest && dest != bot && dest->IsInWorld() && dest->GetMap() == bot->GetMap();
    }

    Landing::Dest DestOf(Unit* dest)
    {
        Landing::Dest d;
        d.center = { dest->GetPositionX(), dest->GetPositionY(), dest->GetPositionZ() };
        d.combatReachYd = dest->GetCombatReach();
        d.facing = dest->GetOrientation();
        return d;
    }

    Geo::Vec3 PosOf(Player* bot)
    {
        return { bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() };
    }

    // WP-F2 (R2 M-1): the melee chase toward `dest` is refused while no point of the melee ring (the bot's melee band
    // round dest) is safe over the horizon. A refusal goes on the chase clock: the brain releases a bot refused for
    // BossMechanics.Bots.ChaseReleaseMs (the safety valve, a walk-in on a short horizon since M5 CD). The
    // brain's view (st.input) is current: the brain steps before the engine picks actions in the same AI tick.
    bool ChaseRefused(Player* bot, MechanicBotState& st, Unit* dest, Milliseconds now)
    {
        Solver::Input const& in = st.input;
        float const radius = std::max(0.f, bot->GetMeleeRange(dest) - Landing::kMeleeBandSlackYd);
        Geo::Vec3 const center{ dest->GetPositionX(), dest->GetPositionY(), dest->GetPositionZ() };
        // R2 m-6: while Watch found no way round the hazard the last chase ended in (MechanicBotBrain BlockBand), the
        // chase stays refused: it would walk the bot a brain step into it before Watch stops it again.
        bool const blocked = now < st.bandBlockedUntil;
        if (!blocked && Landing::RingSafe(in, center, PosOf(bot), radius, now, now + in.params.horizon))
            return false;
        LockRules::NoteChaseRefusal(st.chase, now.count(), LockRules::ChaseRunStaleMs(GetConfig().bots.holdCapMs));
        return true;
    }

    enum class LockVerdict : uint8
    {
        Allow,
        BlockMove,      // a movement action or movement spell
        BlockCast,      // a cast-time or channelled spell, an item use
        BlockFacing,    // look-away: the action would turn the bot
    };

    using LockRules::LeakKind;

    struct Judgement
    {
        LockVerdict verdict = LockVerdict::Allow;
        bool unknownCast = false;   // a cast judged with no spell id
        // The leak detector this block would feed if the action ran (BossMechanics.Bots.ForceLeaks); None for the
        // blocks no detector sees.
        LeakKind leak = LeakKind::None;
        bool pull = false;          // X8: a pull request refused
        bool landing = false;       // WP-Q2: a gap closer (or the melee chase) refused for its landing
        bool escapeFacing = false;  // M5 CD (C6): a facing-only action refused in Escape (not look-away)
        bool quiet = false;         // M5 WP-F (R m-1): refused without a count (`boss focus` while the bot walks)
    };

    Judgement Allow(bool unknownCast = false)
    {
        Judgement j;
        j.unknownCast = unknownCast;
        return j;
    }

    Judgement Blocked(LockVerdict verdict, LeakKind leak = LeakKind::None)
    {
        Judgement j;
        j.verdict = verdict;
        j.leak = leak;
        return j;
    }

    // The verdict, with no counter touched: the multiplier and the gate count it (CountJudgement), a chat `do` only
    // notes it.
    Judgement JudgeAction(PlayerbotAI* botAI, MechanicBotState& st, Action* action)
    {
        Player* bot = botAI->GetBot();
        if (!bot)
            return Allow();
        AiObjectContext* context = botAI->GetAiObjectContext();
        Milliseconds const now = GameTime::GetGameTimeMS();

        // Stratholme X3: a hop for a Jump is in flight (under a second). Anything would cut it: a move or an attack
        // (MotionMaster::Clear), a facing spline, a cast's turn to its target. Everything waits, ours too, uncounted,
        // released or not.
        if (BotBrain::Hopping(bot, st, now))
        {
            Judgement j = Blocked(LockVerdict::BlockMove);
            j.quiet = true;
            return j;
        }

        // A released bot (stuck, rooted, past the escape ceiling) is the stock AI's.
        if (BotBrain::Released(botAI, st, now))
            return Allow();

        // Ours. The move action is dropped while frozen (it is not useful then anyway).
        if (dynamic_cast<MechanicBotMoveAction*>(action))
            return st.mode == MechanicBotMode::Freeze ? Blocked(LockVerdict::BlockMove) : Allow();
        if (dynamic_cast<MechanicBotHoldAction*>(action) || dynamic_cast<MechanicBotFaceAction*>(action))
            return Allow();

        // X8, in every mode: a human's `pull` mid-encounter would start playerbots' pull, whose PullMultiplier zeroes
        // every other action (ours included) until the pull ends, up to 15 s (STAGE-M4-L-REPORT §2.4). Refused
        // silently (a multiplier cannot tell the human); `.mech bots` counts it.
        if (LockRules::IsPullRequestName(action->getName()) && EncounterLive(bot))
        {
            Judgement j = Blocked(LockVerdict::BlockMove);
            j.pull = true;
            return j;
        }

        // M3, in every mode: tank facing owns the tank (one owner; decision §7.5), and the look-away facing (decision
        // §7.6) is not undone by a facing action, a facing cast or a swing at a target outside the arc.
        if (BotBrain::TankOwned(st) && (typeid(*action) == typeid(TankFaceAction) || action->getName() == "tank face"))
            return Blocked(LockVerdict::BlockMove);
        bool const lookAway = BotFacing::LookAwayActive(st, now);
        // M5 WP-F (R m-1): `boss focus` attacks the descriptor's pick (st.targets.focus), not its target value.
        bool const focus = dynamic_cast<MechanicBotFocusAction*>(action) != nullptr;
        if (lookAway)
        {
            if (dynamic_cast<SetFacingTargetAction*>(action) || dynamic_cast<CastFacingUnitAction*>(action))
                return Blocked(LockVerdict::BlockFacing, LeakKind::Turn);
            if (IsAttackOnly(action) && BotFacing::WouldTurnAway(bot, st, now, action->GetTarget()))
                return Blocked(LockVerdict::BlockFacing, LeakKind::Turn);
            if (focus && st.targets.focus.target && BotFacing::WouldTurnAway(bot, st, now,
                botAI->GetUnit(ObjectGuid(st.targets.focus.target))))
                return Blocked(LockVerdict::BlockFacing, LeakKind::Turn);
        }

        // Watch with an unsafe spot whose solve was deferred (rate limit, per-map budget) locks as Escape
        // (STAGE-M2-REVIEW m5). Just after a cast break, whatever the mode, no cast-time spell and no item use for
        // kNoCastAfterBreak (MechanicBotWaker.cpp), so a cast the lock does not otherwise see (an item, a Watch bot) is
        // not restarted and broken again on every update.
        MechanicBotMode mode = st.mode;
        if (mode == MechanicBotMode::Watch && st.pendingEscape)
            mode = MechanicBotMode::Escape;
        // M5 CD (C6): a facing-only action while the brain walks the bot (LockRules::FacingOnlyRefused): it would not
        // stop the escape, but broadcasts a heartbeat that snaps the walk on observers' clients on every AI tick the
        // bot walks away from its target ("not facing target"), and spends an action slot and a react delay.
        if (LockRules::FacingOnlyRefused(mode == MechanicBotMode::Escape, false)
            && (dynamic_cast<SetFacingTargetAction*>(action) || action->getName() == "set facing"))
        {
            Judgement j = Blocked(LockVerdict::BlockFacing);
            j.escapeFacing = true;
            return j;
        }
        bool const afterBreak = now < st.noCastUntil;
        bool const frozen = mode == MechanicBotMode::Freeze;
        bool const idleOrWatch = mode == MechanicBotMode::Idle || mode == MechanicBotMode::Watch;
        // WP-Q2 (m4-s2 Frontal/1, Zone/5), WP-F2 (R2 M-1): in Idle and Watch a gap closer whose landing fails the
        // bot's hazards is refused (a Heroic Leap at the pull landed in the boss's warned front cone, too late to step
        // out), and so is the melee chase while the whole melee ring is unsafe. Escape, Hold and Freeze block every
        // mover below anyway.
        if (idleOrWatch && st.viewValid && !st.input.hazards.empty())
        {
            GapCloser const g = GapCloserOf(botAI, bot, action, mode == MechanicBotMode::Watch || bot->IsInCombat());
            if (Usable(bot, g.dest))
            {
                bool refused = false;
                if (g.chase)
                    refused = ChaseRefused(bot, st, g.dest, now);
                else
                    refused = Landing::LandingUnsafe(st.input, g.spot, DestOf(g.dest), PosOf(bot), now,
                        now + st.input.params.horizon);
                if (refused)
                {
                    Judgement j = Blocked(LockVerdict::BlockMove);
                    j.landing = true;
                    return j;
                }
            }
        }
        // X3: a hop is due (the bot has a Jump to hop for): in Idle and Watch too, a cast-time spell must end before it
        // starts (below, with the casts after a break).
        bool const hopDue = st.nextHopAt < Solver::kForever;
        if (idleOrWatch && !afterBreak && !lookAway && !hopDue)
            return Allow();
        if (afterBreak && dynamic_cast<UseItemAction*>(action))
            return Blocked(LockVerdict::BlockCast);
        if (frozen && dynamic_cast<UseItemAction*>(action) && ItemMoves(botAI, action))
            return Blocked(LockVerdict::BlockMove, LeakKind::Move);

        // Idle or Watch after a break or under look-away: only casts are judged (cast-time after a break, facing casts
        // under look-away).
        bool const castsOnly = mode == MechanicBotMode::Idle || mode == MechanicBotMode::Watch;

        // M5 WP-F (R m-1): `boss focus` is an AttackAction (so a MovementAction) that only picks a target and starts
        // the swing: allowed where the bot stands (Hold, Freeze), as the stock pickers are; in Escape Attack() would
        // clear the walk (MotionMaster::Clear), so it waits, uncounted (its trigger stays true every tick of the
        // escape).
        if (focus)
        {
            if (mode != MechanicBotMode::Escape)
                return Allow();
            Judgement j = Blocked(LockVerdict::BlockMove);
            j.quiet = true;
            return j;
        }

        // Foreign movers (Freeze: every one, decision §7.4).
        if (IsAttackOnly(action))
            return Allow();
        if (!castsOnly && (dynamic_cast<MovementAction*>(action) || IsNamedMover(action)))
            return Blocked(LockVerdict::BlockMove, LeakKind::Move);

        // X7: an item use is a cast of its on-use spell, judged by that spell's length as a spell is below (Escape:
        // none with a cast time; Hold: one that ends in time). Freeze allows casts, items included (decision §7.4).
        if (!castsOnly && !frozen && dynamic_cast<UseItemAction*>(action))
        {
            Milliseconds const busy = ItemBusyTime(botAI, action, bot);
            if (busy <= Milliseconds(0))
                return Allow();
            if (mode == MechanicBotMode::Escape)
                return Blocked(LockVerdict::BlockCast, bot->isMoving() ? LeakKind::None : LeakKind::Cast);
            if (!Solver::CastAllowed(now, busy, st.leaveBy, Milliseconds(GetConfig().bots.safetyMarginMs)))
                return Blocked(LockVerdict::BlockCast, LeakKind::Cast);
            return Allow();
        }

        // Spells.
        CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action);
        if (!cast)
            return Allow();     // facing ("set facing" is a plain Action; blocked above under look-away and in
                                // Escape), items, chat
        uint32 const spellId = CastSpellIdOf(botAI, cast);
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        // The cast target, read once: the look-away test and Ambush's stealth rule need it.
        Unit* target = nullptr;
        bool targetRead = false;
        auto castTarget = [&]() -> Unit*
        {
            if (!targetRead)
            {
                target = cast->GetTarget();
                targetRead = true;
            }
            return target;
        };
        if (!castsOnly && info && (IsMovementSpell(info) || IsScriptedMover(info, bot, castTarget)))
            return Blocked(LockVerdict::BlockMove, LeakKind::Move);
        // Look-away: a cast that turns its caster to a target outside the arc (PlayerbotAI::CastSpell, and the mid-cast
        // re-facing in UpdateAI, for SPELL_FACING_FLAG_INFRONT) would sweep the carried cone over the group. Offensive
        // casts on a target outside the arc are blocked whatever their flags and with no spell id (rework actions turn
        // the bot to their target themselves); a ground cast turns the bot to its point, which can be its own feet
        // (world orientation 0); a turning self cast turns it to the current target. Heals on an ally behind the bot
        // need no facing and stay allowed. The rules: MechanicLockRules.h LookAwayBlocksCast.
        if (lookAway)
        {
            Unit* const victim = castTarget();
            LockRules::LookAwayCast c;
            c.groundCast = dynamic_cast<CastAtPositionAction*>(cast) != nullptr;
            c.turnsToCurrentTarget = LockRules::IsTurningSelfCastName(cast->getName());
            if (c.turnsToCurrentTarget)
                c.currentTargetOutside = BotFacing::WouldTurnAway(bot, st, now, AI_VALUE(Unit*, "current target"));
            c.facingInfront = BotFacing::TurnsCaster(info);
            c.offensiveTarget = victim && victim != bot && bot->IsValidAttackTarget(victim);
            c.targetOutside = BotFacing::WouldTurnAway(bot, st, now, victim);
            if (LockRules::LookAwayBlocksCast(c))
                return Blocked(LockVerdict::BlockFacing, LeakKind::Turn);
        }
        // Unknown spell: allowed (no cast-time or movement test can run). Counted only when the cast could run
        // (STAGE-M4-R-REPORT m-1): the engine applies multipliers before isPossible, so spells the bot never learned
        // (another race's racials, untaken talents) reach here with id 0 and would inflate the count.
        if (!info)
            return Allow(cast->isPossible());
        Milliseconds const busy = BusyTime(info, bot);
        if (busy <= Milliseconds(0))
            return Allow();     // instant
        // Frozen: the bot cannot leave anyway; casting is what it may do (decision §7.4).
        if (frozen)
            return Allow();
        if (castsOnly)
        {
            if (afterBreak)
                return Blocked(LockVerdict::BlockCast);
            // X3: it would end after the hop starts (the hop would stop it).
            if (hopDue && !Solver::CastAllowed(now, busy, st.nextHopAt, Milliseconds(GetConfig().bots.safetyMarginMs)))
                return Blocked(LockVerdict::BlockCast);
            return Allow();
        }
        // Escape: it would fail while moving (PlayerbotAI::CastSpell refuses it and delays the AI), so only a standing
        // bot's block feeds the forced-leak check.
        if (mode == MechanicBotMode::Escape)
            return Blocked(LockVerdict::BlockCast, bot->isMoving() ? LeakKind::None : LeakKind::Cast);
        // Hold: a cast that ends before the bot must leave. leaveBy already holds SafetyMarginMs once
        // (Solver::LeaveBy); the second margin here is deliberate (STAGE-M2-REVIEW n3): it covers the AI tick between
        // the cast's end and the escape's first step, which the solver's StartDelayMs does not see for a bot that was
        // casting.
        Milliseconds const margin(GetConfig().bots.safetyMarginMs);
        if (!Solver::CastAllowed(now, busy, st.leaveBy, margin))
            return Blocked(LockVerdict::BlockCast, LeakKind::Cast);
        return Allow();
    }

    // BossMechanics.Bots.ForceLeaks (M4-SWEEP G8, debug): a block the leak detectors can see is let through while that
    // detector has counted fewer leaks than the key since the counters' last reset. A run with working detectors then
    // reports leaks; one with blind detectors keeps leaking and reports none. Logged once per reset.
    void ForceLeak(Player* bot, MechanicBotState& st, Judgement& j)
    {
        uint32 const budget = GetConfig().bots.forceLeaks;
        if (!budget || j.verdict == LockVerdict::Allow)
            return;
        MechanicBotCounters& c = st.counters;
        uint32 counted = 0;
        uint32* passes = nullptr;
        char const* kind = "";
        switch (j.leak)
        {
            case LeakKind::Move:
                counted = c.leakMoves;
                passes = &c.forcedMoves;
                kind = "moves";
                break;
            case LeakKind::Cast:
                counted = c.leakCasts;
                passes = &c.forcedCasts;
                kind = "casts";
                break;
            case LeakKind::Turn:
                counted = c.leakTurns;
                passes = &c.forcedTurns;
                kind = "turns";
                break;
            case LeakKind::None:
                return;
        }
        if (!LockRules::ForceLeakPasses(j.leak, counted, budget))
            return;
        if (!*passes)
            LOG_INFO("module.bossmechanics.bots", "Bot {}: BossMechanics.Bots.ForceLeaks: the lock lets blocked {} "
                "through in {} until the detector counts {}", bot->GetName(), kind, ModeName(st.mode), budget);
        ++*passes;
        j.verdict = LockVerdict::Allow;
    }

    // Counts a judgement: 1 to allow, 0 to drop.
    float CountJudgement(MechanicBotState& st, Judgement const& j)
    {
        if (j.quiet && j.verdict != LockVerdict::Allow)
            return 0.f;
        if (j.unknownCast)
            ++st.counters.unknownCasts;
        if (j.pull && j.verdict != LockVerdict::Allow)
            ++st.counters.pullBlocks;
        if (j.landing && j.verdict != LockVerdict::Allow)
            ++st.counters.landingBlocks;
        switch (j.verdict)
        {
            case LockVerdict::Allow:
                return 1.f;
            case LockVerdict::BlockMove:
                ++st.counters.lockBlocks;
                ++st.counters.lockMoves;
                return 0.f;
            case LockVerdict::BlockCast:
                ++st.counters.lockBlocks;
                ++st.counters.lockCasts;
                return 0.f;
            case LockVerdict::BlockFacing:
                // Look-away (decision §7.6): an action that would turn the bot off its chosen facing; M5 CD (C6): a
                // facing-only action while the brain walks it.
                ++st.counters.lockBlocks;
                ++(j.escapeFacing ? st.counters.escapeFacingBlocks : st.counters.facingBlocks);
                return 0.f;
        }
        return 1.f;
    }

#if defined(PLAYERBOTS_HAS_EXECUTE_ACTION_GATE)
    // Engine::SetExecuteActionGate: an action run by name (PlayerbotAI::DoSpecificAction: move to loot from an item
    // use, the pull pre-action and pull spell, the bridge, mod-dungeon-clear dispatches, Lua) gets the lock's verdict,
    // as an engine-queued one does through the multiplier. A bot without the strategy is not ours. Chat `do` is a
    // human order (and the GM test flow): let through, counted when the lock would have refused it (decision §7.4).
    bool SpecificActionGate(PlayerbotAI* botAI, Action* action, Event& event)
    {
        if (!botAI || !action)
            return true;
        if (!botAI->HasStrategy("boss mechanics", BOT_STATE_COMBAT)
            && !botAI->HasStrategy("boss mechanics", BOT_STATE_NON_COMBAT))
            return true;
        MechanicBotState* st = BotBrain::StateOf(botAI);
        if (!st)
            return true;
        Judgement j = JudgeAction(botAI, *st, action);
        if (event.GetSource() == "do")
        {
            if (j.verdict != LockVerdict::Allow)
                ++st->counters.doBypasses;
            return true;
        }
        ForceLeak(botAI->GetBot(), *st, j);
        if (CountJudgement(*st, j) > 0.f)
            return true;
        ++st->counters.specificBlocks;
        return false;
    }
#endif
}

float BotLock::Verdict(PlayerbotAI* botAI, MechanicBotState& st, Action* action)
{
    if (!botAI || !action)
        return 1.f;
    Judgement j = JudgeAction(botAI, st, action);
    if (Player* bot = botAI->GetBot())
        ForceLeak(bot, st, j);
    return CountJudgement(st, j);
}

bool BotLock::MovesCaster(SpellInfo const* info)
{
    return info && (IsMovementSpell(info) || LockRules::FindScriptedMover(info->Id) != nullptr);
}

void BotLock::InstallSpecificActionGate()
{
#if defined(PLAYERBOTS_HAS_EXECUTE_ACTION_GATE)
    Engine::SetExecuteActionGate(&SpecificActionGate);
#endif
}

float MechanicBotLockMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.f;
    MechanicBotState* st = _state.Get(botAI);
    if (!st)
        return 1.f;
    return BotLock::Verdict(botAI, *st, action);
}

#endif
