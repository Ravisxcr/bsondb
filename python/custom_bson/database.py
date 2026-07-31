"""Database: a named group of collections, backed by a directory on disk.

A Database's on-disk presence is just its subdirectory under the
MongoClient's data directory (``<client.path>/<db_name>/``); the
directory itself is created lazily by MongoClient._get_handle() on
first collection access, not here.
"""

from __future__ import annotations

from typing import Any

from .collection import Collection


class Database:
    """A named database within a :class:`~custom_bson.client.MongoClient`."""

    def __init__(self, client: Any, name: str) -> None:
        self._client = client
        self._name = name

    @property
    def name(self) -> str:
        return self._name

    def __getattr__(self, name: str) -> Collection:
        if name.startswith("_"):
            raise AttributeError(name)
        return self[name]

    def __getitem__(self, name: str) -> Collection:
        return Collection(self, name)

    def __repr__(self) -> str:
        return f"Database({self._client!r}, {self._name!r})"
