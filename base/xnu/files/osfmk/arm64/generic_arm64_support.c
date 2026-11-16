/*
 * Routines the open-source tree references but only defines for Apple SoCs.
 * Needed when building for a generic ARMv8 board without a monitor (PPL).
 */
#include <mach/mach_types.h>
#include <arm/machine_routines.h>
#include <arm/caches_internal.h>
#include <arm/pmap.h>

/* No PPL / secure monitor to switch on. */
void
ml_enable_monitor(void)
{
}

/*
 * On Apple cores the "_nohid" variant skips toggling a HID chicken bit;
 * elsewhere it is simply a forced clean to the point of coherency.
 */
void
CleanPoC_DcacheRegion_Force_nopreempt_nohid(vm_offset_t va, size_t length)
{
	CleanPoC_DcacheRegion_Force(va, length);
}

/*
 * SM6225 (sapphire): cache maintenance for buffers that bus masters read behind the CPU caches
 * (MDP scanout, DSI command DMA, GPU/SMMU tables). XNU's flush_dcache()/clean_dcache() are no-ops
 * in an Apple-SoC (__ARM_COHERENT_IO__) build, and Cortex-A53/A73 are not I/O coherent with these
 * masters. Ported from iOS 16 patches 0101/0107/0111 (sweet_dcache_clean/flush).
 *   clean : dc cvac  - write dirty lines back to the point of coherency (display buffers)
 *   flush : dc civac - write back and invalidate (buffers a device also writes)
 * addr is a kernel virtual address; DminLine comes from CTR_EL0.
 */
void hakios_dcache_clean(vm_offset_t addr, unsigned length);
void hakios_dcache_flush(vm_offset_t addr, unsigned length);

static void
hakios_dcache_range(vm_offset_t addr, unsigned length, bool invalidate)
{
	uint64_t ctr;
	vm_offset_t line, end;

	if (length == 0) {
		__builtin_arm_dsb(DSB_SY);
		return;
	}
	__asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
	line = 4UL << ((ctr >> 16) & 0xf);              /* DminLine, in words */
	end = addr + length;
	__builtin_arm_dsb(DSB_SY);
	for (vm_offset_t va = addr & ~(line - 1); va < end; va += line) {
		if (invalidate) {
			__asm__ volatile ("dc civac, %0" : : "r"(va) : "memory");
		} else {
			__asm__ volatile ("dc cvac, %0" : : "r"(va) : "memory");
		}
	}
	__builtin_arm_dsb(DSB_SY);
}

void
hakios_dcache_clean(vm_offset_t addr, unsigned length)
{
	hakios_dcache_range(addr, length, false);
}

void
hakios_dcache_flush(vm_offset_t addr, unsigned length)
{
	hakios_dcache_range(addr, length, true);
}

/*
 * Stock LLVM lowers Mach-O static destructors to __cxa_atexit registrations
 * (Apple's clang emits __mod_term_func entries in kernel mode instead).
 * The kernel never exits, so registration is a no-op.
 */
int __cxa_atexit(void (*func)(void *), void *arg, void *dso);
int
__cxa_atexit(void (*func)(void *) __unused, void *arg __unused, void *dso __unused)
{
	return 0;
}

/*
 * Profile-guided-optimisation runtime hooks, normally supplied by Apple's
 * compiler-rt build for the kernel. Report "no profile data".
 */
uint64_t __llvm_profile_get_size_for_buffer_internal(const void *, const void *, const void *, const void *, const void *, const void *);
int __llvm_profile_write_buffer_internal(char *, const void *, const void *, const void *, const void *, const void *, const void *);

uint64_t
__llvm_profile_get_size_for_buffer_internal(const void *a __unused, const void *b __unused,
    const void *c __unused, const void *d __unused, const void *e __unused, const void *f __unused)
{
	return 0;
}

int
__llvm_profile_write_buffer_internal(char *buf __unused, const void *a __unused, const void *b __unused,
    const void *c __unused, const void *d __unused, const void *e __unused, const void *f __unused)
{
	return -1;
}
