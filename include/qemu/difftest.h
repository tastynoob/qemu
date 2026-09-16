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
    /* Read-only extensions: guest MSR instructions maintain these fields. */
    DIFFTEST_MISC_TPIDR_EL0,
    DIFFTEST_MISC_TPIDRRO_EL0,
    DIFFTEST_MISC_TPIDR_EL1,
    DIFFTEST_MISC_TPIDR_EL2,
    DIFFTEST_MISC_TPIDR_EL3,
} DifftestMiscRegId;

/* Literal EL1..EL3 banks, without architectural VHE alias redirection. */
typedef struct DifftestExceptionState {
    uint64_t entered;
    uint64_t pc;
    uint64_t pstate;
    uint64_t spsr[3];
    uint64_t elr[3];
    uint64_t esr[3];
    uint64_t far[3];
} DifftestExceptionState;

typedef enum DifftestDirection {
    DIFFTEST_TO_DUT,
    DIFFTEST_TO_REF,
} DifftestDirection;

/* Initialize one paused mini-virt AArch64 reference CPU. */
QEMU_DIFFTEST_EXPORT int difftest_init(const char *platform);

/* Execute n guest instructions on the initialized reference CPU. */
QEMU_DIFFTEST_EXPORT int difftest_step(uint64_t n);

/* Observe exception entry during the last step; never writes CPU state. */
QEMU_DIFFTEST_EXPORT int difftest_get_exception_state(
    DifftestExceptionState *state, size_t size);

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
