/*
 * Minimal AArch64 virtual machine.
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qobject/qlist.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/arm/bsa.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/pl011.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/arm_gicv3.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/arm/a64-checkpoint.h"
#include "target/arm/cpu.h"

#ifdef CONFIG_ZSTD
#include <zstd.h>
#endif

#define MINI_VIRT_RAM_BASE  0x40000000ULL
#define MINI_VIRT_UART_BASE 0x09000000ULL
#define MINI_VIRT_GIC_DIST_BASE   0x08000000ULL
#define MINI_VIRT_GIC_REDIST_BASE 0x080A0000ULL
#define MINI_VIRT_NUM_SPIS 256
#define MINI_VIRT_UART_IRQ 1
#define MINI_VIRT_ENTRY MINI_VIRT_RAM_BASE
#define MINI_VIRT_ZSTD_CHUNK_SIZE (1 * MiB)
#define TYPE_MINI_VIRT_MACHINE MACHINE_TYPE_NAME("mini-virt")

typedef struct MiniVirtMachineState {
    MachineState parent_obj;

    char *checkpoint_mode;
    char *checkpoint_dir;
    char *cutpoints;
    char *cutpoints_file;
    char *simpoint_path;
    char *simpoint_file;
    uint64_t cpt_interval;
    bool checkpoint_exit_after_last;
} MiniVirtMachineState;

DECLARE_INSTANCE_CHECKER(MiniVirtMachineState, MINI_VIRT_MACHINE,
                         TYPE_MINI_VIRT_MACHINE)

static bool mini_virt_payload_is_zstd(const char *filename)
{
    return g_str_has_suffix(filename, ".zst") ||
           g_str_has_suffix(filename, ".zstd");
}

#ifdef CONFIG_ZSTD
static ssize_t mini_virt_load_zstd_payload(const char *filename,
                                           MemoryRegion *ram,
                                           uint64_t max_sz,
                                           Error **errp)
{
    g_autofree uint8_t *inbuf = g_malloc(MINI_VIRT_ZSTD_CHUNK_SIZE);
    g_autofree uint8_t *outbuf = g_malloc(MINI_VIRT_ZSTD_CHUNK_SIZE);
    uint8_t *ram_ptr = memory_region_get_ram_ptr(ram);
    ZSTD_DCtx *dctx = NULL;
    uint64_t loaded = 0;
    size_t zret = 0;
    int fd;

    fd = qemu_open(filename, O_RDONLY | O_BINARY, errp);
    if (fd < 0) {
        return -1;
    }

    dctx = ZSTD_createDCtx();
    if (!dctx) {
        error_setg(errp, "failed to create zstd decompression context");
        close(fd);
        return -1;
    }

    for (;;) {
        ssize_t rd;

        do {
            rd = read(fd, inbuf, MINI_VIRT_ZSTD_CHUNK_SIZE);
        } while (rd < 0 && errno == EINTR);

        if (rd < 0) {
            error_setg_errno(errp, errno, "failed to read '%s'", filename);
            ZSTD_freeDCtx(dctx);
            close(fd);
            return -1;
        }
        if (rd == 0) {
            break;
        }

        ZSTD_inBuffer input = {
            .src = inbuf,
            .size = rd,
            .pos = 0,
        };

        while (input.pos < input.size) {
            ZSTD_outBuffer output = {
                .dst = outbuf,
                .size = MINI_VIRT_ZSTD_CHUNK_SIZE,
                .pos = 0,
            };

            zret = ZSTD_decompressStream(dctx, &output, &input);
            if (ZSTD_isError(zret)) {
                error_setg(errp, "failed to decompress '%s': %s",
                           filename, ZSTD_getErrorName(zret));
                ZSTD_freeDCtx(dctx);
                close(fd);
                return -1;
            }

            if (output.pos > 0) {
                if (loaded + output.pos > max_sz) {
                    error_setg(errp, "'%s' exceeds maximum image size",
                               filename);
                    ZSTD_freeDCtx(dctx);
                    close(fd);
                    return -1;
                }

                memcpy(ram_ptr + loaded, outbuf, output.pos);
                loaded += output.pos;
            }
        }
    }

    if (zret != 0) {
        error_setg(errp, "truncated zstd payload '%s'", filename);
        ZSTD_freeDCtx(dctx);
        close(fd);
        return -1;
    }
    if (loaded == 0) {
        error_setg(errp, "empty zstd payload '%s'", filename);
        ZSTD_freeDCtx(dctx);
        close(fd);
        return -1;
    }

    ZSTD_freeDCtx(dctx);
    if (close(fd) < 0) {
        error_setg_errno(errp, errno, "failed to close '%s'", filename);
        return -1;
    }

    return loaded;
}
#else
static ssize_t mini_virt_load_zstd_payload(const char *filename,
                                           MemoryRegion *ram,
                                           uint64_t max_sz,
                                           Error **errp)
{
    error_setg(errp, "zstd-compressed payload '%s' requires QEMU built "
               "with --enable-zstd", filename);
    return -1;
}
#endif

static DeviceState *mini_virt_create_gic(DeviceState *cpudev)
{
    DeviceState *gicdev;
    SysBusDevice *gicsbd;
    QList *redist_region_count;
    const int timer_irq[] = {
        [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
        [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
        [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
        [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
        [GTIMER_HYPVIRT] = ARCH_TIMER_NS_EL2_VIRT_IRQ,
        [GTIMER_S_EL2_PHYS] = ARCH_TIMER_S_EL2_IRQ,
        [GTIMER_S_EL2_VIRT] = ARCH_TIMER_S_EL2_VIRT_IRQ,
    };

    gicdev = qdev_new(TYPE_ARM_GICV3);
    qdev_prop_set_uint32(gicdev, "num-cpu", 1);
    qdev_prop_set_uint32(gicdev, "num-irq",
                         MINI_VIRT_NUM_SPIS + GIC_INTERNAL);

    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, 1);
    qdev_prop_set_array(gicdev, "redist-region-count", redist_region_count);

    gicsbd = SYS_BUS_DEVICE(gicdev);
    sysbus_realize_and_unref(gicsbd, &error_fatal);
    sysbus_mmio_map(gicsbd, 0, MINI_VIRT_GIC_DIST_BASE);
    sysbus_mmio_map(gicsbd, 1, MINI_VIRT_GIC_REDIST_BASE);

    for (int i = 0; i < ARRAY_SIZE(timer_irq); i++) {
        qdev_connect_gpio_out(cpudev, i,
                              qdev_get_gpio_in(gicdev,
                                               MINI_VIRT_NUM_SPIS +
                                               timer_irq[i]));
    }

    qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                qdev_get_gpio_in(gicdev,
                                                 MINI_VIRT_NUM_SPIS +
                                                 ARCH_GIC_MAINT_IRQ));

    sysbus_connect_irq(gicsbd, 0, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
    sysbus_connect_irq(gicsbd, 1, qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
    sysbus_connect_irq(gicsbd, 2, qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
    sysbus_connect_irq(gicsbd, 3, qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));

    return gicdev;
}

static void mini_virt_init(MachineState *machine)
{
    MiniVirtMachineState *mms = MINI_VIRT_MACHINE(machine);
    Object *cpuobj;
    DeviceState *cpudev;
    DeviceState *gicdev;
    ssize_t image_size;
    Error *local_err = NULL;

    memory_region_add_subregion(get_system_memory(), MINI_VIRT_RAM_BASE,
                                machine->ram);
    a64_checkpoint_configure(machine->ram, MINI_VIRT_RAM_BASE,
                             machine->ram_size, mms->checkpoint_mode,
                             mms->checkpoint_dir, mms->cutpoints,
                             mms->cutpoints_file, mms->simpoint_path,
                             mms->simpoint_file, mms->cpt_interval,
                             mms->checkpoint_exit_after_last, &local_err);
    if (local_err) {
        error_reportf_err(local_err, "mini-virt checkpoint setup failed: ");
        exit(1);
    }

    cpuobj = object_new(machine->cpu_type);
    if (object_property_find(cpuobj, "rvbar")) {
        object_property_set_int(cpuobj, "rvbar", MINI_VIRT_ENTRY,
                                &error_abort);
    }
    object_property_set_link(cpuobj, "memory", OBJECT(get_system_memory()),
                             &error_abort);
    cpudev = DEVICE(cpuobj);
    qdev_realize(cpudev, NULL, &error_fatal);
    gicdev = mini_virt_create_gic(cpudev);
    object_unref(cpuobj);

    pl011_create(MINI_VIRT_UART_BASE, qdev_get_gpio_in(gicdev,
                                                       MINI_VIRT_UART_IRQ),
                 serial_hd(0));

    if (machine->kernel_filename) {
        if (mini_virt_payload_is_zstd(machine->kernel_filename)) {
            image_size = mini_virt_load_zstd_payload(machine->kernel_filename,
                                                     machine->ram,
                                                     machine->ram_size,
                                                     &local_err);
        } else {
            image_size = load_image_targphys(machine->kernel_filename,
                                             MINI_VIRT_ENTRY,
                                             machine->ram_size,
                                             &local_err);
        }
        if (image_size < 0) {
            if (local_err) {
                error_reportf_err(local_err, "could not load payload '%s': ",
                                  machine->kernel_filename);
            } else {
                error_report("could not load payload '%s'",
                             machine->kernel_filename);
            }
            exit(1);
        }
    }
}

static void mini_virt_set_checkpoint_mode(Object *obj, const char *value,
                                          Error **errp)
{
    MiniVirtMachineState *mms = MINI_VIRT_MACHINE(obj);

    g_free(mms->checkpoint_mode);
    mms->checkpoint_mode = g_strdup(value);
}

static void mini_virt_set_checkpoint_dir(Object *obj, const char *value,
                                         Error **errp)
{
    MiniVirtMachineState *mms = MINI_VIRT_MACHINE(obj);

    g_free(mms->checkpoint_dir);
    mms->checkpoint_dir = g_strdup(value);
}

static void mini_virt_set_cutpoints(Object *obj, const char *value,
                                    Error **errp)
{
    MiniVirtMachineState *mms = MINI_VIRT_MACHINE(obj);

    g_free(mms->cutpoints);
    mms->cutpoints = g_strdup(value);
}

static void mini_virt_set_cutpoints_file(Object *obj, const char *value,
                                         Error **errp)
{
    MiniVirtMachineState *mms = MINI_VIRT_MACHINE(obj);

    g_free(mms->cutpoints_file);
    mms->cutpoints_file = g_strdup(value);
}

static void mini_virt_set_simpoint_path(Object *obj, const char *value,
                                        Error **errp)
{
    MiniVirtMachineState *mms = MINI_VIRT_MACHINE(obj);

    g_free(mms->simpoint_path);
    mms->simpoint_path = g_strdup(value);
}

static void mini_virt_set_simpoint_file(Object *obj, const char *value,
                                        Error **errp)
{
    MiniVirtMachineState *mms = MINI_VIRT_MACHINE(obj);

    g_free(mms->simpoint_file);
    mms->simpoint_file = g_strdup(value);
}

static void mini_virt_set_cpt_interval(Object *obj, const char *value,
                                       Error **errp)
{
    MiniVirtMachineState *mms = MINI_VIRT_MACHINE(obj);
    const char *endp = NULL;
    uint64_t interval;

    if (qemu_strtou64(value, &endp, 0, &interval) < 0 ||
        (endp && *endp != '\0')) {
        error_setg(errp, "invalid cpt-interval '%s'", value);
        return;
    }
    mms->cpt_interval = interval;
}

static void mini_virt_get_checkpoint_exit_after_last(Object *obj, Visitor *v,
                                                     const char *name,
                                                     void *opaque,
                                                     Error **errp)
{
    MiniVirtMachineState *mms = MINI_VIRT_MACHINE(obj);
    bool value = mms->checkpoint_exit_after_last;

    visit_type_bool(v, name, &value, errp);
}

static void mini_virt_set_checkpoint_exit_after_last(Object *obj, Visitor *v,
                                                     const char *name,
                                                     void *opaque,
                                                     Error **errp)
{
    MiniVirtMachineState *mms = MINI_VIRT_MACHINE(obj);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    mms->checkpoint_exit_after_last = value;
}

static void mini_virt_instance_init(Object *obj)
{
    MiniVirtMachineState *mms = MINI_VIRT_MACHINE(obj);

    mms->checkpoint_exit_after_last = true;
}

static void mini_virt_instance_finalize(Object *obj)
{
    MiniVirtMachineState *mms = MINI_VIRT_MACHINE(obj);

    g_free(mms->checkpoint_mode);
    g_free(mms->checkpoint_dir);
    g_free(mms->cutpoints);
    g_free(mms->cutpoints_file);
    g_free(mms->simpoint_path);
    g_free(mms->simpoint_file);
}

static void mini_virt_machine_init(MachineClass *mc)
{
    ObjectClass *oc = OBJECT_CLASS(mc);

    mc->desc = "minimal AArch64 virtual machine with RAM and one PL011 UART";
    mc->init = mini_virt_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a53");
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "mini-virt.ram";
    mc->max_cpus = 1;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;

    object_class_property_add_str(oc, "checkpoint-mode", NULL,
                                  mini_virt_set_checkpoint_mode);
    object_class_property_add_str(oc, "checkpoint-dir", NULL,
                                  mini_virt_set_checkpoint_dir);
    object_class_property_add_str(oc, "cutpoints", NULL,
                                  mini_virt_set_cutpoints);
    object_class_property_add_str(oc, "cutpoints-file", NULL,
                                  mini_virt_set_cutpoints_file);
    object_class_property_add_str(oc, "simpoint-path", NULL,
                                  mini_virt_set_simpoint_path);
    object_class_property_add_str(oc, "simpoint-file", NULL,
                                  mini_virt_set_simpoint_file);
    object_class_property_add_str(oc, "cpt-interval", NULL,
                                  mini_virt_set_cpt_interval);
    object_class_property_add(oc, "checkpoint-exit-after-last", "bool",
                              mini_virt_get_checkpoint_exit_after_last,
                              mini_virt_set_checkpoint_exit_after_last,
                              NULL, NULL);
}

static void mini_virt_class_init(ObjectClass *oc, const void *data)
{
    mini_virt_machine_init(MACHINE_CLASS(oc));
}

static const TypeInfo mini_virt_typeinfo = {
    .name = TYPE_MINI_VIRT_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(MiniVirtMachineState),
    .instance_init = mini_virt_instance_init,
    .instance_finalize = mini_virt_instance_finalize,
    .class_init = mini_virt_class_init,
    .interfaces = aarch64_machine_interfaces,
};

static void mini_virt_register_types(void)
{
    type_register_static(&mini_virt_typeinfo);
}

type_init(mini_virt_register_types)
