#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
TEST_TMP=$(mktemp -d)
trap 'rm -rf "$TEST_TMP"' EXIT
c++ -std=c++11 -Wall -Wextra -Werror -Iinclude tests/button_input_test.cpp -o "$TEST_TMP/buttons"
"$TEST_TMP/buttons"
c++ -std=c++11 -ffunction-sections -fdata-sections -Itests/host_stubs -Iinclude tests/controls_test.cpp -Wl,--gc-sections -o "$TEST_TMP/controls"
"$TEST_TMP/controls"
c++ -std=c++11 -Itests/host_stubs -Iinclude tests/network_test.cpp -o "$TEST_TMP/network"
"$TEST_TMP/network"
c++ -std=c++11 -Itests/host_stubs -Iinclude tests/display_test.cpp -o "$TEST_TMP/display"
"$TEST_TMP/display"
