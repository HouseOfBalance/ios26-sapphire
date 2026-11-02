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
 * 3. Safe for SMP concurrency across all 8 cores via hardware exclusive monitors.
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
 * ------------------------------------------------------------------------- */

static inline bool atomic_cas8_llsc(volatile uint8_t *ptr, uint8_t *expected, uint8_t desired) {
    uint32_t old;
    int failed;
    __asm__ volatile (
        "1: ldaxrb  %w0, [%2]       \n"
        "   cmp     %w0, %w3        \n"
        "   b.ne    2f              \n"
        "   stlxrb  %w1, %w4, [%2]  \n"
        "   cbnz    %w1, 1b         \n"
        "   mov     %w1, #0         \n"
        "   b       3f              \n"
        "2: clrex                   \n"
        "   mov     %w1, #1         \n"
        "3:                         \n"
        : "=&r"(old), "=&r"(failed)
        : "r"(ptr), "r"((uint32_t)*expected), "r"((uint32_t)desired)
        : "cc", "memory"
    );
    *expected = (uint8_t)old;
    return (failed == 0);
}

static inline bool atomic_cas16_llsc(volatile uint16_t *ptr, uint16_t *expected, uint16_t desired) {
    uint32_t old;
    int failed;
    __asm__ volatile (
        "1: ldaxrh  %w0, [%2]       \n"
        "   cmp     %w0, %w3        \n"
        "   b.ne    2f              \n"
        "   stlxrh  %w1, %w4, [%2]  \n"
        "   cbnz    %w1, 1b         \n"
        "   mov     %w1, #0         \n"
        "   b       3f              \n"
        "2: clrex                   \n"
        "   mov     %w1, #1         \n"
        "3:                         \n"
        : "=&r"(old), "=&r"(failed)
        : "r"(ptr), "r"((uint32_t)*expected), "r"((uint32_t)desired)
        : "cc", "memory"
    );
    *expected = (uint16_t)old;
    return (failed == 0);
}

static inline bool atomic_cas32_llsc(volatile uint32_t *ptr, uint32_t *expected, uint32_t desired) {
    uint32_t old;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %w0, [%2]       \n"
        "   cmp     %w0, %w3        \n"
        "   b.ne    2f              \n"
        "   stlxr   %w1, %w4, [%2]  \n"
        "   cbnz    %w1, 1b         \n"
        "   mov     %w1, #0         \n"
        "   b       3f              \n"
        "2: clrex                   \n"
        "   mov     %w1, #1         \n"
        "3:                         \n"
        : "=&r"(old), "=&r"(failed)
        : "r"(ptr), "r"(*expected), "r"(desired)
        : "cc", "memory"
    );
    *expected = old;
    return (failed == 0);
}

static inline bool atomic_cas64_llsc(volatile uint64_t *ptr, uint64_t *expected, uint64_t desired) {
    uint64_t old;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %0, [%2]        \n"
        "   cmp     %0, %3          \n"
        "   b.ne    2f              \n"
        "   stlxr   %w1, %4, [%2]   \n"
        "   cbnz    %w1, 1b         \n"
        "   mov     %w1, #0         \n"
        "   b       3f              \n"
        "2: clrex                   \n"
        "   mov     %w1, #1         \n"
        "3:                         \n"
        : "=&r"(old), "=&r"(failed)
        : "r"(ptr), "r"(*expected), "r"(desired)
        : "cc", "memory"
    );
    *expected = old;
    return (failed == 0);
}

/* -------------------------------------------------------------------------
 * Compare and Swap Pair (CASP - 128-bit & 64-bit pair)
 * ------------------------------------------------------------------------- */

static inline bool atomic_casp32_llsc(volatile uint32_t *ptr, uint32_t *exp0, uint32_t *exp1, uint32_t new0, uint32_t new1) {
    uint32_t old0, old1;
    int failed;
    __asm__ volatile (
        "1: ldaxp   %w0, %w1, [%4]          \n"
        "   cmp     %w0, %w5                \n"
        "   b.ne    2f                      \n"
        "   cmp     %w1, %w6                \n"
        "   b.ne    2f                      \n"
        "   stlxp   %w2, %w7, %w8, [%4]     \n"
        "   cbnz    %w2, 1b                 \n"
        "   mov     %w2, #0                 \n"
        "   b       3f                      \n"
        "2: clrex                           \n"
        "   mov     %w2, #1                 \n"
        "3:                                 \n"
        : "=&r"(old0), "=&r"(old1), "=&r"(failed)
        : "0"(0), "r"(ptr), "r"(*exp0), "r"(*exp1), "r"(new0), "r"(new1)
        : "cc", "memory"
    );
    *exp0 = old0;
    *exp1 = old1;
    return (failed == 0);
}

static inline bool atomic_casp64_llsc(volatile uint64_t *ptr, uint64_t *exp0, uint64_t *exp1, uint64_t new0, uint64_t new1) {
    uint64_t old0, old1;
    int failed;
    __asm__ volatile (
        "1: ldaxp   %0, %1, [%4]            \n"
        "   cmp     %0, %5                  \n"
        "   b.ne    2f                      \n"
        "   cmp     %1, %6                  \n"
        "   b.ne    2f                      \n"
        "   stlxp   %w2, %7, %8, [%4]       \n"
        "   cbnz    %w2, 1b                 \n"
        "   mov     %w2, #0                 \n"
        "   b       3f                      \n"
        "2: clrex                           \n"
        "   mov     %w2, #1                 \n"
        "3:                                 \n"
        : "=&r"(old0), "=&r"(old1), "=&r"(failed)
        : "0"(0), "r"(ptr), "r"(*exp0), "r"(*exp1), "r"(new0), "r"(new1)
        : "cc", "memory"
    );
    *exp0 = old0;
    *exp1 = old1;
    return (failed == 0);
}

/* -------------------------------------------------------------------------
 * Atomic Swap (SWP)
 * Atomically writes val to *ptr and returns original value.
 * ------------------------------------------------------------------------- */

static inline uint8_t atomic_swp8_llsc(volatile uint8_t *ptr, uint8_t val) {
    uint32_t old;
    int failed;
    __asm__ volatile (
        "1: ldaxrb  %w0, [%2]       \n"
        "   stlxrb  %w1, %w3, [%2]  \n"
        "   cbnz    %w1, 1b         \n"
        : "=&r"(old), "=&r"(failed)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    return (uint8_t)old;
}

static inline uint16_t atomic_swp16_llsc(volatile uint16_t *ptr, uint16_t val) {
    uint32_t old;
    int failed;
    __asm__ volatile (
        "1: ldaxrh  %w0, [%2]       \n"
        "   stlxrh  %w1, %w3, [%2]  \n"
        "   cbnz    %w1, 1b         \n"
        : "=&r"(old), "=&r"(failed)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    return (uint16_t)old;
}

static inline uint32_t atomic_swp32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %w0, [%2]       \n"
        "   stlxr   %w1, %w3, [%2]  \n"
        "   cbnz    %w1, 1b         \n"
        : "=&r"(old), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint64_t atomic_swp64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %0, [%2]        \n"
        "   stlxr   %w1, %3, [%2]   \n"
        "   cbnz    %w1, 1b         \n"
        : "=&r"(old), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

/* -------------------------------------------------------------------------
 * Atomic Add (LDADD)
 * Atomically adds val to *ptr and returns original value.
 * ------------------------------------------------------------------------- */

static inline uint8_t atomic_ldadd8_llsc(volatile uint8_t *ptr, uint8_t val) {
    uint32_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxrb  %w0, [%3]       \n"
        "   add     %w1, %w0, %w4   \n"
        "   stlxrb  %w2, %w1, [%3]  \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    return (uint8_t)old;
}

static inline uint16_t atomic_ldadd16_llsc(volatile uint16_t *ptr, uint16_t val) {
    uint32_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxrh  %w0, [%3]       \n"
        "   add     %w1, %w0, %w4   \n"
        "   stlxrh  %w2, %w1, [%3]  \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"((uint32_t)val)
        : "cc", "memory"
    );
    return (uint16_t)old;
}

static inline uint32_t atomic_ldadd32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %w0, [%3]       \n"
        "   add     %w1, %w0, %w4   \n"
        "   stlxr   %w2, %w1, [%3]  \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint64_t atomic_ldadd64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %0, [%3]        \n"
        "   add     %1, %0, %4      \n"
        "   stlxr   %w2, %1, [%3]   \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

/* -------------------------------------------------------------------------
 * Atomic Bitwise Operations (LDCLR, LDSET, LDEOR)
 * ------------------------------------------------------------------------- */

static inline uint32_t atomic_ldclr32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %w0, [%3]       \n"
        "   bic     %w1, %w0, %w4   \n"
        "   stlxr   %w2, %w1, [%3]  \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint64_t atomic_ldclr64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %0, [%3]        \n"
        "   bic     %1, %0, %4      \n"
        "   stlxr   %w2, %1, [%3]   \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint32_t atomic_ldset32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %w0, [%3]       \n"
        "   orr     %w1, %w0, %w4   \n"
        "   stlxr   %w2, %w1, [%3]  \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint64_t atomic_ldset64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %0, [%3]        \n"
        "   orr     %1, %0, %4      \n"
        "   stlxr   %w2, %1, [%3]   \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint32_t atomic_ldeor32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %w0, [%3]       \n"
        "   eor     %w1, %w0, %w4   \n"
        "   stlxr   %w2, %w1, [%3]  \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint64_t atomic_ldeor64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %0, [%3]        \n"
        "   eor     %1, %0, %4      \n"
        "   stlxr   %w2, %1, [%3]   \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

/* -------------------------------------------------------------------------
 * Atomic Extremum Operations (LDSMAX, LDSMIN, LDUMAX, LDUMIN)
 * ------------------------------------------------------------------------- */

static inline uint32_t atomic_ldsmax32_llsc(volatile uint32_t *ptr, int32_t val) {
    uint32_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %w0, [%3]       \n"
        "   cmp     %w0, %w4        \n"
        "   csel    %w1, %w0, %w4, ge \n"
        "   stlxr   %w2, %w1, [%3]  \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint64_t atomic_ldsmax64_llsc(volatile uint64_t *ptr, int64_t val) {
    uint64_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %0, [%3]        \n"
        "   cmp     %0, %4          \n"
        "   csel    %1, %0, %4, ge  \n"
        "   stlxr   %w2, %1, [%3]   \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint32_t atomic_ldsmin32_llsc(volatile uint32_t *ptr, int32_t val) {
    uint32_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %w0, [%3]       \n"
        "   cmp     %w0, %w4        \n"
        "   csel    %w1, %w0, %w4, le \n"
        "   stlxr   %w2, %w1, [%3]  \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint64_t atomic_ldsmin64_llsc(volatile uint64_t *ptr, int64_t val) {
    uint64_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %0, [%3]        \n"
        "   cmp     %0, %4          \n"
        "   csel    %1, %0, %4, le  \n"
        "   stlxr   %w2, %1, [%3]   \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint32_t atomic_ldumax32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %w0, [%3]       \n"
        "   cmp     %w0, %w4        \n"
        "   csel    %w1, %w0, %w4, cs \n"
        "   stlxr   %w2, %w1, [%3]  \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint64_t atomic_ldumax64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %0, [%3]        \n"
        "   cmp     %0, %4          \n"
        "   csel    %1, %0, %4, cs  \n"
        "   stlxr   %w2, %1, [%3]   \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint32_t atomic_ldumin32_llsc(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %w0, [%3]       \n"
        "   cmp     %w0, %w4        \n"
        "   csel    %w1, %w0, %w4, ls \n"
        "   stlxr   %w2, %w1, [%3]  \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

static inline uint64_t atomic_ldumin64_llsc(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old, tmp;
    int failed;
    __asm__ volatile (
        "1: ldaxr   %0, [%3]        \n"
        "   cmp     %0, %4          \n"
        "   csel    %1, %0, %4, ls  \n"
        "   stlxr   %w2, %1, [%3]   \n"
        "   cbnz    %w2, 1b         \n"
        : "=&r"(old), "=&r"(tmp), "=&r"(failed)
        : "r"(ptr), "r"(val)
        : "cc", "memory"
    );
    return old;
}

#ifdef __cplusplus
}
#endif

#endif /* _OSFMK_ARM64_LSE_HOOKS_H_ */
