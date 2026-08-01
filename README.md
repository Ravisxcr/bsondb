# bsondb

An embedded, file-backed document database for Python with a
[PyMongo](https://pymongo.readthedocs.io/)-like local API
(`BsonDBClient`, `Database`, `Collection`, `ObjectId`), built on a
native C BSON serialization engine, an mmap-backed storage engine, and
a persisted on-disk B-Tree index.

There is no network protocol and no real `mongod` involved —
`BsonDBClient` is a local facade over BSON documents stored in
memory-mapped files on disk.

- **License:** MIT
- **Python:** 3.9+
- **Dependencies:** none (stdlib + native extension only)

## Table of contents

- [Features](#features)
- [Quickstart](#quickstart)
- [Supported operations](#supported-operations)
- [Development](#development)
- [Project layout](#project-layout)
- [Documentation](#documentation)
- [Scope limits](#scope-limits)

## Features

| Area | What's implemented |
| --- | --- |
| BSON codec | `encode(dict) -> bytes` / `decode(bytes) -> dict`, `ObjectId`, full exception hierarchy |
| Storage engine | Append-only, mmap-backed collection data file per collection, with crash recovery on unclean shutdown |
| Collection API | Insert, find, update, replace, delete, count/distinct, drop, compact (see table below) |
| Indexing | Persisted on-disk B+Tree per indexed field, used to accelerate equality queries and enforce unique constraints |

## Quickstart

```bash
pip install -e ".[dev]"
```

```python
import bsondb

client = bsondb.BsonDBClient("./data")
coll = client.mydb.people

coll.create_index("age")
coll.insert_many([{"name": "Ada", "age": 36}, {"name": "Bo", "age": 20}])
print(list(coll.find({"age": {"$gte": 30}})))

coll.update_one({"name": "Ada"}, {"$set": {"age": 37}})
coll.delete_one({"name": "Bo"})
client.close()
```

## Supported operations

| Category | Methods / operators |
| --- | --- |
| Insert | `insert_one`, `insert_many` |
| Query | `find`, `find_one` — dot-notation fields, projection, skip/limit |
| Query operators | `$eq`, `$ne`, `$gt`, `$gte`, `$lt`, `$lte`, `$in`, `$nin`, `$and`, `$or`, `$not` |
| Update | `update_one`, `update_many` (`$set`, `$unset`, `$inc`, upsert), `replace_one` |
| Find-and-modify | `find_one_and_update`, `find_one_and_delete`, `find_one_and_replace` |
| Delete | `delete_one`, `delete_many` |
| Aggregation | `count_documents`, `estimated_document_count`, `distinct` |
| Maintenance | `drop`, `compact` |
| Indexing | `create_index`, `drop_index`, `list_indexes` |

## Development

Quickest path, using [Task](https://taskfile.dev):

```bash
task install   # create .venv and pip install -e ".[dev]"
task test      # run the Python and C test suites
```

Without Task:

```bash
# Install with dev dependencies and run the Python test suite
pip install -e ".[dev]"
pytest tests/python_tests/ -v

# Optional: smoke-build the standalone C engine + C test executables
cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build
ctest --test-dir build --output-on-failure
```

For OS-specific prerequisites (C compiler, Task, CMake), the full
`Taskfile.yml` task reference, and a walkthrough of how a document
flows through the codebase, see
**[`docs/development.md`](docs/development.md)**.

## Project layout

| Path | Contents |
| --- | --- |
| `include/custom_bson/` | Public C headers (BSON engine, mmap abstraction, storage/record format, B-Tree index) |
| `src/c_engine/` | Storage/BSON engine implementation (pure C11, no Python dependency) |
| `src/python_bindings/` | CPython C-API bindings: `_bson_core.c` (encode/decode), `_storage_core.c` (storage/index) |
| `python/bsondb/` | Pure-Python package: `ObjectId`, exceptions, `BsonDBClient`/`Database`/`Collection`, query/index/cursor logic |
| `tests/` | C and Python test suites |
| `docs/` | Wire protocol spec and API reference |

## Documentation

- [`docs/development.md`](docs/development.md) — setup per OS, prerequisites, Taskfile reference, folder structure
- [`docs/wire_protocol.md`](docs/wire_protocol.md) — BSON wire format and type coverage
- [`docs/api_reference.md`](docs/api_reference.md) — full Python API reference

## Scope limits

- Single-process access only
- Single-field indexes only
- Index acceleration applies to equality queries only

See `docs/api_reference.md` for full details.

## License

[MIT](LICENSE)
