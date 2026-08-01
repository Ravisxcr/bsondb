"""Query filter evaluation.

Predicate matching runs on a decoded Python dict (``custom_bson.decode()``
-> ``matches(doc, filter)``), not as a raw-bytes C evaluator over
``bson_iter_t``. See docs/api_reference.md for why: a C-level
dot-notation + operator + array-broadcast predicate evaluator is a
large, separable subsystem on its own, and the B-Tree index (added in
a later slice) is the real performance lever for the queries that
matter -- full-scan-then-Python-filter is exactly what an unindexed
MongoDB query does too (COLLSCAN).
"""

from __future__ import annotations

import copy
import operator as _operator
from typing import Any, Dict, List, Mapping, Sequence

from .exceptions import InvalidDocument, InvalidUpdateDocument

_COMPARISON_FUNCS = {
    "$gt": _operator.gt,
    "$gte": _operator.ge,
    "$lt": _operator.lt,
    "$lte": _operator.le,
}


def resolve_path(value: Any, segments: Sequence[str]) -> List[Any]:
    """Returns every value reachable by following dot-notation `segments`
    through nested dicts/lists. An empty result means "missing".

    Matches MongoDB's dot-notation semantics: a numeric segment can
    index into a list, and a remaining path is also broadcast over
    every list element (so ``"tags.name"`` matches a list of dicts).
    With no segments left, a list value itself contributes both as a
    whole (for exact-match queries) and via each element (for
    containment queries like ``{"tags": "x"}``).
    """
    if not segments:
        if isinstance(value, list):
            return [value, *value]
        return [value]

    segment, rest = segments[0], segments[1:]

    if isinstance(value, dict):
        if segment in value:
            return resolve_path(value[segment], rest)
        return []

    if isinstance(value, list):
        candidates: List[Any] = []
        if segment.lstrip("-").isdigit():
            idx = int(segment)
            if -len(value) <= idx < len(value):
                candidates.extend(resolve_path(value[idx], rest))
        for elem in value:
            candidates.extend(resolve_path(elem, rest))
        return candidates

    return []


def _values_equal(value_a: Any, value_b: Any) -> bool:
    # bool is an int subclass in Python; BSON keeps them distinct types,
    # so True must not equal 1 here the way it would with bare `==`.
    if isinstance(value_a, bool) != isinstance(value_b, bool):
        return False
    try:
        return value_a == value_b
    except TypeError:
        return False


def _matches_eq(candidates: List[Any], operand: Any) -> bool:
    if operand is None and not candidates:
        # Querying null also matches a missing field (matches MongoDB's
        # documented behavior).
        return True
    return any(_values_equal(candidate, operand) for candidate in candidates)


def _matches_compare(candidates: List[Any], operand: Any, func) -> bool:
    for candidate in candidates:
        if isinstance(candidate, bool) or isinstance(operand, bool):
            continue  # booleans have no meaningful ordering here
        try:
            if func(candidate, operand):
                return True
        except TypeError:
            continue  # cross-type comparison: treat as non-matching, not an error
    return False


def _matches_in(candidates: List[Any], operand: Any) -> bool:
    if not isinstance(operand, (list, tuple)):
        raise InvalidDocument("$in/$nin operand must be a list")
    return any(_matches_eq(candidates, candidate_value) for candidate_value in operand)


def _field_matches(candidates: List[Any], condition: Any) -> bool:
    if isinstance(condition, dict) and condition and all(key.startswith("$") for key in condition):
        for op, operand in condition.items():
            if op == "$eq":
                ok = _matches_eq(candidates, operand)
            elif op == "$ne":
                ok = not _matches_eq(candidates, operand)
            elif op in _COMPARISON_FUNCS:
                ok = _matches_compare(candidates, operand, _COMPARISON_FUNCS[op])
            elif op == "$in":
                ok = _matches_in(candidates, operand)
            elif op == "$nin":
                ok = not _matches_in(candidates, operand)
            elif op == "$not":
                ok = not _field_matches(candidates, operand)
            else:
                raise InvalidDocument(f"unsupported query operator '{op}'")
            if not ok:
                return False
        return True

    # Plain literal: scalar/list/embedded-doc equality (or containment,
    # via resolve_path's list broadcasting).
    return _matches_eq(candidates, condition)


def matches(doc: Mapping[str, Any], filter: Mapping[str, Any]) -> bool:
    """Evaluates a MongoDB-style filter against a decoded document.

    Top-level keys are implicitly AND'd. ``$and``/``$or`` recurse over
    lists of sub-filters. ``$not`` is a per-field operator modifier
    (``{"field": {"$not": {...}}}``), not a top-level combinator.
    """
    for key, condition in filter.items():
        if key == "$and":
            if not isinstance(condition, (list, tuple)):
                raise InvalidDocument("$and requires a list of filter documents")
            if not all(matches(doc, sub) for sub in condition):
                return False
        elif key == "$or":
            if not isinstance(condition, (list, tuple)):
                raise InvalidDocument("$or requires a list of filter documents")
            if not any(matches(doc, sub) for sub in condition):
                return False
        elif key.startswith("$"):
            raise InvalidDocument(f"unsupported top-level query operator '{key}'")
        else:
            candidates = resolve_path(doc, key.split("."))
            if not _field_matches(candidates, condition):
                return False
    return True


# ===========================================================================
# Update operators: $set / $unset / $inc
# ===========================================================================

_UPDATE_OPERATORS = {"$set", "$unset", "$inc"}
_MISSING = object()


def validate_update_document(update: Mapping[str, Any]) -> None:
    """Raises InvalidUpdateDocument unless `update` is built entirely
    from $set/$unset/$inc (each mapping field-paths to values) --
    matches real pymongo's own rejection of raw replacement-shaped
    documents passed to update_one/update_many."""
    if not isinstance(update, Mapping) or not update:
        raise InvalidUpdateDocument("update document must be a non-empty mapping")
    for key, value in update.items():
        if key not in _UPDATE_OPERATORS:
            raise InvalidUpdateDocument(
                f"update document must consist only of $set/$unset/$inc operators, got '{key}'"
            )
        if not isinstance(value, Mapping) or not value:
            raise InvalidUpdateDocument(f"'{key}' requires a non-empty mapping of field paths to values")


def _set_path(doc: Dict[str, Any], path: str, value: Any) -> None:
    segments = path.split(".")
    cursor = doc
    for segment in segments[:-1]:
        next_value = cursor.get(segment)
        if not isinstance(next_value, dict):
            next_value = {}
            cursor[segment] = next_value
        cursor = next_value
    cursor[segments[-1]] = value


def _unset_path(doc: Dict[str, Any], path: str) -> None:
    segments = path.split(".")
    cursor = doc
    for segment in segments[:-1]:
        next_value = cursor.get(segment)
        if not isinstance(next_value, dict):
            return  # path doesn't exist -- no-op, matches Mongo's $unset semantics
        cursor = next_value
    cursor.pop(segments[-1], None)


def _get_path(doc: Any, path: str) -> Any:
    cursor = doc
    for segment in path.split("."):
        if not isinstance(cursor, dict) or segment not in cursor:
            return _MISSING
        cursor = cursor[segment]
    return cursor


def apply_update_operators(doc: Mapping[str, Any], update: Mapping[str, Any]) -> Dict[str, Any]:
    """Returns a new document with $set/$unset/$inc applied; `doc` is
    never mutated. `_id` is immutable: $set-ing it to a different value
    or $unset-ing/`$inc`-ing it raises."""
    validate_update_document(update)
    new_doc: Dict[str, Any] = copy.deepcopy(dict(doc))
    original_id = new_doc.get("_id", _MISSING)

    for path, value in update.get("$set", {}).items():
        if path == "_id" and original_id is not _MISSING and not _values_equal(value, original_id):
            raise InvalidDocument("_id is immutable and cannot be changed by an update")
        _set_path(new_doc, path, value)

    for path in update.get("$unset", {}):
        if path == "_id":
            raise InvalidDocument("_id is immutable and cannot be removed by an update")
        _unset_path(new_doc, path)

    for path, delta in update.get("$inc", {}).items():
        if path == "_id":
            raise InvalidDocument("_id is immutable and cannot be changed by an update")
        if isinstance(delta, bool) or not isinstance(delta, (int, float)):
            raise InvalidDocument(f"$inc value for '{path}' must be numeric")
        current = _get_path(new_doc, path)
        if current is _MISSING:
            current = 0
        elif isinstance(current, bool) or not isinstance(current, (int, float)):
            raise InvalidDocument(f"cannot $inc non-numeric field '{path}'")
        _set_path(new_doc, path, current + delta)

    return new_doc


def seed_from_filter(filter: Mapping[str, Any]) -> Dict[str, Any]:
    """Builds an upsert seed document from a filter's top-level plain-
    equality/$eq clauses (dot-path expanded). Clauses using any other
    operator ($gt/$in/$ne/...) or top-level $and/$or can't meaningfully
    seed a new document and are skipped -- mirrors real pymongo's own
    upsert seeding behavior."""
    seed: Dict[str, Any] = {}
    for key, condition in filter.items():
        if key.startswith("$"):
            continue
        if isinstance(condition, Mapping):
            if set(condition.keys()) == {"$eq"}:
                _set_path(seed, key, condition["$eq"])
        else:
            _set_path(seed, key, condition)
    return seed


def build_upsert_document(filter: Mapping[str, Any], update: Mapping[str, Any]) -> Dict[str, Any]:
    """Filter-seeded document with the update's $set/$inc/$unset applied
    on top -- the document an upsert inserts when nothing matched."""
    seed = seed_from_filter(filter)
    return apply_update_operators(seed, update)
