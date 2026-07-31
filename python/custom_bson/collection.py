"""Collection: a named set of BSON documents within a Database, backed
by one memory-mapped ``<name>.cbd`` file (see include/custom_bson/storage.h).
"""

from __future__ import annotations

import os
from typing import Any, Dict, Iterator, List, Mapping, Optional, Tuple

from . import _storage_core
from . import index as index_module
from ._bson_core import decode, encode
from .cursor import Cursor, ProjectionSpec
from .exceptions import BSONNotImplementedError, DuplicateKeyError, InvalidDocument
from .object_id import ObjectId
from .query import (
    _values_equal,
    apply_update_operators,
    build_upsert_document,
    matches,
    seed_from_filter,
    validate_update_document,
)
from .results import DeleteResult, InsertManyResult, InsertOneResult, UpdateResult


def _reject_update_operators(replacement: Mapping[str, Any], caller: str) -> None:
    if any(k.startswith("$") for k in replacement):
        raise InvalidDocument(f"{caller}() replacement document must not contain update operators")


def _distinct_candidates(doc: Mapping[str, Any], segments: List[str]) -> List[Any]:
    """Direct (non-broadcasting) field resolution for distinct(): a
    list value at the path contributes its elements, a scalar
    contributes itself, and a missing path contributes nothing."""
    cursor: Any = doc
    for seg in segments:
        if isinstance(cursor, dict) and seg in cursor:
            cursor = cursor[seg]
        else:
            return []
    return cursor if isinstance(cursor, list) else [cursor]


def _replacement_with_preserved_id(
    replacement: Mapping[str, Any], old_doc: Mapping[str, Any], caller: str
) -> Dict[str, Any]:
    """Whole-document replace: `_id` is preserved from `old_doc` unless
    `replacement` supplies a matching one (a different `_id` is an
    error -- `_id` is immutable)."""
    new_doc = dict(replacement)
    if "_id" in new_doc:
        if "_id" in old_doc and new_doc["_id"] != old_doc["_id"]:
            raise InvalidDocument(f"_id is immutable and cannot be changed by {caller}()")
    elif "_id" in old_doc:
        new_doc["_id"] = old_doc["_id"]
    return new_doc


class Collection:
    """A named collection of documents within a :class:`~custom_bson.database.Database`."""

    def __init__(self, database: Any, name: str) -> None:
        self._database = database
        self._name = name

    @property
    def name(self) -> str:
        return self._name

    @property
    def database(self) -> Any:
        return self._database

    @property
    def _file_path(self) -> str:
        return os.path.join(self._database._client.path, self._database._name, f"{self._name}.cbd")

    @property
    def _handle(self) -> "_storage_core.CollectionHandle":
        return self._database._client._get_handle(self._database._name, self._name)

    def _iter_matches(self, filter: Optional[Mapping[str, Any]]) -> Iterator[Tuple[int, Dict[str, Any]]]:
        """Yields (record_offset, decoded_doc) for every LIVE record
        matching `filter`, via a full sequential scan. The single
        primitive both Cursor (read-only find/find_one) and the
        mutation methods below (which need the offset to tombstone/
        rewrite) are built on.

        A plain (non-generator) function that does eager filter-shape
        validation before returning the lazy generator that does the
        actual scanning -- a generator function's body doesn't execute
        at all until first iterated, so an invalid filter (e.g. an
        unknown $operator) against an empty collection would otherwise
        never raise: the scan loop would simply never run.
        """
        flt: Mapping[str, Any] = filter or {}
        matches({}, flt)  # validates operator/shape, independent of any real document
        return self.__scan(flt)

    def __scan(self, flt: Mapping[str, Any]) -> Iterator[Tuple[int, Dict[str, Any]]]:
        indexed = self._index_lookup_for_filter(flt)
        if indexed is not None:
            for offset in indexed:
                raw = self._handle.read_record(offset)
                if raw is None:
                    continue  # tombstoned since the index lookup ran
                doc = decode(raw)
                if matches(doc, flt):  # index only narrows candidates; full filter still re-checked
                    yield offset, doc
            return

        handle = self._handle
        prev = -1
        while True:
            offset = handle.next_live_offset(prev)
            if offset is None:
                return
            prev = offset
            raw = handle.read_record(offset)
            if raw is None:
                continue  # tombstoned between the scan step and this read
            doc = decode(raw)
            if matches(doc, flt):
                yield offset, doc

    def _index_lookup_for_filter(self, flt: Mapping[str, Any]) -> Optional[List[int]]:
        """Returns candidate record offsets from a B-Tree index if `flt`
        reduces to exactly one top-level equality/$eq clause on an
        indexed field, else None (meaning: fall back to a full scan).
        Conservative by design -- see query.py's module docstring and
        include/custom_bson/btree.h's note on why only equality (never
        $gt/$lt ranges) is routed through the index this slice."""
        if len(flt) != 1:
            return None
        ((field, condition),) = flt.items()
        if field.startswith("$"):
            return None
        if isinstance(condition, Mapping):
            if set(condition.keys()) != {"$eq"}:
                return None
            value = condition["$eq"]
        else:
            value = condition

        for handle in self._database._client._get_index_handles(self._database._name, self._name).values():
            if handle.field_path != field:
                continue
            try:
                key = index_module.encode_btree_key(value)
            except (BSONNotImplementedError, OverflowError, ValueError):
                return None  # value type unsupported for indexing -- fall back to full scan
            return handle.lookup(key)
        return None

    def _index_update_for_insert(self, doc: Mapping[str, Any], offset: int) -> None:
        """Inserts `doc`'s indexed-field entries into every index on
        this collection. All-or-nothing across indexes: if a later
        index raises DuplicateKeyError, entries already inserted into
        earlier indexes for this same call are rolled back before
        re-raising, so a failed insert never leaves a partially
        indexed document."""
        handles = self._database._client._get_index_handles(self._database._name, self._name)
        inserted: List[Tuple[Any, bytes]] = []
        try:
            for handle in handles.values():
                value = index_module.resolve_index_value(doc, handle.field_path)
                key = index_module.encode_btree_key(value)
                handle.insert(key, offset)
                inserted.append((handle, key))
        except DuplicateKeyError:
            for handle, key in inserted:
                handle.delete_entry(key, offset)
            raise

    def _index_update_for_delete(self, doc: Mapping[str, Any], offset: int) -> None:
        handles = self._database._client._get_index_handles(self._database._name, self._name)
        for handle in handles.values():
            value = index_module.resolve_index_value(doc, handle.field_path)
            key = index_module.encode_btree_key(value)
            handle.delete_entry(key, offset)

    def _apply_single_update(
        self, offset: int, old_doc: Mapping[str, Any], new_doc: Mapping[str, Any]
    ) -> UpdateResult:
        """Tombstones `offset` and appends `new_doc` -- unless it's
        byte-identical to `old_doc` re-encoded, in which case nothing is
        rewritten and modified_count is 0. No in-place update this
        slice (see docs/wire_protocol.md): always tombstone-old +
        append-new, never a partial in-place byte patch.

        Every index is repointed from the old offset to the new one
        regardless of whether the specific indexed field's value
        changed: since the record's offset always changes on a
        rewrite, every index entry referencing the old (now
        tombstoned) offset would otherwise silently go stale."""
        old_bytes = encode(dict(old_doc))
        new_bytes = encode(dict(new_doc))
        if new_bytes == old_bytes:
            return UpdateResult(1, 0, None)
        self._handle.tombstone_record(offset)
        new_offset = self._handle.append_record(new_bytes)
        self._index_update_for_delete(old_doc, offset)
        try:
            self._index_update_for_insert(new_doc, new_offset)
        except DuplicateKeyError:
            self._handle.tombstone_record(new_offset)
            raise
        return UpdateResult(1, 1, None)

    def _upsert(self, filter: Mapping[str, Any], update: Mapping[str, Any]) -> UpdateResult:
        seeded = build_upsert_document(filter, update)
        if "_id" not in seeded:
            seeded["_id"] = ObjectId()
        offset = self._handle.append_record(encode(seeded))
        try:
            self._index_update_for_insert(seeded, offset)
        except DuplicateKeyError:
            self._handle.tombstone_record(offset)
            raise
        return UpdateResult(0, 0, seeded["_id"])

    # =========================================================================
    # 1. CREATE / INSERT OPERATIONS
    # =========================================================================

    def insert_one(self, document: Mapping[str, Any]) -> InsertOneResult:
        """Insert a single document, generating an ``_id`` if absent."""
        doc = dict(document)
        if "_id" not in doc:
            doc["_id"] = ObjectId()
        offset = self._handle.append_record(encode(doc))
        try:
            self._index_update_for_insert(doc, offset)
        except DuplicateKeyError:
            self._handle.tombstone_record(offset)
            raise
        return InsertOneResult(doc["_id"])

    def insert_many(self, documents: Any, ordered: bool = True) -> InsertManyResult:
        """Insert multiple documents. `ordered=True` (default) stops at
        the first error, propagating it immediately -- documents
        already inserted stay inserted (no rollback, consistent with
        this slice's no-in-place/no-WAL posture elsewhere).
        `ordered=False` attempts every document regardless of earlier
        failures and, if any failed, raises the *first* error after all
        attempts complete -- a deliberately simplified bulk-error model
        compared to real pymongo's aggregating `BulkWriteError`."""
        inserted_ids = []
        if ordered:
            for document in documents:
                inserted_ids.append(self.insert_one(document).inserted_id)
            return InsertManyResult(inserted_ids)

        first_error: Optional[Exception] = None
        for document in documents:
            try:
                inserted_ids.append(self.insert_one(document).inserted_id)
            except (InvalidDocument, DuplicateKeyError) as exc:
                if first_error is None:
                    first_error = exc
        if first_error is not None:
            raise first_error
        return InsertManyResult(inserted_ids)

    # =========================================================================
    # 2. READ / QUERY OPERATIONS
    # =========================================================================

    def find_one(
        self,
        filter: Optional[Mapping[str, Any]] = None,
        projection: Optional[ProjectionSpec] = None,
    ) -> Optional[Dict[str, Any]]:
        """Return the first document matching `filter`, or None."""
        for doc in Cursor(self, filter=filter, projection=projection, limit=1):
            return doc
        return None

    def find(
        self,
        filter: Optional[Mapping[str, Any]] = None,
        projection: Optional[ProjectionSpec] = None,
        skip: int = 0,
        limit: int = 0,
    ) -> Cursor:
        """Return a lazy Cursor over documents matching `filter`."""
        return Cursor(self, filter=filter, projection=projection, skip=skip, limit=limit)

    def find_one_and_update(
        self, filter: Mapping[str, Any], update: Mapping[str, Any], return_document: bool = False
    ) -> Optional[Dict[str, Any]]:
        """`return_document`: False (default) returns the pre-update
        document, True returns the post-update document."""
        validate_update_document(update)
        for offset, old_doc in self._iter_matches(filter):
            new_doc = apply_update_operators(old_doc, update)
            self._apply_single_update(offset, old_doc, new_doc)
            return new_doc if return_document else old_doc
        return None

    def find_one_and_delete(self, filter: Mapping[str, Any]) -> Optional[Dict[str, Any]]:
        for offset, doc in self._iter_matches(filter):
            self._handle.tombstone_record(offset)
            self._index_update_for_delete(doc, offset)
            return doc
        return None

    def find_one_and_replace(
        self, filter: Mapping[str, Any], replacement: Mapping[str, Any], return_document: bool = False
    ) -> Optional[Dict[str, Any]]:
        _reject_update_operators(replacement, "find_one_and_replace")
        for offset, old_doc in self._iter_matches(filter):
            new_doc = _replacement_with_preserved_id(replacement, old_doc, "find_one_and_replace")
            self._apply_single_update(offset, old_doc, new_doc)
            return new_doc if return_document else old_doc
        return None

    # =========================================================================
    # 3. UPDATE OPERATIONS
    # =========================================================================

    def update_one(
        self, filter: Mapping[str, Any], update: Mapping[str, Any], upsert: bool = False
    ) -> UpdateResult:
        """Update a single document matching `filter` with $set/$unset/$inc."""
        validate_update_document(update)
        for offset, old_doc in self._iter_matches(filter):
            new_doc = apply_update_operators(old_doc, update)
            return self._apply_single_update(offset, old_doc, new_doc)
        if upsert:
            return self._upsert(filter, update)
        return UpdateResult(0, 0, None)

    def update_many(
        self, filter: Mapping[str, Any], update: Mapping[str, Any], upsert: bool = False
    ) -> UpdateResult:
        """Update every document matching `filter`. Matches are gathered
        into a list before any mutation starts: tombstoning/appending
        changes data_end and can trigger an mmap resize mid-iteration,
        so scanning and mutating can't safely interleave."""
        validate_update_document(update)
        targets = list(self._iter_matches(filter))
        if not targets:
            if upsert:
                return self._upsert(filter, update)
            return UpdateResult(0, 0, None)
        modified = 0
        for offset, old_doc in targets:
            new_doc = apply_update_operators(old_doc, update)
            modified += self._apply_single_update(offset, old_doc, new_doc).modified_count
        return UpdateResult(len(targets), modified, None)

    def replace_one(
        self, filter: Mapping[str, Any], replacement: Mapping[str, Any], upsert: bool = False
    ) -> UpdateResult:
        """Replace a single document completely while preserving its `_id`."""
        _reject_update_operators(replacement, "replace_one")
        for offset, old_doc in self._iter_matches(filter):
            new_doc = _replacement_with_preserved_id(replacement, old_doc, "replace_one")
            return self._apply_single_update(offset, old_doc, new_doc)
        if upsert:
            seeded = seed_from_filter(filter)
            seeded.update(replacement)
            if "_id" not in seeded:
                seeded["_id"] = ObjectId()
            offset = self._handle.append_record(encode(seeded))
            try:
                self._index_update_for_insert(seeded, offset)
            except DuplicateKeyError:
                self._handle.tombstone_record(offset)
                raise
            return UpdateResult(0, 0, seeded["_id"])
        return UpdateResult(0, 0, None)

    # =========================================================================
    # 4. DELETE OPERATIONS
    # =========================================================================

    def delete_one(self, filter: Mapping[str, Any]) -> DeleteResult:
        """Delete a single document matching `filter`."""
        for offset, doc in self._iter_matches(filter):
            self._handle.tombstone_record(offset)
            self._index_update_for_delete(doc, offset)
            return DeleteResult(1)
        return DeleteResult(0)

    def delete_many(self, filter: Mapping[str, Any]) -> DeleteResult:
        """Delete every document matching `filter`. Matches are gathered
        before any mutation starts, for the same reason as update_many."""
        targets = list(self._iter_matches(filter))
        for offset, doc in targets:
            self._handle.tombstone_record(offset)
            self._index_update_for_delete(doc, offset)
        return DeleteResult(len(targets))

    # =========================================================================
    # 5. AGGREGATION & METRICS
    # =========================================================================

    def count_documents(self, filter: Optional[Mapping[str, Any]] = None) -> int:
        """Exact count of documents matching `filter` (a full or
        index-narrowed scan, per _iter_matches -- never approximate)."""
        return sum(1 for _ in self._iter_matches(filter))

    def estimated_document_count(self) -> int:
        """Fast, approximate count: the storage header's live-record
        counter (maintained incrementally on insert/delete), with no
        scan at all."""
        return self._handle.live_count()

    def distinct(self, key: str, filter: Optional[Mapping[str, Any]] = None) -> List[Any]:
        """Unique values of `key` (dot-notation supported) across
        documents matching `filter`. An array-valued field contributes
        its elements, not the array itself."""
        segments = key.split(".")
        results: List[Any] = []
        for _offset, doc in self._iter_matches(filter):
            for value in _distinct_candidates(doc, segments):
                if not any(_values_equal(value, existing) for existing in results):
                    results.append(value)
        return results

    # =========================================================================
    # 6. INDEX MANAGEMENT
    # =========================================================================

    def create_index(self, keys: Any, unique: bool = False) -> str:
        """Builds a persisted B-Tree index (see include/custom_bson/btree.h)
        over a single field. `keys` is a field name, a single-entry
        mapping (``{"field": 1}``/``{"field": -1}``), or a single-entry
        list of ``(field, direction)`` -- compound indexes are not
        supported yet. Returns the generated index name (e.g. "age_1")."""
        return index_module.create_index(self, keys, unique)

    def drop_index(self, index_name: str) -> None:
        index_module.drop_index(self, index_name)

    def list_indexes(self) -> List[Dict[str, Any]]:
        return index_module.list_indexes(self)

    # =========================================================================
    # 7. COLLECTION LIFECYCLE & MAINTENANCE
    # =========================================================================

    def drop(self) -> None:
        """Drop the collection and delete its backing file from disk."""
        self._database._client._drop_collection(self._database._name, self._name, self._file_path)

    def compact(self) -> None:
        """Rewrite the collection file to remove tombstoned records and
        reclaim disk space, via a sequential scan-and-reappend into a
        fresh file swapped in with os.replace() for atomicity.

        Every existing index's entries reference offsets in the file
        being replaced, so each index is dropped and rebuilt from
        scratch against the recompacted file -- this is also the only
        reclamation path for the B-Tree's tombstone-only delete (see
        include/custom_bson/btree.h)."""
        client = self._database._client
        handle = self._handle
        tmp_path = self._file_path + ".compact_tmp"
        if os.path.exists(tmp_path):
            os.remove(tmp_path)

        tmp_handle = _storage_core.open_collection(tmp_path)
        prev = -1
        while True:
            offset = handle.next_live_offset(prev)
            if offset is None:
                break
            prev = offset
            raw = handle.read_record(offset)
            if raw is None:
                continue
            tmp_handle.append_record(raw)
        tmp_handle.close()

        existing_indexes = index_module.list_indexes(self)

        client._invalidate_handle(self._database._name, self._name)
        os.replace(tmp_path, self._file_path)

        for spec in existing_indexes:
            index_module.drop_index(self, spec["name"])
        for spec in existing_indexes:
            (field_path, direction), = spec["key"].items()
            self.create_index({field_path: direction}, unique=spec["unique"])

    def __repr__(self) -> str:
        return f"Collection({self._database!r}, {self._name!r})"
