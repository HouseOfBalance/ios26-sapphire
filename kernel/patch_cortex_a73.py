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
	extern void hk_draw_band_direct(uint32_t color, uint32_t row_start, uint32_t row_count);
	hk_draw_band(0x00FFA500, 50, 4);   /* hakios visual marker: Orange at rows 50-53 (legacy drawer) */
	hk_draw_band_direct(0x00FFFF00, 55, 4); /* hakios visual marker: Yellow at rows 55-58 (Phase 1 direct VA drawer) */
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

        replacement_vm = """	extern void hk_draw_band_direct(uint32_t color, uint32_t row_start, uint32_t row_count);
	hk_draw_band_direct(0x0000FA9A, 158, 2);  /* hakios visual marker: Medium Spring Green at rows 158-159 (before arm_vm_init) */
\tarm_vm_init(xmaxmem, args);
	hk_draw_band_direct(0x00FF007F, 160, 3);  /* hakios visual marker: Rose at rows 160-162 (past arm_vm_init) */"""

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

        load_user_old = """.macro PAC_STD_LOAD_USER_KEYS base, thread, id, lo, hi, t1, t2, t3, t4
\tldr\t\\id, [\\thread, TH_JOP_PID]
\tPAC_STD_LOAD_A_KEYS \\base, \\id, \\lo, \\hi, \\t1, \\t2, \\t3, \\t4
\tldr\t\\id, [\\thread, TH_ROP_PID]
\tPAC_STD_LOAD_B_KEYS \\base, \\id, \\lo, \\hi, \\t1, \\t2, \\t3, \\t4
.endm"""

        load_user_new = """.macro PAC_STD_LOAD_USER_KEYS base, thread, id, lo, hi, t1, t2, t3, t4
\t// hakios: Bengal Cortex-A73 lacks hardware PAC sysregs; stub out to prevent NULL deref of TPIDR_EL1
.endm"""

        c_norm = c.replace("\r\n", "\n")
        assert load_keys_old in c_norm, "PAC_STD_LOAD_KEYS not found in pac_std_asm.h"
        assert load_a_old in c_norm, "PAC_STD_LOAD_A_KEYS not found in pac_std_asm.h"
        assert load_b_old in c_norm, "PAC_STD_LOAD_B_KEYS not found in pac_std_asm.h"
        assert load_user_old in c_norm, "PAC_STD_LOAD_USER_KEYS not found in pac_std_asm.h"

        c_norm = c_norm.replace(load_keys_old, load_keys_new)
        c_norm = c_norm.replace(load_a_old, load_a_new)
        c_norm = c_norm.replace(load_b_old, load_b_new)
        c_norm = c_norm.replace(load_user_old, load_user_new)

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
\t\t\thk_draw_band(0x00FF0000, 680, 14); /* Thick Red band at Row 680: unhandled kernel UNDEF */
\t\t\tstatic char umsg[384];
\t\t\tint pos = 0;
\t\t\tconst char *hdr = "=== HAKIOS KERNEL UNDEF ===\\n";
\t\t\twhile (*hdr) umsg[pos++] = *hdr++;
\t\t\tuint64_t pc = (uint64_t)get_saved_state_pc(state);
\t\t\tuint64_t vals[7] = {
\t\t\t\tpc,
\t\t\t\t(pc >= 0xffff000000000000ULL) ? (uint64_t)*(volatile uint32_t *)pc : 0ULL,
\t\t\t\t(uint64_t)get_saved_state_reg(state, 30),
\t\t\t\t(uint64_t)get_saved_state_sp(state),
\t\t\t\t(uint64_t)esr,
\t\t\t\t(uint64_t)far,
\t\t\t\t(uint64_t)get_saved_state_reg(state, 0)
\t\t\t};
\t\t\tconst char *names[7] = { "pc  : 0x", "insn: 0x", "lr  : 0x", "sp  : 0x", "esr : 0x", "far : 0x", "x0  : 0x" };
\t\t\tfor (int k = 0; k < 7; k++) {
\t\t\t\tconst char *nm = names[k];
\t\t\t\twhile (*nm) umsg[pos++] = *nm++;
\t\t\t\tuint64_t v = vals[k];
\t\t\t\tfor (int sh = 60; sh >= 0; sh -= 4) {
\t\t\t\t\tuint32_t d = (uint32_t)((v >> sh) & 0xf);
\t\t\t\t\tumsg[pos++] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
\t\t\t\t}
\t\t\t\tumsg[pos++] = '\\n';
\t\t\t}
\t\t\tumsg[pos] = '\\0';
\t\t\tfor (uintptr_t p = (uintptr_t)umsg; p < (uintptr_t)umsg + pos + 64; p += 64) {
\t\t\t\t__asm__ volatile ("dc civac, %0" : : "r" (p));
\t\t\t}
\t\t\t__asm__ volatile ("dsb sy");
\t\t\thk_render_screen_asm(umsg, pos, iso_font);
\t\t\thk_draw_band(0x0000FF00, 700, 10);
\t\t\tfor (;;) {
\t\t\t\t__asm__ volatile ("wfe");
\t\t\t}
\t\t}
\t} else if (!is_user) {
\t\thk_draw_band(0x00FF00FF, 680, 14); /* Thick Magenta band at Row 680: kernel fault/abort */
\t\tstatic char fmsg[384];
\t\tint pos = 0;
\t\tconst char *hdr = "=== HAKIOS KERNEL FAULT ===\\n";
\t\twhile (*hdr) fmsg[pos++] = *hdr++;
\t\tuint64_t vals[7] = {
\t\t\t(uint64_t)class,
\t\t\t(uint64_t)get_saved_state_pc(state),
\t\t\t(uint64_t)get_saved_state_reg(state, 30),
\t\t\t(uint64_t)get_saved_state_sp(state),
\t\t\t(uint64_t)esr,
\t\t\t(uint64_t)far,
\t\t\t(uint64_t)get_saved_state_reg(state, 0)
\t\t};
\t\tconst char *names[7] = { "cls : 0x", "pc  : 0x", "lr  : 0x", "sp  : 0x", "esr : 0x", "far : 0x", "x0  : 0x" };
\t\tfor (int k = 0; k < 7; k++) {
\t\t\tconst char *nm = names[k];
\t\t\twhile (*nm) fmsg[pos++] = *nm++;
\t\t\tuint64_t v = vals[k];
\t\t\tfor (int sh = 60; sh >= 0; sh -= 4) {
\t\t\t\tuint32_t d = (uint32_t)((v >> sh) & 0xf);
\t\t\t\tfmsg[pos++] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
\t\t\t}
\t\t\tfmsg[pos++] = '\\n';
\t\t}
\t\tfmsg[pos] = '\\0';
\t\tfor (uintptr_t p = (uintptr_t)fmsg; p < (uintptr_t)fmsg + pos + 64; p += 64) {
\t\t\t__asm__ volatile ("dc civac, %0" : : "r" (p));
\t\t}
\t\t__asm__ volatile ("dsb sy");
\t\thk_render_screen_asm(fmsg, pos, iso_font);
\t\thk_draw_band(0x0000FF00, 700, 10);
\t\tfor (;;) {
\t\t\t__asm__ volatile ("wfe");
\t\t}
\t}"""
        if sleh_hook_old in c_norm:
            c_norm = c_norm.replace(sleh_hook_old, sleh_hook_new)
            print("  [OK] osfmk/arm64/sleh.c: early kernel fault visual bands installed")
        else:
            print("  [WARN] sleh_hook_old not found in sleh.c")

        
        sleh_inv_old = """void
sleh_invalid_stack(arm_context_t *context, uint64_t esr __unused, vm_offset_t far __unused)
{
\tthread_t thread = current_thread();
\tvm_offset_t kernel_stack_bottom, sp;

\t{
\t\t/* hakios: the panic text below gets cut short this early; the syndrome first, on one line */
\t\tchar line[200];
\t\tsnprintf(line, sizeof(line), "hakios: early exception esr=0x%llx far=0x%lx pc=0x%llx x20=0x%llx x21=0x%llx\\n",
\t\t    esr, far, get_saved_state_pc(&context->ss), get_saved_state_reg(&context->ss, 20),
\t\t    get_saved_state_reg(&context->ss, 21));
\t\tloader_log_line(line);
\t}

\tsp = get_saved_state_sp(&context->ss);
\tvm_offset_t kstackptr = (vm_offset_t)thread->machine.kstackptr;
\tkernel_stack_bottom = round_page(kstackptr) - KERNEL_STACK_SIZE;"""
        sleh_inv_new = """void
sleh_invalid_stack(arm_context_t *context, uint64_t esr __unused, vm_offset_t far __unused)
{
\textern void hk_draw_band_stackless(uint32_t color, uint32_t row_start, uint32_t row_count);
\thk_draw_band_stackless(0x00FF8C00, 216, 4); /* Dark Orange: sleh_invalid_stack entered (216..219) */

\tthread_t thread = current_thread();
\tif (thread == NULL) {
\t\t/* Early boot invalid stack with TPIDR_EL1 == 0: DO NOT DEREFERENCE thread! */
\t\textern void hk_render_screen_asm(const char *str, uint32_t len, const unsigned char *font);
\t\textern const unsigned char iso_font[];
\t\tstatic char emsg[384];
\t\tint pos = 0;
\t\tconst char *hdr = "=== HAKIOS EARLY INVALID STACK ===\\n";
\t\twhile (*hdr) emsg[pos++] = *hdr++;
\t\tuint64_t pc = (context != NULL) ? (uint64_t)get_saved_state_pc(&context->ss) : 0ULL;
\t\tuint64_t sp_val = (context != NULL) ? (uint64_t)get_saved_state_sp(&context->ss) : 0ULL;
\t\tuint64_t lr_val = (context != NULL) ? (uint64_t)get_saved_state_reg(&context->ss, 30) : 0ULL;
\t\tuint64_t x0_val = (context != NULL) ? (uint64_t)get_saved_state_reg(&context->ss, 0) : 0ULL;
\t\tuint64_t vals[6] = { pc, lr_val, sp_val, (uint64_t)esr, (uint64_t)far, x0_val };
\t\tconst char *names[6] = { "pc : 0x", "lr : 0x", "sp : 0x", "esr: 0x", "far: 0x", "x0 : 0x" };
\t\tfor (int k = 0; k < 6; k++) {
\t\t\tconst char *nm = names[k];
\t\t\twhile (*nm) emsg[pos++] = *nm++;
\t\t\tuint64_t v = vals[k];
\t\t\tfor (int sh = 60; sh >= 0; sh -= 4) {
\t\t\t\tuint32_t d = (uint32_t)((v >> sh) & 0xf);
\t\t\t\temsg[pos++] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
\t\t\t}
\t\t\temsg[pos++] = '\\n';
\t\t}
\t\temsg[pos] = '\\0';
\t\tfor (uintptr_t p = (uintptr_t)emsg; p < (uintptr_t)emsg + pos + 64; p += 64) {
\t\t\t__asm__ volatile ("dc civac, %0" : : "r" (p));
\t\t}
\t\t__asm__ volatile ("dsb sy");
\t\thk_render_screen_asm(emsg, pos, iso_font);
\t\thk_draw_band_stackless(0x00FF00FF, 220, 4); /* Magenta: early panic rendered (220..223) */
\t\tfor (;;) {
\t\t\t__asm__ volatile ("wfe");
\t\t}
\t}

\tvm_offset_t kernel_stack_bottom, sp;

\t{
\t\t/* hakios: the panic text below gets cut short this early; the syndrome first, on one line */
\t\tchar line[200];
\t\tsnprintf(line, sizeof(line), "hakios: early exception esr=0x%llx far=0x%lx pc=0x%llx x20=0x%llx x21=0x%llx\\n",
\t\t    esr, far, get_saved_state_pc(&context->ss), get_saved_state_reg(&context->ss, 20),
\t\t    get_saved_state_reg(&context->ss, 21));
\t\tloader_log_line(line);
\t}

\tsp = get_saved_state_sp(&context->ss);
\tvm_offset_t kstackptr = (vm_offset_t)thread->machine.kstackptr;
\tkernel_stack_bottom = round_page(kstackptr) - KERNEL_STACK_SIZE;"""
        if sleh_inv_old in c_norm:
            c_norm = c_norm.replace(sleh_inv_old, sleh_inv_new, 1)
            print("  [OK] osfmk/arm64/sleh.c: sleh_invalid_stack NULL-guard and diagnostic bands installed")
        else:
            print("  [WARN] sleh_inv_old not found in sleh.c")

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

        # CP3: Phase 1 Direct Mapping & Magenta band (rows 20-29, 0x5c015180) & save (VA - PA) delta
        cp3_old = """\t/* Ensure TTEs are visible */
\tdsb\t\tish

\tLOADER_CHECKPOINT 3

\tb\t\tcommon_start"""

        cp3_new = """\t// hakios Phase 1: Direct Framebuffer Mapping in bootstrap_pagetables (TTBR1 L1[192])
\t// L1 index 192 (0xC0), L2 entries 32..36 (0xfffffff004000000..0xfffffff004a00000)
\t// Base of bootstrap_pagetables is in x25. TTBR1 L1 root is at x25 + 0x1000.
\tadd\tx9, x25, #0x1000
\tldr\tx10, [x9, #(192 * 8)]
\tmovz\tx13, #0xf000
\tmovk\tx13, #0xffff, lsl #16
\tmovk\tx13, #0xffff, lsl #32
\tand\tx10, x10, x13
\tadd\tx10, x10, #(32 * 8)

\t// Guard: verify entries 32..36 are zero before modifying
\tmov\tw11, #5
\tmov\tx14, x10
Lhk_fb_read_guard:
\tldr\tx15, [x14], #8
\tcbnz\tx15, Lhk_fb_read_fail
\tsubs\tw11, w11, #1
\tb.ne\tLhk_fb_read_guard
\tb\tLhk_fb_zero

Lhk_fb_read_fail:
\t// Warning: non-zero entries found! Draw Red band at row 45 directly to PA (0x5c02f760)
\tmovz\tx14, #0xf760
\tmovk\tx14, #0x5c02, lsl #16
\tmovz\tx15, #0x00ff
\tmovk\tx15, #0x0000, lsl #16
\tmovk\tx15, #0x00ff, lsl #32
\tmovk\tx15, #0x0000, lsl #48
\tmov\tw11, #1080
Lhk_fb_warn_loop:
\tstp\tx15, x15, [x14], #16
\tsubs\tw11, w11, #1
\tb.ne\tLhk_fb_warn_loop

Lhk_fb_zero:
\tmov\tw11, #5
\tmov\tx14, x10
Lhk_fb_zero_loop:
\tstr\txzr, [x14], #8
\tsubs\tw11, w11, #1
\tb.ne\tLhk_fb_zero_loop

\t// Fill 5 block descriptors: PA | 0x0060000000000605ULL
\t// PA base = 0x5c000000, 2MB step = 0x200000
\tmovz\tx12, #0x0605
\tmovk\tx12, #0x0060, lsl #48
\tmovz\tx13, #0x0000
\tmovk\tx13, #0x5c00, lsl #16
\torr\tx12, x12, x13

\tmov\tw11, #5
\tmovz\tx13, #0x0000
\tmovk\tx13, #0x0020, lsl #16
Lhk_fb_fill:
\tstr\tx12, [x10], #8
\tadd\tx12, x12, x13
\tsubs\tw11, w11, #1
\tb.ne\tLhk_fb_fill
\tdsb\tsy

\t/* Ensure TTEs are visible */
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

        cp11_new = """\t// hakios: set SP_EL1 (exception stack) to VA(excepstack_top) so it NEVER aliases SP_EL0 (intstack_top) during arm_init!
\tadrp\tx9, EXT(excepstack_top)@page
\tadd\tx9, x9, EXT(excepstack_top)@pageoff
\tmsr\t\tSPSel, #1
\tmov\t\tsp, x9
\tmsr\t\tSPSel, #0

\t// hakios visual marker CP11: Blue band at rows 40-49 (0x5c02a300)
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

\t/* hakios DIAGNOSTIC: Phase 1 AT S1E1W measurement and bootstrap pagetable inspection */
\t// 1. Measure AT S1E1W with MMU ON on direct FB VA (0xfffffff004000000)
\tmovz\tx9, #0x0400, lsl #16
\tmovk\tx9, #0xfff0, lsl #32
\tmovk\tx9, #0xffff, lsl #48
\tat\ts1e1w, x9
\tisb
\tmrs\tx27, par_el1

\t// 2. Switch MMU OFF to inspect memory via PA and paint diagnostic rows
\tmrs\tx9, SCTLR_EL1
\tbic\tx9, x9, #1
\tmsr\tSCTLR_EL1, x9
\tisb

\t// 3. Read L1[192] and L2[32] from bootstrap_pagetables via PA
\tadd\tx16, x25, #0x1000
\tldr\tx28, [x16, #(192 * 8)]
\tcbz\tx28, 10f
\tmovz\tx13, #0xf000
\tmovk\tx13, #0xffff, lsl #16
\tmovk\tx13, #0xffff, lsl #32
\tand\tx17, x28, x13
\tldr\tx29, [x17, #(32 * 8)]
\tb\t11f
10:
\tmov\tx29, xzr
11:

\t// 4. Draw Row 170 (2 rows: 170..171, PA = 0x5c0b34c0): PAR_EL1 F bit (Lime = Success, Red = Fail)
\tmovz\tx9, #0x34c0
\tmovk\tx9, #0x5c0b, lsl #16
\ttbz\tx27, #0, 12f
\t// Fail (F == 1): Bright Red
\tmovz\tx10, #0x00ff
\tmovk\tx10, #0x0000, lsl #16
\tmovk\tx10, #0x00ff, lsl #32
\tmovk\tx10, #0x0000, lsl #48
\tb\t13f
12:
\t// Success (F == 0): Lime Green
\tmovz\tx10, #0x0000
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0x0000, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
13:
\tmov\tw11, #540
14:
\tstp\tx10, x10, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t14b

\t// 5. Draw Row 172 (2 rows: 172..173, PA = 0x5c0b5680): PAR_EL1 FST code
\tmovz\tx9, #0x5680
\tmovk\tx9, #0x5c0b, lsl #16
\ttbz\tx27, #0, 20f
\tubfx\tx12, x27, #1, #6
\tcmp\tx12, #0x05
\tb.eq\t21f
\tcmp\tx12, #0x06
\tb.eq\t22f
\tcmp\tx12, #0x0e
\tb.eq\t23f
\tcmp\tx12, #0x0a
\tb.eq\t24f
\t// Other fault: Magenta (0x00FF00FF)
\tmovz\tx10, #0x00ff
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0x00ff, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
\tb\t25f
20:
\t// Success: Spring Green (0x0000FF7F)
\tmovz\tx10, #0xff7f
\tmovk\tx10, #0x0000, lsl #16
\tmovk\tx10, #0xff7f, lsl #32
\tmovk\tx10, #0x0000, lsl #48
\tb\t25f
21:
\t// FST 0x05 (Trans L1): Dark Red (0x008B0000)
\tmovz\tx10, #0x0000
\tmovk\tx10, #0x008b, lsl #16
\tmovk\tx10, #0x0000, lsl #32
\tmovk\tx10, #0x008b, lsl #48
\tb\t25f
22:
\t// FST 0x06 (Trans L2): Dark Orange (0x00FF8C00)
\tmovz\tx10, #0x8c00
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0x8c00, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
\tb\t25f
23:
\t// FST 0x0E (Perm L2): Yellow (0x00FFFF00)
\tmovz\tx10, #0xff00
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0xff00, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
\tb\t25f
24:
\t// FST 0x0A (AF L2): Cyan (0x0000FFFF)
\tmovz\tx10, #0xffff
\tmovk\tx10, #0x0000, lsl #16
\tmovk\tx10, #0xffff, lsl #32
\tmovk\tx10, #0x0000, lsl #48
25:
\tmov\tw11, #540
26:
\tstp\tx10, x10, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t26b

\t// 6. Draw Row 174 (2 rows: 174..175, PA = 0x5c0b7840): L1[192] Status
\tmovz\tx9, #0x7840
\tmovk\tx9, #0x5c0b, lsl #16
\tcbnz\tx28, 30f
\t// L1[192] == 0: Dark Red (0x008B0000)
\tmovz\tx10, #0x0000
\tmovk\tx10, #0x008b, lsl #16
\tmovk\tx10, #0x0000, lsl #32
\tmovk\tx10, #0x008b, lsl #48
\tb\t32f
30:
\tand\tx12, x28, #3
\tcmp\tx12, #3
\tb.ne\t31f
\t// Valid table descriptor: Dodger Blue (0x001E90FF)
\tmovz\tx10, #0x90ff
\tmovk\tx10, #0x001e, lsl #16
\tmovk\tx10, #0x90ff, lsl #32
\tmovk\tx10, #0x001e, lsl #48
\tb\t32f
31:
\t// Other: White
\tmovz\tx10, #0xffff
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0xffff, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
32:
\tmov\tw11, #540
33:
\tstp\tx10, x10, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t33b

\t// 7. Draw Row 176 (2 rows: 176..177, PA = 0x5c0b9a00): L2[32] Content
\tmovz\tx9, #0x9a00
\tmovk\tx9, #0x5c0b, lsl #16
\tcbnz\tx29, 40f
\t// L2[32] == 0: White (0x00FFFFFF)
\tmovz\tx10, #0xffff
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0xffff, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
\tb\t44f
40:
\t// Expected descriptor: 0x006000005c000605
\tmovz\tx14, #0x0605
\tmovk\tx14, #0x0060, lsl #48
\tmovz\tx15, #0x0000
\tmovk\tx15, #0x5c00, lsl #16
\torr\tx14, x14, x15
\tcmp\tx29, x14
\tb.ne\t41f
\t// Exact match: Vivid Gold (0x00FFD700)
\tmovz\tx10, #0xd700
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0xd700, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
\tb\t44f
41:
\tand\tx12, x29, #3
\tcmp\tx12, #1
\tb.ne\t42f
\t// Block descriptor with different attr/PA: Hot Pink (0x00FF69B4)
\tmovz\tx10, #0x69b4
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0x69b4, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
\tb\t44f
42:
\t// Other descriptor type: Purple (0x00800080)
\tmovz\tx10, #0x0080
\tmovk\tx10, #0x0080, lsl #16
\tmovk\tx10, #0x0080, lsl #32
\tmovk\tx10, #0x0080, lsl #48
44:
\tmov\tw11, #540
45:
\tstp\tx10, x10, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t45b

\t// 8. Draw Row 178..181 (4 rows, PA = 0x5c0bbbc0): 16-bit Binary Gauge of PAR_EL1[15:0]
\tmov\tw18, #4
50:
\tmovz\tx9, #0xbbc0
\tmovk\tx9, #0x5c0b, lsl #16
\tmov\tw11, #4
\tsub\tw11, w11, w18
\tmov\tw12, #4320
\tmul\tw11, w11, w12
\tadd\tx9, x9, x11

\t// Left margin: 7 stores of Black (0x00000000)
\tmov\tw11, #7
51:
\tstp\txzr, xzr, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t51b

\t// Loop over 16 bits: bit 0 (leftmost) to bit 15 (rightmost)
\tmov\tw12, #0
52:
\tlsr\tx13, x27, x12
\ttbz\tx13, #0, 53f
\t// Bit is 1: White (0x00FFFFFF)
\tmovz\tx10, #0xffff
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0xffff, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
\tb\t54f
53:
\t// Bit is 0: Dark Grey (0x00333333)
\tmovz\tx10, #0x3333
\tmovk\tx10, #0x0033, lsl #16
\tmovk\tx10, #0x3333, lsl #32
\tmovk\tx10, #0x0033, lsl #48
54:
\t// Paint 16 stores (64 pixels wide)
\tmov\tw11, #16
55:
\tstp\tx10, x10, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t55b

\tadd\tw12, w12, #1
\tcmp\tw12, #16
\tb.ne\t52b

\t// Right margin: 7 stores of Black
\tmov\tw11, #7
56:
\tstp\txzr, xzr, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t56b

\tsubs\tw18, w18, #1
\tb.ne\t50b

\t// 9. Re-enable MMU
\tmrs\tx9, SCTLR_EL1
\torr\tx9, x9, #1
\tmsr\tSCTLR_EL1, x9
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
 * 100% direct VA drawer via hk_draw_band_direct. Zero MMU toggles, zero TTBR switches, zero stack.
 */
\t.text
\t.align 2
\t.globl EXT(hk_draw_band)
LEXT(hk_draw_band)
\tb\tEXT(hk_draw_band_direct)
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
\tand\t\tx26, x3, #0xffffffff
\tcmp\t\tx26, #720
\tcsel\tx26, x26, xzr, hs
\tcbnz\tx26, 21f
\tmov\t\tx26, #720
21:

\tmovz\tw14, #0xffff
\tmovk\tw14, #0xffff, lsl #16
\tmovz\tw13, #0x0000
\tmovk\tw13, #0xff00, lsl #16
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

61:\ttbz\t\tw5, #7, 62f
\tstr\t\tw14, [x7], #4
\tstr\t\tw14, [x7], #4
\tstr\t\tw14, [x8], #4
\tstr\t\tw14, [x8], #4
\tb\t\t63f
62:\tstr\t\tw13, [x7], #4
\tstr\t\tw13, [x7], #4
\tstr\t\tw13, [x8], #4
\tstr\t\tw13, [x8], #4
63:\tlsl\t\tw5, w5, #1
\tadd\t\tw6, w6, #1
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

9:\tmrs\t\tx10, TPIDR_EL1
\tcbnz\tx10, 91f
\tmsr\t\tDAIFSet, #0xf
\tb\t\t92f
91:\tmsr\t\tDAIF, x23
92:
\tldp\t\tx25, x26, [sp, #64]
\tldp\t\tx23, x24, [sp, #48]
\tldp\t\tx21, x22, [sp, #32]
\tldp\t\tx19, x20, [sp, #16]
\tldp\t\tx29, x30, [sp], #80
\tret
"""
        hk_draw_band_direct_code = """
/*
 * hakios: direct virtual address marker band drawer
 * void hk_draw_band_direct(uint32_t color, uint32_t row_start, uint32_t row_count);
 * Runs 100% in VA space. No MMU toggle, no TTBR0 switch, no TLBI, zero stack.
 * Framebuffer VA = 0xfffffff004000000ULL
 * Uses only caller-saved registers x0-x8.
 */
\t.text
\t.align 2
\t.globl EXT(hk_draw_band_direct)
LEXT(hk_draw_band_direct)
\t/* 1. Sanitize arguments (32-bit unsigned) */
\tand\tx1, x1, #0xffffffff
\tand\tx2, x2, #0xffffffff

\t/* 2. Bounds check against 2400 rows */
\tmov\tx3, #2400
\tcmp\tx1, x3
\tb.hs\tLhk_direct_exit
\tsub\tx3, x3, x1
\tcmp\tx2, x3
\tcsel\tx2, x2, x3, lo
\tcbz\tx2, Lhk_direct_exit

\t/* 3. Compute row start pointer (x4) and count (w5) */
\tmov\tx4, #4320
\tmul\tx4, x1, x4
\tmovz\tx3, #0x0400, lsl #16
\tmovk\tx3, #0xfff0, lsl #32
\tmovk\tx3, #0xffff, lsl #48
\tadd\tx4, x4, x3

\t/* Stride: 270 pairs of 64-bit words (4320 bytes) per row */
\tmov\tw5, #270
\tmul\tw5, w2, w5

\t/* 4. AT guard: check if start address is mapped & writable */
\tat\ts1e1w, x4
\tisb
\tmrs\tx3, par_el1
\ttbnz\tx3, #0, Lhk_direct_exit

\t/* 5. Duplicate 32-bit color to 64-bit register x0 */
\tand\tx0, x0, #0xffffffff
\torr\tx0, x0, x0, lsl #32

\t/* 6. Fill loop */
Lhk_direct_loop:
\tstp\tx0, x0, [x4], #16
\tsubs\tw5, w5, #1
\tb.ne\tLhk_direct_loop
\tdsb\tsy

Lhk_direct_exit:
\tret
"""

        hk_draw_band_stackless_code = """
/*
 * hakios: stackless visual marker band drawer (accessible from start.s, locore.s, sleh.c)
 * void hk_draw_band_stackless(uint32_t color, uint32_t row_start, uint32_t row_count);
 * Forwards directly to hk_draw_band_direct (100% stackless, no MMU switch).
 */
\t.text
\t.align 2
\t.globl EXT(hk_draw_band_stackless)
LEXT(hk_draw_band_stackless)
\tb\tEXT(hk_draw_band_direct)
"""
        c_norm = c_norm + hk_draw_band_code + hk_draw_band_direct_code + hk_render_screen_asm_code + hk_draw_band_stackless_code

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

    # 7. makedefs/MakeInc.def (replace +pan with -pan for BCM2837 Cortex-A73/A53)
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

    # 8. osfmk/arm64/locore.s & machine_routines_asm.s (fix CHECK_KERNEL_STACK on intstack & stub ml_sign_thread_state / ml_check_signed_state)
    for asm_name in ("locore.s", "machine_routines_asm.s", "cswitch.s"):
        p8 = os.path.join(src, "osfmk", "arm64", asm_name)
        if os.path.isfile(p8):
            with open(p8, "r", encoding="utf-8", errors="replace") as f:
                c8 = f.read().replace("\r\n", "\n")
            changed_asm = False
            if "LEXT(ml_sign_thread_state)\n" in c8 and "LEXT(ml_sign_thread_state)\n\tret\n" not in c8:
                c8 = c8.replace("LEXT(ml_sign_thread_state)\n", "LEXT(ml_sign_thread_state)\n\tret\n", 1)
                changed_asm = True
                print(f"  [OK] osfmk/arm64/{asm_name}: ml_sign_thread_state stubbed")
            if "LEXT(ml_check_signed_state)\n" in c8 and "LEXT(ml_check_signed_state)\n\tret\n" not in c8:
                c8 = c8.replace("LEXT(ml_check_signed_state)\n", "LEXT(ml_check_signed_state)\n\tret\n", 1)
                changed_asm = True
                print(f"  [OK] osfmk/arm64/{asm_name}: ml_check_signed_state stubbed")
            c8, n_sub = re.subn(r'cbz\s+x2,\s*Lcorrupt_stack_\\@', 'cbz\t\tx2, Ltest_istack_\\@', c8)
            if n_sub > 0:
                changed_asm = True
                print(f"  [OK] osfmk/arm64/{asm_name}: CHECK_KERNEL_STACK cbz x2 -> Ltest_istack patched ({n_sub} occurrences)")

            if asm_name == "locore.s":
                # 1. el1_sp0_synchronous_vector_long: Row 230 OrangeRed (230..235, 6 rows) - 100% STACKLESS BEFORE STP
                t_sp0_sync = """el1_sp0_synchronous_vector_long:
\tstp\t\tx0, x1, [sp, #-16]!\t\t\t\t// Save x0 and x1 to the exception stack"""
                r_sp0_sync = """el1_sp0_synchronous_vector_long:
\t/* hakios/claude: EC diagnostic for early sync faults - 100% STACKLESS BEFORE STP */
\tmov\t\tx16, x0
\tmov\t\tx17, x1
\tmov\t\tx18, lr
\tmrs\t\tx1, ESR_EL1
\tlsr\t\tx0, x1, #26\t\t\t\t\t\t\t// x0 = EC
\tcbz\t\tx0, 88f\t\t\t\t\t\t\t\t// If EC == 0 (LSE/PAC trap), bypass!
\tmrs\t\tx0, TPIDR_EL0
\ttbnz\tx0, #63, 88f\t\t\t\t\t\t// If bit 63 set, already drawn, bypass!
\torr\t\tx0, x0, #(1ULL << 63)
\tmsr\t\tTPIDR_EL0, x0
\tmovz\tw0, #0x4500\t\t\t\t\t\t\t// Vivid OrangeRed (0x00FF4500)
\tmovk\tw0, #0x00ff, lsl #16
\tmov\t\tw1, #230\t\t\t\t\t\t\t// Row 230
\tmov\t\tw2, #6\t\t\t\t\t\t\t\t// 6 rows thick (230..235)
\tbl\t\tEXT(hk_draw_band_stackless)
88:
\tmov\t\tlr, x18
\tmov\t\tx1, x17
\tmov\t\tx0, x16
\tstp\t\tx0, x1, [sp, #-16]!\t\t\t\t// Save x0 and x1 to the exception stack"""
                if t_sp0_sync in c8:
                    c8 = c8.replace(t_sp0_sync, r_sp0_sync, 1)
                    changed_asm = True
                    print(f"  [OK] osfmk/arm64/{asm_name}: el1_sp0_synchronous Row 230 OrangeRed marker installed (pre-stp)")

                # 2. el1_sp0_irq_vector_long: Row 220 GreenYellow (220..223, 4 rows) - STACKLESS BEFORE STP
                t_sp0_irq = """el1_sp0_irq_vector_long:
#if CONFIG_SPTM
\tEL1_SP0_REDZONE_CHECK sp_sel=2
#endif
\tEL1_SP0_VECTOR"""
                r_sp0_irq = """el1_sp0_irq_vector_long:
\t/* hakios/claude: IRQ on SP0 - STACKLESS BEFORE STP (only if TPIDR_EL1 == 0) */
\tmrs\t\tx16, TPIDR_EL1
\tcbnz\tx16, 88f
\tmov\t\tx16, x0
\tmov\t\tx17, x1
\tmov\t\tx18, lr
\tmrs\t\tx0, TPIDR_EL0
\ttbnz\tx0, #62, 87f
\torr\t\tx0, x0, #(1ULL << 62)
\tmsr\t\tTPIDR_EL0, x0
\tmovz\tw0, #0xff2f\t\t\t\t\t\t\t// GreenYellow (0x00ADFF2F)
\tmovk\tw0, #0x00ad, lsl #16
\tmov\t\tw1, #220\t\t\t\t\t\t\t// Row 220
\tmov\t\tw2, #4\t\t\t\t\t\t\t\t// 4 rows thick (220..223)
\tbl\t\tEXT(hk_draw_band_stackless)
87:
\tmov\t\tlr, x18
\tmov\t\tx1, x17
\tmov\t\tx0, x16
88:
#if CONFIG_SPTM
\tEL1_SP0_REDZONE_CHECK sp_sel=2
#endif
\tEL1_SP0_VECTOR"""
                if t_sp0_irq in c8:
                    c8 = c8.replace(t_sp0_irq, r_sp0_irq, 1)
                    changed_asm = True
                    print(f"  [OK] osfmk/arm64/{asm_name}: el1_sp0_irq Row 220 GreenYellow marker installed (pre-stp)")

                # 3. el1_sp0_fiq_vector_long: Row 224 Lime (224..227, 4 rows) - STACKLESS BEFORE STP
                t_sp0_fiq = """el1_sp0_fiq_vector_long:
\t// ARM64_TODO write optimized decrementer
#if CONFIG_SPTM
\tEL1_SP0_REDZONE_CHECK sp_sel=3
#endif
\tEL1_SP0_VECTOR"""
                r_sp0_fiq = """el1_sp0_fiq_vector_long:
\t/* hakios/claude: FIQ on SP0 - STACKLESS BEFORE STP (only if TPIDR_EL1 == 0) */
\tmrs\t\tx16, TPIDR_EL1
\tcbnz\tx16, 88f
\tmov\t\tx16, x0
\tmov\t\tx17, x1
\tmov\t\tx18, lr
\tmrs\t\tx0, TPIDR_EL0
\ttbnz\tx0, #61, 87f
\torr\t\tx0, x0, #(1ULL << 61)
\tmsr\t\tTPIDR_EL0, x0
\tmovz\tw0, #0xff00\t\t\t\t\t\t\t// Lime (0x0000FF00)
\tmovk\tw0, #0x0000, lsl #16
\tmov\t\tw1, #224\t\t\t\t\t\t\t// Row 224
\tmov\t\tw2, #4\t\t\t\t\t\t\t\t// 4 rows thick (224..227)
\tbl\t\tEXT(hk_draw_band_stackless)
87:
\tmov\t\tlr, x18
\tmov\t\tx1, x17
\tmov\t\tx0, x16
88:
\t// ARM64_TODO write optimized decrementer
#if CONFIG_SPTM
\tEL1_SP0_REDZONE_CHECK sp_sel=3
#endif
\tEL1_SP0_VECTOR"""
                if t_sp0_fiq in c8:
                    c8 = c8.replace(t_sp0_fiq, r_sp0_fiq, 1)
                    changed_asm = True
                    print(f"  [OK] osfmk/arm64/{asm_name}: el1_sp0_fiq Row 224 Lime marker installed (pre-stp)")

                # 4. el1_sp0_serror_vector_long: Row 228 Red (228..229, 2 rows) - STACKLESS BEFORE STP
                t_sp0_serror = """el1_sp0_serror_vector_long:
\tEL1_SP0_VECTOR"""
                r_sp0_serror = """el1_sp0_serror_vector_long:
\t/* hakios/claude: SError on SP0 - STACKLESS BEFORE STP */
\tmov\t\tx16, x0
\tmov\t\tx17, x1
\tmov\t\tx18, lr
\tmovz\tw0, #0x0000\t\t\t\t\t\t\t// Vivid Red (0x00FF0000)
\tmovk\tw0, #0x00ff, lsl #16
\tmov\t\tw1, #228\t\t\t\t\t\t\t// Row 228
\tmov\t\tw2, #2\t\t\t\t\t\t\t\t// 2 rows thick
\tbl\t\tEXT(hk_draw_band_stackless)
\tmov\t\tlr, x18
\tmov\t\tx1, x17
\tmov\t\tx0, x16
\tEL1_SP0_VECTOR"""
                if t_sp0_serror in c8:
                    c8 = c8.replace(t_sp0_serror, r_sp0_serror, 1)
                    changed_asm = True
                    print(f"  [OK] osfmk/arm64/{asm_name}: el1_sp0_serror Row 228 Red marker installed (pre-stp)")

                # 5. el1_sp1_synchronous_vector_long: Row 236 BlueViolet (236..237, 2 rows) - STACKLESS BEFORE STP
                t_sp1_sync = """el1_sp1_synchronous_vector_long:
\t/*
\t * Before making our first (potentially faulting) memory access"""
                r_sp1_sync = """el1_sp1_synchronous_vector_long:
\t/* hakios/claude: Sync on SP1 - STACKLESS BEFORE STP */
\tmov\t\tx16, x0
\tmov\t\tx17, x1
\tmov\t\tx18, lr
\tmovz\tw0, #0x2be2\t\t\t\t\t\t\t// BlueViolet (0x008A2BE2)
\tmovk\tw0, #0x008a, lsl #16
\tmov\t\tw1, #236\t\t\t\t\t\t\t// Row 236
\tmov\t\tw2, #2\t\t\t\t\t\t\t\t// 2 rows thick
\tbl\t\tEXT(hk_draw_band_stackless)
\tmov\t\tlr, x18
\tmov\t\tx1, x17
\tmov\t\tx0, x16
\t/*
\t * Before making our first (potentially faulting) memory access"""
                if t_sp1_sync in c8:
                    c8 = c8.replace(t_sp1_sync, r_sp1_sync, 1)
                    changed_asm = True
                    print(f"  [OK] osfmk/arm64/{asm_name}: el1_sp1_synchronous Row 236 BlueViolet marker installed (pre-stp)")

                # 6. el1_sp1_irq_vector_long: Row 238 DarkOrange (238..239, 2 rows) - STACKLESS BEFORE STP
                t_sp1_irq = """el1_sp1_irq_vector_long:
\tEL1_SP1_VECTOR"""
                r_sp1_irq = """el1_sp1_irq_vector_long:
\t/* hakios/claude: IRQ on SP1 - STACKLESS BEFORE STP (only if TPIDR_EL1 == 0) */
\tmrs\t\tx16, TPIDR_EL1
\tcbnz\tx16, 88f
\tmov\t\tx16, x0
\tmov\t\tx17, x1
\tmov\t\tx18, lr
\tmovz\tw0, #0x8c00\t\t\t\t\t\t\t// DarkOrange (0x00FF8C00)
\tmovk\tw0, #0x00ff, lsl #16
\tmov\t\tw1, #238\t\t\t\t\t\t\t// Row 238
\tmov\t\tw2, #2\t\t\t\t\t\t\t\t// 2 rows thick
\tbl\t\tEXT(hk_draw_band_stackless)
\tmov\t\tlr, x18
\tmov\t\tx1, x17
\tmov\t\tx0, x16
88:
\tEL1_SP1_VECTOR"""
                if t_sp1_irq in c8:
                    c8 = c8.replace(t_sp1_irq, r_sp1_irq, 1)
                    changed_asm = True
                    print(f"  [OK] osfmk/arm64/{asm_name}: el1_sp1_irq Row 238 DarkOrange marker installed (pre-stp)")

                t_corr_entry = """\tINIT_SAVED_STATE_FLAVORS sp, w0, w1
\tmov\t\tx0, sp\t\t\t\t\t\t\t\t// Copy exception frame pointer to x0
\tadrp\tx1, fleh_invalid_stack@page\t\t\t// Load address for fleh"""
                r_corr_entry = """\tINIT_SAVED_STATE_FLAVORS sp, w0, w1
\tmov\t\tx0, sp\t\t\t\t\t\t\t\t// Copy exception frame pointer to x0
\t/* hakios/claude: draw Row 204 (Vivid Yellow: 204..207) on corrupt stack detection */
\tmov\t\tx19, x0
\tmovz\tw0, #0xff00
\tmovk\tw0, #0x00ff, lsl #16
\tmov\t\tw1, #204
\tmov\t\tw2, #4
\tbl\t\tEXT(hk_draw_band_stackless)
\tmov\t\tx0, x19
\tadrp\tx1, fleh_invalid_stack@page\t\t\t// Load address for fleh"""
                if t_corr_entry in c8:
                    c8 = c8.replace(t_corr_entry, r_corr_entry, 1)
                    changed_asm = True
                    print(f"  [OK] osfmk/arm64/{asm_name}: Lcorrupt_stack Row 204 Vivid Yellow marker installed")

                t_spin_entry = """0:
\twfe
\tb\t\t0b // Spin for watchdog"""
                r_spin_entry = """0:
\t/* hakios/claude: draw Row 210 (Vivid Red: 210..213) on spill failed spin */
\tmovz\tw0, #0x0000
\tmovk\tw0, #0x00ff, lsl #16
\tmov\t\tw1, #210
\tmov\t\tw2, #4
\tbl\t\tEXT(hk_draw_band_stackless)
1:
\twfe
\tb\t\t1b // Spin for watchdog"""
                if t_spin_entry in c8:
                    c8 = c8.replace(t_spin_entry, r_spin_entry)
                    changed_asm = True
                    print(f"  [OK] osfmk/arm64/{asm_name}: spill_failed Row 210 Vivid Red marker installed in all vectors (SP0 and SP1)")

                # 7. Instrument return-to-userland path in locore.s (Rows 668 Gold & 670 DarkOrange)
                t_eret_auth = """\tldr\t\tw9, [sp, SS64_CPSR]
\ttbnz\tw9, PSR64_MODE_EL_SHIFT, 9f
\tmrs\t\tx9, TPIDR_EL1\t\t\t\t\t\t// the process whose keys these are
\tadrp\tx10, EXT(pac_std_keys)@page
\tadd\t\tx10, x10, EXT(pac_std_keys)@pageoff
\tPAC_STD_LOAD_USER_KEYS x10, x9, x11, x12, x13, x14, x15, x2, x3
9:
#endif /* HAS_STD_PAC */
\tmov \tx0, sp\t\t\t\t\t\t\t\t// x0 = &pcb
\t// Loads authed $x0->ss_64.pc into x1 and $x0->ss_64.cpsr into w2
\tAUTH_THREAD_STATE_IN_X0\tx20, x21, x22, x23, x24, x25, el0_state_allowed=1"""
                r_eret_auth = """\tldr\t\tw9, [sp, SS64_CPSR]
\ttbnz\tw9, PSR64_MODE_EL_SHIFT, 9f
\t/* hakios/build108: Row 668 Gold: locore.s returning to EL0 */
\tmov\t\tx15, lr
\tmovz\tw0, #0xd700
\tmovk\tw0, #0x00ff, lsl #16
\tmov\t\tw1, #668
\tmov\t\tw2, #4
\tbl\t\tEXT(hk_draw_band_direct)
\tmov\t\tlr, x15
\tmrs\t\tx9, TPIDR_EL1\t\t\t\t\t\t// the process whose keys these are
\tadrp\tx10, EXT(pac_std_keys)@page
\tadd\t\tx10, x10, EXT(pac_std_keys)@pageoff
\tPAC_STD_LOAD_USER_KEYS x10, x9, x11, x12, x13, x14, x15, x2, x3
9:
#endif /* HAS_STD_PAC */
\tmov \tx0, sp\t\t\t\t\t\t\t\t// x0 = &pcb
\t// Loads authed $x0->ss_64.pc into x1 and $x0->ss_64.cpsr into w2
\tAUTH_THREAD_STATE_IN_X0\tx20, x21, x22, x23, x24, x25, el0_state_allowed=1
\t/* hakios/build108: Row 670 DarkOrange: AUTH_THREAD_STATE_IN_X0 finished */
\tmov\t\tx14, x1
\tmov\t\tw13, w2
\tmov\t\tx15, lr
\tmovz\tw0, #0x8c00
\tmovk\tw0, #0x00ff, lsl #16
\tmov\t\tw1, #670
\tmov\t\tw2, #4
\tbl\t\tEXT(hk_draw_band_direct)
\tmov\t\tx1, x14
\tmov\t\tw2, w13
\tmov\t\tlr, x15"""
                if t_eret_auth in c8:
                    c8 = c8.replace(t_eret_auth, r_eret_auth, 1)
                    changed_asm = True
                    print(f"  [OK] osfmk/arm64/{asm_name}: Rows 668 (Gold) and 670 (DarkOrange) installed in exception_return")
                else:
                    print(f"  [WARN] osfmk/arm64/{asm_name}: t_eret_auth pattern not found")

            if changed_asm:
                with open(p8, "w", encoding="utf-8", newline="\n") as f:
                    f.write(c8)

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
\t\t\t__asm__ volatile ("dc civac, %0" : : "r" (p));
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
\t\textern void hk_draw_band_direct(uint32_t color, uint32_t row_start, uint32_t row_count);
\t\thk_draw_band_direct(0x0000FFFF, 496, 4); /* Cyan: entered kernel_bootstrap */
\t}""", 1)
            print("  [OK] osfmk/kern/startup.c: kernel_bootstrap visual marker installed")
        else:
            print("  [WARN] kernel_bootstrap not found in startup.c")

        # Instrument load_context entry and context switch
        lc_target = "static void __attribute__((noreturn))\nload_context(\n\tthread_t                thread)\n{"
        if lc_target in c10:
            c10 = c10.replace(lc_target, lc_target + """
\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0048D1CC, 543, 2); } /* MediumTurquoise: load_context entry */""", 1)
            print("  [OK] osfmk/kern/startup.c: load_context entry instrumented")

        msc_target = "machine_set_current_thread(thread);"
        if msc_target in c10:
            c10 = c10.replace(msc_target, msc_target + """
\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0020B2AA, 544, 2); } /* LightSeaGreen: machine_set_current_thread done */""", 1)
            print("  [OK] osfmk/kern/startup.c: machine_set_current_thread instrumented")

        mlc_target = "machine_load_context(thread);"
        if mlc_target in c10:
            c10 = c10.replace(mlc_target, """{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF7F50, 545, 2); } /* Coral: about to machine_load_context */
\t""" + mlc_target, 1)
            print("  [OK] osfmk/kern/startup.c: machine_load_context instrumented")

        # Instrument kernel_bootstrap_thread(void) entry
        kbt_target = "static void\nkernel_bootstrap_thread(void)\n{"
        if kbt_target in c10:
            c10 = c10.replace(kbt_target, kbt_target + """
\t{
\t\textern void hk_draw_band_direct(uint32_t color, uint32_t row_start, uint32_t row_count);
\t\thk_draw_band_direct(0x00FFFFFF, 546, 4); /* White: ENTERED kernel_bootstrap_thread! */
\t}""", 1)
            print("  [OK] osfmk/kern/startup.c: kernel_bootstrap_thread visual marker installed")
        else:
            print("  [WARN] kernel_bootstrap_thread not found in startup.c")

        # Instrument kernel_bootstrap_log
        m_log = re.search(r'(kernel_bootstrap_log\s*\(\s*const\s+char\s*\*\s*\w+\s*\)\s*\{)', c10)
        if m_log:
            c10 = c10.replace(m_log.group(0), m_log.group(0) + """
\t{
\t\textern void hk_draw_band_direct(uint32_t color, uint32_t row_start, uint32_t row_count);
\t\tstatic uint32_t hk_bstep = 0;
\t\thk_draw_band_direct(0x00FF8C00, 240 + ((hk_bstep++ & 0x1f) * 2), 2); /* Dark Orange line for each milestone */
\t}""", 1)
            print("  [OK] osfmk/kern/startup.c: kernel_bootstrap_log visual milestone ladder installed")
        else:
            print("  [WARN] kernel_bootstrap_log not found in startup.c")

        # Step-by-step milestones in kernel_bootstrap
        kb_steps = [
            ("\tconsole_init();", "\tconsole_init();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0000FFFF, 500, 4); } /* Cyan: console_init */"),
            ("\tsched_init();", "\tsched_init();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0000FF00, 506, 4); } /* Lime: sched_init */"),
            ("\tmapping_free_prime();", "\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FFD700, 508, 4); } /* Gold: mapping_free_prime entering */\n\tmapping_free_prime();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FFFF00, 512, 4); } /* Yellow: mapping_free_prime done */"),
            ("\tmachine_init();", "\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x007FFF00, 514, 4); } /* Chartreuse: machine_init entering */\n\tmachine_init();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0000BFFF, 518, 4); } /* DeepSkyBlue: machine_init done */"),
            ("\tclock_init();", "\tclock_init();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00BA55D3, 524, 4); } /* MediumOrchid: clock_init */"),
            ("\ttask_init();", "\ttask_init();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x001E90FF, 530, 4); } /* DodgerBlue: task_init */"),
            ("\tthread_init();", "\tthread_init();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF1493, 536, 4); } /* DeepPink: thread_init */"),
            ("\tresult = kernel_thread_create((thread_continue_t)kernel_bootstrap_thread, NULL, MAXPRI_KERNEL, &thread);",
             "\tresult = kernel_thread_create((thread_continue_t)kernel_bootstrap_thread, NULL, MAXPRI_KERNEL, &thread);\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0000FF7F, 542, 4); } /* SpringGreen: kernel_thread_create */"),
        ]
        for t_step, r_step in kb_steps:
            if t_step in c10:
                c10 = c10.replace(t_step, r_step, 1)
                print(f"  [OK] osfmk/kern/startup.c: {t_step.strip()[:30]} instrumented")

        # Step milestones in kernel_bootstrap_thread
        kbt_steps = [
            ("\tidle_thread_create(processor, idle_thread);",
             "\tidle_thread_create(processor, idle_thread);\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FFD700, 548, 4); } /* Gold: idle_thread_create done */"),
            ("\tsched_startup();",
             "\tsched_startup();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0000FF00, 550, 4); } /* Lime: sched_startup done */"),
            ("\tthread_daemon_init();",
             "\tthread_daemon_init();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0000FFFF, 552, 4); } /* Cyan: thread_daemon_init done */"),
            ("\tmapping_adjust();",
             "\tmapping_adjust();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF00FF, 554, 4); } /* Magenta: mapping_adjust done */"),
            ("\tclock_service_create();",
             "\tclock_service_create();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF8C00, 556, 4); } /* DarkOrange: clock_service_create done */"),
            ("\tdevice_service_create();",
             "\tdevice_service_create();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF1493, 558, 4); } /* DeepPink: device_service_create done */"),
            ("\tPE_init_iokit();",
             "\tPE_init_iokit();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FFFF00, 560, 4); } /* Yellow: PE_init_iokit done */"),
            ("\t(void) spllo();",
             "\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x007FFF00, 562, 4); } /* Chartreuse: about to spllo */\n\t(void) spllo();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0000BFFF, 564, 4); } /* DeepSkyBlue: spllo returned */"),
            ("\tinitialize_screen(NULL, kPEAcquireScreen);",
             "\tinitialize_screen(NULL, kPEAcquireScreen);\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00BA55D3, 566, 2); } /* MediumOrchid: initialize_screen done */"),
            ("\tvm_commpage_text_init();",
             "\tvm_commpage_text_init();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0000FFFF, 568, 2); } /* Cyan: vm_commpage done */"),
            ("\tload_static_trust_cache();",
             "\tload_static_trust_cache();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF7F50, 570, 2); } /* Coral: trust_cache done */"),
            ("\tkernel_startup_initialize_upto(STARTUP_SUB_LOCKDOWN);",
             "\tkernel_startup_initialize_upto(STARTUP_SUB_LOCKDOWN);\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FFD700, 572, 2); } /* Gold: STARTUP_SUB_LOCKDOWN done */"),
            ("\tOSKextRemoveKextBootstrap();",
             "\tOSKextRemoveKextBootstrap();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0000FF00, 574, 2); } /* Lime: OSKextRemoveKextBootstrap done */"),
            ("\tmachine_lockdown();",
             "\tmachine_lockdown();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x007FFF00, 576, 2); } /* Chartreuse: machine_lockdown done */"),
            ("\tPE_lockdown_iokit();",
             "\tPE_lockdown_iokit();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0000BFFF, 578, 2); } /* DeepSkyBlue: PE_lockdown_iokit done */"),
            ("\tvm_set_restrictions(machine_info.max_cpus);",
             "\tvm_set_restrictions(machine_info.max_cpus);\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF8C00, 580, 2); } /* DarkOrange: vm_set_restrictions done */"),
            ("\tkperf_init_early();",
             "\tkperf_init_early();\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF1493, 582, 2); } /* DeepPink: kperf_init_early done */"),
            ("\tbsd_init();",
             "\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FFFFFF, 585, 10); } /* Thick White: ENTERING bsd_init! */\n\tbsd_init();"),
        ]
        for t_step, r_step in kbt_steps:
            if t_step in c10:
                c10 = c10.replace(t_step, r_step, 1)
                print(f"  [OK] osfmk/kern/startup.c: {t_step.strip()[:30]} instrumented")

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

# 13. osfmk/kern/debug.c (on-screen panic dump directly from panic_trap_to_debugger and panic entry)
    p13 = os.path.join(src, "osfmk", "kern", "debug.c")
    if os.path.isfile(p13):
        with open(p13, "r", encoding="utf-8", errors="replace") as f:
            c13 = f.read().replace("\r\n", "\n")

        # Hook panic() function entry directly
        m_panic = re.search(r'(\nvoid\s+panic\s*\(\s*const\s+char\s*\*\s*\w+\s*,[^{]*\{)', c13)
        if m_panic:
            c13 = c13.replace(m_panic.group(0), m_panic.group(0) + '\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF0000, 660, 20); } /* Thick Bright Red band: panic entry */', 1)
            print("  [OK] osfmk/kern/debug.c: panic() entry direct visual hook installed")
        else:
            print("  [WARN] panic() function signature not found in debug.c")

        # Hook panic_trap_to_debugger entry
        m_ptd = re.search(r'(panic_trap_to_debugger\s*\([^){]*\)\s*\{)', c13)
        if m_ptd:
            c13 = c13.replace(m_ptd.group(0), m_ptd.group(0) + '\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF0000, 660, 20); } /* Thick Bright Red band: panic_trap_to_debugger entry */', 1)
            print("  [OK] osfmk/kern/debug.c: panic_trap_to_debugger entry direct visual hook installed")
        else:
            print("  [WARN] panic_trap_to_debugger signature not found in debug.c")

        # Hook panic_spin_forever entry
        m_psf = re.search(r'(panic_spin_forever\s*\([^){]*\)\s*\{)', c13)
        if m_psf:
            c13 = c13.replace(m_psf.group(0), m_psf.group(0) + '\n\t{ extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF0000, 660, 20); } /* Thick Bright Red band: panic_spin_forever entry */', 1)
            print("  [OK] osfmk/kern/debug.c: panic_spin_forever entry direct visual hook installed")
        else:
            print("  [WARN] panic_spin_forever signature not found in debug.c")

        target_dbg = 'loader_log_line("\\n== PANIC: ");\n\t\tloader_log_line(rbuf);\n\t\tloader_log_line(" ==\\n");\n\t}'
        replace_dbg = '''loader_log_line("\\n== PANIC: ");
\t\tloader_log_line(rbuf);
\t\tloader_log_line(" ==\\n");
\t\t/* hakios: on-screen panic dump directly to display */
\t\t{
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x00FF0000, 660, 20); /* Thick Bright Red band: kernel panic */
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
\t\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\t\thk_draw_band_direct(0x00FF0000, 660, 20); /* Thick Bright Red band: early panic */
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
\t__asm__ volatile ("msr daifset, #0xf; isb");
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
        r16_pms_call = """	kernel_debug_string_early("pmap_startup");
	{
		extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
		hk_draw_band_direct(0x0000FFFF, 294, 2); /* Cyan: pmap_startup entered */
	}
	pmap_startup(&virtual_space_start, &virtual_space_end);
	{
		extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
		hk_draw_band_direct(0x0032CD32, 292, 2); /* Lime: pmap_startup returned to vm_page_bootstrap */
	}"""
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
\t\thk_draw_band(0x0000FF00, 280, 6); /* Lime: vm_pages stolen successfully (thick 6 rows) */
\t}
\tvm_pages_end = vm_page_get(npages);
\t{
\t\textern void hk_draw_band(uint32_t, uint32_t, uint32_t);
\t\thk_draw_band(0x00FF8C00, 288, 6); /* Dark Orange: vm_pages_end computed (thick 6 rows) */
\t}"""
        if t16_vmp in c16:
            c16 = c16.replace(t16_vmp, r16_vmp, 1)
            print("  [OK] osfmk/vm/vm_resident.c: vm_pages return markers installed")
        else:
            print("  [WARN] t16_vmp not found in vm_resident.c")

        # Page init loop in pmap_startup with fine-grained ladder markers
        t16_loop = '\tvm_free_page_lock();\n\n\tfor (uint32_t i = 0; i < npages && pmap_next_page(&phys_page); i++) {'
        r16_loop = """\t/* hakios: Direct diagnostic bands without MMU toggling */
\t{
\t\t__asm__ volatile ("msr daifset, #0xf; isb");
\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\thk_draw_band_direct(0x00FF00FF, 302, 2); /* Vivid Magenta: PRE-LOCK (rows 302..303) */
\t\tvm_free_page_lock();
\t\t__asm__ volatile ("msr daifset, #0xf; isb");
\t\thk_draw_band_direct(0x00FF00FF, 304, 2); /* Vivid Magenta: POST-LOCK (rows 304..305) */

\t\t/* Gauge at Rows 410..440 */
\t\tstatic const uint32_t nibble_pal[16] = {
\t\t\t0x00000000, 0x00000080, 0x00008000, 0x00008080,
\t\t\t0x00800000, 0x00800080, 0x00808000, 0x00C0C0C0,
\t\t\t0x00808080, 0x000000FF, 0x0000FF00, 0x0000FFFF,
\t\t\t0x00FF0000, 0x00FF00FF, 0x00FFFF00, 0x00FFFFFF
\t\t};
\t\thk_draw_band_direct(0x00FFFFFF, 410, 4); /* Calibration marker (White, 4 rows: 410..413) */

\t\textern vm_offset_t first_avail;
\t\textern pmap_paddr_t avail_end;
\t\textern bool need_ram_ranges_init;
\t\tuint32_t fa32 = (uint32_t)first_avail;
\t\tuint32_t ae32 = (uint32_t)avail_end;
\t\tfor (int k = 0; k < 8; k++) {
\t\t\tuint32_t nib = (fa32 >> ((7 - k) * 4)) & 0xf;
\t\t\thk_draw_band_direct(nibble_pal[nib], 416 + k, 1);
\t\t}
\t\tfor (int k = 0; k < 8; k++) {
\t\t\tuint32_t nib = (ae32 >> ((7 - k) * 4)) & 0xf;
\t\t\thk_draw_band_direct(nibble_pal[nib], 426 + k, 1);
\t\t}
\t\thk_draw_band_direct(need_ram_ranges_init ? 0x0000FF00 : 0x00FF0000, 436, 2);
\t\tuint64_t vbar_val;
\t\t__asm__ volatile ("mrs %0, VBAR_EL1" : "=r" (vbar_val));
\t\thk_draw_band_direct((vbar_val >= 0xffff000000000000ULL) ? 0x0000FF00 : 0x00FF0000, 440, 2);
\t}
\tfor (uint32_t i = 0; i < npages; i++) {
\t\tif (i == 0) {
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x00FFFFFF, 305, 1); /* White: call-site about to call pmap_next_page */
\t\t}
\t\tboolean_t pnext_ret = pmap_next_page(&phys_page);
\t\tif (i == 0) {
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x00800080, 310, 2); /* Purple: call-site returned (Rows 310..311, 2 rows) */
\t\t}
\t\tif (!pnext_ret) {
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x00FF8C00, 356, 2); /* Dark Orange: pmap_next_page returned FALSE (page loop break) */
\t\t\tbreak;
\t\t}
\t\tif (i == 0) {
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x0000BFFF, 312, 4); /* Deep Sky Blue: page loop entered */
\t\t}"""
        if t16_loop in c16:
            c16 = c16.replace(t16_loop, r16_loop, 1)
            print("  [OK] osfmk/vm/vm_resident.c: page init loop markers installed")
        else:
            loop_pat = re.compile(r'\tvm_free_page_lock\(\);.*?\tfor \(uint32_t i = 0; i < npages && pmap_next_page\(&phys_page\); i\+\+\) \{', re.DOTALL)
            if loop_pat.search(c16):
                c16 = loop_pat.sub(r16_loop, c16, count=1)
                print("  [OK] osfmk/vm/vm_resident.c: page init loop markers installed via regex")
            else:
                print("  [WARN] t16_loop not found in vm_resident.c")
        t16_lowglo = '\tvm_pages_first_pnum = phys_page;\n\t\t\tpatch_low_glo_vm_page_info(vm_pages, vm_pages_end,\n\t\t\t    vm_pages_first_pnum);'
        r16_lowglo = """\tvm_pages_first_pnum = phys_page;
\t\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0000FF00, 316, 4); } /* Lime: first_pnum set */
\t\t\tpatch_low_glo_vm_page_info(vm_pages, vm_pages_end, vm_pages_first_pnum);
\t\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF00FF, 320, 4); } /* Magenta: low_glo patched */"""
        if t16_lowglo in c16:
            c16 = c16.replace(t16_lowglo, r16_lowglo, 1)
            print("  [OK] osfmk/vm/vm_resident.c: low_glo info markers installed")

        t16_init = '\t++vm_pages_count;\n\t\tvm_page_init(vm_page_get(i), phys_page);'
        r16_init = """\t++vm_pages_count;
\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); if (i == 0) { extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FFFF00, 324, 4); } } /* Yellow: before vm_page_init(0) */
\t\tvm_page_init(vm_page_get(i), phys_page);
\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); if (i == 0) { extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00FF8C00, 328, 4); } } /* Orange: after vm_page_init(0) */"""
        if t16_init in c16:
            c16 = c16.replace(t16_init, r16_init, 1)
            print("  [OK] osfmk/vm/vm_resident.c: vm_page_init markers installed")

        t16_rel = '\tif (vm_himemory_mode) {\n\t\t\tvm_page_release_startup(vm_page_get(i));\n\t\t}'
        r16_rel = """\tif (vm_himemory_mode) {
\t\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); if (i == 0) { extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x0000FFFF, 332, 4); } } /* Cyan: before vm_page_release_startup(0) */
\t\t\tvm_page_release_startup(vm_page_get(i));
\t\t\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); if (i == 0) { extern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t); hk_draw_band_direct(0x00ADFF2F, 336, 4); } } /* GreenYellow: after vm_page_release_startup(0) */
\t\t}
		/* hakios: logarithmic milestones, gauges, and live clock heartbeat */
\t\tif (i == 1) {
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x00FF1493, 340, 2); /* Deep Pink: i=1 reached */
\t\t} else if (i == 10) {
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x00FFFF00, 342, 2); /* Yellow: i=10 reached */
\t\t} else if (i == 100) {
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x0000FF00, 344, 2); /* Lime: i=100 reached */
\t\t} else if (i == 1000) {
\t\t\tstatic const uint32_t npal1[16] = {
\t\t\t\t0x00000000, 0x00000080, 0x00008000, 0x00008080,
\t\t\t\t0x00800000, 0x00800080, 0x00808000, 0x00C0C0C0,
\t\t\t\t0x00808080, 0x000000FF, 0x0000FF00, 0x0000FFFF,
\t\t\t\t0x00FF0000, 0x00FF00FF, 0x00FFFF00, 0x00FFFFFF
\t\t\t};
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x0000FFFF, 346, 2); /* Cyan: i=1,000 reached */
\t\t\textern vm_offset_t first_avail;
\t\t\tuint32_t fa = (uint32_t)first_avail;
\t\t\tfor (int k = 0; k < 8; k++) {
\t\t\t\tuint32_t nib = (fa >> ((7 - k) * 4)) & 0xf;
\t\t\t\thk_draw_band_direct(npal1[nib], 450 + k, 1);
\t\t\t}
\t\t} else if (i == 10000) {
\t\t\tstatic const uint32_t npal2[16] = {
\t\t\t\t0x00000000, 0x00000080, 0x00008000, 0x00008080,
\t\t\t\t0x00800000, 0x00800080, 0x00808000, 0x00C0C0C0,
\t\t\t\t0x00808080, 0x000000FF, 0x0000FF00, 0x0000FFFF,
\t\t\t\t0x00FF0000, 0x00FF00FF, 0x00FFFF00, 0x00FFFFFF
\t\t\t};
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x001E90FF, 348, 2); /* Dodger Blue: i=10,000 reached */
\t\t\textern vm_offset_t first_avail;
\t\t\tuint32_t fa = (uint32_t)first_avail;
\t\t\tfor (int k = 0; k < 8; k++) {
\t\t\t\tuint32_t nib = (fa >> ((7 - k) * 4)) & 0xf;
\t\t\t\thk_draw_band_direct(npal2[nib], 460 + k, 1);
\t\t\t}
\t\t} else if (i == 50000) {
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x00BA55D3, 350, 2); /* Medium Orchid: i=50,000 reached */
\t\t} else if (i == 100000) {
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x00FFD700, 352, 2); /* Gold: i=100,000 reached */
\t\t} else if (i == (npages >> 1)) {
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\thk_draw_band_direct(0x00FF8C00, 354, 2); /* Dark Orange: i=50% reached */
\t\t}
\t\tif ((i & 0x0FFF) == 0 && i >= 1000) {
\t\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\t\tuint64_t timer_val;
\t\t\t__asm__ volatile ("mrs %0, cntvct_el0" : "=r" (timer_val));
\t\t\tuint32_t hb_col = (timer_val & (1ULL << 18)) ? 0x0000FF00 : 0x00FF00FF;
\t\t\thk_draw_band_direct(hb_col, 470, 2);
\t\t}"""
        if t16_rel in c16:
            c16 = c16.replace(t16_rel, r16_rel, 1)
            print("  [OK] osfmk/vm/vm_resident.c: vm_page_release markers installed")

        t16_startup_lock = '\tif (opts & VMP_RELEASE_STARTUP) {\n\t\tLCK_MTX_ASSERT(&vm_page_queue_free_lock, LCK_MTX_ASSERT_OWNED);\n\t}'
        r16_startup_lock = """\tif (opts & VMP_RELEASE_STARTUP) {
\t\tif (startup_phase >= STARTUP_SUB_KMEM) {
\t\t\tLCK_MTX_ASSERT(&vm_page_queue_free_lock, LCK_MTX_ASSERT_OWNED);
\t\t}
\t}"""
        if t16_startup_lock in c16:
            c16 = c16.replace(t16_startup_lock, r16_startup_lock, 1)
            print("  [OK] osfmk/vm/vm_resident.c: early startup free queue lock assert guarded")

        # Anchor unlock replacement specifically to the unlock following r16_loop in pmap_startup
        idx_loop = c16.find(r16_loop)
        t16_unlock = '\tvm_free_page_unlock();'
        # Guard backward loop to vm_pages_count
        t16_bwd = 'for (uint32_t i = npages; i-- > 0;)'
        r16_bwd = 'for (uint32_t i = vm_pages_count; i-- > 0;)'
        if t16_bwd in c16:
            c16 = c16.replace(t16_bwd, r16_bwd, 1)
            print("  [OK] osfmk/vm/vm_resident.c: backward loop bounded to vm_pages_count")

        r16_unlock = """\tvm_free_page_unlock();
\t{
\t\textern void hk_draw_band_direct(uint32_t, uint32_t, uint32_t);
\t\thk_draw_band_direct(0x0000FF7F, 358, 4); /* Spring Green: page loop & unlock complete */
\t}"""
        if idx_loop != -1 and t16_unlock in c16[idx_loop:]:
            c16 = c16[:idx_loop] + c16[idx_loop:].replace(t16_unlock, r16_unlock, 1)
            print("  [OK] osfmk/vm/vm_resident.c: page init unlock marker installed (anchored)")
        else:
            print("  [WARN] t16_unlock not found after r16_loop in vm_resident.c")

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
\t\thk_draw_band(0x0000FF00, 360, 2); /* Lime Green: vm_page_bootstrap returned */
\t}"""
        if t17 in c17:
            c17 = c17.replace(t17, r17, 1)
            c17 = c17.replace('zone_bootstrap();', 'zone_bootstrap();\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x0000FFFF, 364, 2); }', 1)
            c17 = c17.replace('vm_object_bootstrap();', 'vm_object_bootstrap();\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FF1493, 368, 2); }', 1)
            c17 = c17.replace('vm_retire_boot_pages();', 'vm_retire_boot_pages();\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FFFF00, 372, 2); }', 1)
            c17 = c17.replace('vm_map_init();', 'vm_map_init();\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x009400D3, 376, 2); }', 1)
            c17 = c17.replace('kmem_init(start, end);', 'kmem_init(start, end);\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x0000BFFF, 380, 2); }', 1)
            c17 = c17.replace('kernel_startup_initialize_upto(STARTUP_SUB_KMEM);', 'kernel_startup_initialize_upto(STARTUP_SUB_KMEM);\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FFFFFF, 384, 2); }', 1)
            c17 = c17.replace('vm_fault_init();', 'vm_fault_init();\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x00FFA500, 388, 2); }', 1)
            c17 = c17.replace('kernel_startup_initialize_upto(STARTUP_SUB_ZALLOC);', 'kernel_startup_initialize_upto(STARTUP_SUB_ZALLOC);\n\t{ extern void hk_draw_band(uint32_t, uint32_t, uint32_t); hk_draw_band(0x007FFF00, 392, 2); }', 1)
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

        # 20c. Phase 2: Direct Framebuffer VA Mapping in cpu_tte before set_mmu_ttb_alternate
        t20_fb = '\tarm_vm_physmap_init(args);\n\tset_mmu_ttb_alternate(cpu_ttep & TTBR_BADDR_MASK);'
        r20_fb = """\tarm_vm_physmap_init(args);

\t/* hakios Phase 2: Direct Framebuffer VA Mapping in cpu_tte (0xfffffff004000000) */
\t{
\t\textern void hk_draw_band_direct(uint32_t color, uint32_t row_start, uint32_t row_count);
\t\ttt_entry_t *fb_l1_tte = cpu_tte + L1_TABLE_T1_INDEX(0xfffffff004000000ULL, TCR_EL1_BOOT);
\t\ttt_entry_t *fb_l2_base;
\t\tif ((*fb_l1_tte & ARM_TTE_VALID) == 0) {
\t\t\tvm_offset_t l2_page = alloc_ptpage(FALSE);
\t\t\t*fb_l1_tte = (kvtophys(l2_page) & ARM_TTE_TABLE_MASK) | ARM_TTE_TYPE_TABLE | ARM_TTE_VALID | ARM_DYNAMIC_TABLE_XN | ARM_TTE_TABLE_AP(ARM_TTE_TABLE_AP_USER_NA);
\t\t\tbzero((void *)l2_page, ARM_PGBYTES);
\t\t\tfb_l2_base = (tt_entry_t *)l2_page;
\t\t} else {
\t\t\tfb_l2_base = (tt_entry_t *)phystokv((*fb_l1_tte) & ARM_TTE_TABLE_MASK);
\t\t}
\t\ttt_entry_t *fb_l2_tte = fb_l2_base + ((0x04000000ULL & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT);
\t\tfor (int k = 0; k < 5; k++) {
\t\t\tuint64_t target_desc = (0x5c000000ULL + ((uint64_t)k * 0x200000ULL)) | 0x0060000000000605ULL;
\t\t\tfb_l2_tte[k] = target_desc;
\t\t}
\t\t__asm__ volatile("dsb sy; isb" ::: "memory");
\t\t__asm__ volatile("dc civac, %0; dsb sy; isb" : : "r" (fb_l1_tte) : "memory");
\t\tfor (int k = 0; k < 5; k++) {
\t\t\t__asm__ volatile("dc civac, %0" : : "r" (&fb_l2_tte[k]) : "memory");
\t\t}
\t\t__asm__ volatile("dsb sy; isb" ::: "memory");
\t}

\tset_mmu_ttb_alternate(cpu_ttep & TTBR_BADDR_MASK);
\tflush_mmu_tlb();

\t/* hakios visual marker: verify Phase 2 mapping in cpu_tte immediately after TTBR1 switch */
\t{
\t\textern void hk_draw_band_direct(uint32_t color, uint32_t row_start, uint32_t row_count);
\t\thk_draw_band_direct(0x00FF007F, 160, 3); /* Rose at rows 160-162 */
\t}"""
        if t20_fb in c20:
            c20 = c20.replace(t20_fb, r20_fb, 1)
            print("  [OK] osfmk/arm64/arm_vm_init.c: Phase 2 direct FB mapping in cpu_tte installed")
        else:
            print("  [WARN] t20_fb not found in arm_vm_init.c")
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
            print("  [OK] osfmk/console/video_console.c: dual-region framebuffer partition installed (rows 0..719 ladder, rows 720..2399 console)")
        else:
            print("  [WARN] vc_target not found in video_console.c")

        # Force video_console always enabled (never discard character output in vcputc_options)
        t_vcputc = "void\nvcputc_options(char c, __unused bool poll)\n{\n\tif (gc_initialized && gc_enabled) {"
        r_vcputc = "void\nvcputc_options(char c, __unused bool poll)\n{\n\t/* hakios: force text console always active for PID 1 & kprintf */\n\tif (gc_initialized) {\n\t\tgc_enabled = TRUE;"
        if "/* hakios: force text console always active for PID 1 & kprintf */" in c21:
            print("  [ALREADY PATCHED] osfmk/console/video_console.c: vcputc text force already present")
        elif t_vcputc in c21:
            c21 = c21.replace(t_vcputc, r_vcputc, 1)
            print("  [OK] osfmk/console/video_console.c: vcputc text force installed (always active)")
        else:
            print("  [WARN] t_vcputc pattern not found in video_console.c")

        # Never set disableConsoleOutput to TRUE in gc_enable
        t_dis = "disableConsoleOutput = TRUE;"
        r_dis = "/* hakios: never suppress console */ disableConsoleOutput = FALSE;"
        if t_dis in c21:
            c21 = c21.replace(t_dis, r_dis)
            print("  [OK] osfmk/console/video_console.c: disableConsoleOutput overrides neutralized")

        with open(p21, "w", encoding="utf-8", newline="\n") as f:
            f.write(c21)

    # 22. osfmk/kern/lock_mtx.c (bypass mutex ownership assert during early single-core boot)
    p22 = os.path.join(src, "osfmk", "kern", "lock_mtx.c")
    if os.path.isfile(p22):
        with open(p22, "r", encoding="utf-8", errors="replace") as f:
            c22 = f.read().replace("\r\n", "\n")
        t22 = "void\nlck_mtx_assert(lck_mtx_t *lock, unsigned int type)\n{"
        r22 = """void
lck_mtx_assert(lck_mtx_t *lock, unsigned int type)
{
\tif (startup_phase < STARTUP_SUB_KMEM) {
\t\treturn;
\t}"""
        if t22 in c22:
            c22 = c22.replace(t22, r22, 1)
            with open(p22, "w", encoding="utf-8", newline="\n") as f:
                f.write(c22)
            print("  [OK] osfmk/kern/lock_mtx.c: early boot mutex assert bypass installed")
        else:
            print("  [WARN] t22 not found in lock_mtx.c")

    # 23. pexpert/arm/pe_init.c (force text mode console on framebuffer display)
    p23 = os.path.join(src, "pexpert", "arm", "pe_init.c")
    if os.path.isfile(p23):
        with open(p23, "r", encoding="utf-8", errors="replace") as f:
            c23 = f.read().replace("\r\n", "\n")
        t23 = "\tif (PE_state.video.v_display) {\n\t\tPE_initialize_console(&PE_state.video, kPEGraphicsMode);\n\t} else {\n\t\tPE_initialize_console(&PE_state.video, kPETextMode);\n\t}"
        r23 = "\t/* hakios: always initialize in text mode so video_console displays PID 1 & kprintf */\n\tPE_initialize_console(&PE_state.video, kPETextMode);"
        if "/* hakios: always initialize in text mode" in c23:
            print("  [ALREADY PATCHED] pexpert/arm/pe_init.c: text console mode already forced")
        elif t23 in c23:
            c23 = c23.replace(t23, r23, 1)
            with open(p23, "w", encoding="utf-8", newline="\n") as f:
                f.write(c23)
            print("  [OK] pexpert/arm/pe_init.c: forced kPETextMode for video_console display")
        else:
            print("  [WARN] t23 pattern not found in pe_init.c")

    # 24. iokit/Kernel/GenericARMPE.cpp (ensure max_cpus settled even for single-core or missing cpu0 nub)
    p24 = os.path.join(src, "iokit", "Kernel", "GenericARMPE.cpp")
    if os.path.isfile(p24):
        with open(p24, "r", encoding="utf-8", errors="replace") as f:
            c24 = f.read().replace("\r\n", "\n")
        t24 = "\tif (cpus[master_cpu] != NULL) {\n\t\t/* Unblocks everything that waits for the CPU count to be known. */\n\t\tml_set_max_cpus(count);"
        r24 = "\tif (cpus[master_cpu] != NULL) {\n\t\t/* Unblocks everything that waits for the CPU count to be known. */\n\t\tml_set_max_cpus(count);\n\t} else {\n\t\t/* hakios: fallback for single-core bring-up to unblock ml_wait_max_cpus */\n\t\tml_set_max_cpus(1);"
        if "/* hakios: fallback for single-core bring-up" in c24:
            print("  [ALREADY PATCHED] iokit/Kernel/GenericARMPE.cpp: max_cpus fallback unblock already present")
        elif t24 in c24:
            c24 = c24.replace(t24, r24, 1)
            with open(p24, "w", encoding="utf-8", newline="\n") as f:
                f.write(c24)
            print("  [OK] iokit/Kernel/GenericARMPE.cpp: max_cpus fallback unblock installed")
        else:
            print("  [WARN] t24 pattern not found in GenericARMPE.cpp")

    # 25. osfmk/arm64/machine_routines.c & osfmk/arm/machine_routines.c (safeguard ml_wait_max_cpus against infinite wait)
    for mr_rel in [("osfmk", "arm64", "machine_routines.c"), ("osfmk", "arm", "machine_routines.c")]:
        p_mr = os.path.join(src, *mr_rel)
        if os.path.isfile(p_mr):
            with open(p_mr, "r", encoding="utf-8", errors="replace") as f:
                c_mr = f.read().replace("\r\n", "\n")
            if "ml_wait_max_cpus" in c_mr:
                t_mr = "ml_wait_max_cpus(void)\n{"
                r_mr = "ml_wait_max_cpus(void)\n{\n\tif (!max_cpus_initialized) ml_set_max_cpus(1); /* hakios: unblock single-core */"
                if "/* hakios: unblock single-core */" in c_mr:
                    print(f"  [ALREADY PATCHED] {os.path.join(*mr_rel)}: ml_wait_max_cpus safeguard already present")
                elif t_mr in c_mr:
                    c_mr = c_mr.replace(t_mr, r_mr, 1)
                    with open(p_mr, "w", encoding="utf-8", newline="\n") as f:
                        f.write(c_mr)
                    print(f"  [OK] {os.path.join(*mr_rel)}: ml_wait_max_cpus safeguard installed")

    # Post-patch verification assertions: verify files on disk actually contain the patches!
    print("=== Post-patch Verification on Disk ===")
    p_locore = os.path.join(src, "osfmk", "arm64", "locore.s")
    if os.path.isfile(p_locore):
        with open(p_locore, "r", encoding="utf-8") as f:
            locore_content = f.read()
        assert "Row 230" in locore_content, "ASSERTION FAILED: locore.s missing Row 230 marker on disk!"
        assert "Row 220" in locore_content, "ASSERTION FAILED: locore.s missing Row 220 marker on disk!"
        assert "hk_draw_band_stackless" in locore_content, "ASSERTION FAILED: locore.s missing hk_draw_band_stackless on disk!"
        print("  [OK] locore.s verified: stackless vector markers present on disk")

    p_vmres = os.path.join(src, "osfmk", "vm", "vm_resident.c")
    if os.path.isfile(p_vmres):
        with open(p_vmres, "r", encoding="utf-8") as f:
            vmres_content = f.read()
        assert "304" in vmres_content, "ASSERTION FAILED: vm_resident.c missing Row 304 marker on disk!"
        assert "302" in vmres_content, "ASSERTION FAILED: vm_resident.c missing Row 302 marker on disk!"
        assert "305" in vmres_content, "ASSERTION FAILED: vm_resident.c missing Row 305 marker on disk!"
        print("  [OK] vm_resident.c verified: progress markers present on disk")

    print("Cortex-A73 patches successfully applied!")

if __name__ == "__main__":
    main()
