/*
 * QTest for the Syborg board: the device behaviour the Symbian baseport relies on.
 *
 * Copyright (c) 2026 The NeoSymbian Authors
 * SPDX-License-Identifier: MIT
 */
#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "qemu/bswap.h"

#define INTC   0xc0000000
#define TIMER  0xc0002000
#define KBD    0xc0003000
#define PTR    0xc0004000
#define FB     0xc0005000
#define NVMEM  0xc0010000
#define HOSTFS 0xc000a000

#define R(base, n) ((base) + (n) * 4)

static char disk[] = "/tmp/syborg-test-XXXXXX";

/* The tick timer raises IRQ 1; CURRENT names it exactly while the IRQ is pending and enabled. */
static void test_timer_and_intc(void)
{
    g_assert_cmphex(readl(R(INTC, 0)), ==, 0xc51d0000);
    g_assert_cmphex(readl(R(TIMER, 0)), ==, 0xc51d0003);
    g_assert_cmpuint(readl(R(TIMER, 7)), ==, 100000);          /* FREQ */

    writel(R(INTC, 3), 0);                                      /* DISABLE_ALL */
    writel(R(INTC, 5), 1);                                      /* ENABLE 1 */
    writel(R(TIMER, 3), 1000);                                  /* LIMIT: 10 ms at 100 kHz */
    writel(R(TIMER, 5), 1);                                     /* INT_ENABLE */
    writel(R(TIMER, 1), 1);                                     /* RUNNING */
    g_assert_cmpuint(readl(R(INTC, 1)), ==, 0);
    g_assert_cmphex(readl(R(INTC, 2)), ==, 0xffffffff);

    clock_step(10 * 1000 * 1000 + 1);
    g_assert_cmpuint(readl(R(TIMER, 6)), ==, 1);                /* INT_STATUS */
    g_assert_cmpuint(readl(R(INTC, 1)), ==, 1);                 /* STATUS */
    g_assert_cmpuint(readl(R(INTC, 2)), ==, 1);                 /* CURRENT */

    writel(R(INTC, 4), 1);                                      /* DISABLE 1 */
    g_assert_cmpuint(readl(R(INTC, 1)), ==, 0);
    g_assert_cmphex(readl(R(INTC, 2)), ==, 0xffffffff);
    writel(R(INTC, 5), 1);
    g_assert_cmpuint(readl(R(INTC, 2)), ==, 1);

    writel(R(TIMER, 6), 1);                                     /* ack: write 1 to clear */
    g_assert_cmpuint(readl(R(INTC, 1)), ==, 0);
    g_assert_cmphex(readl(R(INTC, 2)), ==, 0xffffffff);
    writel(R(TIMER, 1), 0);
}

/* Set-1 scancodes, 0xE0 keys in bit 7, release in bit 31; empty FIFO reads all ones. */
static void test_keyboard(void)
{
    writel(R(KBD, 3), 1);
    qtest_qmp_assert_success(global_qtest,
        "{'execute': 'send-key', 'arguments': {'keys': [{'type': 'qcode', 'data': 'a'}]}}");
    qtest_qmp_assert_success(global_qtest,
        "{'execute': 'send-key', 'arguments': {'keys': [{'type': 'qcode', 'data': 'up'}]}}");
    clock_step(1000 * 1000 * 1000);
    g_assert_cmpuint(readl(R(KBD, 2)), ==, 4);
    g_assert_cmphex(readl(R(KBD, 1)), ==, 0x1e);
    g_assert_cmphex(readl(R(KBD, 1)), ==, 0x8000001e);
    g_assert_cmphex(readl(R(KBD, 1)), ==, 0xc8);                 /* E0 48 */
    g_assert_cmphex(readl(R(KBD, 1)), ==, 0x800000c8);
    g_assert_cmphex(readl(R(KBD, 1)), ==, 0xffffffff);
}

/* Absolute events, latched one at a time; a button press keeps the last position. */
static void test_pointer(void)
{
    g_assert_cmphex(readl(R(PTR, 0)), ==, 0xc51d0006);
    writel(R(PTR, 7), 1);
    qtest_qmp_assert_success(global_qtest,
        "{'execute': 'input-send-event', 'arguments': {'events': ["
        " {'type': 'abs', 'data': {'axis': 'x', 'value': 1000}},"
        " {'type': 'abs', 'data': {'axis': 'y', 'value': 2000}}]}}");
    qtest_qmp_assert_success(global_qtest,
        "{'execute': 'input-send-event', 'arguments': {'events': ["
        " {'type': 'btn', 'data': {'down': true, 'button': 'left'}}]}}");
    g_assert_cmpuint(readl(R(PTR, 2)), ==, 2);                  /* FIFO_COUNT */
    writel(R(PTR, 1), 1);                                       /* LATCH */
    g_assert_cmpuint(readl(R(PTR, 3)), ==, 1000);
    g_assert_cmpuint(readl(R(PTR, 4)), ==, 2000);
    g_assert_cmpuint(readl(R(PTR, 6)), ==, 0);
    writel(R(PTR, 1), 1);
    g_assert_cmpuint(readl(R(PTR, 3)), ==, 1000);
    g_assert_cmpuint(readl(R(PTR, 4)), ==, 2000);
    g_assert_cmpuint(readl(R(PTR, 6)), ==, 1);
    g_assert_cmpuint(readl(R(PTR, 2)), ==, 0);
}

/* A sector written from one guest buffer reads back into another. */
static void test_nvmemory(void)
{
    uint8_t out[512], in[512];

    for (int i = 0; i < 512; i++) {
        out[i] = i * 7;
    }
    g_assert_cmphex(readl(NVMEM), ==, 0xdeadbeef);
    g_assert_cmpuint(readl(NVMEM + 0x60), ==, 128);             /* NV_SIZE in sectors */

    memwrite(0x100000, out, sizeof(out));
    writel(NVMEM + 0x50, 0x100000);                             /* SHM_BASE */
    writel(NVMEM + 0x70, 0x40000);                              /* SHM_SIZE */
    writel(NVMEM + 0x10, 3);                                    /* OFFSET */
    writel(NVMEM + 0x20, 1);                                    /* SIZE */
    writel(NVMEM + 0x30, 2);                                    /* write */
    writel(NVMEM + 0x40, 1);                                    /* EXECUTE */
    g_assert_cmpuint(readl(NVMEM + 0x80), ==, 1);               /* STATUS: sectors moved */

    writel(NVMEM + 0x50, 0x200000);
    writel(NVMEM + 0x30, 1);                                    /* read */
    writel(NVMEM + 0x40, 1);
    g_assert_cmpuint(readl(NVMEM + 0x80), ==, 1);
    memread(0x200000, in, sizeof(in));
    g_assert_cmpmem(in, sizeof(in), out, sizeof(out));
}

static void test_framebuffer(void)
{
    g_assert_cmphex(readl(R(FB, 0)), ==, 0xc51d0007);
    g_assert_cmpuint(readl(R(FB, 3)), ==, 320);                 /* WIDTH */
    g_assert_cmpuint(readl(R(FB, 2)), ==, 240);                 /* HEIGHT */
}

/* ---- hostfs, on a second machine with -M syborg,hostfs=<root>/share ---- */

static char hostfs_root[] = "/tmp/syborg-hostfs-XXXXXX";
static QTestState *hq;

/* Writes the UTF-16LE guest path at 0x100000 + slot * 0x1000, returns its length in units. */
static uint32_t hostfs_name(int slot, const char *path)
{
    uint16_t buf[256];
    uint32_t n = strlen(path);

    for (uint32_t i = 0; i < n; i++) {
        buf[i] = cpu_to_le16(path[i]);
    }
    qtest_memwrite(hq, 0x100000 + slot * 0x1000, buf, n * 2);
    return n;
}

static int32_t hostfs_cmd(uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    qtest_writel(hq, R(HOSTFS, 3), a0);
    qtest_writel(hq, R(HOSTFS, 4), a1);
    qtest_writel(hq, R(HOSTFS, 5), a2);
    qtest_writel(hq, R(HOSTFS, 6), a3);
    qtest_writel(hq, R(HOSTFS, 1), op);
    return qtest_readl(hq, R(HOSTFS, 2));
}

/* FileOpen (9); returns the result, the handle in *h */
static int32_t hostfs_open(const char *path, uint32_t mode, uint32_t how, uint32_t *h)
{
    int32_t r = hostfs_cmd(9, 0x100000, hostfs_name(0, path), mode, how);

    *h = qtest_readl(hq, R(HOSTFS, 3));
    return r;
}

static void test_hostfs_files(void)
{
    uint8_t tree[0x1000];
    char in[16] = { 0 };
    g_autofree char *file = NULL;
    g_autofree char *data = NULL;
    uint32_t h;
    gsize len;

    g_assert_cmphex(qtest_readl(hq, R(HOSTFS, 0)), ==, 0xc51d0008);
    qtest_memread(hq, 0xc0013000, tree, sizeof(tree));          /* the platform's FDT */
    g_assert(memmem(tree, sizeof(tree), "syborg,hostfs", 13));

    g_assert_cmpint(hostfs_open("E:\\hello.txt", 0, 0, &h), ==, 0);
    g_assert_cmpuint(qtest_readl(hq, R(HOSTFS, 6)), ==, 8);    /* ARG3: size */
    g_assert_cmpint(hostfs_cmd(12, h, 3, 0x200000, 16), ==, 0); /* FileRead from 3 */
    g_assert_cmpuint(qtest_readl(hq, R(HOSTFS, 3)), ==, 5);
    qtest_memread(hq, 0x200000, in, 5);
    g_assert_cmpstr(in, ==, "there");
    g_assert_cmpint(hostfs_cmd(11, h, 0, 0, 0), ==, 0);         /* FileClose */
    g_assert_cmpint(hostfs_cmd(11, h, 0, 0, 0), ==, -8);        /* KErrBadHandle */

    g_assert_cmpint(hostfs_cmd(1, 0x100000, hostfs_name(0, "e:\\dir\\"), 0, 0), ==, 0);
    g_assert_cmpint(hostfs_open("E:\\dir\\new.txt", 0x200, 1, &h), ==, 0);
    qtest_memwrite(hq, 0x200000, "written", 7);
    g_assert_cmpint(hostfs_cmd(13, h, 0, 0x200000, 7), ==, 0);  /* FileWrite */
    g_assert_cmpint(hostfs_cmd(11, h, 0, 0, 0), ==, 0);
    file = g_strdup_printf("%s/share/dir/new.txt", hostfs_root);
    g_assert(g_file_get_contents(file, &data, &len, NULL));
    g_assert_cmpmem(data, len, "written", 7);
    g_assert_cmpint(hostfs_open("E:\\dir\\new.txt", 0, 1, &h), ==, -11); /* exists */
}

/* DirOpen (10) + DirRead (17) until KErrEof: the listing has no "." or "..". */
static void test_hostfs_dir(void)
{
    uint16_t name[64];
    int found = 0, n = 0;
    uint32_t h;
    int32_t r;

    g_assert_cmpint(hostfs_cmd(10, 0x100000, hostfs_name(0, "E:\\*"), 0, 0), ==, 0);
    h = qtest_readl(hq, R(HOSTFS, 3));
    while ((r = hostfs_cmd(17, h, 0x200000, 64, 0)) == 0) {
        uint32_t units = qtest_readl(hq, R(HOSTFS, 6));
        char s[64];

        qtest_memread(hq, 0x200000, name, (units + 1) * 2);
        for (uint32_t i = 0; i <= units; i++) {
            s[i] = le16_to_cpu(name[i]);
        }
        g_assert_cmpstr(s, !=, ".");
        g_assert_cmpstr(s, !=, "..");
        found += !strcmp(s, "hello.txt") && qtest_readl(hq, R(HOSTFS, 5)) == 8;
        n++;
    }
    g_assert_cmpint(r, ==, -25);                                /* KErrEof */
    g_assert_cmpint(found, ==, 1);
    g_assert_cmpint(n, ==, 4);                    /* hello.txt, dir, link, outlink */
    g_assert_cmpint(hostfs_cmd(16, h, 0, 0, 0), ==, 0);         /* DirClose */
}

/* Nothing outside <root>/share is reachable: not by "..", other drives, nor symlinks. */
static void test_hostfs_escape(void)
{
    uint32_t h;
    int32_t r;

    g_assert_cmpint(hostfs_open("E:\\..\\secret", 0, 0, &h), ==, -28);
    g_assert_cmpint(hostfs_open("E:\\dir\\..\\..\\secret", 0, 0, &h), ==, -28);
    g_assert_cmpint(hostfs_open("C:\\secret", 0, 0, &h), ==, -28);
    g_assert_cmpint(hostfs_open("E:\\/etc/passwd", 0, 0, &h), ==, -28);
    g_assert_cmpint(hostfs_open("E:\\link", 0, 0, &h), ==, -21);       /* -> ../secret */
    r = hostfs_open("E:\\outlink\\secret", 0, 0, &h);            /* -> .. */
    g_assert(r == -21 || r == -6);              /* ELOOP (Linux) or ENOTDIR (macOS) */
    g_assert_cmpint(hostfs_open("E:\\link", 0x200, 2, &h), ==, -21);
    g_assert_cmpint(hostfs_cmd(3, 0x100000, hostfs_name(0, "E:\\link"), 0, 0), ==, 0);
    g_assert_cmpint(hostfs_cmd(1, 0x100000, 300, 0, 0), ==, -28);        /* too long */
    g_assert(g_file_test(g_strdup_printf("%s/secret", hostfs_root), G_FILE_TEST_EXISTS));
}

static void hostfs_setup(void)
{
    g_autofree char *share = NULL;

    g_assert(g_mkdtemp(hostfs_root));
    share = g_strdup_printf("%s/share", hostfs_root);
    g_assert(mkdir(share, 0700) == 0);
    g_assert(g_file_set_contents(g_strdup_printf("%s/secret", hostfs_root), "x", 1, NULL));
    g_assert(g_file_set_contents(g_strdup_printf("%s/hello.txt", share), "hi there", 8, NULL));
    g_assert(symlink("../secret", g_strdup_printf("%s/link", share)) == 0);
    g_assert(symlink("..", g_strdup_printf("%s/outlink", share)) == 0);
    hq = qtest_initf("-M syborg,hostfs=%s", share);
}

int main(int argc, char **argv)
{
    int fd, r;

    g_test_init(&argc, &argv, NULL);

    fd = mkstemp(disk);
    g_assert(fd >= 0);
    g_assert(ftruncate(fd, 128 * 512) == 0);
    close(fd);

    qtest_start(g_strdup_printf("-M syborg,width=320,height=240 "
                                "-drive if=mtd,format=raw,file=%s", disk));
    qtest_add_func("/syborg/timer-intc", test_timer_and_intc);
    qtest_add_func("/syborg/keyboard", test_keyboard);
    qtest_add_func("/syborg/pointer", test_pointer);
    qtest_add_func("/syborg/nvmemory", test_nvmemory);
    qtest_add_func("/syborg/framebuffer", test_framebuffer);
    hostfs_setup();
    qtest_add_func("/syborg/hostfs/files", test_hostfs_files);
    qtest_add_func("/syborg/hostfs/dir", test_hostfs_dir);
    qtest_add_func("/syborg/hostfs/escape", test_hostfs_escape);
    r = g_test_run();
    qtest_quit(hq);
    qtest_end();
    unlink(disk);
    g_spawn_command_line_sync(g_strdup_printf("rm -rf %s", hostfs_root), NULL, NULL, NULL, NULL);
    return r;
}
