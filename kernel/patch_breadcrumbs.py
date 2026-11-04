#!/usr/bin/env python3
"""
kernel/patch_breadcrumbs.py

Visual Early Breadcrumb Instrumentation for HakiOS XNU 12377 (iOS 26 ARM64E)
Target Device: Xiaomi Redmi Note 13 4G (Qualcomm Snapdragon SM6225 Bengal)

Inserts fine-grained visual diagnostic breadcrumbs around page_frame_init in pmap_startup
(osfmk/vm/vm_resident.c). Each breadcrumb invokes kernel_debug_string_early, which is
hooked to hk_draw_band (bsd/kern/kdebug.c) to render distinct color bands across the early
framebuffer (rows 264, 268, 272, 276, 280...).
"""

import os
import sys
import re

def patch_vm_resident(src_dir):
    vm_resident_path = os.path.join(src_dir, "osfmk", "vm", "vm_resident.c")
    if not os.path.isfile(vm_resident_path):
        print(f"[-] Error: file not found: {vm_resident_path}")
        return False

    with open(vm_resident_path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    # Normalize line endings for reliable matching
    c = content.replace("\r\n", "\n")

    # Check for idempotency
    if 'kernel_debug_string_early("pf:t0");' in c and 'kernel_debug_string_early("pf:span");' in c:
        print("  [OK] osfmk/vm/vm_resident.c: early breadcrumbs already installed")
        return True

    # 1. Target block around page_frame_init:
    # Stock code:
    #   kernel_debug_string_early("page_frame_init");
    #   absolutetime_to_nanoseconds(mach_absolute_time(), &start_ns);
    #
    # We split mach_absolute_time() into a separate variable 't' and insert
    # breadcrumbs "pf:t0", "pf:t1", "pf:t2":
    t1_exact = '\tkernel_debug_string_early("page_frame_init");\n\tabsolutetime_to_nanoseconds(mach_absolute_time(), &start_ns);'
    r1 = (
        '\tkernel_debug_string_early("page_frame_init");\n'
        '\tkernel_debug_string_early("pf:t0");\n'
        '\tuint64_t t = mach_absolute_time();\n'
        '\tkernel_debug_string_early("pf:t1");\n'
        '\tabsolutetime_to_nanoseconds(t, &start_ns);\n'
        '\tkernel_debug_string_early("pf:t2");'
    )

    if t1_exact in c:
        c = c.replace(t1_exact, r1, 1)
        print("  [OK] osfmk/vm/vm_resident.c: pf:t0, pf:t1, pf:t2 breadcrumbs installed")
    else:
        # Regex fallback to tolerate whitespace variations
        pattern1 = re.compile(
            r'(\tkernel_debug_string_early\("page_frame_init"\);)\s*'
            r'(\tabsolutetime_to_nanoseconds\(mach_absolute_time\(\),\s*&start_ns\);)'
        )
        if pattern1.search(c):
            c = pattern1.sub(
                r'\1\n'
                r'\tkernel_debug_string_early("pf:t0");\n'
                r'\tuint64_t t = mach_absolute_time();\n'
                r'\tkernel_debug_string_early("pf:t1");\n'
                r'\tabsolutetime_to_nanoseconds(t, &start_ns);\n'
                r'\tkernel_debug_string_early("pf:t2");',
                c,
                count=1
            )
            print("  [OK] osfmk/vm/vm_resident.c: pf:t0, pf:t1, pf:t2 breadcrumbs installed (via regex)")
        else:
            # Fallback: if patch_timer_sm6225.py was applied first
            p_timer_guard = re.compile(
                r'(\tkernel_debug_string_early\("page_frame_init"\);)\s*'
                r'(/\* hakios: guard against uncalibrated early timebase divide-by-zero \*/[\s\S]*?'
                r'\tabsolutetime_to_nanoseconds\([^;]+;\s*\n\t\})'
            )
            m_tg = p_timer_guard.search(c)
            if m_tg:
                c = p_timer_guard.sub(
                    r'\1\n'
                    r'\tkernel_debug_string_early("pf:t0");\n'
                    r'\tkernel_debug_string_early("pf:t1");\n'
                    r'\2\n'
                    r'\tkernel_debug_string_early("pf:t2");',
                    c,
                    count=1
                )
                print("  [OK] osfmk/vm/vm_resident.c: pf:t0, pf:t1, pf:t2 breadcrumbs installed (around timer guard)")
            else:
                print("  [-] Error: page_frame_init / absolutetime_to_nanoseconds target not found in vm_resident.c")
                return False

    # 2. Target block around pmap_free_pages_span:
    # Stock code:
    #   mem_sz  = ptoa(pmap_free_pages_span());
    #
    # We insert "pf:span" breadcrumb immediately after mem_sz is computed:
    pattern2 = re.compile(r'(\tmem_sz\s*=\s*ptoa\(pmap_free_pages(?:_span)?\(\)\);)')
    if pattern2.search(c):
        c = pattern2.sub(r'\1\n\tkernel_debug_string_early("pf:span");', c, count=1)
        print("  [OK] osfmk/vm/vm_resident.c: pf:span breadcrumb installed")
    else:
        print("  [-] Error: mem_sz = ptoa(pmap_free_pages_span()) target not found in vm_resident.c")
        return False

    with open(vm_resident_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(c)

    print("  [OK] osfmk/vm/vm_resident.c successfully patched with Claude breadcrumbs")
    return True

def confirm_kdebug_visual(src_dir):
    kdebug_path = os.path.join(src_dir, "bsd", "kern", "kdebug.c")
    if os.path.isfile(kdebug_path):
        with open(kdebug_path, "r", encoding="utf-8", errors="replace") as f:
            c = f.read()
        if "hk_draw_band" in c and "kernel_debug_string_early" in c:
            print("  [CONFIRMED] bsd/kern/kdebug.c: kernel_debug_string_early draws diagnostic bands via hk_draw_band (rows 264, 268, 272, 276...)")
        else:
            print("  [INFO] bsd/kern/kdebug.c: hk_draw_band hook will be active once patch_cortex_a73.py applies")
    else:
        print("  [INFO] bsd/kern/kdebug.c not found in target dir (standalone validation)")

def main():
    if len(sys.argv) < 2:
        print("Usage: patch_breadcrumbs.py <xnu_src_dir>")
        sys.exit(1)

    src_dir = sys.argv[1]
    if not os.path.isdir(src_dir):
        print(f"[-] Error: directory does not exist: {src_dir}")
        sys.exit(1)

    print(f"=== Applying Early Breadcrumb Visual Instrumentation to {src_dir} ===")
    success = patch_vm_resident(src_dir)
    if not success:
        sys.exit(1)

    confirm_kdebug_visual(src_dir)
    print("=== Early breadcrumb visual instrumentation complete ===")

if __name__ == "__main__":
    main()
