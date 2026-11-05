#!/usr/bin/env python3
"""
tools/patch_userland_pac.py

Offline Static Sanitizer for Userland ARM64E Mach-O Binaries & Dynamic Libraries.
Converts ARMv8.3 PAC instructions to native ARMv8.0 equivalents so launchd, dyld,
and test userland binaries run natively on Cortex-A73 Gold cores without causing
an EL1 trap storm.

Conversions:
  - retab / retaa   -> ret (0xd65f03c0)
  - eretab / eretaa -> eret (0xd69f03e0)
  - braa / brab     -> br Xn
  - blraa / blrab   -> blr Xn
  - pac-* / aut-*   -> NOP (0xd503201f)
  - pacga           -> mov Xd, xzr
  - ldraa / ldrab   -> ldr / ldur / ldr_pre
  - ldapr / ldapur  -> ldar / ldur
  - stlur           -> stur
  - brk #0xc47x     -> NOP (neutralize software PAC failure assertions)
"""

import sys
import os
import struct

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'kernel'))
from patch_kernel_pac import decode_and_patch

def patch_binary(path):
    if not os.path.isfile(path):
        print(f"[-] File not found: {path}")
        return False

    with open(path, 'rb') as f:
        data = bytearray(f.read())

    if len(data) < 32:
        return False

    magic, = struct.unpack_from('<I', data, 0)
    if magic != 0xfeedfacf:
        print(f"[-] Not a 64-bit Mach-O binary: {path} (magic 0x{magic:08x})")
        return False

    ncmds, sizeofcmds = struct.unpack_from('<II', data, 16)
    off = 32
    total_patched = 0
    stats = {}

    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', data, off)
        if cmd == 0x19: # LC_SEGMENT_64
            segname = data[off+8:off+24].split(b'\0')[0].decode('latin1')
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', data, off+24)
            maxprot, initprot = struct.unpack_from('<2I', data, off+56)
            if (initprot & 4) != 0 and filesize > 0: # Executable segment (__TEXT_EXEC / __TEXT)
                seg_patched = 0
                for pos in range(fileoff, fileoff + filesize, 4):
                    if pos + 4 <= len(data):
                        word, = struct.unpack_from('<I', data, pos)
                        new_insn, name = decode_and_patch(word)
                        if new_insn is not None:
                            struct.pack_into('<I', data, pos, new_insn)
                            stats[name] = stats.get(name, 0) + 1
                            total_patched += 1
                            seg_patched += 1
                if seg_patched > 0:
                    print(f"  [{segname}] Patched {seg_patched} PAC/RCpc instructions")
        off += cmdsize

    with open(path, 'wb') as f:
        f.write(data)

    print(f"[OK] {os.path.basename(path)}: Total {total_patched} instructions neutralized to ARMv8.0:")
    for k, v in sorted(stats.items(), key=lambda x: -x[1]):
        print(f"       {k:12}: {v}")
    return True

def main():
    if len(sys.argv) < 2:
        print("Usage: patch_userland_pac.py <binary_or_directory>")
        sys.exit(1)

    target = sys.argv[1]
    if os.path.isfile(target):
        patch_binary(target)
    elif os.path.isdir(target):
        print(f"Scanning directory for userland Mach-O binaries: {target}")
        for root, dirs, files in os.walk(target):
            for file in files:
                p = os.path.join(root, file)
                try:
                    with open(p, 'rb') as f:
                        hdr = f.read(4)
                    if hdr == b'\xcf\xfa\xed\xfe':
                        patch_binary(p)
                except:
                    pass
    print("Userland PAC offline pre-patching complete.")

if __name__ == '__main__':
    main()
