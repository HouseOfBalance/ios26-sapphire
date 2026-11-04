#!/usr/bin/env python3
"""
patch_ram_alignment.py - 16KB RAM Alignment & Range Fix for HakiOS on SM6225 (Qualcomm Bengal)

Ensures:
1. In osfmk/arm/arm_vm_init.c (or osfmk/arm64/arm_vm_init.c):
   At the end of arm_vm_init(), avail_start and first_avail are rounded up to
   PAGE_SIZE (16KB granule) using full 64-bit (pmap_paddr_t)PAGE_MASK to avoid
   high-bit truncation and assertion failures before handoff to pmap_bootstrap()
   and initialize_ram_ranges().

2. In osfmk/arm/pmap/pmap.c:initialize_ram_ranges() & pmap_free_pages_span():
   - Fail-safe VA to PA normalization: if first_avail >= 0xfffffff000000000ULL,
     translates via ml_static_vtop() and paints red band at row 284.
   - Enforces 16KB alignment without panicking assertions.
   - Clamps invalid ranges (first < gPhysBase || end <= first) to prevent underflow.
   - Instruments pmap_free_pages_span() with visual diagnostic bands (rows 276, 280, 284, 288, 292, 294).
   - Eliminates panicking asserts in pmap_next_page().
"""

import os
import sys
import re

def patch_arm_vm_init(xnu_dir):
    candidates = [
        os.path.join(xnu_dir, "osfmk", "arm", "arm_vm_init.c"),
        os.path.join(xnu_dir, "osfmk", "arm64", "arm_vm_init.c"),
    ]
    target_path = None
    for c in candidates:
        if os.path.isfile(c):
            target_path = c
            break

    if not target_path:
        print(f"  [ERROR] Neither osfmk/arm/arm_vm_init.c nor osfmk/arm64/arm_vm_init.c found in {xnu_dir}")
        return False

    with open(target_path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    rel_path = os.path.relpath(target_path, xnu_dir).replace("\\", "/")

    # Check if already patched
    if "/* hakios: ensure avail_start and first_avail are rounded up to 16KB PAGE_SIZE */" in content:
        print(f"  [ALREADY PATCHED] {rel_path}: avail_start and first_avail alignment already present")
        return True

    patched = False

    # 1. Patch avail_start = (avail_start + PAGE_MASK) & ~(pmap_paddr_t)PAGE_MASK;
    p_avail = re.compile(r'(avail_start\s*=\s*\(avail_start\s*\+\s*PAGE_MASK\)\s*&\s*~)(PAGE_MASK;)')
    if p_avail.search(content):
        content = p_avail.sub(r'\1(pmap_paddr_t)\2', content, count=1)
        patched = True

    # 2. Patch first_avail = avail_start; to explicitly round up avail_start and first_avail
    p_first = re.compile(r'(\n[ \t]*)(first_avail\s*=\s*avail_start;)')
    if p_first.search(content):
        r_first = (
            r'\1/* hakios: ensure avail_start and first_avail are rounded up to 16KB PAGE_SIZE */\n'
            r'\tavail_start = (avail_start + PAGE_MASK) & ~(pmap_paddr_t)PAGE_MASK;\n'
            r'\tfirst_avail = avail_start;'
        )
        content = p_first.sub(r_first, content, count=1)
        patched = True

    if not patched:
        # Fallback string matching
        target_str = "avail_start = (avail_start + PAGE_MASK) & ~PAGE_MASK;"
        repl_str = "avail_start = (avail_start + PAGE_MASK) & ~(pmap_paddr_t)PAGE_MASK;"
        if target_str in content:
            content = content.replace(target_str, repl_str, 1)
            patched = True

        target_first = "first_avail = avail_start;"
        repl_first = (
            "/* hakios: ensure avail_start and first_avail are rounded up to 16KB PAGE_SIZE */\n"
            "\tavail_start = (avail_start + PAGE_MASK) & ~(pmap_paddr_t)PAGE_MASK;\n"
            "\tfirst_avail = avail_start;"
        )
        if target_first in content:
            content = content.replace(target_first, repl_first, 1)
            patched = True

    if patched:
        with open(target_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print(f"  [OK] {rel_path}: patched avail_start and first_avail 16KB alignment")
        return True
    else:
        print(f"  [ERROR] {rel_path}: could not find target patterns for avail_start / first_avail")
        return False


def patch_pmap(xnu_dir):
    target_path = os.path.join(xnu_dir, "osfmk", "arm", "pmap", "pmap.c")
    if not os.path.isfile(target_path):
        print(f"  [ERROR] osfmk/arm/pmap/pmap.c not found in {xnu_dir}")
        return False

    with open(target_path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    rel_path = os.path.relpath(target_path, xnu_dir).replace("\\", "/")

    # Check if already patched
    if "/* hakios/claude: normalize first_avail if passed as kernel VA */" in content:
        print(f"  [ALREADY PATCHED] {rel_path}: initialize_ram_ranges fail-safe patch already present")
        return True

    pattern = re.compile(
        r'static\s+void\s+initialize_ram_ranges\s*\(\s*void\s*\)\s*\{[\s\S]*?'
        r'boolean_t\s+pmap_next_page\s*\(\s*ppnum_t\s*\*pnum\s*\)\s*\{[\s\S]*?'
        r'return\s+FALSE;\s*\n\}',
        re.MULTILINE
    )

    replacement = """static void
initialize_ram_ranges(void)
{
	extern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
	hk_draw_band(0x00FFFFFF, 280, 2); /* White: entered initialize_ram_ranges */

	pmap_paddr_t first = first_avail;
	pmap_paddr_t end   = avail_end;

	/* hakios/claude: normalize first_avail if passed as kernel VA */
	if (first >= 0xfffffff000000000ULL) {
		first = ml_static_vtop((vm_offset_t)first);
		hk_draw_band(0x00FF0000, 284, 2); /* Red: was a kernel VA! */
	}

	first = (first + PAGE_MASK) & ~(pmap_paddr_t)PAGE_MASK;
	end  &= ~(pmap_paddr_t)PAGE_MASK;

	if (first < gPhysBase || end <= first) {
		hk_draw_band(0x00FF0000, 288, 2); /* Red: bad range */
		first = end;                      /* clamp: zero pages, no underflow */
	}

	first_avail      = first;
	avail_end        = end;
	avail_page_count = atop(end - first);
	need_ram_ranges_init = false;

	hk_draw_band(0x0000FF00, 292, 2); /* Lime: initialize_ram_ranges done */
}

unsigned int
pmap_free_pages(
	void)
{
	if (need_ram_ranges_init) {
		initialize_ram_ranges();
	}
	return avail_page_count;
}

unsigned int
pmap_free_pages_span(
	void)
{
	extern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
	hk_draw_band(0x0000FFFF, 276, 2); /* Cyan: entered pmap_free_pages_span */

	if (need_ram_ranges_init) {
		initialize_ram_ranges();
	}

	hk_draw_band(0x00FFD700, 294, 2); /* Gold: span computed */
	return (unsigned int)atop(avail_end - first_avail);
}


boolean_t
pmap_next_page_hi(
	ppnum_t            * pnum,
	__unused boolean_t might_free)
{
	return pmap_next_page(pnum);
}


boolean_t
pmap_next_page(
	ppnum_t *pnum)
{
	if (need_ram_ranges_init) {
		initialize_ram_ranges();
	}

	if (first_avail != avail_end) {
		*pnum = (ppnum_t)atop(first_avail);
		first_avail += PAGE_SIZE;
		if (avail_page_count > 0) {
			--avail_page_count;
		}
		return TRUE;
	}
	return FALSE;
}"""

    if pattern.search(content):
        content = pattern.sub(lambda m: replacement, content, count=1)
        with open(target_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print(f"  [OK] {rel_path}: patched initialize_ram_ranges & pmap_free_pages_span fail-safe")
        return True

    print(f"  [ERROR] {rel_path}: initialize_ram_ranges target block not found")
    return False


def main():
    if len(sys.argv) < 2:
        print("Usage: patch_ram_alignment.py <xnu_src_dir>")
        sys.exit(1)

    xnu_dir = sys.argv[1]
    if not os.path.isdir(xnu_dir):
        print(f"Error: Directory '{xnu_dir}' does not exist.")
        sys.exit(1)

    print(f"=== Applying 16KB RAM Alignment & Range Patch to {xnu_dir} ===")
    ok_vm = patch_arm_vm_init(xnu_dir)
    ok_pmap = patch_pmap(xnu_dir)

    if ok_vm and ok_pmap:
        print("=== 16KB RAM Alignment Patch applied successfully! ===")
        sys.exit(0)
    else:
        print("=== [FAIL] One or more patches failed to apply ===")
        sys.exit(1)


if __name__ == "__main__":
    main()
