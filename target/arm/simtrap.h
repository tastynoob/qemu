/*
 * AArch64 SimPoint trap pseudo-instructions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_ARM_SIMTRAP_H
#define TARGET_ARM_SIMTRAP_H

#define SIMTRAP_GOOD_TRAP             0x000
#define SIMTRAP_DISABLE_TIME_INTR     0x100
#define SIMTRAP_NOTIFY_PROFILER       0x101
#define SIMTRAP_NOTIFY_WORKLOAD_EXIT  0x102

#define A64_SIMTRAP_PROFILE_START SIMTRAP_NOTIFY_PROFILER
#define A64_SIMTRAP_PROFILE_STOP  SIMTRAP_NOTIFY_WORKLOAD_EXIT

#define A64_SIMTRAP_IS_PROFILE_SIGNAL(imm) \
    ((imm) == A64_SIMTRAP_PROFILE_START || \
     (imm) == A64_SIMTRAP_PROFILE_STOP)

#define A64_SIMTRAP_IS_HANDLED(imm) \
    ((imm) == SIMTRAP_DISABLE_TIME_INTR || \
     A64_SIMTRAP_IS_PROFILE_SIGNAL(imm))

#ifdef CONFIG_PLUGIN
bool qemu_plugin_a64_simtrap_in_profiling_mode(void);
#else
static inline bool qemu_plugin_a64_simtrap_in_profiling_mode(void)
{
    return false;
}
#endif

#endif /* TARGET_ARM_SIMTRAP_H */
