#!/usr/bin/env python3
"""patch_vm16_panicmsg.py <xnu_src_dir>  (idempotent)
Print the panic() message on the framebuffer console at the entry of
panic_trap_to_debugger(). The EL1 dump alone only shows the 0xe7ffdeff panic-trap
opcode (decoded as UNDEF), hiding the real panic string."""
import os, sys
src = sys.argv[1] if len(sys.argv) > 1 else "."
p = os.path.join(src, "osfmk", "kern", "debug.c")
if not os.path.isfile(p):
    print("  [WARN] debug.c not found"); sys.exit(0)
c = open(p, encoding="utf-8", errors="replace").read().replace("\r\n", "\n")
if "hk-panicmsg" in c:
    print("  [OK] already present"); sys.exit(0)
anchor = ("uint64_t panic_options_mask, void *panic_data_ptr, unsigned long panic_caller, const char *panic_initiator)\n"
          "{\n#pragma clang diagnostic pop\n")
i = c.find(anchor)
if i < 0:
    print("  [WARN] panic_trap_to_debugger anchor not found"); sys.exit(0)
blk = ('\n\t/* hk-panicmsg: show the panic string on the console */\n'
       '\t{\n'
       '\t\textern void hk_con_log(const char *s);\n'
       '\t\tchar hk_pb[200];\n'
       '\t\thk_con_log("[PANIC] panic() called:");\n'
       '\t\tif (panic_format_str != NULL) {\n'
       '\t\t\tif (panic_args != NULL) {\n'
       '\t\t\t\tva_list hk_ap;\n'
       '\t\t\t\tva_copy(hk_ap, *panic_args);\n'
       '\t\t\t\tvsnprintf(hk_pb, sizeof(hk_pb), panic_format_str, hk_ap);\n'
       '\t\t\t\tva_end(hk_ap);\n'
       '\t\t\t\thk_pb[sizeof(hk_pb) - 1] = 0;\n'
       '\t\t\t\thk_con_log(hk_pb);\n'
       '\t\t\t} else {\n'
       '\t\t\t\thk_con_log(panic_format_str);\n'
       '\t\t\t}\n'
       '\t\t}\n'
       '\t}\n')
j = i + len(anchor)
c = c[:j] + blk + c[j:]
open(p, "w", encoding="utf-8", newline="\n").write(c)
print("  [OK] debug.c: panic message print added")