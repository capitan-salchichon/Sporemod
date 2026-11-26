#!/bin/sh
# Builds and runs the platform-independent tests of ObserverCore.h.
set -e
cd "$(dirname "$0")"
g++ -std=c++14 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined test_core.cpp -o "${TMPDIR:-/tmp}/test_core"
"${TMPDIR:-/tmp}/test_core"
