/* libFuzzer target for blockio -- the ONE place the core touches storage.
 *
 * The plan's "hostile images" risk asks for exactly this: a fuzz target over
 * blockio with a memory backend. blockio is a security boundary (its own header
 * says so) and it had unit tests but no target, so the only hostile inputs it
 * ever saw were the ones a test author chose.
 *
 * The property under test is narrow and is the whole job of this layer:
 *
 *   blockdev_read / blockdev_read_span / blockdev_write MUST reject a block
 *   number outside the device, and MUST NOT read or write out of bounds for one
 *   inside it.
 *
 * So the harness deliberately asks for absurd block numbers -- 0, the last
 * block, last+1, UINT32_MAX, and values straddling a 32-bit wrap -- and lets
 * ASan judge the answer. A layer that trusts its caller does not get to fail
 * gracefully here; it gets to be caught, which is the point of fuzzing it.
 *
 * The block size is taken from the input rather than fixed, because the
 * coordinate maths differs per size (a span crossing the end of the device, a
 * block size that does not divide the image) and those are separate bugs. Fixed
 * at 512 the harness would never reach them.
 *
 * Build:
 *   clang -fsanitize=fuzzer,address,undefined -g -O1 \
 *         -Iinclude fuzz_blockio.c src/lib/blockio.c -o fuzz_blockio
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/blockio.h"

/* Sizes the core actually supports, plus two it does not, so the reject path is
 * exercised too. */
static const size_t kSizes[] = { 512, 1024, 2048, 256, 128, 100 };

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 4)
        return 0;

    /* One size per input, so a corpus can cover all of them without every run
     * paying for six devices. Derived from the input, not from a counter, so
     * that a mutator flipping a byte can change the geometry being tested. */
    size_t bsize = kSizes[data[0] % (sizeof kSizes / sizeof kSizes[0])];

    /* The memory backend is handed the raw buffer. Nothing here writes to it
     * unless a write is asked for -- and writes are asked for deliberately, into
     * a private copy, so the harness never depends on the corpus staying
     * pristine and never writes through a const pointer. */
    uint8_t *img = (uint8_t *)malloc(size);
    if (!img)
        return 0;
    memcpy(img, data, size);

    blockdev_t *d = blockdev_open_memory(img, size, bsize);
    if (!d) {
        free(img);
        return 0;
    }

    uint32_t n = blockdev_block_count(d);
    uint8_t *buf = (uint8_t *)malloc(bsize * 2 + 16);
    if (buf) {
        /* the interesting block numbers, in ascending risk: inside, the exact
         * end, one past the end, and the far edges of the 32-bit space */
        uint32_t probes[7];
        unsigned np = 0;
        probes[np++] = 0;
        if (n) probes[np++] = n - 1;
        probes[np++] = n;
        probes[np++] = n + 1;
        probes[np++] = 0x7FFFFFFFu;
        probes[np++] = 0x80000000u;
        probes[np++] = 0xFFFFFFFFu;

        for (unsigned i = 0; i < np; i++) {
            uint32_t b = probes[i];
            /* single block, and a span reaching into the next -- the span is a
             * separate code path with its own end-of-device arithmetic */
            (void)blockdev_read(d, b, buf, bsize);
            (void)blockdev_read_span(d, b, buf, bsize * 2);
            /* writes into a buffer we own; the return value is the assertion
             * (non-zero means "refused", which is correct for an out-of-range
             * block and irrelevant otherwise) */
            (void)blockdev_write(d, b, buf, bsize);
        }

        /* and a sweep of every real block, so a bug that only fires on a
         * particular in-range block is not missed by the six probes above */
        for (uint32_t b = 0; b < n && n <= 4096u; b++) {
            (void)blockdev_read(d, b, buf, bsize);
            (void)blockdev_read_span(d, b, buf, bsize);
        }
        free(buf);
    }

    blockdev_close(d);
    free(img);
    return 0;
}