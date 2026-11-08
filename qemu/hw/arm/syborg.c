/*
 * Syborg (Symbian Virtual Platform) board, as the Symbian Foundation's baseport/syborg expects it.
 *
 * Memory map (fixed in the baseport's bootstrap/syborg.inc and specific/syborg.h):
 *   0x00000000 RAM (-m, default 512 MiB)        0xc0000000 interrupt controller -> CPU IRQ
 *   0xc0001000 RTC                              0xc0002000 timer (irq 1, 100 kHz)
 *   0xc0003000 keyboard (irq 2)                 0xc0004000 touchscreen (irq 3)
 *   0xc0005000 framebuffer (irq 4)              0xc0006000..0xc0009000 serial 0..3 (irq 5..8)
 *   0xc000a000 hostfs  0xc000b000 snapshot  0xc000c000 virtio-net  0xc000d000 nand
 *   0xc000e000 virtio-audio  0xc000f000 usbtest: not modelled, they read as zero (-d unimp logs)
 *   0xc0010000 NVMemory disk = C: (irq 12)      0xc0011000 second timer (irq 14)
 *   0xc0012000 platform: ID, the device tree at +0x1000 (8 pages)
 *
 * The ROM is loaded with -kernel as a raw image at 0x10000 behind the ARM boot stub, as on the
 * Symbian Foundation's QEMU 0.9.1. Options: -M syborg,width=W,height=H (screen, 768x640 default),
 * -drive if=mtd,format=raw,file=c.img (C:), -serial ... (serial 0 is the kernel's debug port),
 * -M syborg,hostfs=DIR (host folder DIR as the guest's E:, via svphostfs; POSIX hosts only).
 *
 * Copyright (c) 2008 CodeSourcery
 * Copyright (c) 2026 The NeoSymbian Authors
 * SPDX-License-Identifier: MIT
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/arm/syborg.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "system/device_tree.h"
#include "system/system.h"
#include "chardev/char.h"
#include "target/arm/cpu-qom.h"
#include "qom/object.h"

/* ---- platform device: identifies the board and serves the device tree (read by hostfs) ---- */

OBJECT_DECLARE_SIMPLE_TYPE(SyborgPlatformState, SYBORG_PLATFORM)

#define PLATFORM_TREE_OFFSET 0x1000
#define PLATFORM_SIZE        0x8000

struct SyborgPlatformState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    void *fdt;
    int fdt_size;
};

static uint64_t syborg_platform_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgPlatformState *s = opaque;
    uint64_t v = 0;

    if (offset >= PLATFORM_TREE_OFFSET) {
        for (unsigned i = 0; i < size; i++) {
            hwaddr pos = offset - PLATFORM_TREE_OFFSET + i;

            if (pos < s->fdt_size) {
                v |= (uint64_t)((uint8_t *)s->fdt)[pos] << (8 * i);
            }
        }
        return v;
    }
    switch (offset) {
    case 0:
        return SYBORG_ID_PLATFORM;
    case 4:
        return PLATFORM_TREE_OFFSET;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read at 0x%" HWADDR_PRIx "\n", TYPE_SYBORG_PLATFORM,
                  offset);
    return 0;
}

static void syborg_platform_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only 0x%" HWADDR_PRIx "\n",
                  TYPE_SYBORG_PLATFORM, offset);
}

static const MemoryRegionOps syborg_platform_ops = {
    .read = syborg_platform_read,
    .write = syborg_platform_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void syborg_platform_init(Object *obj)
{
    SyborgPlatformState *s = SYBORG_PLATFORM(obj);

    memory_region_init_io(&s->iomem, obj, &syborg_platform_ops, s, TYPE_SYBORG_PLATFORM,
                          PLATFORM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

/* ---- the machine ---- */

#define TYPE_SYBORG_MACHINE MACHINE_TYPE_NAME("syborg")
OBJECT_DECLARE_SIMPLE_TYPE(SyborgMachineState, SYBORG_MACHINE)

struct SyborgMachineState {
    MachineState parent_obj;
    uint32_t width;
    uint32_t height;
    char *hostfs;
};

static struct arm_boot_info syborg_binfo;

static DeviceState *syborg_map(DeviceState *dev, hwaddr base, qemu_irq irq)
{
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    if (irq) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    }
    return dev;
}

static void syborg_init(MachineState *machine)
{
    SyborgMachineState *sms = SYBORG_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj = object_new(machine->cpu_type);
    DeviceState *intc, *dev;
    SyborgPlatformState *platform;
    DriveInfo *dinfo;

    if (machine->ram_size > 0xc0000000) {
        error_report("syborg: RAM must end below the devices at 0xc0000000");
        exit(1);
    }
    /* The baseport's Cortex-A8 has no TrustZone, like the A8 of QEMU 0.9.1. */
    if (object_property_find(cpuobj, "has_el3")) {
        object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
    }
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    memory_region_add_subregion(sysmem, 0, machine->ram);

    intc = syborg_map(qdev_new(TYPE_SYBORG_INTC), 0xc0000000,
                      qdev_get_gpio_in(DEVICE(cpuobj), ARM_CPU_IRQ));
#define IRQ(n) qdev_get_gpio_in(intc, (n))
    syborg_map(qdev_new(TYPE_SYBORG_RTC), 0xc0001000, NULL);
    syborg_map(qdev_new(TYPE_SYBORG_TIMER), 0xc0002000, IRQ(1));
    syborg_map(qdev_new(TYPE_SYBORG_KEYBOARD), 0xc0003000, IRQ(2));
    syborg_map(qdev_new(TYPE_SYBORG_POINTER), 0xc0004000, IRQ(3));

    dev = qdev_new(TYPE_SYBORG_FB);
    qdev_prop_set_uint32(dev, "width", sms->width);
    qdev_prop_set_uint32(dev, "height", sms->height);
    syborg_map(dev, 0xc0005000, IRQ(4));

    for (int i = 0; i < 4; i++) {
        dev = qdev_new(TYPE_SYBORG_SERIAL);
        qdev_prop_set_chr(dev, "chardev", serial_hd(i));
        syborg_map(dev, 0xc0006000 + i * 0x1000, IRQ(5 + i));
    }

    if (sms->hostfs) {
        dev = qdev_try_new(TYPE_SYBORG_HOSTFS);
        if (!dev) {
            error_report("syborg: hostfs needs a POSIX host");
            exit(1);
        }
        qdev_prop_set_string(dev, "path", sms->hostfs);
        syborg_map(dev, 0xc000a000, NULL);
    } else {
        create_unimplemented_device("syborg-hostfs", 0xc000a000, 0x1000);
    }
    create_unimplemented_device("syborg-snapshot", 0xc000b000, 0x1000);
    create_unimplemented_device("syborg-virtio-net", 0xc000c000, 0x1000);
    create_unimplemented_device("syborg-nand", 0xc000d000, 0x1000);
    create_unimplemented_device("syborg-virtio-audio", 0xc000e000, 0x1000);
    create_unimplemented_device("syborg-usbtest", 0xc000f000, 0x1000);

    dev = qdev_new(TYPE_SYBORG_NVMEMORY);
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(dev, "drive", blk_by_legacy_dinfo(dinfo), &error_fatal);
    }
    syborg_map(dev, 0xc0010000, IRQ(12));

    syborg_map(qdev_new(TYPE_SYBORG_TIMER), 0xc0011000, IRQ(14));

    dev = qdev_new(TYPE_SYBORG_PLATFORM);
    platform = SYBORG_PLATFORM(dev);
    platform->fdt = create_device_tree(&platform->fdt_size);
    if (!platform->fdt) {
        error_report("syborg: cannot create the device tree");
        exit(1);
    }
    qemu_fdt_setprop_string(platform->fdt, "/", "compatible", "syborg");
    if (sms->hostfs) {
        /* what svphostfsdriver.ldd parses; svphostfs.exe (started by estart) mounts it */
        qemu_fdt_add_subnode(platform->fdt, "/hostfs@c000a000");
        qemu_fdt_setprop_string(platform->fdt, "/hostfs@c000a000", "compatible",
                                "syborg,hostfs");
        qemu_fdt_setprop_cell(platform->fdt, "/hostfs@c000a000", "reg", 0xc000a000);
        qemu_fdt_setprop_cell(platform->fdt, "/hostfs@c000a000", "drive-number",
                              SYBORG_HOSTFS_DRIVE);
    }
    syborg_map(dev, 0xc0012000, NULL);
#undef IRQ

    syborg_binfo.ram_size = machine->ram_size;
    syborg_binfo.board_id = 0;
    arm_load_kernel(ARM_CPU(cpuobj), machine, &syborg_binfo);
}

static char *syborg_get_hostfs(Object *obj, Error **errp)
{
    return g_strdup(SYBORG_MACHINE(obj)->hostfs);
}

static void syborg_set_hostfs(Object *obj, const char *value, Error **errp)
{
    SyborgMachineState *sms = SYBORG_MACHINE(obj);

    g_free(sms->hostfs);
    sms->hostfs = g_strdup(value);
}

static void syborg_machine_instance_init(Object *obj)
{
    SyborgMachineState *sms = SYBORG_MACHINE(obj);

    sms->width = 768;
    sms->height = 640;
    object_property_add_uint32_ptr(obj, "width", &sms->width, OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "width", "Screen width in pixels (default 768)");
    object_property_add_uint32_ptr(obj, "height", &sms->height, OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "height", "Screen height in pixels (default 640)");
    object_property_add_str(obj, "hostfs", syborg_get_hostfs, syborg_set_hostfs);
    object_property_set_description(obj, "hostfs", "Host folder shown to the guest as E:");
}

static void syborg_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Syborg (Symbian Virtual Platform, Cortex-A8)";
    mc->init = syborg_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a8");
    mc->default_ram_size = 512 * MiB;  /* the bootstrap assumes 512 MiB, as the 0.9.1 DTS gave */
    mc->default_ram_id = "syborg.ram";
    mc->block_default_type = IF_MTD;
    mc->ignore_memory_transaction_failures = true;
    mc->max_cpus = 1;
}

static const TypeInfo syborg_types[] = {
    {
        .name = TYPE_SYBORG_PLATFORM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SyborgPlatformState),
        .instance_init = syborg_platform_init,
    }, {
        .name = TYPE_SYBORG_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(SyborgMachineState),
        .instance_init = syborg_machine_instance_init,
        .class_init = syborg_machine_class_init,
        .interfaces = arm_machine_interfaces,
    },
};

DEFINE_TYPES(syborg_types)
