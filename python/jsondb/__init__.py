"""jsondb: an embedded, file-backed document database with a
PyMongo-like local API, built on a native BSON serialization engine.

The BSON codec (``encode``, ``decode``, ``ObjectId``), the mmap-backed
storage engine (``JsonDBClient``/``Database``/``Collection``, see
include/custom_bson/storage.h), the full CRUD + query/update-operator
surface (see query.py), and the persisted B-Tree index (see
include/custom_bson/btree.h and index.py) are all implemented. Known
scope limits are documented inline: single-process access only (no
multi-process locking), single-field indexes only, index-accelerated
queries only for equality/$eq (not $gt/$lt ranges -- see btree.h), and
filter/update evaluation runs on decoded Python dicts rather than a raw
BSON predicate pushdown (see query.py's module docstring).

Import order matters here: exceptions and object_id are pure Python and
must be fully importable before _bson_core (the C extension) is
imported, because _bson_core's module-init routine imports
jsondb.object_id and jsondb.exceptions by name to cache their
classes for exception translation and ObjectId construction.
_storage_core (imported transitively via client.py) only needs
exceptions, which is already loaded by then.
"""

from __future__ import annotations

from .exceptions import (
    BSONError,
    BSONNotImplementedError,
    DocumentTooLarge,
    DuplicateKeyError,
    InvalidBSON,
    InvalidDocument,
    InvalidUpdateDocument,
)
from .object_id import ObjectId
from ._bson_core import decode, encode
from .client import JsonDBClient
from .database import Database
from .collection import Collection
from .cursor import Cursor
from .results import DeleteResult, InsertManyResult, InsertOneResult, UpdateResult

__version__ = "0.1.0"

__all__ = [
    "encode",
    "decode",
    "ObjectId",
    "JsonDBClient",
    "Database",
    "Collection",
    "Cursor",
    "InsertOneResult",
    "InsertManyResult",
    "UpdateResult",
    "DeleteResult",
    "BSONError",
    "InvalidBSON",
    "InvalidDocument",
    "BSONNotImplementedError",
    "DocumentTooLarge",
    "InvalidUpdateDocument",
    "DuplicateKeyError",
]
