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
echo "redwood only: tools/mk-focaltech-fw.py <your focaltech_ts_fw_m20.bin> $D/bsd/dev/redwood_focaltech_fw.h"

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

