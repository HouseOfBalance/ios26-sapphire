#!/usr/bin/env python3
import os, sys, re

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
\textern void hk_render_screen_asm(const char *str, uint32_t len, const unsigned char *font);
\textern const unsigned char iso_font[];
\tif (class == 0) {
\t\tif (pac_patch_handle_trap(state, is_user)) {
\t\t\treturn;
\t\t}
\t\tif (lse_patch_handle_trap(state, is_user)) {
\t\t\treturn;
\t\t}
\t\tif (!is_user) {
\t\t\thk_draw_band(0x00FF0000, 200, 10); /* Red band: unhandled kernel UNDEF */
\t\t\tstatic char umsg[512];
\t\t\tuint64_t pc = (uint64_t)get_saved_state_pc(state);
\t\t\tuint32_t insn = (pc >= 0xffff000000000000ULL) ? *(volatile uint32_t *)pc : 0;
\t\t\tint n = snprintf(umsg, sizeof(umsg),
\t\t\t\t"=== HAKIOS KERNEL UNDEF ===\\n"
\t\t\t\t"pc  : 0x%llx\\n"
\t\t\t\t"insn: 0x%08x\\n"
\t\t\t\t"lr  : 0x%llx\\n"
\t\t\t\t"sp  : 0x%llx\\n"
\t\t\t\t"esr : 0x%llx\\n"
\t\t\t\t"far : 0x%lx\\n"
\t\t\t\t"x0  : 0x%llx\\n"
\t\t\t\t"x1  : 0x%llx\\n",
\t\t\t\tpc, insn,
\t\t\t\t(uint64_t)get_saved_state_lr(state),
\t\t\t\t(uint64_t)get_saved_state_sp(state),
\t\t\t\tesr, (unsigned long)far,
\t\t\t\t(uint64_t)get_saved_state_reg(state, 0),
\t\t\t\t(uint64_t)get_saved_state_reg(state, 1));
\t\t\tfor (uintptr_t p = (uintptr_t)umsg; p < (uintptr_t)umsg + n + 64; p += 64) {
\t\t\t\t__asm__ volatile ("dc cvac, %0" : : "r" (p));
\t\t\t}
\t\t\t__asm__ volatile ("dsb sy");
\t\t\thk_render_screen_asm(umsg, n, iso_font);
\t\t\thk_draw_band(0x0000FF00, 230, 10);
\t\t\tfor (;;) {
\t\t\t\t__asm__ volatile ("wfe");
\t\t\t}
\t\t}
\t} else if (!is_user) {
\t\thk_draw_band(0x00FF00FF, 210, 10); /* Magenta band: kernel fault/panic */
\t\tstatic char fmsg[512];
\t\tint n = snprintf(fmsg, sizeof(fmsg),
\t\t\t"=== HAKIOS KERNEL FAULT ===\\n"
\t\t\t"class: 0x%x\\n"
\t\t\t"pc   : 0x%llx\\n"
\t\t\t"lr   : 0x%llx\\n"
\t\t\t"sp   : 0x%llx\\n"
\t\t\t"esr  : 0x%llx\\n"
\t\t\t"far  : 0x%lx\\n"
\t\t\t"x0   : 0x%llx\\n",
\t\t\t(unsigned int)class,
\t\t\t(uint64_t)get_saved_state_pc(state),
\t\t\t(uint64_t)get_saved_state_lr(state),
\t\t\t(uint64_t)get_saved_state_sp(state),
\t\t\tesr, (unsigned long)far,
\t\t\t(uint64_t)get_saved_state_reg(state, 0));
\t\tfor (uintptr_t p = (uintptr_t)fmsg; p < (uintptr_t)fmsg + n + 64; p += 64) {
\t\t\t__asm__ volatile ("dc cvac, %0" : : "r" (p));
\t\t\t}
\t\t__asm__ volatile ("dsb sy");
\t\thk_render_screen_asm(fmsg, n, iso_font);
\t\thk_draw_band(0x0000FF00, 230, 10);
\t\tfor (;;) {
\t\t\t__asm__ volatile ("wfe");
\t\t}
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

\t/* Keep callers from writing beyond the 2400-row framebuffer. */
\tmov\t\tx22, #2400
\tcmp\t\tx1, x22
\tb.hs\t5f
\tsub\t\tx22, x22, x1
\tcmp\t\tx2, x22
\tcsel\t\tx2, x2, x22, lo
\tcbz\t\tx2, 5f

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

5:
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

\t// Clean D-cache for text buffer to Point of Coherency before MMU is disabled
\tmov\t\tx10, x19
\tmov\t\tw12, w20
11:\tdc\t\tcvac, x10
\tadd\t\tx10, x10, #64
\tsubs\tw12, w12, #64
\tb.gt\t11b
\tdsb\t\tsy

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
\tmov\t\tx26, #720

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

\tmov\t\tw6, #0
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
63:\tadd\t\tw6, w6, #1
\tcmp\t\tw6, #8
\tb.lt\t61b

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

    # 9. osfmk/arm64/loader_return.c (render panic text directly to screen at entry of loader_return)
    p9 = os.path.join(src, "osfmk", "arm64", "loader_return.c")
    if os.path.isfile(p9):
        with open(p9, "r", encoding="utf-8", errors="replace") as f:
            c9 = f.read().replace("\r\n", "\n")
        lr_old = "void\nloader_return(void)\n{\n\tredwood_ufs_close();"
        lr_new = """void
loader_return(void)
{
\textern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
\thk_draw_band(0x00FF8C00, 220, 10); /* Dark Orange band: loader_return / panic */

\t/* hakios: on-screen panic dump directly to display BEFORE any quiesce */
\t{
\t\textern void hk_render_screen_asm(const char *str, uint32_t len, const unsigned char *font);
\t\textern const unsigned char iso_font[];
\t\textern char *debug_buf_base, *debug_buf_ptr;
\t\textern const char *debugger_panic_str;
\t\textern char loader_log[];
\t\textern uint32_t loader_log_len;
\t\tstatic char panic_onscreen[3500];
\t\tuint32_t n = 0;
\t\tconst char header[] = "=== HAKIOS PANIC CONSOLE ===\\n";
\t\tfor (const char *h = header; *h && n < 100; h++) {
\t\t\tpanic_onscreen[n++] = *h;
\t\t}

\t\tif (debug_buf_base != NULL && debug_buf_ptr > debug_buf_base) {
\t\t\tconst char *p = debug_buf_base;
\t\t\twhile (p < debug_buf_ptr && n < 3400) {
\t\t\t\tpanic_onscreen[n++] = *p++;
\t\t\t}
\t\t} else if (debugger_panic_str != NULL) {
\t\t\tconst char *p = debugger_panic_str;
\t\t\twhile (*p && n < 3400) {
\t\t\t\tpanic_onscreen[n++] = *p++;
\t\t\t}
\t\t} else if (loader_log_len > 0) {
\t\t\tuint32_t start = (loader_log_len > 3000) ? (loader_log_len - 3000) : 0;
\t\t\twhile (start < loader_log_len && n < 3400) {
\t\t\t\tpanic_onscreen[n++] = loader_log[start++];
\t\t\t}
\t\t} else {
\t\t\tconst char *msg = "loader_return() entered, but log buffers empty.\\n";
\t\t\twhile (*msg && n < 3400) {
\t\t\t\tpanic_onscreen[n++] = *msg++;
\t\t\t}
\t\t}

\t\tpanic_onscreen[n] = '\\0';

\t\t/* Clean data cache to DRAM before MMU-off read */
\t\tfor (uintptr_t p = (uintptr_t)panic_onscreen; p < (uintptr_t)panic_onscreen + n + 64; p += 64) {
\t\t\t__asm__ volatile ("dc cvac, %0" : : "r" (p));
\t\t}
\t\t__asm__ volatile ("dsb sy");

\t\thk_render_screen_asm(panic_onscreen, n, iso_font);
\t\thk_draw_band(0x0000FF00, 230, 10); /* Lime band: screen dump completed successfully */

\t\t/* Spin forever so screen remains visible */
\t\tfor (;;) {
\t\t\t__asm__ volatile ("wfe");
\t\t}
\t}

\tredwood_ufs_close();"""
        if lr_old in c9:
            c9 = c9.replace(lr_old, lr_new, 1)
            with open(p9, "w", encoding="utf-8", newline="\n") as f:
                f.write(c9)
            print("  [OK] osfmk/arm64/loader_return.c: early on-screen panic dump installed")
        else:
            print("  [WARN] loader_return target not found in loader_return.c")

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
\t\t/* hakios: visual progress ladder for startup subsystem */
\t\t{
\t\t\textern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
\t\t\t/* Draw 2-pixel band in Lime Green (0x0000FF00) when subsystem starts */
\t\t\thk_draw_band(0x0000FF00, 188 + ((cur->subsystem & 0x3f) * 2), 2);
\t\t}
\t\tcur->func(cur->arg);
\t\t{
\t\t\textern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
\t\t\t/* Turn Cyan (0x0000FFFF) when subsystem completes successfully */
\t\t\thk_draw_band(0x0000FFFF, 188 + ((cur->subsystem & 0x3f) * 2), 2);
\t\t}"""
        if t_startup in c10:
            c10 = c10.replace(t_startup, r_startup, 1)
            print("  [OK] osfmk/kern/startup.c: startup subsystem visual trace installed")
        else:
            print("  [WARN] t_startup not found in startup.c")

        # Instrument kernel_bootstrap(void) entry
        m_kb = re.search(r'(void\s+kernel_bootstrap\s*\(\s*void\s*\)\s*\{)', c10)
        if m_kb:
            c10 = c10.replace(m_kb.group(0), m_kb.group(0) + """
\t{
\t\textern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
\t\thk_draw_band(0x0000FFFF, 184, 2); /* Cyan band at row 184-185: entered kernel_bootstrap */
\t}""", 1)
            print("  [OK] osfmk/kern/startup.c: kernel_bootstrap visual marker installed")
        else:
            print("  [WARN] kernel_bootstrap not found in startup.c")

        # Instrument kernel_bootstrap_thread(void) entry
        m_kbt = re.search(r'(void\s+kernel_bootstrap_thread\s*\(\s*void\s*\)\s*\{)', c10)
        if m_kbt:
            c10 = c10.replace(m_kbt.group(0), m_kbt.group(0) + """
\t{
\t\textern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
\t\thk_draw_band(0x00FF1493, 186, 2); /* Hot Pink band at row 186-187: entered kernel_bootstrap_thread */
\t}""", 1)
            print("  [OK] osfmk/kern/startup.c: kernel_bootstrap_thread visual marker installed")
        else:
            print("  [WARN] kernel_bootstrap_thread not found in startup.c")

        # Instrument kernel_bootstrap_log
        m_log = re.search(r'(kernel_bootstrap_log\s*\(\s*const\s+char\s*\*\s*\w+\s*\)\s*\{)', c10)
        if m_log:
            c10 = c10.replace(m_log.group(0), m_log.group(0) + """
\t{
\t\textern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
\t\tstatic uint32_t hk_bstep = 0;
\t\thk_draw_band(0x00FF8C00, 240 + ((hk_bstep++ & 0x3f) * 2), 2); /* Dark Orange line for each milestone */
\t}""", 1)
            print("  [OK] osfmk/kern/startup.c: kernel_bootstrap_log visual milestone ladder installed")
        else:
            print("  [WARN] kernel_bootstrap_log not found in startup.c")

        with open(p10, "w", encoding="utf-8", newline="\n") as f:
            f.write(c10)


    # 11. osfmk/ipc/ipc_policy.c (bypass validation panic on generic ARM bring-up)
    p11 = os.path.join(src, "osfmk", "ipc", "ipc_policy.c")
    if os.path.isfile(p11):
        with open(p11, "r", encoding="utf-8", errors="replace") as f:
            c11 = f.read()
        target_val = "ipc_sec_policy_configs_validate(void)\n{"
        replace_val = "ipc_sec_policy_configs_validate(void)\n{\n\treturn; /* hakios: bypass dev validation */"
        if target_val in c11:
            c11 = c11.replace(target_val, replace_val, 1)
            with open(p11, "w", encoding="utf-8", newline="\n") as f:
                f.write(c11)
            print("  [OK] osfmk/ipc/ipc_policy.c: ipc_sec_policy_configs_validate bypassed")
        else:
            print("  [WARN] ipc_sec_policy_configs_validate target not found in ipc_policy.c")

    # 12. osfmk/prng/prng_random.c (relax entropy seed check for bootloaders passing < 256 bytes)
    p12 = os.path.join(src, "osfmk", "prng", "prng_random.c")
    if os.path.isfile(p12):
        with open(p12, "r", encoding="utf-8", errors="replace") as f:
            c12 = f.read()
        target_seed = "Expected %u seed bytes from boot loader"
        if target_seed in c12:
            c12 = re.sub(r'panic\s*\(\s*"Expected %u seed bytes from boot loader',
                         r'printf("HAKIOS: Expected %u seed bytes from boot loader', c12)
            with open(p12, "w", encoding="utf-8", newline="\n") as f:
                f.write(c12)
            print("  [OK] osfmk/prng/prng_random.c: relaxed seed length panic to warning")
        else:
            print("  [WARN] Expected %u seed bytes target not found in prng_random.c")

# 13. osfmk/kern/debug.c (on-screen panic dump directly from panic_trap_to_debugger)
    p13 = os.path.join(src, "osfmk", "kern", "debug.c")
    if os.path.isfile(p13):
        with open(p13, "r", encoding="utf-8", errors="replace") as f:
            c13 = f.read().replace("\r\n", "\n")
        target_dbg = 'loader_log_line("\\n== PANIC: ");\n\t\tloader_log_line(rbuf);\n\t\tloader_log_line(" ==\\n");\n\t}'
        replace_dbg = '''loader_log_line("\\n== PANIC: ");
\t\tloader_log_line(rbuf);
\t\tloader_log_line(" ==\\n");
\t\t/* hakios: on-screen panic dump directly to display */
\t\t{
\t\t\textern void hk_render_screen_asm(const char *, uint32_t, const unsigned char *);
\t\t\textern const unsigned char iso_font[];
\t\t\tstatic char hk_pmsg[400];
\t\t\tint pn = snprintf(hk_pmsg, sizeof(hk_pmsg), "=== HAKIOS KERNEL PANIC ===\\n%s\\n", rbuf);
\t\t\tfor (uintptr_t p = (uintptr_t)hk_pmsg; p < (uintptr_t)hk_pmsg + pn + 64; p += 64) {
\t\t\t\t__asm__ volatile ("dc cvac, %0" : : "r" (p));
\t\t\t}
\t\t\t__asm__ volatile ("dsb sy");
\t\t\thk_render_screen_asm(hk_pmsg, pn, iso_font);
\t\t\tfor (;;) {
\t\t\t\t__asm__ volatile ("wfe");
\t\t\t}
\t\t}
\t}'''
        if target_dbg in c13:
            c13 = c13.replace(target_dbg, replace_dbg, 1)
            print("  [OK] osfmk/kern/debug.c: on-screen panic dump installed in panic_trap_to_debugger")
        else:
            print("  [WARN] target_dbg not found in debug.c")

        target_early_panic = 'loader_log_line("\\n== EARLY PANIC: ");\n\t\t\tloader_log_line(buf);\n\t\t\tloader_log_line(" ==\\n");\n\t\t}'
        replace_early_panic = '''loader_log_line("\\n== EARLY PANIC: ");
\t\t\tloader_log_line(buf);
\t\t\tloader_log_line(" ==\\n");
\t\t\t/* hakios: on-screen early panic dump directly to display */
\t\t\t{
\t\t\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\t\t\thk_draw_band(0x00FF0000, 210, 10); /* Bright Red band: early panic */
\t\t\t\textern void hk_render_screen_asm(const char *, uint32_t, const unsigned char *);
\t\t\t\textern const unsigned char iso_font[];
\t\t\t\tstatic char hk_epmsg[400];
\t\t\t\tint epn = snprintf(hk_epmsg, sizeof(hk_epmsg), "=== EARLY PANIC ===\\n%s\\n", buf);
\t\t\t\tfor (uintptr_t p = (uintptr_t)hk_epmsg; p < (uintptr_t)hk_epmsg + epn + 64; p += 64) {
\t\t\t\t\t__asm__ volatile ("dc cvac, %0" : : "r" (p));
\t\t\t\t}
\t\t\t\t__asm__ volatile ("dsb sy");
\t\t\t\thk_render_screen_asm(hk_epmsg, epn, iso_font);
\t\t\t\tfor (;;) {
\t\t\t\t\t__asm__ volatile ("wfe");
\t\t\t\t}
\t\t\t}
\t\t}'''
        if target_early_panic in c13:
            c13 = c13.replace(target_early_panic, replace_early_panic, 1)
            print("  [OK] osfmk/kern/debug.c: on-screen early panic dump installed")
        else:
            print("  [WARN] target_early_panic not found in debug.c")

        with open(p13, "w", encoding="utf-8", newline="\n") as f:
            f.write(c13)

    # 14. osfmk/arm64/pac_decode.h & pac_patch.c (ARMv8.3/ARMv8.4 RCpc LDAPR/LDAPUR/STLUR trap self-patch engine)
    p14_h = os.path.join(src, "osfmk", "arm64", "pac_decode.h")
    if os.path.isfile(p14_h):
        with open(p14_h, "r", encoding="utf-8", errors="replace") as f:
            c14_h = f.read().replace("\r\n", "\n")
        if "ARMv8.3-A RCpc" not in c14_h:
            t14_h = "    return PAC_NOT_PAC;\n}"
            r14_h = """    /* ---- 8. LDAPR / LDAPRB / LDAPRH (ARMv8.3-A RCpc) -> LDAR / LDARB / LDARH (ARMv8.0) ---- */
    if ((i & 0x3ffffc00u) == 0x38bfc000u) {
        unsigned sz = (i >> 30) & 3, rn = (i >> 5) & 31, rt = i & 31;
        uint32_t ldar = ((uint32_t)sz << 30) | 0x08dffc00u | ((uint32_t)rn << 5) | (uint32_t)rt;
        return pac__patch(r, "ldapr", ldar);
    }

    /* ---- 9. LDAPUR (ARMv8.4-A RCpc2) -> LDUR (ARMv8.0) ----------------------------------- */
    if ((i & 0x3fc00c00u) == 0x19400000u) {
        uint32_t ldur = (i & ~0x3fc00000u) | 0x38400000u;
        return pac__patch(r, "ldapur", ldur);
    }

    /* ---- 10. STLUR (ARMv8.4-A RCpc2) -> STUR (ARMv8.0) ---------------------------------- */
    if ((i & 0x3fc00c00u) == 0x19000000u) {
        uint32_t stur = (i & ~0x3fc00000u) | 0x38000000u;
        return pac__patch(r, "stlur", stur);
    }

    return PAC_NOT_PAC;
}"""
            if t14_h in c14_h:
                c14_h = c14_h.replace(t14_h, r14_h, 1)
                with open(p14_h, "w", encoding="utf-8", newline="\n") as f:
                    f.write(c14_h)
                print("  [OK] osfmk/arm64/pac_decode.h: RCpc LDAPR/LDAPUR/STLUR decode installed")
            else:
                print("  [WARN] t14_h not found in pac_decode.h")
        else:
            print("  [OK] osfmk/arm64/pac_decode.h: RCpc already present")

    p14_c = os.path.join(src, "osfmk", "arm64", "pac_patch.c")
    if os.path.isfile(p14_c):
        with open(p14_c, "r", encoding="utf-8", errors="replace") as f:
            c14_c = f.read().replace("\r\n", "\n")
        if "0x08dffc00u" not in c14_c:
            smp_old = "(insn & 0xffe00000u) == 0xf8400000u) {"
            smp_new = """(insn & 0xffe00000u) == 0xf8400000u ||
\t    (insn & 0x3ffffc00u) == 0x08dffc00u || /* LDAR (ARMv8.0) */
\t    (insn & 0x3fc00c00u) == 0x38400000u || /* LDUR (ARMv8.0) */
\t    (insn & 0x3fc00c00u) == 0x38000000u) { /* STUR (ARMv8.0) */"""
            if smp_old in c14_c:
                c14_c = c14_c.replace(smp_old, smp_new, 1)
                with open(p14_c, "w", encoding="utf-8", newline="\n") as f:
                    f.write(c14_c)
                print("  [OK] osfmk/arm64/pac_patch.c: RCpc SMP race defense installed")
            else:
                print("  [WARN] smp_old not found in pac_patch.c")
        else:
            print("  [OK] osfmk/arm64/pac_patch.c: RCpc SMP race already present")

    # 15. bsd/kern/kdebug.c (visual trace on all early debug milestone strings)
    p15 = os.path.join(src, "bsd", "kern", "kdebug.c")
    if os.path.isfile(p15):
        with open(p15, "r", encoding="utf-8", errors="replace") as f:
            c15 = f.read().replace("\r\n", "\n")
        t15 = "void\nkernel_debug_string_early(const char *message)\n{"
        r15 = """void
kernel_debug_string_early(const char *message)
{
\t{
\t\textern void hk_draw_band(uint32_t color, uint32_t row_start, uint32_t row_count);
\t\tstatic uint32_t hk_dbg_step = 0;
\t\tconst uint32_t colors[4] = { 0x00FFD700, 0x0000FFFF, 0x00FF00FF, 0x00FFFF00 }; /* Gold, Cyan, Magenta, Yellow */
\t\thk_draw_band(colors[hk_dbg_step & 3], 244 + ((hk_dbg_step & 0x3f) * 4), 3);
\t\thk_dbg_step++;
\t}"""
        if t15 in c15:
            c15 = c15.replace(t15, r15, 1)
            with open(p15, "w", encoding="utf-8", newline="\n") as f:
                f.write(c15)
            print("  [OK] bsd/kern/kdebug.c: kernel_debug_string_early visual progress installed")
        else:
            print("  [WARN] t15 not found in kdebug.c")

    # 16. osfmk/vm/vm_resident.c (visual trace for vm_page_bootstrap and pmap_startup)
    p16 = os.path.join(src, "osfmk", "vm", "vm_resident.c")
    if os.path.isfile(p16):
        with open(p16, "r", encoding="utf-8", errors="replace") as f:
            c16 = f.read().replace("\r\n", "\n")
        
        # Entry of vm_page_bootstrap
        t16_entry = "vm_page_bootstrap(\n\tvm_offset_t             *startp,\n\tvm_offset_t             *endp)\n{"
        r16_entry = """vm_page_bootstrap(
\tvm_offset_t             *startp,
\tvm_offset_t             *endp)
{
\t{
\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\thk_draw_band(0x00FF7F50, 288, 2); /* Coral: vm_page_bootstrap entered */
\t}"""
        if t16_entry in c16:
            c16 = c16.replace(t16_entry, r16_entry, 1)
            print("  [OK] osfmk/vm/vm_resident.c: vm_page_bootstrap entry marker installed")
        else:
            print("  [WARN] t16_entry not found in vm_resident.c")

        # After pmap_startup call in vm_page_bootstrap
        t16_pms_call = '\tkernel_debug_string_early("pmap_startup");\n\tpmap_startup(&virtual_space_start, &virtual_space_end);'
        r16_pms_call = """\tkernel_debug_string_early("pmap_startup");
\tpmap_startup(&virtual_space_start, &virtual_space_end);
\t{
\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\thk_draw_band(0x0032CD32, 292, 2); /* Lime: pmap_startup returned to vm_page_bootstrap */
\t}"""
        if t16_pms_call in c16:
            c16 = c16.replace(t16_pms_call, r16_pms_call, 1)
            print("  [OK] osfmk/vm/vm_resident.c: pmap_startup call return marker installed")
        else:
            print("  [WARN] t16_pms_call not found in vm_resident.c")

        # Entry of pmap_startup
        t16_pms_def = "pmap_startup(\n\tvm_offset_t     *startp,\n\tvm_offset_t     *endp)\n{"
        r16_pms_def = """pmap_startup(
\tvm_offset_t     *startp,
\tvm_offset_t     *endp)
{
\t{
\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\thk_draw_band(0x00FF8C00, 296, 2); /* Dark Orange: pmap_startup entered */
\t}"""
        if t16_pms_def in c16:
            c16 = c16.replace(t16_pms_def, r16_pms_def, 1)
            print("  [OK] osfmk/vm/vm_resident.c: pmap_startup entry marker installed")
        else:
            print("  [WARN] t16_pms_def not found in vm_resident.c")

        # In pmap_steal_memory_internal: loop start and progress
        t16_steal = '\tmap_addr = round_page(addr);\n\tif (os_add_overflow(addr, size, &end)) {\n\t\tpanic("pmap_steal_memory() overflow, addr: %lx, size: 0x%lx", addr, size);\n\t}\n\twhile (map_addr < end) {'
        r16_steal = """\tmap_addr = round_page(addr);
\tif (os_add_overflow(addr, size, &end)) {
\t\tpanic("pmap_steal_memory() overflow, addr: %lx, size: 0x%lx", addr, size);
\t}
\t{
\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\thk_draw_band(0x00FF1493, 264, 2); /* Deep Pink: pmap_steal_memory_internal loop entered */
\t}
\twhile (map_addr < end) {
\t\tif ((map_addr & 0x000FFFFF) == 0) {
\t\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\t\tuint32_t mb = (uint32_t)((map_addr - addr) >> 20);
\t\t\tuint32_t col = (mb & 1) ? 0x0000FFFF : 0x00FF00FF;
\t\t\thk_draw_band(col, 268 + ((mb & 0x7) * 2), 2);
\t\t}"""
        if t16_steal in c16:
            c16 = c16.replace(t16_steal, r16_steal, 1)
            print("  [OK] osfmk/vm/vm_resident.c: pmap_steal loop progress markers installed")
        else:
            print("  [WARN] t16_steal not found in vm_resident.c")

        # After vm_pages steal in pmap_startup
        t16_vmp = '\tvm_pages     = pmap_steal_freeable_memory(npages * sizeof(struct vm_page));\n\tvm_pages_end = vm_page_get(npages);'
        r16_vmp = """\tvm_pages     = pmap_steal_freeable_memory(npages * sizeof(struct vm_page));
\t{
\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\thk_draw_band(0x0000FF00, 284, 2); /* Lime: vm_pages stolen successfully */
\t}
\tvm_pages_end = vm_page_get(npages);
\t{
\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\thk_draw_band(0x00FF8C00, 288, 2); /* Dark Orange: vm_pages_end computed */
\t}"""
        if t16_vmp in c16:
            c16 = c16.replace(t16_vmp, r16_vmp, 1)
            print("  [OK] osfmk/vm/vm_resident.c: vm_pages return markers installed")
        else:
            print("  [WARN] t16_vmp not found in vm_resident.c")

        # Page init loop in pmap_startup
        t16_loop = '\tvm_free_page_lock();\n\n\tfor (uint32_t i = 0; i < npages && pmap_next_page(&phys_page); i++) {'
        r16_loop = """\tvm_free_page_lock();
\t{
\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\thk_draw_band(0x0087CEEB, 300, 2); /* Sky Blue: before page init loop */
\t}
\tfor (uint32_t i = 0; i < npages && pmap_next_page(&phys_page); i++) {
\t\tif (i == 0) {
\t\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band(0x004682B4, 304, 2); /* Steel Blue: page loop started */
\t\t}
\t\tif (i == (npages >> 1)) {
\t\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band(0x001E90FF, 308, 2); /* Dodger Blue: page loop 50% */
\t\t}"""
        if t16_loop in c16:
            c16 = c16.replace(t16_loop, r16_loop, 1)
            print("  [OK] osfmk/vm/vm_resident.c: page init loop markers installed")
        else:
            print("  [WARN] t16_loop not found in vm_resident.c")

        with open(p16, "w", encoding="utf-8", newline="\n") as f:
            f.write(c16)

    # 17. osfmk/vm/vm_init.c (visual trace for vm_mem_bootstrap)
    p17 = os.path.join(src, "osfmk", "vm", "vm_init.c")
    if os.path.isfile(p17):
        with open(p17, "r", encoding="utf-8", errors="replace") as f:
            c17 = f.read().replace("\r\n", "\n")
        t17 = '\tvm_mem_bootstrap_log("vm_page_bootstrap");\n\tvm_page_bootstrap(&start, &end);'
        r17 = """\tvm_mem_bootstrap_log("vm_page_bootstrap");
\tvm_page_bootstrap(&start, &end);
\t{
\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\thk_draw_band(0x0000FF00, 316, 2); /* Lime Green: vm_page_bootstrap returned */
\t}"""
        if t17 in c17:
            c17 = c17.replace(t17, r17, 1)
            c17 = c17.replace('zone_bootstrap();', 'zone_bootstrap();\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x0000FFFF, 320, 2); }', 1)
            c17 = c17.replace('vm_object_bootstrap();', 'vm_object_bootstrap();\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FF1493, 324, 2); }', 1)
            c17 = c17.replace('vm_retire_boot_pages();', 'vm_retire_boot_pages();\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FFFF00, 328, 2); }', 1)
            c17 = c17.replace('vm_map_init();', 'vm_map_init();\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x009400D3, 332, 2); }', 1)
            c17 = c17.replace('kmem_init(start, end);', 'kmem_init(start, end);\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x0000BFFF, 336, 2); }', 1)
            c17 = c17.replace('kernel_startup_initialize_upto(STARTUP_SUB_KMEM);', 'kernel_startup_initialize_upto(STARTUP_SUB_KMEM);\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FFFFFF, 340, 2); }', 1)
            c17 = c17.replace('vm_fault_init();', 'vm_fault_init();\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FFA500, 344, 2); }', 1)
            c17 = c17.replace('kernel_startup_initialize_upto(STARTUP_SUB_ZALLOC);', 'kernel_startup_initialize_upto(STARTUP_SUB_ZALLOC);\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x007FFF00, 348, 2); }', 1)
            with open(p17, "w", encoding="utf-8", newline="\n") as f:
                f.write(c17)
            print("  [OK] osfmk/vm/vm_init.c: vm_mem_bootstrap milestone bands installed")
        else:
            print("  [WARN] t17 not found in vm_init.c")

    # 18. osfmk/arm/pmap/pmap_data.c (early boot page table allocation fallback when !pmap_initialized)
    p18 = os.path.join(src, "osfmk", "arm", "pmap", "pmap_data.c")
    if os.path.isfile(p18):
        with open(p18, "r", encoding="utf-8", errors="replace") as f:
            c18 = f.read().replace("\r\n", "\n")
        t18 = '#else /* XNU_MONITOR */\n\tvm_page_t mem = VM_PAGE_NULL;'
        r18 = """#else /* XNU_MONITOR */
\textern boolean_t pmap_initialized;
\tif (!pmap_initialized) {
\t\textern boolean_t pmap_next_page(ppnum_t *);
\t\tppnum_t pn;
\t\tif (pmap_next_page(&pn)) {
\t\t\t*pa = (pmap_paddr_t)ptoa(pn);
\t\t\tbzero((void *)phystokv(*pa), size);
\t\t\treturn KERN_SUCCESS;
\t\t}
\t\treturn KERN_RESOURCE_SHORTAGE;
\t}
\tvm_page_t mem = VM_PAGE_NULL;"""
        if t18 in c18:
            c18 = c18.replace(t18, r18, 1)
            with open(p18, "w", encoding="utf-8", newline="\n") as f:
                f.write(c18)
            print("  [OK] osfmk/arm/pmap/pmap_data.c: early pmap page alloc patched")
        else:
            print("  [WARN] t18 not found in pmap_data.c")

    # 19. osfmk/arm/pmap/pmap.c (prevent VM_PAGE_WAIT hang during early boot in pmap_expand & pmap_tt_allocate)
    p19 = os.path.join(src, "osfmk", "arm", "pmap", "pmap.c")
    if os.path.isfile(p19):
        with open(p19, "r", encoding="utf-8", errors="replace") as f:
            c19 = f.read().replace("\r\n", "\n")

        t19_exp = 'while ((ret = pmap_tt_allocate(pmap, &tt_p, ttlevel + 1, ((options & PMAP_TT_ALLOCATE_NOWAIT)? PMAP_PAGES_ALLOCATE_NOWAIT : 0))) != KERN_SUCCESS) {\n\t\t\t\tif (options & PMAP_OPTIONS_NOWAIT) {\n\t\t\t\t\t/* Can be KERN_RESOURCE_SHORTAGE or KERN_ABORTED. */\n\t\t\t\t\treturn ret;\n\t\t\t\t}'
        r19_exp = """while ((ret = pmap_tt_allocate(pmap, &tt_p, ttlevel + 1, ((options & PMAP_TT_ALLOCATE_NOWAIT)? PMAP_PAGES_ALLOCATE_NOWAIT : 0))) != KERN_SUCCESS) {
\t\t\t\tif (options & PMAP_OPTIONS_NOWAIT) {
\t\t\t\t\t/* Can be KERN_RESOURCE_SHORTAGE or KERN_ABORTED. */
\t\t\t\t\treturn ret;
\t\t\t\t}
\t\t\t\tif (!pmap_initialized) {
\t\t\t\t\treturn ret;
\t\t\t\t}"""
        if t19_exp in c19:
            c19 = c19.replace(t19_exp, r19_exp, 1)
            print("  [OK] osfmk/arm/pmap/pmap.c: pmap_expand early return patched")
        else:
            print("  [WARN] t19_exp not found in pmap.c")

        t19_tt = 'while (pmap_pages_alloc_zeroed(&pa, PAGE_SIZE, alloc_flags) != KERN_SUCCESS) {\n\t\t\tif (options & PMAP_OPTIONS_NOWAIT) {\n\t\t\t\treturn KERN_RESOURCE_SHORTAGE;\n\t\t\t}\n\t\t\tVM_PAGE_WAIT();\n\t\t}'
        r19_tt = """while (pmap_pages_alloc_zeroed(&pa, PAGE_SIZE, alloc_flags) != KERN_SUCCESS) {
\t\t\tif (options & PMAP_OPTIONS_NOWAIT) {
\t\t\t\treturn KERN_RESOURCE_SHORTAGE;
\t\t\t}
\t\t\tif (!pmap_initialized) {
\t\t\t\treturn KERN_RESOURCE_SHORTAGE;
\t\t\t}
\t\t\tVM_PAGE_WAIT();
\t\t}"""
        if t19_tt in c19:
            c19 = c19.replace(t19_tt, r19_tt, 1)
            print("  [OK] osfmk/arm/pmap/pmap.c: pmap_tt_allocate early wait patched")
        else:
            print("  [WARN] t19_tt not found in pmap.c")

        t19_ptd = 'while ((ptdp = ptd_alloc(pmap)) == NULL) {\n\t\t\tif (options & PMAP_OPTIONS_NOWAIT) {\n\t\t\t\t/* Deallocate all allocated resources so far. */\n\t\t\t\tpmap_pages_free(pa, PAGE_SIZE);\n\t\t\t\treturn KERN_RESOURCE_SHORTAGE;\n\t\t\t}\n\t\t\tVM_PAGE_WAIT();\n\t\t}'
        r19_ptd = """while ((ptdp = ptd_alloc(pmap)) == NULL) {
\t\t\tif (options & PMAP_OPTIONS_NOWAIT) {
\t\t\t\t/* Deallocate all allocated resources so far. */
\t\t\t\tpmap_pages_free(pa, PAGE_SIZE);
\t\t\t\treturn KERN_RESOURCE_SHORTAGE;
\t\t\t}
\t\t\tif (!pmap_initialized) {
\t\t\t\tpmap_pages_free(pa, PAGE_SIZE);
\t\t\t\treturn KERN_RESOURCE_SHORTAGE;
\t\t\t}
\t\t\tVM_PAGE_WAIT();
\t\t}"""
        if t19_ptd in c19:
            c19 = c19.replace(t19_ptd, r19_ptd, 1)
            print("  [OK] osfmk/arm/pmap/pmap.c: ptd_alloc early wait patched")
        else:
            print("  [WARN] t19_ptd not found in pmap.c")

        with open(p19, "w", encoding="utf-8", newline="\n") as f:
            f.write(c19)

    # 20. osfmk/arm64/arm_vm_init.c (ensure valid L2 table allocated for *cpu_l1_tte and expand heap coverage)
    p20 = os.path.join(src, "osfmk", "arm64", "arm_vm_init.c")
    if os.path.isfile(p20):
        with open(p20, "r", encoding="utf-8", errors="replace") as f:
            c20 = f.read().replace("\r\n", "\n")

        # 20a. Allocate L2 table on demand if *cpu_l1_tte is EMPTY in bootstrap L3 loop
        t20_l1 = 'cpu_l1_tte = cpu_tte + L1_TABLE_T1_INDEX(va_l1, TCR_EL1_BOOT);\n\n\twhile (va_l1 < va_l1_end) {\n\t\tva_l2 = va_l1;'
        r20_l1 = """cpu_l1_tte = cpu_tte + L1_TABLE_T1_INDEX(va_l1, TCR_EL1_BOOT);

\twhile (va_l1 < va_l1_end) {
\t\tif (*cpu_l1_tte == ARM_TTE_EMPTY) {
\t\t\tvm_offset_t l2_page = alloc_ptpage(FALSE);
\t\t\t*cpu_l1_tte = (kvtophys(l2_page) & ARM_TTE_TABLE_MASK) | ARM_TTE_TYPE_TABLE | ARM_TTE_VALID | ARM_DYNAMIC_TABLE_XN | ARM_TTE_TABLE_AP(ARM_TTE_TABLE_AP_USER_NA);
\t\t\tbzero((void *)l2_page, ARM_PGBYTES);
\t\t}
\t\tva_l2 = va_l1;"""
        if t20_l1 in c20:
            c20 = c20.replace(t20_l1, r20_l1, 1)
            print("  [OK] osfmk/arm64/arm_vm_init.c: on-demand L2 table allocation in bootstrap L3 loop installed")
        else:
            print("  [WARN] t20_l1 not found in arm_vm_init.c")

        # 20b. Expand va_l1_end to pre-cover 64MB of heap
        t20_end = '\tva_l1_end = va_l1 + ((2 + (mem_segments * 10)) << 20);'
        r20_end = '\tva_l1_end = va_l1 + ((64 + (mem_segments * 10)) << 20);'
        if t20_end in c20:
            c20 = c20.replace(t20_end, r20_end, 1)
            print("  [OK] osfmk/arm64/arm_vm_init.c: expanded bootstrap heap range from 42MB to 104MB")
        else:
            print("  [WARN] t20_end not found in arm_vm_init.c")

        with open(p20, "w", encoding="utf-8", newline="\n") as f:
            f.write(c20)


    # 21. osfmk/console/video_console.c (reserve rows 0..719 for diagnostic ladder, start console text at row 720)
    p21 = os.path.join(src, "osfmk", "console", "video_console.c")
    if os.path.isfile(p21):
        with open(p21, "r", encoding="utf-8", errors="replace") as f:
            c21 = f.read().replace("\r\n", "\n")
        vc_target = "new_vinfo.v_baseaddr = newVideoVirt + boot_vinfo->v_offset;"
        vc_repl = (
            "new_vinfo.v_baseaddr = newVideoVirt + boot_vinfo->v_offset;\n"
            "\t\t\t/* hakios: reserve rows 0..719 for diagnostic ladder bands, clamp console to row 720..2399 */\n"
            "\t\t\tif (new_vinfo.v_height > 720) {\n"
            "\t\t\t\tnew_vinfo.v_baseaddr += 720 * new_vinfo.v_rowbytes;\n"
            "\t\t\t\tnew_vinfo.v_height -= 720;\n"
            "\t\t\t}"
        )
        if "/* hakios: reserve rows 0..719 for diagnostic ladder bands" in c21:
            print("  [ALREADY PATCHED] osfmk/console/video_console.c: dual-region framebuffer partition already present")
        elif vc_target in c21:
            c21 = c21.replace(vc_target, vc_repl, 1)
            with open(p21, "w", encoding="utf-8", newline="\n") as f:
                f.write(c21)
            print("  [OK] osfmk/console/video_console.c: dual-region framebuffer partition installed (rows 0..719 ladder, rows 720..2399 console)")
        else:
            print("  [WARN] vc_target not found in video_console.c")

    print("Cortex-A73 patches successfully applied!")

if __name__ == "__main__":
    main()
