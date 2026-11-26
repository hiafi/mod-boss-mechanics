#!/usr/bin/env bash
# Builds and runs the standalone mod-boss-mechanics tests with the host compiler.
# Pure headers only (src/MechanicGeometry.h, src/MechanicMath.h, src/MechanicSolver.h, src/MechanicFallibility.h,
# src/BossMechanicsIds.h, src/MechanicAssign.h, src/MechanicRoute.h, src/MechanicViewRules.h, src/MechanicDrill.h,
# src/MechanicDrillPlan.h, src/MechanicLockRules.h, src/MechanicLeaks.h, src/MechanicCost.h, src/MechanicLanding.h,
# src/MechanicDescriptor.h, src/MechanicDescriptorPresets.h, src/MechanicRoleRules.h, src/MechanicDispelRules.h,
# src/MechanicKickRules.h, src/MechanicTargetRules.h, src/MechanicRefHealth.h, src/MechanicJump.h,
# src/MechanicTauntSwap.h, src/MechanicPass.h, src/MechanicFront.h, src/MechanicHide.h, src/MechanicGaze.h,
# src/MechanicHealReadCache.h (gnomer_bmg_test: the BM-G rules in MechanicDescriptor.h, MechanicLockRules.h,
# MechanicAssign.h, MechanicSolver.h);
# tests/brain_model.h models the brain's step cycle on them): no server build needed. drill_parse_test runs every
# drill program's steps through the `.mech test` parser itself, extracted from src/MechanicCommand.cpp by
# tests/extract_parse.awk into parse_extract.inc and compiled against the test's stand-ins for the server types.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

CXX="${CXX:-g++}"
status=0
awk -f "$here/extract_parse.awk" "$root/src/MechanicCommand.cpp" > "$out/parse_extract.inc"
for test in geometry_test math_test barrage_test solver_test solver_m3_test assign_test judge_test drill_test \
    drill_plan_test route_test view_rules_test lock_rules_test bot_stats_test leak_fire_test crowd_test \
    fight_test descriptor_test role_rules_test dispel_rules_test kick_rules_test drop_spot_test \
    target_rules_test ref_health_test hunt_test decoy_test scholo_shapes_test strat_followups_test \
    lethal_test jump_test brs_followups_test taunt_swap_test pass_test front_test brs_upper_followups_test \
    brd_shapes_test descriptor_ds_test hide_test sm_shapes_test sm_drill_test sm2_drill_test rfc_drill_test \
    nopath_test zf_bm1_test zf_drill_test zf_heal_read_cache_test gnomer_bmg_test gnomer_drill_test \
    drill_parse_test; do
  "$CXX" -std=c++20 -Wall -Wextra -Werror -O1 -I "$root/src" -I "$here" -I "$out" "$here/$test.cpp" -o "$out/$test"
  if ! "$out/$test"; then
    status=1
  fi
done
exit $status
