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
there is deliberately no rename/delete yet -- overwrite/truncate landed in the
second half below). New surface:
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

**Overwrite/truncate (same milestone, second half).** `afs_file_write_all`
grew truncate+rewrite (fopen "w"): existing prefix blocks are reused, deficit
blocks are allocated before anything mutates (ENOSPC keeps old content),
tables are rebuilt with stale slots zeroed, surplus data+extension blocks are
freed last via new `afs_free_block`, and shrink-to-empty restores the exact
M13a header-only shape. Restarts live where block boundaries are visible (per
engine slice / per wrapper invocation in spirit: here, the caller defines the
block by calling once per whole new content). `afs put` truncates live files,
including through INTL-folded names (one entry keeps its spelling, content
replaced); put-over-directory still refused. xdftool cannot oracle this
direction (its `write` refuses existing files, like ADFlib "w"), so the gate
asserts round-trip through ADFlib readers + verify + bitmap accounting +
folding resolution instead -- stated, not hidden. Unit suite grew
grow/shrink/empty cases (80 checks).

---

## M17 — FUSE write-op fuzz lane, and two silent corruptions it found (2026-10-14)

The library had a differential fuzz (M16) and the shell had none: the FUSE write
ops were only ever driven by fixed POSIX scenarios. `tests/oracle/mount_write_probe.c`
is that lane — a random write workload through a real `-o rw` mount, checked
against an in-memory model. Every operation the mount ACCEPTS must be observable
afterwards, and every operation it REFUSES must leave the volume as it was. It
does not require the volume to be healthy, which matters because the lane
mounts deliberately mutated images.

Two passes, because the checks mean different things on different volumes:
**strict** (unmutated images — every errno must be one the design sanctions, and
`EIO` is *not* among them) and **lenient** (mutated images, where `EIO` is the
designed answer for "I met a structure I refuse to write through"). Both run the
differential check in full.

### Two silent corruptions, found and fixed

**1. Stale kernel dentries pointing at freed blocks.** A write landed on a block
that was no longer a file, and the shell refused it with `EIO`, having been handed
an inode that no longer named a file. The cause: the shell handed the kernel
3600-second dentry and attribute timeouts *even on a writable mount*. Nothing in
a read-only mount can change, so that promise is honest there; on a writable one
this shell frees blocks, so after an unlink the kernel is still holding an inode
— and **an inode number here IS a block number**, so it cannot be made safe by
expiring anything else the kernel cached. All timeouts now go through one
`cache_timeout()` helper: an hour when read-only, zero when writable.

**Honest caveat:** this is justified by that measurement and by principle, but I
could NOT reproduce the original failure independently — 200 deliberate
unlink-then-recreate cycles on a clean volume are clean, and a mutant that
restores the 3600-second timeouts does **not** turn the gate red. So the lane
does not currently cover it, and this record should not be read as claiming it
does.

**2. Allocating from a bitmap that disagrees with the volume — the serious one.**
`ffdisk0049.adf` has `bm_flag = 1`: AmigaDOS saying its allocation map is suspect
and must be rebuilt by walking the tree, which this library does not do. Its map
marks block 1089 free; block 1089 is the header of `Plot/save.c`. Three `create`
calls later, that block was the header of a brand-new file too. The volume still
*verified* — every pointer resolves, two entries merely claim the same block —
and the corruption only surfaced as a subsequent write failing. `g1a30c.adf` has
the same shape via a different route: two bitmap pages for a volume that needs
one, and the surplus page is simultaneously a live directory entry.

Both are now **refused with a name** in `read_bitmap_root()`, beside the existing
bitmap-extension refusal and for the same reason — refuse rather than corrupt:

- `bm_flag != -1` → "the volume needs its allocation map rebuilt";
- page count ≠ `afs_bitmap_pages_required()` → "the allocation map cannot be
  trusted".

The page-count formula was duplicated in the verifier and has been hoisted into
`bitmap.c` so the writer and the checker cannot drift.

### A check the verifier claimed and did not have

`verify.h`'s header has always listed "used-vs-bitmap agreement" among the
invariants. **No such per-block check existed.** The newly-added one is behind
`--strict`, and off by default for a measured reason: **nine of the eleven
fixtures have bitmaps that disagree with their own contents** while being
otherwise perfectly healthy (`links.adf` marks the live header of `file1` free).
Stale maps are ordinary on real Amiga disks — that is what `bm_flag` is *for* —
so reporting it as damage by default would drown the real findings. It is
`afs verify --strict` now, and the reason it is worth having is exactly finding 2.

### Gate-hygiene findings, all the same shape

A green run not testing what it claimed, five times over:

- **`resolve_dir` walked the wrong path** — the directory's *parent*, so a
  one-component path resolved to the root. The first failure read as a library
  bug (a rename moved a file out of a subdirectory); the volume was right.
- **`expect_file`'s buffer was `MAX_DATA+16`** while appends grow files past
  that, so it reported "read 2016 of 2297" against a perfectly correct file.
- **`dir_empty` never recognised root-level children** (a file directly in the
  scratch root has a path with no leading slash), so the scratch root looked
  permanently empty — and the probe deleted its own working directory, after
  which every operation failed `ENOENT`: two seeds reported 161 refusals and no
  coverage.
- **The model was filled *after* creating.** With the model full, a create left a
  real, untracked file in the volume while the model believed the directory was
  empty; the next `rmdir` was then refused, and reported as an unexplained EIO.
- **A refusal was assumed to mean "nothing changed".** It does not: the errno
  says what a refusal implies (`ENOTEMPTY` → still there, `ENOENT` → gone,
  anything else → unknown, adopt reality). Guessing produced "model dir vanished"
  for an rmdir correctly refused with `ENOENT`.

Also: `create` answered **EIO for "volume full"** — only the `write` op knew about
`ENOSPC`, because seven write ops had each grown their own ad-hoc `strstr` chain
and they had drifted. All seven now share one `err_to_errno()`, and the chain is
gone.

### Load-bearing

Reverting the `bm_flag` refusal, the page-count refusal, and the shared
`err_to_errno` each turn the lane red. The gate asserts absolute coverage floors
(mounted, ops, accepted, per-class lines) so a short or empty run cannot pass,
and a volume that refuses writes on purpose is counted as `nowrite`, not failed.

---

## M16 — Write-path differential fuzz (2026-10-14)

The write path (M13a/b, M14, M15) is pointer surgery — hash-chain splicing,
dir-cache record compaction with `memmove`, OFS header fixups, table rebuilds
— and every gate up to here drove a handful of FIXED scenarios. A bug needing
"an extension block AND a chain relink AND a cache record in one operation" is
invisible to a fixed script and corrupts a volume silently: the file reads back
with the wrong bytes, or the volume stops verifying, and nothing complains
until an unrelated tool opens it.

`tests/write_fuzz.c` + `tests/oracle/m16_write_fuzz.sh`: random operation
sequences against an in-memory model of what the volume should contain, built
and run under **ASan + UBSan + LeakSanitizer**. After **every** operation that
returned success:

1. `afs_verify_memory()` reports **zero** problems — checksums, pointer ranges,
   block types, double-use, bitmap agreement, dir-cache vs hash table. A leaked
   block, an eaten live block, a broken chain and a stale cache record all fail
   here.
2. A recursive tree walk yields **exactly** the model's set of `(path, kind)` —
   no orphans, no duplicates, no missing entries.
3. Every model's file reads back byte-identical.

The model is only updated when the library reports success, so a refusal leaves
it (and the volume) untouched — which is itself the assertion: a write that
half-applies before failing shows up as a mismatch.

**It found a real leak on the first run.** `afs_file_write_all` transferred
ownership of the final block list to `dblks` only *after* every allocation
succeeded, so the two `goto fail_inner`s from the allocation loops leaked the
whole list. Invisible until a leak-checked build ran a write that ran out of
room — an ENOSPC path no fixed gate exercised, and the one M13b's comment
claimed "fails clean". Fixed by freeing `final_data` at `fail_inner` (it is
NULL once ownership has moved). Reverting the fix turns the gate red with 120 KB
in 17 allocations named at `file.c:519`.

Three harness findings, all of the same shape — a green run that was not
testing what it claimed:

- **`resolve_dir` walked the wrong path.** It resolved the block of a
  directory's *parent*, so `resolve_dir("rty")` answered "the root". The first
  failure read as a library bug (a rename moved a file out of a subdirectory);
  the volume was right and the model was wrong.
- **Refusals were never exercised.** Every op picked a valid target, so
  `refused` was 0 across an entire run — a gate with zero refusals has not
  tested a single refusal path. Added a create aimed at an existing name (the
  duplicate check) and an overflow write larger than the volume (the ENOSPC
  discipline), plus a floor that fails the run when nothing was ever refused.
- **"Corrupt" mode was not corrupt.** 24 random byte flips over a 900 KiB image
  almost never land on structure (~4% of the bytes), so every corrupt-mode run
  behaved identically to a clean one — zero refusals, zero interesting paths.
  Damage is now aimed: a random *block*, then a random byte inside that block's
  header or tail region.

One sizing note: the first version allowed 8192-byte files and saturated the
1756-block floppy within ~100 operations, after which every op needing even one
block was refused — seed 3 "passed" with `trunc` never succeeding at all. Capped
at 3000 bytes; out-of-space is now covered deliberately by `op_overflow` rather
than by starving every operation.

**Gate: `m16_write_fuzz.sh` PASS** — 6 clean seeds x 300 iterations, 3 corrupt
seeds x 200, plus a determinism check (the same seed twice must produce the same
verdict and the same tallies, or a failure could not be reproduced). Every op
class succeeds at least once per run except `overflow`, which is *supposed* to
be refused; the floor asserts exactly that rather than asserting the opposite of
the design.

Also added, deterministic and sanitizer-free: `test_enospc_leaves_content_intact`
builds a volume with three free blocks and asserts a too-large write is refused
with every byte of the previous content intact. Writing it surfaced a genuine
trap, now documented at the builder: `afs_bitmap_set`/`afs_bitmap_clear` index
an **assembled** map (page data areas concatenated, each page's checksum long
stripped — what `afs_bitmap_read` produces), NOT a raw storage block. Passing a
raw page parks blocks 0..31 inside that page's own checksum long, which the
allocator never reads; the first version of this test "proved" a volume full of
free blocks was full.

---

## M15 — Partial-range writes + FUSE write ops (2026-10-14)

Two pieces. The library gained `afs_file_write_range` (write `len` bytes at
byte offset `off`), and the FUSE shell gained `-o rw`: `create`, `write`,
`setattr` (size only), `mkdir`, `unlink`, `rmdir`, `rename`, `flush`,
`release`. Default mount is still read-only — every op refuses with EROFS
*first*, before resolving a name, so a read-only mount cannot be half-mutated
by a path that fails late.

`afs_file_write_range` is NOT "read the file, splice, write it all back". Only
the blocks the span covers are rewritten, plus the blocks whose OFS header must
change: a newly allocated block needs one, and when a write grows the file past
a block, that block's `data_size` and `next` pointer are stale even though none
of its bytes changed. Tables are left alone unless the block COUNT changed —
which is the point: a 4 KiB patch into a 1 MiB file moves no block and rewrites
no table. Past-EOF gaps read back as zeros (POSIX hole semantics), with a gap
block's OFS `data_size` set to the full payload so a reader takes all of it as
file data. Allocation order matches write_all: deficits allocated before any
mutation, so a full volume fails with the file untouched. `finish_write()`
(header stamp + dir-cache refresh) is shared by both writers so they cannot
disagree about what a finished write means.

Three FUSE details cost a debugging round each, all worth recording:

1. **`create` must reply with `fuse_reply_create`, not `fuse_reply_entry`.**
   `fuse_reply_entry` leaves the request half-answered and libfuse's first
   write afterwards fails with `fuse: writing device: Invalid argument` — an
   error from the *reply writer*, with nothing pointing at the cause.
2. **`setattr` must reply with `fuse_reply_attr`, not `fuse_reply_err(0)`.**
   Same shape of failure, same misleading message. A truncate that fully
   succeeded appeared to be refused by the filesystem.
3. **O_APPEND is ours to implement.** `fuse_lowlevel.h` states that with
   writeback caching disabled the filesystem must ensure each write appends.
   We clear `FUSE_CAP_WRITEBACK_CACHE` in init, so `fi->fh` carries the
   O_APPEND bit and `write` redirects to the current end of file.

**Gate: `m15_fuse_write.sh` PASS (16 checks).** POSIX flows through the KERNEL
(`cp`, `>>`, `O_TRUNC`, `truncate -s`, `mkdir`, `rm`, `rmdir`, `mv`), so dentry
translation and flag handling actually run — a library-only test cannot see
them. Bytes oracle-diffed with `unadf` after unmount; `verify` clean. A default
mount refuses all four write ops with **EROFS specifically**, not merely "it
failed", and the whole image is sha256-identical afterwards. `rename` across
RDB partitions returns **EXDEV**, asserted through `os.rename` directly because
GNU `mv(1)` catches EXDEV and falls back to copy+delete — a shell-level refusal
test would fail on correct code (the fallback path is asserted by end state
instead). `rename(a, a)` succeeds as a POSIX no-op, also asserted through
`os.rename` for the same reason (`mv` short-circuits it itself).

**Mutant findings.** Blinding the `g_opt.rw` switch left the gate GREEN at
first: the ro cases still failed, just with EIO instead of EROFS, because the
write got through the switch and died on a read-only blockdev. "Did it fail?"
was the wrong assertion; the gate now checks the reason. Blinding the range
splice's in-block offset was caught by the unit suite but NOT by the gate,
because every gate case wrote whole files from offset 0 — the exact case that
does not exercise splice arithmetic. Added section 2b: a `pwrite` at offset
1234 into a 5000-byte file with the expected image computed independently, so
exactly 15 bytes must change. Both mutants now red.

Also fixed: `m12_cli_rdb.sh` used `adfls`/`unadf` without putting ADFlib on
PATH (every other oracle gate does), so it passed only under a shell with the
right exports and FAILED under ctest — a gate that fails for want of an
environment variable reads exactly like a real regression. Fixed, and it now
skips cleanly when an oracle is absent rather than reporting a false failure.
Full ctest: 31/31.

---

## M14 — Delete + rename (2026-10-14)

`afs rm` (files + empty dirs) and `afs mv` (same-volume rename/move),
semantics mirrored from ADFlib's own `adfRemoveEntry`/`adfRenameEntry` source
rather than memory: non-empty rmdir refused, file delete frees header + data
+ extension blocks, empty-dir delete frees the dir plus its whole cache chain
(ADFlib frees only the first -- ours frees all, and bitmap accounting proves
it), rename rewrites name+parent in place with unlink/relink + duplicate
check + cache del/add, same-name same-dir is a no-op, links refused like
ADFlib refuses them, and a directory moved into its own subtree is refused
(the walk-up, not just equality). ENOSPC/order discipline as M13b: unlink
first so failures leak storage but never half-link.

**Gate: `m14_delete.sh` PASS (21 checks).** Bitmap deltas exact per variant
(OFS file -213 = 1+210+2, FFS -203 = 1+200+2, DIRC rmdir -6 = 2 files + dir +
cache); moved bytes == `unadf`; xdftool's `delete` visible to us (it has no
rename -- stated); RDB round-trip leaves all 6 partitions identical; deletions
mount-visible. Refusals all named (non-empty, occupied victim with victim
intact, self/subtree cycles, cross-partition, links).

**Two gate-hygiene findings from the mutant runs.** (1) The always-partition-0
mutant style of proof was repeated for both refusal paths: blinded duplicate
check and blinded cycle check each turn their gate case red -- except the
subtree case PASSED under its mutant, which meant the case was broken, not the
code: the self-move case runs first and, once it succeeds under the mutant,
unlinks D1 from the root, so the subtree case cannot resolve its paths and
passes vacuously on a corrupt volume. Destructive cases now get FRESH copies
each ("shared state between destructive cases is how a mutant hides"), and
both fire. (2) `verify` reports a cyclic-orphan volume clean -- unreachable
blocks are a collection problem, not a verification problem; recorded, not
fixed.


- **12/12** unit tests, **7/7** oracle gates, **20/20** ctest, **0** warnings under `-Wall -Wextra -Wshadow -Wconversion -Werror`.
- Real Amiga disks mount read-only on Linux and read **byte-for-byte identically** to the ADFlib oracle (197 files through a live FUSE mount; 216 via the library).
- 20 commits, ~2,400 lines of C, two pinned oracles used only as subprocesses.

---

## M24 — the oracle's real limits found by a false regression report (2026-10-15)

Hard-link writing was implemented, then reverted. Not because it was wrong — because
the test that condemned it was measuring the oracle, not the code.

**What happened.** After `afs link` created a hard link on `testffs.adf`, `unadf`
extracted 3 of 9 files where it had extracted 9 of 9 before. That was reported as the
oracle catching a regression. Four wrong turns followed, each with a plausible
mechanism, before the truth:

1. **Blamed the `next_link` chain append.** Exonerated — with the append, `unadf`
   extracts 9/9 byte-identical. (The chain shape was independently confirmed against
   `links.adf`: `FILE.next_link → LINKFILE → 0`, exactly what the code wrote.)
2. **Blamed the stale bitmap** on `testffs.adf`. Exonerated — the blocks it handed
   out were genuinely free per an authoritative rebuild.
3. **Blamed `unadf` being nondeterministic.** Exonerated — five runs on the pristine
   fixture all gave 9.
4. **Concluded "unadf drops hard links."** Also wrong. On the pristine, untouched
   `links.adf`, `unadf` extracts both real files **correctly**, resolving each link to
   its target.

**The truth.** `unadf` emits a per-entry `No such file or directory` for whatever it
cannot materialise — links (it cannot give a link its own name) *and some ordinary
files it walks oddly* — then continues, extracting every real file with correct bytes,
and **exits 0 regardless**. So its stderr is not a damage signal and its exit code is
not a link signal. The original "9 → 3" was a miscount: the link's target was being
counted as extracted-after when it never was.

**The transferable lesson, which is why this is a gate and not a note.** An oracle
that silently skips a construct is worse than no oracle, because its output still looks
like evidence. A gate asserting "unadf agrees" would have passed on the broken volume
and failed on the correct one — the assertion was inverted by the oracle's blindness.

**Gate: `m24_unadf_link_blindness.sh` (21st gate).** Pins the oracle's *actual*
contract on link-bearing and link-free fixtures: every file `unadf` extracts is
byte-identical to what our reader returns at the same path; no real file goes missing
(count vs our own listing); our reader verifies each fixture clean. Load-bearing is
**proven**, not asserted: a 1-byte flip in an extracted file is detected. An earlier
load-bearing attempt that blanked the links' `nameLength` was discarded — a zero-length
name stops the directory hash-chain walk dead, so it measured its own corruption rather
than link removal.

**Status.** Link *writing* was **implemented in the next step (M25) and shipped.** See
below.

## M25 — hard-link writing: shipped, and two defects only a live mount could find (2026-10-15)

M24 left the oracle question settled and the implementation reverted. This step landed
it, TDD-first, with the on-disk shape read from `links.adf` block 891 (a real
AmigaDOS link) rather than from memory:

| field | value | source |
|---|---|---|
| primary / secType | `T_HEADER` / `ST_LINKFILE` (−4) | block 891 |
| `real_entry` (0x1d4) | target's header block | block 891 |
| `byte_size` / extension / data blocks | 0 / none / none | block 891 |
| `next_link` chain | hung off the **target**, not the link | both of links.adf's links |

**Library** (`afs_file_link`): FFS-only (OFS refused by name — no link secondary type
exists there), refuses a duplicate name, an invalid name, an out-of-range target and any
non-file target, each with a named reason. **CLI** `afs link <image> <path> <newname>`.
**Mount**: the `.link` op for `ln`, and `unlink` of a hard link via `afs_dir_unlink`.

**Two design points worth recording.** `chain_append` runs *before* the directory
entry is filed, so a chain failure leaves an unreachable block rather than publishing a
name whose nlink is wrong. And `afs_dir_unlink` now calls `afs_link_after_unlink`,
because removing the name without unhooking the chain would leave it counting towards
nlink forever.

**Three defects, each found by a test that could actually reach it:**

1. **The chain splice truncated instead of splicing** — `chain_unhook` wrote `0` into
   the predecessor's `next_link`, so with the chain `real → one → two`, unlinking `one`
   dropped `two` as well and nlink fell to 1. Caught by the unit test asserting the
   *second* link still resolves. The fix reads the removed link's own successor and
   writes that in its place.
2. **`ln` returned EIO even though the link was correctly written.** The `.link` op
   ended in `fuse_reply_err(req, 0)`, but a `.link` op accepts **only**
   `fuse_reply_entry` or `fuse_reply_err` (`fuse_lowlevel.h`) — a bare success reply
   makes libfuse fail the write with `writing device: Invalid argument`. The tell was
   that `afs ls` on the same image already showed the new link: *the operation
   happened and the caller was told it failed* is the shape of a **reply** bug, not a
   write bug. Fixed by replying with the new entry, as `mkdir` does.
3. **`fill_stat` hardcoded `st_nlink = 1`.** The kernel uses `st_nlink` to decide
   whether an unlink may drop an inode's data, so under-reporting risks freeing a file
   that still has names pointing at it. Now takes the real count from the new
   `afs_nlink_for_block` — needed because the stat path resolves an inode to a *block*
   while `afs_file_nlink` needs a *name*, so the count lives in one shared helper
   (`nlink_from_real`) rather than twice.

**Gates: `m25_link_write.sh` and `m25_fuse_link.sh` (22nd, 23rd).** The first asserts
M24's byte-identity contract plus the things the oracle cannot see (the link exists,
shares the target's bytes, survives a bitmap rebuild, frees only its own block).
The second drives the **kernel's own `link()`** through a live mount — the only way to
reach the `.link` op and to check `st_nlink` — and asserts the daemon logged no failed
FUSE reply. Load-bearing proven by mutants: `real_entry → 0`, `secType → ST_FILE`, no
chain append, `rm` refusing links, the bare-success-reply regression, and
`st_nlink → 1`. Each turns a specific assertion red.

`test_nlink_for_block` also caught that the synthetic test volume in `test_links.c`
had **no bitmap at all** — every block read as USED, so any allocating call reported
"volume full". The read-only tests never noticed; the first allocating test did.


## M26 — the rootblock's own dir-cache chain was freed by every rebuild (2026-10-15)

Found by asking an inherited claim a question, not by a failing gate. The plan
carried "extension chains are not implemented — needed only above ~50 MB". Measuring
instead of trusting: **600 KB files round-trip correctly**, so extension chains are
implemented and fine. Pushing further found a hard wall at ~312 KB with

```
afs: cannot write /big320: dir-cache chain corrupt
```

which is wrong on both counts — 1132 blocks were free (a 320 KB file needs ~640), and
the message names a subsystem the user never touched.

**The bug.** `rb_walk` called `rb_mark_dircache` only for entries of kind
`AFS_BLOCK_DIR`. The directory it was *handed* — the rootblock, the one call at the
top of `afs_bitmap_rebuild` — never had its own chain marked, and nothing else marks
it either. Every **root-level** dir-cache block therefore came out of every rebuild
marked **FREE**, and the allocator later handed one out as **file data**.

Isolated exactly: on `testhd.adf`, the 4th consecutive 256 KB write destroys block
1761 (the root's cache block), which then contains `'e'` — the fill byte of the 4th
file. Block 1761 sits inside that file's data range `[1563..2074]`.

**Why nothing caught it.** The damage is silent and *delayed*. The rebuild succeeds,
`verify` says the volume is clean, every read is correct, and the volume works
perfectly — right up until a write big enough to need extension blocks lands on the
freed block. It is a data-loss bug wearing a data-integrity bug's clothes.

**The fix** is four lines in `rb_walk`, marking the directory's own chain before its
entries — which is harmless for every subdirectory (its parent already marked it) and
correct for the rootblock, the one directory nobody marks.

**Also found and fixed in the same trace:** my own probe read the cache block with
little-endian arithmetic and reported `checksum_ok=0` and a garbage chain, which
briefly looked like corruption of the *fixture*. It was the probe. `be32` is not
optional; the same mistake as M21's `headerKey`-at-`+4`, in a new place.

**Gates.** Unit: `test_rebuild_keeps_the_rootblocks_dircache` (RED for exactly the
right reason — block 10 comes out free — with the already-working *subdirectory* case
kept alongside so the fix cannot regress it). Oracle: `m26_root_dircache_survives.sh`
(24th) reproduces the four-write sequence and asserts the writes SUCCEED, the chain's
block sequence is unchanged, every chain block is still valid, and any refusal is the
truthful one. Load-bearing proven: reverting the fix turns write #4 red with the
original misleading message.

Two of my own gate assertions were wrong first and were corrected rather than
adjusted to match the code: the chain's record count is *expected* to grow with each
new file (asserting it unchanged asserted the bug's symptom), and reusing one temp
file for four content comparisons compared three files against the fourth's bytes.


## M27 — the checker for M26 could not see M26 (2026-10-15)

M26 fixed the rebuild. Before calling it closed, I asked the question that gate
design keeps asking: **can anything actually detect this?**

**The check that should have caught it does not.** `verify --strict` exists for
exactly this invariant — "every block the tree references must be marked USED in the
allocation map" — and its implementation iterates the tree walk's `vc.marks[]`. But
`verify_dircache` walked each directory's cache chain, checked its shape, and **never
recorded the blocks it visited**. So dir-cache blocks were absent from `marks[]`, and
the one check whose job is to catch M26 was blind to it. A volume with the root's
cache block freed verified **clean in both modes**.

That is a checker defect and a rebuild defect agreeing with each other: both were wrong
in the same direction, so neither could contradict the other. The invariant was real,
enforced nowhere.

**Fix:** `verify_dircache` now marks each cache block into `vc.marks[]`. Two lines,
with `marks` threaded through.

**A third site of the same shape, found while fixing it.** I first added
`vc.marks[root] = 1` before the walk, reasoning that the rootblock must be
special-cased — the theory that had been right twice already. It was wrong:
`visit_entry` marks the block it is *handed*, so the rootblock was already marked, and
my line made the double-use check fire on the rootblock itself. Six existing tests went
red immediately. **Two sites of a bug is evidence for a third, not a licence to invent
one** — the walk already did the right thing, and the tests said so.

**Also fixed: the test helper that hid this.** `bm_set()` in `test_verify.c` writes at
byte 0 of a bitmap page — which is the page's own **checksum long**, not map data.
Every test using it therefore never actually exercised bitmap agreement, and the
"clean volume" in my new test passed for the wrong reason. The new test writes at `+4`,
where the real reader looks, and says why in a comment.

**Proof it works.** Deliberately freeing the root's cache block — M26's exact broken
state — now makes `verify --strict` report *`block 1761 is referenced by the tree but
the bitmap marks it free`*, while the good volume stays clean. `m26` asserts both, so a
checker that cannot detect the defect it exists for fails its own gate.

Load-bearing proven: removing the marking turns the new unit test red. A no-op check
that reports green forever is the failure mode this closes.


## M28 — the write fuzzer claimed a check it did not run, and its volume's map was broken (2026-10-15)

M27 asked whether anything could detect M26 and fixed the answer for `verify`. The next
question is the same one aimed at the **harnesses**: does the write fuzzer -- the thing
whose entire purpose is silent write corruption -- actually assert what it says?

**Its header claimed:** *"afs_verify_memory() reports ZERO problems. This is the strong
one: it checks checksums, pointer ranges, block types, double-use, **bitmap
agreement** and dir-cache/hash-table agreement."*

**It called `afs_verify_memory()`**, which is `afs_verify_memory_ex(..., 0)`. And bitmap
agreement is the ONE and ONLY thing `AFS_VERIFY_STRICT` adds (`verify.c:380`). So the
claim named the single check it was not running.

Measured, not argued: a volume whose live file header was marked free reported **0
problems** through the everyday call and **1** through strict --
`block 2 is referenced by the tree but the bitmap marks it free`. That is M26's exact
shape, invisible to the harness built to find it.

**The second defect was worse, and enabling the check is what exposed it.** The harness
built its volume's bitmap page by calling `afs_bitmap_set()` — which addresses an
**assembled** map at offset 0 — on a **raw page**, whose map data starts at `+4`
because bytes 0..3 are the page's own checksum long. Every bit it meant to set went into
the checksum word. **The fuzzer's volume has carried a broken map since it was written**,
so the allocator state it exercised was not the map any reader would see. With strict
on, the harness reported blocks 880 and 881 -- the rootblock and the bitmap page -- as
live-but-free, which is what pointed at the offset.

Note the two bugs are of a kind now seen three times: **a bitmap bit addressed in the
wrong coordinate space.** `verify.c`'s page reader, M27's test helper, and now the fuzz
fixture. Worth its own note in the testing strategy below.

**Honest scope.** Turning strict on did **not** reveal new library bugs: the operation
tallies are byte-identical to the broken-map run (seed 1: create 22/55, overflow 0/17),
because the allocator read the same wrong offset and was self-consistent. The harness was
still measuring meaningless allocator state, but it was measuring it *consistently*. What
was wrong was the claim, and the map.

**Gate: `m28_strict_sees_bitmap_mismatch.sh` (25th).** Pins the **capability** rather
than the fix's presence: builds the defect and requires strict to see it; compiles and
runs the harness and requires its volume to survive its own strict check; asserts the
call across line breaks.

**This gate's first version was itself the bug it was written to catch.** It asserted with
`grep "AFS_VERIFY_STRICT" tests/write_fuzz.c` — and the file's own **comment** names that
token, so the grep matched whether or not the call existed. **Both mutants passed it.**
It now matches the call across line breaks, and *that* check was added only after
establishing that running the harness cannot see mutant A either: the harness's map is
correct either way, so running it proves nothing about *which* check it calls.

**Load-bearing proven per assertion:** mutant A (revert to the everyday call) and mutant
B (page offset 0) each turn a specific named check red; good code passes both.


## M29 — the fourth instance of the bitmap coordinate-space bug, in the suite that gates the allocator (2026-10-15)

M28 named the bug kind — *a bitmap bit addressed in the wrong coordinate space* — after
finding three instances. So the next step was mechanical: sweep every remaining site.

**Found in `tests/unit/test_alloc.c`, at `build_writable_img()`.** That function is the
base image for **every** allocation test in the file. It called `afs_bitmap_set()` — which
addresses an assembled map at offset 0 — on a raw page whose map data starts at `+4`.

Measured before fixing: `verify --strict` reports **2 problems** on that volume —

```
block 880 is referenced by the tree but the bitmap marks it free   (the rootblock)
block 881 is referenced by the tree but the bitmap marks it free   (the bitmap page)
```

— and blocks 0, 1, 880, 881, the four the function's own comment says are "marked used",
all read **free**. Blocks 0..31 were living inside the page's own checksum long and every
later block sat four bytes early.

**Why 164 checks never noticed.** The allocator reads the *same* wrong region, so
allocation succeeded and every test in the file agreed with every other about map state
that no reader would ever see. The suite was self-consistently meaningless and reported
green. That is the recurring shape of this whole bug class, and it is why **running the
suite proves nothing** — only asking the invariant does.

**The same file already knew.** `build_writable_img_free()`, fifteen lines below, carries
a nine-line comment describing this exact gotcha — *"the first version of this test got it
wrong and 'proved' a volume full of free blocks was full"* — and gets it right, building
the map separately and copying it in at `+4`. The knowledge existed in the file and had
simply not reached the function above it.

**Fixed by applying that sibling's own correction**, and by adding
`test_the_base_volume_has_a_readable_map`, which asserts what every other test here
assumes: strict verification reports zero problems, and blocks {0, 1, 880, 881} read used
while {2, 100, last} read free. RED showed all five wrong; GREEN is 171/171.

**Swept clean after the fix.** Every remaining site uses `+4`: `test_bitmap_rebuild.c`,
`test_verify.c`, `test_links.c`, `write_fuzz.c`, `test_alloc.c`'s other helper. The shell
gates never hand-craft a page at all — they drive `afs bitmap-rebuild`, which is the
library's own path.

**Gate: `m29_no_raw_page_bitmap_writes.sh` (26th).** A static scan for the idiom
`<name> = img + <block> * 512;` followed by `afs_bitmap_set(<name>, …)`, plus a
behavioural half that builds both the correct and the historic-raw form and requires the
latter to be **detected** — so the gate proves its own detector works.

**That scan took three attempts, and each failure was informative.** Version one matched
the token `afs_bitmap_set(bm` per file and excused any file declaring a `map` buffer —
over- and under-matching at once. Version two anchored a regex at the assignment, which
cannot cross the pointer star in `uint8_t *bm = …`. Version three tried to place the star
before the name, which it is not. The working form matches the name as a **substring**
with awk's `index()`. It is less elegant than what it replaced and it is the only one that
finds the thing — verified by planting an instance in `test_dir.c` and watching two earlier
versions pass straight over it.


## M34 — second-large-write corruption: root cause found, fix awaiting a decision (2026-10-15)

> **RESOLVED 2026-10-16 — shipped in `a3db7283`.** The decision below (refuse, with
> the remedy named) is what shipped. The guard is
> `afs_verify_map_disagrees()` wired into `afs_alloc_block`: the map must agree
> with the **tree**, not merely with itself, and it walks via `visit_entry`
> rather than copying the volume, because it sits in the allocation path.
>
> **Why it took fourteen parts, and which of the sections below are wrong.** One
> gate (`mount_fuzz`) stayed red for the entire time, and it was red because of a
> bug in `mount_write_probe`, not because of anything the guard does. The
> sections are in the order they were written and several are *refuted* by later
> ones — kept because the corrections are the record:
>
> - "What mount_fuzz actually reported" — the first reading of the failure. The
>   errno hypothesis was wrong.
> - "Root cause found: … over-commits blocks on a nearly-full volume" — **WRONG.**
>   Corrected immediately below it; `dd`'s default `bs=512` split one append
>   into sixteen writes, so "7168 bytes committed" was a fact about `dd`.
> - "Down to the bytes: the file's recorded geometry does not match the write" —
>   **WRONG.** The geometry checker written to test it found the file internally
>   consistent, and the hypothesis with it.
> - "A new finding: `bitmap-rebuild` leaves live blocks marked free on g1a30c" —
>   **WRONG.** The rebuild had *refused*; its output had been sent to
>   `/dev/null` and assumed to have succeeded.
> - The sections that survive: the gate had **no seed** (so nothing in it could
>   be reproduced), and **`bitmap-rebuild` refused on `g1a30c` naming a remedy
>   that no command could perform** — which made that volume permanently
>   unwritable, since a stale map is exactly what refuses writes. That one is
>   fixed and pinned (`d455d564`, `d634fc39`).
>
> **The actual blocker, for anyone reading the failed sections:** the probe's
> `do_create` reserved its model slot with a blind `alloc_ent()` and then
> generated a name that could collide with a live one, leaving **two model slots
> tracking one path**. `random_file()` picks among slots, so an op acted on the
> stale one at a length the mount never had, while the mount correctly appended
> at the real end. Every byte the mount wrote was right; the probe's offset was
> wrong. Two compounding lessons are recorded in the wiki — a fix can appear to
> work because it perturbed the RNG, and an invariant that nothing enforces is an
> assumption.
**Status: DIAGNOSED, NOT SHIPPED.** Both candidate fixes were implemented and
measured; the tree is reverted to the M33 baseline (30/30 gates, 45/45 ctest)
pending a decision recorded below. The diagnosis is the durable part.

### The symptom

On `testffs.adf`, one of the project's own fixtures — healthy, and accepted for
writing:

| | result |
|---|---|
| 1 × 300 KB write | `OK, no structural problems found` |
| 2 × 300 KB writes | **20 problems** |
| 1 × 50 KB write | `OK` |

Blocks 1222 (`hlink_blue`) and 883 (`dir_2`) came back with invalid checksums
and unknown types, and **both entries had vanished from the listing**. A third
write then failed with `dir-cache chain corrupt` — because the dir-cache block
it needed, 1220, had been overwritten.

### Root cause

**The fixture's allocation map marks exactly three blocks allocated** — the
rootblock (880), one dir-cache block (881), and the map page itself (882). Its
tree references dozens more. Live blocks **1220, 1222 and 883 are all marked
FREE**.

The pattern is arithmetic, not mysterious:

| write | blocks taken | collides? |
|---|---|---|
| 1st (600 blocks) | 2 – 601 | no — every live block sits above 880 |
| 2nd (600 blocks) | 602 – 1201 | **yes: 883, 885, 886, 1142, 1148, 1160, 1161, 1193** |

So the allocator, correctly following the map, hands out blocks that are live.

**Why the existing guard missed it.** `check_bitmap_root` has two conditions:
`bm_flag == -1` (AmigaDOS's own "this map is valid" claim) and a page count
suits the volume. Both are about the map's **self-consistency**; neither says
the map **agrees with the data**. `testffs.adf` passes both. `bm_flag == -1` is
AmigaDOS's claim, and on this image it plainly does not mean what it appears to.

### Both fixes implemented and measured

- **Repair at write-open** (`afs_bitmap_rebuild` on every rw open). Fixes it
  completely — verify *and* `--strict` clean, all 16 original entries readable —
  but silently mutates the map. Breaks **4 gates whose subject is the map**
  (`m18` can no longer test a rebuild that already happened; `m33`'s
  "orphan count unchanged" is wrong because 22 orphans were correctly
  reclaimed; `m14`'s bitmap accounting; `mount_fuzz`).
- **Refuse with a name and the remedy** (a third guard calling the verifier's
  walk). Clean, honest, consistent with the two refusals already there — and
  **breaks 7 gates and 164 unit checks**, because nearly every fixture has a
  stale map. In the field it means the tool refuses to write on most real
  floppies unless the user knows to run `afs bitmap-rebuild` first.

### The decision

**Chosen: refuse, with the remedy named.** Consistent with the two refusals
already in the allocator, and it never silently mutates a user's disk.

**NOT shipped, because the remedy is itself broken.** Naming a command that
fails is worse than not naming it, and `afs bitmap-rebuild` fails on the OFS
fixtures:

```
$ afs bitmap-rebuild blank.adf --yes
afs: directory block 880 has a corrupt dircache chain

$ afs bitmap-rebuild testofs.adf --yes
afs: directory block 880 has a corrupt dircache chain

$ afs bitmap-rebuild testffs.adf --yes      # DOS\5, has a dir-cache
...: 1760 blocks, 343 used, 1417 free, 23 reclaimed      # works
```

`blank.adf` is **DOS\0 OFS** and `testofs.adf` is **DOS\2 OFS/INTL** — neither
has a dir-cache at all, so there is no chain to be corrupt. The rebuild is
consulting the extension field at `BS-8` without first asking whether the volume
is `DIRC`. `afs verify` and `afs ls` both read those volumes correctly, so this
is the rebuild alone, and it is exactly the same family as M21's wrong dircache
chain offset.

**So the order is fixed, and it is not optional:**

1. **Fix `bitmap-rebuild` on non-DIRC volumes.** Until this lands there is no
   correct remedy to name, so the guard cannot ship. (One condition: gate the
   dircache handling on `afs_volume_has_dircache`.)
2. **Then the M34 guard** (`afs_verify_map_disagrees`, already written) plus the
   rebuild line in the gates that write — measured cascade was 7 gates and 164
   unit checks, because **every** fixture in the repo carries a stale map, which
   is consistent with the long-standing note that 10 of 11 do.
3. **`test_alloc.c`'s synthetic volume** needs its map corrected rather than
   rebuilt: it is the fixture for the allocator, so per M29 its map must be
   right rather than repaired. That is the same defect class as M29's fourth
   instance, and it is the fixture sharing the mistake.

The gate-side work was done and verified (m31, m32 and m33 went green with one
rebuild line each); it is held, not lost.

### Also found while implementing

**A per-allocation check must not scale with the volume.** The first version of
`afs_verify_map_disagrees` copied the whole image and called
`afs_verify_memory_ex`. It sits in `afs_alloc_block`, so writing a 600-block
file would have copied 1760 blocks 600 times. The version that ships walks the
*tree* directly through `visit_entry(afs_volume_t*, ...)` — the same order of
work as the bitmap read the caller was already doing, and it reuses the
verifier's walk so there is exactly one definition of "referenced by the tree".

### What mount_fuzz actually reported — and why that changes the next step

The gate work was done and measured (m31, m32, m33, m13, m14 and m15 all went
green with one rebuild line each), which left **one** gate red. I assumed, from
its `write probe saw a failure` line, that the M34 refusal was arriving as the
wrong errno and that `err_to_errno` needed a case for it. Mapping it to `EROFS`
and teaching the gate to count it **did not clear it** — so the assumption was
wrong, and the tool's own output says so:

```
strict iter 7:  wprobe: iter 71 op append: write landed wrong bytes: .../mnt/.wprobe/lrfcbw
lenient iter 7: wprobe: iter 71 op append: write landed wrong bytes: ...
lenient iter 18: wprobe: iter 71 op append: write landed wrong bytes: ...
```

**`write landed wrong bytes`, not a refusal.** With the rebuild in place the
guard no longer fires, the write is accepted, and the data that comes back is
not the data that went in. So the remaining blocker is not the guard's errno —
it is that a rebuild followed by a write through the mount lands data in the
wrong place on a volume the fuzzer has damaged with its own byte flips.

Two things follow, and they are the same lesson:

- **Read the failing tool's own output before believing the gate's summary
  line.** The gate said "write probe saw a failure"; the probe said which write
  and what happened. The summary line is a category, not a diagnosis, and a
  cycle went into fixing the category.
- **Do not keep adjusting a fix after it stops explaining the evidence.** The
  `EROFS` change was made on a hypothesis I had not tested, it failed, and the
  right move was to revert rather than stack a second adjustment on top.

**So the order becomes:**

1. **`bitmap-rebuild` on a damaged volume, then a write, lands wrong bytes.**
   Reproduce with the mount and `iter 71 op append` in hand. The prime suspect
   is the rebuild freeing a block that damage made unreachable — which is what a
   rebuild is *defined* to do, so the real question is whether the subsequent
   write can tell the volume is no longer sound. That is the same question M34's
   guard asks, arriving from the other direction.
2. Then the M34 guard, whose errno mapping this suggests is already right.

Both are written and measured; they stay out of the tree while one gate is red.

### Narrowed further: it is the MOUNT write path, not the library

What the probe actually asserts: `region_is()` compares the probe's **model**
against what it read back, and only after the syscall reported success. So
"write landed wrong bytes" means the write returned 0 and the data came back
different -- silent corruption, the exact class this project exists to catch.

Attempting to reproduce it outside the fuzzer, on `testffs.adf` with one
structural byte flipped in block 1142:

| path | result |
|---|---|
| `afs put` + `afs cat`, rebuild then damage | **bytes correct** |
| `afs put` + `afs cat`, damage only | **bytes correct** |
| the same, on `testofs.adf` and `blank.adf` | **bytes correct** |

So the **CLI write path does not reproduce it on any fixture tried.** The
failure is specific to writing **through a live FUSE mount** -- `create` plus
`setattr`/`extend` plus `write` -- which is a different path with its own block
allocation and its own truncation/extension handling.

### The reason it could not be reproduced: the gate was not deterministic

There was no seeded sequence to replay, because **the gate had no seed.** The
damage in the write lane was

```sh
off=$(( (RANDOM * 32768 + RANDOM) % (size / 512) * 512 ))
```

and bash seeds `$RANDOM` from the pid and the clock. So the byte pattern
differed on **every run**: a failure could not be replayed, and "iter 7" in one
run said nothing about any other. It looked flaky -- the wrong-bytes write
appeared in some runs and not others -- and **a flaky defect that cannot be
seeded is not a defect anyone can fix.**

`mount_fuzz.sh` now seeds `$RANDOM` once per lane from a single overridable
base, so the same seed replays the same damage exactly:

```sh
MOUNT_FUZZ_SEED=12345 bash tests/oracle/mount_fuzz.sh
```

Measured: two runs at the default seed produce byte-identical tallies
(`iterations=40 mounted=36 refused=4 entries=1097 files=843
bytes=9796749` / `25 mounted, 3 refused, 3838 ops, 3762 accepted`), and the
gate passes at seeds `20261016` (twice) and `7`.

**So the wrong-bytes write was a real damage pattern that the gate could
neither keep nor reproduce** -- it is now both. This is the standing rule from
M23 applied to a gate rather than a test: *never dismiss a flake without
reproducing it*, and the first half of that is making reproduction possible.

### Root cause found: the rebuild is what makes writes possible, and then the
### mount write path over-commits blocks on a nearly-full volume

With the gate now seeded, a sweep settled the attribution in one run.

**1. The defect is not pre-existing and not the guard's.** Rebuilt the gate state
twice and swept eight seeds each:

| tree | seeds swept | result |
|---|---|---|
| guard OFF, no rebuild line | 1, 2, 3, 11, 42, 101, 999, 31337 | **8/8 PASS, clean** |
| guard OFF, **rebuild line only** | the same eight | **8/8 FAIL**, `strict iter 7`, identical every seed |

So the rebuild line alone is sufficient to cause it, and the guard is
irrelevant. Every seed fails identically because the fixture is chosen by
`SOURCES[$(( (i + 7) % n ))]` -- a function of `i` alone, not of `$RANDOM` --
so iteration 7 is the same image in all eight runs.

**2. The volume is not damaged.** `strict iter 7` is the lane the gate itself
describes as "a healthy volume"; only the lenient lane flips bytes. So this is
not a fuzz artefact: it is reproducible on an **untouched fixture**.

**3. The fixture is `ffdisk0049.adf`, and the op is an append.** The CLI cannot
reach this: it has no `append` verb at all (`put` overwrites), which is exactly
why part 3's CLI probe came back clean three times. Any conclusion drawn from
`afs put` about an append defect is a conclusion about a different program.

**4. The rebuild's role: writes are impossible without it.** On the stale map,
`create` fails with `EIO` before a single byte is written. That is why every
write gate (m13, m14, m15, m31, m32, m33) needed the rebuild line -- and it
means the rebuild is load-bearing for the entire write path, not a convenience.

**5. The failure is a size/allocation disagreement on a nearly-full volume.**
`ffdisk0049.adf` reports **40 free blocks**. Reproduced outside the fuzzer by
appending growing chunks through a real mount:

```
append@4097   ok        (model  11354, mount  11354)
append@8193   WRITE FAILED
STOP at append@8193: model=19547 mount=18522
```

The file had 11354 bytes (22 blocks) and the volume had 40 free. The write asked
for 8193 more and **committed 7168 bytes (14 blocks) before reporting failure**.
With ~3 blocks actually free, 14 cannot be allocated. The file's size and its
allocated blocks therefore disagree, and a later read of the tail returns data
that is not the file's -- which is precisely `write landed wrong bytes`, and
precisely why the probe saw the write **succeed** while the bytes were wrong.

**6. What was refuted along the way, by measurement:**

- *The rebuild frees blocks belonging to live files.* Snapshotted all **77**
  files, rebuilt, re-snapshotted, then `put` a new 64-byte file and
  re-snapshotted: **0 files changed by the rebuild, 0 clobbered by the write.**
  So the rebuild's walk does reach the live tree; it is not poaching.
- *The fuzzer's damage.* Refuted by point 2 -- the strict lane never damages.
- *The M34 guard's errno.* Refuted by point 1 -- the rebuild alone reproduces it.
- *The CLI write path.* Refuted by point 3 -- it cannot express an append.

**So the remaining work is one defect, and it is not the M34 guard:** the mount
write path must not let a write commit more blocks than the volume has free,
and must not leave a file whose recorded size exceeds its allocated blocks.
ENOSPC has to surface as a short write or an error with the size rolled back --
never as a successful `write()` whose bytes are not there. The M34 guard is
still correct and still worth shipping, but it is guarding a *different*
failure and must not be held hostage to this one.

Also fixed here: `mount_fuzz.sh` declared `lane_no=0` at the top and never
incremented it, so both lanes received the same seed while the comment claimed
per-lane seeding. It is now derived per iteration from the lane name.

### CORRECTION to part 5: the over-commit claim was mine, and it was wrong

Part 5 concluded "the mount write path over-commits blocks on a nearly-full
volume", on the strength of this:

```
append@4097   ok        (model  11354, mount  11354)
append@8193   WRITE FAILED
STOP at append@8193: model=19547 mount=18522
```

**That measurement was an artefact of my own test.** `dd`'s default `bs=512`
splits an 8193-byte append into **16 separate 512-byte `write()` calls**. Each
one succeeds until the volume fills, so the file grows normally and only the
last write fails — which is ordinary ENOSPC, not a write that committed more
than it should. The file grew by exactly 14 × 512 = 7168, i.e. 14 writes landed
and the 15th hit the wall. "7168 bytes committed before failure" was never a
fact about the library; it was a fact about `dd`.

The library is in fact built the other way on purpose. `afs_file_write_range`
allocates its whole deficit **before writing anything** —

```c
/* deficit first: the volume is still exactly as it was until these all
 * succeed, so a full volume leaves the file untouched */
```

— so a single call that cannot fit commits nothing. The comment states the
invariant and the code honours it.

Re-run with **one write per append** (`bs=$sz count=1`), which is what the probe
actually does, the picture is different again and is *not* the probe's symptom
either: one 8193-byte write committed 934 bytes and then failed. That is a
short write reported as an error rather than as a byte count — a real POSIX
wart worth its own record, but **not** `write landed wrong bytes`, which
requires the write to have fully succeeded.

Recording this rather than quietly restating the number: the direction was
right (the defect is in the mount write path, and the rebuild is what makes
writes possible at all) but the mechanism was wrong, and I drew it from a test
that could not have produced the evidence it appeared to give.

### A faithful, deterministic reproduction

Two facts made this cheap, and both were sitting in the probe:

- `WPROBE_TRACE=1` narrates every operation, with the file and the model's
  length at that moment.
- The gate calls the probe with a **fixed seed** — `mount_write_probe $MNT
  250 17` — so the whole op sequence is deterministic. The earlier flakiness was
  entirely in the *damage* lane; the probe itself was always replayable.

Rebuilding `ffdisk0049.adf`, mounting it rw, and running the probe with the
gate's own arguments reproduces it exactly, every time:

```
wprobe: iter 71 op append: write landed wrong bytes: .../.wprobe/lrfcbw
TRACE iter 35 create .../lrfcbw
TRACE iter 67 create .../lrfcbw
TRACE iter 71 append .../lrfcbw (model len 228)
```

**What this rules out, each by evidence rather than by plausibility:**

- **Not ENOSPC.** The file is **228 bytes** — one data block against a 488-byte
  OFS payload. The append needs at most one more block. Nothing is near a
  capacity boundary here.
- **Not a stale probe model.** `do_create` opens `O_RDWR|O_CREAT|O_TRUNC` and
  then calls `reload(e)`, adopting the mount's reported length, with the
  comment *"Whatever the mount reports is the truth."* So the model saying 228
  at iter 71 is the mount's own answer, not drift. (I had first suspected the
  iter-67 create truncating a live file; `reload` kills that.)
- **Not the rebuild poaching live blocks.** All 77 pre-existing files are
  byte-identical after a rebuild and a 64-byte put.

**So the defect is narrow and specific:** a successful `O_APPEND` that extends a
one-block file to two blocks writes the bytes somewhere other than where the
append must land — or the read-back of them does. Both halves are now
replayable in a single command, which is the precondition part 2 lacked and
part 4 supplied.

The next step follows directly: instrument `afs_file_write_range` for the
two-block case — log `old_n`, `new_n`, `keep`, `need_data`, the allocated
blocks, and the data block's `next` pointer and `data_size` — and check the two
things a 1→2 block append must get right: that block 1 is really allocated and
really written, and that block 0's `next` points at it. `m13_write.sh` covers
growth, but on volumes with free blocks to spare; `ffdisk0049.adf`'s 40 free
blocks are the distinguishing condition and no existing gate uses it.
### The minimal reproduction, and four ways I misled myself getting there

The reproduction is now one command and it is exact:

```sh
cp tests/fixtures/ffdisk0049.adf v.adf
build/afs bitmap-rebuild v.adf --yes
build/amigafs-fuse -f v.adf mnt -o rw &
WPROBE_TRACE=1 mount_write_probe mnt 72 17        # -> iter 71, wrong bytes
```

| iterations | result |
|---|---|
| 10 | PASS |
| 40 | PASS |
| 71 | PASS |
| **72** | **FAIL -- `iter 71 op append: write landed wrong bytes`** |

72 is the minimum, and it is deterministic on seed 17. Every earlier run of
this investigation was 250 iterations, because a positional-argument mistake in
my own harness (`ITERS=${3:-250}` when I was passing the count as `$2`, then
`${2:-250}` when I passed it as `$1`) silently ran 250 every time.

**What is proven correct, by direct test, so the search does not repeat it:**

- **The library's 1 -> 2 block append.** A test that creates 228 bytes and
  extends to 528 in one call -- genuinely past the 488-byte OFS payload, so
  genuinely a second block -- round-trips byte-exact on `ffdisk0049`, `testffs`
  and `testofs`, with no mount involved.
- **The allocator on a full volume.** On the 1760/1760 image, `afs_alloc_block`
  returns `-1` with *"volume full: no free block"* and hands back no block. It
  is not handing out blocks it should not have.

So the defect needs the *state* the preceding ~70 operations build. The write
path and the allocator are each right in isolation; the interaction is wrong.
That points at block reuse across the churn -- the run does creates, unlinks,
renames and mkdirs before iter 71, and an unlink returns a block to the pool.

**Four errors of mine, all of which pointed somewhere false:**

1. **`dd`'s default `bs=512`** split one append into 16 writes, so the file
   grew normally and only the last write failed. "7168 bytes committed" was a
   fact about `dd`, not the library. (Recorded in part 6.)
2. **The library test used `add = 260`**, giving exactly 488 bytes -- *one*
   payload block. It was testing 1 -> 1 while its own comment claimed 1 -> 2,
   and it passed for that reason. The genuine 1 -> 2 case (`add = 300`) also
   passes, but the first test proved nothing it claimed to.
3. **The harness reported `iter 71` for a 10-iteration run.** That is
   impossible: `g_iter = it` under `it < iters`, so `g_iter` cannot exceed 9.
   I should have treated the impossible output as proof that my harness was
   wrong, instead of accepting it for three consecutive runs. An impossible
   measurement is not a measurement.
4. **I inferred the volume was full at iter 71 from the final image being
   1760/1760.** It says nothing about iter 71, because the probe **unlinks**
   files -- iter 70 is one -- and an unlink frees blocks. The volume was not
   full when the append ran.

**What the next step should be.** Not instrumentation of the write path, which
is now excluded, and not the allocator, which is excluded. The remaining
candidate is block reuse: a block freed by an unlink, handed to a later create,
and then reached through a stale pointer -- most likely a directory whose entry
still names a header block that has since been reused. The reproduction is
cheap enough to bisect on the trace: run 72 iterations with `WPROBE_TRACE=1`,
then re-run with the unlink and rename operations suppressed, and see whether
iter 71 still fails. If suppressing them clears it, the defect is in reuse
after removal; if it still fails, the prior state is only a directory layout
and the fault is in how a header block is resolved.
### Down to the bytes: the file's recorded geometry does not match the write

Instrumenting a **copy** of the probe (throwaway, outside the repo -- the
committed gate is untouched) and dumping both sides of the first divergence:

```
DIVERGE len=133 at +0 (file offset 228)
  want[0..48): 3e7237b89c57ce4d820a1d388eeb98bc453f05d89bcfea434240b728b331b4a
  got [0..48): 479b6bd94a187ac60fb60f6a7f68ff1cb5aefadc01b374c6b8275754c2da6a
```

The divergence is at **+0** -- the very first byte, not a shifted tail and not an
off-by-one. So this is not "the append landed a few bytes late".

**What is actually on disk afterwards**, resolved through the file's own chain
(`entries = 512/4 - 56 = 72`, so table entry k is at `hdr + 24 + 4*(71-k)`):

```
.wprobe/lrfcbw
  header block 1132, recorded size 507 bytes  (needs TWO blocks)
  table entry 0 -> 1133, entry 1 -> 1125
  data0 block 1133: type=00000008 seq=1 data_size=488 next=1125   bitmap: used
  data1 block 1125: type=00000008 seq=2 data_size=19  next=0      bitmap: used
  data1 payload: fd 88 41 9c b1 cc 5d 16 4a 11 55 87 c2 ... then zeros
```

**The recorded geometry and the operation that produced it do not match:**

| | probe says | image says |
|---|---|---|
| size before the append | 228 | |
| bytes appended | 133 | |
| size after | **361 (one block)** | **507 (two blocks)** |
| second data block | not needed | 1125, `data_size` 19 |

228 + 133 = 361, which fits in one 488-byte payload. The image has two data
blocks and a size of 507 -- **146 bytes more than the write contained**. And
`data1` holds 19 bytes of stale content followed by zeros, which is the shape
of a block that was freed and reissued, not of a block this write filled.

Reading block 1133's payload at offset 228 gives the probe's `got` bytes
exactly -- so **the read path is self-consistent, and the appended bytes are
simply not there.** The file claims to be longer than the data it contains.

**And the probe's model already disagreed with the mount before the append.**
`do_append` prints `model len 228`, but the header says 507. `do_create` calls
`reload(e)` and adopts the mount's reported length, so the model was 228 as of
iter 67's create. Between iter 67 and iter 71 the probe does exactly three
things -- `mkdir`, `rename`, `unlink` -- and none of them touches this file. So
either

- the model went stale and the append used the wrong offset, or
- **one of those three directory mutations wrote 279 bytes into this file.**

Those have very different fixes, and I cannot yet tell them apart. What is not
in doubt is the invariant that is broken: **a file's recorded size, its block
table and its data must agree**, and here all three disagree with each other.
That is the thing to pin, and it is checkable directly on the image without
running the probe at all: for every file, `header.size` must equal
`sum(data_size)` over its chain, and `data0.data_size + data1.data_size` must
equal the header's size. On this file they do not.

The next step follows from that: make a checker that walks every file on the
image and asserts `header.size == sum(data_size in chain)`, then run it over
the post-failure image and over each fixture after a rebuild. If it fires on the
post-failure image only, the write path corrupts a file's geometry and the
library test that passes in isolation is missing the precondition -- the
directory churn. If it fires on a fixture too, there is a standing geometry bug
that no gate looks for.
### A geometry checker, and part 8's hypothesis is refuted

I wrote the checker part 8 asked for. It walks every file in the tree and
asserts, without a mount, a write, or the oracle:

- `header.size` equals the sum of `data_size` over the file's OFS chain;
- the chain length equals `ceil(size / payload)`;
- each data block's `next` pointer equals the following table entry;
- every block in the chain is marked used in the bitmap.

Committed as `tests/oracle/geomcheck.c`. **Its own two bugs are worth
recording**, because both produced confident nonsense first:

- it walked the chain from the **last** table entry instead of the first, so
  every multi-block file was short by exactly its first block;
- it indexed past `entries` for files needing **extension blocks**, reading
  *before* the header. That accounted for most of its first run's 83 "failures".

Multi-table files are now **skipped and counted**, not mis-read — re-deriving
the extension-block walk here would be a second copy of a library invariant,
which is the mistake `llm-wiki` already records for readers.

**The checker refutes part 8.** The post-failure image is geometrically
perfect:

```
85 files checked, 7 skipped, 13 directories, 0 disagreements -> PASS
```

`.wprobe/lrfcbw` is **internally consistent at 507 bytes** — `data_size` 488 +
19, `next` chain intact, both blocks used. So the file was never malformed. The
size/table/data disagreement part 8 built its next step on **is not there**, and
the write path did not corrupt a file's geometry. Part 8's proposed next step
was itself the thing that disproved it, which is the outcome worth wanting.

**And 9 of 10 fixtures are geometrically clean after a rebuild.** Their first
run "failures" were all one class — `data block N is marked FREE` — which is the
documented stale-map condition of every fixture, not a geometry violation. Once
that class is separated out:

```
after bitmap-rebuild, geometry only:
  arccsh blank ffdisk0049 links testffs testhd test_link_chains
  testofs win32-names                     -> 0 disagreements
  g1a30c                                 -> 10 disagreements
```

### A new finding: `bitmap-rebuild` leaves live blocks marked free on g1a30c

All ten of `g1a30c`'s are the *same* class, and that is the point — **they
survive the rebuild**:

```
BAD /c/TYPE:         data blocks 967, 968, 969, 970 marked FREE
BAD /c/Hola:         data block 972 marked FREE
BAD /l/Ram-Handler:  data blocks 1068, 1069, 1070, 1086, 1087 marked FREE
```

Those files are **in the listing** — `/c/Hola` and `/l/Ram-Handler` both appear
as `f1` — so the tree walk reaches them, and the geometry checker reads their
data tables and finds block 972 in `/c/Hola`'s table. Yet after the rebuild the
bitmap says that block is free.

**This is exactly the hazard the M34 guard exists for**: the map marks free a
block the tree references. It is also the corruption mechanism `mount_fuzz` has
been pointing at all along — a write allocates a block that is live.

It is **not currently exploitable on `g1a30c`**: a `put` after the rebuild is
**REFUSED**, so the allocator never gets that far on that volume. That is worth
stating precisely rather than as "fixed" — the map disagrees, and something
else happens to stop the write first. Which something is the open question, and
it is a good one: a refusal that is not the M34 guard is a refusal from
somewhere the M34 guard would not have needed to be.

**Two things to do, in order:**

1. **Work out why `g1a30c` differs.** It has no soft links (22 `f1`, 6 `d1`, no
   softlink kinds), so the obvious M31-shaped explanation is out. What
   distinguishes it: 998 orphans — by far the most fragmented volume in the set.
   Rebuild its map and watch which walk path skips `/c/Hola`'s data blocks.
2. **Then land the M34 guard**, which is now clearly worth having: it is not
   guarding an edge case, it is guarding the state this fixture is permanently
   in.

`geomcheck.c` is committed but **not yet wired as a gate** — `run_all_gates.sh`
and `CMakeLists.txt` must name the same gates, and that pairing is verified
together rather than in one place. Wiring it, scoped to geometry only, is the
follow-up.
### Part 9's "new finding" was wrong: the rebuild never ran

Part 9 reported that "`bitmap-rebuild` leaves live blocks marked free on
`g1a30c`". It does not. **The rebuild refused**, and I never saw it, because I
ran it with output to `/dev/null` and assumed it had worked — the same mistake
`dd`'s `bs=512` was, in a different costume.

Running it with its output visible:

```
arccsh          rc=0   1760 blocks, 1322 used, 438 free, 20 reclaimed
blank           rc=0   1760 blocks,    7 used, 1753 free,  5 reclaimed
cache_crash     rc=1   DOS\6 is not a supported AmigaDOS volume
ffdisk0049      rc=0   1760 blocks, 1718 used,  42 free, 26 reclaimed
g1a30c          rc=1   volume lists 2 bitmap pages but needs 1;
                         rebuild needs the page list repaired first
links           rc=0   ... 12 used, 1748 free, 10 reclaimed
testffs         rc=0   ... 343 used, 1417 free, 23 reclaimed
testhd          rc=0   3520 blocks, 306 used, 3214 free, 16 reclaimed
test_link_chains rc=0  ...  31 used, 1729 free, 29 reclaimed
testofs         rc=0   ... 366 used, 1394 free, 18 reclaimed
win32-names     rc=0   ...  86 used, 1674 free, 20 reclaimed
```

So `g1a30c`'s ten "marked FREE" disagreements were simply the **un-rebuilt
stale map** — the same condition every fixture has — and the geometry checker
was reading a map that had never been touched. `rb_mark_file` is exonerated:
the walk never reached it because the rebuild aborted before writing anything.

Confirmed directly: calling `afs_bitmap_rebuild` and reading the map back
through the same handle shows 972, 967 and 1086 still free while 880 and 881
stay used — the map is untouched, and the refusal leaves no partial state. That
is the correct behaviour: **refuse with a name, write nothing.**

### The real finding: the refusal names a remedy that does not exist

> `volume lists 2 bitmap pages but needs 1; rebuild needs the page list repaired
> first`

**There is no command that repairs the page list.** The whole tool surface for
this is `afs bitmap` (report) and `afs bitmap-rebuild` (rebuild). So a user
holding such a volume is **stuck in a loop**: the rebuild needs the page list
repaired, nothing repairs the page list, and therefore the map can never be
rebuilt — and since the stale map is exactly what refuses writes, **the volume
cannot be written at all.** One bad field in the rootblock, and the documented
remedy for every other stale-map volume is unreachable.

This is the same class as M34, and the opposite mistake. M34's guard refused and
named `afs bitmap-rebuild` as the remedy — which exists. This refuses and names
a repair that **does not**. A refusal that names a missing remedy is worse than
no refusal, because it tells the user to go and do a thing they cannot do.

**The fix is small and in the spirit of the tool.** The rebuild's whole purpose
is to repair the map from the tree, and the number of bitmap pages a volume
needs is *derivable* — 1760 blocks needs one 512-byte page. So the page count
should be **derived and rewritten**, not demanded of the volume and trusted:

1. Derive the required page count from the block count and block size.
2. If the rootblock's list disagrees, rewrite the list as part of the rebuild —
   which is the same all-or-nothing, nothing-written-until-the-walk-completes
   discipline the rebuild already has.
3. Drop the refusal, or narrow it to the case where the list is *longer* than
   derived (where the surplus pages may hold live data and freeing them is a
   decision, not a repair).

**Next: prove step 2 is safe before writing it.** `g1a30c` is the only fixture
that refuses, so it is the only test data for this, and its 998 orphans mean a
wrong derivation frees a lot. The check that must pass before and after: the
geometry checker clean, `afs verify --orphans` unchanged, and the ADFlib oracle
still reads the same tree. If the derived page count disagrees with the list on
a volume where the list is *right*, that is the case that must refuse.
### Rules M34

- **M34.1 — A self-consistent map is not an accurate map.** The two guards
  checked that the map was well-formed. The defect was that it was *wrong*.
  **When trusting a derived structure, check it against its source, not against
  its own shape.**
- **M34.2 — Validate before allocating, or every allocation is a lottery.** The
  map is consulted once per block; a single wrong bit silently destroys
  something else.
- **M34.3 — Read the symptom's arithmetic.** "1 write fine, 2 corrupt" looked
  like a size threshold and turned out to be `2..601` vs `602..1201` against a
  fixture whose live blocks all sit above 880. The numbers named the cause.

### M37 — INTL/DIRC exactness: the first oracle-differenced round trip on those types

Closes the longest-standing open risk in the plan ("INTL/DIRC exactness: the
FAQ is thin here"). Nothing in the suite had ever generated a volume of these
types — every fixture is DOS\1, DOS\3 or DOS\5 by way of however it was made. The
generator is the oracle's own `adfformat`.

**Obtained, differenced against the oracle:**

| type | volume | result |
|---|---|---|
| `-t 0` | OFS | round trip **byte-identical**, no rebuild needed |
| `-t 1` | FFS | round trip **byte-identical**, no rebuild needed |
| `-t 3` | FFS/INTL | round trip **byte-identical**, no rebuild needed |
| `-t 5` | FFS/DIRC&INTL | round trip **byte-identical**, after one rebuild |
| `-t 7` | FFS/DIRC | refused **on purpose** — AmigaOS 3.2 long-filename type |
| `-t 2/4/6` | OFS+INTL | **cannot be generated at all** |

For each type the gate writes a file, a 3000-byte binary and a nested file, then
lets **`unadf` extract them** and compares the extraction byte for byte against
the source. The listing is compared too. The oracle does the reading; nothing is
self-attested.

### A limit of the oracle's tool, not of the format

`-t 2`, `-t 4` and `-t 6` — every **OFS+INTL** combination — are refused by
`adfformat` itself, at `examples/adfformat.c:261`:

```c
options->fsType > 7 ||
( options->fsType > 0 && options->fsType % 2 == 0 )
```

That is ADFlib declining to *create* those volumes. It says nothing about whether
they are valid, and it means the OFS+INTL cell of this risk **cannot be closed
with the pinned oracles**. Recorded as still open rather than skipped, because a
silently dropped cell is how a risk list becomes fiction. Closing it needs either
an ADFlib that can write them, amitools' `xdftool`, or a hand-built image
cross-checked against the FAQ's layout tables.

### A real user-visible consequence of shipping the M34 guard

A freshly formatted **DIRC** volume is born with a map that disagrees with its
own tree: `adfformat` does not mark the dir-cache block used. So the first
`mkdir` on any DIRC volume from this formatter is refused:

```
afs: cannot mkdir SUB: the allocation map disagrees with the directory tree:
block 882 is referenced but marked free ... Rebuild the map first:
afs bitmap-rebuild <image> --yes
```

and succeeds immediately after that one command. **Every DIRC volume formatted by
this oracle needs one `bitmap-rebuild` before its first write.** That is the guard
working exactly as specified, and the message naming a remedy that exists is the
whole point — but it is a behaviour change for DIRC volumes, so it is pinned in
the gate rather than left for a user to discover.

Note the same shape appears in the non-DIRC types (`-t 0`, `-t 1`, `-t 3`), where
the rebuild is harmless and the gate applies it to all four so that "the rebuild
is safe everywhere" is asserted rather than assumed.
### The directory-GID width question is NOT decidable with the pinned toolchain

The plan listed this as "a blocker for shipping `-o amiga_ids`" and prescribed a
method: *"hex-dump a directory block from a MultiUser-formatted image,
cross-checked against ADFlib's `bDirBlock` and amitools' dir-block layout."*

**That method cannot work, and both cross-checks are empty.** Checked directly:

- **ADFlib `AdfDirBlock`** (`src/adf_blk.h:266`) — the struct has `access`,
  `days/mins/ticks`, `nameLen/dirName`, `real`, `nextLink`, `nextSameHash`,
  `parent`, `extension`, `secType`. There is **no `uid` and no `gid` field at
  all**. ADFlib does not model them anywhere, so it cannot confirm or refute a
  width for a field it does not read.
- **amitools `UserDirBlock`** — reads `own_key`, `protect`, `_read_nac_modts()`,
  `hash_chain`, `parent`, `extension`, `hash_table`. Again **no `uid`, no
  `gid`**. A grep for `uid` across the entire `amitools/fs/` tree returns
  **zero** hits outside `uuid`/`guid`.
- **`xdftool`** — its command list is `open create list type read makedir write
  delete format bitmap blkdev protect comment time block pack unpack repack boot
  root info relabel`. No `uid`, no `gid`, and `format` has **no MultiUser
  option**, so no MultiUser volume can be produced with it either.

So neither pinned oracle carries the field, and no available tool can create an
image that would carry a non-zero one. **A byte-level dump could not settle it
either**: a freshly written MultiUser directory has `GID == 0`, and four zero
bytes and two zero bytes are the same sixteen bytes. Distinguishing the widths
requires a volume with a non-zero GID, which means MultiUser-aware tooling that
does not exist here.

**What is settled is the decision, not the fact.** The reader exposes
`uid = be16(bs - 196)` and `gid = be16(bs - 194)` (`src/lib/dir.c:1009-1010`) —
both 16-bit, which is the conservative reading under either hypothesis: if the
field really is `ushort`, it is exact; if it is really `ulong`, we return its low
half and lose the high half rather than reading past it into whatever follows.
> **CORRECTED by M43: `-o amiga_ids` IS offered.** This section, and the open
> risk below it, both said the option was "not offered". It is registered at
> `src/fuse/main.c:1271`, in the option help, plumbed through `afs_translate_ids`
> into `fill_stat`, and documented in the README as off by default. The claim was
> inherited from a stale plan line and repeated here without reading the option
> table. It matters beyond bookkeeping: **the width question is live in a
> shipped, user-facing option.** `m43_amiga_ids.sh` exercises it end to end.
>
> The two read sites are `src/lib/dir.c:1009` (dirents) and `src/lib/file.c:175`
> (file headers).

The honest status is therefore not "unsettled" but **"unsettleable here, and the
conservative exposure is in place."** Anyone who wants it settled needs the
primary source — the `MultiUser` filesystem handler itself, or a genuine
MultiUser-formatted image — which is outside what a pinned oracle pair can
provide. Recording the impossibility is the deliverable; quietly widening the
field to `be32` on the strength of the FAQ's internal inconsistency would be
worse than leaving it 16-bit, because it would read a field the FAQ contradicts
itself about and no oracle can check.

**And the plan's instruction is corrected in place**, because leaving a
prescribed method that cannot work is how the next session spends a day on it.
### M41 — the last open cell: OFS+INTL, and refusing to create is not refusing to read (2026-10-16)

M38's record of the INTL/DIRC risk said the OFS+INTL cell *"cannot be closed with
the pinned oracles"*, on the evidence that `adfformat` rejects `-t 2/4/6`. That
was true of `adfformat` and **false of the oracles.**

- `adfformat` refuses them at `examples/adfformat.c:261`
  (`fsType > 0 && fsType % 2 == 0`) because it cannot *create* them.
- `adfinfo` and `unadf` read a `DOS\2` image perfectly well — the cell was never
  unobservable, only un*formattable*.

The construction is one byte: the DOS subtype is byte 3 of the bootblock's first
long, so `adfformat -t 0` plus a byte at offset 3 **is** an OFS+INTL volume.
That is a legitimate construction rather than a fake — the subtype byte is the
only difference between these variants, and the oracle reads the result and
extracts from it, which is the check that matters.

    DOS\2  OFS/INTL        ours: "DOS\2 OFS/INTL"        oracle extraction BYTE-IDENTICAL
    DOS\4  OFS/DIRC&INTL   ours: "DOS\4 OFS/DIRC&INTL"   oracle extraction BYTE-IDENTICAL
    DOS\6  refused by us, and the oracle extracts nothing either -- agreement

So **all six DOS types now round-trip byte-identically**, and the last known gap
in the plan's risk list is closed. The lesson is the shape of the error rather
than the byte offset: **a tool's inability to create a thing is not evidence
about anyone's ability to read it**, and I had recorded the former as the latter
and written off a cell as unreachable. It is the same move as the "ADFlib RDB
support is experimental" claim retired in M40 — testing an inherited limitation
instead of inheriting it.

### M40 — the "ADFlib RDB support is experimental" claim is wrong for this purpose (2026-10-16)

The last open risk. The M7 gate used **amitools `rdbtool` alone**, because
"ADFlib's RDB support is experimental". Tested rather than inherited, and that
claim does not hold for this use: **`adfinfo` prints a full partition table**
(idx, first bl., last bl., filesystem, name) that agrees with `rdbtool` on every
field of every partition of the A590 image.

The derivation, because it looked like a conflict first and was not:

    amitools  Partition: #0 'OFS'  2  115   6156  3.0Mi  DOS0
                                            ^part_blks, NOT a last block

`fs/rdb/Partition.py:99` formats `num, name, low_cyl, high_cyl, part_blks`, so
the two numbers are **cylinders**. With `cyl_blks = 54`, 'OFS' `low_cyl=2`
`high_cyl=115` gives blocks `2*54 = 108 .. 115*54+53 = 6263` — exactly what
`adfinfo` prints. The apparent conflict was reading amitools' *third* number
(6156) as a last-block and comparing it to ADFlib's 6263.

All three sources agree on the real image:

| partition | ours (start, size) | ADFlib (first, last) | size = last-first+1 |
|---|---|---|---|
| OFS | 108, 6156 | 108, 6263 | 6156 |
| OFS INTL | 6264, 6156 | 6264, 12419 | 6156 |
| OFS DirCache | 12420, 6156 | 12420, 18575 | 6156 |
| FFS | 18576, 6156 | 18576, 24731 | 6156 |
| FFS INTL | 24732, 6156 | 24732, 30887 | 6156 |
| FFS DirCache | 30888, 11340 | 30888, 42227 | 11340 |

So the oracle is **no longer single-sourced**, and a genuine disagreement between
the two is now visible rather than undetectable. That is the actual value of the
change: not that a second opinion agrees today, but that a day it does not, the
gate says so.

**Load-bearing, proven by doctoring the second oracle** — ADFlib with partition
2's start shifted by one:

    FAIL  partition "OFS DirCache" start: ours 12420, ADFlib 12421
    M7 gate: 1 failure(s)          exit=1

And a process note, because the first attempt at that mutant proved **nothing**:
the `sed` was off by one space, never matched, and I read the resulting clean run
as a survivor — rule 276, an impossible measurement rather than a measurement. The
doctor was verified to have actually altered the output before the gate's verdict
was read, which is the step the first version skipped.

### M39 — a libFuzzer target over blockio (2026-10-16)

Closes the "hostile images" open risk. `blockio` is the one place the core
touches storage and its own header calls it the security boundary, yet it had
unit tests and **no fuzz target** — so the only hostile inputs it ever saw were
the ones a test author chose.

The target (`tests/fuzz/fuzz_blockio.c`) asks for 0, the last block, `last+1`,
`0x7FFFFFFF`, `0x80000000` and `0xFFFFFFFF` through read, read_span and write,
plus a sweep of every real block. Block size comes from the input rather than
being fixed, and two unsupported sizes are included so the reject path is
exercised too.

**Measured:** 20000 inputs, **79 new coverage units**, no out-of-bounds access.

The coverage floor is on `new_units_added`, **not** on
`number_of_executed_units` — the latter only counts runs, so a fuzzer that
explores nothing reports a perfect score. (Getting there also needed
`-print_final_stats=1`; without it the stats block is never printed and every
field reads absent, which made the first version report "0 coverage units" on a
run that was fine.)

**Two mutants, and the surviving one is the result.** Removing
`blockdev_read`'s `if (blockno >= d->nblocks)` **survived** 20000 runs — because
there is a *second* bounds check in the same function, immediately before the
memcpy: `if (off + len > (uint64_t)d->mem_size)`. The read path has **defence in
depth**, and I only found out because a mutant I expected to die did not.
Removing **both** is caught: `heap-buffer-overflow`, exit 1.

## M33 — making a leak visible, and what that immediately found (2026-10-15)

M32 ended with a defect nothing could see: a leaked block is invisible because
`afs_verify` never consulted the bitmap and `--strict` checks only the *other*
direction. This milestone builds the missing capability — and it works
immediately, which is how it earns its keep.

**`AFS_VERIFY_ORPHANS` / `afs verify --orphans`.** Reports blocks the bitmap
marks *allocated* that nothing in the tree references. Opt-in for the same
reason strict is, and the reason is measured rather than argued:

```
g1a30c 998   arccsh 550   testofs 303   testffs 238   testhd 223
ffdisk0049 210   test_link_chains 29   win32-names 20   links 10   blank 5
```

Every non-hostile fixture has some — g1a30c has nearly a thousand — because
AmigaDOS returns a deleted file's blocks only when an optimiser runs. An
everyday check reporting these would be unusable on every real image. They are
bugs only on a volume *this project wrote*, which is the case the flag exists
for.

**The tree walk was incomplete, and the new check is what showed it.**
`visit_entry` descended into directories only, so **no file's data blocks were
ever marked**. Under the old one-directional check that was invisible; under the
reverse question every allocated data block came back "unreferenced", and the
write fuzzer reported blocks 3 and 4 on iteration 0 of a run whose only
operation was a two-block file create. Completing the walk dropped `testffs.adf`
from 316 apparent orphans to 238 — **238 is the truth, not the new count.**

The offset was the fourth trap of this kind (after the bitmap coordinate space,
the dircache chain link at +16, the soft-link target at +24): **the file data
table is filled from the END backwards.** The walk read slot 0, found it empty,
and the pointer for a one-block file lives at `24 + 4*(entries-1)` = 308.
`collect_blocks` says it in one word: `REVERSED`. *When re-deriving an offset
from a format, copy the reader's arithmetic.*

**Also fixed: `afs verify` capped its report at 256 entries**, so on any volume
with more than 256 problems the *reported count stopped counting*. That is not
theoretical — it hid a mutant from this milestone's own gate, which compared
"orphans before a write" with "orphans after" and got 256 both times while the
true counts were 316 and 317. A truncated count is exactly the "an empty run
reports green" failure the project exists to prevent, and it was in the tool
users reach for to check a disk. Now sized to the volume.

**Gate** `m33_orphan_and_enospc.sh` (29 → 30). Its load-bearing assertion is a
**round trip**: writing a 1-block file, a 6-block file, and a link must each
leave the orphan count **exactly** where it was. Three mutants proven
load-bearing:

| mutant | result |
|---|---|
| the orphan pass never runs | `FAIL --orphans reported nothing` |
| data table read forwards, not reversed | `FAIL writing a small file changed the orphan count` |
| the data-block walk removed entirely | `FAIL writing a small file changed the orphan count` |

The third was invisible until the 256-entry cap was fixed — a gate that passed a
mutant because the tool it called was lying to it.

**Two CONFIRMED defects found and NOT fixed here — the next milestone's job.**
Recorded here and in the gate header so they cannot be lost:

1. **ENOSPC leak.** `afs_file_write_range`'s `fail_range` deliberately does not
   free blocks it already allocated ("no transactions: blocks allocated before
   an I/O failure stay allocated but unreferenced", `file.c`). With
   `--orphans` wired into the write fuzzer this fires on `op overflow` after
   34–55 iterations on every seed. The fuzzer therefore does **not** use the
   flag, and the gate pins the limitation instead — pinning a known limit is the
   project's convention; quietly switching a check off is not.
2. **Second-large-write corruption — silent data loss, and the serious one.**
   Found by forcing the volume nearly full:

   | | result |
   |---|---|
   | 1 × 300 KB write | `OK, no structural problems found` |
   | 2 × 300 KB writes | **20 problems** |
   | 1 × 50 KB write | `OK` |

   Blocks 1222 (`hlink_blue`) and 883 (`dir_2`) come back with invalid checksums
   and unknown types, and **both entries have vanished from the listing** — the
   allocator handed out blocks that were already in use. Not the size: the first
   large write is clean.

**Rules M33**

- **M33.1 — A check that asks the reverse question exposes the walk it depends
  on.** The orphan pass was wrong for a day because the *set it inverts* was
  incomplete. A new check is also a new test of everything underneath it.
- **M33.2 — A count that stops counting is worse than no count.** A fixed report
  cap made `verify` report 256 for any volume with more problems, and that made
  a gate pass a mutant. **Assert on quantities your tool computes without
  truncation.**
- **M33.3 — Re-measure every number a capability invalidates.** Completing the
  walk changed `testofs` from 364 to 303 and `testhd` from 295 to 223. The old
  figures were in a header as justification; they were about a *different*
  walk.

## M32 — unlinking a link leaked its block, and no gate could see it (2026-10-15)

Found by **measuring a code comment**, the same move as M31's dir-cache record.
`afs rm`'s hard-link branch and the mount's `.unlink` both said:

> A hard link is only a name. Removing it frees its own header block and leaves
> the target and its data untouched.

Neither freed anything. Measured on `testffs.adf`:

```
after hard link:  free = 1412
after rm h1:      free = 1412      <- the comment says 1413
```

One block per unlinked link, leaked permanently: the name is gone, the entry is
unreachable, and the block stays marked allocated so no allocator will hand it
out again.

**Why no gate caught it** — the more interesting half:

- `afs verify` does not consult the bitmap at all
- `afs verify --strict` checks only the OTHER direction ("referenced by the tree
  but marked free"); on these stale fixtures it produces 11 complaints that
  have nothing to do with a leak
- **nothing anywhere detects "marked allocated, referenced by nothing"** — there
  is no orphan detection in `verify.c` at all
- the unit tests call the library directly, so reverting the **shell** to the
  half-operation passes them (mutant M3, measured)

**Fix:** `afs_link_remove` — name, dir-cache record, block, in that order. It is
a *separate* function rather than a flag on `afs_dir_unlink` because **rename
calls `afs_dir_unlink` and then rewrites that same block**: freeing inside the
chain primitive would free a live entry mid-rename. The chain operation and the
release operation are different things and only one is ever wanted.

**Soft-link removal ships with it.** The refusal said a soft link *"cannot be
removed without touching its target's name"*. The AmigaDOS Developer's Manual
says otherwise: *"If a soft link is deleted then its file header block is
freed."* The link stores a path **string**, so there is no reference to repair
and no target to touch. Both shells now accept `AFS_BLOCK_SOFTLINK`.

**Oracle status, measured before designing the gate:**

- **amitools cannot witness soft links at all** — `xdftool` refuses to even LIST
  a directory containing one (`Unsupported Dir Block(8) block=@906, Sub_Type:
  00000003`). It can neither confirm nor refute anything here. Pinned in the gate.
- **ADFlib refuses to remove one, and non-atomically**: `adfRemoveEntry`'s final
  `else` rejects `ST_SOFTLINK` — but *after* it has already unhooked the entry
  from the parent's hash chain and written the parent block. So ADFlib leaves a
  half-removed entry, which is worth knowing if it is ever used as a reference.
- ADFlib *can* read soft links, so it remains the oracle for visibility, and the
  gate uses exactly that: a stale dir-cache record keeps a deleted link visible
  to `adfls` after our reader dropped the name.

**Also corrected:** `AFS_PROBLEM_BITMAP_MISMATCH`'s comment claimed to detect
"used but marked free, **or free but marked used**". Only the first direction is
implemented. Per rule 251 the claim now matches the code.

**Gate** `m32_link_unlink.sh` (28 → 29 gates). Four mutants, all load-bearing,
including the two the unit tests miss:

| mutant | gate result |
|---|---|
| shells revert to `afs_dir_unlink` | `FAIL unlinking a hard link leaked its block` |
| FUSE `unlink` reverts | `FAIL the mount leaked a block` |
| drop the dir-cache delete | `FAIL ADFlib can still see a link we deleted` |
| soft links refused again | `FAIL the CLI refused to remove a soft link` |

**Rules M32**

- **M32.1 — Measure the comment.** "Frees its own header block" was the most
  confident sentence in the file and was false. Code comments about behaviour are
  claims; `free` is a function name that has to mean something.
- **M32.2 — A checker cannot find a defect it was never asked about.** `verify`
  is a *damage* checker and a stale map is ordinary on a real Amiga disk, so it
  deliberately does one direction. That is the right design and it means **a
  writer's leak is invisible to it**. Assert on the bitmap directly.
- **M32.3 — A leak is the one defect no structural check can see.** Every pointer
  still resolves; nothing is corrupt; the volume verifies. Only the *count* is
  wrong. Reach for a round-trip invariant, not a consistency check.
- **M32.4 — Ask what an oracle can actually witness before building a gate on
  it.** Here one oracle is structurally blind and the other's refusal is
  non-atomic. A gate built on either would have asserted nothing.

## M31 — soft-link creation, and two defects only the outside could see (2026-10-15)

`afs_symlink` (library), `afs symlink` (CLI) and `.symlink` (mount). The last
missing filesystem operation. The layout came from ground truth, not memory:
block 885 of `testffs.adf` is a soft link written by a real AmigaDOS and reads
as `T_HEADER`/`ST_SOFTLINK`, `byte_size` 0, `extension` 0, `parent` = the
containing directory, and the target as ASCII at **+24, zero-padded and not
NUL-terminated**. ADFlib's `AdfEntryBlock` agrees on the fields.

**Two defects, and neither was reachable from a unit test.**

1. **The dir-cache record.** The first version wrote none, justified in a
   comment by reasoning: *"a size of zero would teach the cache this name is an
   empty file"*. Every other writer — file create, hard link, mkdir — writes
   one. `adfls` listed 14 of 15 entries on the volume and **none of the soft
   links**, while our own reader listed all of them: on a DOS\5 volume the cache
   is what an outside implementation walks, and the hash table alone is not
   enough. Ground truth settled the reasoning in one read — the volume's **own**
   soft link sits in cache block 881 as `header=885 size=0 sec_type=3`, i.e.
   exactly the record I had argued against writing. Fixed; the oracle and the
   reader now agree at 19/19.

2. **The `.symlink` parameter order.** `fuse_lowlevel.h` declares it
   `symlink(req, **link**, parent, **name**)` — the reverse of `.link`, and the
   reverse of the POSIX call's own word order. Both string parameters are
   `const char *`, so the obvious way compiles clean and produces a
   **self-consistent** volume: an entry named after the *target*, holding the
   *link name* as its content. `ln -s tgt firstlink` created an entry called
   `tgt`, and the next `ln -s` failed with `EEXIST` for a name that did not
   exist.

**Also fixed:** `tests/oracle/m31_symlink_write.sh` (28 gates, up from 27), and
the CMake `AMIGAFS_GATES` list, which had drifted from `run_all_gates.sh` and
was missing **every gate from M23 onward** — so `ctest` reported green over a
subset of what the audit entry point runs. Both lists now name the same 28, and
the gate itself asserts that. ctest 43/43.

**Rules M31**

- **M31.1 — Do not write a constraint about a format from reasoning; read what a
  real implementation wrote.** The cost was one comment, one defect, and one
  oracle disagreement that a passing test suite would never have caught. The
  fixture already contained the answer, in the cache block, for one `xxd`.
- **M31.2 — Two parameters of the same type, in an order that contradicts every
  sibling and the call that names them, is a silent data swap.** It compiles,
  it runs, and it produces a well-formed wrong answer. Read the header's own doc
  comment, not the parameter names you expected.
- **M31.3 — A self-consistent wrong result is the hardest kind to catch.** Both
  defects here were internally coherent: the swap produced a valid volume with a
  valid-looking entry, and the missing record produced a volume our own reader
  was happy with. Only a *second, independent* reader disagreed. When two
  implementations of the same spec are available, the one that disagrees is
  evidence — check which of you is wrong before assuming you are right.
- **M31.4 — A gate list that drifts from the entry point under-reports.** Two
  lists naming the same gates is duplication; two lists that *can* disagree
  silently is a reporting bug. Assert the agreement.

## M30 — asking the oracle about our own fixtures, for the first time (2026-10-15)

M26, M28 and M29 shared one shape: *a fixture and the code under test agreed with each
other, so every check agreed and the suite reported green while measuring nothing.* Each
was fixed by asking something from outside — a fresh rescan, `verify --strict`, a mutant.

The strongest outside question available is the **oracle**. It had never been asked about
the synthetic fixtures at all.

**The finding.** ADFlib had never been asked about our own fixtures. It runs on
downloaded 880 KiB images in every gate, and a small hand-built volume is exactly where
the subtle offsets live.

**A CLAIM IN THE FIRST DRAFT OF THIS RECORD WAS WRONG, and testing it is what corrected
the gate.** That draft asserted ADFlib "cannot open a volume smaller than a floppy", and
listed the 16-block unit fixtures as permanently oracle-blind on that basis. I had not
tested it. ADFlib reads the image file and infers geometry: it opens a 16-block volume as
`HardFile 8.0 KBytes`, distinguishes FFS DIRCACHE from plain FFS in `unadf`'s banner, and
extracts cleanly from it. **So the gap was never size. It was that nobody had asked.**

**What the oracle says about all three fixture shapes.** Each built exactly as its test
file builds it:

| fixture | blocks | our `verify` | oracle |
|---|---|---|---|
| `test_bitmap_rebuild.c`'s `build_vol` | 16 | clean | `DOS(FFS)`, `adfls` walks it |
| `test_verify.c`'s `build_dirc` | 16 | clean | `DOS(FFS)`, `adfls` walks it |
| a DOS\5 DIRC floppy with content | 1760 | clean | agrees on name, size and bytes |

**The strongest check available, and it passes.** The oracle independently tells our two
16-block fixtures apart by their DOS type — `FFS` versus `FFS DIRCACHE`. If our notion of
the DOS type byte were wrong, that is what would notice.

**One assertion I had to correct rather than make pass.** `adfinfo` prints `DOS(FFS)` for
*every* DOS\5 fixture including real ones — it does not distinguish DIRC from FFS in that
output, so asserting the literal `DIRCACHE` would have asserted a string the oracle emits
for no fixture at all. The check uses `unadf`'s banner, which *does* carry `DIRCACHE`. Same
class of gap as the already-recorded `???` for `DOS\2`/`DOS\4`.

**Gate: `m30_fixtures_valid_to_the_oracle.sh` (27th).** Load-bearing proven twice: building
the DOS\5 fixture with a DOS\1 type byte makes the DIRCACHE discrimination fail, and
corrupting a header so its entry is unreachable makes `adfls` list nothing and `unadf`
extract nothing — a state our own reader accepts.


## 5. Testing strategy

- **Oracle 1:** ADFlib CLI at pinned commit `73880de7` (`adfls`, `adfinfo`, `unadf`, `adfbitmap`). Reference for listings, metadata, content and bitmap. **Compare listings and content only** — its filesystem-type *string* is `???` for `DOS\2`/`DOS\4` because `adfVolIsFsValid()` is a label/overwrite guard (`src/adf_dev.c:529`, `examples/adfformat.c:134`), not a mount rule. Record that display gap in the oracle script.
- **Oracle 2:** **amitools `0.8.1`** (`xdftool`, `rdbtool`). Preferred oracle wherever ADFlib labels a type `???` (i.e. `DOS\2`, `DOS\4`) and for **all RDB work**. Use it when the two oracles disagree. Record any disagreement; **never auto-accept** it.
- **Oracle 3:** unit golden vectors (checksums, hash and INTL fold, Latin-1, protection bits, dates, softlink paths).
- **Cross-check, not an oracle:** AROS `afs`, for checksum, hash and `capitalch` only (C5).
- **Rule:** every read path must match the oracle on every fixture before it counts as done. Where an oracle is known to be weak (ADFlib RDB is experimental upstream), a disagreement is a **recorded finding**, not a pass.
- **Rule (M24):** an oracle's *silence* is not agreement, and its stderr is not a damage signal. Before trusting an oracle to refute a change, measure what it does on the **pristine fixture** — `unadf` emits `No such file or directory` for links and some ordinary entries alike, resolves links to their targets, and exits 0 (see M24). An oracle that skips a construct silently looks like evidence of breakage; pin that limit in a gate (`m24_unadf_link_blindness.sh`) rather than rediscovering it.
- **Rule (M26):** re-measure an inherited claim before building on it. The plan's "extension chains are not implemented, needed above ~50 MB" was wrong in both halves — 600 KB files round-trip correctly. The 312 KB wall it appeared behind was a *bitmap* bug, and the error message pointed at neither.
- **Rule (M28):** a bitmap bit has TWO coordinate spaces. `afs_bitmap_set/test` address an ASSEMBLED map at offset 0; a raw bitmap PAGE carries its own checksum long in bytes 0..3, so map data starts at `+4` (`afs_bitmap_read` assembles from `blk + 4`). Mixing them writes bits into the checksum word and produces a map no reader agrees with -- seen three times now: `verify.c`'s page reader, `test_verify.c`'s `bm_set()`, and the write fuzzer's fixture. When touching bitmap code, name the space in the comment.
- **Rule (M30):** ask the ORACLE about your own fixtures, not only about downloaded images. Until M30 it had never been pointed at one, and a small hand-built volume is exactly where the subtle offsets live.
- **Rule (M33):** a new check is also a test of everything underneath it. The
  orphan pass was wrong because the set of "referenced" blocks it inverts was
  incomplete -- `visit_entry` never marked file DATA blocks, which the old
  one-directional check could not reveal. And **a count that stops counting is
  worse than no count**: `afs verify` capped its report at 256, which made a
  gate pass a mutant. **Assert on quantities your tool computes without
  truncation**, and **re-measure every number a change invalidates** -- completing
  the walk moved `testofs` from 364 to 303.
- **Rule (M32):** measure the comment, and reach for a round trip. Unlinking a
  link leaked its block while both call sites said in plain words that it freed
  it. No gate saw it: `afs verify` never consults the bitmap, `--strict` checks
  only the other direction, and nothing detects "marked allocated, referenced by
  nothing". **A leak is the one defect no structural check can catch** — every
  pointer still resolves and the volume verifies; only the COUNT is wrong. Also:
  **ask what an oracle can witness before gating on it** — amitools cannot even
  list a directory containing a soft link, and ADFlib's refusal is non-atomic
  (it unhooks the name, writes the parent, and *then* rejects).
- **Rule (M31):** two things a unit test cannot reach. **(1) Do not write a constraint about a format from reasoning — read what a real implementation wrote.** The dir-cache record was skipped on the argument that `size=0` "would teach the cache this name is an empty file"; the fixture's own soft link sits in cache block 881 as `size=0 sec_type=3`, and `adfls` could not see our links until we wrote one. **(2) Two same-typed parameters in an order that contradicts every sibling and the call that names them swap silently.** `fuse_lowlevel.h`'s `.symlink` is `(req, link, parent, name)` — the reverse of `.link` and of POSIX's own word order — and the swap compiles clean, producing a well-formed volume with an entry named after the *target*. The general form: **a self-consistent wrong result is the hardest kind to catch**, so when two implementations of one spec exist and they disagree, find out which of you is wrong before assuming it is the other.
- **Rule (M30):** do not write a constraint from reasoning about a tool's limits -- MEASURE it. This record's first draft asserted ADFlib could not open a volume under a floppy and listed the 16-block fixtures as permanently oracle-blind. Untested and wrong: ADFlib infers geometry from the file and reads an 8 KiB volume as `HardFile 8.0 KBytes`. The gap was never size; it was that nobody had asked.
- **Rule (M30):** assert an oracle's AGREEMENT with a known-good reference, never a literal label. `adfinfo` prints `DOS(FFS)` for real DOS\5 fixtures too, so asserting `DIRCACHE` asserts a string the oracle emits for nothing; `unadf`'s banner is where the distinction actually appears.
- **Rule (M29):** when a bug kind appears three times, sweep every remaining site mechanically rather than stopping at the third. The fourth was in the base image of the suite that gates the **allocator**, and 164 checks had agreed about map state no reader would see.
- **Rule (M29):** a fixture and the code under test that share a *mistake* produce a self-consistently meaningless suite. The allocator read the same wrong offset the fixture wrote, so every check agreed and all were green. When a test's own comment says "the first version got this wrong", read the comment before editing the code — the knowledge is usually already in the file.
- **Rule (M29):** a static scan is code and needs the same mutation testing as a test. Three versions of this gate's scan passed straight over a planted instance; the fourth catches it. Prefer the least elegant match that actually finds the thing.
- **Rule (M28):** a harness's comment is a claim, not evidence. The write fuzzer asserted "bitmap agreement" in prose for its whole life while calling the non-strict entry point, which skips the only check it named. Read the call, or run the harness -- do not trust the sentence.
- **Rule (M28):** a grep cannot prove behaviour when the token also appears in prose. This gate's own first version grepped for `AFS_VERIFY_STRICT` and passed both mutants, because the file's comment names it. Match the CALL (across line breaks) or execute the code.
- **Rule (M27):** ask whether anything can DETECT the bug you just fixed, by making the broken state and requiring the checker to reject it. A checker blind to the defect is worse than no checker — it reports green forever. Two sites of the same bug is evidence for a third, not a licence to invent one: M27's third-site "fix" was wrong and six existing tests said so immediately.
- **Rule (M27):** when two components share an invariant, they can be wrong in the SAME direction and never contradict each other. The rebuild freed the root's cache block; the verifier could not see a freed cache block. Both looked healthy. "The rootblock is marked" is not "the rootblock's structures are marked" — the rootblock is the one directory no parent marks, so every rule that fires for children silently misses it. The "9 → 3" that started M24 compared a populated extraction tree against a differently-populated one.
- **Fixtures:** pinned by sha256 in `fixtures.lock`, with measured DOS types. ADFlib's test data carries no separate licence: downloaded at test time, never committed or shipped. The `adfformat`-generated images are ours.

---

## 6. Decisions — ALL RESOLVED 2026-10-13

Owner decisions, recorded. These are settled, not open.

- **D1 — Low-level FUSE3 API.** **Accepted:** `fuse_lowlevel.h`, for inode lifetime and readdir control. The kernel owns names; we own blocks.
- **D2 — ADFlib as a subprocess oracle only.** **Accepted.** Never linked, never vendored, never copied. M0 builds it from a pinned commit outside our tree, keeping GPL-2.0+ (CLI) and LGPL-2.1 (library) out of every build and artifact.
- **D3 — Write-ready seams.** **Accepted:** `blockio` (partition window plus a future write backend) and parsing kept separate from walking. No allocator in this plan.
- **D4 — Bitmap exposure.** **Accepted:** `/.amigafs/bitmap`, a read-only synthetic file. A leading `.` is legal on Amiga, so the reserved directory is checked against real entries at open time; on collision the mount is refused with a named reason. `statfs` must still answer free space.
- **D5 — Lookup case policy.** **Accepted: exact-match in the mount** (byte-exact on the presented UTF-8 name). **Case-insensitive matching is available through the CLI, and is the CLI's default rather than a flag.** Rationale: the kernel resolves and caches paths, so accepting `FOO` for `foo` would put two dentry names on one inode and desynchronise `ls` from what is stored.
  - **CORRECTED 2026-10-15 (M19).** This decision originally read "case-insensitive matching is available only through the CLI (`afs ls -i`)". **That flag was never implemented, and implementing it would have been wrong.** `afs_dir_lookup` compares with the same `capitalch` the hash function uses, so CLI lookup has been case-insensitive from M3 onward — the record named a flag for behaviour that already existed and needed no flag. Adding `-i` would have been a no-op, or would have required making the default exact and breaking shipped behaviour to justify the flag.
  - The promise the record actually made — *mount exact, CLI case-insensitive* — is met and is now **tested**, so the claim is earned rather than asserted: `m3_gate` runs `afs ls` on `win32-names.adf` with `/forbidden-files`, `/FORBIDDEN-FILES` and `/ForBiddEn-FiLeS` and requires all three to succeed with byte-identical output. Mutating `capitalch` to compare byte-exactly turns it red (`rc 0/1/1`).
  - This is the "a design record naming an unimplemented flag is what a later session trusts and cannot find" hazard, caught and closed rather than left in place.
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

- **INTL/DIRC exactness — CLOSED (M41).** All six DOS types round-trip
  byte-identically against `unadf`: `-t 0/1/3/5` formatted by `adfformat`, and
  **DOS\\2 (OFS/INTL) and DOS\\4 (OFS/DIRC&INTL) constructed by patching byte 3 of
  the bootblock's first long** in a `adfformat -t 0` image. The earlier record
  here said the OFS+INTL cell "cannot be closed with these oracles", which was
  true of `adfformat` and **false of the oracles**: refusing to CREATE a volume
  is not refusing to READ one. `adfinfo` and `unadf` read the constructed image
  and extract from it without complaint. DOS\\6 and DOS\\7 are refused by us
  (AmigaOS 3.2 long-filename types) and extract nothing from the oracle either --
  agreement, not a gap.
- **ADFlib RDB support is experimental:** see the M7 gate.
- **Hostile images** (`ffdisk0049.adf`, fuzzed blocks): every pointer is bounds-checked, every chain is cycle-guarded, and nothing in the core aborts. Add a libFuzzer target over `blockio` with a memory backend after M4.
- **Directory-block GID width — SETTLED AS UNSETTLEABLE HERE (M38).** The
  prescribed method was impossible: **ADFlib's `AdfDirBlock` has no `uid` or
  `gid` field at all** (`src/adf_blk.h:266`), **amitools' `UserDirBlock` does
  not read them either** (zero `uid` hits across all of `amitools/fs/`), and
  `xdftool` has no such command and no MultiUser `format` option. Neither
  pinned oracle carries the field, and no tool here can make an image that
  would. A dump could not settle it either: a fresh MultiUser directory has
  `GID == 0`, and four zero bytes and two zero bytes are the same sixteen
  bytes. What IS settled is the decision -- the reader keeps `be16` for both
  (`src/lib/dir.c:1009-1010`), the conservative reading under either
  hypothesis. `-o amiga_ids` is **LIVE** -- corrected by M43, it IS offered;
  see that section. Settling the fact needs the
  MultiUser filesystem handler itself, or a genuine MultiUser-formatted
  image. Do not re-attempt the cross-check: it is empty by construction.

- **Large volumes:** a 32-bit block count with `BSIZE` up to 32 KiB stays in range. Refuse `ffstd64` explicitly.
