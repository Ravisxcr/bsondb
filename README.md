# custom_bson (jsondb)

An embedded, file-backed document database for Python with a
[PyMongo](https://pymongo.readthedocs.io/)-like local API
(`MongoClient`, `Database`, `Collection`, `ObjectId`), built on a
native C BSON serialization engine, an mmap-backed storage engine, and
a persisted on-disk B-Tree index. There is no network protocol and no
real `mongod` involved -- `MongoClient` is a local facade over BSON
documents stored in memory-mapped files on disk.

## Status

Implemented end to end:

- **BSON codec:** `encode(dict) -> bytes` / `decode(bytes) -> dict`,
  `ObjectId`, and the full exception hierarchy. See
  `docs/wire_protocol.md` for the wire format and type coverage.
- **Storage engine:** an append-only, mmap-backed collection data file
  per collection (`include/custom_bson/storage.h`), with crash
  recovery on unclean shutdown.
- **Collection API:** `insert_one`/`insert_many`, `find`/`find_one`
  (dot-notation, `$eq/$ne/$gt/$gte/$lt/$lte/$in/$nin/$and/$or/$not`,
  projection, skip/limit), `update_one`/`update_many`
  (`$set`/`$unset`/`$inc`, upsert), `replace_one`,
  `find_one_and_update`/`_delete`/`_replace`, `delete_one`/`delete_many`,
  `count_documents`/`estimated_document_count`/`distinct`, `drop`,
  `compact`.
- **Indexing:** a persisted on-disk B+Tree per indexed field
  (`include/custom_bson/btree.h`) via `create_index`/`drop_index`/
  `list_indexes`, used to accelerate equality queries and enforce
  unique constraints.

See `docs/api_reference.md` for the full Python API and documented
scope limits (single-process access, single-field indexes, index
acceleration for equality only).

## Quickstart

```bash
pip install -e ".[dev]"
```

```python
import custom_bson

client = custom_bson.MongoClient("./data")
coll = client.mydb.people

coll.create_index("age")
coll.insert_many([{"name": "Ada", "age": 36}, {"name": "Bo", "age": 20}])
print(list(coll.find({"age": {"$gte": 30}})))

coll.update_one({"name": "Ada"}, {"$set": {"age": 37}})
coll.delete_one({"name": "Bo"})
client.close()
```

## Development

```bash
pip install -e ".[dev]"
pytest tests/python_tests/ -v

# Optional: smoke-build the standalone C engine + C test executables
cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build
ctest --test-dir build --output-on-failure
```

## Project layout

```
include/custom_bson/   Public C headers (BSON engine, mmap abstraction, storage/record format, B-Tree index)
src/c_engine/           Storage/BSON engine implementation (pure C11, no Python dependency)
src/python_bindings/    CPython C-API bindings: _bson_core.c (encode/decode), _storage_core.c (storage/index)
python/custom_bson/     Pure-Python package: ObjectId, exceptions, MongoClient/Database/Collection, query/index/cursor logic
tests/                   C and Python test suites
docs/                    Wire protocol spec and API reference
```
