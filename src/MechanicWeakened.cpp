/*
 * mod-boss-mechanics: the Weakened debuff (205000) and its AuraScript spell_boss_mechanics_weakened.
 */

#include "MechanicWeakened.h"
#include "BossMechanicsIds.h"
#include "Log.h"
#include "Map.h"
#include "MechanicMath.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include <algorithm>

namespace BossMechanics::Weakened
{
    namespace
    {
        // The exact threat compensator for the aura's current stack count (EFFECT_1, MOD_THREAT).
        void SyncThreat(Aura* aura)
        {
            if (!aura)
                return;
            if (AuraEffect* threat = aura->GetEffect(EFFECT_1))
                threat->ChangeAmount(WeakenedThreatPct(aura->GetStackAmount()));
        }

        Aura* Find(Player const* player)
        {
            return player->GetAura(Ids::SPELL_WEAKENED, player->GetGUID());
        }
    }

    void AddStack(Player* player)
    {
        if (!player || !player->IsInWorld() || !player->IsAlive())
            return;
        Aura* aura = Find(player);
        if (aura)
            aura->ModStackAmount(1);        // clamps at the DBC cap (7) and refreshes the duration
        else
            aura = player->AddAura(Ids::SPELL_WEAKENED, player);
        if (!aura)
        {
            LOG_ERROR("module.bossmechanics", "Weakened ({}) could not be applied to {} (missing spell row?)",
                Ids::SPELL_WEAKENED, player->GetGUID().ToString());
            return;
        }
        SyncThreat(aura);
    }

    void SetStacks(Player* player, uint8 stacks)
    {
        if (!player || !player->IsInWorld())
            return;
        stacks = std::min<uint8>(stacks, Ids::WEAKENED_MAX_STACKS);
        if (stacks == 0)
        {
            player->RemoveAurasDueToSpell(Ids::SPELL_WEAKENED);
            return;
        }
        Aura* aura = Find(player);
        if (!aura)
            aura = player->AddAura(Ids::SPELL_WEAKENED, player);
        if (!aura)
            return;
        aura->SetStackAmount(stacks);
        aura->RefreshDuration();
        SyncThreat(aura);
    }

    uint8 GetStacks(Player const* player)
    {
        Aura const* aura = player ? Find(player) : nullptr;
        return aura ? aura->GetStackAmount() : 0;
    }

    void Clear(Map* map, Player* only)
    {
        if (only)
        {
            only->RemoveAurasDueToSpell(Ids::SPELL_WEAKENED);
            return;
        }
        if (!map)
            return;
        for (MapReference const& ref : map->GetPlayers())
            if (Player* player = ref.GetSource())
                player->RemoveAurasDueToSpell(Ids::SPELL_WEAKENED);
    }
}

// 205000 Weakened: EFFECT_1 (MOD_THREAT) always calculates to 0 and is never recalculated by the engine; the module
// sets the exact compensator after every stack change (Weakened::SyncThreat).
class spell_boss_mechanics_weakened : public AuraScript
{
    PrepareAuraScript(spell_boss_mechanics_weakened);

    void CalcThreat(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        amount = 0;
        canBeRecalculated = false;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_boss_mechanics_weakened::CalcThreat, EFFECT_1,
            SPELL_AURA_MOD_THREAT);
    }
};

void AddSC_boss_mechanics_weakened()
{
    RegisterSpellScript(spell_boss_mechanics_weakened);
}
