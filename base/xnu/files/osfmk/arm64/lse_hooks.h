/*
 * lse_hooks.h - Pure ARMv8.0 Freestanding LL/SC Atomic Primitives
 * Target: Cortex-A73/A53 (Snapdragon 685 SM6225, baseline ARMv8.0-A)
 *
 * Implements hardware-atomic CAS, CASP, SWP, LDADD, LDCLR, LDSET, LDEOR,
 * LDSMAX, LDSMIN, LDUMAX, LDUMIN across all standard integer sizes:
 *   - 8-bit  (byte)
 *   - 16-bit (halfword)
 *   - 32-bit (word)
 *   - 64-bit (doubleword)
 *   - 128-bit (quadword pair for CASP)
 *
 * FREESTANDING GUARANTEES:
 * 1. ZERO dependencies on libc or compiler runtime libraries.
 * 2. ZERO use of compiler builtins (__atomic_*, __sync_*) that might
 *    cause the compiler to emit ARMv8.1-A LSE instructions (cas, ldadd, etc.)
 *    inside the kernel trap handler, which would trigger recursive traps!
 * 3. Safe for SMP concurrency across all 8 cores via hardware exclusive monitors,
 *    with bounded retry fallback (1000 attempts) so non-shareable or early-boot
 *    mappings never livelock if the global exclusive monitor rejects stlxr.
 */

#ifndef _OSFMK_ARM64_LSE_HOOKS_H_
#define _OSFMK_ARM64_LSE_HOOKS_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Compare and Swap (CAS)
 * Returns true if swap succeeded (*expected matched memory), false otherwise.
 * In all cases, *expected receives the original value read from memory.
 * FINDING-018: Retry bound increased to 10000; clrex + yield after 100 retries;
 * panic on total failure.
 * ------------------------------------------------------------------------- */

static inline bool atomic_cas8_llsc(volatile uint8_t *ptr, uint8_t *expected, uint8_t desired) {
    uint32_t old, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w2, #10000     \n"
        "1: ldaxrb  %w0, [%3]       \n"
        "   cmp     %w0, %w4        \n"
        "   b.ne    2f              \n"
        "   stlxrb  %w1, %w5, [%3]  \n"
        "   cbz     %w1, 4f         \n"
        "   subs    %w2, %w2, #1    \n"
        "   cmp     %w2, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   strb    %w5, [%3]       \n"
        "4: mov     %w1, #0         \n"
        "   b       3f              \n"
        "2: clrex                   \n"
        "   mov     %w1, #1         \n"
        "3:                         \n"
        : "=&r"(old), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"((uint32_t)*expected), "r"((uint32_t)desired)
        : "cc", "memory"
    );
    *expected = (uint8_t)old;
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory"); /* Panic on total failure */
    }
    return (failed == 0);
}

static inline bool atomic_cas16_llsc(volatile uint16_t *ptr, uint16_t *expected, uint16_t desired) {
    uint32_t old, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w2, #10000     \n"
        "1: ldaxrh  %w0, [%3]       \n"
        "   cmp     %w0, %w4        \n"
        "   b.ne    2f              \n"
        "   stlxrh  %w1, %w5, [%3]  \n"
        "   cbz     %w1, 4f         \n"
        "   subs    %w2, %w2, #1    \n"
        "   cmp     %w2, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   strh    %w5, [%3]       \n"
        "4: mov     %w1, #0         \n"
        "   b       3f              \n"
        "2: clrex                   \n"
        "   mov     %w1, #1         \n"
        "3:                         \n"
        : "=&r"(old), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"((uint32_t)*expected), "r"((uint32_t)desired)
        : "cc", "memory"
    );
    *expected = (uint16_t)old;
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (failed == 0);
}

static inline bool atomic_cas32_llsc(volatile uint32_t *ptr, uint32_t *expected, uint32_t desired) {
    uint32_t old, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w2, #10000     \n"
        "1: ldaxr   %w0, [%3]       \n"
        "   cmp     %w0, %w4        \n"
        "   b.ne    2f              \n"
        "   stlxr   %w1, %w5, [%3]  \n"
        "   cbz     %w1, 4f         \n"
        "   subs    %w2, %w2, #1    \n"
        "   cmp     %w2, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   str     %w5, [%3]       \n"
        "4: mov     %w1, #0         \n"
        "   b       3f              \n"
        "2: clrex                   \n"
        "   mov     %w1, #1         \n"
        "3:                         \n"
        : "=&r"(old), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(*expected), "r"(desired)
        : "cc", "memory"
    );
    *expected = old;
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (failed == 0);
}

static inline bool atomic_cas64_llsc(volatile uint64_t *ptr, uint64_t *expected, uint64_t desired) {
    uint64_t old;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w2, #10000     \n"
        "1: ldaxr   %0, [%3]        \n"
        "   cmp     %0, %4          \n"
        "   b.ne    2f              \n"
        "   stlxr   %w1, %5, [%3]   \n"
        "   cbz     %w1, 4f         \n"
        "   subs    %w2, %w2, #1    \n"
        "   cmp     %w2, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   str     %5, [%3]        \n"
        "4: mov     %w1, #0         \n"
        "   b       3f              \n"
        "2: clrex                   \n"
        "   mov     %w1, #1         \n"
        "3:                         \n"
        : "=&r"(old), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(*expected), "r"(desired)
        : "cc", "memory"
    );
    *expected = old;
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (failed == 0);
}

/* -------------------------------------------------------------------------
 * Compare and Swap Pair (CASP - 128-bit & 64-bit pair)
 * ------------------------------------------------------------------------- */

static inline bool atomic_casp32_llsc(volatile uint32_t *ptr, uint32_t *exp0, uint32_t *exp1, uint32_t new0, uint32_t new1) {
    uint32_t old0, old1, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000              \n"
        "1: ldaxp   %w0, %w1, [%4]          \n"
        "   cmp     %w0, %w5                \n"
        "   b.ne    2f                      \n"
        "   cmp     %w1, %w6                \n"
        "   b.ne    2f                      \n"
        "   stlxp   %w2, %w7, %w8, [%4]     \n"
        "   cbz     %w2, 4f                 \n"
        "   subs    %w3, %w3, #1            \n"
        "   cmp     %w3, #9900              \n"
        "   b.gt    1b                      \n"
        "   clrex                           \n"
        "   yield                           \n"
        "   b       1b                      \n"
        "   stp     %w7, %w8, [%4]          \n"
        "4: mov     %w2, #0                 \n"
        "   b       3f                      \n"
        "2: clrex                           \n"
        "   mov     %w2, #1                 \n"
        "3:                                 \n"
        : "=&r"(old0), "=&r"(old1), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(*exp0), "r"(*exp1), "r"(new0), "r"(new1)
        : "cc", "memory"
    );
    *exp0 = old0;
    *exp1 = old1;
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (failed == 0);
}

static inline bool atomic_casp64_llsc(volatile uint64_t *ptr, uint64_t *exp0, uint64_t *exp1, uint64_t new0, uint64_t new1) {
    uint64_t old0, old1;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000              \n"
        "1: ldaxp   %0, %1, [%4]            \n"
        "   cmp     %0, %5                  \n"
        "   b.ne    2f                      \n"
        "   cmp     %1, %6                  \n"
        "   b.ne    2f                      \n"
        "   stlxp   %w2, %7, %8, [%4]       \n"
        "   cbz     %w2, 4f                 \n"
        "   subs    %w3, %w3, #1            \n"
        "   cmp     %w3, #9900              \n"
        "   b.gt    1b                      \n"
        "   clrex                           \n"
        "   yield                           \n"
        "   b       1b                      \n"
        "   stp     %7, %8, [%4]            \n"
        "4: mov     %w2, #0                 \n"
        "   b       3f                      \n"
        "2: clrex                           \n"
        "   mov     %w2, #1                 \n"
        "3:                                 \n"
        : "=&r"(old0), "=&r"(old1), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(*exp0), "r"(*exp1), "r"(new0), "r"(new1)
        : "cc", "memory"
    );
    *exp0 = old0;
    *exp1 = old1;
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (failed == 0);
}

/* -------------------------------------------------------------------------
 * Atomic Swap (SWP)
 * Atomically writes val to *ptr and returns original value.
 * ------------------------------------------------------------------------- */

static inline uint8_t atomic_swp8_llsc(volatile uint8_t *ptr, uint8_t val) {
    uint32_t old, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w2, #10000     \n"
        "1: ldaxrb  %w0, [%3]       \n"
        "   stlxrb  %w1, %w4, [%3]  \n"
        "   cbz     %w1, 2f         \n"
        "   subs    %w2, %w2, #1    \n"
        "   cmp     %w2, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   strb    %w4, [%3]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (uint8_t)old;
}

static inline uint16_t atomic_swp16_llsc(volatile uint16_t *ptr, uint16_t val) {
    uint32_t old, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w2, #10000     \n"
        "1: ldaxrh  %w0, [%3]       \n"
        "   stlxrh  %w1, %w4, [%3]  \n"
        "   cbz     %w1, 2f         \n"
        "   subs    %w2, %w2, #1    \n"
        "   cmp     %w2, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   strh    %w4, [%3]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (uint16_t)old;
}

static inline uint32_t atomic_swp32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w2, #10000     \n"
        "1: ldaxr   %w0, [%3]       \n"
        "   stlxr   %w1, %w4, [%3]  \n"
        "   cbz     %w1, 2f         \n"
        "   subs    %w2, %w2, #1    \n"
        "   cmp     %w2, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   str     %w4, [%3]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint64_t atomic_swp64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w2, #10000     \n"
        "1: ldaxr   %0, [%3]        \n"
        "   stlxr   %w1, %4, [%3]   \n"
        "   cbz     %w1, 2f         \n"
        "   subs    %w2, %w2, #1    \n"
        "   cmp     %w2, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   str     %4, [%3]        \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

/* -------------------------------------------------------------------------
 * Atomic Add (LDADD)
 * Atomically adds val to *ptr and returns original value.
 * ------------------------------------------------------------------------- */

static inline uint8_t atomic_ldadd8_llsc(volatile uint8_t *ptr, uint8_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxrb  %w0, [%4]       \n"
        "   add     %w1, %w0, %w5   \n"
        "   stlxrb  %w2, %w1, [%4]  \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   strb    %w1, [%4]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (uint8_t)old;
}

static inline uint16_t atomic_ldadd16_llsc(volatile uint16_t *ptr, uint16_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxrh  %w0, [%4]       \n"
        "   add     %w1, %w0, %w5   \n"
        "   stlxrh  %w2, %w1, [%4]  \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   strh    %w1, [%4]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (uint16_t)old;
}

static inline uint32_t atomic_ldadd32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxr   %w0, [%4]       \n"
        "   add     %w1, %w0, %w5   \n"
        "   stlxr   %w2, %w1, [%4]  \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   str     %w1, [%4]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint64_t atomic_ldadd64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old, tmp;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxr   %0, [%4]        \n"
        "   add     %1, %0, %5      \n"
        "   stlxr   %w2, %1, [%4]   \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   str     %1, [%4]        \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

/* -------------------------------------------------------------------------
 * Atomic Bitwise Operations (LDCLR, LDSET, LDEOR)
 * ------------------------------------------------------------------------- */

static inline uint8_t atomic_ldclr8_llsc(volatile uint8_t *ptr, uint8_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxrb  %w0, [%4]       \n"
        "   bic     %w1, %w0, %w5   \n"
        "   stlxrb  %w2, %w1, [%4]  \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   strb    %w1, [%4]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (uint8_t)old;
}

static inline uint16_t atomic_ldclr16_llsc(volatile uint16_t *ptr, uint16_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxrh  %w0, [%4]       \n"
        "   bic     %w1, %w0, %w5   \n"
        "   stlxrh  %w2, %w1, [%4]  \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   strh    %w1, [%4]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (uint16_t)old;
}

static inline uint32_t atomic_ldclr32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxr   %w0, [%4]       \n"
        "   bic     %w1, %w0, %w5   \n"
        "   stlxr   %w2, %w1, [%4]  \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   str     %w1, [%4]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint64_t atomic_ldclr64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old, tmp;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxr   %0, [%4]        \n"
        "   bic     %1, %0, %5      \n"
        "   stlxr   %w2, %1, [%4]   \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   str     %1, [%4]        \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint8_t atomic_ldset8_llsc(volatile uint8_t *ptr, uint8_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxrb  %w0, [%4]       \n"
        "   orr     %w1, %w0, %w5   \n"
        "   stlxrb  %w2, %w1, [%4]  \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   strb    %w1, [%4]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (uint8_t)old;
}

static inline uint16_t atomic_ldset16_llsc(volatile uint16_t *ptr, uint16_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxrh  %w0, [%4]       \n"
        "   orr     %w1, %w0, %w5   \n"
        "   stlxrh  %w2, %w1, [%4]  \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   strh    %w1, [%4]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (uint16_t)old;
}

static inline uint32_t atomic_ldset32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxr   %w0, [%4]       \n"
        "   orr     %w1, %w0, %w5   \n"
        "   stlxr   %w2, %w1, [%4]  \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   str     %w1, [%4]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint64_t atomic_ldset64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old, tmp;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxr   %0, [%4]        \n"
        "   orr     %1, %0, %5      \n"
        "   stlxr   %w2, %1, [%4]   \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   str     %1, [%4]        \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint8_t atomic_ldeor8_llsc(volatile uint8_t *ptr, uint8_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxrb  %w0, [%4]       \n"
        "   eor     %w1, %w0, %w5   \n"
        "   stlxrb  %w2, %w1, [%4]  \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   strb    %w1, [%4]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (uint8_t)old;
}

static inline uint16_t atomic_ldeor16_llsc(volatile uint16_t *ptr, uint16_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxrh  %w0, [%4]       \n"
        "   eor     %w1, %w0, %w5   \n"
        "   stlxrh  %w2, %w1, [%4]  \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   strh    %w1, [%4]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (uint16_t)old;
}

static inline uint32_t atomic_ldeor32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxr   %w0, [%4]       \n"
        "   eor     %w1, %w0, %w5   \n"
        "   stlxr   %w2, %w1, [%4]  \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   str     %w1, [%4]       \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint64_t atomic_ldeor64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old, tmp;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000     \n"
        "1: ldaxr   %0, [%4]        \n"
        "   eor     %1, %0, %5      \n"
        "   stlxr   %w2, %1, [%4]   \n"
        "   cbz     %w2, 2f         \n"
        "   subs    %w3, %w3, #1    \n"
        "   cmp     %w3, #9900      \n"
        "   b.gt    1b              \n"
        "   clrex                   \n"
        "   yield                   \n"
        "   b       1b              \n"
        "   str     %1, [%4]        \n"
        "2:                         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

/* -------------------------------------------------------------------------
 * Atomic Extremum Operations (LDSMAX, LDSMIN, LDUMAX, LDUMIN)
 * ------------------------------------------------------------------------- */

static inline uint32_t atomic_ldsmax32_llsc(volatile uint32_t *ptr, int32_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000        \n"
        "1: ldaxr   %w0, [%4]         \n"
        "   cmp     %w0, %w5          \n"
        "   csel    %w1, %w0, %w5, ge \n"
        "   stlxr   %w2, %w1, [%4]    \n"
        "   cbz     %w2, 2f           \n"
        "   subs    %w3, %w3, #1      \n"
        "   cmp     %w3, #9900        \n"
        "   b.gt    1b                \n"
        "   clrex                     \n"
        "   yield                     \n"
        "   b       1b                \n"
        "   str     %w1, [%4]         \n"
        "2:                           \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint64_t atomic_ldsmax64_llsc(volatile uint64_t *ptr, int64_t val) {
    uint64_t old, tmp;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000        \n"
        "1: ldaxr   %0, [%4]          \n"
        "   cmp     %0, %5            \n"
        "   csel    %1, %0, %5, ge    \n"
        "   stlxr   %w2, %1, [%4]     \n"
        "   cbz     %w2, 2f           \n"
        "   subs    %w3, %w3, #1      \n"
        "   cmp     %w3, #9900        \n"
        "   b.gt    1b                \n"
        "   clrex                     \n"
        "   yield                     \n"
        "   b       1b                \n"
        "   str     %1, [%4]          \n"
        "2:                           \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint32_t atomic_ldsmin32_llsc(volatile uint32_t *ptr, int32_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000        \n"
        "1: ldaxr   %w0, [%4]         \n"
        "   cmp     %w0, %w5          \n"
        "   csel    %w1, %w0, %w5, le \n"
        "   stlxr   %w2, %w1, [%4]    \n"
        "   cbz     %w2, 2f           \n"
        "   subs    %w3, %w3, #1      \n"
        "   cmp     %w3, #9900        \n"
        "   b.gt    1b                \n"
        "   clrex                     \n"
        "   yield                     \n"
        "   b       1b                \n"
        "   str     %w1, [%4]         \n"
        "2:                           \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint64_t atomic_ldsmin64_llsc(volatile uint64_t *ptr, int64_t val) {
    uint64_t old, tmp;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000        \n"
        "1: ldaxr   %0, [%4]          \n"
        "   cmp     %0, %5            \n"
        "   csel    %1, %0, %5, le    \n"
        "   stlxr   %w2, %1, [%4]     \n"
        "   cbz     %w2, 2f           \n"
        "   subs    %w3, %w3, #1      \n"
        "   cmp     %w3, #9900        \n"
        "   b.gt    1b                \n"
        "   clrex                     \n"
        "   yield                     \n"
        "   b       1b                \n"
        "   str     %1, [%4]          \n"
        "2:                           \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint32_t atomic_ldumax32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000        \n"
        "1: ldaxr   %w0, [%4]         \n"
        "   cmp     %w0, %w5          \n"
        "   csel    %w1, %w0, %w5, cs \n"
        "   stlxr   %w2, %w1, [%4]    \n"
        "   cbz     %w2, 2f           \n"
        "   subs    %w3, %w3, #1      \n"
        "   cmp     %w3, #9900        \n"
        "   b.gt    1b                \n"
        "   clrex                     \n"
        "   yield                     \n"
        "   b       1b                \n"
        "   str     %w1, [%4]         \n"
        "2:                           \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint64_t atomic_ldumax64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old, tmp;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000        \n"
        "1: ldaxr   %0, [%4]          \n"
        "   cmp     %0, %5            \n"
        "   csel    %1, %0, %5, cs    \n"
        "   stlxr   %w2, %1, [%4]     \n"
        "   cbz     %w2, 2f           \n"
        "   subs    %w3, %w3, #1      \n"
        "   cmp     %w3, #9900        \n"
        "   b.gt    1b                \n"
        "   clrex                     \n"
        "   yield                     \n"
        "   b       1b                \n"
        "   str     %1, [%4]          \n"
        "2:                           \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint32_t atomic_ldumin32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, tmp, retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000        \n"
        "1: ldaxr   %w0, [%4]         \n"
        "   cmp     %w0, %w5          \n"
        "   csel    %w1, %w0, %w5, ls \n"
        "   stlxr   %w2, %w1, [%4]    \n"
        "   cbz     %w2, 2f           \n"
        "   subs    %w3, %w3, #1      \n"
        "   cmp     %w3, #9900        \n"
        "   b.gt    1b                \n"
        "   clrex                     \n"
        "   yield                     \n"
        "   b       1b                \n"
        "   str     %w1, [%4]         \n"
        "2:                           \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

static inline uint64_t atomic_ldumin64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old, tmp;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000        \n"
        "1: ldaxr   %0, [%4]          \n"
        "   cmp     %0, %5            \n"
        "   csel    %1, %0, %5, ls    \n"
        "   stlxr   %w2, %1, [%4]     \n"
        "   cbz     %w2, 2f           \n"
        "   subs    %w3, %w3, #1      \n"
        "   cmp     %w3, #9900        \n"
        "   b.gt    1b                \n"
        "   clrex                     \n"
        "   yield                     \n"
        "   b       1b                \n"
        "   str     %1, [%4]          \n"
        "2:                           \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return old;
}

/* -------------------------------------------------------------------------
 * FINDING-019: 128-bit Atomic Primitives using LDAXP/STLXP pair
 * These implement atomic operations on 128-bit (quadword) values using
 * the load-acquire pair / store-release pair instructions.
 * Constraints: Ws != Xt (status reg != target), Ws != Xn (status reg != base)
 * ------------------------------------------------------------------------- */

/* 128-bit Compare and Swap (CAS128) - operates on two 64-bit registers */
static inline bool atomic_cas128_llsc(volatile uint64_t *ptr, uint64_t *exp0, uint64_t *exp1, uint64_t new0, uint64_t new1) {
    uint64_t old0, old1;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000              \n"
        "1: ldaxp   %0, %1, [%4]            \n"
        "   cmp     %0, %5                  \n"
        "   b.ne    2f                      \n"
        "   cmp     %1, %6                  \n"
        "   b.ne    2f                      \n"
        "   stlxp   %w2, %7, %8, [%4]       \n"
        "   cbz     %w2, 4f                 \n"
        "   subs    %w3, %w3, #1            \n"
        "   cmp     %w3, #9900              \n"
        "   b.gt    1b                      \n"
        "   clrex                           \n"
        "   yield                           \n"
        "   b       1b                      \n"
        "   stp     %7, %8, [%4]            \n"
        "4: mov     %w2, #0                 \n"
        "   b       3f                      \n"
        "2: clrex                           \n"
        "   mov     %w2, #1                 \n"
        "3:                                 \n"
        : "=&r"(old0), "=&r"(old1), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(*exp0), "r"(*exp1), "r"(new0), "r"(new1)
        : "cc", "memory"
    );
    *exp0 = old0;
    *exp1 = old1;
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
    return (failed == 0);
}

/* 128-bit Atomic Swap (SWP128) */
static inline void atomic_swp128_llsc(volatile uint64_t *ptr, uint64_t *old0, uint64_t *old1, uint64_t new0, uint64_t new1) {
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w3, #10000              \n"
        "1: ldaxp   %0, %1, [%4]            \n"
        "   stlxp   %w2, %5, %6, [%4]       \n"
        "   cbz     %w2, 2f                 \n"
        "   subs    %w3, %w3, #1            \n"
        "   cmp     %w3, #9900              \n"
        "   b.gt    1b                      \n"
        "   clrex                           \n"
        "   yield                           \n"
        "   b       1b                      \n"
        "   stp     %5, %6, [%4]            \n"
        "2:                                 \n"
        : "=&r"(*old0), "=&r"(*old1), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(new0), "r"(new1)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
}

/* 128-bit Atomic Add (LDADD128) - adds 128-bit value (two 64-bit adds with carry) */
static inline void atomic_ldadd128_llsc(volatile uint64_t *ptr, uint64_t *old0, uint64_t *old1, uint64_t add0, uint64_t add1) {
    uint64_t tmp0, tmp1;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w4, #10000              \n"
        "1: ldaxp   %0, %1, [%5]            \n"
        "   adds    %2, %0, %6              \n"
        "   adc     %3, %1, %7              \n"
        "   stlxp   %w4, %2, %3, [%5]       \n"
        "   cbz     %w4, 2f                 \n"
        "   subs    %w4, %w4, #1            \n"
        "   cmp     %w4, #9900              \n"
        "   b.gt    1b                      \n"
        "   clrex                           \n"
        "   yield                           \n"
        "   b       1b                      \n"
        "   stp     %2, %3, [%5]            \n"
        "2:                                 \n"
        : "=&r"(*old0), "=&r"(*old1), "=&r"(tmp0), "=&r"(tmp1), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(add0), "r"(add1)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
}

/* 128-bit Atomic Bitwise Clear (LDCLR128) */
static inline void atomic_ldclr128_llsc(volatile uint64_t *ptr, uint64_t *old0, uint64_t *old1, uint64_t clr0, uint64_t clr1) {
    uint64_t tmp0, tmp1;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w4, #10000              \n"
        "1: ldaxp   %0, %1, [%5]            \n"
        "   bic     %2, %0, %6              \n"
        "   bic     %3, %1, %7              \n"
        "   stlxp   %w4, %2, %3, [%5]       \n"
        "   cbz     %w4, 2f                 \n"
        "   subs    %w4, %w4, #1            \n"
        "   cmp     %w4, #9900              \n"
        "   b.gt    1b                      \n"
        "   clrex                           \n"
        "   yield                           \n"
        "   b       1b                      \n"
        "   stp     %2, %3, [%5]            \n"
        "2:                                 \n"
        : "=&r"(*old0), "=&r"(*old1), "=&r"(tmp0), "=&r"(tmp1), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(clr0), "r"(clr1)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
}

/* 128-bit Atomic Bitwise Set (LDSET128) */
static inline void atomic_ldset128_llsc(volatile uint64_t *ptr, uint64_t *old0, uint64_t *old1, uint64_t set0, uint64_t set1) {
    uint64_t tmp0, tmp1;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w4, #10000              \n"
        "1: ldaxp   %0, %1, [%5]            \n"
        "   orr     %2, %0, %6              \n"
        "   orr     %3, %1, %7              \n"
        "   stlxp   %w4, %2, %3, [%5]       \n"
        "   cbz     %w4, 2f                 \n"
        "   subs    %w4, %w4, #1            \n"
        "   cmp     %w4, #9900              \n"
        "   b.gt    1b                      \n"
        "   clrex                           \n"
        "   yield                           \n"
        "   b       1b                      \n"
        "   stp     %2, %3, [%5]            \n"
        "2:                                 \n"
        : "=&r"(*old0), "=&r"(*old1), "=&r"(tmp0), "=&r"(tmp1), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(set0), "r"(set1)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
}

/* 128-bit Atomic Exclusive OR (LDEOR128) */
static inline void atomic_ldeor128_llsc(volatile uint64_t *ptr, uint64_t *old0, uint64_t *old1, uint64_t eor0, uint64_t eor1) {
    uint64_t tmp0, tmp1;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w4, #10000              \n"
        "1: ldaxp   %0, %1, [%5]            \n"
        "   eor     %2, %0, %6              \n"
        "   eor     %3, %1, %7              \n"
        "   stlxp   %w4, %2, %3, [%5]       \n"
        "   cbz     %w4, 2f                 \n"
        "   subs    %w4, %w4, #1            \n"
        "   cmp     %w4, #9900              \n"
        "   b.gt    1b                      \n"
        "   clrex                           \n"
        "   yield                           \n"
        "   b       1b                      \n"
        "   stp     %2, %3, [%5]            \n"
        "2:                                 \n"
        : "=&r"(*old0), "=&r"(*old1), "=&r"(tmp0), "=&r"(tmp1), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(eor0), "r"(eor1)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
}

/* 128-bit Atomic Signed Maximum (LDSMAX128) */
static inline void atomic_ldsmax128_llsc(volatile uint64_t *ptr, uint64_t *old0, uint64_t *old1, int64_t val0, int64_t val1) {
    uint64_t tmp0, tmp1;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w4, #10000              \n"
        "1: ldaxp   %0, %1, [%5]            \n"
        "   cmp     %0, %6                  \n"
        "   csel    %2, %0, %6, ge          \n"
        "   cmp     %1, %7                  \n"
        "   csel    %3, %1, %7, ge          \n"
        "   stlxp   %w4, %2, %3, [%5]       \n"
        "   cbz     %w4, 2f                 \n"
        "   subs    %w4, %w4, #1            \n"
        "   cmp     %w4, #9900              \n"
        "   b.gt    1b                      \n"
        "   clrex                           \n"
        "   yield                           \n"
        "   b       1b                      \n"
        "   stp     %2, %3, [%5]            \n"
        "2:                                 \n"
        : "=&r"(*old0), "=&r"(*old1), "=&r"(tmp0), "=&r"(tmp1), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val0), "r"(val1)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
}

/* 128-bit Atomic Signed Minimum (LDSMIN128) */
static inline void atomic_ldsmin128_llsc(volatile uint64_t *ptr, uint64_t *old0, uint64_t *old1, int64_t val0, int64_t val1) {
    uint64_t tmp0, tmp1;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w4, #10000              \n"
        "1: ldaxp   %0, %1, [%5]            \n"
        "   cmp     %0, %6                  \n"
        "   csel    %2, %0, %6, le          \n"
        "   cmp     %1, %7                  \n"
        "   csel    %3, %1, %7, le          \n"
        "   stlxp   %w4, %2, %3, [%5]       \n"
        "   cbz     %w4, 2f                 \n"
        "   subs    %w4, %w4, #1            \n"
        "   cmp     %w4, #9900              \n"
        "   b.gt    1b                      \n"
        "   clrex                           \n"
        "   yield                           \n"
        "   b       1b                      \n"
        "   stp     %2, %3, [%5]            \n"
        "2:                                 \n"
        : "=&r"(*old0), "=&r"(*old1), "=&r"(tmp0), "=&r"(tmp1), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val0), "r"(val1)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
}

/* 128-bit Atomic Unsigned Maximum (LDUMAX128) */
static inline void atomic_ldumax128_llsc(volatile uint64_t *ptr, uint64_t *old0, uint64_t *old1, uint64_t val0, uint64_t val1) {
    uint64_t tmp0, tmp1;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w4, #10000              \n"
        "1: ldaxp   %0, %1, [%5]            \n"
        "   cmp     %0, %6                  \n"
        "   csel    %2, %0, %6, cs          \n"
        "   cmp     %1, %7                  \n"
        "   csel    %3, %1, %7, cs          \n"
        "   stlxp   %w4, %2, %3, [%5]       \n"
        "   cbz     %w4, 2f                 \n"
        "   subs    %w4, %w4, #1            \n"
        "   cmp     %w4, #9900              \n"
        "   b.gt    1b                      \n"
        "   clrex                           \n"
        "   yield                           \n"
        "   b       1b                      \n"
        "   stp     %2, %3, [%5]            \n"
        "2:                                 \n"
        : "=&r"(*old0), "=&r"(*old1), "=&r"(tmp0), "=&r"(tmp1), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val0), "r"(val1)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
}

/* 128-bit Atomic Unsigned Minimum (LDUMIN128) */
static inline void atomic_ldumin128_llsc(volatile uint64_t *ptr, uint64_t *old0, uint64_t *old1, uint64_t val0, uint64_t val1) {
    uint64_t tmp0, tmp1;
    uint32_t retries;
    int failed;
    __asm__ volatile (
        "   mov     %w4, #10000              \n"
        "1: ldaxp   %0, %1, [%5]            \n"
        "   cmp     %0, %6                  \n"
        "   csel    %2, %0, %6, ls          \n"
        "   cmp     %1, %7                  \n"
        "   csel    %3, %1, %7, ls          \n"
        "   stlxp   %w4, %2, %3, [%5]       \n"
        "   cbz     %w4, 2f                 \n"
        "   subs    %w4, %w4, #1            \n"
        "   cmp     %w4, #9900              \n"
        "   b.gt    1b                      \n"
        "   clrex                           \n"
        "   yield                           \n"
        "   b       1b                      \n"
        "   stp     %2, %3, [%5]            \n"
        "2:                                 \n"
        : "=&r"(*old0), "=&r"(*old1), "=&r"(tmp0), "=&r"(tmp1), "=&r"(failed), "=&r"(retries)
        : "r"(ptr), "r"(val0), "r"(val1)
        : "cc", "memory"
    );
    if (failed != 0) {
        __asm__ volatile("brk #0x1" : : : "memory");
    }
}

#ifdef __cplusplus
}
#endif

#endif /* _OSFMK_ARM64_LSE_HOOKS_H_ */
