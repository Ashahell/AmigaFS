# Implementation Plan — `amigafs`: AmigaDOS File System for Linux via FUSE3

**Status:** rev 3 — all eight owner decisions resolved 2026-10-13. **Approved to implement.** No code written yet.
**Target:** `/home/miller/Work/projects/amigafs` (new standalone project; not yet a git repository)
**Date:** 2026-10-13

---

## 0. Review of rev 1 — what changed and why

Every correction below was checked against a primary source: Laurent Clévy's *ADF FAQ v1.14 (April 8th, 2017)* as shipped in `adflib/ADFlib/doc/FAQ/adf_info.txt`, the AROS `rom/filesys/afs/` sources, or the live GitHub API for `adflib/ADFlib`. The toolchain claims were re-measured: libfuse 3.18.2, CMake 4.4.3, GCC 16.2.1. All three are true.

| # | Rev 1 said | Source says | Fix |
|---|---|---|---|
| C1 | Variants are `DOS\0`–`DOS\3` = OFS/FFS/INTL/DIRC | FAQ: flag values are 0 OFS, 1 FFS, 2 OFS/INTL, 3 FFS/INTL, **4 OFS/DIRC&INTL, 5 FFS/DIRC&INTL**. DIRC implies INTL "but the related flag (bit #1) will stay cleared". The FAQ's list stops at 5. `DOS\6`/`DOS\7` come from a different source: amitools `amitools/fs/DosType.py` defines `DOS6 = 0x444F5306` as `DOS_OFS_INTL_LONGNAME` (`"DOS6:ofs+intl+longname"`) and `DOS7 = 0x444F5307` as `DOS_FFS_INTL_LONGNAME`. These are the AmigaOS 3.2 long-filename types. | Support `DOS\0`–`DOS\5`. Refuse `DOS\6`/`DOS\7` (long names, sourced from amitools rather than the FAQ) and `muFS`. |
| C2 | "There are NO 16-bit BE fields" | FAQ: file header and directory `UID`/`GID` are `ushort` at `BSIZE-196`/`BSIZE-194`. Dir-cache records hold `ushort` UID/GID and `short` days/mins/ticks, packed with only word alignment. Name and comment lengths are single bytes (BCPL strings). | Add `be16()` and byte-string readers. AROS only ever uses its LONG macros because it ignores those fields; that does not describe the format. |
| C3 | FFS reads via `data_blocks[]`; OFS follows `next_data` | FAQ: `data_blocks[]` is used by **both** OFS and FFS. It is stored **in reverse**: "the first is at datablock[71-0]", first entry at `BSIZE-204`, with `high_seq` counting the valid entries. | Read both variants through the table plus extension chain, in reverse order. For OFS, `seq_num`/`next_data`/`data_size` are a cross-check. |
| C4 | DIRC "must be read to list/lookup correctly" | FAQ §4.7: dir-cache blocks exist "To speed up directory listing". Directory blocks keep their hash tables. | The hash table is the authority for lookup and listing. The dir cache is parsed and verified (`afsverify`), and may later serve as a fast path. A stale cache must never hide a file. |
| C5 | AROS `afs` is a behavioural oracle | AROS tests OFS as `dosflags == 0` (`filehandles1.c:763` and others), so `DOS\2`/`DOS\4` OFS volumes are read as FFS there. Its only dir-cache code clears the pointer in the validator (`validator.c:686`). It parses no RDB. | AROS is a **reference for code reading**, not an oracle. Use it only for checksum, hash and `capitalch` (`extstrings.c:7-14`). Its code is AROS Public License, so we re-implement and never copy. |
| C6 | ADFlib CLI is LGPL-2.1 | GitHub lists the library as `LGPL-2.1`. The CLI tools are **GPL-2.0-or-later** (`examples/adfls.c` header). The repository ships `COPYING-GPL2`. | Running the CLI as a subprocess is unaffected. Never vendor or copy ADFlib code. ADFlib is not installed here, so M0 builds the oracle out of tree. |
| C7 | `adfformat -t 0..7` generates fixtures | `adfformat` sets the 3 low flag bits. Types 6 and 7 collide with AmigaOS 3.2's long-filename meaning. | Generate with `-t 0..5` only. |
| C8 | Rootblock: "floppy 880 / middle of partition" | FAQ: `rootKey = INT((numReserved + highKey) / 2)`, with `highKey = numCyls*numSurfaces*numBlocksPerTrack - 1`. That gives 880 for DD and 1760 for HD. AROS computes the same (`volumes.c:232`). Inside a partition, block pointers are **partition-relative**. | One formula for every medium. Check that the block is `T_HEADER`/`ST_ROOT` before trusting it, and translate partition-relative pointers in `blockio`. |
| C9 | not covered | Names are ISO-8859-1, case-insensitive and case-preserving, at most 30 characters. `win32-names.adf` exists for exactly this. | Convert Latin-1 ↔ UTF-8 at the FUSE boundary. Hash and compare in Latin-1 with `capitalch`. Lookup policy: §6 D5. |
| C10 | not covered | Owner RWED bits (0–3) are active-low: a set bit means **denied**. Bits 4–7 (HSPA) and the group/other bits 8–15 are active-high. | `translate.c` inverts bits 0–3 only. Unit-test every bit. |
| C11 | Softlink target used verbatim | FAQ §4.6.2: the soft link holds an Amiga path C-string. `Vol:`, `/` = parent and `:` = volume root are Amiga syntax, not POSIX. | Translate to a POSIX path relative to the link: leading `/` → `../`, `Vol:` or `:` → the mount root. If the target cannot be expressed, return `EIO`. |
| C12 | Hard links "incl. dir links" | A directory hard link forms a cycle that `find`/`du`/`fsck` and the kernel dentry cache do not expect. | File hard links map to the same inode (the `real_entry` block) and count toward `st_nlink`. Directory hard links are exposed as symlinks to the target's path. |
| C13 | Fixture list | The live listing of `tests/data/Dumps/` also has `arccsh.adf`, `testhd.ami`, `testhd.cmd` and `a590_WD93028XA_6parts_not_formatted.hdd.gz`. | Pin every fixture by URL plus sha256 (M0). |
| C14 | Threading risk left open | — | Run single-threaded (`-s`) until the cache has its own concurrency test (M8). |

---

## 1. Goal & agreed scope

Mount classic **AmigaDOS File System** volumes on Linux as a normal read-only filesystem, via FUSE3, in C: OFS, FFS, INTL and DIRC, which is DOS types `DOS\0`–`DOS\5`. Sources are Amiga floppy ADFs (DD and HD), hardfiles, and RDB-partitioned hard-disk images.

| Decision | Choice |
|---|---|
| Architecture | **Library-first**: platform-independent core `libafs`, a thin FUSE shell and a thin CLI shell |
| Read/write | **Read-only now**, with write-ready seams in the core. Mount always forces `ro`. |
| Media | DD/HD floppy ADF, hardfile (HDF without RDB), RDB hard-disk image |
| Variants | `DOS\0`–`DOS\5`. Detect and refuse `DOS\6`/`DOS\7`, `muFS`, `PFS`, `SFS`. |
| Extras | Hard links and softlinks, RDB multi-partition, exposed block bitmap, CLI inspector and verifier |
| Prior art | ADFlib CLI (GPL-2.0+) as an **independent test oracle**, run as a subprocess and never linked or copied |
| Reference | ADF FAQ v1.14 (normative for us), AROS `rom/filesys/afs/` (code reading only, APL; see C5) |
| Toolchain | Measured: libfuse 3.18.2, CMake 4.4.3, GCC 16.2.1 |
| Project licence | **MIT** (D8, §6) |

**Out of scope:** writing; PFS3/SFS/other filesystems; real floppy hardware; muFS; AmigaOS 3.2 long filenames (`DOS\6`/`DOS\7`); 64-bit `ffstd64` volumes; dir-cache as the primary listing path.

---

## 2. Format facts that gate the design

Section numbers refer to ADF FAQ v1.14.

1. **Byte order:** big-endian. Fields are 32-bit (`ulong`/`long`) and 16-bit (`ushort`/`short`: UID/GID, dir-cache dates), plus single-byte lengths. Provide `be32()`, `be16()`, `bstr()`, all built on `memcpy`, never a pointer cast. Hash tables, extension tails and dir-cache records sit at arbitrary or word-aligned offsets.
2. **Normal checksum:** `sum = Σ be32(long)` over the block, with the checksum long counted as zero. A block is valid iff the stored value equals `-sum` (two's complement), i.e. iff the whole-block sum is 0. The checksum long is at byte 20 for root, header, extension, OFS data and dir-cache blocks, and at byte 0 for bitmap blocks.
3. **Bootblock checksum:** 32-bit add with end-around carry over the 1024-byte bootblock, then `NOT`. It sits at byte 4. It is a separate function and never reuses the normal one.
4. **DOS type and flags:** the first long of the bootblock is `'DOS' | flags`. If it is not on block 0, AROS also checks block 1 (`volumes.c:86-93`); match that. OFS is `(flags & 1) == 0`. INTL folding applies when `flags >= 2`, because DIRC implies INTL. In a partition the bootblock is the partition's first block.
5. **Hash:** `h = len (max 30); for each c: h = (h*13 + capitalch(c, flags)) & 0x7FF; return h % (BSIZE/4 - 56)`. `capitalch` folds `a`–`z`, and for INTL also 224–254 except 247 (`extstrings.c:7-14`).
6. **File data:** the header's `data_blocks[]` (`BSIZE/4 - 56` entries, 72 at 512 B) is filled from the **end** backwards, with `high_seq` valid entries. Extension blocks (`T_LIST`, `ST_FILE`) continue the same reversed table. An OFS data block has a 24-byte header (`type`, `header_key`, `seq_num`, `data_size`, `next_data`, checksum) and `BSIZE-24` bytes of payload. An FFS data block is raw `BSIZE` bytes. Truncate to `byte_size`.
7. **Block size:** `BSIZE = SizeBlock*4 * SectorsPerBlock` from the partition's DosEnvec, and 512 on floppies. Every table size derives from `BSIZE`; nothing is hardcoded as 72.
8. **Rootblock:** `INT((Reserved + highKey)/2)` (C8). Validate type and secondary type. The bitmap-valid flag and the bitmap pointers (25 in the root plus an extension chain) come from the root.
9. **Secondary types:** `ST_ROOT 1`, `ST_USERDIR 2`, `ST_SOFTLINK 3`, `ST_LINKDIR 4`, `ST_FILE -3`, `ST_LINKFILE -4`. Any other value is a verifier error and a `EIO` on access.
10. **Dates:** days since 1978-01-01, minutes, and ticks (1/50 s). Amiga clocks are local time with no zone stored; see §6 D6.
11. **Names:** Latin-1, at most 30 bytes (C9). Comments are at most 79 bytes, exposed as the xattr `user.amiga.comment`.

---

## 3. Module layout

```
amigafs/
├── CMakeLists.txt
├── README.md
├── LICENSE                     # owner decision (§1)
├── include/amigafs/
│   ├── afs.h                   # volume, node, afs_stat, error codes
│   ├── be.h                    # be32/be16/bstr (unaligned-safe)
│   ├── blockio.h               # the single I/O seam (block N of a partition window)
│   ├── checksum.h
│   ├── hash.h
│   ├── names.h                 # Latin-1 <-> UTF-8, capitalch compare
│   ├── rdb.h
│   └── dir.h
├── src/lib/                    # core: no FUSE, no OS calls except via blockio
│   ├── be.c  checksum.c  hash.c  names.c         # M1
│   ├── blockio.c                                 # M1 (window offset + bounded cache)
│   ├── volume.c  blocktypes.c                    # M2
│   ├── dir.c                                     # M3 (hash-table authority)
│   ├── dircache.c                                # M3 (parse + verify only)
│   ├── file.c                                    # M4
│   ├── links.c                                   # M5
│   ├── bitmap.c  verify.c                        # M6
│   └── rdb.c                                     # M7
├── src/fuse/  main.c  ops.c  translate.c         # M8
├── src/cli/   main.c                             # M9 (afs info|ls|cat|verify|bitmap)
├── tests/
│   ├── unit/
│   ├── fixtures/fixtures.lock  # URL + sha256 per image; images themselves gitignored
│   └── oracle/                 # scripts diffing our CLI against ADFlib CLI output
└── docs/  format-notes.md  milestones.md
```

**The I/O seam:** the core asks for "block N of partition P". `blockio` adds the partition's start offset (C8) and checks bounds against the partition's block count. A pointer out of range is an error, never a read past the partition. FUSE and CLI each supply a `pread()` backend.

---

## 4. Milestones (each gated independently)

### M0 — Environment and oracle
- `git init`. Licence decision recorded (§1).
- Build ADFlib's CLI from a pinned commit, out of tree, under `~/Work/adflib-oracle/`. Record the commit sha. This is not a dependency of our build.
- `tests/fixtures/fixtures.lock`: every image from `tests/data/Dumps/` (C13) with sha256, plus `adfformat -t 0..5` images for each DOS type on both DD and HD. A fetch script verifies each sha and refuses on mismatch.
- **Gate:** the oracle runs `adfls` on `testffs.adf`; every fixture's sha matches.

### M1 — Foundations
- `be32`/`be16`/`bstr`; normal and bootblock checksums; `hash` with `capitalch`; Latin-1 ↔ UTF-8 conversion; `blockio` with a partition window and a bounded LRU cache.
- **Gate:**
  - golden vectors for both checksums, one taken from a real fixture block;
  - hash vectors for each flag class, including bytes 224–254, byte 247 unchanged, and DIRC (`flags 4/5`) folding like INTL;
  - Latin-1 round trip for all 256 bytes;
  - an out-of-window read refuses.

### M2 — Volume open and variant detection
- DOS type on block 0 or block 1; flags; refuse anything outside `DOS\0`–`\5` with a named reason.
- Rootblock by formula, validated. Parse and validate the header, extension, bitmap and dir-cache block types.
- **Gate:** every fixture reports the variant and rootblock that `adfinfo` reports. The unformatted A590 image is refused cleanly.

### M3 — Directories
- Lookup: hash, then bucket, then follow `hash_chain`, comparing with `capitalch` on Latin-1. Listing: the whole table plus chains. A cycle guard bounds the chain length by the block count.
- `dircache.c`: parse records (`be16` fields, padding byte); not used for listing.
- **Gate:** listings of every fixture match `adfls`, including the DIRC images and `win32-names.adf` (names compared as UTF-8).

### M4 — File data
- The reversed `data_blocks[]` plus the extension chain for both variants. OFS header cross-checks (`seq_num`, `data_size`, `next_data`) produce a warning, never a silent fix.
- **Gate:** byte-for-byte match with `unadf` extraction for every file in every fixture, including empty files and files longer than one extension block.

### M5 — Links
- File hard links: inode = the `real_entry` header block; `st_nlink` counted through the `next_link` chain. Directory hard links become symlinks (C12). Softlinks: Amiga path → POSIX path (C11).
- **Gate:** `links.adf` and `test_link_chains.adf` match the oracle, plus a unit table of path translations: `Vol:a/b`, `:a`, `/x`, `//x`, `a/b`.

### M6 — Bitmap and verifier
- `bitmap.c`: read the free/used bitmap, honouring the bitmap-valid flag. Expose it as `/.amigafs/bitmap`, a read-only synthetic file under a reserved directory name that cannot be an Amiga name (§6 D4).
- `afsverify`: checksums, pointers in range, known types, no block used twice, used-versus-bitmap agreement, dir-cache consistent with the hash table, extension chains terminating.
- **Gate:** clean images verify clean. `ffdisk0049.adf` reports the same problem class as `adfbitmap`. A dir cache deliberately made stale (via a test-only block patch in a temporary copy) is reported, and the file stays visible.

### M7 — RDB and partitions
- `RDSK` chain, `PART` blocks, DosEnvec geometry (`SizeBlock`, `SectorsPerBlock`, `Reserved`, `LowCyl`, `HighCyl`). Partitions map to subdirectories named after `DriveName`, made unique.
- **Gate:** the 6-partition A590 image shows all 6 volumes. Each listing matches the oracle where ADFlib's RDB support (marked experimental upstream) agrees. Any disagreement is investigated and recorded, never auto-accepted.

### M8 — FUSE3 shell
- Low-level API: `lookup`, `forget`, `getattr`, `opendir`/`readdir`/`releasedir`, `open`/`read`/`release`, `readlink`, `statfs`, `getxattr`/`listxattr`. Mount options forced: `ro`, `default_permissions`.
- `translate.c`: protection bits (C10), dates (D6), uid/gid (D7), size.
- Inode = header block number (partition-qualified). Lookup reference counts are honoured through `forget`.
- Single-threaded (`-s`) by default; multi-threaded only after the cache passes a concurrency stress test.
- **Gate:** mount each fixture. `ls -lR`, `cat`/`sha256sum`, `readlink` and `getfattr` match the CLI output. `fusermount3 -u` is clean, with no leaked file handles.

### M9 — CLI and end-to-end harness
- `afs info|ls|cat|verify|bitmap`. An oracle script runs per fixture and is wired into `ctest`. The README covers mount usage and known limits.

---

## 5. Testing strategy

- **Oracle 1:** ADFlib CLI at a pinned commit (`adfls`, `adfinfo`, `unadf`, `adfbitmap`). It is the reference for listings, metadata, content and bitmap.
- **Oracle 2:** unit golden vectors (checksums, hash and INTL fold, Latin-1, protection bits, dates, softlink paths).
- **Cross-check, not an oracle:** AROS `afs`, for checksum, hash and `capitalch` only (C5).
- **Rule:** every read path must match the oracle on every fixture before it counts as done. Where the oracle is known to be weak (RDB), a disagreement is a recorded finding, not a pass.
- **Fixtures:** pinned by sha256. ADFlib's test data has no separate licence. Download it at test time and never commit or ship it. The `adfformat`-generated images are ours.

---

## 6. Decisions — ALL RESOLVED 2026-10-13

Owner decisions, recorded. These are settled, not open.

- **D1 — Low-level FUSE3 API.** **Accepted:** `fuse_lowlevel.h`, for inode lifetime and readdir control. The kernel owns names; we own blocks.
- **D2 — ADFlib as a subprocess oracle only.** **Accepted.** Never linked, never vendored, never copied. M0 builds it from a pinned commit outside our tree, keeping GPL-2.0+ (CLI) and LGPL-2.1 (library) out of every build and artifact.
- **D3 — Write-ready seams.** **Accepted:** `blockio` (partition window plus a future write backend) and parsing kept separate from walking. No allocator in this plan.
- **D4 — Bitmap exposure.** **Accepted:** `/.amigafs/bitmap`, a read-only synthetic file. A leading `.` is legal on Amiga, so the reserved directory is checked against real entries at open time; on collision the mount is refused with a named reason. `statfs` must still answer free space.
- **D5 — Lookup case policy.** **Accepted: exact-match in the mount** (byte-exact on the presented UTF-8 name). Case-insensitive matching is available **only** through the CLI (`afs ls -i`). Rationale: the kernel resolves and caches paths, so accepting `FOO` for `foo` would put two dentry names on one inode and desynchronise `ls` from what is stored.
- **D6 — Timezone.** **Accepted: UTC by default**, with `-o tz=local` for wall-clock parity with a real Amiga. Rationale: deterministic, reproducible across machines, matches common ADF tooling.
- **D7 — uid/gid.** **Accepted: the mounting user's uid/gid by default**, with `-o amiga_ids` exposing the stored 16-bit UID/GID. `amiga_ids` ships with a documented note on the FAQ's dir-block GID-width contradiction (§7).
- **D8 — Project licence.** **Accepted: MIT.** `LICENSE` is added in M0. Clean because everything is re-implemented and nothing is copied from AROS (APL) or ADFlib (GPL-2.0+/LGPL-2.1).

### Mount options (complete set)

| Option | Default | Meaning |
|---|---|---|
| `ro` | forced | Read-only this phase; write ops are not advertised to the kernel. |
| `default_permissions` | on | The kernel enforces mode bits, so `translate.c` need not re-implement them. |
| `tz=local` | off | Interpret Amiga timestamps as local time instead of UTC. |
| `amiga_ids` | off | Expose the stored 16-bit UID/GID instead of the mounting user's. |
| `threads=N` | 1 | Single-threaded; multithreaded only after the block cache passes its concurrency stress test (M8). |

---

## 7. Open risks

- **INTL/DIRC exactness:** the FAQ is thin here. Generate `-t 2..5` images, diff against the oracle, and record any oracle disagreement.
- **ADFlib RDB support is experimental:** see the M7 gate.
- **Hostile images** (`ffdisk0049.adf`, fuzzed blocks): every pointer is bounds-checked, every chain is cycle-guarded, and nothing in the core aborts. Add a libFuzzer target over `blockio` with a memory backend after M4.
- **Directory-block GID width (FAQ inconsistency):** the FAQ lists the file header's `GID` as `ushort` at `BSIZE-194`. The directory block lists `UID` as `ushort` at `BSIZE-196` but `GID` as `ulong` at `BSIZE-194`, a 4-byte field overlapping the 2-byte one before it. This is a spec error, not an established fact. Until it is settled, `-o amiga_ids` (D7) exposes directory GID as the 16-bit value and records the question. Settle it by hex-dumping a directory block from a MultiUser-formatted image, cross-checked against ADFlib's `bDirBlock` and amitools' dir-block layout. Close it in M2.
- **Large volumes:** a 32-bit block count with `BSIZE` up to 32 KiB stays in range. Refuse `ffstd64` explicitly.
