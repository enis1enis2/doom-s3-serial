/*
 * Minimal read-only VFS that exposes the IWAD flash partition as
 * /doom/doom1.wad. No FAT filesystem and no SD card are involved.
 */

#include "wad_vfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs.h"

static const char *TAG = "wad_vfs";

#define WAD_PARTITION_LABEL "doom_wad"
#define WAD_PARTITION_SUBTYPE ((esp_partition_subtype_t)0x40)

typedef struct {
    bool used;
    size_t pos;
} wad_fd_t;

static wad_fd_t s_fds[4];
static const esp_partition_t *s_part;
static size_t s_wad_len;

static void compute_wad_length(void)
{
    uint8_t header[12];
    s_wad_len = s_part->size;

    if (esp_partition_read(s_part, 0, header, sizeof(header)) != ESP_OK) {
        return;
    }
    if (memcmp(header, "IWAD", 4) != 0 && memcmp(header, "PWAD", 4) != 0) {
        return;
    }

    int32_t numlumps;
    int32_t infotableofs;
    memcpy(&numlumps, header + 4, sizeof(numlumps));
    memcpy(&infotableofs, header + 8, sizeof(infotableofs));

    if (numlumps > 0 && infotableofs > 0) {
        size_t len = (size_t)infotableofs + (size_t)numlumps * 16;
        if (len > 0 && len <= s_part->size) {
            s_wad_len = len;
        }
    }
}

static const wad_fd_t *get_fd(int fd)
{
    if (fd < 0 || fd >= (int)(sizeof(s_fds) / sizeof(s_fds[0])) || !s_fds[fd].used) {
        errno = EBADF;
        return NULL;
    }
    return &s_fds[fd];
}

static int wad_open_p(void *ctx, const char *path, int flags, int mode)
{
    (void)ctx;
    (void)mode;

    if (path == NULL ||
        (strcmp(path, "/doom1.wad") != 0 && strcmp(path, "/doom/doom1.wad") != 0)) {
        errno = ENOENT;
        return -1;
    }
    if ((flags & O_ACCMODE) != O_RDONLY) {
        errno = EACCES;
        return -1;
    }

    for (size_t i = 0; i < sizeof(s_fds) / sizeof(s_fds[0]); ++i) {
        if (!s_fds[i].used) {
            s_fds[i].used = true;
            s_fds[i].pos = 0;
            return (int)i;
        }
    }
    errno = EMFILE;
    return -1;
}

static int wad_close_p(void *ctx, int fd)
{
    (void)ctx;
    const wad_fd_t *entry = get_fd(fd);
    if (entry == NULL) {
        return -1;
    }
    s_fds[fd].used = false;
    return 0;
}

static ssize_t wad_read_p(void *ctx, int fd, void *dst, size_t size)
{
    (void)ctx;
    const wad_fd_t *entry = get_fd(fd);
    if (entry == NULL) {
        return -1;
    }
    if (dst == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (s_fds[fd].pos >= s_wad_len || size == 0) {
        return 0;
    }

    size_t available = s_wad_len - s_fds[fd].pos;
    if (size > available) {
        size = available;
    }
    esp_err_t err = esp_partition_read(s_part, s_fds[fd].pos, dst, size);
    if (err != ESP_OK) {
        errno = EIO;
        return -1;
    }
    s_fds[fd].pos += size;
    return (ssize_t)size;
}

static off_t wad_lseek_p(void *ctx, int fd, off_t offset, int whence)
{
    (void)ctx;
    const wad_fd_t *entry = get_fd(fd);
    if (entry == NULL) {
        return -1;
    }

    off_t base;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = (off_t)s_fds[fd].pos;
        break;
    case SEEK_END:
        base = (off_t)s_wad_len;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    off_t next = base + offset;
    if (next < 0) {
        errno = EINVAL;
        return -1;
    }
    if ((size_t)next > s_wad_len) {
        next = (off_t)s_wad_len;
    }
    s_fds[fd].pos = (size_t)next;
    return next;
}

static int wad_fstat_p(void *ctx, int fd, struct stat *st)
{
    (void)ctx;
    if (get_fd(fd) == NULL) {
        return -1;
    }
    if (st == NULL) {
        errno = EFAULT;
        return -1;
    }

    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)s_wad_len;
    st->st_mode = S_IFREG | 0444;
    st->st_blksize = 4096;
    st->st_blocks = (off_t)((s_wad_len + 511) / 512);
    return 0;
}

esp_err_t wad_vfs_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      WAD_PARTITION_SUBTYPE,
                                      WAD_PARTITION_LABEL);
    if (s_part == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found; check partitions.csv", WAD_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    compute_wad_length();
    if (s_wad_len < 12 || s_wad_len > s_part->size) {
        ESP_LOGE(TAG, "WAD looks invalid (length %u)", (unsigned)s_wad_len);
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR,
        .open_p = wad_open_p,
        .close_p = wad_close_p,
        .read_p = wad_read_p,
        .lseek_p = wad_lseek_p,
        .fstat_p = wad_fstat_p,
    };

    esp_err_t err = esp_vfs_register("/doom", &vfs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_register failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mounted /doom/doom1.wad (offset 0x%x, %u bytes)",
             (unsigned)s_part->address, (unsigned)s_wad_len);
    return ESP_OK;
}
