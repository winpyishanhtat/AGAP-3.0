#!/usr/bin/env bash
# Logic tests for the fretboard firmware headers (no Arduino toolchain needed).
# The chord solver is checked against the Python one on every chord, so this
# needs Python as well as g++.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
# Use the first interpreter that actually runs ("python3" on Windows can be a
# Store stub that exists but does nothing).
PY=""
for cand in python3 python py; do
  if "$cand" -c "import sys" >/dev/null 2>&1; then PY="$cand"; break; fi
done
[ -n "$PY" ] || { echo "no working Python found" >&2; exit 1; }
EXPECTED="$(mktemp)"
trap 'rm -f "$EXPECTED"' EXIT
$PY dump_python_chords.py > "$EXPECTED"
g++ -std=c++14 -Wall -Wextra -Werror -I.. test_agap_chords.cpp -o test_agap_chords
./test_agap_chords "$EXPECTED"
g++ -std=c++14 -Wall -Wextra -Werror -I.. test_agap_fret.cpp -o test_agap_fret
./test_agap_fret
