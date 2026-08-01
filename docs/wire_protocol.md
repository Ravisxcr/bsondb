# BSON wire protocol (bsondb subset)

This document describes the on-wire binary format `bsondb` reads
and writes. It is a subset of the standard [BSON spec](http://bsonspec.org/spec.html);
the type table below states exactly which parts are implemented.

The authoritative source for every constant in this document is
`include/custom_bson/wire_spec.h` -- keep the two in sync.

## Byte order

All multi-byte integers and floats are **little-endian** on the wire.

## Document layout

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

**Note on keys:** this BSON layer places no restriction on element
keys beyond "valid UTF-8 with no embedded NUL byte" -- keys containing
`.` or a leading `$` are accepted and round-trip normally. MongoDB's
*query language* restricts such keys, but that is a concern for a
future `Collection`/query layer, not this serialization layer.

## Type table

| Type | Byte | Value layout | Status |
|---|---|---|---|
| Double | `0x01` | 8 bytes, IEEE-754 | Implemented |
| String | `0x02` | `int32 (byte_len+1)` + UTF-8 bytes + `0x00` | Implemented |
| Document | `0x03` | nested `document` | Implemented |
| Array | `0x04` | nested `document` (index-string keys) | Implemented |
| Binary | `0x05` | `int32 len` + `byte subtype` + `len` bytes | Implemented (generic subtype `0x00` only; see limitation below) |
| Undefined | `0x06` | (none) | Deferred -- deprecated in the BSON spec |
| ObjectId | `0x07` | 12 bytes | Implemented |
| Boolean | `0x08` | 1 byte (`0x00`/`0x01`) | Implemented |
| UTC datetime | `0x09` | `int64` milliseconds since Unix epoch | Implemented |
| Null | `0x0A` | (none) | Implemented |
| Regex | `0x0B` | two cstrings (pattern, options) | Deferred -- needs a Python wrapper type |
| DBPointer | `0x0C` | string + 12 bytes | Deferred -- deprecated in the BSON spec |
| JavaScript code | `0x0D` | string | Deferred -- deprecated in the BSON spec |
| Symbol | `0x0E` | string | Deferred -- deprecated in the BSON spec |
| JavaScript code w/ scope | `0x0F` | `int32` + string + document | Deferred -- deprecated in the BSON spec |
| Int32 | `0x10` | 4 bytes | Implemented |
| Timestamp | `0x11` | two `uint32` | Deferred -- needs a Python wrapper type |
| Int64 | `0x12` | 8 bytes | Implemented |
| Decimal128 | `0x13` | 16 bytes | Deferred -- needs a Python wrapper type |
| MinKey | `0xFF` | (none) | Deferred -- needs a Python wrapper type |
| MaxKey | `0x7F` | (none) | Deferred -- needs a Python wrapper type |

**Deferred type behavior:** encountering a deferred-but-structurally-valid
type byte anywhere in a document causes the *entire* decode to fail with
`bsondb.exceptions.BSONNotImplementedError` -- there is no partial
decode. This is distinct from `InvalidBSON`, which means the bytes are
actually corrupt/malformed, not just a type this version doesn't handle
yet.

**Known limitation -- binary subtype:** decode discards the subtype byte
it reads and always presents `bytes` to Python; encode always writes
subtype `0x00` (generic). This means `encode(decode(x))` is not
byte-identical for binary values with a non-generic subtype (e.g. the
legacy UUID subtype `0x03`). Acceptable for the "round-trip common
Python types" bar this version targets; a future `Binary` wrapper type
would resolve it.

## Limits

- `BSON_MAX_DOCUMENT_SIZE` = 16 MiB (matches MongoDB's own default limit).
  Both encode and decode enforce this.
- `BSON_MAX_DEPTH` = 100 levels of document/array nesting. Enforced on
  both encode (Python object recursion) and decode (buffer recursion),
  to bound C stack usage against malicious or accidental deep nesting.

## Python type mapping

See `docs/api_reference.md` for the full Python <-> BSON type mapping table.

## Storage and index file formats

This document covers only the BSON document format itself. Two more
on-disk formats are layered on top of it, each fully documented in its
own header (the authoritative source, kept in sync with the code):

- **Collection data files** (`<collection>.cbd`): a 64-byte header
  (magic, version, DIRTY flag for crash recovery, append cursor,
  live/total record counters) followed by sequential
  `status_byte + BSON document` records. See
  `include/custom_bson/storage.h`.
- **B-Tree index files** (`<collection>.<index_name>.bidx`): a
  256-byte header (magic, version, unique/descending flags, root page,
  page count, indexed field path) followed by 4096-byte B+Tree pages
  (leaves chained for ordered scans, tombstone-only delete). See
  `include/custom_bson/btree.h`.
