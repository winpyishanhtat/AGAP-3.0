#!/usr/bin/env bash
# Compiles and runs the host-side unit tests for agap_logic.h. No Arduino
# toolchain needed - any g++ with C++14 support works.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
g++ -std=c++14 -Wall -Wextra -Werror -I.. test_agap_logic.cpp -o test_agap_logic
./test_agap_logic
