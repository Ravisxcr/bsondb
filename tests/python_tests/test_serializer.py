"""Tests for custom_bson.encode / custom_bson.decode.

Covers round-trip correctness for every implemented BSON type, growth
paths (large strings/arrays, deep nesting), and -- since decode treats
its input as fully attacker-controllable -- a wide range of malformed
and corrupted inputs that must raise a BSONError subclass and never
crash the interpreter.
"""

from __future__ import annotations

import datetime
import random
import struct

import pytest

import custom_bson
from custom_bson import ObjectId
from custom_bson.exceptions import BSONError, DocumentTooLarge, InvalidBSON, InvalidDocument


# ---------------------------------------------------------------------------
# Round-trip: scalars
# ---------------------------------------------------------------------------


def roundtrip(doc: dict) -> dict:
    return custom_bson.decode(custom_bson.encode(doc))


def test_empty_document():
    assert roundtrip({}) == {}


@pytest.mark.parametrize(
    "value",
    [0, -1, 1, 2147483647, -2147483648, 2147483648, -2147483649, 2**62, -(2**62)],
)
def test_int_roundtrip(value):
    assert roundtrip({"v": value}) == {"v": value}


def test_int_out_of_range_raises_overflow():
    with pytest.raises(OverflowError):
        custom_bson.encode({"v": 2**63})
    with pytest.raises(OverflowError):
        custom_bson.encode({"v": -(2**63) - 1})


@pytest.mark.parametrize("value", [0.0, -0.0, 1.5, -1.5, 1e300, -1e-300])
def test_float_roundtrip(value):
    out = roundtrip({"v": value})
    assert struct.pack("<d", out["v"]) == struct.pack("<d", value)


def test_float_nan_and_inf_roundtrip():
    out = roundtrip({"a": float("nan"), "b": float("inf"), "c": float("-inf")})
    assert struct.pack("<d", out["b"]) == struct.pack("<d", float("inf"))
    assert struct.pack("<d", out["c"]) == struct.pack("<d", float("-inf"))
    assert out["a"] != out["a"]  # nan


@pytest.mark.parametrize("value", [True, False])
def test_bool_roundtrip(value):
    out = roundtrip({"v": value})
    assert out["v"] is value


@pytest.mark.parametrize(
    "value", ["", "hello", "unicode: éèê", "emoji: \U0001F600", "a\nb\tc"]
)
def test_str_roundtrip(value):
    assert roundtrip({"v": value}) == {"v": value}


def test_bytes_roundtrip():
    assert roundtrip({"v": b"\x00\x01\xff binary"}) == {"v": b"\x00\x01\xff binary"}


def test_bytearray_encodes_as_bytes():
    out = roundtrip({"v": bytearray(b"abc")})
    assert out["v"] == b"abc"
    assert isinstance(out["v"], bytes)


def test_none_roundtrip():
    assert roundtrip({"v": None}) == {"v": None}


def test_objectid_roundtrip():
    oid = ObjectId()
    out = roundtrip({"_id": oid})
    assert out["_id"] == oid
    assert isinstance(out["_id"], ObjectId)


def test_objectid_from_hex_and_str():
    oid = ObjectId("507f1f77bcf86cd799439011")
    assert str(oid) == "507f1f77bcf86cd799439011"


def test_datetime_naive_roundtrip():
    dt = datetime.datetime(2024, 3, 15, 12, 30, 45, 123000)
    out = roundtrip({"v": dt})
    assert out["v"] == dt
    assert out["v"].tzinfo is None


def test_datetime_aware_roundtrip_normalizes_to_naive_utc():
    tz = datetime.timezone(datetime.timedelta(hours=-5))
    dt = datetime.datetime(2024, 3, 15, 7, 30, 0, 0, tzinfo=tz)
    out = roundtrip({"v": dt})
    expected_utc_naive = dt.astimezone(datetime.timezone.utc).replace(tzinfo=None)
    assert out["v"] == expected_utc_naive


def test_datetime_before_epoch():
    dt = datetime.datetime(1950, 1, 1, 0, 0, 0)
    assert roundtrip({"v": dt})["v"] == dt


# ---------------------------------------------------------------------------
# Round-trip: nested structures
# ---------------------------------------------------------------------------


def test_nested_document():
    doc = {"a": {"b": {"c": {"d": 1}}}}
    assert roundtrip(doc) == doc


def test_list_variants():
    assert roundtrip({"v": []}) == {"v": []}
    assert roundtrip({"v": [1, "two", 3.0, None, True]}) == {"v": [1, "two", 3.0, None, True]}
    assert roundtrip({"v": [{"a": 1}, {"b": 2}]}) == {"v": [{"a": 1}, {"b": 2}]}
    assert roundtrip({"v": {"list": [1, 2, [3, 4]]}}) == {"v": {"list": [1, 2, [3, 4]]}}


def test_tuple_encodes_as_list():
    out = custom_bson.decode(custom_bson.encode({"v": (1, 2, 3)}))
    assert out == {"v": [1, 2, 3]}


def test_large_string():
    big = "x" * (1024 * 1024)
    assert roundtrip({"v": big}) == {"v": big}


def test_large_array():
    arr = list(range(100_000))
    assert roundtrip({"v": arr}) == {"v": arr}


def test_unicode_and_special_char_keys_accepted():
    doc = {"a.b": 1, "$c": 2, "é": 3}
    assert roundtrip(doc) == doc


def test_deep_nesting_within_limit():
    doc = {}
    cursor = doc
    for _ in range(90):
        cursor["n"] = {}
        cursor = cursor["n"]
    cursor["leaf"] = 1
    assert roundtrip(doc) == doc


def test_deep_nesting_beyond_limit_raises():
    doc = {}
    cursor = doc
    for _ in range(200):
        cursor["n"] = {}
        cursor = cursor["n"]
    cursor["leaf"] = 1
    with pytest.raises(BSONError):
        custom_bson.encode(doc)


def test_encode_decode_encode_idempotent():
    doc = {"a": 1, "b": [1, 2, {"c": "d"}], "e": None, "f": 1.5}
    once = custom_bson.encode(doc)
    twice = custom_bson.encode(custom_bson.decode(once))
    assert once == twice


def test_key_order_preserved():
    doc = {"z": 1, "a": 2, "m": 3}
    out = roundtrip(doc)
    assert list(out.keys()) == ["z", "a", "m"]


def test_decode_returns_plain_dict():
    out = roundtrip({"a": 1})
    assert type(out) is dict


# ---------------------------------------------------------------------------
# Encode-side errors
# ---------------------------------------------------------------------------


def test_encode_requires_dict():
    with pytest.raises(TypeError):
        custom_bson.encode([1, 2, 3])


def test_encode_non_str_key_raises():
    with pytest.raises(InvalidDocument):
        custom_bson.encode({1: "a"})


def test_encode_embedded_nul_key_raises():
    with pytest.raises(InvalidDocument):
        custom_bson.encode({"a\x00b": 1})


@pytest.mark.parametrize("value", [set(), object(), 1 + 2j, {"a", "b"}])
def test_encode_unsupported_type_raises(value):
    with pytest.raises(InvalidDocument):
        custom_bson.encode({"v": value})


# ---------------------------------------------------------------------------
# Decode-side corruption
# ---------------------------------------------------------------------------


def test_decode_empty_bytes():
    with pytest.raises(InvalidBSON):
        custom_bson.decode(b"")


@pytest.mark.parametrize("n", [1, 2, 3])
def test_decode_truncated_length_header(n):
    with pytest.raises(InvalidBSON):
        custom_bson.decode(b"\x00" * n)


def test_decode_length_shorter_than_minimum():
    with pytest.raises(InvalidBSON):
        custom_bson.decode(struct.pack("<i", 4) + b"\x00")


def test_decode_length_exceeds_buffer():
    with pytest.raises(InvalidBSON):
        custom_bson.decode(struct.pack("<i", 100) + b"\x00" * 4)


def test_decode_length_overflow_attempt():
    with pytest.raises(InvalidBSON):
        custom_bson.decode(struct.pack("<i", -1) + b"\x00" * 8)
    with pytest.raises(InvalidBSON):
        custom_bson.decode(struct.pack("<I", 0xFFFFFFFF) + b"\x00" * 8)


def test_decode_missing_terminator():
    # length=5 (minimum) but the byte at the end isn't 0x00
    with pytest.raises(InvalidBSON):
        custom_bson.decode(struct.pack("<i", 5) + b"\x01")


def test_decode_unknown_type_byte():
    key = b"a\x00"
    body = b"\xee" + key  # 0xee is not a legal BSON type
    total_len = 4 + len(body) + 1
    data = struct.pack("<i", total_len) + body + b"\x00"
    with pytest.raises(InvalidBSON):
        custom_bson.decode(data)


def test_decode_unterminated_key():
    body = b"\x0aabc"  # null type, key "abc" with no NUL terminator
    total_len = 4 + len(body) + 1
    data = struct.pack("<i", total_len) + body + b"\x00"
    with pytest.raises(InvalidBSON):
        custom_bson.decode(data)


def test_decode_string_length_past_buffer():
    # type=0x02 (string), key="a", declared string length far too large
    body = b"\x02a\x00" + struct.pack("<i", 1000)
    total_len = 4 + len(body) + 1
    data = struct.pack("<i", total_len) + body + b"\x00"
    with pytest.raises(InvalidBSON):
        custom_bson.decode(data)


def test_decode_invalid_utf8():
    payload = b"\xff\xfe\x00"  # invalid UTF-8, NUL-terminated, len=3
    body = b"\x02a\x00" + struct.pack("<i", len(payload)) + payload
    total_len = 4 + len(body) + 1
    data = struct.pack("<i", total_len) + body + b"\x00"
    with pytest.raises(InvalidBSON):
        custom_bson.decode(data)


def test_decode_binary_negative_length():
    body = b"\x05a\x00" + struct.pack("<i", -5) + b"\x00"
    total_len = 4 + len(body) + 1
    data = struct.pack("<i", total_len) + body + b"\x00"
    with pytest.raises(InvalidBSON):
        custom_bson.decode(data)


def test_decode_nested_document_exceeds_container():
    # Outer doc claims to be small, but the embedded doc's length field
    # claims to extend past the outer document's own end.
    inner_header = struct.pack("<i", 100)  # lies about its own length
    body = b"\x03a\x00" + inner_header
    total_len = 4 + len(body) + 1
    data = struct.pack("<i", total_len) + body + b"\x00"
    with pytest.raises(InvalidBSON):
        custom_bson.decode(data)


def test_decode_trailing_bytes():
    valid = custom_bson.encode({"a": 1})
    with pytest.raises(InvalidBSON):
        custom_bson.decode(valid + b"\x00\x01\x02")


def test_decode_document_too_large_declared_length():
    over_limit = 16 * 1024 * 1024 + 1
    data = struct.pack("<i", over_limit) + b"\x00" * 100
    with pytest.raises(BSONError):
        custom_bson.decode(data)


def test_decode_random_fuzz_never_crashes():
    rng = random.Random(1234)
    for _ in range(500):
        length = rng.randint(0, 64)
        blob = bytes(rng.getrandbits(8) for _ in range(length))
        try:
            custom_bson.decode(blob)
        except BSONError:
            pass  # expected for almost all random input
        except Exception as exc:  # pragma: no cover - failure path
            pytest.fail(f"decode() raised a non-BSONError exception on random input: {exc!r}")


def test_decode_fuzz_of_valid_documents():
    # Start from valid encoded documents and flip random bytes; decode
    # must either succeed or raise a BSONError, never crash or hang.
    rng = random.Random(5678)
    base_docs = [
        {"a": 1},
        {"a": "hello", "b": [1, 2, 3]},
        {"a": {"b": {"c": 1}}},
        {"_id": ObjectId(), "v": 3.14},
    ]
    for doc in base_docs:
        original = custom_bson.encode(doc)
        for _ in range(50):
            mutated = bytearray(original)
            idx = rng.randrange(len(mutated))
            mutated[idx] = rng.randrange(256)
            try:
                custom_bson.decode(bytes(mutated))
            except BSONError:
                pass
            except Exception as exc:  # pragma: no cover - failure path
                pytest.fail(f"decode() raised a non-BSONError exception on mutated input: {exc!r}")


def test_decode_large_buffer_gil_release_path_still_validates():
    # Exceeds the GIL-release threshold in _bson_core.c; must still
    # detect corruption (trailing bytes) correctly.
    big_doc = {"v": "x" * 10_000}
    valid = custom_bson.encode(big_doc)
    assert roundtrip(big_doc) == big_doc
    with pytest.raises(InvalidBSON):
        custom_bson.decode(valid + b"\xff")


def test_decode_accepts_bytearray_and_memoryview():
    data = custom_bson.encode({"a": 1})
    assert custom_bson.decode(bytearray(data)) == {"a": 1}
    assert custom_bson.decode(memoryview(data)) == {"a": 1}
