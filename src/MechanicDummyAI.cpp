/*
 * mod-boss-mechanics: npc_boss_mechanics_dummy (Mechanics Training Dummy 300402, STAGE-M4-HANDOFF §3.2, WP-K; and the
 * Descriptor Training Dummy 300403, STAGE-M5-HANDOFF §3.6, M5 WP-K). See MechanicDummyAI.h for the behavior; the
 * program runner is MechanicDrillRunner.h, the descriptor drills MechanicDescriptorDrills.h.
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
#include "WorldSession.h"
#include "Tokenize.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace BossMechanics
{
    namespace
    {
        constexpr char const* kLog = "module.bossmechanics";

        // Gossip actions (sender GOSSIP_SENDER_MAIN). A drill pick is ACTION_DRILL_FIRST + its index in Drill::All();
        // a descriptor drill pick (300403) ACTION_DESC_DRILL_FIRST + its index in Drill::ProgramDrills().
        enum DummyGossipAction : uint32
        {
            ACTION_MAIN = 1,
            ACTION_PROGRAM_ALL,
            ACTION_PROGRAM_BARRAGE,
            ACTION_MENU_DRILLS,
            ACTION_MENU_DAMAGE,
            ACTION_DAMAGE_NONE,
            ACTION_DAMAGE_FLAT,
            ACTION_DAMAGE_PERCENT,
            ACTION_REPORT,
            ACTION_STOP,
            ACTION_PROGRAM_DESCRIPTOR,          // M5, 300403
            ACTION_PROGRAM_DESCRIPTOR_DRILLS,
            ACTION_MENU_DESC_DRILLS,
            ACTION_DRILL_FIRST = 100,
            ACTION_DESC_DRILL_FIRST = 200
        };

        constexpr char const* kProgramAll = "all";
        constexpr char const* kProgramBarrage = "barrage";

        std::string PlayerName(ObjectGuid guid)
        {
            if (Player* p = ObjectAccessor::FindConnectedPlayer(guid))
                return p->GetName();
            return guid.ToString();
        }

        // The program a dummy starts with: every M4 drill on 300402, the descriptor loop on 300403 (M5).
        char const* DefaultProgram(uint32 entry)
        {
            return entry == Ids::NPC_DESCRIPTOR_DUMMY ? Drill::kDescriptorProgram : kProgramAll;
        }

        std::string DescribeProgram(std::string const& program)
        {
            if (program.empty() || program == kProgramAll)
                return "every drill, then barrage and minefield";
            if (program == kProgramBarrage)
                return "barrage only";
            if (Drill::ProgramDef const* d = Drill::FindProgramDrill(program))
                return Acore::StringFormat("{} ({})", d->name, d->summary);
            return program;
        }

        void Tell(Creature* me, Player* player, std::string const& text)
        {
            if (me && player)
                me->Whisper(text, LANG_UNIVERSAL, player);
        }

        std::string DescribeDamage(int32 damage, bool percent)
        {
            if (percent)
                return "each mechanic's percent of health";
            return Acore::StringFormat("{} damage per hit", damage);
        }
    }

    MechanicDummyAI::MechanicDummyAI(Creature* creature) : NullCreatureAI(creature),
        _program(DefaultProgram(creature->GetEntry()))
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
        opt.percent = _percent;
        opt.includeScopePlayer = true;      // a player alone on the dummy is the group
        opt.loop = true;
        // The reference-health provider reads the owner's level (percent damage); combat plays the dummy at the
        // attacker's level + 3 anyway (BOSS_MOB). Not sent to the client, as the test owner does.
        me->SetLevel(scope->GetLevel(), false);
        _scopePlayer = scope->GetGUID();
        _lastReport.clear();
        // A new scope: the last group's players leave the scope set (STAGE-M4-R-REPORT m-5); StartProgram adds the
        // new scope player and the next check the players fighting.
        if (Registry* reg = Registry::Get(me->GetMap()))
            if (Encounter* e = reg->FindEncounterMutable(me->GetGUID()))
                e->testGms.clear();
        if (!StartProgram(me, scope->GetGUID(), _program, opt, err))
        {
            LOG_WARN(kLog, "Training dummy {}: program '{}' did not start for {}: {}", me->GetGUID().ToString(),
                _program, scope->GetName(), err);
            return false;
        }
        _started = true;
        LOG_DEBUG(kLog, "Training dummy {}: program '{}' for {} ({})", me->GetGUID().ToString(), _program,
            scope->GetName(), DescribeDamage(_damage, _percent));
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

        // The scope set is the scope player and every player in the fight (the open-world bot gate and the dummy's
        // targeting read it); a player who left the fight drops out (STAGE-M4-R-REPORT m-5).
        if (Registry* reg = Registry::Get(me->GetMap()))
            if (reg->FindEncounter(me->GetGUID()))
            {
                GuidSet scopeSet;
                scopeSet.insert(_scopePlayer);
                for (Player const* p : fighting)
                    scopeSet.insert(p->GetGUID());
                reg->SetScopePlayers(me->GetGUID(), std::move(scopeSet));
            }
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

    bool MechanicDummyAI::MayChange(Player const* player) const
    {
        if (!player)
            return false;
        if (!Running() || player->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
            return true;
        Player const* scope = ObjectAccessor::GetPlayer(me->GetMap(), _scopePlayer);
        if (!scope)
            return true;    // the scope player left the map: the next check rescopes anyway
        return player == scope || player->IsInSameRaidWith(scope);
    }

    bool MechanicDummyAI::Configure(std::string const& program, int32 damage, bool percent, Player* by,
        std::string& err)
    {
        if (!CheckProgram(program, err))
            return false;
        _program = program.empty() ? std::string(DefaultProgram(me->GetEntry())) : program;
        _damage = std::max<int32>(damage, 0);
        _percent = percent;
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
            if (!creature || !Ids::IsTrainingDummy(creature->GetEntry()))
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

        // The Descriptor Training Dummy's menu (300403, M5): the descriptor loop, every descriptor drill in turn, one
        // descriptor drill, the report and stop. Its programs deal no `.mech test` damage, so it has no damage menu.
        void SendDescriptorMainMenu(Player* player, Creature* creature, MechanicDummyAI const& ai)
        {
            ClearGossipMenuFor(player);
            std::string const& p = ai.Program();
            auto mark = [](bool on) { return on ? " (current)" : ""; };
            Drill::ProgramDef const* current = Drill::FindProgramDrill(p);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Acore::StringFormat("Program: every list at once (60 s "
                "loop){}", mark(p == Drill::kDescriptorProgram)), GOSSIP_SENDER_MAIN, ACTION_PROGRAM_DESCRIPTOR);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Acore::StringFormat("Program: every descriptor drill in "
                "turn{}", mark(p == Drill::kDescriptorDrillsProgram)), GOSSIP_SENDER_MAIN,
                ACTION_PROGRAM_DESCRIPTOR_DRILLS);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Acore::StringFormat("Program: one descriptor drill...{}",
                mark(current && current->listed && p != Drill::kDescriptorProgram
                    && p != Drill::kDescriptorDrillsProgram)), GOSSIP_SENDER_MAIN, ACTION_MENU_DESC_DRILLS);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Whisper me the failure report", GOSSIP_SENDER_MAIN,
                ACTION_REPORT);
            if (ai.Running())
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Stop the program", GOSSIP_SENDER_MAIN, ACTION_STOP);
            SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        }

        void SendDescriptorDrillMenu(Player* player, Creature* creature, MechanicDummyAI const& ai)
        {
            ClearGossipMenuFor(player);
            std::vector<Drill::ProgramDef> const& drills = Drill::ProgramDrills();
            for (uint32 i = 0; i < drills.size(); ++i)
            {
                Drill::ProgramDef const& d = drills[i];
                if (!d.listed || d.name == std::string_view(Drill::kDescriptorProgram)
                    || d.name == std::string_view(Drill::kDescriptorDrillsProgram))
                    continue;
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Acore::StringFormat("{}{}: {}", d.name,
                    ai.Program() == d.name ? " (current)" : "", d.summary), GOSSIP_SENDER_MAIN,
                    ACTION_DESC_DRILL_FIRST + i);
            }
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
            SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        }

        void SendMainMenu(Player* player, Creature* creature, MechanicDummyAI const& ai)
        {
            if (creature->GetEntry() == Ids::NPC_DESCRIPTOR_DUMMY)
            {
                SendDescriptorMainMenu(player, creature, ai);
                return;
            }
            ClearGossipMenuFor(player);
            std::string const& p = ai.Program();
            auto mark = [](bool on) { return on ? " (current)" : ""; };
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Acore::StringFormat("Program: every drill, then barrage and "
                "minefield{}", mark(p == kProgramAll)), GOSSIP_SENDER_MAIN, ACTION_PROGRAM_ALL);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Acore::StringFormat("Program: one drill...{}",
                mark(Drill::Find(p) != nullptr)), GOSSIP_SENDER_MAIN, ACTION_MENU_DRILLS);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Acore::StringFormat("Program: barrage only{}",
                mark(p == kProgramBarrage)), GOSSIP_SENDER_MAIN, ACTION_PROGRAM_BARRAGE);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Acore::StringFormat("Damage: {}...",
                DescribeDamage(ai.Damage(), ai.Percent())), GOSSIP_SENDER_MAIN, ACTION_MENU_DAMAGE);
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
                !ai.Percent() && ai.Damage() == 0 ? " (current)" : ""), GOSSIP_SENDER_MAIN, ACTION_DAMAGE_NONE);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Acore::StringFormat("{} damage per hit{}", kDummyDefaultDamage,
                !ai.Percent() && ai.Damage() == kDummyDefaultDamage ? " (current)" : ""), GOSSIP_SENDER_MAIN,
                ACTION_DAMAGE_FLAT);
            // Real damage: each mechanic's percent of reference health (20% for most, as a dungeon boss would hit).
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Acore::StringFormat("Percent of health, as a boss hits "
                "(dangerous){}", ai.Percent() ? " (current)" : ""), GOSSIP_SENDER_MAIN, ACTION_DAMAGE_PERCENT);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
            SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        }

        // The answer to a player who may not change a running program (MechanicDummyAI::MayChange).
        void RefuseChange(Player* player, Creature* creature, MechanicDummyAI const& ai)
        {
            Player const* scope = ObjectAccessor::GetPlayer(creature->GetMap(), ai.ScopePlayer());
            std::string const who = scope ? scope->GetName() : std::string("another player");
            Tell(creature, player, Acore::StringFormat("The program runs for {}'s group: only that group or a GM can "
                "change or stop it. You can still read the report.", who));
        }

        // Applies a choice and tells the player what happens next.
        void Apply(Player* player, Creature* creature, MechanicDummyAI& ai, std::string const& program, int32 damage,
            bool percent)
        {
            if (!ai.MayChange(player))
            {
                RefuseChange(player, creature, ai);
                return;
            }
            std::string err;
            if (!ai.Configure(program, damage, percent, player, err))
            {
                Tell(creature, player, "That did not work: " + err);
                return;
            }
            Tell(creature, player, Acore::StringFormat("Program: {}, {}. {}", DescribeProgram(ai.Program()),
                DescribeDamage(ai.Damage(), ai.Percent()), ai.Running() ? "Running now." : "Attack me to start."));
        }
    }

    bool HandleMechDummyCommand(ChatHandler* handler, Acore::ChatCommands::Tail args)
    {
        std::vector<std::string_view> const tokens = Acore::Tokenize(args, ' ', false);
        Creature* creature = handler->getSelectedCreature();
        MechanicDummyAI* ai = DummyAI(creature);
        if (tokens.empty() || !ai)
        {
            handler->SendErrorMessage("Usage: .mech dummy <program|stop|report> [dmg=<n>|dmg=pct], with a Mechanics "
                "Training Dummy ({}) or a Descriptor Training Dummy ({}) selected. Programs: all (the default on {}), "
                "none, or comma-separated drill names, lanes, barrage, minefield and uminefield, each with an optional "
                ":<cycles or seconds>, the descriptor drills (descriptor, the default on {}; descriptor-drills, "
                "interrupts, dispels, purges, targets, ignore and their parts, each with an optional :<runs>), and the "
                "descriptor entries wait, cast, add, debuff, buff, swap, hold and describe (arguments split by ':', "
                "e.g. cast:205010:every=4000).",
                Ids::NPC_MECHANIC_DUMMY, Ids::NPC_DESCRIPTOR_DUMMY, Ids::NPC_MECHANIC_DUMMY,
                Ids::NPC_DESCRIPTOR_DUMMY);
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
        bool percent = ai->Percent();
        for (std::size_t i = 1; i < tokens.size(); ++i)
        {
            std::string_view const t = tokens[i];
            if (t == "dmg=pct")
            {
                percent = true;
                continue;
            }
            Optional<int32> v = t.substr(0, 4) == "dmg=" ? Acore::StringTo<int32>(t.substr(4)) : std::nullopt;
            if (!v || *v < 0)
            {
                handler->SendErrorMessage("Bad option '{}' (only dmg=<0 or more> or dmg=pct)", t);
                return false;
            }
            damage = *v;
            percent = false;
        }
        std::string err;
        if (!ai->Configure(what, damage, percent, gm, err))
        {
            handler->SendErrorMessage("Training dummy: {}", err);
            return false;
        }
        handler->PSendSysMessage("Training dummy: program '{}', {}; {}.", ai->Program(),
            DescribeDamage(ai->Damage(), ai->Percent()), ai->Running() ? "running now" : "it starts when a player "
            "engages it");
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
            case ACTION_MENU_DESC_DRILLS:
                SendDescriptorDrillMenu(player, creature, *ai);
                return true;
            case ACTION_PROGRAM_DESCRIPTOR:
                Apply(player, creature, *ai, Drill::kDescriptorProgram, ai->Damage(), ai->Percent());
                break;
            case ACTION_PROGRAM_DESCRIPTOR_DRILLS:
                Apply(player, creature, *ai, Drill::kDescriptorDrillsProgram, ai->Damage(), ai->Percent());
                break;
            case ACTION_PROGRAM_ALL:
                Apply(player, creature, *ai, kProgramAll, ai->Damage(), ai->Percent());
                break;
            case ACTION_PROGRAM_BARRAGE:
                Apply(player, creature, *ai, kProgramBarrage, ai->Damage(), ai->Percent());
                break;
            case ACTION_DAMAGE_NONE:
                Apply(player, creature, *ai, ai->Program(), 0, false);
                break;
            case ACTION_DAMAGE_FLAT:
                Apply(player, creature, *ai, ai->Program(), kDummyDefaultDamage, false);
                break;
            case ACTION_DAMAGE_PERCENT:
                Apply(player, creature, *ai, ai->Program(), ai->Damage(), true);
                break;
            case ACTION_REPORT:
                SendReport(creature, *ai, player);
                break;
            case ACTION_STOP:
                if (!ai->MayChange(player))
                {
                    RefuseChange(player, creature, *ai);
                    break;
                }
                ai->Stop();
                Tell(creature, player, "Program stopped. Attack me again to restart it.");
                break;
            default:
            {
                std::vector<Drill::Def> const& drills = Drill::All();
                std::vector<Drill::ProgramDef> const& descDrills = Drill::ProgramDrills();
                if (action >= ACTION_DESC_DRILL_FIRST && action - ACTION_DESC_DRILL_FIRST < descDrills.size())
                    Apply(player, creature, *ai, descDrills[action - ACTION_DESC_DRILL_FIRST].name, ai->Damage(),
                        ai->Percent());
                else if (action >= ACTION_DRILL_FIRST && action - ACTION_DRILL_FIRST < drills.size())
                    Apply(player, creature, *ai, drills[action - ACTION_DRILL_FIRST].name, ai->Damage(),
                        ai->Percent());
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
