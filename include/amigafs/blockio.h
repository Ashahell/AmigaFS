#ifndef AMIGAFS_BLOCKIO_H
#define AMIGAFS_BLOCKIO_H

#include <stddef.h>
#include <stdint.h>

/* blockio is the single I/O seam of the core library. Everything above it --
 * directory walking, file reads, the verifier -- obtains blocks exclusively
 * through this interface, which keeps the format code free of OS calls and
 * makes it testable without a mount.
 *
 * SECURITY: every block pointer that reaches this layer came from on-disk data
 * and is therefore untrusted. A block number outside the device/window is
 * REFUSED with a non-zero return. It is never clamped, never wrapped, and
 * never allowed to read past the window. A write backend will slot in here
 * later (D3) without changing anything above this interface.
 */

/* Opaque device handle. */
typedef struct blockdev blockdev_t;

/* Open an in-memory image of `size` bytes with `block_size`-byte blocks.
 * The memory must outlive the handle. Returns NULL on invalid arguments. */
blockdev_t *blockdev_open_memory(void *data, size_t size, size_t block_size);

/* Open an image file for reading via pread. */
blockdev_t *blockdev_open_file(const char *path, size_t block_size);

/* Create a PARTITION WINDOW over an existing device: `start_block` blocks in,
 * `nblocks` long. All block numbers passed to the returned handle are relative
 * to this window. Returns NULL if the window does not lie entirely within the
 * underlying device. */
blockdev_t *blockdev_window(blockdev_t *base, uint32_t start_block, uint32_t nblocks);

/* Read one block. Returns 0 on success, non-zero on any out-of-range or I/O
 * error. `len` may be less than the block size (a short read within the block);
 * `len` greater than the block size is refused. */
int blockdev_read(blockdev_t *d, uint32_t blockno, void *buf, size_t len);

/* Read `len` bytes starting at block `blockno`, spanning as many blocks as
 * needed. This is what the bootblock needs: on a 512-byte-block volume the
 * bootblock is 1024 bytes and therefore spans blocks 0 and 1. The whole span
 * must lie inside the window, or the call is refused. */
int blockdev_read_span(blockdev_t *d, uint32_t blockno, void *buf, size_t len);

/* Write (currently unsupported -- read-only phase). Always returns non-zero. */
int blockdev_write(blockdev_t *d, uint32_t blockno, const void *buf, size_t len);

/* Number of blocks in this device/window. */
uint32_t blockdev_block_count(const blockdev_t *d);

/* Block size in bytes. */
size_t blockdev_block_size(const blockdev_t *d);

/* Close the device (and any windows derived from it). */
void blockdev_close(blockdev_t *d);

/* Last error string for the most recent failing operation on this device. */
const char *blockdev_last_error(const blockdev_t *d);

#endif /* AMIGAFS_BLOCKIO_H */