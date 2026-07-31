"""Lazy iterator over Collection.find() results."""

from __future__ import annotations

from typing import Any, Dict, Iterator, Mapping, Optional, Sequence, Union

ProjectionSpec = Union[Mapping[str, bool], Sequence[str]]


def _normalize_projection(projection: Optional[ProjectionSpec]) -> Optional[Dict[str, bool]]:
    if projection is None:
        return None
    if isinstance(projection, Mapping):
        spec = {k: bool(v) for k, v in projection.items()}
    else:
        spec = {k: True for k in projection}

    include_keys = [k for k, v in spec.items() if v and k != "_id"]
    exclude_keys = [k for k, v in spec.items() if not v and k != "_id"]
    if include_keys and exclude_keys:
        raise ValueError("projection cannot mix inclusion and exclusion (except for '_id')")
    return spec


def apply_projection(doc: Dict[str, Any], projection: Optional[Dict[str, bool]]) -> Dict[str, Any]:
    if not projection:
        return doc
    include_keys = [k for k, v in projection.items() if v and k != "_id"]
    if include_keys:
        result = {k: doc[k] for k in include_keys if k in doc}
        if projection.get("_id", True) and "_id" in doc:
            result["_id"] = doc["_id"]
        return result
    exclude_keys = {k for k, v in projection.items() if not v}
    return {k: v for k, v in doc.items() if k not in exclude_keys}


class Cursor:
    """Lazy iterator over ``Collection.find()`` results.

    Wraps ``Collection._iter_matches()`` (a full sequential scan of live
    records, decoded and filtered on demand -- see query.py) and layers
    skip/limit/projection on top. A future slice's B-Tree index narrows
    what ``_iter_matches()`` scans for eligible filters without changing
    this class's interface.
    """

    def __init__(
        self,
        collection: Any,
        filter: Optional[Mapping[str, Any]] = None,
        projection: Optional[ProjectionSpec] = None,
        skip: int = 0,
        limit: int = 0,
    ) -> None:
        self._matches = collection._iter_matches(filter)
        self._projection = _normalize_projection(projection)
        self._skip = skip
        self._limit = limit
        self._yielded = 0
        self._skipped = 0

    def __iter__(self) -> Iterator[Dict[str, Any]]:
        return self

    def __next__(self) -> Dict[str, Any]:
        if self._limit and self._yielded >= self._limit:
            raise StopIteration
        for _offset, doc in self._matches:
            if self._skipped < self._skip:
                self._skipped += 1
                continue
            self._yielded += 1
            return apply_projection(doc, self._projection)
        raise StopIteration
