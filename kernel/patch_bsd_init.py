#!/usr/bin/env python3
"""
kernel/patch_bsd_init.py

HakiOS XNU 12377 (iOS 26 ARM64E) BSD Subsystem Lifecycle Monitor
Target Device: Xiaomi Redmi Note 13 4G (Qualcomm Snapdragon SM6225 Bengal)

Instruments bsd_init() in bsd/kern/bsd_init.c with fine-grained visual diagnostic bands
(rows 500..580) to monitor BSD bootstrap stages:
  - Row 500: bsd_init entered (Goldenrod)
  - Row 510: bcache_init completed (Orange)
  - Row 520: vfs_init completed (Yellow Green)
  - Row 530: devfs_kernel_mount completed (Dodger Blue)
  - Row 540: mbuf_init completed (Medium Orchid)
  - Row 550: net_init completed (Hot Pink)
  - Row 580: bsd_init completed / handoff to root mount & launchd (Cyan)
"""

import os
import sys

def patch_bsd_init(src_dir):
    target = os.path.join(src_dir, "bsd", "kern", "bsd_init.c")
    if not os.path.isfile(target):
        print(f"[-] Error: file not found: {target}")
        return False

    with open(target, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    c = content.replace("\r\n", "\n")

    if "/* hakios: bsd_init lifecycle milestone ladder bands */" in c:
        print("  [ALREADY PATCHED] bsd/kern/bsd_init.c: milestone bands already installed")
        return True

    # 1. bsd_init entry band
    t_entry = "bsd_init(void)\n{\n"
    r_entry = (
        "bsd_init(void)\n{\n"
        "\t/* hakios: bsd_init lifecycle milestone ladder bands */\n"
        "\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00DAA520, 500, 4); } /* Goldenrod: bsd_init entered */\n"
    )
    if t_entry in c:
        c = c.replace(t_entry, r_entry, 1)
        print("  [OK] bsd/kern/bsd_init.c: bsd_init entry band installed (row 500)")
    else:
        print("  [WARN] bsd/kern/bsd_init.c: bsd_init(void) entry pattern not found")

    # 2. bcache_init band
    t_bcache = "bcache_init();"
    r_bcache = (
        "bcache_init();\n"
        "\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FFA500, 510, 4); } /* Orange: bcache_init done */"
    )
    if t_bcache in c:
        c = c.replace(t_bcache, r_bcache, 1)
        print("  [OK] bsd/kern/bsd_init.c: bcache_init band installed (row 510)")
    else:
        print("  [WARN] bsd/kern/bsd_init.c: bcache_init() pattern not found")

    # 3. vfs_init band
    t_vfs = "vfs_init();"
    r_vfs = (
        "vfs_init();\n"
        "\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x009ACD32, 520, 4); } /* Yellow Green: vfs_init done */"
    )
    if t_vfs in c:
        c = c.replace(t_vfs, r_vfs, 1)
        print("  [OK] bsd/kern/bsd_init.c: vfs_init band installed (row 520)")
    else:
        print("  [WARN] bsd/kern/bsd_init.c: vfs_init() pattern not found")

    # 4. devfs_kernel_mount band
    t_devfs = "devfs_kernel_mount(mounthere);"
    r_devfs = (
        "devfs_kernel_mount(mounthere);\n"
        "\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x001E90FF, 530, 4); } /* Dodger Blue: devfs mounted */"
    )
    if t_devfs in c:
        c = c.replace(t_devfs, r_devfs, 1)
        print("  [OK] bsd/kern/bsd_init.c: devfs band installed (row 530)")
    else:
        print("  [WARN] bsd/kern/bsd_init.c: devfs_kernel_mount pattern not found")

    # 5. mbuf_init band
    t_mbuf = "mbuf_init();"
    r_mbuf = (
        "mbuf_init();\n"
        "\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00BA55D3, 540, 4); } /* Medium Orchid: mbuf_init done */"
    )
    if t_mbuf in c:
        c = c.replace(t_mbuf, r_mbuf, 1)
        print("  [OK] bsd/kern/bsd_init.c: mbuf_init band installed (row 540)")
    else:
        print("  [WARN] bsd/kern/bsd_init.c: mbuf_init() pattern not found")

    # 6. net_init band
    t_net = "net_init();"
    r_net = (
        "net_init();\n"
        "\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FF69B4, 550, 4); } /* Hot Pink: net_init done */"
    )
    if t_net in c:
        c = c.replace(t_net, r_net, 1)
        print("  [OK] bsd/kern/bsd_init.c: net_init band installed (row 550)")
    else:
        print("  [WARN] bsd/kern/bsd_init.c: net_init() pattern not found")

    # 7. bsd_init completed band (Row 580: Cyan)
    t_done = 'bsd_init_kprintf("calling bsd_autoconf\\n");'
    r_done = (
        'bsd_init_kprintf("calling bsd_autoconf\\n");\n'
        '\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x0000FFFF, 580, 4); } /* Cyan: bsd_init autoconf -> launchd */'
    )
    if t_done in c:
        c = c.replace(t_done, r_done, 1)
        print("  [OK] bsd/kern/bsd_init.c: bsd_init completed band installed (row 580)")
    else:
        # Fallback to devfs_kernel_mount follow-up
        t_done_alt = 'bsd_init_kprintf("devfs_kernel_mount returned %d\\n", error);'
        r_done_alt = (
            'bsd_init_kprintf("devfs_kernel_mount returned %d\\n", error);\n'
            '\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x0000FFFF, 580, 4); } /* Cyan: bsd_init autoconf -> launchd */'
        )
        if t_done_alt in c:
            c = c.replace(t_done_alt, r_done_alt, 1)
            print("  [OK] bsd/kern/bsd_init.c: bsd_init completed band installed via alt anchor (row 580)")
        else:
            print("  [WARN] bsd/kern/bsd_init.c: bsd_init autoconf completion pattern not found")

    with open(target, "w", encoding="utf-8", newline="\n") as f:
        f.write(c)

    print("  [OK] bsd/kern/bsd_init.c successfully patched with BSD lifecycle ladder")
    return True

def main():
    if len(sys.argv) < 2:
        print("Usage: patch_bsd_init.py <xnu_src_dir>")
        sys.exit(1)

    src_dir = sys.argv[1]
    if not os.path.isdir(src_dir):
        print(f"[-] Error: directory does not exist: {src_dir}")
        sys.exit(1)

    print(f"=== Applying BSD Subsystem Lifecycle Ladder to {src_dir} ===")
    if not patch_bsd_init(src_dir):
        sys.exit(1)
    print("=== BSD lifecycle ladder instrumentation complete ===")

if __name__ == "__main__":
    main()
