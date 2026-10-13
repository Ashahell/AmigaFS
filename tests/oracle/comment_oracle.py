#!/usr/bin/env python3
"""Oracle for AmigaDOS comments: what amitools reads out of a real image.

Comments live in TWO places, and this script reads both with amitools' OWN
parsers so the layout is evidence rather than recollection:

  1. EVERY entry block (DIRC or not) carries the comment as a BCPL string whose
     length byte is at bsize-184 and whose data starts at bsize-183, max 79
     bytes. amitools reads it as ``_get_bstr(-46, 79)``; its ``_get_long(-N)`` /
     ``_get_bstr(-N, max)`` conventions are plain absolute offsets
     ``bsize - 4*N``, which is why its ``name`` at -20 lands on bsize-80 --
     the same place this project's parser reads the name.

  2. On a DIRC volume the dir cache carries a REDUNDANT copy, so a stale cache
     cannot be the only place a comment survives. Records start at offset 24 of
     the dir-cache block, ``num_records`` is at offset 12, and each record is

         +0   entry block pointer (4, big-endian)
         +4   size (4)
         +8   protect (4)
         +12  uid (2)   +14 gid (2)
         +16  days (2, signed)  +18 mins (2, signed)  +20 ticks (2, signed)
         +22  sub_type (1)
         +23  name_len (1)
         +24  name (name_len bytes)
         +24+name_len  comment_len (1)
         +25+name_len  comment (comment_len bytes)
         size = (25 + name_len + comment_len + 1) & ~1

     The dir-cache pointer itself is the rootblock's EXTENSION long at
     bsize-8 -- NOT offset 24, which is the hash table and reads as 0 on a DIRC
     volume, which looks exactly like "this volume has no dir cache".

Output is one stable line per comment found, for diffing against our reader:

    <image-basename> <block> <kind> <comment>

Entries with no comment are omitted; a volume is expected to produce the empty
result rather than being skipped.

    usage: comment_oracle.py <image> [image...]
"""
import os
import struct
import sys

from amitools.fs.block.FileHeaderBlock import FileHeaderBlock
from amitools.fs.block.UserDirBlock import UserDirBlock
from amitools.fs.block.DirCacheBlock import DirCacheBlock


class RawBlkDev:
    """The minimum interface amitools' Block expects."""

    def __init__(self, data, block_bytes=512):
        self.data = data
        self.block_bytes = block_bytes
        self.block_longs = block_bytes // 4

    def read_block(self, blk_num, num=1):
        off = blk_num * self.block_bytes
        return bytes(self.data[off:off + self.block_bytes * num])

    def write_block(self, blk_num, data):
        raise RuntimeError("this oracle is read-only")


def root_block_of(dev):
    """Reserved = 2, highKey = last block: root = INT((Reserved + highKey)/2)."""
    n = len(dev.data) // dev.block_bytes
    return (2 + n - 1) // 2


def dircache_pointer(dev, root):
    return struct.unpack_from(">I", dev.read_block(root),
                              dev.block_bytes - 8)[0]


def entry_blocks(dev, dirblk, depth=0, seen=None):
    """Walk the hash table (the authority, per decision C4) collecting entry
    blocks. Depth is bounded so a hostile image cannot make this loop forever."""
    if seen is None:
        seen = set()
    if depth > 4 or dirblk in seen:
        return
    seen.add(dirblk)
    raw = dev.read_block(dirblk)
    ht = struct.unpack_from(">I", raw, 12)[0] or 72
    n = len(dev.data) // dev.block_bytes
    for i in range(ht):
        p = struct.unpack_from(">I", raw, 24 + 4 * i)[0]
        if p == 0 or p >= n:
            continue
        yield p
        sec = struct.unpack_from(">i", dev.read_block(p), dev.block_bytes - 4)[0]
        if sec == 2:                      # ST_USERDIR
            yield from entry_blocks(dev, p, depth + 1, seen)


def read_comment(dev, blk):
    """amitools' own reading, choosing the block class by secondary type."""
    raw = dev.read_block(blk)
    sec = struct.unpack_from(">i", raw, dev.block_bytes - 4)[0]
    cls = UserDirBlock if sec == 2 else FileHeaderBlock
    try:
        obj = cls(dev, blk, False)
        obj.read()
    except Exception:
        return ""
    c = getattr(obj, "comment", None)
    if c is None:
        return ""
    s = c.get_ami_str()
    return s.decode("latin-1") if isinstance(s, bytes) else str(s)


def main(paths):
    for path in paths:
        data = open(path, "rb").read()
        dev = RawBlkDev(data)
        root = root_block_of(dev)
        name = os.path.basename(path)

        for blk in sorted(set(entry_blocks(dev, root))):
            c = read_comment(dev, blk)
            if c:
                sec = struct.unpack_from(">i", dev.read_block(blk),
                                         dev.block_bytes - 4)[0]
                kind = "dir" if sec == 2 else "file"
                print("%s %d %s %s" % (name, blk, kind, c))

        # the redundant DIRC copy, read with amitools' record parser
        first = dircache_pointer(dev, root)
        if first:
            blk = first
            seen = set()
            n = len(data) // dev.block_bytes
            while blk and blk not in seen and blk < n:
                seen.add(blk)
                try:
                    dc = DirCacheBlock(dev, blk)
                    dc.read()
                except Exception:
                    break
                if not dc.valid:
                    break
                for r in dc.records:
                    c = r.comment.get_ami_str()
                    if c:
                        print("%s %d dircache:%d %s"
                              % (name, r.entry, r.offset,
                                 c.decode("latin-1") if isinstance(c, bytes) else str(c)))
                blk = dc.next_cache
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["tests/fixtures/testffs.adf"]))
