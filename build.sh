#!/bin/bash
# Build av-sync-delay (Windows x64) with portable llvm-mingw from WSL/Linux.
set -e
cd "$(dirname "$0")"

TC="$HOME/.local/llvm-mingw/bin"
CXX="$TC/x86_64-w64-mingw32-clang++"
NM="$TC/llvm-nm"
OBJDUMP="$TC/llvm-objdump"
OUT=build

[ -x "$CXX" ] || { echo "toolchain missing: $CXX"; exit 1; }
mkdir -p "$OUT"

echo "== hook dll =="
# -nostdlib payload: entry is DllMain itself, imports only kernel32/ole32
# (both MS-signed and always loadable). No CRT startup, no UCRT api-set
# imports — a manual-mapped payload must not depend on LoadLibrary of
# anything exotic inside sandboxed browser processes.
# NOTE: do not use -ffunction-sections for the injector (stub adjacency).
"$CXX" -shared -O2 -std=c++17 -DUNICODE -D_UNICODE \
    -fno-exceptions -fno-rtti -fno-stack-protector \
    -nostdlib -ffreestanding \
    dll/dllmain.cpp -o "$OUT/avsync_hook.dll" \
    -lkernel32 -lole32 -Wl,--entry,DllMain -Wl,--exclude-all-symbols

# Embed the hook DLL as a byte array header, so the GUI exe is self-contained.
python3 "$(dirname "$0")/embed.py" "$OUT/avsync_hook.dll" \
    "$(dirname "$0")/common/embedded_dll.h" g_embedded_hook_dll

echo "== injector =="
"$CXX" -O2 -std=c++17 -DUNICODE -D_UNICODE -municode -D_WIN32_WINNT=0x0A00 \
    -static-libgcc -static-libstdc++ \
    injector/main.cpp -o "$OUT/avsync.exe" \
    -lkernel32 -ladvapi32 -lntdll -lole32

echo "== measure gui =="
"$CXX" -O2 -std=c++17 -DUNICODE -D_UNICODE -municode -mwindows \
    -static-libgcc -static-libstdc++ \
    measure/main.cpp -o "$OUT/avsync_measure.exe" \
    -lkernel32 -lole32 -luuid -luser32 -lgdi32 -ladvapi32 -lshell32 -lntdll

echo
echo "== hook DLL imports (must all be MS-signed system DLLs) =="
"$OBJDUMP" -p "$OUT/avsync_hook.dll" | grep "DLL Name" || true

echo
echo "== PIC stub adjacency (LoadDllStubEnd must follow LoadDllStub) =="
"$NM" "$OUT/avsync.exe" | grep -i "LoadDllStub" || true

python3 - <<'EOF'
import subprocess, os
tc = os.path.expanduser('~/.local/llvm-mingw/bin')
out = subprocess.run([f'{tc}/llvm-nm', 'build/avsync.exe'], capture_output=True, text=True).stdout
addrs = {}
for line in out.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[2] in ('LoadDllStub', 'LoadDllStubEnd', '_LoadDllStub', '_LoadDllStubEnd'):
        addrs[parts[2].lstrip('_')] = int(parts[0], 16)
if 'LoadDllStub' in addrs and 'LoadDllStubEnd' in addrs:
    size = addrs['LoadDllStubEnd'] - addrs['LoadDllStub']
    print(f'stub size: {size} bytes')
    assert 0 < size < 3800, 'stub adjacency looks wrong!'
    print('stub adjacency OK')
else:
    print('WARNING: stub symbols not found:', addrs)
EOF

echo
ls -la "$OUT"
echo "build OK"
