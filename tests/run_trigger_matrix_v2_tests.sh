#!/bin/bash
# Portable test runner for TriggerMatrixV2 (pure core behind TMV2_UNITTEST).
set -e
cd "$(dirname "$0")/.."
g++ -std=c++17 -Wall -Wextra -O2 -o /tmp/tmv2_tests tests/test_trigger_matrix_v2.cpp
/tmp/tmv2_tests
