#!/bin/sh
# Compile-only check of the three translation units against the REAL Spore ModAPI headers, using clang
# (via the "ziglang" PyPI package) targeting 32-bit Windows (x86-windows-gnu).
#
# This is NOT an MSVC build and it does NOT link (SporeModAPI.lib / detours.lib are MSVC import libraries that
# cannot be built here). It proves that the code parses, type-checks and generates object code against the SDK headers.
#
# Usage: sh tests/clang_compile_check.sh /path/to/Spore-ModAPI      (git clone of Spore-Community/Spore-ModAPI)
#
# Differences from the MSVC build, all applied ONLY to temporary copies under $WORK:
#   * SDK headers: '\' in #include lines -> '/', 4 missing-case Windows headers shimmed, and MSVC-only constructs
#     neutralized (Args() macro groups in 11 headers, default arguments bound to temporaries in 2 headers,
#     one function-pointer->void* conversion in CppRevEng.h, 2 ASSERT_SIZE of unrelated classes that depend on MSVC layout).
#   * ChiefObserver.cpp: the documented ModAPI form "T::DETOUR(...)" is rewritten as an explicit member specialization
#     (clang requires it), and __try/__except is replaced by plain ifs (clang cannot lower x86 mingw SEH).
set -e
MODAPI="${1:?usage: clang_compile_check.sh /path/to/Spore-ModAPI}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$HERE/.."
WORK="${TMPDIR:-/tmp}/tco_clang"
rm -rf "$WORK"; mkdir -p "$WORK/shim" "$WORK/proj" "$WORK/obj"

python3 -m ziglang version >/dev/null 2>&1 || pip3 install --break-system-packages ziglang >/dev/null

cp -r "$MODAPI/Spore ModAPI" "$WORK/ModAPI"; cp -r "$MODAPI/EASTL-3.02.01" "$WORK/EASTL"; cp -r "$MODAPI/Detours" "$WORK/Detours"
cp "$PROJ"/*.cpp "$PROJ"/*.h "$WORK/proj/"

WORK="$WORK" python3 - <<'PY'
import os, re
W = os.environ['WORK']
# 1. include paths: backslash -> slash (everything copied)
for root in ('ModAPI', 'EASTL', 'Detours', 'proj'):
    for dp, _, fs in os.walk(os.path.join(W, root)):
        for f in fs:
            if f.endswith(('.h', '.hpp', '.inl', '.cpp')):
                p = os.path.join(dp, f)
                s = open(p, encoding='utf-8-sig', errors='ignore').read()
                s2 = re.sub(r'(?m)^([ \t]*#[ \t]*include[^\n]*)$', lambda m: m.group(1).replace('\\', '/'), s)
                if s2 != s: open(p, 'w', encoding='utf-8').write(s2)
S = os.path.join(W, 'ModAPI', 'Spore')
names = ['auto_METHOD', 'auto_METHOD_', 'auto_METHOD_VOID', 'auto_STATIC_METHOD']
hdr = ''.join('#pragma push_macro("%s")\n#undef %s\n#define %s(...)\n' % (n, n, n) for n in names)
ftr = ''.join('#pragma pop_macro("%s")\n' % n for n in names)
# 2. MSVC-style Args() macro groups
for f in ['App/Property.h','RenderWare/CompiledState.h','Editors/EditorRigblock.h','UTFWin/WinComboBox.h','UTFWin/WinTextEdit.h',
          'UTFWin/WinSlider.h','UTFWin/WinButton.h','UTFWin/Graphics2D.h','Input.h','Resource/IKeyFilter.h','Simulator/Cell/cCellResource.h']:
    p = os.path.join(S, f); s = open(p, encoding='utf-8', errors='ignore').read()
    s = s.replace('#pragma once', '#pragma once\n// mirror-only\n' + hdr, 1)
    s = re.sub(r'\binline\s+(auto_)', r'\1', s)
    open(p, 'w', encoding='utf-8').write(s.rstrip('\n') + '\n' + ftr)
def sub(f, a, b, count=0):
    p = os.path.join(S, f); s = open(p, encoding='utf-8', errors='ignore').read()
    assert a in s, (f, a)
    open(p, 'w', encoding='utf-8').write(s.replace(a, b) if not count else s.replace(a, b, count))
# 3. default arguments bound to temporaries (MSVC extension)
sub('MathUtils.h', 'BoundingBox& dst = BoundingBox()', 'BoundingBox& dst/*mirror-only*/')
sub('Graphics/IModelWorld.h', 'FilterSettings& settings = FilterSettings()', 'FilterSettings settings = FilterSettings()/*mirror-only*/')
# 4. function pointer -> void* (MSVC extension) in the ModAPI detour helper
sub('CppRevEng.h', '(&(PVOID&)original_function, detoured_function)', '(&(PVOID&)original_function, (PVOID)detoured_function)')
# 5. layout assertions of two classes unrelated to this mod (depend on MSVC struct layout)
sub('Simulator/cPlayer.h', 'ASSERT_SIZE(cSpacePlayerWarData, 0x40);', '/*mirror-only*/')
sub('Simulator/cRaidEvent.h', 'ASSERT_SIZE(cRaidEvent, 0x250);', '/*mirror-only*/')
# 6. the observer itself: clang needs the explicit member specialization; no x86 mingw SEH
p = os.path.join(W, 'proj', 'ChiefObserver.cpp'); s = open(p).read()
a = 'void ForwardWalkTree_detour::DETOUR('
assert a in s
s = s.replace(a, 'template<> void static_detour_<ForwardWalkTree_detour, void(float, double, void*, void*, void*, void*, void*, void*)>::detoured(')
open(p, 'w').write(s)
PY

printf '#pragma once\n#include <sdkddkver.h>\n' > "$WORK/shim/SDKDDKVer.h"
printf '#pragma once\n#include <windows.h>\n'  > "$WORK/shim/Windows.h"
printf '#pragma once\n#include <winuser.h>\n'  > "$WORK/shim/WinUser.h"
cat > "$WORK/shim/force.h" <<'H'
#pragma once
#define __in_z
#define __in
#define __out
#define __inout
#define __in_opt
#define __out_opt
#define __deref_out
H

cxx() {
  python3 -m ziglang c++ -target x86-windows-gnu -std=c++17 -fms-extensions -fdelayed-template-parsing -malign-double \
    -DWIN32 -DNDEBUG -D_WINDOWS -D_USRDLL -D_CRT_SECURE_NO_WARNINGS \
    -isystem "$WORK/ModAPI" -isystem "$WORK/EASTL/test/packages/EABase/include/Common" -isystem "$WORK/EASTL/test/packages/EAAssert/include" \
    -isystem "$WORK/EASTL/test/packages/EAStdC/include" -isystem "$WORK/EASTL/include" -isystem "$WORK/Detours/include" \
    -I"$WORK/proj" -I"$WORK/shim" -include "$WORK/shim/force.h" -Wall -Wextra "$@"
}
status=0
for f in stdafx dllmain; do
  echo "== $f.cpp"; (cd "$WORK/proj" && cxx -c "$f.cpp" -o "$WORK/obj/$f.o" 2>&1 | grep -v "unused during compilation") || true
  [ -s "$WORK/obj/$f.o" ] && echo "   -> object generated" || { echo "   -> FAILED"; status=1; }
done
echo "== ChiefObserver.cpp"
(cd "$WORK/proj" && cxx -c ChiefObserver.cpp "-D__try=if (true)" "-D__except(x)=if (false)" -o "$WORK/obj/ChiefObserver.o" 2>&1 | grep -v "unused during compilation") || true
[ -s "$WORK/obj/ChiefObserver.o" ] && echo "   -> object generated" || { echo "   -> FAILED"; status=1; }
exit $status
