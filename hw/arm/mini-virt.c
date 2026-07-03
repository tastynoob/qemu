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
#include "qobject/qlist.h"
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
#include "target/arm/cpu.h"

#define MINI_VIRT_RAM_BASE  0x40000000ULL
#define MINI_VIRT_UART_BASE 0x09000000ULL
#define MINI_VIRT_GIC_DIST_BASE   0x08000000ULL
#define MINI_VIRT_GIC_REDIST_BASE 0x080A0000ULL
#define MINI_VIRT_NUM_SPIS 256
#define MINI_VIRT_UART_IRQ 1
#define MINI_VIRT_ENTRY MINI_VIRT_RAM_BASE

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
    Object *cpuobj;
    DeviceState *cpudev;
    DeviceState *gicdev;
    ssize_t image_size;

    memory_region_add_subregion(get_system_memory(), MINI_VIRT_RAM_BASE,
                                machine->ram);

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
        image_size = load_image_targphys(machine->kernel_filename,
                                         MINI_VIRT_ENTRY,
                                         machine->ram_size,
                                         &error_fatal);
        if (image_size < 0) {
            error_report("could not load payload '%s'",
                         machine->kernel_filename);
            exit(1);
        }
    }
}

static void mini_virt_machine_init(MachineClass *mc)
{
    mc->desc = "minimal AArch64 virtual machine with RAM and one PL011 UART";
    mc->init = mini_virt_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a53");
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "mini-virt.ram";
    mc->max_cpus = 1;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
}

DEFINE_MACHINE_AARCH64("mini-virt", mini_virt_machine_init)
