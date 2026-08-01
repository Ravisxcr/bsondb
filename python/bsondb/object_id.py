"""12-byte BSON ObjectId.

Implemented in pure Python rather than C: the BSON-layer cost of an
ObjectId is just 12 raw bytes in/out (a memcpy plus one constructor
call from the native engine), which doesn't benefit meaningfully from a
C implementation. The parts of ObjectId that *are* nontrivial --
hex parsing, ordering, ``generation_time`` -- are simpler to write and
maintain here and are not on the performance-critical encode/decode
path. See docs/api_reference.md.

Generation follows the modern (post-2018) BSON ObjectId spec: a 4-byte
big-endian Unix timestamp, 5 random bytes, and a 3-byte counter that is
random-seeded and incremented per process, guarded by a lock so
concurrent generation in one process never collides.
"""

from __future__ import annotations

import binascii
import datetime
import os
import threading
import time
from typing import Union

_ObjectIdLike = Union["ObjectId", bytes, str]

_counter_lock = threading.Lock()
_counter = int.from_bytes(os.urandom(3), "big")


def _next_counter() -> int:
    global _counter
    with _counter_lock:
        _counter = (_counter + 1) % 0xFFFFFF
        return _counter


class ObjectId:
    """A 12-byte unique identifier, matching MongoDB/BSON's ObjectId format."""

    __slots__ = ("_binary",)

    def __init__(self, oid: _ObjectIdLike | None = None) -> None:
        if oid is None:
            self._binary = self._generate()
        elif isinstance(oid, ObjectId):
            self._binary = oid._binary
        elif isinstance(oid, (bytes, bytearray)):
            if len(oid) != 12:
                raise ValueError(f"ObjectId binary representation must be 12 bytes, got {len(oid)}")
            self._binary = bytes(oid)
        elif isinstance(oid, str):
            self._binary = self._from_hex(oid)
        else:
            raise TypeError(f"cannot create ObjectId from type '{type(oid).__name__}'")

    @staticmethod
    def _generate() -> bytes:
        timestamp = int(time.time()).to_bytes(4, "big")
        random_bytes = os.urandom(5)
        counter_bytes = _next_counter().to_bytes(3, "big")
        return timestamp + random_bytes + counter_bytes

    @staticmethod
    def _from_hex(value: str) -> bytes:
        if len(value) != 24:
            raise ValueError(f"ObjectId hex string must be 24 characters, got {len(value)}")
        try:
            return binascii.unhexlify(value)
        except binascii.Error as exc:
            raise ValueError(f"invalid ObjectId hex string: {value!r}") from exc

    @property
    def binary(self) -> bytes:
        """The raw 12-byte representation (what the BSON wire format stores)."""
        return self._binary

    @property
    def generation_time(self) -> datetime.datetime:
        """UTC datetime this ObjectId's embedded timestamp represents."""
        seconds = int.from_bytes(self._binary[:4], "big")
        return datetime.datetime.fromtimestamp(seconds, tz=datetime.timezone.utc)

    def __bytes__(self) -> bytes:
        return self._binary

    def __str__(self) -> str:
        return binascii.hexlify(self._binary).decode("ascii")

    def __repr__(self) -> str:
        return f"ObjectId('{self}')"

    def __eq__(self, other: object) -> bool:
        if isinstance(other, ObjectId):
            return self._binary == other._binary
        return NotImplemented

    def __ne__(self, other: object) -> bool:
        result = self.__eq__(other)
        if result is NotImplemented:
            return result
        return not result

    def __lt__(self, other: "ObjectId") -> bool:
        if not isinstance(other, ObjectId):
            return NotImplemented
        return self._binary < other._binary

    def __le__(self, other: "ObjectId") -> bool:
        if not isinstance(other, ObjectId):
            return NotImplemented
        return self._binary <= other._binary

    def __gt__(self, other: "ObjectId") -> bool:
        if not isinstance(other, ObjectId):
            return NotImplemented
        return self._binary > other._binary

    def __ge__(self, other: "ObjectId") -> bool:
        if not isinstance(other, ObjectId):
            return NotImplemented
        return self._binary >= other._binary

    def __hash__(self) -> int:
        return hash(self._binary)
