#!/bin/bash
# Portable test runner for Effort vs Result Evaluator.
set -e
cd "$(dirname "$0")/.."
g++ -std=c++17 -Wall -Wextra -O2 -o /tmp/evr_eval_tests tests/test_effort_vs_result_evaluator.cpp
/tmp/evr_eval_tests
