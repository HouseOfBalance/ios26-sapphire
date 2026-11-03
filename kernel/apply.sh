#!/bin/bash
# Make the dada kernel tree: Apple's xnu-12377.121.6 with xnu-12377.121.6-dada.patch applied.
#   kernel/apply.sh DIR      (DIR is created; then build it the way base/xnu-build.sh does)
set -euo pipefail
K=$(cd "$(dirname "$0")" && pwd)
D=${1:?usage: apply.sh DIR}
git clone --depth 1 --branch xnu-12377.121.6 https://github.com/apple-oss-distributions/xnu.git "$D"
git -C "$D" apply --whitespace=nowarn "$K/xnu-12377.121.6-dada.patch"
if [ -f "$K/0001-pac-trap-and-self-patch.patch" ]; then
	git -C "$D" apply --whitespace=nowarn "$K/0001-pac-trap-and-self-patch.patch"
	echo "applied 0001-pac-trap-and-self-patch.patch (ARMv8.0 PAC Trap-and-Self-Patch engine)"
fi
echo "dada kernel tree in $D"
if [ ! -f "$D/bsd/dev/redwood_focaltech_fw.h" ]; then
	cat << 'EOF' > "$D/bsd/dev/redwood_focaltech_fw.h"
#ifndef _REDWOOD_FOCALTECH_FW_H
#define _REDWOOD_FOCALTECH_FW_H
#define FOCALTECH_FW_LENGTH 0
static const unsigned char focaltech_fw[1] = { 0 };
#endif
EOF
	echo "created stub redwood_focaltech_fw.h for non-redwood build"
fi

# Apply Part 2: LSE Branch Islands and Emulation
if [ -f "$K/0002-lse-branch-islands.patch" ]; then
	git -C "$D" apply --whitespace=nowarn "$K/0002-lse-branch-islands.patch"
	echo "applied 0002-lse-branch-islands.patch (ARMv8.0 LSE Branch Islands & Emulation)"
fi

# Apply Part 3: SLEH Trap Hooks (ARMv8.0 PAC & LSE Atomics)
if [ -f "$K/0003-sleh-hook.patch" ]; then
	git -C "$D" apply --whitespace=nowarn "$K/0003-sleh-hook.patch"
	echo "applied 0003-sleh-hook.patch (ARMv8.0 PAC & LSE Trap Hooks in SLEH)"
fi

# Apply Part 4: 4KB Quad-PTE Emulation Engine (SM6225 4KB Hardware Granule / 16KB Mach VM)
if [ -f "$K/0004-pmap-4kb-quad-pte.patch" ]; then
	git -C "$D" apply --whitespace=nowarn "$K/0004-pmap-4kb-quad-pte.patch"
	echo "applied 0004-pmap-4kb-quad-pte.patch (XNU 4KB Quad-PTE Emulation Engine)"
fi

# Apply Part 5: Cortex-A73 PAC sysreg stub and early FB markers
if [ -f "$K/patch_cortex_a73.py" ]; then
	python3 "$K/patch_cortex_a73.py" "$D"
fi

# Fix kasan-denylist regex for older/host Clang parser
if [ -f "$D/san/memory/kasan-denylist" ]; then
	sed -i '' 's/\[\*\]/\[\.\*\]/g' "$D/san/memory/kasan-denylist" 2>/dev/null || sed -i 's/\[\*\]/\[\.\*\]/g' "$D/san/memory/kasan-denylist"
	echo "fixed regex in $D/san/memory/kasan-denylist"
fi


# Fix C23 generic builtins (__builtin_clzg / __builtin_ctzg) for compilers < Clang 19
if [ -f "$D/osfmk/kern/bits.h" ]; then
	sed -i '' 's/__builtin_clzg(bitmap, 64)/(bitmap ? __builtin_clzll(bitmap) : 64)/g' "$D/osfmk/kern/bits.h" 2>/dev/null || sed -i 's/__builtin_clzg(bitmap, 64)/(bitmap ? __builtin_clzll(bitmap) : 64)/g' "$D/osfmk/kern/bits.h"
	sed -i '' 's/__builtin_ctzg(bitmap, -1)/(bitmap ? __builtin_ctzll(bitmap) : -1)/g' "$D/osfmk/kern/bits.h" 2>/dev/null || sed -i 's/__builtin_ctzg(bitmap, -1)/(bitmap ? __builtin_ctzll(bitmap) : -1)/g' "$D/osfmk/kern/bits.h"
	echo "patched clzg/ctzg compatibility in $D/osfmk/kern/bits.h"
fi

