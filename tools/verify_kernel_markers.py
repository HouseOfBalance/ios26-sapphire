#!/usr/bin/env python3
"""
verify_kernel_markers.py - Post-build Mach-O kernel disassembly & marker verification tool
Ensures that critical diagnostic markers exist in the compiled kernel binary before packaging.
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
                vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, sflags = struct.unpack('<QQQQIIII', cmd_data[16:64])
                sec_offset = 64
                for _ in range(nsects):
                    sdata = cmd_data[sec_offset:sec_offset+80]
                    sname = sdata[:16].decode('utf-8', errors='ignore').rstrip('\x00')
                    smseg = sdata[16:32].decode('utf-8', errors='ignore').rstrip('\x00')
                    saddr, ssize, soff = struct.unpack('<QQI', sdata[32:52])
                    sections.append({'name': sname, 'seg': smseg, 'addr': saddr, 'size': ssize, 'off': soff})
                    sec_offset += 80
            elif cmd == 0x2: # LC_SYMTAB
                symoff, nsyms, stroff, strsize = struct.unpack('<IIII', cmd_data[:16])
                symtab_info = (symoff, nsyms, stroff, strsize)

        symbols = {}
        if symtab_info:
            symoff, nsyms, stroff, strsize = symtab_info
            f.seek(stroff)
            strtab = f.read(strsize)
            f.seek(symoff)
            for _ in range(nsyms):
                sym_bytes = f.read(16)
                strx, stype, ssect, sdesc, svalue = struct.unpack('<IBBhQ', sym_bytes)
                if strx < len(strtab):
                    end = strtab.find(b'\x00', strx)
                    sname = strtab[strx:end].decode('utf-8', errors='ignore')
                    symbols[sname] = svalue

    def va_to_off(va):
        for s in sections:
            if s['addr'] <= va < s['addr'] + s['size']:
                return s['off'] + (va - s['addr'])
        return None

    # Check 1: hk_draw_band_stackless must exist
    hk_stackless_va = symbols.get('_hk_draw_band_stackless') or symbols.get('hk_draw_band_stackless')
    if not hk_stackless_va:
        print("ERROR: _hk_draw_band_stackless symbol not found!")
        return False
    print(f"  [OK] Symbol _hk_draw_band_stackless found at {hex(hk_stackless_va)}")

    # Check 2: el1_sp0_synchronous_vector_long must call hk_draw_band_stackless
    vec_va = symbols.get('el1_sp0_synchronous_vector_long') or symbols.get('_el1_sp0_synchronous_vector_long')
    if not vec_va:
        print("ERROR: el1_sp0_synchronous_vector_long symbol not found!")
        return False
    print(f"  [OK] Symbol el1_sp0_synchronous_vector_long found at {hex(vec_va)}")

    off = va_to_off(vec_va)
    with open(macho_path, 'rb') as f:
        f.seek(off)
        code = f.read(0x60) # first 24 instructions

    found_branch_to_stackless = False
    for i in range(0, len(code), 4):
        insn = struct.unpack('<I', code[i:i+4])[0]
        if (insn & 0xFC000000) == 0x94000000: # BL
            imm26 = insn & 0x03FFFFFF
            if imm26 & 0x02000000:
                imm26 -= 0x04000000
            pc = vec_va + i
            dest = pc + (imm26 << 2)
            if dest == hk_stackless_va:
                found_branch_to_stackless = True
                print(f"  [OK] BL hk_draw_band_stackless confirmed inside el1_sp0_synchronous_vector_long at {hex(pc)}!")
                break

    if not found_branch_to_stackless:
        print("ERROR: el1_sp0_synchronous_vector_long does NOT call hk_draw_band_stackless!")
        return False

    # Check 2b: el1_sp0_irq_vector_long must call hk_draw_band_stackless
    irq_va = symbols.get('el1_sp0_irq_vector_long') or symbols.get('_el1_sp0_irq_vector_long')
    if not irq_va:
        print("ERROR: el1_sp0_irq_vector_long symbol not found!")
        return False
    print(f"  [OK] Symbol el1_sp0_irq_vector_long found at {hex(irq_va)}")

    off_irq = va_to_off(irq_va)
    with open(macho_path, 'rb') as f:
        f.seek(off_irq)
        code_irq = f.read(0x60)

    found_irq_branch = False
    for i in range(0, len(code_irq), 4):
        insn = struct.unpack('<I', code_irq[i:i+4])[0]
        if (insn & 0xFC000000) == 0x94000000:
            imm26 = insn & 0x03FFFFFF
            if imm26 & 0x02000000:
                imm26 -= 0x04000000
            pc = irq_va + i
            dest = pc + (imm26 << 2)
            if dest == hk_stackless_va:
                found_irq_branch = True
                print(f"  [OK] BL hk_draw_band_stackless confirmed inside el1_sp0_irq_vector_long at {hex(pc)}!")
                break

    if not found_irq_branch:
        print("ERROR: el1_sp0_irq_vector_long does NOT call hk_draw_band_stackless!")
        return False

    # Check 3: _pmap_next_page must exist and contain Row 306
    pnext_va = symbols.get('_pmap_next_page') or symbols.get('pmap_next_page')
    if not pnext_va:
        print("ERROR: _pmap_next_page symbol not found!")
        return False
    print(f"  [OK] Symbol _pmap_next_page found at {hex(pnext_va)}")

    off_pn = va_to_off(pnext_va)
    with open(macho_path, 'rb') as f:
        f.seek(off_pn)
        code_pn = f.read(0x60)

    found_row306 = False
    for i in range(0, len(code_pn), 4):
        insn = struct.unpack('<I', code_pn[i:i+4])[0]
        # Check for mov w1, #306 (0x132) -> 52802641
        if (insn & 0xFFE0001F) == 0x52800001 and ((insn >> 5) & 0xFFFF) == 306:
            found_row306 = True
            print(f"  [OK] Row 306 instruction found in _pmap_next_page at offset +{hex(i)}!")
            break

    if not found_row306:
        print("ERROR: _pmap_next_page does NOT contain Row 306!")
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
