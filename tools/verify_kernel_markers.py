#!/usr/bin/env python3
"""
verify_kernel_markers.py - Post-build Mach-O kernel disassembly & marker verification tool
Ensures that critical diagnostic markers exist in the compiled kernel binary before packaging.
Specifically verifies:
1. hk_draw_band_direct exists and contains NO msr TTBR0_EL1, NO msr SCTLR_EL1, NO tlbi.
2. hk_draw_band_direct contains AT S1E1W memory guard.
3. Exception vectors call hk_draw_band_direct / hk_draw_band_stackless.
4. pmap_next_page exists (zero-overhead) and pmap_startup contains milestone markers.
"""

import sys
import os
import struct

def verify_macho(macho_path):
    print(f"=== Verifying Mach-O kernel markers in: {macho_path} ===")
    if not os.path.isfile(macho_path):
        print(f"ERROR: File not found: {macho_path}")
        return False

    with open(macho_path, 'rb') as f:
        hdr = f.read(32)
        magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack('<IIIIIIII', hdr)
        if magic != 0xfeedfacf:
            print(f"ERROR: Not a 64-bit Mach-O binary (magic={hex(magic)})")
            return False

        sections = []
        symtab_info = None

        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack('<II', f.read(8))
            cmd_data = f.read(cmdsize - 8)
            if cmd == 0x19: # LC_SEGMENT_64
                segname = cmd_data[:16].decode('utf-8', errors='ignore').rstrip('\x00')
                vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, sflags = struct.unpack('<QQQQIIII', cmd_data[16:56])
                sect_off = 56
                for _ in range(nsects):
                    sname = cmd_data[sect_off:sect_off+16].decode('utf-8', errors='ignore').rstrip('\x00')
                    smseg = cmd_data[sect_off+16:sect_off+32].decode('utf-8', errors='ignore').rstrip('\x00')
                    saddr, ssize, soff = struct.unpack('<QQI', cmd_data[sect_off+32:sect_off+52])
                    sections.append((sname, smseg, saddr, ssize, soff))
                    sect_off += 80
            elif cmd == 0x2: # LC_SYMTAB
                symoff, nsyms, stroff, strsize = struct.unpack('<IIII', cmd_data[:16])
                symtab_info = (symoff, nsyms, stroff, strsize)

    if not symtab_info:
        print("ERROR: LC_SYMTAB not found!")
        return False

    symoff, nsyms, stroff, strsize = symtab_info
    symbols = {}

    with open(macho_path, 'rb') as f:
        f.seek(stroff)
        strtab = f.read(strsize)

        f.seek(symoff)
        for _ in range(nsyms):
            n_strx, n_type, n_sect, n_desc, n_value = struct.unpack('<IBBHI', f.read(12) + f.read(4)) # n_value is 64-bit on arm64?
            # Wait, nlist_64 is: uint32 n_strx (4), uint8 n_type (1), uint8 n_sect (1), uint16 n_desc (2), uint64 n_value (8) = 16 bytes
            # Correct unpacking:
            pass

    # Unpack nlist_64 correctly
    with open(macho_path, 'rb') as f:
        f.seek(stroff)
        strtab = f.read(strsize)

        f.seek(symoff)
        for _ in range(nsyms):
            entry_bytes = f.read(16)
            if len(entry_bytes) < 16:
                break
            n_strx, n_type, n_sect, n_desc, n_value = struct.unpack('<IBBHQ', entry_bytes)
            if n_strx < len(strtab):
                end = strtab.find(b'\x00', n_strx)
                if end != -1:
                    name = strtab[n_strx:end].decode('utf-8', errors='ignore')
                    symbols[name] = n_value

    def va_to_off(va):
        for sname, smseg, saddr, ssize, soff in sections:
            if saddr <= va < saddr + ssize:
                return soff + (va - saddr)
        return None

    # Check 1: hk_draw_band_direct must exist
    hk_direct_va = symbols.get('_hk_draw_band_direct') or symbols.get('hk_draw_band_direct')
    if not hk_direct_va:
        print("ERROR: _hk_draw_band_direct symbol not found!")
        return False
    print(f"  [OK] Symbol _hk_draw_band_direct found at {hex(hk_direct_va)}")

    # Check 1b: Verify hk_draw_band_direct implementation (NO TTBR0 switch, NO SCTLR toggle, has AT guard)
    off_direct = va_to_off(hk_direct_va)
    with open(macho_path, 'rb') as f:
        f.seek(off_direct)
        code_direct = f.read(0xa0)

    found_at_guard = False
    for i in range(0, len(code_direct), 4):
        insn = struct.unpack('<I', code_direct[i:i+4])[0]
        # msr TTBR0_EL1 -> 0xd5182000 | reg
        if (insn & 0xFFFFFFE0) == 0xD5182000:
            print("ERROR: hk_draw_band_direct contains forbidden msr TTBR0_EL1!")
            return False
        # msr SCTLR_EL1 -> 0xd5181000 | reg
        if (insn & 0xFFFFFFE0) == 0xD5181000:
            print("ERROR: hk_draw_band_direct contains forbidden msr SCTLR_EL1!")
            return False
        # tlbi -> 0xd508...
        if (insn & 0xFFF00000) == 0xD5080000 and (insn & 0x00008000):
            print("ERROR: hk_draw_band_direct contains forbidden tlbi instruction!")
            return False
        # at s1e1w, Xt -> 0xd5087820 | reg (or s1e1r -> 0xd5087800 | reg)
        if (insn & 0xFFFFFFE0) in (0xD5087800, 0xD5087820):
            found_at_guard = True

    if not found_at_guard:
        print("ERROR: hk_draw_band_direct does NOT contain AT S1E1W guard instruction!")
        return False
    print("  [OK] hk_draw_band_direct verified (direct VA, no MMU toggle, AT guard present)")

    # Check 2: el1_sp0_synchronous_vector_long must branch to drawer
    hk_stackless_va = symbols.get('_hk_draw_band_stackless') or symbols.get('hk_draw_band_stackless') or hk_direct_va
    vec_va = symbols.get('el1_sp0_synchronous_vector_long') or symbols.get('_el1_sp0_synchronous_vector_long')
    if not vec_va:
        print("ERROR: el1_sp0_synchronous_vector_long symbol not found!")
        return False
    print(f"  [OK] Symbol el1_sp0_synchronous_vector_long found at {hex(vec_va)}")

    off = va_to_off(vec_va)
    with open(macho_path, 'rb') as f:
        f.seek(off)
        code = f.read(0x60)

    found_branch = False
    for i in range(0, len(code), 4):
        insn = struct.unpack('<I', code[i:i+4])[0]
        if (insn & 0xFC000000) == 0x94000000: # BL
            imm26 = insn & 0x03FFFFFF
            if imm26 & 0x02000000:
                imm26 -= 0x04000000
            pc = vec_va + i
            dest = pc + (imm26 << 2)
            if dest in (hk_direct_va, hk_stackless_va):
                found_branch = True
                print(f"  [OK] BL drawer confirmed inside el1_sp0_synchronous_vector_long at {hex(pc)}!")
                break

    if not found_branch:
        print("ERROR: el1_sp0_synchronous_vector_long does NOT branch to diagnostic drawer!")
        return False

    # Check 3: _pmap_next_page must exist and _pmap_startup must contain milestone markers
    pnext_va = symbols.get('_pmap_next_page') or symbols.get('pmap_next_page')
    if not pnext_va:
        print("ERROR: _pmap_next_page symbol not found!")
        return False
    print(f"  [OK] Symbol _pmap_next_page found at {hex(pnext_va)}")

    pstart_va = symbols.get('_pmap_startup') or symbols.get('pmap_startup')
    if not pstart_va:
        print("ERROR: _pmap_startup symbol not found!")
        return False
    print(f"  [OK] Symbol _pmap_startup found at {hex(pstart_va)}")

    off_ps = va_to_off(pstart_va)
    with open(macho_path, 'rb') as f:
        f.seek(off_ps)
        code_ps = f.read(0x400)

    found_marker = False
    for i in range(0, len(code_ps), 4):
        insn = struct.unpack('<I', code_ps[i:i+4])[0]
        # Check for mov w1, #304 (0x130) or mov w1, #340 (0x154) or mov w1, #302 (0x12E)
        if (insn & 0xFFE0001F) == 0x52800001:
            imm = (insn >> 5) & 0xFFFF
            if imm in (302, 304, 340, 342, 358):
                found_marker = True
                print(f"  [OK] Milestone marker Row {imm} instruction found in _pmap_startup at offset +{hex(i)}!")
                break

    if not found_marker:
        print("ERROR: _pmap_startup does NOT contain diagnostic milestone markers!")
        return False

    print("=== All kernel diagnostic marker verifications PASSED! ===")
    return True

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: verify_kernel_markers.py <mach_binary>")
        sys.exit(1)
    if not verify_macho(sys.argv[1]):
        sys.exit(1)
    sys.exit(0)
