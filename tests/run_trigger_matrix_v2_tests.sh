#!/bin/bash
# Portable test runner for TriggerMatrixV2.
# Suite 1: legacy predicate/contract checks (formulas, roles, stacking,
#   warm-ups, fingerprint, self-contained structural gate).
# Suite 2: self-contained internal-producer checks (bands, VPOC ties,
#   68% VA expansion, diagonal ratios, exact-tick VAP rows, integration).
# -Wno-unused-function: each TU includes the whole cpp, so helpers used only
# by the other TU (or only by the ACSIL body) warn harmlessly; the Sierra
# build uses all of them.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
# -I tests: TriggerMatrixV2.cpp includes sierrachart.h on line 1 for F5.
# Portable tests must not pull ACSIL; tests/sierrachart.h is a stub.
g++ -std=c++17 -Wall -Wextra -Wno-unused-function -O2 -I tests -o build/tmv2_tests tests/test_trigger_matrix_v2.cpp
./build/tmv2_tests
g++ -std=c++17 -Wall -Wextra -Wno-unused-function -O2 -I tests -o build/tmv2_selfcontained_tests tests/test_trigger_matrix_v2_selfcontained.cpp
./build/tmv2_selfcontained_tests
