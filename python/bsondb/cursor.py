"""Lazy iterator over Collection.find() results."""

from __future__ import annotations

import itertools
from typing import Any, Callable, Dict, Iterator, List, Mapping, Optional, Sequence, Tuple, Union

ProjectionSpec = Union[Mapping[str, bool], Sequence[str]]
SortSpec = Union[str, Sequence[Tuple[str, int]], Mapping[str, int]]


def _normalize_projection(projection: Optional[ProjectionSpec]) -> Optional[Dict[str, bool]]:
    if projection is None:
        return None
    if isinstance(projection, Mapping):
        spec = {field: bool(include) for field, include in projection.items()}
    else:
        spec = {field: True for field in projection}

    include_keys = [field for field, include in spec.items() if include and field != "_id"]
    exclude_keys = [field for field, include in spec.items() if not include and field != "_id"]
    if include_keys and exclude_keys:
        raise ValueError("projection cannot mix inclusion and exclusion (except for '_id')")
    return spec


def _normalize_sort(sort: Optional[SortSpec]) -> Optional[List[Tuple[str, int]]]:
    if sort is None:
        return None
    if isinstance(sort, str):
        return [(sort, 1)]
    if isinstance(sort, Mapping):
        return list(sort.items())
    if isinstance(sort, Sequence):
        normalized = []
        for item in sort:
            if isinstance(item, str):
                normalized.append((item, 1))
            elif isinstance(item, tuple) and len(item) == 2:
                normalized.append((item[0], int(item[1])))
            else:
                raise ValueError(f"Invalid sort item: {item}")
        return normalized
    raise TypeError(f"Invalid sort specification type: {type(sort)}")


def apply_projection(doc: Dict[str, Any], projection: Optional[Dict[str, bool]]) -> Dict[str, Any]:
    if not projection:
        return doc
    include_keys = [field for field, include in projection.items() if include and field != "_id"]
    if include_keys:
        result = {field: doc[field] for field in include_keys if field in doc}
        if projection.get("_id", True) and "_id" in doc:
            result["_id"] = doc["_id"]
        return result
    exclude_keys = {field for field, include in projection.items() if not include}
    return {field: value for field, value in doc.items() if field not in exclude_keys}


class Cursor:
    """Lazy iterator over ``Collection.find()`` results.

    Wraps ``Collection._iter_matches()`` and layers chaining, skip,
    limit, projection, and in-memory sorting.
    """

    def __init__(
        self,
        collection: Any,
        filter: Optional[Mapping[str, Any]] = None,
        projection: Optional[ProjectionSpec] = None,
        skip: int = 0,
        limit: int = 0,
        sort: Optional[SortSpec] = None,
    ) -> None:
        self._collection = collection
        self._filter = filter
        self._projection = _normalize_projection(projection)
        self._raw_projection = projection
        self._skip = max(0, skip)
        self._limit = max(0, limit)
        self._sort = _normalize_sort(sort)
        self._raw_sort = sort

        self._iterator: Optional[Iterator[Dict[str, Any]]] = None
        self._exhausted: bool = False

    def skip(self, count: int) -> Cursor:
        """Sets the number of documents to skip before yielding."""
        if self._iterator is not None:
            raise RuntimeError("Cannot configure cursor after iteration has begun")
        if count < 0:
            raise ValueError("skip count must be non-negative")
        self._skip = count
        return self

    def limit(self, count: int) -> Cursor:
        """Sets the maximum number of documents to yield."""
        if self._iterator is not None:
            raise RuntimeError("Cannot configure cursor after iteration has begun")
        if count < 0:
            raise ValueError("limit count must be non-negative")
        self._limit = count
        return self

    def sort(self, key_or_list: SortSpec, direction: int = 1) -> Cursor:
        """Sorts the results. Accepts field name, dict, or list of (key, direction) tuples."""
        if self._iterator is not None:
            raise RuntimeError("Cannot configure cursor after iteration has begun")
        if isinstance(key_or_list, str):
            self._sort = [(key_or_list, direction)]
            self._raw_sort = key_or_list
        else:
            self._sort = _normalize_sort(key_or_list)
            self._raw_sort = key_or_list
        return self

    def projection(self, spec: Optional[ProjectionSpec]) -> Cursor:
        """Applies or updates the projection specification."""
        if self._iterator is not None:
            raise RuntimeError("Cannot configure cursor after iteration has begun")
        self._projection = _normalize_projection(spec)
        self._raw_projection = spec
        return self

    def rewind(self) -> Cursor:
        """Resets the cursor to its initial un-iterated state."""
        self._iterator = None
        self._exhausted = False
        return self

    def clone(self) -> Cursor:
        """Creates a fresh, un-iterated copy of this cursor with the same query parameters."""
        return Cursor(
            collection=self._collection,
            filter=self._filter,
            projection=self._raw_projection,
            skip=self._skip,
            limit=self._limit,
            sort=self._raw_sort,
        )

    def to_list(self, length: Optional[int] = None) -> List[Dict[str, Any]]:
        """Materializes the cursor into a list, up to optional length."""
        if length is not None:
            if length < 0:
                raise ValueError("length must be non-negative")
            return [doc for _, doc in zip(range(length), self)]
        return list(self)

    @property
    def alive(self) -> bool:
        """True if the cursor has not been exhausted yet."""
        return not self._exhausted

    def _build_iterator(self) -> Iterator[Dict[str, Any]]:
        raw_matches = self._collection._iter_matches(self._filter)

        if self._sort:
            # Materialize for sorting before applying skip and limit
            docs = [doc for _offset, doc in raw_matches]
            for field, direction in reversed(self._sort):
                reverse = direction < 0
                # Sort None/missing values first (or last if reversed)
                docs.sort(key=lambda d: (d.get(field) is None, d.get(field)), reverse=reverse)
            doc_stream = iter(docs)
        else:
            doc_stream = (doc for _offset, doc in raw_matches)

        if self._skip:
            doc_stream = itertools.islice(doc_stream, self._skip, None)

        if self._limit:
            doc_stream = itertools.islice(doc_stream, self._limit)

        for doc in doc_stream:
            yield apply_projection(doc, self._projection)

    def __iter__(self) -> Iterator[Dict[str, Any]]:
        return self

    def __next__(self) -> Dict[str, Any]:
        if self._exhausted:
            raise StopIteration
        if self._iterator is None:
            self._iterator = self._build_iterator()

        try:
            return next(self._iterator)
        except StopIteration:
            self._exhausted = True
            raise


    def __getitem__(self, index: Union[int, slice]) -> Union[Dict[str, Any], List[Dict[str, Any]]]:
        """Supports indexing (cursor[0]) and slicing (cursor[2:5])."""
        if isinstance(index, slice):
            if (index.start is not None and index.start < 0) or (index.stop is not None and index.stop < 0):
                raise IndexError("Negative slice indices are not supported on cursors")
            skip = index.start or 0
            limit = (index.stop - skip) if index.stop is not None else 0
            clone = self.clone()
            clone.skip(skip)
            if limit > 0:
                clone.limit(limit)
            return clone.to_list()

        if isinstance(index, int):
            if index < 0:
                raise IndexError("Negative indexing is not supported on cursors")
            clone = self.clone()
            clone.skip(index)
            clone.limit(1)
            results = clone.to_list()
            if not results:
                raise IndexError("Cursor index out of range")
            return results[0]

        raise TypeError(f"Cursor indices must be integers or slices, not {type(index).__name__}")