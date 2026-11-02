/*
 * mod-boss-mechanics: the resolve spell script and the EngineBridge implementation (STAGE-M1-HANDOFF §2.4, §3.6).
 *
 * CastResolve: one triggered, instant AoE cast of the mechanic's resolve spell (205001-205007 or a dungeon copy bound
 * to the same script) from a marker, original caster = the owner when it is a live unit in the map (damage, combat log
 * and kill credit use it; the area search's faction checks use the marker, which copies the owner's faction).
 * SPELLVALUE_RADIUS_MOD shrinks the 100 yd DBC radius to the shape's search radius, MISCVALUE0 carries the mechanic id
 * and MISCVALUE1 the pass. Speed 0 + instant + triggered: every hit happens inside the CastSpell call, while the
 * registry's ResolveContext is active.
 *
 * spell_boss_mechanics_resolve is three calls into the registry, so the filter is the same code the bots read:
 * OnObjectAreaTargetSelect -> FilterResolveTargets, OnEffectLaunchTarget -> ResolveDamageFor (per-target flat),
 * AfterHit -> OnResolveHit (ledger, Weakened, onHit), plus the rider cast.
 */

#include "BossMechanicsIds.h"
#include "Creature.h"
#include "EngineBridge.h"
#include "Log.h"
#include "Map.h"
#include "MechanicRegistry.h"
#include "MechanicTelegraph.h"
#include "MechanicWeakened.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include <cmath>

namespace BossMechanics
{
    namespace
    {
        constexpr char const* kLog = "module.bossmechanics";

        Unit* FindUnit(Map* map, ObjectGuid guid)
        {
            if (guid.IsEmpty())
                return nullptr;
            if (guid.IsPlayer())
                return ObjectAccessor::GetPlayer(map, guid);
            if (guid.IsPet())
                return map->GetPet(guid);
            if (guid.IsCreatureOrVehicle())
                return map->GetCreature(guid);
            return nullptr;
        }

        class EngineGlue final : public EngineBridge
        {
        public:
            void SpawnMarkers(Map* map, ActiveMechanic& m) override
            {
                Telegraph::Spawn(map, m);
            }

            void UpdateMarkers(Map* map, ActiveMechanic& m, Milliseconds now) override
            {
                Telegraph::Update(map, m, now);
            }

            void DespawnMarkers(Map* map, ActiveMechanic& m) override
            {
                Telegraph::Despawn(map, m);
            }

            void CastResolve(Map* map, ActiveMechanic& m, uint32 pass) override
            {
                uint32 const spellId = m.damage.ResolveSpell();
                if (!sSpellMgr->GetSpellInfo(spellId))
                {
                    LOG_ERROR(kLog, "Mechanic {}: resolve spell {} does not exist; nothing resolves", m.id, spellId);
                    return;
                }

                Unit* owner = FindUnit(map, m.owner);
                bool const ownerLive = owner && owner->IsInWorld() && owner->IsAlive();

                // Caster: the carrier's marker (Spread/RunOut), else the first marker, else a hidden marker at the
                // pose for this cast only (NoTelegraph, Bait, or markers lost).
                Creature* caster = nullptr;
                if (!m.ctx.carrier.IsEmpty())
                {
                    auto it = m.carrierMarkers.find(m.ctx.carrier);
                    if (it != m.carrierMarkers.end())
                        caster = map->GetCreature(it->second);
                }
                if (!caster && !m.markers.empty())
                    caster = map->GetCreature(m.markers.front());
                bool hidden = false;
                if (!caster || !caster->IsInWorld() || !caster->IsAlive())
                {
                    Position const at(m.ctx.pose.x, m.ctx.pose.y, m.ctx.pose.z, m.ctx.pose.o);
                    caster = Telegraph::SummonMarker(map, at, owner, 0, 1.f);
                    hidden = true;
                }
                if (!caster)
                {
                    LOG_ERROR(kLog, "Mechanic {}: no resolve caster (creature {} could not be summoned)", m.id,
                        Ids::NPC_MECHANIC_MARKER);
                    return;
                }

                float const radius = m.ctx.SearchRadius(caster->GetPositionX(), caster->GetPositionY(),
                    caster->GetPositionZ());
                int32 const radiusMod = static_cast<int32>(std::ceil(radius * 100.f));    // RadiusMod = v / 10000
                CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
                if (ownerLive)
                    args.SetOriginalCaster(owner->GetGUID());
                args.AddSpellMod(SPELLVALUE_RADIUS_MOD, radiusMod);
                args.AddSpellMod(SPELLVALUE_MISCVALUE0, static_cast<int32>(m.id));
                args.AddSpellMod(SPELLVALUE_MISCVALUE1, static_cast<int32>(pass));
                SpellCastResult const result = caster->CastSpell(caster, spellId, args);
                if (result != SPELL_CAST_OK)
                    LOG_WARN(kLog, "Mechanic {} pass {}: resolve spell {} failed to cast (result {})", m.id, pass,
                        spellId, static_cast<uint32>(result));

                if (hidden)
                    caster->DespawnOrUnsummon();
            }

            void AddWeakenedStack(Player* player) override
            {
                Weakened::AddStack(player);
            }

            void ClearWeakened(Map* map, Player* only) override
            {
                Weakened::Clear(map, only);
            }
        };

        EngineGlue sEngineGlue;
    }

    void InstallEngineBridge()
    {
        SetEngineBridge(&sEngineGlue);
    }
}

using namespace BossMechanics;

// 205001-205007 (and any dungeon copy bound to this script): the shape is the hitbox.
class spell_boss_mechanics_resolve : public SpellScript
{
    PrepareSpellScript(spell_boss_mechanics_resolve);

    Registry* GetRegistry()
    {
        Unit* caster = GetCaster();
        return caster && caster->IsInWorld() ? Registry::Get(caster->GetMap()) : nullptr;
    }

    uint32 MechanicId()
    {
        SpellValue const* value = GetSpellValue();
        return value ? value->MiscVal[0] : 0;
    }

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        Registry* reg = GetRegistry();
        uint32 const id = MechanicId();
        if (!reg || !id)
        {
            targets.clear();    // not a registry cast (e.g. `.cast 205001`): hit nobody
            return;
        }
        reg->FilterResolveTargets(id, targets);
    }

    void SetDamage(SpellEffIndex /*effIndex*/)
    {
        Registry* reg = GetRegistry();
        Unit* target = GetHitUnit();
        SetEffectValue(reg && target ? reg->ResolveDamageFor(MechanicId(), target) : 0);
    }

    void AfterHitTarget()
    {
        Registry* reg = GetRegistry();
        Unit* target = GetHitUnit();
        uint32 const id = MechanicId();
        // Player-controlled units only: never a marker, the owner or another creature (no ledger, onHit or rider).
        if (!reg || !target || !id || !target->GetCharmerOrOwnerPlayerOrPlayerItself())
            return;
        uint32 const rider = [reg, id]()
        {
            ActiveMechanic const* m = reg->Find(id);
            return m ? m->damage.riderSpell : 0u;
        }();
        ObjectGuid const original = GetOriginalCaster() ? GetOriginalCaster()->GetGUID() : ObjectGuid::Empty;
        int32 const damage = GetHitDamage();
        reg->OnResolveHit(id, target, damage > 0 ? static_cast<uint32>(damage) : 0);
        if (rider && target->IsInWorld() && target->IsAlive())
            if (Unit* caster = GetCaster())
                caster->CastSpell(target, rider, CastSpellExtraArgs(TRIGGERED_FULL_MASK).SetOriginalCaster(original));
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_boss_mechanics_resolve::FilterTargets, EFFECT_0,
            TARGET_UNIT_SRC_AREA_ENEMY);
        OnEffectLaunchTarget += SpellEffectFn(spell_boss_mechanics_resolve::SetDamage, EFFECT_0,
            SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(spell_boss_mechanics_resolve::AfterHitTarget);
    }
};

void AddSC_boss_mechanics_resolve()
{
    RegisterSpellScript(spell_boss_mechanics_resolve);
}
