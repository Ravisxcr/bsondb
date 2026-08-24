"""Exception hierarchy raised by bsondb's encode/decode engine and
storage/query layer."""

from __future__ import annotations


class BSONError(Exception):
    """Base class for all bsondb errors."""


class InvalidBSON(BSONError):
    """Raised when decode() is given malformed, truncated, or corrupt BSON bytes."""


class InvalidDocument(BSONError):
    """Raised when encode() is given a Python object graph that cannot be
    represented as BSON (unsupported type, non-str key, embedded-NUL key)."""


class BSONNotImplementedError(BSONError):
    """Raised when decode() encounters a structurally valid BSON type that
    this version does not yet support materializing (e.g. Regex, Timestamp,
    Decimal128, MinKey/MaxKey). See docs/wire_protocol.md for the full list."""


class DocumentTooLarge(BSONError):
    """Raised when an encoded document would exceed the maximum BSON
    document size (16 MiB)."""


class InvalidUpdateDocument(BSONError):
    """Raised when an update document passed to update_one/update_many/
    find_one_and_update is not built entirely from $set/$unset/$inc."""


class DuplicateKeyError(BSONError):
    """Raised when an insert or update would violate a unique index."""

class OperationFailure(Exception):
    """Raised when a database operation fails (e.g. index not found)."""
    pass
