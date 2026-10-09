#!/usr/bin/env bash
# Runs the real AGAP_Fretboard.ino on this computer against a mock Arduino and checks
# its behaviour scenario by scenario (see sim_fretboard.cpp). Needs only g++.
# Each scenario is its own process because each needs a fresh power-up.
#   bash run_tests.sh              all scenarios
#   bash run_tests.sh chord_c fuzz just these
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
BIN="$(mktemp -u)_sim"
trap 'rm -f "$BIN" "$BIN.exe"' EXIT
g++ -std=c++14 -Wall -Wextra -include Arduino.h -I../../sim/mock -I.. sim_fretboard.cpp -o "$BIN" || exit 1
if [ $# -gt 0 ]; then names=("$@"); else mapfile -t names < <("$BIN" --list | tr -d '\r'); fi
pass=0; fail=0
for n in "${names[@]}"; do
  if out="$("$BIN" "$n" 2>&1)"; then
    pass=$((pass + 1)); echo "$out" | tail -n 2 | tr -d '\r'
  else
    fail=$((fail + 1)); echo "$out" | tr -d '\r'
  fi
done
echo
echo "$pass scenarios passed, $fail failed"
[ "$fail" -eq 0 ]
