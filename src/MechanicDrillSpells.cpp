/*
 * mod-boss-mechanics: C++ behind the M5 drill stand-in spells (STAGE-M5-HANDOFF §3.6, WP-K).
 *
 * Doom Mark (drill) 205019, the deadline debuff: when it runs out (not when it is dispelled or removed otherwise) the
 * marked player casts Doom Burst (drill) 205020, Shadow damage to its allies within 10 yd. The dbc-tools DSL has no
 * on-expire trigger (STAGE-M5-K-DATA-REPORT), and a unit hook needs no spell_script_names row, so the burst works on
 * any build whose spell_dbc has the two rows. The hook costs one id compare per aura removal.
 */

#include "BossMechanicsIds.h"
#include "Log.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "UnitScript.h"

class BossMechanicsDrillSpellsUnitScript : public UnitScript
{
public:
    BossMechanicsDrillSpellsUnitScript() : UnitScript("BossMechanicsDrillSpellsUnitScript", true,
        { UNITHOOK_ON_AURA_REMOVE }) { }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        if (mode != AURA_REMOVE_BY_EXPIRE || !aurApp || !unit)
            return;
        Aura const* aura = aurApp->GetBase();
        if (!aura || aura->GetId() != BossMechanics::Ids::SPELL_DRILL_DEADLINE)
            return;
        if (!unit->IsInWorld() || !unit->IsAlive())
            return;
        if (!sSpellMgr->GetSpellInfo(BossMechanics::Ids::SPELL_DRILL_DEADLINE_BURST))
        {
            LOG_ERROR("module.bossmechanics", "Doom Mark (drill) {} expired on {}, but spell {} is missing "
                "(spell_dbc row)", BossMechanics::Ids::SPELL_DRILL_DEADLINE, unit->GetGUID().ToString(),
                BossMechanics::Ids::SPELL_DRILL_DEADLINE_BURST);
            return;
        }
        // Triggered: no cast time, no cost, and the doomed player casts it even while stunned or silenced. The player
        // is the caster (no original caster): the spell picks the caster's allies around it (STAGE-M5-K-DATA-REPORT).
        unit->CastSpell(unit, BossMechanics::Ids::SPELL_DRILL_DEADLINE_BURST, true);
    }
};

void AddSC_boss_mechanics_drill_spells()
{
    new BossMechanicsDrillSpellsUnitScript();
}
