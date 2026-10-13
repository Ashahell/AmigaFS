# AmigaDOS comment layout — verified, not recalled

Every offset here was confirmed against **amitools 0.8.1's own parsers**
(`EntryBlock._read_nac_modts`, `DirCacheRecord.get`) driven over the real
fixture bytes, and against the bytes themselves. The oracle that does this is
`tests/oracle/comment_oracle.py`; it is the gate's source of truth.

## Why this needed verifying

The comments in question are the ones an entry carries, and there are two
plausible places to look. The AmigaDOS FAQ available to this project contradicts
itself about field widths in this corner of the format, so the layout was taken
from a reference implementation and checked byte-for-byte rather than inferred.

## amitools' offset convention, and why it lines up with ours

amitools reads fields with `_get_long(-N)` / `_get_bstr(-N, max)`, and those
are **plain absolute offsets `bsize - 4*N`** — nothing is subtracted for the
checksum long. That is why its numbers line up with this project's on the
fields we already trust:

| field | amitools | absolute offset (512-byte block) | ours |
|---|---|---|---|
| rootblock bitmap flag | `_get_long(-50)` | 312 | `bsize - 200` |
| rootblock bitmap pages | `_get_long(-49 + i)` | 316 | `bsize - 196 + 4i` |
| rootblock bitmap extension | `_get_long(-24)` | 416 | `bsize - 96` |
| rootblock name | `_get_bstr(-20, 30)` | **432** | `bsize - 80` |
| rootblock extension / dir cache | `_get_long(-2)` | **504** | `bsize - 8` |

The name agreeing at 432 is worth stating plainly: it is the offset this project
already had to get right (the name is at `bsize-80`, **not** `bsize-79`), and
the reference implementation puts it in the same place.

## 1. The comment in the entry block — every volume

```
offset bsize-184   : comment length (1 byte, BCPL)
offset bsize-183   : comment data (up to 79 bytes)
```

A length of 0 means no comment. This holds on **both** DIRC and non-DIRC
volumes, for files, directories, and both hard and soft links.

Verified against amitools on six entry blocks spanning three volumes, two of
them DIRC and one deliberately damaged:

| image | block | kind | comment |
|---|---|---|---|
| `testofs.adf` | 884 | file | `comment of MOON.GIF` |
| `g1a30c.adf` | 971 | file | `A Hellraisers Lightning-Text production!!` |
| `testhd.adf` | 1772 | file | `protracker module` |
| `testffs.adf` | 886 | file | `protracker module` |
| `testffs.adf` | 1142 | dir | `hlink_dir1 comment` |
| `testffs.adf` | 1144 | dir | *(none)* |

All six matched amitools exactly.

## 2. The redundant copy in the dir cache — DIRC volumes only

On a DIRC volume the dir cache holds a **second copy** of every name and
comment. That redundancy is the point: per decision C4 the hash table stays the
authority for lookup, so a stale or damaged cache cannot be the only place a
comment survives.

The dir-cache pointer is the rootblock's **extension long at `bsize-8`**, not
offset 24. Offset 24 is the hash table and reads as 0 on a DIRC volume, which
is indistinguishable from "this volume has no dir cache" — a mistake that costs
an hour if you make it.

```
dir-cache block:
  +0   checksum
  +4   own_key
  +8   parent
  +12  num_records
  +16  next_cache
  +24  first record

record (variable length, records are packed end to end):
  +0                entry block pointer (4, big-endian)
  +4                size (4)
  +8                protect (4)
  +12               uid (2)
  +14               gid (2)
  +16               days (2, SIGNED)
  +18               mins (2, SIGNED)
  +20               ticks (2, SIGNED)
  +22               sub_type (1)
  +23               name_len (1)
  +24               name (name_len bytes)
  +24+name_len      comment_len (1)
  +25+name_len      comment (comment_len bytes)

  record size = (25 + name_len + comment_len + 1) & ~1
```

Records are **variable length**, so a reader must walk them using the size
formula; `num_records` says how many there are but not how big they are.

Worked example, block 881 of `testffs.adf`, checked against the raw bytes:

```
record at offset 230:  name_len = 19  ("mod.And.DistantCall")
                       comment_len byte at 230+24+19 = 273  = 17
                       comment at 230+25+19 = 274 .. 290  = "protracker module"
                       size = (25 + 19 + 17 + 1) & ~1 = 62
next record starts at 230 + 62 = 292   <-- amitools says 292
```

Every record boundary in the chain was checked this way and all of them match,
including the two that carry a comment and the ten that do not.

## 3. The two copies agree

Where both exist, the entry-block comment and the dir-cache comment are
identical:

| entry | entry block | dir cache |
|---|---|---|
| `mod.And.DistantCall` | 886 | record at 230 |
| `dir_1` | 1142 | record at 292 |
| (`testhd.adf` "protracker module") | 1772 | record at 58 |

That agreement is worth a gate of its own: a reader that gets the offset wrong
in one place and right in the other will disagree, and disagreement between two
copies of the same field is a far sharper signal than either copy alone.

## Still unknown

- The **dir-cache block count** amitools reports for `g1a30c.adf`
  ("Bitmap Block Count Mismatch(15)") is not explained here; that volume is
  refused for other reasons and its cache is not read.
- `DOS\6`/`DOS\7` long-name entries use a different, combined name-and-comment
  field at `bsize-46`. Those volumes are **refused by name** (decision C1), so
  the path is documented from the reference implementation but **never
  exercised**, and no fixture reaches it.
