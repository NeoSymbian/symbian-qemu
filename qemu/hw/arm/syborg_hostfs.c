/*
 * Syborg host filesystem: the guest's svphostfs driver reaches a host folder through it.
 *
 * Registers (32-bit words): 0 ID, 1 COMMAND (a write runs the command), 2 RESULT (a Symbian
 * KErr* code), 3..6 ARG0..ARG3. Guest paths are UTF-16LE "X:\dir\name" in guest RAM, X being
 * the drive letter of the device-tree node (drive-number SYBORG_HOSTFS_DRIVE). Port of
 * CodeSourcery's syborg_hostfs.c (QEMU 0.9.1 SVP) to the current API; POSIX hosts only.
 *
 * The guest is untrusted: every path is resolved one component at a time beneath the host
 * folder with openat(O_NOFOLLOW), "", ".", "..", "/" and over-long names are refused, so no
 * path, symlink or race reaches outside it. Symlinks are never followed, even ones inside.
 *
 * Copyright (c) 2009 CodeSourcery
 * Copyright (c) 2026 The NeoSymbian Authors
 * SPDX-License-Identifier: MIT
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/syborg.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/blocker.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qom/object.h"
#include <dirent.h>
#include <fnmatch.h>

OBJECT_DECLARE_SIMPLE_TYPE(SyborgHostfsState, SYBORG_HOSTFS)

#define HOSTFS_NAME_MAX 256         /* KMaxPath, in UTF-16 units */
#define HOSTFS_HANDLES  256

enum { R_ID, R_COMMAND, R_RESULT, R_ARG0 };

/* Symbian error codes */
#define HOST_FS_SUCCESS            0
#define HOST_FS_GENERAL_ERROR     -2
#define HOST_FS_NO_MEMORY         -4
#define HOST_FS_UNSUPPORTED       -5
#define HOST_FS_BAD_ARG           -6
#define HOST_FS_BAD_HANDLE        -8
#define HOST_FS_EXISTS           -11
#define HOST_FS_PATH_NOT_FOUND   -12
#define HOST_FS_IN_USE           -14
#define HOST_FS_ACCESS_DENIED    -21
#define HOST_FS_EOF              -25
#define HOST_FS_DISKFULL         -26
#define HOST_FS_BAD_NAME         -28
#define HOST_FS_TOO_BIG          -40
#define HOST_FS_PERMISSION_DENIED -46

#define HOSTFS_ATTR_READONLY  0x01
#define HOSTFS_ATTR_HIDDEN    0x02
#define HOSTFS_ATTR_DIRECTORY 0x10

typedef struct {
    int fd;                         /* an open file, or -1 */
    DIR *dir;                       /* an open directory listing, or NULL */
    char *pattern;                  /* the listing's wildcard; "" lists everything */
} HostfsHandle;

struct SyborgHostfsState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    char *path;
    int root;
    uint32_t command;
    uint32_t result;
    uint32_t arg[4];
    HostfsHandle h[HOSTFS_HANDLES];
    unsigned open;
    Error *blocker;
};

static int hostfs_errno(void)
{
    switch (errno) {
    case ENOENT:
        return HOST_FS_PATH_NOT_FOUND;
    case EEXIST:
    case ENOTEMPTY:
        return HOST_FS_EXISTS;
    case EPERM:
        return HOST_FS_PERMISSION_DENIED;
    case EACCES:
    case EROFS:
    case ELOOP:                     /* a symlink: never followed */
        return HOST_FS_ACCESS_DENIED;
    case ENOMEM:
        return HOST_FS_NO_MEMORY;
    case ENOTDIR:
    case EISDIR:
    case EINVAL:
    case EBADF:
        return HOST_FS_BAD_ARG;
    case EBUSY:
        return HOST_FS_IN_USE;
    case EFBIG:
        return HOST_FS_TOO_BIG;
    case ENOSPC:
        return HOST_FS_DISKFULL;
    case ENAMETOOLONG:
        return HOST_FS_BAD_NAME;
    }
    return HOST_FS_GENERAL_ERROR;
}

static bool hostfs_component_ok(const char *c)
{
    return *c && strcmp(c, ".") && strcmp(c, "..");
}

/*
 * Resolves the guest path at addr (len UTF-16 units) beneath the host folder. Returns an fd of
 * the directory holding the last component and points *leaf at that component (inside *buf,
 * which the caller frees), or a HOST_FS_* error. With pattern, the last component is a
 * wildcard (may be empty); otherwise a trailing '\' is dropped and the drive root is ".".
 */
static int hostfs_path(SyborgHostfsState *s, uint32_t addr, uint32_t len, bool pattern,
                       char **buf, char **leaf)
{
    uint16_t name[HOSTFS_NAME_MAX];
    char *p, *sep;
    int fd, next;

    if (len < 3 || len > HOSTFS_NAME_MAX ||
        address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, name,
                           len * 2) != MEMTX_OK) {
        return HOST_FS_BAD_NAME;
    }
    for (uint32_t i = 0; i < len; i++) {
        name[i] = le16_to_cpu(name[i]);
    }
    if ((name[0] & ~0x20) != 'A' + SYBORG_HOSTFS_DRIVE - 1 || name[1] != ':' ||
        name[2] != '\\') {
        return HOST_FS_BAD_NAME;
    }
    *buf = g_utf16_to_utf8(name + 3, len - 3, NULL, NULL, NULL);
    if (!*buf || strchr(*buf, '/')) {
        return HOST_FS_BAD_NAME;
    }
    p = *buf;
    if (!pattern && *p && p[strlen(p) - 1] == '\\') {
        p[strlen(p) - 1] = 0;
    }
    fd = openat(s->root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    while (fd >= 0 && (sep = strchr(p, '\\'))) {
        *sep = 0;
        if (!hostfs_component_ok(p)) {
            close(fd);
            return HOST_FS_BAD_NAME;
        }
        next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        fd = next;
        p = sep + 1;
    }
    if (fd < 0) {
        return hostfs_errno();
    }
    if (!pattern && !*p) {
        p = (char *)".";
    } else if (!pattern && !hostfs_component_ok(p)) {
        close(fd);
        return HOST_FS_BAD_NAME;
    }
    *leaf = p;
    return fd;
}

/* Closes dfd after a host call that returned r (< 0 with errno on failure). */
static int hostfs_done(int dfd, int r)
{
    int err = r < 0 ? hostfs_errno() : HOST_FS_SUCCESS;

    close(dfd);
    return err;
}

/* Hold a migration blocker while the guest has host files or listings open. */
static int hostfs_new_handle(SyborgHostfsState *s)
{
    for (int i = 0; i < HOSTFS_HANDLES; i++) {
        if (s->h[i].fd < 0 && !s->h[i].dir) {
            if (s->open == 0) {
                error_setg(&s->blocker, "%s: the guest has host files open",
                           TYPE_SYBORG_HOSTFS);
                if (migrate_add_blocker(&s->blocker, NULL) < 0) {
                    return HOST_FS_IN_USE;
                }
            }
            s->open++;
            return i;
        }
    }
    return HOST_FS_NO_MEMORY;
}

static void hostfs_free_handle(SyborgHostfsState *s, HostfsHandle *h)
{
    if (h->fd >= 0) {
        close(h->fd);
    }
    if (h->dir) {
        closedir(h->dir);
    }
    g_free(h->pattern);
    *h = (HostfsHandle) { .fd = -1 };
    if (--s->open == 0) {
        migrate_del_blocker(&s->blocker);
    }
}

/* The handle the guest passed in ARG0 (1-based), if it is open as a file (or a listing). */
static HostfsHandle *hostfs_handle(SyborgHostfsState *s, bool dir)
{
    uint32_t i = s->arg[0] - 1;

    if (i >= HOSTFS_HANDLES || (dir ? !s->h[i].dir : s->h[i].fd < 0)) {
        return NULL;
    }
    return &s->h[i];
}

static uint32_t hostfs_att(const struct stat *st)
{
    uint32_t r = 0;

    if (!(st->st_mode & S_IRWXU)) {
        r |= HOSTFS_ATTR_HIDDEN;
    } else if (!(st->st_mode & S_IWUSR)) {
        r |= HOSTFS_ATTR_READONLY;
    }
    if (S_ISDIR(st->st_mode)) {
        r |= HOSTFS_ATTR_DIRECTORY;
    }
    return r;
}

/* ARG0..ARG2 (or ARG1..ARG3) = attributes, modified time, size */
static void hostfs_put_stat(SyborgHostfsState *s, int first, const struct stat *st)
{
    s->arg[first] = hostfs_att(st);
    s->arg[first + 1] = st->st_mtime;
    s->arg[first + 2] = st->st_size;
}

static int hostfs_unsupported(SyborgHostfsState *s)
{
    return HOST_FS_UNSUPPORTED;
}

#define HOSTFS_ONE_PATH(fn, call)                                                   \
    static int fn(SyborgHostfsState *s)                                             \
    {                                                                               \
        g_autofree char *buf = NULL;                                                \
        char *leaf;                                                                 \
        int dfd = hostfs_path(s, s->arg[0], s->arg[1], false, &buf, &leaf);         \
                                                                                    \
        return dfd < 0 ? dfd : hostfs_done(dfd, call);                              \
    }

HOSTFS_ONE_PATH(hostfs_mkdir, mkdirat(dfd, leaf, 0777))
HOSTFS_ONE_PATH(hostfs_rmdir, unlinkat(dfd, leaf, AT_REMOVEDIR))
HOSTFS_ONE_PATH(hostfs_delete, unlinkat(dfd, leaf, 0))

/* Rename and Replace: rename() already replaces an existing file. */
static int hostfs_rename(SyborgHostfsState *s)
{
    g_autofree char *buf1 = NULL;
    g_autofree char *buf2 = NULL;
    char *leaf1, *leaf2;
    int d1, d2, r;

    d1 = hostfs_path(s, s->arg[0], s->arg[1], false, &buf1, &leaf1);
    if (d1 < 0) {
        return d1;
    }
    d2 = hostfs_path(s, s->arg[2], s->arg[3], false, &buf2, &leaf2);
    if (d2 < 0) {
        close(d1);
        return d2;
    }
    r = hostfs_done(d2, renameat(d1, leaf1, d2, leaf2));
    close(d1);
    return r;
}

static int hostfs_entry(SyborgHostfsState *s)
{
    g_autofree char *buf = NULL;
    char *leaf;
    struct stat st;
    int dfd = hostfs_path(s, s->arg[0], s->arg[1], false, &buf, &leaf);
    int r;

    if (dfd < 0) {
        return dfd;
    }
    r = hostfs_done(dfd, fstatat(dfd, leaf, &st, AT_SYMLINK_NOFOLLOW));
    if (r == HOST_FS_SUCCESS) {
        hostfs_put_stat(s, 0, &st);
    }
    return r;
}

/* ARG2 = mode (EFileWrite 0x200), ARG3 = 0 open, 1 create, 2 replace */
static int hostfs_file_open(SyborgHostfsState *s)
{
    g_autofree char *buf = NULL;
    char *leaf;
    struct stat st;
    int flags = (s->arg[2] & 0x200 ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC;
    int dfd, fd, i;

    if (s->arg[3] == 1) {
        flags |= O_CREAT | O_EXCL;
    } else if (s->arg[3] == 2) {
        flags |= O_CREAT | O_TRUNC;
    }
    dfd = hostfs_path(s, s->arg[0], s->arg[1], false, &buf, &leaf);
    if (dfd < 0) {
        return dfd;
    }
    fd = openat(dfd, leaf, flags, 0600);
    i = hostfs_done(dfd, fd);
    if (i == HOST_FS_SUCCESS && fstat(fd, &st) < 0) {
        i = hostfs_errno();
    }
    if (i == HOST_FS_SUCCESS) {
        i = hostfs_new_handle(s);
    }
    if (i < 0) {
        if (fd >= 0) {
            close(fd);
        }
        return i;
    }
    s->h[i].fd = fd;
    s->arg[0] = i + 1;
    hostfs_put_stat(s, 1, &st);
    return HOST_FS_SUCCESS;
}

/* "X:\dir\pattern": lists dir, filtered by the pattern unless it is "" or "*". */
static int hostfs_dir_open(SyborgHostfsState *s)
{
    g_autofree char *buf = NULL;
    char *leaf;
    DIR *d;
    int dfd, i;

    dfd = hostfs_path(s, s->arg[0], s->arg[1], true, &buf, &leaf);
    if (dfd < 0) {
        return dfd;
    }
    d = fdopendir(dfd);
    if (!d) {
        return hostfs_done(dfd, -1);
    }
    i = hostfs_new_handle(s);
    if (i < 0) {
        closedir(d);
        return i;
    }
    s->h[i].dir = d;
    s->h[i].pattern = g_strdup(strcmp(leaf, "*") ? leaf : "");
    s->arg[0] = i + 1;
    return HOST_FS_SUCCESS;
}

static int hostfs_close(SyborgHostfsState *s, bool dir)
{
    HostfsHandle *h = hostfs_handle(s, dir);

    if (!h) {
        return HOST_FS_BAD_HANDLE;
    }
    hostfs_free_handle(s, h);
    return HOST_FS_SUCCESS;
}

static int hostfs_file_close(SyborgHostfsState *s)
{
    return hostfs_close(s, false);
}

static int hostfs_dir_close(SyborgHostfsState *s)
{
    return hostfs_close(s, true);
}

/* ARG0 = handle, ARG1 = file position, ARG2 = guest buffer, ARG3 = length; ARG0 = done */
static int hostfs_file_io(SyborgHostfsState *s, bool write)
{
    HostfsHandle *h = hostfs_handle(s, false);
    uint8_t buf[0x1000];
    uint32_t done = 0;

    if (!h) {
        return HOST_FS_BAD_HANDLE;
    }
    while (done < s->arg[3]) {
        hwaddr addr = (hwaddr)s->arg[2] + done;
        off_t pos = (off_t)s->arg[1] + done;
        uint32_t n = MIN(sizeof(buf), s->arg[3] - done);
        ssize_t r;

        if (write) {
            if (address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                                   buf, n) != MEMTX_OK) {
                return HOST_FS_BAD_ARG;
            }
            r = RETRY_ON_EINTR(pwrite(h->fd, buf, n, pos));
        } else {
            r = RETRY_ON_EINTR(pread(h->fd, buf, n, pos));
            if (r > 0 && address_space_write(&address_space_memory, addr,
                                             MEMTXATTRS_UNSPECIFIED, buf, r) != MEMTX_OK) {
                return HOST_FS_BAD_ARG;
            }
        }
        if (r < 0) {
            return hostfs_errno();
        }
        if (r == 0) {
            break;
        }
        done += r;
    }
    s->arg[0] = done;
    return HOST_FS_SUCCESS;
}

static int hostfs_file_read(SyborgHostfsState *s)
{
    return hostfs_file_io(s, false);
}

static int hostfs_file_write(SyborgHostfsState *s)
{
    return hostfs_file_io(s, true);
}

static int hostfs_set_size(SyborgHostfsState *s)
{
    HostfsHandle *h = hostfs_handle(s, false);

    if (!h) {
        return HOST_FS_BAD_HANDLE;
    }
    return ftruncate(h->fd, s->arg[1]) < 0 ? hostfs_errno() : HOST_FS_SUCCESS;
}

static int hostfs_flush(SyborgHostfsState *s)
{
    HostfsHandle *h = hostfs_handle(s, false);

    if (!h) {
        return HOST_FS_BAD_HANDLE;
    }
    return fsync(h->fd) < 0 ? hostfs_errno() : HOST_FS_SUCCESS;
}

/* ARG0 = handle, ARG1 = name buffer, ARG2 = its size in UTF-16 units;
 * returns ARG0..ARG2 = attributes, modified time, size, ARG3 = name length. */
static int hostfs_dir_read(SyborgHostfsState *s)
{
    HostfsHandle *h = hostfs_handle(s, true);
    struct dirent *de;
    g_autofree gunichar2 *name = NULL;
    glong units = 0;
    struct stat st;

    if (!h) {
        return HOST_FS_BAD_HANDLE;
    }
    for (;;) {
        errno = 0;
        de = readdir(h->dir);
        if (!de) {
            return errno ? hostfs_errno() : HOST_FS_EOF;
        }
        if (!hostfs_component_ok(de->d_name) ||
            (h->pattern[0] && fnmatch(h->pattern, de->d_name, 0))) {
            continue;
        }
        name = g_utf8_to_utf16(de->d_name, -1, NULL, &units, NULL);
        if (name) {
            break;              /* names that are not UTF-8 are skipped */
        }
    }
    if (units + 1 > s->arg[2]) {
        return HOST_FS_TOO_BIG;
    }
    if (fstatat(dirfd(h->dir), de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        return hostfs_errno();
    }
    for (glong i = 0; i <= units; i++) {
        name[i] = cpu_to_le16(name[i]);
    }
    if (address_space_write(&address_space_memory, s->arg[1], MEMTXATTRS_UNSPECIFIED, name,
                            (units + 1) * 2) != MEMTX_OK) {
        return HOST_FS_BAD_ARG;
    }
    hostfs_put_stat(s, 0, &st);
    s->arg[3] = units;
    return HOST_FS_SUCCESS;
}

/* Indexed by the guest's RSVPHostFsDriver::TControl. */
static int (*const hostfs_ops[])(SyborgHostfsState *) = {
    hostfs_unsupported,         /* EDummy */
    hostfs_mkdir,
    hostfs_rmdir,
    hostfs_delete,
    hostfs_rename,
    hostfs_rename,              /* EReplace */
    hostfs_unsupported,         /* EReadUid */
    hostfs_entry,
    hostfs_unsupported,         /* ESetEntry */
    hostfs_file_open,
    hostfs_dir_open,
    hostfs_file_close,
    hostfs_file_read,
    hostfs_file_write,
    hostfs_set_size,
    hostfs_flush,
    hostfs_dir_close,
    hostfs_dir_read,
};

static uint64_t syborg_hostfs_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgHostfsState *s = opaque;

    switch (offset >> 2) {
    case R_ID:
        return SYBORG_ID_HOSTFS;
    case R_COMMAND:
        return s->command;
    case R_RESULT:
        return s->result;
    case R_ARG0 ... R_ARG0 + 3:
        return s->arg[(offset >> 2) - R_ARG0];
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read at offset 0x%" HWADDR_PRIx "\n",
                  TYPE_SYBORG_HOSTFS, offset);
    return 0;
}

static void syborg_hostfs_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgHostfsState *s = opaque;

    switch (offset >> 2) {
    case R_COMMAND:
        s->command = value;
        s->result = value < ARRAY_SIZE(hostfs_ops) ? hostfs_ops[value](s)
                                                   : HOST_FS_UNSUPPORTED;
        return;
    case R_RESULT:
        s->result = value;
        return;
    case R_ARG0 ... R_ARG0 + 3:
        s->arg[(offset >> 2) - R_ARG0] = value;
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write at offset 0x%" HWADDR_PRIx "\n",
                  TYPE_SYBORG_HOSTFS, offset);
}

static const MemoryRegionOps syborg_hostfs_ops = {
    .read = syborg_hostfs_read,
    .write = syborg_hostfs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void syborg_hostfs_init(Object *obj)
{
    SyborgHostfsState *s = SYBORG_HOSTFS(obj);

    s->root = -1;
    for (int i = 0; i < HOSTFS_HANDLES; i++) {
        s->h[i].fd = -1;
    }
    memory_region_init_io(&s->iomem, obj, &syborg_hostfs_ops, s, TYPE_SYBORG_HOSTFS, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void syborg_hostfs_realize(DeviceState *dev, Error **errp)
{
    SyborgHostfsState *s = SYBORG_HOSTFS(dev);

    if (!s->path) {
        error_setg(errp, "%s: 'path' (the host folder) is required", TYPE_SYBORG_HOSTFS);
        return;
    }
    s->root = qemu_open(s->path, O_RDONLY | O_DIRECTORY, errp);
}

/* Open host files do not survive a reset (nor migration: the blocker covers that). */
static void syborg_hostfs_reset(DeviceState *dev)
{
    SyborgHostfsState *s = SYBORG_HOSTFS(dev);

    for (int i = 0; i < HOSTFS_HANDLES; i++) {
        if (s->h[i].fd >= 0 || s->h[i].dir) {
            hostfs_free_handle(s, &s->h[i]);
        }
    }
    s->command = s->result = 0;
    memset(s->arg, 0, sizeof(s->arg));
}

static const Property syborg_hostfs_properties[] = {
    DEFINE_PROP_STRING("path", SyborgHostfsState, path),
};

static const VMStateDescription vmstate_syborg_hostfs = {
    .name = TYPE_SYBORG_HOSTFS,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(command, SyborgHostfsState),
        VMSTATE_UINT32(result, SyborgHostfsState),
        VMSTATE_UINT32_ARRAY(arg, SyborgHostfsState, 4),
        VMSTATE_END_OF_LIST()
    }
};

static void syborg_hostfs_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = syborg_hostfs_realize;
    device_class_set_legacy_reset(dc, syborg_hostfs_reset);
    dc->vmsd = &vmstate_syborg_hostfs;
    device_class_set_props(dc, syborg_hostfs_properties);
}

static const TypeInfo syborg_hostfs_types[] = {
    {
        .name = TYPE_SYBORG_HOSTFS,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SyborgHostfsState),
        .instance_init = syborg_hostfs_init,
        .class_init = syborg_hostfs_class_init,
    },
};

DEFINE_TYPES(syborg_hostfs_types)
