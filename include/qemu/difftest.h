/*
 * Minimal QEMU difftest entry points.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_DIFFTEST_H
#define QEMU_DIFFTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
# define QEMU_DIFFTEST_EXPORT __declspec(dllexport)
#else
# define QEMU_DIFFTEST_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DIFFTEST_GPR_COUNT = 32,
    DIFFTEST_VREG_COUNT = 32,
    DIFFTEST_VREG_U64_COUNT = 2,
};

/*
 * Stable pointers into the reference CPU state.  xregs[31] is SP and each
 * vregs entry points to a 128-bit register stored as two uint64_t lanes.
 * The pointers remain valid for the lifetime of the initialized CPU and may
 * only be accessed while difftest_step() is not executing.
 */
typedef struct DifftestRegPtrs {
    uint64_t *xregs[DIFFTEST_GPR_COUNT];
    uint64_t *pc;
    uint64_t *vregs[DIFFTEST_VREG_COUNT];
} DifftestRegPtrs;

typedef enum DifftestMiscRegId {
    DIFFTEST_MISC_PSTATE,
    DIFFTEST_MISC_FPSR,
    DIFFTEST_MISC_FPCR,
    DIFFTEST_MISC_SPSR_EL1,
    DIFFTEST_MISC_SPSR_EL2,
    DIFFTEST_MISC_SPSR_EL3,
    DIFFTEST_MISC_ELR_EL1,
    DIFFTEST_MISC_ELR_EL2,
    DIFFTEST_MISC_ELR_EL3,
    DIFFTEST_MISC_SP_EL0,
    DIFFTEST_MISC_SP_EL1,
    DIFFTEST_MISC_SP_EL2,
    DIFFTEST_MISC_SP_EL3,

    /*
     * This enum is a shared-library ABI, not an internal QEMU-only enum.
     * Append new IDs so every previously published numeric value remains
     * unchanged, and keep this order identical to gem5's DifftestMiscReg.
     *
     * Synchronize the complete AArch64 thread-ID register family.  These
     * registers remain architecturally live across ordinary instructions and
     * are also used by exception-vector code.  A DUT that restores or changes
     * one of them must be able to transfer the value to the reference before
     * instruction-by-instruction comparison resumes.
     */
    DIFFTEST_MISC_TPIDR_EL0,
    DIFFTEST_MISC_TPIDRRO_EL0,
    DIFFTEST_MISC_TPIDR_EL1,
    DIFFTEST_MISC_TPIDR_EL2,
    DIFFTEST_MISC_TPIDR_EL3,

    /*
     * gem5 handles ARM exception entry locally and then resynchronizes this
     * reference instead of executing QEMU's own exception entry.  FAR and ESR
     * banks for EL1 through EL3 are therefore live architectural state for the
     * next instruction
     * in an exception handler: without these slots, a following MRS reads the
     * fault data of an earlier QEMU exception rather than gem5's current one.
     * All target ELs are transferred because exception routing and later ERET
     * can expose a bank that was inactive when synchronization occurred.
     *
     * Append, rather than insert, these IDs.  This enum is a shared-library
     * ABI and its numeric ordering must remain identical to gem5's
     * DifftestMiscReg.
     */
    DIFFTEST_MISC_FAR_EL1,
    DIFFTEST_MISC_ESR_EL1,
    DIFFTEST_MISC_FAR_EL2,
    DIFFTEST_MISC_ESR_EL2,
    DIFFTEST_MISC_FAR_EL3,
    DIFFTEST_MISC_ESR_EL3,

    /*
     * CTR_EL0 is an implementation-defined, read-only description of cache
     * behavior.  gem5 and QEMU may legitimately choose different reset
     * values, so difftest synchronizes the DUT value into the reference CPU
     * model before executing guest code.  Append this ID to preserve every
     * existing shared-library ABI value, and keep its position identical to
     * gem5's DifftestMiscReg enum.
     */
    DIFFTEST_MISC_CTR_EL0,
} DifftestMiscRegId;

typedef enum DifftestDirection {
    DIFFTEST_TO_DUT,
    DIFFTEST_TO_REF,
} DifftestDirection;

/* Initialize one paused mini-virt AArch64 reference CPU. */
QEMU_DIFFTEST_EXPORT int difftest_init(const char *platform);

/* Execute n guest instructions on the initialized reference CPU. */
QEMU_DIFFTEST_EXPORT int difftest_step(uint64_t n);

/*
 * Copy guest physical memory between the DUT buffer and the reference.
 * DIFFTEST_TO_REF writes the DUT buffer into QEMU; DIFFTEST_TO_DUT reads it.
 */
QEMU_DIFFTEST_EXPORT int difftest_memcpy(uint64_t addr, void *buf, size_t n,
                                        bool direction);

/*
 * Replace mini-virt RAM with a caller-owned COW mapping.  This must be called
 * before the first difftest_step(); QEMU never frees the supplied mapping.
 */
QEMU_DIFFTEST_EXPORT int difftest_get_backed_memory(void *backed_mem,
                                                    size_t n);

/* Return the stable, directly readable and writable register pointer table. */
QEMU_DIFFTEST_EXPORT DifftestRegPtrs *difftest_getregptr(void);

/* Read or write architectural state that cannot expose a stable pointer. */
QEMU_DIFFTEST_EXPORT int difftest_getmisc(uint32_t regid, uint64_t *value);
QEMU_DIFFTEST_EXPORT int difftest_setmisc(uint32_t regid, uint64_t value);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_DIFFTEST_H */
