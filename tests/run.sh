#!/usr/bin/env bash
# Builds and runs the standalone mod-boss-mechanics tests with the host compiler.
# Pure headers only (src/MechanicGeometry.h, src/MechanicMath.h, src/MechanicSolver.h, src/MechanicFallibility.h,
# src/BossMechanicsIds.h): no server build needed.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

CXX="${CXX:-g++}"
status=0
for test in geometry_test math_test barrage_test solver_test; do
  "$CXX" -std=c++20 -Wall -Wextra -Werror -O1 -I "$root/src" -I "$here" "$here/$test.cpp" -o "$out/$test"
  if ! "$out/$test"; then
    status=1
  fi
done
exit $status
