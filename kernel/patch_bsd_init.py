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

    # 8. vfs_mountroot succeeded band (Row 615: Spring Green)
    t_vfsroot = "if (0 == (err = vfs_mountroot())) {"
    r_vfsroot = (
        "if (0 == (err = vfs_mountroot())) {\n"
        "\t\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x0000FF7F, 615, 4); } /* Spring Green: vfs_mountroot succeeded */"
    )
    if t_vfsroot in c:
        c = c.replace(t_vfsroot, r_vfsroot, 1)
        print("  [OK] bsd/kern/bsd_init.c: vfs_mountroot success band installed (row 615)")
    else:
        print("  [WARN] bsd/kern/bsd_init.c: vfs_mountroot pattern not found")

    # 9. bsd_utaskbootstrap band (Row 620: Deep Sky Blue)
    t_utask = "bsd_utaskbootstrap();"
    r_utask = (
        "bsd_utaskbootstrap();\n"
        "\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x0000BFFF, 620, 4); } /* Deep Sky Blue: PID 1 process created */"
    )
    if t_utask in c:
        c = c.replace(t_utask, r_utask, 1)
        print("  [OK] bsd/kern/bsd_init.c: bsd_utaskbootstrap band installed (row 620)")
    else:
        print("  [WARN] bsd/kern/bsd_init.c: bsd_utaskbootstrap pattern not found")

    # 10. bsdinit_task entry band (Row 630: Dark Orange)
    t_bsdinit = "bsdinit_task(void)\n{\n"
    r_bsdinit = (
        "bsdinit_task(void)\n{\n"
        "\t/* hakios: PID 1 kernel bootstrap */\n"
        "\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FF8C00, 630, 4); } /* Dark Orange: bsdinit_task entered */\n"
    )
    if t_bsdinit in c:
        c = c.replace(t_bsdinit, r_bsdinit, 1)
        print("  [OK] bsd/kern/bsd_init.c: bsdinit_task entry band installed (row 630)")
    else:
        print("  [WARN] bsd/kern/bsd_init.c: bsdinit_task entry pattern not found")

    # 11. load_init_program return band (Row 640: Magenta)
    t_loadinit = "load_init_program(p);\n"
    r_loadinit = (
        "load_init_program(p);\n"
        "\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FF00FF, 640, 4); } /* Magenta: load_init_program handoff to EL0 */\n"
    )
    if t_loadinit in c:
        c = c.replace(t_loadinit, r_loadinit, 1)
        print("  [OK] bsd/kern/bsd_init.c: load_init_program band installed (row 640)")
    else:
        print("  [WARN] bsd/kern/bsd_init.c: load_init_program pattern not found")

    with open(target, "w", encoding="utf-8", newline="\n") as f:
        f.write(c)

    print("  [OK] bsd/kern/bsd_init.c successfully patched with BSD lifecycle ladder")

    # 12. Instrument bsd/dev/arm/systemcalls.c for PID 1 userland syscall interception, banner display & heartbeat
    target_sys = os.path.join(src_dir, "bsd", "dev", "arm", "systemcalls.c")
    if os.path.isfile(target_sys):
        with open(target_sys, "r", encoding="utf-8", errors="replace") as f:
            csys = f.read().replace("\r\n", "\n")
        if "/* hakios: userland PID 1 text banner and syscall interception */" in csys:
            print("  [ALREADY PATCHED] bsd/dev/arm/systemcalls.c: userland syscall monitor already installed")
        else:
            t_syscall = "\terror = (*(callp->sy_call))(proc, &uthread->uu_arg[0], &(uthread->uu_rval[0]));"
            r_syscall = (
                "\t/* hakios: userland PID 1 text banner and syscall interception */\n"
                "\tif (pid == 1) {\n"
                "\t\tstatic int pid1_boot_banner_drawn = 0;\n"
                "\t\tif (!pid1_boot_banner_drawn) {\n"
                "\t\t\tpid1_boot_banner_drawn = 1;\n"
                "\t\t\t/* Draw diagnostic band Row 650 (Yellow) */\n"
                "\t\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FFFF00, 650, 6); }\n"
                "\t\t\t/* Clear banner box rows 720..960 to pitch black (240 rows = 64k stores, safe and instant) */\n"
                "\t\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00000000, 720, 240); }\n"
                "\t\t\t/* Draw master banner */\n"
                "\t\t\tstatic const char hk_init_banner[] =\n"
                '\t\t\t\t"\\n"\n'
                '\t\t\t\t"========================================\\n"\n'
                '\t\t\t\t"   HakiOS (iOS 26 / XNU 12377.121.6)   \\n"\n'
                '\t\t\t\t"   Xiaomi Redmi Note 13 4G (sapphire)   \\n"\n'
                '\t\t\t\t"========================================\\n\\n"\n'
                '\t\t\t\t"[*] SoC: Qualcomm Snapdragon SM6225 Bengal\\n"\n'
                '\t\t\t\t"[*] Cores: Cortex-A73 Gold (4KB Quad-PTE)\\n"\n'
                '\t\t\t\t"[*] Root: Mockfs ramdisk mounted on md0\\n"\n'
                '\t\t\t\t"[*] PID 1: /sbin/launchd running in EL0!\\n"\n'
                '\t\t\t\t"[*] System call interface: ACTIVE\\n\\n";\n'
                "\t\t\textern const unsigned char iso_font[];\n"
                "\t\t\textern void hk_render_screen_asm(const char *, uint32_t, const unsigned char *, uint32_t);\n"
                "\t\t\thk_render_screen_asm(hk_init_banner, (uint32_t)sizeof(hk_init_banner) - 1, iso_font, 720);\n"
                "\t\t}\n\n"
                "\t\tif (code == 5 /* SYS_open */) {\n"
                "\t\t\t/* Immediate success for /dev/console open: return fd 1, avoid ttyopen deadlock */\n"
                "\t\t\tuthread->uu_rval[0] = 1;\n"
                "\t\t\terror = 0;\n"
                "\t\t\tgoto hakios_pid1_handled;\n"
                "\t\t}\n\n"
                "\t\tif (code == 4 /* SYS_write */) {\n"
                "\t\t\tstatic char hk_user_write_buf[256];\n"
                "\t\t\tstatic uint32_t hk_text_row = 1080;\n"
                "\t\t\tuser_addr_t ubuf = (user_addr_t)uthread->uu_arg[1];\n"
                "\t\t\tuser_size_t ulen = (user_size_t)uthread->uu_arg[2];\n"
                "\t\t\tuint32_t to_copy = (ulen < sizeof(hk_user_write_buf) - 1) ? (uint32_t)ulen : (sizeof(hk_user_write_buf) - 1);\n"
                "\t\t\tif (to_copy > 0 && copyin(ubuf, hk_user_write_buf, to_copy) == 0) {\n"
                "\t\t\t\thk_user_write_buf[to_copy] = '\\0';\n"
                "\t\t\t\t/* Draw Row 652 (Thick White band: SYS_write executed) */\n"
                "\t\t\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FFFFFF, 652, 4); }\n"
                "\t\t\t\t/* Clean D-cache for hk_user_write_buf so physical RAM is coherent before MMU is disabled */\n"
                "\t\t\t\tfor (uint32_t hk_off = 0; hk_off < to_copy; hk_off += 64) {\n"
                '\t\t\t\t\tasm volatile("dc cvac, %0" : : "r"(&hk_user_write_buf[hk_off]) : "memory");\n'
                "\t\t\t\t}\n"
                '\t\t\t\tasm volatile("dsb sy" : : : "memory");\n'
                "\t\t\t\t/* Clear line background (32 rows) */\n"
                "\t\t\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00000000, hk_text_row, 32); }\n"
                "\t\t\t\textern const unsigned char iso_font[];\n"
                "\t\t\t\textern void hk_render_screen_asm(const char *, uint32_t, const unsigned char *, uint32_t);\n"
                "\t\t\t\thk_render_screen_asm(hk_user_write_buf, to_copy, iso_font, hk_text_row);\n"
                "\t\t\t\thk_text_row += 32;\n"
                "\t\t\t\tif (hk_text_row > 2200) {\n"
                "\t\t\t\t\thk_text_row = 1080;\n"
                "\t\t\t\t}\n"
                "\t\t\t}\n"
                "\t\t\tuthread->uu_rval[0] = (int)ulen;\n"
                "\t\t\terror = 0;\n"
                "\t\t\tgoto hakios_pid1_handled;\n"
                "\t\t}\n\n"
                "\t\tif (code == 20 /* SYS_getpid */) {\n"
                "\t\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00DA70D6, 654, 4); } /* Orchid: SYS_getpid */\n"
                "\t\t\tuthread->uu_rval[0] = 1;\n"
                "\t\t\terror = 0;\n"
                "\t\t\tgoto hakios_pid1_handled;\n"
                "\t\t}\n\n"
                "\t\tif (code == 93 /* SYS_select */) {\n"
                "\t\t\tstatic uint32_t hb_color = 0x0000FF00;\n"
                "\t\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(hb_color, 656, 4); } /* Heartbeat tick */\n"
                "\t\t\thb_color = (hb_color == 0x0000FF00) ? 0x0000FFFF : 0x0000FF00;\n"
                "\t\t}\n"
                "\t}\n\n"
                "\terror = (*(callp->sy_call))(proc, &uthread->uu_arg[0], &(uthread->uu_rval[0]));\nhakios_pid1_handled: ;"
            )
            if t_syscall in csys:
                csys = csys.replace(t_syscall, r_syscall, 1)
                with open(target_sys, "w", encoding="utf-8", newline="\n") as f:
                    f.write(csys)
                print("  [OK] bsd/dev/arm/systemcalls.c: userland syscall interception & text banner engine installed")
            else:
                print("  [WARN] bsd/dev/arm/systemcalls.c: error = (*(callp->sy_call)) pattern not found")

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
