#!/usr/bin/env bash
# Runs the real AGAP_HelperButton.ino (the earlier chord-helper build) on this computer against
# a mock Arduino and checks its behaviour scenario by scenario (see sim_helper.cpp). Needs only g++.
#   bash run_tests.sh              all scenarios
#   bash run_tests.sh calib fuzz   just these
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
BIN="$(mktemp -u)_simh"
trap 'rm -f "$BIN" "$BIN.exe"' EXIT
g++ -std=c++14 -Wall -Wextra -include Arduino.h -I../../sim/mock -I.. sim_helper.cpp -o "$BIN" || exit 1
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
