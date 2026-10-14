# Implementation Plan — `amigafs`: AmigaDOS File System for Linux via FUSE3

**Status:** rev 4 — M0 complete and gated; C1 confirmed against live fixtures. Eight owner decisions resolved 2026-10-13. **Approved to implement.**
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
| C15 | (rev 2 draft) ADFlib "rejects" `DOS\2`/`DOS\4` | `adfVolIsFsValid()` accepts OFS only without INTL or DIRC, but its only callers are the filesystem-name print (`src/adf_dev.c:529`, giving `???`) and `adfformat`'s overwrite guard (`examples/adfformat.c:134`). It is not on the mount or read path. ADFlib's own `testofs.adf` is `DOS\2`. | Keep C1. Oracle scripts compare listings and content, never ADFlib's type string, and record the display gap. |
| C16 | (rev 2 draft) one oracle | ADFlib labels `DOS\2`/`DOS\4` as `???`. amitools models `DOS\0`–`DOS\7` but cannot read a standalone DIRC floppy's dir cache. ADFlib also has no single rootblock formula (§2.8). | amitools `0.8.1` is the second oracle (§5). `cache_crash.adf` (`DOS\6`) is a negative fixture: the pass condition is a named refusal, with no crash and no partial mount. |

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
8. **Rootblock:** `INT((Reserved + highKey)/2)` (C8) — the FAQ/AROS formula, where `highKey = numCyls*numSurfaces*numBlocksPerTrack - 1`. We use **one** formula for every medium. Three rules make it safe:
   - Validate the block is `T_HEADER`/`ST_ROOT` before trusting it.
   - Cross-check the bootblock's root field **when it is non-zero** and warn on a mismatch. (Many ADFlib images leave it 0 and rely on the computed value, e.g. `testffs.adf`.)
   - Any other oracle-vs-formula difference is an **oracle limitation to record**, not a reason to change the formula.

   **Why not ADFlib's:** ADFlib has *no single formula* — floppies take it from the bootblock root field (`src/adf_dev_flop.c:88`), hardfiles use `sizeBlocks/2` (`src/adf_dev_hdfile.c:120`), and `adfVolCalcRootBlk` is `(last-first+1)/2` which ignores `Reserved`. Our formula agrees with ADFlib when `Reserved = 2` and the block count is even, which covers DD (880) and HD (1760).
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

### M0 — Environment and oracle ✅ COMPLETE (commit `027048d`)
- `git init`; MIT `LICENSE` added (D8).
- ADFlib CLI built **out of tree** at `~/Work/adflib-oracle/ADFlib`, pinned to **`73880de78bf472a1cc7d60f07fc217e02d571268` (v0.10.7)**, 0 compile errors, all six tools built. Not a dependency of our build.
- **Second oracle: amitools `0.8.1`** (`xdftool`, `rdbtool`, `vamos`), pinned.
- `tests/fixtures/fixtures.lock`: 13 fixtures, each sha256 **measured from the bytes actually fetched**, plus its **measured bootblock DOS type**. Images are gitignored, downloaded at test time, never redistributed.
- `fetch_fixtures.sh`: sha256-verified fetch. Proven **idempotent** (0 refetches on rerun) and **load-bearing** (one flipped byte is caught, exit 1).
- `tests/oracle/m0_gate.sh`: **PASS**, 13 checks.
- **Measured fixture census** (this is the ground truth M0 established):

| Fixture | DOS type | Character |
|---|---|---|
| `arccsh.adf`, `g1a30c.adf`, `blank.adf`, `ffdisk0049.adf` | `DOS\0` | plain OFS |
| `links.adf`, `test_link_chains.adf` | `DOS\1` | plain FFS, links |
| **`testofs.adf`** | **`DOS\2`** | **OFS + INTL** |
| **`testffs.adf`**, `testhd.adf`, `win32-names.adf` | **`DOS\5`** | **FFS + DirCache&INTL** |
| `cache_crash.adf` | `DOS\6` | negative: corrupt bitmap + unsupported type |
| `a590_WD93028XA_6parts.hdd` | RDB | **one partition per `DOS\0`–`DOS\5`** |

- **Consequences, all now load-bearing:**
  - **C1 is confirmed by the corpus.** `testofs.adf` really is `DOS\2`, i.e. exactly the OFS/INTL case AROS misreads (it tests `dosflags == 0`). C5's downgrade of AROS to reference-only is justified by a real fixture.
  - **C4 is load-bearing from day one:** `DOS\5` (DIRC) is 3 of 11 floppy images, so "the hash table is the authority and a stale dir cache must not hide files" is a core requirement, not a refinement.
  - **Oracle complementarity is now mapped.** ADFlib lists DIRC floppies correctly but *labels* `DOS\2`/`DOS\4` as `???`; amitools reads and labels `DOS\0`–`DOS\7` but its `xdftool list` cannot read a standalone DIRC floppy's dir-cache blocks. Each covers the other's gap.
  - **`cache_crash.adf` is a negative fixture.** Pass = refuse with a named reason (`DOS\6 unsupported`), no crash, no partial mount. It also exercises the `DOS\6` refusal path itself.
  - The RDB image gives **full variant coverage in one mount** (M7).

### M1 — Foundations ✅ COMPLETE (commits `3d34cf6`, `e37303c`)
- `be32`/`be16`/`bstr`, `put_be32`/`put_be16`; both checksums; `hash`+`capitalch`+`afs_name_equal`; Latin-1 ↔ UTF-8; `blockio` with partition windows.
- **Gate: met.** 4/4 ctest, **0 warnings** under `-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror`. TDD throughout — every test was watched failing first.
- **Golden vectors anchored to real bytes, not to our own arithmetic:**
  - The embedded 512-byte `testffs.adf` rootblock validates against its own stored checksum `0x2ed093f8`.
  - The bootblock algorithm was verified offline against every fixture: it matches `arccsh.adf` and `g1a30c.adf` **exactly**; the non-matches are fixtures that legitimately store `0` (non-bootable floppies) or the known-bad `ffdisk0049`.
  - `français` (a real name in `testofs.adf`, `DOS\2`) hashes **71 without INTL and 47 with it** — the exact wrong-bucket bug the INTL fold prevents.
  - DIRC flags 4/5 proven to fold identically to INTL for all 256 byte values.
- **The integration gate earned its keep.** `m1_integration.c` runs the real stack over all 11 floppies: DOS type read correctly, rootblock located by the single formula (**880 for DD, 1760 for the HD fixture automatically**), and **every rootblock validates**. It found a bug unit tests could not: the **bootblock is 1024 bytes and spans blocks 0 and 1**, but `blockdev_read` refused `len > block_size`, so every bootblock read failed. Added `blockdev_read_span()` with whole-span bounds checking. It also caught a bug *in the gate itself* — the DOS id was compared through a host-endian cast, so it never matched big-endian `'D','O','S'`.
- **Not yet built:** the bounded LRU block cache. Deferred to M8 with its concurrency test (D-level `threads=N` default 1); the blockio contract does not depend on it.

### M2 — Volume open and variant detection ✅ COMPLETE (commit `0583e0b`)
- `blocktypes.c`: primary+secondary types resolved to one kind (`T_HEADER` is disambiguated by its secondary type; dir-cache is the distinct primary type **33**). Every offset derives from the block size. Dir-cache records are walked by their **variable** length (26–77 bytes) with a word-alignment skip; `records_nb` is **clamped** to what the block can hold, because it is untrusted on-disk data.
- `volume.c`: DOS id from block 0 **or block 1**; `DOS\0`–`DOS\5` supported; `DOS\6`/`DOS\7` and muFS/PFS **refused by name**; OFS = absence of the FFS bit (`DOS\2`/`DOS\4` are OFS); rootblock located by the single formula then **validated as `T_HEADER`/`ST_ROOT` before trust**.
- **Gate: met — `m2_gate` PASS on all real fixtures.** Reports exactly the measured types (`DOS\0`×4, `DOS\1`×2, `DOS\2`×1, `DOS\5`×3), rootblock 880 (DD) / 1760 (HD), and `cache_crash.adf` refused as `DOS\6` with a named reason. Volume names agree with ADFlib on all 10 openable fixtures; the one intentional divergence is `cache_crash.adf`, which ADFlib reads as `DOS\6`.
- **Bugs caught by tests (both silent-corruption class):** the name BCPL string is read at `BSIZE-80` (the length byte), not `BSIZE-79` — the off-by-one would have mangled every filename on every volume; and the DOS id was compared through a host-endian cast so it never matched big-endian `'D','O','S'`.
- 6/6 ctest, 0 warnings under `-Werror` with `-Wconversion`.

### M3 — Directories ✅ COMPLETE (commit `f173c02`)
- `dir.c`: lookup = hash → bucket → follow `hash_chain`, comparing with `capitalch` (Amiga case-insensitive; returns the **stored** spelling, so the mount's exact-match policy (D5) is unaffected). Listing walks every bucket and every chain.
- **Robustness:** every block pointer on this path is untrusted on-disk data. Pointers are bounds-checked before use, chains are bounded by the volume block count, a self-referential chain terminates, and an out-of-range pointer ends the chain. The dir cache is **never consulted** (C4) — a test proves a dangling cache pointer cannot hide a file.
- **Gate: met — `m3_gate` PASS on all 10 openable fixtures (69 entries), byte-for-byte vs `adfls`**, including the `DOS\5` DIRC images, the accented `français` from the INTL `DOS\2` fixture, the real `same_hash`/`same_hash2`/`same_hash3` collision chain, and both link-chain fixtures.
- **Modelling facts pinned while building the tests:** Amiga directories have **no defined order** (entries emerge in hash-bucket order — listing asserts membership, sorting belongs to the FUSE layer); and the synthetic test volume must be exactly **1760 blocks** for the rootblock formula to land on 880.
- 7/7 ctest, 0 warnings under `-Werror` with `-Wconversion`.

### M4 — File data ✅ COMPLETE (commit `065bf55`)
- `file.c`: **both** OFS and FFS read through the reversed `data_blocks[]` table plus the extension chain (file block *i* at index `entries-1-i`). OFS's `seq_num`/`next_data`/`data_size` are cross-checks only. Reads past EOF clamp; hard links resolve through `real_entry`.
- **Gate: met — all 216 files ADFlib's `unadf` extracts across every fixture are byte-for-byte identical** (sha256 + size), including a 173 847-byte OFS file spanning **five** header/extension blocks and an 80-file Fred Fish disk. Ours is a documented superset by **6 hardlink aliases** (unadf resolves a hard-linked entry to its real file; we expose the link as its own path with identical content).
- **The bug this milestone caught — and it only appeared against real disks:** directory blocks **do not store `ht_size`** (offsets 8/12/16 are unused there and read 0). Reading that field made **every subdirectory on every Amiga volume appear empty**. Verified across five fixtures; `dir.c` now falls back to the derived table size, and `test_dir` has a reproducing test. This had also been silently capping the M3 gate at depth 1.
- **A second real bug:** a read spanning a block boundary returned only the first block's tail, because `nblocks` ignored the partial skip inside it (offset 500 of a 512-byte payload needs 2 blocks).
- **Gate hardening:** exact Python set comparison instead of `comm` (which warns about tab ordering — a warning must never mask a real difference); hashing from stdin because GNU `sha256sum` escapes backslashes in filenames (which corrupted the `win32-names` comparison); `.info` sidecars filtered on both sides.
- 8/8 ctest, 0 warnings under `-Werror` with `-Wconversion`.

### M5 — Links ✅ COMPLETE (commit `be83d46`)
- `links.c`: hard links chased through `real_entry` (BSIZE-44), **including links-to-links** with a loop guard, so one inode serves every name and `afs_file_nlink` counts the `next_link` chain. Directory hard links (`ST_LINKDIR`) are reported but **never traversed** — a hard-linked directory cycles the dentry cache, `find` and `du` (C12); the FUSE layer exposes them as symlinks.
- Soft links (C11) translated: `Vol:`/`:` = volume root, `/x` and `//x` = volume-root-absolute, bare/multi-component = relative with one `../` per level. Because **the mount root *is* the volume root**, an absolute Amiga path becomes a relative POSIX path directly — every emitted target is non-empty and relative.
- **Gate: met — `m5_gate` PASS.** Hard-file links and soft links resolve across `links.adf`, `test_link_chains.adf`, `testffs.adf`; the 3 directory hard links are exposed but not traversed; and hard links are proven to **share content by sha256** (1 shared hash in `links.adf`/`test_link_chains.adf`, 2 in `testffs.adf` — each a real-file + hard-link pair).
- **A real bug the tests caught:** the softlink symbolic name is a plain **C-string, not a BCPL length-prefixed string**. Reading it with `bstr()` ate the first character (`target.txt` → `arget.txt`). The *file name* on the same block is BCPL; only the target is not.
- 9/9 ctest, 0 warnings under `-Werror` with `-Wconversion`.

### M6 — Bitmap and verifier ✅ COMPLETE (commit `6c1e05d`)
- `bitmap.c`: bit helpers (1=free, 0=allocated) and assembly of the volume bitmap across its storage blocks. The **first** storage block's long 0 is its checksum; every later storage block's long 0 is ordinary map data.
- `verify.c`: the structure checker — checksums, pointers in range, known block types, no block used twice, the bitmap-valid flag, and every bitmap block's checksum. Never mutates the image.
- **Gate: met.** The verifier reproduces **both** defects the hostile `ffdisk0049.adf` carries, which is exactly what ADFlib flags on it: the bootblock root field `0x444F5302` (the string `'DOS\2'`, garbage) disagreeing with the computed rootblock 880, and an invalid bitmap (`bm_flag=1`, checksum fails). Every clean fixture verifies clean; `cache_crash.adf` is rejected as `DOS\6` with the named reason.
- **A subtlety resolved and recorded:** the block checksum settles to **one** rule for every block type — stored = negation of the sum of the *other* longs, equivalently the sum *including* the stored value is 0. That covers the checksum-at-20 family **and** bitmap blocks (checksum at byte 0). A note that they differ was my own arithmetic slip in a throwaway script, corrected by re-deriving against real blocks.
- **Rootblock cross-check validated:** `g1a30c.adf`'s bootblock says root=879, but the true rootblock is 880 (verified: 880 is `T_HEADER`/`ST_ROOT`; 879 is an OFS data block). ADFlib tries 879, fails to find an id, and falls back to 880 — agreeing with our single formula. We warn on the mismatch and trust the formula.
- 10/10 ctest, 0 warnings under `-Werror` with `-Wconversion`.

### M7 — RDB and partitions ✅ COMPLETE (commit `2aff05e`)
- `rdb.c`: parses the RDB block region and discovers `PART` blocks with their `DriveName` and DosEnvVec (DOS type, geometry, reserved). A partition **starts at `low_cyl × cyl_blks`** and spans `(high_cyl-low_cyl+1) × cyl_blks`, where `cyl_blks = surfaces × sectors_per_track`.
- **Two facts about real RDB disks, learned by decoding the fixture rather than assuming:**
  1. `PART` blocks are found by **scanning** the RDB region, **not** by following `RDSK.next` — on the real A590 image `RDSK.next` is 512 while the six `PART` blocks live at blocks 1–6.
  2. `start_block = low_cyl × cyl_blks`, **not** a running sum (I assumed sequential packing first and got the wrong starts; the bootblock DOS-id check caught it immediately).
- **Gate: met — `m7_gate` PASS against amitools `rdbtool`** (authoritative here, since ADFlib's RDB support is experimental). All six partitions found with matching names and DOS types — OFS, OFS INTL, OFS DirCache, FFS, FFS INTL, FFS DirCache — and the decisive check: **every computed start block actually holds its declared DOS type on disk**. This one image covers all six supported DOS types end to end.
- **Two real defects the tests caught:** an unbounded block-header read in the scan (a heap over-read the sanitizer found), and the missing whole-block range check before `memcmp`.
- 11/11 ctest, 0 warnings under `-Werror` with `-Wconversion`.

### M8 — FUSE3 shell ✅ COMPLETE (commit `c08063c`)
- `translate.c` (in the core lib — pure logic, no FUSE dependency): owner permission bits 0–3 **active-low**, group/other active-high (C10); Amiga dates → UTC by default (D6); mounting-user uid/gid with `-o amiga_ids` (D7).
- `main.c`: FUSE3 **low-level** API (D1) — `lookup`/`getattr`/`opendir`/`readdir`/`open`/`read`/`readlink`/`statfs`. An inode is the Amiga header block, so a **hard link resolves to the same inode and reads the real file's data**; directory hard links answer `ENOTDIR` (C12); soft links emit a translated path via `readlink`. Read-only forced.
- **Gate: met — `m8_gate` PASS.** Every fixture mounts and **every file read through the mount is byte-identical to the `unadf` extraction** — **197 files across 10 fixtures**, including the 81-file Fred Fish disk and 66 `win32-names` entries. Hard links share the real entry's inode and read the real content; `stat` reports the true size (173847) and mtime (1991-12-11, matching the Amiga timestamp); every mount unmounts cleanly.
- **Two real bugs `translate.c` tests caught:** the Unix epoch offset was wrong by ~2 years (I had `315532800`; correct is `252460800`), and the type bits were masked out of `st_mode`. A `-Werror` use-after-free (reusing a freed block buffer while resolving a hard link) was also caught at build time.
- **Two API subtleties this FUSE 3.18 forced:** with the low-level API you do *not* pass `-f`/`-s`/`-o` to `fuse_session_new` (those are high-level options) — you mount manually and enforce `ro`/single-threaded yourself; and `readdir` has **no flags parameter** in this version, with the cookie a stable 1-based entry index.
- 12/12 ctest, 0 warnings under `-Werror` with `-Wconversion`.

### M9 — CLI and end-to-end harness ✅ COMPLETE (commit `9833cda`)
- `afs info|ls|cat|verify|bitmap` — a thin front-end over the tested core, so the CLI and the mount can never disagree about a disk's contents. `ls` marks entries (`d` dir, `f` file, `l` symlink, `h` multi-link, `D` dir-hard-link); `verify` exits 1 with a **named** reason on a bad disk; `bitmap` reports free/used blocks. Verified against the oracles (`afs cat /MOON.GIF` sha256 matches `unadf`).
- `tests/run_all_gates.sh`: one entry point — unit tests + every oracle gate, ending in `ALL GREEN`/`FAILED`. It runs only the unit-test ctest entries so it does not recurse into the `all_gates` test it is itself.
- CMake registers the seven gate scripts and `all_gates` as ctest tests: a plain `ctest` exercises unit + oracle + **real-mount** verification.
- `README.md`: build, mount, CLI, the supported matrix, the two oracles, and an honest known-limits section (read-only; dir-hard-links as `ENOTDIR`; exact-match lookup; dir-cache verify-only; `ffstd64` refused).

---

## M6 hardening — three verifier bugs found by adversarial testing (2026-10-13)

The verifier's own tests all built volumes whose rootblock hash table was
**empty**, and the one oracle fixture it was checked against (`ffdisk0049.adf`)
has only rootblock- and bitmap-level defects — both detected during *open*. So
a verifier that never looked below the rootblock passed every gate. Three
distinct defects, each found the same way: build a volume that violates a
documented check on purpose and see whether the check fires.

1. **The walk never left the rootblock.** `visit_entry` recursed only on
   `AFS_BLOCK_DIR`/`LINKDIR`, but the entry point is the rootblock, whose kind
   is `AFS_BLOCK_ROOT`. Every volume verified exactly one block; the checksum,
   type, range and double-use checks were dead code. Measured: **0 of 74**
   injected damages detected across four fixtures.
2. **RANGE and TYPE could never fire.** The verifier enumerated via
   `afs_dir_list`, which is right to drop an unreadable pointer and to skip an
   unrecognised block — and those two filters *are* the defects the verifier
   exists to report. Fixed by walking the hash table raw (`visit_dir_raw`). The
   read path stays defensive; the verifier deliberately does not.
3. **A block could be both bitmap storage and a file.** Bitmap pages were never
   entered into the double-use set, so the collision was structurally
   undetectable. Found via amitools, which rejects `g1a30c.adf` as
   "Bitmap Block Count Mismatch" where we reported one unrelated problem.
   Block 948 is bitmap page 2 *and* a file: writing either corrupts the other,
   and the bitmap is what decides whether a write is in bounds.

Also: `g1a30c.adf` was mis-filed as a clean fixture. It carries three defects.

**New in CI:** `m6_gate.sh` (previously M6 had *no* gate — `m6_verify` only
prints and always returns 0), `m6_damage_probe` (222/222 injected damages
detected), and `fuzz_gate.sh` (`fuzz_test.c` under ASan+UBSan, asserting it
*reached* the parser so a green run cannot mean "every image was refused").
Both new checks were confirmed load-bearing by reverting the fix and watching
them go red.

---

## M12 — CLI on RDB-partitioned images (2026-10-14)

The mount has understood partitions since M7/M10; the CLI did not — every
subcommand opened the whole image as one volume, so any RDB image failed with
`no DOS filesystem id on block 0 or block 1`. Fixed by resolving the **first
path component as the partition name when (and only when) the image carries an
RDB**, mirroring the mount namespace, so the two can never disagree about
where a file lives. No new syntax, no new flags: `afs ls disk.hdd "OFS INTL"`,
`afs cat disk.hdd OFS/Trashcan.info`. `info` alone prints the partition table
in `rdbtool`'s shape; `verify` checks every partition's own window and names
it; `bitmap` requires a partition on RDB (named refusal listing the names);
bare `ls` lists the partitions as the mount root does. Floppy behavior is
byte-identical: the no-RDB path is the untouched old code.

**Gate: `m12_cli_rdb.sh` PASS.** `info` table == `rdbtool` (6/6, shape-parsed —
DriveNames contain spaces); `ls -R` per partition == live mount (full relative
paths, not basenames, so same-named files in different dirs cannot collapse);
`cat` bytes == mount for all files **plus** `xdftool` for partition 0 (ends the
transitivity at a second independent oracle); `verify` clean on all 6;
`bitmap` used+free == RDB size per partition; refusals named (unknown
partition, bitmap-without-partition); floppy positive control (`ls` == `adfls`,
9/9 `cat` bytes == `unadf`) in the same harness.

**Load-bearing proof:** a mutant resolving every name to partition 0 is caught
in **four** independent places (5 DOS-type mismatches, 5 byte mismatches, wrong
bitmap counts, unknown-partition refusal never firing). Writing that proof
exposed that the `info <part>` check as first written accepted root=3078 for
five partitions — the size check alone cannot see a wrong-partition open — so
it now also asserts the DOS type the RDB advertises for that partition.

**Two gate bugs caught during the run:** the `D` (dir-hard-link) kind marker
is uppercase and the `[a-z]` strip class dropped those lines; and a whitespace
edit merged two lines and commented out the table assignment (`set -u` turned
it into `info_out: unbound variable` rather than a silent empty diff).

---

## M13 — Writes: create, write, mkdir (2026-10-14)

First half of D3's deferred scope (the allocator; FUSE stays read-only, and
there is deliberately no overwrite/truncate/rename/delete yet). New surface:
`blockdev_open_file_rw` + a real `blockdev_write` under the identical refuse-
never-clamp bounds rule; `afs_volume_open_window_rw` (the read-only opens are
byte-identical, and reads can never gain write permission by a flag flip);
`alloc.c` (`afs_alloc_block`: first free from block 2, per-page checksums,
INVALID→write→VALID flag dance); `afs_file_create` + `afs_file_write_all`
(reversed tables, per-block high_seq, OFS next_data chains, FFS raw payloads,
header flush with byte_size+mtime); `afs_dir_mkdir`; dir-cache insert/refresh
on DIRC volumes (in-place patch preserves comments byte-for-byte); `afs put` /
`afs mkdir` in the CLI reusing the M12 partition rule. Empty files are
header-only (ADFlib issue #30's shape: highSeq=0, firstData=0, all slots zero
— never a pointer at block 0). No transactions: linking is the last structural
step, so a failed create leaves allocated-but-unreferenced blocks, never a
half-linked entry.

**Gate: `m13_write.sh` PASS, oracle-diffed in BOTH directions.** Our files read
back by ADFlib readers (`adfls`/`unadf`, 3/3 identical incl. empty on
DOS\0+DOS\2+DOS\5); xdftool's files read by us (independent writer); `verify`
clean after writes; bitmap accounting exact; RDB write confined to its window
(other 5 partitions byte-identical); written files mount-readable; negatives
named (duplicate, missing parent, unknown partition). INTL folding pinned on
creates with the only discriminator that separates the variants (accented
bytes: ASCII folds everywhere on real AmigaDOS, so a case-only test would have
enshrined a false expectation — caught during the run).

**Load-bearing proof:** an always-partition-0 mutant is caught by the byte
diffs. The bitmap `used` count is asserted as a relationship (ours == oracle
+ reserved-block bits as read), not a literal, because both oracles scope
their totals two blocks smaller without saying which two.


- **12/12** unit tests, **7/7** oracle gates, **20/20** ctest, **0** warnings under `-Wall -Wextra -Wshadow -Wconversion -Werror`.
- Real Amiga disks mount read-only on Linux and read **byte-for-byte identically** to the ADFlib oracle (197 files through a live FUSE mount; 216 via the library).
- 20 commits, ~2,400 lines of C, two pinned oracles used only as subprocesses.

---

## 5. Testing strategy

- **Oracle 1:** ADFlib CLI at pinned commit `73880de7` (`adfls`, `adfinfo`, `unadf`, `adfbitmap`). Reference for listings, metadata, content and bitmap. **Compare listings and content only** — its filesystem-type *string* is `???` for `DOS\2`/`DOS\4` because `adfVolIsFsValid()` is a label/overwrite guard (`src/adf_dev.c:529`, `examples/adfformat.c:134`), not a mount rule. Record that display gap in the oracle script.
- **Oracle 2:** **amitools `0.8.1`** (`xdftool`, `rdbtool`). Preferred oracle wherever ADFlib labels a type `???` (i.e. `DOS\2`, `DOS\4`) and for **all RDB work**. Use it when the two oracles disagree. Record any disagreement; **never auto-accept** it.
- **Oracle 3:** unit golden vectors (checksums, hash and INTL fold, Latin-1, protection bits, dates, softlink paths).
- **Cross-check, not an oracle:** AROS `afs`, for checksum, hash and `capitalch` only (C5).
- **Rule:** every read path must match the oracle on every fixture before it counts as done. Where an oracle is known to be weak (ADFlib RDB is experimental upstream), a disagreement is a **recorded finding**, not a pass.
- **Fixtures:** pinned by sha256 in `fixtures.lock`, with measured DOS types. ADFlib's test data carries no separate licence: downloaded at test time, never committed or shipped. The `adfformat`-generated images are ours.

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
- **Directory-block GID width (FAQ inconsistency):** the FAQ lists the file header's `GID` as `ushort` at `BSIZE-194`. The directory block lists `UID` as `ushort` at `BSIZE-196` but `GID` as `ulong` at `BSIZE-194`, a 4-byte field overlapping the 2-byte one before it. This is a spec error, not an established fact. Until it is settled, `-o amiga_ids` (D7) exposes directory GID as the 16-bit value and records the question. Settle it by hex-dumping a directory block from a MultiUser-formatted image, cross-checked against ADFlib's `bDirBlock` and amitools' dir-block layout. It was scheduled for M2, and M2 closed without settling it. It is now a **blocker for shipping `-o amiga_ids` in M8**: until it is closed, `amiga_ids` is not offered.
- **Large volumes:** a 32-bit block count with `BSIZE` up to 32 KiB stays in range. Refuse `ffstd64` explicitly.
