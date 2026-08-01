"""MongoClient: a local facade over BSON documents stored on disk.

custom_bson is an embedded, file-backed document database, not a
network client -- there is no real ``mongod`` involved. MongoClient
manages a local data directory: one subdirectory per database, one
``<collection>.cbd`` memory-mapped file per collection, and any number
of ``<collection>.<index_name>.bidx`` B-Tree index files alongside it
(see docs/wire_protocol.md, include/custom_bson/storage.h, and
include/custom_bson/btree.h).

Behavior note: unlike a lazy network client, construction now touches
disk (creates the root data directory) -- there's no server to defer
that to in an embedded model.

No multi-process locking: this is a single-process embedded database,
like SQLite's default (non-WAL, non-shared-cache) mode conceptually.
Concurrent access from multiple processes to the same data directory is
not supported in this slice.
"""

from __future__ import annotations

import os
from typing import Dict, Tuple, Union

from . import _storage_core
from .database import Database


class MongoClient:
    """Entry point for the embedded database, rooted at a local directory."""

    def __init__(self, path: Union[str, "os.PathLike[str]"] = "./data") -> None:
        self._path = os.fspath(path)
        os.makedirs(self._path, exist_ok=True)
        self._handles: Dict[Tuple[str, str], "_storage_core.CollectionHandle"] = {}
        self._index_handles: Dict[Tuple[str, str, str], "_storage_core.IndexHandle"] = {}

    @property
    def path(self) -> str:
        return self._path

    def _get_handle(self, db_name: str, coll_name: str) -> "_storage_core.CollectionHandle":
        """Returns the (cached, shared) open handle for one collection's
        data file, opening it on first use. Handles are cached here --
        not on individual Collection objects -- so every ``db.coll``
        attribute access reuses the same open mmap'd file rather than
        reopening it (pymongo's Collection objects are similarly cheap,
        backed by a shared connection pool on the client)."""
        key = (db_name, coll_name)
        handle = self._handles.get(key)
        if handle is None:
            db_dir = os.path.join(self._path, db_name)
            os.makedirs(db_dir, exist_ok=True)
            file_path = os.path.join(db_dir, f"{coll_name}.cbd")
            handle = _storage_core.open_collection(file_path)
            self._handles[key] = handle
        return handle

    def _invalidate_handle(self, db_name: str, coll_name: str) -> None:
        """Drops the cached handle (without deleting the file) so the
        next access reopens it fresh -- used after compact() rewrites a
        collection's file out from under an open handle."""
        handle = self._handles.pop((db_name, coll_name), None)
        if handle is not None:
            handle.close()

    def _drop_collection(self, db_name: str, coll_name: str, file_path: str) -> None:
        handle = self._handles.pop((db_name, coll_name), None)
        if handle is not None:
            handle.close()
        if os.path.exists(file_path):
            os.remove(file_path)

        for key in [handle_key for handle_key in self._index_handles if handle_key[0] == db_name and handle_key[1] == coll_name]:
            self._index_handles.pop(key).close()
        db_dir = os.path.join(self._path, db_name)
        prefix = f"{coll_name}."
        if os.path.isdir(db_dir):
            for fname in os.listdir(db_dir):
                if fname.startswith(prefix) and fname.endswith(".bidx"):
                    os.remove(os.path.join(db_dir, fname))

    def _get_index_handles(self, db_name: str, coll_name: str) -> Dict[str, "_storage_core.IndexHandle"]:
        """Returns {index_name: IndexHandle} for every index currently
        on disk for this collection, opening (and caching) any not
        already open. Re-lists the directory on every call -- a cheap
        os.listdir(), not a persistent watch -- so a newly created
        index becomes visible without an explicit cache-invalidation
        hook."""
        db_dir = os.path.join(self._path, db_name)
        prefix = f"{coll_name}."
        current: Dict[str, "_storage_core.IndexHandle"] = {}
        if not os.path.isdir(db_dir):
            return current
        for fname in os.listdir(db_dir):
            if not (fname.startswith(prefix) and fname.endswith(".bidx")):
                continue
            index_name = fname[len(prefix) : -len(".bidx")]
            key = (db_name, coll_name, index_name)
            handle = self._index_handles.get(key)
            if handle is None:
                handle = _storage_core.open_index_file(os.path.join(db_dir, fname))
                self._index_handles[key] = handle
            current[index_name] = handle
        return current

    def _invalidate_index_handle(self, db_name: str, coll_name: str, index_name: str) -> None:
        handle = self._index_handles.pop((db_name, coll_name, index_name), None)
        if handle is not None:
            handle.close()

    def __getattr__(self, name: str) -> Database:
        if name.startswith("_"):
            raise AttributeError(name)
        return self[name]

    def __getitem__(self, name: str) -> Database:
        return Database(self, name)

    def close(self) -> None:
        """Flushes and closes every open collection and index file handle."""
        for handle in self._handles.values():
            handle.close()
        self._handles.clear()
        for handle in self._index_handles.values():
            handle.close()
        self._index_handles.clear()

    def __repr__(self) -> str:
        return f"MongoClient({self._path!r})"
