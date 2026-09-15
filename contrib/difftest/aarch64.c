/*
 * Minimal AArch64 QEMU difftest adapter.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/difftest.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/cpu-common.h"
#include "exec/tb-flush.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/cpus.h"
#include "system/replay.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "target/arm/internals.h"

static CPUState *difftest_cpu;
static bool difftest_initialized;
static bool difftest_has_stepped;
static char *difftest_machine_arg;
static DifftestRegPtrs difftest_reg_ptrs;
static MemoryRegion difftest_backed_ram;
static MemoryRegion difftest_low_ram_alias;
static bool difftest_backed_ram_mapped;

#define DIFFTEST_MINI_VIRT_RAM_BASE 0x40000000ULL

/* system/main.c is intentionally not part of the shared library. */
int (*qemu_main)(void);

static CPUARMState *difftest_env(void)
{
    return &ARM_CPU(difftest_cpu)->env;
}

static void difftest_init_reg_ptrs(void)
{
    CPUARMState *env = difftest_env();

    for (size_t i = 0; i < DIFFTEST_GPR_COUNT; i++) {
        difftest_reg_ptrs.xregs[i] = &env->xregs[i];
    }
    difftest_reg_ptrs.pc = &env->pc;
    for (size_t i = 0; i < DIFFTEST_VREG_COUNT; i++) {
        difftest_reg_ptrs.vregs[i] = aa64_vfp_qreg(env, i);
    }
}

static uint64_t difftest_get_sp_el(CPUARMState *env, unsigned int el)
{
    unsigned int current_el = arm_current_el(env);

    if ((env->pstate & PSTATE_SP) && el == current_el) {
        return env->xregs[31];
    }
    if (!(env->pstate & PSTATE_SP) && el == 0) {
        return env->xregs[31];
    }
    return env->sp_el[el];
}

static void difftest_set_sp_el(CPUARMState *env, unsigned int el,
                               uint64_t value)
{
    unsigned int current_el = arm_current_el(env);

    if ((env->pstate & PSTATE_SP) && el == current_el) {
        env->xregs[31] = value;
    } else if (!(env->pstate & PSTATE_SP) && el == 0) {
        env->xregs[31] = value;
    } else {
        env->sp_el[el] = value;
    }
}

static uint64_t *difftest_spsr_ptr(CPUARMState *env, unsigned int el)
{
    return &env->banked_spsr[aarch64_banked_spsr_index(el)];
}

static unsigned int difftest_el1_fault_reg_el(CPUARMState *env)
{
    /*
     * FAR_EL1 and ESR_EL1 are architecturally redirected to their EL2 forms
     * when QEMU executes at EL2 with VHE (HCR_EL2.E2H).  gem5's
     * readMiscRegNoEffect(MISCREG_{FAR,ESR}_EL1) applies the same alias rule
     * before passing this difftest value.  Select the backing field that the
     * following QEMU MRS will actually access rather than always selecting
     * the literal EL1 storage.
     *
     * syncDifftestState writes PSTATE before these register values, so this
     * helper observes the same EL and VHE state QEMU will use after the
     * reference-state restore completes.
     */
    if (arm_current_el(env) == 2 &&
        (arm_hcr_el2_eff(env) & HCR_E2H)) {
        return 2;
    }
    return 1;
}

static ARMCPRegInfo *difftest_ctr_el0_reginfo(void)
{
    ARMCPU *cpu = ARM_CPU(difftest_cpu);
    uint32_t key = ENCODE_AA64_CP_REG(3, 3, 0, 0, 1);

    /*
     * QEMU normally exposes registered coprocessor descriptors through the
     * const-qualified get_arm_cp_reginfo() lookup because target execution
     * must not mutate the CPU model.  The difftest adapter has a narrower
     * requirement: before executing reference instructions, it must replace
     * an implementation-defined read-only property with the DUT CPU's value.
     * cpu->cp_regs owns writable ARMCPRegInfo copies allocated during CPU
     * realization, so looking up that owned object directly is safe here.
     * Keeping this exception in one helper also prevents ordinary register
     * synchronization code from treating arbitrary constant cpregs as guest-
     * writable state.
     */
    return g_hash_table_lookup(cpu->cp_regs, (gpointer)(uintptr_t)key);
}

int difftest_init(const char *platform)
{
    char *argv[] = {
        (char *)"qemu-system-aarch64-difftest",
        (char *)"-machine",
        NULL,
        (char *)"-cpu",
        /* v9.2-oriented max profile with a 128-bit SVE and newer extensions off. */
        (char *)"max,sve-max-vq=1,sme=off,pauth=off,lpa2=off,x-rme=off",
        (char *)"-accel",
        (char *)"tcg,thread=single",
        (char *)"-smp",
        (char *)"1",
        (char *)"-m",
        (char *)"8G",
        (char *)"-S",
        (char *)"-display",
        (char *)"none",
        (char *)"-monitor",
        (char *)"none",
        (char *)"-serial",
        (char *)"null",
        NULL,
    };

    if (difftest_initialized) {
        return -EALREADY;
    }
    if (g_strcmp0(platform, "fs") != 0 &&
        g_strcmp0(platform, "bm") != 0) {
        return -EINVAL;
    }

    difftest_machine_arg = g_strdup_printf("mini-virt,platform=%s",
                                           platform);
    argv[2] = difftest_machine_arg;

    qemu_cpu_difftest_set_direct(true);
    qemu_init(ARRAY_SIZE(argv) - 1, argv);

    difftest_cpu = first_cpu;
    if (!difftest_cpu) {
        bql_unlock();
        replay_mutex_unlock();
        return -ENODEV;
    }

    /* Keep virtual time and interrupts out of a single-instruction step. */
    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, false);
    cpu_single_step(difftest_cpu,
                    SSTEP_ENABLE | SSTEP_NOIRQ | SSTEP_NOTIMER);
    difftest_init_reg_ptrs();
    difftest_initialized = true;

    /* qemu_init returns with both locks held. */
    bql_unlock();
    replay_mutex_unlock();
    return 0;
}

int difftest_step(uint64_t n)
{
    if (!difftest_initialized || !difftest_cpu) {
        return -EINVAL;
    }

    if (n) {
        difftest_has_stepped = true;
    }

    for (uint64_t i = 0; i < n; i++) {
        int ret;

        qatomic_set(&difftest_cpu->exit_request, false);
        cpu_exec_start(difftest_cpu);
        ret = cpu_exec(difftest_cpu);
        cpu_exec_end(difftest_cpu);

        if (ret == EXCP_DEBUG) {
            continue;
        }
        if (ret == EXCP_ATOMIC) {
            cpu_exec_step_atomic(difftest_cpu);
            continue;
        }
        /*
         * AArch64 YIELD is an architecturally completed hint, not a guest
         * exception. The translator writes PC + 4 before the helper exits the
         * TCG execution loop with EXCP_YIELD, so this return already accounts
         * for the current difftest instruction. Treat it like the successful
         * single-step EXCP_DEBUG return and advance the loop counter normally.
         * Retrying cpu_exec() for the same iteration would instead execute the
         * following guest instruction and put the reference one instruction
         * ahead of gem5.
         *
         * QEMU's normal system loop consumes EXCP_YIELD only to give another
         * vCPU a scheduling opportunity. The difftest reference has one vCPU
         * and is driven synchronously by gem5, so there is no scheduler action
         * to perform here.
         */
        if (ret == EXCP_YIELD) {
            continue;
        }
        return ret;
    }

    return 0;
}

int difftest_memcpy(uint64_t addr, void *buf, size_t n, bool direction)
{
    MemTxResult result;

    if (!difftest_initialized || !buf) {
        return -EINVAL;
    }
    if (direction != DIFFTEST_TO_DUT && direction != DIFFTEST_TO_REF) {
        return -EINVAL;
    }

    result = address_space_rw(&address_space_memory, addr,
                              MEMTXATTRS_UNSPECIFIED, buf, n,
                              direction == DIFFTEST_TO_REF);
    return result == MEMTX_OK ? 0 : -EIO;
}

int difftest_get_backed_memory(void *backed_mem, size_t n)
{
    if (!difftest_initialized || !current_machine || !backed_mem) {
        return -EINVAL;
    }
    if (difftest_has_stepped || difftest_backed_ram_mapped) {
        return -EBUSY;
    }
    if (n != current_machine->ram_size ||
        n != memory_region_size(current_machine->ram)) {
        return -EINVAL;
    }

    bql_lock();
    memory_region_init_ram_ptr(&difftest_backed_ram, NULL,
                               "mini-virt.difftest-backed-ram", n,
                               backed_mem);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        DIFFTEST_MINI_VIRT_RAM_BASE,
                                        &difftest_backed_ram, 1);

    /*
     * ARM GCPT restore code may temporarily execute with the MMU disabled
     * and architectural physical PCs below the mini-virt RAM base.  gem5
     * treats an otherwise-unmapped low physical address as an offset into the
     * checkpoint RAM at 0x40000000.  Mirror that compatibility view here so a
     * synchronized QEMU reference fetches the same bytes at, for example,
     * PC 0x404 as gem5 fetches from physical address 0x40000404.
     *
     * The alias covers only the low 1 GiB window that gem5 can relocate and
     * uses priority -1.  Real mini-virt devices such as the GIC and UART are
     * registered at the normal priority and therefore continue to win any
     * overlap; the alias fills only otherwise-unmapped holes.  Both mappings
     * reference difftest_backed_ram, so instruction fetches and data accesses
     * observe the same caller-owned COW pages without copying memory.
     */
    memory_region_init_alias(&difftest_low_ram_alias, NULL,
                             "mini-virt.difftest-low-ram-alias",
                             &difftest_backed_ram, 0,
                             MIN((uint64_t)n,
                                 DIFFTEST_MINI_VIRT_RAM_BASE));
    memory_region_add_subregion_overlap(get_system_memory(), 0,
                                        &difftest_low_ram_alias, -1);
    bql_unlock();

    difftest_backed_ram_mapped = true;
    return 0;
}

DifftestRegPtrs *difftest_getregptr(void)
{
    if (!difftest_initialized || !difftest_cpu) {
        return NULL;
    }

    return &difftest_reg_ptrs;
}

int difftest_getmisc(uint32_t regid, uint64_t *value)
{
    CPUARMState *env;
    unsigned int fault_reg_el;

    if (!difftest_initialized || !difftest_cpu || !value) {
        return -EINVAL;
    }

    env = difftest_env();
    switch (regid) {
    case DIFFTEST_MISC_PSTATE:
        *value = pstate_read(env);
        return 0;
    case DIFFTEST_MISC_FPSR:
        *value = vfp_get_fpsr(env);
        return 0;
    case DIFFTEST_MISC_FPCR:
        *value = vfp_get_fpcr(env);
        return 0;
    case DIFFTEST_MISC_SPSR_EL1:
        *value = *difftest_spsr_ptr(env, 1);
        return 0;
    case DIFFTEST_MISC_SPSR_EL2:
        *value = *difftest_spsr_ptr(env, 2);
        return 0;
    case DIFFTEST_MISC_SPSR_EL3:
        *value = *difftest_spsr_ptr(env, 3);
        return 0;
    case DIFFTEST_MISC_ELR_EL1:
        *value = env->elr_el[1];
        return 0;
    case DIFFTEST_MISC_ELR_EL2:
        *value = env->elr_el[2];
        return 0;
    case DIFFTEST_MISC_ELR_EL3:
        *value = env->elr_el[3];
        return 0;
    case DIFFTEST_MISC_SP_EL0:
        *value = difftest_get_sp_el(env, 0);
        return 0;
    case DIFFTEST_MISC_SP_EL1:
        *value = difftest_get_sp_el(env, 1);
        return 0;
    case DIFFTEST_MISC_SP_EL2:
        *value = difftest_get_sp_el(env, 2);
        return 0;
    case DIFFTEST_MISC_SP_EL3:
        *value = difftest_get_sp_el(env, 3);
        return 0;
    /*
     * Use CPUARMState's architectural backing fields directly.  The QEMU
     * system-register descriptors for TPIDR_EL0, TPIDRRO_EL0, and TPIDR_EL1-3
     * use these same fields, so difftest_getmisc() observes exactly the state
     * that a following guest MRS instruction would read.  This is important
     * for TPIDRRO_EL0 in particular: exception-vector code commonly treats it
     * as scratch state, and a stale reference value otherwise appears later
     * as a misleading GPR mismatch at MRS.
     */
    case DIFFTEST_MISC_TPIDR_EL0:
        *value = env->cp15.tpidr_el[0];
        return 0;
    case DIFFTEST_MISC_TPIDRRO_EL0:
        *value = env->cp15.tpidrro_el[0];
        return 0;
    case DIFFTEST_MISC_TPIDR_EL1:
        *value = env->cp15.tpidr_el[1];
        return 0;
    case DIFFTEST_MISC_TPIDR_EL2:
        *value = env->cp15.tpidr_el[2];
        return 0;
    case DIFFTEST_MISC_TPIDR_EL3:
        *value = env->cp15.tpidr_el[3];
        return 0;
    /*
     * QEMU's architectural system-register descriptors for FAR and ESR at
     * EL1 through EL3 use these CPUARMState backing fields.  Access them directly so
     * the difftest ABI transfers exactly the value a guest MRS will observe
     * after gem5 has handled an exception and requested a reference-state
     * resynchronization.
     */
    case DIFFTEST_MISC_FAR_EL1:
        fault_reg_el = difftest_el1_fault_reg_el(env);
        *value = env->cp15.far_el[fault_reg_el];
        return 0;
    case DIFFTEST_MISC_ESR_EL1:
        fault_reg_el = difftest_el1_fault_reg_el(env);
        *value = env->cp15.esr_el[fault_reg_el];
        return 0;
    case DIFFTEST_MISC_FAR_EL2:
        *value = env->cp15.far_el[2];
        return 0;
    case DIFFTEST_MISC_ESR_EL2:
        *value = env->cp15.esr_el[2];
        return 0;
    case DIFFTEST_MISC_FAR_EL3:
        *value = env->cp15.far_el[3];
        return 0;
    case DIFFTEST_MISC_ESR_EL3:
        *value = env->cp15.esr_el[3];
        return 0;
    case DIFFTEST_MISC_CTR_EL0: {
        ARMCPRegInfo *ctr = difftest_ctr_el0_reginfo();

        if (!ctr || !(ctr->type & ARM_CP_CONST)) {
            return -ENODEV;
        }
        /*
         * MRS CTR_EL0 is constant-folded from this descriptor at translation
         * time.  Return the descriptor value, rather than ARMCPU::ctr, so the
         * getter reports exactly what a newly translated guest MRS observes.
         */
        *value = ctr->resetvalue;
        return 0;
    }
    default:
        return -ENOTSUP;
    }
}

int difftest_setmisc(uint32_t regid, uint64_t value)
{
    CPUARMState *env;
    unsigned int fault_reg_el;

    if (!difftest_initialized || !difftest_cpu) {
        return -EINVAL;
    }

    env = difftest_env();
    switch (regid) {
    case DIFFTEST_MISC_PSTATE:
        pstate_write(env, value);
        arm_rebuild_hflags(env);
        return 0;
    case DIFFTEST_MISC_FPSR:
        vfp_set_fpsr(env, value);
        return 0;
    case DIFFTEST_MISC_FPCR:
        vfp_set_fpcr(env, value);
        return 0;
    case DIFFTEST_MISC_SPSR_EL1:
        *difftest_spsr_ptr(env, 1) = value;
        return 0;
    case DIFFTEST_MISC_SPSR_EL2:
        *difftest_spsr_ptr(env, 2) = value;
        return 0;
    case DIFFTEST_MISC_SPSR_EL3:
        *difftest_spsr_ptr(env, 3) = value;
        return 0;
    case DIFFTEST_MISC_ELR_EL1:
        env->elr_el[1] = value;
        return 0;
    case DIFFTEST_MISC_ELR_EL2:
        env->elr_el[2] = value;
        return 0;
    case DIFFTEST_MISC_ELR_EL3:
        env->elr_el[3] = value;
        return 0;
    case DIFFTEST_MISC_SP_EL0:
        difftest_set_sp_el(env, 0, value);
        return 0;
    case DIFFTEST_MISC_SP_EL1:
        difftest_set_sp_el(env, 1, value);
        return 0;
    case DIFFTEST_MISC_SP_EL2:
        difftest_set_sp_el(env, 2, value);
        return 0;
    case DIFFTEST_MISC_SP_EL3:
        difftest_set_sp_el(env, 3, value);
        return 0;
    /*
     * These assignments are used both for initial checkpoint synchronization
     * and after gem5 handles an exception without asking QEMU to execute the
     * exception entry itself.  Write all five registers as one ABI family so
     * the reference resumes from the same complete architectural thread state
     * as the DUT.  No hflags rebuild is required: unlike PSTATE, TPIDR values
     * do not alter translation mode or QEMU's cached execution flags.
     */
    case DIFFTEST_MISC_TPIDR_EL0:
        env->cp15.tpidr_el[0] = value;
        return 0;
    case DIFFTEST_MISC_TPIDRRO_EL0:
        env->cp15.tpidrro_el[0] = value;
        return 0;
    case DIFFTEST_MISC_TPIDR_EL1:
        env->cp15.tpidr_el[1] = value;
        return 0;
    case DIFFTEST_MISC_TPIDR_EL2:
        env->cp15.tpidr_el[2] = value;
        return 0;
    case DIFFTEST_MISC_TPIDR_EL3:
        env->cp15.tpidr_el[3] = value;
        return 0;
    /*
     * These writes restore exception-generated state that QEMU did not
     * produce itself: gem5 takes the exception, then copies the resulting
     * architectural state before QEMU resumes at the handler.  FAR and ESR
     * for EL1 through EL3 do not affect QEMU's translation mode or cached
     * hflags, so a
     * direct backing-field update is both sufficient and matches QEMU's normal
     * system-register access path.
     */
    case DIFFTEST_MISC_FAR_EL1:
        fault_reg_el = difftest_el1_fault_reg_el(env);
        env->cp15.far_el[fault_reg_el] = value;
        return 0;
    case DIFFTEST_MISC_ESR_EL1:
        fault_reg_el = difftest_el1_fault_reg_el(env);
        env->cp15.esr_el[fault_reg_el] = value;
        return 0;
    case DIFFTEST_MISC_FAR_EL2:
        env->cp15.far_el[2] = value;
        return 0;
    case DIFFTEST_MISC_ESR_EL2:
        env->cp15.esr_el[2] = value;
        return 0;
    case DIFFTEST_MISC_FAR_EL3:
        env->cp15.far_el[3] = value;
        return 0;
    case DIFFTEST_MISC_ESR_EL3:
        env->cp15.esr_el[3] = value;
        return 0;
    case DIFFTEST_MISC_CTR_EL0: {
        ARMCPU *cpu = ARM_CPU(difftest_cpu);
        ARMCPRegInfo *ctr = difftest_ctr_el0_reginfo();

        if (!ctr || !(ctr->type & ARM_CP_CONST)) {
            return -ENODEV;
        }
        if (ctr->resetvalue == value) {
            cpu->ctr = value;
            return 0;
        }

        /*
         * CTR_EL0 is not ordinary writable guest state.  It is a read-only
         * CPU implementation property, and QEMU's translator embeds an
         * ARM_CP_CONST descriptor's resetvalue directly into every MRS TB.
         * Update both the canonical ARMCPU property and the realized AArch64
         * descriptor so future translations advertise gem5's cache contract.
         *
         * Existing TBs may already contain the previous QEMU "max" value
         * (notably IDC and DIC).  The difftest machine has one vCPU, remains
         * stopped outside synchronous cpu_exec() calls, and reaches this code
         * only between steps, which satisfies the serial-context requirement
         * of tb_flush__exclusive_or_serial().  Flush only when the value
         * changes.  The following guest MRS still executes normally and its
         * destination GPR remains subject to strict difftest comparison.
         */
        cpu->ctr = value;
        ctr->resetvalue = value;
        tb_flush__exclusive_or_serial();
        return 0;
    }
    default:
        return -ENOTSUP;
    }
}
