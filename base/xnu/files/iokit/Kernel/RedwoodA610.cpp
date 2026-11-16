/*
 * hakios.a610.cpp - the Adreno 610 (A6xx, chip id 0x06010001) of the SM6225 / sapphire.
 *
 * PORTED from ios-sapphire (iOS 16, xnu-8796) iokit/Kernel/arm/SapphireGpu.cpp, patches 0145, 0148, 0149, 0151-0154, 0161,
 * 0163-0165, 0204, 0205, 0212, 0215, 0239, 0329 (rebuilt by applying each patch's SapphireGpu.cpp hunks in order; 0317 had
 * no hunk for this file and 0332, 0333, 0338, 0353, 0355 are malformed as shipped and are NOT included).
 *
 * What changed for xnu-12377 / this tree, and nothing else:
 *   - no ARM64_BOARD_CONFIG_SWEET guard: the file is built by CONFIG_HAKIOS_A610 (drivers/gpu/conf);
 *   - firmware is read with hakios_ufs_read_partition() (was sweet_ufs_read_part());
 *   - sweet_dcache_clean/flush are FlushPoC_DcacheRegion (clean + invalidate to the point of coherency);
 *   - the RPM level vote (hakios_rpm_vote) is optional: without it the GPU runs at the level the bootloader left;
 *   - IOSurface import (SweetSurface) answers "unsupported": this tree's surfaces are MiniIOSurface's;
 *   - the frame-copy hooks (sapphire_gpu_ready/copy) stay but nothing in this tree's framebuffer calls them.
 * Register addresses are the SM6225's own and were NOT changed: kgsl 0x5900000, GPU CC 0x5990000, SMMU 0x59a0000.
 *
 * NOT built or run in this tree: compile it against xnu-12377 and run gpu=1, then gpu=2, one stage at a time, reading
 * the log (a read of the GPU block with its power or clocks off hangs the bus). The microcode and zap shader are not in
 * this repository: fill gpu_fw[] below with tools/fwtable-vendor.py (ios-sapphire) or carry them in the ADT.
 * The original header follows.
 */
/*
 * Adreno 610 v2 (A6xx, chip id 0x06010001) bring-up on sapphire.
 *
 * The phone has no GMU: Linux drives this GPU through a "GMU wrapper", so the host does the
 * power-up itself. The sequence, the register values and the traps below are topaz-woa's
 * TopazGpu (docs/P8_gpu.md, G1 done on the same SoC under Windows), redone here on XNU with
 * the pieces TopazModem already brings to this kernel: SCM calls into TrustZone, an identity
 * SMMU context bank, and RAM below 4 GB that the kernel does not manage.
 *
 * Staged by boot-arg gpu=N, because reading a GPU register while its power domain or its
 * clocks are off hangs the bus (the same trap as the WLAN copy engines before mission mode):
 *
 * gpu_slow=N waits N ms after each step so the persistent log catches up with it,
 * gpu_delay=N waits N seconds before starting so the live console over USB is already up,
 * gpu_div=N runs the core at GPLL0/N (2 = 300 MHz, 8 = 75 MHz), and gpu_smmu_early=1 puts the
 * identity context bank in before the GX domain. Those two default to what has run before:
 * a boot that dies with every clock up and nothing else to show for it has to be bisected one
 * variable at a time. gpu_irq=1 is the exception, because it only turns off something this
 * driver has no business leaving on: the GPU's interrupt, masked by default in the GIC before
 * anything is powered and at the GPU itself as soon as its registers answer.
 *
 *   gpu=1   clock tree only; never touches 0x5900000
 *   gpu=2   + power up, then the first GPU reads (RBBM_STATUS, CP_ALWAYS_ON_COUNTER)
 *   gpu=3   + zap shader through TrustZone, identity SMMU, SQE microcode, ring buffer,
 *             CP_ME_INIT, CP_SET_SECURE_MODE and a CP_MEM_WRITE the CPU reads back
 *   gpu=4   the same without the zap shader, leaving secure mode by writing
 *             RBBM_SECVID_TRUST_CNTL as Linux does when a device has no zap firmware
 *
 * Both need the microcode (a630_sqe.fw, which is in linux-firmware); gpu=3 also needs this
 * phone's signed zap shader. Neither is in this repository. The driver takes them from the
 * device tree when the boot image carries them (tools/mkadt.py --gpu-sqe / --gpu-zap-mdt /
 * --gpu-zap-seg), and otherwise from a stock partition, read only through
 * sweet_ufs_read_part, with gpu_fw[] below filled in by tools/fwtable-file.py.
 *
 * gpu=4 is the shortcut for a first test with only the open microcode. It is a gamble Linux
 * warns about: if the bootloader left the GPU in secure mode, writing that register is
 * refused and a permissions violation follows, so prefer gpu=3 once the zap is in the table.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>               /* IOService::getPlatform() (the msm service) */
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <pexpert/pexpert.h>
#include <libkern/crc.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/systm.h>                         /* copyin, copyout (the msm client) */
#include <pexpert/device_tree.h>

#define LOG(fmt, ...)       IOLog("SapphireGpu: " fmt "\n", ##__VA_ARGS__)

extern "C" void hakios_a610_init(void);
extern "C" int hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes);
/* ios-sapphire's name for it: 0 when read, EAGAIN while the root device is not up (here: ENXIO without the storage) */
static int
sweet_ufs_read_part(const char *want, uint64_t off, void *dst, uint64_t len)
{
	int err = hakios_ufs_read_partition(want, off, (uint8_t *)dst, len, NULL);

	return err == ENXIO ? EAGAIN : err;
}
extern "C" void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
#define sweet_dcache_clean(a, l) FlushPoC_DcacheRegion((a), (l))
#define sweet_dcache_flush(a, l) FlushPoC_DcacheRegion((a), (l))

/* ---------------------------------------------------------------- the hardware */

#define GCC_PA              0x01400000ull
#define GCC_SIZE            0x00080000u
#define GPUCC_PA            0x05990000ull
#define GPUCC_SIZE          0x00009000u
#define GPU_PA              0x05900000ull
#define GPU_SIZE            0x00040000u
#define SMMU_PA             0x059A0000ull       /* kgsl-smmu, SMMU-500 */
#define SMMU_SIZE           0x00010000u

/*
 * The GIC-500 distributor, and the GPU's interrupt in it. The device tree gives kgsl
 * "interrupts = <0 0xb1 4>": SPI 177, level high, so INTID 177 + 32 = 209, which lives in
 * word 6 (209 / 32) of each GICD bitmap, bit 17. Nothing in this kernel handles it.
 */
#define GICD_PA             0x0F200000ull
#define GICD_SIZE           0x00010000u
#define GICD_ISENABLER      0x0100u
#define GICD_ICENABLER      0x0180u
#define GICD_ISPENDR        0x0200u
#define GICD_ICPENDR        0x0280u
#define GICD_ISACTIVER      0x0300u
#define GPU_INTID           (177u + 32u)
#define GPU_INT_WORD        (4u * (GPU_INTID / 32u))
#define GPU_INT_BIT         (1u << (GPU_INTID % 32u))

/* DT gpu_region@55A15000 (topaz has the same region at 0x55B15000) */
#define ZAP_PA              0x55A15000ull
#define ZAP_SIZE            0x00002000u
#define GPU_PAS_ID          13

/*
 * Buffers the GPU reads by physical address: RAM the kernel must NOT manage, below 4 GB (A610 has ADRENO_QUIRK_4GB_VA, and the
 * SMMU context maps the pool 1:1). ios-sapphire used 0xE1000000, which was outside its kernel's RAM. Here the kernel's RAM is the
 * ADT's dram-base/dram-size = 0xb0000000 + 0x4ce00000, so the pool is the 2 MB the ADT no longer lists, 0xfce00000-0xfd000000
 * (the former top of RAM, tools/mkadt.py board 'sapphire' and sapphire.adt were cut by 2 MB for it). gpu_init() refuses to start
 * if the ADT still hands those pages to the kernel: they would be written under it.
 */
#define POOL_BASE_ADT_END   0xFD000000ull
#define POOL_PA             0xFCE00000ull       /* 2 MB aligned: the SMMU maps it as one 2 MB block */
#define POOL_SIZE           0x00100000u
#define OFF_SQE             0x00000u            /* microcode, up to 64 KB */
#define OFF_RING            0x10000u            /* 32 KB, 64 KB aligned */
#define OFF_TEST            0x18000u            /* what CP_MEM_WRITE writes */
#define OFF_META            0x19000u            /* zap metadata, read by TrustZone */
#define OFF_FENCE           0x18100u            /* where the CCU/cache flush events write their seqno */
#define OFF_MSM_FENCE       0x18200u            /* 0153: the msm client's own fence, written by nothing else */
#define OFF_DRAW            0x40000u            /* what the 2D blitter fills: 256 x 256 RGBA8, 256 KB */
#define OFF_PT              0x80000u            /* GPU page tables: 128 pages of 4 KB */
#define OFF_CS              0x30000u            /* compute shader binary */
#define OFF_CSOUT           0x31000u            /* what it writes */
#define PT_PAGES            128u
#define DRAW_W              256u
#define DRAW_H              256u
#define DRAW_PITCH          (DRAW_W * 4u)       /* bytes; the blitter wants a multiple of 64 */
#define RING_BYTES          0x8000u
#define RING_DWORDS         (RING_BYTES / 4)

#define CBCR_EN             (1u << 0)
#define CBCR_OFF            (1u << 31)
#define GDSC_COLLAPSE       (1u << 0)
#define GDSC_PWR_ON         (1u << 31)
#define RCG_UPDATE          (1u << 0)
#define RCG_ROOT_OFF        (1u << 31)
#define SRC_GPLL0           5                   /* GPLL0_OUT_MAIN in the GPU CC parent maps */

/* A6xx registers, dword offsets (registers/adreno/a6xx.xml) */
#define REG_RBBM_GBIF_HALT      0x0016
#define REG_RBBM_GBIF_HALT_ACK  0x0017
#define REG_RBBM_INT_CLEAR_CMD  0x0037
#define REG_GBIF_HALT           0x3c45
#define REG_GBIF_HALT_ACK       0x3c46
#define GBIF_CLIENT_HALT        (1u << 0)
#define GBIF_ARB_HALT           (1u << 1)
#define REG_RBBM_INT_0_MASK     0x0038
#define REG_RBBM_INT_0_STATUS   0x0201
#define REG_RBBM_STATUS         0x0210
#define REG_CP_RB_BASE          0x0800
#define REG_CP_RB_CNTL          0x0802
#define REG_CP_RB_RPTR          0x0806
#define REG_CP_RB_WPTR          0x0807
#define REG_CP_SQE_CNTL         0x0808
#define REG_CP_HW_FAULT         0x0821
#define REG_CP_SQE_INSTR_BASE   0x0830
#define REG_CP_ALWAYS_ON_COUNTER 0x0980
#define REG_RBBM_SECVID_TRUST_CNTL 0xF400

#define CP_ME_INIT              0x48
#define CP_MEM_WRITE            0x3d
#define CP_SET_SECURE_MODE      0x66
#define CP_WAIT_FOR_IDLE        0x26
#define CP_EVENT_WRITE          0x46
#define CP_SET_MARKER           0x65
#define CP_BLIT                 0x2c

/*
 * The 2D blitter, as Mesa's freedreno does a buffer clear on a6xx gen1 (fd6_blitter.cc
 * fd6_clear_buffer, emit_setup / emit_blit_setup / emit_blit_fini; offsets and fields from the
 * same tree's registers/adreno/a6xx.xml, a610's values from common/freedreno_devices.py).
 */
#define REG_RB_A2D_BLT_CNTL         0x8c00
#define REG_RB_A2D_PIXEL_CNTL       0x8c01
#define REG_RB_A2D_DEST_BUFFER_INFO 0x8c17      /* + BASE (64-bit) at 0x8c18, PITCH at 0x8c1a */
#define REG_RB_A2D_CLEAR_COLOR_DW0  0x8c2c      /* DW0..DW3 */
#define REG_TPL1_A2D_SRC_TEXTURE_INFO 0xb4c0    /* + SIZE 0xb4c1, BASE (64-bit) 0xb4c2, PITCH 0xb4c4 */
#define REG_GRAS_A2D_SRC_XMIN       0x8401      /* + XMAX, YMIN, YMAX: coordinate << 8 */
#define REG_GRAS_A2D_BLT_CNTL       0x8400
#define REG_GRAS_A2D_DEST_TL        0x8405      /* + BR at 0x8406 */
#define REG_SP_A2D_OUTPUT_INFO      0xacc0
#define REG_RB_CCU_CNTL             0x8e07
#define REG_RB_DBG_ECO_CNTL         0x8e04
#define REG_RB_RBP_CNTL             0x8e01
#define FMT6_8_8_8_8_UNORM          0x30
#define RM6_BLIT2DSCALE             12
#define BLIT_OP_SCALE               3
#define EV_PC_CCU_FLUSH_COLOR_TS    0x1d
#define EV_PC_CCU_FLUSH_DEPTH_TS    0x1c
#define EV_PC_CCU_INVALIDATE_COLOR  0x19
#define EV_PC_CCU_INVALIDATE_DEPTH  0x18
#define EV_CACHE_FLUSH_TS           0x04
#define EV_DEBUG_LABEL              0x3f

/* TrustZone (same calls as TopazModem's PAS boot, with the GPU's PAS id) */
#define SCM_FN(svc, cmd)    (0x42000000u | ((uint32_t)(svc) << 8) | (uint32_t)(cmd))
#define SCM_SVC_BOOT        0x01
#define SCM_BOOT_SET_REMOTE_STATE 0x0a
#define SCM_SVC_PIL         0x02
#define PIL_INIT_IMAGE      0x01
#define PIL_MEM_SETUP       0x02
#define PIL_AUTH_RESET      0x05
#define PIL_SHUTDOWN        0x06
#define SCM_ARG_RW          2

/* ---------------------------------------------------------------- firmware table */

/*
 * Where the GPU firmware lies in a stock partition of this phone, found on the host by
 * tools/fwtable-vendor.py and checked here by CRC32. Qualcomm's firmware is never copied
 * into this repository or into the iOS root; the driver reads it from the partition it
 * already sits in, read only.
 */
struct gpu_fw_file {
	const char *name;
	const char *part;
	uint32_t    off, size, crc;
};
static const struct gpu_fw_file gpu_fw[] = {
	/*
	 * Entries generated by tools/fwtable-vendor.py, for example
	 *   { "a630_sqe.fw", "vendor_a", 0x1234000, 32504, 0xdeadbeef },
	 *   { "a610_zap.mdt", "vendor_a", 0x1240000, 6860, 0xdeadbeef },
	 *   { "a610_zap.b02", "vendor_a", 0x1242000, 2096, 0xdeadbeef },
	 * The sentinel keeps the array valid while the table is empty.
	 */
	{ NULL, NULL, 0, 0, 0 },
};

/* Does the boot image carry the microcode in its device tree? */
static bool
have_adt_firmware(void)
{
	DTEntry chosen;
	void const *blob;
	unsigned int size;

	return SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	       SecureDTGetProperty(chosen, "gpu-sqe-firmware", &blob, &size) == kSuccess && size > 4;
}

static unsigned
fw_count(void)
{
	unsigned n = 0;
	while (n < sizeof(gpu_fw) / sizeof(gpu_fw[0]) && gpu_fw[n].name != NULL) {
		n++;
	}
	return n;
}

/* ---------------------------------------------------------------- register access */

static IOMemoryMap *gcc_map, *gpucc_map, *gpu_map, *smmu_map, *pool_map, *zap_map;
static volatile uint8_t *gcc, *gpucc, *gpu, *smmu;
static uint8_t *pool;
static uint32_t *ring, *test_buf;
static uint32_t ring_wptr;
static unsigned scm_resumes;
static uint32_t slow_ms;                        /* boot-arg gpu_slow: see step_done() */
/*
 * boot-arg gpu_div: the GPLL0 divider for the core clock, 2 = 300 MHz. Nothing here raises
 * VDD_GFX (Linux does it through the OPP table and RPMh, and there is no GMU to do it for
 * us), so the rail stays wherever the bootloader left it and 300 MHz may be above what it
 * holds: a boot that dies with the clocks up and no bus error is the shape that has. gpu_div=8
 * (75 MHz) is the slowest useful setting; the divider field holds 2 * div - 1.
 */
static uint32_t core_div = 2;
/*
 * boot-arg gpu_mhz: a stock OPP above 600 MHz (785 820 980 1025 1100 1260) from gpu_cc_pll0
 * instead of GPLL0 (ios-topaz 0922, sequence from topaz-woa TopazGpuW v0.43 and the stock
 * gpucc-khaje.c). The same caveat as gpu_div: VDD_GFX is not raised, so a rate above what the
 * bootloader's corner holds may hang the GPU. 0 = off (gpu_div decides).
 */
static uint32_t core_mhz;
extern "C" int hakios_rpm_vote(uint32_t set, uint32_t type, uint32_t id, uint32_t key, uint32_t value) __attribute__((weak));
/*
 * boot-arg gpu_smmu_early: program the identity context bank as soon as its clock is voted,
 * before the GX domain comes up, instead of in cp_start(). The GPU's SMMU comes out of the
 * bootloader with every stream faulting, so anything the GPU does on the bus between the GX
 * domain coming up and cp_start() is a fault with no context bank behind it, and a NoC error
 * on this SoC resets it without the CPU seeing anything. Off by default: it changes the order
 * of the one sequence that has worked.
 */
static uint32_t smmu_early;
/*
 * boot-arg gpu_irq=1 leaves the GPU's interrupt alone; by default it is masked in the GIC
 * before anything is powered. Three boots have now died in the same place: the step after
 * the power-up, which only re-reads registers that were just read, with the phone stopping
 * dead mid-sentence and iOS with it, no panic and no further line. That is not what a bad
 * register read looks like (that hangs the reader), it is what an interrupt nobody handles
 * looks like: the GPU's is level triggered, so if it is asserted and enabled in the
 * distributor the CPU re-takes it forever and nothing ever logs again. Whether it is enabled
 * at all is the bootloader's business, so the state goes in the log either way.
 */
static uint32_t gpu_irq;
/*
 * boot-arg gpu_stop=N ends stage 3 early: 1 after the zap shader, 2 once the SQE runs, 3 after
 * CP_ME_INIT, 4 after CP_SET_SECURE_MODE; 0 (default) runs it all. boot-arg gpu_keep=1 leaves
 * the GPU running afterwards; by default it is parked (bus drained, GX and CX collapsed), as
 * Linux does once a GPU goes idle. Both exist for the one stage-3 boot whose panel stopped
 * taking DSI commands right after the GPU ran ("panel level: NO DONE", the screen filled with
 * colours), when stage 2 and stage 4 did not: they say which step does it, and whether it
 * lasts only while the GPU is up.
 */
static uint32_t stop_at, keep_running;
static uint32_t draw_first = 1;                 /* boot-arg gpu_draw=0 skips the 2D fill */
static bool stopped_early;
static bool service_ok;                         /* stage 3 and the draw tests passed */
static volatile bool service_ready;              /* gpu_service: the GPU takes work from the rest of the kernel */

static bool
stop_here(uint32_t step, const char *what)
{
	if (stop_at != step) {
		return false;
	}
	LOG("gpu_stop=%u: stopping after %s", step, what);
	stopped_early = true;
	return true;
}
static IOMemoryMap *gicd_map;
static volatile uint8_t *gicd;
static bool smmu_identity(void);
static void msm_publish(void);

/*
 * The persistent log is written to userdata about once a second, so a step that hangs or
 * panics the phone takes the lines around it with it: the last boot left nothing after
 * "starting in N s". gpu_slow=N waits N ms after each step, which gives that writer time to
 * put the step's line on disk before the next one runs. 1200 is a good value.
 *
 * Every step of the power-up, the zap shader and the CP ends in one of these, so a boot that
 * stops dead says in the log which register or which TrustZone call it stopped on.
 */
static void
step_done(void)
{
	if (slow_ms != 0) {
		IOSleep(slow_ms);
	}
}

static inline void
gmb(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint32_t
rd(volatile uint8_t *b, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(b + off);
	gmb();
	return v;
}

static void
wr(volatile uint8_t *b, uint32_t off, uint32_t v)
{
	gmb();
	*(volatile uint32_t *)(b + off) = v;
	gmb();
}

static void
rmw(volatile uint8_t *b, uint32_t off, uint32_t clear, uint32_t set)
{
	wr(b, off, (rd(b, off) & ~clear) | set);
}

/* Wait up to us microseconds for (reg & mask) == want. */
static bool
poll(volatile uint8_t *b, uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t i = 0; i <= us; i++) {
		if ((rd(b, off) & mask) == want) {
			return true;
		}
		IODelay(1);
	}
	return false;
}

/* The GPU's own registers are addressed in dwords in Qualcomm's documentation. */
static uint32_t
grd(uint32_t reg)
{
	return rd(gpu, 4 * reg);
}

static void
gwr(uint32_t reg, uint32_t v)
{
	wr(gpu, 4 * reg, v);
}

static void
gwr64(uint32_t reg, uint64_t v)
{
	gwr(reg, (uint32_t)v);
	gwr(reg + 1, (uint32_t)(v >> 32));
}

static IOMemoryMap *
map_phys(uint64_t pa, uint64_t size, bool ram)
{
	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(pa, size, kIODirectionInOut);
	IOMemoryMap *m;

	if (md == NULL) {
		return NULL;
	}
	/*
	 * Registers are device memory (kIOMapInhibitCache). RAM the GPU shares is not: xnu's
	 * bzero and memcpy use dc zva and vector stores, which are alignment faults on device
	 * memory ("Unaligned kernel data abort" in bzero, from the first bzero of the pool,
	 * what stage 3 and 4 died of before). Write-combined is normal non-cacheable memory,
	 * which is what TopazModem and SweetSurface use for theirs, and the GPU sees every
	 * store after a dsb with no cache maintenance.
	 */
	m = md->map(ram ? kIOMapWriteCombineCache : kIOMapInhibitCache);
	md->release();
	return m;
}

/* ---------------------------------------------------------------- TrustZone */

static uint64_t
smc(uint64_t a[8])
{
	register uint64_t x0 __asm__("x0") = a[0], x1 __asm__("x1") = a[1], x2 __asm__("x2") = a[2],
	    x3 __asm__("x3") = a[3], x4 __asm__("x4") = a[4], x5 __asm__("x5") = a[5], x6 __asm__("x6") = a[6],
	    x7 __asm__("x7") = a[7];
	__asm__ volatile ("smc #0"
            : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
            :
            : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	a[0] = x0; a[1] = x1; a[2] = x2; a[3] = x3; a[4] = x4; a[5] = x5; a[6] = x6; a[7] = x7;
	return x0;
}

static uint64_t
scm(uint32_t fn, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *res1)
{
	uint64_t args[8] = { fn, arginfo, a, b, c, 0, 0, 0 };
	unsigned n;

	smc(args);
	/* long calls answer INTERRUPTED (1): resume with x0 = 1 and the session from x6 */
	for (n = 0; args[0] == 1 && n < 100000; n++) {
		uint64_t again[8] = { 1, arginfo, a, b, c, 0, args[6], 0 };
		memcpy(args, again, sizeof(args));
		smc(args);
	}
	scm_resumes = n;
	if (res1 != NULL) {
		*res1 = args[1];
	}
	return args[0];
}

/* ---------------------------------------------------------------- clocks and power */

struct reg_desc { uint8_t block, kind; uint32_t off; const char *name; };

static const struct reg_desc clock_regs[] = {
	{ 0, 1, 0x36004, "gcc_gpu_cfg_ahb" },
	{ 0, 1, 0x3600c, "gcc_gpu_memnoc_gfx" },
	{ 0, 1, 0x36018, "gcc_gpu_snoc_dvm_gfx" },
	{ 0, 1, 0x36100, "gcc_gpu_iref" },
	{ 0, 1, 0x36048, "gcc_gpu_throttle_core" },
	{ 0, 0, 0x79004, "gcc_vote (b15 gpll0)" },
	{ 1, 4, 0x0000,  "gpucc_pll0_mode" },
	{ 1, 0, 0x0004,  "gpucc_pll0_l" },
	{ 1, 4, 0x0100,  "gpucc_pll1_mode" },
	{ 1, 3, 0x101c,  "gpucc_gx_gfx3d_rcg_cmd" },
	{ 1, 0, 0x1020,  "gpucc_gx_gfx3d_rcg_cfg" },
	{ 1, 3, 0x1120,  "gpucc_gmu_rcg_cmd" },
	{ 1, 0, 0x1124,  "gpucc_gmu_rcg_cfg" },
	{ 1, 1, 0x1004,  "gpucc_cxo_aon" },
	{ 1, 1, 0x1054,  "gpucc_gx_gfx3d" },
	{ 1, 1, 0x1060,  "gpucc_gx_cxo" },
	{ 1, 1, 0x1078,  "gpucc_ahb" },
	{ 1, 1, 0x1098,  "gpucc_cx_gmu" },
	{ 1, 1, 0x109c,  "gpucc_cxo" },
	{ 1, 1, 0x10a4,  "gpucc_cx_gfx3d" },
	{ 1, 1, 0x5000,  "gpucc_hlos1_vote_gpu_smmu" },
	{ 1, 2, 0x100c,  "gpucc_gx_gdsc" },
	{ 1, 2, 0x106c,  "gpucc_cx_gdsc" },
	{ 1, 0, 0x1508,  "gpucc_gx_clamp_io" },
	{ 1, 0, 0x1540,  "gpucc_cx_gds_hw_ctrl" },
};

/* The GPU's line in the distributor: enabled, pending, active. */
static void
gic_report(const char *when)
{
	if (gicd == NULL) {
		return;
	}
	LOG("  GIC SPI 177 (INTID %u) %s: enabled %u pending %u active %u (words %08x %08x %08x)", GPU_INTID, when,
	    (rd(gicd, GICD_ISENABLER + GPU_INT_WORD) & GPU_INT_BIT) != 0,
	    (rd(gicd, GICD_ISPENDR + GPU_INT_WORD) & GPU_INT_BIT) != 0,
	    (rd(gicd, GICD_ISACTIVER + GPU_INT_WORD) & GPU_INT_BIT) != 0,
	    rd(gicd, GICD_ISENABLER + GPU_INT_WORD), rd(gicd, GICD_ISPENDR + GPU_INT_WORD),
	    rd(gicd, GICD_ISACTIVER + GPU_INT_WORD));
}

/*
 * Writing a 1 to ICENABLER/ICPENDR clears that interrupt's enable and pending bits and
 * leaves every other line alone, which is why these registers are a bitmap of ones to act on
 * rather than a value to store.
 */
static void
gic_mask_gpu(void)
{
	if (gicd == NULL) {
		return;
	}
	wr(gicd, GICD_ICENABLER + GPU_INT_WORD, GPU_INT_BIT);
	wr(gicd, GICD_ICPENDR + GPU_INT_WORD, GPU_INT_BIT);
	gmb();
	gic_report("masked");
}

static void
dump_clocks(const char *when)
{
	LOG("--- clock tree (%s)", when);
	for (unsigned i = 0; i < sizeof(clock_regs) / sizeof(clock_regs[0]); i++) {
		const struct reg_desc *r = &clock_regs[i];
		volatile uint8_t *b = r->block == 0 ? gcc : gpucc;
		uint32_t v = rd(b, r->off);
		const char *state = "";
		switch (r->kind) {
		case 1: state = (v & CBCR_OFF) ? " off" : " running"; break;
		case 2: state = (v & GDSC_PWR_ON) ? " PWR_ON" : " collapsed"; break;
		case 3: state = (v & RCG_ROOT_OFF) ? " root off" : (v & RCG_UPDATE) ? " update pending" : " root on"; break;
		case 4: state = (v & 1) ? " outctrl" : " unconfigured"; break;
		default: break;
		}
		LOG("  %s %08x%s", r->name, v, state);
	}
}

static bool
branch_on(volatile uint8_t *b, uint32_t off, const char *name)
{
	bool ok;

	rmw(b, off, 0, CBCR_EN);
	ok = poll(b, off, CBCR_OFF, 0, 200);
	LOG("  %s on: %08x %s", name, rd(b, off), ok ? "running" : "STILL OFF");
	step_done();
	return ok;
}

/*
 * A GDSC comes up by clearing SW_COLLAPSE and waiting for PWR_ON. The GX domain also needs
 * its block reset pulsed and its I/O clamp released first; its status is in its own GDSCR,
 * the CX domain's is in GPU_CC_CX_GDS_HW_CTRL (0x1540).
 */
static bool
gdsc_on(uint32_t gdscr, uint32_t bcr, uint32_t clamp, uint32_t status, const char *name)
{
	bool ok;

	if (bcr != 0) {
		rmw(gpucc, bcr, 0, 1);
		IODelay(1);
		rmw(gpucc, bcr, 1, 0);
	}
	if (clamp != 0) {
		rmw(gpucc, clamp, 1, 0);
	}
	rmw(gpucc, gdscr, GDSC_COLLAPSE, 0);
	IODelay(1);
	ok = poll(gpucc, status, GDSC_PWR_ON, GDSC_PWR_ON, 500);
	LOG("  %s on: gdscr %08x status %08x %s", name, rd(gpucc, gdscr), rd(gpucc, status),
	    ok ? "PWR_ON" : "NO PWR_ON");
	step_done();
	return ok;
}

/* Root clock generator: pick a parent and a divider (the register holds 2 * div - 1). */
/* ---- gpu_cc_pll0: Zonda PLL at GPU CC + 0, f = 19.2 MHz * (L + alpha / 2^16) ---- */
#define ZP_MODE         0x00
#define ZP_L            0x04
#define ZP_ALPHA        0x08
#define ZP_USER         0x0c
#define ZP_CFG          0x10
#define ZP_CFG_U        0x14
#define ZP_CFG_U1       0x18
#define ZP_OPMODE       0x28
#define ZP_OUTCTRL      (1u << 0)
#define ZP_BYPASSNL     (1u << 1)
#define ZP_RESET_N      (1u << 2)
#define ZP_LOCK         (1u << 31)
#define SRC_GPU_PLL0    1                   /* GPU_CC_PLL0_OUT_MAIN in gx_gfx3d's parent map */

static bool
zonda_la(uint32_t mhz, uint32_t *l, uint32_t *a)
{
	static const uint32_t t[][3] = {
		{ 785, 41, 0xE2AA }, { 820, 43, 0xB555 }, { 980, 51, 0x0AAB },
		{ 1025, 53, 0x62AA }, { 1100, 57, 0x4AAB }, { 1260, 66, 0xA000 },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		if (t[i][0] == mhz) {
			*l = t[i][1];
			*a = t[i][2];
			return true;
		}
	}
	return false;
}

static bool
gpu_pll_start(uint32_t mhz)
{
	uint32_t l, a;

	if (!zonda_la(mhz, &l, &a)) {
		LOG("gpu_mhz=%u is not a stock OPP above 600 (785 820 980 1025 1100 1260): GPLL0", mhz);
		return false;
	}
	rmw(gpucc, ZP_MODE, ZP_OUTCTRL, 0);
	wr(gpucc, ZP_OPMODE, 0);
	wr(gpucc, ZP_L, l);
	wr(gpucc, ZP_ALPHA, a);
	wr(gpucc, ZP_CFG, 0x08200800);
	wr(gpucc, ZP_CFG_U, 0x05022001);
	wr(gpucc, ZP_CFG_U1, 0x00000010);
	wr(gpucc, ZP_USER, 0x01000101);
	rmw(gpucc, ZP_MODE, ZP_BYPASSNL, 0);
	rmw(gpucc, ZP_MODE, 0, ZP_RESET_N);
	rmw(gpucc, ZP_MODE, 0, ZP_BYPASSNL);
	IODelay(1);
	rmw(gpucc, ZP_MODE, 0, ZP_RESET_N);
	wr(gpucc, ZP_OPMODE, 1);
	if (!poll(gpucc, ZP_MODE, ZP_LOCK, ZP_LOCK, 1000)) {
		LOG("gpu_cc_pll0: no lock at %u MHz (mode %08x): GPLL0", mhz, rd(gpucc, ZP_MODE));
		return false;
	}
	rmw(gpucc, ZP_USER, 0, 0xF);
	rmw(gpucc, ZP_MODE, 0, ZP_OUTCTRL);
	IODelay(100);
	LOG("gpu_cc_pll0 locked at %u MHz (l %u alpha %04x)", mhz, l, a);
	return true;
}

static bool
rcg_set(uint32_t cmd, uint32_t src, uint32_t div, const char *name)
{
	bool ok;

	wr(gpucc, cmd + 4, (src << 8) | (2 * div - 1));
	rmw(gpucc, cmd, 0, RCG_UPDATE);
	ok = poll(gpucc, cmd, RCG_UPDATE, 0, 500);
	LOG("  %s src %u div %u (GPLL0/%u = %u MHz): cmd %08x cfg %08x %s", name, src, div, div, 600 / div,
	    rd(gpucc, cmd), rd(gpucc, cmd + 4), ok ? "updated" : "UPDATE STUCK");
	step_done();
	return ok;
}

static bool
power_up(void)
{
	bool ok = true;

	LOG("--- power up");
	/* GCC side: GPLL0 to the GPU CC, and the GPU's bus clocks */
	LOG("  about to vote GPLL0 to the GPU (GCC 79004 = %08x)", rd(gcc, 0x79004));
	step_done();
	rmw(gcc, 0x79004, 0, 1u << 15);
	LOG("  gcc vote 79004 = %08x (gpu_gpll0_clk_src)", rd(gcc, 0x79004));
	step_done();
	branch_on(gcc, 0x71154, "gcc_bimc_gpu_axi");
	branch_on(gcc, 0x3600c, "gcc_gpu_memnoc_gfx");      /* voted branch: may read off until used */
	branch_on(gcc, 0x36018, "gcc_gpu_snoc_dvm_gfx");
	/* CX first: the GPU CC cxo/ahb branches live in that domain and stay off before it */
	if (!gdsc_on(0x106c, 0, 0, 0x1540, "gpu_cx_gdsc")) {
		return false;
	}
	ok &= branch_on(gpucc, 0x109c, "gpucc_cxo");
	ok &= branch_on(gpucc, 0x1078, "gpucc_ahb");
	if (!ok) {
		return false;
	}
	rcg_set(0x1120, SRC_GPLL0, 3, "gmu_clk_src");
	rmw(gpucc, 0x1098, 0xFF0, 0xFF0);                   /* cx_gmu wakeup/sleep delays, as Linux probes */
	step_done();
	branch_on(gpucc, 0x1098, "gpucc_cx_gmu");
	branch_on(gpucc, 0x5000, "gpucc_hlos1_vote_gpu_smmu");
	if (smmu_early != 0 && smmu != NULL) {
		LOG("  gpu_smmu_early: the identity bank goes in before the GX domain");
		smmu_identity();
		step_done();
	}
	/* GX domain, then the core clock from GPLL0 (the GPU CC PLLs are left unconfigured) */
	if (!gdsc_on(0x100c, 0x1008, 0x1508, 0x100c, "gpu_gx_gdsc")) {
		return false;
	}
	/*
	 * 0204: the GX domain is fed by the CX rail (pm6125 S3, RPM "rwcx" 0, its level by key "vlvl";
	 * gpu_gx_gdsc's parent in the device tree). The stock levels: 1025 MHz TURBO (0x180),
	 * 1114.8 and 1260 MHz TURBO_L1 (0x1a0). Raised before the clock, never lowered.
	 */
	if (core_mhz >= 980) {
		uint32_t level = core_mhz > 1025 ? 0x1a0 : 0x180;
		int r = hakios_rpm_vote ? hakios_rpm_vote(0, 0x78637772, 0, 0x6c766c76, level) : -1;
		LOG("  CX rail to RPM level %#x for %u MHz: %d", level, core_mhz, r);
		if (r != 0 && core_mhz > 1025) {
			LOG("  the CX level was not taken: 1025 MHz instead of %u", core_mhz);
			core_mhz = 1025;
		}
	}
	if (core_mhz != 0 && gpu_pll_start(core_mhz)) {
		ok &= rcg_set(0x101c, SRC_GPU_PLL0, 1, "gx_gfx3d_clk_src");
	} else {
		ok &= rcg_set(0x101c, SRC_GPLL0, core_div, "gx_gfx3d_clk_src");
	}
	rmw(gpucc, 0x1054, 0, (1u << 14) | (1u << 13));     /* force mem core / periph on */
	step_done();
	ok &= branch_on(gpucc, 0x1054, "gpucc_gx_gfx3d");
	branch_on(gpucc, 0x1060, "gpucc_gx_cxo");
	/* the line three boots have died just after this point */
	gic_report("with the GPU powered");
	step_done();
	return ok;
}

static void
power_down(void)
{
	LOG("--- power down");
	rmw(gpucc, 0x1054, CBCR_EN, 0);
	rmw(gpucc, 0x100c, 0, GDSC_COLLAPSE);
	rmw(gpucc, 0x1508, 0, 1);
	rmw(gpucc, 0x1098, CBCR_EN, 0);
	rmw(gpucc, 0x5000, CBCR_EN, 0);
	rmw(gpucc, 0x106c, 0, GDSC_COLLAPSE);
	LOG("  gx gdscr %08x, cx gdscr %08x", rd(gpucc, 0x100c), rd(gpucc, 0x106c));
}

/*
 * Linux a6xx_bus_clear_pending_transactions(gx_off = true), which a GPU without a GMU runs
 * before every power collapse: halt the GX side of GBIF, then new client requests, then all
 * AXI requests, waiting for each ack, and release the GBIF halt. Collapsing the domains with
 * the CP's transactions still in flight leaves GBIF wedged mid-transfer, and the memory NoC
 * the display fetches from with it: the panel underflows to a flat colour and its DSI
 * commands stop completing. That is what every boot that parked the GPU after its CP had
 * run did (gpu_stop=3, 4 and the full stage 3), and what neither the zap-only boot nor the
 * one that left the GPU running did. RBBM_GBIF_HALT stays set; hw_init_regs clears it.
 */
static bool
halt_ack(uint32_t reg, uint32_t mask)
{
	for (int i = 0; i < 1000; i++) {
		if ((grd(reg) & mask) == mask) {
			return true;
		}
		IODelay(1);
	}
	return false;
}

static void
bus_drain(void)
{
	bool gx, client, arb;

	gwr(REG_RBBM_GBIF_HALT, 1);
	gx = halt_ack(REG_RBBM_GBIF_HALT_ACK, 1);
	gwr(REG_GBIF_HALT, GBIF_CLIENT_HALT);
	client = halt_ack(REG_GBIF_HALT_ACK, GBIF_CLIENT_HALT);
	gwr(REG_GBIF_HALT, GBIF_ARB_HALT);
	arb = halt_ack(REG_GBIF_HALT_ACK, GBIF_ARB_HALT);
	gwr(REG_GBIF_HALT, 0);
	gmb();
	LOG("  bus drained: GX %s, clients %s, AXI %s (GBIF_HALT_ACK %08x)", gx ? "halted" : "NO ACK",
	    client ? "halted" : "NO ACK", arb ? "halted" : "NO ACK", grd(REG_GBIF_HALT_ACK));
}

/* The counter runs off the 19.2 MHz crystal: ~192000 ticks in 10 ms means the GPU is alive. */
static void
first_reads(void)
{
	uint32_t lo1, hi1, lo2, hi2;

	LOG("--- first GPU reads: about to touch %llx for the first time", GPU_PA);
	step_done();
	LOG("  RBBM_STATUS   %08x", grd(REG_RBBM_STATUS));
	step_done();
	LOG("  RBBM_INT_0_STATUS %08x (what the GPU is asking for)", grd(REG_RBBM_INT_0_STATUS));
	if (gpu_irq == 0) {
		/* and now at the source, not only in the distributor */
		gwr(REG_RBBM_INT_0_MASK, 0);
		gwr(REG_RBBM_INT_CLEAR_CMD, 0xFFFFFFFF);
		gmb();
		LOG("  interrupts masked at the GPU too: INT_0_STATUS %08x", grd(REG_RBBM_INT_0_STATUS));
		gic_report("after masking at the GPU");
	}
	step_done();
	LOG("  CP_HW_FAULT   %08x", grd(REG_CP_HW_FAULT));
	lo1 = grd(REG_CP_ALWAYS_ON_COUNTER);
	hi1 = grd(REG_CP_ALWAYS_ON_COUNTER + 1);
	IOSleep(10);
	lo2 = grd(REG_CP_ALWAYS_ON_COUNTER);
	hi2 = grd(REG_CP_ALWAYS_ON_COUNTER + 1);
	LOG("  CP_ALWAYS_ON_COUNTER %08x%08x -> %08x%08x in 10 ms (delta %u, 19.2 MHz = ~192000)",
	    hi1, lo1, hi2, lo2, lo2 - lo1);
}

/* ---------------------------------------------------------------- firmware */

/* The same firmware, carried in the device tree by the boot image. */
static const struct { const char *file, *prop; } adt_fw_map[] = {
	{ "a630_sqe.fw",  "gpu-sqe-firmware" },
	{ "a610_zap.mdt", "gpu-zap-metadata" },
	{ "a610_zap.b02", "gpu-zap-segment" },
};

static int64_t
adt_fw_read(const char *name, void *dst, uint32_t max)
{
	DTEntry chosen;
	void const *blob;
	unsigned int size;

	for (unsigned i = 0; i < sizeof(adt_fw_map) / sizeof(adt_fw_map[0]); i++) {
		if (strcmp(adt_fw_map[i].file, name) != 0) {
			continue;
		}
		if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
		    SecureDTGetProperty(chosen, adt_fw_map[i].prop, &blob, &size) != kSuccess) {
			return -1;
		}
		if (size > max) {
			LOG("  %s: %u bytes in the device tree, no room", name, size);
			return -1;
		}
		memcpy(dst, blob, size);
		LOG("  %s: %u bytes from the device tree", name, size);
		return size;
	}
	return -1;
}

/* Read a firmware file into dst. Returns its length, or -1. */
static int64_t
fw_read(const char *name, void *dst, uint32_t max)
{
	int64_t n = adt_fw_read(name, dst, max);

	if (n >= 0) {
		return n;
	}
	for (unsigned i = 0; i < fw_count(); i++) {
		if (strcmp(gpu_fw[i].name, name) != 0) {
			continue;
		}
		if (gpu_fw[i].size > max) {
			LOG("  %s: %u bytes, no room", name, gpu_fw[i].size);
			return -1;
		}
		int err = sweet_ufs_read_part(gpu_fw[i].part, gpu_fw[i].off, dst, gpu_fw[i].size);
		if (err != 0) {
			LOG("  %s: read from %s failed: %d", name, gpu_fw[i].part, err);
			return -1;
		}
		uint32_t crc = crc32(0, dst, gpu_fw[i].size);
		if (crc != gpu_fw[i].crc) {
			/* TrustZone checks the signature anyway: say so and go on */
			LOG("  %s: crc32 %08x, expected %08x: not the firmware this table was made for", name, crc,
			    gpu_fw[i].crc);
		}
		return gpu_fw[i].size;
	}
	LOG("  %s: not in the firmware table", name);
	return -1;
}

/*
 * The zap shader leaves the GPU's secure mode: TrustZone authenticates it (PAS id 13) and
 * runs it. Metadata = the whole .mdt; the loadable segment goes to the reserved region the
 * device tree keeps for it. PAS mem_setup wants a 4 KB-aligned size (Linux mdt_loader does
 * ALIGN(paddr + memsz, SZ_4K)); an unaligned one is refused with 0xffcfffba.
 */
static bool
zap_load(void)
{
	uint8_t *meta = pool + OFF_META;
	uint64_t meta_pa = POOL_PA + OFF_META, st, res = 0;
	uint32_t phoff, phnum, min_addr = 0xFFFFFFFFu, max_addr = 0, load_paddr = 0;
	uint32_t seg_size;
	int64_t mdt_size;
	bool found = false;

	LOG("--- zap shader (PAS %u)", GPU_PAS_ID);
	step_done();
	mdt_size = fw_read("a610_zap.mdt", meta, POOL_SIZE - OFF_META);
	if (mdt_size < 52) {
		return false;
	}
	memcpy(&phoff, meta + 28, 4);
	if (phoff >= (uint32_t)mdt_size) {
		LOG("  program headers at %x, past the %lld bytes read", phoff, mdt_size);
		return false;
	}
	uint16_t n16;
	memcpy(&n16, meta + 44, 2);
	phnum = n16;
	for (uint32_t i = 0; i < phnum && phoff + 32 * (i + 1) <= (uint32_t)mdt_size; i++) {
		uint32_t ptype, paddr, memsz, filesz, flags;
		memcpy(&ptype, meta + phoff + 32 * i, 4);
		memcpy(&paddr, meta + phoff + 32 * i + 12, 4);
		memcpy(&filesz, meta + phoff + 32 * i + 16, 4);
		memcpy(&memsz, meta + phoff + 32 * i + 20, 4);
		memcpy(&flags, meta + phoff + 32 * i + 24, 4);
		LOG("  phdr %u type %u paddr %x filesz %u memsz %u flags %08x", i, ptype, paddr, filesz, memsz, flags);
		if (ptype == 1 && memsz != 0 && ((flags >> 24) & 7) != 2) {   /* PT_LOAD, not the hash segment */
			if (paddr < min_addr) {
				min_addr = paddr;
			}
			if (((paddr + memsz + 0xFFFu) & ~0xFFFu) > max_addr) {
				max_addr = (paddr + memsz + 0xFFFu) & ~0xFFFu;
			}
			load_paddr = paddr;
			found = true;
		}
	}
	if (!found || max_addr - min_addr > ZAP_SIZE) {
		LOG("  unexpected layout (found %d, %x..%x)", found, min_addr, max_addr);
		return false;
	}

	/* A half-finished PAS from an earlier try refuses everything: start clean */
	step_done();
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_SHUTDOWN), 1, GPU_PAS_ID, 0, 0, &res);
	LOG("  shutdown: ret %llx res %llx", st, res);
	step_done();
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_INIT_IMAGE), 2 | (SCM_ARG_RW << 6), GPU_PAS_ID, meta_pa, 0, &res);
	LOG("  init_image: ret %llx res %llx (%lld bytes of metadata at %llx)", st, res, mdt_size, meta_pa);
	step_done();
	if (st != 0 || res != 0) {
		/* already authenticated in this boot: ask TrustZone to resume it */
		st = scm(SCM_FN(SCM_SVC_BOOT, SCM_BOOT_SET_REMOTE_STATE), 2, 0, GPU_PAS_ID, 0, &res);
		LOG("  set_remote_state(resume): ret %llx res %llx (0/0 = the zap shader is already up)", st, res);
		return st == 0 && res == 0;
	}
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_MEM_SETUP), 3, GPU_PAS_ID, ZAP_PA, max_addr - min_addr, &res);
	LOG("  mem_setup %llx+%x: ret %llx res %llx", ZAP_PA, max_addr - min_addr, st, res);
	step_done();
	if (st != 0 || res != 0) {
		return false;
	}

	zap_map = map_phys(ZAP_PA, ZAP_SIZE, true);
	if (zap_map == NULL) {
		LOG("  the zap region could not be mapped");
		return false;
	}
	uint8_t *dst = (uint8_t *)zap_map->getVirtualAddress();
	bzero(dst, ZAP_SIZE);
	seg_size = ZAP_SIZE - (load_paddr - min_addr);
	if (fw_read("a610_zap.b02", dst + (load_paddr - min_addr), seg_size) < 0) {
		return false;
	}
	gmb();
	LOG("  segment loaded at %llx, authenticating", ZAP_PA + (load_paddr - min_addr));
	step_done();
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_AUTH_RESET), 1, GPU_PAS_ID, 0, 0, &res);
	LOG("  auth_and_reset: ret %llx res %llx resumes %u", st, res, scm_resumes);
	step_done();
	return st == 0 && res == 0;
}

/* ---------------------------------------------------------------- GPU SMMU */

/*
 * The GPU's SMMU-500 comes out of the bootloader with every stream faulting. One
 * pass-through context bank (SCTLR.M = 0) for the GPU's stream makes GPU addresses equal
 * physical ones, which is all the ring and the buffers below 4 GB need. Real stage-1 page
 * tables come later, when a user space driver allocates its own buffers.
 */
/*
 * GPU virtual memory: stage-1 page tables in the GPU's SMMU context bank, so the GPU can reach
 * memory that is not physically contiguous (everything iOS allocates). Boot-arg gpu_mmu=1.
 *
 * Format and register values are Linux's: drivers/iommu/io-pgtable-arm.c (ARM_64_LPAE_S1,
 * 4 KB granule, a walker that is not cache coherent) and drivers/iommu/arm/arm-smmu
 * (arm_smmu_write_context_bank, with Qualcomm's Adreno write_sctlr). The a610 has
 * ADRENO_QUIRK_4GB_VA, so the GPU's addresses are 32 bits: T0SZ = 32, the walk starts at level
 * 1 (VA[31:30], 4 entries), then level 2 (2 MB) and level 3 (4 KB). Output addresses are 36
 * bits, which covers all of this phone's RAM.
 *
 * The tables live in the pool, which is write-combined: the CPU's stores reach RAM at the next
 * dsb, and the walker reads RAM (TCR IRGN0/ORGN0 = non-cacheable), so no cache maintenance.
 * Everything that worked with the identity bank stays at VA = PA (the pool, the boot
 * framebuffer) as 2 MB blocks; other memory gets 4 KB pages anywhere below 4 GB.
 */
#define PTE_TYPE_TABLE      3ull                /* also TYPE_PAGE at level 3 */
#define PTE_TYPE_BLOCK      1ull
#define PTE_ADDR_MASK       0x0000fffffffff000ull
/* AttrIndx 0 (MAIR NC), AP_UNPRIV, SH_OS, AF, nG: Linux's prot for non-cached read/write */
#define PTE_RW_NC           ((1ull << 6) | (2ull << 8) | (1ull << 10) | (1ull << 11))
#define SMMU_MAIR0          0xf404ff44u         /* NC, WBRWA, device, INC_OWBRWA */
#define SMMU_TCR            ((2u << 12) | (1u << 23) | 32u)  /* SH0 OS, EPD1, T0SZ 32, TG0 4 KB, NC walks */
#define SMMU_TCR2           ((7u << 15) | 1u)   /* SEP upstream, PASIZE 36 bits */
#define SMMU_CBAR_S1        ((1u << 16) | (0xFu << 12) | (3u << 8))  /* S1 trans S2 bypass, WB, NSH */
#define SMMU_SCTLR_XLATE    (0x1000u | 0x100u | 0x40u | 0x20u | 0x4u | 0x2u | 0x1u)  /* ASIDPNE HUPCF CFIE CFRE AFE TRE M */

static uint32_t mmu;
static uint32_t pt_used;
static uint64_t *pt_l1;
static uint64_t pt_l1_pa;

/*
 * More page tables, for the msm client's buffers (up to 2 GB of 4 KB pages need 1024 tables):
 * the 4 MB of the same unmanaged low-bank hole right after the pool (modem pool 0xE0000000 + 16
 * MB, our pool + 1 MB, these + 4 MB, all below 0xF0000000), mapped write-combined like the
 * pool, so the SMMU's non-cached walks see every entry after a dsb. Mapped on first use.
 */
#define PT2_PA              0xE1100000ull
#define PT2_SIZE            0x00400000u
#define PT2_PAGES           (PT2_SIZE / 4096u)
static IOMemoryMap *pt2_map;
static uint8_t *pt2;
static uint32_t pt2_used;

static uint64_t *
pt_va(uint64_t pa)
{
	if (pa >= POOL_PA && pa < POOL_PA + POOL_SIZE) {
		return (uint64_t *)(pool + (pa - POOL_PA));
	}
	return (uint64_t *)(pt2 + (pa - PT2_PA));
}

static uint64_t
pt_alloc(uint64_t **va)
{
	if (pt_used < PT_PAGES) {
		uint32_t off = OFF_PT + pt_used++ * 4096u;
		*va = (uint64_t *)(pool + off);
		bzero(*va, 4096);
		return POOL_PA + off;
	}
	if (pt2 == NULL) {
		pt2_map = map_phys(PT2_PA, PT2_SIZE, true);
		if (pt2_map == NULL) {
			return 0;
		}
		pt2 = (uint8_t *)pt2_map->getVirtualAddress();
		LOG("  page tables: the pool's %u are used up, %u more at %llx", PT_PAGES, PT2_PAGES, PT2_PA);
	}
	if (pt2_used >= PT2_PAGES) {
		return 0;
	}
	uint32_t off = pt2_used++ * 4096u;
	*va = (uint64_t *)(pt2 + off);
	bzero(*va, 4096);
	return PT2_PA + off;
}

static uint64_t *
pt_next(uint64_t *e)
{
	if (*e == 0) {
		uint64_t *t;
		uint64_t pa = pt_alloc(&t);
		if (pa == 0) {
			return NULL;
		}
		*e = pa | PTE_TYPE_TABLE;
		return t;
	}
	if ((*e & 3) != PTE_TYPE_TABLE) {
		return NULL;                            /* a block is already there */
	}
	return pt_va(*e & PTE_ADDR_MASK);
}

static bool
map_block_2m(uint64_t va, uint64_t pa)
{
	uint64_t *l2 = pt_next(&pt_l1[(va >> 30) & 3]);
	if (l2 == NULL || va >= (1ull << 32)) {
		return false;
	}
	l2[(va >> 21) & 511] = (pa & ~0x1FFFFFull) | PTE_TYPE_BLOCK | PTE_RW_NC;
	return true;
}

static bool
map_page_4k(uint64_t va, uint64_t pa)
{
	uint64_t *l2 = pt_next(&pt_l1[(va >> 30) & 3]);
	uint64_t *l3 = l2 != NULL ? pt_next(&l2[(va >> 21) & 511]) : NULL;
	if (l3 == NULL || va >= (1ull << 32)) {
		return false;
	}
	l3[(va >> 12) & 511] = (pa & PTE_ADDR_MASK) | PTE_TYPE_TABLE | PTE_RW_NC;
	return true;
}

/* Clear one 4 KB entry, if its tables exist; the caller flushes the TLB. */
static void
unmap_page_4k(uint64_t va)
{
	uint64_t e1 = pt_l1[(va >> 30) & 3];
	if ((e1 & 3) != PTE_TYPE_TABLE) {
		return;
	}
	uint64_t *l2 = pt_va(e1 & PTE_ADDR_MASK);
	uint64_t e2 = l2[(va >> 21) & 511];
	if ((e2 & 3) != PTE_TYPE_TABLE) {
		return;
	}
	uint64_t *l3 = pt_va(e2 & PTE_ADDR_MASK);
	l3[(va >> 12) & 511] = 0;
}

/* TLBIASID 0, then TLBSYNC and wait for TLBSTATUS.SACTIVE to clear. */
static bool
tlb_flush(void)
{
	gmb();
	wr(smmu, 0x8610, 0);
	wr(smmu, 0x87f0, 0);
	return poll(smmu, 0x87f4, 1, 0, 1000);
}

static bool
smmu_pagetables(void)
{
	PE_Video video;

	if (smmu == NULL) {
		return false;
	}
	pt_used = 0;
	pt_l1_pa = pt_alloc(&pt_l1);
	bool ok = map_block_2m(POOL_PA, POOL_PA);
	PE_current_console(&video);
	uint64_t fb = video.v_baseAddr, fb_end = fb + (uint64_t)video.v_rowBytes * video.v_height;
	for (uint64_t a = fb & ~0x1FFFFFull; ok && fb != 0 && a < fb_end; a += 0x200000) {
		ok = map_block_2m(a, a);
	}
	if (!ok) {
		LOG("  SMMU: the identity blocks did not fit the page tables");
		return false;
	}
	gmb();
	wr(smmu, 0x8000, 0);                        /* CB0 off while it changes */
	wr(smmu, 0x8058, 0xFFFFFFFF);               /* CB0 FSR: clear */
	wr(smmu, 0x1800, 1);                        /* CBA2R0: VA64 */
	wr(smmu, 0x1000, SMMU_CBAR_S1);             /* CBAR0 */
	wr(smmu, 0x8010, SMMU_TCR2);
	wr(smmu, 0x8030, SMMU_TCR);
	wr(smmu, 0x8020, (uint32_t)pt_l1_pa);
	wr(smmu, 0x8024, (uint32_t)(pt_l1_pa >> 32));  /* TTBR0, ASID 0 */
	wr(smmu, 0x8028, 0);
	wr(smmu, 0x802C, 0);                        /* TTBR1 */
	wr(smmu, 0x8038, SMMU_MAIR0);
	wr(smmu, 0x803C, 0);
	gmb();
	wr(smmu, 0x8000, SMMU_SCTLR_XLATE);
	gmb();
	wr(smmu, 0x0C00, 0);                        /* S2CR0: translate through CB0 */
	wr(smmu, 0x0800, (1u << 31) | (1u << 16));  /* SMR0: SID 0, mask 1 */
	ok = tlb_flush();
	LOG("  SMMU page tables: L1 at %llx, %u tables used, identity for the pool and %llx-%llx; CB0 SCTLR %08x TCR %08x TCR2 %08x TTBR0 %08x%08x, TLB sync %s",
	    pt_l1_pa, pt_used, fb, fb_end, rd(smmu, 0x8000), rd(smmu, 0x8030), rd(smmu, 0x8010), rd(smmu, 0x8024),
	    rd(smmu, 0x8020), ok ? "done" : "STUCK");
	return true;
}

static bool
smmu_identity(void)
{
	if (smmu == NULL) {
		return false;
	}
	wr(smmu, 0x8058, 0xFFFFFFFF);               /* CB0 FSR: clear */
	wr(smmu, 0x8020, 0);
	wr(smmu, 0x8024, 0);                        /* TTBR0 */
	wr(smmu, 0x8030, 0);                        /* TCR */
	wr(smmu, 0x8010, 0);                        /* TCR2 */
	wr(smmu, 0x8038, 0);
	wr(smmu, 0x803C, 0);                        /* MAIR */
	wr(smmu, 0x1800, 1);                        /* CBA2R0: VA64 */
	wr(smmu, 0x1000, 0x0001f000);               /* CBAR0 */
	gmb();
	wr(smmu, 0x8000, 0xE0);                     /* CB0 SCTLR: M = 0, report faults */
	gmb();
	wr(smmu, 0x0C00, 0);                        /* S2CR0: translate through CB0 */
	wr(smmu, 0x0800, (1u << 31) | (1u << 16));  /* SMR0: SID 0, mask 1 */
	gmb();
	LOG("  SMMU: SMR0 %08x S2CR0 %08x CBAR0 %08x CBA2R0 %08x CB0 SCTLR %08x", rd(smmu, 0x800), rd(smmu, 0xC00),
	    rd(smmu, 0x1000), rd(smmu, 0x1800), rd(smmu, 0x8000));
	return true;
}

static void
smmu_fault(const char *tag)
{
	if (smmu == NULL) {
		return;
	}
	LOG("  [%s] SMMU gfsr %08x CB0 SCTLR %08x FSR %08x FAR %08x%08x FSYNR0 %08x", tag, rd(smmu, 0x48),
	    rd(smmu, 0x8000), rd(smmu, 0x8058), rd(smmu, 0x8064), rd(smmu, 0x8060), rd(smmu, 0x8068));
}

/* ---------------------------------------------------------------- CP bring-up */

/* The subset of Linux a6xx_gpu.c hw_init the command processor needs to run at all. */
static void
hw_init_regs(void)
{
	static const uint32_t addr_mode[] = { 0x842, 0xC01, 0x8601, 0x8e05, 0x9e01, 0xbe05, 0xa601, 0x9601,
		                              0xE00, 0xae01, 0xb601, 0xF810 };

	gwr(0x3c45, 0);                             /* GBIF_HALT */
	(void)grd(0x3c45);
	gwr(0x16, 0);                               /* RBBM_GBIF_HALT */
	(void)grd(0x16);
	gwr(0xF803, 0);                             /* RBBM_SECVID_TSB_CNTL */
	gwr64(0xF800, 0);                           /* SECVID_TSB_TRUSTED_BASE */
	gwr(0xF802, 0);                             /* SECVID_TSB_TRUSTED_SIZE */
	for (unsigned i = 0; i < sizeof(addr_mode) / sizeof(addr_mode[0]); i++) {
		gwr(addr_mode[i], 1);               /* 64-bit addressing in every block */
	}
	for (unsigned i = 0; i < 4; i++) {
		gwr(0x3c03 + i, 0x00071620);        /* GBIF_QSB_SIDE0..3 (a610) */
	}
	gwr(0x11, 3);                               /* RBBM_GBIF_CLIENT_QOS_CNTL */
	gwr64(0xE05, 0x1fffffffff000ull + 0xfc0);   /* UCHE_WRITE_RANGE_MAX */
	gwr64(0xE09, 0x1fffffffff000ull);           /* UCHE_TRAP_BASE */
	gwr64(0xE07, 0x1fffffffff000ull);           /* UCHE_WRITE_THRU_BASE */
	gwr64(0xE0B, 0x100000);                     /* UCHE_GMEM_RANGE_MIN */
	gwr64(0xE0D, 0x100000 + 0x21000 - 1);       /* UCHE_GMEM_RANGE_MAX: gmem 128K + 4K */
	gwr(0xE18, 0x804);                          /* UCHE_FILTER_CNTL */
	gwr(0xE17, 4);                              /* UCHE_CACHE_WAYS */
	gwr(0x8c2, 0x00800060);                     /* CP_ROQ_THRESHOLDS_2 */
	gwr(0x8c1, 0x40201b16);                     /* CP_ROQ_THRESHOLDS_1 */
	gwr(0x8C3, 48);                             /* CP_MEM_POOL_SIZE */
	gwr(0x9e00, 0x00080000);                    /* PC_DBG_ECO_CNTL: prim fifo threshold */
	gwr(0x98d, 1);                              /* CP_AHB_CNTL */
	gwr(0x1f, (1u << 30) | 0x3ffff);            /* RBBM_INTERFACE_HANG_INT_CNTL */
	gwr(0xe19, 0x81);                           /* UCHE_CLIENT_PF */
}

/* Type-7 packet header: opcode and count each carry an odd-parity bit. */
static uint32_t
par(uint32_t v)
{
	return (0x9669u >> (0xFu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^
	    (v >> 28)))) & 1;
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (par(cnt) << 15) | ((op & 0x7F) << 16) | (par(op) << 23);
}

/* Type-4 packet header: write cnt registers from reg on, with the same odd-parity bits. */
static uint32_t
pkt4(uint32_t reg, uint32_t cnt)
{
	return 0x40000000u | cnt | (par(cnt) << 7) | ((reg & 0x3FFFF) << 8) | (par(reg) << 27);
}

static void
emit(uint32_t v)
{
	ring[ring_wptr % RING_DWORDS] = v;
	ring_wptr++;
}

/* Publish the write pointer and wait for the CP to catch up. */
static bool
submit(const char *what, uint32_t ms)
{
	uint32_t wptr = ring_wptr % RING_DWORDS, rptr = 0;

	gmb();
	gwr(REG_CP_RB_WPTR, wptr);
	for (uint32_t i = 0; i < ms * 100; i++) {
		rptr = grd(REG_CP_RB_RPTR);
		if (rptr == wptr) {
			if (!service_ready) {           /* (the tests' lines; frame after frame they cost time) */
				LOG("  %s: done (rptr %u) after ~%u us, RBBM_STATUS %08x", what, rptr, i * 10,
				    grd(REG_RBBM_STATUS));
			}
			step_done();
			return true;
		}
		IODelay(10);
	}
	LOG("  %s: TIMEOUT rptr %u wptr %u RBBM_STATUS %08x INT_0_STATUS %08x CP_HW_FAULT %08x", what, rptr, wptr,
	    grd(REG_RBBM_STATUS), grd(REG_RBBM_INT_0_STATUS), grd(REG_CP_HW_FAULT));
	smmu_fault(what);
	step_done();
	return false;
}

/* Room for `need` more dwords in the ring (the CP reads behind us): wait up to ms. */
static bool
ring_room(uint32_t need, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 100; i++) {
		uint32_t rptr = grd(REG_CP_RB_RPTR), w = ring_wptr % RING_DWORDS;
		if ((rptr + RING_DWORDS - w - 1) % RING_DWORDS >= need) {
			return true;
		}
		IODelay(10);
	}
	return false;
}

/* Hand what was emitted to the CP and return at once (the msm client's submissions). */
static void
kick(void)
{
	gmb();
	gwr(REG_CP_RB_WPTR, ring_wptr % RING_DWORDS);
}

/*
 * Start the command processor and prove it runs: its microcode (a630_sqe.fw without its
 * first dword, as adreno_fw_create_bo copies it), a 32 KB ring, CP_ME_INIT, then leaving
 * secure mode, and a CP_MEM_WRITE the CPU reads back from RAM.
 */
static bool
cp_start(bool have_zap)
{
	uint8_t *sqe = pool + OFF_SQE;
	int64_t fw_size;

	LOG("--- CP start");
	step_done();
	/* adreno_fw_create_bo copies the firmware from its second dword on */
	fw_size = fw_read("a630_sqe.fw", sqe, OFF_RING - OFF_SQE);
	if (fw_size <= 4) {
		return false;
	}
	/* dst is below src, so a forward word copy is the shift */
	uint32_t *w = (uint32_t *)sqe;
	for (uint32_t i = 0; i < (uint32_t)(fw_size - 4 + 3) / 4; i++) {
		w[i] = w[i + 1];
	}
	test_buf[0] = 0x11111111;
	gmb();
	LOG("  sqe %lld bytes at %llx, ring at %llx, test buffer at %llx", fw_size - 4, POOL_PA + OFF_SQE,
	    POOL_PA + OFF_RING, POOL_PA + OFF_TEST);
	step_done();

	if (!(mmu ? smmu_pagetables() : smmu_identity())) {
		return false;
	}
	step_done();
	hw_init_regs();
	LOG("  hw_init done: RBBM_STATUS %08x", grd(REG_RBBM_STATUS));
	step_done();
	gwr64(REG_CP_SQE_INSTR_BASE, POOL_PA + OFF_SQE);
	gwr64(REG_CP_RB_BASE, POOL_PA + OFF_RING);
	/* 32 KB ring, 32-byte blocks, the CP does not write the read pointer back to memory */
	gwr(REG_CP_RB_CNTL, 12 | (2 << 8) | (1u << 27));
	ring_wptr = 0;
	gwr(REG_CP_RB_WPTR, 0);
	gwr(REG_CP_SQE_CNTL, 1);
	LOG("  SQE started: RBBM_STATUS %08x CP_HW_FAULT %08x rptr %u", grd(REG_RBBM_STATUS), grd(REG_CP_HW_FAULT),
	    grd(REG_CP_RB_RPTR));
	step_done();
	if (stop_here(2, "starting the SQE")) {
		return false;
	}

	emit(pkt7(CP_ME_INIT, 8));
	emit(0x2f);
	emit(3);
	emit(0x20000000);
	emit(0);
	emit(0);
	emit(0);
	emit(0);
	emit(0);
	if (!submit("CP_ME_INIT", 100)) {
		return false;
	}
	smmu_fault("after CP_ME_INIT");
	if (stop_here(3, "CP_ME_INIT")) {
		return false;
	}
	if (have_zap) {
		emit(pkt7(CP_SET_SECURE_MODE, 1));
		emit(0);
		if (!submit("CP_SET_SECURE_MODE 0", 100)) {
			return false;
		}
		smmu_fault("after CP_SET_SECURE_MODE");
		if (stop_here(4, "CP_SET_SECURE_MODE 0")) {
			return false;
		}
	} else {
		/* no zap shader: tell the hardware directly, and hope the bootloader left it open */
		gwr(REG_RBBM_SECVID_TRUST_CNTL, 0);
		LOG("  no zap shader: RBBM_SECVID_TRUST_CNTL = 0, RBBM_STATUS %08x", grd(REG_RBBM_STATUS));
		step_done();
	}
	emit(pkt7(CP_MEM_WRITE, 3));
	emit((uint32_t)(POOL_PA + OFF_TEST));
	emit((uint32_t)((POOL_PA + OFF_TEST) >> 32));
	emit(0xC0FFEE00);
	if (!submit("CP_MEM_WRITE", 100)) {
		return false;
	}
	IODelay(100);
	gmb();
	LOG("  test buffer: %08x (c0ffee00 = the GPU wrote it)", test_buf[0]);
	smmu_fault("after the test");
	step_done();
	return test_buf[0] == 0xC0FFEE00;
}

static void
reg1(uint32_t reg, uint32_t v)
{
	emit(pkt4(reg, 1));
	emit(v);
}

static uint32_t fence_seq;                      /* the last timestamp handed to the GPU */
static uint32_t msm_seq;                        /* 0153: the last msm submission's (OFF_MSM_FENCE) */

static void
event(uint32_t ev, bool ts)
{

	if (!ts) {
		emit(pkt7(CP_EVENT_WRITE, 1));
		emit(ev);
		return;
	}
	emit(pkt7(CP_EVENT_WRITE, 4));
	/*
	 * 0154: the event alone, so the CP writes the seqno below when the event is done. Bit 30 of
	 * this dword is TIMESTAMP, which writes the GPU's always-on counter (64 bits) instead -- what
	 * turnip asks for in its trace timestamps -- and every fence of 0145-0153 was that counter:
	 * "done" while it was under 2^31 (the first ~112 s of a boot at 19.2 MHz), never after.
	 */
	emit(ev);
	emit((uint32_t)(POOL_PA + OFF_FENCE));
	emit((uint32_t)((POOL_PA + OFF_FENCE) >> 32));
	emit(++fence_seq);
}

/*
 * The 2D blitter, as Mesa's freedreno does a buffer clear: blit_begin() emits the a610's
 * batch-start registers, cleans and invalidates the CCU, sets render mode BLIT2DSCALE, the CCU
 * bypass (sysmem) layout, the clear colour and the blit format; blit_rect() emits one
 * destination and one CP_BLIT(SCALE); blit_end() cleans the CCU and the caches, waits for idle
 * and submits, so the pixels are in RAM when it returns.
 */
static void
blit_setup(bool solid, uint32_t rgba)
{
	uint32_t fmt = FMT6_8_8_8_8_UNORM;
	uint32_t blt = (0xFu << 20) | (fmt << 8) | (solid ? (1u << 7) : 0);  /* MASK, COLOR_FORMAT, IFMT UNORM8, SOLID_COLOR */

	/* the a610's batch-start registers (freedreno_devices.py magic_regs / raw_magic_regs) */
	reg1(0x9804, 0xf);                          /* PC_MODE_CNTL */
	reg1(0x9805, 0);                            /* PC_POWER_CNTL */
	reg1(0xa0f8, 0);                            /* VFD_POWER_CNTL */
	reg1(0xb600, 0);                            /* TPL1_DBG_ECO_CNTL */
	reg1(0x8600, 0);                            /* GRAS_DBG_ECO_CNTL */
	reg1(0xae03, 0);                            /* SP_CHICKEN_BITS */
	reg1(0xae00, 0);                            /* SP_DBG_ECO_CNTL */
	reg1(0xbe04, 0);                            /* HLSQ_DBG_ECO_CNTL */
	reg1(0x9600, 0);                            /* VPC_DBG_ECO_CNTL */
	reg1(0x0e12, 0x10000000);                   /* UCHE_UNKNOWN_0E12 */
	reg1(REG_RB_RBP_CNTL, 1);
	reg1(REG_RB_DBG_ECO_CNTL, 0x04100000);      /* the same value for blits on a610: no toggling */

	/* emit_setup */
	event(EV_PC_CCU_FLUSH_COLOR_TS, true);
	event(EV_PC_CCU_FLUSH_DEPTH_TS, true);
	event(EV_PC_CCU_INVALIDATE_COLOR, false);
	event(EV_PC_CCU_INVALIDATE_DEPTH, false);
	emit(pkt7(CP_SET_MARKER, 1));
	emit(RM6_BLIT2DSCALE);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	/* sysmem layout: depth cache at 0, colour at num_ccu (1) x 8 KB; fast clear off on gen1_low */
	reg1(REG_RB_CCU_CNTL, (1u << 0) | ((0x2000u >> 12) << 23));

	/* emit_clear_color (R2D_UNORM8: one byte per dword, in the format's channel order) */
	if (solid) {
		emit(pkt4(REG_RB_A2D_CLEAR_COLOR_DW0, 4));
		emit((rgba >> 24) & 0xff);
		emit((rgba >> 16) & 0xff);
		emit((rgba >> 8) & 0xff);
		emit(rgba & 0xff);
	}
	/* emit_blit_setup */
	reg1(REG_RB_A2D_BLT_CNTL, blt);
	reg1(REG_GRAS_A2D_BLT_CNTL, blt);
	reg1(REG_SP_A2D_OUTPUT_INFO, (fmt << 3) | (0xFu << 12));  /* IFMT_TYPE FLOAT, COLOR_FORMAT, MASK */
	reg1(REG_RB_A2D_PIXEL_CNTL, 0);
}

static void
blit_begin(uint32_t rgba)
{
	blit_setup(true, rgba);
}

/* One rectangle [x0, x1] x [y0, y1] of a linear surface: base 64-byte aligned, pitch a multiple of 64. */
static void
blit_rect(uint64_t base, uint32_t pitch, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
	emit(pkt4(REG_RB_A2D_DEST_BUFFER_INFO, 4));
	emit(FMT6_8_8_8_8_UNORM);                   /* TILE6_LINEAR, WZYX */
	emit((uint32_t)base);
	emit((uint32_t)(base >> 32));
	emit(pitch >> 6);
	emit(pkt4(REG_GRAS_A2D_DEST_TL, 2));
	emit(x0 | (y0 << 16));
	emit(x1 | (y1 << 16));
	/* emit_blit_fini */
	event(EV_DEBUG_LABEL, false);
	emit(pkt7(CP_BLIT, 1));
	emit(BLIT_OP_SCALE);
}

/*
 * boot-arg gpu_copy_async (default 1, 0152): once the service is up, a copy is handed to the CP and
 * not waited for. The CP runs the ring in order, so nothing queued after it (the next frame's
 * drawing into the same surface) overtakes it; waiting only cost the CPU (1.8 s in 5 s, measured)
 * and held the frame up behind the drawing queued before it.
 */
static uint32_t copy_async = 1;

static bool
blit_end(const char *what)
{
	event(EV_PC_CCU_FLUSH_COLOR_TS, true);
	event(EV_PC_CCU_FLUSH_DEPTH_TS, true);
	event(EV_CACHE_FLUSH_TS, true);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	if (copy_async && service_ready) {
		kick();
		return true;
	}
	return submit(what, 200);
}

/*
 * The first thing the GPU draws: a 256 x 256 RGBA8 solid fill into a buffer the CPU has filled
 * with a pattern first, then read back pixel by pixel.
 */
static bool
blit_fill(uint32_t rgba)
{
	uint32_t *pix = (uint32_t *)(pool + OFF_DRAW);
	uint32_t good = 0, bad_at = 0xFFFFFFFF, bad_val = 0;

	LOG("--- first draw: 2D blitter, %ux%u RGBA8 at %llx, colour %08x", DRAW_W, DRAW_H, POOL_PA + OFF_DRAW, rgba);
	for (uint32_t i = 0; i < DRAW_W * DRAW_H; i++) {
		pix[i] = 0xDEADBEEF;
	}
	gmb();
	blit_begin(rgba);
	blit_rect(POOL_PA + OFF_DRAW, DRAW_PITCH, 0, 0, DRAW_W - 1, DRAW_H - 1);
	if (!blit_end("2D fill")) {
		return false;
	}
	IODelay(100);
	gmb();

	for (uint32_t i = 0; i < DRAW_W * DRAW_H; i++) {
		if (pix[i] == __builtin_bswap32(rgba)) {
			good++;
		} else if (bad_at == 0xFFFFFFFF) {
			bad_at = i;
			bad_val = pix[i];
		}
	}
	LOG("  pixels: first %08x, centre %08x, last %08x; %u of %u as expected (%08x in memory)", pix[0],
	    pix[(DRAW_H / 2) * DRAW_W + DRAW_W / 2], pix[DRAW_W * DRAW_H - 1], good, DRAW_W * DRAW_H,
	    __builtin_bswap32(rgba));
	if (bad_at != 0xFFFFFFFF) {
		LOG("  first wrong pixel: #%u (x %u, y %u) = %08x", bad_at, bad_at % DRAW_W, bad_at / DRAW_W, bad_val);
	}
	LOG("  RBBM_STATUS %08x CP_HW_FAULT %08x RBBM_INT_0_STATUS %08x", grd(REG_RBBM_STATUS), grd(REG_CP_HW_FAULT),
	    grd(REG_RBBM_INT_0_STATUS));
	return good == DRAW_W * DRAW_H;
}

/*
 * A copy of a w x h RGBA8 rectangle between two linear surfaces, as Mesa's emit_blit_buffer does
 * one (TPL1_A2D_SRC_TEXTURE_* for the source, GRAS_A2D_SRC_* its rectangle, then the same
 * destination and CP_BLIT). Both bases must be 64-byte aligned and both pitches multiples of 64,
 * so a pitch that is not (the panel's 4320) is split as the demo does: k interleaved surfaces of
 * pitch k x pitch, each base rounded down to 64 bytes with the difference moved into x.
 */
static bool
blit_copy(uint64_t src, uint32_t spitch, uint64_t dst, uint32_t dpitch, uint32_t w, uint32_t h)
{
	uint32_t k = ((spitch | dpitch) & 63) == 0 ? 1 : 2;

	if (((k * spitch) & 63) != 0 || ((k * dpitch) & 63) != 0 || (src & 3) != 0 || (dst & 3) != 0 ||
	    w + 16 > 0x4000 || h > 2 * 0x3fff) {
		LOG("  blit_copy: pitches %u/%u or size %ux%u do not fit the blitter", spitch, dpitch, w, h);
		return false;
	}
	blit_setup(false, 0);
	for (uint32_t j = 0; j < k && j < h; j++) {
		uint64_t s0 = src + (uint64_t)j * spitch, d0 = dst + (uint64_t)j * dpitch;
		uint32_t sx = (uint32_t)(s0 & 63) / 4, dx = (uint32_t)(d0 & 63) / 4;
		uint32_t n = (h - j + k - 1) / k;
		uint64_t sb = s0 & ~63ull, db = d0 & ~63ull;

		emit(pkt4(REG_TPL1_A2D_SRC_TEXTURE_INFO, 5));        /* INFO, SIZE, BASE lo/hi, PITCH */
		emit(FMT6_8_8_8_8_UNORM | (1u << 20) | (1u << 22));   /* LINEAR, WZYX, UNK20, UNK22 as Mesa */
		emit((sx + w) | (n << 15));                           /* WIDTH, HEIGHT */
		emit((uint32_t)sb);
		emit((uint32_t)(sb >> 32));
		emit(((k * spitch) >> 6) << 9);                       /* PITCH, bits 9..23, in 64-byte units */
		emit(pkt4(REG_GRAS_A2D_SRC_XMIN, 4));
		emit(sx << 8);
		emit((sx + w - 1) << 8);
		emit(0);
		emit((n - 1) << 8);
		blit_rect(db, k * dpitch, dx, 0, dx + w - 1, n - 1);
	}
	return blit_end("copy");
}

/*
 * sapphire 0239: the source rectangle [sx, sx + sw) x [sy, sy + sh) (pixels << 8: the blitter's
 * source coordinates have 8 fraction bits) scaled, bilinear, onto dw x dh pixels at dst. For iOS
 * drawing at 2x (SweetFramebuffer, sweetfb_scale=2): 720 x 1600 of a 750 x 1624 surface onto the
 * 1080 x 2400 panel. The source pitch must be a multiple of 64 bytes; a destination pitch that is
 * not (the panel's 4320) is done as blit_copy does it, in k passes of every k-th row, each pass's
 * source rows k times as far apart and started where its first row's centre falls.
 */
static bool
blit_scale(uint64_t src, uint32_t spitch, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
    uint64_t dst, uint32_t dpitch, uint32_t dw, uint32_t dh)
{
	uint32_t k = (dpitch & 63) == 0 ? 1 : 2;

	if ((spitch & 63) != 0 || ((k * dpitch) & 63) != 0 || (src & 3) != 0 || (dst & 3) != 0 || dw == 0 || dh == 0 ||
	    sw == 0 || sh == 0 || dw + 16 > 0x4000 || ((sx + sw) >> 8) + 16 > 0x4000 || ((sy + sh) >> 8) + 2 > 0x7fff) {
		LOG("  blit_scale: pitches %u/%u or sizes do not fit the blitter", spitch, dpitch);
		return false;
	}
	uint64_t sb = src & ~63ull;
	uint32_t sadd = (uint32_t)(src & 63) / 4;               /* the base's misalignment, as pixels */
	uint32_t swidth = sadd + ((sx + sw) >> 8) + 1, sheight = ((sy + sh) >> 8) + 2;
	blit_setup(false, 0);
	for (uint32_t j = 0; j < k && j < dh; j++) {
		uint64_t d0 = dst + (uint64_t)j * dpitch;
		uint32_t dx = (uint32_t)(d0 & 63) / 4, n = (dh - j + k - 1) / k;
		/* the pass's n rows sample k * n * (sh / dh) source rows, the first centred on row j's */
		int64_t span = (int64_t)sh * k * n / dh;
		int64_t y0 = (int64_t)sy + ((2 * (int64_t)j + 1 - (int64_t)k) * (int64_t)sh) / (2 * (int64_t)dh);
		if (y0 < 0) {
			y0 = 0;
		}
		emit(pkt4(REG_TPL1_A2D_SRC_TEXTURE_INFO, 5));        /* INFO, SIZE, BASE lo/hi, PITCH */
		emit(FMT6_8_8_8_8_UNORM | (1u << 16) | (1u << 20) | (1u << 22));  /* LINEAR, WZYX, FILTER, UNK20, UNK22 */
		emit(swidth | (sheight << 15));
		emit((uint32_t)sb);
		emit((uint32_t)(sb >> 32));
		emit((spitch >> 6) << 9);
		emit(pkt4(REG_GRAS_A2D_SRC_XMIN, 4));
		emit((sadd << 8) + sx);
		emit((sadd << 8) + sx + sw - 256);
		emit((uint32_t)y0);
		emit((uint32_t)(y0 + span - 256));
		blit_rect(d0 & ~63ull, k * dpitch, dx, 0, dx + dw - 1, n - 1);
	}
	return blit_end("scale");
}

/*
 * boot-arg gpu_bench=1: is the GPU worth handing AppleCLCD's per-frame copy? That copy is a
 * full 1080 x 2400 frame (10 MB) from CoreAnimation's surface into the flip buffer, memcpy then a
 * cache clean for the MDP, every frame. The GPU writes RAM behind the caches, so taking it over
 * costs a clean of the source and an invalidate of the destination instead. This times each of
 * those on the real buffers' sizes, the GPU copy of the boot framebuffer into a contiguous 10 MB
 * buffer, and checks that copy against its source, row by row.
 */
static uint32_t bench;


static uint64_t
us_since(uint64_t t0)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	return ns / 1000;
}

static void
bench_copy(void)
{
	PE_Video video;
	IOBufferMemoryDescriptor *a = NULL, *b = NULL;

	PE_current_console(&video);
	uint32_t w = (uint32_t)video.v_width, h = (uint32_t)video.v_height, dpitch = (uint32_t)video.v_rowBytes;
	uint32_t spitch = (w * 4 + 511) & ~511u;        /* CoreAnimation's surfaces: 1125 px in 4608 bytes */
	size_t dsize = (size_t)dpitch * h, ssize = (size_t)spitch * h;

	LOG("--- gpu_bench: one %ux%u frame, source %u bytes a row (%zu), destination %u (%zu)", w, h, spitch,
	    ssize, dpitch, dsize);
	a = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous, dsize, 0x00000000fffff000ull);
	b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous, ssize, 0x00000000fffff000ull);
	if (a == NULL || b == NULL || w == 0 || h == 0) {
		LOG("  no memory for the benchmark");
		OSSafeReleaseNULL(a);
		OSSafeReleaseNULL(b);
		return;
	}
	uint8_t *av = (uint8_t *)a->getBytesNoCopy(), *bv = (uint8_t *)b->getBytesNoCopy();
	uint64_t apa = a->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
	uint64_t bpa = b->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
	uint64_t t, t_cpy, t_cpy_clean, t_src_clean, t_dst_flush, t_gpu, t_inv;
	uint32_t bad_rows = 0, first_bad = 0xFFFFFFFF;

	/* the flip path today: memcpy cached -> cached, row by row (pitches differ), then clean for the MDP */
	memset(bv, 0x5a, ssize);
	memset(av, 0, dsize);
	t = mach_absolute_time();
	for (uint32_t y = 0; y < h; y++) {
		memcpy(av + (uint64_t)y * dpitch, bv + (uint64_t)y * spitch, (size_t)w * 4);
	}
	t_cpy = us_since(t);
	t = mach_absolute_time();
	sweet_dcache_clean((vm_offset_t)av, (unsigned)dsize);
	t_cpy_clean = us_since(t);

	/* a frame as CoreAnimation leaves one: written by the CPU, in its cache; every pixel unique */
	for (uint32_t y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(bv + (uint64_t)y * spitch);
		for (uint32_t x = 0; x < w; x++) {
			row[x] = (y << 12) ^ x ^ 0xA5000000u;
		}
	}
	memset(av, 0x11, dsize);                    /* the destination dirty in the cache too */

	/* the GPU path: clean the source, flush the destination, copy, drop the destination's lines */
	t = mach_absolute_time();
	sweet_dcache_clean((vm_offset_t)bv, (unsigned)ssize);
	t_src_clean = us_since(t);
	t = mach_absolute_time();
	sweet_dcache_flush((vm_offset_t)av, (unsigned)dsize);
	t_dst_flush = us_since(t);
	t = mach_absolute_time();
	bool ok = blit_copy(bpa, spitch, apa, dpitch, w, h);
	t_gpu = us_since(t);
	t = mach_absolute_time();
	sweet_dcache_flush((vm_offset_t)av, (unsigned)dsize);
	t_inv = us_since(t);

	for (uint32_t y = 0; ok && y < h; y++) {
		if (memcmp(av + (uint64_t)y * dpitch, bv + (uint64_t)y * spitch, (size_t)w * 4) != 0) {
			if (first_bad == 0xFFFFFFFF) {
				first_bad = y;
			}
			bad_rows++;
		}
	}
	LOG("  CPU today:  copy %llu us + clean %llu us = %llu us a frame", t_cpy, t_cpy_clean, t_cpy + t_cpy_clean);
	LOG("  GPU copy:   %s in %llu us (submit to idle, the CPU polling), %u of %u rows exactly like the source",
	    ok ? "done" : "FAILED", t_gpu, ok ? h - bad_rows : 0, h);
	if (ok && bad_rows) {
		const uint32_t *gr = (const uint32_t *)(av + (uint64_t)first_bad * dpitch);
		const uint32_t *sr = (const uint32_t *)(bv + (uint64_t)first_bad * spitch);
		LOG("  first row that differs: %u: x0 %08x (want %08x), x%u %08x (want %08x)", first_bad, gr[0], sr[0],
		    w - 1, gr[w - 1], sr[w - 1]);
	}
	LOG("  GPU path:   clean source %llu us + flush dest %llu us + GPU %llu us + drop dest %llu us = %llu us a frame",
	    t_src_clean, t_dst_flush, t_gpu, t_inv, t_src_clean + t_dst_flush + t_gpu + t_inv);
	OSSafeReleaseNULL(a);
	OSSafeReleaseNULL(b);
}

/*
 * The test GPU virtual memory exists for: 256 KB of ordinary kernel memory, allocated without
 * asking for contiguity (so scattered over physical pages the way iOS surfaces are), mapped
 * page by page at one contiguous GPU address, filled by the 2D blitter through that address and
 * read back by the CPU through its own mapping. The identity bank could never do this.
 */
#define MMU_TEST_VA         0x10000000ull       /* below RAM: no overlap with the identity blocks */

static bool
mmu_test(void)
{
	IOBufferMemoryDescriptor *m = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut,
	    DRAW_W * DRAW_H * 4, 4096);
	uint32_t runs = 0, good = 0, bad_at = 0xFFFFFFFF, bad_val = 0;
	uint64_t prev = 0, lo = ~0ull, hi = 0;
	const uint32_t rgba = 0x20C0FFFFu;
	bool ok = true;

	LOG("--- gpu_mmu: %u KB of scattered memory at GPU address %llx", DRAW_W * DRAW_H * 4 / 1024, MMU_TEST_VA);
	if (m == NULL) {
		LOG("  no memory");
		return false;
	}
	uint32_t *cpu = (uint32_t *)m->getBytesNoCopy();
	for (uint32_t off = 0; ok && off < DRAW_W * DRAW_H * 4; off += 4096) {
		uint64_t len = 0;
		uint64_t pa = m->getPhysicalSegment(off, &len, kIOMemoryMapperNone);
		if (pa == 0) {
			ok = false;
			break;
		}
		if (pa != prev + 4096) {
			runs++;
		}
		prev = pa;
		lo = pa < lo ? pa : lo;
		hi = pa > hi ? pa : hi;
		ok = map_page_4k(MMU_TEST_VA + off, pa);
	}
	if (!ok || !tlb_flush()) {
		LOG("  mapping failed (%u tables used)", pt_used);
		m->release();
		return false;
	}
	LOG("  %u pages in %u physical runs, between %llx and %llx; %u page tables in use", DRAW_W * DRAW_H * 4 / 4096,
	    runs, lo, hi + 4095, pt_used);
	for (uint32_t i = 0; i < DRAW_W * DRAW_H; i++) {
		cpu[i] = 0xDEADBEEF;
	}
	sweet_dcache_flush((vm_offset_t)cpu, DRAW_W * DRAW_H * 4);  /* in RAM, nothing dirty left behind */

	blit_begin(rgba);
	blit_rect(MMU_TEST_VA, DRAW_PITCH, 0, 0, DRAW_W - 1, DRAW_H - 1);
	ok = blit_end("2D fill through the GPU MMU");
	sweet_dcache_flush((vm_offset_t)cpu, DRAW_W * DRAW_H * 4);  /* drop any line the CPU still holds */
	for (uint32_t i = 0; ok && i < DRAW_W * DRAW_H; i++) {
		if (cpu[i] == __builtin_bswap32(rgba)) {
			good++;
		} else if (bad_at == 0xFFFFFFFF) {
			bad_at = i;
			bad_val = cpu[i];
		}
	}
	LOG("  pixels: %u of %u as expected (%08x); first %08x, last %08x", good, DRAW_W * DRAW_H,
	    __builtin_bswap32(rgba), cpu[0], cpu[DRAW_W * DRAW_H - 1]);
	if (bad_at != 0xFFFFFFFF) {
		LOG("  first wrong pixel: #%u (page %u, x %u, y %u) = %08x", bad_at, bad_at * 4 / 4096, bad_at % DRAW_W,
		    bad_at / DRAW_W, bad_val);
	}
	smmu_fault("after the MMU test");
	m->release();
	return ok && good == DRAW_W * DRAW_H;
}

/*
 * boot-arg gpu_3d=1 (needs gpu_mmu=1): the first 3D draw, replayed. Mesa's Vulkan driver
 * (turnip), run on a PC under drm-shim impersonating this very a610, recorded one submit:
 * a clear to dark blue, one textured quad blended source-over (a CoreAnimation layer, in
 * short) and a blit of the result to a second image. metal/tools/rd2replay.py turned that
 * capture into SapphireReplay3D.h: every buffer at the GPU address turnip gave it (from
 * 0x30000000, below RAM, free in our page tables), the non-zero pages and the command
 * stream. Here those buffers get real pages, the texture is generated (x in red, y in
 * green), the stream runs as an indirect buffer, and the CPU checks every pixel of both
 * images. Then the 2D blitter puts the picture on the panel so it can be seen.
 */
#include "SapphireReplay3D.h"

#define RP_RT       0x30026000u                 /* render target, BGRA8 256 x 256, pitch 1024 */
#define RP_TX       0x30066000u                 /* texture, RGBA8 256 x 256 */
#define RP_DS       0x300a6000u                 /* blit destination */
#define RP_W        256u
#define RP_PITCH    1024u
#define CP_INDIRECT_BUFFER 0x3f

static uint32_t draw3d;

static uint32_t
rp_check(const uint32_t *img, uint32_t *bad_x, uint32_t *bad_y, uint32_t *bad_v, uint32_t *want)
{
	uint32_t good = 0;

	*bad_x = 0xFFFFFFFF;
	for (uint32_t y = 0; y < RP_W; y++) {
		for (uint32_t x = 0; x < RP_W; x++) {
			uint32_t v = img[y * RP_W + x], ok, w;
			if (x >= 64 && x < 192 && y >= 64 && y < 192) {
				/* bilinear at texel 2(x-64)+0.5: red and green within one of 2(x-64) */
				uint32_t r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, er = 2 * (x - 64), eg = 2 * (y - 64);
				w = 0xff000000u | (er << 16) | (eg << 8);
				ok = (v >> 24) == 0xff && (v & 0xff) == 0 && r - er <= 1 && g - eg <= 1;
			} else {
				w = 0xff000080u;                /* the clear: (0, 0, 0.5, 1) as B, G, R, A bytes */
				ok = v == w;
			}
			if (ok) {
				good++;
			} else if (*bad_x == 0xFFFFFFFF) {
				*bad_x = x;
				*bad_y = y;
				*bad_v = v;
				*want = w;
			}
		}
	}
	return good;
}

static bool
replay_3d(void)
{
	const uint32_t span = rp3d_VA_END - rp3d_VA_START;
	IOBufferMemoryDescriptor *m = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut,
	    span, 4096);
	uint32_t bx, by, bv, bw;
	bool ok = true;

	LOG("--- gpu_3d: turnip's clear + textured quad + blit, %u KB of buffers at GPU address %x, %u command streams",
	    span / 1024, rp3d_VA_START, (unsigned)(sizeof(rp3d_cmds) / sizeof(rp3d_cmds[0])));
	if (m == NULL) {
		LOG("  no memory");
		return false;
	}
	uint8_t *cpu = (uint8_t *)m->getBytesNoCopy();
	bzero(cpu, span);
	const uint32_t *run = rp3d_page_runs;
	for (uint32_t p = 0; p < sizeof(rp3d_page_va) / sizeof(rp3d_page_va[0]); p++) {
		uint32_t *pg = (uint32_t *)(cpu + (rp3d_page_va[p] - rp3d_VA_START));
		while (*run != 0xFFFFFFFFu) {
			uint32_t at = run[0], n = run[1];
			memcpy(pg + at, run + 2, n * 4);
			run += 2 + n;
		}
		run++;
	}
	uint32_t *tx = (uint32_t *)(cpu + (RP_TX - rp3d_VA_START));
	uint32_t *rt = (uint32_t *)(cpu + (RP_RT - rp3d_VA_START));
	uint32_t *ds = (uint32_t *)(cpu + (RP_DS - rp3d_VA_START));
	for (uint32_t y = 0; y < RP_W; y++) {
		for (uint32_t x = 0; x < RP_W; x++) {
			tx[y * RP_W + x] = 0xff000000u | (y << 8) | x;
			rt[y * RP_W + x] = 0xDEADBEEF;
			ds[y * RP_W + x] = 0xDEADBEEF;
		}
	}
	for (uint32_t off = 0; ok && off < span; off += 4096) {
		uint64_t len = 0, pa = m->getPhysicalSegment(off, &len, kIOMemoryMapperNone);
		ok = pa != 0 && map_page_4k(rp3d_VA_START + off, pa);
	}
	if (!ok || !tlb_flush()) {
		LOG("  mapping failed (%u tables used)", pt_used);
		m->release();
		return false;
	}
	sweet_dcache_flush((vm_offset_t)cpu, span);

	for (uint32_t i = 0; i < sizeof(rp3d_cmds) / sizeof(rp3d_cmds[0]); i++) {
		emit(pkt7(CP_INDIRECT_BUFFER, 3));
		emit(rp3d_cmds[i].va);
		emit(0);
		emit(rp3d_cmds[i].dwords);
	}
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	event(EV_CACHE_FLUSH_TS, true);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	uint64_t t0 = mach_absolute_time();
	ok = submit("3D replay", 500);
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	sweet_dcache_flush((vm_offset_t)cpu, span);
	LOG("  submit %s in %llu us; RBBM_STATUS %08x CP_HW_FAULT %08x RBBM_INT_0_STATUS %08x", ok ? "done" : "STUCK",
	    ns / 1000, grd(REG_RBBM_STATUS), grd(REG_CP_HW_FAULT), grd(REG_RBBM_INT_0_STATUS));
	smmu_fault("after the 3D replay");
	LOG("  render target: (0,0) %08x (128,128) %08x (191,64) %08x (255,255) %08x", rt[0], rt[128 * RP_W + 128],
	    rt[64 * RP_W + 191], rt[RP_W * RP_W - 1]);
	uint32_t good_rt = rp_check(rt, &bx, &by, &bv, &bw);
	LOG("  render target: %u of %u pixels right", good_rt, RP_W * RP_W);
	if (bx != 0xFFFFFFFF) {
		LOG("  first wrong: (%u,%u) = %08x, wanted %08x", bx, by, bv, bw);
	}
	uint32_t good_ds = rp_check(ds, &bx, &by, &bv, &bw);
	LOG("  blit copy:     %u of %u pixels right", good_ds, RP_W * RP_W);
	if (bx != 0xFFFFFFFF) {
		LOG("  first wrong: (%u,%u) = %08x, wanted %08x", bx, by, bv, bw);
	}

	/* on the panel, centred, for ten seconds (SpringBoard's next frame covers it) */
	PE_Video video;
	PE_current_console(&video);
	if (ok && video.v_baseAddr != 0 && video.v_width >= RP_W && video.v_height >= RP_W) {
		uint64_t at = video.v_baseAddr + (uint64_t)((video.v_height - RP_W) / 2) * video.v_rowBytes +
		    ((video.v_width - RP_W) / 2) * 4u;
		for (int i = 0; i < 20; i++) {
			if (!blit_copy(RP_RT, RP_PITCH, at, (uint32_t)video.v_rowBytes, RP_W, RP_W)) {
				break;
			}
			IOSleep(500);
		}
	}
	for (uint32_t off = 0; off < span; off += 4096) {
		unmap_page_4k(rp3d_VA_START + off);     /* no GPU address may outlive its page */
	}
	(void)tlb_flush();
	m->release();
	return ok && good_rt == RP_W * RP_W && good_ds == RP_W * RP_W;
}

/*
 * The GPU as a service (boot-arg gpu_service=1, needs gpu_mmu=1): after its tests the GPU is
 * not parked, and the rest of the kernel may hand it copies through sapphire_gpu_copy(). One
 * lock covers the ring, the page tables and the windows. If a copy ever fails or times out the
 * service turns itself off and every caller falls back to the CPU from then on.
 *
 * Sources are IOMemoryDescriptors (iOS surfaces: scattered pages), mapped page by page into
 * one of four 16 MB windows of GPU addresses from 0x20000000, kept while the same memory comes
 * back (same descriptor, same length, same first and last physical page) and replaced round
 * robin otherwise. Destinations are physically contiguous below 4 GB (the panel's flip
 * buffers) and are mapped at their own physical address, once.
 */
#define WIN_BASE            0x20000000ull
#define MSM_VA_START        0x60000000ull       /* the msm client's buffers (below): 2 GB, nothing else there */
#define MSM_VA_END          0xE0000000ull
#define WIN_SIZE            0x01000000ull       /* 16 MB: a 1125 x 2436 surface is 10.9 MB */
#define WIN_COUNT           4u

static uint32_t service;
/* service_ready: defined at the top (submit() reads it) */
static IOLock *service_lock;
static struct {
	IOMemoryDescriptor *md;
	uint64_t len, first, last;
	uint32_t fence;                         /* 0153: the shared seqno of the last copy that read it */
} win[WIN_COUNT];

/* the shared slot's seqno has reached f (the copies' own events) */
static bool
fence_done(uint32_t f)
{
	return (int32_t)(*(volatile uint32_t *)(pool + OFF_FENCE) - f) >= 0;
}
static uint32_t win_next;
static struct {
	uint64_t base, size;
} ident[4];
static uint32_t ident_used;

static bool
map_identity(uint64_t base, uint64_t size)
{
	for (uint32_t i = 0; i < ident_used; i++) {
		if (base >= ident[i].base && base + size <= ident[i].base + ident[i].size) {
			return true;
		}
	}
	if (ident_used == 4 || base + size > (1ull << 32) || (base < MSM_VA_END && base + size > MSM_VA_START)) {
		return false;                           /* (the msm client's addresses are never identity) */
	}
	for (uint64_t a = base & ~0xFFFull; a < base + size; a += 4096) {
		uint64_t *l2 = pt_next(&pt_l1[(a >> 30) & 3]);
		if (l2 == NULL) {
			return false;
		}
		uint64_t e = l2[(a >> 21) & 511];
		if ((e & 3) == PTE_TYPE_BLOCK && (e & PTE_ADDR_MASK & ~0x1FFFFFull) == (a & ~0x1FFFFFull)) {
			continue;                       /* already inside an identity block */
		}
		if (!map_page_4k(a, a)) {
			return false;
		}
	}
	ident[ident_used].base = base;
	ident[ident_used].size = size;
	ident_used++;
	return tlb_flush();
}

static uint64_t
map_window(IOMemoryDescriptor *md)
{
	uint64_t len = md->getLength(), first, last, seg;

	if (len == 0 || len > WIN_SIZE) {
		return 0;
	}
	/*
	 * 0164: a window holds its memory: retained and prepared (surfaces are pageable now, and the
	 * VM must not move or take the pages the GPU reads) until another surface takes the slot.
	 */
	for (uint32_t i = 0; i < WIN_COUNT; i++) {
		if (win[i].md == md && win[i].len == len) {
			return WIN_BASE + i * WIN_SIZE;
		}
	}
	uint32_t i = win_next++ % WIN_COUNT;
	uint64_t va = WIN_BASE + i * WIN_SIZE;
	/* 0153: copies are not waited for (0152): a window still being read is not remapped under it */
	for (uint32_t t = 0; win[i].fence != 0 && !fence_done(win[i].fence) && t < 20000; t++) {
		if (grd(REG_CP_RB_RPTR) == ring_wptr % RING_DWORDS) {
			break;                          /* (the CP is past it: the slot was overwritten, as above) */
		}
		IODelay(10);
	}
	if (win[i].md != NULL) {
		win[i].md->complete();
		win[i].md->release();
		win[i].md = NULL;
	}
	if (md->prepare() != kIOReturnSuccess) {
		return 0;
	}
	md->retain();
	win[i].md = md;
	first = md->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
	last = md->getPhysicalSegment((len - 1) & ~0xFFFull, &seg, kIOMemoryMapperNone);
	for (uint64_t off = 0; off < WIN_SIZE; off += 4096) {
		uint64_t pa = off < len ? md->getPhysicalSegment(off, &seg, kIOMemoryMapperNone) : 0;
		if ((off < len && pa == 0) || (pa != 0 && !map_page_4k(va + off, pa))) {
			md->complete();
			md->release();
			win[i].md = NULL;
			return 0;
		}
		if (pa == 0) {
			/* past the end: no stale page from a previous surface stays reachable */
			uint64_t *l2 = pt_next(&pt_l1[((va + off) >> 30) & 3]);
			uint64_t *l3 = l2 != NULL ? pt_next(&l2[((va + off) >> 21) & 511]) : NULL;
			if (l3 != NULL) {
				l3[((va + off) >> 12) & 511] = 0;
			}
		}
	}
	win[i].md = md;
	win[i].len = len;
	win[i].first = first;
	win[i].last = last;
	return tlb_flush() ? va : 0;
}

/*
 * Copy w x h RGBA8 pixels from src (offset src_off, src_pitch bytes a row) to dst_pa (dst_pitch a
 * row), inside the contiguous destination [dst_base, dst_base + dst_size). The caller has cleaned
 * the source rows from the CPU caches and invalidates the destination rows after. Returns false
 * to have the caller copy with the CPU instead.
 */
bool sapphire_gpu_copy(IOMemoryDescriptor *src, uint64_t src_off, uint32_t src_pitch, uint64_t dst_pa,
    uint32_t dst_pitch, uint32_t w, uint32_t h, uint64_t dst_base, uint64_t dst_size);
bool sapphire_gpu_ready(void);
bool sapphire_gpu_copy_pending(void);

/*
 * 0212: whether the last copy into the panel's memory is still in the GPU's queue (SweetFramebuffer:
 * the screen is made black at display off only once it is done, or the copy lands over the black).
 */
static uint32_t last_copy_fence;

bool
sapphire_gpu_copy_pending(void)
{
	return service_ready && last_copy_fence != 0 && pool != NULL && !fence_done(last_copy_fence);
}

/* So a caller does not pay for cache maintenance before a copy the GPU would refuse. */
bool
sapphire_gpu_ready(void)
{
	return service_ready;
}

bool
sapphire_gpu_copy(IOMemoryDescriptor *src, uint64_t src_off, uint32_t src_pitch, uint64_t dst_pa,
    uint32_t dst_pitch, uint32_t w, uint32_t h, uint64_t dst_base, uint64_t dst_size)
{
	static uint32_t told;
	bool ok = false;

	if (!service_ready || src == NULL || w == 0 || h == 0) {
		return false;
	}
	IOLockLock(service_lock);
	if (service_ready) {
		uint64_t va = map_window(src);
		bool dst_ok = map_identity(dst_base, dst_size);
		if (!ring_room(1024, 200)) {
			LOG("service: the ring stays full (the GPU stopped?): copy refused");
			IOLockUnlock(service_lock);
			return false;
		}
		if (va != 0 && dst_ok && src_off + (uint64_t)(h - 1) * src_pitch + w * 4u <= src->getLength()) {
			ok = blit_copy(va + src_off, src_pitch, dst_pa, dst_pitch, w, h);
			uint32_t wi = (uint32_t)((va - WIN_BASE) / WIN_SIZE);
			if (wi < WIN_COUNT) {
				win[wi].fence = fence_seq;      /* blit_end's last event */
			}
			last_copy_fence = fence_seq;
			if (!ok) {
				service_ready = false;
				LOG("service: a copy failed, the GPU path is off; RBBM_STATUS %08x", grd(REG_RBBM_STATUS));
				smmu_fault("service");
			}
		} else if (told++ < 4) {
			LOG("service: cannot map this copy (window %llx, destination %s, %ux%u at +%llx of %llu bytes)", va,
			    dst_ok ? "ok" : "NOT mapped", w, h, src_off, src->getLength());
		}
	}
	IOLockUnlock(service_lock);
	return ok;
}

/* 0239: sapphire_gpu_copy() scaled: blit_scale() of the source at src_off (coordinates << 8) */
bool sapphire_gpu_copy_scaled(IOMemoryDescriptor *src, uint64_t src_off, uint32_t src_pitch, uint32_t sx, uint32_t sy,
    uint32_t sw, uint32_t sh, uint64_t dst_pa, uint32_t dst_pitch, uint32_t dw, uint32_t dh, uint64_t dst_base,
    uint64_t dst_size);

bool
sapphire_gpu_copy_scaled(IOMemoryDescriptor *src, uint64_t src_off, uint32_t src_pitch, uint32_t sx, uint32_t sy,
    uint32_t sw, uint32_t sh, uint64_t dst_pa, uint32_t dst_pitch, uint32_t dw, uint32_t dh, uint64_t dst_base,
    uint64_t dst_size)
{
	static uint32_t told;
	bool ok = false;
	uint64_t last = src_off + (uint64_t)(((sy + sh) >> 8) + 1) * src_pitch;

	if (!service_ready || src == NULL || dw == 0 || dh == 0) {
		return false;
	}
	IOLockLock(service_lock);
	if (service_ready) {
		uint64_t va = map_window(src);
		bool dst_ok = map_identity(dst_base, dst_size);
		if (!ring_room(1024, 200)) {
			LOG("service: the ring stays full (the GPU stopped?): copy refused");
			IOLockUnlock(service_lock);
			return false;
		}
		if (va != 0 && dst_ok && last <= src->getLength()) {
			ok = blit_scale(va + src_off, src_pitch, sx, sy, sw, sh, dst_pa, dst_pitch, dw, dh);
			uint32_t wi = (uint32_t)((va - WIN_BASE) / WIN_SIZE);
			if (wi < WIN_COUNT) {
				win[wi].fence = fence_seq;
			}
			last_copy_fence = fence_seq;
			if (!ok) {
				service_ready = false;
				LOG("service: a scaled copy failed, the GPU path is off; RBBM_STATUS %08x", grd(REG_RBBM_STATUS));
				smmu_fault("service");
			}
		} else if (told++ < 4) {
			LOG("service: cannot map this scaled copy (window %llx, destination %s, to %ux%u at +%llx of %llu bytes)", va,
			    dst_ok ? "ok" : "NOT mapped", dw, dh, src_off, src->getLength());
		}
	}
	IOLockUnlock(service_lock);
	return ok;
}

/*
 * boot-arg gpu_cs=1: the first shader on the Adreno — a compute kernel of 64 invocations, each
 * storing 0xc0de0000 + its local invocation id at buf[id], the buffer's GPU address in c0.xy.
 * Source metal/ir3/shaders/first.asm, assembled for the a610 by metal/ir3/sapphire-ir3as (Mesa's
 * own ir3 assembler and encoder): 128 bytes, max_reg 2 (3 full registers), no half registers,
 * constlen 4 vec4, mergedregs, double_threadsize (THREAD128), branchstack 0, local id in r0.x.
 * The launch is Mesa's computerator a6xx.cc (cs_restore_emit, cs_program_emit_regs,
 * cs_program_emit, emit_const, a6xx_emit_grid) for a GPU without LPAC; offsets and fields from
 * a6xx.xml (metal/tools/a6xx-reg.py).
 */
static const uint32_t first_cs[32] = {
	0x00000000, 0x202cc004, 0x00000001, 0x202cc005, 0xc0de0000, 0x204cc008, 0x00000000, 0x00000500,
	0x00000008, 0x42100008, 0x00000000, 0x00000500, 0x01800010, 0xc0d60900, 0x00000000, 0x03000000,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static uint32_t cs_test;

#define REG_SP_PERFCTR_SHADER_MASK  0xae0f
#define REG_SP_NC_MODE_CNTL_2       0xae04
#define REG_SP_MODE_CNTL            0xab00
#define REG_SP_UPDATE_CNTL          0xbb08
#define REG_SP_CS_CONST_CONFIG      0xb987
#define REG_SP_CS_CONFIG            0xa9bb
#define REG_SP_CS_INSTR_SIZE        0xa9bc
#define REG_SP_CS_CNTL_0            0xa9b0
#define REG_SP_CS_CNTL_1            0xa9b1
#define REG_SP_CS_CONST_CONFIG_0    0xb997
#define REG_SP_CS_WGE_CNTL          0xb998
#define REG_SP_CS_BASE              0xa9b4
#define REG_SP_CS_NDRANGE_0         0xb990      /* NDRANGE_0..6 consecutive */
#define REG_SP_CS_KERNEL_GROUP_X    0xb999      /* X, Y, Z consecutive */
#define CP_LOAD_STATE6_FRAG         0x34
#define CP_EXEC_CS                  0x33
#define RM6_COMPUTE                 8
#define INVALID_REG                 0xfcu

static bool
cs_first(void)
{
	uint64_t code = POOL_PA + OFF_CS, out = POOL_PA + OFF_CSOUT;
	volatile uint32_t *res = (volatile uint32_t *)(pool + OFF_CSOUT);
	uint32_t good = 0, bad_at = 0xFFFFFFFF, bad_val = 0;

	LOG("--- gpu_cs: the first shader, 64 invocations writing to %llx", out);
	memcpy(pool + OFF_CS, first_cs, sizeof(first_cs));
	for (int i = 0; i < 64; i++) {
		res[i] = 0xDEADBEEF;
	}
	gmb();

	/* cs_restore_emit: shader mask, NC mode, the a610's magic registers */
	reg1(REG_SP_PERFCTR_SHADER_MASK, 1u << 5);
	reg1(REG_SP_NC_MODE_CNTL_2, 0);
	reg1(0x9804, 0xf);                          /* PC_MODE_CNTL */
	reg1(0x9805, 0);                            /* PC_POWER_CNTL */
	reg1(0xa0f8, 0);                            /* VFD_POWER_CNTL */
	reg1(0xb600, 0);                            /* TPL1_DBG_ECO_CNTL */
	reg1(0x8600, 0);                            /* GRAS_DBG_ECO_CNTL */
	reg1(0xae03, 0);                            /* SP_CHICKEN_BITS */
	reg1(0xae00, 0);                            /* SP_DBG_ECO_CNTL */
	reg1(0xbe04, 0);                            /* HLSQ_DBG_ECO_CNTL */
	reg1(0x9600, 0);                            /* VPC_DBG_ECO_CNTL */
	reg1(0x0e12, 0x10000000);                   /* UCHE_UNKNOWN_0E12 */

	/* cs_program_emit_regs */
	reg1(REG_SP_MODE_CNTL, 1u | (2u << 1));     /* constant demotion, ISAMMODE_GL */
	reg1(REG_SP_UPDATE_CNTL, 0x3Fu | (1u << 7)); /* every stage's state, GFX_UAV */
	reg1(REG_SP_CS_CONST_CONFIG, (4u >> 2) | (1u << 8));  /* constlen 4 vec4 (field shr 2), enabled */
	reg1(REG_SP_CS_CONFIG, 1u << 8);            /* enabled, no textures, samplers or UAVs */
	reg1(REG_SP_CS_INSTR_SIZE, 1);              /* instrlen */
	reg1(REG_SP_CS_CNTL_0, (0u << 1) | (3u << 7) | (0u << 13) | (1u << 20) | (1u << 31));
	                                            /* half 0, full 3, branchstack 0, THREAD128, mergedregs */
	reg1(REG_SP_CS_CNTL_1, 1u);                 /* shared_size 1, CONSTLEN_128 */
	reg1(REG_SP_CS_CONST_CONFIG_0, INVALID_REG | (INVALID_REG << 8) | (INVALID_REG << 16) | (0u << 24));
	                                            /* no wg id / size / offset consts, local id in r0.x */
	reg1(REG_SP_CS_WGE_CNTL, INVALID_REG | (1u << 9));  /* no linear local id, THREAD128 */
	emit(pkt4(REG_SP_CS_BASE, 2));
	emit((uint32_t)code);
	emit((uint32_t)(code >> 32));
	reg1(REG_SP_CS_INSTR_SIZE, 1);

	/* cs_program_emit: preload the code (instrlen 1, within the 64-entry cache) */
	emit(pkt7(CP_LOAD_STATE6_FRAG, 3));
	emit(0u | (0u << 14) | (2u << 16) | (0xDu << 18) | (1u << 22));  /* ST6_SHADER, SS6_INDIRECT, SB6_CS_SHADER, 1 unit */
	emit((uint32_t)code);
	emit((uint32_t)(code >> 32));

	/* emit_const: c0 = the buffer's address */
	emit(pkt7(CP_LOAD_STATE6_FRAG, 3 + 4));
	emit(0u | (1u << 14) | (0u << 16) | (0xDu << 18) | (1u << 22));  /* ST6_CONSTANTS, SS6_DIRECT, SB6_CS_SHADER, 1 vec4 */
	emit(0);
	emit(0);
	emit((uint32_t)out);
	emit((uint32_t)(out >> 32));
	emit(0);
	emit(0);

	/* a6xx_emit_grid: one group of 64 */
	emit(pkt7(CP_SET_MARKER, 1));
	emit(RM6_COMPUTE);
	emit(pkt4(REG_SP_CS_NDRANGE_0, 7));
	emit(1u | (63u << 2) | (0u << 12) | (0u << 22));  /* kerneldim 1, local size 64 x 1 x 1 */
	emit(64);                                   /* global size x */
	emit(0);
	emit(1);                                    /* global size y */
	emit(0);
	emit(1);                                    /* global size z */
	emit(0);
	emit(pkt4(REG_SP_CS_KERNEL_GROUP_X, 3));
	emit(1);
	emit(1);
	emit(1);
	emit(pkt7(CP_EXEC_CS, 4));
	emit(0);
	emit(1);
	emit(1);
	emit(1);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	event(EV_CACHE_FLUSH_TS, true);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	if (!submit("compute", 200)) {
		return false;
	}
	IODelay(100);
	gmb();
	for (uint32_t i = 0; i < 64; i++) {
		if (res[i] == 0xC0DE0000u + i) {
			good++;
		} else if (bad_at == 0xFFFFFFFF) {
			bad_at = i;
			bad_val = res[i];
		}
	}
	LOG("  results: [0] %08x [1] %08x [63] %08x; %u of 64 right", res[0], res[1], res[63], good);
	if (bad_at != 0xFFFFFFFF) {
		LOG("  first wrong: [%u] = %08x", bad_at, bad_val);
	}
	LOG("  RBBM_STATUS %08x CP_HW_FAULT %08x RBBM_INT_0_STATUS %08x", grd(REG_RBBM_STATUS), grd(REG_CP_HW_FAULT),
	    grd(REG_RBBM_INT_0_STATUS));
	return good == 64;
}

/*
 * boot-arg gpu_demo=1: the GPU paints three bars (red, green, blue) straight onto the boot
 * framebuffer the panel scans out, over the Apple logo, where they stay until SpringBoard's first
 * frame replaces them. The framebuffer is 1080 x 2400 BGRA with 4320 bytes a row, which the
 * blitter cannot take as a pitch (it counts in 64-byte units and 4320 is 67.5 of them), but two
 * rows are 8640 = 135 x 64: so the even rows are one surface with an 8640-byte pitch, and the odd
 * rows another one starting 4320 bytes later, whose base is 32 bytes off alignment, so it is
 * rounded down and x shifted right by 8 pixels, the trick fd6_clear_buffer uses for unaligned
 * buffers. Two blits per bar.
 */
static uint32_t demo;

static bool
blit_bar(uint64_t fb, uint32_t stride, uint32_t w, uint32_t y0, uint32_t h, uint32_t r, uint32_t g, uint32_t b)
{
	/* the framebuffer is B,G,R,A in memory: channel order of the blit format is R,G,B,A */
	uint32_t rgba = (b << 24) | (g << 16) | (r << 8) | 0xFF;
	uint64_t even = fb + (uint64_t)y0 * stride;
	uint64_t odd = even + stride;
	uint32_t dx = (uint32_t)(odd & 63) / 4;

	blit_begin(rgba);
	blit_rect(even, 2 * stride, 0, 0, w - 1, h / 2 - 1);
	blit_rect(odd & ~63ull, 2 * stride, dx, 0, dx + w - 1, h / 2 - 1);
	return blit_end("bar");
}

static void
demo_bars(void)
{
	PE_Video video;

	PE_current_console(&video);
	uint64_t fb = video.v_baseAddr;
	uint32_t w = (uint32_t)video.v_width, h = (uint32_t)video.v_height, stride = (uint32_t)video.v_rowBytes;

	LOG("--- gpu_demo: bars onto the framebuffer at %llx, %ux%u, %u bytes a row", fb, w, h, stride);
	if (fb == 0 || (fb & 63) != 0 || ((2 * stride) & 63) != 0 || (stride & 3) != 0 || w > 0x3ff0 || h < 1400) {
		LOG("  this framebuffer does not fit the two-surface trick; no demo");
		return;
	}
	uint32_t bar = 200, top = h / 2 - 3 * bar / 2;
	top &= ~1u;                                  /* even first row */
	bool ok = blit_bar(fb, stride, w, top, bar, 0xFF, 0x20, 0x20) &&
	    blit_bar(fb, stride, w, top + bar, bar, 0x20, 0xE0, 0x20) &&
	    blit_bar(fb, stride, w, top + 2 * bar, bar, 0x20, 0x40, 0xFF);
	LOG("  %s: rows %u-%u red, green, blue", ok ? "drawn" : "FAILED", top, top + 3 * bar - 1);

	/* read a pixel of every bar on an even and an odd row back through the CPU */
	IOMemoryMap *m = map_phys(fb, (uint64_t)stride * h, true);
	if (m == NULL) {
		return;
	}
	const uint8_t *v = (const uint8_t *)m->getVirtualAddress();
	for (uint32_t i = 0; i < 3; i++) {
		uint32_t ye = top + i * bar + 10, yo = ye + 1;
		LOG("  bar %u: pixel (%u,%u) %08x, (%u,%u) %08x, (0,%u) %08x, (%u,%u) %08x", i, w / 2, ye,
		    *(const volatile uint32_t *)(v + (uint64_t)ye * stride + (w / 2) * 4), w / 2, yo,
		    *(const volatile uint32_t *)(v + (uint64_t)yo * stride + (w / 2) * 4), yo,
		    *(const volatile uint32_t *)(v + (uint64_t)yo * stride), w - 1, yo,
		    *(const volatile uint32_t *)(v + (uint64_t)yo * stride + (w - 1) * 4));
	}
	m->release();
}

/* ---------------------------------------------------------------- bring-up thread */

static void
gpu_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint32_t stage = 0, delay_s = 0;

	(void)PE_parse_boot_argn("gpu", &stage, sizeof(stage));
	/*
	 * gpu_delay=N waits N seconds before touching anything. The bring-up runs from
	 * AppleCLCD::start, long before USB networking carries the console, so its log would
	 * only be readable from recovery; with a delay it lands in the live log instead.
	 */
	(void)PE_parse_boot_argn("gpu_slow", &slow_ms, sizeof(slow_ms));
	/* 0152: gpu_div=1 is GPLL0 undivided, 600 MHz -- the rail is the bootloader's, so try it, do not assume it */
	(void)PE_parse_boot_argn("gpu_mhz", &core_mhz, sizeof(core_mhz));
	if (PE_parse_boot_argn("gpu_div", &core_div, sizeof(core_div)) && (core_div < 1 || core_div > 16)) {
		core_div = 2;
	}
	(void)PE_parse_boot_argn("gpu_copy_async", &copy_async, sizeof(copy_async));
	(void)PE_parse_boot_argn("gpu_smmu_early", &smmu_early, sizeof(smmu_early));
	(void)PE_parse_boot_argn("gpu_irq", &gpu_irq, sizeof(gpu_irq));
	(void)PE_parse_boot_argn("gpu_stop", &stop_at, sizeof(stop_at));
	(void)PE_parse_boot_argn("gpu_keep", &keep_running, sizeof(keep_running));
	(void)PE_parse_boot_argn("gpu_draw", &draw_first, sizeof(draw_first));
	(void)PE_parse_boot_argn("gpu_demo", &demo, sizeof(demo));
	(void)PE_parse_boot_argn("gpu_bench", &bench, sizeof(bench));
	(void)PE_parse_boot_argn("gpu_cs", &cs_test, sizeof(cs_test));
	(void)PE_parse_boot_argn("gpu_3d", &draw3d, sizeof(draw3d));
	(void)PE_parse_boot_argn("gpu_mmu", &mmu, sizeof(mmu));
	(void)PE_parse_boot_argn("gpu_service", &service, sizeof(service));
	service_lock = IOLockAlloc();
	/*
	 * ios-sapphire started this from AppleCLCD::start, with IOKit up. Here it is started from hakios_adreno_init(), earlier:
	 * wait for the platform expert's first matching round (3 s) unless gpu_delay says otherwise.
	 */
	if (!PE_parse_boot_argn("gpu_delay", &delay_s, sizeof(delay_s))) {
		delay_s = 3;
	}
	if (delay_s != 0) {
		LOG("starting in %u s", delay_s);
		IOSleep(delay_s * 1000);
	}
	LOG("stage %u (Adreno 610 v2, regs %llx, GPU CC %llx), core GPLL0/%u = %u MHz, %u ms after each step",
	    stage, GPU_PA, GPUCC_PA, core_div, 600 / core_div, slow_ms);

	gcc_map = map_phys(GCC_PA, GCC_SIZE, false);
	gpucc_map = map_phys(GPUCC_PA, GPUCC_SIZE, false);
	if (gcc_map == NULL || gpucc_map == NULL) {
		LOG("the clock controllers could not be mapped");
		thread_terminate(current_thread());
		return;
	}
	gcc = (volatile uint8_t *)gcc_map->getVirtualAddress();
	gpucc = (volatile uint8_t *)gpucc_map->getVirtualAddress();
	gicd_map = map_phys(GICD_PA, GICD_SIZE, false);
	if (gicd_map != NULL) {
		gicd = (volatile uint8_t *)gicd_map->getVirtualAddress();
	}
	gic_report("as the bootloader left it");
	dump_clocks("as the bootloader left it");
	if (stage < 2) {
		thread_terminate(current_thread());
		return;
	}

	if (gpu_irq == 0) {
		gic_mask_gpu();
		step_done();
	}
	/* mapped before the power-up only so gpu_smmu_early has it; the registers need the vote */
	smmu_map = map_phys(SMMU_PA, SMMU_SIZE, false);
	if (smmu_map != NULL) {
		smmu = (volatile uint8_t *)smmu_map->getVirtualAddress();
	}
	if (!power_up()) {
		LOG("power-up failed, the GPU stays off");
		power_down();
		thread_terminate(current_thread());
		return;
	}
	dump_clocks("powered");
	gpu_map = map_phys(GPU_PA, GPU_SIZE, false);
	if (gpu_map == NULL) {
		LOG("the GPU registers could not be mapped");
		power_down();
		thread_terminate(current_thread());
		return;
	}
	gpu = (volatile uint8_t *)gpu_map->getVirtualAddress();
	LOG("GPU registers mapped");
	step_done();
	first_reads();
	if (stage < 3) {
		LOG("stage 2: the GPU is powered and answering; stopping here");
		thread_terminate(current_thread());
		return;
	}

	if (fw_count() == 0 && !have_adt_firmware()) {
		LOG("no firmware, in the device tree or in a table: stopping after the power-up");
		thread_terminate(current_thread());
		return;
	}
	/* the pool must not be the kernel's: the ADT's RAM has to end below it */
	{
		DTEntry chosen_dt;
		void const *b = NULL, *z = NULL;
		unsigned int bl = 0, zl = 0;
		uint64_t dbase = 0, dsize = 0;

		if (SecureDTLookupEntry(NULL, "/chosen", &chosen_dt) == kSuccess &&
		    SecureDTGetProperty(chosen_dt, "dram-base", &b, &bl) == kSuccess && bl == 8 &&
		    SecureDTGetProperty(chosen_dt, "dram-size", &z, &zl) == kSuccess && zl == 8) {
			memcpy(&dbase, b, 8);
			memcpy(&dsize, z, 8);
		}
		if (dsize == 0 || dbase + dsize > POOL_PA) {
			LOG("the ADT gives the kernel RAM up to %llx; the GPU pool at %llx would be written under it "
			    "(rebuild sapphire.adt with dram-size 0x4ce00000): not starting", dbase + dsize, (uint64_t)POOL_PA);
			power_down();
			thread_terminate(current_thread());
			return;
		}
	}
	pool_map = map_phys(POOL_PA, POOL_SIZE, true);
	if (pool_map == NULL || smmu_map == NULL) {
		LOG("the buffer pool or the SMMU could not be mapped");
		power_down();
		thread_terminate(current_thread());
		return;
	}
	pool = (uint8_t *)pool_map->getVirtualAddress();
	ring = (uint32_t *)(pool + OFF_RING);
	test_buf = (uint32_t *)(pool + OFF_TEST);
	bzero(pool, OFF_META + 0x4000);

	/* a partition carries the firmware: sweet_ufs answers EAGAIN until the root device is up */
	for (int i = 0; fw_count() != 0 && i < 120; i++) {
		uint8_t probe[16];
		int err = sweet_ufs_read_part(gpu_fw[0].part, gpu_fw[0].off, probe, sizeof(probe));
		if (err != EAGAIN) {
			if (err != 0) {
				LOG("partition %s: %d", gpu_fw[0].part, err);
				thread_terminate(current_thread());
				return;
			}
			break;
		}
		IOSleep(500);
	}

	if (stage == 3 && !zap_load()) {
		LOG("the zap shader did not load; gpu=4 tries it without one");
		thread_terminate(current_thread());
		return;
	}
	if (stage == 3 && stop_here(1, "the zap shader")) {
		;
	} else if (cp_start(stage == 3)) {
		LOG("*** the GPU executes commands ***");
		if (draw_first && blit_fill(0xFF8000FF)) {
			LOG("*** the GPU drew: every pixel of the 2D fill is right ***");
			service_ok = true;
			if (demo) {
				demo_bars();
			}
			if (bench) {
				bench_copy();
			}
			if (cs_test && cs_first()) {
				LOG("*** the first shader ran on the Adreno: 64 of 64 invocations wrote their value ***");
			} else if (cs_test) {
				LOG("the compute shader did not come out right");
			}
			if (mmu && mmu_test()) {
				LOG("*** the GPU reached scattered memory through its own page tables ***");
			} else if (mmu) {
				LOG("the GPU MMU test did not come out right");
				service_ok = false;
			}
			if (mmu && draw3d && service_ok && replay_3d()) {
				LOG("*** the first 3D draw on the Adreno: clear, textured quad and blit, every pixel right ***");
			} else if (mmu && draw3d) {
				LOG("the 3D replay did not come out right");
			}
		} else if (draw_first) {
			LOG("the 2D fill did not come out right");
		}
	} else if (!stopped_early) {
		LOG("the command processor did not come up");
	}
	if (service && mmu && service_lock != NULL && service_ok) {
		service_ready = true;
		LOG("*** gpu_service: the GPU stays up and takes copies (%u page tables in use) ***", pt_used);
		msm_publish();
	} else if (keep_running || service) {
		LOG("%s: the GPU stays up%s", service ? "gpu_service" : "gpu_keep=1",
		    service ? " but takes no work (needs gpu_mmu=1 and every test passed)" : "");
	} else {
		LOG("parking the GPU: RBBM_STATUS %08x", grd(REG_RBBM_STATUS));
		bus_drain();
		power_down();
	}
	thread_terminate(current_thread());
}

/* ---------------------------------------------------------------- user client */

/*
 * The GPU for user space: what a Metal driver needs from the kernel. IOAcceleratorES (in
 * SweetFramebuffer.cpp) hands IOServiceOpen of type 'SGPU' to sapphire_gpu_new_client(); any
 * other type is refused as before, so Metal's own probes see nothing new. Works only once
 * gpu_service is up (gpu=3 gpu_mmu=1 gpu_service=1 and every test passed), and shares the
 * service lock with the frame copies: one ring, one set of page tables.
 *
 *   sel 0  info            out: gpu id, chip id, user VA base, user VA end, page tables free
 *   sel 1  buffer create   in: size, fixed GPU address (0 = any)    out: handle, GPU address
 *   sel 2  release         in: handle
 *   sel 3  surface import  in: IOSurface id                  out: handle, GPU address, size
 *   sel 4  sync            in: handle   (clean + invalidate the CPU caches over it)
 *   sel 5  submit          in: GPU address, dwords, timeout ms   out: fence seqno, us taken
 *
 * Buffers are wired kernel memory, zeroed and cleaned; map one into the task with
 * IOConnectMapMemory64(conn, handle, ..., kIOMapAnywhere), cacheable, as the IOSurface buffers
 * are: never write-combined, because changing the cache mode of kernel pages for a user
 * mapping panics this kernel (phystokv_range: illegal PA, first try of this interface).
 * Every submit cleans and invalidates all of the client's buffers before it starts and after
 * it ends, so CPU writes reach the GPU and GPU writes reach the CPU. A surface is the task's
 * own IOSurface (by id): its pixels get a GPU address and sel 4 cleans what the CPU wrote, or
 * drops what it holds before reading what the GPU wrote. The GPU sees its buffers through 4 KB pages from
 * 0x01000000 to 0x10000000, below RAM: nothing there aliases the identity blocks.
 *
 * A submit runs one indirect buffer, which must lie inside a buffer of this client, then a
 * wait for idle and a timestamped cache flush, and waits for it (no queue yet). If the GPU
 * does not finish in time, the service stops: every later call and frame copy is refused.
 *
 * This is a development interface: the command processor runs what it is given with the
 * whole GPU address space reachable, and no register protection is set up yet.
 */
/*
 * IOSurface import is not ported: ios-sapphire's SweetSurface is not in this tree (MiniIOSurface is). This empty shape keeps
 * the import paths below compiling; sweet_surface_copy() finds nothing, so they answer "no such surface" and never run.
 */
struct SweetSurface {
	IOMemoryDescriptor *memory;
	IOMemoryDescriptor *state;
	uint64_t            wrapDelta;
	bool                wrapped, gpuDrawn;
	uint64_t            offset, bytesPerRow, width, height, pixelFormat, planeCount;
	uint32_t            gpuSeed;
	void                release(void) {}
	void               *kernelAddress(void) { return NULL; }
};
static inline SweetSurface *sweet_surface_copy(uint32_t) { return NULL; }

extern "C" void proc_selfname(char *buf, int size);

#define UVA_BASE            0x01000000ull
#define UVA_END             0x10000000ull
#define UVA_PAGES           ((UVA_END - UVA_BASE) >> 12)
#define UC_BOS              256u

static uint8_t uva_bits[UVA_PAGES / 8];

static bool
uva_used(uint64_t p)
{
	return (uva_bits[p >> 3] >> (p & 7)) & 1;
}

static void
uva_mark(uint64_t p0, uint64_t n, bool used)
{
	for (uint64_t p = p0; p < p0 + n; p++) {
		if (used) {
			uva_bits[p >> 3] |= (uint8_t)(1u << (p & 7));
		} else {
			uva_bits[p >> 3] &= (uint8_t)~(1u << (p & 7));
		}
	}
}

/* n pages of GPU addresses, at fixed if non-zero; 0 if there is no room. */
static uint64_t
uva_alloc(uint64_t n, uint64_t fixed)
{
	if (n == 0 || n > UVA_PAGES) {
		return 0;
	}
	if (fixed != 0) {
		if ((fixed & 0xFFF) != 0 || fixed < UVA_BASE || fixed + (n << 12) > UVA_END) {
			return 0;
		}
		uint64_t p0 = (fixed - UVA_BASE) >> 12;
		for (uint64_t i = 0; i < n; i++) {
			if (uva_used(p0 + i)) {
				return 0;
			}
		}
		uva_mark(p0, n, true);
		return fixed;
	}
	for (uint64_t p = 0, run = 0; p < UVA_PAGES; p++) {
		run = uva_used(p) ? 0 : run + 1;
		if (run == n) {
			uva_mark(p + 1 - n, n, true);
			return UVA_BASE + ((p + 1 - n) << 12);
		}
	}
	return 0;
}

struct uc_bo {
	IOMemoryDescriptor *md;                 /* retained; NULL = free slot */
	SweetSurface       *surf;               /* retained when imported */
	uint64_t            va;                 /* GPU address of the first page */
	uint64_t            pages;
	uint64_t            len;                /* bytes from va + delta */
	uint32_t            delta;              /* where byte 0 sits in the first page */
	uintptr_t           kva;                /* kernel address of byte 0, for cache maintenance */
};

/*
 * Map len bytes of md from offset 0 at fresh GPU addresses (fixed if non-zero), page by page,
 * following the physical segments. Fills va, pages and delta. service_lock held.
 */
static bool
uc_map(struct uc_bo *b, uint64_t fixed)
{
	uint64_t seg = 0, pa0 = b->md->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);

	if (pa0 == 0) {
		return false;
	}
	b->delta = (uint32_t)(pa0 & 0xFFF);
	b->pages = (b->delta + b->len + 4095) >> 12;
	b->va = uva_alloc(b->pages, fixed);
	if (b->va == 0) {
		return false;
	}
	bool ok = true;
	for (uint64_t o = 0; ok && o < b->len;) {
		uint64_t pa = b->md->getPhysicalSegment(o, &seg, kIOMemoryMapperNone);
		uint64_t at = b->delta + o;             /* byte offset from the first page */
		ok = pa != 0 && map_page_4k(b->va + (at & ~0xFFFull), pa & ~0xFFFull);
		o += 4096 - (pa & 0xFFF);
	}
	if (!ok) {
		for (uint64_t p = 0; p < b->pages; p++) {
			unmap_page_4k(b->va + (p << 12));
		}
		uva_mark((b->va - UVA_BASE) >> 12, b->pages, false);
	}
	return tlb_flush() && ok;
}

static void
uc_unmap(struct uc_bo *b)
{
	for (uint64_t p = 0; p < b->pages; p++) {
		unmap_page_4k(b->va + (p << 12));
	}
	(void)tlb_flush();
	uva_mark((b->va - UVA_BASE) >> 12, b->pages, false);
}

class SapphireGpuClient : public IOUserClient {
	OSDeclareDefaultStructors(SapphireGpuClient);

public:
	task_t        fTask;
	uint32_t      fCalls;
	char          fName[32];
	struct uc_bo  fBo[UC_BOS];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;

	int  freeSlot(void);
	void flushAll(void);
	void drop(uint32_t h);
	IOReturn create(uint64_t size, uint64_t fixed, uint64_t *out);
	IOReturn import(uint32_t id, uint64_t *out);
	IOReturn run(uint64_t va, uint64_t dwords, uint64_t ms, uint64_t *out);
};

OSDefineMetaClassAndStructors(SapphireGpuClient, IOUserClient);

bool
SapphireGpuClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fCalls = 0;
	proc_selfname(fName, sizeof(fName));          /* IOServiceOpen runs in the caller */
	bzero(fBo, sizeof(fBo));
	return true;
}

int
SapphireGpuClient::freeSlot(void)
{
	for (uint32_t i = 1; i < UC_BOS; i++) {         /* handle 0 is never used */
		if (fBo[i].md == NULL) {
			return (int)i;
		}
	}
	return -1;
}

/* Clean and invalidate every buffer of this client (not surfaces: sel 4 does those). */
void
SapphireGpuClient::flushAll(void)
{
	for (uint32_t i = 1; i < UC_BOS; i++) {
		if (fBo[i].md != NULL && fBo[i].surf == NULL && fBo[i].kva != 0) {
			sweet_dcache_flush((vm_offset_t)fBo[i].kva, (unsigned)fBo[i].len);
		}
	}
}

/* service_lock held */
void
SapphireGpuClient::drop(uint32_t h)
{
	struct uc_bo *b = &fBo[h];

	if (h == 0 || h >= UC_BOS || b->md == NULL) {
		return;
	}
	uc_unmap(b);
	if (b->surf != NULL) {
		b->md->complete();
	}
	OSSafeReleaseNULL(b->md);
	OSSafeReleaseNULL(b->surf);
	bzero(b, sizeof(*b));
}

IOReturn
SapphireGpuClient::clientClose(void)
{
	if (service_lock != NULL) {
		IOLockLock(service_lock);
		for (uint32_t i = 1; i < UC_BOS; i++) {
			drop(i);
		}
		IOLockUnlock(service_lock);
	}
	LOG("client: %s closed after %u calls", fName, fCalls);
	terminate();
	return kIOReturnSuccess;
}

IOReturn
SapphireGpuClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	IOReturn r = kIOReturnBadArgument;

	IOLockLock(service_lock);
	if (type > 0 && type < UC_BOS && fBo[type].md != NULL && fBo[type].surf == NULL) {
		*memory = fBo[type].md;
		fBo[type].md->retain();
		*options = kIOMapDefaultCache;          /* see above: never write-combined */
		r = kIOReturnSuccess;
	}
	IOLockUnlock(service_lock);
	return r;
}

/* service_lock held */
IOReturn
SapphireGpuClient::create(uint64_t size, uint64_t fixed, uint64_t *out)
{
	int h = freeSlot();

	if (h < 0) {
		return kIOReturnNoResources;
	}
	if (size == 0 || size > (UVA_END - UVA_BASE)) {
		return kIOReturnBadArgument;
	}
	size = (size + 4095) & ~4095ull;
	IOBufferMemoryDescriptor *m = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, size, 4096);
	if (m == NULL) {
		return kIOReturnNoMemory;
	}
	bzero(m->getBytesNoCopy(), size);
	sweet_dcache_flush((vm_offset_t)m->getBytesNoCopy(), (unsigned)size);
	struct uc_bo *b = &fBo[h];
	b->md = m;
	b->len = size;
	b->kva = (uintptr_t)m->getBytesNoCopy();
	if (!uc_map(b, fixed)) {
		m->release();
		bzero(b, sizeof(*b));
		return fixed ? kIOReturnExclusiveAccess : kIOReturnNoSpace;
	}
	out[0] = (uint64_t)h;
	out[1] = b->va + b->delta;
	return kIOReturnSuccess;
}

/* service_lock held */
IOReturn
SapphireGpuClient::import(uint32_t id, uint64_t *out)
{
	int h = freeSlot();

	if (h < 0) {
		return kIOReturnNoResources;
	}
	SweetSurface *s = sweet_surface_copy(id);
	if (s == NULL || s->memory == NULL) {
		OSSafeReleaseNULL(s);
		return kIOReturnNotFound;
	}
	if (s->memory->prepare() != kIOReturnSuccess) {
		s->release();
		return kIOReturnNoMemory;
	}
	struct uc_bo *b = &fBo[h];
	b->md = s->memory;
	b->md->retain();
	b->surf = s;
	b->len = s->memory->getLength();
	b->kva = s->kernelAddress();
	if (!uc_map(b, 0)) {
		b->md->complete();
		OSSafeReleaseNULL(b->md);
		OSSafeReleaseNULL(b->surf);
		bzero(b, sizeof(*b));
		return kIOReturnNoSpace;
	}
	out[0] = (uint64_t)h;
	out[1] = b->va + b->delta + s->wrapDelta;     /* byte 0 of the surface, as IOSurfaceGetBaseAddress */
	out[2] = b->len - s->wrapDelta;
	return kIOReturnSuccess;
}

/* service_lock held */
IOReturn
SapphireGpuClient::run(uint64_t va, uint64_t dwords, uint64_t ms, uint64_t *out)
{
	bool inside = false;

	for (uint32_t i = 1; i < UC_BOS && !inside; i++) {
		struct uc_bo *b = &fBo[i];
		inside = b->md != NULL && va >= b->va + b->delta && va + dwords * 4 <= b->va + b->delta + b->len;
	}
	if (!inside || dwords == 0 || dwords > 0xFFFFF || (va & 3) != 0) {
		return kIOReturnBadArgument;
	}
	ms = ms == 0 ? 100 : (ms > 2000 ? 2000 : ms);
	if (!ring_room(64, 2000)) {
		return kIOReturnTimeout;
	}
	flushAll();                                 /* what the CPU wrote, out to RAM */
	emit(pkt7(CP_INDIRECT_BUFFER, 3));
	emit((uint32_t)va);
	emit((uint32_t)(va >> 32));
	emit((uint32_t)dwords);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	event(EV_CACHE_FLUSH_TS, true);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	uint64_t t0 = mach_absolute_time(), ns;
	bool ok = submit("user", (uint32_t)ms);
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	flushAll();                                 /* and no stale line over what the GPU wrote */
	out[0] = *(volatile uint32_t *)(pool + OFF_FENCE);
	out[1] = ns / 1000;
	if (!ok) {
		service_ready = false;
		LOG("client: %s's submit of %llu dwords at %llx did not finish in %llu ms: the GPU service stops; RBBM_STATUS %08x CP_HW_FAULT %08x",
		    fName, dwords, va, ms, grd(REG_RBBM_STATUS), grd(REG_CP_HW_FAULT));
		smmu_fault("user submit");
		return kIOReturnTimeout;
	}
	return kIOReturnSuccess;
}

IOReturn
SapphireGpuClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a, IOExternalMethodDispatch *,
    OSObject *, void *)
{
	static const uint8_t ins[] = { 0, 2, 1, 1, 1, 3 }, outs[] = { 5, 2, 0, 3, 0, 2 };
	uint64_t out[5] = { 0 };
	IOReturn r = kIOReturnSuccess;

	if (selector >= sizeof(ins) || a->scalarInputCount != ins[selector] || a->scalarOutputCount < outs[selector]) {
		return kIOReturnBadArgument;
	}
	IOLockLock(service_lock);
	if (!service_ready) {
		IOLockUnlock(service_lock);
		return kIOReturnNotReady;
	}
	const uint64_t *in = a->scalarInput;
	switch (selector) {
	case 0:
		out[0] = 610;
		out[1] = 0x06010001;
		out[2] = UVA_BASE;
		out[3] = UVA_END;
		out[4] = PT_PAGES - pt_used;
		break;
	case 1:
		r = create(in[0], in[1], out);
		break;
	case 2:
		if (in[0] == 0 || in[0] >= UC_BOS || fBo[in[0]].md == NULL) {
			r = kIOReturnBadArgument;
		} else {
			drop((uint32_t)in[0]);
		}
		break;
	case 3:
		r = import((uint32_t)in[0], out);
		break;
	case 4:
		if (in[0] == 0 || in[0] >= UC_BOS || fBo[in[0]].md == NULL || fBo[in[0]].kva == 0) {
			r = kIOReturnBadArgument;
		} else {
			sweet_dcache_flush((vm_offset_t)fBo[in[0]].kva, (unsigned)fBo[in[0]].len);
		}
		break;
	case 5:
		r = run(in[0], in[1], in[2], out);
		break;
	}
	IOLockUnlock(service_lock);
	if (fCalls++ < 32 || r != kIOReturnSuccess) {
		LOG("client: %s sel %u (%llx %llx) -> %x, out %llx %llx", fName, selector,
		    ins[selector] > 0 ? in[0] : 0, ins[selector] > 1 ? in[1] : 0, r, out[0], out[1]);
	}
	a->scalarOutputCount = outs[selector];
	for (uint32_t i = 0; i < outs[selector]; i++) {
		a->scalarOutput[i] = out[i];
	}
	return r;
}

IOReturn sapphire_gpu_new_client(IOService *provider, task_t task, void *security, UInt32 type, IOUserClient **handler);

IOReturn
sapphire_gpu_new_client(IOService *provider, task_t task, void *security, UInt32 type, IOUserClient **handler)
{
	char name[32];
	proc_selfname(name, sizeof(name));
	if (!service_ready || service_lock == NULL) {
		LOG("client: %s asked for the GPU, but the service is not up", name);
		return kIOReturnNotReady;
	}
	SapphireGpuClient *c = OSTypeAlloc(SapphireGpuClient);
	if (c == NULL || !c->initWithTask(task, security, type)) {
		OSSafeReleaseNULL(c);
		return kIOReturnNoMemory;
	}
	if (!c->attach(provider)) {
		c->release();
		return kIOReturnError;
	}
	if (!c->start(provider)) {
		c->detach(provider);
		c->release();
		return kIOReturnError;
	}
	LOG("client: %s opened the GPU", name);
	*handler = c;
	return kIOReturnSuccess;
}

/* ---------------------------------------------------------------- msm client (Mesa) */

/*
 * SapphireGPU: the Adreno for Mesa's freedreno, which speaks Linux's msm DRM interface. The
 * interface is HackiOS's (github.com/oosexgang, iokit/Kernel/RedwoodGPU.cpp, whose design
 * this follows; their user-space libdrm stand-in, tools/mesa/redwood-drm, MIT, is what talks
 * to it): every msm request is a call of this user client whose selector is the request's
 * number (the low byte of the ioctl code) and whose structure goes in and comes back as it is;
 * mmap of a buffer is the client memory of the type that is its handle. Request 0x7e, not
 * Linux's, makes an IOSurface's memory a buffer (by surface id).
 *
 * Built on what gpu_service already runs: the same ring, lock and page tables. Buffers get GPU
 * addresses from 0x60000000 to 0xE0000000 (2 GB, no identity mapping may land there) in 4 KB
 * pages, and the kernel chooses them (MSM_INFO_GET_IOVA; VA_START/VA_SIZE refused, as Linux
 * does for a client without its own page tables).
 *
 * Memory is cacheable (never write-combined: changing the cache mode of these pages panics this
 * kernel, see the SGPU client). So: before a submission every buffer it names that the program
 * has mapped is cleaned out of the CPU caches, and every IOSurface whose seed moved (or that was
 * last cleaned more than 4 ms ago: not everyone who writes one says so); GEM_CPU_PREP (the CPU
 * is about to read what the GPU wrote) cleans and invalidates the buffer.
 *
 * Submissions run to completion before the call returns (no queue yet), so every fence is done
 * by the time anyone waits for it; a submission that does not finish in 2 s stops the service.
 * Development quality: one address space for all clients, no register protection.
 */
#define MSM_PAGES           ((MSM_VA_END - MSM_VA_START) >> 12)
#define MSM_BUFFERS         4096u
#define MSM_BUFFER_MAX      (256ull << 20)
#define MSM_SUBMIT_CMDS     256u                /* Mesa sends 68 and more (a frame of CoreAnimation) */
#define MSM_SUBMIT_BOS      4096u

enum {
	kDrmGemClose        = 0x09,
	kMsmGetParam        = 0x40,
	kMsmSetParam        = 0x41,
	kMsmGemNew          = 0x42,
	kMsmGemInfo         = 0x43,
	kMsmGemCpuPrep      = 0x44,
	kMsmGemCpuFini      = 0x45,
	kMsmGemSubmit       = 0x46,
	kMsmWaitFence       = 0x47,
	kMsmGemMadvise      = 0x48,
	kMsmSubmitQueueNew  = 0x4a,
	kMsmSubmitQueueClose = 0x4b,
	kMsmSubmitQueueQuery = 0x4c,
	kMsmSurface         = 0x7e,             /* HackiOS's: an IOSurface's memory as a buffer */
};

struct msm_timespec { int64_t seconds, nanoseconds; };
struct msm_param { uint32_t pipe, param; uint64_t value; uint32_t len, pad; };
struct msm_gem_new { uint64_t size; uint32_t flags, handle; };
struct msm_gem_info { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct msm_gem_cpu_prep { uint32_t handle, op; struct msm_timespec timeout; };
struct msm_gem_submit_cmd { uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs; uint64_t relocs; };
struct msm_gem_submit_bo { uint32_t flags, handle; uint64_t presumed; };
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence { uint32_t fence, flags; struct msm_timespec timeout; uint32_t queueid; };
struct msm_gem_madvise { uint32_t handle, madv, retained; };
struct msm_submitqueue { uint32_t flags, prio, id; };
struct msm_submitqueue_query { uint64_t data; uint32_t id, param, len, pad; };
struct drm_gem_close { uint32_t handle, pad; };
struct msm_surface {                            /* HackiOS's struct redwood_surface */
	uint32_t surface;                       /* the IOSurface id */
	uint32_t handle;                        /* 0xffffffff: only describe it; back: the buffer */
	uint64_t size;
	uint32_t offset, rowBytes;              /* where the pixels start in the buffer, a row's length */
	uint32_t width, height, pixelFormat, planes;
};

#define MSM_INFO_GET_OFFSET 0
#define MSM_INFO_GET_IOVA   1
#define MSM_INFO_SET_NAME   2
#define MSM_INFO_SET_IOVA   4
#define MSM_INFO_GET_FLAGS  5
#define MSM_SUBMIT_CMD_BUF  1
#define MSM_SUBMIT_CMD_CTX_RESTORE_BUF 3
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000
#define MSM_DESCRIBE        0xffffffffu

static uint64_t msm_bits[MSM_PAGES / 64];
static uint64_t msm_next;                       /* where the next search starts (next fit) */

static bool
msm_used(uint64_t p)
{
	return (msm_bits[p >> 6] >> (p & 63)) & 1;
}

static void
msm_mark(uint64_t p0, uint64_t n, bool used)
{
	for (uint64_t p = p0; p < p0 + n; p++) {
		if (used) {
			msm_bits[p >> 6] |= 1ull << (p & 63);
		} else {
			msm_bits[p >> 6] &= ~(1ull << (p & 63));
		}
	}
}

/* n pages of addresses, next fit, wrapping once; 0 if none */
static uint64_t
msm_va_alloc(uint64_t n)
{
	for (int pass = 0; pass < 2; pass++) {
		uint64_t start = pass == 0 ? msm_next : 0, end = pass == 0 ? MSM_PAGES : msm_next, run = 0;
		for (uint64_t p = start; p < end; p++) {
			if ((p & 63) == 0 && run == 0 && msm_bits[p >> 6] == ~0ull) {
				p += 63;                /* a full word: skip it */
				continue;
			}
			run = msm_used(p) ? 0 : run + 1;
			if (run == n) {
				msm_mark(p + 1 - n, n, true);
				msm_next = p + 1;
				return MSM_VA_START + ((p + 1 - n) << 12);
			}
		}
	}
	return 0;
}

/*
 * 0153: the msm fences have a slot and a counter of their own. On 0152 the shared slot once held
 * 0x801d229c -- a pixel, not a seqno (the GPU's SMMU had a translation fault at about the same
 * time) -- and a GPU that was idle with the ring read to its end was taken for hung, the service
 * stopped and backboardd crashed (Mesa on the CPU after its restart). Now a fence that looks
 * unfinished while the CP is idle and has read everything counts as done, and that is said.
 */
static bool
msm_fence_done(uint32_t f)
{
	return (int32_t)(*(volatile uint32_t *)(pool + OFF_MSM_FENCE) - f) >= 0;
}

static bool
msm_gpu_idle(void)
{
	return grd(REG_CP_RB_RPTR) == ring_wptr % RING_DWORDS && (grd(REG_RBBM_STATUS) & ~1u) == 0;
}

/*
 * GPU memory (0151): the gigabyte from 0x1_0000_0000 to 0x1_4000_0000 is DDR that nobody uses
 * here -- xnu runs from 0x1_4000_0000 (the shim clamps it there), the fixed firmware carve-outs
 * end at 0x65b00000, the shim's payload is below 4 GB -- so Mesa's buffers come from it, mapped
 * write-combined into the program (as the panel's own framebuffer surface is; the memory is not
 * the kernel's, so no cache mode of its pages changes) and seen by the GPU without any cache
 * maintenance: no buffer is cleaned before a submission any more. Contiguous runs of 4 KB pages,
 * next fit. boot-arg gpu_pool=0: the cacheable kernel buffers of 0149 instead. GET_PARAM 0x1000
 * tells the program which (its libdrm then asks for write-combined mappings).
 */
#define GPOOL_PA            0x100000000ull
#define GPOOL_SIZE          0x40000000ull
#define GPOOL_PAGES         (GPOOL_SIZE >> 12)
static uint64_t gpool_bits[GPOOL_PAGES / 64];
static uint64_t gpool_next;
static int gpool_on = -1;

static bool
gpool_enabled(void)
{
	if (gpool_on < 0) {
		uint32_t v = 1;
		(void)PE_parse_boot_argn("gpu_pool", &v, sizeof(v));
		gpool_on = v != 0;
		LOG("msm: buffers %s", gpool_on ? "from the GPU pool 0x100000000-0x140000000, write-combined" :
		    "cacheable kernel memory (gpu_pool=0)");
	}
	return gpool_on;
}

static void
bits_mark(uint64_t *bits, uint64_t p0, uint64_t n, bool used)
{
	for (uint64_t p = p0; p < p0 + n; p++) {
		if (used) {
			bits[p >> 6] |= 1ull << (p & 63);
		} else {
			bits[p >> 6] &= ~(1ull << (p & 63));
		}
	}
}

/* n contiguous pages of the pool: their physical address, or 0. service_lock held. */
static uint64_t
gpool_alloc(uint64_t n)
{
	for (int pass = 0; pass < 2; pass++) {
		uint64_t start = pass == 0 ? gpool_next : 0, end = pass == 0 ? GPOOL_PAGES : gpool_next, run = 0;
		for (uint64_t p = start; p < end; p++) {
			if ((p & 63) == 0 && run == 0 && gpool_bits[p >> 6] == ~0ull) {
				p += 63;
				continue;
			}
			run = ((gpool_bits[p >> 6] >> (p & 63)) & 1) ? 0 : run + 1;
			if (run == n) {
				bits_mark(gpool_bits, p + 1 - n, n, true);
				gpool_next = p + 1;
				return GPOOL_PA + ((p + 1 - n) << 12);
			}
		}
	}
	return 0;
}

/*
 * 0333: how long fences took to be seen done by msm_wait (it had to wait for them): <50 us, <100 us,
 * <200 us, <500 us, <1 ms, <2 ms, <5 ms, longer. For tuning the polling below.
 */
static uint64_t msm_fence_hist[8];

static void
msm_fence_note(uint64_t start)
{
	static const uint64_t limit_us[7] = { 50, 100, 200, 500, 1000, 2000, 5000 };
	uint64_t ns, us;
	unsigned i;

	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	us = ns / 1000;
	for (i = 0; i < 7 && us >= limit_us[i]; i++) {
	}
	msm_fence_hist[i]++;
}

/* Wait for fence f, without the service lock, up to ms; false if it never came. */
static bool
msm_wait(uint32_t f, uint64_t ms)
{
	uint64_t deadline, now, start, spin_end, poll_end;

	if (msm_fence_done(f)) {
		return true;
	}
	if (ms == 0) {
		return false;
	}
	clock_interval_to_deadline((uint32_t)ms, kMillisecondScale, &deadline);
	/* 0333: a frame has 8.3 ms at 120 Hz and the GPU is usually done in 100-500 us: spin 200 us at 1 us, poll to 2 ms at 5 us, then sleep */
	start = mach_absolute_time();
	nanoseconds_to_absolutetime(200 * 1000ull, &spin_end);
	nanoseconds_to_absolutetime(2000 * 1000ull, &poll_end);
	spin_end += start;
	poll_end += start;
	while (!msm_fence_done(f)) {
		now = mach_absolute_time();
		if (now >= deadline) {
			if (msm_gpu_idle()) {
				static uint32_t said;
				if (said++ < 8) {
					LOG("msm: fence %u not written (slot %08x) but the GPU is idle with the ring read: done", f,
					    *(volatile uint32_t *)(pool + OFF_MSM_FENCE));
				}
				return true;
			}
			return false;
		}
		if (now < spin_end) {
			IODelay(1);
		} else if (now < poll_end) {
			IODelay(5);
		} else {
			delay_for_interval(50, NSEC_PER_USEC);      /* (0329: sleeps 50-150+ us) */
		}
	}
	msm_fence_note(start);
	return true;
}

/* How long from now a time of msm's is (CLOCK_MONOTONIC = mach_continuous_time here), in ms; at most 2 s. */
static uint64_t
msm_ms_until(const struct msm_timespec *when)
{
	uint64_t now;
	int64_t left;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	left = when->seconds * (int64_t)NSEC_PER_SEC + when->nanoseconds - (int64_t)now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t)left / NSEC_PER_MSEC > 2000 ? 2000 : (uint64_t)left / NSEC_PER_MSEC;
}

/* A submission that never finishes: the service stops (no more work is given to a GPU that may still write). */
static void
msm_hung(const char *who, uint32_t fence)
{
	if (service_ready) {
		service_ready = false;
		LOG("msm: %s: submission %u did not finish in 2 s (done: %u): the GPU service stops; RBBM_STATUS %08x CP_HW_FAULT %08x rptr %u wptr %u",
		    who, fence, *(volatile uint32_t *)(pool + OFF_MSM_FENCE), grd(REG_RBBM_STATUS), grd(REG_CP_HW_FAULT),
		    grd(REG_CP_RB_RPTR), ring_wptr % RING_DWORDS);
		smmu_fault("msm");
	}
}

struct msm_bo {
	IOMemoryDescriptor *md;                 /* NULL: free handle */
	SweetSurface       *surf;
	uint8_t            *kva;                /* kernel address of byte 0 (cache maintenance) */
	uint64_t            va, size;
	uint32_t            flags, seed;
	uint64_t            cleaned;            /* surfaces: when they were last cleaned */
	bool                mapped;             /* the program has mapped it: it may have written it */
	bool                pooled;             /* GPU pool memory at pa (write-combined, never cleaned) */
	uint64_t            pa;
	uint32_t            fence;              /* the last submission that named it */
};
#define MSM_LEFT            256u                /* buffers given back while the GPU still had them */

class SapphireGPU : public IOService {
	OSDeclareDefaultStructors(SapphireGPU);
};

OSDefineMetaClassAndStructors(SapphireGPU, IOService);

class SapphireGPUClient : public IOUserClient {
	OSDeclareDefaultStructors(SapphireGPUClient);

public:
	char            fName[32];
	struct msm_bo  *fBo;                    /* handle n is fBo[n - 1] */
	struct msm_bo  *fLeft;                  /* MSM_LEFT of them: kept until their fence */
	uint32_t        fCount, fSubmits, fQueue;
	uint64_t        fBytes;
	thread_call_t   fReapTimer;             /* 0353: reaps fLeft every 10 ms, so a client that stopped submitting gives its memory back */
	volatile bool   fClosing;

	static void reapTimer(thread_call_param_t self, thread_call_param_t unused);
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;

	struct msm_bo *bo(uint32_t handle);
	struct msm_bo *slot(uint32_t *handle);
	bool place(struct msm_bo *b);
	void drop(struct msm_bo *b);
	void freeBo(struct msm_bo *b);
	void reap(void);
	IOReturn gemNew(struct msm_gem_new *r);
	IOReturn gemInfo(struct msm_gem_info *r);
	IOReturn surface(struct msm_surface *r);
	IOReturn submitIt(struct msm_gem_submit *r);
};

OSDefineMetaClassAndStructors(SapphireGPUClient, IOUserClient);

bool
SapphireGPUClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	proc_selfname(fName, sizeof(fName));
	fBo = IONew(struct msm_bo, MSM_BUFFERS);
	fLeft = IONew(struct msm_bo, MSM_LEFT);
	if (fBo == NULL || fLeft == NULL) {
		if (fBo != NULL) {
			IODelete(fBo, struct msm_bo, MSM_BUFFERS);
			fBo = NULL;
		}
		if (fLeft != NULL) {
			IODelete(fLeft, struct msm_bo, MSM_LEFT);
			fLeft = NULL;
		}
		return false;
	}
	bzero(fBo, sizeof(struct msm_bo) * MSM_BUFFERS);
	bzero(fLeft, sizeof(struct msm_bo) * MSM_LEFT);
	fCount = fSubmits = fQueue = 0;
	fBytes = 0;
	fClosing = false;
	fReapTimer = thread_call_allocate(&SapphireGPUClient::reapTimer, this);
	if (fReapTimer != NULL) {
		uint64_t when;

		clock_interval_to_deadline(10, kMillisecondScale, &when);
		thread_call_enter_delayed(fReapTimer, when);
	}
	LOG("msm: %s opened the GPU", fName);
	return true;
}

/*
 * 0353: buffers a client gave back are kept until the GPU is done with them (fLeft), and were only
 * let go when the client next did something: backboardd blocked on "no memory" never did, so the
 * pool stayed full. A timer of ours looks every 10 ms.
 */
void
SapphireGPUClient::reapTimer(thread_call_param_t self, thread_call_param_t unused __unused)
{
	SapphireGPUClient *c = (SapphireGPUClient *)self;
	uint64_t when;

	if (c->fClosing) {
		return;
	}
	if (service_ready) {
		IOLockLock(service_lock);
		c->reap();
		IOLockUnlock(service_lock);
	}
	clock_interval_to_deadline(10, kMillisecondScale, &when);
	if (!c->fClosing) {
		thread_call_enter_delayed(c->fReapTimer, when);
	}
}

/* service_lock held */
struct msm_bo *
SapphireGPUClient::bo(uint32_t handle)
{
	if (handle == 0 || handle > MSM_BUFFERS || fBo[handle - 1].md == NULL) {
		return NULL;
	}
	return &fBo[handle - 1];
}

struct msm_bo *
SapphireGPUClient::slot(uint32_t *handle)
{
	for (uint32_t i = 0; i < MSM_BUFFERS; i++) {
		if (fBo[i].md == NULL) {
			*handle = i + 1;
			return &fBo[i];
		}
	}
	return NULL;
}

/* Give b->md [0, b->size) GPU addresses, page by page. service_lock held. */
bool
SapphireGPUClient::place(struct msm_bo *b)
{
	uint64_t n = b->size >> 12, seg;

	b->va = msm_va_alloc(n);
	if (b->va == 0) {
		LOG("msm: %s: no %llu MB of GPU addresses left", fName, b->size >> 20);
		return false;
	}
	for (uint64_t p = 0; p < n; p++) {
		uint64_t pa = b->md->getPhysicalSegment(p << 12, &seg, kIOMemoryMapperNone);
		if (pa == 0 || !map_page_4k(b->va + (p << 12), pa & ~0xFFFull)) {
			LOG("msm: %s: cannot map page %llu of %llu (pa %llx; %u + %u page tables used)", fName, p, n, pa, pt_used,
			    pt2_used);
			for (uint64_t q = 0; q < p; q++) {
				unmap_page_4k(b->va + (q << 12));
			}
			(void)tlb_flush();
			msm_mark((b->va - MSM_VA_START) >> 12, n, false);
			return false;
		}
	}
	return tlb_flush();
}

/*
 * msm buffers of every client, by kind, for the 5 s log (a leak shows as one of them growing):
 * [0] imported IOSurfaces (their memory stays wired while imported), [1] the GPU pool (outside
 * the kernel's RAM), [2] ordinary kernel memory (wired).
 */
static SInt32  msm_census_n[3];
static SInt64  msm_census_bytes[3];

static void
msm_census(const struct msm_bo *b, int delta)
{
	unsigned k = b->surf != NULL ? 0 : b->pooled ? 1 : 2;

	OSAddAtomic(delta, &msm_census_n[k]);
	OSAddAtomic64(delta * (SInt64)b->size, &msm_census_bytes[k]);
}

extern "C" void
sapphire_gpu_census(uint32_t n[3], uint64_t bytes[3])
{
	for (unsigned k = 0; k < 3; k++) {
		n[k] = (uint32_t)msm_census_n[k];
		bytes[k] = (uint64_t)msm_census_bytes[k];
	}
}

/* Free a buffer the GPU is done with. service_lock held. */
void
SapphireGPUClient::freeBo(struct msm_bo *b)
{
	uint64_t n = b->size >> 12;

	msm_census(b, -1);

	for (uint64_t p = 0; p < n; p++) {
		unmap_page_4k(b->va + (p << 12));
	}
	(void)tlb_flush();
	msm_mark((b->va - MSM_VA_START) >> 12, n, false);
	if (b->surf != NULL) {
		b->md->complete();
		b->surf->release();
	}
	if (b->pooled) {
		bits_mark(gpool_bits, (b->pa - GPOOL_PA) >> 12, n, false);
	}
	b->md->release();
	fCount--;
	fBytes -= b->size;
	bzero(b, sizeof(*b));
}

/* Buffers given back earlier whose submissions are done now: freed. service_lock held. */
void
SapphireGPUClient::reap(void)
{
	for (uint32_t i = 0; fLeft != NULL && i < MSM_LEFT; i++) {
		if (fLeft[i].md != NULL && msm_fence_done(fLeft[i].fence)) {
			fCount++;                       /* (counted out when it was left: release counts it again) */
			fBytes += fLeft[i].size;
			freeBo(&fLeft[i]);
		}
	}
}

/*
 * GEM_CLOSE: the handle is free at once; the memory and the GPU addresses stay until the last
 * submission that named the buffer is done. service_lock held.
 */
void
SapphireGPUClient::drop(struct msm_bo *b)
{
	if (b->fence == 0 || msm_fence_done(b->fence) || !service_ready) {
		freeBo(b);
		return;
	}
	for (uint32_t i = 0; i < MSM_LEFT; i++) {
		if (fLeft[i].md == NULL) {
			fLeft[i] = *b;
			fCount--;
			fBytes -= b->size;
			bzero(b, sizeof(*b));
			return;
		}
	}
	IOLockUnlock(service_lock);                 /* no room to leave it: wait for it */
	if (!msm_wait(b->fence, 2000)) {
		msm_hung(fName, b->fence);
	}
	IOLockLock(service_lock);
	freeBo(b);
}

IOReturn
SapphireGPUClient::clientClose(void)
{
	fClosing = true;
	if (fReapTimer != NULL) {                   /* (0353) no reap may run past this point */
		thread_call_cancel_wait(fReapTimer);
		thread_call_free(fReapTimer);
		fReapTimer = NULL;
	}
	if (service_ready && !msm_wait(msm_seq, 2000)) {       /* nothing of ours may still be at work */
		msm_hung(fName, msm_seq);
	}
	IOLockLock(service_lock);
	for (uint32_t i = 0; fBo != NULL && i < MSM_BUFFERS; i++) {
		if (fBo[i].md != NULL) {
			freeBo(&fBo[i]);
		}
	}
	for (uint32_t i = 0; fLeft != NULL && i < MSM_LEFT; i++) {
		if (fLeft[i].md != NULL) {
			fCount++;
			fBytes += fLeft[i].size;
			freeBo(&fLeft[i]);
		}
	}
	IOLockUnlock(service_lock);
	LOG("msm: %s closed (%u submissions)", fName, fSubmits);
	if (fBo != NULL) {
		IODelete(fBo, struct msm_bo, MSM_BUFFERS);
		fBo = NULL;
	}
	if (fLeft != NULL) {
		IODelete(fLeft, struct msm_bo, MSM_LEFT);
		fLeft = NULL;
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
SapphireGPUClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	IOReturn r = kIOReturnBadArgument;

	IOLockLock(service_lock);
	struct msm_bo *b = fBo != NULL ? bo(type) : NULL;
	if (b != NULL) {
		b->md->retain();
		b->mapped = true;
		*memory = b->md;
		/* pool memory write-combined; kernel memory never (see above). libdrm asks for the same. */
		*options = b->pooled ? kIOMapWriteCombineCache : kIOMapDefaultCache;
		r = kIOReturnSuccess;
	}
	IOLockUnlock(service_lock);
	return r;
}

IOReturn
SapphireGPUClient::gemNew(struct msm_gem_new *r)
{
	uint64_t size = (r->size + 4095) & ~4095ull;
	uint32_t h;

	if (size == 0 || size > MSM_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	if (gpool_enabled()) {
		IOLockLock(service_lock);
		uint64_t pa = gpool_alloc(size >> 12);
		/* 0353: none free: give back what the GPU is done with, and try again (three times, 1 ms apart) */
		for (int tries = 0; pa == 0 && tries < 3; tries++) {
			reap();
			pa = gpool_alloc(size >> 12);
			if (pa == 0) {
				IOLockUnlock(service_lock);
				IOSleep(1);
				IOLockLock(service_lock);
				pa = gpool_alloc(size >> 12);
			}
		}
		IOLockUnlock(service_lock);
		if (pa == 0) {
			LOG("msm: %s: the GPU pool has no %llu KB in one piece (%u buffers, %llu MB)", fName, size >> 10, fCount,
			    fBytes >> 20);
			return kIOReturnNoMemory;
		}
		IOMemoryDescriptor *pm = IOMemoryDescriptor::withPhysicalAddress(pa, size, kIODirectionInOut);
		/*
		 * 0205: zeroed only when small. CoreAnimation makes and drops screen-sized textures many
		 * times a second while it animates (render targets it then draws all of); mapping 11 MB into
		 * the kernel and zeroing it write-combined made every such GEM_NEW cost 0.5 ms and more on
		 * Mesa's driver thread. Small buffers (state, queries) are still zeroed; boot-arg gpu_zero=1:
		 * every buffer, as before.
		 * 0215: gpu_zero=1 is the default again: with 0205 folders opening in the dark appearance
		 * showed white frames (and once the whole screen went yellow), which gpu_zero=1 took away.
		 */
		static int zero_all = -1;
		if (zero_all < 0) {
			int z = 1;              /* 0215: on by default again (gpu_zero=0: 0205's way); see below */
			PE_parse_boot_argn("gpu_zero", &z, sizeof(z));
			zero_all = z != 0;
		}
		bool zeroed = pm != NULL;
		/*
		 * 0332: the write-combine mapping is made even when the zeroing is skipped (>= 256 KB with
		 * gpu_zero=0). The pool is an unmanaged carve-out (withPhysicalAddress): without the kernel's
		 * own write-combine map, IOConnectMapMemory64 in user space mapped such slabs as Device
		 * memory, and unaligned NEON stores into them (the 256 KB command-buffer slab) took an
		 * alignment fault.
		 */
		if (pm != NULL) {
			IOMemoryMap *km = pm->map(kIOMapWriteCombineCache);
			if (km != NULL) {
				if (zero_all || size < 256 * 1024) {
					bzero((void *)km->getVirtualAddress(), size);
				}
				km->release();
			} else {
				zeroed = false;
			}
		}
		IOLockLock(service_lock);
		struct msm_bo *b = zeroed ? slot(&h) : NULL;
		if (b != NULL) {
			b->md = pm;
			b->kva = NULL;                  /* never cleaned: write-combined everywhere */
			b->size = size;
			b->flags = r->flags;
			b->pooled = true;
			b->pa = pa;
			if (!place(b)) {
				bzero(b, sizeof(*b));
				b = NULL;
			}
		}
		if (b != NULL) {
			fCount++;
			fBytes += size;
			msm_census(b, 1);
			r->handle = h;
		} else {
			bits_mark(gpool_bits, (pa - GPOOL_PA) >> 12, size >> 12, false);
		}
		IOLockUnlock(service_lock);
		if (b == NULL) {
			OSSafeReleaseNULL(pm);
			return kIOReturnNoMemory;
		}
		return kIOReturnSuccess;
	}
	IOBufferMemoryDescriptor *m = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, size, 4096);
	if (m == NULL) {
		LOG("msm: %s: no memory for %llu KB (%u buffers, %llu MB)", fName, size >> 10, fCount, fBytes >> 20);
		return kIOReturnNoMemory;
	}
	bzero(m->getBytesNoCopy(), size);
	sweet_dcache_flush((vm_offset_t)m->getBytesNoCopy(), (unsigned)size);
	IOLockLock(service_lock);
	struct msm_bo *b = slot(&h);
	if (b != NULL) {
		b->md = m;
		b->kva = (uint8_t *)m->getBytesNoCopy();
		b->size = size;
		b->flags = r->flags;
		if (!place(b)) {
			bzero(b, sizeof(*b));
			b = NULL;
		}
	}
	if (b != NULL) {
		fCount++;
		fBytes += size;
		msm_census(b, 1);
		r->handle = h;
	}
	IOLockUnlock(service_lock);
	if (b == NULL) {
		m->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

IOReturn
SapphireGPUClient::gemInfo(struct msm_gem_info *r)
{
	IOReturn ret = kIOReturnSuccess;

	IOLockLock(service_lock);
	struct msm_bo *b = bo(r->handle);
	if (b == NULL) {
		ret = kIOReturnBadArgument;
	} else if (r->info == MSM_INFO_GET_OFFSET) {
		r->value = (uint64_t)r->handle << 32;   /* nobody maps by it here */
	} else if (r->info == MSM_INFO_GET_IOVA) {
		r->value = b->va;
	} else if (r->info == MSM_INFO_GET_FLAGS) {
		r->value = b->flags;
	} else if (r->info != MSM_INFO_SET_NAME) {
		ret = kIOReturnUnsupported;             /* SET_IOVA among them: addresses are the kernel's */
	}
	IOLockUnlock(service_lock);
	return ret;
}

/* An IOSurface's memory as a buffer (HackiOS request 0x7e). */
IOReturn
SapphireGPUClient::surface(struct msm_surface *r)
{
	SweetSurface *s = sweet_surface_copy(r->surface);
	uint32_t h;

	if (s == NULL || s->memory == NULL) {
		OSSafeReleaseNULL(s);
		return kIOReturnNotFound;
	}
	if (s->wrapped) {
		s->release();                           /* a program's own memory: rows the GPU seldom takes */
		return kIOReturnUnsupported;
	}
	uint64_t size = (s->memory->getLength() + 4095) & ~4095ull;
	r->size = size;
	r->offset = s->offset;
	r->rowBytes = s->bytesPerRow;
	r->width = s->width;
	r->height = s->height;
	r->pixelFormat = s->pixelFormat;
	r->planes = s->planeCount;
	if (r->handle == MSM_DESCRIBE) {
		r->handle = 0;
		s->release();
		return kIOReturnSuccess;
	}
	IOLockLock(service_lock);
	for (uint32_t i = 0; i < MSM_BUFFERS; i++) {
		if (fBo[i].md != NULL && fBo[i].surf == s) {
			r->handle = i + 1;              /* the same surface is the same buffer, as Linux's handles */
			IOLockUnlock(service_lock);
			s->release();
			return kIOReturnSuccess;
		}
	}
	struct msm_bo *b = slot(&h);
	IOReturn ret = kIOReturnNoMemory;
	if (b != NULL && s->memory->prepare() == kIOReturnSuccess) {
		b->md = s->memory;
		b->md->retain();
		b->surf = s;
		b->size = size;
		b->kva = (uint8_t *)s->kernelAddress();
		if (place(b)) {
			fCount++;
			fBytes += size;
			msm_census(b, 1);
			r->handle = h;
			s = NULL;                       /* the buffer keeps the reference */
			ret = kIOReturnSuccess;
		} else {
			b->md->complete();
			b->md->release();
			bzero(b, sizeof(*b));
		}
	}
	IOLockUnlock(service_lock);
	OSSafeReleaseNULL(s);
	return ret;
}

IOReturn
SapphireGPUClient::submitIt(struct msm_gem_submit *r)
{
	struct msm_gem_submit_bo *bos = NULL;
	struct msm_gem_submit_cmd *cmds = NULL;
	IOReturn ret = kIOReturnBadArgument;

	if (r->nr_cmds == 0 || r->nr_cmds > MSM_SUBMIT_CMDS || r->nr_bos == 0 || r->nr_bos > MSM_SUBMIT_BOS) {
		LOG("msm: %s: a submission of %u commands in %u buffers: not taken", fName, r->nr_cmds, r->nr_bos);
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, r->nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, r->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t)r->bos, bos, r->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t)r->cmds, cmds, r->nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	IOLockLock(service_lock);
	if (!service_ready) {
		IOLockUnlock(service_lock);
		ret = kIOReturnNotReady;
		goto out;
	}
	reap();
	if (!ring_room(4 * r->nr_cmds + 16, 2000)) {
		msm_hung(fName, msm_seq);
		IOLockUnlock(service_lock);
		ret = kIOReturnTimeout;
		goto out;
	}
	{
		uint64_t now = mach_absolute_time();
		for (uint32_t i = 0; i < r->nr_bos; i++) {
			struct msm_bo *b = bo(bos[i].handle);
			if (b == NULL || b->kva == NULL) {
				continue;
			}
			if (b->surf != NULL && b->surf->state != NULL) {
				uint32_t seed = *(volatile uint32_t *)((uint8_t *)b->surf->state->getBytesNoCopy() + 0xc);
				/* the GPU has drawn it since that seed: the display need not clean it (SweetFramebuffer) */
				b->surf->gpuSeed = seed;
				b->surf->gpuDrawn = true;
				/*
				 * 0165: a surface is cleaned only when the CPU may have written it since: its seed
				 * moved (IOSurfaceLock for writing) or it is mapped as a buffer. The 4 ms window that
				 * also cleaned an unchanged surface again cleaned the 11 MB display surface on every
				 * frame (glFlush took 9 ms a frame, most of the CPU's time); the GPU alone writes it.
				 */
				if (seed == b->seed && !b->mapped) {
					continue;
				}
				b->seed = seed;
				b->cleaned = now;
			} else if (!b->mapped) {
				continue;                       /* never written by the CPU since it was cleaned */
			}
			sweet_dcache_flush((vm_offset_t)b->kva, (unsigned)b->size);
		}
		uint32_t n = 0;
		for (uint32_t i = 0; i < r->nr_cmds; i++) {
			struct msm_bo *b = cmds[i].submit_idx < r->nr_bos ? bo(bos[cmds[i].submit_idx].handle) : NULL;
			if (cmds[i].type != MSM_SUBMIT_CMD_BUF && cmds[i].type != MSM_SUBMIT_CMD_CTX_RESTORE_BUF) {
				continue;                       /* IB_TARGET_BUF: only reached from another */
			}
			if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
			    (uint64_t)cmds[i].submit_offset + cmds[i].size > b->size) {
				LOG("msm: %s: command %u (buffer %u, +%u, %u bytes, %u relocs): not taken", fName, i,
				    cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
				IOLockUnlock(service_lock);
				goto out;
			}
			uint64_t va = b->va + cmds[i].submit_offset;
			emit(pkt7(CP_INDIRECT_BUFFER, 3));
			emit((uint32_t)va);
			emit((uint32_t)(va >> 32));
			emit(cmds[i].size / 4);
			n++;
		}
		emit(pkt7(CP_WAIT_FOR_IDLE, 0));
		emit(pkt7(CP_EVENT_WRITE, 4));          /* CACHE_FLUSH_TS into the msm slot (event() writes the shared one) */
		emit(EV_CACHE_FLUSH_TS);                /* (not TIMESTAMP: see event()) */
		emit((uint32_t)(POOL_PA + OFF_MSM_FENCE));
		emit((uint32_t)((POOL_PA + OFF_MSM_FENCE) >> 32));
		emit(++msm_seq);
		r->fence = msm_seq;
		kick();                                 /* and back to the program: msm_wait() is how it waits */
		for (uint32_t i = 0; i < r->nr_bos; i++) {
			struct msm_bo *b = bo(bos[i].handle);
			if (b != NULL) {
				b->fence = r->fence;
			}
		}
		if (r->flags & MSM_SUBMIT_FENCE_FD_OUT) {
			r->fence_fd = -1;
		}
		if (fSubmits++ < 4) {
			LOG("msm: %s: submission %u, %u command buffers in %u buffers (asynchronous)", fName, r->fence, n,
			    r->nr_bos);
		}
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(service_lock);
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, r->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, r->nr_cmds);
	}
	return ret;
}

/* msm GET_PARAM for this GPU (Linux adreno_get_param; numbers from msm_drm.h) */
static bool
msm_param(uint32_t which, uint64_t *value)
{
	switch (which) {
	case 0x01: *value = 610; return true;                   /* GPU_ID */
	case 0x02: *value = 128 * 1024; return true;            /* GMEM_SIZE */
	case 0x03: *value = 0x06010001; return true;            /* CHIP_ID (a610 v2, read off the GPU) */
	case 0x04: *value = 600000000; return true;             /* MAX_FREQ (nominal) */
	case 0x05: *value = mach_absolute_time(); return true;  /* TIMESTAMP */
	case 0x06: *value = 0x100000; return true;              /* GMEM_BASE: UCHE_GMEM_RANGE_MIN of hw_init */
	case 0x07: *value = 1; return true;                     /* PRIORITIES */
	case 0x08: case 0x09: case 0x0a: case 0x11: *value = 0; return true;  /* PP_PGTABLE FAULTS SUSPENDS RAYTRACING */
	case 0x10: *value = 14; return true;                    /* HIGHEST_BANK_BIT */
	case 0x1000: *value = gpool_enabled(); return true;     /* sapphire: buffers are write-combined (libdrm) */
	default: return false;                                  /* VA_START/VA_SIZE among them */
	}
}

IOReturn
SapphireGPUClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
    OSObject *, void *)
{
	union {
		struct msm_param param;
		struct msm_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
		struct msm_surface surface;
	} r;
	size_t size = args->structureInputSize;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0;
	static uint32_t unknown;

	if (args->structureInput == NULL || size > sizeof(r) || fBo == NULL) {
		return kIOReturnBadArgument;
	}
	if (!service_ready) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);
	switch (selector) {
	case kMsmGetParam:
		ret = msm_param(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kMsmSetParam:
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
	case kMsmGemCpuPrep: {
		/* the CPU is about to use the buffer: the GPU done with it, and (kernel memory) nothing stale cached */
		IOLockLock(service_lock);
		struct msm_bo *b = bo(r.cpuPrep.handle);
		uint32_t f = b != NULL ? b->fence : 0;
		IOLockUnlock(service_lock);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
			break;
		}
		if (f != 0 && !msm_wait(f, (r.cpuPrep.op & 0x04) ? 0 : msm_ms_until(&r.cpuPrep.timeout))) {
			ret = kIOReturnBusy;            /* MSM_PREP_NOSYNC, or not done in time */
			break;
		}
		IOLockLock(service_lock);
		b = bo(r.cpuPrep.handle);
		if (b != NULL && b->kva != NULL) {
			sweet_dcache_flush((vm_offset_t)b->kva, (unsigned)b->size);
		}
		IOLockUnlock(service_lock);
		break;
	}
	case kMsmGemSubmit:
		ret = submitIt(&r.submit);
		break;
	case kMsmWaitFence: {
		uint64_t ms = msm_ms_until(&r.waitFence.timeout);
		if (!msm_wait(r.waitFence.fence, ms)) {
			ret = kIOReturnTimeout;
			if (ms >= 2000) {
				msm_hung(fName, r.waitFence.fence);
			}
		}
		break;
	}
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		r.queue.id = ++fQueue;
		break;
	case kMsmSubmitQueueQuery:
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			(void)copyout(&zero, (user_addr_t)r.query.data, sizeof(zero));
		}
		break;
	case kDrmGemClose: {
		IOLockLock(service_lock);
		struct msm_bo *b = bo(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			drop(b);
		}
		IOLockUnlock(service_lock);
		break;
	}
	case kMsmSurface:
		ret = surface(&r.surface);
		break;
	default:
		if (unknown++ < 16) {
			LOG("msm: %s: request %#x (%zu bytes) is not known here", fName, selector, size);
		}
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t)size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}

/* Called once gpu_service is up: publish the service Mesa's libdrm looks for. */
static void
msm_publish(void)
{
	IOService *provider = IOService::getPlatform();
	SapphireGPU *gpu = OSTypeAlloc(SapphireGPU);

	if (gpu == NULL || provider == NULL || !gpu->init()) {
		OSSafeReleaseNULL(gpu);
		LOG("msm: cannot make the SapphireGPU service");
		return;
	}
	gpu->setProperty(kIOUserClientClassKey, "SapphireGPUClient");
	gpu->setName("SapphireGPU");
	if (gpu->attach(provider)) {
		if (gpu->start(provider)) {
			gpu->registerService();
			LOG("*** msm: SapphireGPU published: Mesa can open the GPU ***");
		} else {
			gpu->detach(provider);
		}
	}
	gpu->release();
}

void
hakios_a610_init(void)
{
	uint32_t stage = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("gpu", &stage, sizeof(stage)) || stage == 0) {
		return;
	}
	if (kernel_thread_start(gpu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
