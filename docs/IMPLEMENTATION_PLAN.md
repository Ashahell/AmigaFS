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

- **INTL/DIRC exactness:** the FAQ is thin here. Generate `-t 2..5` images, diff against the oracle, and record any oracle disagreement.
- **ADFlib RDB support is experimental:** see the M7 gate.
- **Hostile images** (`ffdisk0049.adf`, fuzzed blocks): every pointer is bounds-checked, every chain is cycle-guarded, and nothing in the core aborts. Add a libFuzzer target over `blockio` with a memory backend after M4.
- **Directory-block GID width (FAQ inconsistency):** the FAQ lists the file header's `GID` as `ushort` at `BSIZE-194`. The directory block lists `UID` as `ushort` at `BSIZE-196` but `GID` as `ulong` at `BSIZE-194`, a 4-byte field overlapping the 2-byte one before it. This is a spec error, not an established fact. Until it is settled, `-o amiga_ids` (D7) exposes directory GID as the 16-bit value and records the question. Settle it by hex-dumping a directory block from a MultiUser-formatted image, cross-checked against ADFlib's `bDirBlock` and amitools' dir-block layout. It was scheduled for M2, and M2 closed without settling it. It is now a **blocker for shipping `-o amiga_ids` in M8**: until it is closed, `amiga_ids` is not offered.
- **Large volumes:** a 32-bit block count with `BSIZE` up to 32 KiB stays in range. Refuse `ffstd64` explicitly.
