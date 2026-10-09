#!/usr/bin/env bash
# Compiles and runs the native unit tests for agap_control_logic.h.
# Any g++ with C++14 works; no serial port or Arduino toolchain needed.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
g++ -std=c++14 -Wall -Wextra -Werror -I.. test_agap_control_logic.cpp -o test_agap_control_logic
./test_agap_control_logic
g++ -std=c++14 -Wall -Wextra -Werror -I.. test_agap_control_fret.cpp -o test_agap_control_fret
./test_agap_control_fret
