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
\thk_draw_band(0x0032CD32, 110, 10);  /* hakios visual marker: Lime at rows 110-119 (past PE_init_platform) */"""

        target_boot = """\tthread->machine.preemption_count = 0;
\tloader_checkpoint(44);
\tcpu_bootstrap();
\tloader_checkpoint(13);"""

        replacement_boot = """\tthread->machine.preemption_count = 0;
\tloader_checkpoint(44);
\thk_draw_band(0x00FF69B4, 120, 10);  /* hakios visual marker: Pink at rows 120-129 (preemption 0) */
\tcpu_bootstrap();
\tloader_checkpoint(13);
\thk_draw_band(0x00FFD700, 130, 10);  /* hakios visual marker: Gold at rows 130-139 (past cpu_bootstrap) */"""

        target_rt = """\trtclock_early_init();
\tloader_checkpoint(14);"""

        replacement_rt = """\trtclock_early_init();
\tloader_checkpoint(14);
\thk_draw_band(0x0087CEEB, 140, 10);  /* hakios visual marker: Sky Blue at rows 140-149 (past rtclock_early_init) */"""

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
            with open(p3, "w", encoding="utf-8", newline="\n") as f:
                f.write(c_norm)
            print("  [OK] osfmk/arm64/sleh.c")
        else:
            print("  [WARN] sleh target not found in sleh.c")

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

\t// Draw RED band at rows 110-119 (0x5c074740)
\tmovz\tx10, #0x4740
\tmovk\tx10, #0x5c07, lsl #16
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
 */
\t.text
\t.align 2
\t.globl EXT(hk_draw_band)
LEXT(hk_draw_band)
\tstp\t\tx29, x30, [sp, #-48]!
\tstp\t\tx19, x20, [sp, #16]
\tstp\t\tx21, x22, [sp, #32]

\tand\t\tx0, x0, #0xffffffff
\torr\t\tx19, x0, x0, lsl #32

\tmov\t\tx20, #4320
\tmul\t\tx20, x1, x20
\tmovz\tx21, #0x5c00, lsl #16
\tadd\t\tx20, x20, x21

\tmov\t\tw21, #270
\tmul\t\tw21, w2, w21

\tmrs\t\tx22, SCTLR_EL1
\ttbz\t\tx22, #0, 1f

\tadr\t\tx10, 1f
\tadrp\tx11, EXT(loader_stub_slot)@page
\tadd\tx11, x11, EXT(loader_stub_slot)@pageoff
\tldr\tx11, [x11, #24]
\tsub\tx10, x10, x11
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

\tmsr\t\tSCTLR_EL1, x9
\tisb

\ttbz\t\tx22, #0, 3f

\tadr\t\tx10, 3f
\tadrp\tx12, EXT(loader_stub_slot)@page
\tadd\tx12, x12, EXT(loader_stub_slot)@pageoff
\tldr\tx12, [x12, #24]
\tadd\tx10, x10, x12
\tbr\t\tx10

3:
\tldp\t\tx21, x22, [sp, #32]
\tldp\t\tx19, x20, [sp, #16]
\tldp\t\tx29, x30, [sp], #48
\tret
"""
        c_norm = c_norm + hk_draw_band_code

        with open(p4, "w", encoding="utf-8", newline="\n") as f:
            f.write(c_norm)
        print("  [OK] osfmk/arm64/start.s: visual markers and hk_draw_band installed")

    print("Cortex-A73 patches successfully applied!")

if __name__ == "__main__":
    main()
