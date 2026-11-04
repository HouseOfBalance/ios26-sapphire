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
    original_content = content

    rel_path = os.path.relpath(target_path, xnu_dir).replace("\\", "/")

    marker = "/* hakios: ensure avail_start and first_avail are rounded up to 16KB PAGE_SIZE */"
    alignment = re.compile(
        r'avail_start\s*=\s*\(\s*avail_start\s*\+\s*PAGE_MASK\s*\)\s*'
        r'&\s*~\s*\(\s*pmap_paddr_t\s*\)\s*PAGE_MASK\s*;'
    )
    first_assignment = re.compile(r'first_avail\s*=\s*avail_start\s*;')

    # The marker alone is not proof of a complete patch: an earlier partial run
    # could have written it before failing to match one of the assignments.
    marker_match = re.search(re.escape(marker), content)
    alignment_match = alignment.search(content)
    first_match = first_assignment.search(content)
    if (marker_match and alignment_match and first_match and
            marker_match.start() < alignment_match.start() < first_match.start()):
        print(f"  [ALREADY PATCHED] {rel_path}: avail_start and first_avail alignment already present")
        return True

    if not first_match:
        print(f"  [ERROR] {rel_path}: could not find first_avail = avail_start assignment")
        return False

    # Ensure the full-width round-up is immediately before first_avail is copied.
    # Reuse an existing correctly typed alignment only when it precedes that copy.
    first_match = first_assignment.search(content)
    alignment_before_first = next(
        (m for m in alignment.finditer(content) if m.start() < first_match.start()), None
    )
    if alignment_before_first is None:
        line_start = content.rfind("\n", 0, first_match.start()) + 1
        indent = re.match(r'[ \t]*', content[line_start:first_match.start()]).group(0)
        replacement = (
            f"{indent}{marker}\n"
            f"{indent}avail_start = (avail_start + PAGE_MASK) & ~(pmap_paddr_t)PAGE_MASK;\n"
            f"{indent}first_avail = avail_start;"
        )
        content = content[:line_start] + replacement + content[first_match.end():]
    elif marker_match is None or marker_match.start() > alignment_before_first.start():
        # The cast may already exist, but the diagnostic/idempotency marker does not.
        line_start = content.rfind("\n", 0, alignment_before_first.start()) + 1
        indent = re.match(r'[ \t]*', content[line_start:alignment_before_first.start()]).group(0)
        content = content[:line_start] + f"{indent}{marker}\n" + content[line_start:]

    # Verify both edits in order before writing or reporting success.
    marker_match = re.search(re.escape(marker), content)
    alignment_match = alignment.search(content)
    first_match = first_assignment.search(content)
    if not (marker_match and alignment_match and first_match and
            marker_match.start() < alignment_match.start() < first_match.start()):
        print(f"  [ERROR] {rel_path}: incomplete 16KB alignment patch; source left unchanged")
        return False

    if content != original_content:
        with open(target_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print(f"  [OK] {rel_path}: patched avail_start and first_avail 16KB alignment")
        return True
    print(f"  [ALREADY PATCHED] {rel_path}: avail_start and first_avail alignment already present")
    return True


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
