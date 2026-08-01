"""Result types returned by Collection's write operations."""

from __future__ import annotations

from typing import List, Optional

from .object_id import ObjectId


class InsertOneResult:
    __slots__ = ("inserted_id",)

    def __init__(self, inserted_id: ObjectId) -> None:
        self.inserted_id = inserted_id

    def __repr__(self) -> str:
        return f"InsertOneResult(inserted_id={self.inserted_id!r})"


class InsertManyResult:
    __slots__ = ("inserted_ids",)

    def __init__(self, inserted_ids: List[ObjectId]) -> None:
        self.inserted_ids = inserted_ids

    def __repr__(self) -> str:
        return f"InsertManyResult(inserted_ids={self.inserted_ids!r})"


class UpdateResult:
    __slots__ = ("matched_count", "modified_count", "upserted_id")

    def __init__(self, matched_count: int, modified_count: int, upserted_id: Optional[ObjectId]) -> None:
        self.matched_count = matched_count
        self.modified_count = modified_count
        self.upserted_id = upserted_id

    def __repr__(self) -> str:
        return (
            f"UpdateResult(matched_count={self.matched_count!r}, "
            f"modified_count={self.modified_count!r}, upserted_id={self.upserted_id!r})"
        )


class DeleteResult:
    __slots__ = ("deleted_count",)

    def __init__(self, deleted_count: int) -> None:
        self.deleted_count = deleted_count

    def __repr__(self) -> str:
        return f"DeleteResult(deleted_count={self.deleted_count!r})"
