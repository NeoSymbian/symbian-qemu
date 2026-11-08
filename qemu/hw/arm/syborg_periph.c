/*
 * Syborg (Symbian Virtual Platform) peripherals: interrupt controller, timer, RTC, serial port,
 * keyboard, touchscreen pointer and the NVMemory disk.
 *
 * The register models follow CodeSourcery's Syborg devices (QEMU 0.11-1.0, MIT) and, for the
 * NVMemory disk, the register description in the Symbian baseport (baseport/syborg/mednvmemory).
 * Every register is a 32-bit word at (index * 4); unknown offsets read as 0 and are logged.
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
#include "hw/core/ptimer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "chardev/char-fe.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/rtc.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "ui/input.h"
#include "qom/object.h"

#define SYBORG_OPS(prefix) \
    static const MemoryRegionOps prefix##_ops = { \
        .read = prefix##_read, \
        .write = prefix##_write, \
        .endianness = DEVICE_LITTLE_ENDIAN, \
        .impl.min_access_size = 4, \
        .impl.max_access_size = 4, \
    }

static void syborg_bad(const char *dev, const char *op, hwaddr offset)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad %s at offset 0x%" HWADDR_PRIx "\n",
                  dev, op, offset);
}

static void syborg_init_mmio(Object *obj, MemoryRegion *mr, const MemoryRegionOps *ops,
                             void *opaque, const char *name)
{
    memory_region_init_io(mr, obj, ops, opaque, name, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), mr);
}

/* ---- interrupt controller: 32 level-sensitive inputs, one output to the CPU's IRQ ---- */

OBJECT_DECLARE_SIMPLE_TYPE(SyborgIntcState, SYBORG_INTC)

struct SyborgIntcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq parent_irq;
    uint32_t level;
    uint32_t enabled;
};

enum { INT_ID, INT_STATUS, INT_CURRENT, INT_DISABLE_ALL, INT_DISABLE, INT_ENABLE, INT_TOTAL };

static void syborg_intc_update(SyborgIntcState *s)
{
    /* The guest dispatcher indexes its handler table with CURRENT unchecked: the line is
     * high exactly when CURRENT names a pending, enabled input. */
    qemu_set_irq(s->parent_irq, (s->level & s->enabled) != 0);
}

static void syborg_intc_set_irq(void *opaque, int n, int level)
{
    SyborgIntcState *s = opaque;

    s->level = deposit32(s->level, n, 1, level != 0);
    syborg_intc_update(s);
}

static uint64_t syborg_intc_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgIntcState *s = opaque;
    uint32_t pending = s->level & s->enabled;

    switch (offset >> 2) {
    case INT_ID:
        return SYBORG_ID_INT;
    case INT_STATUS:
        return ctpop32(pending);
    case INT_CURRENT:
        return pending ? ctz32(pending) : 0xffffffffu;
    case INT_TOTAL:
        return 32;
    }
    syborg_bad(TYPE_SYBORG_INTC, "read", offset);
    return 0;
}

static void syborg_intc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgIntcState *s = opaque;

    switch (offset >> 2) {
    case INT_DISABLE_ALL:
        s->enabled = 0;
        break;
    case INT_DISABLE:
        if (value < 32) {
            s->enabled &= ~(1u << value);
        }
        break;
    case INT_ENABLE:
        if (value < 32) {
            s->enabled |= 1u << value;
        }
        break;
    default:
        syborg_bad(TYPE_SYBORG_INTC, "write", offset);
        return;
    }
    syborg_intc_update(s);
}

SYBORG_OPS(syborg_intc);

static void syborg_intc_init(Object *obj)
{
    SyborgIntcState *s = SYBORG_INTC(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->parent_irq);
    qdev_init_gpio_in(DEVICE(obj), syborg_intc_set_irq, 32);
    syborg_init_mmio(obj, &s->iomem, &syborg_intc_ops, s, TYPE_SYBORG_INTC);
}

static void syborg_intc_reset(DeviceState *dev)
{
    SYBORG_INTC(dev)->enabled = 0;
}

static const VMStateDescription vmstate_syborg_intc = {
    .name = TYPE_SYBORG_INTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(level, SyborgIntcState),
        VMSTATE_UINT32(enabled, SyborgIntcState),
        VMSTATE_END_OF_LIST()
    }
};

static void syborg_intc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_syborg_intc;
    device_class_set_legacy_reset(dc, syborg_intc_reset);
}

/* ---- interval timer ---- */

OBJECT_DECLARE_SIMPLE_TYPE(SyborgTimerState, SYBORG_TIMER)

struct SyborgTimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    ptimer_state *timer;
    qemu_irq irq;
    uint32_t freq;
    uint32_t running;
    uint32_t oneshot;
    uint32_t limit;
    uint32_t int_level;
    uint32_t int_enabled;
};

enum {
    TIMER_ID, TIMER_RUNNING, TIMER_ONESHOT, TIMER_LIMIT, TIMER_VALUE,
    TIMER_INT_ENABLE, TIMER_INT_STATUS, TIMER_FREQ
};

static void syborg_timer_update(SyborgTimerState *s)
{
    qemu_set_irq(s->irq, s->int_level && s->int_enabled);
}

static void syborg_timer_tick(void *opaque)
{
    SyborgTimerState *s = opaque;

    s->int_level = 1;
    if (s->oneshot) {
        s->running = 0;
    }
    syborg_timer_update(s);
}

static uint64_t syborg_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgTimerState *s = opaque;

    switch (offset >> 2) {
    case TIMER_ID:
        return SYBORG_ID_TIMER;
    case TIMER_RUNNING:
        return s->running;
    case TIMER_ONESHOT:
        return s->oneshot;
    case TIMER_LIMIT:
        return s->limit;
    case TIMER_VALUE:
        return ptimer_get_count(s->timer);
    case TIMER_INT_ENABLE:
        return s->int_enabled;
    case TIMER_INT_STATUS:
        return s->int_level;
    case TIMER_FREQ:
        return s->freq;
    }
    syborg_bad(TYPE_SYBORG_TIMER, "read", offset);
    return 0;
}

static void syborg_timer_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgTimerState *s = opaque;

    ptimer_transaction_begin(s->timer);
    switch (offset >> 2) {
    case TIMER_RUNNING:
        if (s->running != (value != 0)) {
            s->running = value != 0;
            if (s->running) {
                ptimer_run(s->timer, s->oneshot);
            } else {
                ptimer_stop(s->timer);
            }
        }
        break;
    case TIMER_ONESHOT:
        if (s->running) {
            ptimer_stop(s->timer);
        }
        s->oneshot = value != 0;
        if (s->running) {
            ptimer_run(s->timer, s->oneshot);
        }
        break;
    case TIMER_LIMIT:
        s->limit = value;
        ptimer_set_limit(s->timer, value, 1);
        break;
    case TIMER_VALUE:
        ptimer_set_count(s->timer, value);
        break;
    case TIMER_INT_ENABLE:
        s->int_enabled = value & 1;
        break;
    case TIMER_INT_STATUS:
        s->int_level &= ~value;
        break;
    default:
        syborg_bad(TYPE_SYBORG_TIMER, "write", offset);
        break;
    }
    ptimer_transaction_commit(s->timer);
    syborg_timer_update(s);
}

SYBORG_OPS(syborg_timer);

static void syborg_timer_init(Object *obj)
{
    SyborgTimerState *s = SYBORG_TIMER(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    syborg_init_mmio(obj, &s->iomem, &syborg_timer_ops, s, TYPE_SYBORG_TIMER);
}

static void syborg_timer_realize(DeviceState *dev, Error **errp)
{
    SyborgTimerState *s = SYBORG_TIMER(dev);

    if (!s->freq) {
        error_setg(errp, "syborg-timer: frequency must be set");
        return;
    }
    s->timer = ptimer_init(syborg_timer_tick, s, PTIMER_POLICY_LEGACY);
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, s->freq);
    ptimer_transaction_commit(s->timer);
}

static const Property syborg_timer_properties[] = {
    DEFINE_PROP_UINT32("frequency", SyborgTimerState, freq, 100000),
};

static const VMStateDescription vmstate_syborg_timer = {
    .name = TYPE_SYBORG_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(running, SyborgTimerState),
        VMSTATE_UINT32(oneshot, SyborgTimerState),
        VMSTATE_UINT32(limit, SyborgTimerState),
        VMSTATE_UINT32(int_level, SyborgTimerState),
        VMSTATE_UINT32(int_enabled, SyborgTimerState),
        VMSTATE_PTIMER(timer, SyborgTimerState),
        VMSTATE_END_OF_LIST()
    }
};

static void syborg_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = syborg_timer_realize;
    dc->vmsd = &vmstate_syborg_timer;
    device_class_set_props(dc, syborg_timer_properties);
}

/* ---- real-time clock: a 64-bit nanosecond counter, latched in ns, us, ms or s ---- */

OBJECT_DECLARE_SIMPLE_TYPE(SyborgRtcState, SYBORG_RTC)

struct SyborgRtcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    int64_t offset;
    int64_t data;
};

enum { RTC_ID, RTC_LATCH, RTC_DATA_LOW, RTC_DATA_HIGH };

static uint64_t syborg_rtc_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgRtcState *s = opaque;

    switch (offset >> 2) {
    case RTC_ID:
        return SYBORG_ID_RTC;
    case RTC_DATA_LOW:
        return (uint32_t)s->data;
    case RTC_DATA_HIGH:
        return (uint32_t)(s->data >> 32);
    }
    syborg_bad(TYPE_SYBORG_RTC, "read", offset);
    return 0;
}

static void syborg_rtc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgRtcState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    switch (offset >> 2) {
    case RTC_LATCH:
        if (value >= 4) {
            s->offset = s->data - now;          /* set the time from DATA */
        } else {
            s->data = now + s->offset;          /* value: 0 ns, 1 us, 2 ms, 3 s */
            while (value--) {
                s->data /= 1000;
            }
        }
        break;
    case RTC_DATA_LOW:
        s->data = deposit64(s->data, 0, 32, value);
        break;
    case RTC_DATA_HIGH:
        s->data = deposit64(s->data, 32, 32, value);
        break;
    default:
        syborg_bad(TYPE_SYBORG_RTC, "write", offset);
        break;
    }
}

SYBORG_OPS(syborg_rtc);

static void syborg_rtc_init(Object *obj)
{
    SyborgRtcState *s = SYBORG_RTC(obj);

    syborg_init_mmio(obj, &s->iomem, &syborg_rtc_ops, s, TYPE_SYBORG_RTC);
}

static void syborg_rtc_realize(DeviceState *dev, Error **errp)
{
    SyborgRtcState *s = SYBORG_RTC(dev);
    struct tm tm;

    qemu_get_timedate(&tm, 0);
    s->offset = (int64_t)mktimegm(&tm) * NANOSECONDS_PER_SECOND -
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static const VMStateDescription vmstate_syborg_rtc = {
    .name = TYPE_SYBORG_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(offset, SyborgRtcState),
        VMSTATE_INT64(data, SyborgRtcState),
        VMSTATE_END_OF_LIST()
    }
};

static void syborg_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = syborg_rtc_realize;
    dc->vmsd = &vmstate_syborg_rtc;
}

/* ---- serial port (FIFO mode; the guest drivers never use the DMA registers) ---- */

#define SYBORG_FIFO 16

OBJECT_DECLARE_SIMPLE_TYPE(SyborgSerialState, SYBORG_SERIAL)

struct SyborgSerialState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq;
    uint32_t int_enable;
    uint32_t fifo[SYBORG_FIFO];
    uint32_t pos;
    uint32_t count;
};

enum {
    SERIAL_ID, SERIAL_DATA, SERIAL_FIFO_COUNT, SERIAL_INT_ENABLE,
    SERIAL_FIFO_SIZE = 8
};

static void syborg_serial_update(SyborgSerialState *s)
{
    qemu_set_irq(s->irq, (s->int_enable & 1) && s->count);
}

static uint64_t syborg_serial_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgSerialState *s = opaque;
    uint32_t c;

    switch (offset >> 2) {
    case SERIAL_ID:
        return SYBORG_ID_SERIAL;
    case SERIAL_DATA:
        if (!s->count) {
            return 0xffffffffu;
        }
        c = s->fifo[s->pos];
        s->pos = (s->pos + 1) % SYBORG_FIFO;
        s->count--;
        syborg_serial_update(s);
        qemu_chr_fe_accept_input(&s->chr);
        return c;
    case SERIAL_FIFO_COUNT:
        return s->count;
    case SERIAL_INT_ENABLE:
        return s->int_enable;
    case SERIAL_FIFO_SIZE:
        return SYBORG_FIFO;
    }
    syborg_bad(TYPE_SYBORG_SERIAL, "read", offset);
    return 0;
}

static void syborg_serial_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgSerialState *s = opaque;
    uint8_t ch = value;

    switch (offset >> 2) {
    case SERIAL_DATA:
        /* The kernel's debug output: blocking is fine, nothing is waiting on it. */
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        break;
    case SERIAL_INT_ENABLE:
        s->int_enable = value;
        syborg_serial_update(s);
        break;
    default:
        syborg_bad(TYPE_SYBORG_SERIAL, "write", offset);
        break;
    }
}

SYBORG_OPS(syborg_serial);

static int syborg_serial_can_receive(void *opaque)
{
    SyborgSerialState *s = opaque;

    return SYBORG_FIFO - s->count;
}

static void syborg_serial_receive(void *opaque, const uint8_t *buf, int size)
{
    SyborgSerialState *s = opaque;

    for (int i = 0; i < size && s->count < SYBORG_FIFO; i++) {
        s->fifo[(s->pos + s->count++) % SYBORG_FIFO] = buf[i];
    }
    syborg_serial_update(s);
}

static void syborg_serial_init(Object *obj)
{
    SyborgSerialState *s = SYBORG_SERIAL(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    syborg_init_mmio(obj, &s->iomem, &syborg_serial_ops, s, TYPE_SYBORG_SERIAL);
}

static void syborg_serial_realize(DeviceState *dev, Error **errp)
{
    SyborgSerialState *s = SYBORG_SERIAL(dev);

    qemu_chr_fe_set_handlers(&s->chr, syborg_serial_can_receive, syborg_serial_receive,
                             NULL, NULL, s, NULL, true);
}

static const Property syborg_serial_properties[] = {
    DEFINE_PROP_CHR("chardev", SyborgSerialState, chr),
};

static const VMStateDescription vmstate_syborg_serial = {
    .name = TYPE_SYBORG_SERIAL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(int_enable, SyborgSerialState),
        VMSTATE_UINT32_ARRAY(fifo, SyborgSerialState, SYBORG_FIFO),
        VMSTATE_UINT32(pos, SyborgSerialState),
        VMSTATE_UINT32(count, SyborgSerialState),
        VMSTATE_END_OF_LIST()
    }
};

static void syborg_serial_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = syborg_serial_realize;
    dc->vmsd = &vmstate_syborg_serial;
    device_class_set_props(dc, syborg_serial_properties);
}

/* ---- keyboard: PC set-1 scancodes, the 0xE0 prefix folded into bit 7, release in bit 31 ---- */

OBJECT_DECLARE_SIMPLE_TYPE(SyborgKeyboardState, SYBORG_KEYBOARD)

struct SyborgKeyboardState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t int_enabled;
    uint32_t fifo[SYBORG_FIFO];
    uint32_t pos;
    uint32_t count;
};

enum { KBD_ID, KBD_DATA, KBD_FIFO_COUNT, KBD_INT_ENABLE, KBD_FIFO_SIZE };

static void syborg_keyboard_update(SyborgKeyboardState *s)
{
    qemu_set_irq(s->irq, s->count && s->int_enabled);
}

static uint64_t syborg_keyboard_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgKeyboardState *s = opaque;
    uint32_t c;

    switch (offset >> 2) {
    case KBD_ID:
        return SYBORG_ID_KEYBOARD;
    case KBD_DATA:
        if (!s->count) {
            return 0xffffffffu;
        }
        c = s->fifo[s->pos];
        s->pos = (s->pos + 1) % SYBORG_FIFO;
        s->count--;
        syborg_keyboard_update(s);
        return c;
    case KBD_FIFO_COUNT:
        return s->count;
    case KBD_INT_ENABLE:
        return s->int_enabled;
    case KBD_FIFO_SIZE:
        return SYBORG_FIFO;
    }
    syborg_bad(TYPE_SYBORG_KEYBOARD, "read", offset);
    return 0;
}

static void syborg_keyboard_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgKeyboardState *s = opaque;

    if ((offset >> 2) == KBD_INT_ENABLE) {
        s->int_enabled = value;
        syborg_keyboard_update(s);
    } else {
        syborg_bad(TYPE_SYBORG_KEYBOARD, "write", offset);
    }
}

SYBORG_OPS(syborg_keyboard);

static void syborg_keyboard_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    SyborgKeyboardState *s = SYBORG_KEYBOARD(dev);
    unsigned int lnx = evt->key.key;
    /* qnum is set 1 with 0xE0-prefixed keys already in bit 7: exactly the Syborg code. */
    uint32_t code = lnx < qemu_input_map_linux_to_qnum_len ? qemu_input_map_linux_to_qnum[lnx] : 0;

    if (!code || code > 0xff) {
        return;
    }
    if (!evt->key.down) {
        code |= 0x80000000u;
    }
    if (s->count == SYBORG_FIFO) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: FIFO overflow\n", TYPE_SYBORG_KEYBOARD);
        return;
    }
    s->fifo[(s->pos + s->count++) % SYBORG_FIFO] = code;
    syborg_keyboard_update(s);
}

static const QemuInputHandler syborg_keyboard_handler = {
    .name = "Syborg keyboard",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = syborg_keyboard_event,
};

static void syborg_keyboard_init(Object *obj)
{
    SyborgKeyboardState *s = SYBORG_KEYBOARD(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    syborg_init_mmio(obj, &s->iomem, &syborg_keyboard_ops, s, TYPE_SYBORG_KEYBOARD);
}

static void syborg_keyboard_realize(DeviceState *dev, Error **errp)
{
    QemuInputHandlerState *hs = qemu_input_handler_register(dev, &syborg_keyboard_handler);

    (void)hs;   /* lives as long as the device */
}

static const VMStateDescription vmstate_syborg_keyboard = {
    .name = TYPE_SYBORG_KEYBOARD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(int_enabled, SyborgKeyboardState),
        VMSTATE_UINT32_ARRAY(fifo, SyborgKeyboardState, SYBORG_FIFO),
        VMSTATE_UINT32(pos, SyborgKeyboardState),
        VMSTATE_UINT32(count, SyborgKeyboardState),
        VMSTATE_END_OF_LIST()
    }
};

static void syborg_keyboard_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = syborg_keyboard_realize;
    dc->vmsd = &vmstate_syborg_keyboard;
}

/* ---- touchscreen: absolute 0..0x7FFF; LATCH moves the next FIFO entry into X/Y/Z/BUTTONS ---- */

OBJECT_DECLARE_SIMPLE_TYPE(SyborgPointerState, SYBORG_POINTER)

typedef struct SyborgPointerEvent {
    int32_t x, y, z, buttons;
} SyborgPointerEvent;

struct SyborgPointerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t int_enabled;
    /* fifo[pos] is what the registers show; count entries follow it. */
    SyborgPointerEvent fifo[SYBORG_FIFO];
    uint32_t pos;
    uint32_t count;
    SyborgPointerEvent cur;     /* the host pointer, sent to the FIFO on sync */
};

enum {
    POINTER_ID, POINTER_LATCH, POINTER_FIFO_COUNT, POINTER_X, POINTER_Y, POINTER_Z,
    POINTER_BUTTONS, POINTER_INT_ENABLE, POINTER_FIFO_SIZE
};

static void syborg_pointer_update(SyborgPointerState *s)
{
    qemu_set_irq(s->irq, s->count && s->int_enabled);
}

static uint64_t syborg_pointer_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgPointerState *s = opaque;
    SyborgPointerEvent *e = &s->fifo[s->pos];

    switch (offset >> 2) {
    case POINTER_ID:
        return SYBORG_ID_TOUCHSCREEN;
    case POINTER_FIFO_COUNT:
        return s->count;
    case POINTER_X:
        return e->x;
    case POINTER_Y:
        return e->y;
    case POINTER_Z:
        return e->z;
    case POINTER_BUTTONS:
        return e->buttons;
    case POINTER_INT_ENABLE:
        return s->int_enabled;
    case POINTER_FIFO_SIZE:
        return SYBORG_FIFO;
    }
    syborg_bad(TYPE_SYBORG_POINTER, "read", offset);
    return 0;
}

static void syborg_pointer_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgPointerState *s = opaque;

    switch (offset >> 2) {
    case POINTER_LATCH:
        if (s->count) {
            s->pos = (s->pos + 1) % SYBORG_FIFO;
            s->count--;
        }
        break;
    case POINTER_INT_ENABLE:
        s->int_enabled = value;
        break;
    default:
        syborg_bad(TYPE_SYBORG_POINTER, "write", offset);
        return;
    }
    syborg_pointer_update(s);
}

SYBORG_OPS(syborg_pointer);

static void syborg_pointer_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    SyborgPointerState *s = SYBORG_POINTER(dev);
    int bit;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
    /*
     * shortcut: HMP mouse_move only sends relative events in modern QEMU. Like QEMU 0.9.1 on
     * an absolute device, we take its values as absolute 0..0x7FFF, so taps.sh/mon.sh keep
     * working. Drop this case once every script sends QMP input-send-event abs instead.
     */
    case INPUT_EVENT_KIND_REL:
        if (evt->abs.axis == INPUT_AXIS_X) {
            s->cur.x = evt->abs.value;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            s->cur.y = evt->abs.value;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        bit = evt->btn.button == INPUT_BUTTON_LEFT ? 1 :
              evt->btn.button == INPUT_BUTTON_RIGHT ? 2 : 0;
        if (evt->btn.down) {
            s->cur.buttons |= bit;
        } else {
            s->cur.buttons &= ~bit;
        }
        break;
    default:
        break;
    }
}

static void syborg_pointer_sync(DeviceState *dev)
{
    SyborgPointerState *s = SYBORG_POINTER(dev);

    if (s->count < SYBORG_FIFO - 1) {
        s->count++;
    }
    /* A full FIFO overwrites its newest entry: the latest position wins. */
    s->fifo[(s->pos + s->count) % SYBORG_FIFO] = s->cur;
    syborg_pointer_update(s);
}

static const QemuInputHandler syborg_pointer_handler = {
    .name = "Syborg touchscreen",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_REL,
    .event = syborg_pointer_event,
    .sync = syborg_pointer_sync,
};

static void syborg_pointer_init(Object *obj)
{
    SyborgPointerState *s = SYBORG_POINTER(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    syborg_init_mmio(obj, &s->iomem, &syborg_pointer_ops, s, TYPE_SYBORG_POINTER);
}

static void syborg_pointer_realize(DeviceState *dev, Error **errp)
{
    QemuInputHandlerState *hs = qemu_input_handler_register(dev, &syborg_pointer_handler);

    (void)hs;
}

static const VMStateDescription vmstate_syborg_pointer_event = {
    .name = "syborg-pointer-event",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(x, SyborgPointerEvent),
        VMSTATE_INT32(y, SyborgPointerEvent),
        VMSTATE_INT32(z, SyborgPointerEvent),
        VMSTATE_INT32(buttons, SyborgPointerEvent),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_syborg_pointer = {
    .name = TYPE_SYBORG_POINTER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(int_enabled, SyborgPointerState),
        VMSTATE_STRUCT_ARRAY(fifo, SyborgPointerState, SYBORG_FIFO, 1,
                             vmstate_syborg_pointer_event, SyborgPointerEvent),
        VMSTATE_UINT32(pos, SyborgPointerState),
        VMSTATE_UINT32(count, SyborgPointerState),
        VMSTATE_STRUCT(cur, SyborgPointerState, 1, vmstate_syborg_pointer_event,
                       SyborgPointerEvent),
        VMSTATE_END_OF_LIST()
    }
};

static void syborg_pointer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = syborg_pointer_realize;
    dc->vmsd = &vmstate_syborg_pointer;
}

/* ---- NVMemory disk (drive C:): synchronous sector transfers through a guest buffer ---- */

OBJECT_DECLARE_SIMPLE_TYPE(SyborgNVMemoryState, SYBORG_NVMEMORY)

struct SyborgNVMemoryState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    BlockBackend *blk;
    qemu_irq irq;
    uint32_t sector_size;
    uint32_t offset;            /* in sectors */
    uint32_t count;             /* in sectors */
    uint32_t direction;
    uint32_t shm_base;          /* guest physical address of the transfer buffer */
    uint32_t shm_size;
    int32_t status;             /* sectors moved, or negative on error */
};

/* Register byte offsets (the baseport numbers them 0, 4, 8, ... and multiplies by 4). */
enum {
    NVMEM_ID = 0x00, NVMEM_OFFSET = 0x10, NVMEM_SIZE = 0x20, NVMEM_DIRECTION = 0x30,
    NVMEM_EXECUTE = 0x40, NVMEM_SHM_BASE = 0x50, NVMEM_NV_SIZE = 0x60, NVMEM_SHM_SIZE = 0x70,
    NVMEM_STATUS = 0x80, NVMEM_ENABLE = 0x90,
};
#define NVMEM_READ  1
#define NVMEM_WRITE 2

static uint32_t syborg_nvmemory_sectors(SyborgNVMemoryState *s)
{
    return s->blk ? blk_getlength(s->blk) / s->sector_size : 0;
}

static void syborg_nvmemory_execute(SyborgNVMemoryState *s)
{
    uint64_t bytes = (uint64_t)s->count * s->sector_size;
    int64_t pos = (int64_t)s->offset * s->sector_size;
    g_autofree uint8_t *buf = NULL;
    int ret = -1;

    if (!s->blk || !s->count || bytes > s->shm_size ||
        s->offset + s->count > syborg_nvmemory_sectors(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad transfer of %u sectors at %u\n",
                      TYPE_SYBORG_NVMEMORY, s->count, s->offset);
    } else {
        buf = g_malloc(bytes);
        if (s->direction == NVMEM_READ) {
            ret = blk_pread(s->blk, pos, bytes, buf, 0);
            if (ret >= 0) {
                address_space_write(&address_space_memory, s->shm_base,
                                    MEMTXATTRS_UNSPECIFIED, buf, bytes);
            }
        } else if (s->direction == NVMEM_WRITE) {
            address_space_read(&address_space_memory, s->shm_base,
                               MEMTXATTRS_UNSPECIFIED, buf, bytes);
            ret = blk_pwrite(s->blk, pos, bytes, buf, 0);
        }
    }
    s->status = ret < 0 ? -1 : s->count;
    qemu_irq_raise(s->irq);
}

static uint64_t syborg_nvmemory_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgNVMemoryState *s = opaque;

    switch (offset) {
    case NVMEM_ID:
        return SYBORG_ID_NVMEMORY;
    case NVMEM_OFFSET:
        return s->offset;
    case NVMEM_SIZE:
        return s->count;
    case NVMEM_DIRECTION:
        return s->direction;
    case NVMEM_SHM_BASE:
        return s->shm_base;
    case NVMEM_NV_SIZE:
        return syborg_nvmemory_sectors(s);
    case NVMEM_SHM_SIZE:
        return s->shm_size;
    case NVMEM_STATUS:
        qemu_irq_lower(s->irq);
        return (uint32_t)s->status;
    }
    syborg_bad(TYPE_SYBORG_NVMEMORY, "read", offset);
    return 0;
}

static void syborg_nvmemory_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgNVMemoryState *s = opaque;

    switch (offset) {
    case NVMEM_OFFSET:
        s->offset = value;
        break;
    case NVMEM_SIZE:
        s->count = value;
        break;
    case NVMEM_DIRECTION:
        s->direction = value;
        break;
    case NVMEM_EXECUTE:
        syborg_nvmemory_execute(s);
        break;
    case NVMEM_SHM_BASE:
        s->shm_base = value;
        break;
    case NVMEM_SHM_SIZE:
        s->shm_size = value;
        break;
    case NVMEM_ENABLE:
        break;                  /* the buffer is read through the address space on each transfer */
    default:
        syborg_bad(TYPE_SYBORG_NVMEMORY, "write", offset);
        break;
    }
}

SYBORG_OPS(syborg_nvmemory);

static void syborg_nvmemory_init(Object *obj)
{
    SyborgNVMemoryState *s = SYBORG_NVMEMORY(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    syborg_init_mmio(obj, &s->iomem, &syborg_nvmemory_ops, s, TYPE_SYBORG_NVMEMORY);
}

static void syborg_nvmemory_realize(DeviceState *dev, Error **errp)
{
    SyborgNVMemoryState *s = SYBORG_NVMEMORY(dev);

    if (!s->sector_size) {
        error_setg(errp, "%s: sector-size must not be 0", TYPE_SYBORG_NVMEMORY);
        return;
    }
    if (s->blk) {
        uint64_t perm = BLK_PERM_CONSISTENT_READ |
                        (blk_supports_write_perm(s->blk) ? BLK_PERM_WRITE : 0);

        blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp);
    }
}

static const Property syborg_nvmemory_properties[] = {
    DEFINE_PROP_DRIVE("drive", SyborgNVMemoryState, blk),
    DEFINE_PROP_UINT32("sector-size", SyborgNVMemoryState, sector_size, 512),
};

static const VMStateDescription vmstate_syborg_nvmemory = {
    .name = TYPE_SYBORG_NVMEMORY,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(offset, SyborgNVMemoryState),
        VMSTATE_UINT32(count, SyborgNVMemoryState),
        VMSTATE_UINT32(direction, SyborgNVMemoryState),
        VMSTATE_UINT32(shm_base, SyborgNVMemoryState),
        VMSTATE_UINT32(shm_size, SyborgNVMemoryState),
        VMSTATE_INT32(status, SyborgNVMemoryState),
        VMSTATE_END_OF_LIST()
    }
};

static void syborg_nvmemory_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = syborg_nvmemory_realize;
    dc->vmsd = &vmstate_syborg_nvmemory;
    device_class_set_props(dc, syborg_nvmemory_properties);
}

#define SYBORG_TYPE(tname, State, prefix) { \
    .name = tname, .parent = TYPE_SYS_BUS_DEVICE, .instance_size = sizeof(State), \
    .instance_init = prefix##_init, .class_init = prefix##_class_init, \
}

static const TypeInfo syborg_periph_types[] = {
    SYBORG_TYPE(TYPE_SYBORG_INTC, SyborgIntcState, syborg_intc),
    SYBORG_TYPE(TYPE_SYBORG_TIMER, SyborgTimerState, syborg_timer),
    SYBORG_TYPE(TYPE_SYBORG_RTC, SyborgRtcState, syborg_rtc),
    SYBORG_TYPE(TYPE_SYBORG_SERIAL, SyborgSerialState, syborg_serial),
    SYBORG_TYPE(TYPE_SYBORG_KEYBOARD, SyborgKeyboardState, syborg_keyboard),
    SYBORG_TYPE(TYPE_SYBORG_POINTER, SyborgPointerState, syborg_pointer),
    SYBORG_TYPE(TYPE_SYBORG_NVMEMORY, SyborgNVMemoryState, syborg_nvmemory),
};

DEFINE_TYPES(syborg_periph_types)
