/*
 * mod-boss-mechanics: npc_boss_mechanics_dummy (Mechanics Training Dummy 300402, STAGE-M4-HANDOFF §3.2, WP-K).
 * See MechanicDummyAI.h for the behavior; the program runner is MechanicDrillRunner.h.
 */

#include "MechanicDummyAI.h"
#include "BossMechanicsIds.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CombatManager.h"
#include "Creature.h"
#include "CreatureScript.h"
#include "Log.h"
#include "Map.h"
#include "MechanicDrill.h"
#include "MechanicDrillPlan.h"
#include "MechanicDrillRunner.h"
#include "MechanicRegistry.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include <algorithm>
#include <cmath>

namespace BossMechanics
{
    namespace
    {
        constexpr char const* kLog = "module.bossmechanics";

        // Gossip actions (sender GOSSIP_SENDER_MAIN). A drill pick is kActionDrillFirst + its index in Drill::All().
        enum DummyGossipAction : uint32
        {
            ACTION_MAIN = 1,
            ACTION_PROGRAM_ALL,
            ACTION_PROGRAM_BARRAGE,
            ACTION_MENU_DRILLS,
            ACTION_MENU_DAMAGE,
            ACTION_DAMAGE_NONE,
            ACTION_DAMAGE_FLAT,
            ACTION_REPORT,
            ACTION_STOP,
            ACTION_DRILL_FIRST = 100
        };

        constexpr char const* kProgramAll = "all";
        constexpr char const* kProgramBarrage = "barrage";

        std::string PlayerName(ObjectGuid guid)
        {
            if (Player* p = ObjectAccessor::FindConnectedPlayer(guid))
                return p->GetName();
            return guid.ToString();
        }

        std::string DescribeProgram(std::string const& program)
        {
            if (program.empty() || program == kProgramAll)
                return "every drill, then barrage and minefield";
            if (program == kProgramBarrage)
                return "barrage only";
            return program;
        }

        void Tell(Creature* me, Player* player, std::string const& text)
        {
            if (me && player)
                me->Whisper(text, LANG_UNIVERSAL, player);
        }
    }

    MechanicDummyAI::MechanicDummyAI(Creature* creature) : NullCreatureAI(creature), _program(kProgramAll)
    {
        // Scheduled once for the creature's life: Check() may evade, and Reset() runs inside that, so Reset() must
        // not touch the scheduler (a CancelAll there would let this task's Repeat schedule a second copy). The drill
        // steps never use this scheduler (the runner schedules them on the encounter, MechanicDrillRunner.h).
        scheduler.Schedule(kDummyCheckInterval, [this](TaskContext ctx)
        {
            Check();
            ctx.Repeat(kDummyCheckInterval);
        });
    }

    void MechanicDummyAI::Reset()
    {
        me->SetReactState(REACT_PASSIVE);
        me->SetFullHealth();
        _scopePlayer.Clear();
        _started = false;
        _stopped = false;
        _noPlayerFor = Milliseconds(0);
    }

    // NullCreatureAI swallows both: put back the CreatureAI defaults so the dummy engages (JustEngagedWith) when a
    // player starts a fight with it.
    void MechanicDummyAI::JustEnteredCombat(Unit* who)
    {
        CreatureAI::JustEnteredCombat(who);
    }

    void MechanicDummyAI::JustStartedThreateningMe(Unit* who)
    {
        CreatureAI::JustStartedThreateningMe(who);
    }

    void MechanicDummyAI::JustEngagedWith(Unit* who)
    {
        _stopped = false;
        _noPlayerFor = Milliseconds(0);
        Player* scope = who ? who->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!scope || scope->IsGameMaster())
        {
            std::vector<Player*> const fighting = FightingPlayers();
            if (!fighting.empty())
                scope = fighting.front();
        }
        if (!scope)
            return;     // the next check picks the scope once a player shows up
        std::string err;
        if (!StartFor(scope, err))
            Tell(me, scope, "Could not start the program: " + err);
    }

    void MechanicDummyAI::EnterEvadeMode(EvadeReason why)
    {
        // The evade hook ends the encounter (and its ledger) after this: keep the report for the gossip.
        if (Running())
            _lastReport = LedgerLines();
        CreatureAI::EnterEvadeMode(why);
    }

    void MechanicDummyAI::DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*damageType*/,
        SpellSchoolMask /*damageSchoolMask*/)
    {
        if (damage >= me->GetHealth())
            damage = me->GetHealth() > 1 ? me->GetHealth() - 1 : 0;
    }

    void MechanicDummyAI::UpdateAI(uint32 diff)
    {
        scheduler.Update(diff);
    }

    bool MechanicDummyAI::Running() const
    {
        Registry const* reg = me->IsInWorld() ? Registry::Get(me->GetMap()) : nullptr;
        return reg && reg->FindEncounter(me->GetGUID());
    }

    std::vector<Player*> MechanicDummyAI::FightingPlayers() const
    {
        std::vector<Player*> out;
        for (auto const& [guid, ref] : me->GetCombatManager().GetPvECombatRefs())
        {
            Player* p = ref->GetOther(me)->ToPlayer();
            if (!p || !p->IsInWorld() || p->GetMap() != me->GetMap() || !p->IsAlive() || p->IsGameMaster())
                continue;
            if (p->GetExactDist2d(me) > kDummyLeashYd)
                continue;
            out.push_back(p);
        }
        // Guid order: the pick of a new scope player does not depend on the hash map's order.
        std::sort(out.begin(), out.end(), [](Player const* a, Player const* b)
        {
            return a->GetGUID() < b->GetGUID();
        });
        return out;
    }

    bool MechanicDummyAI::StartFor(Player* scope, std::string& err)
    {
        DrillOptions opt;
        opt.dmg = _damage;
        opt.includeScopePlayer = true;      // a player alone on the dummy is the group
        opt.loop = true;
        // The reference-health provider reads the owner's level (percent damage); combat plays the dummy at the
        // attacker's level + 3 anyway (BOSS_MOB). Not sent to the client, as the test owner does.
        me->SetLevel(scope->GetLevel(), false);
        _scopePlayer = scope->GetGUID();
        _lastReport.clear();
        if (!StartProgram(me, scope->GetGUID(), _program, opt, err))
        {
            LOG_WARN(kLog, "Training dummy {}: program '{}' did not start for {}: {}", me->GetGUID().ToString(),
                _program, scope->GetName(), err);
            return false;
        }
        _started = true;
        LOG_DEBUG(kLog, "Training dummy {}: program '{}' for {} ({} damage)", me->GetGUID().ToString(), _program,
            scope->GetName(), _damage);
        return true;
    }

    void MechanicDummyAI::Check()
    {
        // The floor: hits already leave 1 health; this keeps a long loop off every execute range.
        if (me->HealthBelowPct(kDummyHealthFloorPct))
            me->SetHealth(me->CountPctFromMaxHealth(kDummyHealthFloorPct));

        if (!IsEngaged())
            return;
        // A player who walked away has left the fight: end that combat (NullCreatureAI never evades by itself).
        me->GetCombatManager().EndCombatBeyondRange(kDummyLeashYd);
        if (!me->IsInCombat())
        {
            EnterEvadeMode(EVADE_REASON_NO_HOSTILES);
            return;
        }

        std::vector<Player*> const fighting = FightingPlayers();
        if (fighting.empty())
        {
            _noPlayerFor += kDummyCheckInterval;
            if (_noPlayerFor >= kDummyNoPlayerTimeout)
                EnterEvadeMode(EVADE_REASON_OTHER);
            return;
        }
        _noPlayerFor = Milliseconds(0);
        if (_stopped)
            return;
        if (_started && !Running())
        {
            // Someone else ended the encounter (`.mech clear all`): stay quiet until the next engage or gossip choice.
            _started = false;
            _stopped = true;
            return;
        }

        // The scope player still counts while it is on the map with a fighting group member (dead, say: death ends
        // its combat, but its group fights on and the program keeps landing on them).
        Player const* scopeNow = ObjectAccessor::GetPlayer(me->GetMap(), _scopePlayer);
        bool scopeHere = false;
        for (Player const* p : fighting)
            if (scopeNow && (p == scopeNow || p->IsInSameRaidWith(scopeNow)))
                scopeHere = true;
        if (!scopeHere || !_started)
        {
            // First player after a pet-only start, the scope group left the fight, or the program never started.
            Player* scope = fighting.front();
            std::string err;
            if (!StartFor(scope, err))
            {
                Tell(me, scope, "Could not start the program: " + err);
                _stopped = true;    // do not retry every second; a gossip choice or the next engage tries again
                return;
            }
            Tell(me, scope, "Mechanics program restarted for your group: " + DescribeProgram(_program) + ".");
        }

        // Every player in the fight joins the scope set (the open-world bot gate reads it).
        if (Registry* reg = Registry::Get(me->GetMap()))
            for (Player const* p : fighting)
                reg->AddScopePlayer(me->GetGUID(), p->GetGUID());
        FaceVictim();
    }

    void MechanicDummyAI::FaceVictim()
    {
        // The top of the threat list is the tank: the dummy's victim (the drills' frontal exemption and
        // `pick=nontank` read it), and the dummy turns in place to face it as a boss does, so a frontal points at the
        // tank. Attack(victim, false) never swings (NullCreatureAI has no melee) and never moves the dummy.
        Unit* victim = me->GetThreatMgr().GetCurrentVictim();
        if (!victim)
            return;
        if (me->GetVictim() != victim)
            me->Attack(victim, false);
        float const turn = Position::NormalizeOrientation(me->GetAngle(victim) - me->GetOrientation());
        if (std::min(turn, 2.f * static_cast<float>(M_PI) - turn) > kDummyFacingSlop)
            me->SetFacingToObject(victim);
    }

    bool MechanicDummyAI::Configure(std::string const& program, int32 damage, Player* by, std::string& err)
    {
        if (!Drill::ParseProgram(program, err))
            return false;
        _program = program.empty() ? std::string(kProgramAll) : program;
        _damage = std::max<int32>(damage, 0);
        _stopped = false;
        if (!IsEngaged())
            return true;
        Player* scope = ObjectAccessor::GetPlayer(me->GetMap(), _scopePlayer);
        if (!scope)
        {
            std::vector<Player*> const fighting = FightingPlayers();
            scope = fighting.empty() ? by : fighting.front();
        }
        if (!scope)
        {
            err = "nobody to run it for";
            return false;
        }
        return StartFor(scope, err);
    }

    void MechanicDummyAI::Stop()
    {
        if (Running())
            _lastReport = LedgerLines();
        _started = false;
        _stopped = true;
        StopProgram(me);
    }

    std::vector<std::string> MechanicDummyAI::LedgerLines() const
    {
        std::vector<std::string> out;
        Registry const* reg = Registry::Get(me->GetMap());
        Encounter const* e = reg ? reg->FindEncounter(me->GetGUID()) : nullptr;
        if (!e)
            return out;
        out.push_back(Acore::StringFormat("Encounter {}: {} player(s) with failures, {} group failure(s), {} event(s)",
            e->owner.ToString(), e->ledger.size(), e->groupFailures, e->events.size()));
        for (auto const& [player, l] : e->ledger)
        {
            std::string tags;
            for (auto const& [tag, n] : l.byTag)
                tags += Acore::StringFormat(" {}:{}", tag, n);
            out.push_back(Acore::StringFormat("  {}: {} failure(s) | by tag{}", PlayerName(player), l.total, tags));
        }
        return out;
    }

    std::vector<std::string> MechanicDummyAI::ReportLines() const
    {
        if (Running())
            return LedgerLines();
        return _lastReport;
    }

    namespace
    {
        MechanicDummyAI* DummyAI(Creature* creature)
        {
            if (!creature || creature->GetEntry() != Ids::NPC_MECHANIC_DUMMY)
                return nullptr;
            return dynamic_cast<MechanicDummyAI*>(creature->AI());
        }

        void SendReport(Creature* me, MechanicDummyAI const& ai, Player* player)
        {
            std::vector<std::string> const lines = ai.ReportLines();
            if (lines.empty())
            {
                Tell(me, player, "No failures recorded yet.");
                return;
            }
            for (std::string const& line : lines)
                Tell(me, player, line);
        }

        void SendMainMenu(Player* player, Creature* creature, MechanicDummyAI const& ai)
        {
            ClearGossipMenuFor(player);
            std::string const& p = ai.Program();
            auto mark = [](bool on) { return on ? " (current)" : ""; };
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Acore::StringFormat("Program: every drill, then barrage and "
                "minefield{}", mark(p == kProgramAll)), GOSSIP_SENDER_MAIN, ACTION_PROGRAM_ALL);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Acore::StringFormat("Program: one drill...{}",
                mark(Drill::Find(p) != nullptr)), GOSSIP_SENDER_MAIN, ACTION_MENU_DRILLS);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Acore::StringFormat("Program: barrage only{}",
                mark(p == kProgramBarrage)), GOSSIP_SENDER_MAIN, ACTION_PROGRAM_BARRAGE);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Acore::StringFormat("Damage per hit: {}...", ai.Damage()),
                GOSSIP_SENDER_MAIN, ACTION_MENU_DAMAGE);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Whisper me the failure report", GOSSIP_SENDER_MAIN,
                ACTION_REPORT);
            if (ai.Running())
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Stop the program", GOSSIP_SENDER_MAIN, ACTION_STOP);
            SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        }

        void SendDrillMenu(Player* player, Creature* creature, MechanicDummyAI const& ai)
        {
            ClearGossipMenuFor(player);
            std::vector<Drill::Def> const& drills = Drill::All();
            for (uint32 i = 0; i < drills.size(); ++i)
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Acore::StringFormat("{}{}: {}", drills[i].name,
                    ai.Program() == drills[i].name ? " (current)" : "", drills[i].summary), GOSSIP_SENDER_MAIN,
                    ACTION_DRILL_FIRST + i);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
            SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        }

        void SendDamageMenu(Player* player, Creature* creature, MechanicDummyAI const& ai)
        {
            ClearGossipMenuFor(player);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Acore::StringFormat("No damage (failures still count){}",
                ai.Damage() == 0 ? " (current)" : ""), GOSSIP_SENDER_MAIN, ACTION_DAMAGE_NONE);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Acore::StringFormat("{} damage per hit{}", kDummyDefaultDamage,
                ai.Damage() == kDummyDefaultDamage ? " (current)" : ""), GOSSIP_SENDER_MAIN, ACTION_DAMAGE_FLAT);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
            SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        }

        // Applies a choice and tells the player what happens next.
        void Apply(Player* player, Creature* creature, MechanicDummyAI& ai, std::string const& program, int32 damage)
        {
            std::string err;
            if (!ai.Configure(program, damage, player, err))
            {
                Tell(creature, player, "That did not work: " + err);
                return;
            }
            Tell(creature, player, Acore::StringFormat("Program: {}, {} damage per hit. {}",
                DescribeProgram(ai.Program()), ai.Damage(), ai.Running() ? "Running now." : "Attack me to start."));
        }
    }

    bool HandleMechDummyCommand(ChatHandler* handler, Acore::ChatCommands::Tail args)
    {
        std::vector<std::string_view> const tokens = Acore::Tokenize(args, ' ', false);
        Creature* creature = handler->getSelectedCreature();
        MechanicDummyAI* ai = DummyAI(creature);
        if (tokens.empty() || !ai)
        {
            handler->SendErrorMessage("Usage: .mech dummy <program|stop|report> [dmg=<n>], with a Mechanics "
                "Training Dummy ({}) selected. Programs: all (default), none, or comma-separated drill names, "
                "lanes, barrage, minefield and uminefield, each with an optional :<cycles or seconds>.",
                Ids::NPC_MECHANIC_DUMMY);
            return false;
        }
        std::string const what(tokens[0]);
        Player* gm = handler->GetPlayer();
        if (what == "stop")
        {
            ai->Stop();
            handler->PSendSysMessage("Training dummy: program stopped (it starts again on the next engage).");
            return true;
        }
        if (what == "report")
        {
            std::vector<std::string> const lines = ai->ReportLines();
            for (std::string const& line : lines)
                handler->PSendSysMessage("{}", line);
            if (lines.empty())
                handler->PSendSysMessage("Training dummy: no failures recorded yet.");
            return true;
        }
        int32 damage = ai->Damage();
        for (std::size_t i = 1; i < tokens.size(); ++i)
        {
            std::string_view const t = tokens[i];
            Optional<int32> v = t.substr(0, 4) == "dmg=" ? Acore::StringTo<int32>(t.substr(4)) : std::nullopt;
            if (!v || *v < 0)
            {
                handler->SendErrorMessage("Bad option '{}' (only dmg=<0 or more>)", t);
                return false;
            }
            damage = *v;
        }
        std::string err;
        if (!ai->Configure(what, damage, gm, err))
        {
            handler->SendErrorMessage("Training dummy: {}", err);
            return false;
        }
        handler->PSendSysMessage("Training dummy: program '{}', {} damage per hit; {}.", ai->Program(), ai->Damage(),
            ai->Running() ? "running now" : "it starts when a player engages it");
        return true;
    }
}

class npc_boss_mechanics_dummy : public CreatureScript
{
public:
    npc_boss_mechanics_dummy() : CreatureScript("npc_boss_mechanics_dummy") { }

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new BossMechanics::MechanicDummyAI(creature);
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        BossMechanics::MechanicDummyAI* ai = BossMechanics::DummyAI(creature);
        if (!ai)
            return false;
        BossMechanics::SendMainMenu(player, creature, *ai);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        using namespace BossMechanics;
        MechanicDummyAI* ai = DummyAI(creature);
        if (!ai)
            return false;
        switch (action)
        {
            case ACTION_MAIN:
                SendMainMenu(player, creature, *ai);
                return true;
            case ACTION_MENU_DRILLS:
                SendDrillMenu(player, creature, *ai);
                return true;
            case ACTION_MENU_DAMAGE:
                SendDamageMenu(player, creature, *ai);
                return true;
            case ACTION_PROGRAM_ALL:
                Apply(player, creature, *ai, kProgramAll, ai->Damage());
                break;
            case ACTION_PROGRAM_BARRAGE:
                Apply(player, creature, *ai, kProgramBarrage, ai->Damage());
                break;
            case ACTION_DAMAGE_NONE:
                Apply(player, creature, *ai, ai->Program(), 0);
                break;
            case ACTION_DAMAGE_FLAT:
                Apply(player, creature, *ai, ai->Program(), kDummyDefaultDamage);
                break;
            case ACTION_REPORT:
                SendReport(creature, *ai, player);
                break;
            case ACTION_STOP:
                ai->Stop();
                Tell(creature, player, "Program stopped. Attack me again to restart it.");
                break;
            default:
            {
                std::vector<Drill::Def> const& drills = Drill::All();
                if (action >= ACTION_DRILL_FIRST && action - ACTION_DRILL_FIRST < drills.size())
                    Apply(player, creature, *ai, drills[action - ACTION_DRILL_FIRST].name, ai->Damage());
                break;
            }
        }
        CloseGossipMenuFor(player);
        return true;
    }
};

void AddSC_boss_mechanics_dummy()
{
    new npc_boss_mechanics_dummy();
}
