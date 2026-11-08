/*
 * Syborg (Symbian Virtual Platform) board: device IDs and type names.
 *
 * Copyright (c) 2008 CodeSourcery
 * Copyright (c) 2026 The NeoSymbian Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef HW_ARM_SYBORG_H
#define HW_ARM_SYBORG_H

/* Value of each device's ID register (offset 0), as the Symbian baseport expects. */
#define SYBORG_ID_PLATFORM    0xc51d1000
#define SYBORG_ID_INT         0xc51d0000
#define SYBORG_ID_SERIAL      0xc51d0001
#define SYBORG_ID_KEYBOARD    0xc51d0002
#define SYBORG_ID_TIMER       0xc51d0003
#define SYBORG_ID_RTC         0xc51d0004
#define SYBORG_ID_TOUCHSCREEN 0xc51d0006
#define SYBORG_ID_FRAMEBUFFER 0xc51d0007
#define SYBORG_ID_HOSTFS      0xc51d0008
#define SYBORG_ID_NVMEMORY    0xdeadbeef

#define TYPE_SYBORG_INTC      "syborg-intc"
#define TYPE_SYBORG_TIMER     "syborg-timer"
#define TYPE_SYBORG_RTC       "syborg-rtc"
#define TYPE_SYBORG_SERIAL    "syborg-serial"
#define TYPE_SYBORG_KEYBOARD  "syborg-keyboard"
#define TYPE_SYBORG_POINTER   "syborg-pointer"
#define TYPE_SYBORG_FB        "syborg-fb"
#define TYPE_SYBORG_NVMEMORY  "syborg-nvmemory"
#define TYPE_SYBORG_PLATFORM  "syborg-platform"
#define TYPE_SYBORG_HOSTFS    "syborg-hostfs"

#endif
