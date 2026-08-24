"""B-Tree index key encoding and create/drop/list orchestration.

Index files live alongside a collection's data file, named
``<collection>.<index_name>.bidx``. The filename itself encodes
(collection, index_name), so list_indexes() can discover indexes via a
directory listing with no separate manifest file that could go stale.
"""

from __future__ import annotations

import datetime
import os
import struct
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple, Union

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

ASCENDING = 1
DESCENDING = -1

_EPOCH_UTC = datetime.datetime(1970, 1, 1, tzinfo=datetime.timezone.utc)
_MISSING = object()

IndexKeySpec = Union[
    str,
    Mapping[str, Any],
    Sequence[Tuple[str, Any]],
]


def _datetime_to_epoch_ms(value: datetime.datetime) -> int:
    if value.tzinfo is None:
        value = value.replace(tzinfo=datetime.timezone.utc)
    delta = value.astimezone(datetime.timezone.utc) - _EPOCH_UTC
    return delta.days * 86400000 + delta.seconds * 1000 + delta.microseconds // 1000


def encode_btree_key(value: Any) -> bytes:
    if value is None:
        return bytes([TAG_NULL]) + b"\x00" * 12
    if isinstance(value, bool):
        return bytes([TAG_BOOL, 1 if value else 0]) + b"\x00" * 11
    if isinstance(value, int):
        if not (-(2**63) <= value < 2**63):
            raise OverflowError("integer value out of range for an index key (max 64-bit signed)")
        return bytes([TAG_INT]) + struct.pack("<q", value) + b"\x00" * 4
    if isinstance(value, float):
        if value != value:
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
    cursor = doc
    for segment in segments:
        if isinstance(cursor, dict) and segment in cursor:
            cursor = cursor[segment]
        else:
            return _MISSING
    return cursor


def resolve_index_value(doc: Mapping[str, Any], field_path: str) -> Any:
    value = _direct_value(doc, field_path.split("."))
    return None if value is _MISSING else value


def _direction_is_descending(direction: Any) -> bool:
    return direction in (DESCENDING, -1, "descending", "desc")


def normalize_index_spec(keys: IndexKeySpec) -> Tuple[str, bool]:
    """Normalizes field specifiers into (field_path, descending)."""
    if isinstance(keys, str):
        return keys, False

    if isinstance(keys, Mapping):
        items = list(keys.items())
        if len(items) != 1:
            raise BSONNotImplementedError("compound indexes are not supported yet; pass a single field")
        field, direction = items[0]
        return field, _direction_is_descending(direction)

    if isinstance(keys, (list, tuple)):
        if len(keys) != 1:
            raise BSONNotImplementedError("compound indexes are not supported yet; pass a single field")
        first_item = keys[0]
        if isinstance(first_item, (list, tuple)) and len(first_item) == 2:
            field, direction = first_item
            return str(field), _direction_is_descending(direction)
        if isinstance(first_item, str):
            return first_item, False

    raise InvalidDocument(
        "keys must be a field name, a single-entry mapping, "
        "or a single-entry list of (field, direction) tuples"
    )


def default_index_name(field_path: str, descending: bool) -> str:
    """Generates PyMongo standard name format '<field>_1' or '<field>_-1'."""
    return f"{field_path}_{-1 if descending else 1}"


def index_file_path(db_dir: str, collection_name: str, index_name: str) -> str:
    return os.path.join(db_dir, f"{collection_name}.{index_name}.bidx")


def _resolve_index_name_for_drop(collection: Any, index_or_name: Any) -> str:
    """Resolves index identifier to an existing disk file name following PyMongo rules:
    
    1. If a non-string spec is provided (dict/list), generate standard name '<field>_<dir>'.
    2. If a string is provided:
       a. Matches exact index name directly on disk (supports custom names & standard names).
       b. Fallback: treats string as bare field name and checks for '<field>_1' or '<field>_-1'.
    """
    db_dir = os.path.dirname(collection._file_path)

    if not isinstance(index_or_name, str):
        field_path, descending = normalize_index_spec(index_or_name)
        return default_index_name(field_path, descending)

    # String input: check direct filename match first
    direct_path = index_file_path(db_dir, collection._name, index_or_name)
    if os.path.exists(direct_path):
        return index_or_name

    # Check default directional suffixes if caller passed a bare field name
    for descending in (False, True):
        candidate_name = default_index_name(index_or_name, descending)
        if os.path.exists(index_file_path(db_dir, collection._name, candidate_name)):
            return candidate_name

    return index_or_name


def create_index(
    collection: Any,
    keys: IndexKeySpec,
    unique: bool = False,
    name: Optional[str] = None,
) -> str:
    """Builds a persisted B-Tree index for `collection`."""
    field_path, descending = normalize_index_spec(keys)
    index_name = name if name is not None else default_index_name(field_path, descending)
    
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
        if os.path.exists(path):
            os.remove(path)
        raise
    handle.close()

    collection._database._client._invalidate_index_listing(
        collection._database._name, collection._name
    )
    return index_name


def drop_index(collection: Any, index_or_name: Any) -> None:
    """Drops an index by exact name, bare field name, or index key spec."""
    index_name = _resolve_index_name_for_drop(collection, index_or_name)
    db_dir = os.path.dirname(collection._file_path)
    path = index_file_path(db_dir, collection._name, index_name)
    client = collection._database._client

    client._invalidate_index_handle(collection._database._name, collection._name, index_name)

    if not os.path.exists(path):
        raise InvalidDocument(f"index '{index_or_name}' does not exist")

    os.remove(path)
    client._invalidate_index_listing(collection._database._name, collection._name)


def drop_indexes(collection: Any) -> None:
    """Drops all non-system indexes on the collection."""
    for idx in list_indexes(collection):
        drop_index(collection, idx["name"])


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