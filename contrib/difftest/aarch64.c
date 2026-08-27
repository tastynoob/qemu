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
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/cpus.h"
#include "system/replay.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"

static CPUState *difftest_cpu;
static bool difftest_initialized;
static bool difftest_has_stepped;
static char *difftest_machine_arg;
static DifftestRegPtrs difftest_reg_ptrs;
static MemoryRegion difftest_backed_ram;
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
    default:
        return -ENOTSUP;
    }
}

int difftest_setmisc(uint32_t regid, uint64_t value)
{
    CPUARMState *env;

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
    default:
        return -ENOTSUP;
    }
}
