import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt

root, _ = adt.parse(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'sapphire.adt'), 'rb').read())

def find_node(parent, name):
    for c in parent.children:
        if c.name == name:
            return c
    return None

# 1. Framebuffer
fbcon = find_node(root, 'sweet-fbcon')
assert fbcon is not None, 'sweet-fbcon missing'
fb_base, fb_sz = struct.unpack('<QQ', fbcon.prop('reg'))
w, = struct.unpack('<I', fbcon.prop('width'))
h, = struct.unpack('<I', fbcon.prop('height'))
stride, = struct.unpack('<I', fbcon.prop('stride'))
assert fb_base == 0x5c000000, f'FB base {hex(fb_base)} != 0x5c000000'
print(f'CHECK 1: Framebuffer at {hex(fb_base)} ({w}x{h}, stride={stride}) - OK')

# 2. GICv3
arm_io = find_node(root, 'arm-io')
assert arm_io is not None, 'arm-io missing'
gic = find_node(arm_io, 'gic')
assert gic is not None, 'gic missing'
gicd_off, gicd_sz, gicr_off, gicr_sz = struct.unpack('<QQQQ', gic.prop('reg'))
soc_base = 0x01000000
gicd = soc_base + gicd_off
gicr = soc_base + gicr_off
assert gicd == 0x0f200000, f'GICD {hex(gicd)} != 0x0f200000'
assert gicr == 0x0f300000, f'GICR {hex(gicr)} != 0x0f300000'
print(f'CHECK 2: GICv3 at GICD={hex(gicd)} (size {hex(gicd_sz)}), GICR={hex(gicr)} (size {hex(gicr_sz)}) - OK')

# 3. UFS
ufs = find_node(root, 'sweet-ufs')
assert ufs is not None, 'sweet-ufs missing'
ufs_base, ufs_sz = struct.unpack('<QQ', ufs.prop('reg'))
assert ufs_base == 0x04804000, f'UFS base {hex(ufs_base)} != 0x04804000'
print(f'CHECK 3: UFS 2.2 at {hex(ufs_base)} (size {hex(ufs_sz)}) - OK')

# 4. UART
uart0 = find_node(arm_io, 'uart0')
assert uart0 is not None, 'uart0 missing'
uart_off, uart_sz = struct.unpack('<QQ', uart0.prop('reg'))
uart_base = soc_base + uart_off
assert uart_base == 0x04a90000, f'UART base {hex(uart_base)} != 0x04a90000'
sweet_uart = find_node(root, 'sweet-uart')
assert sweet_uart is not None, 'sweet-uart missing'
suart_base, suart_sz = struct.unpack('<QQ', sweet_uart.prop('reg'))
assert suart_base == 0x04a90000, f'sweet-uart base {hex(suart_base)} != 0x04a90000'
print(f'CHECK 4: UART at {hex(uart_base)} (arm-io/uart0 + sweet-uart) - OK')

# 5. CPUs
cpus = find_node(root, 'cpus')
assert cpus is not None, 'cpus missing'
assert len(cpus.children) == 8, f'CPU count {len(cpus.children)} != 8'
gold_mpidrs = []
silver_mpidrs = []
for cpu in cpus.children:
    reg, = struct.unpack('<I', cpu.prop('reg'))
    ctype = cpu.prop('cluster-type')
    freq, = struct.unpack('<I', cpu.prop('clock-frequency'))
    if reg >= 0x100:
        gold_mpidrs.append(hex(reg))
    else:
        silver_mpidrs.append(hex(reg))
    print(f'  CPU {cpu.name}: reg={hex(reg)}, freq={freq // 1000000}MHz')

assert gold_mpidrs == ['0x100', '0x101', '0x102', '0x103'], f'Gold MPIDRs unexpected: {gold_mpidrs}'
assert silver_mpidrs == ['0x0', '0x1', '0x2', '0x3'], f'Silver MPIDRs unexpected: {silver_mpidrs}'
print(f'CHECK 5: 4 Gold cores ({gold_mpidrs}) + 4 Silver cores ({silver_mpidrs}) - OK')

# 6. Device Identity & Model Alignment (iOS 26 / HakiOS)
model_bytes = root.prop('model')
assert model_bytes is not None, 'model property missing in root'
model_str = model_bytes.decode('utf-8', errors='ignore').rstrip('\0')
assert model_str == 'iPhone14,7', f'Unexpected model: {model_str}'
compat_bytes = root.prop('compatible')
assert b'xiaomi,sapphire' in compat_bytes, 'xiaomi,sapphire missing in compatible'
chosen = find_node(root, 'chosen')
assert chosen is not None, 'chosen missing'
bid_bytes = chosen.prop('board-id')
assert bid_bytes is not None, 'board-id missing in chosen'
bid, = struct.unpack('<I', bid_bytes)
assert bid == 24, f'board-id {bid} != 24 (D27AP for iPhone14,7)'
print(f'CHECK 6: Device identity model={model_str}, board-id={bid} (0x{bid:02x}), compatible matched - OK')

print('ALL VERIFICATIONS PASSED SUCCESSFULLY!')
