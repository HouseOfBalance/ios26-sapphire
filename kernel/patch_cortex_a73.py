#!/usr/bin/env python3
import os, sys

def main():
    if len(sys.argv) < 2:
        print("Usage: patch_cortex_a73.py <xnu_src_dir>")
        sys.exit(1)
    
    src = sys.argv[1]
    print(f"Applying Cortex-A73 PAC sysreg stubs and visual FB markers to {src}...")

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
            with open(p1, "w", encoding="utf-8") as f:
                f.write(c)
            print("  [OK] osfmk/arm/arm_init.c")
        else:
            print("  [WARN] targets not found in arm_init.c")

    # 2. osfmk/arm64/pac_std_asm.h
    p2 = os.path.join(src, "osfmk", "arm64", "pac_std_asm.h")
    if os.path.isfile(p2):
        with open(p2, "r", encoding="utf-8", errors="replace") as f:
            c = f.read()
        
        load_keys_old = """.macro PAC_STD_LOAD_KEYS base, lo, hi
	PAC_STD_LOAD_KEY \\base, PAC_STD_KEY_IA, \\lo, \\hi
	msr	APIAKeyLo_EL1, \\lo
	msr	APIAKeyHi_EL1, \\hi
	PAC_STD_LOAD_KEY \\base, PAC_STD_KEY_IB, \\lo, \\hi
	msr	APIBKeyLo_EL1, \\lo
	msr	APIBKeyHi_EL1, \\hi
	PAC_STD_LOAD_KEY \\base, PAC_STD_KEY_DA, \\lo, \\hi
	msr	APDAKeyLo_EL1, \\lo
	msr	APDAKeyHi_EL1, \\hi
	PAC_STD_LOAD_KEY \\base, PAC_STD_KEY_DB, \\lo, \\hi
	msr	APDBKeyLo_EL1, \\lo
	msr	APDBKeyHi_EL1, \\hi
	PAC_STD_LOAD_KEY \\base, PAC_STD_KEY_GA, \\lo, \\hi
	msr	APGAKeyLo_EL1, \\lo
	msr	APGAKeyHi_EL1, \\hi
.endm"""

        load_keys_new = """.macro PAC_STD_LOAD_KEYS base, lo, hi
	// hakios: Bengal Cortex-A73 lacks hardware PAC sysregs; stub out
.endm"""

        load_a_old = """.macro PAC_STD_LOAD_A_KEYS base, id, lo, hi, t1, t2, t3, t4
	PAC_STD_DERIVE_KEY \\base, PAC_STD_KEY_IA, \\id, \\lo, \\hi, \\t1, \\t2, \\t3, \\t4
	msr	APIAKeyLo_EL1, \\lo
	msr	APIAKeyHi_EL1, \\hi
	msr	APDAKeyLo_EL1, \\lo
	msr	APDAKeyHi_EL1, \\hi
.endm"""

        load_a_new = """.macro PAC_STD_LOAD_A_KEYS base, id, lo, hi, t1, t2, t3, t4
	// hakios: Bengal Cortex-A73 lacks hardware PAC sysregs; stub out
.endm"""

        load_b_old = """.macro PAC_STD_LOAD_B_KEYS base, id, lo, hi, t1, t2, t3, t4
	PAC_STD_DERIVE_KEY \\base, PAC_STD_KEY_IB, \\id, \\lo, \\hi, \\t1, \\t2, \\t3, \\t4
	msr	APIBKeyLo_EL1, \\lo
	msr	APIBKeyHi_EL1, \\hi
	msr	APDBKeyLo_EL1, \\lo
	msr	APDBKeyHi_EL1, \\hi
.endm"""

        load_b_new = """.macro PAC_STD_LOAD_B_KEYS base, id, lo, hi, t1, t2, t3, t4
	// hakios: Bengal Cortex-A73 lacks hardware PAC sysregs; stub out
.endm"""

        # Normalize newlines
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

        start_old = """\tmov\t\tx20, x0
\tmov\t\tx21, #0
\tLOADER_CHECKPOINT 1"""

        start_new = """\tmov\t\tx20, x0
\tmov\t\tx21, #0
\t// hakios visual marker: yellow bar at top of screen (0x5c000000, 40 rows)
\tmovz\tx9, #0x5c00, lsl #16
\tmovz\tx10, #0xff00
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0xff00, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
\tmov\t\tw11, #10800
1:
\tstp\tx10, x10, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t1b
\tLOADER_CHECKPOINT 1"""

        assert start_old in c_norm, "start_first_cpu checkpoint 1 not found in start.s"
        c_norm = c_norm.replace(start_old, start_new)

        exc_old = """\tadr\t\tx9, Lloader_stub_slot
\tldr\t\tx9, [x9, #8]
\tcbz\t\tx9, .
\tbr\t\tx9"""

        exc_new = """\tadr\t\tx9, Lloader_stub_slot
\tldr\t\tx9, [x9, #8]
\tcbnz\tx9, 1f
\tmovz\tx9, #0xa300
\tmovk\tx9, #0x5c02, lsl #16
\tmovz\tx10, #0x0000
\tmovk\tx10, #0x00ff, lsl #16
\tmovk\tx10, #0x0000, lsl #32
\tmovk\tx10, #0x00ff, lsl #48
\tmov\t\tw11, #10800
2:
\tstp\tx10, x10, [x9], #16
\tsubs\tw11, w11, #1
\tb.ne\t2b
\tb\t\t.
1:
\tbr\t\tx9"""

        assert exc_old in c_norm, "Lloader_early_exception stub slot not found in start.s"
        c_norm = c_norm.replace(exc_old, exc_new)

        with open(p4, "w", encoding="utf-8", newline="\n") as f:
            f.write(c_norm)
        print("  [OK] osfmk/arm64/start.s")

    print("Cortex-A73 patches successfully applied!")

if __name__ == "__main__":
    main()
