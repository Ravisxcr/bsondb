# API reference

A glossary-style lookup for the `bsondb` Python API. Entries are
grouped by category — jump straight to the symbol you need via the
[quick reference](#quick-reference) table, or browse a category below.

For the underlying wire/file formats, see
[`docs/wire_protocol.md`](wire_protocol.md).

## Table of contents

- [Quick reference](#quick-reference)
- [1. Module-level codec functions](#1-module-level-codec-functions)
  - [1.1 `bsondb.encode`](#11-bsondbencodedocument-dict---bytes)
  - [1.2 `bsondb.decode`](#12-bsondbdecodedata-bytes---dict)
- [2. Core types](#2-core-types)
  - [2.1 `bsondb.ObjectId`](#21-bsondbobjectid)
- [3. Exceptions](#3-exceptions)
  - [3.1 Codec exceptions](#31-codec-exceptions)
  - [3.2 Collection exceptions](#32-collection-exceptions)
- [4. Python ↔ BSON type mapping](#4-python--bson-type-mapping)
- [5. `BsonDBClient` / `Database` / `Collection`](#5-bsondbclient--database--collection)
  - [5.1 Construction & lifecycle](#51-construction--lifecycle)
  - [5.2 Insert](#52-insert)
  - [5.3 Query](#53-query)
    - [5.3.1 Cursor](#531-cursor)
  - [5.4 Query filter reference](#54-query-filter-reference)
  - [5.5 Update & replace](#55-update--replace)
  - [5.6 Find-and-modify](#56-find-and-modify)
  - [5.7 Delete](#57-delete)
  - [5.8 Aggregation-style helpers](#58-aggregation-style-helpers)
  - [5.9 Indexing](#59-indexing)
  - [5.10 Maintenance](#510-maintenance)
  - [5.11 Result types](#511-result-types)

## Quick reference

| Symbol | Category | Summary |
| --- | --- | --- |
| [`bsondb.encode(document)`](#11-bsondbencodedocument-dict---bytes) | Codec | `dict` → BSON `bytes` |
| [`bsondb.decode(data)`](#12-bsondbdecodedata-bytes---dict) | Codec | BSON `bytes` → `dict` |
| [`bsondb.ObjectId`](#21-bsondbobjectid) | Core type | 12-byte unique document id |
| [`bsondb.BsonDBClient(path)`](#51-construction--lifecycle) | Client | Opens/creates a local data directory |
| [`client.<db_name>` / `client[<db_name>]`](#51-construction--lifecycle) | Client | Access a `Database` |
| [`db.<collection_name>` / `db[<collection_name>]`](#51-construction--lifecycle) | Database | Access a `Collection` |
| [`client.list_database_names()`](#51-construction--lifecycle) | Client | List database directories |
| [`client.drop_database(name)`](#51-construction--lifecycle) | Client | Delete a database and its files |
| [`db.list_collection_names()`](#51-construction--lifecycle) | Database | List collection data files |
| [`db.drop_collection(name)`](#51-construction--lifecycle) | Database | Delete one collection and its indexes |
| [`coll.insert_one(doc)`](#52-insert) | Collection | Insert a single document |
| [`coll.insert_many(docs)`](#52-insert) | Collection | Insert multiple documents |
| [`coll.find(filter)`](#53-query) | Collection | Lazy `Cursor` over matching documents |
| [`coll.find_one(filter)`](#53-query) | Collection | First matching document, or `None` |
| [`Cursor.sort()` / `skip()` / `limit()`](#531-cursor) | Cursor | Configure and consume query results |
| [`coll.update_one(filter, update)`](#55-update--replace) | Collection | Update the first match |
| [`coll.update_many(filter, update)`](#55-update--replace) | Collection | Update every match |
| [`coll.replace_one(filter, replacement)`](#55-update--replace) | Collection | Replace the first match wholesale |
| [`coll.find_one_and_update(...)`](#56-find-and-modify) | Collection | Atomic find + update, returns a document |
| [`coll.find_one_and_delete(...)`](#56-find-and-modify) | Collection | Atomic find + delete, returns a document |
| [`coll.find_one_and_replace(...)`](#56-find-and-modify) | Collection | Atomic find + replace, returns a document |
| [`coll.delete_one(filter)`](#57-delete) | Collection | Delete the first match |
| [`coll.delete_many(filter)`](#57-delete) | Collection | Delete every match |
| [`coll.count_documents(filter)`](#58-aggregation-style-helpers) | Collection | Exact match count |
| [`coll.estimated_document_count()`](#58-aggregation-style-helpers) | Collection | O(1) approximate count |
| [`coll.distinct(field)`](#58-aggregation-style-helpers) | Collection | Unique values for a field |
| [`coll.create_index(keys)`](#59-indexing) | Collection | Build a persisted B+Tree index |
| [`coll.drop_index(name)`](#59-indexing) | Collection | Remove an index |
| [`coll.list_indexes()`](#59-indexing) | Collection | Enumerate indexes |
| [`coll.drop()`](#510-maintenance) | Collection | Delete the collection's data/index files |
| [`coll.compact()`](#510-maintenance) | Collection | Reclaim tombstoned space |
| [`InsertOneResult` / `InsertManyResult` / `UpdateResult` / `DeleteResult`](#511-result-types) | Result types | Return values of the above |
| [`BSONError` hierarchy](#31-codec-exceptions) | Exceptions | Codec-level failures |
| [`InvalidUpdateDocument`, `DuplicateKeyError`, `OperationFailure`](#32-collection-exceptions) | Exceptions | Collection-level failures |

## 1. Module-level codec functions

### 1.1 `bsondb.encode(document: dict) -> bytes`

Serializes a Python `dict` to BSON bytes.

**Raises**

| Exception | Cause |
| --- | --- |
| `TypeError` | `document` is not a `dict` |
| `bsondb.InvalidDocument` | a key is not `str` (or contains an embedded NUL byte), or a value has no BSON representation (see the [type table](#4-python--bson-type-mapping)) |
| `OverflowError` | an `int` value doesn't fit in a signed 64-bit BSON int |
| `bsondb.DocumentTooLarge` | the encoded document would exceed 16 MiB |
| `MemoryError` | allocation failure |

### 1.2 `bsondb.decode(data: bytes) -> dict`

Deserializes BSON bytes back to a Python `dict`. Accepts anything
implementing the buffer protocol (`bytes`, `bytearray`, `memoryview`).

**Raises**

| Exception | Cause |
| --- | --- |
| `bsondb.InvalidBSON` | the bytes are truncated, malformed, or otherwise corrupt (bad length field, invalid type byte, unterminated string, invalid UTF-8, trailing bytes after the document, missing end-of-object byte, or nesting deeper than 100 levels) |
| `bsondb.BSONNotImplementedError` | the document contains a structurally valid BSON type this version doesn't support yet (Regex, Timestamp, Decimal128, MinKey/MaxKey, or a deprecated type) |
| `MemoryError` | allocation failure |

> For inputs over ~4KB, the initial validation pass runs with the GIL
> released (see `src/python_bindings/_bson_core.c`), so a large
> `decode()` call doesn't block other Python threads for its full
> duration. `encode()` holds the GIL throughout, since its recursive
> walk touches Python objects at effectively every step — there's no
> extended pure-buffer phase to release around, unlike decode's
> validation pass. See [`docs/wire_protocol.md`](wire_protocol.md#c-engine-implementation)
> for the underlying reader/iterator/writer design.

## 2. Core types

### 2.1 `bsondb.ObjectId`

A 12-byte unique identifier, matching MongoDB/BSON's ObjectId format.
Pure Python (`python/bsondb/object_id.py`) — see the module docstring
for why this isn't implemented in C.

**Construction**

```python
ObjectId()                             # generates a new id
ObjectId("507f1f77bcf86cd799439011")   # from a 24-char hex string
ObjectId(b"...")                       # from 12 raw bytes
```

**Attributes / conversions**

| Member | Type | Description |
| --- | --- | --- |
| `oid.binary` | `bytes` | the 12 raw bytes |
| `oid.generation_time` | `datetime.datetime` (UTC) | the timestamp embedded in the id |
| `str(oid)` | `str` | 24-char hex string |

**Supported operators:** equality, hashing, and ordering (`<`, `<=`,
`>`, `>=`) by raw byte value.

## 3. Exceptions

### 3.1 Codec exceptions

```
BSONError
├── InvalidBSON               # malformed/corrupt wire data (decode)
├── InvalidDocument            # unencodable Python object graph (encode)
├── BSONNotImplementedError    # valid but unsupported BSON type (decode)
└── DocumentTooLarge            # encoded size exceeds the 16 MiB limit
```

| Exception | Raised by |
| --- | --- |
| `InvalidBSON` | [`decode`](#12-bsondbdecodedata-bytes---dict) |
| `InvalidDocument` | [`encode`](#11-bsondbencodedocument-dict---bytes), or a mutation that touches `_id` (see [§5.5](#55-update--replace)) |
| `BSONNotImplementedError` | [`decode`](#12-bsondbdecodedata-bytes---dict), or [`create_index`](#59-indexing) on a non-indexable value type |
| `DocumentTooLarge` | [`encode`](#11-bsondbencodedocument-dict---bytes) |

### 3.2 Collection exceptions

```
BSONError
├── ...(codec exceptions above)
├── InvalidUpdateDocument   # update document isn't built from $set/$unset/$inc
└── DuplicateKeyError        # unique index violation

OperationFailure             # a collection operation cannot be completed
```

| Exception | Raised by |
| --- | --- |
| `InvalidUpdateDocument` | [`update_one`/`update_many`/`find_one_and_update`](#55-update--replace) when the update document isn't built entirely from `$set`/`$unset`/`$inc` |
| `DuplicateKeyError` | any write through a `unique=True` index (see [§5.9](#59-indexing)) |
| `bsondb.exceptions.OperationFailure` | `drop_index` when the index does not exist, or `create_index` when its requested name already has a different specification |

## 4. Python ↔ BSON type mapping

| Python type | BSON type | Notes |
| --- | --- | --- |
| `dict` | Document | keys must be `str` with no embedded NUL |
| `list`, `tuple` | Array | decode always returns `list` |
| `str` | String | must be valid Unicode |
| `bytes`, `bytearray` | Binary (subtype `0x00`) | decode always returns `bytes` |
| `bool` | Boolean | checked before `int` (`bool` is an `int` subclass in Python) |
| `int` | Int32 or Int64 | chosen by magnitude; `OverflowError` if it doesn't fit in 64 bits |
| `float` | Double | raw IEEE-754 bits preserved, including `nan`/`inf` |
| `None` | Null | |
| `bsondb.ObjectId` | ObjectId | |
| `datetime.datetime` | UTC datetime | naive datetimes are treated as already-UTC; decode always returns naive datetimes |

## 5. `BsonDBClient` / `Database` / `Collection`

### 5.1 Construction & lifecycle

```python
client = bsondb.BsonDBClient("./data")   # local data directory, not a network connection; creates it if missing
db = client.mydb                          # -> Database (also: client["mydb"])
coll = db.mycollection                    # -> Collection (also: db["mycollection"])
```

| Call | Returns | Notes |
| --- | --- | --- |
| `BsonDBClient(path)` | `BsonDBClient` | Creates `path` eagerly — there's no server to defer that to in an embedded model |
| `client.<db_name>` / `client[name]` | `Database` | Returns a lightweight database handle; its directory is created on first collection I/O |
| `db.<collection_name>` / `db[name]` | `Collection` | Returns a lightweight collection handle; its `.cbd` file is created on first I/O |
| `client.list_database_names()` | `list[str]` | Names of subdirectories under `client.path` |
| `client.drop_database(name_or_database)` | `None` | Deletes a database directory; accepts its name or a `Database` handle |
| `db.list_collection_names()` | `list[str]` | Names of this database's `.cbd` files |
| `db.drop_collection(name_or_collection)` | `None` | Deletes a collection's data and index files; accepts its name or a `Collection` handle |
| `client.close()` | `None` | Flushes and closes every open collection and index file handle |
| `with BsonDBClient(path) as client` | `BsonDBClient` | Closes the client when the context exits |

> No multi-process locking: this is a single-process embedded
> database, like SQLite's default (non-WAL) mode conceptually —
> concurrent access from multiple processes to the same data directory
> is not supported.

### 5.2 Insert

| Method | Signature | Returns |
| --- | --- | --- |
| `insert_one` | `coll.insert_one(document: dict)` | [`InsertOneResult`](#511-result-types) (`inserted_id`) |
| `insert_many` | `coll.insert_many(documents: list[dict], ordered: bool = True)` | [`InsertManyResult`](#511-result-types) (`inserted_ids`) |

```python
coll.insert_one({"name": "Ada"})
coll.insert_many([{"a": 1}, {"a": 2}], ordered=True)
```

### 5.3 Query

| Method | Signature | Returns |
| --- | --- | --- |
| `find_one` | `coll.find_one(filter: dict = None, projection: dict \| list[str] = None)` | `dict \| None` |
| `find` | `coll.find(filter: dict = None, projection: dict \| list[str] = None, skip: int = 0, limit: int = 0)` | `Cursor` (lazy, iterable) |

```python
coll.find_one({"name": "Ada"})
list(coll.find({"age": {"$gte": 18}}, projection={"name": 1}, skip=0, limit=10))
```

### 5.3.1 Cursor

`find()` returns a lazy, single-pass `Cursor`. Configure it before
iteration; configuration methods return the same cursor to allow
chaining.

| Method / member | Signature | Notes |
| --- | --- | --- |
| `skip` | `cursor.skip(count: int)` | Skip `count` documents; `count` must be non-negative |
| `limit` | `cursor.limit(count: int)` | Yield at most `count` documents; `0` means unlimited |
| `sort` | `cursor.sort(field, direction=1)` | Sorts in memory; accepts a field name, a mapping, or a sequence of `(field, direction)` pairs. A negative direction sorts descending. |
| `projection` | `cursor.projection(spec)` | Replaces the projection; accepts an inclusion/exclusion mapping or a sequence of field names |
| `rewind` | `cursor.rewind()` | Reset the cursor so it can be iterated again |
| `clone` | `cursor.clone()` | Return a fresh cursor with the same settings |
| `to_list` | `cursor.to_list(length=None)` | Materialize remaining results, optionally capped at `length` |
| `alive` | `cursor.alive` | `True` until the cursor is exhausted |
| indexing | `cursor[index]` / `cursor[start:stop]` | Reads from a fresh clone; negative indexes are not supported |

Calling `skip`, `limit`, `sort`, or `projection` after iteration has
begun raises `RuntimeError`. Sorting materializes the matching documents
before applying skip and limit.

A B-Tree index (see [§5.9](#59-indexing)) narrows candidates for
eligible filters; the full filter is always re-evaluated against every
candidate, so an index can only make a query faster, never wrong.
Filter/update evaluation runs on a decoded Python `dict`
(`bsondb.decode()` + `python/bsondb/query.py`'s `matches()`), not as a
raw-BSON predicate pushdown — see `query.py`'s module docstring for
why.

### 5.4 Query filter reference

Top-level keys are implicitly AND'd.

| Feature | Example | Notes |
| --- | --- | --- |
| Dot-notation | `{"a.b": 1}` | Matches nested fields |
| Array containment | `{"tags": "x"}` | Matches a list containing `"x"` |
| `$eq`, `$ne` | `{"age": {"$eq": 30}}` | Equality / inequality |
| `$gt`, `$gte`, `$lt`, `$lte` | `{"age": {"$gte": 18}}` | Range comparisons — always a full scan, never index-accelerated (see [§5.9](#59-indexing)) |
| `$in`, `$nin` | `{"tag": {"$in": ["a", "b"]}}` | Membership |
| `$and`, `$or` | `{"$or": [{"a": 1}, {"b": 2}]}` | Lists of sub-filters |
| `$not` | `{"field": {"$not": {"$eq": 1}}}` | Per-field operator modifier, not a top-level combinator |

**Not supported:** `$regex`, `$elemMatch`, array-position operators.

### 5.5 Update & replace

| Method | Signature | Returns |
| --- | --- | --- |
| `update_one` | `coll.update_one(filter: dict, update: dict, upsert: bool = False)` | [`UpdateResult`](#511-result-types) |
| `update_many` | `coll.update_many(filter: dict, update: dict, upsert: bool = False)` | [`UpdateResult`](#511-result-types) |
| `replace_one` | `coll.replace_one(filter: dict, replacement: dict, upsert: bool = False)` | [`UpdateResult`](#511-result-types) |

```python
coll.update_one({"name": "Ada"}, {"$set": {"age": 37}, "$inc": {"visits": 1}})
coll.update_many({"tag": "x"}, {"$unset": {"temp": ""}})
coll.replace_one({"name": "Ada"}, {"name": "Ada", "age": 99})  # _id preserved unless replacement supplies a matching one
```

**Update operators:** `$set`, `$unset`, `$inc`. An update document
must be built *entirely* from these — a raw non-operator document
raises `InvalidUpdateDocument` (matches real pymongo's own rule).

**Rules**

- `_id` is immutable: changing it via `$set`, `$unset`, `$inc`, or a
  mismatched `_id` in a replacement raises `InvalidDocument`.
- `upsert=True` seeds the inserted document from the filter's
  top-level plain-equality/`$eq` clauses (operator clauses like `$gt`
  can't meaningfully seed a value and are skipped), then applies the
  update's `$set`/`$inc` on top.
- There is no in-place update: every mutation tombstones the old
  record and appends a new one (see
  [`docs/wire_protocol.md`](wire_protocol.md#collection-data-file-format-custom)'s
  record format).

### 5.6 Find-and-modify

| Method | Signature | Returns |
| --- | --- | --- |
| `find_one_and_update` | `coll.find_one_and_update(filter, update, return_document=False)` | `dict \| None` |
| `find_one_and_delete` | `coll.find_one_and_delete(filter)` | `dict \| None` |
| `find_one_and_replace` | `coll.find_one_and_replace(filter, replacement, return_document=False)` | `dict \| None` |

`return_document`: `False` (default) returns the pre-image, `True`
returns the post-image. Update-document rules match
[§5.5](#55-update--replace).

### 5.7 Delete

| Method | Signature | Returns |
| --- | --- | --- |
| `delete_one` | `coll.delete_one(filter: dict)` | [`DeleteResult`](#511-result-types) (`deleted_count`) |
| `delete_many` | `coll.delete_many(filter: dict)` | [`DeleteResult`](#511-result-types) (`deleted_count`) |

```python
coll.delete_one({"name": "Ada"})
coll.delete_many({"tag": "x"})
```

### 5.8 Aggregation-style helpers

| Method | Signature | Notes |
| --- | --- | --- |
| `count_documents` | `coll.count_documents(filter: dict = None)` | Exact; scans (or index-narrows) matches |
| `estimated_document_count` | `coll.estimated_document_count()` | O(1): the storage header's live-record counter |
| `distinct` | `coll.distinct(field: str, filter: dict = None)` | Unique values among matches; an array field contributes its elements |

```python
coll.count_documents({"tag": "x"})
coll.estimated_document_count()
coll.distinct("tags")
```

### 5.9 Indexing

| Method | Signature | Returns |
| --- | --- | --- |
| `create_index` | `coll.create_index(keys: str \| dict, unique: bool = False)` | index name (`str`) |
| `drop_index` | `coll.drop_index(name: str)` | `None` |
| `list_indexes` | `coll.list_indexes()` | `list[dict]` — `{"name", "key", "unique"}` per index |

```python
coll.create_index("age")                    # -> "age_1"
coll.create_index({"age": -1}, unique=True)  # -> "age_-1"
coll.list_indexes()                          # -> [{"name": "age_1", "key": {"age": 1}, "unique": False}, ...]
coll.drop_index("age_1")
```

`drop_index()` also accepts a bare field name (for example,
`coll.drop_index("age")`) and resolves it to that field's ascending or
descending generated index name. Attempting to drop a missing index, or
creating an existing generated index name with a different specification,
raises `bsondb.exceptions.OperationFailure`.

Backed by a persisted on-disk B+Tree per index — see
[`docs/wire_protocol.md`](wire_protocol.md#b-tree-index-file-format-custom)
for the on-disk format. Scope limits:

| Limit | Detail |
| --- | --- |
| Single-field indexes only | a dict/list with more than one field raises `BSONNotImplementedError` |
| Indexable value types | `int`, `float` (not `NaN`), `bool`, `datetime.datetime`, `ObjectId`, and `None` (a missing field is indexed under a `None`/null key, so index behavior never silently changes based on field presence) |
| Non-indexable value types | `str`, `bytes`, `list`, `dict` raise `BSONNotImplementedError` during `create_index()`'s build scan — the partial index file is deleted before the error propagates (all-or-nothing) |
| Acceleration scope | equality-only (`{"field": value}` or `{"field": {"$eq": value}}`) — `$gt`/`$gte`/`$lt`/`$lte` always fall back to a full scan, because the index's key ordering across mixed numeric BSON subtypes (e.g. int vs. float) isn't a true cross-type total order; using it for a range scan could silently skip qualifying documents. Equality lookups don't have this hazard |
| Delete | tombstone-only (no B-Tree merge/rebalance) — entry count only grows until `compact()` rebuilds every index from scratch, which is also what reclaims the tombstoned space |
| `unique=True` | enforced across the whole index (not just one B-Tree page); a violation raises `DuplicateKeyError` and rolls back the just-inserted data record and any already-inserted entries in other indexes for that same write |

### 5.10 Maintenance

| Method | Signature | Notes |
| --- | --- | --- |
| `drop` | `coll.drop()` | Deletes the collection's data file and all index files |
| `compact` | `coll.compact()` | Rebuilds the data file and every index from scratch, reclaiming tombstoned space |

### 5.11 Result types

| Type | Fields |
| --- | --- |
| `InsertOneResult` | `inserted_id` |
| `InsertManyResult` | `inserted_ids` |
| `UpdateResult` | `matched_count`, `modified_count`, `upserted_id` |
| `DeleteResult` | `deleted_count` |
