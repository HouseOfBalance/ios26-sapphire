#!/usr/bin/env python3
"""
kernel/patch_iokit_root.py

HakiOS XNU 12377 (iOS 26 ARM64E) IOKit Root Device Resolver
Target Device: Xiaomi Redmi Note 13 4G (Qualcomm Snapdragon SM6225 Bengal)

Instruments IOFindBSDRoot in iokit/bsddev/IOKitBSDInit.cpp to trace root device resolution
and ensure immediate detection of memory disk root (rd=md0):
  - Row 600: IOFindBSDRoot entered (Dark Cyan)
  - Row 610: Memory disk root (md0) registered and selected (Lime)
"""

import os
import sys

def patch_iokit_root(src_dir):
    target = os.path.join(src_dir, "iokit", "bsddev", "IOKitBSDInit.cpp")
    if not os.path.isfile(target):
        print(f"[-] Error: file not found: {target}")
        return False

    with open(target, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    c = content.replace("\r\n", "\n")

    if "/* hakios: iokit root resolution visual ladder bands */" in c:
        print("  [OK] iokit/bsddev/IOKitBSDInit.cpp: root bands already installed")
        return True

    # 1. IOFindBSDRoot entry band
    idx = c.find("IOFindBSDRoot(")
    if idx != -1:
        brace_idx = c.find("{\n", idx)
        if brace_idx != -1:
            insert_pos = brace_idx + 2
            entry_band = (
                "\t/* hakios: iokit root resolution visual ladder bands */\n"
                "\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00008B8B, 600, 4); } /* Dark Cyan: IOFindBSDRoot entered */\n"
            )
            c = c[:insert_pos] + entry_band + c[insert_pos:]
            print("  [OK] iokit/bsddev/IOKitBSDInit.cpp: IOFindBSDRoot entry band installed (row 600)")
        else:
            print("  [WARN] iokit/bsddev/IOKitBSDInit.cpp: opening brace for IOFindBSDRoot not found")
    else:
        print("  [WARN] iokit/bsddev/IOKitBSDInit.cpp: IOFindBSDRoot function signature not found")

    # 2. md0 root selection band: hook directly into the rd=md fastpath
    t_md0 = "rootName[0] = 'm';"
    r_md0 = (
        "rootName[0] = 'm';\n"
        "\t\t\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x0000FF00, 610, 4); } /* Lime: rd=md0 selected */"
    )
    if t_md0 in c:
        c = c.replace(t_md0, r_md0, 1)
        print("  [OK] iokit/bsddev/IOKitBSDInit.cpp: rd=md0 selection band installed (row 610)")
    else:
        # Fallback to alternative anchor
        t_md0_alt = "bsdWait = kSecondScale / 10;"
        r_md0_alt = (
            "bsdWait = kSecondScale / 10;\n"
            "\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x0000FF00, 610, 4); } /* Lime: rd=md0 selected */"
        )
        if t_md0_alt in c:
            c = c.replace(t_md0_alt, r_md0_alt, 1)
            print("  [OK] iokit/bsddev/IOKitBSDInit.cpp: rd=md0 selection band installed via bsdWait (row 610)")
        else:
            print("  [WARN] iokit/bsddev/IOKitBSDInit.cpp: rootName[0] = 'm' and bsdWait patterns not found")

    with open(target, "w", encoding="utf-8", newline="\n") as f:
        f.write(c)

    print("  [OK] iokit/bsddev/IOKitBSDInit.cpp successfully instrumented for root device resolution")
    return True

def main():
    if len(sys.argv) < 2:
        print("Usage: patch_iokit_root.py <xnu_src_dir>")
        sys.exit(1)

    src_dir = sys.argv[1]
    if not os.path.isdir(src_dir):
        print(f"[-] Error: directory does not exist: {src_dir}")
        sys.exit(1)

    print(f"=== Applying IOKit Root Device Resolver Patch to {src_dir} ===")
    if not patch_iokit_root(src_dir):
        sys.exit(1)
    print("=== IOKit root device resolver patch complete ===")

if __name__ == "__main__":
    main()
