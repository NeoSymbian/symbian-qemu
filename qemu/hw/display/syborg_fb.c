/*
 * Syborg (Symbian Virtual Platform) framebuffer.
 *
 * The register set follows CodeSourcery's Syborg framebuffer (QEMU 0.11-1.0, MIT). The Symbian
 * baseport only uses 32 bpp XRGB words (COLOR_ORDER 0) and reads the size from WIDTH/HEIGHT,
 * which the board sets from its width/height properties.
 *
 * Copyright (c) 2008 CodeSourcery
 * Copyright (c) 2026 The NeoSymbian Authors
 * SPDX-License-Identifier: MIT
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/syborg.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "ui/console.h"
#include "framebuffer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(SyborgFbState, SYBORG_FB)

struct SyborgFbState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegionSection fbsection;
    QemuConsole *con;
    qemu_irq irq;
    uint32_t width;
    uint32_t height;
    uint32_t base;
    uint32_t orientation;
    uint32_t blank;
    uint32_t int_mask;
    uint32_t int_status;
    uint32_t bpp;
    uint32_t color_order;
    uint32_t byte_order;
    uint32_t pixel_order;
    uint32_t enabled;
    uint32_t palette[256];
    int32_t invalidate;
};

enum {
    FB_ID, FB_BASE, FB_HEIGHT, FB_WIDTH, FB_ORIENTATION, FB_BLANK, FB_INT_MASK,
    FB_INTERRUPT_CAUSE, FB_BPP, FB_COLOR_ORDER, FB_BYTE_ORDER, FB_PIXEL_ORDER, FB_ROW_PITCH,
    FB_ENABLED, FB_PALETTE_START = 0x400 >> 2, FB_PALETTE_END = FB_PALETTE_START + 255,
};
#define FB_INT_VSYNC            (1u << 0)
#define FB_INT_BASE_UPDATE_DONE (1u << 1)

static void syborg_fb_update_irq(SyborgFbState *s)
{
    qemu_set_irq(s->irq, (s->int_status & s->int_mask) != 0);
}

/* shortcut: guest XRGB words are copied as they are, which is right on little-endian hosts only. */
static void syborg_fb_draw_line(void *opaque, uint8_t *d, const uint8_t *src, int width,
                                int deststep)
{
    memcpy(d, src, width * 4);
}

static bool syborg_fb_update_display(void *opaque)
{
    SyborgFbState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first = 0, last;    /* in: the first row to draw; out: the first row drawn or -1 */

    if (!s->enabled || s->blank) {
        return true;
    }
    if (s->bpp != 32) {
        qemu_log_mask(LOG_UNIMP, "%s: only 32 bpp is implemented (bpp %u)\n", TYPE_SYBORG_FB,
                      s->bpp);
        return true;
    }
    if (surface_width(surface) != s->width || surface_height(surface) != s->height) {
        qemu_console_resize(s->con, s->width, s->height);
        surface = qemu_console_surface(s->con);
        s->invalidate = 1;
    }
    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection, get_system_memory(), s->base,
                                          s->height, s->width * 4);
    }
    framebuffer_update_display(surface, &s->fbsection, s->width, s->height, s->width * 4,
                               surface_stride(surface), 0, s->invalidate,
                               syborg_fb_draw_line, NULL, &first, &last);
    if (first >= 0) {
        qemu_console_update(s->con, 0, first, s->width, last - first + 1);
    }
    s->invalidate = 0;
    s->int_status |= FB_INT_VSYNC;
    syborg_fb_update_irq(s);
    return true;
}

static void syborg_fb_invalidate_display(void *opaque)
{
    SYBORG_FB(opaque)->invalidate = 1;
}

static const GraphicHwOps syborg_fb_gfx_ops = {
    .invalidate = syborg_fb_invalidate_display,
    .gfx_update = syborg_fb_update_display,
};

static uint64_t syborg_fb_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgFbState *s = opaque;
    unsigned reg = offset >> 2;

    switch (reg) {
    case FB_ID:
        return SYBORG_ID_FRAMEBUFFER;
    case FB_BASE:
        return s->base;
    case FB_HEIGHT:
        return s->height;
    case FB_WIDTH:
        return s->width;
    case FB_ORIENTATION:
        return s->orientation;
    case FB_BLANK:
        return s->blank;
    case FB_INT_MASK:
        return s->int_mask;
    case FB_INTERRUPT_CAUSE:
        return s->int_status;
    case FB_BPP:
        return s->bpp;
    case FB_COLOR_ORDER:
        return s->color_order;
    case FB_BYTE_ORDER:
        return s->byte_order;
    case FB_PIXEL_ORDER:
        return s->pixel_order;
    case FB_ROW_PITCH:
        return s->width * 4;
    case FB_ENABLED:
        return s->enabled;
    }
    if (reg >= FB_PALETTE_START && reg <= FB_PALETTE_END) {
        return s->palette[reg - FB_PALETTE_START];
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read at 0x%" HWADDR_PRIx "\n", TYPE_SYBORG_FB,
                  offset);
    return 0;
}

static void syborg_fb_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgFbState *s = opaque;
    unsigned reg = offset >> 2;

    s->invalidate = 1;
    switch (reg) {
    case FB_BASE:
        s->base = value;
        s->int_status |= FB_INT_BASE_UPDATE_DONE;
        break;
    case FB_HEIGHT:
        s->height = value;
        break;
    case FB_WIDTH:
        s->width = value;
        break;
    case FB_ORIENTATION:
        s->orientation = value;
        break;
    case FB_BLANK:
        s->blank = value & 1;
        break;
    case FB_INT_MASK:
        s->int_mask = value;
        break;
    case FB_INTERRUPT_CAUSE:
        s->int_status &= ~value;
        break;
    case FB_BPP:
        s->bpp = value;
        break;
    case FB_COLOR_ORDER:
        s->color_order = value;
        break;
    case FB_BYTE_ORDER:
        s->byte_order = value;
        break;
    case FB_PIXEL_ORDER:
        s->pixel_order = value;
        break;
    case FB_ENABLED:
        s->enabled = value;
        break;
    default:
        if (reg >= FB_PALETTE_START && reg <= FB_PALETTE_END) {
            s->palette[reg - FB_PALETTE_START] = value;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write at 0x%" HWADDR_PRIx "\n",
                          TYPE_SYBORG_FB, offset);
        }
        break;
    }
    syborg_fb_update_irq(s);
}

static const MemoryRegionOps syborg_fb_ops = {
    .read = syborg_fb_read,
    .write = syborg_fb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void syborg_fb_init(Object *obj)
{
    SyborgFbState *s = SYBORG_FB(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &syborg_fb_ops, s, TYPE_SYBORG_FB, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void syborg_fb_realize(DeviceState *dev, Error **errp)
{
    SyborgFbState *s = SYBORG_FB(dev);

    if (s->width < 16 || s->height < 16 || s->width > 4096 || s->height > 4096) {
        error_setg(errp, "%s: width and height must be 16..4096", TYPE_SYBORG_FB);
        return;
    }
    s->bpp = 32;
    s->con = qemu_graphic_console_create(dev, 0, &syborg_fb_gfx_ops, s);
    qemu_console_resize(s->con, s->width, s->height);
}

static const Property syborg_fb_properties[] = {
    DEFINE_PROP_UINT32("width", SyborgFbState, width, 768),
    DEFINE_PROP_UINT32("height", SyborgFbState, height, 640),
};

static int syborg_fb_post_load(void *opaque, int version_id)
{
    SYBORG_FB(opaque)->invalidate = 1;
    return 0;
}

static const VMStateDescription vmstate_syborg_fb = {
    .name = TYPE_SYBORG_FB,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = syborg_fb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(width, SyborgFbState),
        VMSTATE_UINT32(height, SyborgFbState),
        VMSTATE_UINT32(base, SyborgFbState),
        VMSTATE_UINT32(orientation, SyborgFbState),
        VMSTATE_UINT32(blank, SyborgFbState),
        VMSTATE_UINT32(int_mask, SyborgFbState),
        VMSTATE_UINT32(int_status, SyborgFbState),
        VMSTATE_UINT32(bpp, SyborgFbState),
        VMSTATE_UINT32(color_order, SyborgFbState),
        VMSTATE_UINT32(byte_order, SyborgFbState),
        VMSTATE_UINT32(pixel_order, SyborgFbState),
        VMSTATE_UINT32(enabled, SyborgFbState),
        VMSTATE_UINT32_ARRAY(palette, SyborgFbState, 256),
        VMSTATE_END_OF_LIST()
    }
};

static void syborg_fb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = syborg_fb_realize;
    dc->vmsd = &vmstate_syborg_fb;
    device_class_set_props(dc, syborg_fb_properties);
}

static const TypeInfo syborg_fb_types[] = {
    {
        .name = TYPE_SYBORG_FB,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SyborgFbState),
        .instance_init = syborg_fb_init,
        .class_init = syborg_fb_class_init,
    },
};

DEFINE_TYPES(syborg_fb_types)
