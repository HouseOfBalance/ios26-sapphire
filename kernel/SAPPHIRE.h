/*
 * Copyright (c) 2024 HakiOS Project. All rights reserved.
 */

#ifndef _PEXPERT_ARM_SAPPHIRE_H
#define _PEXPERT_ARM_SAPPHIRE_H

#define NO_MONITOR 1
#define NO_ECORE 0 /* Sapphire has both Cortex-A73 (Gold) and Cortex-A53 (Silver) */

#define SAPPHIRE
#define SAPPHIRE_BRINGUP
#define ARM_ARCH_TIMER
#define GENERIC_GIC_V3_IRQ                 1 /* interrupts via GICv3 Group 1, see osfmk/arm64/gic_v3_irq.c */
#define GENERIC_PSCI_SMP                  1 /* more cores are started through PSCI, see osfmk/arm64/psci_smp.c */

/*
 * The cores (Cortex-A73/A53) are ARMv8.0-A baseline:
 * - No ARMv8.1 FEAT_PAN (no hardware PAN support)
 * - No ARMv8.3 FEAT_PAC (no hardware pointer authentication)
 * - No ARMv8.1 FEAT_LSE (no hardware LSE atomics)
 * These features are emulated via trap-and-self-patch (PAC) and branch islands (LSE)
 */
#define __ARM_PAN_AVAILABLE__     0

#define __ARM_ARCH__              8
#define __ARM_VMSA__              8
#define __ARM_VFP__               4
#define __ARM_COHERENT_CACHE__    1
#define __ARM_DEBUG__             7
#define __ARM64_PMAP_SUBPAGE_L1__ 1

/*
 * Pointer authentication. Cortex-A73/A53 lack hardware PAC:
 * - HAS_STD_PAC: PAC instructions are standard ARMv8.3+, emulated via trap-and-self-patch
 * - HAS_APPLE_PAC: arm64e ABI with signed user thread state, per-process keys, signed credentials
 */
#if defined(__arm64e__)
#define HAS_APPLE_PAC             1
#define HAS_STD_PAC               1
#endif

/*
 * MPIDR core-type field for non-Apple boards.
 * Cortex-A73 (Gold/Performance) = type 1
 * Cortex-A53 (Silver/Efficiency) = type 0
 */
#define MPIDR_CORETYPE_SHIFT  (16)
#define MPIDR_CORETYPE_WIDTH  (3)
#define MPIDR_CORETYPE_MASK   ((1ULL << MPIDR_CORETYPE_WIDTH) - 1)
#define MPIDR_CORETYPE_ACC_E  (0ULL)  /* Cortex-A53 */
#define MPIDR_CORETYPE_ACC_P  (1ULL)  /* Cortex-A73 */
#define MPIDR_CORETYPE_ACC_M  (4ULL)

#ifndef ASSEMBLER

#define VMAPPLE_UART                    /* PL011 serial UART */

#define SAPPHIRE_GICD_BASE_PA          0x0A000000
#define SAPPHIRE_GICR_BASE_PA          0x0A0A0000
#define SAPPHIRE_GICD_BASE_VA          0xFFFFFFF00A000000ULL
#define SAPPHIRE_GICR_BASE_VA          0xFFFFFFF00A0A0000ULL

#define SAPPHIRE_UART_BASE_PA          0x0A880000
#define SAPPHIRE_UART_BASE_VA          0xFFFFFFF00A880000ULL

#define SAPPHIRE_PUT32(addr, value) do { *((volatile uint32_t *) addr) = value; } while(0)
#define SAPPHIRE_GET32(addr) *((volatile uint32_t *) addr)

/*
 * Modem pool: 16MB below 0xF0000000 for TrustZone/modem (from loader)
 * GPU pool: allocated by loader
 */
#define SAPPHIRE_MODEM_POOL_BASE       0xEFF00000
#define SAPPHIRE_MODEM_POOL_SIZE       0x00100000 /* 16MB */

/*
 * RPMh power management for SM6225
 */
#define SAPPHIRE_RPMH_BASE_PA          0x1A200000
#define SAPPHIRE_RPMH_BASE_VA          0xFFFFFFF01A200000ULL

/*
 * UFS host controller
 */
#define SAPPHIRE_UFS_BASE_PA           0x1D840000
#define SAPPHIRE_UFS_BASE_VA           0xFFFFFFF01D840000ULL

/*
 * GPU (Adreno) registers
 */
#define SAPPHIRE_GPU_BASE_PA           0x06000000
#define SAPPHIRE_GPU_BASE_VA           0xFFFFFFF006000000ULL

/*
 * Framebuffer
 */
#define SAPPHIRE_FB_BASE_PA            0x5C000000
#define SAPPHIRE_FB_BASE_VA            0xFFFFFFF004000000ULL

/*
 * SPMI (PMIC)
 */
#define SAPPHIRE_SPMI_BASE_PA          0x1B800000
#define SAPPHIRE_SPMI_BASE_VA          0xFFFFFFF01B800000ULL

#endif /* ! ASSEMBLER */

#endif /* ! _PEXPERT_ARM_SAPPHIRE_H */