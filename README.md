# amigafs — mount classic AmigaDOS disks on Linux

`amigafs` mounts **AmigaDOS File System** volumes — the `DOS\0`–`DOS\5` format
used by Amiga floppy ADFs and RDB-partitioned hard-disk images — as an ordinary
read-only Linux filesystem, via **FUSE3**, in C.

It reads **OFS** (DOS\0, DOS\2, DOS\4), **FFS** (DOS\1, DOS\3, DOS\5),
International mode and directory-cache volumes, on both floppy images and
multi-partition hard-disk images. It is a from-scratch implementation: the only
external code is two **test oracles**, run as subprocesses and never linked.

## Build

Requires libfuse3, CMake ≥ 3.16 and a C99 compiler.

```sh
cmake -S . -B build
cmake --build build
```

## Mount

```sh
./build/amigafs-fuse disk.adf /mnt/africa
ls -la /mnt/africa
fusermount3 -u /mnt/africa
```

An **RDB-partitioned hard-disk image** mounts its partitions as directories
named after their RDB `DriveName`, so `/mnt/africa/Workbench/Startup` is a file
inside partition `Workbench`:

```sh
./build/amigafs-fuse hardfile.hdd /mnt/africa
ls /mnt/africa                 # one directory per partition
ls /mnt/africa/Workbench
```

A floppy mounts its root directly, and each partition of an RDB image mounts
its own root. Inside a partition every block pointer is partition-relative,
which is why each is opened as a window of its **own** length rather than "to
the end of the image" — an over-long window puts the computed rootblock in the
middle of the next partition, leaving only the last partition mountable.

### The block bitmap

Every volume's root carries a synthetic `.amigafs/` directory holding that
volume's raw block bitmap:

```sh
ls -l /mnt/africa/.amigafs/bitmap    # 220 bytes for a 1760-block floppy
```

One bit per block, most significant bit first, `1 = free`. It is a real
read-only file with real bytes, so `cmp` against a dump is a real comparison.

We report the bitmap over **every** block of the volume. Both ADFlib and
amitools report a scope two blocks smaller — their totals are always
`blocks − 2` — and neither says which two they omit (it is not the two
reserved blocks at the low end: on `arccsh.adf` those carry *used* bits, yet
the two omitted are free ones). The gate asserts the relationship that *is*
invariant — a total difference of exactly 2, with used and free taking the
remainder — rather than guessing at the rest.

### Mount options

| Option | Default | Meaning |
|---|---|---|
| `ro` | forced | The mount is read-only; no write operation is advertised. |
| `default_permissions` | on | The kernel enforces mode bits. |
| `tz=local` | off | Interpret Amiga timestamps as local time instead of UTC. |
| `amiga_ids` | off | Expose the stored 16-bit Amiga uid/gid instead of the mounting user's. |

## CLI

```sh
./build/afs info   disk.adf              # volume type, DOS type, rootblock, geometry
./build/afs ls     disk.adf [dir] -R     # list a directory (recursively with -R)
./build/afs cat    disk.adf /path/to/file
./build/afs verify disk.adf              # structure check; exit 1 on problems
./build/afs bitmap disk.adf              # free/used block summary
```

`afs ls` marks each entry: `d` directory, `f` file, `l` symlink, `h` file with
multiple hard links, `D` directory hard link.

## What it supports

- **Volumes:** `DOS\0`–`DOS\5`. `DOS\6`/`DOS\7` (AmigaOS 3.2 long filenames),
  `muFS` and `PFS` are detected and **refused with a named reason**.
- **Media:** DD/HD floppy ADFs, RDB-partitioned hard-disk images (the 6-partition
  A590 fixture exposes one partition per DOS type), and hardfiles.
- **Files:** regular files, directories, hard links (one shared inode, real
  content), soft links (Amiga path syntax translated to a mount-relative POSIX
  path), empty files, and files spanning many extension blocks.
- **Verification:** `afs verify` walks the directory tree reading the hash
  table **raw** — deliberately not through the same reader the filesystem uses,
  because that reader drops bad pointers and unrecognised blocks, and those are
  exactly the defects worth reporting. It checks block checksums, pointers in
  range, known block types, double-referenced blocks, the bootblock root field
  against the computed rootblock, the bitmap-valid flag, the bitmap page count
  against the volume size, blocks claimed as both bitmap and file, and each
  bitmap block's checksum. Exit status is 1 if anything is found.

## Known limits

- **Read-only.** Writing (including creating, deleting and the block
  allocator) is a later phase; the core keeps a clean seam for it.
- **Directory hard links** are exposed as `ENOTDIR` rather than traversed —
  a hard-linked directory is a cycle the kernel dentry cache cannot represent.
  They are still listed.
- **Lookup is exact-match** in the mount (the kernel owns names); the Amiga's
  case-insensitivity is available in the CLI, not behind the POSIX VFS.
- **The directory cache is parsed and verified but not used for listing.** The
  hash table is the authority, so a stale cache cannot hide a file.
- **64-bit (`ffstd64`) volumes** are refused rather than misread.
- Block sizes are read from the volume; the standard 512-byte floppy geometry
  is what the fixtures cover.

## Testing

Two independent oracles, pinned and run as subprocesses (never linked):

- **ADFlib** `73880de7` (v0.10.7) — library LGPL-2.1, CLI GPL-2.0+.
- **amitools** `0.8.1` (GPL-3.0) — the reference for `DOS\2`/`DOS\4` and RDB.

```sh
bash tests/fixtures/fetch_fixtures.sh   # download + sha256-verify the fixtures
bash tests/run_all_gates.sh             # unit tests + every oracle gate
```

The gates compare, per fixture, our listing against `adfls`, our file contents
against `unadf` extraction (byte-for-byte), our link resolution against the
secondary types, our RDB against `rdbtool`, and — for the mount — the bytes read
*through a real FUSE mount* against the oracle.

## Licence

MIT (see `LICENSE`). No third-party source is copied; ADFlib's test images carry
no separate licence and are downloaded at test time, never redistributed.