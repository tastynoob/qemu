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
#include "hw/core/cpu.h"
#include "system/cpus.h"
#include "system/replay.h"
#include "system/system.h"

static CPUState *difftest_cpu;
static bool difftest_initialized;
static char *difftest_machine_arg;

/* system/main.c is intentionally not part of the shared library. */
int (*qemu_main)(void);

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
