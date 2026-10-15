# amigafs — mount classic AmigaDOS disks on Linux

`amigafs` mounts **AmigaDOS File System** volumes — the `DOS\0`–`DOS\5` format
used by Amiga floppy ADFs and RDB-partitioned hard-disk images — as an ordinary
Linux filesystem, via **FUSE3**, in C. Read-only by default; `-o rw` enables
writes.

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
| `ro` | **default** | Read-only. Every mutating op returns `EROFS` before resolving anything. |
| `rw` | off | Allow writes: create, write, truncate, mkdir, unlink, rmdir, rename. |
| `default_permissions` | on | The kernel enforces mode bits. |
| `tz=local` | off | Interpret Amiga timestamps as local time instead of UTC. |
| `amiga_ids` | off | Expose the stored 16-bit Amiga uid/gid instead of the mounting user's. |

With `-o rw` the mount writes straight through to the image file, so writes are
on disk the moment they are acknowledged. Renaming across RDB partitions
returns `EXDEV` (two partitions are two volumes); `mv(1)` handles that itself.
`chmod`/`chown`/`utimes` return `EPERM` — an AmigaDOS entry has nowhere to
store them, and a silent success would be worse than a refusal.

## CLI

```sh
./build/afs info   disk.adf              # volume type, DOS type, rootblock, geometry
./build/afs ls     disk.adf [dir] -R     # list a directory (recursively with -R)
./build/afs cat    disk.adf /path/to/file
./build/afs verify disk.adf [--strict]   # structure check; exit 1 on problems
./build/afs bitmap disk.adf              # free/used block summary
./build/afs bitmap-rebuild disk.adf --dry-run   # what a rebuild would reclaim
./build/afs bitmap-rebuild disk.adf --yes       # rebuild it (frees unreferenced blocks)
```

`afs ls` marks each entry: `d` directory, `f` file, `l` symlink, `h` file with
multiple hard links, `D` directory hard link.

On an RDB-partitioned image the first path component names the partition --
the same namespace the mount exposes, so the two can never disagree about
where a file lives:

```sh
./build/afs info   disk.hdd              # the partition table (names, DOS types, sizes)
./build/afs info   disk.hdd "OFS INTL"   # volume info for one partition
./build/afs ls     disk.hdd              # the partition names, as the mount root shows
./build/afs ls     disk.hdd "FFS DirCache" -R
./build/afs cat    disk.hdd "OFS/Trashcan.info"
./build/afs verify disk.hdd              # every partition, each report named
./build/afs bitmap disk.hdd FFS          # the bitmap needs one partition
./build/afs put    disk.hdd "FFS/NEWDIR/payload" payload.bin
./build/afs mkdir  disk.hdd "FFS/NEWDIR"
./build/afs rm     disk.hdd "FFS/NEWDIR/payload"   # files + empty dirs only
./build/afs mv     disk.hdd "FFS/NEWDIR/payload" "FFS/NEWDIR/renamed"
```

## What it supports

- **Volumes:** `DOS\0`–`DOS\5`. `DOS\6`/`DOS\7` (AmigaOS 3.2 long filenames),
  `muFS` and `PFS` are detected and **refused with a named reason**.
- **Media:** DD/HD floppy ADFs, RDB-partitioned hard-disk images (the 6-partition
  A590 fixture exposes one partition per DOS type), and hardfiles.
- **Files:** regular files, directories, hard links (one shared inode, real
  content), soft links (Amiga path syntax translated to a mount-relative POSIX
  path), empty files, and files spanning many extension blocks.
- **Extended attributes:** `user.amiga.comment` carries the AmigaDOS comment;
  nothing else is exposed, and unknown names return `ENODATA`.
- **Verification:** `afs verify` walks the directory tree reading the hash
  table **raw** — deliberately not through the same reader the filesystem uses,
  because that reader drops bad pointers and unrecognised blocks, and those are
  exactly the defects worth reporting. It checks block checksums, pointers in
  range, known block types, double-referenced blocks, the bootblock root field
  against the computed rootblock, the bitmap-valid flag, the bitmap page count
  against the volume size, blocks claimed as both bitmap and file, and each
  bitmap block's checksum. Exit status is 1 if anything is found.

### Comments

An AmigaDOS comment is exposed as the extended attribute `user.amiga.comment`:

```sh
getfattr -d -m - /mnt/africa/mod.And.DistantCall
# user.amiga.comment="protracker module"
getfattr -m - /mnt/africa/mod.And.DistantCall
# user.amiga.comment
```

The comment lives in the **entry block** at `bsize-184`, on every volume —
DIRC or not, file or directory. On a DIRC volume the dir cache holds a second,
redundant copy; the reader deliberately uses the entry block, so a damaged or
stale cache cannot lose a comment. `listxattr` advertises the attribute only for
a non-empty comment, so "no comment" and "empty comment" stay distinguishable,
and the value carries no trailing NUL, matching every other `user.*` attribute.

The layout was verified byte-for-byte against amitools' own parsers before it
was implemented; `docs/comment-layout.md` records the method, the worked
example, and the two traps worth knowing about.

## Known limits

- **Links are not written.** Both `afs rm` and the mount's `unlink`/`rename`
  refuse hard and soft links, and `afs mv` refuses to move them — the same set
  ADFlib's `adfRemoveEntry`/`adfRenameEntry` handle (FILE/DIR only).
  **This is no longer attributed to the oracle being unable to help** (see
  M24 in `docs/IMPLEMENTATION_PLAN.md`): hard-link *writing* was implemented
  and held correct — the on-disk shape is pinned from `links.adf` block 891,
  written by a real AmigaDOS, and `unadf` validates the result by reading it.
  It is the last missing filesystem operation and is simply not yet landed.
- **No rename across partitions**, by design: block addresses are
  partition-relative, so a cross-partition move is a copy+delete. The CLI
  refuses it by name, the mount returns `EXDEV`.
- **`chmod`/`chown`/`utimes` are refused** (`EPERM`) rather than silently
  ignored. AmigaDOS has only the 16-bit Amiga uid/gid plus protection bits,
  which map onto mode bits rather than being settable through POSIX calls.
- **CLI writes are for scripting and scratch copies.** `afs put`/`mkdir`/`rm`/
  `mv` operate on a volume path, never on a mount; use the mount with `-o rw`
  for ordinary filesystem work.
- **Directory hard links** are exposed as `ENOTDIR` rather than traversed —
  a hard-linked directory is a cycle the kernel dentry cache cannot represent.
  They are still listed.
- **Lookup is exact-match** in the mount (the kernel owns names); the Amiga's
  case-insensitivity is the CLI's default (`afs ls img /FORBIDDEN-FILES` finds
  `forbidden-files`), not behind the POSIX VFS and not behind a flag.
- **The directory cache is parsed and verified but not used for listing.** The
  hash table is the authority, so a stale cache cannot hide a file.
- **64-bit (`ffstd64`) volumes** are refused rather than misread.
- **Writes are refused on a volume whose bitmap cannot be trusted** — `bm_flag`
  not `-1` (AmigaDOS saying the map is suspect), or a bitmap page count the
  volume does not need. Allocating from such a map hands out a block that is
  still in use, and the corruption is silent because every pointer still
  resolves. `afs verify --strict` reports the disagreement; the default check
  does not, because a stale map is ordinary on a real Amiga disk.
  **The map can now be rebuilt** — see `afs bitmap-rebuild` below.
- **Bitmap rebuild** (`afs bitmap-rebuild <image> [--dry-run] [--yes]`) is a
  full unconditional rescan: it walks the tree, marks what it reaches, and frees
  everything else. That is the AmigaDOS policy rather than a cautious variant of
  it — `bm_Flags` means "TRUE if bitmap on disk is valid", and ADFlib's
  `adfReconstructBitmap` (our pinned oracle, `adf_bitm.c:443`) does the same
  thing: no pre-verification, no consult of `bm_Flags`, all-or-nothing if any
  block cannot be read. The safety rests on the free pass's bounds, which
  exclude blocks 0, 1 and the volume-tail mirror structurally.
  It is never run automatically on open — the refusal above stays exactly as it
  was. `--dry-run` rebuilds a **copy** and reports what would be reclaimed, so
  the original is never opened for writing.
  One behaviour is worth stating precisely: a link's own block is marked, but
  a **soft link's target is not followed** — and that is correct, not a
  limitation. A soft link's block holds the target's *path*, which dos.library
  resolves at access time, so the link owns nothing but its own block. The
  target is owned by its own directory entry and marked when the walk reaches
  it. The only case where this shows is a *dangling* soft link, where there is
  nothing to lose because the target does not exist. ADFlib does the same.
- A stale map is ordinary on a real Amiga disk: `afs verify --strict` flags
  **10 of the 11 fixtures**, and only `testhd.adf` is clean. That is a count of
  fixtures *flagged*, not of fixtures with a stale map — `cache_crash.adf` is
  flagged for being an unsupported `DOS\6` volume, and three of the fixtures
  already fail the default check for unrelated reasons.
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

Two gates build their own sanitizer binaries instead of using the normal build,
because "no crash" from an ordinary build only means "did not happen to
segfault this run":

- `fuzz_gate.sh` — hostile images through the **read** path (no crash, no hang).
- `m16_write_fuzz.sh` — random create/overwrite/range/truncate/rename/unlink/
  mkdir/rmdir/overflow sequences against an in-memory model, under **ASan +
  UBSan + LeakSanitizer**. After every operation the volume must still *verify*
  (checksums, pointer ranges, double-use, bitmap agreement, dir-cache vs hash
  table), the tree must contain exactly the entries the model predicts, and
  every file must read back byte-identical. It is what found a real leak on the
  out-of-space path of `afs_file_write_all`.
- `mount_fuzz.sh` — also carries a **write lane**: the same random workload
  driven through a real `-o rw` mount, so the FUSE write ops themselves are
  fuzzed. That is what found the stale-dentry and bitmap-trust corruptions
  below.

## Licence

MIT (see `LICENSE`). No third-party source is copied; ADFlib's test images carry
no separate licence and are downloaded at test time, never redistributed.