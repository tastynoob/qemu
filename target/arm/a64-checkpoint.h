/*
 * AArch64 SimPoint checkpoint support.
 *
 * This is a lightweight performance-checkpoint path for mini-virt raw
 * payloads. It writes the AArch64 libcheckpoint memory format, not a QEMU VM
 * migration snapshot.
 */

#ifndef TARGET_ARM_A64_CHECKPOINT_H
#define TARGET_ARM_A64_CHECKPOINT_H

#include "qapi/error.h"
#include "system/memory.h"
#include "target/arm/cpu.h"

#ifndef CONFIG_USER_ONLY
void a64_checkpoint_configure(MemoryRegion *ram, uint64_t ram_base,
                              uint64_t ram_size, const char *mode,
                              const char *output_dir,
                              const char *cutpoints,
                              const char *cutpoints_file,
                              const char *simpoint_path,
                              const char *simpoint_file,
                              uint64_t cpt_interval,
                              bool exit_after_last,
                              Error **errp);
bool a64_checkpoint_is_enabled(void);
bool a64_checkpoint_has_pending(void);
void a64_checkpoint_try_take(CPUARMState *env, uint64_t pc);
void a64_checkpoint_notify_profiler(CPUARMState *env, bool start);
#else
static inline bool a64_checkpoint_is_enabled(void)
{
    return false;
}

static inline bool a64_checkpoint_has_pending(void)
{
    return false;
}

static inline void a64_checkpoint_try_take(CPUARMState *env, uint64_t pc)
{
}

static inline void a64_checkpoint_notify_profiler(CPUARMState *env, bool start)
{
}
#endif

#endif /* TARGET_ARM_A64_CHECKPOINT_H */
