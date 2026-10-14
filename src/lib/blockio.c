#include "amigafs/blockio.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

/* A blockdev is either a backing device (memory or file) or a window onto one.
 * Windows share the parent's lifetime: closing the base invalidates them, so
 * callers must not close a window separately. */
struct blockdev {
    /* backing */
    int         is_file;
    int         fd;
    int         readonly;   /* refuse writes (read-only opens, incl. the mount) */
    uint8_t    *mem;
    size_t      mem_size;
    size_t      block_size;

    /* window: absolute first block, and how many blocks are visible */
    uint32_t    start_block;
    uint32_t    nblocks;

    /* set when this handle is a window (do not free separately) */
    int         is_window;

    char        err[160];
};

static void set_err(blockdev_t *d, const char *msg) {
    if (d) snprintf(d->err, sizeof d->err, "%s", msg);
}

const char *blockdev_last_error(const blockdev_t *d) {
    return (d && d->err[0]) ? d->err : "";
}

/* Validate the backing store geometry and compute the absolute block count. */
static int init_geometry(blockdev_t *d, size_t size) {
    if (d->block_size == 0 || (d->block_size % 4) != 0) {
        set_err(d, "block size must be a nonzero multiple of 4");
        return -1;
    }
    if (size < d->block_size) {
        set_err(d, "device is smaller than one block");
        return -1;
    }
    d->start_block = 0;
    d->nblocks = (uint32_t)(size / d->block_size);
    return 0;
}

blockdev_t *blockdev_open_memory(void *data, size_t size, size_t block_size) {
    if (!data) return NULL;
    blockdev_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->is_file = 0;
    d->fd = -1;
    d->readonly = 0;   /* memory images are writable; the caller owns the bytes */
    d->mem = (uint8_t *)data;
    d->mem_size = size;
    d->block_size = block_size;
    if (init_geometry(d, size) != 0) {
        free(d);
        return NULL;
    }
    return d;
}

static blockdev_t *open_file_impl(const char *path, size_t block_size, int rw) {
    if (!path) return NULL;
    FILE *f = fopen(path, rw ? "r+b" : "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    fclose(f);
    if (sz <= 0) return NULL;

    int fd = open(path, rw ? O_RDWR : O_RDONLY);
    if (fd < 0) return NULL;

    blockdev_t *d = calloc(1, sizeof *d);
    if (!d) { close(fd); return NULL; }
    d->is_file = 1;
    d->fd = fd;
    d->readonly = !rw;
    d->block_size = block_size;
    if (init_geometry(d, (size_t)sz) != 0) {
        close(fd);
        free(d);
        return NULL;
    }
    return d;
}

blockdev_t *blockdev_open_file(const char *path, size_t block_size) {
    return open_file_impl(path, block_size, 0);
}

/* Writable file backend, for the write phase (D3). Writes go only to scratch
 * copies in practice -- gates copy fixtures to tmp first -- but the layer
 * itself does not know that: same bounds discipline as reads either way. */
blockdev_t *blockdev_open_file_rw(const char *path, size_t block_size) {
    return open_file_impl(path, block_size, 1);
}

blockdev_t *blockdev_window(blockdev_t *base, uint32_t start_block, uint32_t nblocks) {
    if (!base || nblocks == 0) return NULL;
    /* the window must lie entirely within the backing device */
    if ((uint64_t)start_block + (uint64_t)nblocks > (uint64_t)base->nblocks) {
        set_err(base, "partition window extends past the device");
        return NULL;
    }
    blockdev_t *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    /* share the backing by reference: copy the pointers and mark it a window */
    *w = *base;
    w->is_window = 1;
    w->start_block = base->start_block + start_block;
    w->nblocks = nblocks;
    w->err[0] = '\0';
    return w;
}

int blockdev_read(blockdev_t *d, uint32_t blockno, void *buf, size_t len) {
    if (!d || !buf) return -1;
    if (len == 0) return 0;
    if (len > d->block_size) {
        set_err(d, "read length exceeds block size");
        return -1;
    }
    /* BOUNDS: this is the security boundary. A block number outside the
     * window is refused -- never clamped, never wrapped. */
    if (blockno >= d->nblocks) {
        set_err(d, "block number outside window");
        return -1;
    }
    uint64_t abs = (uint64_t)d->start_block + blockno;
    uint64_t off = abs * (uint64_t)d->block_size;

    if (d->is_file) {
        size_t got = 0;
        while (got < len) {
            ssize_t n = pread(d->fd, (uint8_t *)buf + got, len - got,
                              (off_t)(off + got));
            if (n <= 0) { set_err(d, "short or failed pread"); return -1; }
            got += (size_t)n;
        }
        return 0;
    } else {
        if (off + len > (uint64_t)d->mem_size) {
            set_err(d, "read past memory device");
            return -1;
        }
        memcpy(buf, d->mem + off, len);
        return 0;
    }
}

int blockdev_read_span(blockdev_t *d, uint32_t blockno, void *buf, size_t len) {
    if (!d || !buf) return -1;
    if (len == 0) return 0;
    size_t bs = d->block_size;
    /* the span must lie entirely within the window -- refused, never clamped */
    uint64_t first = blockno;
    uint64_t nblocks_needed = (len + bs - 1) / bs;
    if (first + nblocks_needed > (uint64_t)d->nblocks) {
        set_err(d, "read span extends past window");
        return -1;
    }
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > bs) chunk = bs;
        if (blockdev_read(d, (uint32_t)(first + done / bs), out + done, chunk) != 0)
            return -1;
        done += chunk;
    }
    return 0;
}

int blockdev_write(blockdev_t *d, uint32_t blockno, const void *buf, size_t len) {
    if (!d || !buf) return -1;
    if (d->readonly) {
        set_err(d, "read-only device");
        return -1;
    }
    if (len == 0) return 0;
    if (len > d->block_size) {
        set_err(d, "write length exceeds block size");
        return -1;
    }
    /* BOUNDS: the same security boundary as reads. A block number outside
     * the window is REFUSED -- never clamped, never wrapped. On a write path
     * a clamp would corrupt the wrong block silently, which is worse than
     * the read-side equivalent. */
    if (blockno >= d->nblocks) {
        set_err(d, "block number outside window");
        return -1;
    }
    uint64_t abs = (uint64_t)d->start_block + blockno;
    uint64_t off = abs * (uint64_t)d->block_size;

    if (d->is_file) {
        size_t done = 0;
        while (done < len) {
            ssize_t n = pwrite(d->fd, (const uint8_t *)buf + done, len - done,
                               (off_t)(off + done));
            if (n <= 0) { set_err(d, "short or failed pwrite"); return -1; }
            done += (size_t)n;
        }
        return 0;
    } else {
        if (off + len > (uint64_t)d->mem_size) {
            set_err(d, "write past memory device");
            return -1;
        }
        memcpy(d->mem + off, buf, len);
        return 0;
    }
}

uint32_t blockdev_block_count(const blockdev_t *d) {
    return d ? d->nblocks : 0;
}

size_t blockdev_block_size(const blockdev_t *d) {
    return d ? d->block_size : 0;
}

void blockdev_close(blockdev_t *d) {
    if (!d) return;
    if (d->is_file && d->fd >= 0) close(d->fd);
    free(d);
}