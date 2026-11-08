#!/usr/bin/env bash
# Builds and runs the standalone mod-boss-mechanics tests with the host compiler.
# Pure headers only (src/MechanicGeometry.h, src/MechanicMath.h, src/MechanicSolver.h, src/MechanicFallibility.h,
# src/BossMechanicsIds.h, src/MechanicAssign.h, src/MechanicRoute.h, src/MechanicViewRules.h, src/MechanicDrill.h,
# src/MechanicDrillPlan.h, src/MechanicLockRules.h, src/MechanicLeaks.h, src/MechanicCost.h, src/MechanicLanding.h,
# src/MechanicDescriptor.h, src/MechanicDescriptorPresets.h, src/MechanicRoleRules.h, src/MechanicDispelRules.h,
# src/MechanicKickRules.h, src/MechanicTargetRules.h, src/MechanicRefHealth.h; tests/brain_model.h models the
# brain's step cycle on them): no server build needed.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

CXX="${CXX:-g++}"
status=0
for test in geometry_test math_test barrage_test solver_test solver_m3_test assign_test judge_test drill_test \
    drill_plan_test route_test view_rules_test lock_rules_test bot_stats_test leak_fire_test crowd_test \
    fight_test descriptor_test role_rules_test dispel_rules_test kick_rules_test \
    target_rules_test ref_health_test hunt_test; do
  "$CXX" -std=c++20 -Wall -Wextra -Werror -O1 -I "$root/src" -I "$here" "$here/$test.cpp" -o "$out/$test"
  if ! "$out/$test"; then
    status=1
  fi
done
exit $status
