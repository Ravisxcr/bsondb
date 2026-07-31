"""B-Tree index key encoding and create/drop/list orchestration.

Index files live alongside a collection's data file, named
``<collection>.<index_name>.bidx``. The filename itself encodes
(collection, index_name), so list_indexes() can discover indexes via a
directory listing with no separate manifest file that could go stale.

Key encoding/decoding from typed Python values lives here, not in the
C layer -- see include/custom_bson/btree.h and src/python_bindings/
_storage_core.c's IndexHandle, which only ever handle opaque
BSON_BTREE_KEY_SIZE-byte blobs.
"""

from __future__ import annotations

import datetime
import os
import struct
from typing import Any, Dict, List, Mapping, Tuple

from . import _storage_core
from .exceptions import BSONNotImplementedError, InvalidDocument
from .object_id import ObjectId

BTREE_KEY_SIZE = 13

TAG_NULL = 0
TAG_BOOL = 1
TAG_INT = 2
TAG_DOUBLE = 3
TAG_DATETIME = 4
TAG_OBJECTID = 5

_EPOCH_UTC = datetime.datetime(1970, 1, 1, tzinfo=datetime.timezone.utc)
_MISSING = object()


def _datetime_to_epoch_ms(dt: datetime.datetime) -> int:
    """Mirrors _bson_core.c's encode-side datetime convention (naive
    datetimes treated as UTC) so a document's indexed key always
    matches the epoch-ms value that would be stored in its encoded
    BSON -- if these two conversions ever diverged, index lookups
    could silently miss documents."""
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=datetime.timezone.utc)
    delta = dt.astimezone(datetime.timezone.utc) - _EPOCH_UTC
    return delta.days * 86400000 + delta.seconds * 1000 + delta.microseconds // 1000


def encode_btree_key(value: Any) -> bytes:
    """Encodes a Python value into the fixed 13-byte B-Tree key format.
    Only fixed-width-comparable scalar types are supported for
    indexing this slice -- str/bytes/list/dict raise
    BSONNotImplementedError (matches docs/wire_protocol.md's framing of
    "valid but not supported yet")."""
    if value is None:
        return bytes([TAG_NULL]) + b"\x00" * 12
    if isinstance(value, bool):  # must precede the int check: bool is an int subclass
        return bytes([TAG_BOOL, 1 if value else 0]) + b"\x00" * 11
    if isinstance(value, int):
        if not (-(2**63) <= value < 2**63):
            raise OverflowError("integer value out of range for an index key (max 64-bit signed)")
        return bytes([TAG_INT]) + struct.pack("<q", value) + b"\x00" * 4
    if isinstance(value, float):
        if value != value:  # NaN has no defined order
            raise ValueError("cannot index a NaN value")
        return bytes([TAG_DOUBLE]) + struct.pack("<d", value) + b"\x00" * 4
    if isinstance(value, datetime.datetime):
        ms = _datetime_to_epoch_ms(value)
        return bytes([TAG_DATETIME]) + struct.pack("<q", ms) + b"\x00" * 4
    if isinstance(value, ObjectId):
        return bytes([TAG_OBJECTID]) + value.binary
    raise BSONNotImplementedError(
        f"indexing values of type '{type(value).__name__}' is not supported yet"
    )


def _direct_value(doc: Any, segments: List[str]) -> Any:
    """Non-broadcasting field lookup for indexing: unlike
    query.resolve_path (which broadcasts over list elements for
    containment queries), an index needs exactly the value present at
    the path (or _MISSING). A list/dict value at the path falls
    through to encode_btree_key's final raise, since neither is a
    supported fixed-width key type."""
    cursor = doc
    for seg in segments:
        if isinstance(cursor, dict) and seg in cursor:
            cursor = cursor[seg]
        else:
            return _MISSING
    return cursor


def resolve_index_value(doc: Mapping[str, Any], field_path: str) -> Any:
    """The value to index for `doc`: None when the field is absent, so
    every index is effectively sparse-with-a-NULL-key rather than
    skipping the document entirely -- avoids "sometimes indexed,
    sometimes not" surprises based on field presence."""
    value = _direct_value(doc, field_path.split("."))
    return None if value is _MISSING else value


def normalize_index_spec(keys: Any) -> Tuple[str, bool]:
    """Normalizes `keys` (a field name, a single-entry dict, or a
    single-entry list of (field, direction) tuples) to
    (field_path, descending). Single-field indexes only this slice --
    anything with more than one field raises BSONNotImplementedError
    (compound-key comparison is a meaningfully bigger algorithm, out of
    scope here)."""
    if isinstance(keys, str):
        return keys, False
    if isinstance(keys, Mapping):
        if len(keys) != 1:
            raise BSONNotImplementedError("compound indexes are not supported yet; pass a single field")
        ((field, direction),) = keys.items()
        return field, _direction_is_descending(direction)
    if isinstance(keys, (list, tuple)):
        if len(keys) != 1:
            raise BSONNotImplementedError("compound indexes are not supported yet; pass a single field")
        field, direction = keys[0]
        return field, _direction_is_descending(direction)
    raise InvalidDocument(
        "create_index() keys must be a field name, a single-entry mapping, "
        "or a single-entry list of (field, direction) tuples"
    )


def _direction_is_descending(direction: Any) -> bool:
    return direction in (-1, "descending", "desc")


def default_index_name(field_path: str, descending: bool) -> str:
    return f"{field_path}_{-1 if descending else 1}"


def index_file_path(db_dir: str, collection_name: str, index_name: str) -> str:
    return os.path.join(db_dir, f"{collection_name}.{index_name}.bidx")


def create_index(collection: Any, keys: Any, unique: bool) -> str:
    """Builds (or, if an identical index already exists, returns the
    name of) a persisted B-Tree index for `collection`. All-or-nothing:
    on any failure partway through the initial build scan, the partial
    index file is deleted rather than left in a half-built state."""
    field_path, descending = normalize_index_spec(keys)
    index_name = default_index_name(field_path, descending)
    db_dir = os.path.dirname(collection._file_path)
    path = index_file_path(db_dir, collection._name, index_name)

    if os.path.exists(path):
        existing = _storage_core.open_index_file(path)
        try:
            same_spec = (
                existing.field_path == field_path
                and existing.unique == unique
                and existing.descending == descending
            )
        finally:
            existing.close()
        if same_spec:
            return index_name
        raise InvalidDocument(f"index '{index_name}' already exists with a different specification")

    key_type_tag = TAG_NULL
    for _offset, doc in collection._iter_matches({}):
        value = resolve_index_value(doc, field_path)
        if value is not None:
            key_type_tag = encode_btree_key(value)[0]
            break

    handle = _storage_core.create_index_file(path, field_path, key_type_tag, unique, descending)
    try:
        for offset, doc in collection._iter_matches({}):
            value = resolve_index_value(doc, field_path)
            handle.insert(encode_btree_key(value), offset)
        handle.flush()
    except Exception:
        handle.close()
        os.remove(path)
        raise
    handle.close()
    return index_name


def drop_index(collection: Any, index_name: str) -> None:
    db_dir = os.path.dirname(collection._file_path)
    path = index_file_path(db_dir, collection._name, index_name)
    client = collection._database._client
    client._invalidate_index_handle(collection._database._name, collection._name, index_name)
    if not os.path.exists(path):
        raise InvalidDocument(f"index '{index_name}' does not exist")
    os.remove(path)


def list_indexes(collection: Any) -> List[Dict[str, Any]]:
    db_dir = os.path.dirname(collection._file_path)
    prefix = f"{collection._name}."
    results: List[Dict[str, Any]] = []
    if not os.path.isdir(db_dir):
        return results
    for fname in sorted(os.listdir(db_dir)):
        if not (fname.startswith(prefix) and fname.endswith(".bidx")):
            continue
        index_name = fname[len(prefix) : -len(".bidx")]
        handle = _storage_core.open_index_file(os.path.join(db_dir, fname))
        try:
            results.append(
                {
                    "name": index_name,
                    "key": {handle.field_path: -1 if handle.descending else 1},
                    "unique": handle.unique,
                }
            )
        finally:
            handle.close()
    return results
