"""Database: a named group of collections, backed by a directory on disk.

A Database's on-disk presence is just its subdirectory under the
BsonDBClient's data directory (``<client.path>/<db_name>/``); the
directory itself is created lazily by BsonDBClient._get_handle() on
first collection access, not here.
"""

from __future__ import annotations

import os
from typing import Any, Union

from .collection import Collection


class Database:
    """A named database within a :class:`~bsondb.client.BsonDBClient`."""

    def __init__(self, client: Any, name: str) -> None:
        self._client = client
        self._name = name

    @property
    def name(self) -> str:
        return self._name

    def list_collection_names(self) -> list[str]:
        return self._client._list_collection_names(self._name)

    def drop_collection(self, name_or_collection: Union[str, Collection]) -> None:
        if isinstance(name_or_collection, Collection):
            coll = name_or_collection
        elif isinstance(name_or_collection, str):
            coll = self[name_or_collection]
        else:
            raise TypeError(
                f"name_or_collection must be an instance of str or Collection, not {type(name_or_collection).__name__}"
            )

        coll.drop()


    def __getattr__(self, name: str) -> Collection:
        if name.startswith("_"):
            raise AttributeError(name)
        return self[name]

    def __getitem__(self, name: str) -> Collection:
        return Collection(self, name)

    def __repr__(self) -> str:
        return f"Database({self._client!r}, {self._name!r})"
