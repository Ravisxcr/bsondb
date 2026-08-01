# BSON wire protocol (bsondb subset)

This document describes every on-disk binary format `bsondb` reads and
writes: the BSON document format itself, and the two custom file
formats layered on top of it (collection data files, B-Tree index
files).

## Table of contents

- [Scope: spec vs. custom](#scope-spec-vs-custom)
- [Byte order](#byte-order)
- [Document layout](#document-layout)
- [Type table](#type-table)
- [Deferred-type decode behavior](#deferred-type-decode-behavior)
- [Known limitation — binary subtype](#known-limitation--binary-subtype)
- [Limits](#limits)
- [Python type mapping](#python-type-mapping)
- [C engine implementation](#c-engine-implementation)
  - [Reader / iterator / writer](#reader--iterator--writer)
  - [Error codes](#error-codes)
- [Collection data file format (custom)](#collection-data-file-format-custom)
- [B-Tree index file format (custom)](#b-tree-index-file-format-custom)
- [Source-of-truth files](#source-of-truth-files)
- [External references](#external-references)

## Scope: spec vs. custom

`bsondb` combines one external specification with two file formats
that are entirely `bsondb`'s own design. Keep these apart when
reading this document:

| Layer | Origin | Covered in |
| --- | --- | --- |
| BSON document encoding (types, byte layout) | **External** — a subset of the official [bsonspec.org](http://bsonspec.org/spec.html) spec | [Document layout](#document-layout), [Type table](#type-table) |
| `bson_reader_t` / `bson_iter_t` / `bson_writer_t` engine | **Custom** — `bsondb`'s own C11 implementation of a reader/writer for the spec above (no third-party BSON library is used) | [C engine implementation](#c-engine-implementation) |
| Collection data file (`.cbd`) | **Custom** — `bsondb`-specific, not part of BSON or any MongoDB on-disk format | [Collection data file format](#collection-data-file-format-custom) |
| B-Tree index file (`.bidx`) | **Custom** — `bsondb`-specific, loosely inspired by classic B+Tree design (not MongoDB's WiredTiger format) | [B-Tree index file format](#b-tree-index-file-format-custom) |

## Byte order

All multi-byte integers and floats are **little-endian** on the wire,
per the BSON spec. See [C engine implementation](#c-engine-implementation)
for how `bsondb` handles this portably.

## Document layout

This grammar is the standard BSON document/element/cstring layout
defined by the spec:

```
document ::= int32 total_length
             element*
             0x00                  ; EOO (end-of-object) byte

element  ::= byte     type
             cstring  key          ; UTF-8, NUL-terminated, no embedded NUL
             <value>                ; layout depends on `type`, see table below

cstring  ::= (byte*) 0x00           ; UTF-8 bytes, NUL-terminated
```

`total_length` counts every byte of the document, including the 4-byte
length field itself and the trailing EOO byte. A document is therefore
never shorter than 5 bytes (`BSON_DOC_MIN_LEN`).

Arrays use exactly the same `document` layout; the "keys" are the
ASCII decimal string of each element's index (`"0"`, `"1"`, ...), in
order.

> **Note on keys:** this BSON layer places no restriction on element
> keys beyond "valid UTF-8 with no embedded NUL byte" — keys containing
> `.` or a leading `$` are accepted and round-trip normally. MongoDB's
> *query language* restricts such keys, but that is a concern for the
> `Collection`/query layer (`python/bsondb/query.py`), not this
> serialization layer.

## Type table

| Type | Byte | Value layout | Status |
| --- | --- | --- | --- |
| Double | `0x01` | 8 bytes, IEEE-754 | Implemented |
| String | `0x02` | `int32 (byte_len+1)` + UTF-8 bytes + `0x00` | Implemented |
| Document | `0x03` | nested `document` | Implemented |
| Array | `0x04` | nested `document` (index-string keys) | Implemented |
| Binary | `0x05` | `int32 len` + `byte subtype` + `len` bytes | Implemented (generic subtype `0x00` only; see [limitation](#known-limitation--binary-subtype)) |
| Undefined | `0x06` | (none) | Deferred — deprecated in the BSON spec |
| ObjectId | `0x07` | 12 bytes | Implemented |
| Boolean | `0x08` | 1 byte (`0x00`/`0x01`) | Implemented |
| UTC datetime | `0x09` | `int64` milliseconds since Unix epoch | Implemented |
| Null | `0x0A` | (none) | Implemented |
| Regex | `0x0B` | two cstrings (pattern, options) | Deferred — needs a Python wrapper type |
| DBPointer | `0x0C` | string + 12 bytes | Deferred — deprecated in the BSON spec |
| JavaScript code | `0x0D` | string | Deferred — deprecated in the BSON spec |
| Symbol | `0x0E` | string | Deferred — deprecated in the BSON spec |
| JavaScript code w/ scope | `0x0F` | `int32` + string + document | Deferred — deprecated in the BSON spec |
| Int32 | `0x10` | 4 bytes | Implemented |
| Timestamp | `0x11` | two `uint32` | Deferred — needs a Python wrapper type |
| Int64 | `0x12` | 8 bytes | Implemented |
| Decimal128 | `0x13` | 16 bytes | Deferred — needs a Python wrapper type |
| MinKey | `0xFF` | (none) | Deferred — needs a Python wrapper type |
| MaxKey | `0x7F` | (none) | Deferred — needs a Python wrapper type |

Type bytes and layouts above are exactly as defined by the BSON spec;
`bsondb` implements a subset (all "core" scalar/container types) and
defers the rest. See `include/custom_bson/wire_spec.h` for the
authoritative `#define` for every byte value.

### Binary (`0x05`) subtypes

| Subtype | Byte | bsondb support |
| --- | --- | --- |
| Generic | `0x00` | Round-tripped |
| Function | `0x01` | Accepted on decode, not distinguished from generic |
| Binary (old) | `0x02` | Accepted on decode, not distinguished from generic |
| UUID (old) | `0x03` | Accepted on decode, not distinguished from generic |
| UUID | `0x04` | Accepted on decode, not distinguished from generic |
| MD5 | `0x05` | Accepted on decode, not distinguished from generic |
| User-defined | `0x80` | Accepted on decode, not distinguished from generic |

## Deferred-type decode behavior

Encountering a deferred-but-structurally-valid type byte anywhere in a
document causes the *entire* decode to fail with
`bsondb.exceptions.BSONNotImplementedError` — there is no partial
decode. This is distinct from `InvalidBSON`, which means the bytes are
actually corrupt/malformed, not just a type this version doesn't
handle yet.

## Known limitation — binary subtype

Decode discards the subtype byte it reads and always presents `bytes`
to Python; encode always writes subtype `0x00` (generic). This means
`encode(decode(x))` is not byte-identical for binary values with a
non-generic subtype (e.g. the legacy UUID subtype `0x03`). Acceptable
for the "round-trip common Python types" bar this version targets; a
future `Binary` wrapper type would resolve it.

## Limits

| Constant | Value | Purpose |
| --- | --- | --- |
| `BSON_MAX_DOCUMENT_SIZE` | 16 MiB | Matches MongoDB's own default limit. Enforced on both encode and decode. |
| `BSON_MAX_DEPTH` | 100 levels | Bounds document/array nesting on both encode (Python object recursion) and decode (buffer recursion), to bound C stack usage against malicious or accidental deep nesting. |

## Python type mapping

See [`docs/api_reference.md`](api_reference.md) for the full Python
&harr; BSON type mapping table.

## C engine implementation

Everything in this section is `bsondb`-specific: there is no
third-party BSON library involved anywhere in the stack. The engine
lives in `src/c_engine/bson_engine.c` / `include/custom_bson/bson_engine.h`,
is pure C11, and has **zero dependency on `Python.h`** — all Python
object traversal lives in `src/python_bindings/_bson_core.c`, the only
file permitted to include `Python.h` and call into this API.

### Reader / iterator / writer

| Component | Role |
| --- | --- |
| `bson_reader_t` | A bounds-checked cursor over a *borrowed* buffer (never owns it — the caller, e.g. the Python bindings or an mmap-backed scanner, keeps the underlying memory alive). Provides primitive reads (`u8`/`i32`/`i64`/`double`/raw bytes/bounded cstring) plus UTF-8 validation. |
| `bson_iter_t` | A structural walk over one document/array level, built on a `bson_reader_t`. Exposes the current element's type, key, and typed value accessors (`bson_iter_value_int32`, `..._objectid`, etc). |
| `bson_writer_t` | A growable heap buffer with backpatched document lengths (`bson_writer_begin_document` writes a placeholder length, `bson_writer_end_document` backfills it once the nested document's true length is known). Ownership transfers to the caller via `bson_writer_release()`. |
| `bson_validate_document()` | A single pure-buffer validation pass that recursively confirms every length/type-byte/cstring/UTF-8/depth invariant *without* constructing any output. |

**Why validation is a separate pass:** for inputs over ~4KB,
`_bson_core.c` runs this validation pass with the **GIL released**
(it touches only the raw buffer, no Python objects), so a large
`decode()` call doesn't block other Python threads for its full
duration. A subsequent `bson_iter_t` walk over the same buffer is then
guaranteed not to hit a bounds error. `encode()` holds the GIL
throughout, since its recursive walk touches Python objects at
effectively every step — there's no extended pure-buffer phase to
release around, unlike decode's validation pass.

**Byte order handling:** `wire_spec.h` routes every multi-byte field
through `bson_le32_to_host()` / `bson_le64_to_host()` helpers. On all
platforms this project targets today (x86/x86_64/ARM in their default
modes, all little-endian), these compile down to no-ops; they exist so
the codebase stays technically portable to a big-endian host without a
redesign.

### Error codes

`bson_status_t` (`include/custom_bson/bson_engine.h`) is one shared
enum across the BSON codec *and* the storage/index engines described
below, paired with a fixed-size `bson_error_t` (no heap allocation on
the error path).

| Code | Meaning |
| --- | --- |
| `BSON_ERR_TRUNCATED_BUFFER` | Buffer ends before a declared length/field is satisfied |
| `BSON_ERR_INVALID_LENGTH` | A length prefix is inconsistent with the buffer |
| `BSON_ERR_INVALID_TYPE_BYTE` | An element's type byte isn't a recognized BSON type |
| `BSON_ERR_UNTERMINATED_STRING` | A cstring has no NUL terminator within its bound |
| `BSON_ERR_INVALID_UTF8` | A string/key's bytes aren't valid UTF-8 |
| `BSON_ERR_TRAILING_BYTES` | Extra bytes exist after a document's EOO byte |
| `BSON_ERR_MISSING_EOO` | A document doesn't end with the `0x00` terminator |
| `BSON_ERR_INTEGER_OVERFLOW` | A Python `int` doesn't fit in a signed 64-bit BSON int |
| `BSON_ERR_MAX_DEPTH_EXCEEDED` | Nesting exceeds `BSON_MAX_DEPTH` (100) |
| `BSON_ERR_UNSUPPORTED_TYPE` | Structurally valid but deferred BSON type (decode) |
| `BSON_ERR_UNSUPPORTED_PYTHON_TYPE` | A Python value has no BSON representation (encode) |
| `BSON_ERR_INVALID_KEY` | A dict key isn't `str`, or contains an embedded NUL |
| `BSON_ERR_DOCUMENT_TOO_LARGE` | Encoded size would exceed `BSON_MAX_DOCUMENT_SIZE` (16 MiB) |
| `BSON_ERR_VALUE_OUT_OF_RANGE` | A value is out of range for its target wire encoding |
| `BSON_ERR_OUT_OF_MEMORY` | Allocation failure |
| `BSON_ERR_IO` | open/read/write/ftruncate/mmap syscall failure (storage engine) |
| `BSON_ERR_INVALID_FILE_HEADER` | Bad magic/version on a `.cbd` or `.bidx` file |
| `BSON_ERR_RECORD_NOT_FOUND` | Offset doesn't reference a live record |
| `BSON_ERR_INDEX_UNSUPPORTED` | Compound keys, or a non-fixed-width key type (e.g. `str`) |
| `BSON_ERR_DUPLICATE_KEY` | Unique index violation |
| `BSON_ERR_INVALID_UPDATE_DOCUMENT` | Update doc without `$` operators, or mixed operator+literal |
| `BSON_ERR_READ_ONLY_VIOLATION` | Resize/flush/insert attempted on a `READ_ONLY` mmap |

Each Python-visible `bsondb.exceptions` class maps onto one or more of
these codes — see [`docs/api_reference.md`](api_reference.md#bsondbexceptions).

## Collection data file format (custom)

**This format is entirely `bsondb`'s own design** — it is not part of
the BSON spec and does not match MongoDB's own storage engine
(WiredTiger) or file layout. One append-only data file (`<collection>.cbd`)
exists per collection, memory-mapped via `include/custom_bson/platform_mmap.h`.

```
[0, 64)    fixed header
[64, ...)  records, packed back to back:
             status_byte(1) + BSON document bytes(N)
           where N is the document's own leading int32 length prefix
           (no separate redundant length field is stored)
```

### Header (64 bytes, little-endian)

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | `magic` | `"CBD1"` |
| 4 | 2 | `version` | `u16`, currently `1` |
| 6 | 2 | `flags` | `u16`; bit 0 = `DIRTY` |
| 8 | 4 | `header_len` | `u32`, `= 64` |
| 12 | 4 | *reserved* | |
| 16 | 8 | `data_end` | `u64` — authoritative append cursor (the *logical* length; the mmap'd capacity is pre-grown ahead of this, doubling on full and rounding to a page, so appends don't resize on every call) |
| 24 | 8 | `live_count` | `u64` — approximate, maintained incrementally |
| 32 | 8 | `total_count` | `u64` — records ever appended (live + tombstoned) |
| 40 | 4 | `next_index_ordinal` | `u32` |
| 44 | 20 | *reserved* | |

### Record status bytes

| Value | Name | Meaning |
| --- | --- | --- |
| `0x00` | `UNUSED` | Never written (only appears past `data_end`, in pre-grown but unwritten mmap space) |
| `0x01` | `LIVE` | An active, queryable record |
| `0x02` | `TOMBSTONE` | A deleted/superseded record — space is not reclaimed until `compact()` |

There is no in-place update: every mutation tombstones the old record
and appends a new one.

### Crash recovery

Opening an existing file for read-write with `flags.DIRTY` set (i.e.
the previous session didn't close cleanly) triggers
`bson_storage_recover()` before the handle is returned:

1. Walk records sequentially from the header forward.
2. Validate each via `bson_validate_document()`.
3. Stop at the first invalid/truncated record.
4. Rewrite `data_end`/`live_count`/`total_count` to the recovered
   values — a **logical** truncation (no physical file truncation, no
   write-ahead log).

`flags.DIRTY` is set on every successful open in read-write mode, and
cleared only on a clean `close()`, so an unclean process exit is
always detectable on the next open.

## B-Tree index file format (custom)

**This format is entirely `bsondb`'s own design** — a from-scratch
B+Tree, not an adaptation of MongoDB's WiredTiger index format or any
third-party library. One index file (`<collection>.<index_name>.bidx`)
exists per (collection, field). Data lives only in leaf pages, which
are chained via a next-leaf pointer for ordered scans. **Single-field
indexes only** — no compound keys this slice.

File layout: page 0 is a 256-byte header padded to a full page; real
pages are numbered `1..page_count`, each `BSON_BTREE_PAGE_SIZE` (4096)
bytes, at file offset `page_no * 4096`.

### Header (256 bytes, little-endian)

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | `magic` | `"CBI1"` |
| 4 | 2 | `version` | `u16`, currently `1` |
| 6 | 2 | `flags` | `u16`; bit 1 = `UNIQUE`, bit 2 = `DESCENDING` |
| 8 | 4 | `header_len` | `u32`, `= 256` |
| 12 | 4 | `page_size` | `u32`, `= 4096` |
| 16 | 8 | `root_page_no` | `u64` |
| 24 | 8 | `page_count` | `u64` — pages in use (`1..page_count`) |
| 32 | 8 | `free_page_head` | `u64` — always `0` this slice; no page reclamation (delete is tombstone-only; a future reindex paired with `Collection.compact()` is the only reclamation path) |
| 40 | 1 | `key_type_tag` | one of the key-tag values below |
| 41 | 1 | `field_path_len` | |
| 42 | 14 | *reserved* | |
| 56 | 200 | `field_path` | UTF-8, NUL-padded — makes the index file fully self-describing, so `list_indexes()` needs no external metadata store |

### Page layout

Both leaf and internal pages share a 16-byte page header:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `page_type` (`1` = leaf, `2` = internal) |
| 1 | 1 | `flags` (reserved) |
| 2 | 2 | `count` — entry count (leaf) / key count (internal) |
| 4 | 4 | *reserved* |
| 8 | 8 | `link` — `next_leaf_page_no` (leaf) or leftmost-child page number (internal) |
| 16 | — | entries start |

| Page type | Entry format | Size | Notes |
| --- | --- | --- | --- |
| Leaf | `key[13]` + `record_offset (u64)` + `entry_flags (u8, bit0 = DEAD)` | 22 bytes | Duplicates (non-unique indexes) are adjacent equal-key entries, kept contiguous by insertion order |
| Internal | `key[13]` + `child_page_no (u64)` | 21 bytes | Paired with the page header's `link` as the 0th (leftmost) child; `child_i` covers keys in `[key_{i-1}, key_i)`, the leftmost child covers keys `< key_0` |

Delete is tombstone-only (the entry's `DEAD` bit) — no merge/rebalance.
Entry count only grows until `Collection.compact()` rebuilds every
index from scratch, which is also what reclaims the tombstoned space.

### Key encoding (13 bytes: 1 tag byte + 12 payload bytes)

| Tag | Value | BSON source type(s) |
| --- | --- | --- |
| `BSON_BTREE_TAG_NULL` | 0 | `None`, or a missing field |
| `BSON_BTREE_TAG_BOOL` | 1 | `bool` |
| `BSON_BTREE_TAG_INT` | 2 | Int32 *and* Int64 (always widened to a little-endian `int64` payload, so a field mixing both BSON integer subtypes still compares purely by numeric value) |
| `BSON_BTREE_TAG_DOUBLE` | 3 | `float` (not `NaN`) |
| `BSON_BTREE_TAG_DATETIME` | 4 | `datetime.datetime` |
| `BSON_BTREE_TAG_OBJECTID` | 5 | `ObjectId` |

> **Important — not a cross-type total order.** The tag order above
> keeps the tree internally well-ordered and supports exact-key
> equality lookup; it is **not** a cross-type numeric total order (an
> `INT`-tagged key always compares less than a `DOUBLE`-tagged key,
> regardless of value). For this reason:
>
> - `bson_btree_lookup()` (equality) is always safe — it only ever
>   compares same-tagged keys.
> - `bson_btree_range()` exists for completeness/testability, but the
>   Python query planner never routes `$gt`/`$gte`/`$lt`/`$lte`
>   through it, since a range scan on a field mixing e.g. `int` and
>   `float` values could silently skip qualifying entries. Range
>   filters always fall back to a full collection scan.
> - `str`, `bytes`, `list`, and `dict` values have no fixed-width key
>   encoding and raise `BSONNotImplementedError` during
>   `create_index()`'s build scan (the partial index file is deleted
>   before the error propagates — all-or-nothing).

## Source-of-truth files

This document is kept in sync with these files by convention — when
in doubt, the code wins:

| File | Defines |
| --- | --- |
| `include/custom_bson/wire_spec.h` | BSON type byte values, binary subtypes, structural constants, byte-order helpers |
| `include/custom_bson/bson_engine.h` | Reader/iterator/writer API, `bson_status_t` error codes |
| `include/custom_bson/storage.h` | Collection data file (`.cbd`) header/record layout |
| `include/custom_bson/btree.h` | B-Tree index file (`.bidx`) header/page/key layout |

## External references

- [bsonspec.org](http://bsonspec.org/spec.html) — the official BSON
  specification that `document`/`element`/type-byte layout in this
  document is a subset of.
- [MongoDB BSON types](https://www.mongodb.com/docs/manual/reference/bson-types/) —
  background on the deferred types (Regex, Timestamp, Decimal128,
  MinKey/MaxKey) and binary subtypes referenced above.
- [`docs/api_reference.md`](api_reference.md) — the Python-facing API
  and Python &harr; BSON type mapping built on top of this wire format.
- [`docs/development.md`](development.md) — build/setup instructions
  for working on the C engine described here.
