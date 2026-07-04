#!/usr/bin/env bash
# Host-side unit tests for pump_side_ai2 safety logic.
# Compiles the REAL pump_control/failsafe/commands against fakes/ (no hardware,
# no Arduino toolchain — just clang++). Run:  bash test/run.sh
set -euo pipefail
cd "$(dirname "$0")"        # test/
SRC=..                       # pump_side_ai2/

clang++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
  -I fakes -I "$SRC" \
  test_main.cpp \
  "$SRC/commands.cpp" \
  "$SRC/pump_control.cpp" \
  "$SRC/failsafe.cpp" \
  "$SRC/door_control.cpp" \
  -o /tmp/wd_pump_tests

/tmp/wd_pump_tests
