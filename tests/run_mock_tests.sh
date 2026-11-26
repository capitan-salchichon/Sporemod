#!/bin/sh
# Linux SIMULATION of the observer glue (ChiefObserver.cpp) against minimal stand-ins of Windows/ModAPI.
# It checks the prototype's own logic (address verification, conditional attach, "original is always called",
# identification, log throttling). It does NOT replace building with MSVC + the ModAPI SDK.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$HERE/.."
OUT="${TMPDIR:-/tmp}/tco_mock"
mkdir -p "$OUT"
# Adapt only: includes, the DETOUR definition (GCC needs the explicit member specialization MSVC does not).
sed -e 's|^#include "stdafx.h"|#include "mock_stdafx.h"|' \
    -e '/^#include <Spore\\/d' \
    -e '/^#include <share.h>/d' \
    -e 's|void ForwardWalkTree_detour::DETOUR(|template<> void static_detour_<ForwardWalkTree_detour, void(float, double, void*, void*, void*, void*, void*, void*)>::detoured(|' \
    "$PROJ/ChiefObserver.cpp" > "$OUT/ChiefObserver.transformed.cpp"
cp "$HERE/mock/mock_env.h" "$HERE/mock/mock_stdafx.h" "$HERE/mock/mock_main.cpp" "$PROJ/ObserverCore.h" "$PROJ/ChiefObserver.h" "$OUT/"
g++ -std=c++14 -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter -Wno-unused-function -I"$OUT" "$OUT/mock_main.cpp" -o "$OUT/mock_main"
status=0
for s in probe nobytes short badbytes outside nonexec basemismatch verified budget; do
    echo "== $s"
    "$OUT/mock_main" "$s" || status=1
done
exit $status
