#!/usr/bin/env python3
import os, sys

def main():
    if len(sys.argv) < 2:
        print("Usage: patch_cortex_a73.py <xnu_src_dir>")
        sys.exit(1)
    
    src = sys.argv[1]
    print(f"Applying Cortex-A73 PAC stubs, Method 1 exception handler, and visual FB ladder to {src}...")

    # 1. osfmk/arm/arm_init.c
    p1 = os.path.join(src, "osfmk", "arm", "arm_init.c")
    if os.path.isfile(p1):
        with open(p1, "r", encoding="utf-8", errors="replace") as f:
            c = f.read()
        t1 = '__asm__ volatile ("mrs %0, APIBKeyLo_EL1" : "=r" (r->reg_before));'
        t2 = '__asm__ volatile ("mrs %0, APIBKeyLo_EL1" : "=r" (r->reg_after));'
        if t1 in c and t2 in c:
            c = c.replace(t1, 'r->reg_before = 0;')
            c = c.replace(t2, 'r->reg_after = 0;')
            print("  [OK] osfmk/arm/arm_init.c: APIBKeyLo_EL1 stubbed")

        c_norm = c.replace("\r\n", "\n")

        # Bypass PAC structor signing (OSRuntimeSignStructors uses pacia/retab which UNDEF on Cortex-A73)
        sign1_old = "OSRuntimeSignStructors(&_mh_execute_header);"
        sign1_new = "// hakios: bypass PAC structor signing on Cortex-A73\n\t\t// OSRuntimeSignStructors(&_mh_execute_header);"
        if sign1_old in c_norm:
            c_norm = c_norm.replace(sign1_old, sign1_new)
            print("  [OK] osfmk/arm/arm_init.c: OSRuntimeSignStructors bypassed")
        else:
            print("  [WARN] sign1_old not found in arm_init.c")

        sign2_old = "OSRuntimeSignStructorsInFileset(kc_mh);"
        sign2_new = "// hakios: bypass PAC structor signing on Cortex-A73\n\t\t// OSRuntimeSignStructorsInFileset(kc_mh);"
        if sign2_old in c_norm:
            c_norm = c_norm.replace(sign2_old, sign2_new)
            print("  [OK] osfmk/arm/arm_init.c: OSRuntimeSignStructorsInFileset bypassed")
        else:
            print("  [WARN] sign2_old not found in arm_init.c")

        # Instrument early checkpoints in arm_init()
        target_init = """\tloader_checkpoint(30);   /* hakios: reached arm_init (C) at all (ret from start.s worked) */
\tarm_slide_rebase_and_sign_image();
\tloader_checkpoint(31);   /* past slide/rebase/sign */

\tarm_static_if_init(args);
\tloader_checkpoint(32);   /* past static_if_init */"""

        replacement_init = """\textern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
\thk_draw_band(0x00FFA500, 50, 10);   /* hakios visual marker: Orange at rows 50-59 (entered arm_init) */
\tloader_checkpoint(30);   /* hakios: reached arm_init (C) at all (ret from start.s worked) */
\thk_draw_band(0x0000FF00, 60, 10);   /* hakios visual marker: Green at rows 60-69 (past CP30, about to slide/rebase) */
\tarm_slide_rebase_and_sign_image();
\tloader_checkpoint(31);   /* past slide/rebase/sign */
\thk_draw_band(0x00800080, 70, 10);   /* hakios visual marker: Purple at rows 70-79 (past slide/rebase) */

\t// hakios: bypass arm_static_if_init on Cortex-A73 (uses blraa with chained fixup pointers)
\t// arm_static_if_init(args);
\tloader_checkpoint(32);   /* past static_if_init */
\thk_draw_band(0x00FFFF00, 80, 10);   /* hakios visual marker: Yellow at rows 80-89 (past static_if_init) */"""

        target_cpu = """\tloader_checkpoint(4);   /* before cpu_data_init */
\tcpu_data_init(&BootCpuData);
\tloader_checkpoint(5);   /* past cpu_data_init */"""

        replacement_cpu = """\thk_draw_band(0x0000FFFF, 90, 10);   /* hakios visual marker: Cyan at rows 90-99 (before cpu_data_init) */
\tloader_checkpoint(4);   /* before cpu_data_init */
\tcpu_data_init(&BootCpuData);
\tloader_checkpoint(5);   /* past cpu_data_init */
\thk_draw_band(0x00A52A2A, 100, 10);  /* hakios visual marker: Brown at rows 100-109 (past cpu_data_init) */"""

        target_pe = """\tPE_init_platform(FALSE, args); /* Get platform expert set up */
\tloader_checkpoint(6);   /* past PE_init_platform */"""

        replacement_pe = """\tPE_init_platform(FALSE, args); /* Get platform expert set up */
\tloader_checkpoint(6);   /* past PE_init_platform */
\thk_draw_band(0x0032CD32, 110, 5);  /* hakios visual marker: Lime at rows 110-114 (past PE_init_platform) */"""

        target_topo = """\tml_parse_cpu_topology();

\tmaster_cpu = ml_get_boot_cpu_number();
\tassert(master_cpu >= 0 && master_cpu <= ml_get_max_cpu_number());"""

        replacement_topo = """\thk_draw_band(0x00FF7F50, 115, 5);  /* hakios visual marker: Coral at rows 115-119 (before ml_parse_cpu_topology) */
\tml_parse_cpu_topology();
\thk_draw_band(0x00008080, 120, 5);  /* hakios visual marker: Teal at rows 120-124 (past ml_parse_cpu_topology) */

\tmaster_cpu = ml_get_boot_cpu_number();
\tif (master_cpu < 0 || master_cpu > ml_get_max_cpu_number()) {
\t\tmaster_cpu = 0;
\t}
\thk_draw_band(0x00FF1493, 125, 5);  /* hakios visual marker: Hot Pink at rows 125-129 (master_cpu valid) */
\tassert(master_cpu >= 0 && master_cpu <= ml_get_max_cpu_number());"""

        target_boot = """\tthread->machine.preemption_count = 0;
\tloader_checkpoint(44);
\tcpu_bootstrap();
\tloader_checkpoint(13);"""

        replacement_boot = """\tthread->machine.preemption_count = 0;
\tloader_checkpoint(44);
\thk_draw_band(0x00FF69B4, 130, 5);  /* hakios visual marker: Pink at rows 130-134 (preemption count 0) */
\tcpu_bootstrap();
\tloader_checkpoint(13);
\thk_draw_band(0x00FFD700, 135, 5);  /* hakios visual marker: Gold at rows 135-139 (past cpu_bootstrap) */"""

        target_rt = """\trtclock_early_init();
\tloader_checkpoint(14);"""

        replacement_rt = """\trtclock_early_init();
\tloader_checkpoint(14);
\thk_draw_band(0x0087CEEB, 140, 5);  /* hakios visual marker: Sky Blue at rows 140-144 (past rtclock_early_init) */"""

        target_ksb = """\tkernel_startup_bootstrap();
\tloader_checkpoint(15);"""

        replacement_ksb = """\tkernel_startup_bootstrap();
\tloader_checkpoint(15);
\thk_draw_band(0x00FFFFFF, 145, 5);  /* hakios visual marker: White at rows 145-149 (past kernel_startup_bootstrap) */"""

        target_cpu_init = """\tcpu_init();
\tloader_checkpoint(16);

\tprocessor_bootstrap();"""

        replacement_cpu_init = """\tcpu_init();
\tloader_checkpoint(16);
\thk_draw_band(0x00E6E6FA, 150, 5);  /* hakios visual marker: Lavender at rows 150-154 (past cpu_init) */

\tprocessor_bootstrap();
\thk_draw_band(0x00FF4500, 155, 5);  /* hakios visual marker: Orange-Red at rows 155-159 (past processor_bootstrap) */"""

        target_vm = """\tarm_vm_init(xmaxmem, args);"""

        replacement_vm = """\thk_draw_band(0x0000FA9A, 158, 2);  /* hakios visual marker: Medium Spring Green at rows 158-159 (before arm_vm_init) */
\tarm_vm_init(xmaxmem, args);
\thk_draw_band(0x00FF007F, 160, 3);  /* hakios visual marker: Rose at rows 160-162 (past arm_vm_init) */"""

        target_serial = """\tconst unsigned int serial_exists = serial_init();
\tloader_checkpoint(18);
\tkernel_startup_initialize_upto(STARTUP_SUB_KPRINTF);
\tloader_checkpoint(19);"""

        replacement_serial = """\tconst unsigned int serial_exists = serial_init();
\tloader_checkpoint(18);
\thk_draw_band(0x00DC143C, 163, 3);  /* hakios visual marker: Crimson at rows 163-165 (past serial_init) */
\tkernel_startup_initialize_upto(STARTUP_SUB_KPRINTF);
\tloader_checkpoint(19);
\thk_draw_band(0x007FFF00, 166, 4);  /* hakios visual marker: Chartreuse at rows 166-169 (past STARTUP_SUB_KPRINTF) */"""

        target_console = """\tPE_create_console();
\tloader_checkpoint(20);"""

        replacement_console = """\tPE_create_console();
\tloader_checkpoint(20);
\thk_draw_band(0x0000BFFF, 170, 5);  /* hakios visual marker: Deep Sky Blue at rows 170-174 (past PE_create_console) */"""

        target_pe_true = """\tPE_init_platform(TRUE, &BootCpuData);"""

        replacement_pe_true = """\tPE_init_platform(TRUE, &BootCpuData);
\thk_draw_band(0x0000FF7F, 175, 5);  /* hakios visual marker: Spring Green at rows 175-179 (past PE_init_platform TRUE) */"""

        target_mstart = """\tloader_checkpoint(21);
\tmachine_startup(args);"""

        replacement_mstart = """\tloader_checkpoint(21);
\thk_draw_band(0x00FFFF00, 180, 5);  /* hakios visual marker: Yellow at rows 180-184 (entering machine_startup) */
\tmachine_startup(args);"""

        if target_init in c_norm:
            c_norm = c_norm.replace(target_init, replacement_init)
            print("  [OK] osfmk/arm/arm_init.c: arm_init visual markers instrumented")
        else:
            print("  [WARN] target_init not found in arm_init.c")

        if target_cpu in c_norm:
            c_norm = c_norm.replace(target_cpu, replacement_cpu)
            print("  [OK] osfmk/arm/arm_init.c: cpu_data_init visual markers instrumented")
        else:
            print("  [WARN] target_cpu not found in arm_init.c")

        if target_pe in c_norm:
            c_norm = c_norm.replace(target_pe, replacement_pe)
            print("  [OK] osfmk/arm/arm_init.c: PE_init_platform visual markers instrumented")
        else:
            print("  [WARN] target_pe not found in arm_init.c")

        if target_topo in c_norm:
            c_norm = c_norm.replace(target_topo, replacement_topo)
            print("  [OK] osfmk/arm/arm_init.c: cpu_topology visual markers and master_cpu clamp instrumented")
        else:
            print("  [WARN] target_topo not found in arm_init.c")

        if target_boot in c_norm:
            c_norm = c_norm.replace(target_boot, replacement_boot)
            print("  [OK] osfmk/arm/arm_init.c: cpu_bootstrap visual markers instrumented")
        else:
            print("  [WARN] target_boot not found in arm_init.c")

        if target_rt in c_norm:
            c_norm = c_norm.replace(target_rt, replacement_rt)
            print("  [OK] osfmk/arm/arm_init.c: rtclock_early_init visual markers instrumented")
        else:
            print("  [WARN] target_rt not found in arm_init.c")

        if target_ksb in c_norm:
            c_norm = c_norm.replace(target_ksb, replacement_ksb)
            print("  [OK] osfmk/arm/arm_init.c: kernel_startup_bootstrap visual markers instrumented")
        else:
            print("  [WARN] target_ksb not found in arm_init.c")

        if target_cpu_init in c_norm:
            c_norm = c_norm.replace(target_cpu_init, replacement_cpu_init)
            print("  [OK] osfmk/arm/arm_init.c: cpu_init visual markers instrumented")
        else:
            print("  [WARN] target_cpu_init not found in arm_init.c")

        if target_vm in c_norm:
            c_norm = c_norm.replace(target_vm, replacement_vm)
            print("  [OK] osfmk/arm/arm_init.c: arm_vm_init visual markers instrumented")
        else:
            print("  [WARN] target_vm not found in arm_init.c")

        if target_serial in c_norm:
            c_norm = c_norm.replace(target_serial, replacement_serial)
            print("  [OK] osfmk/arm/arm_init.c: serial_init visual markers instrumented")
        else:
            print("  [WARN] target_serial not found in arm_init.c")

        if target_console in c_norm:
            c_norm = c_norm.replace(target_console, replacement_console)
            print("  [OK] osfmk/arm/arm_init.c: PE_create_console visual markers instrumented")
        else:
            print("  [WARN] target_console not found in arm_init.c")

        if target_pe_true in c_norm:
            c_norm = c_norm.replace(target_pe_true, replacement_pe_true)
            print("  [OK] osfmk/arm/arm_init.c: PE_init_platform(TRUE) visual markers instrumented")
        else:
            print("  [WARN] target_pe_true not found in arm_init.c")

        if target_mstart in c_norm:
            c_norm = c_norm.replace(target_mstart, replacement_mstart)
            print("  [OK] osfmk/arm/arm_init.c: machine_startup visual markers instrumented")
        else:
            print("  [WARN] target_mstart not found in arm_init.c")

        with open(p1, "w", encoding="utf-8", newline="\n") as f:
            f.write(c_norm)

    # 2. osfmk/arm64/pac_std_asm.h
    p2 = os.path.join(src, "osfmk", "arm64", "pac_std_asm.h")
    if os.path.isfile(p2):
        with open(p2, "r", encoding="utf-8", errors="replace") as f:
            c = f.read()
        
        load_keys_old = """.macro PAC_STD_LOAD_KEYS base, lo, hi
\tPAC_STD_LOAD_KEY \\base, PAC_STD_KEY_IA, \\lo, \\hi
\tmsr\tAPIAKeyLo_EL1, \\lo
\tmsr\tAPIAKeyHi_EL1, \\hi
\tPAC_STD_LOAD_KEY \\base, PAC_STD_KEY_IB, \\lo, \\hi
\tmsr\tAPIBKeyLo_EL1, \\lo
\tmsr\tAPIBKeyHi_EL1, \\hi
\tPAC_STD_LOAD_KEY \\base, PAC_STD_KEY_DA, \\lo, \\hi
\tmsr\tAPDAKeyLo_EL1, \\lo
\tmsr\tAPDAKeyHi_EL1, \\hi
\tPAC_STD_LOAD_KEY \\base, PAC_STD_KEY_DB, \\lo, \\hi
\tmsr\tAPDBKeyLo_EL1, \\lo
\tmsr\tAPDBKeyHi_EL1, \\hi
\tPAC_STD_LOAD_KEY \\base, PAC_STD_KEY_GA, \\lo, \\hi
\tmsr\tAPGAKeyLo_EL1, \\lo
\tmsr\tAPGAKeyHi_EL1, \\hi
.endm"""

        load_keys_new = """.macro PAC_STD_LOAD_KEYS base, lo, hi
\t// hakios: Bengal Cortex-A73 lacks hardware PAC sysregs; stub out
.endm"""

        load_a_old = """.macro PAC_STD_LOAD_A_KEYS base, id, lo, hi, t1, t2, t3, t4
\tPAC_STD_DERIVE_KEY \\base, PAC_STD_KEY_IA, \\id, \\lo, \\hi, \\t1, \\t2, \\t3, \\t4
\tmsr\tAPIAKeyLo_EL1, \\lo
\tmsr\tAPIAKeyHi_EL1, \\hi
\tmsr\tAPDAKeyLo_EL1, \\lo
\tmsr\tAPDAKeyHi_EL1, \\hi
.endm"""

        load_a_new = """.macro PAC_STD_LOAD_A_KEYS base, id, lo, hi, t1, t2, t3, t4
\t// hakios: Bengal Cortex-A73 lacks hardware PAC sysregs; stub out
.endm"""

        load_b_old = """.macro PAC_STD_LOAD_B_KEYS base, id, lo, hi, t1, t2, t3, t4
\tPAC_STD_DERIVE_KEY \\base, PAC_STD_KEY_IB, \\id, \\lo, \\hi, \\t1, \\t2, \\t3, \\t4
\tmsr\tAPIBKeyLo_EL1, \\lo
\tmsr\tAPIBKeyHi_EL1, \\hi
\tmsr\tAPDBKeyLo_EL1, \\lo
\tmsr\tAPDBKeyHi_EL1, \\hi
.endm"""

        load_b_new = """.macro PAC_STD_LOAD_B_KEYS base, id, lo, hi, t1, t2, t3, t4
\t// hakios: Bengal Cortex-A73 lacks hardware PAC sysregs; stub out
.endm"""

        c_norm = c.replace("\r\n", "\n")
        assert load_keys_old in c_norm, "PAC_STD_LOAD_KEYS not found in pac_std_asm.h"
        assert load_a_old in c_norm, "PAC_STD_LOAD_A_KEYS not found in pac_std_asm.h"
        assert load_b_old in c_norm, "PAC_STD_LOAD_B_KEYS not found in pac_std_asm.h"

        c_norm = c_norm.replace(load_keys_old, load_keys_new)
        c_norm = c_norm.replace(load_a_old, load_a_new)
        c_norm = c_norm.replace(load_b_old, load_b_new)

        with open(p2, "w", encoding="utf-8", newline="\n") as f:
            f.write(c_norm)
        print("  [OK] osfmk/arm64/pac_std_asm.h")

    # 3. osfmk/arm64/sleh.c
    p3 = os.path.join(src, "osfmk", "arm64", "sleh.c")
    if os.path.isfile(p3):
        with open(p3, "r", encoding="utf-8", errors="replace") as f:
            c = f.read()
        sleh_old = """\t\t\tchar line[200];
\t\t\tuint64_t ia, ga;
\t\t\t__asm__ volatile ("mrs %0, APIAKeyLo_EL1" : "=r" (ia));
\t\t\t__asm__ volatile ("mrs %0, APGAKeyLo_EL1" : "=r" (ga));"""
        sleh_new = """\t\t\tchar line[200];
\t\t\tuint64_t ia = 0, ga = 0;"""
        c_norm = c.replace("\r\n", "\n")
        if sleh_old in c_norm:
            c_norm = c_norm.replace(sleh_old, sleh_new)
            print("  [OK] osfmk/arm64/sleh.c: APIAKeyLo_EL1/APGAKeyLo_EL1 stubbed")
        else:
            print("  [WARN] sleh target not found in sleh.c")

        sleh_hook_old = """\tif (class == 0) {
\t\tif (pac_patch_handle_trap(state, is_user)) {
\t\t\treturn;
\t\t}
\t\tif (lse_patch_handle_trap(state, is_user)) {
\t\t\treturn;
\t\t}
\t}"""
        sleh_hook_new = """\textern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
\tif (class == 0) {
\t\tif (pac_patch_handle_trap(state, is_user)) {
\t\t\treturn;
\t\t}
\t\tif (lse_patch_handle_trap(state, is_user)) {
\t\t\treturn;
\t\t}
\t\tif (!is_user) {
\t\t\thk_draw_band(0x00FF0000, 200, 10); /* Red band: unhandled kernel UNDEF */
\t\t}
\t} else if (!is_user) {
\t\thk_draw_band(0x00FF00FF, 210, 10); /* Magenta band: kernel fault/panic */
\t}"""
        if sleh_hook_old in c_norm:
            c_norm = c_norm.replace(sleh_hook_old, sleh_hook_new)
            print("  [OK] osfmk/arm64/sleh.c: early kernel fault visual bands installed")
        else:
            print("  [WARN] sleh_hook_old not found in sleh.c")

        with open(p3, "w", encoding="utf-8", newline="\n") as f:
            f.write(c_norm)

    # 4. osfmk/arm64/start.s
    p4 = os.path.join(src, "osfmk", "arm64", "start.s")
    if os.path.isfile(p4):
        with open(p4, "r", encoding="utf-8", errors="replace") as f:
            c = f.read()
        c_norm = c.replace("\r\n", "\n")

        # CP1: start_first_cpu entry - Yellow band (rows 0-9, 0x5c000000)
        start_old = """\tmov\t\tx20, x0
\tmov\t\tx21, #0
\tLOADER_CHECKPOINT 1"""

        start_new = """\tmov\t\tx20, x0
\tmov\t\tx21, #0
\t// hakios visual marker CP1: Yellow band at rows 0-9 (0x5c000000)
\tmovz\tx9, #0x5c00, lsl #16
\tmovz\tx10, #0xff00
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0xff00, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
\tmov\t\tw11, #2700
1:
\tstp\tx10, x10, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t1b
\tLOADER_CHECKPOINT 1"""

        assert start_old in c_norm, "start_first_cpu checkpoint 1 not found in start.s"
        c_norm = c_norm.replace(start_old, start_new)

        # CP2: after OSLAR_EL1 - Cyan band (rows 10-19, 0x5c00a8c0)
        cp2_old = """\t// Unlock the core for debugging
\tmsr\t\tOSLAR_EL1, xzr
\tLOADER_CHECKPOINT 2"""

        cp2_new = """\t// Unlock the core for debugging
\tmsr\t\tOSLAR_EL1, xzr
\t// hakios visual marker CP2: Cyan band at rows 10-19 (0x5c00a8c0)
\tmovz\tx9, #0xa8c0
\tmovk\tx9, #0x5c00, lsl #16
\tmovz\tx10, #0xffff
\tmovk\tx10, #0x0000, lsl #16
\tmovk\tx10, #0xffff, lsl #32
\tmovk\tx10, #0x0000, lsl #48
\tmov\t\tw11, #2700
1:
\tstp\tx10, x10, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t1b
\tLOADER_CHECKPOINT 2"""

        assert cp2_old in c_norm, "checkpoint 2 not found in start.s"
        c_norm = c_norm.replace(cp2_old, cp2_new)

        # CP3: after page tables built - Magenta band (rows 20-29, 0x5c015180) & save (VA - PA) delta
        cp3_old = """\t/* Ensure TTEs are visible */
\tdsb\t\tish

\tLOADER_CHECKPOINT 3

\tb\t\tcommon_start"""

        cp3_new = """\t/* Ensure TTEs are visible */
\tdsb\t\tish
\t// hakios: record (VA - PA) delta in loader_stub_slot[3] for runtime PA transitions
\tadrp\tx9, EXT(loader_stub_slot)@page
\tadd\tx9, x9, EXT(loader_stub_slot)@pageoff
\tsub\tx10, x22, x23
\tstr\tx10, [x9, #24]
\t// hakios visual marker CP3: Magenta band at rows 20-29 (0x5c015180)
\tmovz\tx9, #0x5180
\tmovk\tx9, #0x5c01, lsl #16
\tmovz\tx10, #0x00ff
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0x00ff, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
\tmov\t\tw11, #2700
1:
\tstp\tx10, x10, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t1b

\tLOADER_CHECKPOINT 3

\tb\t\tcommon_start"""

        assert cp3_old in c_norm, "checkpoint 3 not found in start.s"
        c_norm = c_norm.replace(cp3_old, cp3_new)

        # CP7: in common_start right before MMU enable - White band (rows 30-39, 0x5c01fa40)
        cp7_old = """\tLOADER_CHECKPOINT 7\t\t// hakios: reached common_start, about to enable the MMU
\t// Enable caches, MMU, ROP and JOP
\tMOV64   x0, SCTLR_EL1_DEFAULT"""

        cp7_new = """\t// hakios visual marker CP7: White band at rows 30-39 (0x5c01fa40)
\tmovz\tx9, #0xfa40
\tmovk\tx9, #0x5c01, lsl #16
\tmovz\tx10, #0xffff
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0xffff, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
\tmov\t\tw11, #2700
1:
\tstp\tx10, x10, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t1b

\tLOADER_CHECKPOINT 7\t\t// hakios: reached common_start, about to enable the MMU
\t// Enable caches, MMU, ROP and JOP
\tMOV64   x0, SCTLR_EL1_DEFAULT"""

        assert cp7_old in c_norm, "checkpoint 7 not found in start.s"
        c_norm = c_norm.replace(cp7_old, cp7_new)

        # CP11: in common_start right before ret into arm_init - Blue band (rows 40-49, 0x5c02a300)
        cp11_old = """\tLOADER_CHECKPOINT 11\t\t// hakios: about to ret into arm_init()
\t// Return to arm_init()
\tret"""

        cp11_new = """\t// hakios visual marker CP11: Blue band at rows 40-49 (0x5c02a300)
\tmrs\t\tx9, SCTLR_EL1
\tbic\t\tx9, x9, #1
\tmsr\t\tSCTLR_EL1, x9
\tisb
\tmovz\tx9, #0xa300
\tmovk\tx9, #0x5c02, lsl #16
\tmovz\tx10, #0x00ff
\tmovk\tx10, #0x0000, lsl #16
\tmovk\tx10, #0x00ff, lsl #32
\tmovk\tx10, #0x0000, lsl #48
\tmov\t\tw11, #2700
1:
\tstp\tx10, x10, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t1b
\tmrs\t\tx9, SCTLR_EL1
\torr\t\tx9, x9, #1
\tmsr\t\tSCTLR_EL1, x9
\tisb

\tLOADER_CHECKPOINT 11\t\t// hakios: about to ret into arm_init()
\t// Return to arm_init()
\tret"""

        assert cp11_old in c_norm, "checkpoint 11 not found in start.s"
        c_norm = c_norm.replace(cp11_old, cp11_new)

        # Early Exception: Method 1 (Pure asm, no stack, safe transition, dump to 0x82f00000, Red band at rows 110-119)
        exc_old = """\tmov\t\tx2, #1
\tmrs\t\tx9, SCTLR_EL1
\tbic\t\tx9, x9, #1
\tmsr\t\tSCTLR_EL1, x9
\tisb
\tadr\t\tx9, Lloader_stub_slot
\tldr\t\tx9, [x9, #8]
\tcbz\t\tx9, .
\tbr\t\tx9"""

        exc_new = """\t// Method 1: Dump crash registers to fixed RAM 0x82f00000 (accessible in V=P via TTBR0)
\tmovz\tx9, #0x0000
\tmovk\tx9, #0x82f0, lsl #16
\tstp\tx3, x4, [x9]
\tstp\tx5, x6, [x9, #16]
\tstr\tx7, [x9, #32]
\tdc\tcvac, x9
\tdsb\tsy

\t// Transition to PA if MMU is enabled before disabling MMU:
\tmrs\tx12, SCTLR_EL1
\ttbz\tx12, #0, 1f
\tadr\tx10, 1f
\tadrp\tx11, EXT(loader_stub_slot)@page
\tadd\tx11, x11, EXT(loader_stub_slot)@pageoff
\tldr\tx11, [x11, #24]
\tsub\tx10, x10, x11
\tbr\tx10
1:
\t// Now running at PA: safely disable MMU
\tmrs\tx11, SCTLR_EL1
\tbic\tx11, x11, #1
\tmsr\tSCTLR_EL1, x11
\tisb

\t// Draw RED band at rows 200-209 (0x5c0d3000)
\tmovz\tx10, #0x3000
\tmovk\tx10, #0x5c0d, lsl #16
\tmovz\tw13, #0x0000
\tmovk\tw13, #0x00ff, lsl #16
\tmov\tw14, #10800
3:\tstr\tw13, [x10], #4
\tsubs\tw14, w14, #1
\tb.ne\t3b

2:\twfe
\tb\t2b"""

        assert exc_old in c_norm, "Lloader_early_exception old block not found in start.s"
        c_norm = c_norm.replace(exc_old, exc_new)

        # Append hk_draw_band helper to start.s
        hk_draw_band_code = """
/*
 * hakios: visual marker band drawer (accessible from both start.s and arm_init.c)
 * void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
 * Temporarily restores TTBR0_EL1 to PA(bootstrap_pagetables) when MMU is ON so
 * PA transition works both before and after arm_vm_init() switches TTBR0_EL1.
 */
\t.text
\t.align 2
\t.globl EXT(hk_draw_band)
LEXT(hk_draw_band)
\tstp\t\tx29, x30, [sp, #-64]!
\tstp\t\tx19, x20, [sp, #16]
\tstp\t\tx21, x22, [sp, #32]
\tstp\t\tx23, x24, [sp, #48]

\tand\t\tx0, x0, #0xffffffff
\torr\t\tx19, x0, x0, lsl #32

\tmov\t\tx20, #4320
\tmul\t\tx20, x1, x20
\tmovz\tx21, #0x5c00, lsl #16
\tadd\t\tx20, x20, x21

\tmov\t\tw21, #270
\tmul\t\tw21, w2, w21

\tmrs\t\tx23, DAIF
\tmsr\t\tDAIFSet, #0xf

\tmrs\t\tx22, SCTLR_EL1
\ttbz\t\tx22, #0, 1f

\tmrs\t\tx24, TTBR0_EL1
\tadrp\tx11, EXT(loader_stub_slot)@page
\tadd\tx11, x11, EXT(loader_stub_slot)@pageoff
\tldr\tx11, [x11, #24]
\tadrp\tx12, EXT(bootstrap_pagetables)@page
\tadd\tx12, x12, EXT(bootstrap_pagetables)@pageoff
\tsub\tx12, x12, x11
\tmsr\t\tTTBR0_EL1, x12
\tisb
\ttlbi\tvmalle1
\tdsb\t\tsy
\tisb

\tadr\t\tx13, 3f
\tadr\t\tx10, 1f
\tsub\t\tx10, x10, x11
\tbr\t\tx10

1:
\tmrs\t\tx9, SCTLR_EL1
\tbic\t\tx10, x9, #1
\tmsr\t\tSCTLR_EL1, x10
\tisb

\tmov\t\tx11, x20
\tmov\t\tw12, w21
2:
\tstp\tx19, x19, [x11], #16
\tsubs\tw12, w12, #1
\tb.ne\t2b
\tdsb\t\tsy

\tmsr\t\tSCTLR_EL1, x9
\tisb

\ttbz\t\tx22, #0, 4f
\tbr\t\tx13

3:
\tmsr\t\tTTBR0_EL1, x24
\tisb
\ttlbi\tvmalle1
\tdsb\t\tsy
\tisb
4:
\tmsr\t\tDAIF, x23
\tldp\t\tx23, x24, [sp, #48]
\tldp\t\tx21, x22, [sp, #32]
\tldp\t\tx19, x20, [sp, #16]
\tldp\t\tx29, x30, [sp], #64
\tret
"""
        hk_render_screen_asm_code = """
/*
 * hakios: on-screen panic text renderer
 * void hk_render_screen_asm(const char *str, uint32_t len, const unsigned char *font);
 * Renders 2x scaled ASCII text directly to physical framebuffer at 0x5c000000.
 */
\t.text
\t.align 2
\t.globl EXT(hk_render_screen_asm)
LEXT(hk_render_screen_asm)
\tstp\t\tx29, x30, [sp, #-80]!
\tstp\t\tx19, x20, [sp, #16]
\tstp\t\tx21, x22, [sp, #32]
\tstp\t\tx23, x24, [sp, #48]
\tstp\t\tx25, x26, [sp, #64]

\tmov\t\tx19, x0
\tmov\t\tw20, w1
\tmov\t\tx21, x2

\tmrs\t\tx23, DAIF
\tmsr\t\tDAIFSet, #0xf

\tadrp\tx11, EXT(loader_stub_slot)@page
\tadd\t\tx11, x11, EXT(loader_stub_slot)@pageoff
\tldr\t\tx11, [x11, #24]

\tsub\t\tx19, x19, x11
\tsub\t\tx21, x21, x11

\tmrs\t\tx22, SCTLR_EL1
\ttbz\t\tx22, #0, 1f

\tmrs\t\tx24, TTBR0_EL1
\tadrp\tx12, EXT(bootstrap_pagetables)@page
\tadd\t\tx12, x12, EXT(bootstrap_pagetables)@pageoff
\tsub\t\tx12, x12, x11
\tmsr\t\tTTBR0_EL1, x12
\tisb
\ttlbi\tvmalle1
\tdsb\t\tsy
\tisb

\tadr\t\tx13, 8f
\tadr\t\tx10, 1f
\tsub\t\tx10, x10, x11
\tbr\t\tx10

1:
\tmrs\t\tx9, SCTLR_EL1
\tbic\t\tx10, x9, #1
\tmsr\t\tSCTLR_EL1, x10
\tisb

\tmov\t\tx25, #20
\tmov\t\tx26, #250

\tmovz\tw14, #0xffff
\tmovk\tw14, #0x00ff, lsl #16
\tmov\t\tx15, #4320

2:\tcbz\t\tw20, 7f
\tldrb\tw0, [x19], #1
\tsub\t\tw20, w20, #1
\tcbz\t\tw0, 7f

\tcmp\t\tw0, #13
\tb.eq\t2b

\tcmp\t\tw0, #10
\tb.ne\t3f
\tmov\t\tx25, #20
\tadd\t\tx26, x26, #32
\tcmp\t\tx26, #2360
\tb.ge\t7f
\tb\t\t2b

3:\tcmp\t\tw0, #32
\tb.lt\t4f
\tcmp\t\tw0, #126
\tb.le\t5f
4:\tmov\t\tw0, #32

5:\tand\t\tx1, x0, #0xff
\tlsl\t\tx1, x1, #4
\tadd\t\tx1, x21, x1

\tmul\t\tx2, x26, x15
\tmovz\tx3, #0x5c00, lsl #16
\tadd\t\tx2, x2, x3
\tadd\t\tx2, x2, x25, lsl #2

\tmov\t\tw4, #0
6:\tldrb\tw5, [x1, x4]

\tmov\t\tw6, #7
\tmov\t\tx7, x2
\tadd\t\tx8, x2, x15

61:\tlsr\t\tw10, w5, w6
\ttbz\t\tw10, #0, 62f
\tstr\t\tw14, [x7], #4
\tstr\t\tw14, [x7], #4
\tstr\t\tw14, [x8], #4
\tstr\t\tw14, [x8], #4
\tb\t\t63f
62:\tstr\t\twzr, [x7], #4
\tstr\t\twzr, [x7], #4
\tstr\t\twzr, [x8], #4
\tstr\t\twzr, [x8], #4
63:\tsubs\tw6, w6, #1
\tb.ge\t61b

\tadd\t\tx2, x2, x15
\tadd\t\tx2, x2, x15
\tadd\t\tw4, w4, #1
\tcmp\t\tw4, #16
\tb.lt\t6b

\tadd\t\tx25, x25, #16
\tcmp\t\tx25, #1060
\tb.lt\t2b
\tmov\t\tx25, #20
\tadd\t\tx26, x26, #32
\tcmp\t\tx26, #2360
\tb.lt\t2b

7:\tdsb\t\tsy

\tmsr\t\tSCTLR_EL1, x9
\tisb

\ttbz\t\tx22, #0, 9f
\tbr\t\tx13

8:\tmsr\t\tTTBR0_EL1, x24
\tisb
\ttlbi\tvmalle1
\tdsb\t\tsy
\tisb

9:\tmsr\t\tDAIF, x23
\tldp\t\tx25, x26, [sp, #64]
\tldp\t\tx23, x24, [sp, #48]
\tldp\t\tx21, x22, [sp, #32]
\tldp\t\tx19, x20, [sp, #16]
\tldp\t\tx29, x30, [sp], #80
\tret
"""
        c_norm = c_norm + hk_draw_band_code + hk_render_screen_asm_code

        with open(p4, "w", encoding="utf-8", newline="\n") as f:
            f.write(c_norm)
        print("  [OK] osfmk/arm64/start.s: visual markers, hk_draw_band, and hk_render_screen_asm installed")

    # 5. pexpert/pexpert/arm64/board_config.h (SM6225 Bengal has 2 clusters: Gold A73 + Silver A53)
    p5 = os.path.join(src, "pexpert", "pexpert", "arm64", "board_config.h")
    if os.path.isfile(p5):
        with open(p5, "r", encoding="utf-8", errors="replace") as f:
            c5 = f.read()
        if "#define MAX_CPU_CLUSTERS               1" in c5:
            c5 = c5.replace("#define MAX_CPU_CLUSTERS               1",
                            "#define MAX_CPU_CLUSTERS               4\n#define MAX_CPU_CLUSTER_PHY_ID         4")
            with open(p5, "w", encoding="utf-8", newline="\n") as f:
                f.write(c5)
            print("  [OK] pexpert/pexpert/arm64/board_config.h: MAX_CPU_CLUSTERS set to 4")
        elif "MAX_CPU_CLUSTERS               4" in c5:
            print("  [OK] pexpert/pexpert/arm64/board_config.h: already has MAX_CPU_CLUSTERS 4")
        else:
            print("  [WARN] MAX_CPU_CLUSTERS not found in board_config.h")

    # 6. pexpert/pexpert/arm64/BCM2837.h (Cortex-A73/A53 is ARMv8.0-A: lacks ARMv8.1 FEAT_PAN)
    p6 = os.path.join(src, "pexpert", "pexpert", "arm64", "BCM2837.h")
    if os.path.isfile(p6):
        with open(p6, "r", encoding="utf-8", errors="replace") as f:
            c6 = f.read().replace("\r\n", "\n")
        if "#define __ARM_PAN_AVAILABLE__     1" in c6:
            c6 = c6.replace("#define __ARM_PAN_AVAILABLE__     1", "// #define __ARM_PAN_AVAILABLE__     1")
            with open(p6, "w", encoding="utf-8", newline="\n") as f:
                f.write(c6)
            print("  [OK] pexpert/pexpert/arm64/BCM2837.h: __ARM_PAN_AVAILABLE__ disabled for Cortex-A73")
        else:
            print("  [WARN] __ARM_PAN_AVAILABLE__ not found in BCM2837.h")

    # 7. makedefs/MakeInc.def (replace +pan with -pan for BCM2837)
    p7 = os.path.join(src, "makedefs", "MakeInc.def")
    if os.path.isfile(p7):
        with open(p7, "r", encoding="utf-8", errors="replace") as f:
            c7 = f.read().replace("\r\n", "\n")
        if "-Xclang -target-feature -Xclang +pan" in c7:
            c7 = c7.replace("-Xclang -target-feature -Xclang +pan", "-Xclang -target-feature -Xclang -pan")
            with open(p7, "w", encoding="utf-8", newline="\n") as f:
                f.write(c7)
            print("  [OK] makedefs/MakeInc.def: +pan replaced with -pan")
        else:
            print("  [WARN] +pan not found in MakeInc.def")

    # 8. osfmk/arm64/locore.s (stub ml_sign_thread_state & ml_check_signed_state so trap PC/LR updates never panic)
    p8 = os.path.join(src, "osfmk", "arm64", "locore.s")
    if os.path.isfile(p8):
        with open(p8, "r", encoding="utf-8", errors="replace") as f:
            c8 = f.read().replace("\r\n", "\n")
        changed_locore = False
        if "LEXT(ml_sign_thread_state)\n" in c8 and "LEXT(ml_sign_thread_state)\n\tret\n" not in c8:
            c8 = c8.replace("LEXT(ml_sign_thread_state)\n", "LEXT(ml_sign_thread_state)\n\tret\n", 1)
            changed_locore = True
        if "LEXT(ml_check_signed_state)\n" in c8 and "LEXT(ml_check_signed_state)\n\tret\n" not in c8:
            c8 = c8.replace("LEXT(ml_check_signed_state)\n", "LEXT(ml_check_signed_state)\n\tret\n", 1)
            changed_locore = True
        if changed_locore:
            with open(p8, "w", encoding="utf-8", newline="\n") as f:
                f.write(c8)
            print("  [OK] osfmk/arm64/locore.s: ml_sign_thread_state and ml_check_signed_state stubbed")
        else:
            print("  [WARN] ml_sign_thread_state / ml_check_signed_state not found in locore.s")

    # 9. osfmk/arm64/loader_return.c (draw Dark Orange band and render panic text to screen)
    p9 = os.path.join(src, "osfmk", "arm64", "loader_return.c")
    if os.path.isfile(p9):
        with open(p9, "r", encoding="utf-8", errors="replace") as f:
            c9 = f.read().replace("\r\n", "\n")
        lr_old = "void\nloader_return(void)\n{\n\tredwood_ufs_close();"
        lr_new = "void\nloader_return(void)\n{\n\textern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);\n\thk_draw_band(0x00FF8C00, 220, 10); /* Dark Orange band: loader_return / panic */\n\tredwood_ufs_close();"
        if lr_old in c9:
            c9 = c9.replace(lr_old, lr_new, 1)
            print("  [OK] osfmk/arm64/loader_return.c: Dark Orange band added to loader_return()")
        else:
            print("  [WARN] loader_return target not found in loader_return.c")

        dump_target = "\tloader_jump(0, 0);"
        dump_code = """\t/* hakios: on-screen panic dump directly to display */
\t{
\t\textern void hk_render_screen_asm(const char *str, uint32_t len, const unsigned char *font);
\t\textern const unsigned char iso_font[];
\t\tstatic char panic_onscreen[3500];
\t\tuint32_t n = 0;
\t\tif (debug_buf_base != NULL && debug_buf_ptr > debug_buf_base) {
\t\t\tconst char *p = debug_buf_base;
\t\t\twhile (p < debug_buf_ptr && n < 3400) {
\t\t\t\tpanic_onscreen[n++] = *p++;
\t\t\t}
\t\t} else if (loader_log_len > 0) {
\t\t\tuint32_t start = (loader_log_len > 3000) ? (loader_log_len - 3000) : 0;
\t\t\twhile (start < loader_log_len && n < 3400) {
\t\t\t\tpanic_onscreen[n++] = loader_log[start++];
\t\t\t}
\t\t}
\t\tif (n > 0) {
\t\t\tpanic_onscreen[n] = '\\0';
\t\t\thk_render_screen_asm(panic_onscreen, n, iso_font);
\t\t}
\t}
\tloader_jump(0, 0);"""
        if dump_target in c9:
            c9 = c9.replace(dump_target, dump_code, 1)
            print("  [OK] osfmk/arm64/loader_return.c: on-screen panic dump added before loader_jump")
        else:
            print("  [WARN] loader_jump target not found in loader_return.c")

        with open(p9, "w", encoding="utf-8", newline="\n") as f:
            f.write(c9)

    # 10. osfmk/kern/startup.c (trace startup entries with visual markers)
    p10 = os.path.join(src, "osfmk", "kern", "startup.c")
    if os.path.isfile(p10):
        with open(p10, "r", encoding="utf-8", errors="replace") as f:
            c10 = f.read().replace("\r\n", "\n")
        t_startup = """\t\tstartup_phase = cur->subsystem - 1;
\t\tkernel_startup_log(cur->subsystem);
\t\tcur->func(cur->arg);"""
        r_startup = """\t\tstartup_phase = cur->subsystem - 1;
\t\tkernel_startup_log(cur->subsystem);
\t\t/* hakios: trace startup subsystem */
\t\t{
\t\t\textern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
\t\t\thk_draw_band(0x00FFD700, 164 + (cur->subsystem & 3), 1);
\t\t}
\t\tcur->func(cur->arg);"""
        if t_startup in c10:
            c10 = c10.replace(t_startup, r_startup, 1)
            with open(p10, "w", encoding="utf-8", newline="\n") as f:
                f.write(c10)
            print("  [OK] osfmk/kern/startup.c: startup subsystem visual trace installed")
        else:
            print("  [WARN] t_startup not found in startup.c")

    print("Cortex-A73 patches successfully applied!")

if __name__ == "__main__":
    main()
