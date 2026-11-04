// Standalone tests for src/MechanicLockRules.h, the pure rules of the bots' lock multiplier (STAGE-M4-HANDOFF §3.5,
// WP-X; STAGE-M4-L-REPORT §6.3, §10 X1-X3): the scripted-mover table and its conditions, the named movers and the
// turning self casts, and the look-away cast rule.
// Build: tests/run.sh.

#include "check.h"
#include "MechanicLockRules.h"
#include <cstdint>
#include <set>

using namespace BossMechanics::LockRules;

namespace
{
    // The six scripted movers are pinned: an accidental removal (or a stray addition) fails here.
    void TestScriptedMoverTable()
    {
        CHECK(kScriptedMovers.size() == 6);
        std::set<uint32_t> ids;
        for (ScriptedMover const& mover : kScriptedMovers)
            ids.insert(mover.spellId);
        CHECK(ids.size() == kScriptedMovers.size());    // no duplicate row
        for (uint32_t id : { 204148u, 51690u, 8676u, 203963u, 204400u, 48020u })
            CHECK(FindScriptedMover(id) != nullptr);

        CHECK(FindScriptedMover(204148)->rule == MoverRule::Always);    // Death from Above
        CHECK(FindScriptedMover(51690)->rule == MoverRule::Always);     // Killing Spree
        CHECK(FindScriptedMover(8676)->rule == MoverRule::StealthOutOfMelee);   // Ambush
        CHECK(FindScriptedMover(203963)->rule == MoverRule::Always);    // Grappling Hook
        CHECK(FindScriptedMover(204400)->rule == MoverRule::Always);    // Heroic Leap
        CHECK(FindScriptedMover(48020)->rule == MoverRule::Always);     // Demonic Circle: Teleport

        // Not movers: 0, the Ambush ranks without the teleport script, the teleport helpers themselves (they are cast
        // by the scripts, never by an action), Shadowstep (its effect is TELEPORT_UNITS, the effect test sees it).
        for (uint32_t id : { 0u, 8724u, 8725u, 11267u, 11268u, 11269u, 27441u, 48689u, 48690u, 48691u, 203726u,
                 57840u, 36554u })
            CHECK(FindScriptedMover(id) == nullptr);
    }

    void TestScriptedMoverConditions()
    {
        // Always: whatever the state.
        for (bool stealthed : { false, true })
            for (bool hasTarget : { false, true })
                for (bool inMelee : { false, true })
                    CHECK(ScriptedMoverMoves(MoverRule::Always, stealthed, hasTarget, inMelee));

        // Ambush: only from stealth on a target out of melee range (spell_rog_ambush's CheckCast / BeforeCast).
        CHECK(ScriptedMoverMoves(MoverRule::StealthOutOfMelee, true, true, false));
        CHECK(!ScriptedMoverMoves(MoverRule::StealthOutOfMelee, true, true, true));     // in melee: a plain strike
        CHECK(!ScriptedMoverMoves(MoverRule::StealthOutOfMelee, false, true, false));   // out of stealth: no teleport
        CHECK(!ScriptedMoverMoves(MoverRule::StealthOutOfMelee, false, true, true));
        CHECK(!ScriptedMoverMoves(MoverRule::StealthOutOfMelee, true, false, false));   // no target: the cast fails

        // Through the id.
        CHECK(IsScriptedMoverCast(8676, true, true, false));
        CHECK(!IsScriptedMoverCast(8676, true, true, true));
        CHECK(IsScriptedMoverCast(51690, false, true, true));
        CHECK(IsScriptedMoverCast(204148, false, true, true));
        CHECK(!IsScriptedMoverCast(0, true, true, false));
        CHECK(!IsScriptedMoverCast(8724, true, true, false));
    }

    void TestNamedMovers()
    {
        // M2/M3 names keep their place.
        for (char const* name : { "heroic leap", "heroic leap loose", "cmbt grappling hook", "cmbt blade rush",
                 "cmbt pack blade rush", "shadowstep", "subt shadowstep", "blink back", "disengage",
                 "hunter disengage" })
            CHECK(IsNamedMoverName(name));
        // M4 X3 interim names.
        CHECK(IsNamedMoverName("arms intervene"));
        CHECK(IsNamedMoverName("subt death from above"));
        CHECK(kNamedMovers.size() == 12);
        // Exact names only: not a prefix, not the bare spell names of name casts the effect test covers.
        for (char const* name : { "", "arms", "intervene", "death from above", "arms intervene::x", "charge",
                 "killing spree", "ambush", "frozen orb" })
            CHECK(!IsNamedMoverName(name));
    }

    void TestTurningSelfCasts()
    {
        for (char const* name : { "frozen orb", "frost frozen orb", "dk frost breath", "dk frost breath plain",
                 "dk frost glacial advance" })
            CHECK(IsTurningSelfCastName(name));
        CHECK(kTurningSelfCasts.size() == 5);
        for (char const* name : { "", "dk frost obliterate", "dk pillar of frost", "frostbolt", "blizzard",
                 "dk death and decay" })
            CHECK(!IsTurningSelfCastName(name));
    }

    LookAwayCast Cast(bool ground, bool turnsSelf, bool currentOut, bool infront, bool offensive, bool targetOut)
    {
        LookAwayCast c;
        c.groundCast = ground;
        c.turnsToCurrentTarget = turnsSelf;
        c.currentTargetOutside = currentOut;
        c.facingInfront = infront;
        c.offensiveTarget = offensive;
        c.targetOutside = targetOut;
        return c;
    }

    void TestLookAwayRule()
    {
        // Ground casts: always blocked (DnD at the bot's own feet turns it to world orientation 0; a heal point has no
        // target to judge).
        CHECK(LookAwayBlocksCast(Cast(true, false, false, false, false, false)));
        CHECK(LookAwayBlocksCast(Cast(true, false, false, true, true, false)));

        // Turning self casts: judged by the current target.
        CHECK(LookAwayBlocksCast(Cast(false, true, true, false, false, false)));
        CHECK(!LookAwayBlocksCast(Cast(false, true, false, false, false, false)));

        // An offensive cast with no spell id (H1: facingInfront false): its target alone decides.
        CHECK(LookAwayBlocksCast(Cast(false, false, false, false, true, true)));
        CHECK(!LookAwayBlocksCast(Cast(false, false, false, false, true, false)));

        // A facing-flagged spell on anything outside the arc (a friendly target too).
        CHECK(LookAwayBlocksCast(Cast(false, false, false, true, false, true)));
        CHECK(!LookAwayBlocksCast(Cast(false, false, false, true, false, false)));

        // A heal (no facing flag, friendly target) behind the bot: allowed.
        CHECK(!LookAwayBlocksCast(Cast(false, false, false, false, false, true)));
        // A self buff: no target outside, nothing turns.
        CHECK(!LookAwayBlocksCast(Cast(false, false, false, false, false, false)));

        // Exhaustive: blocked exactly when one of the three rules holds.
        for (int bits = 0; bits < 64; ++bits)
        {
            LookAwayCast const c = Cast(bits & 1, bits & 2, bits & 4, bits & 8, bits & 16, bits & 32);
            bool const expected = c.groundCast || (c.turnsToCurrentTarget && c.currentTargetOutside)
                || ((c.facingInfront || c.offensiveTarget) && c.targetOutside);
            CHECK(LookAwayBlocksCast(c) == expected);
        }
    }
}

int main()
{
    TestScriptedMoverTable();
    TestScriptedMoverConditions();
    TestNamedMovers();
    TestTurningSelfCasts();
    TestLookAwayRule();
    return Report("lock_rules_test");
}
