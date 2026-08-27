/*
 * Minimal QEMU difftest entry points.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_DIFFTEST_H
#define QEMU_DIFFTEST_H

#include <stdint.h>

#if defined(_WIN32)
# define QEMU_DIFFTEST_EXPORT __declspec(dllexport)
#else
# define QEMU_DIFFTEST_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize one paused mini-virt AArch64 reference CPU. */
QEMU_DIFFTEST_EXPORT int difftest_init(const char *platform);

/* Execute n guest instructions on the initialized reference CPU. */
QEMU_DIFFTEST_EXPORT int difftest_step(uint64_t n);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_DIFFTEST_H */
