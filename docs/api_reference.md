# API reference

## `jsondb.encode(document: dict) -> bytes`

Serializes a Python `dict` to BSON bytes. Raises:

- `TypeError` -- `document` is not a `dict`.
- `jsondb.InvalidDocument` -- a key is not `str` (or contains an
  embedded NUL byte), or a value has no BSON representation (see the
  type table below).
- `OverflowError` -- an `int` value doesn't fit in a signed 64-bit BSON int.
- `jsondb.DocumentTooLarge` -- the encoded document would exceed 16 MiB.
- `MemoryError` -- allocation failure.

## `jsondb.decode(data: bytes) -> dict`

Deserializes BSON bytes back to a Python `dict`. Accepts anything
implementing the buffer protocol (`bytes`, `bytearray`, `memoryview`).
Raises:

- `jsondb.InvalidBSON` -- the bytes are truncated, malformed, or
  otherwise corrupt (bad length field, invalid type byte, unterminated
  string, invalid UTF-8, trailing bytes after the document, missing
  end-of-object byte, or nesting deeper than 100 levels).
- `jsondb.BSONNotImplementedError` -- the document contains a
  structurally valid BSON type this version doesn't support yet
  (Regex, Timestamp, Decimal128, MinKey/MaxKey, or a deprecated type).
- `MemoryError` -- allocation failure.

For inputs over ~4KB, the initial validation pass runs with the GIL
released (see `src/python_bindings/_bson_core.c`), so a large `decode()`
call doesn't block other Python threads for its full duration. `encode()`
holds the GIL throughout, since its recursive walk touches Python
objects at effectively every step -- there's no extended pure-buffer
phase to release around, unlike decode's validation pass.

## `jsondb.ObjectId`

A 12-byte unique identifier, matching MongoDB/BSON's ObjectId format.
Pure Python (`python/jsondb/object_id.py`) -- see the module
docstring for why this isn't implemented in C.

```python
ObjectId()                     # generates a new id
ObjectId("507f1f77bcf86cd799439011")   # from a 24-char hex string
ObjectId(b"...")               # from 12 raw bytes

oid.binary          # -> bytes, the 12 raw bytes
oid.generation_time  # -> datetime.datetime (UTC) embedded in the id
str(oid)             # -> 24-char hex string
```

Supports equality, hashing, and ordering (`<`, `<=`, `>`, `>=`) by raw
byte value.

## `jsondb.exceptions`

```
BSONError
├── InvalidBSON               # malformed/corrupt wire data (decode)
├── InvalidDocument            # unencodable Python object graph (encode)
├── BSONNotImplementedError    # valid but unsupported BSON type (decode)
└── DocumentTooLarge            # encoded size exceeds the 16 MiB limit
```

## Python <-> BSON type mapping

| Python type | BSON type | Notes |
|---|---|---|
| `dict` | Document | keys must be `str` with no embedded NUL |
| `list`, `tuple` | Array | decode always returns `list` |
| `str` | String | must be valid Unicode |
| `bytes`, `bytearray` | Binary (subtype `0x00`) | decode always returns `bytes` |
| `bool` | Boolean | checked before `int` (`bool` is an `int` subclass in Python) |
| `int` | Int32 or Int64 | chosen by magnitude; `OverflowError` if it doesn't fit in 64 bits |
| `float` | Double | raw IEEE-754 bits preserved, including `nan`/`inf` |
| `None` | Null | |
| `jsondb.ObjectId` | ObjectId | |
| `datetime.datetime` | UTC datetime | naive datetimes are treated as already-UTC; decode always returns naive datetimes |

## `jsondb.JsonDBClient` / `Database` / `Collection`

```python
client = jsondb.JsonDBClient("./data")   # local data directory, not a network connection; creates it if missing
db = client.mydb                              # -> Database (a subdirectory of client.path)
coll = db.mycollection                        # -> Collection (one <name>.cbd mmap'd data file)
```

`JsonDBClient(path)` creates `path` eagerly (there's no server to defer
that to in an embedded model). `client.close()` flushes and closes
every open collection and index file handle. No multi-process locking:
this is a single-process embedded database, like SQLite's default
(non-WAL) mode conceptually -- concurrent access from multiple
processes to the same data directory is not supported.

### CRUD

```python
coll.insert_one({"name": "Ada"})                       # -> InsertOneResult(inserted_id)
coll.insert_many([{"a": 1}, {"a": 2}], ordered=True)    # -> InsertManyResult(inserted_ids)

coll.find_one({"name": "Ada"})                          # -> dict | None
list(coll.find({"age": {"$gte": 18}}, projection={"name": 1}, skip=0, limit=10))  # -> Cursor (lazy)

coll.update_one({"name": "Ada"}, {"$set": {"age": 37}, "$inc": {"visits": 1}})     # -> UpdateResult
coll.update_many({"tag": "x"}, {"$unset": {"temp": ""}})
coll.replace_one({"name": "Ada"}, {"name": "Ada", "age": 99})   # _id preserved unless replacement supplies a matching one

coll.find_one_and_update(filter, update, return_document=False)  # False (default) = pre-image, True = post-image
coll.find_one_and_delete(filter)
coll.find_one_and_replace(filter, replacement, return_document=False)

coll.delete_one({"name": "Ada"})                        # -> DeleteResult(deleted_count)
coll.delete_many({"tag": "x"})
```

`update`/`find_one_and_update` documents must be built entirely from
`$set`/`$unset`/`$inc` -- a raw non-operator document raises
`InvalidUpdateDocument` (matches real pymongo's own rule).
`_id` is immutable: changing it via `$set`, `$unset`, `$inc`, or a
mismatched `_id` in a replacement raises `InvalidDocument`.
`upsert=True` seeds the inserted document from the filter's top-level
plain-equality/`$eq` clauses (operator clauses like `$gt` can't
meaningfully seed a value and are skipped), then applies the update's
`$set`/`$inc` on top.

There is no in-place update: every mutation tombstones the old record
and appends a new one (see `docs/wire_protocol.md`'s record format).

### Query filters

Top-level keys are implicitly AND'd. Supported: dot-notation
(`"a.b"`), array containment (`{"tags": "x"}` matches a list
containing `"x"`), `$eq`/`$ne`/`$gt`/`$gte`/`$lt`/`$lte`/`$in`/`$nin`,
`$and`/`$or` (lists of sub-filters), and `$not` (a per-field operator
modifier: `{"field": {"$not": {...}}}`, not a top-level combinator).
No `$regex`/`$elemMatch`/array operators.

Filter/update evaluation runs on a decoded Python `dict`
(`jsondb.decode()` + `python/jsondb/query.py`'s `matches()`),
not as a raw-BSON predicate pushdown -- see `query.py`'s module
docstring for why. A B-Tree index (below) narrows candidates for
eligible filters; the full filter is always re-evaluated against every
candidate, so an index can only make a query faster, never wrong.

### Aggregation-style helpers

```python
coll.count_documents({"tag": "x"})    # exact, scans (or index-narrows) matches
coll.estimated_document_count()        # O(1): the storage header's live-record counter
coll.distinct("tags")                  # unique values; an array field contributes its elements
```

### Indexing

```python
coll.create_index("age")                    # -> "age_1"
coll.create_index({"age": -1}, unique=True)  # -> "age_-1"
coll.list_indexes()                          # -> [{"name": "age_1", "key": {"age": 1}, "unique": False}, ...]
coll.drop_index("age_1")
```

Backed by a persisted on-disk B+Tree per index
(`include/custom_bson/btree.h`). Scope limits, documented in the
header:

- **Single-field indexes only** -- a dict/list with more than one
  field raises `BSONNotImplementedError`.
- **Indexable value types:** `int`, `float` (not `NaN`), `bool`,
  `datetime.datetime`, `ObjectId`, and `None` (a missing field is
  indexed under a `None`/null key, so index behavior never silently
  changes based on field presence). `str`, `bytes`, `list`, and `dict`
  values raise `BSONNotImplementedError` when encountered during
  `create_index()`'s build scan -- the partial index file is deleted
  before the error propagates (all-or-nothing).
- **Index acceleration is equality-only** (`{"field": value}` or
  `{"field": {"$eq": value}}`) -- `$gt`/`$gte`/`$lt`/`$lte` always fall
  back to a full scan, because the index's key ordering across mixed
  numeric BSON subtypes (e.g. int vs. float) isn't a true cross-type
  total order; using it for a range scan could silently skip
  qualifying documents. Equality lookups don't have this hazard.
- **Delete is tombstone-only** (no B-Tree merge/rebalance) -- entry
  count only grows until `compact()` rebuilds every index from
  scratch, which is also what reclaims the tombstoned space.
- `unique=True` is enforced across the whole index (not just one
  B-Tree page); a violation raises `DuplicateKeyError` and rolls back
  the just-inserted data record and any already-inserted entries in
  other indexes for that same write.

### Result types

```
InsertOneResult(inserted_id)
InsertManyResult(inserted_ids)
UpdateResult(matched_count, modified_count, upserted_id)
DeleteResult(deleted_count)
```

### Additional exceptions

```
BSONError
├── ...(see above)
├── InvalidUpdateDocument   # update document isn't built from $set/$unset/$inc
└── DuplicateKeyError        # unique index violation
```
