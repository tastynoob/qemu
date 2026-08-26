/*
 * AArch64 performance checkpoint support.
 *
 * This is a lightweight checkpoint path for mini-virt raw payloads. It writes
 * the AArch64 libcheckpoint memory format, not a QEMU VM migration snapshot.
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
                              const char *checkpoint_pcs,
                              const char *cutpoints_file,
                              const char *simpoint_path,
                              const char *simpoint_file,
                              uint64_t cpt_interval,
                              uint64_t warmup_interval,
                              const char *trigger,
                              const char *instruction_count,
                              uint64_t compare_start_pc,
                              uint64_t compare_limit,
                              const char *compare_counter,
                              bool exit_after_last,
                              Error **errp);
bool a64_instruction_count_is_enabled(void);
bool a64_instruction_count_is_exact(void);
bool a64_checkpoint_is_timed_mode(void);
bool a64_checkpoint_is_exact_timed_mode(void);
bool a64_checkpoint_is_compare_mode(void);
bool a64_checkpoint_uses_exact_count(void);
bool a64_checkpoint_compare_uses_exact(void);
bool a64_checkpoint_compare_start_pending(uint64_t pc);
bool a64_checkpoint_is_pc_mode(void);
bool a64_checkpoint_pc_pending(uint64_t pc);
void a64_checkpoint_cpu_init(CPUARMState *env);
void a64_checkpoint_try_take(CPUARMState *env, uint64_t pc);
void a64_checkpoint_try_take_exact(CPUARMState *env, uint64_t pc);
void a64_checkpoint_try_take_pc(CPUARMState *env, uint64_t pc);
void a64_checkpoint_compare_start(CPUARMState *env, uint64_t pc);
void a64_checkpoint_compare_check(CPUARMState *env);
void a64_checkpoint_notify_profiler(CPUARMState *env, bool start, uint64_t pc);
#else
static inline bool a64_instruction_count_is_enabled(void)
{
    return false;
}

static inline bool a64_instruction_count_is_exact(void)
{
    return true;
}

static inline bool a64_checkpoint_is_timed_mode(void)
{
    return false;
}

static inline bool a64_checkpoint_is_exact_timed_mode(void)
{
    return false;
}

static inline bool a64_checkpoint_is_compare_mode(void)
{
    return false;
}

static inline bool a64_checkpoint_uses_exact_count(void)
{
    return false;
}

static inline bool a64_checkpoint_compare_uses_exact(void)
{
    return false;
}

static inline bool a64_checkpoint_compare_start_pending(uint64_t pc)
{
    return false;
}

static inline bool a64_checkpoint_is_pc_mode(void)
{
    return false;
}

static inline bool a64_checkpoint_pc_pending(uint64_t pc)
{
    return false;
}

static inline void a64_checkpoint_cpu_init(CPUARMState *env)
{
}

static inline void a64_checkpoint_try_take(CPUARMState *env, uint64_t pc)
{
}

static inline void a64_checkpoint_try_take_exact(CPUARMState *env,
                                                 uint64_t pc)
{
}

static inline void a64_checkpoint_try_take_pc(CPUARMState *env, uint64_t pc)
{
}

static inline void a64_checkpoint_compare_start(CPUARMState *env, uint64_t pc)
{
}

static inline void a64_checkpoint_compare_check(CPUARMState *env)
{
}

static inline void a64_checkpoint_notify_profiler(CPUARMState *env, bool start,
                                                  uint64_t pc)
{
}

#endif

#endif /* TARGET_ARM_A64_CHECKPOINT_H */
