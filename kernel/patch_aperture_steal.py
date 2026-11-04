#!/usr/bin/env python3
"""
patch_aperture_steal.py - Aperture-Based Early Memory Steal for HakiOS XNU 12377 (ARM64E).

On Qualcomm Snapdragon SM6225 (Bengal / Kryo 260 Cortex-A73), during early boot
before pmap is initialized (!pmap_initialized), calling pmap_enter() inside
pmap_steal_memory_internal() causes an infinite retry loop or hang:
  do { ... } while (kr == KERN_RESOURCE_SHORTAGE);
because page table pools (L3 tables) and PV pools are not yet initialized,
and zero pages have been freed into the VM queues.

Since all physical RAM is already mapped 1:1 in the kernel physical aperture (phystokv),
this patch modifies pmap_steal_memory_internal() in osfmk/vm/vm_resident.c to directly
advance the early physical bump pointer (first_avail / avail_start) when !pmap_initialized,
zero the memory in the physical aperture, and immediately return its virtual address.
"""

import os
import sys
import re

def patch_vm_resident(src_dir):
    candidates = [
        os.path.join(src_dir, "osfmk", "vm", "vm_resident.c"),
        os.path.join(src_dir, "vm_resident.c"),
    ]
    target_path = None
    for c in candidates:
        if os.path.isfile(c):
            target_path = c
            break

    if not target_path:
        print(f"[ERROR] Cannot find vm_resident.c in '{src_dir}'", file=sys.stderr)
        return 1

    print(f"Target file: {target_path}")

    with open(target_path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    # Check for existing patch (idempotency)
    if "/* hakios: aperture-based early memory steal via pmap_next_page */" in content or \
       ("first_pn" in content and "!pmap_initialized" in content and "steal_pages" in content) or \
       ("first_avail = pa + size;" in content and "!pmap_initialized" in content):
        print("  [OK] osfmk/vm/vm_resident.c is already patched for aperture steal, skipping.")
        return 0

    # Pattern to match the pmap_steal_memory_internal function definition
    pattern = re.compile(
        r'(pmap_steal_memory_internal\s*\([^{]+\)\s*\{)',
        re.MULTILINE
    )

    m = pattern.search(content)
    if not m:
        print("[ERROR] Could not find 'pmap_steal_memory_internal' definition in vm_resident.c", file=sys.stderr)
        return 1

    steal_patch = """
	/* hakios: aperture-based early memory steal via pmap_next_page */
	extern boolean_t pmap_initialized;
	extern vm_offset_t first_avail;
	extern pmap_paddr_t avail_start;

	if (!pmap_initialized) {
		ppnum_t first_pn = 0, pn;
		uint32_t steal_pages = (uint32_t)(round_page(size) >> PAGE_SHIFT);
		for (uint32_t p = 0; p < steal_pages; p++) {
			if (!pmap_next_page(&pn)) {
				panic("pmap_steal_memory_internal: early aperture steal exhausted RAM (%u pages)", steal_pages);
			}
			if (p == 0) {
				first_pn = pn;
			}
		}
		vm_page_wire_count += steal_pages;
		vm_page_stolen_count += steal_pages;
		avail_start = first_avail;      /* keep both in sync */
		pmap_paddr_t pa = ptoa(first_pn);
		bzero((void *)phystokv(pa), round_page(size));
		{
			extern void hk_draw_band(uint32_t, uint32_t, uint32_t);
			hk_draw_band(0x00FF1493, 264, 2); /* Deep Pink: aperture steal */
		}
		return (void *)(vm_offset_t)phystokv(pa);
	}"""

    patched_content = pattern.sub(lambda m: m.group(1) + steal_patch, content, count=1)

    if patched_content == content:
        print("[ERROR] Substitution failed in vm_resident.c", file=sys.stderr)
        return 1

    with open(target_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(patched_content)

    print("  [OK] Successfully patched pmap_steal_memory_internal in osfmk/vm/vm_resident.c with aperture bump pointer steal.")
    return 0

def main():
    if len(sys.argv) < 2:
        print("Usage: patch_aperture_steal.py <xnu_src_dir>", file=sys.stderr)
        sys.exit(1)

    xnu_dir = sys.argv[1]
    ret = patch_vm_resident(xnu_dir)
    sys.exit(ret)

if __name__ == "__main__":
    main()
